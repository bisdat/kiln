// SPDX-License-Identifier: MIT
// kiln.repl.net's item remap (kiln_repl.h remap_set / remap_peers / remap_stats; the dossier's chapter 15 §4 M3 "Item-id
// remap" and "What a vanilla peer can safely be sent"; DESIGN slice B4).
//
// A peer that lacks Kiln-only items (a vanilla client; a Kiln client whose content state does not define them) must
// never be sent one of their ids: its game does not know them. For such a peer the snapshot step (filter.cpp) runs this
// as its LAST step, after every mod filter, on every component that has an item-id site: each id at a site that is not
// one of the game's own items becomes its declared look-alike, or 0 (the empty item: an empty slot). It fails closed:
//   - an id that is neither a game item nor in the map becomes 0 (a table that lags behind a new item hides it);
//   - a fault inside the step zeroes every site of that component (plain stores at offsets checked when the table was
//     built) and counts it;
// and the managed side (Remap.cs) refuses clients without Kiln while Kiln-only items exist and the remap is not working.
// The table is immutable once published (built on the tick thread, read by the replication jobs); a replaced table is
// freed at the second Pre after it left the hot set, when no job can read it.
#include <windows.h>

#include <algorithm>
#include <cstring>
#include <new>
#include <vector>

#include "filter.h"
#include "snapcore.h"
#include "wire.h"

