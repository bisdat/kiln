// SPDX-License-Identifier: MIT
// kiln.items v6: the executor of an equipment set, one transaction with the terminal state machine of chapter 10 §4.4.
// Pure: it drives the game through SetEngine, an interface whose every call returns false when the game faulted (the
// adapter in setexec.cpp catches the structured exception), so a host test runs every transition with a fake engine
// and a failure injected at each step (tests/setexec_test.cpp). Runs inside kiln_items_equip, on whatever thread the game
// runs that program on: a worker once a player is in the world. It reads only through the engine (the program's
// declared accessors and kiln.items' stamp table) and takes no lock.
//
//   Queued -> StaleSnapshot        the canonical bytes read now differ from the snapshot: no transaction
//   Queued -> NoChange / Refused   every entry holds already / the planner refuses: no transaction
//   Validated -> TxOpen            ctor and init once, on the program's one transaction buffer
//   TxOpen -> refusal              a step the game refused (or an item not where the plan put it): rollback once,
//                                  compare with the snapshot while the accessors are valid, free once: Refused
//   TxOpen -> dry run              every step staged: rollback once (never commit), compare, free once: Simulated
//   TxOpen -> ready to commit      the commit's own guards (flags bit 0x40, no InventoryEmptyState accessor with touched
//                                  inventories) would roll back: a refusal, rollback once: Refused
//   CommitCalled                   commit once, never a rollback after it; free once
//   CommitCalled -> desired        Done, the actual deltas
//   CommitCalled -> snapshot       CommitRolledBack (the commit rolled back by itself)
//   anything else, a mismatch after a rollback, a fault: InvariantFailure (MayHaveMutated), the capability goes off; no
//   engine call repairs anything after a fault.
// "As it was" after a rollback is two comparisons: the canonical snapshot (what a set selects) and every byte of the
// seven components the program declares, of every entity of the player's tree (SetEngine::full), read before the
// transaction opened and again after the rollback, in the same program update. An engine fault after the transaction
// opened is not rolled back: the game's own rollback replays a record of operations that a fault may have left half
// written, so after a fault the inventory may be in an unknown state, and the result says so (docs/kiln/services/items.md).
#pragma once
#include <tuple>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "setplan.h"
#include "setsnap.h"

