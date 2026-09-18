# cat-re — Choshuku / CAT Reverse Engineering

A from-scratch, free reimplementation of the `.qcf` (QCM) archiver shipped in 2003 as
[Choshuku Professional (超圧縮)](https://www.sourcenext.com/download/cho_pro.html) by
SOURCENEXT / QuikCAT Technologies — and, under different branding, as *Miliki Super
Compressor Pro*. Same QuikArchive engine, same format.

`catre` reads and writes the real thing with **no original DLLs**: DEFLATE, lossy JPEG2000
images, Office (MSOC21) and PDF (PdfProc) members, single file, multi-file and nested folders.
The original engine decompresses what `catre` writes, and vice versa.

📖 **[Documentation is in the wiki](https://github.com/YadeWira/cat-re/wiki)** — usage, format,
codec map, measured cloning status, benchmarks, reverse-engineering notes.

## Install

Zero-dependency static binaries on the [Releases](https://github.com/YadeWira/cat-re/releases)
page — no DLLs, no runtime, nothing to install:

| Platform | File |
|---|---|
| Linux x64 | `catre-linux-x64` (static) |
| Windows x64 | `catre-windows-x64.exe` (tested on Windows 7 SP1) |
| Windows x86 | `catre-windows-x86.exe` |

Verify against `SHA256SUMS.txt`. On Linux, `chmod +x` the binary — GitHub strips the executable
bit from release assets. Building it yourself:
**[Building from Source](https://github.com/YadeWira/cat-re/wiki/Building-from-Source)**.

## Use

```bash
catre compress file.txt mydir/ -o out.qcf      # files & folders (native folder records)
catre compress photo.png -o out.qcf -q 50      # images → JPEG2000 lossy (quality 0-100)
catre add out.qcf more.txt                     # add to an existing archive
catre delete out.qcf file.txt                  # remove members (a folder takes its contents)
catre list out.qcf -v                          # sizes, ratio, codec, date
catre extract out.qcf -o ./restored/ -m file.txt   # all of it, or only what you name
catre info out.qcf                             # container + codec details
catre test out.qcf                             # decode every member and verify it
```

Commands mirror the original software (`compress`/`c`, `add`/`a`, `delete`/`d`,
`extract`/`x`, `list`/`l`, `info`/`i`, `test`/`t`). `add` and `delete` rewrite the archive
from the stored streams, so members in codecs only the original engine can decode come
through byte-for-byte. `-q` sets the JPEG2000 **target PSNR**, calibrated to the original engine;
`--store` forces lossless DEFLATE for files you need bit-exact. Every flag, the Python
front-end and the caveats:
**[Quick Start](https://github.com/YadeWira/cat-re/wiki/Quick-Start)**.

## How complete is it?

Measured against the original engine, not asserted. `scripts/engine_matrix.py` compresses with
the engine, decompresses **with the engine**, and compares — which turns the question into what
is recoverable *at all*. Over 31 real Office documents:

| | engine restores exactly | engine does **not** |
|---|:--:|:--:|
| **`catre` restores exactly** | **22** | 0 |
| **`catre` skips** | **0** | **9** |

**Zero real gaps**: every member we skip is one the *original software* returns altered — it
re-encodes the document instead of storing it. Same shape for PDF. A random 250-file corpus
sample: 248 DEFLATE members, all byte-exact in both directions. Deliberately out of scope: the
LEADTOOLS image codecs (a third party's IP, still sold today).

Details: **[Cloning Status](https://github.com/YadeWira/cat-re/wiki/Cloning-Status)** ·
**[Codec Map](https://github.com/YadeWira/cat-re/wiki/Codec-Map)**.

## Repository layout

```
tools/catre.c             the archiver (main tool) + catre_img.c (JPEG2000 via OpenJPEG)
qcf_tool/                 Python reimplementation: reads every codec, writes DEFLATE
libcat/, tools/cat-tool.c older C library and CLI, kept for reference
harness/                  Wine/MinGW harnesses that drive the original DLLs
scripts/engine_matrix.py  conformance harness: original engine vs catre, file by file
tests/                    pytest suite (53) + real engine-made fixtures
docs/                     the specification of record — QCF_FORMAT_SPEC.md, RE_verified.md
```

`docs/` is versioned with the code and is authoritative for the format; the wiki is the reading
material around it.

## License

MIT. The original DLLs, installer and manual are **not** redistributed here. The CAT patents
have expired and this clone implements only DEFLATE and JPEG2000 — the reasoning is in
**[Legal](https://github.com/YadeWira/cat-re/wiki/Legal)**.
