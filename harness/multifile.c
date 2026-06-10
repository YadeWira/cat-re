/* EXPERIMENTAL: produce a multi-file .qcf via IQCQuikArch::Compress + a
 * hand-built IQCEnum. The enum yields full file paths as BSTRs (that is how
 * the engine's Compress loop consumes items: enum->Next(&BSTR, &flags)).
 *
 * RECETA REAL (de decompilar CompressFile @ 0x10026430, ver docs/RE_verified.md §8.5.1):
 *   El camino correcto NO es el Compress público (vtable[8], IQCEnum). CompressFile hace:
 *     eng->Create(destArch, 0xC0000000, 1, &ctx);   // access=GENERIC_RW, createNew=1, ctx obj
 *     eng->vtable[12](eng, srcBSTR, nameBSTR, &ctx, &flags);  // 0x10029070 = comprime-un-archivo
 *   Multi-archivo = Create una vez + loop vtable[12] por archivo + close.
 *   ESTE harness usaba Create(path,0,1,NULL) (access/ctx mal) y Compress(vtable[8]) -> E_FAIL.
 *   Pendiente: replicar el objeto ctx (vtable 0x10030294, ~8 métodos) y llamar vtable[12] en loop.
 *

 * Build: i686-w64-mingw32-gcc -municode -mconsole -O2 -o multifile.exe multifile.c -lole32 -loleaut32 -luuid
 * Run:   WINEPREFIX=~/.wine-test wine multifile.exe Z:\out.qcf Z:\a.txt Z:\b.txt ...
 */
#include <windows.h>
#include <stdio.h>
#include <initguid.h>
#include <oleauto.h>

DEFINE_GUID(CLSID_QCQuikArch, 0x7F0B34D0,0xD90A,0x49e9,0x92,0x12,0x31,0x34,0x9D,0x54,0x5F,0x4B);
DEFINE_GUID(IID_IQCQuikArch,  0xBFDCA750,0xA117,0x46CD,0x8C,0xE6,0x29,0xB5,0x16,0x27,0xB2,0x68);
DEFINE_GUID(IID_IQCEnum,      0x001FA16A,0xE030,0x437E,0x8C,0xF5,0xDF,0x64,0x3A,0x0F,0x3B,0x86);

/* ---- IQCQuikArch custom vtable (method order from the binary vtable) ---- */
typedef struct IQCQuikArch IQCQuikArch;
typedef struct {
    HRESULT (__stdcall *QueryInterface)(IQCQuikArch*, REFIID, void**);
    ULONG   (__stdcall *AddRef)(IQCQuikArch*);
    ULONG   (__stdcall *Release)(IQCQuikArch*);
    HRESULT (__stdcall *Create)(IQCQuikArch*, BSTR, ULONG, ULONG, void*);
    HRESULT (__stdcall *Delete)(IQCQuikArch*, BSTR);
    HRESULT (__stdcall *Close)(IQCQuikArch*);
    HRESULT (__stdcall *EnumerateItems)(IQCQuikArch*, BSTR, void**);
    HRESULT (__stdcall *Extract)(IQCQuikArch*, void*, BSTR, void*, ULONG, ULONG);
    HRESULT (__stdcall *Compress)(IQCQuikArch*, void*, BSTR, ULONG, void*, ULONG, ULONG);
    HRESULT (__stdcall *Remove)(IQCQuikArch*, void*, void*, ULONG, ULONG);
    HRESULT (__stdcall *Move)(IQCQuikArch*, void*, BSTR, void*, ULONG, ULONG);
} IQCQuikArchVtbl;
struct IQCQuikArch { IQCQuikArchVtbl *lpVtbl; };

/* ---- our IQCEnum implementation: yields BSTR paths ---- */
typedef struct {
    void *lpVtbl;
    LONG ref;
    BSTR *items;
    int count, pos;
} MyEnum;

static HRESULT __stdcall E_QI(MyEnum *t, REFIID riid, void **pp) {
    if (IsEqualGUID(riid,&IID_IUnknown) || IsEqualGUID(riid,&IID_IQCEnum)) {
        *pp = t; t->ref++; return S_OK;
    }
    *pp = NULL; return E_NOINTERFACE;
}
static ULONG __stdcall E_AddRef(MyEnum *t){ return ++t->ref; }
static ULONG __stdcall E_Release(MyEnum *t){ return --t->ref; }
/* Next(item_out, flags_out): set *item to a BSTR path, *flags to 0; S_OK while items remain. */
static HRESULT __stdcall E_Next(MyEnum *t, BSTR *item, ULONG *flags) {
    if (t->pos >= t->count) { if(item)*item=NULL; return S_FALSE; }
    if (item) *item = SysAllocString(t->items[t->pos]);
    if (flags) *flags = 0;
    wprintf(L"  [enum] Next -> %ls\n", t->items[t->pos]);
    t->pos++;
    return S_OK;
}
static HRESULT __stdcall E_Reset(MyEnum *t){ t->pos=0; return S_OK; }

typedef struct {
    HRESULT (__stdcall *QueryInterface)(MyEnum*, REFIID, void**);
    ULONG   (__stdcall *AddRef)(MyEnum*);
    ULONG   (__stdcall *Release)(MyEnum*);
    HRESULT (__stdcall *Next)(MyEnum*, BSTR*, ULONG*);
    HRESULT (__stdcall *Reset)(MyEnum*);
} MyEnumVtbl;
static MyEnumVtbl g_enumVtbl = { E_QI, E_AddRef, E_Release, E_Next, E_Reset };

int wmain(int argc, wchar_t **argv) {
    if (argc < 3) { wprintf(L"usage: <out.qcf> <file1> [file2...]\n"); return 1; }
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    IQCQuikArch *arc = NULL;
    HRESULT hr = CoCreateInstance(&CLSID_QCQuikArch, NULL, CLSCTX_INPROC_SERVER,
                                  &IID_IQCQuikArch, (void**)&arc);
    wprintf(L"CoCreate(IQCQuikArch): hr=0x%08lx -> %p\n", hr, arc);
    if (FAILED(hr)) return 1;

    BSTR outpath = SysAllocString(argv[1]);
    /* Try Create(path, 0, 1, NULL): param4 (3rd) = create-new flag per decompile. */
    void *ctx[16] = {0};   /* buffer ctx para Create/Compress (arg4) */
    hr = arc->lpVtbl->Create(arc, outpath, 0xC0000000, 1, ctx);
    wprintf(L"Create(%ls, 0xC0000000,1,&ctx): hr=0x%08lx\n", argv[1], hr);

    MyEnum en = { &g_enumVtbl, 1, &argv[2], argc-2, 0 };
    wprintf(L"Compress over %d item(s)...\n", argc-2);
    hr = arc->lpVtbl->Compress(arc, &en, NULL, 0, NULL, 0, 0);
    wprintf(L"Compress: hr=0x%08lx\n", hr);

    HRESULT hc = arc->lpVtbl->Close(arc);
    wprintf(L"Close: hr=0x%08lx\n", hc);

    arc->lpVtbl->Release(arc);
    CoUninitialize();
    return 0;
}
