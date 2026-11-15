// SPDX-License-Identifier: MIT
// Exercise the production thunk, extracted by run.sh, with a fake VM dispatch.
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
struct NativeResult { uint8_t error; uint32_t zero; uint8_t value; };
struct ArgSlice {};
struct NativeCtx {};
std::atomic<uint32_t> g_inflight{0};
thread_local int t_slot = -1;
thread_local bool t_orig_ran = false, t_in_game = false;
thread_local int32_t t_stage = 0;
struct OwnerGate;
thread_local OwnerGate* t_gate = nullptr;
thread_local int t_inside = -1;
int seh_dispatch(int, NativeResult*, ArgSlice*, NativeCtx*);
void on_fault(int, NativeResult*, ArgSlice*, NativeCtx*) {}
#include "thunk_under_test.h"
int seh_dispatch(int slot, NativeResult*, ArgSlice* args, NativeCtx* ctx) {
    if (slot == 0) {
        t_stage = 2; // outer After: original was suppressed, CallOriginal must remain forbidden
        NativeResult nested{};
        thunk<1>(&nested, args, ctx);
        assert(t_stage == 2);
        assert(t_slot == 0 && !t_orig_ran && !t_in_game);
        assert(g_inflight == 1);
    } else {
        t_stage = 0; // nested wrapped dispatch resets its stage to Before
        t_orig_ran = true;
    }
    return 0;
}
int main() {
    NativeResult out{};
    ArgSlice args;
    NativeCtx ctx;
    thunk<0>(&out, &args, &ctx);
    assert(g_inflight == 0 && t_slot == -1 && t_stage == 0);
    puts("thunk_test: nested stage, latch, slot and in-flight restoration passed");
}
