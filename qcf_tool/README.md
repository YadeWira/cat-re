# qcf-tool

A free, modern reimplementation of the [Choshuku](docs/RE_notes.md) (CAT)
`.qcf` reader/writer, written in Python.

## Install

```bash
pip install -e .
```

## Usage

```bash
# Inspect a .qcf file
qcf-tool info archive.qcf

# Extract its contents
qcf-tool extract archive.qcf -o ./out

# Pack one or more files
qcf-tool pack one.txt two.png -o out.qcf

# JSON sniff (for scripting)
qcf-tool sniff archive.qcf
```

## Python API

```python
from qcf_tool import QcfFile
from qcf_tool.dispatch import Dispatcher

qcf = QcfFile.read(open("archive.qcf", "rb"))
files = Dispatcher().decode(qcf)
for f in files:
    open(f.name, "wb").write(f.data)
```

## What's supported

| Inner format          | Backend        | Status      |
|-----------------------|----------------|-------------|
| Raw single file       | `RawBackend`   | read/write  |
| ZIP archive           | `ZipBackend`   | read/write  |
| JPEG2000 (JP2 / J2K)  | `Jp2Backend`   | read (PNG) / write |
| OLE2 / Office         | `Ole2Backend`  | read only   |

The `QCF\x01` (single-volume) and `QCM\x01` (multi-volume) magics are
both detected; plain `PK\x03\x04` ZIP is also accepted as a passthrough.
