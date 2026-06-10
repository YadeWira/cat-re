# Linux Port: `libcat` + `cat-tool`

A complete reimplementation of the Choshuku (CAT) `.qcf` container format
and its compression backends, written in portable C and tested on Linux.
This is a sister project to the Python `qcf-tool` and shares the same RE
ground truth in [RE_notes.md](RE_notes.md).

## What you can do with it

- Read/inspect any `.qcf` (or `QCM`, or passthrough ZIP) archive.
- Extract the inner files (raw, ZIP, OLE2/Office, or JPEG2000 image).
- Pack one or more files into a new `.qcf`.
- Decode JPEG2000 images inside `.qcf` archives to BMP.
- All on Linux, no Windows, no WINE, no Office installation.

## Build

System dependencies (Debian / Ubuntu):

```bash
apt-get install -y zlib1g-dev libopenjp2-7-dev
```

If `libopenjp2-7-dev` is unavailable, the headers can be extracted from
the `.deb` directly:

```bash
apt-get download libopenjp2-7-dev
dpkg-deb -x libopenjp2-7-dev_*.deb /tmp/openjpeg
```

Set the include path and build:

```bash
export OPENJPEG_INC=/tmp/openjpeg/usr/include/openjpeg-2.5
export OPENJPEG_LIB=/usr/lib/x86_64-linux-gnu

make          # builds libcat.so and cat-tool
make test     # runs the unit tests
```

## Layout

```
libcat/                  # C library
  cat.h                  # public API
  format.c               # .qcf format parser/serializer
  dispatch.c             # top-level read/write/dispatch
  backends/
    raw.c                # raw passthrough
    zip.c                # PKZIP (STORED + DEFLATE) reader/writer
    jp2.c                # JPEG2000 via libopenjp2
    ole2.c               # OLE2 (MS-CFB) reader for .doc/.xls/.ppt

tools/
  cat-tool.c             # CLI front-end

tests/
  test_format.c          # format, magic, header roundtrip
  test_zip.c             # zip and raw roundtrips
  test_jp2.c             # JP2 encode->decode roundtrip
  runtests_main.c        # single test driver

Makefile
```

## Public API (`libcat/cat.h`)

```c
cat_status cat_read_file (const char *path,
                          cat_header *out_header,
                          cat_file ***out_files, size_t *out_count);

cat_status cat_write_file(const char *path,
                          const char **names, size_t name_count,
                          const uint8_t *ext_header, size_t ext_size);

cat_status cat_decode_jp2(const void *payload, size_t plen,
                          const char *out_bmp_path);

cat_status cat_encode_jp2_from_bmp(const char *in_bmp_path,
                                   uint8_t **out, size_t *out_len);
```

Result codes are negative on error (`cat_status` enum in `cat.h`).

## CLI usage

```bash
# Inspect a .qcf
./cat-tool info archive.qcf

# Extract
./cat-tool extract archive.qcf -o ./out/

# Pack one file (raw passthrough, name goes into ext header)
./cat-tool pack -o out.qcf -n hello.txt hello.txt

# Pack multiple files (auto-wraps them in ZIP)
./cat-tool pack -o out.qcf a.txt b.png c.pdf
```

## Codec backend matrix

| Input file type | Backend       | Algorithm                            |
|----------------|---------------|--------------------------------------|
| `QCF`          | `format.c`    | 28-byte fixed header + ext header     |
| `QCM`          | `format.c`    | same as QCF (multi-volume flag)      |
| `PK\x03\x04`   | passthrough   | raw ZIP via `cat_decode_zip`          |
| raw bytes      | `raw.c`       | passthrough; name from ext header    |
| ZIP            | `zip.c`       | STORED + DEFLATE via zlib (hand-rolled, no miniz) |
| JPEG2000       | `jp2.c`       | JP2 + J2K via libopenjp2              |
| OLE2/Office    | `ole2.c`      | MS-CFB (compound file binary) reader |

ZIP is fully self-contained — no external dep beyond zlib. OLE2 reader
walks the directory tree but does not decompress MS-OFFCRYP-compressed
streams (the deflate variant with custom Huffman tables used by Office
97-2003). For uncompressed Office files this works.

## .qcf format reminder

```
+0x00  DWORD  magic           'QCF\x01' / 'QCM\x01' / 'PK\x03\x04'
+0x04  BYTE[23] opaque fields  type-specific metadata
+0x1B  BYTE  ext_hdr_size    0..255
+0x1C  BYTE[] ext_header      filename / other metadata
+0x1C+N       payload         inner stream (jp2, zip, ole2, raw)
```

See [RE_notes.md](RE_notes.md) for the full reverse-engineering notes.

## What's been tested

`make test` runs:

- `test_format` — magic constants, type detection, header parse/serialize
  roundtrip, inner-format detection.
- `test_zip_roundtrip` — pack two files, read back, verify content and
  count. Tests the libcat ZIP encoder against the hand-rolled decoder.
- `test_raw_roundtrip` — single-file pack, verifies the ext header is
  recovered as the filename.
- `test_jp2_roundtrip` — write a 32x32 gradient BMP, encode to JP2 (lossless),
  decode to BMP, verify dimensions.

## What hasn't been tested

- OLE2: needs a real `.doc`/`.xls`/`.ppt` to exercise. The MS-OFFCRYP
  deflate path is not implemented (would need a custom Huffman-table aware
  inflate).
- LFC (LEADTOOLS filter compression): proprietary format, requires a
  reference encoder.
- Multi-volume QCM files.
- Reading `.qcf` files produced by the original Choshuku — we have no
  reference samples yet (the Wine harness can produce them; see RE_notes).

## License

MIT (same as the rest of cat-re).
