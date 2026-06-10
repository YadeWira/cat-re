# RE verificada — `.qcf` / QCArch.dll (pase de revisión)

> Este documento **verifica contra el binario real** (`original/DLLs/1_Core_Engine/QCArch.dll`)
> las afirmaciones de `RE_notes.md` / `SUMMARY.md`. Todo lo que sigue está respaldado
> por desensamblado (objdump) y/o decompilación Ghidra, con la dirección citada.
> Base de carga: `0x10000000`. Proyecto Ghidra: `ghidra/ghidra_projects/QCArch`.
> Script: `ghidra/scripts/DecompBatch.java`.

## Método

- `objdump -d` para verificación instrucción-a-instrucción.
- Ghidra headless (`analyzeHeadless ... -process QCArch.dll -postScript DecompBatch.java`)
  para decompilación C de las funciones clave.
- **No existe ningún `.qcf` de muestra** en el repo, el instalador 7z, el manual ni
  fixtures. Por tanto el formato está derivado **solo del código**, no validado contra
  bytes reales. Esto se marca explícitamente abajo.

## 1. Layout del header — CONFIRMADO (3 rutas de código independientes)

El header se lee/escribe en un buffer en el objeto contenedor en `this+0x204`.
Tres funciones lo manejan idénticamente:

| Func | RVA | Qué hace |
|------|-----|----------|
| `read_header (IStream)` | `0x1001f810` | `IStream::Read(this+0x204, 0x1C, &n)` vía vtable+0xC |
| `write_header (IStream)`| `0x1001f960` | `IStream::Write(this+0x204, 0x1C+ext, &n)` vía vtable+0x10 |
| `read_header (HANDLE)`  | `0x1001fa30` | `ReadFile(h, this+0x204, 0x1C, &n, 0)` |

Las tres: leen/escriben **28 bytes (0x1C)** fijos; comprueban `n == 0x1C`; y **solo si**
`byte[0x1B] != 0` **y** `magic == 0x01464351` leen/escriben el ext header en `this+0x220`
(offset de archivo 0x1C), de longitud `byte[0x1B]`.

### Estructura exacta (del constructor `FUN_1001f7b0` @ `0x1001f7b0`)

El constructor inicializa el struct campo por campo — esto da el layout sin ambigüedad
(`param_2` = objeto base; `param_2[0x81]` = `+0x204`):

```
offset  tipo    init   significado (ver §2 para los que se rellenan al comprimir)
+0x00   DWORD   QCF    magic = 0x01464351  ("Q","C","F",0x01)   [param_2[0x81]]
+0x04   DWORD   0      campo1
+0x08   DWORD   0      campo2
+0x0C   DWORD   0      campo3
+0x10   DWORD   0      campo4
+0x14   DWORD   0      campo5
+0x18   BYTE    0      b18
+0x19   BYTE    0      b19
+0x1A   BYTE    0      b1A
+0x1B   BYTE    0      ext_hdr_size (0..255)
= 28 bytes (0x1C)
```
Tras el header, el constructor zerea **256 bytes** en `this+0x220` (buffer del ext
header; máx 255 cabe). El buffer del ext header guarda típicamente el **nombre de
archivo**.

> **Corrección a `RE_notes.md`**: los "BYTE[23] opaque" son en realidad
> **5 DWORDs + 3 bytes + ext_size**. Y la afirmación "el reader rechaza si magic != QCF"
> es **falsa**: el reader devuelve el `HRESULT` del `Read`; un magic incorrecto solo
> *omite* la lectura del ext header (loguea E_FAIL vía un trace helper en `0x100130d6`,
> pero no aborta).

## 2. Los 5 DWORDs NO son opaque — se rellenan al comprimir

Hay un cluster de funciones (`0x10020df0`, `0x10020ee0`, `0x10020f90`, `0x10021070`)
que pueblan un **registro de item** durante la compresión. De su decompilación:

- **Nombre en UTF-8**: usan `GetACP` (`PTR_FUN_1003e21c`) y luego
  `WideCharToMultiByte(CP_UTF8 = 0xFDE9 = 65001, ...)` para convertir el nombre wide
  → UTF-8. El nombre se copia a `+0x224` del registro.
