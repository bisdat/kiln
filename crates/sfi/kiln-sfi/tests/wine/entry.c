/* SPDX-License-Identifier: MIT - synthetic import/delay-import fixture. */
#include <windows.h>
__declspec(dllimport) int __cdecl fixture_value(void);
__declspec(dllexport) int __cdecl entry_value(void) { return fixture_value(); }
#ifdef ENTRY_ID
__declspec(dllexport) int __cdecl entry_id(void) { return ENTRY_ID; }
#endif
BOOL WINAPI DllMain(HINSTANCE instance,DWORD reason,LPVOID reserved) {
    (void)instance;(void)reserved;
    if(reason==DLL_PROCESS_ATTACH) SetEnvironmentVariableW(L"SFI_ENTRY_RAN",L"1");
    return TRUE;
}
