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
// thread that evaluated it. Which world a registry belongs to is set by the world itself, on its own thread, at its
// start and at every Pre (Sidecars::bind: its NpcRegistry singleton, under its core session); a page is attached only
// for a registry a live world bound, kept under that (kind, session), and only that world's stop removes it. Nothing
// reads another world's memory to learn who owns what. An id at index 64 or more is never answered from the game's
// value (that would be another entry's bit): from the registry's page while it matches what the game's cache holds
// and the world that owns the registry did not change during the read (page_query), else with the game's own "cache
// invalid" status 1, which its only caller (unique_npc_spawns) treats like an unlocked NPC: kept (counted: no page,
// stale, unowned, moved, busy). Everything else goes to the game as it was asked.
//
// On a dedicated server, a page missing from a computed cache (a setter that did not pair, a busy lock, a world
// that bound its registry late) is computed again at that world's next Pre, where nothing of its step runs: the game's evaluator on
// the knowledge context the game itself passed for that world, kept only when it agrees with the cache (page 0 and
// version); at most twice per cache. On clients recovery stays off until verified; missing pages stay kept
// until the game's next evaluation.
//
// A roster the boot build cannot hold (more than 128 entries, or more than 64 where the second page cannot be
// installed) stops the start at ResourceInit, before any world loads (native_pack.cpp asks start_stop.cpp's
// refuse_start, StartStop::Roster): a world loaded without the mods' NPCs and knowledge would drop their progress at its
// next save.
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>
#include <string_view>
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

std::atomic<uint64_t> c_paged{0}, c_attached{0}, c_unpaired{0}, c_answered{0}, c_closed{0}, c_faults{0}, c_off_tick{0};
// why an attach did not happen, why a query was kept, what the recovery did, failed registry reads
std::atomic<uint64_t> c_att_unowned{0}, c_att_old{0}, c_att_busy{0};
std::atomic<uint64_t> c_kept_nopage{0}, c_kept_stale{0}, c_kept_unowned{0}, c_kept_moved{0}, c_kept_busy{0}, c_kept_noctx{0},
    c_kept_unreadable{0};
std::atomic<uint64_t> c_recovered{0}, c_recover_failed{0}, c_recover_noctx{0}, c_unreadable{0};
std::atomic<bool> g_said_eval_off{false}, g_said_query_off{false};
std::atomic<int> g_ctx_is_singleton[3] = {-1, -1, -1};  // per kind: the game's context is the world's G38Knowledge

// kiln/control switches, read once at the start and logged at every start while present (labs; never needed in play):
//   roster.fail-install  the hooks are not installed (as a failed installation): a roster past 64 stops the start
//   roster.no-pages      NEGATIVE CONTROL, only together with roster.lab: the build takes 128 without the second page, no
//                        hook (page-2 states are wrong); alone it is ignored, with a warning
//   roster.lab           lab diagnostics: an attach line per page, and roster.drop-page (polled) drops a world's page
bool g_ctl_fail_install = false, g_ctl_no_pages = false, g_ctl_lab = false;

