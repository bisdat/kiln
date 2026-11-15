// SPDX-License-Identifier: MIT
// Host test of the rollback invalidation (reset_watch.h, chapter 07 §4.1) on the real broker, metadata, death holds
// and pickup exposures: the world pointer, the entity id and its SpawnTime stay the same across the rollback, only
// kiln.items' reset generation moves. Afterwards no old decision identity, hold, suppression or permit survives: the
// open requests ended StaleEntity, the same id gets a new life serial, and both producers start again (review P1
// "rollback invalidation required by chapter 4.1 is unwired", 2026-11-14). With -DKILN_RESET_BEFORE it runs against
// what the service did before the fix (nothing at a regression), which is the red run.
#include <cstdio>
#include <map>

#include "../death_core.h"
#include "../pickup_core.h"
#include "../reset_watch.h"

using namespace kiln::decisions;

static int g_pass = 0, g_fail = 0;
#define CHECK(x) ((x) ? ++g_pass : (++g_fail, std::printf("FAIL line %d: %s\n", __LINE__, #x)))

// The world: the same entities, the same SpawnTimes, before and after the rollback.
static bool sampler(uint32_t e, Sample* out, void*) {
    *out = Sample{};
    out->exists = true;
    out->has_spawn_time = true;
    out->spawn_time = 5000;  // unchanged by the rollback
    (void)e;
    return true;
}

struct DeathPorts {
    Metadata& md;
    int32_t hp = 0;
    uint32_t e = 2000;
    uint64_t ep = 1, st = 10;
    int32_t health() const { return hp; }
    bool armed() const { return true; }
    bool server_world() const { return true; }
    uint32_t entity() const { return e; }
    uint64_t epoch() const { return ep; }
    uint64_t step() const { return st; }
    uint32_t guard(uint64_t, uint32_t, int32_t) const { return KILN_DEATH_GUARD_PASS; }
    bool any_interest() const { return true; }
    bool interested(uint32_t) const { return true; }
    bool actor_blocked() const { return false; }
    bool holding() const { return true; }
    bool row(uint32_t x, uint64_t epoch, Row* out) const {
        const auto t = md.table();
        if (t->world_epoch != epoch) return false;
        const Row* r = t->find(x);
        return r ? (*out = *r, true) : false;
    }
    void filed(uint32_t, uint64_t) const {}
};
struct PickupPorts {
    Metadata& md;
    uint32_t drop = 3000, recv = 2022;
    uint64_t ep = 1, st = 10;
    bool armed() const { return true; }
    bool server_world() const { return true; }
    uint32_t receiver() const { return recv; }
    bool any_interest() const { return true; }
    uint32_t entity() const { return drop; }
    bool interested(uint32_t) const { return true; }
    bool item(uint32_t* item, uint32_t* amount) const { return *item = 7, *amount = 3, true; }
    uint32_t feedback() const { return 1; }
    uint64_t epoch() const { return ep; }
    uint64_t step() const { return st; }
    bool holding() const { return true; }
    bool row(uint32_t x, uint64_t epoch, Row* out) const {
        const auto t = md.table();
        if (t->world_epoch != epoch) return false;
        const Row* r = t->find(x);
        return r ? (*out = *r, true) : false;
    }
};

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
static std::vector<KilnDecisionRequestV1> requests(Broker& b, uint64_t step) {
    uint64_t batch = 0;
    uint32_t n = 0, req = 0;
    b.begin_requests(&batch, &n);
    std::vector<KilnDecisionRequestV1> v(n);
    if (n) b.read_requests(batch, v.data(), n, sizeof(KilnDecisionRequestV1), &req);
    b.ack_requests(batch, step);
    return v;
}

// What the service does at window_begin with kiln.items' generation (before the fix: nothing).
static void window_begin(ResetWatch& w, uint64_t generation, Broker& b, Metadata& md, death::Holds& h, pickup::Exposures& x, uint64_t step) {
#ifdef KILN_RESET_BEFORE
    (void)w, (void)generation, (void)b, (void)md, (void)h, (void)x, (void)step;
#else
    if (w.changed(generation)) invalidate_after_reset(b, md, h, x, step);
#endif
    std::vector<uint32_t> wanted = {2000, 2022, 3000};
    uint32_t misses = 0;
    md.rebuild(1, step, wanted, &misses);
    b.window_begin(1, step);
}

int main() {
    Broker b;
    Metadata md;
    death::Holds holds;
    pickup::Exposures exposures;
    ResetWatch watch;
    md.set_sampler(&sampler, nullptr);
    b.attach(9);
    window_begin(watch, 4, b, md, holds, exposures, 10);  // the first value seen: no invalidation
    const uint64_t serial_before = md.table()->find(2000)->key.life_serial;

    // step 10: a death is held; a pickup is filed and canceled (suppressed); another drop is allowed (a permit)
    DeathPorts dp{md};
    PickupPorts pp{md};
    CHECK(holds.visit(dp, b) == death::Visit::Skip);
    CHECK(exposures.visit(pp, b) == pickup::Visit::Refuse);
    auto reqs = requests(b, 11);
    CHECK(reqs.size() == 2);
    for (auto& r : reqs)
        if (r.kind == KILN_DECISION_WALKOVER) {
            KilnDecisionAnswerV1 a{};
            a.size = sizeof a, a.kind = r.kind, a.request_id = r.request_id, a.world_epoch = r.world_epoch, a.allow = 0;
            b.answer(a, nullptr, 11);
        }
    pp.st = dp.st = 11;
    CHECK(exposures.visit(pp, b) == pickup::Visit::Refuse);  // suppressed
    outcomes(b, 11);                                          // the pickup's Prevented is read

    // step 12: a rollback. kiln.items' Pre saw the entity id counter go down; the world pointer, the entity ids and
    // their SpawnTimes did not change.
    window_begin(watch, 5, b, md, holds, exposures, 12);
    pp.st = dp.st = 12;
    auto out = outcomes(b, 12);
    CHECK(out.size() == 1 && !out.empty() && out.begin()->second.kind == KILN_DECISION_DEATH &&
          out.begin()->second.status == KILN_DECISION_STALE_ENTITY);  // the held death ended
    CHECK(b.active_count() == 0);
    CHECK(md.table()->find(2000) && md.table()->find(2000)->key.life_serial != serial_before);  // another life now
    CHECK(holds.size() == 0 && exposures.size() == 0);
    // the old suppression does not refuse the pickup of what is now another past: a new decision is asked for
    CHECK(exposures.visit(pp, b) == pickup::Visit::Refuse && b.counters().filed == 3);
    // the death at 0 health is a new decision of the new life, not the old hold
    CHECK(holds.visit(dp, b) == death::Visit::Skip && b.counters().filed == 4);
    auto fresh = requests(b, 13);
    CHECK(fresh.size() == 2);
    for (auto& r : fresh)
        if (r.kind == KILN_DECISION_DEATH) CHECK(r.subject.life_serial != serial_before && r.capture_step == 12);
    // the same generation again: nothing more
    const uint64_t serial_after = md.table()->find(2000)->key.life_serial;
    window_begin(watch, 5, b, md, holds, exposures, 13);
    CHECK(md.table()->find(2000)->key.life_serial == serial_after && b.active_count() == 2);
#ifndef KILN_RESET_BEFORE
    CHECK(watch.invalidations() == 1);
    // unknown (no kiln.items): never an invalidation
    ResetWatch blind;
    CHECK(!blind.changed(ResetWatch::kUnknown) && !blind.changed(ResetWatch::kUnknown));
#endif
    std::printf("reset_test: %d PASS, %d FAIL\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
