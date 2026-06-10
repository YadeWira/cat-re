# Choshuku / CAT — Reverse Engineering Notes

`cat-re` is a reverse-engineering workspace for **Choshuku Professional (超圧縮) v1.0.2**,
a 2003 Japanese compression utility by SOURCENEXT / QuikCAT Technologies.

## TL;DR

The DLLs are not packed, not obfuscated, and have no anti-debug. The `.qcf`
container format is small and well-defined. The "CAT algorithm" is a
**multi-codec engine**: it dispatches to different compression backends based
on the input file type, all wrapped in a common `.qcf` container.

## .qcf container format

```
+0x00  DWORD  magic           = 'QCF\x01'  (0x01464351 LE)
+0x04  ?      other fields    (TBD; engine uses at least one of these)
+0x1B  BYTE   ext_hdr_size    additional metadata bytes (e.g. filename)
+0x1B+ext  ...
```

- The reader in `QCArch.dll @ 0x1001f810` issues `IStream::Read(this+0x204, 0x1C, &n)`.
  It throws E_FAIL if `n != 0x1C`. **Correction (see `docs/RE_verified.md`):** a wrong
  magic does NOT reject the file — it only *skips* the extended-header read. There are
  three identical header handlers: `0x1001f810` (IStream read), `0x1001f960` (IStream
  write), `0x1001fa30` (raw `HANDLE`/`ReadFile`).
- The 28-byte header is `magic (DWORD) + 5×DWORD + 3×BYTE + ext_hdr_size (BYTE)` — not
  "23 opaque bytes". Confirmed from the constructor `FUN_1001f7b0 @ 0x1001f7b0`.
- Followed by N bytes of "extended header" where `N = byte[0x1B]`, read at file offset
  0x1C into `this+0x220` (a 256-byte buffer). Holds the filename (UTF-8, CP 65001).
- `QCM\x01` is a sibling magic (multi-volume / manifest variant).
- `PK\x03\x04` is also accepted — i.e. plain ZIP files are read as archives
  by the same code path (fallback to `UnzDLL.dll`).

## Compression backends (per input type)

The CAT engine picks a backend based on file-type detection (header magic +
extension). Internally each backend is a separate COM DLL loaded by
`QCArch.dll`:

| Input type      | Backend DLL        | Underlying algorithm                          |
|-----------------|--------------------|-----------------------------------------------|
| BMP / GIF / PNG | `CODEC4.dll`       | JPEG2000 (Kakadu) — wavelet + MQ coder        |
| / TIFF / JPEG   | `IMGCMP.dll`       | wrapper around CODEC4                         |
| PDF             | `PdfProc.dll`      | parses PDF, applies Deflate to `FlateDecode`  |
| DOC / XLS / PPT | `MSOC21.dll`       | OLE2 compound storage + Deflate (MS-OFFCRYP)  |
| anything else   | `ZipDLL.dll`       | ZIP = Deflate (LZ77 + Huffman)                |
| medical/image   | `LFCMP13n.DLL`     | LFC (LEADTOOLS filter compression)            |

So the "CAT algorithm" is really four backends plus a dispatcher:

1. **JPEG2000 (Kakadu)** — full kdu_* class hierarchy in `CODEC4.dll`. Marker
   segment parameter classes exposed: `siz_params`, `cod_params`, `qcd_params`,
   `poc_params`, `rgn_params`, `crg_params`, `org_params`. These are
   standard JPEG2000 Part 1 / Part 2 marker segments (SIZ, COD, QCD, POC, RGN,
   CRG, ORG). The CAT "wrapping" is in custom quantization / precinct /
   progression defaults.
2. **Deflate / zlib** — used by ZipDLL, UnzDLL, PdfProc, MSOC21, MStream,
   plus the LEADTOOLS PNG codec.
3. **LFC (LEADTOOLS)** — proprietary `LFCMP13n.DLL` filter format
   (`fltCompressBuffer` / `fltStartCompressBuffer` / `fltGetStamp` etc.).
   Exposed through the standard LEAD filter API.
