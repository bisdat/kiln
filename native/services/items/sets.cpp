// SPDX-License-Identifier: MIT
// kiln.items v6: equipment sets, the service's side (sets.h). This part: the component layouts through kiln.types, the
// Pre reader (GetComponent), the snapshot store's lifetime and the table functions set_capture, set_release_snapshot
// and set_status. The executor inside kiln_items_equip is setexec.cpp.
#include "sets.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <vector>

#include "engine.h"
#include "equip.h"
#include "instances.h"
#include "setexec.h"
#include "setplan.h"
#include "setsnap.h"
#include "setsvc.h"
#include "services/kiln_decisions.h"
#include "services/kiln_types.h"
#include "services/kiln_window_clock.h"
#include "xfer.h"

namespace kitems {

static_assert(sizeof(KilnSetSnapshotV1) == 256 && offsetof(KilnSetSnapshotV1, identity) == 48 &&
                  offsetof(KilnSetSnapshotV1, refusal) == 88 && offsetof(KilnSetSnapshotV1, reason) == 96,
              "KilnSetSnapshotV1: the chapter's 88 bytes, then the refusal tail (the managed mirror depends on it)");
static_assert(sizeof(KilnSetStepV1) == 40 && offsetof(KilnSetStepV1, instance_stamp) == 16 && offsetof(KilnSetStepV1, count) == 32,
              "KilnSetStepV1 layout");
static_assert(sizeof(KilnSetPlanV1) == 5408 && offsetof(KilnSetPlanV1, valid_through_step) == 24 && offsetof(KilnSetPlanV1, reason) == 32 &&
                  offsetof(KilnSetPlanV1, steps) == 288,
              "KilnSetPlanV1 layout");
KilnResult items_item_info(uint32_t id, KilnItemInfo* out);  // items.cpp
static_assert(sizeof(KilnSetStatusV1) == 208 && offsetof(KilnSetStatusV1, captured) == 16 && offsetof(KilnSetStatusV1, detail) == 48,
              "KilnSetStatusV1 layout");

namespace {

using namespace sets;

#define LOGI(...) items_log(KILN_LOG_INFO, __VA_ARGS__)
#define LOGW(...) items_log(KILN_LOG_WARN, __VA_ARGS__)
#define LOGE(...) items_log(KILN_LOG_ERROR, __VA_ARGS__)

constexpr uint32_t kHashOwner = 0xd7361788u;  // keen::ecs::OwnerRelationship

std::mutex g_mu;  // the store and its counters; held for container work only, never while calling the game or a service
SnapStore g_store;
uint64_t g_captured = 0;
uint64_t g_stamp_epoch = 0;
std::atomic<uint32_t> g_live{0};

// ---- lookups (first v6 call) ----

template <typename T>
const T* service(const char* name, uint32_t version) {
    return static_cast<const T*>(items_api()->service(KilnStr{name, std::strlen(name)}, version));
}

struct Layout {
    std::once_flag once;
    bool ok = false;
    SetLayout l;
    char why[160] = "";
} g_layout;

KilnStr str(const char* s) { return KilnStr{s, std::strlen(s)}; }

// One own field of `type` by name: its offset and its type's size. False when the type or the field is not there.
bool field(const KilnTypesApi* t, const KilnType* type, const char* name, uint32_t* offset, uint32_t* size) {
    KilnTypeInfo ti{};
    ti.size = sizeof ti;
    if (!type || t->type_info(type, &ti) != KILN_OK) return false;
    const size_t n = std::strlen(name);
    for (uint32_t i = 0; i < ti.field_count; ++i) {
        KilnTypeField f{};
        if (t->type_field(type, i, &f) != KILN_OK || f.name.len != n || std::memcmp(f.name.ptr, name, n) != 0) continue;
        KilnTypeInfo fi{};
        fi.size = sizeof fi;
        if (t->type_info(f.type, &fi) != KILN_OK) return false;
        *offset = static_cast<uint32_t>(f.offset);
        *size = fi.type_size;
        return true;
    }
    return false;
}

uint32_t type_size(const KilnTypesApi* t, const KilnType* type) {
    KilnTypeInfo ti{};
    ti.size = sizeof ti;
    return type && t->type_info(type, &ti) == KILN_OK ? ti.type_size : 0;
}

// The fields the snapshot reads, by this build's own reflection: names, offsets and sizes are checked, nothing assumed.
bool resolve_layout(SetLayout* L, char* why, size_t cap) {
    const auto* t = service<KilnTypesApi>(KILN_TYPES_SERVICE, 1);
    if (!t || t->header.size < sizeof(KilnTypesApi)) return std::snprintf(why, cap, "kiln.types is not loaded"), false;
    auto ty = [t](const char* n) { return t->type_by_name(str(n)); };
    uint32_t o = 0, s = 0;
    auto need = [&](const KilnType* type, const char* f, uint32_t want_size, uint32_t* out) {
        if (!field(t, type, f, &o, &s) || (want_size && s != want_size)) return false;
        if (out) *out = o;
        return true;
    };
    auto fail = [&](const char* what) { return std::snprintf(why, cap, "this build's %s is not what equipment sets read", what), false; };
    const KilnType* setup = ty("keen::ecs::InventorySetup");
    if (!need(setup, "linksEntities", 64, &L->setup_links) || !need(setup, "linksCategories", 16, &L->setup_cats) ||
        !need(setup, "inventoryFlags", 1, &L->setup_flags) || !need(setup, "genericSlotCount", 1, &L->setup_generic) ||
        !need(setup, "availableSlotCount", 1, &L->setup_available) || !need(setup, "isInitialized", 1, &L->setup_initialized))
        return fail("InventorySetup");
    L->setup_size = type_size(t, setup);
    if (L->setup_initialized != static_cast<uint32_t>(E.setup_initialized) || L->setup_size < L->setup_initialized + 1)
        return fail("InventorySetup.isInitialized (sigdb and reflection disagree)");
    const KilnType* inv = ty("keen::ecs::Inventory");
    const KilnType* stack = ty("keen::ItemStack");
    const KilnType* data = ty("keen::ItemData");
    uint32_t slots = 0, id = 0, dat = 0, count = 0, pide = 0;
    if (!need(inv, "slots", 96, &slots) || slots != 0 || type_size(t, stack) != 12 || !need(stack, "id", 4, &id) || id != 0 ||
        !need(stack, "data", 8, &dat) || dat != 4 || !need(data, "count", 4, &count) || count != 0 || !need(data, "pide", 4, &pide) ||
        pide != 4)
        return fail("Inventory (8 x {id, count, pide})");
    L->inv_size = type_size(t, inv);
    const KilnType* slot = ty("keen::InventorySlotId");
    if (type_size(t, slot) != 8 || !need(slot, "entityId", 4, &L->slot_entity) || !need(slot, "slotIndex", 1, &L->slot_index))
        return fail("InventorySlotId");
    L->slot_size = 8;
    const KilnType* sel = ty("keen::ecs::SlotSelection");
    if (!need(sel, "equipment", 160, &L->sel_equipment) || !need(sel, "actionbarEntityIds", 8, &L->sel_actionbar)) return fail("SlotSelection");
    L->sel_size = type_size(t, sel);
    // the 20 slots in EquipmentSlot's order (SlotSelection.equipment: 0-16, HookShot, Glider, Backpack)
    static const char* const kSlots[sets::kSelectionSlots] = {"vanityHead", "vanityUpperBody", "vanityArms", "vanityLowerBody", "vanityFeet",
                                                     "armourHead", "armourUpperBody", "armourArms", "armourLowerBody", "armourFeet",
                                                     "ring0", "ring1", "rangedWeapon", "shield", "arrow", "spell", "fishingBait",
                                                     "hookShot", "glider", "backpack"};
    const KilnType* eq = ty("keen::ecs::EquipmentSlotData");
    for (uint32_t i = 0; i < sets::kSelectionSlots; ++i) {
        uint32_t at = 0;
        if (!need(eq, kSlots[i], 8, &at) || at != i * 8) return fail("SlotSelection.equipment (its 20 slots in order)");
    }
    const KilnType* st = ty("keen::ecs::ItemState");
    if (!need(st, "containedInSlotId", 8, &L->state_slot) || !need(st, "itemId", 4, &L->state_item)) return fail("ItemState");
    L->state_size = type_size(t, st);
    const KilnType* ow = ty("keen::ecs::OwnerRelationship");
    if (!need(ow, "ownerEntityId", 4, &L->owner_entity)) return fail("OwnerRelationship");
    L->owner_size = type_size(t, ow);
    if (L->setup_size < 84 || L->inv_size < 96 || L->sel_size < L->sel_actionbar + 8 || L->state_size < L->state_item + 4 ||
        L->owner_size < L->owner_entity + 4)
        return fail("component sizes");
    return true;
}

const SetLayout* layout(char* why, size_t cap) {
    std::call_once(g_layout.once, [] {
        g_layout.ok = resolve_layout(&g_layout.l, g_layout.why, sizeof g_layout.why);
        if (g_layout.ok) LOGI("equipment sets: component layouts checked against this build's reflection");
        else LOGW("equipment sets are off: %s", g_layout.why);
    });
    if (!g_layout.ok) std::snprintf(why, cap, "%s", g_layout.why);
    return g_layout.ok ? &g_layout.l : nullptr;
}

// ---- the Pre reader (tick thread, a world-safe phase): GetComponent ----

class PreReader final : public SnapReader {
public:
    PreReader(uintptr_t world, const SetLayout& l) : world_(world), l_(l) {}
    const uint8_t* get(Part part, uint32_t e) override {
        uint32_t hash = 0, size = 0;
        switch (part) {
            case Part::Setup: hash = kHashInventorySetup, size = l_.setup_size; break;
            case Part::Inventory: hash = kHashInventory, size = l_.inv_size; break;
            case Part::Selection: hash = kHashSlotSelection, size = l_.sel_size; break;
            case Part::ItemState: hash = kHashItemState, size = l_.state_size; break;
            case Part::Owner: hash = kHashOwner, size = l_.owner_size; break;
        }
        PtrSize ps{};
        E.get_component(&ps, world_, hash, e);
        return ps.ptr && ps.size >= size ? static_cast<const uint8_t*>(ps.ptr) : nullptr;
    }
    uint32_t stamp(uint32_t pide, uint32_t item) override { return stamp_for(pide, item); }

private:
    uintptr_t world_;
    const SetLayout& l_;
};

// A fault in the game's reads ends the read; nothing with a destructor lives in this frame.
bool guarded_read(SnapReader* r, const SetLayout* l, uint32_t player, SetModel* m, SnapStatus* st, unsigned long* code) {
    __try {
        *st = read_model(*r, *l, player, m);
        return true;
    } __except (*code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void copy_reason(KilnSetSnapshotV1* out, bool tail, uint32_t refusal, const char* fmt, uint32_t x = 0) {
    if (!tail) return;
    out->refusal = refusal;
    std::snprintf(out->reason, sizeof out->reason, fmt, x);
}

}  // namespace

// ---- shared with setexec.cpp (setsvc.h) ----

const KilnWindowClockApiV1* sets_clock() {
    static std::atomic<const KilnWindowClockApiV1*> c{nullptr};
    const auto* have = c.load(std::memory_order_acquire);
    if (have) return have;
    const auto* t = service<KilnWindowClockApiV1>(KILN_WINDOW_CLOCK_SERVICE, KILN_WINDOW_CLOCK_VERSION);
    if (!t || t->header.size < sizeof(KilnWindowClockApiV1)) return nullptr;
    c.store(t, std::memory_order_release);
    return t;
}

const KilnDecisionsApiV1* sets_decisions() {
    static std::atomic<const KilnDecisionsApiV1*> d{nullptr};
    const auto* have = d.load(std::memory_order_acquire);
    if (have) return have;
    const auto* t = service<KilnDecisionsApiV1>(KILN_DECISIONS_SERVICE, KILN_DECISIONS_VERSION);
    // enqueue_native (the verdict route of a native Pre adapter) is the member sets need
    if (!t || t->header.size < offsetof(KilnDecisionsApiV1, enqueue_native) + sizeof(void*)) return nullptr;
    d.store(t, std::memory_order_release);
    return t;
}

const SetLayout* sets_layout(char* why, size_t cap) { return layout(why, cap); }

SetCounters& set_counters() {
    static SetCounters c;
    return c;
}

namespace {
std::atomic<bool> g_broken{false};
std::mutex g_broken_mu;
char g_broken_why[160] = "";
std::atomic<uint32_t> g_parts{UINT32_MAX};  // UINT32_MAX: no set has run yet

// "" when all seven are read; else which are not (for kiln.log and the capability row)
void parts_text(uint32_t parts, char* out, size_t cap) {
    out[0] = 0;
    size_t at = 0;
    for (int k = 0; k < 7; ++k)
        if (!(parts & (1u << k)) && at < cap)
            at += static_cast<size_t>(std::snprintf(out + at, cap - at, "%s%s", at ? ", " : "", sets::kFullParts[k]));
}
}  // namespace

void sets_full_parts(uint32_t parts) {
    if (g_parts.exchange(parts) == parts) return;
    char missing[160];
    parts_text(parts, missing, sizeof missing);
    if (missing[0]) LOGW("equipment sets: a rollback is compared over the components this build's equip program declares; it cannot read: %s", missing);
    else LOGI("equipment sets: a rollback is compared over all seven components of the equip program (%s, ...)", sets::kFullParts[0]);
}

void sets_break(const char* why) {
    {
        std::lock_guard lock(g_broken_mu);
        if (g_broken.load()) return;
        std::snprintf(g_broken_why, sizeof g_broken_why, "%s", why);
        g_broken.store(true);
    }
    LOGE("equipment sets are off until the server, or the hosting player's game, restarts: %s", why);
}

bool sets_available(char* why, size_t cap) {
    if (g_broken.load()) {
        std::lock_guard lock(g_broken_mu);
        std::snprintf(why, cap, "sets and equips were switched off until the server or the hosting game restarts: %s", g_broken_why);
        return false;
    }
    if (!E.sets_ok) return std::snprintf(why, cap, "items.equip_sets is off on this build (the commit's guards or the equip functions did not resolve)"), false;
    if (!instances_on()) return std::snprintf(why, cap, "equipment sets need item instances (items.instances is off)"), false;
    // the program's state, not equip's availability now: "no server world here" is the session, which set_capture
    // answers by itself (KILN_ERR_UNAVAILABLE) and Kiln.Items' row follows like items.equip's
    KilnItemsEquipStatus es{};
    es.size = sizeof es;
    if (equip_status(&es) != KILN_OK || es.program == KILN_ITEMS_PROGRAM_NONE || es.program == KILN_ITEMS_PROGRAM_BROKEN)
        return std::snprintf(why, cap, "equipment sets run in kiln_items_equip, and equip is off: %s", es.detail[0] ? es.detail : "see kiln.log"), false;
    if (!sets_clock()) return std::snprintf(why, cap, "kiln.window_clock is not here (an older core)"), false;
    const KilnDecisionsApiV1* d = sets_decisions();
    if (!d) return std::snprintf(why, cap, "kiln.decisions is not here, or too old for a native set decision (enqueue_native)"), false;
    if (!((d->supported_kinds() >> KILN_DECISION_EQUIP_SET) & 1u))
        return std::snprintf(why, cap, "kiln.decisions takes no equipment-set decisions (kind 18) in this build"), false;
    if (d->header.size >= offsetof(KilnDecisionsApiV1, is_stopped) + sizeof(void*) && d->is_stopped && d->is_stopped())
        return std::snprintf(why, cap, "kiln.decisions stopped after a fault"), false;
    return layout(why, cap) != nullptr;
}

bool sets_broken() { return g_broken.load(); }

// ---- the table ----

KilnResult sets_capture(KilnOwner owner, uint32_t player, KilnSetSnapshotV1* out) {
    constexpr uint32_t kChapter = offsetof(KilnSetSnapshotV1, refusal);
    if (!out || out->size < kChapter || player == 0) return KILN_ERR_INVALID_ARGUMENT;
    const uint32_t size = std::min<uint32_t>(out->size, sizeof(KilnSetSnapshotV1));
    const bool tail = size >= sizeof(KilnSetSnapshotV1);
    std::memset(out, 0, size);
    out->size = size;
    out->player = player;
    char why[160] = "";
    if (!sets_available(why, sizeof why)) return KILN_ERR_UNAVAILABLE;
    KilnWindowStateV1 w{};
    w.size = sizeof w;
    if (sets_clock()->get(&w) != KILN_OK || !kiln_window_world_safe(&w, GetCurrentThreadId())) return KILN_ERR_WRONG_THREAD;
    const uintptr_t world = items_server_world();
    if (!world || w.world_epoch == 0) return KILN_ERR_UNAVAILABLE;
    KilnEventEntityKey key{};
    double at[3] = {};
    uint32_t has = 0;
    const KilnResult rc = sets_decisions()->resolve_entity(player, &key, at, &has);
    if (rc == KILN_ERR_WRONG_THREAD) return rc;
    if (rc != KILN_OK) {
        copy_reason(out, tail, KILN_SET_REFUSAL_NO_IDENTITY, "kiln.decisions has no identity for entity %u (no such entity, or none it can tell apart)", player);
        return KILN_ERR_NOT_FOUND;
    }
    const SetLayout* l = layout(why, sizeof why);
    PreReader reader(world, *l);
    SnapEntry e;
    SnapStatus st = SnapStatus::Ok;
    unsigned long code = 0;
    if (!guarded_read(&reader, l, player, &e.model, &st, &code)) {
        LOGE("equipment sets: reading entity %u's inventory faulted (0x%08lx)", player, code);
        copy_reason(out, tail, KILN_SET_REFUSAL_SHAPE, "reading the inventory of entity %u faulted (see kiln.log)", player);
        return KILN_ERR_INTERNAL;
    }
    if (st != SnapStatus::Ok) {
        const bool who = st == SnapStatus::NoSetup || st == SnapStatus::NotInitialized || st == SnapStatus::NoSelection || st == SnapStatus::NotPlayer;
        if (tail) {
            out->refusal = who ? KILN_SET_REFUSAL_NO_PLAYER : KILN_SET_REFUSAL_SHAPE;
            std::snprintf(out->reason, sizeof out->reason, "%s", snap_status_text(st));
        }
        return KILN_ERR_NOT_FOUND;
    }
    e.owner = owner;
    e.captured_step = w.step;
    e.stamp_epoch = instances_epoch();
    std::memcpy(e.prefix.session, w.process_session, 16);
    e.prefix.world_epoch = w.world_epoch;
    e.prefix.player = player;
    e.prefix.identity = key;
    encode(e.prefix, e.model, &e.bytes);
    const size_t bytes = e.bytes.size();
    const uint32_t inventories = static_cast<uint32_t>(e.model.leaves.size()), items = static_cast<uint32_t>(e.model.items.size());
    uint64_t token = 0;
    SnapStore::Put put;
    {
        std::lock_guard lock(g_mu);
        put = g_store.put(std::move(e), w.step, &token);
        if (put == SnapStore::Put::Ok) ++g_captured;
        g_live.store(static_cast<uint32_t>(g_store.size()));
    }
    if (put != SnapStore::Put::Ok) {
        copy_reason(out, tail, KILN_SET_REFUSAL_BUDGET,
                    put == SnapStore::Put::GlobalFull ? "%u snapshots are live (release a plan you do not apply, or wait 250 steps)"
                                                      : "this owner holds %u snapshots (release a plan you do not apply, or wait 250 steps)",
                    put == SnapStore::Put::GlobalFull ? SnapStore::kGlobal : SnapStore::kPerOwner);
        return KILN_ERR_BUFFER_TOO_SMALL;
    }
    out->token = token;
    out->world_epoch = w.world_epoch;
    out->captured_step = w.step;
    std::memcpy(out->process_session, w.process_session, 16);
    out->identity = key;
    items_log(KILN_LOG_DEBUG, "equipment sets: snapshot %llu of player %u for owner %llu at step %llu: %u inventories, %u item entities, %zu bytes",
              static_cast<unsigned long long>(token), player, static_cast<unsigned long long>(owner),
              static_cast<unsigned long long>(w.step), inventories, items, bytes);
    return KILN_OK;
}

KilnResult sets_release(KilnOwner owner, uint64_t token) {
    std::lock_guard lock(g_mu);
    const bool ok = g_store.release(token, owner);
    g_live.store(static_cast<uint32_t>(g_store.size()));
    return ok ? KILN_OK : KILN_ERR_NOT_FOUND;
}

KilnResult sets_status(KilnSetStatusV1* out) {
    if (!out || out->size < sizeof(KilnSetStatusV1)) return KILN_ERR_INVALID_ARGUMENT;
    std::memset(out, 0, sizeof *out);
    out->size = sizeof *out;
    char why[160] = "";
    out->available = sets_available(why, sizeof why) ? 1 : 0;
    out->broken = g_broken.load() ? 1 : 0;
    std::snprintf(out->detail, sizeof out->detail, "%s", out->available ? "" : why);
    if (const uint32_t parts = g_parts.load(); out->available && parts != UINT32_MAX) {  // known once a set ran
        char missing[160];
        parts_text(parts, missing, sizeof missing);
        if (missing[0]) std::snprintf(out->detail, sizeof out->detail, "a rollback is not compared for %s (not in this build's equip program)", missing);
    }
    const SetCounters& c = set_counters();
    out->pending = c.pending.load();
    out->done = c.done.load();
    out->refused = c.refused.load();
    out->invariant_failures = c.invariants.load();
    std::lock_guard lock(g_mu);
    out->snapshots = static_cast<uint32_t>(g_store.size());
    out->captured = g_captured;
    return KILN_OK;
}

// ---- planning ----

namespace {
uint8_t slot_type_of_item(uint32_t item, void* ctx) { return sets_slot_type(item, ctx); }
}  // namespace

uint8_t sets_slot_type(uint32_t item, void*) {
    KilnItemInfo info{};
    info.size = sizeof info;
    return items_item_info(item, &info) == KILN_OK ? info.equip_slot : 0;  // unknown: the game's equip decides
}

namespace {

const char* find_text(SnapStore::Find f) {
    switch (f) {
        case SnapStore::Find::Ok: return "";
        case SnapStore::Find::Unknown: return "the snapshot is unknown (never taken, used by an apply, released, or the world stopped)";
        case SnapStore::Find::OtherOwner: return "the snapshot is another owner's";
        case SnapStore::Find::Expired: return "the snapshot expired (250 steps after it was taken)";
        case SnapStore::Find::OtherWorld: return "the snapshot is of another server world";
        case SnapStore::Find::StampsReset: return "the item stamps were reset since the snapshot (the engine's id counter went down)";
        case SnapStore::Find::OtherPlayer: return "the snapshot is another player's";
    }
    return "the snapshot cannot be used";
}
}  // namespace

namespace {
bool g_lab = false, g_matrix = false, g_notes_lab = false;
PlanOptions g_plan_options;
}  // namespace

void sets_lab_init() {
    char buf[1024];
    size_t len = 0;
    if (items_api()->path(KILN_PATH_DATA, buf, sizeof buf, &len) != KILN_OK) return;
    std::error_code ec;
    const std::filesystem::path dir = std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(buf), len)) / "kiln.items";
    g_lab = std::filesystem::exists(dir / "sets-lab", ec);
    g_matrix = g_lab && std::filesystem::exists(dir / "sets-matrix", ec);
    g_notes_lab = std::filesystem::exists(dir / "notes-lab", ec);
    if (g_notes_lab) LOGW("kiln.items: lab switch notes-lab (kiln/data/kiln.items): give/take jobs log their component watch note's cost");
    // cycles (two worn items exchanged through a free slot) passed the rollback matrix (2026-11-15): on unless a lab turns
    // them off to see the refusal
    g_plan_options.cycles = !std::filesystem::exists(dir / "sets-no-cycles", ec);
    g_plan_options.backpack = std::filesystem::exists(dir / "sets-backpack", ec);
    if (g_lab || !g_plan_options.cycles || g_plan_options.backpack)
        LOGW("equipment sets: lab switches%s%s%s%s (kiln/data/kiln.items): never on a player's server", g_lab ? " sets-lab (fault injection)" : "",
             g_matrix ? " sets-matrix" : "", !g_plan_options.cycles ? " sets-no-cycles" : "", g_plan_options.backpack ? " sets-backpack" : "");
}

bool sets_lab() { return g_lab; }
bool sets_lab_matrix() { return g_matrix; }
bool notes_lab() { return g_notes_lab; }

PlanOptions sets_plan_options() { return g_plan_options; }

void sets_fill_plan(const SetPlan& p, uint64_t valid_through, KilnSetPlanV1* out) {
    out->status = p.status == SetPlan::Planned ? KILN_SET_PLAN_PLANNED : p.status == SetPlan::NoChange ? KILN_SET_PLAN_NO_CHANGE : KILN_SET_PLAN_REFUSED;
    out->refusal = p.refusal;
    out->failed_entry = p.failed_entry;
    out->required_free_slots = p.required_free;
    out->valid_through_step = valid_through;
    std::snprintf(out->reason, sizeof out->reason, "%s", p.reason);
    out->step_count = static_cast<uint32_t>(std::min<size_t>(p.steps.size(), KILN_SET_MAX_STEPS));
    for (uint32_t i = 0; i < out->step_count; ++i) {
        const PlanStep& s = p.steps[i];
        KilnSetStepV1& o = out->steps[i];
        o.index = i;
        o.op = static_cast<uint8_t>(s.op);
        o.equipment_slot = s.game_slot;
        o.entry = s.entry;
        o.kind = s.kind;
        o.item_id = s.item;
        o.item_entity = s.pide;
        o.instance_stamp = s.stamp;
        o.source_slot = s.op == OpKind::Equip ? s.source : 0;
        o.count = s.count;
    }
}

KilnResult sets_plan(KilnOwner owner, uint64_t token, const KilnSetOpV1* entries, uint32_t count, KilnSetPlanV1* out) {
    if (!out || out->size < sizeof(KilnSetPlanV1)) return KILN_ERR_INVALID_ARGUMENT;
    char why[160] = "";
    if (!valid_entries(entries, count, why, sizeof why)) return KILN_ERR_INVALID_ARGUMENT;
    std::memset(out, 0, sizeof *out);
    out->size = sizeof *out;
    out->failed_entry = UINT32_MAX;
    if (!sets_available(why, sizeof why)) return KILN_ERR_UNAVAILABLE;
    KilnWindowStateV1 w{};
    w.size = sizeof w;
    if (sets_clock()->get(&w) != KILN_OK) return KILN_ERR_UNAVAILABLE;
    SnapStore::Find f = SnapStore::Find::Unknown;
    SetModel model;
    SnapPrefix prefix;
    uint64_t captured = 0;
    if (!sets_peek_model(token, owner, w.world_epoch, w.step, &f, &model, &prefix, &captured)) {
        out->status = KILN_SET_PLAN_STALE;
        out->refusal = KILN_SET_REFUSAL_STALE;
        std::snprintf(out->reason, sizeof out->reason, "%s", find_text(f));
        return KILN_OK;
    }
    const SetPlan p = plan_set(model, entries, count, sets_plan_options(), &slot_type_of_item, nullptr);
    sets_fill_plan(p, captured + SnapStore::kTtl, out);
    return KILN_OK;
}

void sets_pre(uint64_t step) {
    const uint64_t epoch = instances_epoch();
    if (g_live.load(std::memory_order_relaxed) == 0 && epoch == g_stamp_epoch) return;
    size_t forgot = 0;
    {
        std::lock_guard lock(g_mu);
        if (g_stamp_epoch != 0 && epoch != g_stamp_epoch) forgot = g_store.clear();
        g_stamp_epoch = epoch;
        g_store.expire(step);
        g_live.store(static_cast<uint32_t>(g_store.size()));
    }
    if (forgot) LOGW("equipment sets: the item stamps were reset (the engine's id counter went down): %zu snapshot(s) forgotten", forgot);
}

void sets_world_closed() {
    size_t n = 0;
    {
        std::lock_guard lock(g_mu);
        n = g_store.clear();
        g_live.store(0);
    }
    if (n) LOGI("equipment sets: the server world closed: %zu snapshot(s) forgotten", n);
}

void sets_cancel_owner(KilnOwner owner) {
    std::lock_guard lock(g_mu);
    g_store.release_owner(owner);
    g_live.store(static_cast<uint32_t>(g_store.size()));
}

// ---- for setexec.cpp (setsvc.h): the store under its lock ----

SnapStore::Find sets_take(uint64_t token, KilnOwner owner, uint64_t epoch, uint64_t now, SnapEntry* out) {
    std::lock_guard lock(g_mu);
    const SnapStore::Find f = g_store.take(token, owner, epoch, instances_epoch(), now, out);
    g_live.store(static_cast<uint32_t>(g_store.size()));
    return f;
}

SnapStore::Find sets_take_for(uint64_t token, KilnOwner owner, uint32_t player, uint64_t epoch, uint64_t now, SnapEntry* out) {
    std::lock_guard lock(g_mu);
    const SnapEntry* e = nullptr;
    SnapStore::Find f = g_store.peek(token, owner, epoch, instances_epoch(), now, &e);
    if (f == SnapStore::Find::Ok && e->prefix.player != player) f = SnapStore::Find::OtherPlayer;
    else if (f == SnapStore::Find::Ok) f = g_store.take(token, owner, epoch, instances_epoch(), now, out);
    g_live.store(static_cast<uint32_t>(g_store.size()));
    return f;
}

bool sets_peek_model(uint64_t token, KilnOwner owner, uint64_t epoch, uint64_t now, SnapStore::Find* f, SetModel* model,
                     SnapPrefix* prefix, uint64_t* captured_step) {
    std::lock_guard lock(g_mu);
    const SnapEntry* e = nullptr;
    *f = g_store.peek(token, owner, epoch, instances_epoch(), now, &e);
    g_live.store(static_cast<uint32_t>(g_store.size()));
    if (*f != SnapStore::Find::Ok) return false;
    *model = e->model;
    *prefix = e->prefix;
    *captured_step = e->captured_step;
    return true;
}

}  // namespace kitems
