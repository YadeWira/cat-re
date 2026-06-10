# Wine harness for Choshuku / CAT RE

Drives the original Windows COM DLLs from Linux via Wine + mingw32,
to capture ground truth for the `.qcf` format and the codec dispatch table.

## Status

| Step                                  | Status |
|---------------------------------------|--------|
| Wine 32-bit prefix                    | working |
| `regsvr32 QCArch.dll`                 | working |
| `CoCreateInstance(CLSID_QCQuikArch)`  | working |
| Query vtable                          | working |
| Get IDispatch                         | **blocked** (custom vtable interface) |
| Resolve DISPIDs                       | **pending** (need to parse MSFT TLB) |
| Call CompressFile/DecompressFile      | **pending** |

The class is IUnknown-only (not IDispatch-based) and exposes 9 custom vtable
methods. Calling them requires:

1. The IQCQuikArch IID (16-byte GUID) — extractable from the MSFT TLB blob
   at file offset `0x07e298` in `QCArchUI.dll`. The blob is the serialized
   `QArchUI 1.0 Type Library`.
2. A working `LoadTypeLib` / `GetTypeInfo` parser to enumerate methods.
3. Or: call methods by their vtable index once we know the order from the
   type library (CheatEngine-style).

## Files

- `compress.c` — minimal C harness using mingw32, builds with
  `i686-w64-mingw32-gcc -mconsole -municode`, runs with
  `WINEPREFIX=… wine compress.exe`.

## Next steps (when revisited)

1. Parse the `MSFT\x02\x00\x01\x00` blob in `QCArchUI.dll`'s `.rsrc` to
   extract the IQCQuikArch IID. The blob is at file offset 0x07e298.
2. Once the IID is known, query for it instead of IUnknown.
3. Walk the vtable (12 entries) and call each method with stub args to
   map method indices to names.
4. Once we know `CompressFile`'s vtable index, call it with a real input
   file and capture the .qcf bytes for static analysis.
