// SPDX-License-Identifier: MIT
// Host tests of the decision broker (queue.h) and the metadata cache (metadata.h): tests/run.sh.
#include <cstdio>
#include <map>
#include <memory>
#include <string>

#include "../metadata.h"
#include "../queue.h"

using namespace kiln::decisions;

static int g_pass = 0, g_fail = 0;
#define CHECK(x) ((x) ? ++g_pass : (++g_fail, std::printf("FAIL line %d: %s\n", __LINE__, #x)))

static KilnDecisionRequestV1 request(uint32_t kind, uint32_t entity) {
    KilnDecisionRequestV1 r{};
    r.size = sizeof r;
    r.version = 1;
    r.kind = kind;
    r.payload_bytes = payload_size(kind);
    r.subject.entity = entity;
    r.subject.flags = KILN_EVENT_KEY_SPAWN_TIME;
    r.subject.life_serial = entity;
    return r;
}
static KilnDecisionAnswerV1 answer_for(const KilnDecisionRequestV1& r, uint64_t id, uint64_t epoch, bool allow, int hp = 0) {
    KilnDecisionAnswerV1 a{};
    a.size = sizeof a;
    a.kind = r.kind;
    a.request_id = id;
    a.world_epoch = epoch;
    a.allow = allow;
    a.survival_health = hp;
    return a;
}
// Drains the outcome queue; returns status by request id.
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

static void test_validation_and_consumer() {
    Broker b;
    uint64_t id = 7;
    CHECK(b.file(request(KILN_DECISION_DEATH, 2000), 1, 10, &id) == KILN_ERR_UNAVAILABLE && id == 0);  // no consumer: fail open
    CHECK(b.attach(5) == KILN_OK && b.attach(5) == KILN_OK);
    CHECK(b.attach(6) == KILN_ERR_UNAVAILABLE);  // one consumer
    CHECK(b.is_consumer(5) && !b.is_consumer(6));
    auto bad = request(KILN_DECISION_DEATH, 2000);
    bad.size = 100;
    CHECK(b.file(bad, 1, 10, &id) == KILN_ERR_INVALID_ARGUMENT);
    bad = request(KILN_DECISION_DEATH, 2000);
    bad.version = 2;
    CHECK(b.file(bad, 1, 10, &id) == KILN_ERR_INVALID_ARGUMENT);
    bad = request(KILN_DECISION_DEATH, 2000);
    bad.payload_bytes += 8;  // kind and payload size must agree
    CHECK(b.file(bad, 1, 10, &id) == KILN_ERR_INVALID_ARGUMENT);
    bad = request(KILN_DECISION_DEATH, 0);
    CHECK(b.file(bad, 1, 10, &id) == KILN_ERR_INVALID_ARGUMENT);
    bad = request(KILN_DECISION_DEATH, 2000);
    bad.kind = KILN_DECISION_TERRAFORM_RESERVED;  // reserved kind
    bad.payload_bytes = 16;
    CHECK(b.file(bad, 1, 10, &id) == KILN_ERR_INVALID_ARGUMENT);
    bad.kind = 99;
    CHECK(b.file(bad, 1, 10, &id) == KILN_ERR_INVALID_ARGUMENT);
    CHECK(b.active_count() == 0);
    CHECK(b.detach(6, 10) == KILN_ERR_UNAVAILABLE && b.detach(5, 10) == KILN_OK && !b.attached());
}

