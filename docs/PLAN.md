# cat-re — Roadmap

Three deliverables, ordered by ROI:

## 1. `qcf-tool` (free ChoDecoder replacement) — START HERE

Python library + CLI to read/write `.qcf` files without the original
2003 software, on any modern OS.

- `.qcf` parser: 28-byte header, version/ext-size fields, extended metadata
- Dispatcher: detect inner format (JP2, ZIP, OLE2) and route to backend
- Backends: `openjpeg`/Pillow for images, stdlib `zipfile` for ZIP,
  `olefile` for OLE2/Office
- CLI: `qcf-tool info file.qcf`, `qcf-tool extract file.qcf -o out/`,
  `qcf-tool pack input/ -o out.qcf`

Why: the original ChoDecoder.exe is no longer distributed, the license
server is dead, and old `.qcf` archives are inaccessible without it.

## 2. Wine harness — capture ground truth

Run `QCArch.dll` + `CODEC4.dll` under Wine on Linux. Hook `IStream::Read`
/`Write` to record the exact byte sequences for real `.qcf` files
(creation + extraction). This fills in the rest of the header layout
beyond the first 28 bytes and confirms the codec-routing table.

- Use `winetricks` + a Windows vcrun, register the COM DLLs with
  `regsvr32`, drive them with a small C harness or Python `comtypes`.

## 3. `cat-re` for modern systems

Standalone reimplementation of the CAT engine in Python:

- Same `.qcf` I/O
- Same codec dispatch (4 backends)
- Same Kakadu-style JP2 output, but with `openjpeg` underneath
- Optional: CLI drop-in for `Choshuku.EXE` so existing shell-extension
  users can keep right-click workflows

Stretch: a web service that decodes `.qcf` uploads.

## Why this is feasible

The binaries are unpacked, unstripped where it matters, and the format is
small. Kakadu is replaced by OpenJPEG. Deflate is stdlib. OLE2 is
`olefile`. The only unknown is the rest of the 28-byte header, and that
falls out of the Wine harness in (2).
