// SPDX-License-Identifier: MIT
// kiln.repl's per-peer stats (kiln_repl.h stats_enable / peer_stats; the dossier's chapter 11 §4.1, §2.5, §2.6).
//
// Four raw detours, installed while any owner holds the stats (the first enable zeroes the counters, the last disable
// or the owner's revoke removes them):
//   SendCallback   job, once per (machine, channel) and frame: after the game's callback, batch->sent says how many of
//                  the batch's messages the session took (in order; at the first it could not allocate it hands the rest
//                  back): frames, messages, bytes, refused, creates, updates, destroy lists and their ids;
//   EntityWriter   job, once per entity message the queue reader asks for: a skipped message is "unchanged" when the
//                  context's unchanged counter (ctx+0x8db78) moved during the call, else "too big" (§2.6);
//   AckReceive     tick: ack messages per machine and channel;
//   EcsReset       tick: reset requests per machine and channel.
// Each hook has guard.h's shape (the game's function runs exactly once, outside the guard). Counters live per
// (machine, channel), a cache line each, so the game's parallel jobs never share one. Nothing allocates in a hook: the
// operator new of kiln-repl.dll counts any allocation made inside observer code (lab_hook_cost reports it; 0 by design).
//
// A machine's counters start over when the peer in its slot changes and when the world stops. Machine 0 of a hosted
// world is the local client (KILN_REPL_PEER_LOCAL). On lab servers the hooks' own code is timed (rdtsc, per (machine,
// channel)), with the game's send step and frame interval beside it: lab_hook_cost.
#include <windows.h>

#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <iterator>
#include <cstdlib>
#include <cstring>
#include <malloc.h>
#include <mutex>
#include <new>
#include <vector>

#include "kiln_hook.hpp"
#include "repl.h"
#include "services/kiln_hooks.h"
#include "wire.h"

namespace repl {

std::atomic<bool> g_measure{false};
std::atomic<uint64_t> g_observer_allocs{0};

namespace {

using Counter = std::atomic<uint64_t>;
inline void add(Counter& c, uint64_t n = 1) { c.fetch_add(n, std::memory_order_relaxed); }
inline uint64_t get(const Counter& c) { return c.load(std::memory_order_relaxed); }
inline uint64_t tsc() { return g_measure.load(std::memory_order_relaxed) ? __rdtsc() : 0; }

// One (machine, channel): written by its own jobs and the tick thread's ack/reset hooks; read in Pre.
struct alignas(64) Slot {
    Counter frames{0}, messages{0}, bytes{0}, refused{0}, creates{0}, updates{0}, destroy_lists{0}, destroyed_ids{0};
    Counter unchanged{0}, too_big{0}, acks{0}, resets{0};
    Counter cb_calls{0}, cb_cycles{0}, wr_calls{0}, wr_cycles{0};  // lab: the hooks' own code on this (machine, channel)

