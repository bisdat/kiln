// SPDX-License-Identifier: MIT
#include "../history_create.h"
#include "../../rules/material_scaling.h"
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <barrier>
#include <thread>

namespace {
int checks = 0;
void check(bool value, const char* message) {
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
    ++checks;
}
}

int main() {
    using kiln::tools::HistoryTags;
    HistoryTags<5> tags;
    constexpr uintptr_t first = 0x1000, second = 0x2000, other_player = 0x3000;
    constexpr uintptr_t materials = 0x110;
    check(!tags.exact(first + materials), "unrecorded vanilla list is not exact");
    check(tags.created(first, true), "add history can be tagged");
    check(tags.exact(first + materials), "add undo uses its original payment list");
    check(!tags.exact(first), "history pointer is not a material-list descriptor");
    check(!tags.exact(first + materials + 8), "material count pointer is not the descriptor");
    check(tags.created(second, false), "interleaved vanilla history is accepted");
    check(!tags.exact(second + materials), "interleaved vanilla undo retains rule scaling");
    check(tags.exact(first + materials), "vanilla interleaving retains preceding tool identity");
    check(tags.created(other_player, true), "another player's remove history can be tagged");
    check(tags.exact(other_player + materials), "remove undo takes exact original yields");
    check(tags.exact(first + materials), "different players retain independent identities");
    check(tags.created(first, false), "ring slot reuse for vanilla erases its prior tool tag");
    check(!tags.exact(first + materials), "reused vanilla entry cannot inherit exact refunds");
    check(tags.exact(other_player + materials), "another player's identity survives ring reuse");
    check(tags.created(second, true), "vanilla slot can later become a tool entry");
    check(tags.exact(second + materials), "tool ring reuse becomes exact");
    check(tags.created(second, true), "recording the same slot is idempotent");
    check(tags.exact(second + materials), "repeated exact-list query does not consume metadata");
    tags.reset();
    check(!tags.exact(second + materials) && !tags.exact(other_player + materials),
          "world teardown invalidates all pointer identities");
    check(!tags.created(0, true) && !tags.exact(0), "null entry and list are rejected");
    check(!tags.created(std::numeric_limits<uintptr_t>::max() - 0x100, true), "descriptor overflow is rejected");
    for (uintptr_t i = 1; i <= 5; ++i) check(tags.created(i * 0x1000, true), "bounded identity slots accept tool entries");
    check(!tags.created(0x6000, true), "full identity storage refuses instead of losing exactness");
    check(tags.created(0x1000, false) && tags.created(0x6000, true), "vanilla slot reuse releases bounded storage");
    check(!tags.exact(0x1110) && tags.exact(0x6110), "bounded storage replacement never aliases an old list");
    using kiln::rules::scale_materials;
    check(!scale_materials(1.0f, true, false), "default rules leave vanilla material counts intact");
    check(scale_materials(2.0f, true, false), "non-default vanilla refunds still scale");
    check(scale_materials(0.0f, true, false), "non-default vanilla costs still scale");
    check(!scale_materials(2.0f, true, true), "tool add refunds cannot be multiplied");
    check(!scale_materials(0.0f, true, true), "tool remove undo cannot waive yield takeback");
    check(!scale_materials(2.0f, false, false), "unrelated material operations remain unscaled");
    HistoryTags<1> small;
    uintptr_t next = first;
    int discarded = 0;
    auto begin = [&](uintptr_t allocator, uintptr_t storage) {
        check(allocator == 0xabc && storage == 0xdef, "native history creation preserves its context");
        return next;
    };
    auto discard = [&](uintptr_t storage) {
        check(storage == 0xdef, "failed policy history discards only the native player's pending entry");
        ++discarded;
    };
    check(kiln::tools::create_history(small, begin, discard, 0xabc, 0xdef, true) == first,
          "policy history creation returns the native entry");
    next = second;
    int evicted = 0;
    auto full_ring_begin = [&](uintptr_t allocator, uintptr_t storage) {
        ++evicted;  // Native HistoryBegin evicts the oldest entry before returning a new slot in a full ring.
        return begin(allocator, storage);
    };
    check(kiln::tools::create_history(small, full_ring_begin, discard, 0xabc, 0xdef, true) == 0 &&
          evicted == 0 && discarded == 0,
          "full sidecar refuses before native begin can evict an older vanilla undo entry");
    check(small.exact(first + materials) && !small.exact(second + materials),
          "failed history creation retains prior exact payment identity");
    next = first;
    check(kiln::tools::create_history(small, begin, discard, 0xabc, 0xdef, false) == first &&
          !small.exact(first + materials) && discarded == 0, "vanilla history reuse invalidates old policy identity");
    next = 0;
    check(kiln::tools::create_history(small, begin, discard, 0xabc, 0xdef, true) == 0 && discarded == 0,
          "native history failure never discards an unrelated entry");
    next = std::numeric_limits<uintptr_t>::max() - 0x100;
    check(kiln::tools::create_history(small, begin, discard, 0xabc, 0xdef, true) == 0 && discarded == 1,
          "untaggable nonzero native result discards its pending entry exactly once");
    next = second;
    check(kiln::tools::create_history(small, begin, discard, 0xabc, 0xdef, true) == second &&
          small.exact(second + materials), "native failure and malformed result release reserved capacity");
    small.reset();
    int nested_begins = 0;
    auto nested_begin = [&](uintptr_t allocator, uintptr_t storage) {
        auto inner = [&](uintptr_t, uintptr_t) { ++nested_begins; return second; };
        check(kiln::tools::create_history(small, inner, discard, allocator, storage, true) == 0 && nested_begins == 0,
              "nested policy cannot consume capacity reserved by its outer invocation");
        check(!small.exact(first + materials), "a pending reservation is not an exact history identity");
        return first;
    };
    check(kiln::tools::create_history(small, nested_begin, discard, 0xabc, 0xdef, true) == first &&
          small.exact(first + materials) && discarded == 1, "outer invocation commits its own reservation");
    HistoryTags<2> pair;
    auto nested_pair = [&](uintptr_t allocator, uintptr_t storage) {
        auto inner = [&](uintptr_t, uintptr_t) { return second; };
        check(kiln::tools::create_history(pair, inner, discard, allocator, storage, true) == second,
              "nested policy with spare capacity owns an independent reservation");
        return first;
    };
    check(kiln::tools::create_history(pair, nested_pair, discard, 0xabc, 0xdef, true) == first &&
          pair.exact(first + materials) && pair.exact(second + materials), "both nested histories remain exact");
    {
        HistoryTags<1> reset;
        auto old = reset.reserve();
        reset.reset();
        auto current = reset.reserve();
        check(!old.created(first) && current.created(second), "world reset invalidates older reservations");
    }
    HistoryTags<2> concurrent;
    std::barrier rendezvous(3);
    auto worker = [&](uintptr_t entry) {
        auto pending = [&](uintptr_t, uintptr_t) {
            rendezvous.arrive_and_wait();
            rendezvous.arrive_and_wait();
            return entry;
        };
        kiln::tools::create_history(concurrent, pending, [](uintptr_t) {}, 0, entry, true);
    };
    std::thread player_one(worker, first), player_two(worker, second);
    rendezvous.arrive_and_wait();
    check(!concurrent.reserve() && !concurrent.exact(first + materials) && !concurrent.exact(second + materials),
          "two players reserve bounded slots without exposing unfinished history as exact");
    rendezvous.arrive_and_wait();
    player_one.join(); player_two.join();
    check(concurrent.exact(first + materials) && concurrent.exact(second + materials),
          "concurrent players commit independent exact identities");
    // Exhaust every build/refund multiplier branch against the old rule decision. Only exact lists may differ.
    for (const float multiplier : {0.0f, 0.25f, 1.0f, 2.0f}) {
        for (const bool building : {false, true}) {
            check(scale_materials(multiplier, building, false) == (multiplier != 1.0f && building),
                  "every untagged building and non-building path keeps the original rule decision");
            check(!scale_materials(multiplier, building, true), "every exact-list multiplier is suppressed");
        }
    }
    std::printf("%d checks passed\n", checks);
}
