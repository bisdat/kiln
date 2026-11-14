// SPDX-License-Identifier: MIT
// kiln.decisions v1 (abi/services/kiln_decisions.h): the service around the broker (queue.h) and the metadata table
// (metadata.h). This file holds what touches the process: the table's thread and phase checks, the world reads of
// the metadata sampler, the world events. Producers (death, pickup, ...) are files of their own beside it.
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
#include <vector>

#include "kiln_api.h"
#include "metadata.h"
#include "queue.h"
#include "services/kiln_caps.h"
#include "services/kiln_decisions.h"
#include "services/kiln_players.h"
#include "services/kiln_window_clock.h"
#include "services/kiln_world.h"

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

// The consumer's interest per kind: all entities, or a list.
struct Interest {
    bool all = false;
    std::vector<uint32_t> ids;  // sorted
};
std::mutex g_interest_mutex;
Interest g_interest[64];
bool g_budget_exceeded = false;  // world thread only

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
    if (rc == KILN_OK) {
        std::lock_guard lock(g_interest_mutex);
        for (auto& i : g_interest) i = Interest{};
    }
    return rc;
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
    std::lock_guard lock(g_interest_mutex);
    g_interest[in->kind] = std::move(next);
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
    return g_broker->answer(*answer, receipt, s.step);
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

KilnResult KILN_CALL d_reevaluate_pickup(uint32_t, uint32_t, uint32_t* exposures_reset) {
    if (exposures_reset) *exposures_reset = 0;
    return KILN_ERR_UNAVAILABLE;  // the walk-over producer is not part of this build yet
}

// ---- step windows (core only) ----
void KILN_CALL d_window_begin(uint64_t epoch, uint64_t step) {
    if (!g_on.load(std::memory_order_relaxed)) return;
    // The metadata of this step: every entity an interest names. Producers add their discovery here.
    static std::vector<uint32_t> wanted;  // world thread only; kept to avoid an allocation per step
    wanted.clear();
    {
        std::lock_guard lock(g_interest_mutex);
        for (const auto& i : g_interest) wanted.insert(wanted.end(), i.ids.begin(), i.ids.end());
    }
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
}

void KILN_CALL d_window_end(uint64_t epoch, uint64_t step) {
    if (!g_on.load(std::memory_order_relaxed)) return;
    g_broker->window_end(epoch, step);
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
};

void KILN_CALL on_world(const KilnWorldEvent* e, void*) {
    if (e->kind != KILN_WORLD_SERVER || e->event != KILN_WORLD_STOPPING) return;
    KilnWindowStateV1 s{};
    g_clock->get(&s);
    g_broker->world_ended(s.step);
    g_metadata.clear();
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
    g_metadata.set_sampler(&sample, nullptr);
    KilnHandle h = 0;
    if (g_api->subscribe_world(g_owner, &on_world, nullptr, &h) != KILN_OK) return off("no world events");
    g_on.store(true);
    logf(KILN_LOG_INFO, "decisions: on (requests %u, outcomes %u, metadata rows %u; players %s)", KILN_DECISIONS_MAX_ACTIVE,
         KILN_DECISIONS_MAX_OUTCOMES, KILN_DECISIONS_MAX_METADATA, g_players ? "from kiln.players" : "unknown: no kiln.players");
}

}  // namespace
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
