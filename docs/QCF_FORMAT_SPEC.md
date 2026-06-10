# Especificación del formato `.qcf` / QCM (Choshuku / CAT)

> Spec **implementable** destilada de toda la RE (ver `RE_verified.md` para evidencia y
> direcciones). Todo lo marcado ✅ está validado contra muestras reales del motor original.
> Endianness: **little-endian**. Producto: Choshuku Professional v1.0.2 (SOURCENEXT /
> QuikCAT, 2003), motor `QCArch.dll`.

## 1. Visión general

Un `.qcf` es un **archivo contenedor**. El motor emite dos magics:
- **`QCM\x01`** (`0x014D4351`) — contenedor multi-miembro (lo que produce `CompressFile`). ✅
- **`QCF\x01`** (`0x01464351`) — header de **stream de un miembro**, embebido dentro del QCM. ✅

Estructura del archivo:
```
[QCM header (8B)] [stream miembro #1] [stream miembro #2] ... [directorio central "TOP"]
```
Para un solo archivo (caso `CompressFile`) hay 1 stream + el directorio con 1 entrada.

## 2. QCM header (contenedor externo) ✅
```
off  size  campo
+00  4     magic = "QCM\x01" (0x014D4351)
+04  4     offset del directorio central MENOS 4  (el directorio empieza en este valor + 4)
+08  ...   primer stream de miembro (empieza con un QCF header)
```

## 3. QCF header de stream de miembro (28 bytes) ✅
Embebido en el offset del miembro (p.ej. +0x08 para el primero). Verificado en 3 rutas de
código (`QCArch.dll`: reader IStream `0x1001f810`, writer `0x1001f960`, reader HANDLE `0x1001fa30`)
y el constructor `0x1001f7b0`.
```
off  size  campo
+00  4     magic = "QCF\x01" (0x01464351)
+04  4     campo específico del codec (deflate: 0 ; OLE2: tamaño original)
+08  4     campo específico del codec (deflate: tamaño comprimido del payload)
+0C  4     0
+10  4     constante 0x0011001E  (version/flags del codec)
+14  2     [+0x15] byte de sub-tipo de codec
+16  2
+18  1     CODEC: 0x00 = deflate/stream ; 0x01 = imagen (JPEG2000)   ✅
+19  1     0x05
+1A  1     0x04
+1B  1     ext_hdr_size (0..255)
+1C  N     ext header (N = byte[+1B]); el motor pone aquí la 1ª letra del nombre
+1C+N      payload comprimido
```
- **No hay timestamp ni CRC** en este header (la fecha vive en el directorio). ✅
- Payload deflate empieza con cabecera zlib `78 DA`. ✅

## 4. Directorio central ✅
Empieza en `QCM[+04] + 4`. Contiene un marcador raíz `"TOP"` y un registro por item:
```
... datetime DOS (4B) ... 03 00 00  "TOP"  DWORD = offset_directorio (= fin del payload)
por cada item:
  +00  4   count/atributo (observado 0x00000004)
  +04  1   tipo de item (0x02 = archivo)
  +05  4   datetime DOS (FileTimeToDosDateTime del momento de compresión)
  +09  4   tamaño ORIGINAL (descomprimido)            ✅
  +0D  1   longitud del nombre
  +0E  2   padding/flags (00 00)
  +10  L   nombre en UTF-8 (CP 65001)                  ✅
```
DOS datetime: `año=((d>>9)&0x7f)+1980, mes=(d>>5)&0xf, día=d&0x1f, h=(t>>11)&0x1f, min=(t>>5)&0x3f, seg=(t&0x1f)*2`.

## 5. Codecs (selección por tipo de entrada) ✅
El motor decide en tiempo de compresión. Byte `+0x18` del stream QCF: `1`=imagen, `0`=resto.

| Entrada | Backend | Algoritmo | Lossless |
|---|---|---|---|
| PNG, GIF, BMP, JPEG | CODEC4 (vía IMGCMP) | **JPEG2000** Part-1 (Kakadu) | NO (lossy) |
| TIFF, WAV, HTML, EXE, binario, ZIP | ZipDLL/zlib | **DEFLATE** (zlib 1.1.3) | sí |
| PDF | PdfProc | **DEFLATE** (zlib 1.1.3) | sí (contenido) |
| DOC/XLS/PPT | MSOC21 | OLE2 + **DEFLATE** (zlib 1.1.3) por-stream | contenido sí; .xls no byte-exacto |
| imágenes médicas | LFCMP13n | **LFC/CMW** (LEADTOOLS, propietario) | — |

