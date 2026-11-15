// SPDX-License-Identifier: MIT
// kiln.component_changes v1 (abi/services/kiln_component_changes.h, chapter 07 §4.6): named component watches. The
// rules (budgets, baselines, tokens, origin, queue) are watch_core.h; this file is what touches the process: the
// table's thread and phase checks, the world reads (kiln.world's GetComponent, kiln.players, kiln.decisions' metadata
// for the roots' lives, kiln.combat's and kiln.items' typed attribute values), the world's end, the rows.
//
// It needs no game hook of its own: it reads components through kiln.world at the two window calls, when no world job
// is live, and Kiln's own writers tell it what they wrote (record_write). Nothing runs while nobody watches: the core
// makes no window call (window_wanted is 0), record_write returns at its first check, nothing is allocated.
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>

#include "host.h"
#include "kiln_api.h"
#include "services/kiln_caps.h"
#include "services/kiln_combat.h"
#include "services/kiln_component_changes.h"
#include "services/kiln_items.h"
#include "services/kiln_players.h"
#include "services/kiln_window_clock.h"
#include "services/kiln_world.h"
#include "watch_core.h"

namespace kiln::decisions::component_changes {

// A guarded copy out of game memory (no C++ object in a function with __try).
static bool guarded_copy(void* dst, const void* src, size_t n) {
    __try {
        std::memcpy(dst, src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

namespace {

const KilnApi* g_api = nullptr;
KilnOwner g_owner = 0;
const KilnWorldApi* g_world = nullptr;
const KilnPlayersApi* g_players = nullptr;
const KilnCombatApi* g_combat = nullptr;
const KilnItemsApi* g_items = nullptr;
const KilnWindowClockApiV1* g_clock = nullptr;
const KilnCapsApi* g_caps = nullptr;
uint32_t g_hash[7] = {};
uint32_t g_setup_hash = 0;
std::atomic<uint32_t> g_world_thread{0};
std::atomic<bool> g_read_faulted{false};
// Between the server world's KILN_WORLD_STOPPING and the next KILN_WORLD_STARTED: a window call then would be a bug of
// the core's (counted, logged at the stop and at the process exit, and nothing is read).
std::atomic<bool> g_world_gone{false};
std::atomic<uint64_t> g_calls_after_stop{0}, g_calls_this_world{0};

KilnStr ks(const char* s) { return KilnStr{s, std::strlen(s)}; }
constexpr const char* kNames[7] = {"", "Health", "Stamina", "Mana", "Fuel", "Inventory", "SlotSelection"};

bool has_session() { return g_world && g_world->header.size >= offsetof(KilnWorldApi, game_world_session) + sizeof(void*) && g_world->game_world_session; }
bool combat_attributes() { return g_combat && g_combat->header.size >= offsetof(KilnCombatApi, attribute_add) + sizeof(void*); }
bool items_fuel() { return g_items && g_items->header.size >= offsetof(KilnItemsApi, fuel_read) + sizeof(void*) && g_items->fuel_read; }

// kiln.combat's attribute ids of this world (Health 1, Stamina 2, Mana 3): asked once per world epoch.
struct AttrIds {
    uint64_t epoch = ~0ull;
    bool ok[4] = {};
    KilnAttributeIds ids[4] = {};
} g_attr;

void KILN_CALL owner_probe(void*) {}

struct GamePorts final : watch::Ports {
    bool read(uint32_t entity, uint32_t component, uint8_t* out, uint32_t* size) override {
        *size = 0;
        if (component < 1 || component > 6 || !g_hash[component]) return false;
        uint32_t n = 0;
        const void* p = g_world->component(entity, g_hash[component], &n);
        if (!p || n == 0) return false;
        *size = n;
        if (n > KILN_WATCH_MAX_BYTES) return false;
        if (!guarded_copy(out, p, n)) {
            g_read_faulted.store(true);
            *size = 0;
            return false;
        }
        return true;
    }
    bool setup(uint32_t entity, uint8_t* out84) override {
        uint32_t n = 0;
        const void* p = g_setup_hash ? g_world->component(entity, g_setup_hash, &n) : nullptr;
        if (!p || n < watch::kSetupSize) return false;
        if (!guarded_copy(out84, p, watch::kSetupSize)) {
            g_read_faulted.store(true);
            return false;
        }
        return true;
    }
    bool key(uint32_t entity, KilnEventEntityKey* out, bool* exists) override {
        *out = KilnEventEntityKey{};
        *exists = false;
        Row row;
        if (host::resolve(entity, &row, false)) {
            *out = row.key;
            *exists = true;
            return true;
        }
        *exists = g_world->exists(entity);
        return false;
    }
    uint32_t players(uint32_t* out, uint32_t capacity) override {
        if (!g_players) return 0;
        static KilnPlayer list[64];  // world thread only
        uint32_t n = 0;
        for (auto& p : list) p.size = sizeof p;
        const KilnResult rc = g_players->list(KILN_PLAYERS_LIST_IN_GAME, list, 64, &n);
        if (rc != KILN_OK && rc != KILN_ERR_BUFFER_TOO_SMALL) return 0;
        uint32_t written = 0;
        for (uint32_t i = 0; i < n && i < 64 && written < capacity; ++i)
            if (list[i].state == KILN_PLAYER_IN_GAME && list[i].entity) out[written++] = list[i].entity;
        return written;
    }
    bool typed(uint32_t entity, uint32_t component, int32_t* current, int32_t* maximum) override {
        if (component >= KILN_WATCH_HEALTH && component <= KILN_WATCH_MANA) {
            if (!combat_attributes()) return false;
            const uint64_t epoch = host::epoch();
            if (g_attr.epoch != epoch) {
                g_attr = AttrIds{};
                g_attr.epoch = epoch;
                for (uint32_t kind = 1; kind <= 3; ++kind) {
                    g_attr.ids[kind].size = sizeof(KilnAttributeIds);
                    g_attr.ok[kind] = g_combat->attribute_ids(kind, &g_attr.ids[kind]) == KILN_OK && g_attr.ids[kind].max_id != 0;
                }
            }
            // the component index and kiln.combat's kind agree for these three (1 Health, 2 Stamina, 3 Mana)
            if (!g_attr.ok[component]) return false;
            KilnAttributeValue v{};
            v.size = sizeof v;
            if (g_combat->attribute_get(entity, g_attr.ids[component].current_id, &v) != KILN_OK || !v.has_max) return false;
            *current = v.current;
            *maximum = v.max;
            return true;
        }
        if (component == KILN_WATCH_FUEL) return items_fuel() && g_items->fuel_read(entity, current, maximum) == KILN_OK;
        return false;
    }
    bool owner_alive(KilnOwner owner) override { return owner == g_owner || g_api->post(owner, &owner_probe, nullptr) != KILN_ERR_REVOKED; }
    uint64_t session() override { return has_session() ? g_world->game_world_session(KILN_WORLD_SERVER) : 0; }
};

GamePorts g_ports;
watch::Watcher g_core(&g_ports);

bool world_safe(KilnWindowStateV1* out = nullptr) {
    KilnWindowStateV1 s{};
    if (!g_clock || g_clock->get(&s) != KILN_OK || !kiln_window_world_safe(&s, GetCurrentThreadId())) return false;
    if (out) *out = s;
    return true;
}

void lower(const char* row, uint32_t reason, const char* why) {
    if (g_caps) g_caps->report(g_owner, ks(row), KILN_CAP_UNAVAILABLE, reason, ks(why));
}

void stop(const char* why) {
    g_core.stop(why);
    lower("events.component_net", KILN_CAP_REASON_FAULTED, why);
    lower(KILN_COMPONENT_CHANGES_SERVICE, KILN_CAP_REASON_FAULTED, why);
    host::logf(KILN_LOG_ERROR, "component changes: stopped for this process: %s. Every watch ended (Disposed)", why);
}

// After a call that read the world: a read that faulted stops the service (the watches cannot be trusted).
void check_reads() {
    if (g_read_faulted.exchange(false)) stop("a component read faulted");
}

void fill_refusal(KilnComponentWatchStatusV1* st, uint32_t status, const char* why) {
    *st = KilnComponentWatchStatusV1{};
    st->size = sizeof *st;
    st->status = status;
    st->limit = KILN_WATCH_MAX_PAIRS;
    std::snprintf(st->reason, sizeof st->reason, "%s", why);
}

// ---- table ----
KilnResult KILN_CALL c_attach(KilnOwner d) {
    if (!g_core.on()) return KILN_ERR_UNAVAILABLE;
    if (!world_safe()) return KILN_ERR_WRONG_THREAD;
    return g_core.attach(d);
}
KilnResult KILN_CALL c_detach(KilnOwner d) {
    if (!g_core.on()) return KILN_ERR_UNAVAILABLE;
    if (!world_safe()) return KILN_ERR_WRONG_THREAD;
    return g_core.detach(d);
}
KilnResult KILN_CALL c_watch(KilnOwner owner, const KilnComponentWatchRequestV1* rq, KilnComponentWatchStatusV1* st) {
    if (!rq || !st) return KILN_ERR_INVALID_ARGUMENT;
    if (!g_core.on()) return KILN_ERR_UNAVAILABLE;
    if (!g_api->server_world()) {
        fill_refusal(st, KILN_WATCH_SUSPENDED_METADATA, "no server world runs in this process (a client that joined a game has none)");
        return KILN_OK;
    }
    KilnWindowStateV1 s{};
    if (!world_safe(&s)) return KILN_ERR_WRONG_THREAD;
    if (rq->scope == KILN_WATCH_SCOPE_PLAYERS && !g_players) {
        fill_refusal(st, KILN_WATCH_SUSPENDED_METADATA, "kiln.players is not loaded: the players in the game cannot be followed");
        return KILN_OK;
    }
    const KilnResult rc = g_core.create(owner, *rq, s.world_epoch, s.step, st);
    check_reads();
    return rc;
}
KilnResult KILN_CALL c_resume(KilnOwner owner, uint64_t id, KilnComponentWatchStatusV1* st) {
    if (!st) return KILN_ERR_INVALID_ARGUMENT;
    if (!g_core.on()) return KILN_ERR_UNAVAILABLE;
    KilnWindowStateV1 s{};
    if (!world_safe(&s)) return KILN_ERR_WRONG_THREAD;
    const KilnResult rc = g_core.resume(owner, id, s.step, st);
    check_reads();
    return rc;
}
KilnResult KILN_CALL c_status(uint64_t id, KilnComponentWatchStatusV1* st) {
    if (!st) return KILN_ERR_INVALID_ARGUMENT;
    return g_core.status(id, st);
}
KilnResult KILN_CALL c_remove(KilnOwner owner, uint64_t id) {
    if (!g_core.on()) return KILN_ERR_UNAVAILABLE;
    return g_core.remove(owner, id);  // any thread: it reads nothing of the world
}
KilnResult KILN_CALL c_release_owner(KilnOwner owner) {
    if (!g_core.on()) return KILN_ERR_UNAVAILABLE;
    const uint32_t n = g_core.release_owner(owner);
    if (n > 0) {
        KilnComponentChangesCountersV1 c{};
        c.size = sizeof c;
        g_core.counters(&c);
        host::logf(KILN_LOG_INFO, "component changes: %u watch(es) released with their owner (its mod unloaded); %u watch(es), %u pair(s) left%s",
                   n, c.watches, c.pairs, c.watches == 0 ? ": nothing is watched, no window call from the next step" : "");
    }
    return KILN_OK;
}
KilnResult KILN_CALL c_begin_events(KilnOwner d, uint64_t* batch, uint32_t* records) {
    if (!batch || !records) return KILN_ERR_INVALID_ARGUMENT;
    if (!g_core.on()) return KILN_ERR_UNAVAILABLE;
    if (!world_safe()) return KILN_ERR_WRONG_THREAD;
    return g_core.begin_events(d, batch, records);
}
KilnResult KILN_CALL c_read_events(KilnOwner d, uint64_t batch, KilnComponentChangeV1* out, uint32_t capacity, uint32_t stride,
                                   uint32_t* required) {
    if (!g_core.on()) return KILN_ERR_UNAVAILABLE;
    if (!world_safe()) return KILN_ERR_WRONG_THREAD;
    return g_core.read_events(d, batch, out, capacity, stride, required);
}
KilnResult KILN_CALL c_ack_events(KilnOwner d, uint64_t batch) {
    if (!g_core.on()) return KILN_ERR_UNAVAILABLE;
    if (!world_safe()) return KILN_ERR_WRONG_THREAD;
    return g_core.ack_events(d, batch);
}
KilnResult KILN_CALL c_record_write(KilnOwner owner, const KilnKnownWriteV1* w) {
    if (!w) return KILN_ERR_INVALID_ARGUMENT;
    if (!g_core.any()) return KILN_OK;  // nothing watched: one atomic load
    return g_core.record_write(owner, *w, GetCurrentThreadId() == g_world_thread.load(std::memory_order_relaxed));
}

uint64_t now_ns() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

uint64_t g_windows_with_pairs = 0;

void log_stats() {
    KilnComponentChangesCountersV1 c{};
    c.size = sizeof c;
    g_core.counters(&c);
    host::logf(KILN_LOG_INFO,
               "component changes: %u watch(es), %u pair(s) (%u present); window_begin p50 %.1f us p99 %.1f us max %.1f us, "
               "window_end p50 %.1f us p99 %.1f us max %.1f us over %llu window(s); %llu change(s), %llu token(s) (%llu off the world "
               "thread), %llu lost, %llu queue overflow(s)",
               c.watches, c.pairs, c.present_pairs, c.begin_ns_p50 / 1000.0, c.begin_ns_p99 / 1000.0, c.begin_ns_max / 1000.0,
               c.end_ns_p50 / 1000.0, c.end_ns_p99 / 1000.0, c.end_ns_max / 1000.0, static_cast<unsigned long long>(c.samples),
               static_cast<unsigned long long>(c.changes), static_cast<unsigned long long>(c.tokens_recorded),
               static_cast<unsigned long long>(c.tokens_off_world_thread), static_cast<unsigned long long>(c.tokens_lost),
               static_cast<unsigned long long>(c.queue_overflows));
}

void KILN_CALL c_window_begin(uint64_t epoch, uint64_t step) {
    if (g_world_gone.load(std::memory_order_relaxed)) {
        g_calls_after_stop.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    g_calls_this_world.fetch_add(1, std::memory_order_relaxed);
    if (!g_core.on() || g_core.stopped()) return;
    g_world_thread.store(GetCurrentThreadId(), std::memory_order_relaxed);
    const uint64_t t0 = now_ns();
    g_core.window_begin(epoch, step);
    g_core.timed(true, now_ns() - t0);
    check_reads();
}
void KILN_CALL c_window_end(uint64_t epoch, uint64_t step) {
    if (g_world_gone.load(std::memory_order_relaxed)) {
        g_calls_after_stop.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    g_calls_this_world.fetch_add(1, std::memory_order_relaxed);
    if (!g_core.on() || g_core.stopped()) return;
    const uint64_t t0 = now_ns();
    g_core.window_end(epoch, step);
    g_core.timed(false, now_ns() - t0);
    check_reads();
    if (g_core.any() && ++g_windows_with_pairs % 1500 == 0) log_stats();  // about once a minute while something is watched
}
uint32_t KILN_CALL c_window_wanted() { return g_core.window_wanted(); }
uint32_t KILN_CALL c_watched(uint32_t entity) { return g_core.watched(entity); }
void KILN_CALL c_window_faulted(uint32_t code) {
    char why[96];
    std::snprintf(why, sizeof why, "a window call of kiln.component_changes faulted (0x%08x)", code);
    stop(why);
}
KilnResult KILN_CALL c_counters(KilnComponentChangesCountersV1* out) {
    if (!out || out->size < sizeof(KilnComponentChangesCountersV1)) return KILN_ERR_INVALID_ARGUMENT;
    g_core.counters(out);
    return KILN_OK;
}

// Every entry: an exception (a lock that could not be had, a std::bad_alloc) stops the service, never the game.
template <typename T, T Fn> struct Safe;
template <typename R, typename... A, R(KILN_CALL* Fn)(A...)>
struct Safe<R(KILN_CALL*)(A...), Fn> {
    static R KILN_CALL call(A... args) noexcept {
        try {
            return Fn(args...);
        } catch (...) {
            try {
                stop("a call could not acquire its lock or threw");
            } catch (...) {
            }
            if constexpr (std::is_void_v<R>) return;
            else if constexpr (std::is_same_v<R, KilnResult>) return KILN_ERR_UNAVAILABLE;
            else return 0;
        }
    }
};
#define SAFE(fn) &Safe<decltype(&fn), &fn>::call

const KilnComponentChangesApiV1 kTable = {
    {sizeof(KilnComponentChangesApiV1), KILN_COMPONENT_CHANGES_VERSION},
    SAFE(c_attach),
    SAFE(c_detach),
    SAFE(c_watch),
    SAFE(c_resume),
    SAFE(c_status),
    SAFE(c_remove),
    SAFE(c_release_owner),
    SAFE(c_begin_events),
    SAFE(c_read_events),
    SAFE(c_ack_events),
    SAFE(c_record_write),
    SAFE(c_window_begin),
    SAFE(c_window_end),
    SAFE(c_window_wanted),   // appended (0.2.0-dev.29)
    SAFE(c_window_faulted),  // appended
    SAFE(c_counters),        // appended
    SAFE(c_watched),         // appended
};

void KILN_CALL on_world(const KilnWorldEvent* e, void*) {
    if (e->kind != KILN_WORLD_SERVER) return;
    if (e->event == KILN_WORLD_STARTED) {
        g_world_gone.store(false);
        g_calls_this_world.store(0);
        return;
    }
    if (e->event != KILN_WORLD_STOPPING) return;
    g_world_gone.store(true);
    if (!g_core.on() || g_core.stopped()) return;
    const auto t0 = std::chrono::steady_clock::now();
    const watch::Watcher::Stopped s = g_core.world_stopped("the server world stopped; create a new watch in the next world");
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    host::logf(KILN_LOG_INFO,
               "component changes: the server world stopped: %u watch(es) ended (Disposed), %u unread record(s) in %u frame(s) dropped "
               "(never delivered into another world), in %.3f ms; %llu window call(s) in this world, none after it",
               s.watches, s.records, s.frames, ms, static_cast<unsigned long long>(g_calls_this_world.load()));
}

void off(uint32_t reason, const char* why) {
    host::logf(KILN_LOG_WARN, "component changes: unavailable (%s)", why);
    lower(KILN_COMPONENT_CHANGES_SERVICE, reason, why);
    lower("events.component_net", reason, why);
}

}  // namespace

// The process exits (kiln.decisions' on_unload): what came after the world's stop. Logs only; no game memory.
void at_exit() {
    KilnComponentChangesCountersV1 c{};
    c.size = sizeof c;
    g_core.counters(&c);
    host::logf(KILN_LOG_INFO, "component changes: process exit: %llu window call(s) after the server world stopped (0 expected), %u watch(es) left, "
               "%llu change(s) and %llu queue overflow(s) in this process",
               static_cast<unsigned long long>(g_calls_after_stop.load()), c.watches, static_cast<unsigned long long>(c.changes),
               static_cast<unsigned long long>(c.queue_overflows));
}

KilnResult register_table(const KilnApi* api, KilnOwner owner) {
    g_api = api;
    g_owner = owner;
    return api->register_service(owner, ks(KILN_COMPONENT_CHANGES_SERVICE), &kTable);
}

// kiln.decisions' on_ready, after its own: `decisions_on` says whether it is on (the metadata of the roots' lives and
// the window clock are its).
void ready(bool decisions_on) {
    g_caps = static_cast<const KilnCapsApi*>(g_api->service(ks(KILN_CAPS_SERVICE), KILN_CAPS_VERSION));
    g_clock = static_cast<const KilnWindowClockApiV1*>(g_api->service(ks(KILN_WINDOW_CLOCK_SERVICE), KILN_WINDOW_CLOCK_VERSION));
    g_world = static_cast<const KilnWorldApi*>(g_api->service(ks(KILN_WORLD_SERVICE), KILN_WORLD_VERSION));
    g_players = static_cast<const KilnPlayersApi*>(g_api->service(ks(KILN_PLAYERS_SERVICE), KILN_PLAYERS_VERSION));
    g_combat = static_cast<const KilnCombatApi*>(g_api->service(ks(KILN_COMBAT_SERVICE), 1));  // v2 members checked by size
    g_items = static_cast<const KilnItemsApi*>(g_api->service(ks(KILN_ITEMS_SERVICE), 2));     // fuel_read is v2
    if (!decisions_on) return off(KILN_CAP_REASON_SERVICE_MISSING, "kiln.decisions is off (see its row): the roots' lives and the step windows are its");
    if (!g_clock || !g_world) return off(KILN_CAP_REASON_SERVICE_MISSING, "kiln.window_clock or kiln.world is not there");
    if (!g_api->feature(ks("events.component_net")))
        return off(KILN_CAP_REASON_SYMBOL_MISSING, "the game's GetComponent or WaitWorldJobs did not resolve on this build");
    for (uint32_t c = 1; c <= 6; ++c) g_hash[c] = g_world->component_hash(ks(kNames[c]));
    g_setup_hash = g_world->component_hash(ks("InventorySetup"));
    KilnHandle h = 0;
    if (g_api->subscribe_world(g_owner, SAFE(on_world), nullptr, &h) != KILN_OK) return off(KILN_CAP_REASON_SERVICE_MISSING, "no world events");
    g_core.set_on(true);
    // Not raised above what the core says of the sigdb row: the core's own report (the symbols) still counts.
    std::string text = "named component watches (ComponentChanges.Watch) on the server world";
    text += g_players ? "" : "; scope Players is off: kiln.players is not loaded";
    text += combat_attributes() ? "" : "; no typed Health, Stamina, Mana values: kiln.combat v2 is not loaded";
    text += items_fuel() ? "" : "; no typed Fuel values: kiln.items v4 is not loaded";
    if (g_api->size >= offsetof(KilnApi, side) + sizeof(void*) && g_api->side && g_api->side() == KILN_SIDE_CLIENT)
        text += "; on a game client only while it hosts the world";
    if (g_caps) g_caps->report(g_owner, ks("events.component_net"), KILN_CAP_AVAILABLE, KILN_CAP_REASON_NONE, ks(text.c_str()));
    host::logf(KILN_LOG_INFO, "component changes: on (%u watches, %u pairs, %u roots a watch; nothing is read while nobody watches)",
               KILN_WATCH_MAX_WATCHES, KILN_WATCH_MAX_PAIRS, KILN_WATCH_MAX_ROOTS);
}

}  // namespace kiln::decisions::component_changes
