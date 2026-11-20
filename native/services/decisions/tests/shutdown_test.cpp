// SPDX-License-Identifier: MIT
// Host test of the coordinated shutdown (shutdown.h) on the real broker, death holds and pickup exposures: a window call
// that faults (the core stops calling it, so no deadline advances any more) or a hook that faults, with requests open.
// Afterwards nothing is held, a lethal visit and a pickup visit pass to the game, every open decision ended
// CAPABILITY_RELEASED, and the rows read unavailable (review P1 "a caught window fault does not shut down the
// producers", 2026-11-14). Built with -DKILN_SHUTDOWN_BEFORE it runs against what the service did before the fix
// (nothing), which is the red run.
#include <cstdio>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "../death_core.h"
#include "../pickup_core.h"
#include "../shutdown.h"

using namespace kiln::decisions;

static int g_pass = 0, g_fail = 0;
#define CHECK(x) ((x) ? ++g_pass : (++g_fail, std::printf("FAIL line %d: %s\n", __LINE__, #x)))

#ifdef KILN_SHUTDOWN_BEFORE
// The service before the fix: a fault changed nothing in it.
struct Life {
    bool holding() const { return true; }
    void stop(const char*) {}
    bool pending() const { return false; }
    template <class H, class X>
    bool finish(Broker&, H&&, X&&, uint64_t, RowSink, void*, Shutdown::Finished* = nullptr) { return false; }
};
#else
using Life = Shutdown;
#endif

static Row row_of(uint32_t e, uint64_t serial) {
    Row r;
    r.key.entity = e;
    r.key.flags = KILN_EVENT_KEY_SPAWN_TIME;
    r.key.spawn_time = 100 + e;
    r.key.life_serial = serial;
    return r;
}

// The service's two hooks as the service wires them: armed while the hook is in AND the service holds.
struct DeathPorts {
    const Life& life;
    int32_t hp = 0;
    uint32_t e = 2000;
    uint64_t ep = 1, st = 10;
    int32_t health() const { return hp; }
    bool armed() const { return life.holding(); }
    bool server_world() const { return true; }
    uint32_t entity() const { return e; }
    uint64_t epoch() const { return ep; }
    uint64_t step() const { return st; }
    uint32_t guard(uint64_t, uint32_t, int32_t) const { return KILN_DEATH_GUARD_PASS; }
    bool any_interest() const { return true; }
    bool interested(uint32_t) const { return true; }
    bool actor_blocked() const { return false; }
    bool holding() const { return true; }
    bool row(uint32_t x, uint64_t, Row* out) const { return *out = row_of(x, 7), true; }
    void filed(uint32_t, uint64_t) const {}
};
struct PickupPorts {
    const Life& life;
    uint32_t drop = 3000, recv = 2022;
    uint64_t ep = 1, st = 10;
    bool armed() const { return life.holding(); }
    bool server_world() const { return true; }
    uint32_t receiver() const { return recv; }
    bool any_interest() const { return true; }
    uint32_t entity() const { return drop; }
    bool interested(uint32_t) const { return true; }
    bool item(uint32_t* item, uint32_t* amount) const { return *item = 0x2b0cf1d5, *amount = 3, true; }
    uint32_t feedback() const { return 1; }
    uint64_t epoch() const { return ep; }
    uint64_t step() const { return st; }
    bool holding() const { return true; }
    bool row(uint32_t x, uint64_t, Row* out) const { return *out = row_of(x, 9), true; }
};

