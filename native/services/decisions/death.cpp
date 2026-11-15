// SPDX-License-Identifier: MIT
// kiln.decisions, the death producer (kind 1): the mid-hook at events/DeathBoundary inside combat/DeathProgram, its
// skip to events/DeathNextEntity, the answer through kiln.combat's health setter, and the native guard seam of
// 19d-pets. The rules are in death_core.h; this file gives them the game.
//
// events.death is on since 0.2.0-dev.29 (nothing ships held) and not yet confirmed in a real game (guided check
// 07-events.decision-rollback). The core's gate list has no entry for it; should the owner ever hold the row again,
// kiln_caps_held keeps the hook out.
//
// Engine facts (chapter 07 §2, VERIFY.md §B): at the boundary EAX is the entity's current health (signed), RBX the
// Health component, RSI the program context, [RSP+0x48] the Actor component of the declared slot. The program runs
// on workers when the world schedules entity programs in parallel: nothing here reads the world beyond those. A
// hosting client runs the death program in its client and prediction worlds as well: a visit whose context is not
// the server world's (world_gate.h) passes before the guard and anything else is asked.
#include <windows.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <string>

#include "death_core.h"
#include "host.h"
#include "kiln_hook.hpp"
#include "producer_arm.h"
#include "services/kiln_combat.h"

namespace kiln::decisions::death_producer {
namespace {

using CurrentEntityFn = uint32_t* (*)(void* ctx, uint32_t* out);
using StateTestFn = bool (*)(const void* actor, uint8_t state);

constexpr uint8_t kStateDead = 7, kState12 = 12;
constexpr uintptr_t kActorSlot = 0x48;  // [rsp+0x48] at the boundary: the Actor of the declared slot

CurrentEntityFn g_current = nullptr;
StateTestFn g_state_test = nullptr;
uintptr_t g_next = 0;      // events/DeathNextEntity
uintptr_t g_boundary = 0;  // events/DeathBoundary: hooked at the first listener (ProducerArm)
const KilnCombatApi* g_combat = nullptr;
uint32_t g_health_hash = 0, g_actor_hash = 0;

kiln::MidHook2 g_hook;
death::Holds g_holds;
ProducerArm g_arm;  // Ready at the start, Armed once a mod listens (or a native guard is set)
std::atomic<uint32_t> g_logged{0};
std::atomic<uint64_t> g_off_world{0};  // holds filed off the world thread (a worker running the death program)

bool tid_off_world() { return GetCurrentThreadId() != host::world_thread(); }

std::mutex g_guard_mutex;
std::atomic<KilnDeathGuardFn> g_guard{nullptr};
KilnOwner g_guard_owner = 0;

KilnStr ks(const char* s) { return KilnStr{s, std::strlen(s)}; }

bool actor_blocked(const void* actor) {
    if (!actor) return true;  // no Actor: nothing this producer can tell apart from a corpse
    return g_state_test(actor, kState12) || g_state_test(actor, kStateDead);
}

// ---- the hook ----
struct HookPorts {
    KilnRegisters& r;
    uint32_t e = UINT32_MAX;

