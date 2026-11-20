// SPDX-License-Identifier: MIT
// kiln.items v6: the planner of equipment sets (chapter 10 §4.3). Pure: it works on a snapshot's model (setsnap.h) and
// the requested entries, and returns the game's equip and unequip calls in the order one transaction runs them, or a
// refusal before anything is touched. A host test runs it on made-up models (tests/setplan_test.cpp).
//
// The primitives are the game's own: equip(source slot, equipment slot) moves the stack in `source` into the equipment
// slot and the slot's old stack into `source` (a swap, docs/re/items.md section 16); unequip(equipment slot) moves the
// slot's stack into the first free slot (count 0) of a top-level Generic link. The order:
//   A. targets whose item is in a non-equipment slot, in numeric slot order: equip from there (repeated: a swap can
//      put the next target's item into the backpack);
//   B. targets that are to be empty: unequip (each needs a free Generic slot); then A again, since what came off may be
//      what another target asks for;
//   C. what is left is an item worn in one equipment slot and asked for in another: a cycle. It is solved only with a
//      free Generic slot as a temporary (unequip it, then A), and only when the caller allows it: until the lab's
//      rollback matrix proved that the game handles a slot emptied and filled again in one transaction, a cycle is
//      refused (KILN_SET_REFUSAL_CYCLE) before mutation.
// Each source is found again in the staged state by its item entity (a plain stack by its slot), never trusted from the
// request after an earlier step moved it. A backpack change (its contents move with it) is refused unless allowed. An
// item worn in a slot the set leaves out is refused (the set would change that slot). A stack of the item type its
// slot already holds is refused (the game could merge the stacks instead of swapping). At most 128 steps.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "equip_logic.h"
#include "setsnap.h"

namespace kitems::sets {

constexpr uint32_t kMaxSteps = 128;
constexpr uint8_t kGameBackpack = 23;

enum class OpKind : uint8_t { Equip = 1, Unequip = 2 };

struct PlanStep {
    OpKind op = OpKind::Equip;
    uint8_t game_slot = 0;    // keen::EquipmentSlot
    uint8_t entry = 0xFF;     // the request entry it serves; 0xFF: a temporary unequip (a cycle's free slot)
    uint8_t kind = 0;         // what goes in: 0 empty (unequip), 1 an item entity, 2 a plain stack
    uint32_t item = 0, pide = 0, stamp = 0, count = 0;
    uint64_t source = 0;      // equip: where the stack is when the step runs (as planned); unequip: the slot id the
                              // selection names for the equipment slot
};

struct PlanOptions {
    bool cycles = false;    // solve cycles through a free Generic slot (off until the rollback matrix passes)
    bool backpack = false;  // allow a backpack change (off until the rollback matrix passes)
};

struct SetPlan {
    enum Status : uint32_t { Planned = 0, NoChange = 1, Refused = 5 } status = NoChange;
    uint32_t refusal = KILN_SET_REFUSAL_NONE;
    uint32_t failed_entry = UINT32_MAX;
    uint32_t required_free = 0;  // the most free Generic slots the steps use at one time
    char reason[256] = "";
    std::vector<PlanStep> steps;
    // per step (the swap check): the item entity an equip took off its slot (0: the slot was empty; 0xFFFFFFFF: a plain
    // stack came off), whether the step's slot held something before it
    std::vector<uint32_t> swap_out;
    std::vector<bool> target_held;
    // per entry, filled for the executor's checks after the commit
    bool satisfied[kSelectionSlots] = {};
};

// What an item type's EquipmentSlotType is (ItemInfo +0x104; 0 = unknown, the check is then the game's).
using SlotTypeOf = uint8_t (*)(uint32_t item, void* ctx);

namespace plan_detail {

// A stack as the plan follows it: by its item entity, or for a plain stack by the slot it started in.
struct Token {
    uint32_t item = 0, count = 0, pide = 0;
    uint64_t origin = 0;  // the baseline slot of a plain stack; 0 for an item entity
    bool empty() const { return item == 0 || count == 0; }
    bool same(const Token& o) const { return pide ? pide == o.pide : (o.pide == 0 && origin == o.origin && origin != 0); }
};

struct Staged {
    std::vector<std::pair<uint64_t, Token>> bag;  // every non-equipment slot of the player, in link order then index
    Token eq[kSelectionSlots];                         // what each equipment slot holds
    std::vector<uint64_t> generic;                 // slots of top-level Generic links, in order (where an unequip goes)

