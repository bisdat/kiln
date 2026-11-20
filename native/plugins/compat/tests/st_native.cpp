// SPDX-License-Identifier: MIT
// st_native.dll: a native mod shaped like Auto Doors 1.1.0 for the original-code scenario (inbox 26c). compathost
// --native calls st_native_start before any Shroudtopia mod starts; it hooks the exe's st_text_target at its entry
// (safetyhook: an E9 rel32 to a relay near the exe that jumps on to the detour, as MinHook does). The detour traces 'N'
// and calls on.
#include <windows.h>

#include "safetyhook.hpp"

namespace {
using TraceFn = void (*)(char);
TraceFn g_trace = nullptr;
safetyhook::InlineHook g_hook;
int detour(int a) {
    if (g_trace) g_trace('N');
    return g_hook.call<int>(a);
}
}  // namespace

extern "C" __declspec(dllexport) bool st_native_start() {
    HMODULE exe = GetModuleHandleW(nullptr);
    g_trace = reinterpret_cast<TraceFn>(GetProcAddress(exe, "st_trace"));
    void* target = reinterpret_cast<void*>(GetProcAddress(exe, "st_text_target"));
    if (!g_trace || !target) return false;
    g_hook = safetyhook::create_inline(target, reinterpret_cast<void*>(&detour));
    return static_cast<bool>(g_hook);
}
