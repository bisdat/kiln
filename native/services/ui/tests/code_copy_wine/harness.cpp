// SPDX-License-Identifier: MIT
// code_copy_win.h in a Windows process under plain Wine (tests/test-code-copy-wine.sh): a function of this exe is copied
// from the exe file as kiln.ui copies the game's footer function, its call of `line` sent to `thunk`. The copy runs,
// reaches the thunk, reads and writes the exe's data where the original does, and an exception thrown in the thunk
// unwinds through the copy (its unwind information is registered).
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "../../code_copy_win.h"

static int g_fail = 0;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);     \
            ++g_fail;                                                    \
        }                                                                \
    } while (0)

volatile int g_data = 7;
static int g_thunk_calls = 0;

extern "C" __declspec(noinline) int line(int x) { return x * 3 + g_data; }

extern "C" __declspec(noinline) int thunk(int x) {
    ++g_thunk_calls;
    if (x == 99) throw 42;
    return line(x) + 1000;
}

// The "footer": a call to `line`, then work that uses its result and the exe's data (no tail call).
extern "C" __declspec(noinline) int footer(int x) {
    const int a = line(x);
    g_data = g_data + 1;
    return a * 2 + g_data;
}

int main() {
    // the call of `line` inside `footer` (its live bytes; nothing patched them)
    const auto fn = reinterpret_cast<uintptr_t>(&footer);
    const auto target = reinterpret_cast<uintptr_t>(&line);
    uintptr_t site = 0;
    for (uintptr_t p = fn; p < fn + 64 && !site; ++p) {
        int32_t rel = 0;
        std::memcpy(&rel, reinterpret_cast<const void*>(p + 1), 4);
        if (*reinterpret_cast<const uint8_t*>(p) == 0xE8 && p + 5 + rel == target) site = p;
    }
    CHECK(site != 0);
    code_copy::Copy c;
    std::string why;
    const bool made = site && code_copy::make(fn, site, reinterpret_cast<const void*>(&thunk), &c, &why);
    if (!made) std::printf("make: %s\n", why.c_str());
    CHECK(made);
    if (made) {
        const auto d = reinterpret_cast<intptr_t>(c.fn) - static_cast<intptr_t>(fn);
        CHECK(d > -(1ll << 31) && d < (1ll << 31));  // within rel32 reach
        CHECK(c.size >= 16 && c.file.size() == c.size);
        auto copy = reinterpret_cast<int (*)(int)>(c.fn);
        // copy: a = thunk(5) = line(5) + 1000 = 15 + 7 + 1000; g_data 8; 2 * 1022 + 8
        CHECK(copy(5) == 2052);
        CHECK(g_thunk_calls == 1 && g_data == 8);
        // the original is unchanged: a = line(5) = 15 + 8; g_data 9; 2 * 23 + 9
        CHECK(footer(5) == 55);
        CHECK(g_thunk_calls == 1 && g_data == 9);
        // an exception in the thunk passes through the copy's frame to here
        bool caught = false;
        try {
            copy(99);
        } catch (int v) {
            caught = v == 42;
        }
        CHECK(caught);
        // a function the copy cannot be made of: a call site outside it
        code_copy::Copy bad;
        CHECK(!code_copy::make(fn, fn + 0x4000, reinterpret_cast<const void*>(&thunk), &bad, &why));
    }
    std::printf("code_copy_wine: %s\n", g_fail ? "FAIL" : "ok");
    return g_fail ? 1 : 0;
}
