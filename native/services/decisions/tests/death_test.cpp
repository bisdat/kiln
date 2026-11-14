// SPDX-License-Identifier: MIT
// Host tests of the death producer's rules (death_core.h) on the real broker (queue.h): tests/run.sh.
// The visit order (the guard before the zero-interest exit, corpses, holds, filters, fail-open paths), the request a
// lethal visit files, the housekeeping, and every outcome of an answer (§4.2 "Death": AlreadyAlive, Allowed,
// SourceChanged, NoPositiveRange, WriteRefused, StaleEntity, Prevented with its clamp).
#include <cstdio>
#include <cstring>
#include <map>
#include <set>

#include "../death_core.h"

using namespace kiln::decisions;

static int g_pass = 0, g_fail = 0;
#define CHECK(x) ((x) ? ++g_pass : (++g_fail, std::printf("FAIL line %d: %s\n", __LINE__, #x)))

// The game around one visit, scripted.
struct Ports {
    int32_t hp = 0;
    bool on = true;
    uint32_t e = 2000;
    uint64_t ep = 1, st = 10;
    uint32_t guard_answer = KILN_DEATH_GUARD_PASS;
    int guard_calls = 0, entity_calls = 0;
    uint32_t interest = 2;  // 0 none, 1 list, 2 all
    std::set<uint32_t> ids;
    bool blocked = false, hold_ok = true;
    std::map<uint32_t, Row> rows;
    std::vector<std::pair<uint32_t, uint64_t>> filed_log;

    int32_t health() const { return hp; }
    bool armed() const { return on; }
    uint32_t entity() { return ++entity_calls, e; }
    uint64_t epoch() const { return ep; }
    uint64_t step() const { return st; }
    uint32_t guard(uint64_t, uint32_t, int32_t) { return ++guard_calls, guard_answer; }
    bool any_interest() const { return interest != 0; }
    bool interested(uint32_t x) const { return interest == 2 || (interest == 1 && ids.count(x)); }
    bool actor_blocked() const { return blocked; }
    bool holding() const { return hold_ok; }
    bool row(uint32_t x, uint64_t, Row* out) const {
        auto it = rows.find(x);
        if (it == rows.end()) return false;
        *out = it->second;
        return true;
    }
    void filed(uint32_t x, uint64_t id) { filed_log.push_back({x, id}); }
};

static Row row_of(uint32_t e, uint64_t serial, bool pos = true) {
    Row r;
    r.key.entity = e;
    r.key.flags = KILN_EVENT_KEY_SPAWN_TIME;
    r.key.spawn_time = 5000 + e;
    r.key.life_serial = serial;
    r.has_position = pos;
    r.position[0] = 1, r.position[1] = 2, r.position[2] = 3;
    return r;
}

