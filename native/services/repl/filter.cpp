// SPDX-License-Identifier: MIT
// kiln.repl.net's per-peer snapshot filters (kiln_repl.h filter_add / filter_set_peers; the dossier's chapter 15 §4 M3
// "Site", "Faults", "Filter lists", "Consistency"; DESIGN slice B1).
//
// One raw detour on repl/Snapshot: the game builds each player's own snapshot of an entity (from that player's pool,
// on that player's replication job) and, once the player acknowledges the message coded from it, it is that player's
// baseline. So an edit made right after the original returns is what the delta is coded from and what the client
// reconstructs, end to end. The hook:
//   1. runs the original exactly once (outside any guard) and returns its result untouched;
//   2. reads the hot set (immutable, published on the tick thread between frames) and tests its 640-bit mask against
//      the snapshot's list 1 (the components carrying data): when nothing matches that test is the whole cost;
//   3. for each matching component of a filter whose machines and channel take this snapshot: copies its bytes into a
//      per-job scratch, runs the filters (priority, then registration order) each under its own __try, and copies the
//      scratch back only when every filter returned normally (a fault or a filter that says it failed discards the
//      component's edits: the game's bytes go out; the filter is removed on the tick thread and logged once);
//   4. last, the item remap (remap.cpp) on the snapshot itself for the machines it applies to: it never faults out and
//      fails closed.
// Filter lists are immutable sets swapped on the tick thread (filter_add, remove, Pre): the replication jobs only run
// inside the game's send step, while the tick thread waits in it, so no job holds a set while it is replaced; replaced
// sets are freed at the next Pre all the same. An owner's revoke marks its filters; Pre takes them out of the set before
// the next send step, and a filter is never called after that (its code may go then).
#include <windows.h>

#include <intrin.h>

#include <algorithm>
#include <cstring>
#include <iterator>
#include <new>
#include <unordered_map>
#include <vector>

#include "filter.h"
#include "kiln_hook.hpp"
#include "rings.h"
#include "snapcore.h"
#include "wire.h"