    void clear_stats() {
        for (Counter* c : {&frames, &messages, &bytes, &refused, &creates, &updates, &destroy_lists, &destroyed_ids, &unchanged,
                           &too_big, &acks, &resets})
            c->store(0, std::memory_order_relaxed);
    }
    void clear_cost() {
        for (Counter* c : {&cb_calls, &cb_cycles, &wr_calls, &wr_cycles}) c->store(0, std::memory_order_relaxed);
    }
    bool any() const { return get(frames) || get(acks) || get(resets) || get(unchanged) || get(too_big); }
};
Slot g_slot[KILN_REPL_MACHINES][2];
Counter g_tick_calls{0}, g_tick_cycles{0};

// ---- owners holding the stats ----
constexpr uint32_t kMaxOwners = 64;
std::mutex g_owners_mu;                // the tick thread and an owner-retiring callback (any thread)
KilnOwner g_owners[kMaxOwners];
uint32_t g_owner_n = 0;
std::atomic<bool> g_hooks_dirty{false};  // an owner went off the tick thread: Pre applies it
std::atomic<bool> g_counting{false};     // some owner holds the stats (the hooks may be in for a watch alone)

// ---- the hooks ----
Guard g_cb_guard{"stats-callback"}, g_wr_guard{"stats-writer"}, g_ack_guard{"stats-ack"}, g_reset_guard{"stats-reset"};
kiln::RawHook h_cb, h_wr, h_ack, h_reset;
bool g_send_hooked = false;  // tick thread: SendCallback and EntityWriter (the stats and the watches)
bool g_tick_hooked = false;  // tick thread: AckReceive and EcsReset (the stats only)

using CallbackFn = void (*)(CbPair* pair);
using WriterFn = uint64_t (*)(WriterOut* out, uint8_t* args);
using AckFn = void (*)(uintptr_t world, uint64_t machine, const Slice* msg, uint64_t pred);
using ResetFn = void (*)(uintptr_t world, uint64_t machine, const Slice* ids, uint64_t pred);

// SendCallback
struct CbCall {
    CbPair* pair;
};
void cb_after(void* p) {
    const uint64_t t0 = tsc();
    const auto* c = static_cast<CbCall*>(p);
    const Batch* b = c->pair ? c->pair->batch : nullptr;
    if (!b || b->machine >= KILN_REPL_MACHINES) return;
    Slot& s = g_slot[b->machine][b->pred & 1];
    if (g_counting.load(std::memory_order_relaxed)) {
        const BatchCount n = count_batch(b->msgs, b->count, b->sent);
        add(s.frames);
        add(s.messages, n.messages);
        add(s.bytes, n.bytes);
        add(s.refused, n.refused);
        add(s.creates, n.creates);
        add(s.updates, n.updates);
        add(s.destroy_lists, n.destroy_lists);
        add(s.destroyed_ids, n.destroyed_ids);
    }
    if (watch_active()) watch_callback(b, c->pair->gs);
    if (t0) {
        add(s.cb_calls);
        add(s.cb_cycles, __rdtsc() - t0);
    }
}
void d_cb(CbPair* pair) {
    CbCall c{pair};
    detour(g_cb_guard, &c, nullptr, &cb_after, h_cb.original<CallbackFn>(), pair);
}

// EntityWriter
struct WrCall {
    WriterOut* out;
    uint8_t* args;
    uint64_t result;
    uint64_t cycles;
    uintptr_t ctx;
    uint64_t unchanged_before;
    int32_t machine;
    uint8_t pred;
};
void wr_before(void* p) {
    const uint64_t t0 = tsc();
    auto* c = static_cast<WrCall*>(p);
    const uintptr_t world = *reinterpret_cast<const uintptr_t*>(c->args + kWriterWorld);
    const uintptr_t ctx = *reinterpret_cast<const uintptr_t*>(c->args + kWriterCtx);
    uint8_t pred = 0;
    const int m = machine_of_ctx(world, ctx, &pred);
    if (m < 0) return;
    c->ctx = ctx;
    c->pred = pred;
    if (g_counting.load(std::memory_order_relaxed)) c->unchanged_before = *reinterpret_cast<const uint64_t*>(ctx + kCtxUnchanged);
    c->machine = m;
    watch_writer_before(c->args, ctx, m, pred);
    if (t0) c->cycles = __rdtsc() - t0;
}
void wr_after(void* p) {
    const uint64_t t0 = tsc();
    auto* c = static_cast<WrCall*>(p);
    if (c->machine < 0) return;
    Slot& s = g_slot[c->machine][c->pred & 1];
    if (g_counting.load(std::memory_order_relaxed) && (c->result & 0xff) && c->out && c->out->skipped) {  // bool in AL
        // §2.6: nothing changed moves the context's unchanged counter; a message over the channel's maximum does not
        if (*reinterpret_cast<const uint64_t*>(c->ctx + kCtxUnchanged) != c->unchanged_before) add(s.unchanged);
        else add(s.too_big);
    }
    watch_writer_after(c->out, c->result);
    if (t0) {
        add(s.wr_calls);
        add(s.wr_cycles, c->cycles + (__rdtsc() - t0));
    }
}
uint64_t d_wr(WriterOut* out, uint8_t* args) {
    WrCall c{out, args, 0, 0, 0, 0, -1, 0};
    const uint64_t r = detour(g_wr_guard, &c, &wr_before, &wr_after, h_wr.original<WriterFn>(), out, args);
    watch_writer_done();  // outside the guard: no watch reservation outlives the writer call
    return r;
}

// AckReceive, EcsReset (tick thread)
struct TickCall {
    uint64_t machine, pred;
    bool reset;
};
void tick_before(void* p) {
    const uint64_t t0 = tsc();
    const auto* c = static_cast<TickCall*>(p);
    const uint32_t m = static_cast<uint8_t>(c->machine);
    if (m >= KILN_REPL_MACHINES || !g_counting.load(std::memory_order_relaxed)) return;
    Slot& s = g_slot[m][static_cast<uint8_t>(c->pred) & 1];
    add(c->reset ? s.resets : s.acks);
    if (t0) {
        add(g_tick_calls);
        add(g_tick_cycles, __rdtsc() - t0);
    }
}
void d_ack(uintptr_t world, uint64_t machine, const Slice* msg, uint64_t pred) {
    TickCall c{machine, pred, false};
    detour(g_ack_guard, &c, &tick_before, nullptr, h_ack.original<AckFn>(), world, machine, msg, pred);
}
void d_reset(uintptr_t world, uint64_t machine, const Slice* ids, uint64_t pred) {
    TickCall c{machine, pred, true};
    detour(g_reset_guard, &c, &tick_before, nullptr, h_reset.original<ResetFn>(), world, machine, ids, pred);
}

struct One {
    kiln::RawHook* h;
    uintptr_t target;
    void* fn;
    const char* label;
};

// Installs a group of hooks, all or none.
bool install_all(const One* all, size_t n, const char* what) {
    for (size_t i = 0; i < n; ++i)
        if (!all[i].h->install(g_api, g_owner, all[i].target, all[i].fn, all[i].label)) {
            logf(KILN_LOG_WARN, "repl: %s hooks not installed (%s: %s)", what, all[i].label, all[i].h->error() ? all[i].h->error() : "?");
            for (size_t k = 0; k < n; ++k) all[k].h->revoke();
            return false;
        }
    return true;
}

}  // namespace

// Tick thread: the hooks follow the owners (counting) and the watches (the writer and the callback serve both). True
// when every hook wanted now is in (false: kiln.hooks refused one; it is asked again at the next call).
bool stats_apply_hooks() {
    g_hooks_dirty.store(false, std::memory_order_relaxed);
    bool counting;
    {
        std::lock_guard lock(g_owners_mu);
        counting = g_owner_n > 0;
    }
    g_counting.store(counting, std::memory_order_relaxed);
    const bool want_send = counting || watch_active(), want_tick = counting;
    if (want_send != g_send_hooked) {
        if (want_send) {
            const One send[] = {
                {&h_cb, F.send_callback, reinterpret_cast<void*>(&d_cb), "repl/stats SendCallback"},
                {&h_wr, F.entity_writer, reinterpret_cast<void*>(&d_wr), "repl/stats EntityWriter"},
            };
            g_send_hooked = install_all(send, std::size(send), "send");
            if (g_send_hooked) logf(KILN_LOG_INFO, "repl: send hooks installed (send callback, entity writer) for %s", counting ? "the stats" : "a watch");
        } else {
            h_cb.revoke();
            h_wr.revoke();
            g_send_hooked = false;
            logf(KILN_LOG_INFO, "repl: send hooks removed (no stats, no watch)");
        }
    }
    if (want_tick != g_tick_hooked) {
        if (want_tick) {
            const One tick[] = {
                {&h_ack, F.ack_receive, reinterpret_cast<void*>(&d_ack), "repl/stats AckReceive"},
                {&h_reset, F.ecs_reset, reinterpret_cast<void*>(&d_reset), "repl/stats EcsReset"},
            };
            g_tick_hooked = install_all(tick, std::size(tick), "ack");
            if (g_tick_hooked) logf(KILN_LOG_INFO, "repl: ack hooks installed (ack, reset)");
        } else {
            h_ack.revoke();
            h_reset.revoke();
            g_tick_hooked = false;
            logf(KILN_LOG_INFO, "repl: ack hooks removed (no owner holds the stats)");
        }
    }
    return (!want_send || g_send_hooked) && (!want_tick || g_tick_hooked);
}

bool stats_send_hooked() { return g_send_hooked; }

namespace {

void clear_all_stats() {
    for (auto& m : g_slot)
        for (Slot& s : m) s.clear_stats();
    watch_clear_dropped();  // the rows' dropped (the watch rings') starts at 0 with the others
}

// ---- owner revoked (any thread) ----
void KILN_CALL owner_retiring(KilnOwner owner, void*) {
    watch_owner_retiring(owner);
    client_owner_retiring(owner);
    std::lock_guard lock(g_owners_mu);
    for (uint32_t i = 0; i < g_owner_n; ++i)
        if (g_owners[i] == owner) {
            g_owners[i] = g_owners[--g_owner_n];
            g_hooks_dirty.store(true, std::memory_order_relaxed);
            return;
        }
}

// ---- lab: the send step and frame interval (tick thread) ----
constexpr uint32_t kSteps = 4096;
uint64_t g_step_cycles[kSteps], g_interval_cycles[kSteps];
uint32_t g_steps = 0;            // samples in the window (the newest kSteps are kept)
uint64_t g_last_step_start = 0;
uint64_t g_window_tsc = 0;
LARGE_INTEGER g_window_qpc{};

uint64_t median(const uint64_t* v, uint32_t n) {
    if (!n) return 0;
    std::vector<uint64_t> c(v, v + n);
    std::nth_element(c.begin(), c.begin() + n / 2, c.end());
    return c[n / 2];
}

}  // namespace

// ---- the service's side of the stats (repl.h) ----
void stats_init() {
    for (Guard* g : {&g_cb_guard, &g_wr_guard, &g_ack_guard, &g_reset_guard}) add_guard(g);
}

// kiln_plugin_main, whatever repl.observe resolved to: a revoked owner's stats hold, send watches, capture and receive
// watches all go (a receive watch works without repl.observe).
void retiring_init() {
    const auto* hooks = static_cast<const KilnHooksApi*>(g_api->service(KilnStr{KILN_HOOKS_SERVICE, sizeof(KILN_HOOKS_SERVICE) - 1}, 2));
    KilnHandle h = 0;
    if (hooks && hooks->header.size >= offsetof(KilnHooksApi, subscribe_owner_retiring) + sizeof(hooks->subscribe_owner_retiring) &&
        hooks->subscribe_owner_retiring)
        hooks->subscribe_owner_retiring(g_owner, &owner_retiring, nullptr, &h);
}

void stats_pre() {
    if (g_hooks_dirty.load(std::memory_order_relaxed)) stats_apply_hooks();
}

void stats_peer_changed(uint32_t machine) {
    if (machine >= KILN_REPL_MACHINES) return;
    for (Slot& s : g_slot[machine]) s.clear_stats();
}

void stats_world_stopping() { clear_all_stats(); }

void note_send_step(uint64_t start, uint64_t end) {
    if (!start || end < start) return;
    const uint32_t i = g_steps % kSteps;
    g_step_cycles[i] = end - start;
    g_interval_cycles[i] = g_last_step_start && start > g_last_step_start ? start - g_last_step_start : 0;
    g_last_step_start = start;
    ++g_steps;
}

KilnResult KILN_CALL t_stats_enable(KilnOwner owner, bool on) {
    if (client_stats_thread()) return client_stats_enable(owner, on);  // a game client's main thread: what it receives
    if (!observe_on()) return KILN_ERR_UNAVAILABLE;
    if (!g_api->on_tick_thread()) return KILN_ERR_WRONG_THREAD;
    if (!owner) return KILN_ERR_INVALID_ARGUMENT;
    {
        std::lock_guard lock(g_owners_mu);
        uint32_t i = 0;
        while (i < g_owner_n && g_owners[i] != owner) ++i;
        if (on && i == g_owner_n) {
            if (g_owner_n >= kMaxOwners) return KILN_ERR_INVALID_ARGUMENT;
            if (g_owner_n == 0) clear_all_stats();  // the first enable: counters from 0 (tick thread: no replication job runs)
            g_owners[g_owner_n++] = owner;
        } else if (!on && i < g_owner_n) {
            g_owners[i] = g_owners[--g_owner_n];
        }
    }
    if (!stats_apply_hooks() && on) {
        // kiln_repl.h: the hooks could not be installed. This owner does not hold the stats then.
        {
            std::lock_guard lock(g_owners_mu);
            for (uint32_t i = 0; i < g_owner_n; ++i)
                if (g_owners[i] == owner) {
                    g_owners[i] = g_owners[--g_owner_n];
                    break;
                }
        }
        stats_apply_hooks();
        return KILN_ERR_UNAVAILABLE;
    }
    return KILN_OK;
}

KilnResult KILN_CALL t_peer_stats(KilnReplPeerStats* out, uint32_t cap, uint32_t* count) {
    if (count) *count = 0;
    if (client_stats_thread()) return client_peer_stats(out, cap, count);
    if (!observe_on()) return KILN_ERR_UNAVAILABLE;
    if (!g_api->on_tick_thread()) return KILN_ERR_WRONG_THREAD;
    if (!count || (cap && !out)) return KILN_ERR_INVALID_ARGUMENT;
    uint32_t rows = 0;
    for (uint32_t m = 0; m < KILN_REPL_MACHINES; ++m) {
        uint8_t flags = 0;
        const uint32_t peer = peer_of(m, &flags);
        if (m == 0 && side() == KILN_SIDE_CLIENT) flags |= KILN_REPL_PEER_LOCAL;
        for (uint32_t ch = 0; ch < 2; ++ch) {
            const Slot& s = g_slot[m][ch];
            const uint64_t dropped = watch_dropped(m, ch);  // records the watch rings dropped
            if (!s.any() && !dropped) continue;
            if (rows < cap) {
                KilnReplPeerStats& r = out[rows];
                if (r.size < sizeof(KilnReplPeerStats)) return KILN_ERR_INVALID_ARGUMENT;
                r.peer = peer;
                r.channel = static_cast<uint8_t>(ch ? 5 : 4);
                r.flags = flags;
                std::memset(r.pad, 0, sizeof r.pad);
                r.frames = get(s.frames);
                r.messages = get(s.messages);
                r.bytes = get(s.bytes);
                r.refused = get(s.refused);
                r.creates = get(s.creates);
                r.updates = get(s.updates);
                r.destroy_lists = get(s.destroy_lists);
                r.destroyed_ids = get(s.destroyed_ids);
                r.unchanged = get(s.unchanged);
                r.too_big = get(s.too_big);
                r.acks = get(s.acks);
                r.resets = get(s.resets);
                r.dropped = dropped;
            }
            ++rows;
        }
    }
    *count = rows;
    return rows > cap ? KILN_ERR_BUFFER_TOO_SMALL : KILN_OK;
}

KilnResult KILN_CALL lab_hook_cost(KilnReplHookCost* out, bool reset) {
    if (!out || out->size < sizeof(KilnReplHookCost)) return KILN_ERR_INVALID_ARGUMENT;
    if (!lab_allowed()) return KILN_ERR_UNAVAILABLE;
    if (!g_api->on_tick_thread()) return KILN_ERR_WRONG_THREAD;
    const uint32_t size = out->size;
    std::memset(out, 0, sizeof *out);
    out->size = size;
    LARGE_INTEGER qpc{}, freq{};
    QueryPerformanceCounter(&qpc);
    QueryPerformanceFrequency(&freq);
    const uint64_t now = __rdtsc();
    if (g_window_tsc && freq.QuadPart && qpc.QuadPart > g_window_qpc.QuadPart) {
        const double seconds = static_cast<double>(qpc.QuadPart - g_window_qpc.QuadPart) / static_cast<double>(freq.QuadPart);
        const double hz = static_cast<double>(now - g_window_tsc) / seconds;
        const auto ns = [hz](uint64_t cycles) { return hz > 0 ? static_cast<uint64_t>(static_cast<double>(cycles) * 1e9 / hz) : 0; };
        out->window_ns = static_cast<uint64_t>(seconds * 1e9);
        const uint32_t n = std::min(g_steps, kSteps);
        out->frames = g_steps;
        uint64_t step = 0, interval = 0;
        uint32_t intervals = 0;
        for (uint32_t i = 0; i < n; ++i) {
            step += g_step_cycles[i];
            if (g_interval_cycles[i]) {
                interval += g_interval_cycles[i];
                ++intervals;
            }
        }
        out->send_step_mean_ns = n ? ns(step / n) : 0;
        out->send_step_p50_ns = ns(median(g_step_cycles, n));
        out->frame_interval_mean_ns = intervals ? ns(interval / intervals) : 0;
        std::vector<uint64_t> iv;
        for (uint32_t i = 0; i < n; ++i)
            if (g_interval_cycles[i]) iv.push_back(g_interval_cycles[i]);
        out->frame_interval_p50_ns = ns(median(iv.data(), static_cast<uint32_t>(iv.size())));
        uint64_t cb = 0, cbc = 0, wr = 0, wrc = 0;
        for (auto& m : g_slot)
            for (Slot& s : m) {
                cb += get(s.cb_calls);
                cbc += get(s.cb_cycles);
                wr += get(s.wr_calls);
                wrc += get(s.wr_cycles);
            }
        out->callback_calls = cb;
        out->callback_ns = ns(cbc);
        out->writer_calls = wr;
        out->writer_ns = ns(wrc);
        out->tick_calls = get(g_tick_calls);
        out->tick_ns = ns(get(g_tick_cycles));
        out->allocations = g_observer_allocs.load(std::memory_order_relaxed);
        WatchCost wc{};
        watch_cost(&wc);
        out->delta_calls = wc.delta_calls;
        out->delta_ns = ns(wc.delta_cycles);
        out->watch_records = wc.records;
        out->watch_dropped = wc.dropped;
    }
    if (reset) {
        for (auto& m : g_slot)
            for (Slot& s : m) s.clear_cost();
        g_tick_calls.store(0, std::memory_order_relaxed);
        g_tick_cycles.store(0, std::memory_order_relaxed);
        g_observer_allocs.store(0, std::memory_order_relaxed);
        watch_cost_reset();
        g_steps = 0;
        g_last_step_start = 0;
        g_window_tsc = now;
        g_window_qpc = qpc;
    }
    return KILN_OK;
}

}  // namespace repl

