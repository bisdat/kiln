// SPDX-License-Identifier: MIT
// Host tests of the walk-over pickup producer's rules (pickup_core.h) on the real broker (queue.h): tests/run.sh.
// Sampled exposures and their keys (§4.3: PICKUP-KEYS, PICKUP-EXPOSURE at the producer), the request a visit files,
// refusal while pending and after a cancel, the permit of an allow for exactly one key, fail-open paths, the window
// samples that close an exposure, and ReevaluatePickup.
#include <cstdio>
#include <map>
#include <set>

#include "../pickup_core.h"
#include "fake_worlds.h"

using namespace kiln::decisions;

static int g_pass = 0, g_fail = 0;
#define CHECK(x) ((x) ? ++g_pass : (++g_fail, std::printf("FAIL line %d: %s\n", __LINE__, #x)))

struct World {
    struct Drop {
        uint32_t receiver = 0, item = 0, amount = 0;
    };
    std::map<uint32_t, Drop> drops;
    std::map<uint32_t, Row> rows;
    std::set<uint32_t> listened;  // empty: every entity
    std::set<uint32_t> stale;     // keys whose life changed
};

static Row row_of(uint32_t e, uint64_t serial) {
    Row r;
    r.key.entity = e;
    r.key.flags = KILN_EVENT_KEY_SPAWN_TIME;
    r.key.spawn_time = 900;  // the same spawn time for every drop: never a key on its own
    r.key.life_serial = serial;
    r.has_position = 1;
    r.position[0] = e, r.position[1] = 2, r.position[2] = 3;
    return r;
}

struct Visitor {
    World& w;
    uint32_t drop = 0;
    uint64_t ep = 1, st = 10;
    bool on = true, hold_ok = true;
    // The world the visit runs in (world_gate.h); without a gate every visit is the server world's (the older tests).
    const WorldGate* gate = nullptr;
    const void* ctx = nullptr;
    uintptr_t server = 0;
    mutable int receiver_reads = 0;
    bool armed() const { return on; }
    bool server_world() const { return !gate || gate->owns(ctx, server); }
    uint32_t receiver() const { return ++receiver_reads, w.drops.count(drop) ? w.drops.at(drop).receiver : 0; }
    bool any_interest() const { return true; }
    uint32_t entity() const { return drop; }
    bool interested(uint32_t e) const { return w.listened.empty() || w.listened.count(e); }
    bool item(uint32_t* item, uint32_t* amount) const {
        if (!w.drops.count(drop)) return false;
        *item = w.drops.at(drop).item, *amount = w.drops.at(drop).amount;
        return true;
    }
    uint32_t feedback() const { return 1; }
    uint64_t epoch() const { return ep; }
    uint64_t step() const { return st; }
    bool holding() const { return hold_ok; }
    bool row(uint32_t e, uint64_t, Row* out) const {
        auto it = w.rows.find(e);
        if (it == w.rows.end()) return false;
        *out = it->second;
        return true;
    }
};

struct DropSampler {
    World& w;
    bool read(uint32_t drop, uint32_t* r, uint32_t* item, uint32_t* amount) const {
        auto it = w.drops.find(drop);
        if (it == w.drops.end()) return false;
        *r = it->second.receiver, *item = it->second.item, *amount = it->second.amount;
        return true;
    }
    bool same_life(const KilnEventEntityKey& k) const { return !w.stale.count(k.entity); }
    bool interested(uint32_t e) const { return w.listened.empty() || w.listened.count(e); }
};

static std::vector<KilnDecisionRequestV1> drain(Broker& b, uint64_t step) {
    uint64_t batch = 0;
    uint32_t n = 0, req = 0;
    b.begin_requests(&batch, &n);
    std::vector<KilnDecisionRequestV1> v(n);
    if (n) b.read_requests(batch, v.data(), n, sizeof(KilnDecisionRequestV1), &req);
    b.ack_requests(batch, step);
    return v;
}
static std::map<uint64_t, KilnDecisionOutcomeV1> outcomes(Broker& b, uint64_t step) {
    std::map<uint64_t, KilnDecisionOutcomeV1> m;
    uint64_t batch = 0;
    uint32_t n = 0, req = 0;
    b.begin_outcomes(&batch, &n);
    std::vector<KilnDecisionOutcomeV1> v(n);
    if (n && b.read_outcomes(batch, v.data(), n, sizeof(KilnDecisionOutcomeV1), &req) == KILN_OK)
        for (auto& o : v) m[o.request_id] = o;
    b.ack_outcomes(batch, step);
    return m;
}
static void answer(Broker& b, const KilnDecisionRequestV1& r, bool allow, uint64_t step) {
    KilnDecisionAnswerV1 a{};
    a.size = sizeof a, a.kind = r.kind, a.request_id = r.request_id, a.world_epoch = r.world_epoch, a.allow = allow;
    b.answer(a, nullptr, step);
}

