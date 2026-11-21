/* SPDX-License-Identifier: MIT
 * Synthetic native test DLL. No game or third-party bytes or runtime exports.
 */
#include <windows.h>
#ifndef FIXTURE_VALUE
#define FIXTURE_VALUE 1
#endif
__declspec(dllexport) int __cdecl fixture_value(void) { return FIXTURE_VALUE; }
#ifdef FIXTURE_THREAD_FAULT
static DWORD WINAPI published_thread(void* unused){(void)unused;Sleep(200);SetEnvironmentVariableW(L"SFI_FAULT_THREAD_RAN",L"1");return 0;}
#endif
#ifdef FIXTURE_TOUCH
/* An initializer that changes a file the startup witness covers (SFI_TOUCH_FILE). */
static void touch_witnessed_file(void){
    WCHAR path[1024];DWORD written=0;
    DWORD length=GetEnvironmentVariableW(L"SFI_TOUCH_FILE",path,1024);
    if(!length||length>=1024)return;
    HANDLE file=CreateFileW(path,FILE_APPEND_DATA,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
    if(file==INVALID_HANDLE_VALUE)return;
    WriteFile(file,"changed by a DLL",16,&written,NULL);CloseHandle(file);
}
#endif
BOOL WINAPI DllMain(HINSTANCE instance,DWORD reason,LPVOID reserved){
    (void)instance;(void)reserved;
    if(reason==DLL_PROCESS_ATTACH){
#ifdef FIXTURE_TOUCH
        touch_witnessed_file();
#endif
#ifdef FIXTURE_SLEEP_MS
        /* An initializer that holds the loader lock this long, then says it returned (SFI_SLEEP_DONE). */
        Sleep(FIXTURE_SLEEP_MS);
        SetEnvironmentVariableW(L"SFI_SLEEP_DONE",L"1");
#endif
#ifdef FIXTURE_FAULT
        RaiseException(0xe0424242u,0,0,NULL);
#endif
#ifdef FIXTURE_FALSE
        return FALSE;
#endif
#ifdef FIXTURE_THREAD_FAULT
        HANDLE thread=CreateThread(NULL,0,published_thread,NULL,0,NULL);if(thread)CloseHandle(thread);
#endif
#if defined(FIXTURE_AV) || defined(FIXTURE_THREAD_FAULT)
        *(volatile int*)(ULONG_PTR)0=17;
#endif
    }
    return TRUE;
}