**No hay compresión propietaria de Choshuku**: todo es zlib estándar o JPEG2000 (ambos públicos),
salvo LFC que es de LEADTOOLS (terceros). ✅

### Wrapper de imagen (26 bytes, antes del codestream J2K) ✅ implementado

El payload de un member de imagen (codec=1) es `[wrapper 26B][codestream J2K]`. Wrapper
(verificado diffeando 4 muestras del motor + reimplementado en `tools/catre_img.c`):
```
+00  1   tag (0x15 en salida del motor actual)
+01  1   0
+02  2   width  (u16 LE)
+04  2   height (u16 LE)
+06  2   bpp = 24
+08  8   0
+10  2   0x0001
+12  2   0x2000
+14  2   0
+16  2   lQuality (u16 LE, 0..100)   ← el parámetro de calidad
+18  2   0
```
Y en el QCF header del member-imagen: `inner+04` = tamaño del archivo fuente, `inner+18`=01
(codec), `inner+19`=01. `catre` produce esto con OpenJPEG y el **motor original lo decodifica**.

### JPEG2000 (imágenes) ✅
- Payload = wrapper de 26B (ancho/alto/bpp) + codestream J2K crudo (`FF4F FF51`).
- Config fija: Part-1, 1 tile, 3 comps RGB, MCT on, 5 niveles, 2 quality layers, LRCP, wavelet
  9/7 irreversible (**lossy**).
- `lQuality` (0..100) = **target de tasa del asignador PCRD** (no cambia COD/QCD; controla cuántos
  coding-passes por code-block). Curva empírica ≈ `bytes ≈ 1287·q + 45000` (foto 1215×1080).

### Office / OLE2 (`MSOC21`) ✅ implementado (variante whole-file)

El codec Office tiene **dos variantes** de payload:
- **whole-file** (OLE2 genérico / no reconocido como Word/Excel): header de 36 bytes +
  `zlib(archivo OLE2 entero)`. Reverseado e implementado; el motor original decodifica la salida
  de `catre` **byte-exacto**.
  ```
  header 36B = 32 01 12 00 00 00 | 33 02 | u32 zlib_size | 00×6 | 04 0a 00 05 | tail14
  ```
  El QCF header del member-office: `inner+04`=tamaño fuente, `inner+08`=**0** (el tamaño real
  está en el header MSOC21+8), tail8 = `01 00 04 00 00 00 02 01`. El `tail14` el motor lo quiere
  presente y no-cero pero **no lo valida** contra el contenido (constante sirve).
- **per-stream** (Word/Excel *reconocidos*): header `32 01 aa ...`, sin un único zlib. **Localizado
  pero NO reimplementado** (formato custom complejo). En `MSOC21.dll`: `FUN_10068200` = wrapper
  `uncompress` (inflate "1.1.3"); su único llamador `FUN_10040960` = el decompresor formato-B
  (switch por tipo en `+0x19`, byteswaps big-endian de width/height, índice de chunks
  `[u16 tag][u32 size]` — p.ej. 0x198/0x199/0x133/0x134 con tamaños 99/395/4141/365, descomprime
  cada chunk por offset). Raw-inflate directo del payload NO funciona (framing custom). Reimplementar
  requiere trazar esa función densa + validar con muestras — desproporcionado para el valor (la
  variante whole-file ya cubre el caso común). Entradas documentadas para retomar.

### Office (ratios reales medidos) ✅
`.doc`/`.xls`/`.ppt` reales → **~7-26% del original** (ahorro 74-93%). Excelente por la redundancia
del binario Office. `.xls`: el motor altera ~0.17% del stream Workbook (campo FONT) → no byte-exacto.