namespace repl {
namespace {

constexpr uint32_t kMaxFilters = 256, kMaxPerComponent = 16, kMaxPeers = KILN_REPL_MACHINES, kMaxSources = 8;
constexpr uint32_t kMaxScratch = 8192;            // a component bigger than this is refused at filter_add
constexpr uintptr_t kCtxPool = 0x8da20;           // the player's state pool in its context (SnapArgs.playerPool)
// SnapArgs (docs/re/network.md "Writer state"): {Alloc* playerPool, Holder* temp, Registry*, Storage*, u32 tick,
// Record*, u8 pred}
constexpr size_t kSnapPool = 0x00, kSnapRegistry = 0x10, kSnapTick = 0x20, kSnapRecord = 0x28;
constexpr uintptr_t kRecordEntity = 0x10;

bool g_resolved = false;
uintptr_t f_snapshot = 0, f_get_component = 0;
using SnapshotFn = StateRef* (*)(StateRef* out, uint8_t* args);
using GetComponentFn = void (*)(Slice* out, uintptr_t world, uint32_t hash, uint32_t entity);

// ---- filters (tick thread, but faulted / removed are set anywhere) ----
struct Filter {
    std::atomic<uint64_t> handle{0};    // 0: free
    KilnOwner owner = 0;
    uint32_t hash = 0;
    uint32_t channels = 0;              // KILN_REPL_*_CHANNEL
    int32_t priority = 0;
    uint64_t order = 0;
    uint32_t peers[kMaxPeers] = {};
    uint32_t peer_count = 0;            // 0: every peer
    uint32_t sources[kMaxSources] = {};
    uint32_t source_count = 0;
    KilnReplFilterFn fn = nullptr;
    void* user = nullptr;
    uint32_t machines = 0;              // resolved at the last rebuild
    bool active = false;                // in the published set (its component is in this world)
    bool unknown_logged = false;
    std::atomic<bool> faulted{false};   // a job saw it fault: Pre removes it
    std::atomic<uintptr_t> fault_where{0};
    std::atomic<bool> removed{false};   // remove() / its owner went: Pre removes it
};
Filter g_f[kMaxFilters];
uint64_t g_serial = 0, g_order = 0;
std::atomic<bool> g_dirty{false};       // rebuild the hot set at the next Pre

// ---- what the hook reads ----
struct Entry {
    Filter* filter;
    KilnReplFilterFn fn;
    void* user;
    uint32_t machines;
    uint32_t channels;
};
struct Hot {
    uint64_t mask[10] = {};             // registry indices with a filter or a remap site
    uint16_t first[kMaxComponents] = {};
    uint16_t count[kMaxComponents] = {};
    uint32_t n = 0;
    Entry entries[kMaxFilters];
    const RemapTable* remap = nullptr;
    uint32_t remap_machines = 0;
    bool any = false;
};
std::atomic<const Hot*> g_hot{nullptr};
std::vector<const Hot*> g_retired;       // freed at the next Pre

// ---- counters (lab: the hook's own cost) ----
struct alignas(64) Cost {
    std::atomic<uint64_t> calls{0}, cycles{0}, matched{0}, matched_cycles{0}, filtered{0}, unplaced{0};
};
Cost g_cost[KILN_REPL_MACHINES][2];

// ---- sources (FilterOptions.Sources): which entities a filter with sources ran on (client channel), per machine; the
// tick thread compares their source bytes every Pre and touches an entity whose sources changed ----
struct SourceRec {
    uint32_t entity;
    uint32_t filter;  // index into g_f
};
Ring g_src_ring[KILN_REPL_MACHINES];
std::atomic<bool> g_src_on{false};       // rings are there (tick thread sets it once)
struct SourceSeen {
    uint64_t digest = 0;
    bool known = false;
};
std::unordered_map<uint64_t, SourceSeen> g_src_seen;  // (filter << 32) | entity; tick thread
constexpr size_t kMaxSourceEntities = 16384;
std::atomic<uint64_t> g_src_touches{0};

// ---- the hook ----
Guard g_guard{"filter-snapshot"};
kiln::RawHook h_snapshot;
bool g_hooked = false;
bool g_hook_failed_logged = false;

alignas(16) thread_local uint8_t t_scratch[kMaxScratch];

const void* KILN_CALL ctx_read(const KilnReplFilterCtx*, uint32_t entity, uint32_t hash, uint32_t* out_size) {
    if (out_size) *out_size = 0;
    const uintptr_t world = g_world.load(std::memory_order_relaxed);
    if (!f_get_component || !world || !entity || !hash) return nullptr;
    Slice s{};
    __try {
        reinterpret_cast<GetComponentFn>(f_get_component)(&s, world, hash, entity);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
    if (!s.ptr) return nullptr;
    if (out_size) *out_size = static_cast<uint32_t>(s.size);
    return s.ptr;
}

struct SnapCall {
    StateRef* result;
    uint8_t* args;
};

inline bool has_bit(const uint64_t* mask, uint32_t i) { return (mask[i >> 6] >> (i & 63)) & 1; }

void filter_after(void* p) {
    const auto* c = static_cast<SnapCall*>(p);
    const Hot* hot = g_hot.load(std::memory_order_acquire);
    if (!hot || !hot->any || !c->result || !c->result->state) return;
    const uint64_t t0 = g_measure.load(std::memory_order_relaxed) ? __rdtsc() : 0;
    const uintptr_t world = g_world.load(std::memory_order_relaxed);
    const uintptr_t pool = *reinterpret_cast<const uintptr_t*>(c->args + kSnapPool);
    uint8_t pred = 0;
    const int m = pool > kCtxPool ? machine_of_ctx(world, pool - kCtxPool, &pred) : -1;
    if (m < 0) return;  // not a player's replication snapshot
    Cost& cost = g_cost[m][pred & 1];
    const uint32_t chan_flag = pred ? KILN_REPL_PREDICTION_CHANNEL : KILN_REPL_CLIENT_CHANNEL;
    const Registry* reg = registry_hot();
    if (!reg) return;
    uint8_t* s = c->result->state;
    // 2. the mask test over list 1
    const bool any = list1_match(hot->mask, s, reg->count, [reg](uint32_t i) {
        return (reg->at[i].flags & KILN_REPL_COMPONENT_VERSIONED) != 0;
    }) == 1;
    if (t0) {
        cost.calls.fetch_add(1, std::memory_order_relaxed);
        cost.cycles.fetch_add(__rdtsc() - t0, std::memory_order_relaxed);
    }
    if (!any) return;
    const bool remap_here = hot->remap && ((hot->remap_machines >> m) & 1);
    // 3. offsets of the components with data, from the game's pure StateIndex (room for every component: a state with
    // many is still placed, never sent past the remap)
    Part parts[kMaxComponents];
    uint32_t n = 0;
    const uintptr_t registry = *reinterpret_cast<const uintptr_t*>(c->args + kSnapRegistry);
    if (!state_parts(*reg, s, [&](uint8_t* index, const uint8_t* st) { return state_index(index, st, registry); }, parts,
                     static_cast<uint32_t>(std::size(parts)), &n)) {
        cost.unplaced.fetch_add(1, std::memory_order_relaxed);  // the game's own state did not parse: no filter ran
        if (remap_here) {
            // fails closed: a remapped peer never gets this state's item ids raw; every data byte goes to 0 (the lists
            // stay, so the client reads a well-formed state), or the remap stops (Kiln.Replication then acts per peer)
            if (blank_state(s, reg->count, [reg](uint32_t i) { return (reg->at[i].flags & KILN_REPL_COMPONENT_VERSIONED) != 0; }))
                g_remap_counters.blanked.fetch_add(1, std::memory_order_relaxed);
            else
                remap_set_stopped(KILN_REPL_REMAP_UNPLACED);
        }
        return;
    }
    KilnReplFilterCtx ctx{};
    ctx.size = sizeof ctx;
    uint8_t flags = 0;
    ctx.peer = peer_of(static_cast<uint32_t>(m), &flags);
    ctx.peer_flags = flags;
    ctx.channel = static_cast<uint8_t>(pred ? 5 : 4);
    ctx.machine = static_cast<uint8_t>(m);
    const uintptr_t record = *reinterpret_cast<const uintptr_t*>(c->args + kSnapRecord);
    ctx.entity = record ? *reinterpret_cast<const uint32_t*>(record + kRecordEntity) : 0;
    ctx.tick = *reinterpret_cast<const uint32_t*>(c->args + kSnapTick);
    ctx.read = &ctx_read;
    for (uint32_t k = 0; k < n; ++k) {
        const uint16_t idx = parts[k].index;
        if (!has_bit(hot->mask, idx)) continue;
        uint8_t* data = s + parts[k].off;
        const uint32_t size = parts[k].size;
        // mod filters, on a scratch copy
        const uint32_t first = hot->first[idx], cnt = hot->count[idx];
        bool copied = false, ok = true;
        for (uint32_t e = first; e < first + cnt && ok; ++e) {
            const Entry& en = hot->entries[e];
            if (!((en.machines >> m) & 1) || !(en.channels & chan_flag)) continue;
            if (en.filter->faulted.load(std::memory_order_relaxed)) continue;
            if (size > kMaxScratch) {
                ok = false;
                break;
            }
            if (!copied) {
                std::memcpy(t_scratch, data, size);
                copied = true;
            }
            ctx.component_hash = reg->at[idx].hash;
            if (en.filter->source_count && !pred && g_src_on.load(std::memory_order_relaxed)) {
                const SourceRec rec{ctx.entity, static_cast<uint32_t>(en.filter - g_f)};
                g_src_ring[m].push(1, &rec, sizeof rec);  // full: dropped (the entity is seen again at its next send)
            }
            uintptr_t where = 0;
            if (!call_filter(en.fn, t_scratch, size, &ctx, en.user, &where)) {
                uintptr_t none = 0;
                en.filter->fault_where.compare_exchange_strong(none, where ? where : 1, std::memory_order_relaxed);
                en.filter->faulted.store(true, std::memory_order_release);
                ok = false;
            }
        }
        if (copied && ok) {
            std::memcpy(data, t_scratch, size);
            cost.filtered.fetch_add(1, std::memory_order_relaxed);
        }
        // 4. the remap, last, in place
        if (remap_here && has_bit(hot->remap->mask, idx)) remap_component(hot->remap, idx, data, size);
    }
    if (t0) {
        cost.matched.fetch_add(1, std::memory_order_relaxed);
        cost.matched_cycles.fetch_add(__rdtsc() - t0, std::memory_order_relaxed);
    }
}

// After a fault in the step: a remapped peer's state is blanked when its machine can still be told; else the remap stops.
void emergency(const SnapCall* c) {
    const Hot* hot = g_hot.load(std::memory_order_acquire);
    const bool any = hot && hot->remap && hot->remap_machines;
    if (!any) return;
    // recheck A: a fault is a stop per the ruling (vanillaWithoutRemap applies), also when the blank below works
    remap_set_stopped(KILN_REPL_REMAP_FAULTED);
    if (!c->result || !c->result->state) return;
    const Registry* reg = registry_hot();
    const uintptr_t pool = *reinterpret_cast<const uintptr_t*>(c->args + kSnapPool);
    uint8_t pred = 0;
    const int m = pool > kCtxPool && reg ? machine_of_ctx(g_world.load(std::memory_order_relaxed), pool - kCtxPool, &pred) : -1;
    const FaultOutcome o = after_fault(any, m >= 0, m >= 0 && ((hot->remap_machines >> m) & 1));
    if (o.blank && blank_state(c->result->state, reg->count, [reg](uint32_t i) { return (reg->at[i].flags & KILN_REPL_COMPONENT_VERSIONED) != 0; }))
        g_remap_counters.blanked.fetch_add(1, std::memory_order_relaxed);
}
std::atomic<uint64_t> g_step_faults{0};
std::atomic<uintptr_t> g_step_fault_at{0};
void seh_step(SnapCall* c) {
    uintptr_t where = 0;
    __try {
        filter_after(c);
        return;
    } __except (snap_detail::note(GetExceptionInformation(), &where)) {
    }
    uintptr_t none = 0;
    g_step_fault_at.compare_exchange_strong(none, where ? where : 1, std::memory_order_relaxed);
    g_step_faults.fetch_add(1, std::memory_order_relaxed);
    __try {
        emergency(c);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        remap_set_stopped(KILN_REPL_REMAP_FAULTED);
    }
}

StateRef* d_snapshot(StateRef* out, uint8_t* args) {
    SnapCall c{nullptr, args};
    StateRef* r = h_snapshot.original<SnapshotFn>()(out, args);  // exactly once, outside any guard
    c.result = r;
    ++t_in_observer;
    seh_step(&c);  // never turns itself off: the remap must keep failing closed
    --t_in_observer;
    return r;
}

// ---- the hot set (tick thread) ----
const char* name_of(uint32_t hash) {
    const Registry* reg = registry();
    if (reg)
        for (uint32_t i = 0; i < reg->count; ++i)
            if (reg->at[i].hash == hash) return reg->names[i].c_str();
    return "?";
}
int index_of(const Registry* reg, uint32_t hash) {
    if (!reg) return -1;
    for (uint32_t i = 0; i < reg->count; ++i)
        if (reg->at[i].hash == hash) return static_cast<int>(i);
    return -1;
}

uint32_t machines_of(const Filter& f) {
    if (!f.peer_count) return (1u << KILN_REPL_MACHINES) - 1;
    uint32_t m = 0;
    for (uint32_t k = 0; k < KILN_REPL_MACHINES; ++k) {
        uint8_t flags = 0;
        const uint32_t p = peer_of(k, &flags);
        if (!p) continue;
        for (uint32_t i = 0; i < f.peer_count; ++i)
            if (f.peers[i] == p) m |= 1u << k;
    }
    return m;
}

void rebuild() {
    g_dirty.store(false, std::memory_order_relaxed);
    const Registry* reg = registry();
    auto* hot = new (std::nothrow) Hot;
    if (!hot) {
        logf(KILN_LOG_WARN, "repl: filters: out of memory; the filter set is unchanged");
        return;
    }
    struct Item {
        uint16_t idx;
        Filter* f;
    };
    std::vector<Item> items;
    for (Filter& f : g_f) {
        f.active = false;
        if (!f.handle.load(std::memory_order_relaxed) || f.faulted.load(std::memory_order_relaxed) ||
            f.removed.load(std::memory_order_relaxed))
            continue;
        const int idx = index_of(reg, f.hash);
        if (idx < 0) {
            if (reg && !f.unknown_logged) {
                logf(KILN_LOG_WARN, "repl: filter of owner 0x%llx: this world has no component 0x%08x; it does nothing",
                     static_cast<unsigned long long>(f.owner), f.hash);
                f.unknown_logged = true;
            }
            continue;
        }
        items.push_back({static_cast<uint16_t>(idx), &f});
    }
    // by component index, then priority (higher first), then registration order
    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
        if (a.idx != b.idx) return a.idx < b.idx;
        if (a.f->priority != b.f->priority) return a.f->priority > b.f->priority;
        return a.f->order < b.f->order;
    });
    for (const Item& it : items) {
        if (hot->n >= kMaxFilters || hot->count[it.idx] >= kMaxPerComponent) continue;
        if (hot->count[it.idx] == 0) hot->first[it.idx] = static_cast<uint16_t>(hot->n);
        it.f->machines = machines_of(*it.f);
        it.f->active = true;
        hot->entries[hot->n++] = Entry{it.f, it.f->fn, it.f->user, it.f->machines, it.f->channels};
        hot->count[it.idx]++;
        hot->mask[it.idx >> 6] |= 1ull << (it.idx & 63);
        hot->any = true;
    }
    if (const RemapTable* t = remap_table()) {
        hot->remap = t;
        hot->remap_machines = remap_machines();
        if (hot->remap_machines)
            for (int w = 0; w < 10; ++w) {
                hot->mask[w] |= t->mask[w];
                if (t->mask[w]) hot->any = true;
            }
    }
    const Hot* old = g_hot.exchange(hot, std::memory_order_acq_rel);
    if (old) g_retired.push_back(old);
    // the hook is in while anything can match
    if (hot->any && !g_hooked) {
        if (h_snapshot.install(g_api, g_owner, f_snapshot, reinterpret_cast<void*>(&d_snapshot), "repl/filter Snapshot")) {
            g_hooked = true;
            remap_hook_refused(false);
            logf(KILN_LOG_INFO, "repl: snapshot hook installed (%u filters, remap %s)", hot->n, hot->remap_machines ? "on" : "off");
        } else {
            if (hot->remap_machines) remap_hook_refused(true);  // the remap stops (fails closed), reported to Kiln.Replication
            if (!g_hook_failed_logged) {
                g_hook_failed_logged = true;
                logf(KILN_LOG_WARN, "repl: the snapshot hook could not be installed (%s): filters do nothing and the item remap stops",
                     h_snapshot.error() ? h_snapshot.error() : "?");
            }
        }
    } else if (!hot->any && g_hooked) {
        h_snapshot.revoke();
        g_hooked = false;
        logf(KILN_LOG_INFO, "repl: snapshot hook removed (no filter, no remapped peer)");
    }
}

// The machines whose output of a component changed: touch what they hold of it (client channel only: the prediction
// channel re-snapshots every tick).
void invalidate(uint32_t machines, uint32_t hash) {
    if (!machines || !touch_on()) return;
    const uint32_t n = touch_relevant(machines, &hash, 1);
    if (n) logf(KILN_LOG_INFO, "repl: filter change on %s: touched %u entities", name_of(hash), n);
}

void free_filter(Filter& f) {
    const uint32_t was = f.active && (f.channels & KILN_REPL_CLIENT_CHANNEL) ? f.machines : 0;
    const uint32_t hash = f.hash;
    f.fn = nullptr;
    f.user = nullptr;
    f.active = false;
    f.faulted.store(false, std::memory_order_relaxed);
    f.fault_where.store(0, std::memory_order_relaxed);
    f.removed.store(false, std::memory_order_relaxed);
    f.handle.store(0, std::memory_order_release);
    g_dirty.store(true, std::memory_order_relaxed);
    invalidate(was, hash);
}

uint32_t seh_relevant(uintptr_t ctx, void (*fn)(uint32_t, void*), void* user) {
    uint32_t n = 0;
    __try {
        const uintptr_t map = ctx + kCtxStateMap;
        const uint64_t cap = *reinterpret_cast<const uint64_t*>(map + kMapCap);
        if (!cap || (cap & (cap - 1)) || !*reinterpret_cast<const uint32_t*>(map + kMapCount)) return 0;
        const auto* bits = *reinterpret_cast<const uint64_t* const*>(map + kMapBits);
        const auto* keys = *reinterpret_cast<const uint32_t* const*>(map + kMapKeys);
        for (uint64_t i = 0; i < cap; ++i)
            if ((bits[i >> 6] >> (i & 63)) & 1) {
                fn(keys[i], user);
                ++n;
            }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return n;
}

// FNV-1a-64 over the bytes of the filter's source components of an entity, as the world holds them now (tick thread).
// false when the entity has none of them any more (gone).
bool source_digest(const Filter& f, uint32_t entity, uint64_t* out) {
    uint64_t h = 1469598103934665603ull;
    bool any = false;
    for (uint32_t s = 0; s < f.source_count; ++s) {
        uint32_t size = 0;
        const auto* p = static_cast<const uint8_t*>(world_component(entity, f.sources[s], &size));
        h = (h ^ (p ? 1u : 0u)) * 1099511628211ull;
        if (!p) continue;
        any = true;
        for (uint32_t i = 0; i < size; ++i) h = (h ^ p[i]) * 1099511628211ull;
    }
    *out = h;
    return any;
}

void sources_pre() {
    if (!g_src_on.load(std::memory_order_relaxed)) return;
    for (uint32_t m = 0; m < KILN_REPL_MACHINES; ++m)
        g_src_ring[m].drain([](const RecordHeader& rh, const uint8_t* p) {
            if (rh.len < sizeof(SourceRec)) return;
            SourceRec r;
            std::memcpy(&r, p, sizeof r);
            if (r.filter >= kMaxFilters || !r.entity) return;
            const uint64_t key = (uint64_t{r.filter} << 32) | r.entity;
            if (g_src_seen.size() < kMaxSourceEntities || g_src_seen.count(key)) g_src_seen.try_emplace(key);
        });
    uint32_t touched = 0;
    for (auto it = g_src_seen.begin(); it != g_src_seen.end();) {
        const uint32_t fi = static_cast<uint32_t>(it->first >> 32), entity = static_cast<uint32_t>(it->first);
        const Filter& f = g_f[fi];
        uint64_t d = 0;
        if (!f.handle.load(std::memory_order_relaxed) || !f.active || !source_digest(f, entity, &d)) {
            it = g_src_seen.erase(it);
            continue;
        }
        if (it->second.known && it->second.digest != d) {
            uint32_t st = 0;
            if (touch_entity(entity, &st) == KILN_OK && st == KILN_REPL_TOUCH_QUEUED) ++touched;
        }
        it->second.digest = d;
        it->second.known = true;
        ++it;
    }
    if (touched) g_src_touches.fetch_add(touched, std::memory_order_relaxed);
}

void sources_need() {
    if (g_src_on.load(std::memory_order_relaxed)) return;
    for (Ring& r : g_src_ring)
        if (!r.ready() && !r.init(64u << 10, 4096)) {
            logf(KILN_LOG_WARN, "repl: filter sources: out of memory; sources are not followed");
            return;
        }
    g_src_on.store(true, std::memory_order_release);
}

}  // namespace

// ---- filter.h ----
uintptr_t client_ctx(uintptr_t world, uint32_t machine) {
    if (!world || machine >= KILN_REPL_MACHINES || L.world_objects < 0 || L.obj_stride <= 0 || L.obj_client_ctx < 0) return 0;
    const uintptr_t base = read<uintptr_t>(world + static_cast<uintptr_t>(L.world_objects));
    if (!base) return 0;
    const uintptr_t obj = base + machine * static_cast<uintptr_t>(L.obj_stride);
    if (!read<uint8_t>(obj + kObjActive)) return 0;
    return obj + static_cast<uintptr_t>(L.obj_client_ctx);
}

uint32_t relevant_entities(uintptr_t world, uint32_t machine, void (*fn)(uint32_t entity, void* user), void* user) {
    const uintptr_t ctx = client_ctx(world, machine);
    return ctx ? seh_relevant(ctx, fn, user) : 0;
}

void filter_set_dirty() { g_dirty.store(true, std::memory_order_relaxed); }

// ---- repl.h ----
void filter_start() {
    auto sym = [](const char* name, uintptr_t* out) { return g_api->symbol(KilnStr{name, std::strlen(name)}, out) == KILN_OK; };
    g_resolved = observe_on() && g_api->feature(KilnStr{"repl.filter", 11}) && sym("repl/Snapshot", &f_snapshot) &&
                 F.state_index != 0;
    sym("GetComponent", &f_get_component);  // FilterContext.read answers NULL without it
    add_guard(&g_guard);
    touch_start();
    if (g_resolved)
        logf(KILN_LOG_INFO, "repl: filters on (the snapshot hook goes in while a filter or a remapped peer exists)");
    else
        logf(KILN_LOG_INFO, "repl: filters off (sigdb feature repl.filter or repl.observe is off on this build)");
}

void filter_ready() {
    if (g_resolved) touch_ready();
}

uint32_t filter_caps() {
    if (!g_resolved) return 0;
    return KILN_REPL_NET_CAP_FILTER | KILN_REPL_NET_CAP_REMAP | (touch_on() ? KILN_REPL_NET_CAP_TOUCH : 0u);
}

namespace {
// Lab servers (the hooks are timed there): one line every 600 Pres with the snapshot hook's own cost and the touches.
uint32_t g_report_pres = 0;
uint64_t g_report_tsc = 0;
LARGE_INTEGER g_report_qpc{};
void lab_report() {
    if (!g_measure.load(std::memory_order_relaxed) || ++g_report_pres < 600) return;
    g_report_pres = 0;
    LARGE_INTEGER qpc{}, freq{};
    QueryPerformanceCounter(&qpc);
    QueryPerformanceFrequency(&freq);
    const uint64_t now = __rdtsc();
    if (g_report_tsc && freq.QuadPart && qpc.QuadPart > g_report_qpc.QuadPart) {
        const double secs = static_cast<double>(qpc.QuadPart - g_report_qpc.QuadPart) / static_cast<double>(freq.QuadPart);
        const double ghz = static_cast<double>(now - g_report_tsc) / secs / 1e9;
        uint64_t calls = 0, cycles = 0, matched = 0, matched_cycles = 0, filtered = 0, unplaced = 0;
        for (auto& m : g_cost)
            for (Cost& c : m) {
                calls += c.calls.exchange(0, std::memory_order_relaxed);
                cycles += c.cycles.exchange(0, std::memory_order_relaxed);
                matched += c.matched.exchange(0, std::memory_order_relaxed);
                matched_cycles += c.matched_cycles.exchange(0, std::memory_order_relaxed);
                filtered += c.filtered.exchange(0, std::memory_order_relaxed);
                unplaced += c.unplaced.exchange(0, std::memory_order_relaxed);
            }
        if (calls && ghz > 0)
            logf(KILN_LOG_INFO,
                 "repl: filter cost %.0f s: %llu snapshots, mask test %.0f ns mean; %llu matched, %.0f ns mean each (filters and "
                 "remap); %llu components filtered; %llu not placed; remap %llu remapped %llu zeroed %llu faults; %llu source touches",
                 secs, static_cast<unsigned long long>(calls), static_cast<double>(cycles) / calls / ghz,
                 static_cast<unsigned long long>(matched), matched ? static_cast<double>(matched_cycles) / matched / ghz : 0.0,
                 static_cast<unsigned long long>(filtered), static_cast<unsigned long long>(unplaced),
                 static_cast<unsigned long long>(g_remap_counters.remapped.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_remap_counters.zeroed.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_remap_counters.faults.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_src_touches.load(std::memory_order_relaxed)));
    }
    g_report_tsc = now;
    g_report_qpc = qpc;
}
}  // namespace

void filter_pre(uintptr_t) {
    if (!g_resolved) return;
    lab_report();
    static uint64_t logged_faults = 0;
    if (const uint64_t n = g_step_faults.load(std::memory_order_relaxed); n != logged_faults) {
        char where[160];
        describe(g_step_fault_at.load(std::memory_order_relaxed), where, sizeof where);
        if (!logged_faults) logf(KILN_LOG_WARN, "repl: the snapshot step faulted at %s (%llu times so far); a remapped peer's state is blanked, else the remap stops", where, static_cast<unsigned long long>(n));
        logged_faults = n;
    }
    for (const Hot* h : g_retired) delete h;
    g_retired.clear();
    remap_retire_pre();
    // faulted and removed filters leave the set before the next send step
    for (Filter& f : g_f) {
        if (!f.handle.load(std::memory_order_relaxed)) continue;
        if (f.faulted.load(std::memory_order_acquire)) {
            char where[160];
            describe(f.fault_where.load(std::memory_order_relaxed), where, sizeof where);
            logf(KILN_LOG_WARN, "repl: filter of owner 0x%llx on %s faulted at %s; removed", static_cast<unsigned long long>(f.owner),
                 name_of(f.hash), where);
            free_filter(f);
        } else if (f.removed.load(std::memory_order_acquire)) {
            free_filter(f);
        }
    }
    // peers -> machines (a peer that left, a fake added or bound)
    for (Filter& f : g_f) {
        if (!f.handle.load(std::memory_order_relaxed) || !f.active) continue;
        const uint32_t m = machines_of(f);
        if (m != f.machines) {
            if (f.channels & KILN_REPL_CLIENT_CHANNEL) invalidate(m ^ f.machines, f.hash);  // peers in and out of it: re-send
            g_dirty.store(true, std::memory_order_relaxed);
        }
    }
    if (remap_resolve_peers()) g_dirty.store(true, std::memory_order_relaxed);
    if (g_dirty.load(std::memory_order_relaxed)) rebuild();
    sources_pre();
    touch_pre();
}

void filter_peer_changed(uint32_t, uint32_t) { g_dirty.store(true, std::memory_order_relaxed); }

void filter_world_stopping() {
    g_dirty.store(true, std::memory_order_relaxed);
    remap_world_stopping();
}

void filter_owner_retiring(KilnOwner owner) {
    for (Filter& f : g_f)
        if (f.handle.load(std::memory_order_acquire) && f.owner == owner) f.removed.store(true, std::memory_order_release);
    g_dirty.store(true, std::memory_order_relaxed);
}

KilnResult filter_remove(KilnHandle h) {
    for (Filter& f : g_f)
        if (h && f.handle.load(std::memory_order_acquire) == h) {
            f.removed.store(true, std::memory_order_release);
            g_dirty.store(true, std::memory_order_relaxed);
            if (g_api->on_tick_thread()) {  // out of the set at once (no replication job runs while the tick thread does)
                free_filter(f);
                rebuild();
            }
            return KILN_OK;
        }
    return KILN_ERR_NOT_FOUND;
}

KilnResult KILN_CALL t_filter_add(KilnOwner owner, uint32_t component_hash, const uint32_t* peers, uint32_t peer_count,
                                  uint32_t channels, int32_t priority, const uint32_t* sources, uint32_t source_count,
                                  KilnReplFilterFn fn, void* user, KilnHandle* out) {
    if (out) *out = 0;
    if (!g_resolved) return KILN_ERR_UNAVAILABLE;
    if (!g_api->on_tick_thread()) return KILN_ERR_WRONG_THREAD;
    if (!fn || !out || !component_hash || !channels || (channels & ~(KILN_REPL_CLIENT_CHANNEL | KILN_REPL_PREDICTION_CHANNEL)) ||
        peer_count > kMaxPeers || (peer_count && !peers) || source_count > kMaxSources || (source_count && !sources))
        return KILN_ERR_INVALID_ARGUMENT;
    if (const Registry* reg = registry()) {
        const int idx = index_of(reg, component_hash);
        if (idx < 0) return KILN_ERR_INVALID_ARGUMENT;
        const uint32_t fl = reg->at[idx].flags;
        if ((channels & KILN_REPL_CLIENT_CHANNEL) && !(fl & KILN_REPL_COMPONENT_CLIENT)) return KILN_ERR_INVALID_ARGUMENT;
        if ((channels & KILN_REPL_PREDICTION_CHANNEL) && !(fl & KILN_REPL_COMPONENT_PREDICTION)) return KILN_ERR_INVALID_ARGUMENT;
        if (reg->at[idx].size > kMaxScratch) return KILN_ERR_INVALID_ARGUMENT;
    }
    if (source_count && (channels & KILN_REPL_CLIENT_CHANNEL) && !touch_on()) return KILN_ERR_UNAVAILABLE;
    Filter* slot = nullptr;
    uint32_t per = 0;
    for (Filter& f : g_f) {
        if (!f.handle.load(std::memory_order_relaxed)) {
            if (!slot) slot = &f;
        } else if (f.hash == component_hash) {
            ++per;
        }
    }
    if (!slot || per >= kMaxPerComponent) return KILN_ERR_INVALID_ARGUMENT;  // 256 filters, 16 per component
    slot->owner = owner;
    slot->hash = component_hash;
    slot->channels = channels;
    slot->priority = priority;
    slot->order = ++g_order;
    slot->peer_count = peer_count;
    for (uint32_t i = 0; i < peer_count; ++i) slot->peers[i] = peers[i];
    slot->source_count = source_count;
    for (uint32_t i = 0; i < source_count; ++i) slot->sources[i] = sources[i];
    slot->fn = fn;
    slot->user = user;
    slot->unknown_logged = false;
    slot->machines = 0;
    slot->faulted.store(false, std::memory_order_relaxed);
    slot->fault_where.store(0, std::memory_order_relaxed);
    slot->removed.store(false, std::memory_order_relaxed);
    if (source_count && (channels & KILN_REPL_CLIENT_CHANNEL)) sources_need();
    const KilnHandle h = kFilterTag | (++g_serial & ~kTagMask);
    slot->handle.store(h, std::memory_order_release);
    rebuild();
    if (slot->active && (channels & KILN_REPL_CLIENT_CHANNEL)) invalidate(slot->machines, component_hash);
    *out = h;
    return KILN_OK;
}

KilnResult KILN_CALL t_filter_set_peers(KilnHandle filter, const uint32_t* peers, uint32_t peer_count) {
    if (!g_resolved) return KILN_ERR_UNAVAILABLE;
    if (!g_api->on_tick_thread()) return KILN_ERR_WRONG_THREAD;
    if (peer_count > kMaxPeers || (peer_count && !peers)) return KILN_ERR_INVALID_ARGUMENT;
    for (Filter& f : g_f) {
        if (!filter || f.handle.load(std::memory_order_acquire) != filter || f.removed.load(std::memory_order_relaxed)) continue;
        const uint32_t before = f.active ? f.machines : 0;
        f.peer_count = peer_count;
        for (uint32_t i = 0; i < peer_count; ++i) f.peers[i] = peers[i];
        rebuild();
        const uint32_t after = f.active ? f.machines : 0;
        if (f.channels & KILN_REPL_CLIENT_CHANNEL) invalidate(before ^ after, f.hash);
        return KILN_OK;
    }
    return KILN_ERR_NOT_FOUND;
}

// Lab: the hook's own cost since the start, all machines (cycles; kiln.repl.lab converts).
void filter_cost(uint64_t* calls, uint64_t* cycles, uint64_t* matched, uint64_t* matched_cycles, uint64_t* filtered) {
    *calls = *cycles = *matched = *matched_cycles = *filtered = 0;
    for (auto& m : g_cost)
        for (Cost& c : m) {
            *calls += c.calls.load(std::memory_order_relaxed);
            *cycles += c.cycles.load(std::memory_order_relaxed);
            *matched += c.matched.load(std::memory_order_relaxed);
            *matched_cycles += c.matched_cycles.load(std::memory_order_relaxed);
            *filtered += c.filtered.load(std::memory_order_relaxed);
        }
}

}  // namespace repl
