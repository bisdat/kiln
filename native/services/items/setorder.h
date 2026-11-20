// SPDX-License-Identifier: MIT
// kiln.items v6: the order of one player's equip requests, single equips and equipment sets together, FIFO by ticket
// (chapter 10 §4.5). Pure: equip.cpp's Pre (which single equips wait) and its program (what one update of the player runs)
// both decide through these two functions, and tests/setorder_test.cpp drives them through a run of steps.
//
// The rules: a player's requests run in ticket order. At most one set runs per player and step, and only the player's
// oldest set can be the one (a later set that is ready waits for an earlier one that is still being decided). A single
// equip waits while any set before it has not run; a set waits while any request before it has not run.
#pragma once
#include <algorithm>
#include <cstdint>
#include <vector>

namespace kitems::sets {

// One pending set of a player as Pre sees it after its checks.
struct PendingSet {
    uint64_t ticket = 0;
    bool ready = false;  // allowed, its session verified in this step's Pre: its program can run it this step
};

// The ticket from which the player's single equips wait this step (UINT64_MAX: none waits). Only the player's oldest set
// can run this step, and only when it is ready; it holds back no single equip after it (the program runs it first, at its
// place in ticket order), but every other set of the player holds back the single equips after it.
inline uint64_t singles_wait_from(const std::vector<PendingSet>& sets) {
    uint64_t oldest = UINT64_MAX, second = UINT64_MAX;
    bool oldest_ready = false;
    for (const PendingSet& s : sets) {
        if (s.ticket < oldest) {
            second = oldest;
            oldest = s.ticket;
            oldest_ready = s.ready;
        } else if (s.ticket < second) {
            second = s.ticket;
        }
    }
    return oldest_ready ? second : oldest;
}

// One queued request of the player inside the program.
struct Queued {
    uint64_t ticket = 0;
    bool set = false;
    bool ready = false;  // a single equip verified in this step's Pre; a set allowed and verified in this step's Pre
};

// The tickets one program update of the player runs, in ticket order: the longest run of ready requests from the oldest,
// with at most `cap` single equips and at most one set in it. A request that cannot run now holds back every later one.
inline std::vector<uint64_t> run_now(std::vector<Queued> q, uint32_t cap) {
    std::sort(q.begin(), q.end(), [](const Queued& a, const Queued& b) { return a.ticket < b.ticket; });
    std::vector<uint64_t> out;
    uint32_t singles = 0;
    bool set = false;
    for (const Queued& r : q) {
        if (!r.ready) break;
        if (r.set) {
            if (set) break;
            set = true;
        } else {
            if (singles == cap) break;
            ++singles;
        }
        out.push_back(r.ticket);
    }
    return out;
}

}  // namespace kitems::sets
