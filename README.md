# cat-re — Choshuku / CAT Reverse Engineering

A from-scratch reimplementation of the [Choshuku Professional (超圧縮)](https://web.archive.org/web/2003*/http://www.sourcenext.co.jp/) v1.0.2
compression utility by SOURCENEXT / QuikCAT Technologies (2003).

Deliverables (the C tool runs on Linux **and** Windows):

| Component     | Language | Purpose                                                |
|---------------|----------|--------------------------------------------------------|
| `tools/catre.c` | C      | **CAT RE v1.0** — the main archiver (zero-dependency static binary). |
| `qcf_tool/`   | Python   | Free ChoDecoder replacement: read/write `.qcf` files.   |
| `libcat/`     | C        | Native port of the algorithms + `cat-tool` CLI.        |
| `harness/`    | C        | Wine harness to drive the original Windows DLLs.       |

## Prebuilt binaries

`catre` ships as **zero-dependency static binaries** (no DLLs, no runtime, nothing to install):

| Platform | File | Notes |
|---|---|---|
| Linux x64   | `catre-linux-x64`        | static; `ldd` → "not a dynamic executable" |
| Windows x64 | `catre-windows-x64.exe`  | tested on **Windows 7 SP1 x64** (build 7601) |
| Windows x86 | `catre-windows-x86.exe`  | 32-bit; runs on x86 and x64 Windows |

See the repository **Releases** page for downloads. To rebuild them yourself:

```bash
scripts/build-win-deps.sh        # build static zlib + OpenJPEG for mingw (x64 + x86)
make dist                        # → dist/{catre-linux-x64,catre-windows-x64.exe,catre-windows-x86.exe}
```

## What is `.qcf`?

`.qcf` is a small binary container with a 28-byte fixed header plus an
optional extended header, wrapping a single inner stream. The inner
stream is one of:

- raw bytes (default)
- a ZIP archive
- a JPEG2000 codestream (the "CAT algorithm" — built on Kakadu)
- an OLE2 compound document (Office docs)

See [docs/RE_notes.md](docs/RE_notes.md) for the full reverse-engineering
notes (format layout, codec dispatch table, COM IIDs, etc.).

## Quick start

### CAT RE v1.0 CLI (recommended)

`catre` is the main archiver — a **native C** tool (links zlib + OpenJPEG statically; no
original DLLs). It reads real `.qcf` (single/multi/nested folders) and writes DEFLATE +
**lossy JPEG2000** for images. Verified: the original Choshuku engine decompresses archives
`catre` produces (including images and folders).

```bash
make catre            # native binary (zlib + OpenJPEG, static)
make catre-static     # fully static, zero-dependency build

./catre compress file.txt mydir/ -o out.qcf      # files & folders (native folder records)
./catre compress photo.png -o out.qcf -q 50      # images → JPEG2000 lossy (quality 0-100)
./catre list out.qcf -v                          # sizes, ratio, codec, date
./catre extract out.qcf -o ./restored/           # folders restored; images decoded to PNG
./catre info out.qcf                             # container + codec details
./catre test out.qcf                             # verify integrity
```
Commands mirror the original software: `compress`/`c`, `extract`/`x`, `list`/`l`,
`info`/`i`, `test`/`t`. `-q/--quality` is the engine's `lQuality`; `--store` forces DEFLATE.

A Python implementation of the same CLI also exists: `python3 -m qcf_tool.catre ...`
(source: `qcf_tool/catre.py`, used by the test suite).

### Python library (`qcf-tool`)

```bash
# 35 tests, all green
PYTHONPATH=. python3 -m pytest tests/ -v

# Inspect a .qcf file
PYTHONPATH=. python3 -m qcf_tool info archive.qcf

# Extract contents
PYTHONPATH=. python3 -m qcf_tool extract archive.qcf -o out/

# Pack files
PYTHONPATH=. python3 -m qcf_tool pack input.txt -o out.qcf -n input.txt
```

### C (`libcat` + `cat-tool`)

```bash
# Install system deps (Debian/Ubuntu)
apt-get install -y zlib1g-dev libopenjp2-7-dev

# Build
make

# Run the test suite
make test

# Use the CLI
./cat-tool info archive.qcf
./cat-tool extract archive.qcf -o out/
./cat-tool pack -o out.qcf -n hello.txt hello.txt
```

See [docs/LINUX_PORT.md](docs/LINUX_PORT.md) for the full API and
backend matrix.

## Status & limitations

**Reading** (fully supported, validated against real engine archives):
- Any `.qcf`: single-file, multi-file, and nested folders.
- DEFLATE members → decompressed losslessly (zlib).
- Image members → decoded (JPEG2000 via OpenJPEG) and written as PNG.
- Office/OLE2 members in the **whole-file `MSOC21` variant** (36-byte header + zlib of the
  whole compound file) → decompressed. The engine's **per-stream** variant for *recognized*
  Word/Excel docs is not yet read (see below).

**Writing** (`catre compress`):
- **DEFLATE** for generic files, with **native nested-folder records** — verified: the
  *original* Choshuku engine decompresses archives `catre` produces.
- **JPEG2000 (lossy)** for images (PNG/JPG/BMP/GIF) via `-q/--quality`, with the exact
  26-byte IMGCMP wrapper — verified: the *original* engine decodes `catre`'s image archives.
- **Office/OLE2** (`.doc/.xls/.ppt`) via the **`MSOC21` whole-file variant** (36-byte header +
  zlib of the compound file) — verified: the *original* engine decodes `catre`'s Office archives
  byte-exact. `--store` forces plain DEFLATE.

**What is NOT implemented** (beyond the original shell UI, which is intentionally out of scope):

| Missing | Notes |
|---|---|
| **Reading the per-stream `MSOC21` variant** | For *recognized* Word/Excel docs the engine emits a per-stream format (`32 01 aa…` header, no single zlib). `catre` writes/reads the **whole-file** `MSOC21` variant (engine-validated) but does not yet read the per-stream one. |
| **LFC / LEADTOOLS codec** | Not supported — proprietary third-party (medical-imaging) format; `LFCMP13n.dll`'s codec is not reimplemented. |
| **Shell integration & GUI** | Context-menu handler (`QCShExt`), Explorer preview (`QCShView`), and dialogs (`QCArchUI`) are out of scope — this is a CLI/format tool. |

Implemented & validated against the real engine: container (single/multi/**folders**), **DEFLATE**
(zlib), **JPEG2000** (OpenJPEG) lossy images with quality control, and **Office/OLE2** (`MSOC21`
whole-file zlib). Only **LFC** (LEADTOOLS, third-party) and the per-stream Office variant remain.
See `docs/QCF_FORMAT_SPEC.md` and `docs/RE_verified.md`.

## Project layout

```
tools/catre.c            CAT RE v1.0 — native C archiver (main tool; build with `make catre`)
qcf_tool/                Python reimplementation (same CLI + library)
  catre.py               CAT RE v1.0 CLI (compress/extract/list/info/test)
  qcm.py                 REAL QCM parser+encoder (single/multi/folders, validated)
  format.py              low-level 28-byte QCF header primitive
  dispatch.py            backend router
  cli.py                 legacy library CLI (info/extract/pack/sniff)
  backends/              raw / zip_ / jp2 / ole2

libcat/                  C library (cat.h, format.c, dispatch.c, backends/)
tools/cat-tool.c         older C CLI (libcat-based)

tests/
  test_*.py              pytest suite (Python)
  c/                     C test sources (built into tests/runtests)
  fixtures/real_qcf/     small real .qcf samples (synthetic inputs)
  fixtures/real_office/  Apache POI Office docs + their .qcf

docs/
  QCF_FORMAT_SPEC.md     implementable format spec (start here)
  RE_verified.md         binary-verified RE findings + evidence
  RE_notes.md, SUMMARY.md, SESION_NOCTURNA.md, PLAN.md, LINUX_PORT.md

harness/                 working Wine/MinGW harnesses (drive the original DLLs)
  sfa.c                  produce/consume REAL .qcf via CompressFile
  dec.c, dca.c           decompress/extract validators
  multifile.c            multi-file Compress experiment

OLD/  (git-ignored — NOT part of the public repo)
  original/              the 2003 product: proprietary DLLs/installer (not redistributed)
  samples/jpg-files/     76 MB JPEG corpus (test material)
  fixtures-multi/        real multi-file + nested-folder ground-truth archives (large)
  harness-experiments/   superseded exploratory probes
  pll/                   packJPG-style lossless JPEG recompression experiment
  resultados-jpg-test/   generated JPEG2000 comparison images

ghidra/                  Ghidra project + scripts (git-ignored, large)
```

> **Note on `OLD/`**: it holds the original proprietary product binaries, large test
> corpora, and experiments. It is git-ignored and **not uploaded** to the public repo.
> The harness and some skipped tests use files under `OLD/` when present locally.

## License

MIT.
