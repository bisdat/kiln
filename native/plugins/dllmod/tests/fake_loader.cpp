// SPDX-License-Identifier: MIT
// A stand-in for Enshrouded ModLoader's own DLL as its 0.6.15 installer puts it next to Kiln
// (kiln/native/enshrouded-modloader/modloader.dll): the DirectInput8 export, the Kiln native init and its world profile
// header text, which is how kiln-dllmod knows it. It does nothing (tests/run_plugin.py never loads it).
#include <windows.h>

extern "C" __declspec(dllexport) HRESULT WINAPI DirectInput8Create(HINSTANCE, DWORD, const void*, void**, void*) { return E_FAIL; }
extern "C" __declspec(dllexport) int kiln_native_init(const void*) { return 2; }
extern "C" __declspec(dllexport) const char* fake_loader_marker() { return "ENSHROUDED_MODS_V1"; }
