/* DecompressArchiveW(0,0,path[,0]) — extrae un archivo .qcf completo (multi-file)
 * a su carpeta. Oráculo para validar el formato multi-item. */
#include <windows.h>
#include <stdio.h>
typedef HRESULT (__stdcall *PFN)(void*,void*,const wchar_t*,void*);
int wmain(int argc,wchar_t**argv){
    if(argc<2){wprintf(L"uso: dca <archivo.qcf>\n");return 2;}
    CoInitialize(NULL);
    HMODULE h=LoadLibraryW(L"Z:\\home\\forum\\git\\cat-re\\original\\DLLs\\1_Core_Engine\\QCShExt.dll");
    if(!h){wprintf(L"LoadLibrary fail %lu\n",GetLastError());return 1;}
    PFN f=(PFN)GetProcAddress(h,"DecompressArchiveW");
    wprintf(L"DecompressArchiveW=%p, archivo=%ls\n",f,argv[1]);
    if(!f)return 1;
    /* path como 3er arg; 4o arg dummy por el ret $0x10 */
    HRESULT hr=f((void*)0,(void*)0,argv[1],(void*)0);
    wprintf(L"hr=0x%08lx\n",hr);
    return 0;
}
