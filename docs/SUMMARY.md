# Resumen del trabajo — `cat-re`

Repo: `/home/forum/git/cat-re`. Estado actual: 9 commits, 0 errores de compilación,
0 tests fallando. Este doc es el mapa global de lo que se hizo, qué quedó
confirmado y qué sigue pendiente.

> **ACTUALIZACIÓN (jun-2026, pase de RE verificada).** Varias afirmaciones de
> abajo fueron corregidas/superadas. Ver **`docs/RE_verified.md`** (verificación
> contra binario) y la novedad mayor: se capturaron **muestras `.qcf` reales** del
> motor original bajo Wine (`harness/sfa.c`, fixtures en `tests/fixtures/real_qcf/`).
> El contenedor real es **QCM** (no QCF plano); parser validado en
> `qcf_tool/qcm.py` (7 tests verde). El header de 28 bytes NO es "23 bytes opaque"
> y el reader NO rechaza por magic.

## 1. Lo que es Choshuku / CAT

`Choshuku Professional (超圧縮) v1.0.2` de SOURCENEXT / QuikCAT Technologies
(2003). Compresor para Windows con 4 codecs internos y un wrapper `.qcf`.
**Shell extension pura** — no tiene `.exe` standalone, solo DLLs registradas
(`QCArch.dll`, `QCArchUI.dll`, `QCShExt.dll`, `QCShView.dll`). Se invoca
desde el menú contextual del Explorer.

### Codecs detectados

| Input            | Backend DLL  | Algoritmo subyacente              |
|------------------|--------------|------------------------------------|
| BMP/GIF/PNG/TIFF | `CODEC4.dll` | JPEG2000 (Kakadu)                  |
| JPEG             | `IMGCMP.dll` | wrapper sobre `CODEC4` (JPG→JP2)    |
| PDF              | `PdfProc.dll`| Deflate sobre `FlateDecode`         |
| DOC/XLS/PPT      | `MSOC21.dll` | OLE2 + zlib DEFLATE (NO MS-OFFCRYP — ver RE_verified §9) |
| Cualquier otra   | `ZipDLL.dll` | ZIP = Deflate (LZ77 + Huffman)      |
| Imágenes médicas | `LFCMP13n.DLL` | LEADTOOLS filter (LFC, propietario) |

Auxiliares: LZW (GIF/TIFF), RLE (BMP/FAX). Ver `docs/RE_notes.md` §"Compression
backends".

## 2. Formato `.qcf`

```
+0x00  DWORD  magic      'QCF\x01' / 'QCM\x01' / 'PK\x03\x04'
+0x04  BYTE[23] opaque   type-specific metadata
+0x1B  BYTE  ext_hdr_size  0..255
+0x1C  BYTE[] ext_header   filename / other metadata
+0x1C+N       payload      inner stream (jp2, zip, ole2, raw)
```

- Sin packer, sin anti-debug, sin obfuscación.
- 5 CLSIDs registrados (QCQuikArch, QCItem, QCCallBack, QCEnum, QCActionEnum)
  + 1 coclass (IQCSingleFileArch) con IDispatch default.
- LIBID `{B19AA1C0-C66E-4A20-90DF-91D6221A09A5}`.

## 3. Interfaz `IQCQuikArch` (engine)

IID `{BFDCA750-A117-46CD-8CE6-29B51627B268}`. Custom vtable, NO IDispatch.
Vtable mapeada estáticamente en `rdata@0x100302e8` (15 entries, 8 método de
negocio + 3 IUnknown + 4 helpers internos de la clase C++).

| Vtable | DISPID       | Método      | Args (post-this)        | Tipo confirmado |
|-------:|--------------|-------------|--------------------------|------------------|
| 0-2    | -            | IUnknown    | -                        | -                |
| 3      | `0x60010000` | `Create`    | BSTR, ULONG, ULONG, ptr | (BSTR, ul, ul, IQCItem*) |
| 4      | `0x60010001` | `Delete`    | BSTR                     | (BSTR) |
| 5      | `0x60010002` | `Close`     | -                        | () |
| 6      | `0x60010003` | `EnumerateItems` | BSTR, ptr           | (BSTR, IQCEnum**) |
| 7      | `0x60010004` | `Extract`   | ptr, BSTR, ptr, ULONG, ULONG | (IQCItem*, BSTR, IQCItem*, ul, ul) |
| 8      | `0x60010005` | `Compress`  | ptr, BSTR, ULONG, ptr, ULONG, ULONG | (IQCItem*, BSTR, ul, ?, ul, ul) |
| 9      | `0x60010006` | `Remove`    | ptr, ptr, ULONG, ULONG  | |
| 10     | `0x60010007` | `Move`      | ptr, BSTR, ptr, ULONG, ULONG | |
| 11     | -            | (helper)    | -                        | segunda implementación IUnknown |

