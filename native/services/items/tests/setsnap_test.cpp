// SPDX-License-Identifier: MIT
// kiln.items v6 host test: the canonical snapshot and the snapshot store of equipment sets (setsnap.h; chapter 10
// §4.1, §4.2), on made-up component memory. The encoding leaves out padding, the tails of empty stacks and the selected
// action-bar slot, and sees every change of ownership, slot, count, link and session; snapshots compare byte for byte;
// the tree's bounds are refusals, never a silent cut; the budget refuses softly; a snapshot expires after captured + 250;
// tokens are never handed out twice. What the game's components hold on a real server is the lab test
// (mods/tests/equipment-sets-test).
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>

#include "../setsnap.h"

using namespace kitems;
using namespace kitems::sets;

namespace {
int failures = 0;
void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    failures += !ok;
}

// A world of components by entity, laid out as SetLayout's defaults (the checked-in dumps).
struct World final : SnapReader {
    std::map<uint32_t, std::vector<uint8_t>> setup, inv, sel, state, owner;
    std::map<uint32_t, uint32_t> stamps;
    const uint8_t* get(Part p, uint32_t e) override {
        auto& m = p == Part::Setup ? setup : p == Part::Inventory ? inv : p == Part::Selection ? sel : p == Part::ItemState ? state : owner;
        auto it = m.find(e);
        return it == m.end() ? nullptr : it->second.data();
    }
    uint32_t stamp(uint32_t pide, uint32_t) override {
        auto it = stamps.find(pide);
        return it == stamps.end() ? 0 : it->second;
    }

    template <typename T>
    static void put(std::vector<uint8_t>& b, uint32_t off, T v) { std::memcpy(b.data() + off, &v, sizeof v); }

    void node(uint32_t e, std::vector<std::pair<uint32_t, uint8_t>> links, uint8_t initialized = 1) {
        std::vector<uint8_t> b(84, 0);
        for (size_t k = 0; k < links.size(); ++k) {
            put<uint32_t>(b, static_cast<uint32_t>(4 * k), links[k].first);
            b[64 + k] = links[k].second;
        }
        b[80] = 0x01;  // IsPlayer
        b[81] = 3;
        b[82] = 24;
        b[83] = initialized;
        setup[e] = b;
    }
    void page(uint32_t e) { inv[e] = std::vector<uint8_t>(96, 0); }
    void stack(uint32_t e, uint32_t k, uint32_t item, uint32_t count, uint32_t pide) {
        if (!inv.count(e)) page(e);
        put<uint32_t>(inv[e], 12 * k, item);
        put<uint32_t>(inv[e], 12 * k + 4, count);
        put<uint32_t>(inv[e], 12 * k + 8, pide);
    }
    void selection(uint32_t player, uint32_t bar0, uint32_t bar1) {
        std::vector<uint8_t> b(216, 0);
        put<uint32_t>(b, 160, bar0);
        put<uint32_t>(b, 164, bar1);
        sel[player] = b;
    }
    void equip(uint32_t player, uint32_t index, uint32_t entity, uint8_t slot) {
        put<uint32_t>(sel[player], index * 8, entity);
        sel[player][index * 8 + 4] = slot;
    }
    void item(uint32_t pide, uint32_t inventory, uint8_t slot, uint32_t item_id, uint32_t owner_id, uint32_t stamp) {
        std::vector<uint8_t> s(20, 0);
        put<uint32_t>(s, 0, inventory);
        s[4] = slot;
        put<uint32_t>(s, 8, item_id);
        state[pide] = s;
        std::vector<uint8_t> o(4, 0);
        put<uint32_t>(o, 0, owner_id);
        owner[pide] = o;
        stamps[pide] = stamp;
    }
};