static void test_request_and_states() {
    World w;
    Broker b;
    pickup::Exposures x;
    b.attach(9);
    w.drops[3000] = {2022, 0x2b0cf1d5, 3};
    w.rows[3000] = row_of(3000, 50);
    w.rows[2022] = row_of(2022, 51);
    Visitor v{w, 3000};

    // first visit files a request and refuses (the helper's own "nothing added")
    CHECK(x.visit(v, b) == pickup::Visit::Refuse);
    auto reqs = drain(b, 11);
    CHECK(reqs.size() == 1);
    const auto r = reqs.at(0);
    CHECK(r.kind == KILN_DECISION_WALKOVER && r.payload_bytes == sizeof(KilnPickupRequestV1));
    CHECK(r.subject.entity == 3000 && r.subject.life_serial == 50 && r.actor.entity == 2022 && r.actor.life_serial == 51);
    CHECK(r.payload.pickup.item_id == 0x2b0cf1d5 && r.payload.pickup.raw_count == 3 && r.payload.pickup.entry == 1 && r.payload.pickup.feedback == 1);
    CHECK((r.flags & KILN_DECISION_FLAG_HAS_POSITION) && r.payload.pickup.position[0] == 3000);
    CHECK(r.exposure_epoch != 0 && r.deadline_step == 12);
    // while pending: refused, nothing filed again
    CHECK(x.visit(v, b) == pickup::Visit::Refuse && b.counters().filed == 1);
    // canceled: suppressed for this exposure
    answer(b, r, false, 11);
    CHECK(x.visit(v, b) == pickup::Visit::Refuse && x.visit(v, b) == pickup::Visit::Refuse && b.counters().filed == 1);
    pickup::Exposure e;
    CHECK(x.get(3000, &e) && e.state == pickup::State::Suppressed);

    // ReevaluatePickup reopens it with a new epoch: the next visit asks again
    const uint64_t old_epoch = e.exposure_epoch;
    CHECK(x.reopen(3000, 2022) == 1);
    CHECK(x.reopen(3000, 2022) == 0);  // open now: nothing more to reset
    CHECK(x.visit(v, b) == pickup::Visit::Refuse && b.counters().filed == 2);
    auto r2 = drain(b, 12).at(0);
    CHECK(r2.exposure_epoch > old_epoch);
    // allowed: a permit for this key, the helper runs (and keeps running for this exposure)
    answer(b, r2, true, 12);
    CHECK(x.visit(v, b) == pickup::Visit::Pass && x.stats.permitted.load() == 1);
    CHECK(x.visit(v, b) == pickup::Visit::Pass && b.counters().filed == 2);
    // a partial grant changes the amount: a new exposure and a new decision (the permit does not carry over)
    w.drops[3000].amount = 1;
    CHECK(x.visit(v, b) == pickup::Visit::Refuse && b.counters().filed == 3);
    auto r3 = drain(b, 13).at(0);
    CHECK(r3.payload.pickup.raw_count == 1 && r3.exposure_epoch > r2.exposure_epoch);
}

