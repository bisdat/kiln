// SPDX-License-Identifier: MIT
// kiln.decisions, the walk-over pickup producer (kind 2): the entry hook at events/PickupInRange (the helper the
// game's walk-over program loot_pickup_in_range calls), the exposure samples at both window calls, the discovery of
// drops and receivers, and ReevaluatePickup. The rules are in pickup_core.h; this file gives them the game. The
// interaction helper (events/PickupToInventory) is not touched: interaction loot is LootEvent's.
//
// Client gate: events.pickup is held until its guided check (native/core/gates/dev28-07-cancelable-events.json); the
// hook is installed only when kiln_caps_held says the row is not held. A lab proves it with "preview" in kiln.json.
//
// Engine facts (chapter 07 §2): Result8* pickup(Result8* out, ProgramContext* ctx, const PickupSlice* source,
// const Accessors* other, const Singletons*, const EventSenders*, uint32 receiver, uint8 feedback); out, ctx, source
// in RCX, RDX, R8; receiver at entry [RSP+0x38], feedback at [RSP+0x40]. source+8 is the drop's PickupItem (item +0,
// raw amount u16 +0x1a). Result8 is {uint8 error; pad[3]; uint32 zero}: a refusal writes those 8 bytes, never 12.
// CHUNKED on a dedicated server (the world thread), but nothing here assumes it.
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <string>

#include "host.h"
#include "kiln_hook.hpp"
#include "pickup_core.h"
#include "services/kiln_players.h"

namespace kiln::decisions::pickup_producer {
namespace {

using CurrentEntityFn = uint32_t* (*)(void* ctx, uint32_t* out);

constexpr uint8_t kNothingAdded = 0x15;     // the helper's own "nothing added" result
constexpr uintptr_t kReceiverArg = 0x38;    // [rsp+0x38] at entry: argument 7
constexpr uintptr_t kFeedbackArg = 0x40;    // [rsp+0x40] at entry: argument 8
constexpr uintptr_t kSourcePickupItem = 8;  // source slice: DynamicIsPlayerInRange* +0, PickupItem* +8
constexpr uint32_t kItemAmount = 0x1a;      // PickupItem: item +0, raw amount u16 +0x1a (28 bytes)
constexpr uint32_t kRangeReceiver = 4;      // DynamicIsPlayerInRange (16 bytes): receiver +4

CurrentEntityFn g_current = nullptr;
uint32_t g_zone_hash = 0, g_range_hash = 0, g_item_hash = 0;
const KilnPlayersApi* g_players = nullptr;

kiln::MidHook2 g_hook;
pickup::Exposures g_exposures;
std::atomic<bool> g_armed{false};
std::string g_why = "the pickup producer did not start";
uint32_t g_why_reason = KILN_CAP_REASON_SERVICE_MISSING;
std::atomic<uint32_t> g_logged{0};
std::atomic<uint64_t> g_off_world{0};

KilnStr ks(const char* s) { return KilnStr{s, std::strlen(s)}; }

struct HookPorts {
    KilnRegisters& r;
    uint32_t e = UINT32_MAX;