## 6. Cómo manejar el motor (COM, bajo Wine) ✅
- Coclass `QCQuikArch` CLSID `{7F0B34D0-D90A-49E9-9212-31349D545F4B}`.
- `QueryInterface(IID_IQCSingleFileArch {F1FE45A8-9619-45F0-ACE3-11C3F14E32BA})` → dispinterface.
- `CompressFile(BSTR src, BSTR destArch, BSTR fileName, long lQuality, long *plCompType)` (dispid 1).
- `DecompressFile(BSTR srcArch, BSTR destFile, long *plCompType)` (dispid 2).
- Requiere `regsvr32` de los backends (`MStream/CODEC4/IMGCMP/PdfProc/MSOC21`) + `ZipDLL`/`UnzDLL`/
  LEADTOOLS en `system32`. Harness: `harness/sfa.c`.

## 6.6 Multi-archivo ✅ RESUELTO Y VALIDADO

Reverseado de una muestra real de 5 archivos (`tests/fixtures/real_qcf/multi/Choshuku.qcf`).
Implementado en `qcf_tool.qcm` (lee y escribe); el **motor original descomprime** un multi-archivo
hecho por nuestro código (`DecompressFile` hr=0, miembro byte-idéntico).

```
+00  QCM header (8B): magic "QCM\x01" + [+04] = (fin del stream 1) - 4
+08  Stream 1: QCF header(28) + ext(1) + payload     ← SIN prefijo
     Stream 2..N: [u32 chunk_size = 4+28+ext+comp] + QCF header(28) + ext(1) + payload
     ...
     Directorio central (al final):
       9 bytes 00
       datetime DOS (4B)
       4 bytes 00
       [namelen=3][00 00] "TOP"                       ← entrada raíz
       por cada item (archivos Y carpetas):
         parent_off (4)      = offset (archivo) del record PADRE; raíz = offset de "TOP"
         stream_offset (4)   = (offset del QCF header del miembro) - 4  [archivo]; 0 si carpeta
         type (1)            = 0x02 archivo  |  0x00 carpeta
         datetime DOS (4)
         original_size (4)   = 0 si carpeta
         namelen (1)
         pad (2) = 00 00
         name (UTF-8)        = nombre del componente (NO la ruta completa)
```
**Jerarquía de carpetas** ✅ (validado con muestra `ChoshukuV2.qcf`: `XD/nocreo.txt`,
`XD/JAJA/jajaja.txt` a 2 niveles): cada record apunta a su carpeta padre vía `parent_off`
(el offset absoluto del record de la carpeta; los items de nivel raíz apuntan al offset de
"TOP"). Las carpetas son records `type=0x00` sin stream. La ruta completa se reconstruye
siguiendo `parent_off` hacia arriba. En flat/single-file todos los items tienen
`parent_off = offset("TOP")` (por eso parecía constante).

Cada record referencia su stream por `stream_offset` (= hdr-4), así el orden de records es
independiente del orden físico de streams. Single-file = caso N=1. La receta COM del motor para
*producir* multi (Create+vtable[12]) quedó en `RE_verified.md` §8.5.1, pero ya no hace falta:
lo producimos nosotros y el motor lo lee.

## 6.5 Validación END-TO-END de la spec ✅

La spec del camino deflate está **probada produciendo `.qcf` válidos**:
1. `qcf_tool.qcm.build_qcm_deflate(data, name)` construye un `.qcf` desde cero.
2. **Reproduce `tests/fixtures/real_qcf/in.txt.qcf` byte-a-byte** (test automatizado).
3. **El motor original Choshuku descomprime un `.qcf` hecho por nuestro código** y devuelve
   el contenido **byte-idéntico** (harness `harness/dec.exe` → `DecompressFile` hr=0). 
   → No solo leemos el formato: lo escribimos y el software original lo acepta.

## 7. Estado de cobertura
| Área | Estado |
|---|---|
| Formato QCM/QCF single-file | ✅ 100% (validado) |
| Directorio central | ✅ (campos clave validados) |
| Codecs (zlib + JPEG2000) | ✅ ~100% (estándar, reproducibles) |
| API COM compress/decompress | ✅ funcional |
| Multi-archivo | ✅ **100%** — reverseado, leído, escrito y validado (muestra real de 5 archivos) |
| LFC (LEADTOOLS) | fuera de alcance (IP de terceros, médico) |
| Shell/preview handlers, constante Kakadu | bajo valor / no hecho |
