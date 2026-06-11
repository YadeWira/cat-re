# Changelog

All notable changes to **CAT RE** (the native `catre` archiver). Dates are UTC.
The format loosely follows [Keep a Changelog](https://keepachangelog.com/).

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