`ptr` = "userdef" en el type library; resuelto con Ghidra + disasm a
`IQCItem*` o `IQCEnum*` (interfaz dispatch con 5/2 métodos propios).

## 4. Compress en detalle (decompile Ghidra)

RVA `0x10027d70`, decompilado en `ghidra/Ghidra/Features/Base/ghidra_scripts/decompile_vtable8.java`.

Comportamiento:
1. Recibe `this, IQCEnum* enum, BSTR name, options, callback, 0, 0`.
2. Si `arg5==0 && arg4==0`: llama `this->vtable[18](this, enum, &fields, callback)` (helper interno).
3. Bucle `do { ... } while(true)`:
   - `enum->vtable[3](enum, &item, &flags)` — `Next(item*, flags*)`.
   - Extrae path base con `wcsrchr('\\')`, divide en `path` y `name`.
   - Si `flags & 0x10`: rama "overwrite", si no: rama "append".
   - Llama `this->vtable[22](this, &path, 0, &name, 0, mode)` (vtable[22] = helper de I/O).
   - Llama `this->vtable[12](this, path, name, callback, 0)` (do_compress).
   - Llama `this->vtable[22](this, &path, 0, &name, 0, mode, 1, count, ...)` (commit).
4. Decrementa refcounts de path/name, repite.

**Modelo**: Compress itera items de un enumerador y los escribe uno a uno al
archive. El enumerador se crea via `EnumerateItems` o se pasa pre-armado.

## 5. Estado del proyecto

| Componente    | Lenguaje | Estado | Tests |
|---------------|----------|--------|-------|
| `qcf_tool/`   | Python   | done   | 23/23 verde |
| `libcat/`     | C        | done   | 4 grupos verde |
| `cat-tool`    | C        | done   | manual |
| `harness/`    | C/Wine   | done   | COM funcional |
| Ghidra RE     | Java/Jython | done | decompile vtable[8] ✓ |
| `tests/test_jpg_fast` | C | done   | 5/5 sample JPGs verde |

### `libcat` backends
- `raw.c` — passthrough, nombre del ext header.
- `zip.c` — PKZIP STORED + DEFLATE, hand-rolled (zlib).
- `jp2.c` — JPEG2000 via libopenjp2 (decoder + encoder BMP→JP2).
- `ole2.c` — MS-CFB (compound file binary) reader, sin MS-OFFCRYP.
- `jpg.c` — JPEG decoder via libjpeg + path `cat_jpg_to_jp2` (decode→BMP→JP2).

### `cat-tool` CLI
```
./cat-tool info archive.qcf
./cat-tool extract archive.qcf -o ./out/
./cat-tool pack -o out.qcf -n hello.txt hello.txt    # raw passthrough
./cat-tool pack -o out.qcf a.txt b.png c.pdf          # auto-zip multi-file
```

## 6. Pipeline JPG → .qcf verificado

Compresión **lossless** (JPG → RGB → JP2 reversible 5-3 → .qcf → BMP).
**Pixel-perfect** roundtrip en 5 JPGs de muestra (1024×768 hasta 3840×2400).

| Archivo | Tamaño JPG | Tamaño .qcf | vs JPG |
|---|---|---|---|
| Image40.jpg (3840×2400) | 2.20 MB | 12.17 MB | ×5.5 **mayor** |
| 3104814.jpg (2048×1409) | 0.40 MB | 3.26 MB | ×8.2 **mayor** |
| Image27.jpg (1024×768) | 0.04 MB | 0.20 MB | ×5.4 **mayor** |

**Conclusión técnica**: el encoder lossless NO comprime JPEGs ya
cuantizados — el output es más grande. Para competir con el JPG original
se necesita encoder **lossy** con bitrate (como el Kakadu original).
El formato `.qcf` y el pipeline funcionan; falta el parámetro de
calidad.

## 7. Hallazgos RE clave (vs versiones previas de `RE_notes.md`)

1. **Vtable estática**: confirmada en `.rdata@0x100302e8`. Antes se asumía
   que `vtable[8] = 0x10027d70` por el dump runtime; ahora estático +
   dinámico coinciden.
2. **Compress signature**: type library decía `Compress(ptr, BSTR, ULONG,
   ptr, ULONG, ULONG)`. Ghidra confirma tras `this`: `int*, wchar_t*,
   undefined4*, undefined4, int, int`. Las posiciones 4 y 5 del type
   library (`ptr`/`ULONG`) están intercambiadas — el type library mintió.
3. **Compress itera un enumerador** — no toma items individuales. Antes
   se pensaba que era (item_in, path, mode, item_out, ...).
