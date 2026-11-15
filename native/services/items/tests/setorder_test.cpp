// SPDX-License-Identifier: MIT
// kiln.items v6 host test: the order of one player's single equips and equipment sets (setorder.h; chapter 10 §4.5),
// driven through a run of steps as equip.cpp does it: each step's Pre decides which sets are ready (a decision ends) and
// which single equips wait (singles_wait_from), then the player's program update runs what run_now gives. The orders: two
// ready sets and a single equip after them; a ready set behind one still being decided; more single equips than one step
// takes, then a set; a set between two single equips; two players that do not hold each other back.
#include <cstdio>
#include <string>
#include <vector>

#include "../setorder.h"

using namespace kitems::sets;

namespace {
int failures = 0;
void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    failures += !ok;
}

struct Req {
    uint64_t ticket;
    uint32_t player;
    bool set;
    uint64_t ready_at;  // a set: the step whose Pre ends its decision (allowed); a single: submitted before step 1
    uint64_t ran = 0;   // the step it ran in
};

struct Run {
    std::vector<uint64_t> order;  // tickets in the order they ran
    bool two_sets_in_a_step = false;
};

constexpr uint32_t kCap = 4;  // equip.cpp's kPerPlayerStep

Run simulate(std::vector<Req>& q, uint64_t steps = 10) {
    Run r;
    for (uint64_t step = 1; step <= steps; ++step) {
        for (uint32_t player : {1u, 2u}) {
            // Pre
            std::vector<PendingSet> sets;
            for (const Req& x : q)
                if (!x.ran && x.player == player && x.set) sets.push_back({x.ticket, step >= x.ready_at});
            const uint64_t wait = singles_wait_from(sets);
            // the program
            std::vector<Queued> mine;
            for (const Req& x : q)
                if (!x.ran && x.player == player) mine.push_back({x.ticket, x.set, x.set ? step >= x.ready_at : x.ticket < wait});
            int sets_now = 0;
            for (uint64_t t : run_now(mine, kCap))
                for (Req& x : q)
                    if (x.ticket == t) {
                        x.ran = step;
                        sets_now += x.set;
                        r.order.push_back(t);
                    }
            r.two_sets_in_a_step = r.two_sets_in_a_step || sets_now > 1;
        }
    }
    return r;
}

std::string show(const std::vector<Req>& q, const Run& r) {
    std::string s;
    for (uint64_t t : r.order)
        for (const Req& x : q)
            if (x.ticket == t) s += (x.set ? "set " : "single ") + std::to_string(t) + "@" + std::to_string(x.ran) + " ";
    return s;
}

// Every request ran; each player's requests in ticket order; never two sets of one player in one step.
bool fifo(const std::vector<Req>& q, const Run& r) {
    for (const Req& x : q)
        if (!x.ran) return false;
    for (uint32_t player : {1u, 2u}) {
        uint64_t last = 0, last_step = 0;
        for (uint64_t t : r.order)
            for (const Req& x : q)
                if (x.ticket == t && x.player == player) {
                    if (t < last || x.ran < last_step) return false;
                    last = t, last_step = x.ran;
                }
    }
    return !r.two_sets_in_a_step;
}

void scenario(const char* name, std::vector<Req> q, const std::vector<uint64_t>& expected_steps = {}) {
    const Run r = simulate(q);
    bool steps_ok = true;
    for (size_t i = 0; i < expected_steps.size() && i < q.size(); ++i) steps_ok = steps_ok && q[i].ran == expected_steps[i];
    char what[400];
    std::snprintf(what, sizeof what, "%s: %s", name, show(q, r).c_str());
    check(fifo(q, r) && steps_ok, what);
}
}  // namespace

int main() {
    // 1. set A and set B ready in the same step, then single C: A, then B, then C (not A, C, B)
    scenario("two ready sets and a single equip after them run A, B, C", {{1, 1, true, 1}, {2, 1, true, 1}, {3, 1, false, 0}}, {1, 2, 2});
    // 2. set A still being decided, set B ready, single C: B does not pass A
    scenario("a ready set behind one still deciding waits for it", {{1, 1, true, 3}, {2, 1, true, 1}, {3, 1, false, 0}}, {3, 4, 4});
    // 3. five single equips, then a set: four singles in the first step, the fifth before the set in the next
    scenario("more single equips than one step takes, then a set: the set after the last of them",
             {{1, 1, false, 0}, {2, 1, false, 0}, {3, 1, false, 0}, {4, 1, false, 0}, {5, 1, false, 0}, {6, 1, true, 1}}, {1, 1, 1, 1, 2, 2});
    // 4. a single, a set, a single: one step, in ticket order
    scenario("a single, a set, a single run in one step in ticket order", {{1, 1, false, 0}, {2, 1, true, 1}, {3, 1, false, 0}}, {1, 1, 1});
    // 5. another player's waiting set holds back nobody here
    scenario("another player's set still deciding does not hold this player back", {{1, 2, true, 5}, {2, 1, false, 0}, {3, 1, true, 1}, {4, 2, false, 0}},
             {5, 1, 1, 5});
    // 6. three ready sets: one per step, in ticket order, the single after the last
    scenario("three ready sets: one per step, then the single", {{1, 1, true, 1}, {2, 1, true, 1}, {3, 1, true, 1}, {4, 1, false, 0}}, {1, 2, 3, 3});

    // the two functions alone
    check(singles_wait_from({}) == UINT64_MAX, "no set: no single equip waits");
    check(singles_wait_from({{5, true}}) == UINT64_MAX, "one ready set: no single waits on it (it runs first at its place)");
    check(singles_wait_from({{5, false}}) == 5, "one set not ready: the singles after it wait");
    check(singles_wait_from({{9, true}, {5, true}}) == 9, "two ready sets: the singles after the second wait");
    check(singles_wait_from({{9, true}, {5, false}}) == 5, "the oldest set not ready: everything after it waits");
    check(run_now({{3, true, true}, {1, false, false}}, kCap).empty(), "a single not verified holds back the set after it");
    check(run_now({{2, true, true}, {4, true, true}}, kCap) == std::vector<uint64_t>{2}, "one set per update");

    std::printf("%s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