// QUEUE-RETRY: freeze, probe, short read, read twice, ack, ack again, late arrivals.
static void test_batches() {
    Broker b;
    b.attach(1);
    uint64_t id1 = 0, id2 = 0, id3 = 0;
    CHECK(b.file(request(KILN_DECISION_DEATH, 2000), 1, 10, &id1) == KILN_OK);
    CHECK(b.file(request(KILN_DECISION_WALKOVER, 2001), 1, 10, &id2) == KILN_OK);
    CHECK(id1 != 0 && id2 > id1);
    uint64_t batch = 0, again = 0;
    uint32_t n = 0, required = 99;
    CHECK(b.begin_requests(&batch, &n) == KILN_OK && batch != 0 && n == 2);
    CHECK(b.file(request(KILN_DECISION_DEATH, 2002), 1, 10, &id3) == KILN_OK);  // arrives after the freeze
    CHECK(b.begin_requests(&again, &n) == KILN_OK && again == batch && n == 2);  // same batch until ack
    CHECK(b.read_requests(batch, nullptr, 0, 0, &required) == KILN_ERR_BUFFER_TOO_SMALL && required == 2);  // probe
    KilnDecisionRequestV1 one[1];
    std::memset(one, 0xAB, sizeof one);
    CHECK(b.read_requests(batch, one, 1, sizeof one[0], &required) == KILN_ERR_BUFFER_TOO_SMALL && required == 2);
    CHECK(one[0].size == 0xABABABABu);  // a short read writes nothing
    KilnDecisionRequestV1 two[2];
    CHECK(b.read_requests(batch, two, 2, sizeof two[0] - 8, &required) == KILN_ERR_INVALID_ARGUMENT);  // stride too small
    CHECK(b.read_requests(batch + 1, two, 2, sizeof two[0], &required) == KILN_ERR_NOT_FOUND);
    CHECK(b.read_requests(batch, two, 2, sizeof two[0], &required) == KILN_OK && required == 2);
    CHECK(two[0].request_id == id1 && two[1].request_id == id2 && two[0].world_epoch == 1 && two[0].capture_step == 10);
    CHECK(two[0].deadline_step == 12);
    // a wider stride (a newer caller's bigger record) is honoured
    std::vector<uint8_t> wide(2 * (sizeof two[0] + 64), 0xCD);
    CHECK(b.read_requests(batch, reinterpret_cast<KilnDecisionRequestV1*>(wide.data()), 2, sizeof two[0] + 64, &required) == KILN_OK);
    KilnDecisionRequestV1 second;
    std::memcpy(&second, wide.data() + sizeof two[0] + 64, sizeof second);
    CHECK(second.request_id == id2 && wide[sizeof two[0]] == 0xCD);
    // reading again gives the same: a read consumes nothing
    KilnDecisionRequestV1 re[2];
    CHECK(b.read_requests(batch, re, 2, sizeof re[0], &required) == KILN_OK && re[1].request_id == id2);
    CHECK(b.active_count() == 3);
    CHECK(b.ack_requests(batch, 11) == KILN_OK);
    CHECK(b.active_count() == 3);  // ack consumes delivery copies, never held state
    CHECK(b.ack_requests(batch, 12) == KILN_OK && b.ack_requests(batch, 13) == KILN_OK);  // idempotent for two Pres
    CHECK(b.ack_requests(batch, 14) == KILN_ERR_NOT_FOUND);
    CHECK(b.read_requests(batch, re, 2, sizeof re[0], &required) == KILN_ERR_NOT_FOUND);
    uint64_t next = 0;
    CHECK(b.begin_requests(&next, &n) == KILN_OK && next != batch && n == 1);  // the late arrival is the next batch
    CHECK(b.read_requests(next, re, 2, sizeof re[0], &required) == KILN_OK && required == 1 && re[0].request_id == id3);
    CHECK(b.ack_requests(next, 11) == KILN_OK);
    CHECK(b.begin_requests(&next, &n) == KILN_OK && next == 0 && n == 0 && b.ack_requests(0, 11) == KILN_OK);  // empty
}

static uint32_t g_finisher_status = KILN_DECISION_PREVENTED;
static int g_finisher_calls = 0;
static uint32_t finisher(Finish& f, void*) {
    ++g_finisher_calls;
    f.health_known = true;
    f.health_before = -3;
    f.applied_health = f.answer->allow ? -3 : f.answer->survival_health;
    std::snprintf(f.reason, sizeof f.reason, "finisher said %u", g_finisher_status);
    return f.answer->allow ? KILN_DECISION_ALLOWED : g_finisher_status;
}