- **Timestamp DOS**: `GetSystemTimeAsFileTime` + `FileTimeToDosDateTime` escriben
  fecha/hora estilo DOS (igual que ZIP) en pares de WORDs del registro.
- Campos numéricos (tamaño, offset) se escriben en los DWORDs (`+0x208`, `+0x20c`,
  `+0x210`, `+0x214`, `+0x218`).

`FUN_10022720` @ `0x10022720` es un **seek de sub-stream** y revela el rol de tres
campos en el wrapper de stream:
- `+0x218` = offset base del payload
- `+0x210` = longitud del payload
- `+0x20c` = posición actual
- `+0x21c` = byte de flags (bit 1 se setea con `orb 0x1`, bit 2 se testea → "no crecer/solo-lectura")
- llama `IStream::Seek` (vtable+0x14).

> **Confianza**: el *hecho* de que estos campos guardan tamaños/offsets/datetime/nombre
> está confirmado por código. El *mapeo byte-exacto* de cada DWORD del header de 28 bytes
> a (origSize / compSize / codecId / flags / crc) **no puede fijarse al 100% sin un
> `.qcf` real**, porque el registro de item (`+0x224...`) y el header de contenedor
> (`+0x204`) son estructuras distintas que comparten rangos de offset.

## 3. El motor maneja QCF **y** ZIP en el mismo objeto — CONFIRMADO

`FUN_10020000` @ `0x10020000`:
```c
if (*(int*)(obj+0x670) == 0x01464351)  return *(byte*)(obj+0x423);  // QCF
if (*(int*)(obj+0x670) == 0x04034B50)  return *(byte*)(obj+0x566);  // PK\x03\x04 (ZIP)
return 0;
```
Una clase derivada (`FUN_1001fd10` @ `0x1001fd10`) embebe **dos** headers QCF: uno en
`+0x204` y otro en `+0x670`. El segundo es el que se compara contra QCF/ZIP en docenas de
sitios (`grep 0x1464351 ... 0x670`). Probable: contenedor de entrada vs salida, o
variante multi-volumen (`QCM`).

> **Implicación para el port**: en `libcat/format.c`, `cat_header_parse` mete `ZIP`
> (`PK\x03\x04`) por el mismo parser de 28 bytes y rechaza con `BADMAGIC` cualquier otra
> cosa. El motor real (a) **no** rechaza por magic y (b) trata un ZIP como ZIP desde el
> byte 0 (vía `UnzDLL`/`ZipDLL`), **no** como un header de 28 bytes + ext. Conviene separar
> el path ZIP del parser del header QCF.

## 4. `Compress` (vtable[8]) — firma CONFIRMADA, type library mintió

`FUN_10027d70` @ `0x10027d70`. Firma Ghidra:
```c
void __stdcall Compress(wchar_t *path, int *enum, wchar_t *name,
                        undefined4 *opts, undefined4 callback, int z1, int z2);
```
- Itera un **enumerador** (`enum->vtable[0xC]` = `Next(&item,&flags)` en bucle `do{}while(true)`).
- Si `flags & 0x10` → rama "overwrite" (llama vtable+0x3c), si no → rama "append" (vtable+0x30).
- Usa `vtable+0x58` como helper de I/O de header (con args distintos para abrir/commit).
- `wcsrchr(path, '\\')` parte path/nombre.

Esto coincide con el modelo de `SUMMARY.md §4`. **Pero** los exports a explotar son más
simples (ver §5).

## 5. Exports reales (tabla PE, verificada)

| DLL | Exports (además de los 4 COM estándar) |
|-----|----------------------------------------|
| `QCArch.dll`  | *(solo)* DllCanUnloadNow, DllGetClassObject, DllRegister/UnregisterServer |
| `QCShExt.dll` | **`DecompressArchive`** (`0x1e560`), **`DecompressArchiveW`** (`0x1e490`), `ShowProp` (`0x1e290`) |
| `QCShView.dll`| solo COM estándar |
| `QCArchUI.dll`| `ShowProp` (`0x26130`), `ShowPropWithLicenseCheck` (`0x25fa0`) |

