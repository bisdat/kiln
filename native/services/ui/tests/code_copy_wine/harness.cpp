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

// Two calls sent elsewhere, as kiln.ui sends the footer's window call and its build-number call: `win6` (six integer
// arguments, two of them on the stack, like the game's window call) through `win_thunk`, which calls what g_win_to
// names (the game's function, or another module's patched target), and `line` through `thunk`.
extern "C" __declspec(noinline) long long win6(long long a, long long b, long long c, long long d, long long e, long long f) {
    return a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * f + g_data;
}
extern "C" __declspec(noinline) long long other6(long long a, long long b, long long c, long long d, long long e, long long f) {
    return 100000 + win6(a, b, c, d, e, f);
}
using Win6 = long long (*)(long long, long long, long long, long long, long long, long long);
static Win6 volatile g_win_to = &win6;
static int g_win_calls = 0;
extern "C" __declspec(noinline) long long win_thunk(long long a, long long b, long long c, long long d, long long e, long long f) {
    ++g_win_calls;
    return g_win_to(a, b, c, d, e, f);
}
extern "C" __declspec(noinline) int footer2(int x) {
    const long long w = win6(x, 1, 2, 3, 4, 5);
    const int a = line(x);
    g_data = g_data + 1;
    return static_cast<int>(w + a * 2 + g_data);
}

static uintptr_t call_to(uintptr_t fn, uintptr_t target) {
    for (uintptr_t p = fn; p < fn + 96; ++p) {
        int32_t rel = 0;
        std::memcpy(&rel, reinterpret_cast<const void*>(p + 1), 4);
        if (*reinterpret_cast<const uint8_t*>(p) == 0xE8 && p + 5 + rel == target) return p;
    }
    return 0;
}

static void two_sites() {
    const auto fn = reinterpret_cast<uintptr_t>(&footer2);
    const uintptr_t sw = call_to(fn, reinterpret_cast<uintptr_t>(&win6)), sl = call_to(fn, reinterpret_cast<uintptr_t>(&line));
    CHECK(sw != 0 && sl != 0);
    if (!sw || !sl) return;
    const code_copy::Site sites[] = {{sw, reinterpret_cast<const void*>(&win_thunk)}, {sl, reinterpret_cast<const void*>(&thunk)}};
    code_copy::Copy c;
    std::string why;
    const bool made = code_copy::make(fn, sites, 2, &c, &why);
    if (!made) std::printf("make (two sites): %s\n", why.c_str());
    CHECK(made);
    if (!made) return;
    auto copy = reinterpret_cast<int (*)(int)>(c.fn);
    const int calls = g_thunk_calls;
    // the game's window function: w = 5 + 2 + 6 + 12 + 20 + 30 + g; a = 15 + g + 1000; g + 1 at the end
    int g = g_data;
    CHECK(copy(5) == (75 + g) + 2 * (1015 + g) + (g + 1));
    CHECK(g_win_calls == 1 && g_thunk_calls == calls + 1);
    // the patched target from now on (what kiln.ui does while it follows XHL-Core)
    g_win_to = &other6;
    g = g_data;
    CHECK(copy(5) == (100075 + g) + 2 * (1015 + g) + (g + 1));
    CHECK(g_win_calls == 2 && g_thunk_calls == calls + 2);
    // the original still calls the game's functions directly
    g = g_data;
    CHECK(footer2(5) == (75 + g) + 2 * (15 + g) + (g + 1));
    CHECK(g_win_calls == 2);
    // a site that is not a call refuses the copy
    code_copy::Copy bad;
    const code_copy::Site wrong[] = {{sw, reinterpret_cast<const void*>(&win_thunk)}, {sl + 1, reinterpret_cast<const void*>(&thunk)}};
    CHECK(!code_copy::make(fn, wrong, 2, &bad, &why));
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
    two_sites();
    std::printf("code_copy_wine: %s\n", g_fail ? "FAIL" : "ok");
    return g_fail ? 1 : 0;
}
