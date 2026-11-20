// SPDX-License-Identifier: MIT
// kiln.abilities: the game's Impact VM for mods (instances, natives, programs). Evidence: docs/re/abilities.md.
// Usage: docs/kiln/services/abilities.md. Table: native/core/abi/services/kiln_abilities.h.
//
// Threads: the detours on createImpactInstance and runImpact, and every native thunk, run on the game's job threads
// inside the sim step. They only touch Kiln's own state under g_mu / g_ev_mu (never while calling into the game) and
// game memory under SEH guards. Everything else (the listener, registrations, instance reads/writes) runs on the
// tick thread in Pre/Post, when no world job runs.
#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "engine.h"
#include "kiln_api.h"
#include "impact_watch.h"
#include "nlohmann/json.hpp"
#include "kiln_hook.hpp"
#include "natives_core.h"
#include "natives_json.h"
#include "services/kiln_abilities.h"
#include "services/kiln_assets.h"
#include "services/kiln_profile.h"
#include "services/kiln_spawn.h"
#include "services/kiln_world.h"
#include "verify.h"

using namespace kab;

static_assert(sizeof(KilnAbilityEvent) == 640 && sizeof(KilnImpactVar) == 88 && sizeof(KilnConfigOverride) == 80 &&
              sizeof(KilnNativeInfo) == 88 && sizeof(KilnImpactProgramInfo) == 44 && sizeof(KilnNativeCall) == 104 &&
              sizeof(KilnAbilitiesStatus) == 160);
// since 0.2.0-dev.29: the table's tail (append only) and the counters
static_assert(offsetof(KilnAbilitiesApi, sequence_play) == 232 && offsetof(KilnAbilitiesApi, native_register_sync) == 240 &&
              offsetof(KilnAbilitiesApi, native_counters) == 248 && offsetof(KilnAbilitiesApi, native_attach) == 256 &&
              offsetof(KilnAbilitiesApi, native_detach) == 264 && offsetof(KilnAbilitiesApi, native_declare_typed) == 272 &&
              offsetof(KilnAbilitiesApi, native_state) == 280 && offsetof(KilnAbilitiesApi, native_attach_state) == 288 &&
              offsetof(KilnAbilitiesApi, native_ecs_read) == 296 && offsetof(KilnAbilitiesApi, native_ecs_write) == 304 &&
              sizeof(KilnAbilitiesApi) == 312 && sizeof(KilnNativeCounters) == 128 && sizeof(KilnNativeFilter) == 16 &&
              sizeof(KilnNativeArg) == 8);