struct Rows {
    std::map<std::string, std::pair<uint32_t, std::string>> lowered;
    static void sink(const char* row, uint32_t reason, const char* why, void* user) {
        static_cast<Rows*>(user)->lowered[row] = {reason, why};
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

static KilnDecisionRequestV1 prepared(uint32_t kind, uint32_t entity) {
    KilnDecisionRequestV1 r{};
    r.size = sizeof r;
    r.version = 1;
    r.kind = kind;
    r.payload_bytes = payload_size(kind);
    r.subject = row_of(entity, 1).key;
    return r;
}

// A window call faults with a death held, a pickup pending and a set decision open; the core calls no window again.
static void test_window_fault_with_open_requests() {
    Broker b;
    death::Holds holds;
    pickup::Exposures exposures;
    Life life;
    Rows rows;
    b.attach(9);
    DeathPorts dp{life};
    PickupPorts pp{life};
    CHECK(holds.visit(dp, b) == death::Visit::Skip);
    CHECK(exposures.visit(pp, b) == pickup::Visit::Refuse);
    uint64_t set_id = 0;
    CHECK(b.file(prepared(KILN_DECISION_EQUIP_SET, 2022), 1, 10, &set_id) == KILN_OK);
    CHECK(b.active_count() == 3);

    // step 11: window_begin faults. The core's window_faulted reaches the service.
    life.stop("a window call of kiln.decisions faulted (0xc0000005)");
    Shutdown::Finished f;
    const bool finished = life.finish(b, holds, exposures, 11, &Rows::sink, &rows, &f);
    CHECK(finished && f.released && f.cleared);
    CHECK(!life.holding() && !life.pending());

    // no window call ever again: steps 12..20 pass; the death and the pickup must not be held any more
    for (uint64_t step = 12; step <= 20; ++step) {
        dp.st = pp.st = step;
        CHECK(holds.visit(dp, b) == death::Visit::Pass);
        CHECK(exposures.visit(pp, b) == pickup::Visit::Pass);
    }
    CHECK(b.active_count() == 0 && holds.size() == 0 && exposures.size() == 0);
    CHECK(b.counters().filed == 3);  // nothing new was filed after the fault
    auto out = outcomes(b, 20);
    CHECK(out.size() == 3);
    for (auto& [id, o] : out) CHECK(o.status == KILN_DECISION_CAPABILITY_RELEASED && std::strstr(o.reason, "shut down") != nullptr);
    CHECK(rows.lowered.size() == 5);
    for (const char* row : kShutdownRows)
        CHECK(rows.lowered.count(row) && rows.lowered[row].first == KILN_CAP_REASON_FAULTED &&
              rows.lowered[row].second.find("faulted") != std::string::npos);
    // a second finish does nothing (logged once by the caller)
    CHECK(!life.finish(b, holds, exposures, 21, &Rows::sink, &rows));
}

// A hook faults on a worker: holding stops there and then; the rest waits for the world thread.
static void test_hook_fault_on_a_worker() {
    Broker b;
    death::Holds holds;
    pickup::Exposures exposures;
    Life life;
    Rows rows;
    b.attach(9);
    DeathPorts dp{life};
    CHECK(holds.visit(dp, b) == death::Visit::Skip);
    std::thread worker([&] { life.stop("the death hook faulted (0xc0000005)"); });
    worker.join();
    CHECK(!life.holding() && life.pending());
    dp.st = 11;
    CHECK(holds.visit(dp, b) == death::Visit::Pass);  // already before the world thread finished it
    CHECK(life.finish(b, holds, exposures, 11, &Rows::sink, &rows));
    auto out = outcomes(b, 11);
    CHECK(out.size() == 1 && out.begin()->second.status == KILN_DECISION_CAPABILITY_RELEASED);
    CHECK(rows.lowered.count("events.death") && rows.lowered["events.death"].second.find("death hook faulted") != std::string::npos);
}

#ifndef KILN_SHUTDOWN_BEFORE
// The fault left the broker's lock held (no destructor ran): finish neither waits nor touches it; the hooks pass.
static void test_lock_left_held() {
    Broker b;
    death::Holds holds;
    pickup::Exposures exposures;
    Shutdown life;
    Rows rows;
    b.attach(9);
    DeathPorts dp{life};
    CHECK(holds.visit(dp, b) == death::Visit::Skip);
    Shutdown::Finished f;
    std::thread faulted([&] {
        auto held = b.lock_for_tests();  // a fault inside the broker on this thread
        life.stop("a window call of kiln.decisions faulted (0xc0000005)");
        life.finish(b, holds, exposures, 11, &Rows::sink, &rows, &f);
    });
    faulted.join();
    CHECK(!f.released && !life.broker_ok() && f.cleared);
    CHECK(holds.visit(dp, b) == death::Visit::Pass);  // no lock taken: the hook passes first
    CHECK(rows.lowered.size() == 5);
}
#endif

int main() {
    test_window_fault_with_open_requests();
    test_hook_fault_on_a_worker();
#ifndef KILN_SHUTDOWN_BEFORE
    test_lock_left_held();
#endif
    std::printf("shutdown_test: %d PASS, %d FAIL\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
