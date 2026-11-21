/* SPDX-License-Identifier: MIT - synthetic package that delay-imports an OS DLL (version.dll). */
#include <windows.h>
__declspec(dllimport) DWORD WINAPI GetFileVersionInfoSizeW(LPCWSTR,LPDWORD);
__declspec(dllexport) int __cdecl entry_value(void) { DWORD handle=0;return GetFileVersionInfoSizeW(L"kernel32.dll",&handle)>0; }
#ifdef ENTRY_ID
__declspec(dllexport) int __cdecl entry_id(void) { return ENTRY_ID; }
#endif
BOOL WINAPI DllMain(HINSTANCE instance,DWORD reason,LPVOID reserved) {
    (void)instance;(void)reserved;
    if(reason==DLL_PROCESS_ATTACH) SetEnvironmentVariableW(L"SFI_ENTRY_RAN",L"1");
    return TRUE;
}
