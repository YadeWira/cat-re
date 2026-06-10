/* Explore IQCSingleFileArch (dispinterface) and try to produce a real .qcf
 * via CompressFile. Build: i686-w64-mingw32-gcc -municode -mconsole
 * Run:   WINEPREFIX=~/.wine-test wine sfa.exe <src> <dst.qcf>
 */
#include <windows.h>
#include <stdio.h>
#include <wchar.h>
#include <initguid.h>
#include <oleauto.h>

DEFINE_GUID(LIBID_QCQuikArch, 0xB19AA1C0, 0xC66E, 0x4A20,
            0x90, 0xDF, 0x91, 0xD6, 0x22, 0x1A, 0x09, 0xA5);
DEFINE_GUID(CLSID_QCQuikArch, 0x7F0B34D0, 0xD90A, 0x49e9,
            0x92, 0x12, 0x31, 0x34, 0x9D, 0x54, 0x5F, 0x4B);
DEFINE_GUID(IID_IQCSingleFileArch, 0xF1FE45A8, 0x9619, 0x45F0,
            0xAC, 0xE3, 0x11, 0xC3, 0xF1, 0x4E, 0x32, 0xBA);
DEFINE_GUID(IID_IQCQuikArch, 0xBFDCA750, 0xA117, 0x46CD,
            0x8C, 0xE6, 0x29, 0xB5, 0x16, 0x27, 0xB2, 0x68);

static void dump_sfa_methods(void) {
    ITypeLib *tl = NULL;
    HRESULT hr = LoadRegTypeLib(&LIBID_QCQuikArch, 1, 0, 0, &tl);
    if (FAILED(hr)) { wprintf(L"LoadRegTypeLib: 0x%08lx\n", hr); return; }
    UINT n = tl->lpVtbl->GetTypeInfoCount(tl);
    for (UINT i = 0; i < n; i++) {
        ITypeInfo *ti = NULL;
        if (FAILED(tl->lpVtbl->GetTypeInfo(tl, i, &ti))) continue;
        BSTR name = NULL;
        ti->lpVtbl->GetDocumentation(ti, MEMBERID_NIL, &name, NULL, NULL, NULL);
        TYPEATTR *ta = NULL;
        ti->lpVtbl->GetTypeAttr(ti, &ta);
        if (name && wcscmp(name, L"IQCSingleFileArch") == 0 && ta) {
            wprintf(L"=== IQCSingleFileArch  (kind=%d, cFuncs=%d) ===\n",
                    ta->typekind, ta->cFuncs);
            for (UINT f = 0; f < ta->cFuncs; f++) {
                FUNCDESC *fd = NULL;
                if (FAILED(ti->lpVtbl->GetFuncDesc(ti, f, &fd))) continue;
                BSTR names[16]; UINT cn = 0;
                ti->lpVtbl->GetNames(ti, fd->memid, names, 16, &cn);
                wprintf(L"  func[%2u] memid=0x%08lx invkind=%d params=%d  %ls\n",
                        f, fd->memid, fd->invkind, fd->cParams,
                        cn ? names[0] : L"?");
                for (UINT p = 1; p < cn; p++) wprintf(L"      arg: %ls\n", names[p]);
                for (UINT p = 0; p < cn; p++) if (names[p]) SysFreeString(names[p]);
                ti->lpVtbl->ReleaseFuncDesc(ti, fd);
            }
        }
        if (ta) ti->lpVtbl->ReleaseTypeAttr(ti, ta);
        if (name) SysFreeString(name);
        ti->lpVtbl->Release(ti);
    }
    tl->lpVtbl->Release(tl);
}