static void test_keys_never_cross() {
    // PICKUP-KEYS at the producer: two drops of the same item and the same spawn time, one receiver; permitting one
    // never permits the other
    World w;
    Broker b;
    pickup::Exposures x;
    b.attach(9);
    w.drops[3000] = {2022, 0x2b0cf1d5, 3};
    w.drops[3001] = {2022, 0x2b0cf1d5, 3};
    w.rows[3000] = row_of(3000, 60);
    w.rows[3001] = row_of(3001, 61);
    w.rows[2022] = row_of(2022, 51);
    Visitor a{w, 3000}, c{w, 3001};
    CHECK(x.visit(a, b) == pickup::Visit::Refuse && x.visit(c, b) == pickup::Visit::Refuse);
    auto reqs = drain(b, 11);
    CHECK(reqs.size() == 2 && reqs[0].subject.life_serial != reqs[1].subject.life_serial);
    CHECK(reqs.size() == 2 && reqs[0].subject.spawn_time == reqs[1].subject.spawn_time && reqs[0].exposure_epoch != reqs[1].exposure_epoch);
    answer(b, reqs.at(0), true, 11);
    CHECK(x.visit(a, b) == pickup::Visit::Pass);
    CHECK(x.visit(c, b) == pickup::Visit::Refuse);  // the other drop is still held
    answer(b, reqs.at(1), false, 11);
    CHECK(x.visit(c, b) == pickup::Visit::Refuse && x.visit(a, b) == pickup::Visit::Pass);
    // a second receiver on the permitted drop is another key: a new decision
    w.rows[2023] = row_of(2023, 52);
    w.drops[3000].receiver = 2023;
    CHECK(x.visit(a, b) == pickup::Visit::Refuse && b.counters().filed == 3);
}

static void test_samples_close_exposures() {
    // PICKUP-EXPOSURE at the producer: receiver 0 at a sample closes the exposure; the next contact is a new epoch, and
    // the old cancel does not suppress it
    World w;
    Broker b;
    pickup::Exposures x;
    DropSampler s{w};
    b.attach(9);
    w.drops[3000] = {2022, 0x2b0cf1d5, 3};
    w.rows[3000] = row_of(3000, 50);
    w.rows[2022] = row_of(2022, 51);
    Visitor v{w, 3000};
    CHECK(x.visit(v, b) == pickup::Visit::Refuse);
    auto r = drain(b, 11).at(0);
    answer(b, r, false, 11);
    CHECK(x.visit(v, b) == pickup::Visit::Refuse);  // suppressed
    x.sample(1, 11, b, s, false);
    CHECK(x.size() == 1);  // nothing changed: the suppression stays
    w.drops[3000].receiver = 0;
    x.sample(1, 12, b, s, false);
    CHECK(x.size() == 0);
    w.drops[3000].receiver = 2022;
    CHECK(x.visit(v, b) == pickup::Visit::Refuse && b.counters().filed == 2);
    auto r2 = drain(b, 13).at(0);
    CHECK(r2.exposure_epoch > r.exposure_epoch);
    // a pending hold whose drop goes is completed (StaleEntity), not left to time out
    w.drops.erase(3000);
    x.sample(1, 13, b, s, true);
    auto out = outcomes(b, 13);
    CHECK(out.count(r2.request_id) && out[r2.request_id].status == KILN_DECISION_STALE_ENTITY && x.size() == 0);
    // another life of the receiver at window_begin closes it too
    w.drops[3000] = {2022, 0x2b0cf1d5, 3};
    CHECK(x.visit(v, b) == pickup::Visit::Refuse);
    auto r3 = drain(b, 14).at(0);
    w.stale.insert(2022);
    x.sample(1, 14, b, s, false);  // window_end: lives are not asked
    CHECK(x.size() == 1);
    x.sample(1, 15, b, s, true);
    out = outcomes(b, 15);
    CHECK(x.size() == 0 && out.count(r3.request_id) && out[r3.request_id].status == KILN_DECISION_STALE_ENTITY);
    w.stale.clear();
    // nobody listens any more: a pending hold is released, a suppression forgotten
    CHECK(x.visit(v, b) == pickup::Visit::Refuse);
    auto r4 = drain(b, 16).at(0);
    w.listened = {9999};
    x.sample(1, 16, b, s, true);
    out = outcomes(b, 16);
    CHECK(x.size() == 0 && out.count(r4.request_id) && out[r4.request_id].status == KILN_DECISION_CAPABILITY_RELEASED);
    // a filter that names neither the receiver nor the drop: no hold at all
    CHECK(x.visit(v, b) == pickup::Visit::Pass && x.size() == 0);
    w.listened = {3000};  // the drop named: held
    CHECK(x.visit(v, b) == pickup::Visit::Refuse);
}

