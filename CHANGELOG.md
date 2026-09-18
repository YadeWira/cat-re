# Changelog

All notable changes to **CAT RE** (the native `catre` archiver). Dates are UTC.
The format loosely follows [Keep a Changelog](https://keepachangelog.com/).

## v1.7 — 2026-09-18

Driven by a new conformance harness (`scripts/engine_matrix.py`) that runs the original
engine and `catre` over the same corpus files and compares bytes. Measured results are in
`docs/RE_verified.md` §11.

### Added
- **PDF members in the engine's whole-file mode now extract byte-exact.** `PdfProc` has the
  same two-mode shape as the Office codec: one mode stores `zlib(whole file)` inside its
  payload. That was the only *real* gap the measurements found — for the structural mode, the
  original software does not restore the PDF either.
- **Engine images with an alpha channel now decode.** An RGBA source makes the engine store
  the alpha in its own block *before* the JPEG2000 codestream (35,859 bytes on an 800×600
  PNG), so the codestream is not at `payload+26`; `catre` locates it by the `FF4F FF51` marker
  and validates `Xsiz/Ysiz` against the wrapper. The picture is recovered (34.5 dB), the
  transparency is not (that block uses an undecoded coder).
- `scripts/engine_matrix.py`: the harness itself — CSV + summary of codec used, what we read,
  what is byte-exact, and whether the engine reads our output. It exits cleanly where the
  engine isn't available.

### Measured (no code change, but it settles the roadmap)
- **Office: zero real gaps.** Over 31 real documents, every file the *original engine* restores
  exactly, `catre` also restores exactly (22/22); every file `catre` skips (9/9) is one the
  engine itself returns altered — it re-encodes the document (a 66 KB `.xls` comes back as
  85 KB with 58,355 differing bytes). Cloning that mode would reproduce the engine's damage,
  not the input.
- **A random 250-file corpus sample**: 248 deflate members, all byte-exact in both directions.
- The engine's format→codec table is **declared in the registry**
  (`SOFTWARE\QuikCAT\CODEC\1\{CLSID}`), and the header byte `+0x1A` is that `CodecID`.

## v1.6 — 2026-09-18

Found by running the **original DLLs** (Wine) against `catre` on the same inputs and
comparing bytes, rather than trusting our own round-trip.

### Fixed
- **Images were exchanged upside down with the original software.** The engine's
  JPEG2000 codestream is **bottom-up** (its pipeline feeds CODEC4 a Windows DIB,
  whose first row is the bottom one); we wrote and read rows top-down. Our own
  round-trip never showed it — it flipped twice — but every picture handed to or
  taken from the original engine was vertically mirrored (PSNR 10.6 dB as-is vs
  31.2 dB flipped). Both sides now use the engine's order; our decode of an engine
  archive matches the engine's **own** decode of it at 35.8–38.6 dB.
  *Compatibility note*: images written by `catre` **v1.0–v1.5** carry the old row
  order, so v1.6 extracts those upside down. Re-create such archives if it matters.
- **Engine image members that are not JPEG2000 were reported as a failed decode.**
  The image sub-codec byte (`+0x19`) has at least three values: `0x01` = JPEG2000,
  `0x02` = another engine codec (GIF, paletted/grayscale PNG), `0x09` = the LEAD
  path (TIFF, some PNG). Only `0x01` carries an `FF4F` codestream. Non-J2K members
  are now identified (`image-x`, `lead-cmp`) and skipped cleanly, and `extract`
  checks for the codestream marker before calling the decoder.
- **Engine PDF members were mislabelled `office-ps`.** `PdfProc` writes its own
  structural payload (`32 01 78 …`), not zlib-of-file; it is told apart by the
  stream-family byte `+0x1A` (`0x05` = PDF, `0x02` = office, `0x04` = deflate) and
  now lists as `pdf-proc`. We still cannot decode it — but we no longer pretend the
  member is something else.

### Changed
- `docs/QCF_FORMAT_SPEC.md` §5 now carries the measured codec-byte table and the
  row-order rule; `docs/RE_verified.md` gained the engine-vs-`catre` exactness
  matrix behind all of this.

### Verified
- Windows 7 SP1, x86 and x64: labelling, `test`, and byte-exact Office extraction
  (`fc /b`). The x64 build decodes an image **byte-identically to the Linux build**;
  the x86 build differs by at most 1 unit per channel (92 dB), which is 32-bit
  floating-point rounding in the wavelet reconstruction, not a logic difference.

## v1.5 — 2026-09-18

### Fixed
- **`test` now really verifies.** It used to report `OK` for every member it did not
  inflate — image and Office members passed untouched, so a **corrupted JPEG2000
  payload was reported as OK**. Each decodable member is now actually decoded
  (JPEG2000 via OpenJPEG, Office/DEFLATE via zlib); members in a codec we cannot
  decode are reported as **skipped**, never as OK.
- **The pure-Python front-end caught up with v1.4.** It classified engine per-stream
  Office members as `deflate` and died with `Error -5 while decompressing data`
  instead of identifying them. It now shares the C tool's classification
  (`office-ps`, `lead-cmp`), skips what it cannot decode, and its `test` no longer
  passes members it never decompressed.
- **Reproducible builds.** The build deps lived in `/tmp` (a tmpfs), so they vanished
  on reboot and `make catre` failed with "openjpeg.h: No such file". They now build
  into `~/.cache/catre-deps` via `make deps` / `scripts/build-*-deps.sh`, with the
  system OpenJPEG as a fallback.

### Added
- **Office per-stream: the whole-file mode is decoded.** That codec is multi-mode; one
  of its modes stores the original file as a single zlib stream. Those members now
  extract **byte-exact** (C tool and Python), instead of being skipped wholesale. The
  structural-model and sparse-XLS modes remain opaque and are skipped with a message.
- Regression tests for both (suite is now 46), including an engine-made per-stream
  fixture (`tests/fixtures/real_office/Bug49919.doc.qcf`) and a corrupted-image test
  that must fail.

### Changed
- `list` prints `?` instead of `0` / `0.0%` for the packed size of an opaque member,
  whose size the header does not record.
- Docs: corrected the two claims the binary analysis had already disproven but that
  survived in `docs/RE_notes.md` / `docs/SUMMARY.md` — Office is standard **zlib
  1.1.3**, not a proprietary "MS-OFFCRYP", and **TIFF does not go to JPEG2000**.

## v1.4 — 2026-06-11

### Fixed
- **Reads `.qcf` archives the original engine produced.** Earlier versions wrongly
  rejected real engine files — including the engine's own **JPEG2000 images** — with
  "not a valid .qcf". Root causes, both fixed:
  - `qcm_read` assumed a member's `comp` field includes the 26-byte image wrapper, but
    the engine stores the codestream size only, so the stream walk missed the central
    directory. Added a fallback that locates the directory by scanning for the `TOP`
    record, so any engine archive can be listed and its decodable members extracted.
  - OpenJPEG memory-stream `skip`/`seek` callbacks didn't clamp at EOF, overflowing the
    read cursor on some engine (Kakadu) codestreams; `extract` also fed a truncated
    buffer. Engine images (e.g. 1920×1200) now decode correctly.
- **Graceful handling of undecodable proprietary codecs.** `list` now identifies
  per-stream Office (`office-ps`) and LEAD CMP (`lead-cmp`) members; `extract` skips them
  with a clear "needs the original engine" message instead of failing the archive.

## v1.3 — 2026-06-11

### Added
- **Store-if-not-smaller fallback.** Images are compressed with JPEG2000 only when
  that actually beats DEFLATE; otherwise the file is stored with DEFLATE. A tiny or
  already-compressed image (e.g. a small GIF) can no longer *grow* (was up to ~227 %).
- Regression test for the C image path (`tests/test_catre_image_c.py`): JPEG2000
  round-trip + the store fallback. Suite is now 39 tests.
- `CHANGELOG.md`.

### Changed
- The pure-Python front-end (`qcf_tool/catre.py`) is now honestly documented as a
  **reader (all codecs) + DEFLATE writer**; JPEG2000/Office *encoding* is C-only.
  Removed the misleading "this build uses DEFLATE" notes.
- `list` and `info` now accept the `--no-progress` / `-p` / `--progress` flags too
  (previously only `compress`/`extract` did).
- Docs: `-q` is documented as a PSNR target; `--store` noted for bit-exact images.

## v1.2 — 2026-06-11

### Changed
- **PSNR-targeted image compression (closes the ~8× size gap).** The JPEG2000 encoder
  switched from a fixed target bitrate to content-adaptive PSNR-targeted rate control —
  the original engine's actual method — calibrated to its measured quality→PSNR curve
  (`q0 → 26.7 dB … q100 → 32.2 dB`). Validated: `catre` now delivers the same PSNR per
  `-q` as the original engine, with competitive-or-smaller files. A 628 KB photo at
  `-q 50` is now ~82 KB (13 %) vs ~144 KB (23 %) before.

## v1.1 — 2026-06-11

### Fixed
- Extracting an image no longer doubles the extension (`file.png` → `file.png`, not
  `file.png.png`); the original extension is replaced rather than appended
  (`photo.jpg` → `photo.png`). Reported by **xman** on encode.su.

## v1.0 — 2026-06-10

- First public release. Free, reverse-engineered clone of the 2003 Choshuku/CAT `.qcf`
  (QCM) archiver. Reads/writes the container (single, multi-file, nested folders) with
  DEFLATE, lossy JPEG2000 images (`-q`), and the Office MSOC21 whole-file variant —
  validated against the original engine. Zero-dependency static binaries for Linux x64
  and Windows x64/x86 (tested on Windows 7 SP1).