int wmain(int argc, wchar_t **argv) {
    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) return 1;

    dump_sfa_methods();

    IUnknown *pUnk = NULL;
    hr = CoCreateInstance(&CLSID_QCQuikArch, NULL, CLSCTX_INPROC_SERVER,
                          &IID_IUnknown, (void**)&pUnk);
    if (FAILED(hr)) { wprintf(L"CoCreate QCQuikArch: 0x%08lx\n", hr); return 1; }
    wprintf(L"QCQuikArch IUnknown @ %p\n", pUnk);

    /* Does the engine expose the high-level dispinterface? */
    IDispatch *disp = NULL;
    hr = pUnk->lpVtbl->QueryInterface(pUnk, &IID_IQCSingleFileArch, (void**)&disp);
    wprintf(L"QI(IQCSingleFileArch): hr=0x%08lx -> %p\n", hr, disp);

    if (SUCCEEDED(hr) && disp && argc >= 3) {
        /* Resolve CompressFile dispid by name */
        OLECHAR *mname = L"CompressFile";
        DISPID did = 0;
        HRESULT h2 = disp->lpVtbl->GetIDsOfNames(disp, &IID_NULL, &mname, 1,
                                                 LOCALE_USER_DEFAULT, &did);
        wprintf(L"GetIDsOfNames(CompressFile): hr=0x%08lx dispid=0x%lx\n", h2, did);
        if (SUCCEEDED(h2)) {
            /* CompressFile(src, destArch, fileName, lQuality, plCompType[out])
             * DISPPARAMS args are in REVERSE order. */
            long compType = -1;
            long quality = (argc >= 4) ? _wtoi(argv[3]) : 0;
            const wchar_t *fname = (argc >= 5) ? argv[4] : L"in.txt";
            VARIANT args[5];
            for (int i = 0; i < 5; i++) VariantInit(&args[i]);
            args[4].vt = VT_BSTR;            args[4].bstrVal = SysAllocString(argv[1]); /* src */
            args[3].vt = VT_BSTR;            args[3].bstrVal = SysAllocString(argv[2]); /* destArch */
            args[2].vt = VT_BSTR;            args[2].bstrVal = SysAllocString(fname);   /* fileName */
            args[1].vt = VT_I4;              args[1].lVal    = quality;                 /* lQuality */
            args[0].vt = VT_I4 | VT_BYREF;   args[0].plVal   = &compType;               /* plCompType [out] */
            DISPPARAMS dp = { args, NULL, 5, 0 };
            VARIANT res; VariantInit(&res);
            EXCEPINFO ei; ZeroMemory(&ei, sizeof ei); UINT err = 0;
            HRESULT h3 = disp->lpVtbl->Invoke(disp, did, &IID_NULL,
                LOCALE_USER_DEFAULT, DISPATCH_METHOD, &dp, &res, &ei, &err);
            wprintf(L"Invoke(CompressFile q=%ld): hr=0x%08lx argErr=%u compType=%ld\n",
                    quality, h3, err, compType);
            if (FAILED(h3))
                wprintf(L"  excep: scode=0x%08lx wcode=%u %ls\n",
                        ei.scode, ei.wCode, ei.bstrDescription ? ei.bstrDescription : L"(no desc)");
            SysFreeString(args[4].bstrVal); SysFreeString(args[3].bstrVal);
            SysFreeString(args[2].bstrVal);

            /* Round-trip: DecompressFile(destArch, recovered, &ct) */
            OLECHAR *dn = L"DecompressFile"; DISPID dd = 0;
            if (SUCCEEDED(disp->lpVtbl->GetIDsOfNames(disp, &IID_NULL, &dn, 1,
                                                      LOCALE_USER_DEFAULT, &dd)) && argc >= 6) {
                long ct = -1;
                VARIANT da[3];
                for (int i = 0; i < 3; i++) VariantInit(&da[i]);
                da[2].vt = VT_BSTR;          da[2].bstrVal = SysAllocString(argv[2]); /* srcArch */
                da[1].vt = VT_BSTR;          da[1].bstrVal = SysAllocString(argv[5]); /* destFile */
                da[0].vt = VT_I4 | VT_BYREF; da[0].plVal   = &ct;
                DISPPARAMS dp2 = { da, NULL, 3, 0 };
                EXCEPINFO ei2; ZeroMemory(&ei2, sizeof ei2); UINT e2 = 0;
                HRESULT h4 = disp->lpVtbl->Invoke(disp, dd, &IID_NULL,
                    LOCALE_USER_DEFAULT, DISPATCH_METHOD, &dp2, NULL, &ei2, &e2);
                wprintf(L"Invoke(DecompressFile -> %ls): hr=0x%08lx compType=%ld",
                        argv[5], h4, ct);
                if (FAILED(h4)) wprintf(L" scode=0x%08lx", ei2.scode);
                wprintf(L"\n");
                SysFreeString(da[2].bstrVal); SysFreeString(da[1].bstrVal);
            }
        }
        disp->lpVtbl->Release(disp);
    }
    pUnk->lpVtbl->Release(pUnk);
    CoUninitialize();
    return 0;
}