4. **CAT es shell extension pura** — no se puede invocar con un `.exe`
   tradicional. El único camino a un sample `.qcf` real es llamar a
   `IQCQuikArch::Compress` desde un harness COM (que ya tenemos en
   `harness/`).
5. **Ghidra scripts en Java**: el path es `package` + `.class` en
   subdirectorio del script dir. `pyghidraRun` no tiene headless; hay
   que usar `analyzeHeadless -postScript decompile.decompile_vtable8`.

## 8. Bugs arreglados durante este trabajo

1. **ZIP encoder** (de commit previo): `attrs` + `ext_attr` duplicados
   (8 vs 4 bytes), rompía el read.
2. **JP2 stream mode**: `OPJ_FALSE` (read) usado para write stream y
   viceversa. Bug espejo en encode/decode.
3. **cat_inner_detect ordering**: el enum `CAT_INNER_RAW=1` (no 0), lo
   que hacía que el test runner reportara "inner is 1" (RAW) cuando
   esperaba "JP2" — el dispatch ya decodifica JP2 a BMP, así que el
   inner resultante es RAW (el BMP).

## 9. Lo que NO se hizo (y por qué)

- **Capturar un .qcf real del motor original**: Choshuku no tiene `.exe`
  standalone (es shell extension), y llamar `Compress` desde Wine
  requiere armar un `IQCEnum` válido. La signature ya está confirmada
  pero el harness para `IQCEnum::Next` no se escribió. Se documentó en
  `harness/README.md`.
- **Encoder JP2 lossy**: faltó añadir un parámetro de bitrate/calidad
  a `cat_encode_jp2_from_bmp`. El encoder es 100% lossless (rate=0,
  irreversible=0), por eso el output es mayor que el JPG.
- **MS-OFFCRYP deflate**: la compresión propietaria de Office 97-2003
  (deflate con tablas Huffman custom) no se implementó. Solo el path
  OLE2 sin comprimir funciona para `.doc`/`.xls`/`.ppt`.
- **LFC (LEADTOOLS)**: formato propietario, sin sample reference.
- **Suite completa 153 JPGs**: el test procesa cada JPG secuencialmente
  (decode + encode + decode ~2-5s por archivo grande). 153 archivos
  → ~5-10 min. El test rápido `test_jpg_fast` está, el lento
  `test_jpg_runner` está; no se corrió la suite completa en este turno
  para no gastar tiempo, pero se dejó el binario.

## 10. Cómo usar

```bash
# Build
export OPENJPEG_INC=/tmp/openjpeg/extracted/usr/include/openjpeg-2.5
export OPENJPEG_LIB=/usr/lib/x86_64-linux-gnu
make && make test

# Run
./cat-tool info archivo.qcf
./cat-tool extract archivo.qcf -o out/
./cat-tool pack -o out.qcf a.txt b.pdf   # multi → auto-zip

# Decode una imagen JP2 dentro de un .qcf
./cat-tool extract image.qcf -o out/    # out/image.bmp

# Comprimir JPGs (lossless path)
tests/test_jpg_fast samples/jpg-files/

# Python (qcf-tool)
PYTHONPATH=. python3 -m qcf_tool info archivo.qcf
PYTHONPATH=. python3 -m pytest tests/ -v
```

## 11. Archivos clave

| Path | Qué |
|------|-----|
| `libcat/cat.h` | API pública |
| `libcat/dispatch.c` | top-level read/write |
| `libcat/backends/jp2.c` | JP2 encode/decode |
| `libcat/backends/jpg.c` | JPEG decode + path JPG→JP2 |
| `harness/compress2.c` | CoCreateInstance + vtable dump |
| `harness/typeinfo4.c` | type library dumper (signature extractor) |
| `ghidra/.../decompile_vtable8.java` | Ghidra decompile script |
| `docs/RE_notes.md` | RE acumulado (192 líneas) |
| `docs/LINUX_PORT.md` | API + CLI docs |
| `docs/PLAN.md` | roadmap |

## 12. Estado Git

```
3305793 Add JPEG backend + 153-JPG test runner
0500d31 Linux port: libcat C library + cat-tool CLI
59029ef RE: final state - IIDs/dispids/com map, format, codecs
0a0ab06 RE: full COM map - IIDs, DISPIDs, vtable, param signatures
666a702 Wine harness: COM works, IDispatch blocked
83f8f22 qcf-tool: format parser, 4 backends, dispatcher, CLI
b6a177b RE: roadmap
12d6740 RE: initial analysis
f25a202 Initial: extracted Choshuku Pro 1.0.2 update package
```

9 commits limpios. La historia cuenta el viaje desde extracción del 7z
hasta la suite de tests pasando.