> **Camino más corto a ground truth**: `QCShExt.dll!DecompressArchiveW` es una función C
> exportada (no COM) que descomprime un `.qcf`. Driblando esto bajo Wine podemos validar
> la lectura sin armar toda la coreografía COM. Para *producir* un `.qcf` sigue haciendo
> falta el path `Compress` (COM, con un `IQCEnum`).

## 6. Estado vs `RE_notes.md` / `SUMMARY.md`

| Afirmación previa | Veredicto |
|---|---|
| Header 28 bytes, ext en 0x1B | ✅ confirmado (3 rutas) |
| Magic en +0x00 = `QCF\x01` | ✅ confirmado |
| "BYTE[23] opaque" | ⚠️ refinado → 5 DWORDs + 3 bytes + ext_size |
| "reader rechaza si magic != QCF" | ❌ falso (solo omite ext header) |
| Sin packer / anti-debug | ✅ confirmado |
| `Compress` itera enumerador | ✅ confirmado |
| Firma `Compress` (6 args, posiciones 4/5 intercambiadas) | ✅ confirmado: 7 params `__stdcall`, itera enum |
| Filename/timestamp en el formato | 🆕 nuevo: filename **UTF-8 (CP 65001)**, datetime **DOS** |
| QCF y ZIP coexisten en el motor | 🆕 nuevo: confirmado en `FUN_10020000` |

## 7. GROUND TRUTH — muestras `.qcf` reales capturadas ✅

**Logrado.** Se generaron las primeras muestras `.qcf` reales del proyecto ejecutando el
motor original bajo Wine. Camino usado (no la coreografía COM completa):

1. `QCQuikArch` (CLSID `{7F0B34D0-...}`) **sí** responde a `QueryInterface(IQCSingleFileArch
   {F1FE45A8-...})` — un **dispinterface** IDispatch de alto nivel.
2. Método `CompressFile` (dispid=1):
   `CompressFile(BSTR src, BSTR destArch, BSTR fileName, long lQuality, long *plCompType)`.
   `DecompressFile` (dispid=2): `(BSTR srcArch, BSTR destFile, long *plCompType)`.
3. **Requisito clave**: hay que `regsvr32` los backends COM (`MStream`, `CODEC4`, `IMGCMP`,
   `PdfProc`, `MSOC21`) y copiar `ZipDLL`/`UnzDLL` + LEADTOOLS a `system32`. Sin esto,
   `CompressFile` escribe solo el header+directorio y aborta con `E_FAIL` (lo que parecía un
   "bug" era backends no registrados).

Harness: `harness/sfa.c`. Muestras: `tests/fixtures/real_qcf/`.
Round-trip **lossless verificado** para texto y binario (60/20000/4096 bytes idénticos).

### Formato real (de diffear 3+ muestras)

El contenedor que emite `CompressFile` es **`QCM\x01`** (no `QCF`). Estructura tipo-archivo:

```
[QCM header] [stream del miembro: QCF header + payload] ... [directorio central "TOP"]
```

**QCM header (externo):**
```
+0x00  DWORD  magic = 0x014D4351  "QCM\x01"
+0x04  DWORD  = 0x21 + tamaño_payload_comprimido  (apunta cerca del fin del stream)
+0x08  ...    aquí empieza el primer stream de miembro (header QCF embebido)
```

**QCF header del stream del miembro (el de 28 bytes de §1, embebido en +0x08):**
```
inner+0x00  DWORD  magic "QCF\x01"
inner+0x04  DWORD  0
inner+0x08  DWORD  tamaño_comprimido del payload   [CONFIRMADO: 66/47/4107/0xdc24]
inner+0x0C  DWORD  0
inner+0x10  DWORD  0x0011001e   (constante en las muestras; flags/version del codec?)
inner+0x14  bytes  01 0?[codec] 04 00   <- byte inner+0x15 = tipo de codec
inner+0x18  byte   0?[codec]            <- 0x00=deflate, 0x01=imagen/JP2
inner+0x19  byte   05
inner+0x1A  byte   04
inner+0x1B  byte   ext_hdr_size (=1 en las muestras)
inner+0x1C  byte[] ext header (1 byte: primera letra del nombre, p.ej. 'i','b','r')
inner+0x1D  payload comprimido (deflate: empieza con 78 da; imagen: codestream JP2)
```