// Every terminal status, each exactly once per request, and idempotent answers.
static void test_answers_and_statuses() {
    Broker b;
    b.attach(1);
    b.set_finisher(KILN_DECISION_DEATH, &finisher, nullptr);
    const auto death = request(KILN_DECISION_DEATH, 2000);
    const auto equip = request(KILN_DECISION_EQUIP_SET, 5);
    uint64_t ids[8] = {};
    for (int i = 0; i < 6; ++i) CHECK(b.file(death, 1, 10, &ids[i]) == KILN_OK);
    CHECK(b.file(equip, 1, 10, &ids[6]) == KILN_OK && b.file(equip, 1, 10, &ids[7]) == KILN_OK);
    KilnDecisionReceiptV1 rc{};
    // wrong epoch / kind / unknown id
    CHECK(b.answer(answer_for(death, ids[0], 2, true), &rc, 11) == KILN_ERR_INVALID_ARGUMENT);
    CHECK(b.answer(answer_for(equip, ids[0], 1, true), &rc, 11) == KILN_ERR_INVALID_ARGUMENT);
    CHECK(b.answer(answer_for(death, 999999, 1, true), &rc, 11) == KILN_ERR_NOT_FOUND && rc.terminal_status == 0);
    CHECK(g_finisher_calls == 0 && b.active_count() == 8);
    // Allowed
    CHECK(b.answer(answer_for(death, ids[0], 1, true), &rc, 11) == KILN_OK && rc.accepted == 1 && rc.terminal_status == KILN_DECISION_ALLOWED);
    // Prevented, with health fields from the finisher
    g_finisher_status = KILN_DECISION_PREVENTED;
    CHECK(b.answer(answer_for(death, ids[1], 1, false, 25), &rc, 11) == KILN_OK && rc.terminal_status == KILN_DECISION_PREVENTED);
    CHECK(std::string(rc.reason) == "finisher said 2");
    // the same answer again: the terminal status, no second write, no second outcome
    const int calls = g_finisher_calls;
    CHECK(b.answer(answer_for(death, ids[1], 1, false, 25), &rc, 11) == KILN_OK && rc.accepted == 0 && rc.terminal_status == KILN_DECISION_PREVENTED);
    CHECK(b.answer(answer_for(death, ids[1], 1, true), &rc, 11) == KILN_OK && rc.terminal_status == KILN_DECISION_PREVENTED);
    CHECK(g_finisher_calls == calls);
    CHECK(b.answer(answer_for(equip, ids[1], 1, true), &rc, 11) == KILN_ERR_NOT_FOUND);  // terminal id, other kind
    // finisher-made statuses
    g_finisher_status = KILN_DECISION_ALREADY_ALIVE;
    CHECK(b.answer(answer_for(death, ids[2], 1, false, 5), &rc, 11) == KILN_OK && rc.terminal_status == KILN_DECISION_ALREADY_ALIVE);
    g_finisher_status = KILN_DECISION_NO_POSITIVE_RANGE;
    CHECK(b.answer(answer_for(death, ids[3], 1, false, 5), &rc, 11) == KILN_OK && rc.terminal_status == KILN_DECISION_NO_POSITIVE_RANGE);
    g_finisher_status = KILN_DECISION_WRITE_REFUSED;
    CHECK(b.answer(answer_for(death, ids[4], 1, false, 5), &rc, 11) == KILN_OK && rc.terminal_status == KILN_DECISION_WRITE_REFUSED);
    // producer-side endings
    CHECK(b.complete(ids[5], KILN_DECISION_SOURCE_CHANGED, "the actor is already dead", 11));
    CHECK(!b.complete(ids[5], KILN_DECISION_STALE_ENTITY, "again", 11));  // once
    CHECK(b.complete(ids[6], KILN_DECISION_STALE_ENTITY, "the entity is gone", 11));
    // no finisher: as answered
    CHECK(b.answer(answer_for(equip, ids[7], 1, false), &rc, 11) == KILN_OK && rc.terminal_status == KILN_DECISION_PREVENTED);
    CHECK(b.status_of(ids[7]) == KILN_DECISION_PREVENTED && b.status_of(424242) == UINT32_MAX);
    CHECK(b.active_count() == 0);
    auto out = outcomes(b, 11);
    CHECK(out.size() == 8);  // one outcome per request, repeats made none
    CHECK(out[ids[0]].status == KILN_DECISION_ALLOWED && out[ids[1]].status == KILN_DECISION_PREVENTED);
    CHECK(out[ids[1]].requested_health == 25 && out[ids[1]].applied_health == 25 && out[ids[1]].health_before == -3);
    CHECK((out[ids[1]].flags & KILN_DECISION_OUTCOME_HEALTH_KNOWN) != 0 && out[ids[7]].flags == 0);
    CHECK(out[ids[2]].status == KILN_DECISION_ALREADY_ALIVE && out[ids[3]].status == KILN_DECISION_NO_POSITIVE_RANGE);
    CHECK(out[ids[4]].status == KILN_DECISION_WRITE_REFUSED && out[ids[5]].status == KILN_DECISION_SOURCE_CHANGED);
    CHECK(out[ids[6]].status == KILN_DECISION_STALE_ENTITY && std::string(out[ids[6]].reason) == "the entity is gone");
    CHECK(out[ids[0]].subject.entity == 2000 && out[ids[0]].world_epoch == 1 && out[ids[0]].step == 11 && out[ids[0]].size == sizeof(KilnDecisionOutcomeV1));
    CHECK(outcomes(b, 11).empty());
}

