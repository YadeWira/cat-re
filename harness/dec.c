/* Decompress-only: DecompressFile(srcArch, destFile) via IQCSingleFileArch.
 * Para validar .qcf hechos a mano. Build con mingw + ole32/oleaut32/uuid. */
#include <windows.h>
#include <stdio.h>
#include <initguid.h>
#include <oleauto.h>
DEFINE_GUID(CLSID_QCQuikArch,0x7F0B34D0,0xD90A,0x49e9,0x92,0x12,0x31,0x34,0x9D,0x54,0x5F,0x4B);
DEFINE_GUID(IID_IQCSingleFileArch,0xF1FE45A8,0x9619,0x45F0,0xAC,0xE3,0x11,0xC3,0xF1,0x4E,0x32,0xBA);
int wmain(int argc,wchar_t**argv){
    if(argc<3){wprintf(L"uso: dec <src.qcf> <dest>\n");return 2;}
    CoInitializeEx(NULL,COINIT_APARTMENTTHREADED);
    IUnknown*u=NULL; CoCreateInstance(&CLSID_QCQuikArch,NULL,CLSCTX_INPROC_SERVER,&IID_IUnknown,(void**)&u);
    IDispatch*disp=NULL; u->lpVtbl->QueryInterface(u,&IID_IQCSingleFileArch,(void**)&disp);
    OLECHAR*nm=L"DecompressFile"; DISPID did=0;
    disp->lpVtbl->GetIDsOfNames(disp,&IID_NULL,&nm,1,LOCALE_USER_DEFAULT,&did);
    long ct=-1; VARIANT a[3]; for(int i=0;i<3;i++)VariantInit(&a[i]);
    a[2].vt=VT_BSTR; a[2].bstrVal=SysAllocString(argv[1]);
    a[1].vt=VT_BSTR; a[1].bstrVal=SysAllocString(argv[2]);
    a[0].vt=VT_I4|VT_BYREF; a[0].plVal=&ct;
    DISPPARAMS dp={a,NULL,3,0}; EXCEPINFO ei; ZeroMemory(&ei,sizeof ei); UINT e=0;
    HRESULT hr=disp->lpVtbl->Invoke(disp,did,&IID_NULL,LOCALE_USER_DEFAULT,DISPATCH_METHOD,&dp,NULL,&ei,&e);
    wprintf(L"DecompressFile: hr=0x%08lx compType=%ld scode=0x%08lx\n",hr,ct,ei.scode);
    return FAILED(hr)?1:0;
}