**Directorio central (al final del archivo):**
```
... datetime DOS (4B) ... 03 00 00  "TOP"  DWORD=offset_directorio(=fin payload)
luego, por item:
  04 00 00 00      (count/atributo)
  02               (tipo = archivo)
  datetime DOS (4B, = FileTimeToDosDateTime del momento de compresión)
  DWORD            tamaño_ORIGINAL   [CONFIRMADO: 60/20000/4096]
  BYTE  namelen    + 00 00           [CONFIRMADO: 6/7/8]
  char[namelen]    nombre UTF-8       [CONFIRMADO: "in.txt"/"big.txt"/"rand.bin"]
```

### Codec dispatch (tarea 4) — empírico

- `lQuality` (param de `CompressFile`) controla la compresión **lossy** de imágenes:
  un JPEG de 438 KB → 17.8 KB (q=0) / 32.7 KB (q=50) / 56.5 KB (q=100). **Este es
  exactamente el parámetro de calidad que `SUMMARY.md` decía que faltaba en el port.**
- El **codec real** queda registrado en el byte `inner+0x15`/`inner+0x18` del stream:
  `0` = deflate (texto/binario genérico), `1` = imagen (JPEG2000/CODEC4, lossy).
- `plCompType` que retorna la API = tipo de **contenedor** (=1 para QCM), no el codec.

## 8. Sesión autónoma (jun-2026) — tabla de codecs, JP2, campos

### 8.1 Tabla de dispatch de codecs (empírica, `lQuality=50`)

Comprimiendo un corpus de tipos variados y mirando el byte `inner+0x18` y el round-trip:

| Tipo de entrada | `inner+0x18` | ¿lossless? | Backend / comportamiento |
|---|:---:|:---:|---|
| PNG, PNG-paleta, GIF, BMP | **1** | **NO** | CODEC4 → JPEG2000 **lossy** (recomprime formatos sin pérdida) |
| **TIFF** | 0 | SÍ | deflate (¡NO va a imagen — corrige al `SUMMARY` que decía JP2!) |
| WAV, HTML, PDF, ZIP, EXE, binario | 0 | SÍ | deflate (zlib) |
| OLE2 (`.doc`/`.xls`) | 0 | contenido sí, bytes **no** | `MSOC21` re-serializa el compound file |

**Discriminador**: el byte `inner+0x18` solo distingue **imagen (1)** vs **stream/deflate (0)**.
La especialización OLE2 (`MSOC21`) y PDF (`PdfProc`) ocurre dentro del path "0" (preprocesan
y luego deflactan); no hay byte de codec dedicado para ellos.

> **Gotcha del producto**: PNG/GIF/BMP (formatos normalmente lossless) se recomprimen con
> **pérdida** a JPEG2000. Al descomprimir devuelve el mismo formato pero degradado. TIFF
> se libra (va a deflate lossless).

### 8.2 Codestream JPEG2000 — config fija, `lQuality` = bitrate

Extrayendo el payload de imagen (`qcf_tool.qcm` → `member.extract()`) y parseando marcadores:

- El payload lleva un **wrapper de 26 bytes** (de IMGCMP/CODEC4) que codifica `ancho`,
  `alto`, `bpp` (p.ej. `a00f`=4000, `b80b`=3000, `18`=24bpp), seguido del **codestream J2K crudo**
  (`FF4F` SOC + `FF51` SIZ).
- Parámetros (idénticos a q=0/50/100): JPEG2000 **Part-1**, **1 tile**, **3 comps RGB**,
  **MCT on**, **5 niveles de descomposición** (6 resoluciones), **2 quality layers**,
  progresión **LRCP**.
- `lQuality` **NO** cambia la estructura del codestream — solo el **bitrate objetivo**
  (asignación de tasa EBCOT/PCRD). El payload escala 121KB→271KB→536KB para 4000×3000.

#### Qué controla `lQuality` exactamente (barrido fino + cross-image)

Barrido q=0..100 sobre la misma imagen y comparación entre imágenes distintas:

- **COD y QCD son IDÉNTICOS** para todo q (mismo 2-layer/5-niveles/MCT y mismo cuantizador
  base). → `lQuality` **NO** es un factor de cuantización.