namespace kitems::sets {

constexpr uint32_t kRequestDryRun = 1;  // KilnSetRequestV1.flags
constexpr uint32_t kResultDryRun = 1, kResultCommitted = 2, kResultMayHaveMutated = 4;

enum class SetStatus : uint32_t {
    Done = 0, NoChange = 1, Simulated = 2, Canceled = 3, StaleSnapshot = 4, Refused = 5, CommitRolledBack = 6, Dropped = 7,
    InvariantFailure = 8, Unavailable = 9,
};

// What an equipment slot held.
struct SlotValue {
    uint32_t item = 0, pide = 0, count = 0, stamp = 0;
    uint64_t slot = 0;  // the inventory slot (an equipment page's) that holds it; 0 when empty
    bool operator==(const SlotValue& o) const { return item == o.item && pide == o.pide && count == o.count && stamp == o.stamp && slot == o.slot; }
};
struct SlotChange {
    uint8_t game_slot = 0;
    SlotValue before, after;
};

struct SetOutcome {
    SetStatus status = SetStatus::Unavailable;
    uint32_t game_error = 0;
    uint32_t failed_operation = UINT32_MAX;  // the step the game refused
    uint32_t flags = 0;                      // kResult*
    uint32_t refusal = KILN_SET_REFUSAL_NONE;
    bool engine_fault = false;               // equipment sets go off until the server restarts
    bool seh = false;                        // the game faulted (a structured exception), not a state mismatch
    uint32_t steps = 0;                      // steps staged
    char reason[256] = "";
    std::vector<SlotChange> changes;
};

// Lab only (kiln/data/kiln.items/sets-lab at start; never on a player's server): faults injected into a real run.
struct SetLabFaults {
    int fail_step = -1;          // the game's equip at this step is called with an empty source: a real refusal mid-way
    bool guard_bit = false;      // set the commit's own rollback bit (0x40) before the guard check: refused before commit
    bool commit_anyway = false;  // set it and skip the guard: the game's commit rolls back by itself
    bool fake_mismatch = false;  // after the commit, read the request as unmet: InvariantFailure (sets go off)
    bool any() const { return fail_step >= 0 || guard_bit || commit_anyway || fake_mismatch; }
};

// One set request as the program sees it: everything copied in Pre, no game pointer.
struct SetJob {
    uint64_t ticket = 0;
    uint32_t player = 0;
    uint32_t flags = 0;  // kRequestDryRun
    uint32_t count = 0;
    KilnSetOpV1 entries[kSelectionSlots] = {};
    SnapPrefix prefix;
    std::vector<uint8_t> baseline;  // the snapshot's canonical bytes
    SetLabFaults lab;
};

// The game, for one program update of the player. false: the call faulted (nothing about the state is known after it).
class SetEngine {
public:
    virtual ~SetEngine() = default;
    virtual bool read(SetModel* m, SnapStatus* st) = 0;  // the canonical model now (the program's accessors)
    virtual bool tx_open(bool dry) = 0;                  // ctor + init, once
    virtual bool equip(uint64_t source, uint8_t game_slot, uint8_t* error) = 0;
    virtual bool unequip(uint64_t equipped, uint8_t game_slot, uint8_t* error) = 0;  // equipped: the selection's slot id
    virtual bool locate(uint32_t pide, uint64_t* slot) = 0;  // the staged slot of an item entity; 0 when not found
    virtual bool guards(bool* commit_would_roll_back) = 0;
    virtual bool force_guard_bit() = 0;  // lab only: sets the commit's rollback bit (0x40) of the open transaction
    virtual bool commit() = 0;
    virtual bool rollback() = 0;
    virtual bool free_arrays() = 0;
    // Every byte of the seven components kiln_items_equip declares (kFullParts) of every entity of `m`: the player, its
    // InventorySetup nodes, its inventories and its item entities, each as {u32 entity, u8 part, u32 size, the bytes}. The
    // canonical snapshot reads only the fields that decide what a set selects; a rollback must give back all of these
    // (fuel, ItemState's level, rarity and damage, the action bars' selection, the empty states, ...).
    virtual bool full(const SetModel& m, std::vector<uint8_t>* out) = 0;
};

// The parts of SetEngine::full in its records' order (kiln.items' Acc order: setexec.cpp checks it).
inline constexpr const char* kFullParts[7] = {"Fuel", "Inventory", "InventoryEmptyState", "InventorySetup", "ItemState", "OwnerRelationship", "SlotSelection"};

namespace exec_detail {

inline SlotValue value_at(const SetModel& m, uint32_t x) {
    SlotValue v;
    const uint64_t at = m.equipment[x];
    const Stack* s = at ? m.stack_at(at) : nullptr;
    if (!s || is_empty(*s)) return v;
    v.item = s->item;
    v.pide = s->pide;
    v.count = s->count;
    v.slot = at;
    if (const ItemRec* r = s->pide ? m.item(s->pide) : nullptr) v.stamp = r->stamp;
    return v;
}

// The slots whose value differs between two models, in slot order.
inline std::vector<SlotChange> deltas(const SetModel& before, const SetModel& after) {
    std::vector<SlotChange> out;
    for (uint32_t x = 0; x < kSelectionSlots; ++x) {
        SlotChange c;
        c.game_slot = game_slot_of(static_cast<int>(x));
        c.before = value_at(before, x);
        c.after = value_at(after, x);
        if (!(c.before == c.after)) out.push_back(c);
    }
    return out;
}

// Equipping relocates whole stacks; it must preserve every stack and instance identity, the inventory
// topology and the action bars. Contained slots may move, but must still name the actual stack. An item's
// OwnerRelationship names the inventory that holds it (the lab on server 26430253: the backpack's, an equipment page's),
// so it moves with the item: an owner that named the item's inventory names its new one after the commit, and an owner
// that named anything else stays what it was.
inline bool conserved(const SetModel& before, const SetModel& after) {
    auto stacks = [](const SetModel& m) {
        std::vector<std::tuple<uint32_t, uint32_t, uint32_t>> out;
        for (const auto& leaf : m.leaves)
            for (const auto& s : leaf.s)
                if (!is_empty(s)) out.emplace_back(s.item, s.count, s.pide);
        std::sort(out.begin(), out.end());
        return out;
    };
    if (stacks(before) != stacks(after)) return false;
    for (const auto& r : after.items) {
        const Stack* s = after.stack_of(r.pide);
        if (!s || (r.has_state && r.contained != s->slot)) return false;
        const ItemRec* b = before.item(r.pide);
        const Stack* bs = before.stack_of(r.pide);
        if (!b || !bs || b->has_owner != r.has_owner) return false;
        if (r.has_owner && r.owner != (b->owner == KILN_SLOT_INVENTORY(bs->slot) ? KILN_SLOT_INVENTORY(s->slot) : b->owner)) return false;
    }
    auto stable = [](SetModel m) {
        for (auto& leaf : m.leaves)
            for (auto& s : leaf.s) s.item = s.count = s.pide = 0;
        for (auto& r : m.items) r.contained = 0, r.owner = 0;  // both checked above, against where the item lies now
        for (auto& e : m.equipment) e = 0;
        std::vector<uint8_t> bytes;
        encode(SnapPrefix{}, m, &bytes);
        return bytes;
    };
    return stable(before) == stable(after);
}

// Whether two SetEngine::full reads are the same bytes; if not, the first difference in `why`.
inline bool same_full(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, char* why, size_t cap) {
    if (a == b) return true;
    size_t i = 0;
    while (i + 9 <= a.size() && i + 9 <= b.size()) {
        uint32_t ea = 0, eb = 0, sa = 0, sb = 0;
        std::memcpy(&ea, &a[i], 4), std::memcpy(&eb, &b[i], 4), std::memcpy(&sa, &a[i + 5], 4), std::memcpy(&sb, &b[i + 5], 4);
        const uint8_t part = a[i + 4];
        const char* name = part < 7 ? kFullParts[part] : "a component";
        if (ea != eb || part != b[i + 4] || sa != sb || i + 9 + sa > a.size() || i + 9 + sb > b.size()) {
            std::snprintf(why, cap, "%s of entity %u is %s after it (size %u, was %u)", name, ea, sb == 0 ? "gone" : "not the same record", sb, sa);
            return false;
        }
        for (uint32_t k = 0; k < sa; ++k)
            if (a[i + 9 + k] != b[i + 9 + k]) {
                std::snprintf(why, cap, "%s of entity %u differs at byte %u (0x%02x, was 0x%02x)", name, ea, k, b[i + 9 + k], a[i + 9 + k]);
                return false;
            }
        i += 9 + sa;
    }
    std::snprintf(why, cap, "the component bytes differ in length (%zu, were %zu)", b.size(), a.size());
    return false;
}

// Whether `after` is what the request asked: each named slot holds its selection; every slot the request leaves out holds
// what it held before (its slot id and its stack). Why not, in `why`.
inline bool desired(const SetModel& before, const SetModel& after, const KilnSetOpV1* e, uint32_t count, char* why, size_t cap) {
    bool named[kSelectionSlots] = {};
    for (uint32_t i = 0; i < count; ++i) {
        const int x = selection_index(e[i].equipment_slot);
        named[x] = true;
        const SlotValue v = value_at(after, static_cast<uint32_t>(x));
        bool ok = false;
        switch (e[i].kind) {
            case 0: ok = v.item == 0; break;
            case 1: ok = v.pide == e[i].item_entity && v.item == e[i].item_id && v.stamp == static_cast<uint32_t>(e[i].instance_stamp); break;
            case 2: ok = v.item == e[i].item_id && v.count == e[i].expected_count; break;
        }
        if (!ok)
            return std::snprintf(why, cap, "equipment slot %u holds item 0x%08x entity %u x%u in %u:%u, not what the set asked (item 0x%08x entity %u)",
                                 e[i].equipment_slot, v.item, v.pide, v.count, KILN_SLOT_INVENTORY(v.slot), KILN_SLOT_INDEX(v.slot), e[i].item_id,
                                 e[i].item_entity),
                   false;
    }
    for (uint32_t x = 0; x < kSelectionSlots; ++x)
        if (!named[x] && !(value_at(before, x) == value_at(after, x)))
            return std::snprintf(why, cap, "equipment slot %u changed although the set leaves it out", game_slot_of(static_cast<int>(x))), false;
    if (!conserved(before, after))
        return std::snprintf(why, cap, "the inventory stacks, instance membership, topology or action bars changed unexpectedly"), false;
    return true;
}

}  // namespace exec_detail

// Runs `job` (its snapshot already taken in Pre; the session checked in this step's Pre).
inline SetOutcome execute(SetEngine& g, const SetJob& job, const PlanOptions& opt, SlotTypeOf slot_type, void* ctx) {
    using namespace exec_detail;
    SetOutcome o;
    const bool dry = (job.flags & kRequestDryRun) != 0;
    if (dry) o.flags |= kResultDryRun;
    // a fault: nothing is known after it, nothing is repaired or rolled back; the capability goes off
    uint32_t at_step = UINT32_MAX, of_steps = 0;  // the step being staged when the game faulted (UINT32_MAX: none)
    auto fault = [&](const char* where, bool tx_open, bool commit_called) {
        o.status = SetStatus::InvariantFailure;
        o.engine_fault = true;
        o.seh = true;
        if (tx_open) o.flags |= kResultMayHaveMutated;
        if (commit_called) o.flags |= kResultCommitted;
        o.failed_operation = at_step;
        o.changes.clear();
        char at[48] = "";
        if (at_step != UINT32_MAX) std::snprintf(at, sizeof at, " at step %u of %u", at_step, of_steps);
        std::snprintf(o.reason, sizeof o.reason, "the game faulted %s%s%s", where, at,
                      tx_open ? ": the inventory may be in an unknown state (nothing rolled back or repaired); equipment sets and single equips are "
                                "off until the server restarts"
                              : ": nothing was changed; equipment sets and single equips are off until the server restarts");
        return o;
    };

    // ---- Queued: the snapshot again, from the program's accessors ----
    SetModel now;
    SnapStatus st = SnapStatus::Ok;
    if (!g.read(&now, &st)) {
        o.status = SetStatus::Unavailable;
        o.engine_fault = true;
        o.seh = true;
        std::snprintf(o.reason, sizeof o.reason, "reading the player's inventory inside the program faulted: nothing was changed; equipment sets are off until the server restarts");
        return o;
    }
    std::vector<uint8_t> bytes;
    if (st == SnapStatus::Ok) encode(job.prefix, now, &bytes);
    if (st != SnapStatus::Ok || bytes != job.baseline) {
        o.status = SetStatus::StaleSnapshot;
        o.refusal = KILN_SET_REFUSAL_STALE;
        if (st != SnapStatus::Ok) std::snprintf(o.reason, sizeof o.reason, "the player's inventory cannot be read as it was (%s)", snap_status_text(st));
        else std::snprintf(o.reason, sizeof o.reason, "the player's inventory, equipment or item ownership changed since the snapshot");
        return o;
    }
    const SetPlan plan = plan_set(now, job.entries, job.count, opt, slot_type, ctx);
    if (plan.status == SetPlan::NoChange) {
        o.status = SetStatus::NoChange;
        return o;
    }
    if (plan.status != SetPlan::Planned) {
        o.status = SetStatus::Refused;
        o.refusal = plan.refusal;
        std::snprintf(o.reason, sizeof o.reason, "%s", plan.reason);
        return o;
    }

    // lab: an empty slot to call the game's equip with (the last free Generic slot: an unequip fills the first)
    uint64_t lab_empty = 0;
    if (job.lab.fail_step >= 0)
        for (uint32_t k = 0; k < now.nodes.front().link_count; ++k) {
            if (now.nodes.front().link_cat[k] != kCatGeneric) continue;
            for (uint32_t i = 0; i < kSlotsPerPage; ++i)
                if (const Stack* st2 = now.stack_at(KILN_SLOT_ID(now.nodes.front().link_entity[k], i)); st2 && is_empty(*st2)) lab_empty = st2->slot;
        }

    // every byte of the program's components before anything is staged: a rollback must give back all of them
    std::vector<uint8_t> whole;
    if (!g.full(now, &whole)) {
        o.status = SetStatus::Unavailable;
        o.engine_fault = true;
        o.seh = true;
        std::snprintf(o.reason, sizeof o.reason, "reading the player's components inside the program faulted: nothing was changed; equipment sets are off until the server restarts");
        return o;
    }

    // ---- Validated -> TxOpen ----
    if (!g.tx_open(dry)) return fault("setting up the transaction", true, false);
    bool refused = false;
    of_steps = static_cast<uint32_t>(plan.steps.size());
    for (uint32_t i = 0; i < plan.steps.size(); ++i) {
        const PlanStep& s = plan.steps[i];
        uint8_t err = 0;
        at_step = i;
        if (static_cast<int>(i) == job.lab.fail_step && lab_empty) {  // lab: a real refusal of the game at this step
            if (!g.equip(lab_empty, s.game_slot, &err)) return fault("in its equip (lab refusal)", true, false);
            if (err == 0) err = 0xEE;  // the game took it: report it as a refusal all the same (the rollback undoes it)
        } else if (s.op == OpKind::Equip) {
            uint64_t from = s.source;
            if (s.pide) {  // an item entity: where the steps before put it, never the request's slot
                if (!g.locate(s.pide, &from)) return fault("looking for an item inside the transaction", true, false);
                if (!from) {
                    refused = true;
                    o.failed_operation = i;
                    o.refusal = KILN_SET_REFUSAL_NOT_HERE;
                    std::snprintf(o.reason, sizeof o.reason, "step %u: item entity %u is not where the plan put it", i, s.pide);
                    break;
                }
            }
            if (!g.equip(from, s.game_slot, &err)) return fault("in its equip", true, false);
        } else if (!g.unequip(s.source, s.game_slot, &err)) {
            return fault("in its unequip", true, false);
        }
        ++o.steps;
        if (err != 0) {
            refused = true;
            o.failed_operation = i;
            o.game_error = err;
            std::snprintf(o.reason, sizeof o.reason, "step %u (%s equipment slot %u): the game refused it (error 0x%02x)", i, s.op == OpKind::Equip ? "equip into" : "unequip",
                   s.game_slot, err);
            break;
        }
    }
    at_step = UINT32_MAX;
    bool would = false;
    if (!refused && !dry && (job.lab.guard_bit || job.lab.commit_anyway) && !g.force_guard_bit())
        return fault("setting the commit's rollback bit (lab)", true, false);
    if (!refused && !dry && !job.lab.commit_anyway) {
        if (!g.guards(&would)) return fault("reading the transaction's guards", true, false);
        if (would) {
            refused = true;
            o.refusal = KILN_SET_REFUSAL_NONE;
            std::snprintf(o.reason, sizeof o.reason, "the commit would roll the transaction back by itself (its flags or its InventoryEmptyState accessor): refused before the commit");
        }
    }

    // ---- refusal or dry run: rollback once, compare while the accessors are valid, free once ----
    if (refused || dry) {
        if (!g.rollback()) return fault("in its rollback", true, false);
        SetModel back;
        SnapStatus bst = SnapStatus::Ok;
        if (!g.read(&back, &bst)) return fault("reading the inventory after the rollback", true, false);
        std::vector<uint8_t> after, whole_after;
        if (bst == SnapStatus::Ok) encode(job.prefix, back, &after);
        const bool restored = bst == SnapStatus::Ok && after == job.baseline;
        if (restored && !g.full(now, &whole_after)) return fault("reading the components after the rollback", true, false);
        char differs[160] = "";
        const bool intact = restored && same_full(whole, whole_after, differs, sizeof differs);
        if (!g.free_arrays()) return fault("freeing the transaction", true, false);
        if (!intact) {
            o.status = SetStatus::InvariantFailure;
            o.engine_fault = true;
            o.flags |= kResultMayHaveMutated;
            if (bst == SnapStatus::Ok) o.changes = deltas(now, back);
            char was[256];
            std::snprintf(was, sizeof was, "%s", o.reason);
            if (restored)  // every slot as it was, an item's or an inventory's own bytes not
                std::snprintf(o.reason, sizeof o.reason, "after the rollback (%s) %s%s%s", dry ? "a dry run" : "a refusal", differs, was[0] ? "; " : "", was);
            else
                std::snprintf(o.reason, sizeof o.reason, "after the rollback the inventory is not as it was (%s)%s%s", dry ? "a dry run" : "a refusal",
                              was[0] ? ": " : "", was);
            return o;
        }
        o.status = refused ? SetStatus::Refused : SetStatus::Simulated;
        return o;  // Changes stay empty: a dry run says what it would do in its plan, not as deltas
    }

    // ---- CommitCalled: never a rollback after it ----
    if (!g.commit()) return fault("in its commit", true, true);
    if (!g.free_arrays()) return fault("freeing the transaction after its commit", true, true);
    SetModel done;
    SnapStatus dst = SnapStatus::Ok;
    if (!g.read(&done, &dst)) return fault("reading the inventory after the commit", true, true);
    char why[160] = "";
    const bool met = dst == SnapStatus::Ok && desired(now, done, job.entries, job.count, why, sizeof why);
    if (met && job.lab.fake_mismatch) std::snprintf(why, sizeof why, "a mismatch faked by the lab");
    if (met && !job.lab.fake_mismatch) {
        o.status = SetStatus::Done;
        o.flags |= kResultCommitted;
        o.changes = deltas(now, done);
        return o;
    }
    std::vector<uint8_t> after;
    if (dst == SnapStatus::Ok) encode(job.prefix, done, &after);
    if (dst == SnapStatus::Ok && after == job.baseline) {
        std::vector<uint8_t> whole_after;
        if (!g.full(now, &whole_after)) return fault("reading the components after the commit", true, true);
        char differs[160] = "";
        if (same_full(whole, whole_after, differs, sizeof differs)) {
            o.status = SetStatus::CommitRolledBack;
            std::snprintf(o.reason, sizeof o.reason, "the game's commit rolled the transaction back by itself; the inventory is as it was");
            return o;
        }
        o.status = SetStatus::InvariantFailure;
        o.engine_fault = true;
        o.flags |= kResultMayHaveMutated | kResultCommitted;
        std::snprintf(o.reason, sizeof o.reason, "the game's commit rolled the transaction back by itself, but %s", differs);
        return o;
    }
    o.status = SetStatus::InvariantFailure;
    o.engine_fault = true;
    o.flags |= kResultMayHaveMutated | kResultCommitted;
    if (dst == SnapStatus::Ok) o.changes = deltas(now, done);
    std::snprintf(o.reason, sizeof o.reason, "after the commit the inventory is neither as asked nor as it was (%s)",
                  dst == SnapStatus::Ok ? why : snap_status_text(dst));
    return o;
}

}  // namespace kitems::sets