static void test_deadlines() {
    Broker b;
    b.attach(1);
    uint64_t hook = 0, input = 0, set = 0;
    b.file(request(KILN_DECISION_DEATH, 2000), 1, 10, &hook);        // captured in the sim of step 10
    b.file(request(KILN_DECISION_EQUIP_SET, 5), 1, 10, &set);        // queued set decision: two Pres as well
    b.window_begin(1, 11);                                           // after Pre 11: first chance used or not
    b.file(request(KILN_DECISION_EQUIP_SINGLE, 5), 1, 11, &input);   // a client input taken in Pre 11... (filed late on purpose)
    b.window_end(1, 10);
    CHECK(b.status_of(input) == 0);                                  // not due at an earlier step's end
    b.window_begin(1, 12);                                           // after Pre 12: second chance
    CHECK(b.status_of(hook) == 0 && b.status_of(set) == 0);          // answers in Pre 11 and Pre 12 are in time
    b.window_end(1, 11);
    CHECK(b.status_of(input) == KILN_DECISION_TIMEOUT_RELEASED);     // one-Pre hold: over at its own step's end
    CHECK(b.status_of(hook) == 0);                                   // window_end never ends a two-Pre hold
    b.window_begin(1, 13);                                           // capture + 3
    CHECK(b.status_of(hook) == KILN_DECISION_TIMEOUT_RELEASED && b.status_of(set) == KILN_DECISION_TIMEOUT_RELEASED);
    KilnDecisionReceiptV1 rc{};
    CHECK(b.answer(answer_for(request(KILN_DECISION_DEATH, 2000), hook, 1, false, 9), &rc, 13) == KILN_OK && rc.accepted == 0 &&
          rc.terminal_status == KILN_DECISION_TIMEOUT_RELEASED);     // a late answer changes nothing
    CHECK(b.counters().timeouts == 3);
    auto out = outcomes(b, 13);
    CHECK(out.size() == 3 && out[hook].status == KILN_DECISION_TIMEOUT_RELEASED && out[hook].step == 13 && out[input].step == 11);
    // an answer in the second Pre is taken
    uint64_t late = 0;
    b.file(request(KILN_DECISION_DEATH, 2001), 1, 20, &late);
    b.window_begin(1, 21);
    b.window_begin(1, 22);
    CHECK(b.answer(answer_for(request(KILN_DECISION_DEATH, 2001), late, 1, true), &rc, 22) == KILN_OK && rc.accepted == 1);
}

static void test_world_and_consumer_turnover() {
    Broker b;
    b.attach(1);
    uint64_t a = 0, c = 0, d = 0;
    b.file(request(KILN_DECISION_DEATH, 2000), 1, 10, &a);
    b.world_ended(10);
    CHECK(b.status_of(a) == KILN_DECISION_WORLD_ENDED && b.active_count() == 0);
    uint64_t batch = 1;
    uint32_t n = 1;
    CHECK(b.begin_requests(&batch, &n) == KILN_OK && batch == 0 && n == 0);  // undelivered copies are gone
    // a request of an older epoch that is still active when the next world steps
    b.file(request(KILN_DECISION_DEATH, 2000), 1, 10, &c);
    b.window_begin(2, 11);
    CHECK(b.status_of(c) == KILN_DECISION_WORLD_ENDED);
    // detach releases
    b.file(request(KILN_DECISION_WALKOVER, 2000), 2, 11, &d);
    CHECK(b.detach(1, 11) == KILN_OK && b.status_of(d) == KILN_DECISION_CAPABILITY_RELEASED);
    CHECK(b.attach(2) == KILN_OK);  // the next consumer reads the outcomes
    auto out = outcomes(b, 11);
    CHECK(out.size() == 3 && out[a].status == KILN_DECISION_WORLD_ENDED && out[d].status == KILN_DECISION_CAPABILITY_RELEASED);
    // ids never repeat within the process
    uint64_t e = 0;
    b.file(request(KILN_DECISION_DEATH, 2000), 2, 12, &e);
    CHECK(e > d);
}

