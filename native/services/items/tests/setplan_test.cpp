// SPDX-License-Identifier: MIT
// kiln.items v6 host test: the planner of equipment sets (setplan.h; chapter 10 §4.3) on made-up snapshot models. The
// order (sources from the backpack first, in numeric slot order, then removals), sources found again after a swap,
// cycles refused unless a free slot is allowed as a temporary, the free-slot count, the refusals before mutation
// (no room, a backpack change, an item worn in a slot the set leaves out, a stack that could merge, a slot type, an
// item that is not there) and the entry rules of the C ABI. What the game does with the steps is the lab test
// (mods/tests/equipment-sets-test).
#include <cstdio>
#include <cstring>
#include <vector>

#include "../setplan.h"

using namespace kitems;
using namespace kitems::sets;

namespace {
int failures = 0;
void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    failures += !ok;
}

constexpr uint32_t G1 = 2001, G2 = 2002, EQ = 2003, CUR = 2004;
constexpr uint8_t kRing0 = 10, kRing1 = 11, kHead = 5, kArrow = 14, kBackpack = 23;  // keen::EquipmentSlot
constexpr uint32_t RingA = 0x79442800, RingB = 0x4623bcb0, RingC = 0x11111111, Hat = 0x22222222, Arrows = 77, Bolts = 88, Pack = 0x33333333;

uint64_t slot(uint32_t inv, uint32_t i) { return KILN_SLOT_ID(inv, i); }

struct Model {
    SetModel m;
    Model() {
        m.player = 1;
        NodeRec root{};
        root.entity = 1;
        const uint32_t links[4] = {G1, G2, EQ, CUR};
        const uint8_t cats[4] = {4, 4, 2, 3};
        for (int k = 0; k < 4; ++k) root.link_entity[k] = links[k], root.link_cat[k] = cats[k];
        root.link_count = 4;
        root.initialized = 1;
        m.nodes.push_back(root);
        for (uint32_t e : links) {
            LeafRec l{};
            l.entity = e;
            for (uint32_t k = 0; k < 8; ++k) l.s[k].slot = slot(e, k);
            m.leaves.push_back(l);
        }
        m.actionbar[0] = 3001;
    }
    Stack& at(uint64_t s) { return const_cast<Stack&>(*m.stack_at(s)); }
    void put(uint64_t s, uint32_t item, uint32_t count, uint32_t pide = 0, uint32_t stamp = 0) {
        Stack& st = at(s);
        st.item = item, st.count = count, st.pide = pide;
        if (pide) {
            ItemRec r{};
            r.pide = pide, r.item = item, r.stamp = stamp ? stamp : pide - 4000, r.has_state = 1, r.contained = s, r.has_owner = 1, r.owner = 1;
            m.items.push_back(r);
            std::sort(m.items.begin(), m.items.end(), [](const ItemRec& a, const ItemRec& b) { return a.pide < b.pide; });
        }
    }
    void wear(uint8_t game_slot, uint64_t s) { m.equipment[selection_index(game_slot)] = s; }
    void fill_generic() {
        for (uint32_t e : {G1, G2})
            for (uint32_t k = 0; k < 8; ++k)
                if (is_empty(at(slot(e, k)))) put(slot(e, k), 5, 1);
    }
};

// A player with ring A worn in Ring0 (equipment page slot EQ:0), ring B and a hat in the backpack, arrows worn.
Model base() {
    Model x;
    x.put(slot(EQ, 0), RingA, 1, 5001);
    x.wear(kRing0, slot(EQ, 0));
    x.put(slot(G1, 2), RingB, 1, 5002);
    x.put(slot(G1, 3), Hat, 1, 5003);
    x.put(slot(EQ, 4), Arrows, 40);
    x.wear(kArrow, slot(EQ, 4));
    x.put(slot(G1, 5), Bolts, 30);
    x.put(slot(G2, 0), RingC, 1, 5004);
    return x;
}

KilnSetOpV1 empty(uint8_t game_slot) {
    KilnSetOpV1 o{};
    o.size = sizeof o;
    o.equipment_slot = game_slot;
    return o;
}
KilnSetOpV1 inst(uint8_t game_slot, uint32_t pide, uint32_t item, uint32_t stamp = 0) {
    KilnSetOpV1 o = empty(game_slot);
    o.kind = 1;
    o.item_entity = pide;
    o.item_id = item;
    o.expected_count = 1;
    o.instance_stamp = stamp ? stamp : pide - 4000;
    return o;
}
KilnSetOpV1 stack(uint8_t game_slot, uint32_t item, uint64_t source, uint32_t count) {
    KilnSetOpV1 o = empty(game_slot);
    o.kind = 2;
    o.item_id = item;
    o.source_slot = source;
    o.expected_count = count;
    return o;
}

