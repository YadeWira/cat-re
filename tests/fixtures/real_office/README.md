# Muestras Office REALES + sus .qcf (ground truth del codec MSOC21)

Documentos OLE2 reales (Apache POI test-data) comprimidos por el motor original
Choshuku vía CompressFile (harness/sfa.c). Codec = MSOC21 = zlib DEFLATE sobre
los streams del compound file OLE2 (ver docs/RE_verified.md §9).

Ratios medidos: .doc/.xls/.ppt → ~7-26% del original (ahorro 74-93%).
Fidelidad: .doc/.ppt preservan contenido; .xls altera ~0.17% del Workbook (campo FONT).
