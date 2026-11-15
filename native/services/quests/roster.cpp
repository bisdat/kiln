// SPDX-License-Identifier: MIT
// kiln.knowledge's roster (package 17c; chapter 17.3 decisions I2 and I7; the consumer audit docs/re/npc-roster.md;
// the portable part roster_core.h; signatures sigdb/symbols/{server,client}.d/dev29-17c-roster-cap.json).
//
// What runs, by player:
//  - Mods add no roster NPC, or keep the roster within 64 entries: nothing is hooked. Two Post callbacks (server
//    world, client world) return at once until a mod first reads the roster (roster_world, roster_budget,
//    npc_unlock_at); from then on each world's Post reads its NpcRegistry's cached unlock value (a few reads, a
//    snapshot only when something changed).
//  - A mod's roster needs 65 to 128 entries: the boot build asks roster_max(); where the four functions and every
//    layout constant resolved (feature quests.npc_pages), the three hooks go in right then (no world runs at boot;
//    through kiln.hooks' checked installation, which refuses before writing a byte while a thread cannot be shown to
//    be outside them), and the roster's maximum becomes 128 for the process. Anything missing keeps 64, with the
//    reason in the build's refusal and in npcs.expanded_roster's row.
//
// The hooks act per NpcRegistry: every world (the server world, the client world; a hosting client has both) has its
// own registry, its own producer program and its own extra page, attached in the SetUnlocks of that registry on the
// thread that evaluated it. An id at index 64 or more is never answered from the game's value (that would be another
// entry's bit): from the registry's page while it matches what the game's cache holds, else with the game's own
// "cache invalid" status 1, which its only caller (unique_npc_spawns) treats like an unlocked NPC: kept. Everything
// else goes to the game as it was asked.
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#include "kiln_hook.hpp"
#include "knowledge_internal.h"
#include "roster_core.h"
#include "services/kiln_caps.h"

