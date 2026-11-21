/* SPDX-License-Identifier: MIT */
#include <windows.h>
#include <stdio.h>
#ifndef SEC_IMAGE_NO_EXECUTE
#define SEC_IMAGE_NO_EXECUTE 0x11000000
#endif
typedef LONG (NTAPI* SameMappedFiles)(PVOID,PVOID);
static LONG compare(SameMappedFiles same, HMODULE loaded, const WCHAR* path) {
    HANDLE file=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,0,NULL);
    if(file==INVALID_HANDLE_VALUE) { printf("open failed %lu\n",GetLastError());return (LONG)0xc0000001u; }
    HANDLE mapping=CreateFileMappingW(file,NULL,PAGE_READONLY|SEC_IMAGE_NO_EXECUTE,0,0,NULL);
    if(!mapping) { printf("mapping failed %lu\n",GetLastError());CloseHandle(file);return (LONG)0xc0000001u; }
    void* view=MapViewOfFile(mapping,FILE_MAP_READ,0,0,0);
    if(!view) { printf("view failed %lu\n",GetLastError());CloseHandle(mapping);CloseHandle(file);return (LONG)0xc0000001u; }
    LONG status=same((void*)loaded,view);
    UnmapViewOfFile(view);CloseHandle(mapping);CloseHandle(file);return status;
}
int wmain(int argc,WCHAR** argv) {
    if(argc!=4) return 2;
    const WCHAR* apisets[2]={L"api-ms-win-core-file-l1-1-0.dll",L"api-ms-win-crt-runtime-l1-1-0.dll"};
    for(size_t i=0;i<2;i++){HMODULE host=GetModuleHandleW(apisets[i]);WCHAR path[1024];path[0]=0;if(host)GetModuleFileNameW(host,path,1024);wprintf(L"API-set %ls host=%p path=%ls\n",apisets[i],(void*)host,path);}
    SameMappedFiles same=(SameMappedFiles)GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"NtAreMappedFilesTheSame");
    if(!same) { puts("FAIL NtAreMappedFilesTheSame unavailable");return 3; }
    HMODULE loaded=LoadLibraryExW(argv[1],NULL,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if(!loaded) { printf("load failed %lu\n",GetLastError());return 4; }
    LONG original=compare(same,loaded,argv[1]);LONG different=compare(same,loaded,argv[2]);
    printf("mapping original=%08lx different=%08lx\n",(unsigned long)original,(unsigned long)different);
    if(original!=0||different==0) return 5;
    if(!MoveFileW(argv[1],argv[3])||!CopyFileW(argv[2],argv[1],FALSE)) { printf("rename/replace failed %lu\n",GetLastError());return 6; }
    LONG replaced=compare(same,loaded,argv[1]);LONG retained=compare(same,loaded,argv[3]);
    printf("mapping replaced=%08lx retained=%08lx\n",(unsigned long)replaced,(unsigned long)retained);
    if(replaced==0||retained!=0) return 7;
    puts("PASS backing section detects mapped-A/replaced-path-B");
    /* Retain the loaded DLL: never test cleanup by unloading arbitrary native code. */
    return 0;
}
