# Sesión autónoma (noche 2026-06-09 → 06-10)

Trabajo hecho en "modo autómata" mientras dormías. Todo verificado contra el motor
original bajo Wine o desensamblado. Detalle completo en `docs/RE_verified.md` §8.

## Resultados (qué quedó cerrado)

### ✅ Tabla completa del dispatcher de codecs
Comprimí un corpus de tipos variados (png/gif/tiff/bmp/wav/html/pdf/zip/exe/binario/OLE2):

- **Imagen lossy (JPEG2000)**: PNG, PNG-paleta, GIF, BMP → recomprimidos **con pérdida**.
- **Deflate lossless**: TIFF (¡no va a imagen!), WAV, HTML, PDF, ZIP, EXE, binario.
- **OLE2** (`.doc`/`.xls`): pasa por `MSOC21`, preserva contenido pero re-serializa (no byte-exacto).
- El byte discriminador es `inner+0x18`: `1`=imagen, `0`=stream/deflate.
- **Gotcha**: formatos normalmente lossless (PNG/GIF/BMP) se degradan al pasar por Choshuku.

### ✅ Codestream JPEG2000 + mapeo de lQuality
Config fija (Part-1, 1 tile, RGB, MCT on, 5 niveles, 2 layers, LRCP) con wrapper de 26 bytes
(ancho/alto/bpp). `lQuality` solo cambia el **bitrate** (truncamiento EBCOT), no la estructura.

### ✅ Campos del header confirmados
- **No hay timestamp ni CRC** en el header QCF interno (la fecha vive solo en el directorio).
- `inner+0x10 = 0x0011001e` = constante (version/flags).
- `ext_header` (1 byte) = **primera letra del nombre** (confirmado en 6 muestras).
- Los 5 DWORDs son **específicos del codec** (deflate: +0x08=tam comprimido; OLE2: +0x04=tam original).

### ✅ DecompressArchiveW (export C de QCShExt)
`__stdcall`, 4 args, usa una CLSID interna. Redundante (el path COM `DecompressFile` ya descomprime).

### 🟡 Multi-archivo — avanzado, no cerrado
- vtable real de `IQCQuikArch` mapeada (`0x100302e8`), `Create`/`Compress`/`Close` localizadas.
- **Contrato `IQCEnum` RESUELTO** (lo que el proyecto previo abandonó): implementé un `IQCEnum`
  en C (`harness/multifile.c`) y el motor llamó `Next` consumiendo los paths (items = BSTR).
  `Create(path,0,1,NULL)` funciona (crea archivo nuevo).
- **Bloqueo**: `Compress` devuelve E_FAIL al procesar items. La receta exacta está dentro del
  dispatch de `IDispatch::Invoke` (los slots de vtable de CompressFile son stubs). Trazar eso
  es el siguiente paso.

## Archivos nuevos/tocados esta sesión
- `docs/RE_verified.md` §8 (tabla de codecs, JP2, campos, multi-archivo)
- `harness/multifile.c` (scaffold IQCEnum funcional, Compress pendiente)
- `harness/decompress_export.c` (análisis estático del export)
- `resultados-jpg-test/` (comparaciones visuales; foto pequeña + 4000×3000 de internet)
- `ghidra/scripts/DecompBatch.java` (script de decompilación reutilizable)

## Qué falta para el 100% (todo deep-dig o bajo valor)
1. Multi-archivo: trazar el switch de DISPID en `Invoke` (`0x10001cb0`) → receta de `Compress`.
2. Internos de codecs propietarios: `MSOC21` (MS-OFFCRYP), `PdfProc`, `LFC`/LEADTOOLS.
3. Reimplementar el **encoder** QCM en `libcat`/`qcf_tool` (ahora hay muestras reales para validar).

---

# Sesión autónoma 2 (06-10) — codecs internos + experimento pll

## Experimento packJPG-style (carpeta `pll/`) — CERRADO
Recompresor JPEG **lossless** propio (extrae coeficientes DCT con libjpeg → reorg → entropy).
Progresión: brotli ~10% → aritmético+contexto ~14% → +predicción 2D DC ~15% de ahorro.
Comparado contra el **packJPG del usuario** (v4.0d, /usr/bin/packjpg): packJPG gana (~20%).
Conclusión: el gap es el modelo de contexto maduro de packJPG. Round-trip byte-exacto verificado.
Detalle en `pll/README.md`. (El usuario dio por terminado el experimento.)

## Codecs internos — RESUELTO (corrige notas viejas)
RE estático de los backend DLLs (strings + imports):
- **MSOC21 (Office) = zlib 1.1.3 DEFLATE estático** sobre streams OLE2. La idea de "MS-OFFCRYP"
  propietario era **FALSA** (las strings de crypto son la tabla de errores HRESULT estándar).
- **PdfProc / MStream = zlib 1.1.3** también.
- **IMGCMP = JPEG2000** (delega a CODEC4/Kakadu).
- **LFCMP13n = LEADTOOLS comercial** (formato CMW/LFC propietario) — NO es IP de Choshuku, fuera
  de alcance (médico, sin muestras).
- **Meta-conclusión**: Choshuku **no tiene algoritmo de compresión propio**; todo son librerías
  de terceros (zlib, Kakadu, LEADTOOLS). Su IP es el formato `.qcf`/QCM, que está al 100%.
  → **Algoritmos al ~100%** salvo LFC (de LEADTOOLS). Ver `docs/RE_verified.md` §9.

## Capstone: spec VALIDADA escribiendo .qcf ✅
- `docs/QCF_FORMAT_SPEC.md`: spec implementable consolidada.
- `qcf_tool.qcm.build_qcm_deflate()`: encoder QCM propio. **Reproduce un .qcf real byte-a-byte**
  y, lo definitivo, **el motor original descomprime un .qcf hecho por nuestro código** y devuelve
  contenido idéntico (`harness/dec.exe`). No solo leemos el formato — lo escribimos y el software
  original lo acepta. 32 tests verde.
- Office real medido (Apache POI): ratios ~7-26%, `.xls` no byte-exacto (campo FONT). Fixtures en
  `tests/fixtures/real_office/`.

## Estado de la RE al cierre
- Formato `.qcf`/QCM single-file: **100%** (validado con muestras reales).
- Algoritmos de compresión: **~100%** (zlib + JPEG2000, ambos estándar) salvo LFC (terceros).
- Multi-archivo: contrato `IQCEnum` resuelto; receta de `Compress` parkeada (rabbit-hole ATL).
- Pendiente real bajo: shell/preview handlers (UI), constante exacta `lQuality→rate` (Kakadu).