// A player 1 with a backpack (two Generic pages), an equipment page, a currency page, one Virtual node with a page, a
// ring worn in Ring0 and a ring and arrows in the backpack.
World player() {
    World w;
    w.node(1, {{2001, 4}, {2002, 4}, {2003, 2}, {2004, 3}, {2005, 5}});
    w.node(2005, {{2006, 4}});
    for (uint32_t e : {2001u, 2002u, 2003u, 2004u, 2006u}) w.page(e);
    w.stack(2003, 0, 0x79442800, 1, 5001);  // Ring0's equipment slot
    w.stack(2001, 2, 0x4623bcb0, 1, 5002);  // a ring in the backpack
    w.stack(2001, 5, 77, 40, 0);            // arrows (a plain stack)
    w.stack(2006, 1, 99, 3, 0xFFFFFFFFu);   // a stack whose pide reads 0xFFFFFFFF
    w.selection(1, 3001, 3002);
    w.equip(1, 10, 2003, 0);
    w.item(5001, 2003, 0, 0x79442800, 1, 11);
    w.item(5002, 2001, 2, 0x4623bcb0, 1, 12);
    return w;
}

SnapPrefix prefix() {
    SnapPrefix p;
    for (int i = 0; i < 16; ++i) p.session[i] = static_cast<uint8_t>(i + 1);
    p.world_epoch = 7;
    p.player = 1;
    p.identity = KilnEventEntityKey{1, KILN_EVENT_KEY_PLAYER_SESSION, 0, 9, 4, 20};
    return p;
}

std::vector<uint8_t> bytes(World& w, const SnapPrefix& p = prefix(), SnapStatus* st = nullptr) {
    SetModel m;
    const SnapStatus s = read_model(w, SetLayout{}, 1, &m);
    if (st) *st = s;
    std::vector<uint8_t> out;
    if (s == SnapStatus::Ok) encode(p, m, &out);
    return out;
}

SnapStatus status(World& w) {
    SetModel m;
    return read_model(w, SetLayout{}, 1, &m);
}

SnapEntry entry(KilnOwner owner, uint64_t step, uint64_t epoch = 7) {
    SnapEntry e;
    e.owner = owner;
    e.captured_step = step;
    e.stamp_epoch = 1;
    e.prefix.world_epoch = epoch;
    return e;
}
}  // namespace

