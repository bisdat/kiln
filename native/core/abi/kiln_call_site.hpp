// SPDX-License-Identifier: MIT
// kiln_call_site.hpp: the game's call that a hooked function runs for when another module's code made the call.
//
// kiln.hooks' caller() is the return address of the call that entered a target. Some mods redirect the game's own call
// sites instead of hooking a function's entry (Building Tools 0.34.0 rewrites the building programs' E8 calls of
// ApplyMaterials to enter its code, which then calls ApplyMaterials itself). A Kiln hook that recognises the game's
// call by that return address then sees the mod's code instead. game_call_through_foreign finds the game's call behind
// it on the stack, and only when it is certain:
//  - the recorded return address (ret) is not the game's, and the walk of this thread's stack reaches it;
//  - every frame after it, up to the first one in the game's code, is foreign (at most max_foreign of them), and each has
//    unwind data (stack_returns stops at code without any, so a guess never stands in for a frame);
//  - the game's own bytes before that frame, as the exe file has them, are a direct call (E8 rel32) of the hooked target:
//    the game called the target there, and the module redirected that call.
// Otherwise 0, and the caller treats the call as before (not the game's). Header-only. Not a mod API: Kiln's services
// share it. Host test: tests/test-call-site.sh (the decision; the stack walk itself runs only in the game).
#ifndef KILN_CALL_SITE_HPP
#define KILN_CALL_SITE_HPP

#include <cstddef>
#include <cstdint>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#endif

namespace kiln {

// frames: return addresses from the innermost frame outward (stack_returns). in_game(a): a is in the game's exe.
// original_call(r): the target of the game's own call instruction that ends at r, as the exe file has it (0: none).
template <class InGame, class OriginalCall>
uintptr_t game_call_through_foreign(const uintptr_t* frames, size_t n, uintptr_t ret, uintptr_t target, InGame&& in_game,
                                    OriginalCall&& original_call, size_t max_foreign = 6) {
    if (!frames || !ret || !target || in_game(ret)) return 0;
    size_t i = 0;
    while (i < n && frames[i] != ret) ++i;
    if (i == n) return 0;  // the recorded call is not on this stack (a stale record): nothing is certain
    size_t foreign = 1;
    for (size_t k = i + 1; k < n; ++k) {
        if (in_game(frames[k])) return original_call(frames[k]) == target ? frames[k] : 0;
        if (++foreign > max_foreign) return 0;
    }
    return 0;  // the walk ended (code without unwind data, or the stack's end) before the game's code
}

// The target of a direct call (E8 rel32) whose 5 bytes are `call` and which returns to ret; 0 when it is not one.
inline uintptr_t call_target(const uint8_t call[5], uintptr_t ret) {
    if (call[0] != 0xe8) return 0;
    int32_t rel = 0;
    std::memcpy(&rel, call + 1, 4);
    return static_cast<uintptr_t>(static_cast<intptr_t>(ret) + rel);
}

#ifdef _WIN32
// Return addresses of this thread's stack from this function's caller outward, unwound by each frame's unwind data;
// stops at a frame whose code has none (dynamic code, a leaf, garbage) instead of guessing. Returns how many it wrote.
inline size_t stack_returns(uintptr_t* out, size_t cap) noexcept {
    size_t n = 0;
    __try {
        CONTEXT c;
        std::memset(&c, 0, sizeof c);
        RtlCaptureContext(&c);
        while (n < cap) {
            DWORD64 base = 0;
            PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(c.Rip, &base, nullptr);
            if (!f) break;
            PVOID handler = nullptr;
            DWORD64 establisher = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, c.Rip, f, &c, &handler, &establisher, nullptr);
            if (!c.Rip) break;
            out[n++] = static_cast<uintptr_t>(c.Rip);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return n;
}

// The game's exe: [lo, hi).
inline void game_image(uintptr_t* lo, uintptr_t* hi) noexcept {
    const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    *lo = base;
    *hi = base + nt->OptionalHeader.SizeOfImage;
}

// For a hook whose recorded caller (kiln.hooks' caller()) is ret: ret itself when it is in the game's code, the game's
// call of target behind another module's code (game_call_through_foreign on this thread's stack), or 0. read_original(at,
// out, n): the exe file's bytes (kiln.hooks' read_original); false when they cannot be read.
template <class ReadOriginal>
uintptr_t game_call_of(uintptr_t ret, uintptr_t target, ReadOriginal&& read_original) noexcept {
    uintptr_t lo = 0, hi = 0;
    game_image(&lo, &hi);
    const auto in_game = [&](uintptr_t a) { return a >= lo && a < hi; };
    if (!ret || in_game(ret)) return ret;
    uintptr_t frames[32];
    const size_t n = stack_returns(frames, 32);
    const auto original = [&](uintptr_t r) -> uintptr_t {
        uint8_t call[5] = {};
        return r >= 5 && read_original(r - 5, call, sizeof call) ? call_target(call, r) : 0;
    };
    return game_call_through_foreign(frames, n, ret, target, in_game, original);
}

// The file name of the module that holds code address a ("BuildingTools.dll"), or "another module".
inline void module_name(uintptr_t a, char* out, size_t cap) noexcept {
    if (!cap) return;
    out[0] = 0;
    HMODULE m = nullptr;
    wchar_t path[MAX_PATH] = {};
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(a), &m) &&
        GetModuleFileNameW(m, path, MAX_PATH)) {
        const wchar_t* name = path;
        for (const wchar_t* p = path; *p; ++p)
            if (*p == L'\\' || *p == L'/') name = p + 1;
        size_t i = 0;
        for (; name[i] && i + 1 < cap; ++i) out[i] = name[i] < 0x80 ? static_cast<char>(name[i]) : '?';
        out[i] = 0;
    }
    if (!out[0]) std::strncpy(out, "another module", cap - 1), out[cap - 1] = 0;
}
#endif

}  // namespace kiln

#endif /* KILN_CALL_SITE_HPP */