    int32_t health() const { return static_cast<int32_t>(static_cast<uint32_t>(r.rax)); }
    bool armed() const { return g_arm.armed() && host::holding_allowed(); }
    bool server_world() const { return host::in_server_world(reinterpret_cast<const void*>(r.rsi)); }  // RSI: the context
    uint32_t entity() {
        if (e == UINT32_MAX) {
            uint32_t out = 0;
            const uint32_t* p = g_current(reinterpret_cast<void*>(r.rsi), &out);
            e = p ? *p : 0;
        }
        return e;
    }
    uint64_t epoch() const { return host::epoch(); }
    uint64_t step() const { return host::step(); }
    uint32_t guard(uint64_t epoch, uint32_t entity, int32_t hp) const {
        const KilnDeathGuardFn g = g_guard.load(std::memory_order_acquire);
        return g ? g(epoch, entity, hp) : KILN_DEATH_GUARD_PASS;
    }
    bool any_interest() const { return host::interest_state(KILN_DECISION_DEATH) != 0; }
    bool interested(uint32_t entity) const { return host::interested(KILN_DECISION_DEATH, entity); }
    bool actor_blocked() const { return death_producer::actor_blocked(*reinterpret_cast<void* const*>(r.rsp + kActorSlot)); }
    bool holding() const { return host::holding(); }
    bool row(uint32_t entity, uint64_t epoch, Row* out) const {
        const auto t = host::table();
        if (t->world_epoch != epoch) return false;
        const Row* found = t->find(entity);
        if (!found) return false;
        *out = *found;
        return true;
    }
    void filed(uint32_t entity, uint64_t id) const {
        const uint32_t n = g_logged.fetch_add(1, std::memory_order_relaxed);
        if (tid_off_world()) g_off_world.fetch_add(1, std::memory_order_relaxed);
        if (n < 32 || n % 1000 == 0) {
            const unsigned long tid = GetCurrentThreadId();
            host::logf(KILN_LOG_INFO, "decisions: death of entity %u held (request %llu, health %d, step %llu, thread %lu, %s)", entity,
                       static_cast<unsigned long long>(id), health(), static_cast<unsigned long long>(host::step()), tid,
                       tid == host::world_thread() ? "the world thread" : "a worker thread");
        }
    }
};

void death_body(KilnRegisters& r) {
    HookPorts p{r};
    if (g_holds.visit(p, host::broker()) == death::Visit::Skip) r.rip = g_next;  // the only register it changes, last
}

void death_mid(KilnRegisters& r) {
    if (static_cast<int32_t>(static_cast<uint32_t>(r.rax)) > 0) return;  // every living entity: one compare
    unsigned long code = 0;
    if (!seh::call(&death_body, r, &code)) host::hook_faulted("the death hook", code);  // the game goes on: Pass
}

// ---- the answer (Pre, tick thread, inside the broker's answer) ----
struct FinishPorts {
    bool same_life(const KilnEventEntityKey& key) const {
        Row row;
        return host::resolve(key.entity, &row) && row.key.life_serial == key.life_serial;
    }
    bool live(uint32_t entity, death::Live* out) const {
        KilnHealth h{};
        h.size = sizeof h;
        if (g_combat->health(entity, &h) != KILN_OK) return false;
        out->current = h.current;
        out->max = h.max;
        uint32_t size = 0;
        const void* actor = host::world()->component(entity, g_actor_hash, &size);
        out->actor_dead = actor && actor_blocked(actor);
        return true;
    }
    KilnResult set_health(uint32_t entity, int32_t value, int32_t* applied) const { return g_combat->set_health(entity, value, applied); }
};

uint32_t finish(Finish& f, void*) {
    FinishPorts p;
    return death::finish(f, p);
}

void not_in(const char* why, uint32_t reason, int level) {
    g_arm.refuse(why, reason);
    host::logf(level, "decisions: deaths are not held: %s", why);
}

}  // namespace

void ready() {
    const KilnApi* api = host::api();
    uintptr_t boundary = 0, next = 0, current = 0, state = 0;
    // A hold the owner put on the row (none since 0.2.0-dev.29): asked before anything is installed.
    if (kiln_caps_held(host::caps(), ks("events.death")))
        return not_in("events.death is held by the core's gate list (\"preview\" in kiln.json lifts it)", KILN_CAP_REASON_POLICY,
                      KILN_LOG_INFO);
    if (!api->feature(ks("events.death"))) return not_in("feature events.death did not resolve on this game build", KILN_CAP_REASON_SYMBOL_MISSING, KILN_LOG_WARN);
    if (!host::world_layout_known())
        return not_in("the world layout constants world.storage and storage.kind did not resolve: the hook could not tell the server world "
                      "from a hosting client's client and prediction worlds",
                      KILN_CAP_REASON_SYMBOL_MISSING, KILN_LOG_WARN);
    if (api->symbol(ks("events/DeathBoundary"), &boundary) != KILN_OK || api->symbol(ks("events/DeathNextEntity"), &next) != KILN_OK ||
        api->symbol(ks("world/CurrentEntityId"), &current) != KILN_OK || api->symbol(ks("combat/ActorStateTest"), &state) != KILN_OK ||
        !boundary || !next || !current || !state)
        return not_in("a symbol of the death hook did not resolve (events/DeathBoundary, events/DeathNextEntity, world/CurrentEntityId, "
                      "combat/ActorStateTest)",
                      KILN_CAP_REASON_SYMBOL_MISSING, KILN_LOG_WARN);
    const auto* combat = static_cast<const KilnCombatApi*>(api->service(ks(KILN_COMBAT_SERVICE), 1));
    if (!combat || combat->header.size < offsetof(KilnCombatApi, set_health) + sizeof(void*) || !combat->health || !combat->set_health)
        return not_in("kiln.combat is not here: a canceled death could not keep its health", KILN_CAP_REASON_SERVICE_MISSING, KILN_LOG_WARN);
    const KilnWorldApi* world = host::world();
    g_health_hash = world->component_hash(ks("Health"));
    g_actor_hash = world->component_hash(ks("Actor"));
    g_current = reinterpret_cast<CurrentEntityFn>(current);
    g_state_test = reinterpret_cast<StateTestFn>(state);
    g_next = next;
    g_combat = combat;
    host::broker().set_finisher(KILN_DECISION_DEATH, &finish, nullptr);
    g_boundary = boundary;
    g_arm.ready();
    host::hook_kind(KILN_DECISION_DEATH);
    host::logf(KILN_LOG_INFO, "decisions: deaths can be held; the hook at events/DeathBoundary goes in when a mod listens for a death (or a "
               "native guard is set), nothing is hooked before");
}

bool arm() {
    if (g_arm.armed()) return true;
    const bool ok = g_arm.arm([](std::string& error) {
        if (g_hook.install(host::api(), host::owner(), g_boundary, &death_mid, "kiln.decisions/death-boundary", 100)) return true;
        error = g_hook.error() ? g_hook.error() : "the death hook could not be installed";
        return false;
    });
    static std::atomic<bool> told{false};
    if (told.exchange(true)) return ok;
    if (ok) {
        host::logf(KILN_LOG_INFO, "decisions: deaths are held from now on (hook at events/DeathBoundary, skip to events/DeathNextEntity)");
    } else if (g_arm.state() == ProducerArm::State::Refused && g_arm.reason() == KILN_CAP_REASON_FAULTED) {
        const std::string why = g_arm.why();
        host::logf(KILN_LOG_WARN, "decisions: deaths are not held: %s", why.c_str());
        host::hook_kind_off(KILN_DECISION_DEATH);
        host::lower("events.death", KILN_CAP_REASON_FAULTED, why.c_str());
        host::lower("decisions.native_guards", KILN_CAP_REASON_FAULTED, why.c_str());
    } else {
        told.store(false);  // not ready (refused at the start): its rows already say why
    }
    return ok;
}

bool installed() { return g_arm.armed(); }
bool usable() { return g_arm.usable(); }
std::string why_not() { return g_arm.why(); }
uint32_t why_not_reason() { return g_arm.reason(); }
bool busy() { return g_holds.size() != 0 || g_guard.load(std::memory_order_relaxed) != nullptr; }
bool try_clear() { return g_holds.try_clear(); }
void clear() { g_holds.clear(); }

void discover(std::vector<uint32_t>& wanted) {
    if (!g_arm.armed() || host::interest_state(KILN_DECISION_DEATH) != 2) return;
    // An unfiltered listener: every entity with Health needs a row (one pass over the entity map; the metadata budget
    // of 8192 rows turns the producer off rather than covering part of the world).
    static std::vector<KilnEntity> found;  // world thread only
    const KilnWorldApi* world = host::world();
    uint32_t count = 0;
    if (found.empty()) found.resize(4096);
    KilnResult rc = world->find(g_health_hash, found.data(), static_cast<uint32_t>(found.size()), &count);
    if (rc == KILN_ERR_BUFFER_TOO_SMALL && count <= 4 * KILN_DECISIONS_MAX_METADATA) {
        found.resize(count + 256);
        rc = world->find(g_health_hash, found.data(), static_cast<uint32_t>(found.size()), &count);
    }
    if (rc == KILN_ERR_BUFFER_TOO_SMALL) {
        // more than the metadata can ever hold: hand over too many ids, so the rebuild says MetadataBudgetExceeded
        wanted.insert(wanted.end(), found.begin(), found.end());
        for (uint32_t i = 0; wanted.size() <= KILN_DECISIONS_MAX_METADATA; ++i) wanted.push_back(0x7fffff00u + i);
        return;
    }
    if (rc != KILN_OK) return;
    wanted.insert(wanted.end(), found.begin(), found.begin() + std::min<size_t>(count, found.size()));
}

void window_begin(uint64_t epoch, uint64_t step) {
    if (!g_arm.armed()) return;
    g_holds.housekeeping(epoch, step, host::broker(), [](uint32_t e) { return host::interested(KILN_DECISION_DEATH, e); });
}

void world_stopped() {
    if (!g_holds.try_clear()) return;  // a fault left its lock held (the service is shut down): nothing more here
    const auto& s = g_holds.stats;
    if (g_arm.armed())
        host::logf(KILN_LOG_INFO, "decisions: deaths this world: filed %llu (%llu on a worker thread), held visits %llu, released %llu, guarded %llu, metadata misses %llu, dropped %llu, other worlds' visits passed %llu",
                   static_cast<unsigned long long>(s.filed.load()), static_cast<unsigned long long>(g_off_world.load()), static_cast<unsigned long long>(s.held.load()),
                   static_cast<unsigned long long>(s.released.load()), static_cast<unsigned long long>(s.guarded.load()),
                   static_cast<unsigned long long>(s.misses.load()), static_cast<unsigned long long>(s.dropped.load()),
                   static_cast<unsigned long long>(s.foreign.load()));
}

KilnResult set_guard(KilnOwner owner, KilnDeathGuardFn guard) {
    if (owner == 0) return KILN_ERR_INVALID_ARGUMENT;
    std::lock_guard lock(g_guard_mutex);
    if (guard == nullptr) {
        if (g_guard_owner != owner) return KILN_ERR_NOT_FOUND;
        g_guard.store(nullptr, std::memory_order_release);
        g_guard_owner = 0;
        return KILN_OK;
    }
    if (g_guard_owner != 0 && g_guard_owner != owner) return KILN_ERR_UNAVAILABLE;
    if (!arm()) return KILN_ERR_UNAVAILABLE;  // a guard needs the hook: it goes in with the first guard
    g_guard_owner = owner;
    g_guard.store(guard, std::memory_order_release);
    host::logf(KILN_LOG_INFO, "decisions: a native death guard is set (owner %llu)", static_cast<unsigned long long>(owner));
    return KILN_OK;
}

}  // namespace kiln::decisions::death_producer