    Token* at(uint64_t slot) {
        for (auto& b : bag)
            if (b.first == slot) return &b.second;
        return nullptr;
    }
    // where `t` is: a bag slot (>0) or equipment index (via *eq_index), 0 when nowhere
    uint64_t find(const Token& t, int* eq_index) {
        *eq_index = -1;
        for (uint32_t i = 0; i < kSelectionSlots; ++i)
            if (!eq[i].empty() && eq[i].same(t)) return (*eq_index = static_cast<int>(i)), 0;
        for (auto& b : bag)
            if (!b.second.empty() && b.second.same(t)) return b.first;
        return 0;
    }
    uint64_t free_generic() {
        for (uint64_t s : generic)
            if (const Token* t = at(s); t && t->empty()) return s;
        return 0;
    }
    uint32_t free_count() {
        uint32_t n = 0;
        for (uint64_t s : generic)
            if (const Token* t = at(s); t && t->empty()) ++n;
        return n;
    }
};

inline Token token_of(const Stack& s) {
    Token t;
    if (is_empty(s)) return t;
    t.item = s.item;
    t.count = s.count;
    t.pide = s.pide;
    t.origin = s.pide ? 0 : s.slot;
    return t;
}

}  // namespace plan_detail

// Plans `entries` (count <= 20, already checked by valid_entries) against model `m`.
inline SetPlan plan_set(const SetModel& m, const KilnSetOpV1* entries, uint32_t count, const PlanOptions& opt, SlotTypeOf slot_type,
                        void* ctx) {
    using namespace plan_detail;
    SetPlan p;
#define KSET_REFUSE(code, entry, ...)                                  \
    do {                                                               \
        p.status = SetPlan::Refused;                                   \
        p.refusal = (code);                                            \
        p.failed_entry = (entry);                                      \
        p.steps.clear();                                               \
        p.swap_out.clear();                                            \
        p.target_held.clear();                                         \
        std::snprintf(p.reason, sizeof p.reason, __VA_ARGS__);         \
        return p;                                                      \
    } while (0)
    // the staged state: equipment, every other slot of the player's inventories, the Generic slots an unequip fills
    Staged st;
    std::vector<uint64_t> equipped_slots;
    for (uint32_t i = 0; i < kSelectionSlots; ++i) {
        const uint64_t at = m.equipment[i];
        if (!at) continue;
        const Stack* s = m.stack_at(at);
        if (!s) KSET_REFUSE(KILN_SET_REFUSAL_SHAPE, UINT32_MAX, "equipment slot %u points at %u:%u, which is not one of the player's inventories",
                              game_slot_of(static_cast<int>(i)), KILN_SLOT_INVENTORY(at), KILN_SLOT_INDEX(at));
        st.eq[i] = token_of(*s);
        equipped_slots.push_back(at);
    }
    const NodeRec& root = m.nodes.front();
    std::vector<uint32_t> equipment_pages;
    for (uint32_t k = 0; k < root.link_count; ++k) {
        if (root.link_cat[k] == kCatEquipment) equipment_pages.push_back(root.link_entity[k]);
        if (root.link_cat[k] != kCatGeneric) continue;
        for (uint32_t i = 0; i < kSlotsPerPage; ++i) st.generic.push_back(KILN_SLOT_ID(root.link_entity[k], i));
    }
    for (const LeafRec& l : m.leaves)
        for (const Stack& s : l.s) {
            if (std::find(equipped_slots.begin(), equipped_slots.end(), s.slot) != equipped_slots.end()) continue;
            st.bag.push_back({s.slot, token_of(s)});
        }
    auto in_equipment_page = [&](uint64_t slot) {
        for (uint32_t e : equipment_pages)
            if (KILN_SLOT_INVENTORY(slot) == e) return true;
        return false;
    };

    // what each entry asks for, and whether it holds already
    struct Want {
        bool named = false;
        uint8_t kind = 0;
        Token token;
        uint32_t entry = 0, stamp = 0;
    } want[kSelectionSlots];
    for (uint32_t e = 0; e < count; ++e) {
        const KilnSetOpV1& op = entries[e];
        const int x = selection_index(op.equipment_slot);
        Want& w = want[x];
        w.named = true;
        w.kind = op.kind;
        w.entry = e;
        if (op.kind == 0) {
            p.satisfied[x] = st.eq[x].empty();
            continue;
        }
        const Stack* s = nullptr;
        if (op.kind == 1) {
            s = m.stack_of(op.item_entity);
            const ItemRec* rec = m.item(op.item_entity);
            if (!s || !rec) KSET_REFUSE(KILN_SET_REFUSAL_NOT_HERE, e, "item entity %u is not in the player's inventories", op.item_entity);
            if (s->item != op.item_id || rec->stamp != static_cast<uint32_t>(op.instance_stamp))
                KSET_REFUSE(KILN_SET_REFUSAL_NOT_HERE, e, "entity %u is not that item any more (another item entity has the id)", op.item_entity);
            if (rec->has_state && rec->contained != s->slot)
                KSET_REFUSE(KILN_SET_REFUSAL_NOT_HERE, e, "item entity %u's state names another slot than the one that holds it", op.item_entity);
            w.stamp = rec->stamp;
        } else {
            s = m.stack_at(op.source_slot);
            if (!s || is_empty(*s) || s->item != op.item_id)
                KSET_REFUSE(KILN_SET_REFUSAL_NOT_HERE, e, "the stack of item 0x%08x is not in %u:%u", op.item_id, KILN_SLOT_INVENTORY(op.source_slot),
                              KILN_SLOT_INDEX(op.source_slot));
            if (s->count != op.expected_count)
                KSET_REFUSE(KILN_SET_REFUSAL_NOT_HERE, e, "the stack in %u:%u holds %u, not %u", KILN_SLOT_INVENTORY(op.source_slot),
                              KILN_SLOT_INDEX(op.source_slot), s->count, op.expected_count);
        }
        w.token = token_of(*s);
        // Different wire kinds can resolve to the same physical item (instance + whole stack).
        for (uint32_t y = 0; y < kSelectionSlots; ++y)
            if (y != static_cast<uint32_t>(x) && want[y].named && want[y].kind != 0 && want[y].token.same(w.token))
                KSET_REFUSE(KILN_SET_REFUSAL_SHAPE, e, "the same inventory item is selected for more than one equipment slot");
        if (const uint8_t t = slot_type ? slot_type(s->item, ctx) : 0; t && t != slot_type_of(op.equipment_slot))
            KSET_REFUSE(KILN_SET_REFUSAL_SLOT_TYPE, e, "item 0x%08x does not fit equipment slot %u", s->item, op.equipment_slot);
        p.satisfied[x] = !st.eq[x].empty() && st.eq[x].same(w.token);
        if (p.satisfied[x]) continue;
        // where it is: another equipment slot the set must also name; an equipment page slot nothing wears is not used
        int y = -1;
        st.find(w.token, &y);
        if (y >= 0 && !want[y].named) {
            bool later = false;  // the entry for y may come later in the request
            for (uint32_t f = e + 1; f < count; ++f) later = later || selection_index(entries[f].equipment_slot) == y;
            if (!later)
                KSET_REFUSE(KILN_SET_REFUSAL_LOCKED, e, "the item is worn in equipment slot %u, which the set leaves as it is", game_slot_of(y));
        }
        if (y < 0 && in_equipment_page(s->slot))
            KSET_REFUSE(KILN_SET_REFUSAL_NOT_HERE, e, "the item lies in an equipment page slot nothing wears (%u:%u)", KILN_SLOT_INVENTORY(s->slot),
                          KILN_SLOT_INDEX(s->slot));
        if (w.kind == 2 && !st.eq[x].empty() && st.eq[x].item == w.token.item)
            KSET_REFUSE(KILN_SET_REFUSAL_MERGE, e, "equipment slot %u already holds item 0x%08x: another stack of it could merge instead of swapping",
                          op.equipment_slot, w.token.item);
    }
    // a backpack that changes takes its contents along (MoveAll): refused until allowed
    {
        const int b = selection_index(kGameBackpack);
        if (want[b].named && !p.satisfied[b] && !opt.backpack)
            KSET_REFUSE(KILN_SET_REFUSAL_BACKPACK, want[b].entry,
                          "changing the backpack moves its contents: Kiln refuses it in a set until its rollback matrix has passed");
    }

    bool open[kSelectionSlots] = {};
    uint32_t left = 0;
    for (uint32_t x = 0; x < kSelectionSlots; ++x)
        if (want[x].named && !p.satisfied[x]) open[x] = true, ++left;
    if (left == 0) return p;  // NoChange: nothing to run

    const uint32_t free_at_start = st.free_count();
    uint32_t low = free_at_start;
    auto note_free = [&] { low = std::min(low, st.free_count()); };
    bool filled_after_empty[kSelectionSlots] = {};  // emptied by a step, so a later fill is a cycle's
    auto equip = [&](uint32_t x, uint64_t from, uint8_t kind, uint32_t entry, uint32_t stamp) {
        Token* t = st.at(from);
        PlanStep s;
        s.op = OpKind::Equip;
        s.game_slot = game_slot_of(static_cast<int>(x));
        s.entry = static_cast<uint8_t>(entry);
        s.kind = kind;
        s.item = t->item;
        s.pide = t->pide;
        s.stamp = stamp;
        s.count = t->count;
        s.source = from;
        p.steps.push_back(s);
        p.swap_out.push_back(st.eq[x].empty() ? 0u : (st.eq[x].pide ? st.eq[x].pide : 0xFFFFFFFFu));
        p.target_held.push_back(!st.eq[x].empty());
        const Token in = *t;
        *t = st.eq[x];
        st.eq[x] = in;
        note_free();
    };
    auto unequip = [&](uint32_t x, uint32_t entry, uint64_t into) {
        PlanStep s;
        s.op = OpKind::Unequip;
        s.game_slot = game_slot_of(static_cast<int>(x));
        s.entry = static_cast<uint8_t>(entry);
        s.kind = 0;
        s.source = m.equipment[x];  // the slot id the selection names (the game's unequip gets it; filled slots are never unequipped)
        s.item = st.eq[x].item;
        s.pide = st.eq[x].pide;
        s.count = st.eq[x].count;
        p.steps.push_back(s);
        p.swap_out.push_back(0);
        p.target_held.push_back(true);
        *st.at(into) = st.eq[x];
        st.eq[x] = Token{};
        filled_after_empty[x] = true;
        note_free();
    };
    while (left > 0) {
        if (p.steps.size() >= kMaxSteps) KSET_REFUSE(KILN_SET_REFUSAL_TOO_MANY_STEPS, UINT32_MAX, "the set needs more than %u steps", kMaxSteps);
        bool progress = false;
        // A: items from a non-equipment slot into equipment slots that are empty now, in slot order (a plain stack waits
        // for D: the game equips arrows and the like where they lie, so they go last)
        for (uint32_t x = 0; x < kSelectionSlots; ++x) {
            if (!open[x] || want[x].kind != 1 || !st.eq[x].empty()) continue;
            int y = -1;
            const uint64_t at = st.find(want[x].token, &y);
            if (y >= 0 || !at) continue;
            if (filled_after_empty[x] && !opt.cycles)
                KSET_REFUSE(KILN_SET_REFUSAL_CYCLE, want[x].entry, "the set moves items between equipment slots in a cycle: Kiln refuses it until its rollback matrix has passed");
            equip(x, at, want[x].kind, want[x].entry, want[x].stamp);
            open[x] = false, --left, progress = true;
        }
        if (progress) continue;
        // B: removals, one at a time (each takes a free slot; what came off may be wanted: A again)
        for (uint32_t x = 0; x < kSelectionSlots && !progress; ++x) {
            if (!open[x] || want[x].kind != 0) continue;
            const uint64_t g = st.free_generic();
            if (!g) KSET_REFUSE(KILN_SET_REFUSAL_NO_ROOM, want[x].entry, "equipment slot %u comes off, and the backpack has no free slot", game_slot_of(static_cast<int>(x)));
            unequip(x, want[x].entry, g);
            open[x] = false, --left, progress = true;
        }
        if (progress) continue;
        // A2: items into equipment slots that hold another item (the game swaps them), in slot order; the swap check
        // after the plan refuses an order the game's commit does not keep
        for (uint32_t x = 0; x < kSelectionSlots && !progress; ++x) {
            if (!open[x] || want[x].kind != 1) continue;
            int y = -1;
            const uint64_t at = st.find(want[x].token, &y);
            if (y >= 0 || !at) continue;
            equip(x, at, want[x].kind, want[x].entry, want[x].stamp);
            open[x] = false, --left, progress = true;
        }
        if (progress) continue;
        // D: plain stacks (arrows, spells, bait), last
        for (uint32_t x = 0; x < kSelectionSlots && !progress; ++x) {
            if (!open[x] || want[x].kind != 2) continue;
            int y = -1;
            const uint64_t at = st.find(want[x].token, &y);
            if (y >= 0 || !at) continue;
            equip(x, at, want[x].kind, want[x].entry, want[x].stamp);
            open[x] = false, --left, progress = true;
        }
        if (progress) continue;
        // C: a cycle: an item worn in one named slot and asked for in another
        for (uint32_t x = 0; x < kSelectionSlots && !progress; ++x) {
            if (!open[x]) continue;
            int y = -1;
            st.find(want[x].token, &y);
            if (y < 0) KSET_REFUSE(KILN_SET_REFUSAL_NOT_HERE, want[x].entry, "the item for equipment slot %u went missing in the plan", game_slot_of(static_cast<int>(x)));
            if (!opt.cycles)
                KSET_REFUSE(KILN_SET_REFUSAL_CYCLE, want[x].entry,
                              "the set moves items between equipment slots in a cycle: Kiln refuses it until its rollback matrix has passed");
            const uint64_t g = st.free_generic();
            if (!g) KSET_REFUSE(KILN_SET_REFUSAL_CYCLE, want[x].entry, "the set moves items between equipment slots in a cycle, and the backpack has no free slot to do it with");
            unequip(static_cast<uint32_t>(y), 0xFF, g);
            progress = true;
        }
        if (!progress) KSET_REFUSE(KILN_SET_REFUSAL_NOT_HERE, UINT32_MAX, "the set cannot be planned");
    }
    // The swap check (the rollback matrix of 2026-11-15, server 26430253): an equip into a slot that held an item with its
    // own entity is kept by the game's commit only when no later step moves a stack other than the one it took off (a
    // swap, then another equip or a removal, left the swapped slot pointing at the item that came off). Plain stacks are
    // equipped where they lie and do not count.
    for (size_t i = 0; i < p.steps.size(); ++i) {
        const PlanStep& sw = p.steps[i];
        if (sw.op != OpKind::Equip || sw.kind != 1 || !p.swap_out[i]) continue;
        for (size_t j = i + 1; j < p.steps.size(); ++j) {
            const PlanStep& later = p.steps[j];
            if (later.kind == 2) continue;
            const bool only_that_item = later.op == OpKind::Equip && later.pide == p.swap_out[i] && !p.target_held[j];
            if (!only_that_item)
                KSET_REFUSE(KILN_SET_REFUSAL_SWAPS, sw.entry,
                            "more than one slot that holds an item gets another one (or a slot is emptied after a swap): the game's commit "
                            "would point a slot at the item that came off; apply it as two sets (Kiln refuses it before mutation)");
        }
    }
    p.status = SetPlan::Planned;
    p.required_free = free_at_start - low;
    return p;
#undef KSET_REFUSE
}

// The entries a caller sent, as the C ABI allows them (kiln_items.h "equipment sets"). False with the reason.
inline bool valid_entries(const KilnSetOpV1* e, uint32_t count, char* why, size_t cap) {
    if (count > kSelectionSlots || (count && !e)) return std::snprintf(why, cap, "a set has at most 20 entries"), false;
    bool slot_seen[kSelectionSlots] = {};
    for (uint32_t i = 0; i < count; ++i) {
        const KilnSetOpV1& op = e[i];
        if (op.size != sizeof(KilnSetOpV1) || op.reserved || op.reserved2) return std::snprintf(why, cap, "entry %u: bad size or reserved fields", i), false;
        const int x = selection_index(op.equipment_slot);
        if (x < 0) return std::snprintf(why, cap, "entry %u: %u is not an equipment slot", i, op.equipment_slot), false;
        if (slot_seen[x]) return std::snprintf(why, cap, "entry %u: equipment slot %u is named twice", i, op.equipment_slot), false;
        slot_seen[x] = true;
        switch (op.kind) {
            case 0:
                if (op.item_entity || op.item_id || op.expected_count || op.instance_stamp || op.source_slot)
                    return std::snprintf(why, cap, "entry %u: an empty selection carries an item", i), false;
                break;
            case 1:
                if (op.item_entity < 1024 || op.item_entity > 0x7FFFFFFFu || op.item_id == 0 || op.instance_stamp == 0 || op.instance_stamp > 0xFFFFFFFFull)
                    return std::snprintf(why, cap, "entry %u: an item selection needs an item entity, its item id and its stamp", i), false;
                for (uint32_t j = 0; j < i; ++j)
                    if (e[j].kind == 1 && e[j].item_entity == op.item_entity) return std::snprintf(why, cap, "entry %u: item entity %u is selected twice", i, op.item_entity), false;
                break;
            case 2:
                if (op.item_id == 0 || op.expected_count == 0 || !op.source_slot || (op.source_slot >> 40) != 0 || KILN_SLOT_INDEX(op.source_slot) > 7 ||
                    KILN_SLOT_INVENTORY(op.source_slot) == 0 || op.item_entity || op.instance_stamp)
                    return std::snprintf(why, cap, "entry %u: a stack selection needs an item id, a source slot and a count", i), false;
                for (uint32_t j = 0; j < i; ++j)
                    if (e[j].kind == 2 && e[j].source_slot == op.source_slot) return std::snprintf(why, cap, "entry %u: the stack in that slot is selected twice", i), false;
                break;
            default:
                return std::snprintf(why, cap, "entry %u: kind %u is not empty, instance or stack", i, op.kind), false;
        }
    }
    return true;
}

}  // namespace kitems::sets