// Once per process: where the hooks ran (a dedicated server runs the roster programs on workers once a player is in;
// a lab proves it with kiln.world's parallel switch). The log is the lab's evidence.
void note_thread(std::atomic<bool>& said, const char* what) {
    if (g_api->on_tick_thread()) return;
    c_off_tick.fetch_add(1, std::memory_order_relaxed);
    if (!said.exchange(true)) KI("roster: %s on thread %lu, not the server world's tick thread", what, GetCurrentThreadId());
}

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
    uint64_t epochs[3];
    g_sidecars.epochs(epochs);  // before the evaluation: a world that stops during it gets no page from it
    const auto original = g_eval_hook.original<EvalFn>();
    const PageOutcome o = eval_pages(out, ctx, seen, s, static_cast<uint64_t>(R.stride), static_cast<uint8_t>(R.eval_error),
                                     [original](Result16* r, void* c, uint16_t* v, const Span* sp) { original(r, c, v, sp); });
    c_paged.fetch_add(1, std::memory_order_relaxed);
    note_thread(g_said_eval_off, "a paged evaluation ran");
    if (o.complete) {
        Pending p;
        for (uint32_t k = 0; k < 3; ++k) p.epochs[k] = epochs[k];
        p.ctx = reinterpret_cast<uintptr_t>(ctx);
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
        switch (g_sidecars.attach(reg, p)) {
            case Sidecars::Attach::Attached:
                c_attached.fetch_add(1, std::memory_order_relaxed);
                if (g_ctl_lab) KI("roster: lab: a page attached by the game's own evaluation (version %u)", version);
                return;
            case Sidecars::Attach::Unowned: c_att_unowned.fetch_add(1, std::memory_order_relaxed); break;
            case Sidecars::Attach::Old: c_att_old.fetch_add(1, std::memory_order_relaxed); break;
            case Sidecars::Attach::Busy: c_att_busy.fetch_add(1, std::memory_order_relaxed); return;  // the old page no longer matches
        }
        g_sidecars.invalidate(reg);
        return;
    }
    c_unpaired.fetch_add(count > kPage, std::memory_order_relaxed);
    g_sidecars.invalidate(reg);  // whatever page this registry had no longer belongs to its cache
}