static std::vector<KilnDecisionRequestV1> drain_requests(Broker& b, uint64_t step) {
    uint64_t batch = 0;
    uint32_t n = 0, req = 0;
    b.begin_requests(&batch, &n);
    std::vector<KilnDecisionRequestV1> v(n);
    if (n) b.read_requests(batch, v.data(), n, sizeof(KilnDecisionRequestV1), &req);
    b.ack_requests(batch, step);
    return v;
}
static std::map<uint64_t, KilnDecisionOutcomeV1> drain_outcomes(Broker& b, uint64_t step) {
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

// The answer's side of the game, scripted.
struct Live {
    bool same = true, has_health = true, actor_dead = false;
    int32_t current = 0, max = 100;
    KilnResult set_rc = KILN_OK;
    int32_t clamp_to = -1;  // >= 0: what the setter leaves (god mode keeps 1; a refusal that writes nothing leaves 0)
    std::vector<int32_t> writes;

    bool same_life(const KilnEventEntityKey&) const { return same; }
    bool live(uint32_t, death::Live* out) const {
        if (!has_health) return false;
        out->current = current, out->max = max, out->actor_dead = actor_dead;
        return true;
    }
    KilnResult set_health(uint32_t, int32_t v, int32_t* applied) {
        writes.push_back(v);
        if (set_rc != KILN_OK) return set_rc;
        current = clamp_to >= 0 ? clamp_to : v;
        *applied = current;
        return KILN_OK;
    }
};
static uint32_t finish_with(Finish& f, void* user) { return death::finish(f, *static_cast<Live*>(user)); }

static KilnDecisionAnswerV1 answer_of(const KilnDecisionRequestV1& r, bool allow, int32_t hp = 1) {
    KilnDecisionAnswerV1 a{};
    a.size = sizeof a;
    a.kind = r.kind;
    a.request_id = r.request_id;
    a.world_epoch = r.world_epoch;
    a.allow = allow;
    a.survival_health = hp;
    return a;
}

static void test_visit_order() {
    Broker b;
    death::Holds h;
    Ports p;
    b.attach(9);
    p.rows[2000] = row_of(2000, 77);

    // a living entity: one compare, nothing asked
    p.hp = 5;
    CHECK(h.visit(p, b) == death::Visit::Pass && p.guard_calls == 0 && p.entity_calls == 0);

    // the native guard answers before the zero-interest exit (ruling for 07: 19d-pets' guard with no listener)
    p.hp = 0;
    p.interest = 0;
    p.guard_answer = KILN_DEATH_GUARD_SKIP;
    CHECK(h.visit(p, b) == death::Visit::Skip && p.guard_calls == 1 && h.stats.guarded.load() == 1);
    CHECK(b.counters().filed == 0 && h.size() == 0);  // a guarded skip files nothing
    p.guard_answer = KILN_DEATH_GUARD_PASS;
    CHECK(h.visit(p, b) == death::Visit::Pass && p.guard_calls == 2 && b.counters().filed == 0);  // zero interest: pass

    // a corpse (or state 12) is no new transition
    p.interest = 2;
    p.blocked = true;
    CHECK(h.visit(p, b) == death::Visit::Pass && b.counters().filed == 0);
    p.blocked = false;

    // a filter that does not name the entity: no hold, no request (DEATH-FILTER at the hook)
    p.interest = 1;
    p.ids = {3000};
    CHECK(h.visit(p, b) == death::Visit::Pass && b.counters().filed == 0);

    // fail open: metadata over budget / outcomes unread
    p.interest = 2;
    p.hold_ok = false;
    CHECK(h.visit(p, b) == death::Visit::Pass && b.counters().filed == 0);
    p.hold_ok = true;

    // no row yet (an entity first seen this step): pass, counted
    p.e = 2001;
    CHECK(h.visit(p, b) == death::Visit::Pass && h.stats.misses.load() == 1 && b.counters().metadata_misses == 1);
    p.e = 2000;

    // disarmed (the hook's flag): pass before anything
    p.on = false;
    const int calls = p.guard_calls;
    CHECK(h.visit(p, b) == death::Visit::Pass && p.guard_calls == calls);
    p.on = true;

    // no consumer: fail open
    Broker lonely;
    death::Holds h2;
    CHECK(h2.visit(p, lonely) == death::Visit::Pass && h2.size() == 0);
}

static void test_file_hold_and_release() {
    Broker b;
    death::Holds h;
    Ports p;
    b.attach(9);
    p.rows[2000] = row_of(2000, 77);
    p.hp = -12;
    CHECK(h.visit(p, b) == death::Visit::Skip && p.filed_log.size() == 1);
    auto reqs = drain_requests(b, 11);
    CHECK(reqs.size() == 1);
    const KilnDecisionRequestV1 r = reqs.empty() ? KilnDecisionRequestV1{} : reqs[0];
    CHECK(r.kind == KILN_DECISION_DEATH && r.payload_bytes == sizeof(KilnDeathRequestV1));
    CHECK(r.subject.entity == 2000 && r.subject.life_serial == 77 && r.subject.spawn_time == 7000);
    CHECK(r.actor.entity == 0 && (r.flags & KILN_DECISION_FLAG_HAS_POSITION));
    CHECK(r.payload.death.health == -12 && r.payload.death.suggested_survival_health == 1);
    CHECK(r.capture_step == 10 && r.deadline_step == 12 && r.world_epoch == 1);
    // the same entity visited again while held: skipped, nothing filed again
    p.st = 11;
    CHECK(h.visit(p, b) == death::Visit::Skip && b.counters().filed == 1 && h.stats.held.load() == 1);
    // allowed: the next visit passes once (the game kills it), the hold is forgotten
    CHECK(b.answer(answer_of(r, true), nullptr, 11) == KILN_OK);
    CHECK(h.visit(p, b) == death::Visit::Pass && h.size() == 0 && h.stats.released.load() == 1);
    p.blocked = true;  // the death program marked it dead
    CHECK(h.visit(p, b) == death::Visit::Pass && b.counters().filed == 1);
}

static void test_prevented_then_new_death() {
    Broker b;
    death::Holds h;
    Ports p;
    Live live;
    b.attach(9);
    b.set_finisher(KILN_DECISION_DEATH, &finish_with, &live);
    p.rows[2000] = row_of(2000, 77);
    p.hp = 0;
    CHECK(h.visit(p, b) == death::Visit::Skip);
    auto r = drain_requests(b, 11).at(0);
    live.current = 0;
    KilnDecisionReceiptV1 rc{};
    CHECK(b.answer(answer_of(r, false, 1), &rc, 11) == KILN_OK && rc.terminal_status == KILN_DECISION_PREVENTED);
    auto out = drain_outcomes(b, 11);
    CHECK(out.count(r.request_id) && out[r.request_id].status == KILN_DECISION_PREVENTED);
    CHECK((out[r.request_id].flags & KILN_DECISION_OUTCOME_HEALTH_KNOWN) && out[r.request_id].health_before == 0 &&
          out[r.request_id].applied_health == 1 && out[r.request_id].requested_health == 1);
    // the survival write made it positive: a living visit is one compare
    p.hp = 1;
    CHECK(h.visit(p, b) == death::Visit::Pass);
    // a new lethal hit later is a new decision, not the old hold
    p.hp = 0;
    p.st = 20;
    CHECK(h.visit(p, b) == death::Visit::Skip && b.counters().filed == 2);
    auto r2 = drain_requests(b, 21).at(0);
    CHECK(r2.request_id > r.request_id && r2.capture_step == 20);
}

static void test_timeout_and_housekeeping() {
    Broker b;
    death::Holds h;
    Ports p;
    b.attach(9);
    p.rows[2000] = row_of(2000, 77);
    p.rows[2002] = row_of(2002, 78);
    p.hp = 0;
    CHECK(h.visit(p, b) == death::Visit::Skip);
    const uint64_t first = p.filed_log.at(0).second;
    // nobody answers: at the window of capture + 3 the hold is released; the visit then passes
    b.window_begin(1, 12);
    CHECK(b.status_of(first) == 0);
    b.window_begin(1, 13);
    CHECK(b.status_of(first) == KILN_DECISION_TIMEOUT_RELEASED);
    p.st = 13;
    CHECK(h.visit(p, b) == death::Visit::Pass && h.size() == 0);

    // a pending hold whose entity is no longer listened for is released at the next window
    p.e = 2002;
    p.st = 30;
    CHECK(h.visit(p, b) == death::Visit::Skip);
    const uint64_t second = p.filed_log.back().second;
    h.housekeeping(1, 31, b, [](uint32_t) { return false; });
    CHECK(b.status_of(second) == KILN_DECISION_CAPABILITY_RELEASED);
    auto out = drain_outcomes(b, 31);
    CHECK(out.count(second) && out[second].status == KILN_DECISION_CAPABILITY_RELEASED);
    // a terminal hold the hook never comes back to (the entity went) is forgotten after three windows
    h.housekeeping(1, 32, b, [](uint32_t) { return true; });
    CHECK(h.size() == 1);
    h.housekeeping(1, 34, b, [](uint32_t) { return true; });
    CHECK(h.size() == 1);
    h.housekeeping(1, 35, b, [](uint32_t) { return true; });
    CHECK(h.size() == 0);

    // a hold of an older world goes at the next world's first window; its entity id may die again in the new world
    p.e = 2000;
    p.st = 40;
    CHECK(h.visit(p, b) == death::Visit::Skip && h.size() == 1);
    h.housekeeping(2, 41, b, [](uint32_t) { return true; });
    CHECK(h.size() == 0);
    p.ep = 2;
    CHECK(h.visit(p, b) == death::Visit::Skip && h.size() == 1);  // a fresh request of epoch 2
}

static void test_queue_full_fails_open() {
    Broker b;
    death::Holds h;
    Ports p;
    b.attach(9);
    for (uint32_t i = 0; i < KILN_DECISIONS_MAX_ACTIVE; ++i) {
        KilnDecisionRequestV1 r{};
        r.size = sizeof r, r.version = 1, r.kind = KILN_DECISION_DEATH, r.payload_bytes = sizeof(KilnDeathRequestV1);
        r.subject.entity = 10000 + i;
        uint64_t id = 0;
        b.file(r, 1, 10, &id);
    }
    p.rows[2000] = row_of(2000, 77);
    p.hp = 0;
    CHECK(h.visit(p, b) == death::Visit::Pass && h.size() == 0 && h.stats.dropped.load() == 1 && b.counters().queue_dropped == 1);
}

static void test_answers() {
    struct Case {
        const char* name;
        Live live;
        bool allow;
        int32_t survival;
        uint32_t want;
        int32_t applied;
        size_t writes;
    };
    Live base;
    auto with = [&](auto f) {
        Live l = base;
        f(l);
        return l;
    };
    const Case cases[] = {
        {"stale life", with([](Live& l) { l.same = false; }), false, 1, KILN_DECISION_STALE_ENTITY, 0, 0},
        {"no health", with([](Live& l) { l.has_health = false; }), false, 1, KILN_DECISION_STALE_ENTITY, 0, 0},
        {"healed by a handler (cancel)", with([](Live& l) { l.current = 100; }), false, 1, KILN_DECISION_ALREADY_ALIVE, 100, 0},
        {"healed by a handler (allow)", with([](Live& l) { l.current = 40; }), true, 0, KILN_DECISION_ALREADY_ALIVE, 40, 0},
        {"allowed", base, true, 0, KILN_DECISION_ALLOWED, 0, 0},
        {"actor forced dead while held", with([](Live& l) { l.actor_dead = true; }), false, 1, KILN_DECISION_SOURCE_CHANGED, 0, 0},
        {"no positive maximum", with([](Live& l) { l.max = 0; }), false, 1, KILN_DECISION_NO_POSITIVE_RANGE, 0, 0},
        {"setter refuses", with([](Live& l) { l.set_rc = KILN_ERR_NOT_FOUND; }), false, 1, KILN_DECISION_WRITE_REFUSED, 0, 1},
        {"setter leaves 0", with([](Live& l) { l.clamp_to = 0; }), false, 1, KILN_DECISION_WRITE_REFUSED, 0, 1},
        {"prevented at 1", base, false, 1, KILN_DECISION_PREVENTED, 1, 1},
        {"prevented, clamped to max", base, false, 500, KILN_DECISION_PREVENTED, 100, 1},
        {"prevented, 0 asked means 1", base, false, 0, KILN_DECISION_PREVENTED, 1, 1},
        {"god mode keeps 1", with([](Live& l) { l.clamp_to = 1; }), false, 50, KILN_DECISION_PREVENTED, 1, 1},
    };
    for (const Case& c : cases) {
        Broker b;
        Live live = c.live;
        b.attach(9);
        b.set_finisher(KILN_DECISION_DEATH, &finish_with, &live);
        KilnDecisionRequestV1 r{};
        r.size = sizeof r, r.version = 1, r.kind = KILN_DECISION_DEATH, r.payload_bytes = sizeof(KilnDeathRequestV1);
        r.subject.entity = 2000;
        r.subject.life_serial = 77;
        uint64_t id = 0;
        b.file(r, 1, 10, &id);
        r.request_id = id;
        r.world_epoch = 1;
        KilnDecisionReceiptV1 rc{};
        const KilnResult res = b.answer(answer_of(r, c.allow, c.survival), &rc, 11);
        auto out = drain_outcomes(b, 11);
        const KilnDecisionOutcomeV1 o = out.count(id) ? out[id] : KilnDecisionOutcomeV1{};
        const bool known = o.flags & KILN_DECISION_OUTCOME_HEALTH_KNOWN;
        const bool ok_status = rc.accepted == 1 && rc.terminal_status == c.want && o.status == c.want;
        const bool ok_writes = live.writes.size() == c.writes;
        const bool ok_health = c.want == KILN_DECISION_STALE_ENTITY ? !known : (known && o.applied_health == c.applied);
        const bool ok_reason = (c.want == KILN_DECISION_ALLOWED || c.want == KILN_DECISION_PREVENTED) || o.reason[0] != 0;
        if (!(res == KILN_OK && ok_status && ok_writes && ok_health && ok_reason)) {
            ++g_fail;
            std::printf("FAIL answer case '%s': status %u (want %u), writes %zu (want %zu), applied %d known %d (want %d), reason '%s'\n", c.name,
                        o.status, c.want, live.writes.size(), c.writes, o.applied_health, known, c.applied, o.reason);
        } else {
            ++g_pass;
        }
        // answered once: a repeat returns the terminal status and writes nothing
        KilnDecisionReceiptV1 again{};
        b.answer(answer_of(r, c.allow, c.survival), &again, 12);
        CHECK(again.accepted == 0 && again.terminal_status == c.want && live.writes.size() == c.writes);
    }
}

int main() {
    test_visit_order();
    test_file_hold_and_release();
    test_prevented_then_new_death();
    test_timeout_and_housekeeping();
    test_queue_full_fails_open();
    test_answers();
    std::printf("death_test: %d PASS, %d FAIL\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