- El codestream a q bajo **NO es prefijo** del de q alto (divergen al empezar los datos de
  coeficientes). → **NO** es truncamiento literal del bitstream.
- A misma `q`, dos imágenes del **mismo tamaño** dan **bpp distinto** (q=80: 0.25 vs 0.83 bpp).
  → `lQuality` **NO** es un bpp/ratio objetivo fijo.
- **Conclusión**: `lQuality` alimenta el **asignador PCRD (post-compression rate-distortion)**
  de Kakadu — con cuantizador fijo, decide **cuántos coding-passes incluir por code-block**,
  produciendo packets distintos. Es **control de tasa estándar de JPEG2000**; el tamaño final
  depende del contenido (como todo JP2 rate-controlled).
- **Curva empírica** (foto típica 1215×1080): bpp ≈ 0.32 (q=0) → 1.12 (q=100), casi lineal
  (R²≈0.97), `bytes ≈ 1287·q + 45000`. La constante exacta `lQuality→rate` vive en
  `IMGCMP.dll`/Kakadu (parámetro de calibración, no algoritmo).

#### Chain completo de `lQuality` (traceado)

`lQuality` (0-100) → `IQCSingleFileArch::CompressFile` lo guarda en `engine+0x394`
(**default 0x64 = 100**) → `vtable[12]` lo lee (`QCArch @ 0x10029590`) → vía COM al codec de
imagen `IMGCMP` (selección por EngineID) → `CODEC4` lo recibe como **`nQFactor`** (visto en la
firma de log `InternalSave(PNGIO, nFormat, nBitsPerPixel, nQFactor, ...)`, CODEC4 `FUN_1002a940`/
`FUN_1002aaf0`) → `FUN_1002a300` lo mete en el objeto-engine Kakadu → **PCRD de Kakadu** produce
el bitstream. COD/QCD quedan fijos; `nQFactor` controla el **target de tasa del PCRD**.
La fórmula numérica exacta `nQFactor→rate` está enterrada en el encode de Kakadu (varios niveles
bajo `FUN_1002a300`); por valor/esfuerzo se deja con la curva empírica como respuesta práctica.

> **Veredicto del algoritmo de imagen**: 100% identificado = **JPEG2000 Part-1 estándar con
> COD/QCD fijos y asignación PCRD dirigida por `lQuality`**. Reproducible con OpenJPEG fijando
> target-rate. Lo único no byte-exacto es la constante interna `lQuality→rate` (calibración).

### 8.3 Campos del header — confirmaciones

- **Sin timestamp ni CRC en el header QCF interno**: dos archivos de contenido idéntico
  comprimidos por separado dan headers internos **byte-idénticos**. La fecha/hora vive
  **solo en el directorio central** (`FileTimeToDosDateTime`).
- `inner+0x10 = 0x0011001e` es **constante fija** en todos los tipos → version/flags, no dato.
- **`ext_header` (1 byte) = primera letra del nombre** del archivo. Confirmado en 6 muestras
  (`i`,`b`,`r`,`s`, etc.). Curioso pero consistente.
- Los 5 DWORDs son **específicos del codec**: en deflate, `inner+0x08` = tamaño comprimido;
  en OLE2, `inner+0x04` = tamaño original (0x2600 = 9728). No hay un layout único.
- `inner+0x14`: deflate=`0x00040001`, imagen=`0x00040101` (el byte `inner+0x15` acompaña al codec).

### 8.4 `DecompressArchiveW` (export C de `QCShExt`)

Desensamblado (`RVA 0x1e490`): **`__stdcall` con 4 argumentos** (`ret $0x10`), usa una CLSID
en `rdata@0x10025ad8` para un `CoCreateInstance` interno. No se ejercitó: el path COM
`DecompressFile` ya descomprime con round-trip, así que este export es redundante para validar.

### 8.5 Multi-archivo — ✅ RESUELTO (con muestra real)