static void test_fail_open() {
    World w;
    Broker b;
    pickup::Exposures x;
    b.attach(9);
    w.drops[3000] = {2022, 0x2b0cf1d5, 3};
    w.rows[3000] = row_of(3000, 50);
    Visitor v{w, 3000};
    // no row for the receiver yet: pass, counted
    CHECK(x.visit(v, b) == pickup::Visit::Pass && x.stats.misses.load() == 1 && x.size() == 0);
    w.rows[2022] = row_of(2022, 51);
    // over budget / unread outcomes
    v.hold_ok = false;
    CHECK(x.visit(v, b) == pickup::Visit::Pass && x.size() == 0);
    v.hold_ok = true;
    // disarmed
    v.on = false;
    CHECK(x.visit(v, b) == pickup::Visit::Pass);
    v.on = true;
    // receiver 0: the helper is not for anyone
    w.drops[3000].receiver = 0;
    CHECK(x.visit(v, b) == pickup::Visit::Pass);
    w.drops[3000].receiver = 2022;
    // no consumer: this exposure fails open (released), the helper runs
    Broker lonely;
    pickup::Exposures y;
    CHECK(y.visit(v, lonely) == pickup::Visit::Pass && y.visit(v, lonely) == pickup::Visit::Pass);
    // a timeout releases it the same way
    CHECK(x.visit(v, b) == pickup::Visit::Refuse);
    b.window_begin(1, 13);  // capture 10 + 3
    CHECK(x.visit(v, b) == pickup::Visit::Pass && x.stats.released.load() == 1);
}

static void test_reset_after_answer_before_next_visit() {
    World w;
    Broker b;
    pickup::Exposures x;
    b.attach(9);
    w.drops[3000] = {2022, 0x2b0cf1d5, 3};
    w.rows[3000] = row_of(3000, 50);
    w.rows[2022] = row_of(2022, 51);
    Visitor v{w, 3000};
    CHECK(x.visit(v, b) == pickup::Visit::Refuse);
    auto r = drain(b, 11).at(0);
    CHECK(x.reopen(3000, 2022, b) == 0); // genuinely pending: cannot reset
    answer(b, r, false, 11);
    // A completion handler resets immediately in Pre, before the range helper
    // visits again. The completed veto must already count as suppression.
    CHECK(x.reopen(3000, 2022, b) == 1);
    CHECK(x.visit(v, b) == pickup::Visit::Refuse && b.counters().filed == 2);
    auto again = drain(b, 11);
    CHECK(again.size() == 1 && again[0].exposure_epoch > r.exposure_epoch);
}

// A hosting game client: drop 3000 and receiver 2022 exist with the same ids in the server, client and prediction
// worlds; a joined client has no server world. Only the server world's own visits reach the exposures and the broker.
static void test_other_worlds_pass() {
    FakeWorlds fw;
    World w;
    Broker b;
    pickup::Exposures x;
    b.attach(9);
    w.drops[3000] = {2022, 0x2b0cf1d5, 3};
    w.rows[3000] = row_of(3000, 50);
    w.rows[2022] = row_of(2022, 51);
    Visitor v{w, 3000};
    v.gate = &fw.gate;
    v.server = fw.server_world();
    v.ctx = fw.ctx_server;
    CHECK(x.visit(v, b) == pickup::Visit::Refuse && b.counters().filed == 1);  // pending in the server world
    const int reads = v.receiver_reads;
    for (const void* other : {static_cast<const void*>(fw.ctx_client), static_cast<const void*>(fw.ctx_prediction)}) {
        v.ctx = other;
        CHECK(x.visit(v, b) == pickup::Visit::Pass);  // the client's own pickup runs as without Kiln
    }
    CHECK(v.receiver_reads == reads && b.counters().filed == 1 && x.stats.refused.load() == 1 && x.stats.foreign.load() == 2);
    // a joined client: no server world
    v.server = 0;
    v.ctx = fw.ctx_client;
    CHECK(x.visit(v, b) == pickup::Visit::Pass && b.counters().filed == 1);
    // the server world's exposure is still pending, refused there
    v.server = fw.server_world();
    v.ctx = fw.ctx_server;
    CHECK(x.visit(v, b) == pickup::Visit::Refuse && b.counters().filed == 1);
}

int main() {
    test_reset_after_answer_before_next_visit();
    test_request_and_states();
    test_keys_never_cross();
    test_samples_close_exposures();
    test_fail_open();
    test_other_worlds_pass();
    std::printf("pickup_test: %d PASS, %d FAIL\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