namespace repl {

RemapCounters g_remap_counters;

namespace {

constexpr uint32_t kMaxSites = 4096, kMaxMap = 65536, kMaxGameIds = 1u << 20;

const RemapTable* g_table = nullptr;           // tick thread
std::vector<const RemapTable*> g_retiring;     // replaced: freed at the next Pre after the hot set moved on
std::vector<const RemapTable*> g_retired;
uint32_t g_peers[KILN_REPL_MACHINES] = {};
uint32_t g_peer_count = 0;
bool g_peers_set = false;                      // remap_peers was called (else nobody is remapped)
uint32_t g_machines = 0;                       // resolved in Pre

// The step itself (snapcore.h); may fault only on a bad table or state (never expected).
void remap_sites(const RemapTable* t, uint16_t index, uint8_t* data, uint32_t size, uint64_t* remapped, uint64_t* zeroed) {
    remap_ids(t->sites.data() + t->first[index], t->count[index], t->game_ids.data(), t->game_ids.size(), t->map.data(),
              t->map.size(), data, size, remapped, zeroed);
}
bool seh_remap(const RemapTable* t, uint16_t index, uint8_t* data, uint32_t size, uint64_t* remapped, uint64_t* zeroed) {
    __try {
        remap_sites(t, index, data, size, remapped, zeroed);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
void zero_all(const RemapTable* t, uint16_t index, uint8_t* data, uint32_t size) {
    zero_ids(t->sites.data() + t->first[index], t->count[index], data, size);
}

int index_of(const Registry* reg, uint32_t hash) {
    for (uint32_t i = 0; reg && i < reg->count; ++i)
        if (reg->at[i].hash == hash) return static_cast<int>(i);
    return -1;
}

void touch_sites(const RemapTable* t, uint32_t machines) {
    if (!t || !machines || !touch_on()) return;
    const Registry* reg = registry();
    std::vector<uint32_t> hashes;
    for (uint32_t i = 0; reg && i < reg->count; ++i)
        if ((t->mask[i >> 6] >> (i & 63)) & 1) hashes.push_back(reg->at[i].hash);
    if (hashes.empty()) return;
    const uint32_t n = touch_relevant(machines, hashes.data(), static_cast<uint32_t>(hashes.size()));
    if (n) logf(KILN_LOG_INFO, "repl: remap change: touched %u entities of %u machine(s)", n, static_cast<unsigned>(__popcnt(machines)));
}

}  // namespace

const RemapTable* remap_table() { return g_table; }
uint32_t remap_machines() { return g_table ? g_machines : 0; }

bool remap_resolve_peers() {
    uint32_t m = 0;
    if (g_peers_set)
        for (uint32_t k = 0; k < KILN_REPL_MACHINES; ++k) {
            uint8_t flags = 0;
            const uint32_t p = peer_of(k, &flags);
            if (!p) continue;
            for (uint32_t i = 0; i < g_peer_count; ++i)
                if (g_peers[i] == p) m |= 1u << k;
        }
    if (m == g_machines) return false;
    const uint32_t added = m & ~g_machines;
    g_machines = m;
    touch_sites(g_table, added);  // newly remapped machines get their entities again, remapped
    return true;
}

void remap_retire_pre() {
    for (const RemapTable* t : g_retired) delete t;
    g_retired.clear();
    g_retired.swap(g_retiring);  // freed at the next Pre: by then the hot set that held them is gone too
}

void remap_component(const RemapTable* t, uint16_t index, uint8_t* data, uint32_t size) {
    uint64_t remapped = 0, zeroed = 0;
    g_remap_counters.snapshots.fetch_add(1, std::memory_order_relaxed);
    if (!seh_remap(t, index, data, size, &remapped, &zeroed)) {
        zero_all(t, index, data, size);  // fails closed
        g_remap_counters.faults.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (remapped) g_remap_counters.remapped.fetch_add(remapped, std::memory_order_relaxed);
    if (zeroed) g_remap_counters.zeroed.fetch_add(zeroed, std::memory_order_relaxed);
}

KilnResult KILN_CALL t_remap_set(const KilnReplItemSite* sites, uint32_t site_count, const uint32_t* game_ids,
                                 uint32_t game_id_count, const KilnReplItemMap* map, uint32_t map_count) {
    if (!(filter_caps() & KILN_REPL_NET_CAP_REMAP)) return KILN_ERR_UNAVAILABLE;
    if (!g_api->on_tick_thread()) return KILN_ERR_WRONG_THREAD;
    // An empty table turns the remap off. Otherwise the game's ids must be there: without them every id would go.
    if (site_count == 0) {
        if (g_table) g_retiring.push_back(g_table);
        g_table = nullptr;
        filter_set_dirty();
        logf(KILN_LOG_INFO, "repl: remap table cleared");
        return KILN_OK;
    }
    if (!sites || site_count > kMaxSites || !game_ids || !game_id_count || game_id_count > kMaxGameIds ||
        map_count > kMaxMap || (map_count && !map))
        return KILN_ERR_INVALID_ARGUMENT;
    const Registry* reg = registry();
    if (!reg) return KILN_ERR_UNAVAILABLE;  // no world yet: call again from the world's first tick
    auto* t = new (std::nothrow) RemapTable;
    if (!t) return KILN_ERR_INTERNAL;
    t->game_ids.assign(game_ids, game_ids + game_id_count);
    std::sort(t->game_ids.begin(), t->game_ids.end());
    t->map.assign(map, map + map_count);
    std::sort(t->map.begin(), t->map.end(), [](const KilnReplItemMap& a, const KilnReplItemMap& b) { return a.from < b.from; });
    // sites grouped by registry index; each checked against the component's size
    struct S {
        uint16_t idx;
        RemapSite s;
    };
    std::vector<S> v;
    uint32_t unknown = 0, bad = 0;
    for (uint32_t i = 0; i < site_count; ++i) {
        const KilnReplItemSite& in = sites[i];
        const int idx = index_of(reg, in.component_hash);
        if (idx < 0) {
            ++unknown;
            continue;
        }
        const uint32_t count = in.count ? in.count : 1;
        const uint32_t last = in.offset + (count - 1) * uint32_t{in.stride} + 4;
        if ((count > 1 && in.stride < 4) || last > reg->at[idx].size) {
            ++bad;
            continue;
        }
        v.push_back({static_cast<uint16_t>(idx), RemapSite{in.offset, static_cast<uint16_t>(count), in.stride}});
    }
    if (bad) {
        delete t;
        logf(KILN_LOG_WARN, "repl: remap table refused: %u site(s) outside their component", bad);
        return KILN_ERR_INVALID_ARGUMENT;
    }
    std::stable_sort(v.begin(), v.end(), [](const S& a, const S& b) { return a.idx < b.idx; });
    for (const S& s : v) {
        if (t->count[s.idx] == 0) t->first[s.idx] = static_cast<uint16_t>(t->sites.size());
        t->sites.push_back(s.s);
        t->count[s.idx]++;
        t->mask[s.idx >> 6] |= 1ull << (s.idx & 63);
    }
    t->site_count = static_cast<uint32_t>(v.size());
    if (g_table) g_retiring.push_back(g_table);
    g_table = t;
    filter_set_dirty();
    touch_sites(t, g_machines);
    logf(KILN_LOG_INFO, "repl: remap table: %u sites (%u of components this world lacks), %u game items, %u Kiln-only items mapped",
         t->site_count, unknown, game_id_count, map_count);
    return KILN_OK;
}

KilnResult KILN_CALL t_remap_peers(const uint32_t* peers, uint32_t peer_count) {
    if (!(filter_caps() & KILN_REPL_NET_CAP_REMAP)) return KILN_ERR_UNAVAILABLE;
    if (!g_api->on_tick_thread()) return KILN_ERR_WRONG_THREAD;
    if (peer_count > KILN_REPL_MACHINES || (peer_count && !peers)) return KILN_ERR_INVALID_ARGUMENT;
    g_peer_count = peer_count;
    for (uint32_t i = 0; i < peer_count; ++i) g_peers[i] = peers[i];
    g_peers_set = true;
    if (remap_resolve_peers()) filter_set_dirty();
    return KILN_OK;
}

KilnResult KILN_CALL t_remap_stats(KilnReplRemapStats* out) {
    if (!out || out->size < sizeof(KilnReplRemapStats)) return KILN_ERR_INVALID_ARGUMENT;
    if (!(filter_caps() & KILN_REPL_NET_CAP_REMAP)) return KILN_ERR_UNAVAILABLE;
    const uint32_t size = out->size;
    std::memset(out, 0, sizeof *out);
    out->size = size;
    out->sites = g_table ? g_table->site_count : 0;
    out->snapshots = g_remap_counters.snapshots.load(std::memory_order_relaxed);
    out->remapped = g_remap_counters.remapped.load(std::memory_order_relaxed);
    out->zeroed = g_remap_counters.zeroed.load(std::memory_order_relaxed);
    out->faults = g_remap_counters.faults.load(std::memory_order_relaxed);
    return KILN_OK;
}

}  // namespace repl
