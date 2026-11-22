// SPDX-License-Identifier: MIT
// kiln.repl.net's touch (kiln_repl.h touch; the dossier's chapter 15 §4 M3 "Invalidation", "Touch"; DESIGN slice B2).
//
// The game re-snapshots an entity on the client channel only when its replication version moves (a tracked write of a
// component with the replication bit). A filter whose output changes without a component change (added, removed,
// another peer set, a remapped peer) would leave the client with the old bytes. A touch moves the version without
// changing a byte: a SYSTEM + HOST_ONLY program registered through kiln.world's public program API with other_write
// accessors on four carrier components (CurrentTransform, ItemState, Inventory, NetworkServerProgress: in the lab
// every live entity had one) calls write_other on the first carrier the entity has. That marks it (the game's
// WriteOther -> WriteMark -> MarkReplication), and that frame's send step re-snapshots it for every player [lab run 6/7:
// 10 touches -> 40 re-snapshots on 4 fakes, 0 without]. Side effect: write_other also marks the entity for the save.
// Requested on the tick thread; the program takes the batch in the next sim step (the batch is handed over in Pre, when
// no sim job runs).
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <vector>

#include "filter.h"
#include "services/kiln_world.h"

namespace repl {
namespace {

const KilnWorldApi* g_world_api = nullptr;
KilnHandle g_program = 0;
bool g_on = false;
constexpr uint32_t kCarriers = 4;
const KilnStr kCarrierNames[kCarriers] = {{"CurrentTransform", 16}, {"ItemState", 9}, {"Inventory", 9}, {"NetworkServerProgress", 21}};
uint32_t g_carrier_hash[kCarriers] = {};
int32_t g_slot[kCarriers] = {-1, -1, -1, -1};

std::vector<uint32_t> g_pending;           // tick thread
std::vector<uint32_t> g_batch;             // handed to the program in Pre
std::atomic<bool> g_ready{false};          // the program has a batch to take
std::atomic<uint64_t> g_done{0}, g_missed{0}, g_requested{0};

void KILN_CALL touch_each(KilnProgramCtx* ctx, uint64_t* iter, KilnEntity, void*) {
    if (!g_ready.load(std::memory_order_acquire)) return;
    uint64_t done = 0, missed = 0;
    for (uint32_t e : g_batch) {
        void* p = nullptr;
        for (uint32_t c = 0; c < kCarriers && !p; ++c) {
            if (g_slot[c] < 0) continue;
            uint32_t size = 0;
            p = g_world_api->write_other(ctx, iter[g_slot[c]], e, &size);  // marks; no byte is changed
        }
        ++(p ? done : missed);
    }
    g_done.fetch_add(done, std::memory_order_relaxed);
    g_missed.fetch_add(missed, std::memory_order_relaxed);
    g_ready.store(false, std::memory_order_release);
}

}  // namespace

const void* world_component(uint32_t entity, uint32_t hash, uint32_t* size) {
    if (size) *size = 0;
    return g_world_api && entity ? g_world_api->component(entity, hash, size) : nullptr;
}

void touch_start() {}

void touch_ready() {
    g_world_api = static_cast<const KilnWorldApi*>(g_api->service(KilnStr{KILN_WORLD_SERVICE, sizeof(KILN_WORLD_SERVICE) - 1}, KILN_WORLD_VERSION));
    if (!g_world_api || !(g_world_api->capabilities() & KILN_WORLD_CAP_WRITE_OTHER)) {
        logf(KILN_LOG_INFO, "repl: touch off (kiln.world %s)", g_world_api ? "cannot write other entities on this build" : "is not here");
        return;
    }
    for (uint32_t c = 0; c < kCarriers; ++c) g_carrier_hash[c] = g_world_api->component_hash(kCarrierNames[c]);
    KilnProgramDesc d{};
    d.size = sizeof d;
    d.flags = KILN_PROGRAM_SYSTEM | KILN_PROGRAM_HOST_ONLY;
    d.name = KilnStr{"kiln_repl_touch", 15};
    d.each = &touch_each;
    d.other_write = KilnNames{kCarrierNames, kCarriers, 0};
    const KilnResult r = g_world_api->register_program(g_owner, &d, &g_program);
    if (r != KILN_OK) {
        logf(KILN_LOG_WARN, "repl: touch off (kiln.world refused the touch program: %d)", static_cast<int>(r));
        return;
    }
    bool any = false;
    for (uint32_t c = 0; c < kCarriers; ++c) {
        g_slot[c] = g_world_api->program_slot(g_program, KILN_SLOT_OTHER_WRITE, kCarrierNames[c]);
        any = any || g_slot[c] >= 0;
    }
    g_on = any;
    logf(KILN_LOG_INFO, "repl: touch %s (program kiln_repl_touch, carriers CurrentTransform %d, ItemState %d, Inventory %d, "
         "NetworkServerProgress %d)", g_on ? "on" : "off", g_slot[0], g_slot[1], g_slot[2], g_slot[3]);
}

bool touch_on() { return g_on; }

void touch_pre() {
    if (!g_on || g_pending.empty() || g_ready.load(std::memory_order_acquire)) return;  // the last batch is not taken yet
    std::sort(g_pending.begin(), g_pending.end());
    g_pending.erase(std::unique(g_pending.begin(), g_pending.end()), g_pending.end());
    g_batch.swap(g_pending);
    g_pending.clear();
    g_ready.store(true, std::memory_order_release);
}

KilnResult touch_entity(uint32_t entity, uint32_t* status) {
    if (status) *status = KILN_REPL_TOUCH_GONE;
    if (!g_on) return KILN_ERR_UNAVAILABLE;
    if (!entity) return KILN_OK;
    // carriers first: kiln.world's exists() asks for a transform, which an item inside an inventory does not have
    bool carrier = false;
    for (uint32_t c = 0; c < kCarriers && !carrier; ++c) {
        uint32_t size = 0;
        carrier = g_slot[c] >= 0 && g_world_api->component(entity, g_carrier_hash[c], &size) != nullptr;
    }
    if (!carrier) {
        if (status) *status = g_world_api->exists(entity) ? KILN_REPL_TOUCH_NO_CARRIER : KILN_REPL_TOUCH_GONE;
        return KILN_OK;
    }
    if (g_pending.size() < (1u << 20)) g_pending.push_back(entity);
    g_requested.fetch_add(1, std::memory_order_relaxed);
    if (status) *status = KILN_REPL_TOUCH_QUEUED;
    return KILN_OK;
}

namespace {
struct Sweep {
    std::vector<uint32_t>* out;
};
void collect(uint32_t e, void* user) { static_cast<Sweep*>(user)->out->push_back(e); }
}  // namespace

uint32_t touch_relevant(uint32_t machines, const uint32_t* hashes, uint32_t n) {
    if (!g_on || !machines || !n) return 0;
    const uintptr_t world = g_world.load(std::memory_order_relaxed);
    std::vector<uint32_t> ents;
    Sweep s{&ents};
    for (uint32_t m = 0; m < KILN_REPL_MACHINES; ++m)
        if ((machines >> m) & 1) relevant_entities(world, m, &collect, &s);
    std::sort(ents.begin(), ents.end());
    ents.erase(std::unique(ents.begin(), ents.end()), ents.end());
    uint32_t touched = 0;
    for (uint32_t e : ents) {
        bool has = false;
        for (uint32_t k = 0; k < n && !has; ++k) {
            uint32_t size = 0;
            has = g_world_api->component(e, hashes[k], &size) != nullptr;
        }
        uint32_t st = 0;
        if (has && touch_entity(e, &st) == KILN_OK && st == KILN_REPL_TOUCH_QUEUED) ++touched;
    }
    return touched;
}

KilnResult KILN_CALL t_touch(KilnEntity e, uint32_t* out_status) {
    if (out_status) *out_status = KILN_REPL_TOUCH_GONE;
    if (!g_on) return KILN_ERR_UNAVAILABLE;
    if (!g_api->on_tick_thread()) return KILN_ERR_WRONG_THREAD;
    if (!e) return KILN_ERR_INVALID_ARGUMENT;
    return touch_entity(static_cast<uint32_t>(e), out_status);
}

// Lab: touches requested, done (marked) and missed (no carrier by the time the program ran).
void touch_counts(uint64_t* requested, uint64_t* done, uint64_t* missed) {
    *requested = g_requested.load(std::memory_order_relaxed);
    *done = g_done.load(std::memory_order_relaxed);
    *missed = g_missed.load(std::memory_order_relaxed);
}

}  // namespace repl