4. **OLE2 / MS-OFFCRYP** — Office docs go through `MSOC21.dll`, which is
   OLE2 compound storage with deflate-compressed streams (the "Office 97+
   compression" used in `.doc`/`.xls`/`.ppt`).

Auxiliary codecs seen in the LEADTOOLS image DLLs but not used as
general-purpose compression: LZW (GIF/TIFF), RLE (BMP/FAX).

## Protection analysis

None of the binaries is packed. No anti-debug. All exports and imports
visible.

- All DLLs are PE32 i386, GUI subsystem, with standard sections.
- `CODEC4.dll` is **not stripped** — 145 MSVC-mangled exports visible
  (including the full Kakadu class API).
- `QCArch.dll` / `QCArchUI.dll` / `QCShExt.dll` / `QCShView.dll` are
  stripped (only the 4 standard COM exports + `ShowProp` / `DecompressArchive`).
- The only "suspicious" imports are `VirtualProtect` (used in normal codec
  self-modifying code) and `OutputDebugStringA` (debug logging).
- COM CLSID registration is the only product-level protection; the license
  key `C4A2-CCCC-B4DF-3747-1720` is embedded in the original MSI.

A debugger attaches cleanly. Ghidra / IDA / radare2 will work without unpacking.

## COM surface

- `QCQuikArch 1.0 Type Library` (GUID `{B19AA1C0-C66E-4A20-90DF-91D6221A09A5}`) — engine.
  - `IQCSingleFileArch` with `CompressFile`, `DecompressFileW`,
    `opCompressFile`, `opCompressDir`, `opExtractFile`, `opExtractDir`,
    `opRemoveFile`.
  - `QCActionEnum` (action codes), `QCCallBack` class.
- `QArchUI 1.0 Type Library` — UI dialogs.
  - `IPropDlg`, `IQCAFileProperty`, `IQCACommDlg`, `IQCEvaluation`,
    `IQAProgress`.

## Suggested next steps

1. Build a `.qcf` parser/decoder in Python using the layout above — first
   step toward a free ChoDecoder replacement.
2. Disassemble the codec-selection dispatcher in `QCArch.dll` to map
   each file-type detection branch to the corresponding backend.
3. Reconstruct the `MSOC21` OLE2 layer to read/write Office docs directly
   (decoupling from the `.qcf` wrapper).
4. Optional: hook `IStream::Read`/`Write` on the running DLLs (via Wine on
   Linux) to capture real `.qcf` files for analysis.

## COM interface map (from `LoadRegTypeLib` introspection)

The type library `QCQuikArch 1.0` (LIBID `{B19AA1C0-C66E-4A20-90DF-91D6221A09A5}`)
was successfully loaded under Wine and dumped via `ITypeInfo::GetDocumentation`.

### Coclasses (CLSIDs)

| Name              | CLSID                                  | Registered | Default interface |
|-------------------|----------------------------------------|------------|---------------------|
| `QCQuikArch`      | `{7F0B34D0-D90A-49E9-9212-31349D545F4B}` | yes        | `IQCQuikArch` (custom) |
| `QCItem`          | `{F5AA02E0-59CB-4550-AF73-42F52F9A4C6C}` | yes        | `IQCItem` (custom) |
| `QCCallBack`      | `{4FD64C77-2743-4106-96C3-56C1C5616BDD}` | yes        | `IQCCallBack` (custom) |
| `QCEnum`          | `{46A5BB64-57B1-4307-B987-BA8536A89669}` | yes        | `IQCEnum` (custom) |
| `QCActionEnum`    | `{55C31B6F-B327-4390-B640-0AD3E697E617}` | yes        | `IQCActionEnum` (custom) |
| `IQCSingleFileArch` | `{F1FE45A8-9619-45F0-ACE3-11C3F14E32BA}` | no      | IDispatch |

### Interface IIDs and vtable

**`IQCQuikArch` = `{BFDCA750-A117-46CD-8CE6-29B51627B268}`** — main engine
interface, custom vtable (NOT IDispatch). 12 vtable entries (3 IUnknown + 8 methods + 1
extra). Verified working with `QueryInterface` from `CLSID_QCQuikArch`.

| Vtable | DISPID       | Name             | Params | Note |
|-------:|--------------|------------------|-------:|------|
| 0-2    | -            | IUnknown         | -      | QI/AddRef/Release |
| 3      | `0x60010000` | `Create`         | 4      | |
| 4      | `0x60010001` | `Delete`         | 1      | |
| 5      | `0x60010002` | `Close`          | 0      | |
| 6      | `0x60010003` | `EnumerateItems` | 2      | |
| 7      | `0x60010004` | `Extract`        | 5      | |
| 8      | `0x60010005` | `Compress`       | 6      | |
| 9      | `0x60010006` | `Remove`         | 4      | |
| 10     | `0x60010007` | `Move`           | 5      | |
| 11     | -            | (extra)          | -      | `push esi` — second IUnknown impl? |

**`IQCItem` = `{CEE2A09C-EF07-4F6D-9F5A-FD2FC3B84661}`** — per-file item
interface, 10 methods (5 read/write property pairs).

| DISPID       | Name              | Get/Set | Type    |
|--------------|-------------------|---------|---------|
| `0x60010000` | `FileName`        | G/S     | BSTR    |
| `0x60010002` | `OriginalSize`    | G/S     | I4      |
| `0x60010004` | `CompressedSize`  | G/S     | I4      |
| `0x60010006` | `CreationTime`    | G/S     | (FILETIME) |
| `0x60010008` | `FileAttributes`  | G/S     | I4      |

**`IQCCallBack` = `{54C10BB1-797A-4B68-A1E0-A8C3380DA79D}`** — progress
callback: `ReportProgress(11)`, `SetParentHandle(1)`, `SetFlags(1)`.

**`IQCEnum` = `{001FA16A-E030-437E-8CF5-DF643A0F3B86}`** — enumerator:
`Next(1)`, `Reset(0)`.

**`IQCActionEnum` = `{001FA16A-E030-437E-8CF5-DF643A0F3B87}`** — action
enumerator: `Next(2)`, `Add(2)`, `Reset(0)`.

### Enums

- `tag_op_type` — 13 values, operation types
- `tag_rs_type` — 5 values, result types

## Harness status

| Step                                | Status |
|-------------------------------------|--------|
| Wine 32-bit prefix                  | ok     |
| `regsvr32 QCArch.dll`               | ok     |
| `LoadRegTypeLib` -> typeinfo dump   | ok     |
| `CoCreateInstance(QCQuikArch)` -> IUnknown  | ok |
| `QueryInterface(IQCQuikArch)` -> custom vtable | ok |
| Dump vtable[0..11] addresses         | ok     |
| Disasm Compress (vtable[8])         | partial - 6 args, mixed BSTR/I4 |
| Call Compress with 6 BSTRs          | 0x80004005 E_FAIL (param signature not yet pinned) |
| Capture real .qcf file               | **pending** (needs more disasm) |

### Why Compress fails

The disassembly at `0x10027d70` shows the function takes 6 explicit parameters
after `this`, accessed at `ebp+0xc, +0x10, +0x14, +0x18, +0x1c, +0x20`. It also
calls into an `IStream`-like object via vtable+0xc (likely `IStream::Read`).
This means at least one parameter is an `IStream*`, not a BSTR. The full
parameter order is:
- `arg0 (ebp+0xc)` = BSTR (path or filename)
- `arg1 (ebp+0x10)` = IStream* (used at `0x10027dba` for Read) ← **not a BSTR**
- `arg2..arg5` = various

To call successfully we need to:
1. Create an IStream over the input file (`CreateFileW` + `IStream::Create`)
2. Call Compress with the stream and a destination path

This is straightforward but requires another ~50 lines of harness code.
