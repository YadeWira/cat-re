# Muestras `.qcf` REALES (ground truth)

Generadas por el motor original `QCArch.dll` v1.0.2 bajo Wine, vía
`IQCSingleFileArch::CompressFile` (ver `harness/sfa.c`). Son las PRIMERAS
muestras reales del proyecto — antes todo el formato estaba inferido del código.

| archivo | input | codec interno | nota |
|---------|-------|---------------|------|
| `in.txt.qcf`   | texto 60 B   | deflate (zlib `78 da`) | roundtrip lossless ✓ |
| `big.txt.qcf`  | texto 20 KB  | deflate | roundtrip lossless ✓ |
| `rand.bin.qcf` | random 4 KB  | deflate (stored, +overhead) | roundtrip lossless ✓ |
| `real.jpg.qcf` | JPEG 438 KB  | JPEG2000 (CODEC4), lossy | recompresión con calidad |

Contenedor externo = `QCM\x01`; stream de cada miembro = `QCF\x01` embebido.
Layout completo verificado en `docs/RE_verified.md` §"Formato real".