> **Actualización**: el usuario generó una muestra real de 5 archivos en una VM WinXP
> (`tests/fixtures/real_qcf/multi/Choshuku.qcf`, 16 MB: Choshuku.EXE, Choshuku.cab, README.md,
> keygen.c, keygen.exe). Con ese ground truth el formato multi-item quedó **completamente
> reverseado, implementado (`qcf_tool.qcm` lee y escribe) y validado** (el motor original
> descomprime un multi-archivo hecho por nuestro código). Layout exacto en
> `docs/QCF_FORMAT_SPEC.md` §6.6. El texto de abajo (las 4 vías de *producción* vía el motor)
> queda como referencia histórica — ya no es necesario, porque lo producimos nosotros.

### 8.5-old Multi-archivo — investigación de producción vía el motor (histórico)

`CompressFile` repetido **sobrescribe** (no añade), así que el multi-item exige el path
`IQCQuikArch::Compress` con un `IQCEnum`. Avances (harness `harness/multifile.c`):

- **vtable real de `IQCQuikArch`** en `.rdata @ rva 0x302e8` (VA `0x100302e8`): `Create=0x10028ce0`,
  `Close=0x10024b10`, `EnumerateItems=0x10028970`, `Extract=0x100282f0`, `Compress=0x10027d70`,
  etc. Hay **dos** vtables (segunda impl de IUnknown).
- **`Create` (vtable[3])** decompilada y funcionando: `Create(BSTR path, ULONG a, ULONG createNew, ptr)`.
  `Create(path, 0, 1, NULL)` → S_OK (crea nuevo); `Create(path, 1, 0, NULL)` → 0x80070002 (abrir, no existe).
- **Contrato `IQCEnum` CONFIRMADO** (lo que el trabajo previo nunca logró): implementé un `IQCEnum`
  en C y el motor llamó `Next(&item, &flags)` consumiendo los paths — los items son **BSTR de
  ruta completa** (via `SysAllocString`); `flags & 0x10` = rama overwrite. `Next` se llama
  hasta agotar.
- **Bloqueo**: tras consumir los items, `Compress` devuelve `E_FAIL (0x80004005)` en el
  procesamiento por-item. Falta la **receta exacta de args** que usa `CompressFile` internamente.
  La receta NO está en una vtable simple: los slots dual de `IQCSingleFileArch` en `.rdata@0x2f9a4`
  (índices 7/8) son **stubs** (`0x10001cc0` = `mov eax,1; ret`; `0x10001cd0` = `xor eax,eax; ret`).
  La lógica real de `CompressFile` se ejecuta vía el **dispatch de `IDispatch::Invoke`** (`0x10001cb0`),
  no por slot de vtable — hay que trazar el switch de DISPID dentro de Invoke para extraer la receta.

> El formato multi-item es en gran parte **inferible** del single-file: el directorio central
> tendría varios records (uno por item) tras el "TOP" root, con el mismo layout ya confirmado.
> Falta validarlo con una muestra real.

### 8.5.1 Receta REAL de `CompressFile` (decompilada) — la solución

La dual vtable de `IQCSingleFileArch` está en `.rdata@0x100302c0` (el constructor la asigna en
`objeto+4`; `objeto+0` = vtable `IQCQuikArch` `0x100302e8`). `CompressFile` = vtable[7] = `0x10026430`.
Decompilada, hace (con `eng = this-4` = la interfaz `IQCQuikArch`):

```c
// CompressFile(this, BSTR src, BSTR destArch, BSTR name, long quality, long* outType)
eng->vtable[3] (eng, destArch, 0xC0000000, 1, &ctx);   // Create(path, accessRW, createNew, &ctx)
this[0x390] = quality;                                  // guarda lQuality
eng->vtable[12](eng, src, name, &ctx, &flags);          // vtable[12]=0x10029070: comprime UN archivo
// (cleanup/close vía FUN_10026601)
*outType = (flags&...) ? 2 : 1;
```

**Por qué mi `harness/multifile.c` fallaba**: llamé `Create(path, 0, 1, NULL)` — el access debía
ser **`0xC0000000`** (GENERIC_READ|WRITE) y arg4 un **objeto ctx** (no NULL). Además usé el
`Compress` público (vtable[8], basado en `IQCEnum`) cuando el camino real es **`vtable[12]`**
(`0x10029070`, "comprimir-un-archivo" con src+name directos).

