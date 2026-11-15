// SPDX-License-Identifier: MIT
#include <windows.h>
extern "C" __declspec(dllexport) HRESULT WINAPI DirectInput8Create(HINSTANCE, DWORD, REFIID, void**, void*) { return E_FAIL; }
#ifndef TEST_PLAIN_PROXY
extern "C" __declspec(dllexport) void shroudlink_shutdown() {}
#endif
// chain_test: a module without this export is not this file (Wine mapped its own builtin in its place).
extern "C" __declspec(dllexport) unsigned kiln_proxy_test_helper() { return 0x4b494c4eu; }
BOOL WINAPI DllMain(HINSTANCE, DWORD, void*) { return TRUE; }
