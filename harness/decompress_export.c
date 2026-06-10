/* Exercise QCShExt.dll!DecompressArchiveW (a plain C export, not COM).
 * Probe the calling convention by trying 1- and 2-arg forms.
 * Build: i686-w64-mingw32-gcc -municode -mconsole -O2 -o decompress_export.exe decompress_export.c
 * Run:   WINEPREFIX=~/.wine-test wine decompress_export.exe Z:\path\to.qcf Z:\outdir
 */
#include <windows.h>
#include <stdio.h>

typedef HRESULT (__stdcall *PFN1)(const wchar_t*);
typedef HRESULT (__stdcall *PFN2)(const wchar_t*, const wchar_t*);
typedef HRESULT (__cdecl  *PFN1c)(const wchar_t*);

int wmain(int argc, wchar_t **argv) {
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    const wchar_t *dll = L"Z:\\home\\forum\\git\\cat-re\\original\\DLLs\\1_Core_Engine\\QCShExt.dll";
    HMODULE h = LoadLibraryW(dll);
    if (!h) { wprintf(L"LoadLibrary failed: %lu\n", GetLastError()); return 1; }
    wprintf(L"QCShExt loaded @ %p\n", h);

    FARPROC w = GetProcAddress(h, "DecompressArchiveW");
    FARPROC a = GetProcAddress(h, "DecompressArchive");
    wprintf(L"DecompressArchiveW=%p  DecompressArchive=%p\n", w, a);
    if (!w) return 1;
    if (argc < 2) { wprintf(L"usage: <archive.qcf> [outdir]\n"); return 0; }

    /* Try 2-arg __stdcall (src, dest) first, then 1-arg. Wine will fault-guard. */
    if (argc >= 3) {
        wprintf(L"trying 2-arg __stdcall(src,dst)...\n");
        HRESULT hr = ((PFN2)w)(argv[1], argv[2]);
        wprintf(L"  -> hr=0x%08lx\n", hr);
    }
    wprintf(L"trying 1-arg __stdcall(src)...\n");
    HRESULT hr1 = ((PFN1)w)(argv[1]);
    wprintf(L"  -> hr=0x%08lx\n", hr1);

    FreeLibrary(h);
    CoUninitialize();
    return 0;
}
