// SPDX-License-Identifier: MIT
// kiln.stations host test: request checks, the fill's arithmetic against the game's own (AutoTransferRecipe,
// docs/re/stations.md §3.1), the base id rule, the stop-after-craft rule, the state diff, the event ring, the unlock
// verdict, and the ABI's sizes (the managed mirror in Kiln.Stations depends on them).
#include "../stations_logic.h"

#include <cstddef>
#include <cstdio>

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
// fills `moved` per input, or 4 when the stock is short. Used to check the planner never asks for what the game refuses.
int game_fill(const std::vector<InputNow>& in, uint32_t n, std::vector<uint32_t>* moved) {
    const uint32_t loaded = crafts_loaded(in);
    moved->assign(in.size(), 0);
    for (size_t k = 0; k < in.size(); ++k) {
        const uint32_t need = game_need(in[k], n, loaded);
        if (need > in[k].stock) return 4;
        (*moved)[k] = need;
    }
    return 0;
}
}  // namespace

int main() {
    // ---- ABI sizes (the managed mirror) ----
    static_assert(sizeof(KilnBaseId) == 16 && sizeof(KilnStationChoice) == 8 && sizeof(KilnStationStack) == 8);
    static_assert(sizeof(KilnStationBase) == 144 && offsetof(KilnStationBase, min) == 80);
    static_assert(sizeof(KilnStation) == 200 && offsetof(KilnStation, position) == 32 && offsetof(KilnStation, state) == 56 &&
                  offsetof(KilnStation, crafts_loaded) == 132 && offsetof(KilnStation, recipe_start_ns) == 136 &&
                  offsetof(KilnStation, progress) == 168);
    static_assert(sizeof(KilnStationInput) == 16 && sizeof(KilnStationRecipe) == 624 && offsetof(KilnStationRecipe, inputs) == 48);
    static_assert(sizeof(KilnStationRequest) == 88 && offsetof(KilnStationRequest, choices) == 24 &&
                  offsetof(KilnStationRequest, keep) == 64);
    static_assert(sizeof(KilnItemsMoveChange) == 32);
    static_assert(sizeof(KilnStationResult) == 1224 && offsetof(KilnStationResult, ticket) == 56 &&
                  offsetof(KilnStationResult, changes) == 72 && offsetof(KilnStationResult, message) == 1096);
    static_assert(sizeof(KilnStationPlan) == 680 && offsetof(KilnStationPlan, inputs) == 40);
    static_assert(sizeof(KilnStationEvent) == 96 && offsetof(KilnStationEvent, by) == 64 && offsetof(KilnStationEvent, position) == 72);
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
    CHECK(crafts_first_try(KILN_STATION_FILL_EXACT, 20, 7) == 20);
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
    CHECK(after_craft_due(KILN_STATION_STATE_OUTPUT_FULL, 0x5568cc2b, 5, 5)); // ended, outputs waiting
    CHECK(after_craft_due(KILN_STATION_STATE_MISSING_MATERIALS, 0x5568cc2b, 5, 5));

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

    std::printf("stations: %d checks, %d failures\n", checks, failed);
    return failed ? 1 : 0;
}