int main() {
    static_assert(sizeof(KilnEventEntityKey) == 40 && sizeof(KilnSetOpV1) == 40, "the shared wire shapes (kiln_decisions.h)");

    // ---- the model ----
    {
        World w = player();
        SetModel m;
        check(read_model(w, SetLayout{}, 1, &m) == SnapStatus::Ok, "a player's tree is read");
        check(m.nodes.size() == 2 && m.nodes[0].entity == 1 && m.nodes[0].link_count == 5 && m.nodes[1].entity == 2005 && m.nodes[1].depth == 1,
              "the root and the Virtual node it reaches, depth first");
        check(m.leaves.size() == 5 && m.leaves[0].entity == 2001 && m.leaves[4].entity == 2006, "the leaf inventories, in ascending order");
        check(m.equipment[10] == KILN_SLOT_ID(2003, 0) && m.equipment[0] == 0 && m.actionbar[0] == 3001,
              "the equipment slots as slot ids, the action bars");
        check(m.items.size() == 2 && m.items[0].pide == 5001 && m.items[0].stamp == 11 && m.items[0].owner == 1 &&
                  m.items[0].contained == KILN_SLOT_ID(2003, 0) && m.items[1].pide == 5002,
              "every item entity once, ascending, with its stamp, ItemState and owner");
        const Stack* s = m.stack_at(KILN_SLOT_ID(2006, 1));
        check(s && s->item == 99 && s->count == 3 && s->pide == 0, "pide 0xFFFFFFFF reads as no item entity");
        check(m.stack_of(5002) && m.stack_of(5002)->slot == KILN_SLOT_ID(2001, 2) && !m.stack_of(4242), "an item entity's stack is found");
        check(!m.stack_at(KILN_SLOT_ID(9999, 0)) && !m.stack_at(KILN_SLOT_ID(2001, 0) | (1ull << 40)), "a slot that is not the player's has no stack");
    }

    // ---- what the encoding leaves out ----
    {
        World a = player();
        const std::vector<uint8_t> base = bytes(a);
        check(!base.empty(), "the snapshot is encoded");
        World b = player();
        b.sel[1][10 * 8 + 5] = 0xAB;  // padding of the Ring0 slot id
        b.sel[1][10 * 8 + 7] = 0xCD;
        check(bytes(b) == base, "the slot ids' padding is not read");
        World c = player();
        c.stack(2002, 4, 0, 17, 9);   // an empty stack (item 0) with a tail
        c.stack(2002, 6, 55, 0, 9);   // count 0
        check(bytes(c) == base, "an empty stack is all zero whatever its tail holds");
        World d = player();
        d.sel[1][192] = 3;            // actionbarSlotSelection: the selected hotbar slot
        for (uint32_t i = 168; i < 188; ++i) d.sel[1][i] = 0x5A;  // actionItem (the item in hand)
        d.sel[1][200] = 9;            // materialSelection
        d.sel[1][188] = 1;            // disabledEquipment
        check(bytes(d) == base, "the selected hotbar slot, the item in hand and the materials are left out");
        World e = player();
        e.stack(2001, 3, 0, 0, 0xFFFFFFFFu);  // an empty slot whose pide reads 0xFFFFFFFF
        check(bytes(e) == base, "an empty slot with pide 0xFFFFFFFF is all zero");
    }

    // ---- what it sees ----
    {
        World a = player();
        const std::vector<uint8_t> base = bytes(a);
        auto differs = [&](const char* what, auto change) {
            World w = player();
            change(w);
            const std::vector<uint8_t> now = bytes(w);
            check(!now.empty() && now != base, what);
        };
        differs("an item entity's owner", [](World& w) { World::put<uint32_t>(w.owner[5002], 0, 2); });
        differs("an item entity's lost OwnerRelationship", [](World& w) { w.owner.erase(5002); });
        differs("an item entity's ItemState slot", [](World& w) { w.state[5002][4] = 3; });
        differs("an equipment slot", [](World& w) { w.equip(1, 11, 2003, 1); });
        differs("an action bar", [](World& w) { World::put<uint32_t>(w.sel[1], 164, 3003); });
        differs("a count", [](World& w) { w.stack(2001, 5, 77, 39, 0); });
        differs("an item moved to another slot", [](World& w) {
            w.stack(2001, 2, 0, 0, 0);
            w.stack(2001, 3, 0x4623bcb0, 1, 5002);
        });
        differs("another item entity in the same slot (same item type)", [](World& w) {
            w.stack(2001, 2, 0x4623bcb0, 1, 5003);
            w.item(5003, 2001, 2, 0x4623bcb0, 1, 13);
        });
        differs("a stamp (the same id, another item entity)", [](World& w) { w.stamps[5002] = 99; });
        differs("a link of the root", [](World& w) { World::put<uint32_t>(w.setup[1], 4, 2007), w.page(2007); });
        differs("a link's category", [](World& w) { w.setup[1][64 + 1] = 1; });
        differs("the generic slot count", [](World& w) { w.setup[1][81] = 2; });
        differs("a link of a nested node", [](World& w) { w.stack(2006, 7, 5, 5, 0); });
        SnapPrefix p = prefix();
        p.identity.player_handle = 68;
        check(bytes(a, p) != base, "another session of the player (the prefix)");
        p = prefix();
        p.session[3] ^= 1;
        check(bytes(a, p) != base, "another process (the nonce)");
        p = prefix();
        p.world_epoch = 8;
        check(bytes(a, p) != base, "another server world (the epoch)");
        World again = player();
        check(bytes(again) == base, "the same state gives the same bytes (no hash: compared byte for byte)");
    }

    // ---- the bounds: refusals, never a cut tree ----
    {
        World w = player();
        w.setup.erase(1);
        check(status(w) == SnapStatus::NoSetup, "no InventorySetup: refused");
        w = player();
        w.setup[1][83] = 0;
        check(status(w) == SnapStatus::NotInitialized, "an uninitialised inventory: refused");
        w = player();
        w.sel.erase(1);
        check(status(w) == SnapStatus::NoSelection, "no SlotSelection: refused");
        w = player();
        World::put<uint32_t>(w.sel[1], 160, 0);
        check(status(w) == SnapStatus::NotPlayer, "a selection without an action bar: not a player");
        w = player();
        w.setup.erase(2005);
        check(status(w) == SnapStatus::MissingNode, "a Virtual link without an InventorySetup: refused");
        w = player();
        w.inv.erase(2002);
        check(status(w) == SnapStatus::MissingInventory, "a link without an Inventory: refused (the baseline reads it as empty)");
        // depth: 1 -> 2005 (1) -> 2101 (2) -> 2102 (3) -> 2103 would be 4
        w = player();
        w.node(2005, {{2006, 4}, {2101, 5}});
        w.node(2101, {{2102, 5}});
        w.node(2102, {{2201, 4}, {2103, 5}});
        w.node(2103, {{2202, 4}});
        w.page(2201);
        w.page(2202);
        check(status(w) == SnapStatus::TooDeep, "a Virtual link below depth 3: refused (the baseline stops there silently)");
        w.node(2102, {{2201, 4}});
        check(status(w) == SnapStatus::Ok, "depth 3 itself is read");
        // a cycle back into the path
        w = player();
        w.node(2005, {{2006, 4}, {1, 5}});
        check(status(w) == SnapStatus::Cycle, "a Virtual link back to its own path: refused");
        // a node reached twice (not a cycle) is read once
        w = player();
        w.node(1, {{2001, 4}, {2002, 4}, {2003, 2}, {2004, 3}, {2005, 5}, {2010, 5}});
        w.node(2010, {{2005, 5}});
        SetModel m;
        check(read_model(w, SetLayout{}, 1, &m) == SnapStatus::Ok && m.nodes.size() == 3, "a node reached twice is read once");
        // 33 leaves
        w = player();
        std::vector<std::pair<uint32_t, uint8_t>> many;
        for (uint32_t i = 0; i < 16; ++i) many.push_back({4000 + i, 4});
        w.node(2005, many);
        w.node(1, {{2001, 4}, {2002, 4}, {2003, 2}, {2004, 3}, {2005, 5}, {2101, 5}});
        std::vector<std::pair<uint32_t, uint8_t>> last;
        for (uint32_t i = 0; i < 13; ++i) last.push_back({4200 + i, 4});  // 4 + 16 + 13 = 33
        w.node(2101, last);
        for (auto& l : many) w.page(l.first);
        for (auto& l : last) w.page(l.first);
        check(status(w) == SnapStatus::TooManyLeaves, "more than 32 leaf inventories: refused (the baseline cuts at 32)");
        w.node(2101, std::vector<std::pair<uint32_t, uint8_t>>(last.begin(), last.end() - 1));
        check(status(w) == SnapStatus::Ok, "32 leaf inventories are read");
        // 65 nodes
        w = player();
        std::vector<std::pair<uint32_t, uint8_t>> virt;
        for (uint32_t i = 0; i < 16; ++i) virt.push_back({6000 + i, 5});
        w.node(2005, virt);
        for (uint32_t i = 0; i < 16; ++i) {
            std::vector<std::pair<uint32_t, uint8_t>> inner;
            for (uint32_t j = 0; j < 4; ++j) inner.push_back({7000 + i * 4 + j, 5});
            w.node(6000 + i, inner);
            for (uint32_t j = 0; j < 4; ++j) w.node(7000 + i * 4 + j, {});
        }
        check(status(w) == SnapStatus::TooManyNodes, "more than 64 inventory setups: refused");
        check(std::strlen(snap_status_text(SnapStatus::TooDeep)) > 0 && snap_status_text(SnapStatus::Ok)[0] == 0, "every refusal has a text");
    }

    // ---- the store ----
    {
        SnapStore s;
        uint64_t t1 = 0, t2 = 0;
        check(s.put(entry(9, 100), 100, &t1) == SnapStore::Put::Ok && t1 == 1, "the first token is 1");
        check(s.put(entry(9, 100), 100, &t2) == SnapStore::Put::Ok && t2 == 2, "tokens grow");
        const SnapEntry* e = nullptr;
        check(s.peek(t1, 9, 7, 1, 350, &e) == SnapStore::Find::Ok && e && e->token == t1, "usable at captured + 250");
        check(s.peek(t1, 9, 7, 1, 351, &e) == SnapStore::Find::Expired && s.size() == 1, "gone at captured + 251");
        check(s.peek(t1, 9, 7, 1, 120, &e) == SnapStore::Find::Unknown, "an expired token is unknown afterwards");
        check(s.peek(t2, 8, 7, 1, 120, &e) == SnapStore::Find::OtherOwner && s.size() == 1, "another owner's token is not found, and stays");
        check(!s.release(t2, 8) && s.release(t2, 9) && s.size() == 0, "only its owner releases a snapshot");
        uint64_t t3 = 0;
        s.put(entry(9, 200, 7), 200, &t3);
        check(s.peek(t3, 9, 8, 1, 200, &e) == SnapStore::Find::OtherWorld && s.size() == 0, "a snapshot of another world is refused and dropped");
        s.put(entry(9, 200), 200, &t3);
        check(s.peek(t3, 9, 7, 2, 200, &e) == SnapStore::Find::StampsReset && s.size() == 0, "a snapshot from before a stamp reset is refused");
        s.put(entry(9, 200), 200, &t3);
        SnapEntry taken;
        check(s.take(t3, 9, 7, 1, 210, &taken) == SnapStore::Find::Ok && taken.token == t3 && s.size() == 0, "take moves it out");
        check(s.take(t3, 9, 7, 1, 210, &taken) == SnapStore::Find::Unknown, "a taken snapshot is used up");
        // the budget: 16 per owner, 256 in all, a soft refusal that evicts nothing
        SnapStore b;
        uint64_t t = 0, last = 0;
        bool all = true;
        for (int i = 0; i < 16; ++i) all = all && b.put(entry(5, 1000), 1000, &t) == SnapStore::Put::Ok;
        check(all && b.count_of(5) == 16, "16 snapshots for one owner");
        check(b.put(entry(5, 1000), 1000, &t) == SnapStore::Put::OwnerFull && t == 0 && b.size() == 16, "the 17th of one owner is refused, nothing evicted");
        for (KilnOwner o = 100; b.size() < 256; ++o)
            for (int i = 0; i < 16 && b.size() < 256; ++i) b.put(entry(o, 1000), 1000, &last);
        check(b.size() == 256 && b.put(entry(4242, 1000), 1000, &t) == SnapStore::Put::GlobalFull && b.size() == 256,
              "the 257th is refused, nothing evicted");
        check(b.put(entry(4242, 1251), 1251, &t) == SnapStore::Put::Ok && b.size() == 1 && t > last, "expired snapshots make room (and do not count)");
        check(b.release_owner(4242) == 1 && b.size() == 0, "an owner's snapshots go with it");
        b.put(entry(1, 2000), 2000, &t);
        const uint64_t before = t;
        b.clear();
        b.put(entry(1, 2000), 2000, &t);
        check(t == before + 1 && b.issued() == t, "a cleared store never hands out a token twice");
        check(s.peek(0, 9, 7, 1, 0, &e) == SnapStore::Find::Unknown && !s.release(0, 9), "token 0 is never a snapshot");
    }

    std::printf("%s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
