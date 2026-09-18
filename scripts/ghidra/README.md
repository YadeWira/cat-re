# Ghidra scripts (reverse engineering)

Headless scripts used to verify claims about the engine against the actual binaries.
They are what produced the addresses cited in `docs/RE_verified.md` and
`docs/QCF_FORMAT_SPEC.md`.

They need Ghidra (any recent release) and the original DLLs, which are **not** in this
repository — see the note on `OLD/` in the README.

```bash
# import + analyse + run a script (first time on a DLL)
ghidra/support/analyzeHeadless /tmp/gproj myproj \
    -import path/to/IMGCMP.dll \
    -scriptPath scripts/ghidra -postScript FindPick.java "EngineID" "QuikCAT\CODEC"

# re-run a script on an already-imported program (fast, no re-analysis)
ghidra/support/analyzeHeadless /tmp/gproj myproj -process IMGCMP.dll -noanalysis \
    -scriptPath scripts/ghidra -postScript DecompAt.java 0x10010b60 0x10012680
```

| Script | What it does |
|---|---|
| `FindPick.java` | Decompiles every function that references a given string (args = the strings). How the registry-driven codec lookup and the PNG dispatch were found |
| `DecompAt.java` | Decompiles functions by address (args) and lists their callers |
| `DecompBatch.java` | Decompiles a hard-coded list of addresses — the original pass over `QCArch.dll` |
| `FindStr.java` | Finds one string and decompiles the first functions that reference it |
| `FindDec.java`, `Callers.java`, `Dec1.java` | Earlier one-off helpers, kept for reference |
