// SPDX-License-Identifier: MIT
// kiln.repl.net's snapshot step, shared by filter.cpp (the Snapshot hook and the filter lists), remap.cpp (the item
// remap, the last step of every snapshot it applies to) and touch.cpp (re-sending entities whose output changed). Not
// an ABI.
#pragma once
#include <atomic>
#include <cstdint>
#include <vector>

#include "repl.h"
#include "snapcore.h"

namespace repl {

// ---- the remap table as the snapshot hook reads it: immutable once published (remap.cpp builds it, tick thread) ----
struct RemapTable {
    uint64_t mask[10] = {};                    // registry indices with a site
    uint16_t first[kMaxComponents] = {};       // into sites
    uint16_t count[kMaxComponents] = {};
    std::vector<RemapSite> sites;
    std::vector<uint32_t> game_ids;            // sorted
    std::vector<KilnReplItemMap> map;          // sorted by from
    uint32_t site_count = 0;                   // as given (sites of components this world has)
};
struct RemapCounters {
    std::atomic<uint64_t> snapshots{0}, remapped{0}, zeroed{0}, faults{0};
};
extern RemapCounters g_remap_counters;

// remap.cpp
const RemapTable* remap_table();               // tick thread: the published table, or nullptr
uint32_t remap_machines();                     // tick thread: the machines it applies to now (peers resolved in Pre)
bool remap_resolve_peers();                    // tick thread (Pre): the remap's peers -> machines; true when it changed
void remap_retire_pre();                       // tick thread (Pre): frees tables no snapshot can still read
// The snapshot step for one component (job thread): every id at a site that is not a game item becomes its look-alike
// or 0. Never faults out: a fault inside zeroes every site of the component (fails closed).
void remap_component(const RemapTable* t, uint16_t index, uint8_t* data, uint32_t size);

// touch.cpp
void touch_start();                            // kiln_plugin_main
void touch_ready();                            // on_ready: the touch program (kiln.world)
bool touch_on();                               // the program is registered and kiln.world can write_other
void touch_pre();                              // tick thread: the program's next batch
KilnResult touch_entity(uint32_t entity, uint32_t* status);  // tick thread
// Tick thread (Pre): touches every entity in the client-channel state maps of the machines (bit per machine) that has
// one of the components (hashes). One pass per machine. Returns the entities touched.
uint32_t touch_relevant(uint32_t machines, const uint32_t* hashes, uint32_t n);
// Tick thread: the component bytes of an entity (kiln.world's reader), or nullptr.
const void* world_component(uint32_t entity, uint32_t hash, uint32_t* size);

// filter.cpp
uintptr_t client_ctx(uintptr_t world, uint32_t machine);   // 0 when unknown
// fn(entity, user) for every entity relevant to machine m on its client channel (tick thread; guarded). The count.
uint32_t relevant_entities(uintptr_t world, uint32_t machine, void (*fn)(uint32_t entity, void* user), void* user);
void filter_set_dirty();                       // tick thread: rebuild the hot set before the next send step

}  // namespace repl
