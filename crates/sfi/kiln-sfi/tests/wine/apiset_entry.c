/* SPDX-License-Identifier: MIT - synthetic ordinary API-set dependency. */
#include <windows.h>
__declspec(dllimport) int __cdecl _initialize_narrow_environment(void);
__declspec(dllexport) int __cdecl entry_value(void) { return _initialize_narrow_environment(); }
BOOL WINAPI DllMain(HINSTANCE instance,DWORD reason,LPVOID reserved) {
    (void)instance;(void)reason;(void)reserved;return TRUE;
}