    bool armed() const { return g_armed.load(std::memory_order_relaxed); }
    uint32_t receiver() const { return *reinterpret_cast<const uint32_t*>(r.rsp + kReceiverArg); }
    bool any_interest() const { return host::interest_state(KILN_DECISION_WALKOVER) != 0; }
    uint32_t entity() {
        if (e == UINT32_MAX) {
            uint32_t out = 0;
            const uint32_t* p = g_current(reinterpret_cast<void*>(r.rdx), &out);
            e = p ? *p : 0;
        }
        return e;
    }
    bool interested(uint32_t entity) const { return host::interested(KILN_DECISION_WALKOVER, entity); }
    bool item(uint32_t* item, uint32_t* amount) const {
        const auto* source = reinterpret_cast<const uint8_t*>(r.r8);
        if (!source) return false;
        const auto* pi = *reinterpret_cast<const uint8_t* const*>(source + kSourcePickupItem);
        if (!pi) return false;
        *item = *reinterpret_cast<const uint32_t*>(pi);
        *amount = *reinterpret_cast<const uint16_t*>(pi + kItemAmount);
        return true;
    }
    uint32_t feedback() const { return *reinterpret_cast<const uint8_t*>(r.rsp + kFeedbackArg); }
    uint64_t epoch() const { return host::epoch(); }
    uint64_t step() const { return host::step(); }
    bool holding() const { return host::holding(); }
    bool row(uint32_t entity, uint64_t epoch, Row* out) const {
        const auto t = host::table();
        if (t->world_epoch != epoch) return false;
        const Row* found = t->find(entity);
        if (!found) return false;
        *out = *found;
        return true;
    }
};

void pickup_mid(KilnRegisters& r) {
    HookPorts p{r};
    const uint64_t filed = g_exposures.stats.filed.load(std::memory_order_relaxed);
    if (g_exposures.visit(p, host::broker()) != pickup::Visit::Refuse) return;
    // The helper's own ordinary "nothing added": 8 bytes, the rest of the caller's buffer untouched; return to the
    // caller with out in RAX, as the helper does.
    auto* out = reinterpret_cast<uint8_t*>(r.rcx);
    std::memset(out, 0, 8);
    out[0] = kNothingAdded;
    r.rax = r.rcx;
    r.trampoline_rsp = r.rsp;
    if (g_exposures.stats.filed.load(std::memory_order_relaxed) != filed) {
        const unsigned long tid = GetCurrentThreadId();
        const bool worker = tid != host::world_thread();
        if (worker) g_off_world.fetch_add(1, std::memory_order_relaxed);
        const uint32_t n = g_logged.fetch_add(1, std::memory_order_relaxed);
        if (n < 32 || n % 1000 == 0)
            host::logf(KILN_LOG_INFO, "decisions: walk-over pickup of drop %u by %u held (step %llu, thread %lu, %s)", p.entity(), p.receiver(),
                       static_cast<unsigned long long>(host::step()), tid, worker ? "a worker thread" : "the world thread");
    }
}

// The window samples: the drop's DynamicIsPlayerInRange and PickupItem, read where no world job is live.
struct SamplePorts {
    bool read(uint32_t drop, uint32_t* receiver, uint32_t* item, uint32_t* amount) const {
        const KilnWorldApi* w = host::world();
        uint32_t size = 0;
        const auto* range = static_cast<const uint8_t*>(w->component(drop, g_range_hash, &size));
        if (!range || size < kRangeReceiver + 4) return false;
        *receiver = *reinterpret_cast<const uint32_t*>(range + kRangeReceiver);
        const auto* pi = static_cast<const uint8_t*>(w->component(drop, g_item_hash, &size));
        if (!pi || size < kItemAmount + 2) return false;
        *item = *reinterpret_cast<const uint32_t*>(pi);
        *amount = *reinterpret_cast<const uint16_t*>(pi + kItemAmount);
        return true;
    }
    bool same_life(const KilnEventEntityKey& key) const {
        Row row;
        return host::resolve(key.entity, &row) && row.key.life_serial == key.life_serial;
    }
    bool interested(uint32_t entity) const { return host::interested(KILN_DECISION_WALKOVER, entity); }
};

void not_in(const char* why, uint32_t reason, int level) {
    g_why = why;
    g_why_reason = reason;
    host::logf(level, "decisions: walk-over pickups are not held: %s", why);
}

}  // namespace

void ready() {
    const KilnApi* api = host::api();
    uintptr_t helper = 0, current = 0;
    if (kiln_caps_held(host::caps(), ks("events.pickup")))
        return not_in("events.pickup is held until its game test (07-events.decision-rollback); \"preview\" in kiln.json lifts it",
                      KILN_CAP_REASON_POLICY, KILN_LOG_INFO);
    if (!api->feature(ks("events.pickup"))) return not_in("feature events.pickup did not resolve on this game build", KILN_CAP_REASON_SYMBOL_MISSING, KILN_LOG_WARN);
    if (api->symbol(ks("events/PickupInRange"), &helper) != KILN_OK || api->symbol(ks("world/CurrentEntityId"), &current) != KILN_OK || !helper ||
        !current)
        return not_in("a symbol of the pickup hook did not resolve (events/PickupInRange, world/CurrentEntityId)", KILN_CAP_REASON_SYMBOL_MISSING,
                      KILN_LOG_WARN);
    const KilnWorldApi* world = host::world();
    g_zone_hash = world->component_hash(ks("PickupItemZone"));
    g_range_hash = world->component_hash(ks("DynamicIsPlayerInRange"));
    g_item_hash = world->component_hash(ks("PickupItem"));
    g_players = static_cast<const KilnPlayersApi*>(api->service(ks(KILN_PLAYERS_SERVICE), KILN_PLAYERS_VERSION));
    g_current = reinterpret_cast<CurrentEntityFn>(current);
    if (!g_hook.install(api, host::owner(), helper, &pickup_mid, "kiln.decisions/walk-over-pickup", 100))
        return not_in(g_hook.error() ? g_hook.error() : "the pickup hook could not be installed", KILN_CAP_REASON_FAULTED, KILN_LOG_WARN);
    g_why.clear();
    g_why_reason = KILN_CAP_REASON_NONE;
    g_armed.store(true);
    host::hook_kind(KILN_DECISION_WALKOVER);
    host::logf(KILN_LOG_INFO, "decisions: walk-over pickups can be held (entry hook at events/PickupInRange; the interaction helper is untouched)");
}

bool installed() { return g_armed.load(); }
const char* why_not() { return g_why.c_str(); }
uint32_t why_not_reason() { return g_why_reason; }

void discover(std::vector<uint32_t>& wanted) {
    if (!g_armed.load(std::memory_order_relaxed) || host::interest_state(KILN_DECISION_WALKOVER) == 0) return;
    // Drops: every entity with PickupItemZone (the walk-over program's own set). Receivers: the live players; a
    // spawned character as receiver is named by the interest itself (decisions.cpp adds those ids).
    static std::vector<KilnEntity> found;  // world thread only
    const KilnWorldApi* world = host::world();
    uint32_t count = 0;
    if (found.empty()) found.resize(1024);
    KilnResult rc = world->find(g_zone_hash, found.data(), static_cast<uint32_t>(found.size()), &count);
    if (rc == KILN_ERR_BUFFER_TOO_SMALL && count <= 4 * KILN_DECISIONS_MAX_METADATA) {
        found.resize(count + 256);
        rc = world->find(g_zone_hash, found.data(), static_cast<uint32_t>(found.size()), &count);
    }
    if (rc == KILN_ERR_BUFFER_TOO_SMALL) {
        wanted.insert(wanted.end(), found.begin(), found.end());
        for (uint32_t i = 0; wanted.size() <= KILN_DECISIONS_MAX_METADATA; ++i) wanted.push_back(0x7ffffe00u + i);  // over budget: say so
        return;
    }
    if (rc == KILN_OK) wanted.insert(wanted.end(), found.begin(), found.begin() + std::min<size_t>(count, found.size()));
    if (g_players && g_players->list) {
        KilnPlayer players[32];
        uint32_t n = 0;
        for (auto& pl : players) pl.size = sizeof pl;
        if (g_players->list(0, players, 32, &n) == KILN_OK || n > 0)
            for (uint32_t i = 0; i < std::min<uint32_t>(n, 32); ++i)
                if (players[i].state == KILN_PLAYER_IN_GAME && players[i].entity) wanted.push_back(players[i].entity);
    }
}

void window_begin(uint64_t epoch, uint64_t step) {
    if (!g_armed.load(std::memory_order_relaxed)) return;
    SamplePorts s;
    g_exposures.sample(epoch, step, host::broker(), s, true);
}

void window_end(uint64_t epoch, uint64_t step) {
    if (!g_armed.load(std::memory_order_relaxed)) return;
    SamplePorts s;
    g_exposures.sample(epoch, step, host::broker(), s, false);
}

void world_stopped() {
    g_exposures.clear();
    const auto& s = g_exposures.stats;
    if (g_armed.load())
        host::logf(KILN_LOG_INFO,
                   "decisions: walk-over pickups this world: filed %llu (%llu on a worker thread), refused visits %llu, permitted %llu, released %llu, "
                   "exposures closed %llu, reopened %llu, metadata misses %llu, dropped %llu",
                   static_cast<unsigned long long>(s.filed.load()), static_cast<unsigned long long>(g_off_world.load()),
                   static_cast<unsigned long long>(s.refused.load()), static_cast<unsigned long long>(s.permitted.load()),
                   static_cast<unsigned long long>(s.released.load()), static_cast<unsigned long long>(s.closed.load()),
                   static_cast<unsigned long long>(s.reopened.load()), static_cast<unsigned long long>(s.misses.load()),
                   static_cast<unsigned long long>(s.dropped.load()));
}

KilnResult reevaluate(uint32_t drop, uint32_t receiver, uint32_t* reset) {
    if (reset) *reset = 0;
    if (!g_armed.load()) return KILN_ERR_UNAVAILABLE;
    const uint32_t n = g_exposures.reopen(drop, receiver, host::broker());
    if (reset) *reset = n;
    return KILN_OK;
}

}  // namespace kiln::decisions::pickup_producer