static void test_limits() {
    auto b = std::make_unique<Broker>();
    b->attach(1);
    const auto r = request(KILN_DECISION_DEATH, 2000);
    uint64_t id = 0, first = 0;
    for (uint32_t i = 0; i < KILN_DECISIONS_MAX_ACTIVE; ++i) {
        if (b->file(r, 1, 10, &id) != KILN_OK) break;
        if (!first) first = id;
    }
    CHECK(b->active_count() == KILN_DECISIONS_MAX_ACTIVE);
    uint64_t dropped = 0;
    CHECK(b->file(r, 1, 10, &dropped) == KILN_ERR_BUFFER_TOO_SMALL && dropped != 0);  // fails open, with an id
    CHECK(b->status_of(dropped) == KILN_DECISION_QUEUE_DROPPED && b->counters().queue_dropped == 1);
    CHECK(b->active_count() == KILN_DECISIONS_MAX_ACTIVE);
    uint64_t batch = 0;
    uint32_t n = 0;
    b->begin_requests(&batch, &n);
    CHECK(n == KILN_DECISIONS_MAX_ACTIVE);
    b->ack_requests(batch, 10);
    // answering frees a slot
    KilnDecisionReceiptV1 rc{};
    CHECK(b->answer(answer_for(r, first, 1, true), &rc, 11) == KILN_OK);
    CHECK(b->file(r, 1, 11, &id) == KILN_OK);
    // outbox: nobody reads; at 8192 undelivered outcomes further ones are counted as a gap, not queued
    b->world_ended(11);  // 4096 outcomes at once
    CHECK(!b->saturated());
    uint64_t made = 2 + KILN_DECISIONS_MAX_ACTIVE;
    while (!b->saturated()) {
        b->file(r, 1, 12, &id);
        b->complete(id, KILN_DECISION_STALE_ENTITY, "gone", 12);
        ++made;
    }
    CHECK(made == KILN_DECISIONS_MAX_OUTCOMES);
    b->file(r, 1, 12, &id);
    b->complete(id, KILN_DECISION_STALE_ENTITY, "gone", 12);
    CHECK(b->counters().outcome_gap == 1 && b->status_of(id) == KILN_DECISION_STALE_ENTITY);
    b->begin_outcomes(&batch, &n);
    CHECK(n == KILN_DECISIONS_MAX_OUTCOMES);
    uint32_t required = 0;
    CHECK(b->read_outcomes(batch, nullptr, 0, 0, &required) == KILN_ERR_BUFFER_TOO_SMALL && required == KILN_DECISIONS_MAX_OUTCOMES);
    CHECK(b->ack_outcomes(batch, 12) == KILN_OK && !b->saturated());
    // terminal statuses are remembered for the last 8192 ids only: the first terminal one of this test (8193 ago) is forgotten, the second is not
    CHECK(b->status_of(dropped) == UINT32_MAX && b->status_of(first) == KILN_DECISION_ALLOWED);
    CHECK(b->status_of(id) == KILN_DECISION_STALE_ENTITY);
}

// ---- metadata ----
static std::map<uint32_t, Sample> g_world;
static int g_samples = 0;
static bool sampler(uint32_t e, Sample* out, void*) {
    ++g_samples;
    auto it = g_world.find(e);
    if (it == g_world.end()) {
        *out = Sample{};
        return true;
    }
    *out = it->second;
    return true;
}
static Sample thing(uint64_t spawn, bool pos = true) {
    Sample s;
    s.exists = true;
    s.has_spawn_time = true;
    s.spawn_time = spawn;
    s.has_position = pos;
    s.position[0] = 1.5;
    return s;
}
static Sample player(uint64_t handle, uint64_t joined, bool in_game = true) {
    Sample s;
    s.exists = true;
    s.player_in_game = in_game;
    s.player_handle = handle;
    s.joined_step = joined;
    s.has_position = true;
    return s;
}