// ---- kiln-repl.dll's heap: allocations made inside observer code are counted (repl::t_in_observer, guard.h) ----
namespace {
void* counted(size_t n) {
    if (repl::t_in_observer) repl::g_observer_allocs.fetch_add(1, std::memory_order_relaxed);
    return std::malloc(n ? n : 1);
}
void* counted_aligned(size_t n, std::align_val_t a) {
    if (repl::t_in_observer) repl::g_observer_allocs.fetch_add(1, std::memory_order_relaxed);
    const size_t al = static_cast<size_t>(a) < sizeof(void*) ? sizeof(void*) : static_cast<size_t>(a);
    return _aligned_malloc(n ? n : 1, al);
}
}  // namespace
// The CRT's own operator delete forms (free, _aligned_free) match these.
void* operator new(size_t n) {
    if (void* p = counted(n)) return p;
    throw std::bad_alloc();
}
void* operator new[](size_t n) {
    if (void* p = counted(n)) return p;
    throw std::bad_alloc();
}
void* operator new(size_t n, const std::nothrow_t&) noexcept { return counted(n); }
void* operator new[](size_t n, const std::nothrow_t&) noexcept { return counted(n); }
void* operator new(size_t n, std::align_val_t a) {
    if (void* p = counted_aligned(n, a)) return p;
    throw std::bad_alloc();
}
void* operator new[](size_t n, std::align_val_t a) {
    if (void* p = counted_aligned(n, a)) return p;
    throw std::bad_alloc();
}
void* operator new(size_t n, std::align_val_t a, const std::nothrow_t&) noexcept { return counted_aligned(n, a); }
void* operator new[](size_t n, std::align_val_t a, const std::nothrow_t&) noexcept { return counted_aligned(n, a); }