namespace {

const KilnApi* g_api = nullptr;
KilnOwner g_owner = 0;
const KilnSpawnApi* g_spawn = nullptr;
const KilnAssetsApi* g_assets = nullptr;

bool g_have_natives = false, g_have_programs = false, g_have_instances = false, g_have_attrs = false;

// ---------------------------------------------------------------------------------------------------- logging ----
void logv(int level, const char* fmt, va_list ap) {
    char buf[1024];
    int n = std::vsnprintf(buf, sizeof buf, fmt, ap);
    if (n < 0) return;
    g_api->log(g_owner, level, KilnStr{buf, static_cast<size_t>(std::min(n, static_cast<int>(sizeof buf) - 1))});
}
void logi(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    logv(KILN_LOG_INFO, fmt, ap);
    va_end(ap);
}
void logw(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    logv(KILN_LOG_WARN, fmt, ap);
    va_end(ap);
}

std::string guid_text(const uint8_t* b) {
    char s[40];
    std::snprintf(s, sizeof s, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", b[3], b[2], b[1],
                  b[0], b[5], b[4], b[7], b[6], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
    return s;
}

bool on_tick() { return g_api->on_tick_thread(); }

// ------------------------------------------------------------------------------------------- shared state -------
std::mutex g_mu;     // everything below except the event queue
std::mutex g_ev_mu;  // the event queue

std::atomic<uint64_t> g_step{0};
std::atomic<uintptr_t> g_world{0};
std::atomic<uintptr_t> g_system{0};        // impact::ImpactSystem (createImpactInstance/runImpact rcx)
std::atomic<uintptr_t> g_prog_registry{0}; // *(system + 8)
std::atomic<uintptr_t> g_native_registry{0};
std::atomic<uint32_t> g_interest{0};

// counters
std::atomic<uint64_t> c_created{0}, c_held{0}, c_ovr_applied{0}, c_ovr_mismatch{0}, c_runs{0}, c_native_calls{0},
    c_observed{0}, c_signals{0}, c_managed_calls{0}, c_managed_suspends{0}, c_delivered{0}, c_dropped{0}, c_wrap{0},
    c_refused{0};

// since 0.2.0-dev.29: the native thunks' own counters (native_counters)
std::atomic<uint64_t> c_sync_calls{0}, c_sync_faults{0}, c_sync_slow{0}, c_dead_calls{0}, c_lock_bypass{0},
    c_attach_entered{0}, c_attach_skipped{0}, c_typed_refused{0}, c_typed_unconnected{0};
std::atomic<uint32_t> g_inflight{0};  // calls inside a Kiln thunk right now

int64_t g_qpf = 0;  // QueryPerformanceFrequency, set at init
int64_t qpc() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}
int64_t now_ns() { return g_qpf > 0 ? static_cast<int64_t>(static_cast<double>(qpc()) * 1e9 / static_cast<double>(g_qpf)) : 0; }
constexpr int64_t kRetireWaitNs = 2'000'000;  // the bounded wait of a retirement or a world stop (review P1)

// A hook visit never waits on a lock for long. Kiln's tick-thread work under g_mu / g_ev_mu is short, so a job thread
// that cannot take one within about a millisecond gives up and takes the path that needs no lock (a dropped event, a
// managed call that suspends one more step).
bool bounded_lock(std::mutex& m) {
    if (m.try_lock()) return true;
    const int64_t end = qpc() + (g_qpf > 0 ? g_qpf / 1000 : 0);
    while (qpc() < end) {
        SwitchToThread();
        if (m.try_lock()) return true;
    }
    c_lock_bypass.fetch_add(1, std::memory_order_relaxed);
    return false;
}

// Pending work the hot detours must check: held + canceled + per-impact overrides (0 = the fast path).
std::atomic<uint32_t> g_pending{0};
std::atomic<uint32_t> g_first_instance{0};  // the first instance seen (layout check)
std::atomic<uint32_t> g_entity_ovr_count{0};

struct Overrides {
    std::vector<KilnConfigOverride> list;
    uint64_t expires = 0;
};
std::unordered_map<uint32_t, Overrides> g_entity_ovr;   // entity -> overrides for its next instance
std::unordered_map<uint32_t, Overrides> g_impact_ovr;   // impact id -> overrides to apply before its first run
std::unordered_set<uint32_t> g_held;                    // impact ids held for the listener
std::unordered_map<uint32_t, uint64_t> g_canceled;      // impact id -> step until which it stays blocked
std::unordered_set<uint32_t> g_hold_filter;             // program ids (empty = all)

struct Tracked {
    uint32_t program;
    uint32_t self;
};
std::unordered_map<uint32_t, Tracked> g_tracked;  // instances reported STARTED, watched for ENDED

std::vector<KilnAbilityEvent> g_events;  // filled by job threads (g_ev_mu)
constexpr size_t kMaxEvents = 4096;

KilnAbilityListenerFn g_listener = nullptr;
void* g_listener_user = nullptr;

void recount_pending() {  // g_mu held
    g_pending.store(static_cast<uint32_t>(g_held.size() + g_canceled.size() + g_impact_ovr.size()),
                    std::memory_order_release);
    g_entity_ovr_count.store(static_cast<uint32_t>(g_entity_ovr.size()), std::memory_order_release);
}

bool push_event(const KilnAbilityEvent& ev) {  // job threads
    if (!bounded_lock(g_ev_mu)) {
        c_dropped.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    std::lock_guard lock(g_ev_mu, std::adopt_lock);
    if (g_events.size() >= kMaxEvents) {
        c_dropped.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    g_events.push_back(ev);
    return true;
}

KilnAbilityEvent new_event(uint32_t kind) {
    KilnAbilityEvent ev;
    std::memset(&ev, 0, sizeof ev);
    ev.size = sizeof ev;
    ev.kind = kind;
    ev.step = g_step.load(std::memory_order_relaxed);
    std::memset(ev.arg_offsets, 0xFF, sizeof ev.arg_offsets);
    return ev;
}

bool interested(uint32_t kind) { return (g_interest.load(std::memory_order_relaxed) >> kind) & 1u; }

// ---------------------------------------------------------------------------------------- natives (slots) -------
struct Captured {
    uint32_t hash;
    NativeFn fn;
    uint8_t argc;
    char name[33];
};
std::vector<Captured> g_captured;          // the game's registrations into g_cap_registry (g_mu)
uintptr_t g_cap_registry = 0;              // the registry the server world's natives went into (chosen in setup)
// Every registry the game registered natives into, by pointer (a client also builds registries for its own worlds;
// the server world's is the one its impact system passes to programs: EcsIterCtx+0x110, seen in runImpact).
std::map<uintptr_t, std::vector<Captured>> g_registries;  // (g_mu)
uintptr_t g_last_registry = 0;                            // (g_mu)
thread_local bool t_kiln_registering = false;

// SLOT_MANAGED_SYNC (since 0.2.0-dev.29): a Kiln native answered inside the ECALL by its own fn (no original).
enum SlotKind : uint32_t { SLOT_FREE = 0, SLOT_WRAPPED, SLOT_LOG, SLOT_SIGNAL, SLOT_MANAGED, SLOT_DEAD, SLOT_MANAGED_SYNC };
bool kiln_kind(uint32_t k) { return k == SLOT_LOG || k == SLOT_SIGNAL || k == SLOT_MANAGED || k == SLOT_MANAGED_SYNC; }
bool mod_kind(uint32_t k) { return k == SLOT_MANAGED || k == SLOT_MANAGED_SYNC; }  // a mod's native: unregister, owner

struct Slot {
    std::atomic<uint32_t> kind{SLOT_FREE};
    uint32_t hash = 0;
    uint32_t argc = 0;
    std::atomic<NativeFn> original{nullptr};
    std::atomic<bool> wrap{false};
    std::atomic<bool> observe{false};
    char name[96]{};
    KilnOwner owner = 0;
    uintptr_t registered_in = 0;  // the native registry this slot's thunk is registered in
    // SLOT_MANAGED_SYNC: the native's own fn, user and owner gate as one immutable binding, stored before `kind` is
    // published (release) and read after it (acquire); retired (not freed) when taken off
    std::atomic<const SyncBinding*> sync{nullptr};
    // SLOT_WRAPPED: the attachments of this native (natives_core.h), an immutable set swapped on the tick thread; an
    // old set is freed in a later Pre when no thunk runs (g_retired)
    std::atomic<const AttachSet*> attach{nullptr};
    // a Kiln native's typed declaration (native_declare_typed), checked before its handler sees a call; swapped like
    // `attach`
    std::atomic<const std::vector<KilnNativeArg>*> typed{nullptr};
};
constexpr int kSlots = 160;
Slot g_slots[kSlots];

// since 0.2.0-dev.29 (review P1): owner admission and deferred retirement (natives_core.h). g_mu.
OwnerGates g_gates;
std::vector<const SyncBinding*> g_retired_bindings;  // taken off, freed in a Pre once their gate is quiet
void retire_binding(const SyncBinding* b) {          // g_mu held
    if (b) g_retired_bindings.push_back(b);
}
std::vector<Deferred<KilnOwner>> g_deferred_owners;  // owners whose retirement waits for its calls to leave (g_mu)
std::atomic<bool> g_programs_added{false};           // a Kiln program went in after the readiness check

std::unordered_map<uint32_t, int> g_slot_of;          // hash -> slot (g_mu)
std::unordered_map<uint32_t, uint32_t> g_vanilla_argc; // hash -> argc, the game's own natives (g_mu)
std::unordered_map<uint32_t, uint32_t> g_declared;     // hash -> argc, from natives.json (g_mu)
std::unordered_map<uint32_t, std::vector<KilnNativeArg>> g_typed;  // hash -> typed declaration (g_mu; since dev.29)
std::vector<const std::vector<KilnNativeArg>*> g_retired_typed;   // swapped out, freed in a Pre (g_mu)

KilnNativeWrapFn g_wrap_fn = nullptr;
void* g_wrap_user = nullptr;

// ---- managed native calls (suspend / resume) ----
struct ManagedCall {
    uint64_t id;
    int slot;
    uint8_t state;  // 0 queued, 1 done
    uint8_t error, value;
    uint64_t step;
    bool delivered;
};
std::unordered_map<uint64_t, ManagedCall> g_calls;  // (impact id << 32 | site) -> call (g_mu)
std::unordered_map<uint64_t, uint64_t> g_call_key;  // call id -> key
uint64_t g_call_seq = 0;

// Resolves an argument value: a handle 0xFFFF0000|index into the instance's variable. Job thread, guarded reads.
bool resolve(const NativeCtx* ctx, uint32_t h, ImpactVariable* var, uint8_t** ptr) {
    if ((h & 0xFFFF0000u) != 0xFFFF0000u || !ctx) return false;
    const uint32_t idx = h & 0xFFFF;
    if (idx >= ctx->layout_count) return false;
    if (!read_bytes(ctx->layout + idx * sizeof(ImpactVariable), var, sizeof *var)) return false;
    if (static_cast<uint64_t>(var->offset) + var->size > ctx->data_size) return false;
    *ptr = ctx->data + var->offset;
    return true;
}

uint32_t self_of(uint32_t impact_id) {
    uint8_t* inst = instance(g_system.load(std::memory_order_relaxed), impact_id);
    uint32_t e = 0;
    if (inst) read_bytes(inst + E.inst_entity, &e, 4);
    return e;
}

// Fills args/arg_types/arg_values/value[] of a NATIVE event (after the call, so outputs are in).
void capture_args(KilnAbilityEvent& ev, const ArgSlice* args, const NativeCtx* ctx) {
    const uint32_t n = static_cast<uint32_t>(std::min<uint64_t>(args ? args->count : 0, 31));
    ev.argc = n;
    uint32_t used = 0;
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t a = 0;
        read_bytes(args->data + i, &a, 4);
        ev.args[i] = a;
        ImpactVariable v{};
        uint8_t* p = nullptr;
        if (resolve(ctx, a, &v, &p)) {
            ev.arg_types[i] = v.type;
            uint32_t first = 0;
            read_bytes(p, &first, std::min<uint32_t>(4, v.size));
            ev.arg_values[i] = first;
            const uint32_t take = std::min<uint32_t>(v.size, sizeof ev.value - used);
            if (v.size > 4 && take == v.size && take <= 255) {
                if (read_bytes(p, ev.value + used, take)) {
                    ev.arg_offsets[i] = static_cast<uint8_t>(used);
                    ev.arg_sizes[i] = static_cast<uint8_t>(take);
                    used += take;
                }
            }
        } else {
            ev.arg_values[i] = a;
        }
    }
}

// The thunk body. No C++ objects with destructors live here (seh_dispatch wraps it); locks are taken in helpers.
thread_local bool t_in_game = false;   // inside the game's own native (a fault there is not retried)
thread_local NativeCtx* t_ctx = nullptr;  // the active VM frame on this thread
thread_local int t_slot = -1;          // the slot whose thunk runs on this thread now (call_original; no registration)
thread_local bool t_orig_ran = false;  // the game's native ran in this call: it never runs a second time (the latch)
thread_local int32_t t_stage = KILN_NATIVE_BEFORE;  // the attachment stage running on this thread (call_original)
// since 0.2.0-dev.29 (review P1): the owner gate this thread's callback entered, and its watchdog record (a fault
// leaves both through on_fault)
thread_local OwnerGate* t_gate = nullptr;
thread_local int t_inside = -1;
InsideTable<64> g_inside;
std::atomic<bool> g_watchdog_on{false};
std::atomic<uint64_t> c_watchdog_reports{0}, c_retire_deferred{0};
void leave_gate() {
    if (OwnerGate* g = t_gate) {
        t_gate = nullptr;
        gate_leave(*g);
    }
}
void watch(int64_t now, const char* what, KilnOwner owner) {
    if (g_watchdog_on.load(std::memory_order_relaxed)) t_inside = g_inside.claim(now, what, owner);
}
void unwatch() {
    g_inside.release(t_inside);
    t_inside = -1;
}
NativeResult* call_fn(NativeFn fn, NativeResult* out, ArgSlice* args, NativeCtx* ctx) {
    t_orig_ran = true;
    t_in_game = true;
    NativeResult* r = fn(out, args, ctx);
    t_in_game = false;
    return r;
}

// The call record a dispatcher or a sync native's fn gets (the wrap path's, also for sync natives).
void fill_call(KilnNativeCall& c, const Slot& s, NativeResult* out, ArgSlice* args, NativeCtx* ctx) {
    std::memset(&c, 0, sizeof c);
    c.size = sizeof c;
    c.hash = s.hash;
    c.argc = static_cast<uint32_t>(std::min<uint64_t>(args->count, 31));
    c.args = args->data;
    c.layout = ctx->layout;
    c.layout_count = ctx->layout_count;
    c.data = ctx->data;
    c.data_size = ctx->data_size;
    c.impact_id = ctx->impact_id;
    c.program_id = ctx->program_id;
    c.site = ctx->site;
    c.self = self_of(ctx->impact_id);
    c.engine[0] = out;
    c.engine[1] = args;
    c.engine[2] = ctx;
    c.out_error = kUndecided;  // nothing handled the call yet (a wrapped native: the game's native runs)
    c.out_value = 0;
}

// A wrapped vanilla native: the global wrap dispatcher (native_wrap) and the attachments (native_attach) share one call
// record; the game's native runs once, when nothing answered. A call that matches no attachment's filter enters no
// managed code (the filters are compared here).
void dispatch_wrapped(Slot& s, NativeResult* out, ArgSlice* args, NativeCtx* ctx) {
    c_native_calls.fetch_add(1, std::memory_order_relaxed);
    NativeFn orig = s.original.load(std::memory_order_acquire);
    const bool legacy = s.wrap.load(std::memory_order_relaxed) && g_wrap_fn;
    const AttachSet* set = s.attach.load(std::memory_order_seq_cst);
    if (legacy || set) {
        KilnNativeCall c;
        fill_call(c, s, out, args, ctx);
        auto decided = [&c] { return c.original_called != 0 || c.out_error != kUndecided; };
        auto original = [&] {
            c.original_called = 1;
            call_fn(orig, out, args, ctx);
            c.out_error = out->error;
            c.out_value = out->value;
        };
        t_stage = KILN_NATIVE_BEFORE;
        if (legacy) {
            c_wrap.fetch_add(1, std::memory_order_relaxed);
            g_wrap_fn(&c, g_wrap_user);  // guarded by the thunk's SEH frame
        }
        if (set) {
            uint64_t entered = 0, skipped = 0;
            run_stages(
                *set, ctx->program_id, ctx->site,
                [&c, &s](const Attachment& a, int32_t stage) {
                    if (!a.gate || !gate_enter(*a.gate)) return;  // its owner is retiring: as if it were not there
                    t_gate = a.gate;
                    t_stage = stage;
                    watch(qpc(), s.name, a.gate->owner);
                    a.fn(&c, a.user);
                    unwatch();
                    leave_gate();
                },
                original, decided, &entered, &skipped);
            if (entered) c_attach_entered.fetch_add(entered, std::memory_order_relaxed);
            if (skipped) c_attach_skipped.fetch_add(skipped, std::memory_order_relaxed);
        } else if (!decided()) {
            original();
        }
        t_stage = KILN_NATIVE_BEFORE;
        out->error = c.out_error;
        out->value = c.out_value;
    } else {
        call_fn(orig, out, args, ctx);
    }
    if (s.observe.load(std::memory_order_relaxed) && interested(KILN_ABILITY_EV_NATIVE)) {
        KilnAbilityEvent ev = new_event(KILN_ABILITY_EV_NATIVE);
        ev.native_hash = s.hash;
        ev.impact_id = ctx->impact_id;
        ev.program_id = ctx->program_id;
        ev.site = ctx->site;
        ev.self = self_of(ctx->impact_id);
        ev.result = out->value;
        ev.error = out->error;
        capture_args(ev, args, ctx);
        c_observed.fetch_add(1, std::memory_order_relaxed);
        push_event(ev);
    }
}

std::string format_value(uint32_t type, const uint8_t* p, uint32_t size) {
    char b[256];
    if (size == 4 && type == KILN_IMPACT_TYPE_FLOAT32) {
        float f;
        std::memcpy(&f, p, 4);
        std::snprintf(b, sizeof b, "%g", f);
    } else if (size == 4 && type == KILN_IMPACT_TYPE_SINT32) {
        int32_t v;
        std::memcpy(&v, p, 4);
        std::snprintf(b, sizeof b, "%d", v);
    } else if (size == 4) {
        uint32_t v;
        std::memcpy(&v, p, 4);
        std::snprintf(b, sizeof b, "%u (0x%08x)", v, v);
    } else if (size == 1) {
        std::snprintf(b, sizeof b, "%u", p[0]);
    } else if (size == 8 && type == KILN_IMPACT_TYPE_TIME) {
        int64_t v;
        std::memcpy(&v, p, 8);
        std::snprintf(b, sizeof b, "%.3f s", static_cast<double>(v) / 1e9);
    } else if (size == 12) {
        float f[3];
        std::memcpy(f, p, 12);
        std::snprintf(b, sizeof b, "(%g, %g, %g)", f[0], f[1], f[2]);
    } else {
        std::string s;
        for (uint32_t i = 0; i < std::min<uint32_t>(size, 64); ++i) {
            char h[4];
            std::snprintf(h, sizeof h, "%02x", p[i]);
            s += h;
        }
        return s;
    }
    return b;
}

std::string var_name(const NativeCtx* ctx, uint32_t idx) {
    const uint8_t* e = ctx->layout + idx * sizeof(ImpactVariable);
    ImpactVariable v{};
    if (!read_bytes(e, &v, sizeof v) || !v.dbg_len || !v.dbg_off) return "?";
    char n[80]{};
    const uint32_t len = std::min<uint32_t>(v.dbg_len, sizeof n - 1);
    if (!read_bytes(e + 16 + v.dbg_off, n, len)) return "?";
    return n;
}

void dispatch_log(NativeResult* out, ArgSlice* args, NativeCtx* ctx) {
    uint32_t a = 0;
    if (args->count >= 1) read_bytes(args->data, &a, 4);
    ImpactVariable v{};
    uint8_t* p = nullptr;
    uint8_t buf[64]{};
    std::string what;
    if (resolve(ctx, a, &v, &p) && read_bytes(p, buf, std::min<uint32_t>(v.size, sizeof buf)))
        what = var_name(ctx, a & 0xFFFF) + " = " + format_value(v.type, buf, std::min<uint32_t>(v.size, sizeof buf));
    else {
        char b[48];
        std::snprintf(b, sizeof b, "raw %u (0x%08x)", a, a);
        what = b;
    }
    logi("kiln.Log: program 0x%08x impact %u self %u site 0x%08x: %s", ctx->program_id, ctx->impact_id,
         self_of(ctx->impact_id), ctx->site, what.c_str());
    out->error = 0;
    out->value = 1;
}

void dispatch_signal(NativeResult* out, ArgSlice* args, NativeCtx* ctx) {
    c_signals.fetch_add(1, std::memory_order_relaxed);
    if (interested(KILN_ABILITY_EV_SIGNAL)) {
        KilnAbilityEvent ev = new_event(KILN_ABILITY_EV_SIGNAL);
        ev.impact_id = ctx->impact_id;
        ev.program_id = ctx->program_id;
        ev.site = ctx->site;
        ev.self = self_of(ctx->impact_id);
        ev.native_hash = fnv1a("kiln.Signal");
        uint32_t a0 = 0, a1 = 0;
        if (args->count >= 1) read_bytes(args->data, &a0, 4);
        if (args->count >= 2) read_bytes(args->data + 1, &a1, 4);
        ImpactVariable v{};
        uint8_t* p = nullptr;
        if (resolve(ctx, a1, &v, &p)) read_bytes(p, &ev.tag, std::min<uint32_t>(4, v.size));
        else ev.tag = a1;
        if (resolve(ctx, a0, &v, &p)) {
            ev.value_type = v.type;
            ev.value_size = std::min<uint32_t>(v.size, 64);
            read_bytes(p, ev.value, ev.value_size);
        } else {
            ev.value_type = 0;
            ev.value_size = 4;
            std::memcpy(ev.value, &a0, 4);
        }
        ev.argc = 2;
        ev.args[0] = a0;
        ev.args[1] = a1;
        push_event(ev);
    }
    out->error = 0;
    out->value = 1;
}

// A Kiln native's typed declaration refuses this call (natives_core.h check_typed_args): the answer is set.
bool refused_typed(const Slot& s, NativeResult* out, const ArgSlice* args, const NativeCtx* ctx) {
    const std::vector<KilnNativeArg>* t = s.typed.load(std::memory_order_acquire);
    if (!t || t->empty()) return false;
    const uint8_t e = check_typed_args(t->data(), static_cast<uint32_t>(t->size()), args->data, args->count, ctx->layout,
                                       ctx->layout_count, ctx->data_size);
    if (!e) return false;
    (e == kErrNoConnection ? c_typed_unconnected : c_typed_refused).fetch_add(1, std::memory_order_relaxed);
    out->error = e;
    out->value = 0;
    return true;
}

void dispatch_managed(int slot, NativeResult* out, ArgSlice* args, NativeCtx* ctx) {
    if (refused_typed(g_slots[slot], out, args, ctx)) return;
    const uint64_t key = (static_cast<uint64_t>(ctx->impact_id) << 32) | ctx->site;
    bool queued_new = false;
    uint64_t id = 0;
    if (!bounded_lock(g_mu)) {  // the tick thread holds it (it never does during the step): wait one more step
        out->error = 1;
        out->value = 0;
        return;
    }
    {
        std::lock_guard lock(g_mu, std::adopt_lock);
        auto it = g_calls.find(key);
        if (it != g_calls.end() && it->second.state == 1) {
            out->error = it->second.error;
            out->value = it->second.value;
            g_call_key.erase(it->second.id);
            g_calls.erase(it);
            return;
        }
        if (it == g_calls.end()) {
            id = ++g_call_seq;
            queued_new = true;
        }
    }
    if (queued_new) {
        KilnAbilityEvent ev = new_event(KILN_ABILITY_EV_MANAGED_CALL);
        ev.native_hash = g_slots[slot].hash;
        ev.impact_id = ctx->impact_id;
        ev.program_id = ctx->program_id;
        ev.site = ctx->site;
        ev.self = self_of(ctx->impact_id);
        ev.call_id = id;
        capture_args(ev, args, ctx);
        // Publish the pending call and its event together. If the bounded event-queue
        // wait fails, leave no orphaned call: the VM retries the ECALL next step.
        if (bounded_lock(g_mu)) {
            std::lock_guard lock(g_mu, std::adopt_lock);
            if (!g_calls.count(key)) {
                g_calls[key] = ManagedCall{id, slot, 0, 0, 0, g_step.load(), false};
                g_call_key[id] = key;
                if (push_event(ev)) {
                    c_managed_calls.fetch_add(1, std::memory_order_relaxed);
                } else {
                    g_call_key.erase(id);
                    g_calls.erase(key);
                }
            }
        }
    } else {
        c_managed_suspends.fetch_add(1, std::memory_order_relaxed);
    }
    out->error = 1;  // suspend: the same ECALL runs again next step
    out->value = 0;
}

std::atomic<uint32_t> g_slow_logged{0};


// A sync native (since 0.2.0-dev.29): its fn answers inside the ECALL. No lock, no queue, no suspend; a fault in fn is
// caught by the thunk's guard and answers 0x11.
void dispatch_sync(Slot& s, NativeResult* out, ArgSlice* args, NativeCtx* ctx) {
    const SyncBinding* b = s.sync.load(std::memory_order_seq_cst);
    if (!b || !b->fn || !gate_enter(*b->gate)) {  // taken off, or its owner is retiring: no new entry
        c_dead_calls.fetch_add(1, std::memory_order_relaxed);
        out->error = kErrUnknownNative;
        return;
    }
    t_gate = b->gate;
    if (refused_typed(s, out, args, ctx)) {
        leave_gate();
        return;
    }
    c_sync_calls.fetch_add(1, std::memory_order_relaxed);
    KilnNativeCall c;
    fill_call(c, s, out, args, ctx);
    const int64_t t0 = qpc();
    watch(t0, b->name.c_str(), b->gate->owner);
    b->fn(&c, b->user);
    unwatch();
    leave_gate();
    const int64_t dt = qpc() - t0;
    if (over_budget(dt, g_qpf)) {
        c_sync_slow.fetch_add(1, std::memory_order_relaxed);
        if (g_slow_logged.fetch_add(1, std::memory_order_relaxed) < 5)
            logw("kiln.abilities: sync native %s took %.3f ms (budget 0.2 ms): impact_simulation waits for it", b->name.c_str(),
                 g_qpf > 0 ? static_cast<double>(dt) * 1000.0 / static_cast<double>(g_qpf) : 0.0);
    }
    sync_result(c, &out->error, &out->value);
    if (out->error == kErrInternal) c_sync_faults.fetch_add(1, std::memory_order_relaxed);  // fn caught its own fault
}

void ecs_first(const NativeCtx* ctx);  // the ECS bridge (below)

void dispatch_inner(int slot, NativeResult* out, ArgSlice* args, NativeCtx* ctx) {
    ecs_first(ctx);  // once per world: one load and a compare after that
    Slot& s = g_slots[slot];
    switch (s.kind.load(std::memory_order_acquire)) {
        case SLOT_WRAPPED: dispatch_wrapped(s, out, args, ctx); return;
        case SLOT_LOG: dispatch_log(out, args, ctx); return;
        case SLOT_SIGNAL: dispatch_signal(out, args, ctx); return;
        case SLOT_MANAGED: dispatch_managed(slot, out, args, ctx); return;
        case SLOT_MANAGED_SYNC: dispatch_sync(s, out, args, ctx); return;
        default:
            c_dead_calls.fetch_add(1, std::memory_order_relaxed);
            out->error = kErrUnknownNative;  // the interpreter's "unknown native" code: the instance stops with Error
            return;
    }
}

int seh_dispatch(int slot, NativeResult* out, ArgSlice* args, NativeCtx* ctx) {
    __try {
        dispatch_inner(slot, out, args, ctx);
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 1;
    }
}

std::atomic<uint32_t> g_faults{0};

// A fault caught by a thunk's guard (natives_core.h fault_answer): the game's own function runs at most once per call.
void on_fault(int i, NativeResult* out, ArgSlice* args, NativeCtx* ctx) {
    unwatch();      // a callback that faulted is not inside any more
    leave_gate();
    const bool in_game = t_in_game;
    t_in_game = false;
    const uint32_t k = g_slots[i].kind.load();
    const ThunkKind kind = k == SLOT_WRAPPED ? ThunkKind::Wrapped : k == SLOT_MANAGED_SYNC ? ThunkKind::Sync : ThunkKind::Other;
    NativeFn orig = g_slots[i].original.load();
    const FaultAnswer a = fault_answer(kind, in_game, t_orig_ran, orig != nullptr);
    if (g_faults.fetch_add(1) < 5)
        logw("kiln.abilities: native slot %d (0x%08x, %s) faulted %s", i, g_slots[i].hash, g_slots[i].name,
             in_game ? "inside the game's native: the instance stops"
             : a == FaultAnswer::RunOriginal ? "in Kiln's code: the game's native runs"
             : a == FaultAnswer::KeepResult ? "in Kiln's code after the game's native ran: its result stands"
             : a == FaultAnswer::Internal ? "in its handler: the instance stops (0x11)"
                                          : "in Kiln's code: the instance stops");
    switch (a) {
        case FaultAnswer::RunOriginal: call_fn(orig, out, args, ctx); return;  // outside the guard, as vanilla runs it
        case FaultAnswer::KeepResult: return;
        case FaultAnswer::Internal:
            c_sync_faults.fetch_add(1, std::memory_order_relaxed);
            out->error = kErrInternal;
            out->value = 0;
            return;
        case FaultAnswer::Unknown: out->error = kErrUnknownNative; return;
    }
}

template <int I>
NativeResult* __fastcall thunk(NativeResult* out, ArgSlice* args, NativeCtx* ctx) {
    g_inflight.fetch_add(1, std::memory_order_seq_cst);
    const int32_t outer_stage = t_stage;
    OwnerGate* const outer_gate = t_gate;
    const int outer_inside = t_inside;
    t_gate = nullptr;
    t_inside = -1;
    NativeCtx* const outer_ctx = t_ctx;
    t_ctx = ctx;
    const int outer_slot = t_slot;  // a game native that runs a program inside its call (nested thunks)
    const bool outer_ran = t_orig_ran, outer_in_game = t_in_game;
    t_slot = I;
    t_orig_ran = false;
    t_in_game = false;
    out->error = 0;
    out->zero = 0;
    out->value = 1;
    if (seh_dispatch(I, out, args, ctx) != 0) on_fault(I, out, args, ctx);
    t_stage = outer_stage;
    t_gate = outer_gate;
    t_inside = outer_inside;
    t_ctx = outer_ctx;
    t_slot = outer_slot;
    t_orig_ran = outer_ran;
    t_in_game = outer_in_game;
    g_inflight.fetch_sub(1, std::memory_order_seq_cst);
    return out;
}

template <int... I>
constexpr std::array<NativeFn, sizeof...(I)> make_thunks(std::integer_sequence<int, I...>) {
    return {&thunk<I>...};
}
const auto kThunks = make_thunks(std::make_integer_sequence<int, kSlots>{});

int alloc_slot() {  // g_mu held
    for (int i = 0; i < kSlots; ++i)
        if (g_slots[i].kind.load() == SLOT_FREE) return i;
    return -1;
}

// ---------------------------------------------------------------------------------------- hooks ----------------
kiln::RawHook g_h_register, g_h_create, g_h_run;

uint64_t __fastcall hk_register(uintptr_t registry, uint32_t hash, NativeFn fn, const void* name, uint8_t argc) {
    const uint64_t r = g_h_register.call<uint64_t>(registry, hash, fn, name, argc);
    if (!t_kiln_registering) {
        Captured c{hash, fn, argc, {}};
        struct Sl {
            const char* p;
            uint64_t n;
        } sl{};
        if (name && read_bytes(name, &sl, sizeof sl) && sl.p && sl.n)
            read_bytes(sl.p, c.name, std::min<uint64_t>(sl.n, 32));
        std::lock_guard lock(g_mu);
        auto& list = g_registries[registry];
        if (registry != g_last_registry) {
            g_last_registry = registry;
            if (list.size() >= 100) list.clear();  // a registry address reused by a new world's setup
        }
        bool dup = false;
        for (auto& x : list)
            if (x.hash == hash) {
                x = c;
                dup = true;
            }
        if (!dup) list.push_back(c);
    }
    return r;
}

bool lab_lockhold_armed();  // lab only (below, with lab_lockhold)
void lab_hold_lock_now();

uint32_t __fastcall hk_create(uintptr_t system, uint32_t program_id, uint32_t entity) {
    const uint32_t id = g_h_create.call<uint32_t>(system, program_id, entity);
    if (!id) return id;
    if (lab_lockhold_armed()) lab_hold_lock_now();
    g_system.store(system, std::memory_order_relaxed);
    iw::on_created(system, id);  // the impact pool watch's creation time (lock-free, no allocation)
    if (c_created.fetch_add(1, std::memory_order_relaxed) == 0 || g_first_instance.load(std::memory_order_relaxed) == 0)
        g_first_instance.store(id, std::memory_order_relaxed);
    const uint32_t interest = g_interest.load(std::memory_order_relaxed);
    const bool want_started = (interest >> KILN_ABILITY_EV_IMPACT_STARTED) & 1u;
    const bool want_hold = (interest & KILN_ABILITY_INTEREST_HOLD) != 0;
    const bool want_ended = (interest >> KILN_ABILITY_EV_IMPACT_ENDED) & 1u;
    if (!want_started && !want_hold && !want_ended && g_entity_ovr_count.load(std::memory_order_relaxed) == 0)
        return id;
    bool held = false;
    if (!bounded_lock(g_mu)) return id;  // review P1: no unbounded wait on a job thread; this instance runs unheld
    {
        std::lock_guard lock(g_mu, std::adopt_lock);
        auto it = g_entity_ovr.find(entity);
        if (it != g_entity_ovr.end()) {
            g_impact_ovr[id] = std::move(it->second);
            g_entity_ovr.erase(it);
        }
        if (want_hold && (g_hold_filter.empty() || g_hold_filter.count(program_id))) {
            g_held.insert(id);
            held = true;
            c_held.fetch_add(1, std::memory_order_relaxed);
        }
        if ((want_ended || want_started) && g_tracked.size() < 8192) g_tracked[id] = Tracked{program_id, entity};
        recount_pending();
    }
    if (want_started || held) {
        KilnAbilityEvent ev = new_event(KILN_ABILITY_EV_IMPACT_STARTED);
        ev.impact_id = id;
        ev.program_id = program_id;
        ev.self = entity;
        ev.held = held;
        push_event(ev);
    }
    return id;
}

// Applies config overrides to a live instance (tick thread in Pre, or a job thread at its first run). Returns
// (applied, mismatched).
std::pair<uint32_t, uint32_t> apply_overrides(uint32_t impact_id, const std::vector<KilnConfigOverride>& list) {
    uint32_t applied = 0, mismatched = 0;
    uint8_t* inst = instance(g_system.load(), impact_id);
    if (!inst) return {0, static_cast<uint32_t>(list.size())};
    uint32_t program_id = 0;
    uint8_t* data = nullptr;
    uint64_t data_size = 0;
    if (!read_bytes(inst + E.inst_program, &program_id, 4) || !read_bytes(inst + E.inst_data, &data, 8) ||
        !read_bytes(inst + E.inst_data_size, &data_size, 8) || !data)
        return {0, static_cast<uint32_t>(list.size())};
    const uint8_t* prog = get_program(g_prog_registry.load(), program_id);
    ProgramHeader h{};
    if (!prog || !read_bytes(prog, &h, sizeof h)) return {0, static_cast<uint32_t>(list.size())};
    const uint8_t* layout = h.layout.offset ? prog + offsetof(ProgramHeader, layout) + h.layout.offset : nullptr;
    if (!layout || h.data.count != data_size) return {0, static_cast<uint32_t>(list.size())};  // layout sanity
    for (const auto& o : list) {
        bool any = false, bad = false;
        for (uint32_t i = 0; i < h.layout.count; ++i) {
            ImpactVariable v{};
            if (!read_bytes(layout + i * sizeof v, &v, sizeof v)) break;
            const bool match = (o.match == KILN_OVERRIDE_BY_CONFIG && v.config_id == o.key && o.key != 0) ||
                               (o.match == KILN_OVERRIDE_BY_NAME && v.name == o.key) ||
                               (o.match == KILN_OVERRIDE_BY_INDEX && i == o.key);
            if (!match) continue;
            if (v.size != o.size || static_cast<uint64_t>(v.offset) + v.size > data_size) {
                bad = true;
                continue;
            }
            if (write_bytes(data + v.offset, o.data, v.size)) any = true;
        }
        if (any) ++applied;
        else if (bad || !any) ++mismatched;
    }
    c_ovr_applied.fetch_add(applied, std::memory_order_relaxed);
    c_ovr_mismatch.fetch_add(mismatched, std::memory_order_relaxed);
    return {applied, mismatched};
}

void __fastcall hk_run(uintptr_t system, uint32_t impact_id, uintptr_t iter_ctx) {
    c_runs.fetch_add(1, std::memory_order_relaxed);
    if (!g_native_registry.load(std::memory_order_relaxed) && iter_ctx) {
        uintptr_t reg = 0;
        if (read_bytes(reinterpret_cast<const void*>(iter_ctx + 0x110), &reg, 8) && reg)
            g_native_registry.store(reg, std::memory_order_relaxed);
    }
    if (g_pending.load(std::memory_order_acquire) != 0) {
        Overrides ovr;
        bool have_ovr = false;
        if (!bounded_lock(g_mu)) return;  // review P1: no unbounded wait on a job thread; held or not, it runs next step
        {
            std::lock_guard lock(g_mu, std::adopt_lock);
            if (g_held.count(impact_id) || g_canceled.count(impact_id)) return;  // not this step
            auto it = g_impact_ovr.find(impact_id);
            if (it != g_impact_ovr.end()) {
                ovr = std::move(it->second);
                g_impact_ovr.erase(it);
                have_ovr = true;
                recount_pending();
            }
        }
        if (have_ovr) apply_overrides(impact_id, ovr.list);
    }
    g_h_run.call<void>(system, impact_id, iter_ctx);
}

// ------------------------------------------------------------------------------- world setup and registration ---
uintptr_t g_setup_world = 0;      // the world the registrations below were made for
uintptr_t g_setup_registry = 0;   // the native registry they went into

bool register_slot_in(int slot, uintptr_t registry) {  // tick thread
    Slot& s = g_slots[slot];
    t_kiln_registering = true;
    const bool ok = register_native(registry, s.hash, kThunks[slot], s.name, std::strlen(s.name),
                                    static_cast<uint8_t>(s.argc));
    t_kiln_registering = false;
    if (ok) s.registered_in = registry;
    return ok;
}

int wrap_slot(uint32_t hash, uint32_t* argc_out = nullptr);  // g_mu held

// ---- lab only: impact_simulation onto the game's workers (review P1's worker rows) ----
// With kiln/control/impact.unpin present when the service started, on a dedicated server of the lab's build
// (f0d824be…), each Pre clears the world's pin byte (storage + 0x3d17f8, copied from world config + 0x23; docs/re/
// world.md "Parallel selection and affinity"): in the parallel path (a player, or kiln/control/systems.parallel's
// forced window) CHUNKED programs then get no world-thread affinity, so a native's handler runs on a worker. Nothing
// else ever writes it; without the file nothing is checked.
bool g_lab_unpin = false;
std::filesystem::path g_lab_lockhold;  // lab only: kiln/control/impact.lockhold at start (see lab_lockhold)
bool g_lab_lockhold_on = false;
std::atomic<int> g_lockhold_armed{0};

// Lab only (review round 2): with kiln/control/impact.lockhold present at start, a file impact.lockhold.now (its text: the
// milliseconds, at most 2000) arms a hold: the next createImpactInstance visit starts a thread that holds g_mu that long,
// so that visit and the step's later createImpactInstance / runImpact visits meet a held lock and take their bounded
// give-up paths (lock_bypass). In the step itself: the tick thread would wait for the lock in Pre or Post instead.
void lab_lockhold() {
    if (!g_lab_lockhold_on) return;
    std::error_code ec;
    const auto now = g_lab_lockhold / "impact.lockhold.now";
    if (!std::filesystem::exists(now, ec)) return;
    int ms = 200;
    {
        std::ifstream in(now);
        in >> ms;
    }
    std::filesystem::remove(now, ec);
    g_lockhold_armed.store(std::clamp(ms, 1, 2000));
    logi("kiln.abilities: LAB (kiln/control/impact.lockhold): the next instance's creation holds Kiln's lock for %d ms", ms);
}
bool lab_lockhold_armed() { return g_lab_lockhold_on && g_lockhold_armed.load(std::memory_order_relaxed) > 0; }
void lab_hold_lock_now() {  // a job thread, inside the step
    const int ms = g_lockhold_armed.exchange(0);
    if (ms <= 0) return;
    std::atomic<bool> held{false};
    std::thread([ms, &held] {
        std::lock_guard lock(g_mu);
        held.store(true);
        Sleep(static_cast<DWORD>(ms));
    }).detach();
    while (!held.load()) SwitchToThread();  // `held` is not touched once it is true
    logw("kiln.abilities: LAB (kiln/control/impact.lockhold): Kiln's lock is held for %d ms by a thread of its own", ms);
}
int64_t g_world_storage = -1;  // sigdb world.storage
int64_t g_pin_off = -1;        // sigdb abilities/impact.pin (feature abilities.ecs_pin): the pin byte in the world's storage

void lab_unpin() {
    if (!g_lab_unpin || g_world_storage <= 0 || g_pin_off <= 0) return;
    const uintptr_t w = g_api->server_world();
    if (!w) return;
    auto* pin = reinterpret_cast<uint8_t*>(w + static_cast<uintptr_t>(g_world_storage) + static_cast<uintptr_t>(g_pin_off));
    uint8_t v = 0;
    if (!read_bytes(pin, &v, 1) || v == 0) return;
    const uint8_t zero = 0;
    static bool said = false;
    if (write_bytes(pin, &zero, 1) && !said) {
        said = true;
        logw("kiln.abilities: LAB (kiln/control/impact.unpin): the world's pin byte was %u and is cleared: CHUNKED programs, "
             "impact_simulation among them, can run on the game's workers in the parallel path", v);
    }
}

// ---- the ECS bridge (since 0.2.0-dev.29, chapter 06c 9.1): NativeContext reads and writes other entities ----
// kiln.world's foreign view of impact_simulation (the wraps' view algorithm over its descriptor), made in the first Pre
// that has the server world's directory, once per world session. A Kiln native's call holds that program's iterator
// (NativeCtx+0x30): the first call of each view checks it (ecs_verdict); a mismatch turns the bridge off for that world
// (native_ecs_read/write answer KILN_ERR_UNAVAILABLE; the natives themselves still run).
const KilnWorldApi* g_world_api = nullptr;
struct EcsView {
    KilnForeignLayout l{};
    int32_t system_slot = -1;
};
std::atomic<const EcsView*> g_ecs{nullptr};          // the live world's view (old ones are kept: one per world session)
std::atomic<const EcsView*> g_ecs_checked{nullptr};  // the view the first call checked
std::atomic<uint32_t> g_ecs_state{KILN_NATIVE_ECS_PENDING}, g_ecs_reason{KILN_NATIVE_ECS_REASON_NONE};
uint64_t g_ecs_tried = UINT64_MAX;  // tick thread: the session a view was asked for
constexpr const char* kImpactSimulation = "impact_simulation";

bool world_foreign() {
    return g_world_api && g_world_api->header.size >= offsetof(KilnWorldApi, foreign_attr_changed) + sizeof(void*) &&
           g_world_api->foreign_layout && g_world_api->foreign_slot && g_world_api->foreign_check &&
           g_world_api->foreign_read && g_world_api->foreign_write && g_world_api->foreign_attr_changed &&
           g_world_api->game_program_find && g_world_api->game_world_session;
}

void ecs_off(uint32_t reason) {
    g_ecs_reason.store(reason);
    g_ecs_state.store(KILN_NATIVE_ECS_OFF);
}

// Pre (tick thread): the view of this world session's impact_simulation.
void ecs_pre() {
    if (!world_foreign()) {
        if (g_ecs_state.load() == KILN_NATIVE_ECS_PENDING && g_api->server_world()) ecs_off(KILN_NATIVE_ECS_REASON_NO_WORLD_API);
        return;
    }
    const uint64_t session = g_world_api->game_world_session(KILN_WORLD_SERVER);
    if (session == g_ecs_tried) return;
    if (g_world_api->game_program_find(KILN_WORLD_SERVER, KilnStr{kImpactSimulation, std::strlen(kImpactSimulation)}) < 0) return;
    g_ecs_tried = session;
    auto* v = new EcsView{};
    v->l.size = sizeof v->l;
    const KilnResult rc = g_world_api->foreign_layout(KILN_WORLD_SERVER, KilnStr{kImpactSimulation, std::strlen(kImpactSimulation)}, &v->l);
    if (rc != KILN_OK) {
        logw("kiln.abilities: NativeContext's reads and writes of other entities are off in this world: kiln.world has no "
             "view of impact_simulation (%s)", rc == KILN_ERR_UNAVAILABLE ? v->l.why : "not found");
        delete v;
        g_ecs.store(nullptr);
        ecs_off(KILN_NATIVE_ECS_REASON_LAYOUT);
        return;
    }
    const char* sys = "impact::ImpactSystem";
    v->system_slot = g_world_api->foreign_slot(&v->l, KILN_GAME_LIST_SYS_WRITE, KilnStr{sys, std::strlen(sys)});
    const char* attr = "impact::AttributeRegistry";
    const int32_t attr_slot = g_world_api->foreign_slot(&v->l, KILN_GAME_LIST_SYS_READ, KilnStr{attr, std::strlen(attr)});
    const uint32_t* c = v->l.counts;
    logi("kiln.abilities: impact_simulation's view (kiln.world, session %llu): ctx + tpl %u, read %u, write %u, singletons "
         "%u, other_tpl %u, other %u, events %u/%u (%u bytes); impact::ImpactSystem at qword %d (+0x%x), "
         "impact::AttributeRegistry at qword %d",
         static_cast<unsigned long long>(session), c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7], v->l.iter_size,
         v->system_slot, v->system_slot > 0 ? v->system_slot * 8 : 0, attr_slot);
    g_ecs_state.store(KILN_NATIVE_ECS_PENDING);
    g_ecs_reason.store(KILN_NATIVE_ECS_REASON_NONE);
    g_ecs.store(v, std::memory_order_release);
}

// The serial condition now (ecs_serial): the world's pin byte and the calling thread.
uint32_t ecs_serial_now() {
    const uintptr_t w = g_api->server_world();
    uint8_t pin = 0;
    const bool known = g_world_storage > 0 && g_pin_off > 0 && w &&
                       read_bytes(reinterpret_cast<const void*>(w + static_cast<uintptr_t>(g_world_storage) + static_cast<uintptr_t>(g_pin_off)), &pin, 1);
    return ecs_serial(known, pin, on_tick());
}

const char* ecs_reason_text(uint32_t reason) {
    switch (reason) {
        case KILN_NATIVE_ECS_REASON_COUNTS: return "the job's group counts are not kiln.world's view";
        case KILN_NATIVE_ECS_REASON_REGISTRY: return "the iterator's impact::ImpactSystem qword is not the captured registry";
        case KILN_NATIVE_ECS_REASON_NOT_SERIAL:
            return "impact_simulation does not run serially on the world's thread here (its pin is clear, or a call came on another "
                   "thread): two slices could touch one entity at once";
        case KILN_NATIVE_ECS_REASON_PIN_UNKNOWN: return "the world's pin of impact_simulation to its thread cannot be read on this build";
        default: return "see above";
    }
}

// The first Kiln native call of each view (any thread inside impact_simulation): the self-check.
void ecs_first(const NativeCtx* ctx) {
    const EcsView* v = g_ecs.load(std::memory_order_acquire);
    if (!v || g_ecs_checked.load(std::memory_order_acquire) == v || g_ecs_checked.exchange(v) == v) return;
    const auto* iter = reinterpret_cast<const uint64_t*>(ctx ? ctx->iter_ctx : 0);
    uint32_t job[8] = {};
    const bool counts_ok = iter && g_world_api->foreign_check(&v->l, iter, job) == KILN_OK;
    uint64_t at = 0;
    if (iter && v->system_slot > 0) read_bytes(iter + v->system_slot, &at, 8);
    // the registry Kiln's natives went into at the world's setup (one of those the RegisterNative hook captured)
    const auto registry = static_cast<uint64_t>(g_native_registry.load(std::memory_order_relaxed));
    uint32_t reason = ecs_verdict(counts_ok, v->system_slot, at, registry);
    const uint32_t serial = ecs_serial_now();
    if (reason == KILN_NATIVE_ECS_REASON_NONE) reason = serial;
    logi("kiln.abilities: NativeContext self-check: impact_simulation serial on the world's thread: %s",
         serial == KILN_NATIVE_ECS_REASON_NONE ? "yes (the pin is set, the call is on the world's thread)" : ecs_reason_text(serial));
    logi("kiln.abilities: NativeContext self-check, first native call in this world: the job's group counts are %s the "
         "view's (tpl %u, read %u, write %u, singletons %u, other_tpl %u, other %u, events %u/%u)",
         counts_ok ? "" : "NOT", job[0], job[1], job[2], job[3], job[4], job[5], job[6], job[7]);
    logi("kiln.abilities: NativeContext self-check: the iterator's qword %d (+0x%x) is 0x%llx, the captured native registry "
         "0x%llx: %s", v->system_slot, v->system_slot > 0 ? v->system_slot * 8 : 0, static_cast<unsigned long long>(at),
         static_cast<unsigned long long>(registry), at == registry && registry ? "the same" : "DIFFERENT");
    if (reason == KILN_NATIVE_ECS_REASON_NONE) {
        g_ecs_state.store(KILN_NATIVE_ECS_ON);
        logi("kiln.abilities: NativeContext's reads and writes of other entities are on in this world (abilities.natives.ecs)");
    } else {
        ecs_off(reason);
        logw("kiln.abilities: NativeContext's reads and writes of other entities are off for this world: %s", ecs_reason_text(reason));
    }
}

// The call's iterator and the view, when the bridge is on for this world.
const EcsView* ecs_for(const KilnNativeCall* call, const uint64_t** iter) {
    if (t_slot < 0 || !call || call->size < sizeof(KilnNativeCall) || g_ecs_state.load(std::memory_order_acquire) != KILN_NATIVE_ECS_ON) return nullptr;
    const EcsView* v = g_ecs.load(std::memory_order_acquire);
    const auto* ctx = static_cast<const NativeCtx*>(call->engine[2]);
    if (!v || !ctx || ctx != t_ctx || !ctx->iter_ctx || g_ecs_checked.load(std::memory_order_acquire) != v) return nullptr;
    // the serial condition at every access: a cleared pin or a call off the world's thread switches it off for the world
    if (const uint32_t serial = ecs_serial_now(); serial != KILN_NATIVE_ECS_REASON_NONE) {
        g_ecs_reason.store(serial);
        if (g_ecs_state.exchange(KILN_NATIVE_ECS_OFF) != KILN_NATIVE_ECS_OFF)
            logw("kiln.abilities: NativeContext's reads and writes of other entities are off for this world: %s", ecs_reason_text(serial));
        return nullptr;
    }
    *iter = reinterpret_cast<const uint64_t*>(ctx->iter_ctx);
    return v;
}

KilnResult ecs_access_result(KilnResult rc) {
    if (rc == KILN_ERR_INTERNAL || rc == KILN_ERR_UNAVAILABLE) {
        g_ecs_reason.store(KILN_NATIVE_ECS_REASON_FAULT);
        if (g_ecs_state.exchange(KILN_NATIVE_ECS_OFF) != KILN_NATIVE_ECS_OFF)
            logw("kiln.abilities: foreign ECS access failed; the bridge is off for this world (a write may already have begun)");
    }
    return rc;
}

KilnResult KILN_CALL t_native_ecs_read(const KilnNativeCall* call, KilnStr component, uint32_t entity, void* out, uint32_t size) {
    if (!out || !size) return KILN_ERR_INVALID_ARGUMENT;
    const uint64_t* iter = nullptr;
    const EcsView* v = ecs_for(call, &iter);
    if (!v) return KILN_ERR_UNAVAILABLE;
    uint32_t have = 0;
    KilnResult rc = KILN_OK;
    const void* p = g_world_api->foreign_read(&v->l, iter, component, entity, &have, &rc);
    if (rc != KILN_OK) return ecs_access_result(rc);
    if (!p) return KILN_ERR_NOT_FOUND;
    if (size > have) return KILN_ERR_INVALID_ARGUMENT;  // the whole component, or its first `size` bytes
    return read_bytes(p, out, size) ? KILN_OK : ecs_access_result(KILN_ERR_INTERNAL);
}

KilnResult KILN_CALL t_native_ecs_write(KilnNativeCall* call, KilnStr component, uint32_t entity, const void* value,
                                        uint32_t size, uint32_t flags, void* before, void* after) {
    if (!value || !size) return KILN_ERR_INVALID_ARGUMENT;
    const uint64_t* iter = nullptr;
    const EcsView* v = ecs_for(call, &iter);
    if (!v) return KILN_ERR_UNAVAILABLE;
    // the size first, through a read: a write marks the component written before it can be refused
    uint32_t have = 0;
    KilnResult rc = KILN_OK;
    const void* r = g_world_api->foreign_read(&v->l, iter, component, entity, &have, &rc);
    if (rc != KILN_OK) return ecs_access_result(rc);
    if (!r) return KILN_ERR_NOT_FOUND;
    if (have != size) return KILN_ERR_INVALID_ARGUMENT;
    if (flags & KILN_NATIVE_ECS_ATTRIBUTE) {
        uint8_t old_header[8], new_header[8];
        if (size < sizeof old_header) return KILN_ERR_INVALID_ARGUMENT;
        if (!read_bytes(r, old_header, sizeof old_header) || !read_bytes(value, new_header, sizeof new_header))
            return ecs_access_result(KILN_ERR_INTERNAL);
        if (std::memcmp(old_header, new_header, sizeof old_header) != 0) return KILN_ERR_INVALID_ARGUMENT;
    }
    void* p = g_world_api->foreign_write(&v->l, iter, component, entity, &have, &rc);
    if (rc != KILN_OK) return ecs_access_result(rc);
    if (!p || have != size) return KILN_ERR_NOT_FOUND;
    if (before && !read_bytes(p, before, size)) return ecs_access_result(KILN_ERR_INTERNAL);
    if (!write_bytes(p, value, size)) return ecs_access_result(KILN_ERR_INTERNAL);
    if (flags & KILN_NATIVE_ECS_ATTRIBUTE) {
        const KilnResult f = g_world_api->foreign_attr_changed(&v->l, iter, p);
        if (f != KILN_OK && f != KILN_ERR_INVALID_ARGUMENT) return ecs_access_result(f);
    }
    if (after && !read_bytes(p, after, size)) return ecs_access_result(KILN_ERR_INTERNAL);
    return KILN_OK;
}

// ---- the watchdog (review P1) ----
// A handler that is still inside after a second is named once per call, from a thread of its own: while it is
// inside, the world waits for it, so nothing on the world's thread could say why the server stopped. Started at the
// first sync native or attachment (nothing runs for a player whose mods use neither).
void KILN_CALL stop_watchdog(void*) { g_watchdog_on.store(false, std::memory_order_release); }

void watchdog_loop() {
    while (g_watchdog_on.load(std::memory_order_acquire)) {
        Sleep(250);
        if (!g_watchdog_on.load(std::memory_order_acquire)) break;
        g_inside.scan(qpc(), g_qpf, [](const char* what, KilnOwner owner, int64_t inside) {
            c_watchdog_reports.fetch_add(1, std::memory_order_relaxed);
            logw("kiln.abilities: the handler of native %s (owner %llu) has been inside for %.1f s: impact_simulation and "
                 "the server's step wait for it (a native handler must not block)", what ? what : "?",
                 static_cast<unsigned long long>(owner), g_qpf > 0 ? static_cast<double>(inside) / static_cast<double>(g_qpf) : 0.0);
        });
    }
}
void start_watchdog() {
    bool off = false;
    if (g_qpf > 0 && g_watchdog_on.compare_exchange_strong(off, true)) std::thread(watchdog_loop).detach();
}

// The native map of a registry (keen hash map at registry + natives_map: capacity +0x10, count +0x54) has room for one
// more key (natives_core.h registry_has_room: the game takes 128 of its 256). An unreadable map has none.
bool registry_room(uintptr_t registry) {
    if (!registry) return false;
    uint64_t cap = 0;
    uint32_t count = 0;
    const auto* map = reinterpret_cast<const uint8_t*>(registry + E.natives_map);
    if (!read_bytes(map + 0x10, &cap, 8) || !read_bytes(map + 0x54, &count, 4)) return false;
    return registry_has_room(count, cap);
}

// ---- attachments (since 0.2.0-dev.29) ----
std::vector<Attachment> g_attachments;     // every attachment, all natives (g_mu)
std::vector<const AttachSet*> g_retired;   // sets swapped out, freed in a Pre with no thunk running (g_mu)
uint64_t g_attach_seq = 0;                 // handles (g_mu)

// Publishes the attachments of `hash` to its wrapped slot (g_mu held). No slot yet (no world): published at setup.
void publish_set(uint32_t hash) {
    auto it = g_slot_of.find(hash);
    if (it == g_slot_of.end() || g_slots[it->second].kind.load() != SLOT_WRAPPED) return;
    AttachSet built = build_set(g_attachments, hash);
    const AttachSet* next = built.empty() ? nullptr : new AttachSet(std::move(built));
    const AttachSet* old = g_slots[it->second].attach.exchange(next, std::memory_order_seq_cst);
    if (old) g_retired.push_back(old);
}

// Pre: no impact_simulation slice runs, so no thunk can still read a set that was swapped out.
void free_retired() {
    std::vector<const AttachSet*> r;
    std::vector<const std::vector<KilnNativeArg>*> rt;
    std::vector<const SyncBinding*> rb;
    {
        std::lock_guard lock(g_mu);
        // a deferred owner retirement finishes once its gate is quiet (review P1)
        finish_quiet(g_deferred_owners, g_step.load(), [](KilnOwner) {}, [](OwnerGate* g, uint64_t steps) {
            logi("kiln.abilities: owner %llu retired %llu step(s) late: its last native call left",
                 static_cast<unsigned long long>(g->owner), static_cast<unsigned long long>(steps));
        });
        // A reader increments g_inflight before loading the binding, before entering its owner gate.
        // Keep the binding through that admission window as well as through the callback.
        for (auto it = g_retired_bindings.begin(); it != g_retired_bindings.end();) {
            if (g_inflight.load(std::memory_order_seq_cst) != 0 ||
                ((*it)->gate && (*it)->gate->inflight.load(std::memory_order_seq_cst) != 0)) {
                ++it;
                continue;
            }
            rb.push_back(*it);
            it = g_retired_bindings.erase(it);
        }
        if (g_inflight.load(std::memory_order_seq_cst) == 0) {
            r.swap(g_retired);
            rt.swap(g_retired_typed);
        }
    }
    for (const AttachSet* p : r) delete p;
    for (const auto* p : rt) delete p;
    for (const SyncBinding* p : rb) delete p;
}

// The program `id` calls `hash` (at `site`, 0 = any): 1 yes, 0 no, -1 the program is not in the registry. Tick thread.
int program_calls(uintptr_t preg, uint32_t id, uint32_t hash, uint32_t site) {
    const uint8_t* prog = get_program(preg, id);
    ProgramHeader h{};
    if (!prog || !read_bytes(prog, &h, sizeof h)) return -1;
    const std::pair<const BlobArray*, size_t> arrays[2] = {{&h.code, offsetof(ProgramHeader, code)},
                                                           {&h.shutdown, offsetof(ProgramHeader, shutdown)}};
    for (const auto& [arr, field] : arrays) {
        if (!arr->count || !arr->offset || arr->count > 65535) continue;
        std::vector<uint32_t> words(arr->count);
        if (!read_bytes(prog + field + arr->offset, words.data(), words.size() * 4)) continue;
        if (code_calls(words.data(), words.size(), hash, site)) return 1;
    }
    return 0;
}

// Once the world's programs are in (and for a new world): attachments that name a program are checked against it; one
// whose program or site is not there stays off, with a log line. Tick thread.
void check_attachments(uintptr_t preg) {
    std::vector<Attachment> named;
    {
        std::lock_guard lock(g_mu);
        for (const auto& a : g_attachments)
            if (a.program) named.push_back(a);
    }
    std::vector<std::pair<uint64_t, int>> results;
    for (const auto& a : named) results.emplace_back(a.id, program_calls(preg, a.program, a.hash, a.site));
    std::lock_guard lock(g_mu);
    std::unordered_set<uint32_t> touched;
    for (const auto& [id, r] : results)
        for (auto& a : g_attachments) {
            if (a.id != id) continue;
            const bool on = r == 1;
            if (a.active != on) touched.insert(a.hash);
            // said once per change (the check runs again whenever a program is registered)
            if (!on && (!a.checked || a.active))
                logw("kiln.abilities: attachment %llu on native 0x%08x stays off: %s 0x%08x%s", static_cast<unsigned long long>(id),
                     a.hash, r < 0 ? "no program" : "the native is not called by program", a.program,
                     r < 0 ? " in the ImpactRegistry" : a.site ? " at that site" : "");
            else if (on && a.checked && !a.active)
                logi("kiln.abilities: attachment %llu on native 0x%08x is on: program 0x%08x is in now", static_cast<unsigned long long>(id),
                     a.hash, a.program);
            a.active = on;
            a.checked = true;
        }
    for (uint32_t h : touched) publish_set(h);
}

// Builds the vanilla native table for the captured registry and puts Kiln's slots into it. Tick thread.
void setup_natives(uintptr_t registry) {
    std::vector<Captured> caps;
    {
        std::lock_guard lock(g_mu);
        if (registry == g_cap_registry) caps = g_captured;
    }
    std::vector<int> to_register;
    {
        std::lock_guard lock(g_mu);
        g_vanilla_argc.clear();
        for (const auto& c : caps) g_vanilla_argc[c.hash] = c.argc;
        // the default observed natives: damage, buff, spawn
        for (uint32_t h : {KILN_NATIVE_APPLY_DAMAGE, KILN_NATIVE_APPLY_BUFF, KILN_NATIVE_SPAWN_ENTITY}) {
            const int s = wrap_slot(h);
            if (s >= 0) g_slots[s].observe.store(true);
        }
        // attachments made before the world had its natives: their slots now
        std::unordered_set<uint32_t> hashes;
        for (const auto& a : g_attachments) hashes.insert(a.hash);
        for (uint32_t h : hashes) {
            if (wrap_slot(h) < 0) {
                for (auto& a : g_attachments)
                    if (a.hash == h && a.active) {
                        a.active = false;
                        logw("kiln.abilities: attachment %llu stays off: 0x%08x is not a native the game registered (or no free slot)",
                             static_cast<unsigned long long>(a.id), h);
                    }
                continue;
            }
            publish_set(h);
        }
        for (int i = 0; i < kSlots; ++i) {
            const uint32_t k = g_slots[i].kind.load();
            if (k == SLOT_FREE || g_slots[i].registered_in == registry) continue;
            if (k == SLOT_WRAPPED) {
                // the original of this world: the captured fn (the thunk itself is never an original)
                for (const auto& c : caps)
                    if (c.hash == g_slots[i].hash) g_slots[i].original.store(c.fn);
            }
            to_register.push_back(i);
        }
    }
    int ok = 0, bad = 0;
    for (int i : to_register) {
        // a wrap re-registers a key the game has; every other slot adds one, and the map keeps its last empty entry
        if (g_slots[i].kind.load() != SLOT_WRAPPED && !registry_room(registry)) {
            logw("kiln.abilities: native %s not registered: the native registry is full", g_slots[i].name);
            ++bad;
            continue;
        }
        (register_slot_in(i, registry) ? ok : bad)++;
    }
    logi("kiln.abilities: native registry %p: %zu vanilla natives captured; %d Kiln slot(s) registered%s", (void*)registry,
         caps.size(), ok, bad ? " (some failed: see above)" : "");
    if (bad) logw("kiln.abilities: %d native registration(s) failed (registry full?)", bad);
}

int wrap_slot(uint32_t hash, uint32_t* argc_out) {  // g_mu held; finds or creates the WRAPPED slot of a vanilla native
    auto it = g_slot_of.find(hash);
    if (it != g_slot_of.end()) return g_slots[it->second].kind.load() == SLOT_WRAPPED ? it->second : -1;
    NativeFn fn = nullptr;
    uint8_t argc = 0;
    const char* name = nullptr;
    for (const auto& c : g_captured)
        if (c.hash == hash) {
            fn = c.fn;
            argc = c.argc;
            name = c.name;
        }
    if (!fn) return -1;  // not a vanilla native (or a null one: debugDraw*)
    const int s = alloc_slot();
    if (s < 0) return -1;
    Slot& sl = g_slots[s];
    sl.hash = hash;
    sl.argc = argc;
    sl.original.store(fn);
    std::snprintf(sl.name, sizeof sl.name, "%s", name ? name : "");
    sl.owner = 0;
    sl.registered_in = 0;
    sl.kind.store(SLOT_WRAPPED);
    g_slot_of[hash] = s;
    if (argc_out) *argc_out = argc;
    return s;
}

// What the verifier knows of `hash`'s arguments (g_mu held): a typed declaration, or "a Kiln native without types".
TypedArgs typed_of(uint32_t hash) {
    TypedArgs t;
    if (g_vanilla_argc.count(hash)) return t;  // the game's own: never a mod's declaration
    if (auto it = g_typed.find(hash); it != g_typed.end()) {
        t.args = it->second.data();
        t.count = static_cast<uint32_t>(it->second.size());
        return t;
    }
    // declared by argc only and not registered yet (registration drops it from g_declared): the W_ARGTYPE warning. A
    // registered native without types (Register, RegisterSync) is how managed natives always worked: no warning
    if (g_declared.count(hash)) t.untyped_kiln = true;
    return t;
}

int argc_of(uint32_t hash) {  // g_mu held
    if (auto it = g_vanilla_argc.find(hash); it != g_vanilla_argc.end()) return static_cast<int>(it->second);
    if (auto it = g_slot_of.find(hash); it != g_slot_of.end()) {
        const uint32_t k = g_slots[it->second].kind.load();
        if (k != SLOT_FREE && k != SLOT_DEAD) return static_cast<int>(g_slots[it->second].argc);
    }
    if (auto it = g_declared.find(hash); it != g_declared.end()) return static_cast<int>(it->second);
    return -1;
}

// ---- programs ----
struct KProgram {
    uint32_t id;
    uint8_t guid[16];
    uint8_t* blob;  // permanent copy (VirtualAlloc, never freed: running instances read it)
    size_t len;
    KilnOwner owner;
    uintptr_t inserted_in = 0;
    std::string source;
};
std::vector<KProgram> g_programs;  // inserted (g_mu)
struct PendingProgram {
    std::vector<uint8_t> bytes;
    KilnOwner owner;
    std::string source;
    std::vector<uint32_t> unknown;
};
std::vector<PendingProgram> g_waiting;  // valid, waiting for natives or for the registry (g_mu)

// The game's own programs: the ImpactRegistryResource's list (c4933fae-…), by id. Kiln never inserts one of them.
std::unordered_set<uint32_t> g_vanilla_programs;  // (g_mu)
std::atomic<bool> g_programs_ready{false};         // the game filled its ImpactRegistry in this world
uint32_t g_last_count = 0;
uint64_t g_count_stable_since = 0;
constexpr uint32_t kTypeImpactRegistryResource = 0x64958357u;  // FNV-1a("keen::ImpactRegistryResource")

void load_vanilla_list() {  // tick thread
    if (!g_assets || !g_assets->resource_list || !g_assets->resource_read) return;
    size_t n = 0;
    g_assets->resource_list(kTypeImpactRegistryResource, nullptr, 0, &n);
    std::vector<KilnAssetKey> keys(n + 4);
    size_t got = 0;
    if (g_assets->resource_list(kTypeImpactRegistryResource, keys.data(), keys.size(), &got) != KILN_OK) return;
    std::unordered_set<uint32_t> ids;
    for (size_t k = 0; k < std::min(got, keys.size()); ++k) {
        size_t len = 0;
        g_assets->resource_read(&keys[k], nullptr, 0, &len);
        if (len < 8 || len > (4u << 20)) continue;
        std::vector<uint8_t> b(len);
        if (g_assets->resource_read(&keys[k], b.data(), b.size(), &len) != KILN_OK) continue;
        int32_t off;
        uint32_t count;
        std::memcpy(&off, b.data(), 4);
        std::memcpy(&count, b.data() + 4, 4);
        if (off <= 0 || static_cast<uint64_t>(off) + count * 16ull > len) continue;
        for (uint32_t i = 0; i < count; ++i) ids.insert(fnv1a(b.data() + off + i * 16, 16));
    }
    std::lock_guard lock(g_mu);
    g_vanilla_programs.swap(ids);
}

// The impact pool watch's label: is a program in the game's own ImpactRegistry list (known false while there is none).
bool program_is_vanilla(uint32_t id, bool* known) {
    std::lock_guard lock(g_mu);
    *known = !g_vanilla_programs.empty();
    return g_vanilla_programs.count(id) != 0;
}

uintptr_t prog_registry() {
    const uintptr_t sys = g_system.load();
    uintptr_t reg = 0;
    if (sys && read_bytes(reinterpret_cast<const void*>(sys + E.system_registry), &reg, 8)) return reg;
    return 0;
}


// Verify + insert. Tick thread. Returns KILN_OK, or INVALID_ARGUMENT with err set; *waiting = queued for natives.
KilnResult add_program(const uint8_t* blob, size_t len, KilnOwner owner, const std::string& source, uint32_t* out_id,
                       std::string& err, bool* waiting) {
    *waiting = false;
    VerifyResult r;
    {
        std::lock_guard lock(g_mu);
        r = verify_program(blob, len, [](uint32_t h) { return argc_of(h); }, [](uint32_t h) { return typed_of(h); });
    }
    if (out_id) *out_id = r.id;
    for (const auto& w : r.warnings) logw("kiln.abilities: program from %s: %s", source.c_str(), w.c_str());
    if (!r.ok) {
        err = r.error;
        c_refused.fetch_add(1);
        return KILN_ERR_INVALID_ARGUMENT;
    }
    const uintptr_t reg = g_prog_registry.load();
    std::lock_guard lock(g_mu);
    if (g_vanilla_programs.count(r.id)) {
        err = "the GUID belongs to a program the game has (clone it to a new GUID)";
        c_refused.fetch_add(1);
        return KILN_ERR_INVALID_ARGUMENT;
    }
    if (!reg || !g_programs_ready.load()) {
        // the game fills its ImpactRegistry while the world loads: wait for it
        PendingProgram w{std::vector<uint8_t>(blob, blob + len), owner, source, {}};
        g_waiting.push_back(std::move(w));
        *waiting = true;
        return KILN_OK;
    }
    for (const auto& p : g_programs)
        if (p.id == r.id) {
            err = "a Kiln program with this GUID is registered already";
            return KILN_ERR_INVALID_ARGUMENT;
        }
    if (get_program(reg, r.id)) {
        err = "the GUID belongs to a program the game has (clone it to a new GUID)";
        c_refused.fetch_add(1);
        return KILN_ERR_INVALID_ARGUMENT;
    }
    if (!r.unknown_natives.empty()) {
        PendingProgram w{std::vector<uint8_t>(blob, blob + len), owner, source, r.unknown_natives};
        for (auto& x : g_waiting)
            if (x.bytes.size() >= 20 && std::memcmp(x.bytes.data() + 4, blob + 4, 16) == 0) {
                x = std::move(w);
                *waiting = true;
                return KILN_OK;
            }
        g_waiting.push_back(std::move(w));
        *waiting = true;
        return KILN_OK;
    }
    auto* mem = static_cast<uint8_t*>(VirtualAlloc(nullptr, len, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!mem) {
        err = "out of memory";
        return KILN_ERR_INTERNAL;
    }
    std::memcpy(mem, blob, len);
    bool existed = false;
    if (!program_map_insert(reg, r.id, mem, &existed) || existed) {
        VirtualFree(mem, 0, MEM_RELEASE);
        err = existed ? "the GUID is in the registry already" : "the ImpactRegistry map is full or unreadable";
        return KILN_ERR_INTERNAL;
    }
    KProgram p{r.id, {}, mem, len, owner, reg, source};
    std::memcpy(p.guid, r.guid, 16);
    g_programs.push_back(std::move(p));
    g_programs_added.store(true, std::memory_order_release);  // attachments naming it are checked in the next Pre
    logi("kiln.abilities: program %s (0x%08x) registered from %s: %u code words, max stack depth %d",
         guid_text(r.guid).c_str(), r.id, source.c_str(), reinterpret_cast<const ProgramHeader*>(blob)->code.count,
         r.max_depth);
    return KILN_OK;
}

// Retries programs that waited for natives. Tick thread.
void retry_waiting() {
    std::vector<PendingProgram> w;
    {
        std::lock_guard lock(g_mu);
        if (g_waiting.empty()) return;
        w.swap(g_waiting);
    }
    for (auto& p : w) {
        uint32_t id = 0;
        std::string err;
        bool waiting = false;
        const KilnResult rc = add_program(p.bytes.data(), p.bytes.size(), p.owner, p.source, &id, err, &waiting);
        if (rc != KILN_OK) logw("kiln.abilities: program from %s refused: %s", p.source.c_str(), err.c_str());
    }
}

// Re-inserts Kiln's programs into a new world's registry and lists kiln.assets' ImpactProgram resources.
uint32_t refresh_programs() {  // tick thread
    const uintptr_t reg = g_prog_registry.load();
    if (!reg || !g_programs_ready.load()) return 0;
    {
        std::lock_guard lock(g_mu);
        for (auto& p : g_programs) {
            if (p.inserted_in == reg) continue;
            bool existed = false;
            if (program_map_insert(reg, p.id, p.blob, &existed)) p.inserted_in = reg;
            else logw("kiln.abilities: program 0x%08x could not be re-inserted", p.id);
        }
    }
    uint32_t added = 0;
    if (g_assets && g_assets->resource_list && g_assets->resource_read) {
        size_t count = 0;
        g_assets->resource_list(kTypeImpactProgram, nullptr, 0, &count);
        std::vector<KilnAssetKey> keys(count + 16);
        size_t got = 0;
        if (g_assets->resource_list(kTypeImpactProgram, keys.data(), keys.size(), &got) == KILN_OK) {
            keys.resize(std::min(got, keys.size()));
            for (const auto& k : keys) {
                const uint32_t id = fnv1a(k.guid, 16);
                {
                    std::lock_guard lock(g_mu);
                    if (g_vanilla_programs.count(id)) continue;  // the game's own (kiln.assets lists those too)
                }
                if (get_program(reg, id)) continue;  // already in the registry
                size_t len = 0;
                g_assets->resource_read(&k, nullptr, 0, &len);
                if (len == 0 || len > (16u << 20)) continue;
                std::vector<uint8_t> bytes(len);
                if (g_assets->resource_read(&k, bytes.data(), bytes.size(), &len) != KILN_OK) continue;
                std::string err;
                bool waiting = false;
                uint32_t pid = 0;
                const std::string src = "kiln.assets " + guid_text(k.guid);
                const KilnResult rc = add_program(bytes.data(), len, 0, src, &pid, err, &waiting);
                if (rc == KILN_OK && !waiting) ++added;
                else if (rc == KILN_OK) logi("kiln.abilities: program %s waits for natives", src.c_str());
                else logw("kiln.abilities: program %s refused: %s", src.c_str(), err.c_str());
            }
        }
    }
    retry_waiting();
    return added;
}

// ------------------------------------------------------------------------------------------- Pre ---------------
std::vector<KilnAbilityEvent> g_batch;  // tick thread

// Lab self-test (kiln/data/kiln.abilities/selftest exists): every program in the registry through the verifier.
size_t seh_extent(const uint8_t* prog) {
    __try {
        const auto* h = reinterpret_cast<const ProgramHeader*>(prog);
        size_t end = sizeof(ProgramHeader);
        const BlobArray* arrs[5] = {&h->code, &h->shutdown, &h->layout, &h->data, &h->query_calls};
        const size_t elem[5] = {4, 4, sizeof(ImpactVariable), 1, 4};
        for (int i = 0; i < 5; ++i) {
            if (!arrs[i]->count || !arrs[i]->offset) continue;
            const int64_t start = reinterpret_cast<const uint8_t*>(arrs[i]) - prog + arrs[i]->offset;
            const int64_t e = start + static_cast<int64_t>(arrs[i]->count * elem[i]);
            if (start < 0 || e > (16 << 20)) return 0;
            end = std::max(end, static_cast<size_t>(e));
        }
        if (h->layout.count && h->layout.offset) {
            const uint8_t* l = reinterpret_cast<const uint8_t*>(&h->layout) + h->layout.offset;
            for (uint32_t i = 0; i < h->layout.count; ++i) {
                const auto* v = reinterpret_cast<const ImpactVariable*>(l + i * sizeof(ImpactVariable));
                if (!v->dbg_len || !v->dbg_off) continue;
                const int64_t s0 = reinterpret_cast<const uint8_t*>(&v->dbg_off) - prog + v->dbg_off;
                end = std::max(end, static_cast<size_t>(s0 + v->dbg_len));
            }
        }
        volatile uint8_t probe = prog[end - 1];  // the whole extent must be readable
        (void)probe;
        return end;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

void selftest_programs(uintptr_t preg) {
    char path[1024];
    size_t n = 0;
    if (g_api->path(KILN_PATH_DATA, path, sizeof path, &n) != KILN_OK) return;
    std::error_code ec;  // the data folder is UTF-8: build the path from UTF-8, not from the ANSI code page
    const std::u8string data(reinterpret_cast<const char8_t*>(path), n);
    if (!std::filesystem::exists(std::filesystem::path(data) / "kiln.abilities" / "selftest", ec)) return;
    std::vector<uint32_t> ids(4096);
    std::vector<const uint8_t*> progs(4096);
    const size_t total = program_map_snapshot(preg, ids.data(), progs.data(), ids.size());
    if (total == SIZE_MAX) {
        logw("kiln.abilities selftest: the program map is unreadable");
        return;
    }
    int ok = 0, bad = 0, max_depth = 0;
    std::string first_bad;
    for (size_t i = 0; i < std::min(total, ids.size()); ++i) {
        const size_t len = seh_extent(progs[i]);
        if (!len) {
            ++bad;
            continue;
        }
        std::vector<uint8_t> copy(progs[i], progs[i] + len);
        VerifyResult r;
        {
            std::lock_guard lock(g_mu);
            r = verify_program(copy.data(), copy.size(), [](uint32_t h) { return argc_of(h); });
        }
        if (r.ok && r.unknown_natives.empty()) {
            ++ok;
            max_depth = std::max(max_depth, r.max_depth);
        } else {
            ++bad;
            if (first_bad.size() < 600) {
                char b[200];
                std::snprintf(b, sizeof b, "%s0x%08x: %s", first_bad.empty() ? "" : "; ", ids[i],
                              r.ok ? "unknown natives" : r.error.c_str());
                first_bad += b;
            }
        }
    }
    (bad ? logw : logi)("kiln.abilities selftest: %d/%zu programs in the registry pass the verifier (max stack depth %d)%s%s",
                        ok, total, max_depth, bad ? "; refused: " : "", first_bad.c_str());
}

void world_setup(uintptr_t world) {
    const char* n = "impact::ImpactSystem";
    const uintptr_t sys = g_api->singleton(world, KilnStr{n, std::strlen(n)});
    if (sys) g_system.store(sys);
    const uintptr_t preg = prog_registry();
    if (preg) g_prog_registry.store(preg);
    uintptr_t nreg = 0;
    {
        std::lock_guard lock(g_mu);
        const uintptr_t seen = g_native_registry.load();  // from runImpact's context: certainly the server world's
        if (seen && g_registries.count(seen)) nreg = seen;
        else if (g_registries.size() == 1) nreg = g_registries.begin()->first;  // a dedicated server has one
        if (nreg && nreg != g_cap_registry) {
            g_cap_registry = nreg;
            g_captured = g_registries[nreg];
        }
    }
    if (!sys || !preg) return;  // not yet: try again next Pre
    if (g_have_natives && nreg && nreg != g_setup_registry) {
        g_native_registry.store(nreg);
        setup_natives(nreg);
        g_setup_registry = nreg;
    }
    if (g_setup_world != world) {
        g_setup_world = world;
        MapView m{};
        uint32_t count = 0;
        if (program_map(preg, &m)) read_bytes(m.count, &count, 4);
        load_vanilla_list();
        g_programs_ready.store(false);
        g_last_count = 0;
        g_count_stable_since = g_step.load();
        size_t vanilla = 0;
        {
            std::lock_guard lock(g_mu);
            vanilla = g_vanilla_programs.size();
        }
        logi("kiln.abilities: world %p: ImpactSystem %p, ImpactRegistry %p (%u programs so far, capacity %llu); the game "
             "lists %zu programs; Kiln programs are inserted once the game has loaded them",
             (void*)world, (void*)sys, (void*)preg, count, static_cast<unsigned long long>(m.cap), vanilla);
    }
}

// Pre: the game fills its ImpactRegistry while the world loads. Kiln inserts its programs once the registry holds
// the game's whole list (or, without the list, once its count stayed the same for 50 steps).
void check_programs_ready() {
    if (g_programs_ready.load() || !g_have_programs) return;
    const uintptr_t preg = g_prog_registry.load();
    MapView m{};
    uint32_t count = 0;
    if (!preg || !program_map(preg, &m) || !read_bytes(m.count, &count, 4)) return;
    size_t vanilla = 0;
    {
        std::lock_guard lock(g_mu);
        vanilla = g_vanilla_programs.size();
    }
    if (count != g_last_count) {
        g_last_count = count;
        g_count_stable_since = g_step.load();
    }
    const bool ready = vanilla ? count >= vanilla : (count > 0 && g_step.load() >= g_count_stable_since + 50);
    if (!ready) return;
    g_programs_ready.store(true);
    if (g_have_natives && g_setup_registry) selftest_programs(preg);
    const uint32_t added = refresh_programs();
    check_attachments(preg);
    logi("kiln.abilities: the game's ImpactRegistry holds %u programs (step %llu); %u Kiln program(s) from kiln.assets "
         "registered", count, static_cast<unsigned long long>(g_step.load()), added);
}

void expire() {  // tick thread, g_mu held
    const uint64_t step = g_step.load();
    for (auto it = g_entity_ovr.begin(); it != g_entity_ovr.end();)
        it = it->second.expires <= step ? g_entity_ovr.erase(it) : std::next(it);
    for (auto it = g_canceled.begin(); it != g_canceled.end();)
        it = it->second <= step ? g_canceled.erase(it) : std::next(it);
    for (auto it = g_calls.begin(); it != g_calls.end();) {
        if (it->second.step + 400 < step) {
            g_call_key.erase(it->second.id);
            it = g_calls.erase(it);
        } else {
            ++it;
        }
    }
}

uint32_t root_of(uintptr_t world, uint32_t entity) {
    uint64_t size = 0;
    auto* h = static_cast<const uint8_t*>(component(world, kImpactHierarchy, entity, &size));
    uint32_t root = 0;
    if (h && size >= 8) read_bytes(h + 4, &root, 4);
    return root;
}

uint32_t KILN_CALL seh_listener(KilnAbilityListenerFn fn, KilnAbilityEvent* ev, uint32_t n, void* user, bool* ok) {
    __try {
        *ok = true;
        return fn(ev, n, user);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *ok = false;
        return 0;
    }
}

bool g_layout_checked = false;

// Once: the instance layout (program, entity, data, size) of the first created instance against its program.
void check_layout() {
    const uint32_t id = g_first_instance.load();
    if (g_layout_checked || !id) return;
    g_layout_checked = true;
    uint8_t* inst = instance(g_system.load(), id);
    uint32_t pid = 0, ent = 0;
    uint8_t* data = nullptr;
    uint64_t size = 0;
    if (!inst) {
        logi("kiln.abilities: first instance %u is gone before the layout check", id);
        g_layout_checked = false;
        g_first_instance.store(0);
        return;
    }
    read_bytes(inst + E.inst_program, &pid, 4);
    read_bytes(inst + E.inst_entity, &ent, 4);
    read_bytes(inst + E.inst_data, &data, 8);
    read_bytes(inst + E.inst_data_size, &size, 8);
    const uint8_t* prog = get_program(g_prog_registry.load(), pid);
    ProgramHeader h{};
    const bool ok = prog && read_bytes(prog, &h, sizeof h) && h.data.count == size && data;
    (ok ? logi : logw)("kiln.abilities: instance layout %s: impact %u program 0x%08x entity %u data %p size %llu "
                       "(program data %u bytes, %u variables)",
                       ok ? "ok" : "MISMATCH (instance reads/writes and overrides will fail)", id, pid, ent,
                       (void*)data, static_cast<unsigned long long>(size), h.data.count, h.layout.count);
}

void holds_pre(uintptr_t world);  // held sequences (below, with sequence_play)
void holds_clear();

void KILN_CALL on_pre(const KilnTickInfo* tick, void*) {
    g_step.store(tick->step);
    free_retired();
    lab_unpin();
    lab_lockhold();
    ecs_pre();
    // review P1: a program registered after the readiness check (a mod's, at any time) gets its attachments checked
    if (g_programs_ready.load() && g_programs_added.exchange(false, std::memory_order_acq_rel))
        if (const uintptr_t preg = g_prog_registry.load()) check_attachments(preg);
    const uintptr_t world = g_api->server_world();
    if (!world) return;
    g_world.store(world);
    if (world != g_setup_world || !g_setup_registry) world_setup(world);
    if (g_setup_world) {
        check_layout();
        check_programs_ready();
    }
    holds_pre(world);

    // overrides for instances created last step: apply now (the game copied its configs already)
    std::vector<std::pair<uint32_t, Overrides>> ovr;
    {
        std::lock_guard lock(g_mu);
        expire();
        for (auto& kv : g_impact_ovr) ovr.emplace_back(kv.first, std::move(kv.second));
        g_impact_ovr.clear();
        recount_pending();
    }
    for (auto& [id, o] : ovr) apply_overrides(id, o.list);

    // collect events
    g_batch.clear();
    {
        std::lock_guard lock(g_ev_mu);
        g_batch.swap(g_events);
    }
    const uint32_t interest = g_interest.load();
    if ((interest >> KILN_ABILITY_EV_IMPACT_ENDED) & 1u) {
        std::vector<std::pair<uint32_t, Tracked>> gone;
        {
            std::lock_guard lock(g_mu);
            for (auto it = g_tracked.begin(); it != g_tracked.end();) {
                uint8_t* inst = instance(g_system.load(), it->first);
                uint32_t pid = 0;
                if (!inst || !read_bytes(inst + E.inst_program, &pid, 4) || pid != it->second.program) {
                    gone.emplace_back(*it);
                    it = g_tracked.erase(it);
                } else {
                    ++it;
                }
            }
        }
        for (auto& [id, t] : gone) {
            KilnAbilityEvent ev = new_event(KILN_ABILITY_EV_IMPACT_ENDED);
            ev.impact_id = id;
            ev.program_id = t.program;
            ev.self = t.self;
            g_batch.push_back(ev);
        }
    } else {
        std::lock_guard lock(g_mu);
        g_tracked.clear();
    }
    for (auto& ev : g_batch)
        if (ev.self && ev.kind != KILN_ABILITY_EV_IMPACT_ENDED) ev.root = root_of(world, ev.self);

    if (g_listener) {
        bool ok = true;
        const uint32_t next = seh_listener(g_listener, g_batch.data(), static_cast<uint32_t>(g_batch.size()),
                                           g_listener_user, &ok);
        if (!ok) {
            logw("kiln.abilities: the listener faulted; removed");
            g_listener = nullptr;
            g_interest.store(0);
        } else {
            g_interest.store(next);
            c_delivered.fetch_add(g_batch.size());
        }
    } else {
        g_interest.store(0);
    }

    // held instances: canceled ones are destroyed, the rest run from this step on
    std::vector<uint32_t> destroy;
    {
        std::lock_guard lock(g_mu);
        for (auto& ev : g_batch) {
            if (ev.kind == KILN_ABILITY_EV_IMPACT_STARTED && ev.held && ev.canceled) {
                g_canceled[ev.impact_id] = g_step.load() + 8;
                if (ev.self > 1023) destroy.push_back(ev.self);
            }
            if (ev.kind == KILN_ABILITY_EV_MANAGED_CALL) {
                auto k = g_call_key.find(ev.call_id);
                if (k != g_call_key.end()) {
                    auto& c = g_calls[k->second];
                    if (c.state == 0) {  // nobody answered: complete with 0 so the program goes on
                        c.state = 1;
                        c.error = 0;
                        c.value = 0;
                    }
                }
            }
        }
        g_held.clear();
        recount_pending();
    }
    for (uint32_t e : destroy)
        if (g_spawn) g_spawn->destroy(e);
}

void KILN_CALL on_world(const KilnWorldEvent* ev, void*) {
    if (ev->kind != KILN_WORLD_SERVER) return;
    if (ev->event == KILN_WORLD_STOPPING) {
        // review P1: the world's state goes only with no Kiln native call inside (bounded; reported when not)
        const int64_t end = now_ns() + kRetireWaitNs;
        while (g_inflight.load(std::memory_order_seq_cst) != 0 && now_ns() < end) SwitchToThread();
        if (const uint32_t n = g_inflight.load(std::memory_order_seq_cst))
            logw("kiln.abilities: the server world stops with %u Kiln native call(s) inside: bindings and sets stay until "
                 "they leave", n);
        else logi("kiln.abilities: the server world stops with no Kiln native call inside");
        // Jobs have drained: no final-step event may be delivered in the next world. Do not nest this lock with g_mu
        // (managed-call producers can enqueue while holding g_mu).
        {
            std::lock_guard event_lock(g_ev_mu);
            g_events.clear();
        }
        g_batch.clear();
        holds_clear();
        g_ecs.store(nullptr, std::memory_order_release);  // the next world gets a view of its own
        g_ecs_state.store(KILN_NATIVE_ECS_PENDING);
        g_ecs_reason.store(KILN_NATIVE_ECS_REASON_NONE);
        std::lock_guard lock(g_mu);
        g_cap_registry = 0;
        g_held.clear();
        g_canceled.clear();
        g_impact_ovr.clear();
        g_entity_ovr.clear();
        g_tracked.clear();
        g_calls.clear();
        g_call_key.clear();
        recount_pending();
        g_system.store(0);
        g_prog_registry.store(0);
        g_native_registry.store(0);
        g_setup_world = 0;
        g_setup_registry = 0;
        g_programs_ready.store(false);
        for (auto& s : g_slots) s.registered_in = 0;
    }
}

// ------------------------------------------------------------------------------------------- table -------------
KilnResult KILN_CALL t_status(KilnAbilitiesStatus* out) {
    if (!out || out->size < sizeof(KilnAbilitiesStatus)) return KILN_ERR_INVALID_ARGUMENT;
    std::memset(reinterpret_cast<uint8_t*>(out) + 4, 0, sizeof *out - 4);
    out->natives = g_have_natives && g_setup_registry != 0;
    out->programs = g_have_programs && g_prog_registry.load() != 0;
    out->instances = g_have_instances;
    out->attributes = g_have_attrs;
    {
        std::lock_guard lock(g_mu);
        out->vanilla_natives = static_cast<uint32_t>(g_vanilla_argc.size());
        uint32_t k = 0;
        for (auto& s : g_slots)
            if (kiln_kind(s.kind.load())) ++k;
        out->kiln_natives = k;
        out->programs_kiln = static_cast<uint32_t>(g_programs.size());
        out->held_now = static_cast<uint32_t>(g_held.size());
    }
    const uintptr_t nreg = g_setup_registry;
    if (nreg) {
        uint64_t cap = 0;
        read_bytes(reinterpret_cast<const void*>(nreg + E.natives_map + 0x10), &cap, 8);
        out->native_capacity = static_cast<uint32_t>(cap);
    }
    MapView m{};
    if (program_map(g_prog_registry.load(), &m)) read_bytes(m.count, &out->programs_total, 4);
    out->interest = g_interest.load();
    out->instances_created = c_created;
    out->instances_held = c_held;
    out->overrides_applied = c_ovr_applied;
    out->overrides_mismatched = c_ovr_mismatch;
    out->runs = c_runs;
    out->native_calls = c_native_calls;
    out->native_calls_observed = c_observed;
    out->signals = c_signals;
    out->managed_calls = c_managed_calls;
    out->managed_suspends = c_managed_suspends;
    out->events_delivered = c_delivered;
    out->events_dropped = c_dropped;
    out->wrap_calls = c_wrap;
    out->programs_refused = c_refused;
    out->step = g_step.load();
    return KILN_OK;
}

KilnResult KILN_CALL t_set_listener(KilnAbilityListenerFn fn, void* user) {
    g_listener_user = user;
    g_listener = fn;
    if (!fn) g_interest.store(0);
    return KILN_OK;
}

KilnResult KILN_CALL t_hold_filter(const uint32_t* ids, uint32_t count) {
    if (count && !ids) return KILN_ERR_INVALID_ARGUMENT;
    std::lock_guard lock(g_mu);
    g_hold_filter.clear();
    for (uint32_t i = 0; i < count; ++i) g_hold_filter.insert(ids[i]);
    return KILN_OK;
}

// Live instance -> (data, size, layout, layout count). Tick thread.
bool instance_view(uint32_t impact_id, uint8_t** data, uint64_t* size, const uint8_t** layout, uint32_t* count,
                   uint32_t* program_id = nullptr, uint32_t* self = nullptr, uint32_t* flags = nullptr) {
    uint8_t* inst = instance(g_system.load(), impact_id);
    if (!inst) return false;
    uint32_t pid = 0, e = 0;
    uint8_t fl = 0;
    if (!read_bytes(inst + E.inst_program, &pid, 4) || !read_bytes(inst + E.inst_entity, &e, 4) ||
        !read_bytes(inst + E.inst_data, data, 8) || !read_bytes(inst + E.inst_data_size, size, 8) ||
        !read_bytes(inst + E.inst_flags, &fl, 1))
        return false;
    const uint8_t* prog = get_program(g_prog_registry.load(), pid);
    ProgramHeader h{};
    if (!prog || !read_bytes(prog, &h, sizeof h) || h.data.count != *size) return false;
    *layout = h.layout.offset ? prog + offsetof(ProgramHeader, layout) + h.layout.offset : nullptr;
    *count = h.layout.count;
    if (program_id) *program_id = pid;
    if (self) *self = e;
    if (flags) *flags = fl;
    return *layout != nullptr;
}

void fill_var(KilnImpactVar* out, uint32_t index, const uint8_t* entry) {
    ImpactVariable v{};
    std::memset(out, 0, sizeof *out);
    if (!read_bytes(entry, &v, sizeof v)) return;
    out->index = index;
    out->name_hash = v.name;
    out->config_id = v.config_id;
    out->type = v.type;
    out->value_size = v.size;
    out->offset = v.offset;
    if (v.dbg_len && v.dbg_off) read_bytes(entry + 16 + v.dbg_off, out->name, std::min<uint32_t>(v.dbg_len, 63));
}

KilnResult KILN_CALL t_instance_vars(uint32_t impact_id, KilnImpactVar* out, uint32_t cap, uint32_t* count) {
    if (!on_tick()) return KILN_ERR_WRONG_THREAD;
    uint8_t* data;
    uint64_t size;
    const uint8_t* layout;
    uint32_t n;
    if (!instance_view(impact_id, &data, &size, &layout, &n)) return KILN_ERR_NOT_FOUND;
    if (count) *count = n;
    for (uint32_t i = 0; i < n && i < cap && out; ++i) fill_var(&out[i], i, layout + i * sizeof(ImpactVariable));
    return cap < n ? KILN_ERR_BUFFER_TOO_SMALL : KILN_OK;
}

KilnResult KILN_CALL t_instance_read(uint32_t impact_id, uint32_t var, void* dst, uint32_t cap, uint32_t* len) {
    if (!on_tick()) return KILN_ERR_WRONG_THREAD;
    uint8_t* data;
    uint64_t size;
    const uint8_t* layout;
    uint32_t n;
    if (!instance_view(impact_id, &data, &size, &layout, &n)) return KILN_ERR_NOT_FOUND;
    if (var >= n) return KILN_ERR_INVALID_ARGUMENT;
    ImpactVariable v{};
    if (!read_bytes(layout + var * sizeof v, &v, sizeof v) || v.offset + v.size > size) return KILN_ERR_INTERNAL;
    if (len) *len = v.size;
    if (!dst || cap < v.size) return KILN_ERR_BUFFER_TOO_SMALL;
    return read_bytes(data + v.offset, dst, v.size) ? KILN_OK : KILN_ERR_INTERNAL;
}

KilnResult KILN_CALL t_instance_write(uint32_t impact_id, uint32_t var, const void* src, uint32_t len) {
    if (!on_tick()) return KILN_ERR_WRONG_THREAD;
    uint8_t* data;
    uint64_t size;
    const uint8_t* layout;
    uint32_t n;
    if (!instance_view(impact_id, &data, &size, &layout, &n)) return KILN_ERR_NOT_FOUND;
    if (var >= n || !src) return KILN_ERR_INVALID_ARGUMENT;
    ImpactVariable v{};
    if (!read_bytes(layout + var * sizeof v, &v, sizeof v) || v.offset + v.size > size) return KILN_ERR_INTERNAL;
    if (len != v.size) return KILN_ERR_INVALID_ARGUMENT;
    return write_bytes(data + v.offset, src, v.size) ? KILN_OK : KILN_ERR_INTERNAL;
}

KilnResult KILN_CALL t_instance_info(uint32_t impact_id, KilnEntity* self, uint32_t* program_id, uint32_t* flags) {
    if (!on_tick()) return KILN_ERR_WRONG_THREAD;
    uint8_t* data;
    uint64_t size;
    const uint8_t* layout;
    uint32_t n;
    if (!instance_view(impact_id, &data, &size, &layout, &n, program_id, self, flags)) return KILN_ERR_NOT_FOUND;
    return KILN_OK;
}

KilnResult KILN_CALL t_entity_overrides(KilnEntity entity, const KilnConfigOverride* list, uint32_t count,
                                        uint32_t steps) {
    if (!entity || (count && !list) || count > 64) return KILN_ERR_INVALID_ARGUMENT;
    if (!g_have_instances) return KILN_ERR_UNAVAILABLE;
    std::lock_guard lock(g_mu);
    if (count == 0) {
        g_entity_ovr.erase(entity);
    } else {
        Overrides o;
        o.list.assign(list, list + count);
        for (auto& x : o.list) x.size = std::min<uint32_t>(x.size, sizeof x.data);
        o.expires = g_step.load() + (steps ? steps : 50);
        g_entity_ovr[entity] = std::move(o);
    }
    recount_pending();
    return KILN_OK;
}

uint32_t KILN_CALL t_program_count() {
    MapView m{};
    uint32_t n = 0;
    if (program_map(g_prog_registry.load(), &m)) read_bytes(m.count, &n, 4);
    return n;
}

KilnResult KILN_CALL t_program_info(uint32_t id, KilnImpactProgramInfo* out) {
    if (!out || out->size < sizeof(KilnImpactProgramInfo)) return KILN_ERR_INVALID_ARGUMENT;
    const uint8_t* prog = get_program(g_prog_registry.load(), id);
    ProgramHeader h{};
    if (!prog || !read_bytes(prog, &h, sizeof h)) return KILN_ERR_NOT_FOUND;
    out->id = h.id;
    std::memcpy(out->guid, h.guid, 16);
    out->code_words = h.code.count;
    out->shutdown_words = h.shutdown.count;
    out->variables = h.layout.count;
    out->data_bytes = h.data.count;
    out->stack_size = h.stack_size;
    out->streams = h.streams;
    std::lock_guard lock(g_mu);
    out->kiln = 0;
    for (const auto& p : g_programs) out->kiln |= p.id == id;
    return KILN_OK;
}

KilnResult KILN_CALL t_program_vars(uint32_t id, KilnImpactVar* out, uint32_t cap, uint32_t* count) {
    const uint8_t* prog = get_program(g_prog_registry.load(), id);
    ProgramHeader h{};
    if (!prog || !read_bytes(prog, &h, sizeof h) || !h.layout.offset) return KILN_ERR_NOT_FOUND;
    const uint8_t* layout = prog + offsetof(ProgramHeader, layout) + h.layout.offset;
    if (count) *count = h.layout.count;
    for (uint32_t i = 0; i < h.layout.count && i < cap && out; ++i) fill_var(&out[i], i, layout + i * sizeof(ImpactVariable));
    return cap < h.layout.count ? KILN_ERR_BUFFER_TOO_SMALL : KILN_OK;
}

void put_error(char* buf, size_t cap, const std::string& s) {
    if (!buf || !cap) return;
    const size_t n = std::min(cap - 1, s.size());
    std::memcpy(buf, s.data(), n);
    buf[n] = 0;
}

KilnResult KILN_CALL t_program_register(KilnOwner owner, const void* blob, size_t len, uint32_t* out_id, char* error,
                                        size_t error_cap) {
    if (!on_tick()) return KILN_ERR_WRONG_THREAD;
    if (!g_have_programs) return KILN_ERR_UNAVAILABLE;
    if (!blob || !len) return KILN_ERR_INVALID_ARGUMENT;
    std::string err;
    bool waiting = false;
    const KilnResult rc = add_program(static_cast<const uint8_t*>(blob), len, owner, "a mod (program_register)", out_id,
                                      err, &waiting);
    if (rc == KILN_OK && waiting)
        err = g_programs_ready.load() ? "waiting for natives that are not registered or declared yet"
                                      : "waiting for the game to load its ImpactRegistry";
    put_error(error, error_cap, err);
    return rc;
}

KilnResult KILN_CALL t_program_refresh(uint32_t* added) {
    if (!on_tick()) return KILN_ERR_WRONG_THREAD;
    if (!g_have_programs || !g_prog_registry.load() || !g_programs_ready.load()) return KILN_ERR_UNAVAILABLE;
    const uint32_t n = refresh_programs();
    if (added) *added = n;
    return KILN_OK;
}

void fill_native(KilnNativeInfo* out, uint32_t hash, uint32_t argc, uint32_t kind, uintptr_t fn, const char* name) {
    out->hash = hash;
    out->argc = argc;
    out->kind = kind;
    out->fn = fn;
    std::snprintf(out->name, sizeof out->name, "%s", name);
}

uint32_t KILN_CALL t_native_count() {
    std::lock_guard lock(g_mu);
    uint32_t n = static_cast<uint32_t>(g_captured.size());
    for (auto& s : g_slots)
        if (kiln_kind(s.kind.load())) ++n;
    return n;
}

KilnResult KILN_CALL t_native_at(uint32_t index, KilnNativeInfo* out) {
    if (!out || out->size < sizeof(KilnNativeInfo)) return KILN_ERR_INVALID_ARGUMENT;
    std::lock_guard lock(g_mu);
    if (index < g_captured.size()) {
        const auto& c = g_captured[index];
        auto it = g_slot_of.find(c.hash);
        const bool wrapped = it != g_slot_of.end() && g_slots[it->second].kind.load() == SLOT_WRAPPED;
        fill_native(out, c.hash, c.argc, wrapped ? KILN_NATIVE_KIND_WRAPPED : KILN_NATIVE_KIND_VANILLA,
                    reinterpret_cast<uintptr_t>(c.fn), c.name);
        return KILN_OK;
    }
    uint32_t i = static_cast<uint32_t>(g_captured.size());
    for (int s = 0; s < kSlots; ++s) {
        const uint32_t k = g_slots[s].kind.load();
        if (!kiln_kind(k)) continue;
        if (i++ == index) {
            fill_native(out, g_slots[s].hash, g_slots[s].argc,
                        k == SLOT_MANAGED ? KILN_NATIVE_KIND_MANAGED
                        : k == SLOT_MANAGED_SYNC ? KILN_NATIVE_KIND_SYNC : KILN_NATIVE_KIND_BUILTIN,
                        reinterpret_cast<uintptr_t>(kThunks[s]), g_slots[s].name);
            return KILN_OK;
        }
    }
    return KILN_ERR_NOT_FOUND;
}

KilnResult KILN_CALL t_native_find(uint32_t hash, KilnNativeInfo* out) {
    const uint32_t n = t_native_count();
    KilnNativeInfo tmp;
    for (uint32_t i = 0; i < n; ++i) {
        tmp.size = sizeof tmp;
        if (t_native_at(i, &tmp) == KILN_OK && tmp.hash == hash) {
            if (!out || out->size < sizeof tmp) return KILN_ERR_INVALID_ARGUMENT;
            *out = tmp;
            return KILN_OK;
        }
    }
    return KILN_ERR_NOT_FOUND;
}

KilnResult KILN_CALL t_native_declare(uint32_t hash, uint32_t argc) {
    if (argc > 31) return KILN_ERR_INVALID_ARGUMENT;
    {
        std::lock_guard lock(g_mu);
        if (g_vanilla_argc.count(hash)) return KILN_ERR_INVALID_ARGUMENT;
        g_declared[hash] = argc;
    }
    if (on_tick()) retry_waiting();
    return KILN_OK;
}

// since 0.2.0-dev.29 (review P1, see the table)
KilnResult KILN_CALL t_native_state(uint32_t hash, uint32_t* out) {
    if (!out) return KILN_ERR_INVALID_ARGUMENT;
    std::lock_guard lock(g_mu);
    *out = KILN_NATIVE_STATE_NONE;
    auto it = g_slot_of.find(hash);
    if (it == g_slot_of.end()) return KILN_OK;
    const Slot& s = g_slots[it->second];
    const uint32_t k = s.kind.load();
    if (k == SLOT_DEAD) *out = KILN_NATIVE_STATE_DEAD;
    else if (!kiln_kind(k)) return KILN_OK;
    else if (!g_setup_registry) *out = KILN_NATIVE_STATE_WAITING;
    else *out = s.registered_in == g_setup_registry ? KILN_NATIVE_STATE_LIVE : KILN_NATIVE_STATE_REFUSED;
    return KILN_OK;
}

KilnResult KILN_CALL t_native_attach_state(KilnHandle h, uint32_t* out) {
    if (!out) return KILN_ERR_INVALID_ARGUMENT;
    std::lock_guard lock(g_mu);
    *out = KILN_NATIVE_ATTACH_GONE;
    for (const auto& a : g_attachments)
        if (a.id == h) *out = !a.checked ? KILN_NATIVE_ATTACH_PENDING : a.active ? KILN_NATIVE_ATTACH_ACTIVE : KILN_NATIVE_ATTACH_OFF;
    return KILN_OK;
}

// since 0.2.0-dev.29 (see the table)
KilnResult KILN_CALL t_native_declare_typed(uint32_t hash, uint32_t argc, const KilnNativeArg* args) {
    if (argc > 31 || (argc && !args)) return KILN_ERR_INVALID_ARGUMENT;
    {
        std::lock_guard lock(g_mu);
        if (g_vanilla_argc.count(hash)) return KILN_ERR_INVALID_ARGUMENT;
        for (const auto& c : g_captured)
            if (c.hash == hash) return KILN_ERR_INVALID_ARGUMENT;
        g_typed[hash].assign(args, args + argc);
        auto it = g_slot_of.find(hash);
        if (it == g_slot_of.end()) {
            g_declared[hash] = argc;  // the verifier's argc until the native registers
        } else if (mod_kind(g_slots[it->second].kind.load())) {
            Slot& s = g_slots[it->second];
            if (s.argc != argc)
                logw("kiln.abilities: native %s registered with argc %u is declared with %u arguments", s.name, s.argc, argc);
            if (const auto* old = s.typed.exchange(new std::vector<KilnNativeArg>(args, args + argc))) g_retired_typed.push_back(old);
        }
    }
    if (on_tick() && t_slot < 0) retry_waiting();
    return KILN_OK;
}

// A Kiln native in a slot (built-ins, managed, sync). Refusals set *refusal (KILN_NATIVE_REFUSED_*) and why. A new key
// goes into the live registry now (on the tick thread); before the first world it goes in at the world's setup.
int add_kiln_native(KilnOwner owner, std::string_view name, uint32_t argc, uint32_t kind, uint32_t* out_hash,
                    std::string& why, uint32_t* refusal = nullptr, KilnNativeWrapFn fn = nullptr, void* user = nullptr) {
    const uint32_t hash = fnv1a(name.data(), name.size());
    if (out_hash) *out_hash = hash;
    auto refuse = [&](uint32_t code, const char* text) {
        if (refusal) *refusal = code;
        why = text;
        return -1;
    };
    int slot;
    bool fresh = false;
    {
        std::lock_guard lock(g_mu);
        if (g_vanilla_argc.count(hash)) return refuse(KILN_NATIVE_REFUSED_VANILLA, "collides with a vanilla native");
        for (const auto& c : g_captured)
            if (c.hash == hash) return refuse(KILN_NATIVE_REFUSED_VANILLA, "collides with a vanilla native");
        auto it = g_slot_of.find(hash);
        if (it != g_slot_of.end()) {
            Slot& s = g_slots[it->second];
            if (s.kind.load() != SLOT_DEAD) return refuse(KILN_NATIVE_REFUSED_TAKEN, "a Kiln native with this name is registered");
            slot = it->second;  // re-registering a native that was taken off: reuse its slot (same hash, same key)
        } else {
            slot = alloc_slot();
            if (slot < 0) return refuse(KILN_NATIVE_REFUSED_NO_SLOT, "no free native slot (Kiln has 160)");
            fresh = true;
        }
        if (fresh && g_setup_registry && !registry_room(g_setup_registry))
            return refuse(KILN_NATIVE_REFUSED_REGISTRY_FULL, "native registry full (the game takes 128 natives, 107 of them its own)");
        Slot& s = g_slots[slot];
        if (fresh) s.hash = hash;
        // A revived hash with a new argc: the engine's entry must be written again (the VM pops the registry's argc).
        // The same argc keeps the entry as it is: no registration the game could refuse once its map is full.
        if (!fresh && s.argc != argc) s.registered_in = 0;
        s.argc = argc;
        s.owner = owner;
        s.original.store(nullptr);
        if (fresh) std::snprintf(s.name, sizeof s.name, "%.*s", static_cast<int>(name.size()), name.data());
        retire_binding(s.sync.exchange(fn ? new SyncBinding{fn, user, g_gates.of(owner), std::string(name)} : nullptr, std::memory_order_seq_cst));
        if (auto t = g_typed.find(hash); t != g_typed.end()) {  // declared before it registered
            if (const auto* old = s.typed.exchange(new std::vector<KilnNativeArg>(t->second))) g_retired_typed.push_back(old);
        }
        s.kind.store(kind, std::memory_order_release);
        g_slot_of[hash] = slot;
        g_declared.erase(hash);
    }
    if (g_setup_registry && g_slots[slot].registered_in != g_setup_registry && !register_slot_in(slot, g_setup_registry)) {
        std::lock_guard lock(g_mu);  // the game said no to the new key: give the slot back
        Slot& s = g_slots[slot];
        s.kind.store(fresh ? SLOT_FREE : SLOT_DEAD, std::memory_order_release);
        retire_binding(s.sync.exchange(nullptr, std::memory_order_seq_cst));
        s.owner = 0;
        if (fresh) g_slot_of.erase(hash);
        return refuse(KILN_NATIVE_REFUSED_REGISTRY_FULL, "the game's native registry refused it (full)");
    }
    return slot;
}

KilnResult KILN_CALL t_native_register(KilnOwner owner, KilnStr name, uint32_t argc, uint32_t* out_hash) {
    if (!on_tick() || t_slot >= 0) return KILN_ERR_WRONG_THREAD;
    if (!g_have_natives) return KILN_ERR_UNAVAILABLE;
    const std::string_view n(name.ptr ? name.ptr : "", name.len);
    if (argc > 31 || !valid_native_name(n) || n.rfind("kiln.", 0) == 0) return KILN_ERR_INVALID_ARGUMENT;
    std::string why;
    if (add_kiln_native(owner, n, argc, SLOT_MANAGED, out_hash, why) < 0) {
        logw("kiln.abilities: native %.*s refused: %s", static_cast<int>(n.size()), n.data(), why.c_str());
        return KILN_ERR_INVALID_ARGUMENT;
    }
    logi("kiln.abilities: managed native %.*s (0x%08x, argc %u) registered", static_cast<int>(n.size()), n.data(),
         out_hash ? *out_hash : 0, argc);
    retry_waiting();
    return KILN_OK;
}

// since 0.2.0-dev.29: a synchronous native (see the table)
KilnResult KILN_CALL t_native_register_sync(KilnOwner owner, KilnStr name, uint32_t argc, KilnNativeWrapFn fn, void* user,
                                            uint32_t* out_hash, uint32_t* out_refusal) {
    if (out_refusal) *out_refusal = KILN_NATIVE_REFUSED_NONE;
    if (!on_tick() || t_slot >= 0) return KILN_ERR_WRONG_THREAD;
    if (!g_have_natives) return KILN_ERR_UNAVAILABLE;
    const std::string_view n(name.ptr ? name.ptr : "", name.ptr ? name.len : 0);
    if (!fn || argc > 31 || !valid_native_name(n) || n.rfind("kiln.", 0) == 0) return KILN_ERR_INVALID_ARGUMENT;
    std::string why;
    if (add_kiln_native(owner, n, argc, SLOT_MANAGED_SYNC, out_hash, why, out_refusal, fn, user) < 0) {
        logw("kiln.abilities: sync native %.*s refused: %s", static_cast<int>(n.size()), n.data(), why.c_str());
        return KILN_ERR_INVALID_ARGUMENT;
    }
    start_watchdog();
    logi("kiln.abilities: sync native %.*s (0x%08x, argc %u) registered", static_cast<int>(n.size()), n.data(),
         out_hash ? *out_hash : 0, argc);
    retry_waiting();
    return KILN_OK;
}

// Takes a mod's native off (g_mu held): from now on its hash answers 0x1F. On the tick thread outside a native call no
// impact_simulation slice runs, so no call can be inside the slot's fn: the in-flight counter says so.
void take_off(Slot& s) {
    s.kind.store(SLOT_DEAD, std::memory_order_release);
    retire_binding(s.sync.exchange(nullptr, std::memory_order_seq_cst));  // freed once its owner's gate is quiet
    s.owner = 0;
}

KilnResult KILN_CALL t_native_unregister(uint32_t hash) {
    if (!on_tick() || t_slot >= 0) return KILN_ERR_WRONG_THREAD;
    std::lock_guard lock(g_mu);
    auto it = g_slot_of.find(hash);
    if (it == g_slot_of.end() || !mod_kind(g_slots[it->second].kind.load())) return KILN_ERR_NOT_FOUND;
    take_off(g_slots[it->second]);
    return KILN_OK;
}

KilnResult KILN_CALL t_native_complete(uint64_t call_id, uint8_t error, uint8_t value) {
    std::lock_guard lock(g_mu);
    auto k = g_call_key.find(call_id);
    if (k == g_call_key.end()) return KILN_ERR_NOT_FOUND;
    auto& c = g_calls[k->second];
    c.state = 1;
    c.error = error;
    c.value = value;
    return KILN_OK;
}

std::vector<std::pair<uint32_t, int>> g_deferred_wrap;  // (hash, mode) from other threads, applied in Pre

KilnResult wrap_or_observe(uint32_t hash, uint32_t on, bool wrap) {
    if (!g_have_natives) return KILN_ERR_UNAVAILABLE;
    int slot;
    {
        std::lock_guard lock(g_mu);
        slot = wrap_slot(hash);
        if (slot < 0) return g_captured.empty() ? KILN_ERR_UNAVAILABLE : KILN_ERR_NOT_FOUND;
        (wrap ? g_slots[slot].wrap : g_slots[slot].observe).store(on != 0);
    }
    if (g_setup_registry && g_slots[slot].registered_in != g_setup_registry) {
        if (!on_tick()) return KILN_ERR_WRONG_THREAD;  // a new wrap must be registered on the tick thread
        register_slot_in(slot, g_setup_registry);
    }
    return KILN_OK;
}

KilnResult KILN_CALL t_native_wrap(uint32_t hash, uint32_t on) { return wrap_or_observe(hash, on, true); }
KilnResult KILN_CALL t_native_observe(uint32_t hash, uint32_t on) { return wrap_or_observe(hash, on, false); }

KilnResult KILN_CALL t_set_wrap_dispatcher(KilnNativeWrapFn fn, void* user) {
    g_wrap_user = user;
    g_wrap_fn = fn;
    return KILN_OK;
}

// From inside a dispatcher call only: the thunk running on this thread is the call's (no lock on the job thread). A
// sync native has no original; the game's native never runs twice in one call (the latch).
void KILN_CALL t_call_original(KilnNativeCall* c) {
    if (!c || c->original_called) return;
    const int slot = t_slot;
    if (slot < 0 || slot >= kSlots || g_slots[slot].hash != c->hash) return;
    NativeFn orig = g_slots[slot].original.load();
    if (!orig || t_orig_ran || t_stage == KILN_NATIVE_AFTER) return;
    auto* out = static_cast<NativeResult*>(c->engine[0]);
    auto* args = static_cast<ArgSlice*>(c->engine[1]);
    auto* ctx = static_cast<NativeCtx*>(c->engine[2]);
    c->original_called = 1;
    call_fn(orig, out, args, ctx);
    c->out_error = out->error;
    c->out_value = out->value;
}

// since 0.2.0-dev.29 (review P1): an owner's retirement closes its gate first (no new call enters its natives or
// attachments), takes them off, then waits a bounded 2 ms (never holding g_mu) for the calls inside to leave. One still
// inside after that keeps the owner's bindings and sets alive: the retirement is deferred, reported, and finished in a
// later Pre when the gate is quiet (free_retired). On the tick thread in Pre or Post no impact_simulation slice runs,
// so the wait ends at once there; the bound covers a caller that is not, and a call that never returns.
KilnResult KILN_CALL t_release_owner(KilnOwner owner) {
    if (!on_tick() || t_slot >= 0) return KILN_ERR_WRONG_THREAD;  // never beside a running step, never from a call
    OwnerGate* gate = nullptr;
    {
        std::lock_guard lock(g_mu);
        gate = owner != 0 ? g_gates.find(owner) : nullptr;
        if (gate) gate->closed.store(true, std::memory_order_seq_cst);  // admission closed first
        for (auto& s : g_slots)
            if (s.owner == owner && mod_kind(s.kind.load())) take_off(s);
        std::unordered_set<uint32_t> touched;
        for (auto it = g_attachments.begin(); it != g_attachments.end();) {
            if (it->owner == owner && owner != 0) {
                touched.insert(it->hash);
                it = g_attachments.erase(it);
            } else {
                ++it;
            }
        }
        for (uint32_t h : touched) publish_set(h);
    }
    if (!gate) return KILN_OK;
    if (gate_close_and_wait(*gate, kRetireWaitNs, now_ns, [] { SwitchToThread(); })) return KILN_OK;
    std::lock_guard lock(g_mu);
    g_deferred_owners.push_back(Deferred<KilnOwner>{gate, owner, g_step.load()});
    c_retire_deferred.fetch_add(1, std::memory_order_relaxed);
    logw("kiln.abilities: owner %llu retires with %u native call(s) still inside its handlers: its natives and attachments "
         "take no new call; what they use stays until the calls leave (checked each Pre)",
         static_cast<unsigned long long>(owner), gate->inflight.load());
    return KILN_OK;
}

// since 0.2.0-dev.29 (see the table)
KilnResult KILN_CALL t_native_attach(KilnOwner owner, uint32_t hash, const KilnNativeFilter* filter, int32_t stage,
                                     int32_t priority, KilnNativeWrapFn fn, void* user, KilnHandle* out,
                                     uint32_t* out_refusal) {
    if (out_refusal) *out_refusal = KILN_NATIVE_REFUSED_NONE;
    if (out) *out = 0;
    if (!on_tick() || t_slot >= 0) return KILN_ERR_WRONG_THREAD;
    if (!g_have_natives) return KILN_ERR_UNAVAILABLE;
    if (!fn || !out || stage < KILN_NATIVE_BEFORE || stage > KILN_NATIVE_AFTER ||
        (filter && (filter->size < sizeof(KilnNativeFilter) || filter->reserved != 0)))
        return KILN_ERR_INVALID_ARGUMENT;
    auto refuse = [&](uint32_t code) {
        if (out_refusal) *out_refusal = code;
        return KILN_ERR_INVALID_ARGUMENT;
    };
    const uint32_t program = filter ? filter->program_id : 0, site = filter ? filter->site : 0;
    const uintptr_t preg = g_prog_registry.load();
    const bool programs = g_programs_ready.load() && preg;
    if (program && programs) {
        const int r = program_calls(preg, program, hash, site);
        if (r < 0) return refuse(KILN_NATIVE_REFUSED_PROGRAM_NOT_LOADED);
        if (r == 0) return refuse(KILN_NATIVE_REFUSED_SITE_NOT_FOUND);
    }
    int slot = -1;
    uint64_t id = 0;
    {
        std::lock_guard lock(g_mu);
        if (stage == KILN_NATIVE_INSTEAD && instead_taken(g_attachments, hash, program, site))
            return refuse(KILN_NATIVE_REFUSED_SECOND_INSTEAD);
        if (!g_captured.empty()) {  // the world registered its natives: the hash must be one, with a slot
            bool vanilla = false;
            for (const auto& c : g_captured) vanilla |= c.hash == hash && c.fn;
            if (!vanilla) return refuse(KILN_NATIVE_REFUSED_NOT_A_NATIVE);
            slot = wrap_slot(hash);
            if (slot < 0) return refuse(KILN_NATIVE_REFUSED_NO_SLOT);
        }
        Attachment a;
        a.id = id = ++g_attach_seq;
        a.hash = hash;
        a.program = program;
        a.site = site;
        a.stage = stage;
        a.priority = priority;
        a.order = id;
        a.fn = fn;
        a.user = user;
        a.owner = owner;
        a.gate = g_gates.of(owner);
        a.checked = program == 0 || programs;  // with a program filter: checked above, or when the world's programs are in
        g_attachments.push_back(a);
        publish_set(hash);
    }
    if (slot >= 0 && g_setup_registry && g_slots[slot].registered_in != g_setup_registry) register_slot_in(slot, g_setup_registry);
    start_watchdog();
    *out = id;
    logi("kiln.abilities: attachment %llu: native 0x%08x, %s, program 0x%08x, site 0x%08x, priority %d%s",
         static_cast<unsigned long long>(id), hash,
         stage == KILN_NATIVE_BEFORE ? "Before" : stage == KILN_NATIVE_INSTEAD ? "Instead" : "After", program, site,
         priority, slot < 0 ? " (waits for the world's natives)" : program && !programs ? " (checked when the world's programs are in)" : "");
    return KILN_OK;
}

KilnResult KILN_CALL t_native_detach(KilnHandle h) {
    if (!on_tick() || t_slot >= 0) return KILN_ERR_WRONG_THREAD;
    std::lock_guard lock(g_mu);
    for (auto it = g_attachments.begin(); it != g_attachments.end(); ++it) {
        if (it->id != h) continue;
        const uint32_t hash = it->hash;
        g_attachments.erase(it);
        publish_set(hash);
        if (const uint32_t n = g_inflight.load())
            logw("kiln.abilities: attachment %llu detached while %u native call(s) are in flight (expected none in Pre or Post)",
                 static_cast<unsigned long long>(h), n);
        return KILN_OK;
    }
    return KILN_ERR_NOT_FOUND;
}

KilnResult KILN_CALL t_native_counters(KilnNativeCounters* out) {
    if (!out || out->size < offsetof(KilnNativeCounters, sync_calls)) return KILN_ERR_INVALID_ARGUMENT;
    KilnNativeCounters c;
    std::memset(&c, 0, sizeof c);
    c.size = sizeof c;
    c.inflight = g_inflight.load();
    c.flags = g_have_natives ? KILN_NATIVE_COUNTERS_NATIVES : 0;
    c.sync_calls = c_sync_calls.load();
    c.sync_faults = c_sync_faults.load();
    c.sync_slow = c_sync_slow.load();
    c.dead_calls = c_dead_calls.load();
    c.attach_entered = c_attach_entered.load();
    c.attach_skipped = c_attach_skipped.load();
    c.typed_refused = c_typed_refused.load();
    c.typed_unconnected = c_typed_unconnected.load();
    c.retire_deferred = c_retire_deferred.load();
    c.watchdog_reports = c_watchdog_reports.load();
    c.lock_bypass = c_lock_bypass.load();
    c.ecs_state = g_ecs_state.load();
    c.ecs_reason = g_ecs_reason.load();
    {
        std::lock_guard lock(g_mu);
        c.retire_pending = static_cast<uint32_t>(g_deferred_owners.size());
        for (const auto& a : g_attachments) ++(!a.checked ? c.attach_pending : a.active ? c.attach_active : c.attach_off);
    }
    const size_t n = std::min<size_t>(out->size, sizeof c);
    std::memcpy(reinterpret_cast<uint8_t*>(out) + 4, reinterpret_cast<const uint8_t*>(&c) + 4, n - 4);
    if (out->size > sizeof c) std::memset(reinterpret_cast<uint8_t*>(out) + sizeof c, 0, out->size - sizeof c);
    return KILN_OK;
}

const void* attr_registry() {
    const uintptr_t w = g_world.load();
    if (!w) return nullptr;
    const char* n = "impact::AttributeRegistry";
    return reinterpret_cast<const void*>(g_api->singleton(w, KilnStr{n, std::strlen(n)}));
}

KilnResult KILN_CALL t_attr_get(KilnEntity e, uint32_t component_hash, uint32_t attr, int32_t* value) {
    if (!on_tick()) return KILN_ERR_WRONG_THREAD;
    if (!g_have_attrs || !g_world.load()) return KILN_ERR_UNAVAILABLE;
    auto* comp = static_cast<uint8_t*>(component(g_world.load(), component_hash, e));
    int32_t* slot = nullptr;
    if (!comp || !attr_slot(attr_registry(), comp, attr, &slot)) return KILN_ERR_NOT_FOUND;
    return read_bytes(slot, value, 4) ? KILN_OK : KILN_ERR_INTERNAL;
}

KilnResult KILN_CALL t_attr_add(KilnEntity e, uint32_t component_hash, uint32_t attr, int32_t delta) {
    if (!on_tick()) return KILN_ERR_WRONG_THREAD;
    if (!g_have_attrs || !g_world.load()) return KILN_ERR_UNAVAILABLE;
    auto* comp = static_cast<uint8_t*>(component(g_world.load(), component_hash, e));
    int32_t* slot = nullptr;
    const void* reg = attr_registry();
    if (!comp || !attr_slot(reg, comp, attr, &slot)) return KILN_ERR_NOT_FOUND;
    return attr_add(reg, comp, attr, delta) ? KILN_OK : KILN_ERR_INTERNAL;
}

KilnResult KILN_CALL t_sequence_state(KilnEntity e, uint32_t* seq, int64_t* time, uint32_t* call) {
    if (!on_tick()) return KILN_ERR_WRONG_THREAD;
    if (!g_world.load()) return KILN_ERR_UNAVAILABLE;
    uint64_t size = 0;
    auto* a = static_cast<const uint8_t*>(component(g_world.load(), kActor, e, &size));
    if (!a || size < static_cast<uint64_t>(Engine::actor_sequence) + 0x18) return KILN_ERR_NOT_FOUND;
    uint8_t rid[0x18];
    if (!read_bytes(a + Engine::actor_sequence, rid, sizeof rid)) return KILN_ERR_INTERNAL;
    if (seq) std::memcpy(seq, rid, 4);
    if (time) std::memcpy(time, rid + 8, 8);
    if (call) *call = rid[0x14];
    return KILN_OK;
}

constexpr uint32_t kActorInput = fnv1a("keen::ecs::ActorInput");               // 0x5c3c6dd8
constexpr uint32_t kTypeActorSequence = fnv1a("keen::actor::ActorSequenceResource"); // 0x9fd4a561
bool g_have_sequences = false;
std::unordered_map<uint32_t, KilnAssetKey> g_sequence_keys;  // tick thread: sequence id -> resource key

// ---- held sequences (docs/re/abilities.md 9) ----
// Two kinds of keep-alive gates end a sequence that nothing holds (the actor's effective state is
// (Actor.currentState | playback.setStateMask) & ~playback.unsetStateMask, 0x3b770; "has all bits of the mask"):
//  - a KeepAliveEvent(mask) at t makes mask the sequence's requirement from t on (playback.sequenceKeepAliveMask,
//    +0xbe8, checked every step by 0x36620) until the next KeepAliveEvent replaces it (an empty mask clears it);
//  - a LoopEvent [t, t+d) with a mask leaves its loop when the mask drops (jumpToLoopEndOnAbort) and repeats it while
//    the mask is still held at its end.
// For players those bits come from held input (InRangeCombat, InRangeCombatPreparing, InRangeCombatShooting: the
// ranged weapon program), so a staff cast Kiln started ended at its first KeepAliveEvent (0.2 s). With the hold flag
// Kiln ORs the needed bits into the running sequence's own setStateMask (the field SetStateEvent writes; replicated
// through NetworkActor.setStateMask) a little ahead of each gate, plays every loop body once (the loop's bits are let
// go just before its end, which also ends a sequence whose requirement shares them: a release, as a player's would),
// and takes back only the bits it added.
// Actor layout (reflection, server svn 1285601 and 1287369, client 26430625): currentAction (ActionSequence) at +0,
// its playbackContext at +0x170: subSequenceId +0x170, currentTime +0x180, state +0x1b1 (bit 0 = playing),
// setStateMask +0xbd0; Actor.sequenceRuntimeId +0xc08 {sequenceId, +8 triggerTime}.
constexpr uint32_t kSeqFlagHold = 0x10000;  // sequence_play: trigger | flag (Kiln extension, see Kiln.Abilities)
constexpr uint32_t kEvKeepAlive = 0xc6b36da5u, kEvLoop = 0xb52eef3fu;  // keen::actor::KeepAliveEvent / LoopEvent
static_assert(fnv1a("keen::actor::KeepAliveEvent") == kEvKeepAlive && fnv1a("keen::actor::LoopEvent") == kEvLoop);
constexpr int64_t kActSub = 0x170, kActTime = 0x180, kActPlay = 0x1b1, kActSetStates = 0xbd0, kActSize = 0xc20;
constexpr int64_t kStepNs = 40'000'000;                         // one server step (25 Hz)
constexpr int64_t kLookNs = 2 * kStepNs, kReleaseNs = kStepNs * 3 / 2;

struct HoldSub {
    std::vector<std::pair<int64_t, uint64_t>> keep;      // KeepAliveEvent (time, mask), sorted
    std::vector<std::array<int64_t, 3>> loops;           // LoopEvent {start, end, mask}
    int64_t last = 0;                                    // time of the last gate
    bool any = false;
};
struct HoldSeq {
    std::vector<HoldSub> subs;  // per sub-sequence
    size_t gates = 0;
};
std::unordered_map<uint32_t, std::shared_ptr<const HoldSeq>> g_hold_cache;  // tick thread
struct SeqHold {
    uint32_t entity = 0, sequence = 0;
    int64_t time = 0;
    std::shared_ptr<const HoldSeq> spec;
    uint64_t ours = 0;  // bits Kiln added to setStateMask
    uint32_t waits = 0;
    bool started = false;
};
std::vector<SeqHold> g_seq_holds;  // tick thread
std::atomic<uint64_t> c_holds{0}, c_hold_bits{0};

bool blob_array(const uint8_t* base, size_t n, const uint8_t* field, size_t elem, const uint8_t** first,
                uint32_t* count) {
    if (field < base || field + 8 > base + n) return false;
    int32_t off;
    std::memcpy(&off, field, 4);
    std::memcpy(count, field + 4, 4);
    if (*count == 0) {
        *first = nullptr;
        return true;
    }
    const uint8_t* p = field + off;
    if (p < base || *count > 4096 || p + static_cast<size_t>(*count) * elem > base + n) return false;
    *first = p;
    return true;
}

// ActorSequenceResource {Guid resourceId; BlobArray<ActorSubSequenceResource 72 B> subSequences +0x10};
// ActorSubSequenceResource {SequenceEvents events (BlobArray<BlobVariant 12 B {u32 type, i32 rel (from +4), u32}>)};
// KeepAliveEvent / LoopEvent {Time time +0, Time duration +8, StateMask keepAliveMask +0x60}.
std::shared_ptr<const HoldSeq> parse_holds(const uint8_t* b, size_t n) {
    auto out = std::make_shared<HoldSeq>();
    const uint8_t* subs = nullptr;
    uint32_t nsub = 0;
    if (n < 24 || !blob_array(b, n, b + 0x10, 72, &subs, &nsub)) return nullptr;
    out->subs.resize(nsub);
    for (uint32_t i = 0; i < nsub; ++i) {
        const uint8_t* sub = subs + static_cast<size_t>(i) * 72;
        const uint8_t* evs = nullptr;
        uint32_t nev = 0;
        if (!blob_array(b, n, sub, 12, &evs, &nev)) return nullptr;
        HoldSub& hs = out->subs[i];
        for (uint32_t k = 0; k < nev; ++k) {
            const uint8_t* ent = evs + static_cast<size_t>(k) * 12;
            uint32_t type;
            int32_t rel;
            std::memcpy(&type, ent, 4);
            std::memcpy(&rel, ent + 4, 4);
            if (type != kEvKeepAlive && type != kEvLoop) continue;
            const uint8_t* d = ent + 4 + rel;
            if (d < b || d + 0x70 > b + n) return nullptr;
            int64_t t, dur;
            uint64_t mask;
            std::memcpy(&t, d, 8);
            std::memcpy(&dur, d + 8, 8);
            std::memcpy(&mask, d + 0x60, 8);
            if (type == kEvKeepAlive) {
                hs.keep.emplace_back(t, mask);  // an empty mask clears the requirement: kept
                hs.last = std::max(hs.last, t);
            } else if (mask) {
                hs.loops.push_back({t, t + dur, static_cast<int64_t>(mask)});
                hs.last = std::max(hs.last, t + dur);
            } else {
                continue;
            }
            hs.any |= mask != 0;
        }
        std::sort(hs.keep.begin(), hs.keep.end());
        if (hs.any) ++out->gates;
    }
    return out;
}

// The bits a sequence needs held at sequence time cur (see above); *future: a gate is still ahead.
uint64_t hold_need(const HoldSub& s, int64_t cur, bool* future) {
    auto keep_at = [&](int64_t t) {
        uint64_t m = 0;
        for (const auto& [kt, km] : s.keep)
            if (kt <= t) m = km;
        return m;
    };
    const uint64_t keep = keep_at(cur) | keep_at(cur + kLookNs);
    uint64_t need = keep;
    for (const auto& l : s.loops)
        if (cur + kLookNs >= l[0] && cur < l[1] - kReleaseNs) need |= static_cast<uint64_t>(l[2]);
    // let go just before a loop's end so it plays once: only the bits the sequence's requirement does not need (the
    // loop leaves, the sequence goes on); a loop that needs nothing beyond the requirement can only be left by a
    // release, which also ends the sequence (as a player's release would)
    for (const auto& l : s.loops)
        if (cur >= l[1] - kReleaseNs && cur < l[1] + kStepNs) {
            const uint64_t m = static_cast<uint64_t>(l[2]);
            need &= ~((m & ~keep) ? (m & ~keep) : m);
        }
    *future = cur < s.last + kStepNs;
    return need;
}

const KilnAssetKey* sequence_key(uint32_t sequence) {
    if (g_sequence_keys.empty() && g_assets && g_assets->resource_list) {
        size_t n = 0;
        g_assets->resource_list(kTypeActorSequence, nullptr, 0, &n);
        std::vector<KilnAssetKey> keys(n + 16);
        size_t got = 0;
        if (g_assets->resource_list(kTypeActorSequence, keys.data(), keys.size(), &got) == KILN_OK)
            for (size_t i = 0; i < std::min(got, keys.size()); ++i) g_sequence_keys[fnv1a(keys[i].guid, 16)] = keys[i];
    }
    auto it = g_sequence_keys.find(sequence);
    return it == g_sequence_keys.end() ? nullptr : &it->second;
}

std::shared_ptr<const HoldSeq> hold_spec(uint32_t sequence) {
    auto c = g_hold_cache.find(sequence);
    if (c != g_hold_cache.end()) return c->second;
    std::shared_ptr<const HoldSeq> spec;
    const KilnAssetKey* key = sequence_key(sequence);
    if (key && g_assets && g_assets->resource_read) {
        size_t len = 0;
        g_assets->resource_read(key, nullptr, 0, &len);
        std::vector<uint8_t> bytes(len);
        if (len && g_assets->resource_read(key, bytes.data(), bytes.size(), &len) == KILN_OK && len <= bytes.size())
            spec = parse_holds(bytes.data(), len);
        if (!spec) logw("kiln.abilities: sequence 0x%08x: its events did not parse; no hold", sequence);
    }
    g_hold_cache[sequence] = spec;
    return spec;
}

void holds_pre(uintptr_t world) {
    if (g_seq_holds.empty()) return;
    for (auto it = g_seq_holds.begin(); it != g_seq_holds.end();) {
        SeqHold& h = *it;
        uint64_t size = 0;
        auto* a = static_cast<uint8_t*>(component(world, kActor, h.entity, &size));
        bool keep = a && size >= static_cast<uint64_t>(kActSize);
        uint32_t seq = 0;
        int64_t time = 0, cur = 0;
        uint8_t play = 0, sub = 0;
        uint64_t set = 0;
        if (keep)
            keep = read_bytes(a + Engine::actor_sequence, &seq, 4) && read_bytes(a + Engine::actor_sequence + 8, &time, 8) &&
                   read_bytes(a + kActPlay, &play, 1) && read_bytes(a + kActSub, &sub, 1) &&
                   read_bytes(a + kActTime, &cur, 8) && read_bytes(a + kActSetStates, &set, 8);
        const bool ours = keep && seq == h.sequence && time == h.time && (play & 1);
        if (keep && !h.started) {
            if (ours) h.started = true;
            else keep = ++h.waits < 6;  // the actor takes the trigger in its next step
        } else if (keep && !ours) {
            keep = false;  // ended or replaced: the next sequence has a fresh playback context
        }
        if (keep && h.started) {
            uint64_t need = 0;
            bool future = false;
            if (sub < h.spec->subs.size()) {
                need = hold_need(h.spec->subs[sub], cur, &future);
            } else {
                for (const auto& v : h.spec->subs) {
                    bool f = false;
                    need |= hold_need(v, cur, &f);
                    future |= f;
                }
            }
            const uint64_t add = need & ~set, rel = h.ours & ~need;
            const uint64_t next = (set | add) & ~rel;
            if (next != set && write_bytes(a + kActSetStates, &next, 8)) {
                h.ours = (h.ours | add) & ~rel;
                c_hold_bits.fetch_add(static_cast<uint64_t>(__builtin_popcountll(add)));
            }
            if (!future && !h.ours) keep = false;
        }
        if (keep) ++it;
        else it = g_seq_holds.erase(it);
    }
}

void holds_clear() {
    g_seq_holds.clear();
    g_hold_cache.clear();
}

KilnResult KILN_CALL t_sequence_play(KilnEntity e, uint32_t sequence, uint32_t trigger_and_flags) {
    if (!on_tick()) return KILN_ERR_WRONG_THREAD;
    if (!g_have_sequences || !g_world.load()) return KILN_ERR_UNAVAILABLE;
    const uint32_t trigger = trigger_and_flags & 0xffffu, flags = trigger_and_flags & ~0xffffu;
    if ((trigger != KILN_SEQUENCE_TRIGGER_EXTERNAL && trigger != KILN_SEQUENCE_TRIGGER_ABORT_OVERRIDE) ||
        (flags & ~kSeqFlagHold))
        return KILN_ERR_INVALID_ARGUMENT;
    if (!sequence_key(sequence) && !g_sequence_keys.empty()) return KILN_ERR_NOT_FOUND;
    uint64_t size = 0;
    void* ai = component(g_world.load(), kActorInput, e, &size);
    if (!ai || size < static_cast<uint64_t>(E.trigger_counter) + 1) return KILN_ERR_NOT_FOUND;
    const int64_t now = ecs_now(g_world.load());
    std::shared_ptr<const HoldSeq> spec = (flags & kSeqFlagHold) ? hold_spec(sequence) : nullptr;
    if (!trigger_sequence(ai, e, sequence, now, static_cast<uint16_t>(trigger))) return KILN_ERR_INTERNAL;
    g_seq_holds.erase(std::remove_if(g_seq_holds.begin(), g_seq_holds.end(),
                                     [&](const SeqHold& h) { return h.entity == e; }),
                      g_seq_holds.end());
    if (spec && spec->gates && g_seq_holds.size() < 512) {
        SeqHold h;
        h.entity = e;
        h.sequence = sequence;
        h.time = now;
        h.spec = spec;
        g_seq_holds.push_back(std::move(h));
        c_holds.fetch_add(1);
    }
    logi("kiln.abilities: sequence 0x%08x triggered on %u (trigger %u, time %lld%s)", sequence, e, trigger,
         static_cast<long long>(now),
         !(flags & kSeqFlagHold) ? "" : spec && spec->gates ? ", holding its keep-alive states" : ", nothing to hold");
    return KILN_OK;
}

const KilnAbilitiesApi kTable = {
    {sizeof(KilnAbilitiesApi), KILN_ABILITIES_VERSION},
    &t_status,
    &t_set_listener,
    &t_hold_filter,
    &t_instance_vars,
    &t_instance_read,
    &t_instance_write,
    &t_instance_info,
    &t_entity_overrides,
    &t_program_count,
    &t_program_info,
    &t_program_vars,
    &t_program_register,
    &t_program_refresh,
    &t_native_count,
    &t_native_at,
    &t_native_find,
    &t_native_declare,
    &t_native_register,
    &t_native_unregister,
    &t_native_complete,
    &t_native_wrap,
    &t_native_observe,
    &t_set_wrap_dispatcher,
    &t_call_original,
    &t_release_owner,
    &t_attr_get,
    &t_attr_add,
    &t_sequence_state,
    &t_sequence_play,
    &t_native_register_sync,
    &t_native_counters,
    &t_native_attach,
    &t_native_detach,
    &t_native_declare_typed,
    &t_native_state,
    &t_native_attach_state,
    &t_native_ecs_read,
    &t_native_ecs_write,
};

// ------------------------------------------------------------------------------------------- boot --------------
uintptr_t sym(const char* name) {
    uintptr_t a = 0;
    return g_api->symbol(KilnStr{name, std::strlen(name)}, &a) == KILN_OK ? a : 0;
}
int64_t cst(const char* name, int64_t dflt) {
    int64_t v = 0;
    return g_api->constant(KilnStr{name, std::strlen(name)}, &v) == KILN_OK ? v : dflt;
}
bool feat(const char* name) { return g_api->feature(KilnStr{name, std::strlen(name)}); }

// ---- the mods' impact/natives.json (since 0.2.0-dev.29) ----
std::filesystem::path utf8_path(std::string_view s) {
    return std::filesystem::path(std::u8string_view(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}

std::string read_bounded(const std::filesystem::path& file, size_t cap) {  // "" when missing, unreadable or too large
    std::error_code ec;
    if (!std::filesystem::is_regular_file(file, ec) || ec) return {};
    const auto size = std::filesystem::file_size(file, ec);
    if (ec || size > cap) return {};
    std::ifstream in(file, std::ios::binary);
    std::string text(static_cast<size_t>(size), '\0');
    if (!in.read(text.data(), static_cast<std::streamsize>(text.size()))) return {};
    return text;
}

// The folders of the mods that load: the active profile's enabled Kiln mods, then the drop-ins of kiln/mods without a
// .disabled marker unless the profile turns drop-ins off (native/services/world/boot_programs.cpp finds
// kiln.programs.json the same way). The id is the manifest's.
std::vector<std::pair<std::string, std::filesystem::path>> mod_folders() {
    namespace fs = std::filesystem;
    std::vector<fs::path> dirs;
    bool dropins = true;
    const auto* profile = static_cast<const KilnProfileApi*>(g_api->service(KilnStr{KILN_PROFILE_SERVICE, sizeof KILN_PROFILE_SERVICE - 1}, 1));
    if (profile && profile->header.size >= sizeof(KilnProfileApi)) {
        const uint32_t mode = profile->mode();
        if (mode == KILN_PROFILE_NO_MODS || mode == KILN_PROFILE_INVALID) return {};
        if (mode == KILN_PROFILE_ACTIVE || mode == KILN_PROFILE_ONCE) {
            dropins = (profile->dropins() & (1u << KILN_PROFILE_KIND_KILN)) != 0;
            const uint32_t n = std::min<uint32_t>(profile->count(KILN_PROFILE_KIND_KILN), 1024);
            for (uint32_t i = 0; i < n; ++i) {
                KilnProfileMod m{};
                m.size = sizeof m;
                if (profile->get(KILN_PROFILE_KIND_KILN, i, &m) == KILN_OK && m.enabled && m.path.ptr)
                    dirs.push_back(utf8_path(std::string_view(m.path.ptr, m.path.len)));
            }
        }
    }
    if (dropins) {
        char path[4096];
        size_t n = 0;
        if (g_api->path(KILN_PATH_MODS, path, sizeof path, &n) == KILN_OK && n < sizeof path) {
            std::error_code ec;
            std::vector<fs::path> found;
            for (auto it = fs::directory_iterator(utf8_path(std::string_view(path, n)), ec); !ec && it != fs::directory_iterator() && found.size() < 1024; it.increment(ec))
                if (it->is_directory(ec) && !fs::exists(it->path() / L".disabled", ec)) found.push_back(it->path());
            std::sort(found.begin(), found.end());
            dirs.insert(dirs.end(), found.begin(), found.end());
        }
    }
    std::vector<std::pair<std::string, fs::path>> out;
    std::unordered_set<std::string> seen;
    for (const auto& d : dirs) {
        const std::string text = read_bounded(d / L"kiln.mod.json", 64 * 1024);
        if (text.empty()) continue;
        try {
            const auto m = nlohmann::json::parse(text, nullptr, true, true);  // manifests may carry comments
            const std::string id = m.is_object() && m.contains("id") && m["id"].is_string() ? m["id"].get<std::string>() : "";
            if (!id.empty() && seen.insert(id).second) out.emplace_back(id, d);
        } catch (const std::exception&) {
            // the host says why the manifest does not load
        }
    }
    return out;
}

// Before the first program is verified: every loading mod's impact/natives.json declares its natives (typed ones with
// their argument types, older entries by argc only).
void load_mod_natives() {
    if (!g_have_natives) return;
    uint32_t typed = 0, plain = 0, files = 0;
    for (const auto& [id, dir] : mod_folders()) {
        const auto file = dir / L"impact" / L"natives.json";
        std::error_code ec;
        if (!std::filesystem::exists(file, ec)) continue;
        const std::string text = read_bounded(file, kNativesJsonBytes);
        std::vector<std::string> notes;
        std::vector<NativeDecl> decls;
        try {
            if (text.empty()) throw std::runtime_error("unreadable, or larger than 256 KiB");
            decls = parse_natives_json(text, id, &notes);
        } catch (const std::exception& e) {
            logw("kiln.abilities: %s/impact/natives.json not read: %s", id.c_str(), e.what());
            continue;
        }
        ++files;
        for (const auto& n : notes) logw("kiln.abilities: %s/impact/natives.json: %s (skipped)", id.c_str(), n.c_str());
        for (const auto& d : decls) {
            const KilnResult rc = d.typed ? t_native_declare_typed(d.hash, d.argc, d.args.data()) : t_native_declare(d.hash, d.argc);
            if (rc != KILN_OK) logw("kiln.abilities: %s/impact/natives.json: %s not declared (a vanilla native's hash?)", id.c_str(), d.name.c_str());
            else (d.typed ? typed : plain)++;
        }
    }
    if (files) logi("kiln.abilities: impact/natives.json of %u mod(s): %u typed and %u argc-only declaration(s)", files, typed, plain);
}

void KILN_CALL on_ready(void*) {
    g_world_api = static_cast<const KilnWorldApi*>(g_api->service(KilnStr{KILN_WORLD_SERVICE, sizeof KILN_WORLD_SERVICE - 1}, 4));
    g_spawn = static_cast<const KilnSpawnApi*>(g_api->service(KilnStr{KILN_SPAWN_SERVICE, sizeof KILN_SPAWN_SERVICE - 1}, 1));
    g_assets = static_cast<const KilnAssetsApi*>(g_api->service(KilnStr{KILN_ASSETS_SERVICE, sizeof KILN_ASSETS_SERVICE - 1}, 1));
    load_mod_natives();
}

}  // namespace

extern "C" KILN_EXPORT KilnResult KILN_CALL kiln_plugin_main(const KilnApi* api, KilnOwner owner, KilnPluginInfo* info) {
    if (api->abi_version != KILN_ABI_VERSION || api->size < offsetof(KilnApi, subscribe_world) + sizeof(void*))
        return KILN_ERR_UNAVAILABLE;
    g_api = api;
    g_owner = owner;
    {
        LARGE_INTEGER f;
        if (QueryPerformanceFrequency(&f)) g_qpf = f.QuadPart;
    }
    {  // lab only (see lab_unpin)
        char path[1024];
        size_t n = 0;
        KilnBuildInfo bi{};
        bi.size = sizeof bi;
        std::error_code ec;
        if (api->path(KILN_PATH_CONTROL, path, sizeof path, &n) == KILN_OK && n < sizeof path &&
            std::filesystem::exists(std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(path), n)) / "impact.unpin", ec) &&
            api->build_info(&bi) == KILN_OK && bi.side == KILN_SIDE_SERVER)
            g_lab_unpin = true;  // with the pin resolved (below)
        if (api->path(KILN_PATH_CONTROL, path, sizeof path, &n) == KILN_OK && n < sizeof path) {
            const auto control = std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(path), n));
            if (std::filesystem::exists(control / "impact.lockhold", ec) && api->build_info(&bi) == KILN_OK && bi.side == KILN_SIDE_SERVER)
            {
                g_lab_lockhold = control;
                g_lab_lockhold_on = true;
            }
        }
    }
    {  // the ECS bridge's serial condition and the lab's unpin: the pin byte through sigdb (feature abilities.ecs_pin)
        int64_t v = -1;
        g_world_storage = api->constant(KilnStr{"world.storage", 13}, &v) == KILN_OK ? v : -1;
        v = -1;
        g_pin_off = api->feature(KilnStr{"abilities.ecs_pin", 17}) && api->constant(KilnStr{"abilities/impact.pin", 20}, &v) == KILN_OK ? v : -1;
        if (g_world_storage <= 0 || g_pin_off <= 0) g_lab_unpin = false;
    }
    info->id = KilnStr{KILN_ABILITIES_SERVICE, sizeof(KILN_ABILITIES_SERVICE) - 1};
    info->version = KilnStr{"1", 1};
    info->on_ready = &on_ready;
    info->on_unload = &stop_watchdog;

    E.get_component = reinterpret_cast<Engine::GetComponentFn>(sym("GetComponent"));
    E.name_cap = cst("abilities/natives.name_cap", 0x20);
    E.natives_map = cst("abilities/natives.map", 0x20);
    E.prog_count = cst("abilities/programs.count", 0xbc);
    E.prog_cap = cst("abilities/programs.cap", 0x78);
    E.prog_bits = cst("abilities/programs.bits", 0x70);
    E.prog_keys = cst("abilities/programs.keys", 0x90);
    E.prog_values = cst("abilities/programs.values", 0xa8);
    E.system_registry = cst("abilities/system.registry", 8);
    E.inst_program = cst("abilities/inst.program", 4);
    E.inst_entity = cst("abilities/inst.entity", 8);
    E.inst_flags = cst("abilities/inst.flags", 0x1f2);

    std::string off;
    if (feat("abilities.natives")) {
        E.register_native = reinterpret_cast<Engine::RegisterNativeFn>(sym("abilities/RegisterNative"));
        if (E.register_native) {
            g_have_natives = g_h_register.install(api, owner, reinterpret_cast<uintptr_t>(E.register_native),
                                                  reinterpret_cast<void*>(&hk_register),
                                                  "abilities/RegisterNative");
            // keep calling the game's function through the hook's trampoline (registrations of Kiln's own are
            // marked with t_kiln_registering so the capture skips them)
        }
    }
    if (feat("abilities.programs")) {
        E.get_program = reinterpret_cast<Engine::GetProgramFn>(sym("abilities/GetProgram"));
        g_have_programs = E.get_program != nullptr;
    }
    if (feat("abilities.instances") && g_have_programs) {
        E.create_instance = reinterpret_cast<Engine::CreateInstanceFn>(sym("abilities/CreateInstance"));
        E.run_impact = reinterpret_cast<Engine::RunImpactFn>(sym("abilities/RunImpact"));
        E.instance_lookup = reinterpret_cast<Engine::InstanceLookupFn>(sym("abilities/InstanceLookup"));
        if (E.create_instance && E.run_impact && E.instance_lookup) {
            g_h_create.install(api, owner, reinterpret_cast<uintptr_t>(E.create_instance), reinterpret_cast<void*>(&hk_create),
                               "abilities/CreateInstance");
            g_h_run.install(api, owner, reinterpret_cast<uintptr_t>(E.run_impact), reinterpret_cast<void*>(&hk_run),
                            "abilities/RunImpact");
            g_have_instances = static_cast<bool>(g_h_create) && static_cast<bool>(g_h_run);
        }
    }
    if (feat("abilities.attributes")) {
        E.attr_find = reinterpret_cast<Engine::AttrFindFn>(sym("combat/AttrFind"));
        E.attr_add = reinterpret_cast<Engine::AttrAddFn>(sym("combat/AttrAdd"));
        g_have_attrs = E.attr_find && E.attr_add && E.get_component;
    }
    if (feat("abilities.sequences")) {
        E.trigger_sequence = reinterpret_cast<Engine::TriggerSequenceFn>(sym("abilities/TriggerSequence"));
        E.trigger_counter = cst("abilities/actor.trigger_counter", 0x178);
        E.world_storage = cst("world.storage", -1);
        E.storage_clock = cst("storage.clock", -1);
        E.storage_period = cst("storage.clock_period", -1);
        g_have_sequences = E.trigger_sequence && E.get_component && E.world_storage >= 0;
    }
    if (g_have_natives) {
        // the call into the game goes through the trampoline, so Kiln's own registrations are not re-captured
        E.register_native = g_h_register.original<Engine::RegisterNativeFn>();
        std::string why;
        add_kiln_native(owner, "kiln.Log", 1, SLOT_LOG, nullptr, why);
        add_kiln_native(owner, "kiln.Signal", 2, SLOT_SIGNAL, nullptr, why);
    }
    if (g_have_programs && !g_have_instances)
        logw("kiln.abilities: instance hooks unavailable on this build: no events, overrides or holds");

    KilnHandle h = 0;
    if (api->subscribe_tick(owner, KILN_TICK_PRE, &on_pre, nullptr, &h) != KILN_OK) return KILN_ERR_UNAVAILABLE;
    iw::init(api, owner, &program_is_vanilla);  // the impact pool watch (inbox 116): read-only
    api->subscribe_world(owner, &on_world, nullptr, &h);
    logi("kiln.abilities: natives %s, programs %s, instances %s, attributes %s, sequences %s",
         g_have_natives ? "on" : "off", g_have_programs ? "on" : "off", g_have_instances ? "on" : "off",
         g_have_attrs ? "on" : "off", g_have_sequences ? "on" : "off");
    return api->register_service(owner, KilnStr{KILN_ABILITIES_SERVICE, sizeof(KILN_ABILITIES_SERVICE) - 1}, &kTable);
}