namespace kiln::knowledge {
namespace {

using namespace kiln::roster;

using EvalFn = Result16* (*)(Result16* out, void* ctx, uint16_t* seen, const Span* span);
using SetFn = void (*)(uintptr_t registry, uint64_t mask, uint16_t version);
using QueryFn = uint8_t* (*)(uint8_t* out16, uintptr_t registry, uint32_t npc_id);
using FindFn = const uint8_t* (*)(uintptr_t registry, uint32_t npc_id);

// ---- what this side has
struct RosterLayout {
    int64_t npcs = -1, version = -1, state = -1, mask = -1, stride = -1, eval_error = -1, query_invalid = -1;
    uintptr_t eval = 0, set = 0, query = 0, find = 0;
} R;
bool g_layout_ok = false;     // the registry's cache can be read (feature quests.npc): the reads work at 64
bool g_pages_ok = false;      // feature quests.npc_pages and its cross-checks: the second page could be installed
std::string g_pages_why;      // why not (the row's text, the build's refusal)
uint32_t g_pages_reason = KILN_CAP_REASON_SYMBOL_MISSING;
bool g_client = false;

Limit g_limit;
std::mutex g_install_mu;
std::string g_install_why;    // a failed installation
bool g_install_failed = false;

kiln::RawHook g_eval_hook, g_set_hook, g_query_hook;
FindFn g_find = nullptr;
Sidecars g_sidecars;
thread_local PendingStack t_pending;

Snapshots g_snaps;
Owners g_owners;              // written once by the boot build (before any world), read on world threads
std::atomic<bool> g_wanted{false};

std::atomic<uint64_t> c_paged{0}, c_attached{0}, c_unpaired{0}, c_answered{0}, c_closed{0}, c_faults{0};

bool cst_value(const char* name, int64_t* out) { return g_api->constant(kstr(name), out) == KILN_OK; }
bool sym_value(const char* name, uintptr_t* out) { return g_api->symbol(kstr(name), out) == KILN_OK && *out; }

const KilnCapsApi* caps() {
    static const KilnCapsApi* c = static_cast<const KilnCapsApi*>(g_api->service(kstr(KILN_CAPS_SERVICE), 1));
    return c && c->report ? c : nullptr;
}
void row(const char* name, uint32_t state, uint32_t reason, const std::string& text) {
    if (const KilnCapsApi* c = caps()) c->report(g_owner, kstr(name), state, reason, KilnStr{text.data(), text.size()});
}

// ---- guarded reads of the registry
bool read_span(uintptr_t reg, uintptr_t* ptr, uint64_t* count) {
    uintptr_t coll = 0;
    int32_t rel = 0;
    uint32_t n = 0;
    if (!rd(reg + R.npcs, &coll) || !coll || !rd(coll, &rel) || !rd(coll + 4, &n)) return false;
    *ptr = rel ? coll + static_cast<intptr_t>(rel) : 0;
    *count = n;
    return true;
}
bool read_live(uintptr_t reg, LiveCache* live) {
    return reg && rd(reg + R.state, &live->status) && rd(reg + R.version, &live->version) && rd(reg + R.mask, &live->payload) &&
           read_span(reg, &live->span_ptr, &live->count);
}

const uint8_t* find_guarded(uintptr_t reg, uint32_t id, bool* fault) {
    __try {
        return g_find(reg, id);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *fault = true;
        return nullptr;
    }
}

// ---- the hooks (worker threads: a dedicated server evaluates the roster on workers once a player is in)
Result16* hk_eval(Result16* out, void* ctx, uint16_t* seen, const Span* span) {
    Span s{};
    if (!span || !seen || !out || !copy_guarded(&s, span, sizeof s) || s.count <= kPage || s.count > kExpanded)
        return g_eval_hook.call<Result16*>(out, ctx, seen, span);  // the game's own call, unchanged
    const auto original = g_eval_hook.original<EvalFn>();
    const PageOutcome o = eval_pages(out, ctx, seen, s, static_cast<uint64_t>(R.stride), static_cast<uint8_t>(R.eval_error),
                                     [original](Result16* r, void* c, uint16_t* v, const Span* sp) { original(r, c, v, sp); });
    c_paged.fetch_add(1, std::memory_order_relaxed);
    if (o.complete) {
        Pending p;
        p.span_ptr = s.ptr;
        p.count = s.count;
        p.pages = o.pages;
        for (uint32_t i = 0; i < kPages; ++i) p.masks[i] = o.masks[i];
        p.version = o.version;
        t_pending.push(p);
    }
    return out;
}

void hk_set(uintptr_t reg, uint64_t mask, uint16_t version) {
    g_set_hook.call<void>(reg, mask, version);  // the game's write first, always once
    uintptr_t ptr = 0;
    uint64_t count = 0;
    Pending p;
    if (read_span(reg, &ptr, &count) && count > kPage && t_pending.take(ptr, count, mask, version, &p) && p.pages > 1) {
        if (g_sidecars.attach(reg, p)) c_attached.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    c_unpaired.fetch_add(count > kPage, std::memory_order_relaxed);
    g_sidecars.invalidate(reg);  // whatever page this registry had no longer belongs to its cache
}

uint8_t* hk_query(uint8_t* out, uintptr_t reg, uint32_t id) {
    LiveCache live;
    bool fault = false;
    const uint8_t* res = out && reg && g_find ? find_guarded(reg, id, &fault) : nullptr;
    if (fault) c_faults.fetch_add(1, std::memory_order_relaxed);
    if (!res || !read_live(reg, &live) || !live.span_ptr || reinterpret_cast<uintptr_t>(res) < live.span_ptr)
        return g_query_hook.call<uint8_t*>(out, reg, id);  // unknown id, unreadable registry: the game's own answer
    const uint64_t index = (reinterpret_cast<uintptr_t>(res) - live.span_ptr) / static_cast<uint64_t>(R.stride);
    if (index < kPage || live.status != 0) return g_query_hook.call<uint8_t*>(out, reg, id);
    const Sidecars::Bit b = g_sidecars.bit(reg, live, index);
    uint8_t status = static_cast<uint8_t>(R.query_invalid), value = 0;
    if (b == Sidecars::Bit::Unlocked || b == Sidecars::Bit::Locked) {
        status = 0;
        value = b == Sidecars::Bit::Unlocked;
        c_answered.fetch_add(1, std::memory_order_relaxed);
    } else {
        c_closed.fetch_add(1, std::memory_order_relaxed);
    }
    // the game's ResultBool: status byte +0, dword +4 = 0, bool byte +8
    out[0] = status;
    std::memset(out + 4, 0, 4);
    out[8] = value;
    return out;
}

void place_noop(KilnRegisters&) {}

// The checked installation (kiln.hooks' mid2_checked) places the physical detour, refusing before any byte is written
// while a thread cannot be shown to be outside the replaced bytes; the raw link then joins that target, which writes no
// game byte. A refusal is retried twice, then reported.
bool install_one(kiln::RawHook& hook, uintptr_t target, void* fn, const char* label, std::string* why) {
    for (int attempt = 0; attempt < 3; ++attempt) {
        kiln::MidHook2 placer;
        if (placer.install_checked(g_api, g_owner, target, &place_noop, label)) {
            const bool ok = hook.install(g_api, g_owner, target, fn, label);
            placer.revoke();
            if (ok) return true;
            *why = std::string(label) + ": " + (hook.error() ? hook.error() : "the hook was refused");
            return false;
        }
        *why = std::string(label) + ": " + (placer.error() ? placer.error() : "the checked installation was refused");
        Sleep(5);
    }
    return false;
}

bool lab_switch_set() {
    char b[1024];
    size_t n = 0;
    if (g_api->path(KILN_PATH_DATA, b, sizeof b - 40, &n) != KILN_OK) return false;
    std::string p(b, n);
    p += "\\kiln.knowledge\\roster-64";
    std::wstring w(static_cast<size_t>(MultiByteToWideChar(CP_UTF8, 0, p.data(), static_cast<int>(p.size()), nullptr, 0)), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, p.data(), static_cast<int>(p.size()), w.data(), static_cast<int>(w.size()));
    return GetFileAttributesW(w.c_str()) != INVALID_FILE_ATTRIBUTES;
}

void report_expanded() {
    if (g_limit.expanded())
        row("npcs.expanded_roster", KILN_CAP_AVAILABLE, KILN_CAP_REASON_NONE,
            "installed: the roster holds 128 entries (the game's unlock value and Kiln's second page)");
    else if (g_install_failed)
        row("npcs.expanded_roster", KILN_CAP_UNAVAILABLE, KILN_CAP_REASON_FAULTED, "the roster keeps 64 entries: " + g_install_why);
    else if (g_pages_ok)
        row("npcs.expanded_roster", KILN_CAP_AVAILABLE, KILN_CAP_REASON_NONE,
            "ready: the second page installs when a mod's roster needs more than 64 entries (no hook before)");
    else
        row("npcs.expanded_roster", KILN_CAP_UNAVAILABLE, g_pages_reason, "the roster keeps 64 entries: " + g_pages_why);
}

// ---- snapshots
WorldNow world_now(uint32_t kind) {
    KilnSessionInfo s{};
    s.size = sizeof s;
    WorldNow w;
    if (!valid_kind(kind) || g_api->size < offsetof(KilnApi, session) + sizeof(void*) || g_api->session(&s) != KILN_OK) return w;
    w.live = s.worlds[kind] != 0;
    w.generation = s.sessions[kind];
    return w;
}

bool available(uint32_t kind) { return g_layout_ok && (kind == KILN_WORLD_SERVER || (kind == KILN_WORLD_CLIENT && g_client)); }

struct Seen {
    uintptr_t world = 0, reg = 0;
    uint32_t generation = 0;
    LiveCache live;
    uint64_t sidecar_serial = 0;
    bool any = false;
};
Seen g_seen[3];  // per kind, on that world's thread only

void post(uint32_t kind, const KilnTickInfo* tick) {
    if (!g_wanted.load(std::memory_order_acquire) || !available(kind)) return;
    const uintptr_t world = g_api->world(kind);
    if (!world) return;
    const WorldNow now = world_now(kind);
    if (!now.live || !now.generation) return;
    Seen& seen = g_seen[kind];
    if (seen.world != world || seen.generation != now.generation) {
        seen = Seen{};
        seen.world = world;
        seen.generation = now.generation;
        seen.reg = g_api->singleton(world, kstr("NpcRegistry"));
    }
    if (!seen.reg) return;
    LiveCache live;
    if (!read_live(seen.reg, &live)) return;
    Sidecar page;
    const bool have_page = live.count > kPage && g_sidecars.read(seen.reg, live, &page);
    const uint64_t serial = have_page ? page.serial : 0;
    if (seen.any && live.status == seen.live.status && live.version == seen.live.version && live.payload == seen.live.payload &&
        live.span_ptr == seen.live.span_ptr && live.count == seen.live.count && serial == seen.sidecar_serial)
        return;  // nothing changed since the last snapshot
    std::vector<uint32_t> ids;
    if (live.span_ptr && live.count <= kReadGuard) {
        ids.reserve(static_cast<size_t>(live.count));
        for (uint64_t i = 0; i < live.count; ++i) {
            uint32_t id = 0;
            if (!rd(live.span_ptr + i * static_cast<uint64_t>(R.stride), &id)) break;
            ids.push_back(id);
        }
    }
    if (live.count > g_limit.max() && (!seen.any || seen.live.count != live.count))
        KW("roster: the %s world's NpcCollection has %llu entries, more than this side's maximum %u (an unsupported roster): its unlock "
           "states are not answered", kind == KILN_WORLD_SERVER ? "server" : "client", static_cast<unsigned long long>(live.count), g_limit.max());
    std::vector<Change> changes;
    g_snaps.publish(make_snapshot(kind, now.generation, live, ids, have_page ? &page : nullptr, g_owners, g_limit.max()), &changes);
    g_sidecars.adopt(seen.reg, kind, now.generation);
    seen.live = live;
    seen.sidecar_serial = serial;
    seen.any = true;
    for (const Change& c : changes) emit_roster_change(c.kind, c.generation, c.npc_id, c.before, c.after, c.version, c.revision, tick->step);
}

void KILN_CALL on_server_post(const KilnTickInfo* tick, void*) { post(KILN_WORLD_SERVER, tick); }
void KILN_CALL on_client_post(const KilnTickInfo* tick, void*) { post(KILN_WORLD_CLIENT, tick); }

void want() {
    if (!g_wanted.exchange(true, std::memory_order_acq_rel)) KI("roster: a mod reads the roster: each world publishes its snapshot after its step");
}

}  // namespace

// ============================================================================================= the service ======

void roster_main() {
    g_client = g_api->side() == KILN_SIDE_CLIENT;
    g_layout_ok = g_api->feature(kstr("quests.npc")) && cst_value("quests/npcreg.npcs", &R.npcs) &&
                  cst_value("quests/npcreg.version", &R.version) && cst_value("quests/npcreg.state", &R.state) && R.state > 0 &&
                  R.state < 0x1000;
    R.mask = R.state + 8;  // the result's payload (the second page's check below reads the game's own offset)
    R.stride = 136;        // the NpcResource the catalog reads
    // the second page: the four functions and every constant, and the constants must agree with each other
    const bool sigs = g_api->feature(kstr("quests.npc_pages"));
    int64_t qstate = -1, fnpcs = -1, mask = -1, stride = -1;
    if (!g_layout_ok) {
        g_pages_why = "the NPC registry's layout did not resolve on this build (feature quests.npc)";
    } else if (!sigs) {
        g_pages_why = "the roster's unlock functions did not resolve on this build (feature quests.npc_pages)";
    } else if (!sym_value("quests/NpcEvalUnlocks", &R.eval) || !sym_value("quests/NpcRegistrySetUnlocks", &R.set) ||
               !sym_value("quests/NpcIsUnlocked", &R.query) || !sym_value("quests/NpcFind", &R.find) ||
               !cst_value("quests/npcreg.mask", &mask) || !cst_value("quests/npc.stride", &stride) ||
               !cst_value("quests/npceval.error", &R.eval_error) || !cst_value("quests/npcquery.invalid", &R.query_invalid) ||
               !cst_value("quests/npcquery.state", &qstate) || !cst_value("quests/npcfind.npcs", &fnpcs)) {
        g_pages_why = "a symbol or constant of feature quests.npc_pages is missing";
    } else if (qstate != R.state || fnpcs != R.npcs || mask != R.state + 8 || stride != R.stride || R.eval_error <= 0 ||
               R.eval_error > 0xff || R.query_invalid <= 0 || R.query_invalid > 0xff) {
        g_pages_why = "the roster's functions do not agree with the registry's layout on this build";
        g_pages_reason = KILN_CAP_REASON_FAULTED;
    } else if (!kiln::hooks_v2(g_api) || kiln::hooks_v2(g_api)->header.size < offsetof(KilnHooksApi, mid2_checked) + sizeof(void*)) {
        g_pages_why = "kiln.hooks has no checked installation";
        g_pages_reason = KILN_CAP_REASON_SERVICE_OLDER;
    } else if (lab_switch_set()) {
        g_pages_why = "lab switch kiln/data/kiln.knowledge/roster-64";
        g_pages_reason = KILN_CAP_REASON_POLICY;
    } else {
        g_pages_ok = true;
        g_find = reinterpret_cast<FindFn>(R.find);
    }
    KilnHandle h = 0;
    g_api->subscribe_world_tick(g_owner, KILN_WORLD_SERVER, KILN_TICK_POST, &on_server_post, nullptr, &h);
    if (g_client) g_api->subscribe_world_tick(g_owner, KILN_WORLD_CLIENT, KILN_TICK_POST, &on_client_post, nullptr, &h);
    KI("roster: reads %s; the second unlock page %s", g_layout_ok ? "on" : "OFF",
       g_pages_ok ? "can be installed (only when a mod's roster needs more than 64 entries)" : ("is not available: " + g_pages_why).c_str());
}

void roster_ready() {
    const std::string off = "the NPC registry's layout did not resolve on this build (feature quests.npc)";
    if (g_layout_ok) {
        row("npcs.roster_read", KILN_CAP_AVAILABLE, KILN_CAP_REASON_NONE, "kiln.knowledge, 0.2.0-dev.29: the server world's snapshot");
        row("npcs.roster_budget", KILN_CAP_AVAILABLE, KILN_CAP_REASON_NONE, "kiln.knowledge, 0.2.0-dev.29");
    } else {
        row("npcs.roster_read", KILN_CAP_UNAVAILABLE, KILN_CAP_REASON_SYMBOL_MISSING, off);
        row("npcs.roster_budget", KILN_CAP_UNAVAILABLE, KILN_CAP_REASON_SYMBOL_MISSING, off);
    }
    if (!g_client)
        row("npcs.client_roster_read", KILN_CAP_UNAVAILABLE, KILN_CAP_REASON_WRONG_SIDE, "the client world's roster: a game client only");
    else if (g_layout_ok)
        row("npcs.client_roster_read", KILN_CAP_AVAILABLE, KILN_CAP_REASON_NONE, "kiln.knowledge, 0.2.0-dev.29: the client world's snapshot");
    else
        row("npcs.client_roster_read", KILN_CAP_UNAVAILABLE, KILN_CAP_REASON_SYMBOL_MISSING, off);
    std::lock_guard lock(g_install_mu);
    report_expanded();
}

uint32_t roster_max(uint32_t needed, std::string* why) {
    std::lock_guard lock(g_install_mu);
    if (g_limit.expanded()) return kExpanded;
    if (!g_pages_ok || g_install_failed) {
        if (why) *why = g_install_failed ? g_install_why : g_pages_why;
        return kPage;
    }
    if (needed > kExpanded) return kExpanded;  // refused at 128 by the build: nothing to install for it
    std::string err;
    bool ok = install_one(g_query_hook, R.query, reinterpret_cast<void*>(&hk_query), "quests/NpcIsUnlocked", &err);
    ok = ok && install_one(g_set_hook, R.set, reinterpret_cast<void*>(&hk_set), "quests/NpcRegistrySetUnlocks", &err);
    ok = ok && install_one(g_eval_hook, R.eval, reinterpret_cast<void*>(&hk_eval), "quests/NpcEvalUnlocks", &err);
    if (!ok) {
        g_eval_hook.revoke();
        g_set_hook.revoke();
        g_query_hook.revoke();
        g_install_failed = true;
        g_install_why = "the hooks could not be installed (" + err + ")";
        KW("roster: the second unlock page is not installed: %s; the roster keeps 64 entries", g_install_why.c_str());
        if (why) *why = g_install_why;
        report_expanded();
        return kPage;
    }
    g_limit.expand();
    KI("roster: a mod's roster needs %u entries: the second unlock page is installed (quests/NpcEvalUnlocks, NpcRegistrySetUnlocks, "
       "NpcIsUnlocked); the roster holds 128", needed);
    report_expanded();
    return kExpanded;
}

void roster_built(std::vector<uint32_t> boot_ids, std::vector<uint32_t> kiln_ids) {
    g_owners.boot_ids = std::move(boot_ids);
    g_owners.kiln_ids = std::move(kiln_ids);
    g_owners.known = true;
}

uint32_t roster_limit() { return g_limit.max(); }

void roster_world_event(const KilnWorldEvent* e) {
    if (!e || e->event != KILN_WORLD_STOPPING || !valid_kind(e->kind)) return;
    g_snaps.end(e->kind, e->session);
    g_sidecars.drop_world(e->kind);
    if (g_limit.expanded() || g_wanted.load())
        KI("roster: the %s world stopped: its snapshots and second pages are gone (paged %llu, attached %llu, unpaired %llu, answered "
           "%llu, failed closed %llu)", e->kind == KILN_WORLD_SERVER ? "server" : "client", static_cast<unsigned long long>(c_paged.load()),
           static_cast<unsigned long long>(c_attached.load()), static_cast<unsigned long long>(c_unpaired.load()),
           static_cast<unsigned long long>(c_answered.load()), static_cast<unsigned long long>(c_closed.load()));
}

bool npc_unlock_pages(uintptr_t registry, uint64_t pages[2], uint32_t* npages, uint16_t* version, uint32_t* count) {
    LiveCache live;
    if (!g_layout_ok || !read_live(registry, &live) || live.status != 0) return false;
    pages[0] = live.payload;
    pages[1] = 0;
    *npages = 1;
    *version = live.version;
    *count = static_cast<uint32_t>(std::min<uint64_t>(live.count, kReadGuard));
    Sidecar page;
    if (live.count > kPage && g_sidecars.read(registry, live, &page)) {
        pages[1] = page.masks[1];
        *npages = 2;
    }
    return true;
}

KilnResult KILN_CALL roster_world(uint32_t kind, uint32_t* out_generation, uint32_t* out_readiness) {
    want();
    return kiln::roster::world(g_snaps, kind, world_now(kind), available(kind), out_generation, out_readiness);
}
KilnResult KILN_CALL roster_budget(uint32_t kind, uint32_t generation, KilnCapacityInfo* out) {
    want();
    return kiln::roster::budget(g_snaps, kind, generation, world_now(kind), available(kind), g_limit.max(), g_limit.expanded(), out);
}
KilnResult KILN_CALL npc_unlock_at(uint32_t kind, uint32_t generation, uint32_t npc_id, KilnNpcUnlockReply* out) {
    want();
    return kiln::roster::unlock_at(g_snaps, kind, generation, npc_id, world_now(kind), available(kind), out);
}

}  // namespace kiln::knowledge