**Cómo cerrar multi-archivo**: `Create(destArch,0xC0000000,1,&ctx)` **una vez**, luego
`vtable[12](eng, srcBSTR_i, nameBSTR_i, &ctx, &flags)` **por cada archivo**, luego cerrar.

### 8.5.2 Las 4 vías de producción multi-archivo (todas exploradas)

Intenté producir un `.qcf` multi-item real (ground truth) por todos los caminos; todos chocan
con internals COM / dependencia del shell:

1. **`CompressFile` repetido al mismo archivo** → **sobrescribe** (no añade). Single-file only.
2. **`IQCQuikArch::Compress` (vtable[8], `IQCEnum`)** → el `IQCEnum` propio **funciona** (el motor
   llama `Next` y consume los BSTR), pero `Compress` da **E_FAIL** incluso con `Create` correcto
   (`0xC0000000,1,&ctx`). vtable[8] no es el camino que usa el motor.
3. **`vtable[12]` (`0x10029070`, el que usa `CompressFile`)** → requiere el objeto `ctx` (vtable
   `0x10030294`) + la orquestación interna (struct de 688B, `FUN_10029070` con C++ COM que Ghidra
   decompila al límite de fiabilidad). Replicarlo a mano es alto riesgo.
4. **Shell `QCShExt!DecompressArchiveW(0,0,path)`** (extracción) → crea un **QCActionEnum**
   (`CLSID {55C31B6F-...}`), QI `IQCActionEnum`, y **configura** la acción de extracción, pero
   **necesita el framework del shell (Explorer)** para ejecutarla; headless prepara pero no extrae.

**Conclusión honesta**: el formato single-file está 100% (leído, escrito, validado contra el motor).
El multi-archivo **no se pudo producir/validar** sin reimplementar internals COM profundos o el
shell — es la frontera dura genuina (el proyecto cat-re original también paró aquí). La **receta
está completamente mapeada**; lo que falta es ingeniería de harness COM de alto riesgo, no RE.

### 8.5.3 Layout multi-item INFERIDO (no validado)

Del single-file: tras `"TOP"` habría **un record por item** (mismo formato: count/type/datetime/
origsize/namelen/name) y los streams QCF irían secuenciales antes del directorio. **No confirmado**
— el mecanismo exacto de cómo cada record referencia su stream (offset) no es deducible de una sola
muestra single-file. Requiere una muestra multi-item real (que no se pudo generar, ver arriba).

## 9. Internos de los codecs backend (RE estático de DLLs)

Revisión de strings + tabla de imports de cada backend DLL:

| DLL | Algoritmo real | Evidencia |
|-----|----------------|-----------|
| `MSOC21` (Office) | **zlib 1.1.3 DEFLATE** sobre streams OLE2 | strings `deflate/inflate 1.1.3 ... Gailly/Adler`, `m_pParentStream->Read/Seek/Write`; **no importa** ninguna DLL de compresión (zlib estático) |
| `PdfProc` (PDF) | **zlib 1.1.3 DEFLATE** | mismas strings zlib 1.1.3 |
| `MStream` (helper streams) | **zlib 1.1.3** (`zlibNewSizeWW`) | strings zlib |
| `IMGCMP` (imágenes) | **JPEG2000** (delega a CODEC4) | sin zlib; wrapper sobre el codec de imagen |
| `ZipDLL`/`UnzDLL` | DEFLATE (ZIP) | — |
| `CODEC4` | JPEG2000 (Kakadu) | exports `kdu_*` |
| `LFCMP13n` | **LFC propietario** (LEADTOOLS) | único no estándar |

### Conclusión: NO hay compresión propietaria (salvo LFC)

> **Corrección mayor a `RE_notes.md`/`SUMMARY.md`**: la idea de que `MSOC21` usaba
> **"MS-OFFCRYP"** (deflate propietario con tablas Huffman custom) es **FALSA**. MSOC21
> es **zlib estándar 1.1.3** aplicado **por-stream** dentro del compound file OLE2 (por eso
> el round-trip reconstruye el contenido pero re-serializa el CFB → no byte-exacto). Las
> strings de "crypto" (`DIGSIG_E_CRYPTO`, `NTE_DOUBLE_ENCRYPT`, `ERROR_*_CROSS_ENCRYPTION`)
> son solo la tabla de mensajes de error HRESULT estándar de Windows enlazada, **no** crypto real.

