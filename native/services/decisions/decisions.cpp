// SPDX-License-Identifier: MIT
// kiln.decisions v1 (abi/services/kiln_decisions.h): the service around the broker (queue.h) and the metadata table
// (metadata.h). This file holds what touches the process: the table's thread and phase checks, the world reads of
// the metadata sampler, the world events. Producers (death.cpp, ...) are files of their own beside it; host.h is
// what they get from here.
//
// Rules of this module:
//  - No hook calls managed code or a tick-only service. A hook copies a request and reads a verdict.
//  - World reads happen in sample() only, which runs at window_begin (no world job is live) or in resolve_entity
//    (world thread, world-safe phase). A hook reads the published table.
#include <windows.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#include "host.h"
#include "kiln_api.h"
#include "metadata.h"
#include "queue.h"
#include "services/kiln_caps.h"
#include "services/kiln_decisions.h"
#include "services/kiln_players.h"
#include "services/kiln_window_clock.h"
#include "services/kiln_world.h"
#include "world_gate.h"

namespace kiln::decisions {
namespace {

const KilnApi* g_api = nullptr;
KilnOwner g_owner = 0;
const KilnWorldApi* g_world = nullptr;
const KilnPlayersApi* g_players = nullptr;
const KilnWindowClockApiV1* g_clock = nullptr;
const KilnCapsApi* g_caps = nullptr;
std::atomic<bool> g_on{false};
uint32_t g_spawn_time_hash = 0;

Broker* g_broker = nullptr;  // made in kiln_plugin_main, never freed (4096 request slots: too big for a static)
Metadata g_metadata;
WorldGate g_gate;  // set in on_ready from the core constants world.storage and storage.kind

// The consumer's interest per kind: all entities, or a list.
struct Interest {
    bool all = false;
    std::vector<uint32_t> ids;  // sorted
};
std::mutex g_interest_mutex;
Interest g_interest[64];
std::atomic<uint32_t> g_interest_state[64];  // 0 none, 1 a list, 2 all: what a hook asks first, without the lock
std::atomic<uint64_t> g_interest_kinds{0};    // bit n: kind n has an interest (window_wanted asks it)
std::atomic<bool> g_budget_exceeded{false};   // written at window_begin
std::atomic<uint64_t> g_epoch{0}, g_step{0};  // the window the step is in
std::atomic<uint32_t> g_world_thread{0};      // the thread window_begin ran on

// Decision kinds that need no native producer: a Pre adapter files them with enqueue_prepared.
constexpr uint64_t kPreparedKinds =
    (1ull << KILN_DECISION_EQUIP_SINGLE) | (1ull << KILN_DECISION_EQUIP_COLLECTION) | (1ull << KILN_DECISION_EQUIP_SET);
std::atomic<uint64_t> g_hook_kinds{0};  // set by the producers that installed

void logf(int level, const char* fmt, ...) {
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    const int n = std::vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    if (n > 0) g_api->log(g_owner, level, KilnStr{b, static_cast<size_t>(n < static_cast<int>(sizeof b) ? n : sizeof b - 1)});
}

KilnStr ks(const char* s) { return KilnStr{s, std::strlen(s)}; }

// The clock state when the caller is on the world thread in a world-safe phase.
bool world_safe(KilnWindowStateV1* out) {
    KilnWindowStateV1 s{};
    if (!g_on.load(std::memory_order_relaxed) || g_clock->get(&s) != KILN_OK) return false;
    if (!kiln_window_world_safe(&s, GetCurrentThreadId())) return false;
    if (out) *out = s;
    return true;
}

// ---- metadata sampler: the only world reads of this service ----
bool sample(uint32_t entity, Sample* out, void*) {
    *out = Sample{};
    if (!g_world->exists(entity)) return true;
    out->exists = true;
    uint32_t size = 0;
    if (const void* p = g_world->component(entity, g_spawn_time_hash, &size); p && size >= 8) {
        out->has_spawn_time = true;
        std::memcpy(&out->spawn_time, p, 8);
    }
    KilnVec3 pos{};
    if (g_world->position(entity, &pos) == KILN_OK) {
        out->has_position = true;
        out->position[0] = pos.x;
        out->position[1] = pos.y;
        out->position[2] = pos.z;
    }
    if (is_player_id(entity) && g_players) {
        KilnPlayer pl{};
        pl.size = sizeof pl;
        if (g_players->get(entity, &pl) == KILN_OK && pl.state == KILN_PLAYER_IN_GAME && (pl.flags & KILN_PLAYER_FROM_RECORDS)) {
            out->player_in_game = true;
            out->player_handle = pl.handle;
            out->joined_step = pl.joined_step;
        }
    }
    return true;
}

// ---- table ----
uint64_t KILN_CALL d_supported_kinds() {
    if (!g_on.load(std::memory_order_relaxed)) return 0;
    return kPreparedKinds | g_hook_kinds.load(std::memory_order_relaxed);
}

KilnResult KILN_CALL d_attach(KilnOwner dispatcher) {
    if (!g_on.load()) return KILN_ERR_UNAVAILABLE;
    if (!world_safe(nullptr)) return KILN_ERR_WRONG_THREAD;
    return g_broker->attach(dispatcher);
}

KilnResult KILN_CALL d_detach(KilnOwner dispatcher) {
    if (!g_on.load()) return KILN_ERR_UNAVAILABLE;
    KilnWindowStateV1 s{};
    if (!world_safe(&s)) return KILN_ERR_WRONG_THREAD;
    const KilnResult rc = g_broker->detach(dispatcher, s.step);
    g_broker->drain_sinks();
    if (rc == KILN_OK) {
        std::lock_guard lock(g_interest_mutex);
        for (auto& i : g_interest) i = Interest{};
        for (auto& i : g_interest_state) i.store(0);
        g_interest_kinds.store(0);
    }
    return rc;
}

// A hooked kind's producer puts its hook in (the first listener). False: it cannot; its row says why.
bool arm_kind(uint32_t kind) {
    if (kind == KILN_DECISION_DEATH) return death_producer::arm();
    if (kind == KILN_DECISION_WALKOVER) return pickup_producer::arm();
    return true;
}

KilnResult KILN_CALL d_resolve_entity(uint32_t entity, KilnEventEntityKey* key, double position[3], uint32_t* has_position) {
    if (!g_on.load()) return KILN_ERR_UNAVAILABLE;
    if (!key || entity == 0) return KILN_ERR_INVALID_ARGUMENT;
    KilnWindowStateV1 s{};
    if (!world_safe(&s)) return KILN_ERR_WRONG_THREAD;  // also the world thread inside the sim step (phase 3)
    Row row;
    bool miss = false;
    if (!g_metadata.resolve(s.world_epoch, s.step, entity, &row, &miss)) {
        if (miss) g_broker->count_metadata_miss();
        return KILN_ERR_NOT_FOUND;
    }
    *key = row.key;
    if (has_position) *has_position = row.has_position;
    if (position) std::memcpy(position, row.position, sizeof row.position);
    return KILN_OK;
}

KilnResult KILN_CALL d_interest(KilnOwner dispatcher, const KilnDecisionInterestV1* in) {
    if (!g_on.load()) return KILN_ERR_UNAVAILABLE;
    if (!in || in->size < sizeof(KilnDecisionInterestV1) || in->kind >= 64 || payload_size(in->kind) == 0 ||
        in->entity_count > KILN_DECISIONS_MAX_INTEREST || (in->entity_count && !in->entities))
        return KILN_ERR_INVALID_ARGUMENT;
    if (!world_safe(nullptr)) return KILN_ERR_WRONG_THREAD;
    if (!g_broker->is_consumer(dispatcher)) return KILN_ERR_UNAVAILABLE;
    Interest next;
    next.all = in->all_entities != 0;
    next.ids.assign(in->entities, in->entities + in->entity_count);
    std::sort(next.ids.begin(), next.ids.end());
    next.ids.erase(std::unique(next.ids.begin(), next.ids.end()), next.ids.end());
    const uint32_t state = next.all ? 2u : next.ids.empty() ? 0u : 1u;
    // The first interest of a hooked kind puts its hook in (also for a consumer that never called prepare_kind).
    if (state != 0 && !arm_kind(in->kind)) return KILN_ERR_UNAVAILABLE;
    std::lock_guard lock(g_interest_mutex);
    g_interest[in->kind] = std::move(next);
    g_interest_state[in->kind].store(state);
    if (state != 0) g_interest_kinds.fetch_or(1ull << in->kind);
    else g_interest_kinds.fetch_and(~(1ull << in->kind));
    return KILN_OK;
}

KilnResult KILN_CALL d_enqueue_prepared(KilnOwner dispatcher, const KilnDecisionRequestV1* request, uint64_t* request_id) {
    if (!g_on.load()) return KILN_ERR_UNAVAILABLE;
    if (!request || !request_id) return KILN_ERR_INVALID_ARGUMENT;
    if (request->kind >= 64 || !(kPreparedKinds & (1ull << request->kind))) return KILN_ERR_INVALID_ARGUMENT;  // hooks file 1, 2, 32
    KilnWindowStateV1 s{};
    if (!world_safe(&s)) return KILN_ERR_WRONG_THREAD;
    if (!g_broker->is_consumer(dispatcher)) return KILN_ERR_UNAVAILABLE;
    return g_broker->file(*request, s.world_epoch, s.step, request_id);
}

// The consumer's batch calls: world thread, world-safe phase, the attached owner.
KilnResult consumer(KilnOwner dispatcher, KilnWindowStateV1* s) {
    if (!g_on.load()) return KILN_ERR_UNAVAILABLE;
    if (!world_safe(s)) return KILN_ERR_WRONG_THREAD;
    return g_broker->is_consumer(dispatcher) ? KILN_OK : KILN_ERR_UNAVAILABLE;
}

KilnResult KILN_CALL d_begin_requests(KilnOwner d, uint64_t* batch, uint32_t* records) {
    if (!batch || !records) return KILN_ERR_INVALID_ARGUMENT;
    KilnWindowStateV1 s{};
    if (KilnResult rc = consumer(d, &s); rc != KILN_OK) return rc;
    return g_broker->begin_requests(batch, records);
}
KilnResult KILN_CALL d_read_requests(KilnOwner d, uint64_t batch, KilnDecisionRequestV1* out, uint32_t capacity, uint32_t stride,
                                     uint32_t* required) {
    KilnWindowStateV1 s{};
    if (KilnResult rc = consumer(d, &s); rc != KILN_OK) return rc;
    return g_broker->read_requests(batch, out, capacity, stride, required);
}
KilnResult KILN_CALL d_ack_requests(KilnOwner d, uint64_t batch) {
    KilnWindowStateV1 s{};
    if (KilnResult rc = consumer(d, &s); rc != KILN_OK) return rc;
    return g_broker->ack_requests(batch, s.step);
}
KilnResult KILN_CALL d_answer(KilnOwner d, const KilnDecisionAnswerV1* answer, KilnDecisionReceiptV1* receipt) {
    if (!answer) return KILN_ERR_INVALID_ARGUMENT;
    KilnWindowStateV1 s{};
    if (KilnResult rc = consumer(d, &s); rc != KILN_OK) return rc;
    const KilnResult rc = g_broker->answer(*answer, receipt, s.step);
    g_broker->drain_sinks();
    return rc;
}
KilnResult KILN_CALL d_begin_outcomes(KilnOwner d, uint64_t* batch, uint32_t* records) {
    if (!batch || !records) return KILN_ERR_INVALID_ARGUMENT;
    KilnWindowStateV1 s{};
    if (KilnResult rc = consumer(d, &s); rc != KILN_OK) return rc;
    return g_broker->begin_outcomes(batch, records);
}
KilnResult KILN_CALL d_read_outcomes(KilnOwner d, uint64_t batch, KilnDecisionOutcomeV1* out, uint32_t capacity, uint32_t stride,
                                     uint32_t* required) {
    KilnWindowStateV1 s{};
    if (KilnResult rc = consumer(d, &s); rc != KILN_OK) return rc;
    return g_broker->read_outcomes(batch, out, capacity, stride, required);
}
KilnResult KILN_CALL d_ack_outcomes(KilnOwner d, uint64_t batch) {
    KilnWindowStateV1 s{};
    if (KilnResult rc = consumer(d, &s); rc != KILN_OK) return rc;
    return g_broker->ack_outcomes(batch, s.step);
}

// since 0.2.0-dev.29: a native Pre adapter's request (kind 18) and its sink
KilnResult KILN_CALL d_enqueue_native(KilnOwner producer, const KilnDecisionRequestV1* request, KilnDecisionSinkFn sink, void* user,
                                      uint64_t* request_id) {
    if (request_id) *request_id = 0;
    if (!g_on.load()) return KILN_ERR_UNAVAILABLE;
    if (!request || !request_id || !sink || producer == 0 || request->kind != KILN_DECISION_EQUIP_SET) return KILN_ERR_INVALID_ARGUMENT;
    KilnWindowStateV1 s{};
    if (!world_safe(&s)) return KILN_ERR_WRONG_THREAD;
    const KilnResult rc = g_broker->file_native(*request, s.world_epoch, s.step, sink, user, request_id);
    g_broker->drain_sinks();  // a QUEUE_DROPPED outcome is told before the call returns
    return rc;
}

KilnResult KILN_CALL d_reevaluate_pickup(uint32_t drop, uint32_t receiver, uint32_t* exposures_reset) {
    if (exposures_reset) *exposures_reset = 0;
    if (!g_on.load() || !pickup_producer::usable()) return KILN_ERR_UNAVAILABLE;
    if (!world_safe(nullptr)) return KILN_ERR_WRONG_THREAD;
    return pickup_producer::reevaluate(drop, receiver, exposures_reset);
}

// ---- step windows (core only) ----
void KILN_CALL d_window_begin(uint64_t epoch, uint64_t step) {
    if (!g_on.load(std::memory_order_relaxed)) return;
    g_epoch.store(epoch);
    g_step.store(step);
    g_world_thread.store(GetCurrentThreadId());
    // The metadata of this step: every entity an interest names, and what the producers discover.
    static std::vector<uint32_t> wanted;  // world thread only; kept to avoid an allocation per step
    wanted.clear();
    {
        std::lock_guard lock(g_interest_mutex);
        for (const auto& i : g_interest) wanted.insert(wanted.end(), i.ids.begin(), i.ids.end());
    }
    death_producer::discover(wanted);
    pickup_producer::discover(wanted);
    uint32_t misses = 0;
    const bool ok = g_metadata.rebuild(epoch, step, wanted, &misses);
    for (uint32_t i = 0; i < misses; ++i) g_broker->count_metadata_miss();
    if (!ok && !g_budget_exceeded) {
        logf(KILN_LOG_WARN, "decisions: more than %u entities to describe (MetadataBudgetExceeded); nothing is held until the interest shrinks",
             KILN_DECISIONS_MAX_METADATA);
    } else if (ok && g_budget_exceeded) {
        logf(KILN_LOG_INFO, "decisions: the metadata fits again; holding resumes");
    }
    g_budget_exceeded = !ok;
    g_broker->window_begin(epoch, step);
    death_producer::window_begin(epoch, step);
    pickup_producer::window_begin(epoch, step);
    g_broker->drain_sinks();
}

void KILN_CALL d_window_end(uint64_t epoch, uint64_t step) {
    if (!g_on.load(std::memory_order_relaxed)) return;
    pickup_producer::window_end(epoch, step);
    g_broker->window_end(epoch, step);
    g_broker->drain_sinks();
}

KilnResult KILN_CALL d_set_death_guard(KilnOwner owner, KilnDeathGuardFn guard) {
    if (!g_on.load()) return KILN_ERR_UNAVAILABLE;
    return death_producer::set_guard(owner, guard);
}

// since 0.2.0-dev.29: a kind's producer ready to hold now (its hook goes in); any thread
KilnResult KILN_CALL d_prepare_kind(uint32_t kind, char* reason, uint32_t reason_size) {
    if (reason && reason_size) reason[0] = 0;
    auto say = [&](const std::string& why) {
        if (reason && reason_size) std::snprintf(reason, reason_size, "%s", why.c_str());
    };
    if (kind == 0 || kind >= 64 || payload_size(kind) == 0) return KILN_ERR_INVALID_ARGUMENT;
    if (!g_on.load()) {
        say("kiln.decisions is off (see its capability row)");
        return KILN_ERR_UNAVAILABLE;
    }
    if (kPreparedKinds & (1ull << kind)) return KILN_OK;
    if (kind == KILN_DECISION_DEATH) {
        if (death_producer::arm()) return KILN_OK;
        say(death_producer::why_not());
        return KILN_ERR_UNAVAILABLE;
    }
    if (kind == KILN_DECISION_WALKOVER) {
        if (pickup_producer::arm()) return KILN_OK;
        say(pickup_producer::why_not());
        return KILN_ERR_UNAVAILABLE;
    }
    say("this build has no producer of that kind");
    return KILN_ERR_UNAVAILABLE;
}

// since 0.2.0-dev.29, core only: whether this step needs the window calls (and the wait for the world's jobs)
uint32_t KILN_CALL d_window_wanted() {
    if (!g_on.load(std::memory_order_relaxed)) return 0;
    if (g_interest_kinds.load(std::memory_order_relaxed) != 0) return 1;  // a listener: metadata, deadlines, samples
    if (death_producer::busy() || pickup_producer::busy()) return 1;      // a hold or an exposure to follow; a guard
    return g_broker->active_count() != 0 ? 1u : 0u;                       // an open request's deadline
}

const KilnDecisionsApiV1 kTable = {
    {sizeof(KilnDecisionsApiV1), KILN_DECISIONS_VERSION},
    &d_supported_kinds,
    &d_attach,
    &d_resolve_entity,
    &d_interest,
    &d_enqueue_prepared,
    &d_begin_requests,
    &d_read_requests,
    &d_ack_requests,
    &d_answer,
    &d_begin_outcomes,
    &d_read_outcomes,
    &d_ack_outcomes,
    &d_reevaluate_pickup,
    &d_detach,
    &d_window_begin,
    &d_window_end,
    &d_set_death_guard,
    &d_enqueue_native,  // since 0.2.0-dev.29
    &d_prepare_kind,    // since 0.2.0-dev.29
    &d_window_wanted,   // since 0.2.0-dev.29
    nullptr,            // window_faulted: since 0.2.0-dev.29
};

void KILN_CALL on_world(const KilnWorldEvent* e, void*) {
    if (e->kind != KILN_WORLD_SERVER || e->event != KILN_WORLD_STOPPING) return;
    KilnWindowStateV1 s{};
    g_clock->get(&s);
    g_broker->world_ended(s.step);
    g_broker->drain_sinks();
    g_metadata.clear();
    death_producer::world_stopped();
    pickup_producer::world_stopped();
    const Counters c = g_broker->counters();
    logf(KILN_LOG_INFO, "decisions: world stopped (filed %llu, dropped %llu, timeouts %llu, outcome gap %llu, metadata misses %llu)",
         static_cast<unsigned long long>(c.filed), static_cast<unsigned long long>(c.queue_dropped),
         static_cast<unsigned long long>(c.timeouts), static_cast<unsigned long long>(c.outcome_gap),
         static_cast<unsigned long long>(c.metadata_misses));
}

void off(const char* why) {
    logf(KILN_LOG_WARN, "decisions: unavailable (%s)", why);
    if (g_caps)
        g_caps->report(g_owner, ks(KILN_DECISIONS_SERVICE), KILN_CAP_UNAVAILABLE, KILN_CAP_REASON_SERVICE_MISSING, ks(why));
}

void KILN_CALL on_ready(void*) {
    g_caps = static_cast<const KilnCapsApi*>(g_api->service(ks(KILN_CAPS_SERVICE), KILN_CAPS_VERSION));
    g_clock = static_cast<const KilnWindowClockApiV1*>(g_api->service(ks(KILN_WINDOW_CLOCK_SERVICE), KILN_WINDOW_CLOCK_VERSION));
    g_world = static_cast<const KilnWorldApi*>(g_api->service(ks(KILN_WORLD_SERVICE), KILN_WORLD_VERSION));
    g_players = static_cast<const KilnPlayersApi*>(g_api->service(ks(KILN_PLAYERS_SERVICE), KILN_PLAYERS_VERSION));
    // Without WaitWorldJobs the core makes no window call: nothing here could look at the world safely.
    if (!g_api->feature(ks("tick.wait_jobs"))) return off("feature tick.wait_jobs is off: no step window without the world's jobs done");
    if (!g_clock) return off("kiln.window_clock is not there");
    if (!g_world) return off("kiln.world is not there");
    g_spawn_time_hash = g_world->component_hash(ks("SpawnTime"));
    {
        // The hooks tell the server world's program invocations from a hosting client's other worlds by these.
        int64_t world_storage = -1, storage_kind = -1;
        if (g_api->constant(ks("world.storage"), &world_storage) != KILN_OK) world_storage = -1;
        if (g_api->constant(ks("storage.kind"), &storage_kind) != KILN_OK) storage_kind = -1;
        g_gate.set_layout(world_storage, storage_kind);
    }
    g_metadata.set_sampler(&sample, nullptr);
    KilnHandle h = 0;
    if (g_api->subscribe_world(g_owner, &on_world, nullptr, &h) != KILN_OK) return off("no world events");
    g_on.store(true);
    logf(KILN_LOG_INFO, "decisions: on (requests %u, outcomes %u, metadata rows %u; players %s)", KILN_DECISIONS_MAX_ACTIVE,
         KILN_DECISIONS_MAX_OUTCOMES, KILN_DECISIONS_MAX_METADATA, g_players ? "from kiln.players" : "unknown: no kiln.players");
    // Producers: they resolve what their hooks need; nothing is hooked until a mod listens (since 0.2.0-dev.29). The
    // death producer decides decisions.native_guards (19d-pets' guard needs its hook, which then goes in with the
    // guard). A producer that cannot hold here lowers its row with the reason (a row the gate holds says so itself).
    death_producer::ready();
    pickup_producer::ready();
    if (g_caps) {
        if (death_producer::usable()) {
            g_caps->report(g_owner, ks("decisions.native_guards"), KILN_CAP_AVAILABLE, KILN_CAP_REASON_NONE,
                           ks("the death boundary's guard (set_death_guard); its hook goes in with the first guard or death listener"));
        } else {
            const std::string why = death_producer::why_not();
            g_caps->report(g_owner, ks("decisions.native_guards"), KILN_CAP_UNAVAILABLE, death_producer::why_not_reason(), ks(why.c_str()));
            if (death_producer::why_not_reason() != KILN_CAP_REASON_POLICY)
                g_caps->report(g_owner, ks("events.death"), KILN_CAP_UNAVAILABLE, death_producer::why_not_reason(), ks(why.c_str()));
        }
        if (!pickup_producer::usable() && pickup_producer::why_not_reason() != KILN_CAP_REASON_POLICY) {
            const std::string why = pickup_producer::why_not();
            g_caps->report(g_owner, ks("events.pickup"), KILN_CAP_UNAVAILABLE, pickup_producer::why_not_reason(), ks(why.c_str()));
        }
        // The rows of chapter 07 that this build does not have (0.2.0-dev.29), so none of them reads on: player build
        // pieces wait for their replay gate (07 S0/S9, after 06a's program wraps), managed terraforming is out of V1
        // (owner decision 10), the component watches (07 S7, kiln.component_changes) are not built yet, and an
        // every-store feed is not promised at all. events.component_net is also a sigdb feature whose symbols resolve:
        // lowered here, never raised.
        g_caps->report(g_owner, ks("events.build.pieces"), KILN_CAP_UNAVAILABLE, KILN_CAP_REASON_SERVICE_MISSING,
                       ks("player build pieces are not cancelable in this build (their replay gate has not passed)"));
        g_caps->report(g_owner, ks("events.build.terraform"), KILN_CAP_UNAVAILABLE, KILN_CAP_REASON_SERVICE_MISSING,
                       ks("not implemented: managed terraform cancel is not part of V1 (Gameplay.Protect's native policy still applies)"));
        g_caps->report(g_owner, ks("events.component_net"), KILN_CAP_UNAVAILABLE, KILN_CAP_REASON_SERVICE_MISSING,
                       ks("kiln.component_changes (named component watches) is not in this build"));
        g_caps->report(g_owner, ks("events.component_writes.exact"), KILN_CAP_UNAVAILABLE, KILN_CAP_REASON_SERVICE_MISSING,
                       ks("reserved: Kiln promises no feed of every component store"));
    }
}

}  // namespace

namespace host {
const KilnApi* api() { return g_api; }
KilnOwner owner() { return g_owner; }
const KilnWorldApi* world() { return g_world; }
const KilnCapsApi* caps() { return g_caps; }
Broker& broker() { return *g_broker; }
bool in_server_world(const void* ctx) { return g_gate.owns(ctx, g_api->server_world()); }
bool world_layout_known() { return g_gate.layout_known(); }
uint64_t epoch() { return g_epoch.load(std::memory_order_relaxed); }
uint64_t step() { return g_step.load(std::memory_order_relaxed); }
uint32_t world_thread() { return g_world_thread.load(std::memory_order_relaxed); }
uint32_t interest_state(uint32_t kind) { return kind < 64 ? g_interest_state[kind].load(std::memory_order_relaxed) : 0; }
bool interested(uint32_t kind, uint32_t entity) {
    const uint32_t state = interest_state(kind);
    if (state != 1) return state == 2;
    std::lock_guard lock(g_interest_mutex);
    const auto& ids = g_interest[kind].ids;
    return std::binary_search(ids.begin(), ids.end(), entity);
}
bool holding() { return !g_budget_exceeded.load(std::memory_order_relaxed) && !g_broker->saturated(); }
std::shared_ptr<const Table> table() { return g_metadata.table(); }
bool resolve(uint32_t entity, Row* out) {
    KilnWindowStateV1 s{};
    if (!world_safe(&s)) return false;
    bool miss = false;
    if (g_metadata.resolve(s.world_epoch, s.step, entity, out, &miss)) return true;
    if (miss) g_broker->count_metadata_miss();
    return false;
}
void hook_kind(uint32_t kind) {
    if (kind < 64) g_hook_kinds.fetch_or(1ull << kind);
}
void hook_kind_off(uint32_t kind) {
    if (kind < 64) g_hook_kinds.fetch_and(~(1ull << kind));
}
bool holding_allowed() { return true; }
void lower(const char* row, uint32_t reason, const char* why) {
    if (g_caps) g_caps->report(g_owner, ks(row), KILN_CAP_UNAVAILABLE, reason, ks(why));
}
void logf(int level, const char* fmt, ...) {
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    const int n = std::vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    if (n > 0) g_api->log(g_owner, level, KilnStr{b, static_cast<size_t>(n < static_cast<int>(sizeof b) ? n : sizeof b - 1)});
}
}  // namespace host
}  // namespace kiln::decisions

extern "C" KILN_EXPORT KilnResult KILN_CALL kiln_plugin_main(const KilnApi* api, KilnOwner owner, KilnPluginInfo* info) {
    using namespace kiln::decisions;
    if (api->abi_version != KILN_ABI_VERSION || api->size < sizeof(KilnApi)) return KILN_ERR_UNAVAILABLE;
    g_api = api;
    g_owner = owner;
    info->id = KilnStr{KILN_DECISIONS_SERVICE, sizeof(KILN_DECISIONS_SERVICE) - 1};
    info->version = KilnStr{"1", 1};
    info->on_ready = &on_ready;
    g_broker = new Broker();
    return api->register_service(owner, KilnStr{KILN_DECISIONS_SERVICE, sizeof(KILN_DECISIONS_SERVICE) - 1}, &kTable);
}
