// SPDX-License-Identifier: MIT
// Host tests of natives_core.h: the decisions kiln.abilities' native thunks make (since 0.2.0-dev.29).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

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

static Attachment att(uint64_t id, int32_t stage, int32_t priority, uint32_t program = 0, uint32_t site = 0, uint32_t hash = 7) {
    Attachment a;
    a.id = id;
    a.order = id;
    a.hash = hash;
    a.stage = stage;
    a.priority = priority;
    a.program = program;
    a.site = site;
    return a;
}

static void stages() {
    // registration order: After, Instead, Before 5, Before 10, Before 10 (tie), an other native's Before
    std::vector<Attachment> all = {att(1, KILN_NATIVE_AFTER, 0),  att(2, KILN_NATIVE_INSTEAD, 0), att(3, KILN_NATIVE_BEFORE, 5),
                                   att(4, KILN_NATIVE_BEFORE, 10), att(5, KILN_NATIVE_BEFORE, 10), att(6, KILN_NATIVE_BEFORE, 1, 0, 0, 8),
                                   att(7, KILN_NATIVE_AFTER, -3)};
    const AttachSet set = build_set(all, 7);
    CHECK(set.before.size() == 3 && set.instead.size() == 1 && set.after.size() == 2);
    std::vector<uint64_t> order;
    bool answered = false, ran = false;
    uint64_t entered = 0, skipped = 0;
    run_stages(set, 0x11, 0x22, [&](const Attachment& a, int32_t) { order.push_back(a.id); if (a.stage == KILN_NATIVE_INSTEAD) answered = true; },
               [&] { ran = true; }, [&] { return answered || ran; }, &entered, &skipped);
    // Before high to low (ties by registration), the Instead, After low to high; the Instead answered: no original
    CHECK((order == std::vector<uint64_t>{4, 5, 3, 2, 7, 1}));
    CHECK(!ran && entered == 6 && skipped == 0);

    // a Before that answers: no Instead, no original, the After still runs
    order.clear();
    answered = ran = false;
    run_stages(set, 0x11, 0x22, [&](const Attachment& a, int32_t) { order.push_back(a.id); if (a.id == 5) answered = true; },
               [&] { ran = true; }, [&] { return answered || ran; }, &entered, &skipped);
    CHECK((order == std::vector<uint64_t>{4, 5, 3, 7, 1}) && !ran);

    // nothing answers: the original runs once, between the stages
    order.clear();
    answered = ran = false;
    int originals = 0;
    run_stages(set, 0x11, 0x22, [&](const Attachment& a, int32_t) { order.push_back(a.id); },
               [&] { ran = true; ++originals; order.push_back(99); }, [&] { return answered || ran; }, &entered, &skipped);
    CHECK((order == std::vector<uint64_t>{4, 5, 3, 2, 99, 7, 1}) && originals == 1);
}

static void filters() {
    std::vector<Attachment> all = {att(1, KILN_NATIVE_BEFORE, 0, 0xAAAA, 0x1), att(2, KILN_NATIVE_BEFORE, 0, 0xAAAA),
                                   att(3, KILN_NATIVE_BEFORE, 0, 0, 0x2), att(4, KILN_NATIVE_INSTEAD, 0, 0xBBBB)};
    const AttachSet set = build_set(all, 7);
    std::vector<uint64_t> order;
    uint64_t entered = 0, skipped = 0;
    bool ran = false;
    auto call = [&](uint32_t program, uint32_t site) {
        order.clear();
        entered = skipped = 0;
        ran = false;
        run_stages(set, program, site, [&](const Attachment& a, int32_t) { order.push_back(a.id); }, [&] { ran = true; },
                   [&] { return ran; }, &entered, &skipped);
    };
    call(0xAAAA, 0x1);
    CHECK((order == std::vector<uint64_t>{1, 2}) && entered == 2 && skipped == 2 && ran);
    call(0xCCCC, 0x9);  // matches nothing: no handler entered, every filter counted as skipped
    CHECK(order.empty() && entered == 0 && skipped == 4 && ran);
    call(0xBBBB, 0x2);
    CHECK((order == std::vector<uint64_t>{3, 4}));
    // an attachment whose program was not found stays off
    all[1].active = false;
    const AttachSet off = build_set(all, 7);
    order.clear();
    run_stages(off, 0xAAAA, 0x5, [&](const Attachment& a, int32_t) { order.push_back(a.id); }, [&] {}, [] { return false; },
               &entered, &skipped);
    CHECK(order.empty());
    // one Instead per (native, program, site)
    CHECK(instead_taken(all, 7, 0xBBBB, 0));
    CHECK(!instead_taken(all, 7, 0xBBBB, 0x3));
    CHECK(!instead_taken(all, 8, 0xBBBB, 0));
    CHECK(build_set(all, 9).empty());
}

static void sites() {
    // ICONST h; ECALL site hash; POP; BR 0 with an operand that looks like ECALL; HALT
    const uint32_t code[] = {11, 0xFFFF0000u, 19, 0x1234, 0xa18f5959u, 27, 11, 19, 8, 0, 30};
    CHECK(code_calls(code, 11, 0xa18f5959u, 0x1234));
    CHECK(code_calls(code, 11, 0xa18f5959u, 0));
    CHECK(!code_calls(code, 11, 0xa18f5959u, 0x9999));
    CHECK(!code_calls(code, 11, 0x01d09759u, 0));
    // the ECALL-looking operand of ICONST (19 at index 7) is not an instruction
    const uint32_t tricky[] = {11, 19, 11, 0x1234, 30};
    CHECK(!code_calls(tricky, 5, 0x1234, 0));
    const uint32_t bad[] = {99, 19, 1, 2};
    CHECK(!code_calls(bad, 4, 2, 1));
    const uint32_t cut[] = {19, 0x1234};  // operands run past the end
    CHECK(!code_calls(cut, 2, 0, 0x1234));
}

int main() {
    names();
    stages();
    filters();
    sites();
    room();
    results();
    faults();
    budget();
    std::printf("natives_test: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