static void test_metadata() {
    Metadata m;
    m.set_sampler(&sampler, nullptr);
    CHECK(m.table() && m.table()->rows.empty());
    g_world[2000] = thing(77);
    g_world[2001] = thing(77);          // same SpawnTime, another id
    g_world[2002] = thing(0);           // SpawnTime present and zero
    Sample bare;
    bare.exists = true;                 // no SpawnTime, not a player
    g_world[2003] = bare;
    g_world[3] = player(0xAA, 5);
    g_world[4] = player(0, 0, false);   // player id without an in-game record
    g_world[2004] = thing(9, false);    // no position
    std::vector<uint32_t> wanted = {2001, 2000, 2000, 2002, 2003, 3, 4, 2004, 9999, 0};
    uint32_t misses = 0;
    CHECK(m.rebuild(1, 10, wanted, &misses));
    auto t = m.table();
    CHECK(t->world_epoch == 1 && t->step == 10 && t->rows.size() == 5 && misses == 2);
    const Row* a = t->find(2000);
    const Row* b = t->find(2001);
    CHECK(a && b && a->key.life_serial != 0 && a->key.life_serial != b->key.life_serial);  // equal timestamps, different rows
    CHECK(a->key.flags == KILN_EVENT_KEY_SPAWN_TIME && a->key.spawn_time == 77 && a->has_position == 1 && a->position[0] == 1.5 && a->sampled_step == 10);
    CHECK(t->find(2002) && t->find(2002)->key.flags == KILN_EVENT_KEY_SPAWN_TIME && t->find(2002)->key.spawn_time == 0);
    CHECK(!t->find(2003) && !t->find(4) && !t->find(9999));  // no made-up identity
    const Row* p = t->find(3);
    CHECK(p && p->key.flags == KILN_EVENT_KEY_PLAYER_SESSION && p->key.player_handle == 0xAA && p->key.joined_step == 5 && p->key.spawn_time == 0);
    CHECK(t->find(2004) && t->find(2004)->has_position == 0);  // a missing position alone is legal
    // the same lives keep their serials; the old table object is untouched by a rebuild
    const uint64_t serial_a = a->key.life_serial, serial_p = p->key.life_serial;
    g_world[2000] = thing(77);
    g_world[2001] = thing(78);       // the id was reused: another SpawnTime
    g_world[3] = player(0xAA, 900);  // the player rejoined
    wanted = {2000, 2001, 3};
    CHECK(m.rebuild(1, 11, wanted, &misses) && misses == 0);
    auto t2 = m.table();
    CHECK(t->rows.size() == 5 && t->find(2001)->key.spawn_time == 77);  // immutable
    CHECK(t2->find(2000)->key.life_serial == serial_a);
    CHECK(t2->find(2001)->key.life_serial != b->key.life_serial && t2->find(2001)->key.life_serial > serial_p);
    CHECK(t2->find(3)->key.life_serial != serial_p);
    // not asked about for a while, then again: still the same life, the same serial
    wanted = {3};
    for (uint64_t s = 12; s < 40; ++s) m.rebuild(1, s, wanted, &misses);
    wanted = {2000};
    m.rebuild(1, 40, wanted, &misses);
    CHECK(m.table()->find(2000)->key.life_serial == serial_a);
    // a new world epoch is a new life although nothing else changed
    wanted = {2000};
    m.rebuild(2, 41, wanted, &misses);
    CHECK(m.table()->find(2000)->key.life_serial != serial_a);
    // resolve: one row, no scan
    g_samples = 0;
    Row r;
    bool miss = false;
    CHECK(m.resolve(2, 41, 2000, &r, &miss) && g_samples == 1 && r.key.life_serial == m.table()->find(2000)->key.life_serial && !miss);
    CHECK(!m.resolve(2, 41, 2003, &r, &miss) && miss);
    CHECK(!m.resolve(2, 41, 9999, &r, &miss) && !miss);
    CHECK(!m.resolve(2, 41, 0, &r, &miss));
    // budget: more than 8192 distinct ids publishes an empty table and says so
    std::vector<uint32_t> many;
    for (uint32_t e = 0; e <= KILN_DECISIONS_MAX_METADATA; ++e) {
        g_world[100000 + e] = thing(e);
        many.push_back(100000 + e);
    }
    CHECK(!m.rebuild(2, 42, many, &misses) && m.table()->rows.empty());
    many.pop_back();
    CHECK(m.rebuild(2, 43, many, &misses) && m.table()->rows.size() == KILN_DECISIONS_MAX_METADATA);
    // clear: no rows, and serials do not start over
    const uint64_t before = m.table()->rows.back().key.life_serial;
    m.clear();
    CHECK(m.table()->rows.empty());
    wanted = {2000};
    m.rebuild(2, 44, wanted, &misses);
    CHECK(m.table()->find(2000)->key.life_serial > before);
}

int main() {
    test_validation_and_consumer();
    test_batches();
    test_answers_and_statuses();
    test_deadlines();
    test_world_and_consumer_turnover();
    test_limits();
    test_metadata();
    std::printf("queue_test: %d PASS, %d FAIL\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
