// SPDX-License-Identifier: MIT
// Host tests of natives_core.h: the decisions kiln.abilities' native thunks make (since 0.2.0-dev.29).
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "natives_core.h"

using namespace kab;

static int g_failed = 0, g_passed = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (cond) {                                                              \
            ++g_passed;                                                          \
        } else {                                                                 \
            ++g_failed;                                                          \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);          \
        }                                                                        \
    } while (0)

static void names() {
    CHECK(valid_native_name("mymod.OnHit"));
    CHECK(valid_native_name("a.b"));
    CHECK(valid_native_name("my-mod.On_Hit2"));
    CHECK(!valid_native_name("OnHit"));
    CHECK(!valid_native_name(".OnHit"));
    CHECK(!valid_native_name("mymod."));
    CHECK(!valid_native_name("mymod.On-Hit"));
    CHECK(!valid_native_name("mymod.On.Hit"));
    char longname[100];
    std::memset(longname, 'a', sizeof longname);
    longname[3] = '.';
    CHECK(!valid_native_name(std::string_view(longname, 91)));
    CHECK(valid_native_name(std::string_view(longname, 90)));
}

static void room() {
    // measured in the lab (2026-11-15, server 26430253): the game's RegisterNative takes no new key once the map holds
    // half its capacity (128 of 256; 107 are the game's own), so Kiln refuses first, with the reason
    CHECK(registry_has_room(107, 256));
    CHECK(registry_has_room(127, 256));
    CHECK(!registry_has_room(128, 256));
    CHECK(!registry_has_room(254, 256));
    CHECK(!registry_has_room(256, 256));
    CHECK(!registry_has_room(0, 0));  // unreadable map
    CHECK(!registry_has_room(0xFFFFFFFFu, 256));
}

static void results() {
    KilnNativeCall c;
    std::memset(&c, 0, sizeof c);
    uint8_t e = 9, v = 9;
    c.out_error = kUndecided;
    c.out_value = 7;
    sync_result(c, &e, &v);
    CHECK(e == 0 && v == 0);  // fn set nothing: error 0, value 0
    c.out_error = 0;
    c.out_value = 1;
    sync_result(c, &e, &v);
    CHECK(e == 0 && v == 1);
    c.out_error = kErrInternal;
    c.out_value = 0;
    sync_result(c, &e, &v);
    CHECK(e == kErrInternal);
}

static void faults() {
    // a wrapped native: run the game's function once; never twice
    CHECK(fault_answer(ThunkKind::Wrapped, false, false, true) == FaultAnswer::RunOriginal);
    CHECK(fault_answer(ThunkKind::Wrapped, false, true, true) == FaultAnswer::KeepResult);
    CHECK(fault_answer(ThunkKind::Wrapped, false, true, false) == FaultAnswer::KeepResult);
    CHECK(fault_answer(ThunkKind::Wrapped, false, false, false) == FaultAnswer::Unknown);
    // a fault inside the game's own native is never retried
    CHECK(fault_answer(ThunkKind::Wrapped, true, true, true) == FaultAnswer::Unknown);
    // a sync native: Internal (0x11), whatever ran
    CHECK(fault_answer(ThunkKind::Sync, false, false, false) == FaultAnswer::Internal);
    CHECK(fault_answer(ThunkKind::Sync, false, true, false) == FaultAnswer::Internal);
    CHECK(fault_answer(ThunkKind::Other, false, false, true) == FaultAnswer::Unknown);
}

static void budget() {
    const int64_t f = 10'000'000;  // 10 MHz, QueryPerformanceFrequency's usual value
    CHECK(!over_budget(2000, f));   // 0.2 ms exactly: within
    CHECK(over_budget(2001, f));
    CHECK(!over_budget(100, f));
    CHECK(!over_budget(1'000'000, 0));  // no frequency: never
}

int main() {
    names();
    room();
    results();
    faults();
    budget();
    std::printf("natives_test: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
