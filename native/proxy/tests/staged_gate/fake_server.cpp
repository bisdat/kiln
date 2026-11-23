// SPDX-License-Identifier: MIT
// A stand-in for enshrouded_server.exe (test-staged-gate.sh): it imports DirectInput8Create, so the loader maps
// <folder>\dinput8.dll (Kiln's proxy) at the start, and the proxy's entry gate (staged updates, then kiln-core) runs before
// main, as in the game.
#include <cstdio>
extern "C" __declspec(dllimport) long __stdcall DirectInput8Create(void*, unsigned long, const void*, void**, void*);
int main() {
    volatile auto keep = &DirectInput8Create;  // keeps the import
    (void)keep;
    std::printf("main reached\n");
    return 0;
}