uint8_t types(uint32_t item, void*) {
    switch (item) {
        case RingA: case RingB: case RingC: return 37;
        case Hat: return 13;
        case Arrows: case Bolts: return 6;
        case Pack: return 38;
        default: return 0;
    }
}

SetPlan plan(const Model& x, std::vector<KilnSetOpV1> e, PlanOptions o = {}) {
    char why[160];
    if (!valid_entries(e.data(), static_cast<uint32_t>(e.size()), why, sizeof why)) {
        std::printf("     (entries refused: %s)\n", why);
        SetPlan p;
        p.status = SetPlan::Refused;
        return p;
    }
    return plan_set(x.m, e.data(), static_cast<uint32_t>(e.size()), o, &types, nullptr);
}

bool refused(const SetPlan& p, uint32_t code) { return p.status == SetPlan::Refused && p.refusal == code && p.steps.empty() && p.reason[0]; }
bool step(const SetPlan& p, size_t i, OpKind op, uint8_t game_slot, uint64_t source = 0) {
    if (i >= p.steps.size()) return false;
    const PlanStep& s = p.steps[i];
    return s.op == op && s.game_slot == game_slot && (op == OpKind::Unequip || s.source == source);
}
}  // namespace

int main() {
    // ---- nothing to do ----
    {
        Model x = base();
        SetPlan p = plan(x, {});
        check(p.status == SetPlan::NoChange && p.steps.empty(), "no entries: no change, no step");
        p = plan(x, {inst(kRing0, 5001, RingA), stack(kArrow, Arrows, slot(EQ, 4), 40), empty(kRing1)});
        check(p.status == SetPlan::NoChange && p.steps.empty() && p.satisfied[selection_index(kRing0)] && p.satisfied[selection_index(kRing1)],
              "every entry holds already (the worn ring, the worn arrows, an empty slot asked empty): no change");
    }

    // ---- one swap ----
    {
        Model x = base();
        SetPlan p = plan(x, {inst(kRing0, 5002, RingB)});
        check(p.status == SetPlan::Planned && p.steps.size() == 1 && step(p, 0, OpKind::Equip, kRing0, slot(G1, 2)) && p.required_free == 0,
              "a ring from the backpack into a worn slot: one equip (a swap), no free slot needed");
        check(p.steps[0].pide == 5002 && p.steps[0].stamp == 1002 && p.steps[0].entry == 0 && p.steps[0].kind == 1, "the step names the item and its entry");
        p = plan(x, {stack(kArrow, Bolts, slot(G1, 5), 30)});
        check(p.status == SetPlan::Planned && step(p, 0, OpKind::Equip, kArrow, slot(G1, 5)), "another stack type into the arrow slot: one equip");
    }

    // ---- removals ----
    {
        Model x = base();
        SetPlan p = plan(x, {empty(kRing0)});
        check(p.status == SetPlan::Planned && p.steps.size() == 1 && step(p, 0, OpKind::Unequip, kRing0) && p.required_free == 1,
              "an explicit empty: one unequip, one free slot");
        x.fill_generic();
        p = plan(x, {empty(kRing0)});
        check(refused(p, KILN_SET_REFUSAL_NO_ROOM) && p.failed_entry == 0, "no free Generic slot: refused before mutation (NoRoom)");
        Model y = base();
        y.put(slot(CUR, 0), 9, 9);  // the currency page is not where an unequip goes
        for (uint32_t k = 0; k < 8; ++k)
            if (is_empty(y.at(slot(G1, k)))) y.put(slot(G1, k), 5, 1);
        for (uint32_t k = 0; k < 7; ++k)
            if (is_empty(y.at(slot(G2, k)))) y.put(slot(G2, k), 5, 1);
        p = plan(y, {empty(kRing0), empty(kArrow)});
        check(refused(p, KILN_SET_REFUSAL_NO_ROOM) && p.failed_entry == 1, "two removals, one free slot: the second is refused, nothing runs");
    }

    // ---- order and sources found again ----
    {
        Model x = base();
        // Ring0 = B (backpack), Ring1 = A (worn in Ring0): B goes in first, A swaps into B's old slot, and goes on from there
        SetPlan p = plan(x, {inst(kRing1, 5001, RingA), inst(kRing0, 5002, RingB)});
        check(p.status == SetPlan::Planned && p.steps.size() == 2 && step(p, 0, OpKind::Equip, kRing0, slot(G1, 2)) &&
                  step(p, 1, OpKind::Equip, kRing1, slot(G1, 2)) && p.steps[1].pide == 5001 && p.steps[0].entry == 1 && p.steps[1].entry == 0,
              "a chain: numeric order, the second source found where the swap put it");
        // Ring0 = empty, Ring1 = A: the unequip first, then A from the free slot it went to
        p = plan(x, {empty(kRing0), inst(kRing1, 5001, RingA)});
        check(p.status == SetPlan::Planned && p.steps.size() == 2 && step(p, 0, OpKind::Unequip, kRing0) &&
                  step(p, 1, OpKind::Equip, kRing1, slot(G1, 0)) && p.required_free == 1,
              "a removal whose item another slot wants: unequip, then equip from the slot it came off into");
        // three slots in reverse order of the request: steps in numeric slot order
        p = plan(x, {stack(kArrow, Bolts, slot(G1, 5), 30), inst(kRing1, 5004, RingC), inst(kHead, 5003, Hat)});
        check(p.status == SetPlan::Planned && p.steps.size() == 3 && p.steps[0].game_slot == kHead && p.steps[1].game_slot == kRing1 &&
                  p.steps[2].game_slot == kArrow,
              "the steps run in numeric slot order whatever the request's order");
    }

    // ---- the swap rule (the lab's rollback matrix: a swap must not be followed by a step that moves another stack) ----
    {
        Model x = base();
        x.put(slot(EQ, 1), RingB, 1, 5006);
        x.wear(kRing1, slot(EQ, 1));
        x.put(slot(EQ, 5), Hat, 1, 5007);
        x.wear(kHead, slot(EQ, 5));
        // two slots that hold items both get others: refused before mutation
        SetPlan p = plan(x, {inst(kRing0, 5002, RingB), inst(kRing1, 5004, RingC)});
        check(refused(p, KILN_SET_REFUSAL_SWAPS), "two swaps: refused before mutation (UnsupportedSwaps)");
        // a swap and an equip into an empty slot: the empty slot first, the swap last
        Model y = base();
        p = plan(y, {inst(kRing0, 5002, RingB), inst(kRing1, 5004, RingC)});
        check(p.status == SetPlan::Planned && p.steps.size() == 2 && step(p, 0, OpKind::Equip, kRing1, slot(G2, 0)) && step(p, 1, OpKind::Equip, kRing0, slot(G1, 2)),
              "a swap and an equip into an empty slot: the empty slot first, the swap last");
        // a swap and a removal: the removal first, the swap last
        p = plan(x, {inst(kHead, 5003, Hat), empty(kRing1)});
        check(p.status == SetPlan::Planned && p.steps.size() == 2 && step(p, 0, OpKind::Unequip, kRing1) && step(p, 1, OpKind::Equip, kHead, slot(G1, 3)),
              "a swap and a removal: the removal first, the swap last");
        // a swap and a stack: the stack (equipped where it lies) after it is fine
        p = plan(y, {inst(kRing0, 5002, RingB), stack(kArrow, Bolts, slot(G1, 5), 30)});
        check(p.status == SetPlan::Planned && p.steps.size() == 2 && step(p, 0, OpKind::Equip, kRing0, slot(G1, 2)) && p.steps[1].game_slot == kArrow,
              "a swap and a stack: the stack last, allowed");
    }

    // ---- cycles ----
    {
        Model x = base();
        x.put(slot(EQ, 1), RingB, 1, 5006);
        x.wear(kRing1, slot(EQ, 1));
        // swap the two worn rings
        SetPlan p = plan(x, {inst(kRing0, 5006, RingB), inst(kRing1, 5001, RingA)});
        check(refused(p, KILN_SET_REFUSAL_CYCLE), "two worn rings swapped: a cycle, refused by default (UnsupportedCycle)");
        PlanOptions cycles;
        cycles.cycles = true;
        p = plan(x, {inst(kRing0, 5006, RingB), inst(kRing1, 5001, RingA)}, cycles);
        check(p.status == SetPlan::Planned && p.steps.size() == 3 && step(p, 0, OpKind::Unequip, kRing1) && p.steps[0].entry == 0xFF &&
                  step(p, 1, OpKind::Equip, kRing0, slot(G1, 0)) && step(p, 2, OpKind::Equip, kRing1, slot(G1, 0)) && p.required_free == 1,
              "with cycles allowed: a temporary unequip into a free slot, then two equips through it");
        x.fill_generic();
        p = plan(x, {inst(kRing0, 5006, RingB), inst(kRing1, 5001, RingA)}, cycles);
        check(refused(p, KILN_SET_REFUSAL_CYCLE), "a cycle with no free slot: refused");
    }

    // ---- refusals before mutation ----
    {
        Model x = base();
        check(refused(plan(x, {inst(kRing1, 5001, RingA)}), KILN_SET_REFUSAL_LOCKED), "an item worn in a slot the set leaves out: refused");
        check(refused(plan(x, {inst(kHead, 5002, RingB)}), KILN_SET_REFUSAL_SLOT_TYPE), "a ring into the head slot: refused (slot type)");
        check(refused(plan(x, {inst(kRing0, 4242 + 4000, RingB)}), KILN_SET_REFUSAL_NOT_HERE), "an item entity the player does not have: refused");
        check(refused(plan(x, {inst(kRing0, 5002, RingB, 77)}), KILN_SET_REFUSAL_NOT_HERE), "another stamp (the id names another item entity): refused");
        check(refused(plan(x, {inst(kRing0, 5002, RingA)}), KILN_SET_REFUSAL_NOT_HERE), "another item type for the entity: refused");
        Model y = base();
        const_cast<ItemRec*>(y.m.item(5002))->contained = slot(G1, 7);
        check(refused(plan(y, {inst(kRing0, 5002, RingB)}), KILN_SET_REFUSAL_NOT_HERE), "an item whose state names another slot: refused");
        check(refused(plan(x, {stack(kArrow, Bolts, slot(G1, 5), 29)}), KILN_SET_REFUSAL_NOT_HERE), "a stack that changed its count: refused (a guard, not a split)");
        check(refused(plan(x, {stack(kArrow, Bolts, slot(G1, 6), 30)}), KILN_SET_REFUSAL_NOT_HERE), "a stack that is not in its slot: refused");
        Model z = base();
        z.put(slot(G1, 6), Arrows, 30);
        check(refused(plan(z, {stack(kArrow, Arrows, slot(G1, 6), 30)}), KILN_SET_REFUSAL_MERGE), "a stack of the type the slot already holds: refused (merge)");
        Model w = base();
        w.put(slot(EQ, 7), RingC, 1, 5007);  // in an equipment page, worn by nothing
        check(refused(plan(w, {inst(kRing1, 5007, RingC)}), KILN_SET_REFUSAL_NOT_HERE), "an item in an equipment page slot nothing wears: refused");
        Model b = base();
        b.put(slot(G2, 3), Pack, 1, 5008);
        check(refused(plan(b, {inst(kBackpack, 5008, Pack)}), KILN_SET_REFUSAL_BACKPACK), "a backpack change: refused by default");
        PlanOptions bp;
        bp.backpack = true;
        check(plan(b, {inst(kBackpack, 5008, Pack)}, bp).status == SetPlan::Planned, "a backpack change when allowed: planned");
        Model s = base();
        s.m.equipment[selection_index(kHead)] = slot(9999, 0);
        check(refused(plan(s, {inst(kRing0, 5002, RingB)}), KILN_SET_REFUSAL_SHAPE), "an equipment slot pointing outside the inventories: refused");
    }

    // ---- the entry rules ----
    {
        char why[160];
        auto bad = [&](std::vector<KilnSetOpV1> e) { return !valid_entries(e.data(), static_cast<uint32_t>(e.size()), why, sizeof why); };
        check(!bad({}) && !bad({empty(kRing0), inst(kRing1, 5001, RingA), stack(kArrow, Bolts, slot(G1, 5), 30)}), "well-formed entries pass");
        KilnSetOpV1 o = empty(kRing0);
        o.size = 8;
        check(bad({o}), "a wrong entry size is refused");
        o = empty(kRing0);
        o.reserved = 1;
        check(bad({o}), "a non-zero reserved field is refused");
        check(bad({empty(17)}) && bad({empty(20)}) && bad({empty(24)}) && bad({empty(25)}), "the materials, Invalid and MainHand are refused");
        check(bad({empty(kRing0), empty(kRing0)}), "a slot named twice is refused");
        check(bad({inst(kRing0, 5001, RingA), inst(kRing1, 5001, RingA)}), "an item selected twice is refused");
        check(bad({stack(kArrow, Bolts, slot(G1, 5), 30), stack(kArrow + 1, Bolts, slot(G1, 5), 30)}), "a stack selected twice is refused");
        KilnSetOpV1 n = inst(kRing0, 5001, RingA);
        n.instance_stamp = 0;
        check(bad({n}), "an item without its stamp is refused");
        n = inst(kRing0, 5, RingA);
        check(bad({n}), "a player id as an item entity is refused");
        check(bad({stack(kArrow, Bolts, 0, 30)}) && bad({stack(kArrow, Bolts, slot(G1, 5), 0)}) && bad({stack(kArrow, Bolts, slot(G1, 9), 30)}),
              "a stack without a slot or a count, or with index 9, is refused");
        o = empty(kRing0);
        o.kind = 3;
        check(bad({o}), "an unknown kind is refused");
        o = empty(kRing0);
        o.item_id = 5;
        check(bad({o}), "an empty selection carrying an item is refused");
        std::vector<KilnSetOpV1> many(21, empty(kRing0));
        check(!valid_entries(many.data(), 21, why, sizeof why), "21 entries are refused");
    }

    std::printf("%s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