### `LFCMP13n` = LEADTOOLS comercial, NO de Choshuku

Exports `flt*` (`fltCompressBuffer`, `fltStartCompressBuffer`, `fltGetStamp`, `fltTransform`…)
= API de filtros LEADTOOLS. Strings: `LEAD Proprietary`, `LEAD Technologies Inc. V1.0x`,
`Lossless JPEG`, `Progressive LEAD`. Es el formato **CMP/CMW de LEADTOOLS** (wavelet propietario
de LEAD + Lossless JPEG). Es un **codec comercial de terceros** que Choshuku solo empaqueta para
imágenes médicas/especializadas — **no es IP de Choshuku** ni su algoritmo.

### Conclusión meta: Choshuku no tiene algoritmo de compresión propio

Todos los codecs del motor son **librerías de terceros**: zlib (Gailly/Adler), JPEG2000/Kakadu,
LEADTOOLS. La IP real de Choshuku es **el formato contenedor `.qcf`/QCM y la orquestación COM**
— y eso está reverseado al 100% y validado con muestras reales. El único algoritmo no reverseado
(LFC/CMW) pertenece a **LEADTOOLS**, no a Choshuku, y queda fuera de alcance (producto comercial,
caso médico raro, sin muestras).

### 9.1 Office REAL — ratios medidos y fidelidad de round-trip

Comprimiendo documentos Office **reales** (test-data de Apache POI) con el motor (`lQuality=50`):

| Archivo real | original | `.qcf` | ratio | round-trip |
|---|--:|--:|--:|---|
| simple.doc | 19 KB | 1.3 KB | **6.7%** | contenido OK |
| SampleDoc.doc | 27 KB | 5.3 KB | **19.5%** | contenido OK |
| SampleShow.ppt | 125 KB | 32 KB | **25.6%** | contenido OK |
| SampleSS.xls | 17 KB | 3.5 KB | **20.2%** | Workbook difiere 0.17% |
| 49219.xls | 258 KB | 28 KB | **10.9%** | Workbook difiere |

- **Ratio práctico: ~7-26%** del original (ahorro 74-93%). Office binario comprime muy bien por su
  redundancia/padding; zlib lo aplasta.
- **Fidelidad**: `.doc`/`.ppt` preservan el contenido de los streams. `.xls` **NO es byte/stream-
  perfecto**: MSOC21 (a) re-serializa streams pequeños de mini-FAT a regulares (padding a 4096 — el
  cutoff), y (b) **altera ~23 bytes (0.17%) del stream Workbook** a intervalos de 34 bytes (un campo
  de los records FONT, `0xb2`→`0x00`). El `.xls` abre y su contenido es usable, pero no idéntico.
  → Choshuku para `.xls` es "lossless de contenido principal" pero no byte-exacto.

Fixtures reales en `tests/fixtures/real_office/`.

**El motor entero usa solo dos algoritmos de compresión propios-del-flujo, ambos estándar y públicos:**
- **zlib/DEFLATE** (RFC 1951) — para Office, PDF, ZIP, y todo lo genérico.
- **JPEG2000** (ISO/IEC 15444-1) — para imágenes (lossy).
- (más **LFC/LEADTOOLS** propietario, solo imágenes médicas — sin muestras, ~5% RE).

→ Los **algoritmos de compresión están al ~100%** salvo LFC: todo lo demás es reproducible
con zlib + OpenJPEG.

### Pendiente para el 100%

1. **Multi-archivo / carpetas**: ver §8.5 — contrato `IQCEnum` resuelto; falta la receta de
   args de `Compress` (decompilar la impl de `CompressFile`).
2. **Codecs propietarios sin RE interna**: `MSOC21` (MS-OFFCRYP), `PdfProc` (re-deflate de
   FlateDecode), `LFC`/LEADTOOLS (sin muestras). Difíciles y de bajo valor.
3. Reimplementar el **encoder** (escribir QCM, no solo leer) en `libcat`/`qcf_tool`.