uint8_t* hk_query(uint8_t* out, uintptr_t reg, uint32_t id) {
    bool fault = false;
    const uint8_t* res = out && reg && g_find ? find_guarded(reg, id, &fault) : nullptr;
    if (fault) c_faults.fetch_add(1, std::memory_order_relaxed);
    if (!res) return g_query_hook.call<uint8_t*>(out, reg, id);  // unknown id, unreadable registry: the game's own answer
    const uintptr_t at = reinterpret_cast<uintptr_t>(res);
    const QueryAnswer a = page_query(
        g_sidecars, reg, [reg](LiveCache* l) { return read_live(reg, l); },
        [at](const LiveCache& l, uint64_t* index) {
            if (!l.span_ptr || at < l.span_ptr) return false;
            *index = (at - l.span_ptr) / static_cast<uint64_t>(R.stride);  // past the collection: the page says Stale
            return true;
        });
    if (a.kind == QueryAnswer::Kind::Game) return g_query_hook.call<uint8_t*>(out, reg, id);
    uint8_t status = static_cast<uint8_t>(R.query_invalid), value = 0;
    if (a.kind == QueryAnswer::Kind::Page) {
        status = 0;
        value = a.unlocked;
        c_answered.fetch_add(1, std::memory_order_relaxed);
        note_thread(g_said_query_off, "an id past 63 was answered from its page");
    } else {
        c_closed.fetch_add(1, std::memory_order_relaxed);
        switch (a.why) {
            case Sidecars::Bit::Stale: c_kept_stale.fetch_add(1, std::memory_order_relaxed); break;
            case Sidecars::Bit::Unowned: c_kept_unowned.fetch_add(1, std::memory_order_relaxed); break;
            case Sidecars::Bit::Moved: c_kept_moved.fetch_add(1, std::memory_order_relaxed); break;
            case Sidecars::Bit::Busy: c_kept_busy.fetch_add(1, std::memory_order_relaxed); break;
            case Sidecars::Bit::NoContext: c_kept_noctx.fetch_add(1, std::memory_order_relaxed); break;
            case Sidecars::Bit::Unreadable: c_kept_unreadable.fetch_add(1, std::memory_order_relaxed); break;
            default: c_kept_nopage.fetch_add(1, std::memory_order_relaxed); break;
        }
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

std::wstring wide(const std::string& s) {
    std::wstring w(static_cast<size_t>(MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0)), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), static_cast<int>(w.size()));
    return w;
}
std::wstring kiln_path(uint32_t kind, const char* leaf) {
    char b[1024];
    size_t n = 0;
    if (g_api->path(kind, b, sizeof b - 1, &n) != KILN_OK) return {};
    return wide(std::string(b, n) + "\\" + leaf);
}
bool control_file(const char* leaf) {
    const std::wstring p = kiln_path(KILN_PATH_CONTROL, leaf);
    return !p.empty() && GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool eval_guarded(EvalFn f, Result16* r, void* c, uint16_t* v, const Span* sp) {
    __try {
        f(r, c, v, sp);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

const char* kind_name(uint32_t kind) { return kind == KILN_WORLD_SERVER ? "server" : kind == KILN_WORLD_CLIENT ? "client" : "prediction"; }

void report_expanded() {
    if (g_ctl_no_pages)
        row("npcs.expanded_roster", KILN_CAP_UNAVAILABLE, KILN_CAP_REASON_POLICY,
            "LAB NEGATIVE CONTROL kiln/control/roster.no-pages: 128 entries without the second page; page-2 unlock states are wrong");
    else if (g_limit.expanded())
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

// One world kind as its own thread sees it (Pre, Post and STARTED run there; nothing else touches it).
struct WorldSeen {
    uintptr_t world = 0, reg = 0, knowledge = 0;
    uint32_t session = 0;
    uint32_t steps = 0, pre_steps = 0;
    bool looked = false, bound = false, bind_refused = false;
    // the snapshot last published
    LiveCache live;
    uint64_t sidecar_serial = 0;
    bool any = false, unreadable = false;
    // the recovery of a lost page: the cache tried and how often
    LiveCache tried;
    uint32_t tries = 0;
};
WorldSeen g_seen[3];

// The world of `kind` now, its registry looked up on every step until it has one and then every 64th step (a registry
// replaced within the session), and bound to its session while the second page is installed. World thread only.
WorldSeen* resolve(uint32_t kind) {
    const uintptr_t world = g_api->world(kind);
    if (!world) return nullptr;
    const WorldNow now = world_now(kind);
    if (!now.live || !now.generation) return nullptr;
    WorldSeen& w = g_seen[kind];
    if (w.world != world || w.session != now.generation) {
        w = WorldSeen{};
        w.world = world;
        w.session = now.generation;
    }
    if (!w.reg || (++w.steps & 63) == 0) {
        const uintptr_t reg = g_api->singleton(world, kstr("NpcRegistry"));
        if (!w.looked || reg != w.reg) {
            if (w.reg && reg != w.reg) KI("roster: the %s world's NpcRegistry is another object now", kind_name(kind));
            w.looked = true;
            w.reg = reg;
            w.knowledge = g_api->singleton(world, kstr("G38Knowledge"));
            w.any = false;
            w.bound = w.bind_refused = false;
        }
    }
    // bound to this session while the second page is installed (a session that already stopped is refused, once)
    if (w.reg && !w.bound && !w.bind_refused && g_limit.expanded() && !g_ctl_no_pages) {
        w.bound = g_sidecars.bind(kind, w.session, w.reg);
        w.bind_refused = !w.bound;
    }
    return &w;
}

// A lost page (a computed cache of more than 64 entries without its page), computed again at the world's safe point.
void recover(uint32_t kind, WorldSeen& w) {
    // Neither hosted-server nor client-world recovery may call the client executable's evaluator
    // outside its program until its side effects and context lifetime have been verified in a client.
    if (g_client) return;
    LiveCache live;
    if (!read_live(w.reg, &live) || !g_sidecars.wants_page(kind, w.session, w.reg, live)) {
        w.tries = 0;
        return;
    }
    if (!(live == w.tried)) w.tried = live, w.tries = 0;
    if (w.tries >= 2) return;  // this cache was tried: the producer's next evaluation (a knowledge change) attaches
    ++w.tries;
    // only the context the game itself passed for this world, and only while it is the world's G38Knowledge now (a
    // temporary the game passed, or a world whose knowledge object changed, would be a stale pointer): otherwise the
    // page stays missing and its ids are kept, counted as "no checked context"
    const uintptr_t ctx = g_sidecars.checked_context(kind, w.session, g_api->singleton(w.world, kstr("G38Knowledge")));
    if (!ctx) {
        if (c_recover_noctx.fetch_add(1, std::memory_order_relaxed) < 4)
            KW("roster: the %s world's second page is missing and is not computed again: the knowledge context the game passed is not "
               "the world's G38Knowledge now (or none was passed yet); its ids past 63 are kept until the game evaluates them",
               kind_name(kind));
        return;
    }
    Pending p;
    g_sidecars.epochs(p.epochs);
    const auto original = g_eval_hook.original<EvalFn>();
    bool fault = false;
    const bool ok = recompute(live, Span{live.span_ptr, live.count}, static_cast<uint64_t>(R.stride), static_cast<uint8_t>(R.eval_error),
                              reinterpret_cast<void*>(ctx),
                              [&](Result16* r, void* c, uint16_t* v, const Span* sp) {
                                  if (!eval_guarded(original, r, c, v, sp)) {
                                      fault = true;
                                      *r = Result16{static_cast<uint8_t>(R.eval_error), {}, 0, 0};
                                  }
                              },
                              &p, !g_client);
    LiveCache again;
    if (ok && !fault && read_live(w.reg, &again) && again == live && g_sidecars.attach(w.reg, p) == Sidecars::Attach::Attached) {
        const uint64_t n = c_recovered.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 8 || n % 1000 == 0)
            KI("roster: the %s world's second page was missing for its cache (version %u): computed again at the step's start "
               "(%llu so far)", kind_name(kind), live.version, static_cast<unsigned long long>(n));
        return;
    }
    if (fault) c_faults.fetch_add(1, std::memory_order_relaxed);
    if (c_recover_failed.fetch_add(1, std::memory_order_relaxed) < 8)
        KW("roster: the %s world's second page could not be computed again (version %u%s); its ids past 63 are kept until the "
           "knowledge changes", kind_name(kind), live.version, fault ? ", the evaluator faulted" : "");
}

void pre(uint32_t kind) {
    if (!g_limit.expanded() || g_ctl_no_pages || !available(kind)) return;
    WorldSeen* w = resolve(kind);
    if (!w || !w->bound) return;
    if (g_ctx_is_singleton[kind].load() < 0) {
        if (const uintptr_t ctx = g_sidecars.context(kind, w->session)) {  // the first page the game's evaluation attached
            const bool same = w->knowledge && ctx == w->knowledge;
            g_ctx_is_singleton[kind].store(same ? 1 : 0);
            KI("roster: the %s world's evaluator context %s its G38Knowledge singleton%s", kind_name(kind), same ? "is" : "is NOT",
               same ? "" : " (a lost page is computed again only with the context the game passed)");
        }
    }
    if (g_ctl_lab && (++w->pre_steps % 30) == 0 && control_file("roster.drop-page")) {
        DeleteFileW(kiln_path(KILN_PATH_CONTROL, "roster.drop-page").c_str());
        g_sidecars.drop_page(kind);
        KI("roster: lab: the %s world's second page dropped (kiln/control/roster.drop-page; attached %llu, recovered %llu)", kind_name(kind),
           static_cast<unsigned long long>(c_attached.load()), static_cast<unsigned long long>(c_recovered.load()));
    }
    recover(kind, *w);
}

void post(uint32_t kind, const KilnTickInfo* tick) {
    if (!g_wanted.load(std::memory_order_acquire) || !available(kind)) return;
    WorldSeen* wp = resolve(kind);
    if (!wp) return;
    WorldSeen& seen = *wp;
    LiveCache live;
    if (!seen.reg || !read_live(seen.reg, &live)) {
        // no answer this step (never the last ready snapshot): NotReady until the registry reads again
        if (!seen.unreadable) {
            c_unreadable.fetch_add(1, std::memory_order_relaxed);
            g_snaps.publish(unreadable_snapshot(kind, seen.session));
            seen.unreadable = true;
            seen.any = false;
        }
        return;
    }
    seen.unreadable = false;
    Sidecar page;
    const bool have_page = live.count > kPage && seen.bound && g_sidecars.read(kind, seen.session, seen.reg, live, &page);
    const uint64_t serial = have_page ? page.serial : 0;
    if (seen.any && live == seen.live && serial == seen.sidecar_serial) return;  // nothing changed since the last snapshot
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
        KE("roster: the %s world's NpcCollection has %llu entries, more than this side's maximum %u (an "
             "unsupported roster): its unlock states are not answered", kind_name(kind), static_cast<unsigned long long>(live.count),
             g_limit.max());
    std::vector<Change> changes;
    g_snaps.publish(make_snapshot(kind, seen.session, live, ids, have_page ? &page : nullptr, g_owners, g_limit.max()), &changes);
    seen.live = live;
    seen.sidecar_serial = serial;
    seen.any = true;
    for (const Change& c : changes) emit_roster_change(c.kind, c.generation, c.npc_id, c.before, c.after, c.version, c.revision, tick->step);
}

void KILN_CALL on_server_pre(const KilnTickInfo*, void*) { pre(KILN_WORLD_SERVER); }
void KILN_CALL on_client_pre(const KilnTickInfo*, void*) { pre(KILN_WORLD_CLIENT); }
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
    } else if (!g_api->feature(kstr("singletons"))) {
        g_pages_why = "the worlds' singletons cannot be read on this build (feature singletons): a page could not be kept per world";
    } else {
        g_pages_ok = true;
        g_find = reinterpret_cast<FindFn>(R.find);
    }
    // the switches, loud at every start while present
    g_ctl_fail_install = control_file("roster.fail-install");
    g_ctl_lab = control_file("roster.lab");
    g_ctl_no_pages = control_file("roster.no-pages");
    if (g_ctl_no_pages && !g_ctl_lab) {
        g_ctl_no_pages = false;
        KW("roster: kiln/control/roster.no-pages is present but IGNORED: the negative control runs only in a lab, with "
           "kiln/control/roster.lab too; delete the file");
    }
    if (g_ctl_fail_install)
        KW("roster: LAB SWITCH kiln/control/roster.fail-install is present: the second unlock page is not installed, so a roster of "
           "more than 64 entries stops the start; delete the file to play");
    if (g_ctl_no_pages)
        KE("roster: LAB NEGATIVE CONTROL kiln/control/roster.no-pages is present: a roster of up to 128 is "
             "built WITHOUT the second unlock page, so the unlock states of entries 64 and up are wrong and the game can keep or remove "
             "the wrong NPCs; never on a save you keep. Delete the file to play");
    if (g_ctl_lab) KW("roster: lab switch kiln/control/roster.lab is present: lab diagnostics (an attach line per page, kiln/control/roster.drop-page)");
    KilnHandle h = 0;
    g_api->subscribe_world_tick(g_owner, KILN_WORLD_SERVER, KILN_TICK_PRE, &on_server_pre, nullptr, &h);
    g_api->subscribe_world_tick(g_owner, KILN_WORLD_SERVER, KILN_TICK_POST, &on_server_post, nullptr, &h);
    if (g_client) {
        g_api->subscribe_world_tick(g_owner, KILN_WORLD_CLIENT, KILN_TICK_PRE, &on_client_pre, nullptr, &h);
        g_api->subscribe_world_tick(g_owner, KILN_WORLD_CLIENT, KILN_TICK_POST, &on_client_post, nullptr, &h);
    }
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
    if (g_ctl_no_pages) {
        g_limit.expand();
        KE("roster: LAB NEGATIVE CONTROL: a mod's roster needs %u entries: taken WITHOUT the second unlock "
             "page (kiln/control/roster.no-pages); entries 64 and up share the bits of entries 0 to 63", needed);
        report_expanded();
        return kExpanded;
    }
    if (g_pages_ok && !g_install_failed && g_ctl_fail_install && needed <= kExpanded) {
        g_install_failed = true;
        g_install_why = "the hooks were not installed (lab switch kiln/control/roster.fail-install)";
        KW("roster: the second unlock page is not installed: %s; the roster keeps 64 entries", g_install_why.c_str());
        report_expanded();
    }
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
    if (!e || !valid_kind(e->kind)) return;
    if (e->event == KILN_WORLD_STARTED) {  // on the world's thread, before its first Pre: the world binds its registry
        if (g_limit.expanded() && !g_ctl_no_pages && available(e->kind)) resolve(e->kind);
        return;
    }
    if (e->event != KILN_WORLD_STOPPING) return;
    g_snaps.end(e->kind, e->session);
    g_sidecars.unbind(e->kind, e->session);  // this world's pages only; the other world's stay
    if (g_limit.expanded() || g_wanted.load())
        KI("roster: the %s world stopped: its snapshots and second pages are gone (paged %llu, attached %llu, not attached: unowned %llu, "
           "old %llu, busy %llu; unpaired %llu, answered %llu, kept %llu: no page %llu, stale %llu, unowned %llu, moved %llu, busy %llu, "
           "no checked context %llu, unreadable %llu; recovered %llu, recovery failed %llu, without a checked context %llu; snapshot reads "
           "failed %llu; off the tick thread %llu, lock busy %llu)",
           kind_name(e->kind), static_cast<unsigned long long>(c_paged.load()), static_cast<unsigned long long>(c_attached.load()),
           static_cast<unsigned long long>(c_att_unowned.load()), static_cast<unsigned long long>(c_att_old.load()),
           static_cast<unsigned long long>(c_att_busy.load()), static_cast<unsigned long long>(c_unpaired.load()),
           static_cast<unsigned long long>(c_answered.load()), static_cast<unsigned long long>(c_closed.load()),
           static_cast<unsigned long long>(c_kept_nopage.load()), static_cast<unsigned long long>(c_kept_stale.load()),
           static_cast<unsigned long long>(c_kept_unowned.load()), static_cast<unsigned long long>(c_kept_moved.load()),
           static_cast<unsigned long long>(c_kept_busy.load()), static_cast<unsigned long long>(c_kept_noctx.load()),
           static_cast<unsigned long long>(c_kept_unreadable.load()), static_cast<unsigned long long>(c_recovered.load()),
           static_cast<unsigned long long>(c_recover_failed.load()), static_cast<unsigned long long>(c_recover_noctx.load()),
           static_cast<unsigned long long>(c_unreadable.load()), static_cast<unsigned long long>(c_off_tick.load()),
           static_cast<unsigned long long>(g_sidecars.busy()));
}

bool npc_unlock_pages(uintptr_t registry, uint64_t pages[2], uint32_t* npages, uint16_t* version, uint32_t* count) {
    LiveCache live;
    if (!g_layout_ok || !read_live(registry, &live) || live.status != 0) return false;
    // a roster longer than this side's maximum (or a damaged one): no entry's state is answered, not even below 64
    // (the game's value then holds two entries in a bit)
    if (live.count > g_limit.max() || live.count > kReadGuard) return false;
    pages[0] = live.payload;
    pages[1] = 0;
    *npages = 1;
    *version = live.version;
    *count = static_cast<uint32_t>(live.count);
    uint32_t kind = kKindUnknown, session = 0;
    Sidecar page;
    if (live.count > kPage && g_sidecars.owner(registry, &kind, &session) == Sidecars::Owner::Found &&
        g_sidecars.read(kind, session, registry, live, &page)) {
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
