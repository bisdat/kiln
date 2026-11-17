// SPDX-License-Identifier: MIT
// kiln.stations host test: request checks, the fill's arithmetic against the game's own (AutoTransferRecipe,
// docs/re/stations.md §3.1), the base id rule, the stop-after-craft rule, the state diff, the event ring, the unlock
// verdict, and the ABI's sizes (the managed mirror in Kiln.Stations depends on them).
#include "../stations_logic.h"

#include <cstddef>
#include <cstdio>
#include <cstring>

using namespace kitems::stations;

namespace {
int checks = 0, failed = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failed; std::printf("FAIL %d %s\n", __LINE__, #x); } } while (0)

KilnStationRequest req(uint32_t action) {
    KilnStationRequest r{};
    r.size = sizeof r;
    r.action = action;
    r.station = 1318;
    if (action == KILN_STATION_FILL) r.crafts = 5;
    return r;
}

// The game's AutoTransferRecipe, as read from the code (docs/re/stations.md §3.1), on plain numbers: returns 0 and
// fills `moved` per input, or 4 when the stock is short, 2 when the station cannot hold it (water: its capacity). The
// water input (kWaterItem) counts in "loaded" as the game counts it. Used to check the planner never asks for what the
// game refuses.
int game_fill(const std::vector<InputNow>& in, uint32_t n, std::vector<uint32_t>* moved) {
    const uint32_t loaded = crafts_loaded(in);
    moved->assign(in.size(), 0);
    for (size_t k = 0; k < in.size(); ++k) {
        const uint32_t need = game_need(in[k], n, loaded);
        if (need > in[k].stock) return 4;
        if (in[k].room != UINT64_MAX && in[k].have + need > in[k].room) return 2;
        (*moved)[k] = need;
    }
    return 0;
}
}  // namespace

int main() {
    // ---- ABI sizes (the managed mirror) ----
    static_assert(sizeof(KilnBaseId) == 16 && sizeof(KilnStationChoice) == 8 && sizeof(KilnStationStack) == 8);
    static_assert(sizeof(KilnStationBase) == 432 && offsetof(KilnStationBase, altar_ids) == 92 && offsetof(KilnStationBase, min) == 368);
    static_assert(sizeof(KilnStation) == 200 && offsetof(KilnStation, position) == 32 && offsetof(KilnStation, state) == 56 &&
                  offsetof(KilnStation, crafts_loaded) == 132 && offsetof(KilnStation, recipe_start_ns) == 136 &&
                  offsetof(KilnStation, progress) == 168 && offsetof(KilnStation, flags) == 180);
    static_assert(sizeof(KilnStationInput) == 16 && sizeof(KilnStationRecipe) == 624 && offsetof(KilnStationRecipe, inputs) == 48);
    static_assert(sizeof(KilnStationRequest) == 88 && offsetof(KilnStationRequest, choices) == 24 &&
                  offsetof(KilnStationRequest, keep) == 64);
    static_assert(sizeof(KilnItemsMoveChange) == 32);
    static_assert(sizeof(KilnStationResult) == 1232 && offsetof(KilnStationResult, limited_by) == 52 &&
                  offsetof(KilnStationResult, short_item) == 56 && offsetof(KilnStationResult, ticket) == 64 &&
                  offsetof(KilnStationResult, changes) == 80 && offsetof(KilnStationResult, message) == 1104);
    static_assert(sizeof(KilnStationPlan) == 696 && offsetof(KilnStationPlan, short_item) == 36 &&
                  offsetof(KilnStationPlan, water_per_craft) == 44 && offsetof(KilnStationPlan, inputs) == 56);
    static_assert(sizeof(KilnStationEvent) == 112 && offsetof(KilnStationEvent, by) == 64 && offsetof(KilnStationEvent, position) == 72 &&
                  offsetof(KilnStationEvent, base) == 96);
    static_assert(sizeof(KilnStationsStatus) == 188);
    static_assert(sizeof(KilnStationsApi) == 8 + 21 * 8);

    // ---- requests ----
    CHECK(request_valid(req(KILN_STATION_FILL)));
    CHECK(request_valid(req(KILN_STATION_STOP)));
    CHECK(request_valid(req(KILN_STATION_RESUME)));
    auto sel = req(KILN_STATION_SELECT);
    sel.recipe = 0x5568cc2b;
    CHECK(request_valid(sel));
    sel.recipe = 0;  // clears the selection
    CHECK(request_valid(sel));
    auto r = req(KILN_STATION_FILL);
    r.crafts = 0;
    CHECK(!request_valid(r));
    r.crafts = 1001;
    CHECK(!request_valid(r));
    r = req(KILN_STATION_FILL);
    r.flags = KILN_STATION_AFTER_CRAFT;  // a stop flag on a fill
    CHECK(!request_valid(r));
    r = req(KILN_STATION_FILL);
    r.keep_count = 2;  // no list
    CHECK(!request_valid(r));
    r = req(KILN_STATION_FILL);
    r.choices[0].category = 0xee5ae93f;  // choices without a recipe
    CHECK(!request_valid(r));
    r.recipe = 0x5568cc2b;
    CHECK(request_valid(r));
    r.choices[0].reserved[1] = 1;
    CHECK(!request_valid(r));
    r = req(KILN_STATION_STOP);
    r.flags = KILN_STATION_AFTER_CRAFT;
    CHECK(request_valid(r));
    r.crafts = 3;
    CHECK(!request_valid(r));
    r = req(KILN_STATION_RESUME);
    r.flags = KILN_STATION_ALLOW_LOCKED;
    CHECK(!request_valid(r));
    r = req(KILN_STATION_FILL);
    r.station = 0;
    CHECK(!request_valid(r));
    r = req(KILN_STATION_FILL);
    r.reserved[3] = 1;
    CHECK(!request_valid(r));
    r = req(7);
    CHECK(!request_valid(r));
    // FILL's player: a player entity (1..1023) or 0; never another entity (a chest's id would make the game take from it)
    r = req(KILN_STATION_FILL);
    r.player = 2550;
    CHECK(!request_valid(r));
    r.player = 1;
    CHECK(request_valid(r));
    r.player = 1023;
    CHECK(request_valid(r));

    // ---- the fill's arithmetic: the charcoal kiln (12 of category wood, 3 of another per craft) ----
    std::vector<InputNow> in{{0x2b0cf1d5, 12, 30, 100}, {0x07e67b5b, 3, 4, 20}};
    CHECK(crafts_loaded(in) == 1);                   // 30/12 = 2, 4/3 = 1
    CHECK(game_need(in[0], 4, 1) == 5 * 12 - 30);     // (n + loaded) * count - have
    CHECK(game_need(in[1], 4, 1) == 5 * 3 - 4);
    CHECK(game_need(in[0], 0, 1) == 0);
    const uint32_t aff = crafts_affordable(in, 1);    // wood (30+100)/12 = 10 -> 9 more; other (4+20)/3 = 8 -> 7 more
    CHECK(aff == 7);
    std::vector<uint32_t> moved;
    CHECK(game_fill(in, aff, &moved) == 0);           // the game accepts what the planner offers
    CHECK(game_fill(in, aff + 1, &moved) == 4);       // and refuses one more
    CHECK(crafts_wanted(0, 5, 1) == 4);               // UpTo 5 with 1 loaded
    CHECK(crafts_wanted(0, 1, 3) == 0);               // already more than asked
    CHECK(crafts_wanted(KILN_STATION_FILL_ADD, 5, 1) == 5);
    CHECK(crafts_first_try(0, 20, 7) == 7);
    CHECK(crafts_first_try(KILN_STATION_FILL_EXACT, 20, 7) == 0);   // EXACT and the stock (after reserves) short: no call
    CHECK(crafts_first_try(KILN_STATION_FILL_EXACT, 7, 7) == 7);
    CHECK(crafts_next_try(0, 7, 1) == 3);
    CHECK(crafts_next_try(0, 1, 1) == 0);
    CHECK(crafts_next_try(KILN_STATION_FILL_EXACT, 7, 1) == 0);
    CHECK(crafts_next_try(0, 64, kMaxTries) == 0);
    // reserves
    CHECK(after_keep(100, 40) == 60);
    CHECK(after_keep(10, 40) == 0);
    std::vector<InputNow> kept{{1, 12, 30, after_keep(100, 90)}, {2, 3, 4, 20}};  // 10 wood left to take
    CHECK(crafts_affordable(kept, crafts_loaded(kept)) == 2);  // (30 + 10) / 12 = 3 -> 2 more
    // an input with count 0 (the data has none; the game moves nothing for it and loaded is 0)
    std::vector<InputNow> zero{{1, 0, 5, 5}, {2, 3, 0, 9}};
    CHECK(crafts_loaded(zero) == 0);
    CHECK(crafts_affordable(zero, 0) == 3);
    // the game's u16 clamp: never plan more than 0xFFFF of one input per call
    std::vector<InputNow> big{{1, 1, 0, 1000000}};
    CHECK(crafts_affordable(big, 0) == kGameMaxCount);
    CHECK(game_need(big[0], 70000, 0) == kGameMaxCount);
    CHECK(crafts_loaded({}) == 0 && crafts_affordable({}, 0) == 0);

    // ---- stop after the running craft ----
    CHECK(after_craft_due(KILN_STATION_STATE_NO_RECIPE, 0, 5, 5));
    CHECK(!after_craft_due(KILN_STATION_STATE_RUNNING, 0x5568cc2b, 5, 5));   // the same cycle runs
    CHECK(after_craft_due(KILN_STATION_STATE_RUNNING, 0x5568cc2b, 5, 9));    // a new cycle started: the old one ended
    CHECK(!after_craft_due(KILN_STATION_STATE_OUTPUT_FULL, 0x5568cc2b, 5, 5)); // outputs waiting: a stop now refunds the inputs
    CHECK(!after_craft_due(KILN_STATION_STATE_PAUSED, 0x5568cc2b, 5, 5));      // paused mid-cycle: a stop now loses it
    CHECK(after_craft_due(KILN_STATION_STATE_MISSING_MATERIALS, 0x5568cc2b, 5, 5));

    // ---- water counts in "loaded" (the game's count), unless the request pours none ----
    {
        std::vector<InputNow> w{{0x2b0cf1d5, 12, 60, 100}};  // items for 5 crafts in the station
        InputNow water{};
        CHECK(water_input(1, false, 0, 256, 1000, &water) && water.item == kWaterItem && water.count == 64);
        w.push_back(water);
        CHECK(crafts_loaded(w) == 0);  // no water: no complete craft
        PlanMath m = plan_math(0, 5, w);  // up to 5: the water for them (the station holds 4 crafts' worth)
        CHECK(m.loaded == 0 && m.wanted == 5 && m.planned == 4 && m.status == KILN_STATION_PARTIAL);
        CHECK(m.limit.why == KILN_STATION_LIMIT_WATER);
        std::vector<uint32_t> moved;
        CHECK(game_fill(w, m.planned, &moved) == 0 && moved[0] == 0 && moved[1] == 4 * 64);
        CHECK(game_fill(w, m.planned + 1, &moved) == 2);
        w[1].have = 64;  // one craft's water
        PlanMath add = plan_math(KILN_STATION_FILL_ADD, 1, w);
        CHECK(add.loaded == 1 && add.planned == 1 && add.loaded + add.planned == 2);  // not 6
        CHECK(!water_input(1, true, 0, 256, 1000, &water));  // NO_WATER: items alone
        CHECK(!water_input(0, false, 0, 256, 1000, &water)); // a recipe without water
        std::vector<InputNow> dry{{0x2b0cf1d5, 12, 60, 100}};
        CHECK(plan_math(0, 5, dry).status == KILN_STATION_UNCHANGED);
        w[1].stock = 0;
        w[1].have = 0;
        PlanMath none = plan_math(0, 1, w);
        CHECK(none.planned == 0 && none.status == KILN_STATION_NOT_ENOUGH && none.limit.why == KILN_STATION_LIMIT_WATER);
    }
    // ---- the player's bag, reserves, EXACT ----
    {
        std::vector<InputNow> bag{{1, 12, 0, after_keep(10, 10) + 24, 10}};  // the base's 10 all reserved, 24 in the bag
        CHECK(plan_math(0, 5, bag).planned == 2);
        std::vector<InputNow> res{{1, 12, 0, after_keep(30, 20), 20}};        // 10 may be taken: not one craft
        PlanMath e = plan_math(KILN_STATION_FILL_EXACT, 1, res);
        CHECK(e.planned == 0 && e.status == KILN_STATION_NOT_ENOUGH && e.limit.why == KILN_STATION_LIMIT_RESERVE && e.limit.item == 1);
        std::vector<InputNow> ok{{1, 12, 0, after_keep(50, 20), 20}};         // 30 may be taken: 2 crafts
        CHECK(plan_math(KILN_STATION_FILL_EXACT, 2, ok).planned == 2);
        CHECK(plan_math(KILN_STATION_FILL_EXACT, 3, ok).planned == 0);
        std::vector<InputNow> short_{{1, 12, 0, 5}, {2, 3, 0, 100}};
        PlanMath s = plan_math(0, 1, short_);
        CHECK(s.status == KILN_STATION_NOT_ENOUGH && s.limit.why == KILN_STATION_LIMIT_STOCK && s.limit.item == 1);
    }
    // ---- one item named twice is one input ----
    {
        std::vector<InputNow> dup{{1, 12, 30, 0}, {1, 3, 30, 0}, {2, 1, 9, 0}};
        merge_inputs(&dup);
        CHECK(dup.size() == 2 && dup[0].count == 15 && crafts_loaded(dup) == 2);
    }
    // ---- the game's function, tried with less after a refusal ----
    {
        int calls = 0;
        auto room3 = [&](uint32_t n) { ++calls; return n > 3 ? 2 : 0; };  // the station takes 3 crafts' inputs
        Attempts a = run_attempts(0, 13, room3);
        CHECK(a.ok && a.n == 3 && a.tries == 3 && calls == 3 && fill_status(a, 13) == KILN_STATION_PARTIAL);
        Attempts x = run_attempts(KILN_STATION_FILL_EXACT, 13, room3);  // all or nothing: no smaller try
        CHECK(!x.ok && x.tries == 1 && fill_status(x, 13) == KILN_STATION_NO_ROOM);
        Attempts never = run_attempts(0, 64, [](uint32_t) { return 4; });
        CHECK(!never.ok && never.tries == kMaxTries && fill_status(never, 64) == KILN_STATION_NOT_ENOUGH);
        Attempts fault = run_attempts(0, 64, [](uint32_t) { return kAttemptFault; });
        CHECK(!fault.ok && fault.tries == 1);
        CHECK(fill_status(run_attempts(0, 5, [](uint32_t) { return 0; }), 5) == KILN_STATION_DONE);
        CHECK(fill_status(run_attempts(0, 5, [](uint32_t) { return 5; }), 5) == KILN_STATION_REFUSED);
        CHECK(!run_attempts(0, 0, room3).ok);  // nothing planned: no call
    }
    // ---- a waiting stop, step by step ----
    {
        StopSeen s{};
        s.present = true;
        s.state = KILN_STATION_STATE_RUNNING;
        s.running = 0x5568cc2b;
        s.selected = 0x5568cc2b;
        s.start = 100;
        s.now = 50'000'000'000;
        CHECK(waiting_stop(s, 0x5568cc2b, 100, 10, 1000) == StopWait::Wait);      // the same cycle runs
        CHECK(waiting_stop(s, 0x5568cc2b, 100, 1001, 1000) == StopWait::TimedOut); // too long: nothing written
        StopSeen p = s;
        p.state = KILN_STATION_STATE_PAUSED;
        CHECK(waiting_stop(p, 0x5568cc2b, 100, 10, 1000) == StopWait::Wait);
        p.state = KILN_STATION_STATE_OUTPUT_FULL;
        CHECK(waiting_stop(p, 0x5568cc2b, 100, 10, 1000) == StopWait::Wait);
        StopSeen resumed = s;
        resumed.start = s.now - 30'000'000'000;  // resumed from a pause: start = now - what it had run
        CHECK(waiting_stop(resumed, 0x5568cc2b, 100, 10, 1000) == StopWait::Wait);
        StopSeen fresh = s;
        fresh.start = s.now - 40'000'000;  // a new cycle, one step old
        CHECK(waiting_stop(fresh, 0x5568cc2b, 100, 10, 1000) == StopWait::Write);
        StopSeen between = s;
        between.state = KILN_STATION_STATE_NO_RECIPE;  // the step between two cycles
        CHECK(waiting_stop(between, 0x5568cc2b, 100, 10, 1000) == StopWait::Write);
        StopSeen stopped = s;
        stopped.stopped = 1;
        CHECK(waiting_stop(stopped, 0x5568cc2b, 100, 10, 1000) == StopWait::Unchanged);
        StopSeen other = s;
        other.selected = 0x1234;
        CHECK(waiting_stop(other, 0x5568cc2b, 100, 10, 1000) == StopWait::Canceled);
        StopSeen gone{};
        CHECK(waiting_stop(gone, 0x5568cc2b, 100, 10, 1000) == StopWait::Gone);
        auto stop = req(KILN_STATION_STOP);
        CHECK(withdraws_waiting(stop));
        stop.flags = KILN_STATION_AFTER_CRAFT;
        CHECK(!withdraws_waiting(stop));
        CHECK(withdraws_waiting(req(KILN_STATION_RESUME)) && withdraws_waiting(req(KILN_STATION_SELECT)));
        CHECK(!withdraws_waiting(req(KILN_STATION_FILL)));
    }
    // ---- the queue: withdraw a station's waiting stops, flush a list ----
    {
        std::vector<Req> after(3);
        after[0].r.station = 10;
        after[1].r.station = 11;
        after[2].r.station = 10;
        auto w = take_if(&after, [](const Req& q) { return q.r.station == 10; });
        CHECK(w.size() == 2 && after.size() == 1 && after[0].r.station == 11);
        auto all = take_if(&after, [](const Req&) { return true; });
        CHECK(all.size() == 1 && after.empty());
    }

    // ---- one step's requests: withdrawals in order, after the action passed ----
    {
        // a fake game: station 10 runs a cycle (a deferred stop waits), station 11's recipe is not this workshop's
        auto act = [](Req& q, KilnStationResult* res) {
            if (is_deferred_stop(q.r)) return true;  // the cycle runs: wait
            if (q.r.action == KILN_STATION_SELECT && q.r.recipe == 0xbad) {
                res->status = KILN_STATION_WRONG_WORKSHOP;
                std::snprintf(res->message, sizeof res->message, "recipe 0x00000bad is a recipe of another workshop");
                return false;
            }
            if (q.r.action == KILN_STATION_RESUME) {  // the station is not stopped
                res->status = KILN_STATION_UNCHANGED;
                std::snprintf(res->message, sizeof res->message, "the station is not stopped");
            }
            return false;
        };
        std::vector<std::pair<uint64_t, KilnStationResult>> out;
        auto done = [&](const Req& q, const KilnStationResult& r) { out.emplace_back(q.ticket, r); };
        auto mk = [](uint64_t ticket, uint32_t action, uint32_t flags = 0, uint32_t recipe = 0) {
            Req q;
            q.ticket = ticket;
            q.r = req(action);
            q.r.station = 10;
            q.r.flags = flags;
            q.r.recipe = recipe;
            q.r.crafts = 0;
            return q;
        };
        auto status_of = [&](uint64_t ticket) {
            for (auto& [t, r] : out)
                if (t == ticket) return r.status;
            return -1;
        };
        // a deferred stop and a resume in the same step: the resume withdraws it
        auto w = run_batch({mk(1, KILN_STATION_STOP, KILN_STATION_AFTER_CRAFT), mk(2, KILN_STATION_RESUME)}, {}, act, done);
        CHECK(w.empty() && status_of(1) == KILN_STATION_CANCELED);
        // two deferred stops in one step: the second finds the first
        out.clear();
        w = run_batch({mk(3, KILN_STATION_STOP, KILN_STATION_AFTER_CRAFT), mk(4, KILN_STATION_STOP, KILN_STATION_AFTER_CRAFT)}, {}, act, done);
        CHECK(w.size() == 1 && w[0].ticket == 3 && status_of(4) == KILN_STATION_UNCHANGED);
        // a select that fails its checks leaves the waiting stop alone
        out.clear();
        std::vector<Req> waiting{mk(5, KILN_STATION_STOP, KILN_STATION_AFTER_CRAFT)};
        w = run_batch({mk(6, KILN_STATION_SELECT, 0, 0xbad)}, waiting, act, done);
        CHECK(w.size() == 1 && w[0].ticket == 5 && status_of(6) == KILN_STATION_WRONG_WORKSHOP && status_of(5) == -1);
        // a resume answering UNCHANGED still withdraws it, and its message says so
        out.clear();
        w = run_batch({mk(7, KILN_STATION_RESUME)}, waiting, act, done);
        CHECK(w.empty() && status_of(5) == KILN_STATION_CANCELED && status_of(7) == KILN_STATION_UNCHANGED);
        bool noted = false;
        for (auto& [t, r] : out)
            if (t == 7) noted = std::strstr(r.message, "not stopped") && std::strstr(r.message, "withdrew");
        CHECK(noted);
        // another station's waiting stop is not touched
        out.clear();
        std::vector<Req> other{mk(8, KILN_STATION_STOP, KILN_STATION_AFTER_CRAFT)};
        other[0].r.station = 11;
        w = run_batch({mk(9, KILN_STATION_RESUME)}, other, act, done);
        CHECK(w.size() == 1 && w[0].ticket == 8);
    }
    // ---- the watch scans at once when the bases were rebuilt ----
    CHECK(scan_due(false, 5, 0, 32, false));
    CHECK(!scan_due(true, 40, 20, 32, false));
    CHECK(scan_due(true, 40, 20, 32, true));
    CHECK(scan_due(true, 60, 20, 32, false));
    // ---- a pause and a resume early in a cycle is the same cycle ----
    {
        Req q;
        q.selected0 = 0x5568cc2b;
        q.start0 = 1'000'000'000;
        StopSeen s{};
        s.present = true;
        s.running = s.selected = 0x5568cc2b;
        s.state = KILN_STATION_STATE_PAUSED;
        s.start = q.start0;
        s.now = 2'000'000'000;  // paused one second in
        CHECK(waiting_stop_step(&q, s, 10, 1000) == StopWait::Wait);
        s.state = KILN_STATION_STATE_RUNNING;
        s.now = 9'000'000'000;
        s.start = s.now - 1'000'000'000;  // resumed: start = now - the second it had run
        CHECK(waiting_stop_step(&q, s, 11, 1000) == StopWait::Wait);
        CHECK(waiting_stop_step(&q, s, 12, 1000) == StopWait::Wait);  // and the steps after
        StopSeen next = s;
        next.state = KILN_STATION_STATE_NO_RECIPE;  // it ended
        CHECK(waiting_stop_step(&q, next, 13, 1000) == StopWait::Write);
    }

    // ---- a base's id ----
    KilnBaseId ids[3]{};
    ids[0].guid[0] = 0x80;
    ids[1].guid[0] = 0x10;
    ids[1].guid[15] = 7;
    // ids[2] all zero: skipped
    KilnBaseId m = smallest_id(ids, 3);
    CHECK(m.guid[0] == 0x10 && m.guid[15] == 7);
    KilnBaseId none = smallest_id(ids + 2, 1);
    CHECK(none.guid[0] == 0 && none.guid[15] == 0);

    // ---- the state diff and the ring ----
    Snap a{}, b{};
    a.state = KILN_STATION_STATE_RUNNING;
    a.running = a.selected = 0x5568cc2b;
    b = a;
    CHECK(a == b);
    b.stopped = 1;
    b.state = KILN_STATION_STATE_STOPPED;
    CHECK(!(a == b));
    b.pos.x = 3800.0;
    KilnStationEvent ev = state_event(1318, a, b);
    CHECK(ev.kind == KILN_STATION_EV_STATE && ev.stopped_before == 0 && ev.stopped_after == 1 && ev.state_after == 3 &&
          ev.position.x == 3800.0);
    EventRing ring;
    KilnStationEvent out[4];
    bool more = false;
    CHECK(ring.read(0, out, 4, &more) == 0 && !more);
    for (int i = 0; i < 6; ++i) ring.push(ev);
    CHECK(ring.last() == 6);
    CHECK(ring.read(0, out, 4, &more) == 4 && more && out[0].seq == 1 && out[3].seq == 4);
    CHECK(ring.read(4, out, 4, &more) == 2 && !more && out[1].seq == 6);
    CHECK(ring.read(6, out, 4, &more) == 0);
    for (int i = 0; i < 2000; ++i) ring.push(ev);  // the ring keeps the last 1024
    CHECK(ring.last() == 2006);
    CHECK(ring.read(0, out, 1, &more) == 1 && out[0].seq == 2006 - 1024 + 1 && more);
    ring.clear();
    CHECK(ring.read(0, out, 4, &more) == 0 && ring.last() == 2006);
    ring.push(ev);
    CHECK(ring.read(2006, out, 4, &more) == 1 && out[0].seq == 2007);

    // ---- SELECT's unlock verdict ----
    CHECK(unlock_verdict(true, -1, {}) == Unlock::Yes);           // no requirement
    CHECK(unlock_verdict(false, 1, {}) == Unlock::Yes);           // the world has it (shared progression)
    CHECK(unlock_verdict(false, 0, {0, 1}) == Unlock::Yes);       // one player in game has it
    CHECK(unlock_verdict(false, 0, {0, 0}) == Unlock::No);
    CHECK(unlock_verdict(false, -1, {0}) == Unlock::No);
    CHECK(unlock_verdict(false, -1, {}) == Unlock::Unknown);      // a player query and no player in game
    CHECK(unlock_verdict(false, -1, {-1, -1}) == Unlock::Unknown);

    // ---- status: the events are on before a world runs (a mod subscribes in OnLoad) ----
    {
        const StatusBits menu = status_bits(true, true, true, true, false, false);  // no world yet, no program yet
        CHECK(!menu.read && !menu.fill && !menu.control);
        CHECK(menu.events);
        const StatusBits world = status_bits(true, true, true, true, true, true);
        CHECK(world.read && world.fill && world.control && world.events);
        const StatusBits broken = status_bits(true, true, true, true, true, false);  // the program broke
        CHECK(broken.read && !broken.fill && !broken.control && broken.events);
        CHECK(!status_bits(false, true, true, true, true, true).events);              // no reads, no watch
        CHECK(!status_bits(true, true, true, false, true, true).events);
    }

    std::printf("stations: %d checks, %d failures\n", checks, failed);
    return failed ? 1 : 0;
}
