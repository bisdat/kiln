// SPDX-License-Identifier: MIT
// kiln.gemcraft host test (gem wave G3): the forge actions' logic without game memory (gems_logic.h): request checks,
// equipment_actions' declaration against the handler's iterator layout, the hold book of players' actions, what a
// snapshot before and after says an action did, the prechecks, and a free or replaced cost around the game's action.
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>

#include "../gems_logic.h"

namespace {
int failures = 0;
void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    failures += !ok;
}

using namespace kitems::gems;

KilnGemRequest req(uint32_t action) {
    KilnGemRequest r{};
    r.size = sizeof r;
    r.action = action;
    r.player = 1;
    r.forge = 900;
    return r;
}

void requests() {
    KilnGemRequest r = req(KILN_GEM_ACTION_SLOT);
    r.carrier = 2001;
    r.gem_slot = KILN_SLOT_ID(2100, 3);
    check(request_valid(r), "request: Slot with a carrier and a gem slot");
    KilnGemRequest x = r;
    x.gem_slot = 0;
    check(!request_valid(x), "request: Slot without a gem slot is refused");
    x = r;
    x.carrier = 0;
    check(!request_valid(x), "request: Slot without a carrier is refused");
    x = r;
    x.player = 0;
    check(!request_valid(x), "request: player 0 is refused");
    x = r;
    x.reserved[1] = 1;
    check(!request_valid(x), "request: a reserved field is refused");
    x = r;
    x.gem_slot = KILN_SLOT_ID(2100, 8);
    check(!request_valid(x), "request: slot index 8 is refused");
    x = r;
    x.size = sizeof x - 4;
    check(!request_valid(x), "request: a short struct is refused");
    x = r;
    x.flags = KILN_GEM_FREE | KILN_GEM_COST;
    check(!request_valid(x), "request: FREE and COST together are refused");
    x = r;
    x.flags = 0x80;
    check(!request_valid(x), "request: an unknown flag is refused");
    x = r;
    x.cost_item = 5;
    check(!request_valid(x), "request: a cost item without KILN_GEM_COST is refused");
    x = r;
    x.flags = KILN_GEM_COST;
    x.cost_count = 3;
    check(!request_valid(x), "request: a cost count without an item is refused");
    x.cost_item = 0x1234;
    check(request_valid(x), "request: KILN_GEM_COST 3 x item");
    x.cost_count = kMaxCostCount + 1;
    check(!request_valid(x), "request: a cost over 100000 is refused");
    x.cost_item = 0;
    x.cost_count = 0;
    check(request_valid(x), "request: KILN_GEM_COST 0 x nothing (free with Kiln's own wording)");

    KilnGemRequest u = req(KILN_GEM_ACTION_UNSLOT);
    u.carrier = 2001;
    check(request_valid(u), "request: Unslot with a carrier");
    u.gem_slot = KILN_SLOT_ID(2100, 1);
    check(!request_valid(u), "request: Unslot with a gem slot is refused");

    KilnGemRequest g = req(KILN_GEM_ACTION_UPGRADE);
    g.carrier = 2001;
    check(request_valid(g), "request: Upgrade of a socketed gem (carrier)");
    g.carrier = 0;
    g.gem_slot = KILN_SLOT_ID(2100, 0);
    check(request_valid(g), "request: Upgrade of a loose gem (slot)");
    g.carrier = 2001;
    check(!request_valid(g), "request: Upgrade with both carrier and slot is refused");
    g.carrier = g.gem_slot = 0;
    check(!request_valid(g), "request: Upgrade with neither is refused");
    KilnGemRequest c = req(KILN_GEM_ACTION_RECYCLE);
    c.carrier = 2001;
    check(request_valid(c), "request: Recycle of a socketed gem");
    c.action = 5;
    check(!request_valid(c), "request: action 5 is refused");
    c.action = 0;
    check(!request_valid(c), "request: action 0 (Invalid) is refused");

    // review fix: KILN_GEM_RETURN, a forgeless free unsocket into the player's own inventory (Gems.ReturnSocketedGems)
    KilnGemRequest rt = req(KILN_GEM_ACTION_UNSLOT);
    rt.forge = 0;
    rt.carrier = 2001;
    rt.flags = KILN_GEM_RETURN;
    check(request_valid(rt), "request: RETURN: an Unslot of a carrier without a forge");
    rt.gem = 3001;
    check(request_valid(rt), "request: RETURN may name the gem it expects in the socket");
    KilnGemRequest bad = rt;
    bad.forge = 900;
    check(!request_valid(bad), "request: RETURN with a forge is refused (it never runs the forge's handler)");
    bad = rt;
    bad.holder = 1127;
    check(!request_valid(bad), "request: RETURN with a holder is refused (the player's own inventories only)");
    bad = rt;
    bad.flags = KILN_GEM_RETURN | KILN_GEM_FREE;
    check(!request_valid(bad), "request: RETURN with FREE is refused");
    bad = rt;
    bad.action = KILN_GEM_ACTION_UPGRADE;
    check(!request_valid(bad), "request: RETURN on an Upgrade is refused");
    bad = rt;
    bad.carrier = 0;
    check(!request_valid(bad), "request: RETURN without a carrier is refused");
}

std::vector<std::vector<std::string>> game_lists() {
    // equipment_actions as server 26430253 declares it (sigdb/programs/server/f0d824be...json), descriptor order
    std::vector<std::vector<std::string>> l(16);
    l[2] = {"PlayerInput"};
    l[4] = {"Equipment", "ServerConsumedPlayerInput"};
    l[6] = {"BalancingRegistry", "g38_items::ItemRegistry", "g38_items::RecipeRegistry", "impact::AttributeRegistry"};
    l[7] = {"KnowledgeTriggerQueue"};
    l[8] = {"ShroudForge"};
    l[10] = {"Inventory", "InventoryEmptyState", "InventorySetup", "ItemState", "Level", "OwnerRelationship", "SlotSelection",
             "WeaponGemSlot"};
    l[15] = {"GemCraftingEvent", "InventoryFull"};
    return l;
}

void layout() {
    char why[160] = "";
    check(I_COUNT == 30 && kIterBytes == 0xf0, "layout: the handler's iterator is 30 qwords (0xf0, sigdb gems/iter.size)");
    check(I_CONSUMED == 0x40 / 8 && I_BALANCING == 0x70 / 8 && I_RECIPES == 0x80 / 8 && I_ATTRIBUTES == 0x88 / 8 &&
              I_FORGE_TPL == 0x98 / 8 && I_INVENTORY == 0xa0 / 8 && I_LEVEL == 0xc0 / 8 && I_GEMSLOT == 0xd8 / 8 &&
              I_EV_GEM == 0xe0 / 8,
          "layout: the slots the handler reads (+0x40 consumed, +0x70 balancing, +0x80 recipes, +0x88 attributes, +0x98 "
          "forge, +0xa0 Inventory, +0xc0 Level, +0xd8 WeaponGemSlot, +0xe0 GemCraftingEvent)");
    auto l = game_lists();
    check(lists_match(l, why, sizeof why), "layout: equipment_actions' declaration on 26430253 matches");
    auto q = l;
    for (auto& s : q[10]) s = "keen::ecs::" + s;
    check(lists_match(q, why, sizeof why), "layout: names with keen::ecs:: are the same names");
    q = l;
    std::swap(q[10][0], q[10][7]);
    check(lists_match(q, why, sizeof why), "layout: other-write order does not matter (iterator order is by type index)");
    q = l;
    std::swap(q[6][1], q[6][2]);
    check(!lists_match(q, why, sizeof why), "layout: singleton order matters (descriptor order)");
    q = l;
    q[10].push_back("Fuel");
    check(!lists_match(q, why, sizeof why) && std::strstr(why, "list 10"), "layout: one more other-write accessor shifts the events: refused");
    q = l;
    q[15].pop_back();
    check(!lists_match(q, why, sizeof why), "layout: a missing event sender is refused");
    q = l;
    q[3] = {"Actor"};
    check(!lists_match(q, why, sizeof why), "layout: an optional read shifts every slot: refused");
}

KilnGemCraft info(uint32_t player, uint32_t version) {
    KilnGemCraft c{};
    c.size = sizeof c;
    c.player = player;
    c.version = version;
    c.action = KILN_GEM_ACTION_UPGRADE;
    return c;
}

void hold_book() {
    HoldBook b;
    using Now = HoldBook::Now;
    KilnGemCraft i = info(1, 7);
    check(b.on_action(1, 7, 100, &i) == Now::Hold, "hold: a new action is held");
    check(b.on_action(1, 7, 101, &i) == Now::Hold, "hold: still held without an answer");
    uint32_t n = 0;
    std::vector<KilnGemCraft> got;
    b.report([&](const Held& h) { got.push_back(h.info); }, 8);
    check(got.size() == 1 && got[0].version == 7 && got[0].player == 1, "hold: reported once with its record");
    got.clear();
    b.report([&](const Held& h) { got.push_back(h.info); }, 8);
    check(got.empty(), "hold: not reported twice");
    check(!b.decide(1, 8, true), "hold: an answer for another version is not taken");
    check(b.decide(1, 7, false), "hold: allow");
    check(b.on_action(1, 7, 102, &i) == Now::Run, "hold: allowed: it runs");
    check(b.size() == 0, "hold: the entry goes once it ran");

    check(b.on_action(2, 3, 200, nullptr) == Now::Hold && b.decide(2, 3, true), "hold: a second player's action, canceled");
    check(b.on_action(2, 3, 201, nullptr) == Now::Cancel, "hold: canceled: the hook consumes it without a forge");
    check(b.canceled() == 1, "hold: the cancel is counted");

    check(b.on_action(3, 1, 300, nullptr) == Now::Hold, "hold: held at step 300");
    check(b.on_action(3, 1, 300 + kHoldTimeoutSteps - 1, nullptr) == Now::Hold, "hold: still held one step before the timeout");
    check(b.on_action(3, 1, 300 + kHoldTimeoutSteps, nullptr) == Now::Run && b.timed_out() == 1, "hold: nobody answered in 8 steps: it runs");

    check(b.on_action(4, 1, 400, nullptr) == Now::Hold, "hold: player 4 v1 held");
    check(b.on_action(4, 2, 401, nullptr) == Now::Hold && b.size() == 1, "hold: a newer version replaces the older one");
    check(!b.decide(4, 1, false) && b.decide(4, 2, false), "hold: the old version cannot be answered, the new one can");

    HoldBook r;
    r.on_action(5, 1, 10, nullptr);
    r.release_all();
    check(r.on_action(5, 1, 11, nullptr) == Now::Run, "hold: holding stopped: what was held runs");
    r.on_action(6, 1, 10, nullptr);
    r.decide(6, 1, true);
    r.release_all();
    check(r.on_action(6, 1, 11, nullptr) == Now::Cancel, "hold: a cancel stands when holding stops");
    r.on_action(7, 1, 10, nullptr);
    r.expire(500, 250);
    check(r.size() == 0, "hold: an entry the hook never asked about again is dropped (the player left)");
    (void)n;

    // review fixes
    HoldBook q;
    q.on_action(8, 1, 10, nullptr);
    q.release_all();
    got.clear();
    q.report([&](const Held& h) { got.push_back(h.info); }, 8);
    check(got.empty(), "hold: an action released before Pre saw it is not reported (a cancel could no longer stop it)");
    check(q.on_action(8, 1, 11, nullptr) == Now::Run, "hold: ... and it runs");

    HoldBook nh;  // holding stopped: the hook asks the book without holding anything new
    check(nh.on_action(9, 1, 10, nullptr, false) == Now::Run && nh.size() == 0 && nh.total() == 0,
          "hold: not holding: a new action runs at once, nothing is recorded or reported");
    nh.on_action(9, 2, 11, nullptr);
    nh.decide(9, 2, true);
    check(nh.on_action(9, 2, 12, nullptr, false) == Now::Cancel, "hold: not holding any more: a cancel already given still stands");
    nh.on_action(9, 3, 13, nullptr);
    check(nh.on_action(9, 4, 14, nullptr, false) == Now::Run && nh.size() == 0,
          "hold: not holding: a newer version drops the older entry and runs");

    HoldBook lp;  // the game's handler did not consume a version it ran (no Level on the gem): it comes back every step
    lp.on_action(10, 5, 20, nullptr);
    lp.decide(10, 5, false);
    check(lp.on_action(10, 5, 21, nullptr) == Now::Run, "hold: allowed version 5 runs");
    check(lp.on_action(10, 5, 22, nullptr) == Now::Run && lp.size() == 0 && lp.total() == 1,
          "hold: the same version again (the handler left it unconsumed): it runs again, never held or reported twice");
    got.clear();
    lp.report([&](const Held& h) { got.push_back(h.info); }, 8);
    check(got.empty(), "hold: no second GemCraftingEvent for a version that already ran");
    check(lp.on_action(10, 6, 23, nullptr) == Now::Hold && lp.total() == 2, "hold: the player's next version is held as usual");
    HoldBook to;
    to.on_action(11, 1, 30, nullptr);
    check(to.on_action(11, 1, 30 + kHoldTimeoutSteps, nullptr) == Now::Run, "hold: timed out: runs");
    check(to.on_action(11, 1, 30 + kHoldTimeoutSteps + 1, nullptr) == Now::Run && to.size() == 0,
          "hold: a timed-out version seen again runs without a new hold");
    to.expire(30 + kHoldTimeoutSteps + 1 + 251, 250);
    check(to.on_action(11, 1, 600, nullptr) == Now::Hold, "hold: the memory of a version that ran expires with the book's entries");
}

void gifts() {
    check(gift_left(0, 40, 40) == 0, "gift: nothing given, nothing to take back");
    check(gift_left(27, 57, 30) == 0, "gift: the game took its price (57 -> 30): nothing to take back");
    check(gift_left(27, 57, 57) == 27, "gift: the game took nothing (refused): all 27 back");
    check(gift_left(27, 57, 57 + 5) == 27, "gift: never more than was given");
    check(gift_left(27, 27, 0) == 0, "gift: the player had none of their own: the game took exactly the gift");
    check(gift_left(27, 20, 20) == 20, "gift: a count lower than the gift (something else took some) takes back what is there");
}

void setup_block() {
    uint64_t h[I_COUNT];
    for (int i = 0; i < I_COUNT; ++i) h[i] = 0x1000u + static_cast<uint64_t>(i);
    alignas(16) uint8_t s[0xF0];
    std::memset(s, 0xCC, sizeof s);
    handler_setup(h, 2542, 0xC8, s);
    auto q = [&](int off) {
        uint64_t v;
        std::memcpy(&v, s + off, 8);
        return v;
    };
    uint32_t op = 0;
    std::memcpy(&op, s + 0xC8, 4);
    // server 0x144c1c..0x144d03 / client 0x3858ec..0x3859d3: the block the gem handler hands InventoryTxInit
    check(s[0] == 4 && s[1] == 0, "setup: flags 4 (InventoryFull to the operator), stock mode 0");
    check(q(0x08) == h[I_CTX] && q(0x10) == h[I_ITEMS] && q(0x18) == h[I_BALANCING] && q(0x20) == 0,
          "setup: ctx, ItemRegistry, BalancingRegistry; no AttributeRegistry (as the handler)");
    check(q(0x28) == h[I_SETUP] && q(0x30) == h[I_INVENTORY] && q(0x38) == h[I_SLOTSEL] && q(0x40) == h[I_ITEMSTATE] &&
              q(0x48) == h[I_OWNER] && q(0x50) == h[I_SETUP] && q(0x58) == h[I_EMPTY],
          "setup: the seven inventory accessors at the handler's offsets");
    check(q(0x60) == h[I_GEMSLOT], "setup: WeaponGemSlot at +0x60: a socket {carrier, 0} is a slot of the transaction");
    check(q(0x68) == 0 && q(0x70) == 0 && q(0xA0) == 0 && q(0xA8) == h[I_EV_FULL] && q(0xC0) == 0,
          "setup: no NetworkFuel, no Fuel, no PickupEvent, the InventoryFull sender, no knowledge queue");
    check(op == 2542 && q(0xD0) == 0 && q(0xE8) == 0, "setup: the player as the operator, the rest zero");
}

void judging() {
    Snapshot b, a;
    b.socket = a.socket = true;
    b.source_item = 0x55;
    b.source_count = 1;
    b.source_pide = 3001;
    b.gem_pide = 3001;
    b.material = 40;
    a.socket_item = 0x55;
    a.socket_pide = 3001;
    a.material = 35;
    Outcome o = judge(KILN_GEM_ACTION_SLOT, b, a);
    check(o.status == KILN_GEM_DONE && o.paid == 5, "judge: Slot: the socket holds the gem from the slot; 5 paid");
    a.socket_pide = 0;
    a.socket_item = 0;
    check(judge(KILN_GEM_ACTION_SLOT, b, a).status == KILN_GEM_REFUSED, "judge: Slot: the socket is still empty: refused");

    Snapshot ub, ua;
    ub.socket = ua.socket = true;
    ub.socket_pide = 3001;
    ub.socket_item = 0x55;
    ub.gem_pide = 3001;
    ua.gem_in_player = true;
    check(judge(KILN_GEM_ACTION_UNSLOT, ub, ua).status == KILN_GEM_DONE, "judge: Unslot: socket empty, the gem in the backpack");
    ua.gem_in_player = false;
    check(judge(KILN_GEM_ACTION_UNSLOT, ub, ua).status == KILN_GEM_REFUSED, "judge: Unslot: the gem is gone (not in the backpack): refused");

    Snapshot gb, ga;
    gb.gem_pide = ga.gem_pide = 3001;
    gb.level_known = ga.level_known = true;
    gb.level = 9;
    ga.level = 10;
    gb.material = 30;
    ga.material = 3;
    o = judge(KILN_GEM_ACTION_UPGRADE, gb, ga);
    check(o.status == KILN_GEM_DONE && o.level_after == 10 && o.paid == 27, "judge: Upgrade: level 9 -> 10, 27 paid");
    ga.level = 9;
    check(judge(KILN_GEM_ACTION_UPGRADE, gb, ga).status == KILN_GEM_REFUSED, "judge: Upgrade: level unchanged: refused");

    Snapshot rb, ra;
    rb.gem_pide = 3001;
    rb.level = 12;
    rb.essence = 4;
    ra.essence = 13;
    o = judge(KILN_GEM_ACTION_RECYCLE, rb, ra);
    check(o.status == KILN_GEM_DONE && o.got == 9 && o.level_after == 0, "judge: Recycle: the gem is gone, 9 essence got");
    ra.gem_pide = 3001;
    check(judge(KILN_GEM_ACTION_RECYCLE, rb, ra).status == KILN_GEM_REFUSED, "judge: Recycle: the gem is still there: refused");
}

void prechecks() {
    const char* why = "";
    Snapshot s;
    s.socket = true;
    s.source_pide = 3001;
    s.gem_pide = 3001;
    s.gem_category = kWeaponGemCategory;
    check(precheck(KILN_GEM_ACTION_SLOT, s, 10, 5, 5, true, &why) == KILN_GEM_DONE, "precheck: Slot ok (5 to pay, 5 there)");
    check(precheck(KILN_GEM_ACTION_SLOT, s, 10, 5, 4, true, &why) == KILN_GEM_NOT_ENOUGH, "precheck: Slot: cannot pay");
    check(precheck(KILN_GEM_ACTION_SLOT, s, 0, 0, 0, true, &why) == KILN_GEM_NO_FORGE, "precheck: no forge (maxGemLevel 0)");
    s.socket_pide = 3002;
    check(precheck(KILN_GEM_ACTION_SLOT, s, 10, 0, 0, true, &why) == KILN_GEM_OCCUPIED, "precheck: Slot: socket holds a gem");
    s.socket = false;
    check(precheck(KILN_GEM_ACTION_SLOT, s, 10, 0, 0, true, &why) == KILN_GEM_NOT_FOUND, "precheck: Slot: no socket");
    Snapshot u;
    u.socket = true;
    check(precheck(KILN_GEM_ACTION_UNSLOT, u, 10, 0, 0, true, &why) == KILN_GEM_OCCUPIED, "precheck: Unslot: socket empty");
    u.socket_pide = 3001;
    check(precheck(KILN_GEM_ACTION_UNSLOT, u, 10, 0, 0, false, &why) == KILN_GEM_NO_ROOM, "precheck: Unslot: no backpack room");
    check(precheck(KILN_GEM_ACTION_UNSLOT, u, 10, 0, 0, true, &why) == KILN_GEM_DONE, "precheck: Unslot ok");
    Snapshot g;
    g.gem_pide = 3001;
    g.gem_category = kWeaponGemCategory;
    g.level_known = true;
    g.level = 10;
    g.cap = 105;
    check(precheck(KILN_GEM_ACTION_UPGRADE, g, 10, 0, 0, true, &why) == KILN_GEM_MAX_LEVEL, "precheck: Upgrade at the forge's maximum (10)");
    check(precheck(KILN_GEM_ACTION_UPGRADE, g, 15, 30, 30, true, &why) == KILN_GEM_DONE, "precheck: Upgrade below the forge's maximum");
    g.cap = 10;
    check(precheck(KILN_GEM_ACTION_UPGRADE, g, 50, 0, 0, true, &why) == KILN_GEM_MAX_LEVEL, "precheck: Upgrade at the gem's own cap");
    Snapshot r;
    check(precheck(KILN_GEM_ACTION_RECYCLE, r, 10, 0, 0, true, &why) == KILN_GEM_NOT_FOUND, "precheck: Recycle without a gem");

    // review fix: the handler takes any item with an entity as "the gem" (Recycle destroys it, Upgrade raises its Level);
    // Kiln refuses anything that is not a WeaponGem, and an item whose category it cannot read
    Snapshot sword;
    sword.socket = true;
    sword.source_pide = sword.gem_pide = 4001;
    sword.gem_category = 2;  // Weapons
    sword.level_known = true;
    sword.level = 3;
    sword.cap = 105;
    check(precheck(KILN_GEM_ACTION_RECYCLE, sword, 10, 0, 0, true, &why) == KILN_GEM_NOT_FOUND && std::strstr(why, "not a gem"),
          "precheck: Recycle of a sword (a loose slot naming a non-gem) is refused");
    check(precheck(KILN_GEM_ACTION_UPGRADE, sword, 10, 0, 0, true, &why) == KILN_GEM_NOT_FOUND,
          "precheck: Upgrade of a sword's Level through the forge is refused");
    check(precheck(KILN_GEM_ACTION_SLOT, sword, 10, 0, 0, true, &why) == KILN_GEM_NOT_FOUND, "precheck: Slot of a sword is refused");
    sword.gem_category = 0xFF;
    check(precheck(KILN_GEM_ACTION_RECYCLE, sword, 10, 0, 0, true, &why) == KILN_GEM_NOT_FOUND,
          "precheck: an item whose category cannot be read is refused (fail closed)");
    Snapshot odd;  // an odd socket content still comes out: Unslot only moves it into the backpack
    odd.socket = true;
    odd.socket_pide = odd.gem_pide = 4002;
    odd.gem_category = 2;
    check(precheck(KILN_GEM_ACTION_UNSLOT, odd, 10, 0, 0, true, &why) == KILN_GEM_DONE, "precheck: Unslot does not ask what the socket holds");

    // review fix: the forgeless return (KILN_GEM_RETURN)
    Snapshot ret;
    check(precheck_return(ret, true, &why) == KILN_GEM_NOT_FOUND, "return: no socket");
    ret.socket = true;
    check(precheck_return(ret, true, &why) == KILN_GEM_OCCUPIED, "return: an empty socket");
    ret.socket_pide = ret.gem_pide = 3001;
    check(precheck_return(ret, false, &why) == KILN_GEM_NO_ROOM, "return: no free backpack slot");
    check(precheck_return(ret, true, &why) == KILN_GEM_DONE, "return: ok (no forge, no cost)");
}

void costs() {
    KilnGemRequest r = req(KILN_GEM_ACTION_UPGRADE);
    r.carrier = 1;
    CostPlan p = plan_cost(r, 27);
    check(p.give_material == 0 && p.take_count == 0 && !p.replace_yield, "cost: the game's cost: nothing around the action");
    r.flags = KILN_GEM_FREE;
    p = plan_cost(r, 27);
    check(p.give_material == 27 && p.take_count == 0, "cost: free: the game's 27 given before (the game takes them back)");
    r.flags = KILN_GEM_COST;
    r.cost_item = 0x77;
    r.cost_count = 3;
    p = plan_cost(r, 27);
    check(p.give_material == 27 && p.take_item == 0x77 && p.take_count == 3, "cost: replaced: 27 given, 3 x 0x77 taken");
    p = plan_cost(r, 0);
    check(p.give_material == 0 && p.take_count == 3, "cost: replaced where the game charges nothing (Unslot): only the take");
    KilnGemRequest c = req(KILN_GEM_ACTION_RECYCLE);
    c.carrier = 1;
    c.flags = KILN_GEM_FREE;
    p = plan_cost(c, 9);
    check(p.replace_yield && p.yield_count == 0 && p.give_material == 0, "cost: Recycle free: the yield goes, nothing instead");
    c.flags = KILN_GEM_COST;
    c.cost_item = 0x99;
    c.cost_count = 2;
    p = plan_cost(c, 9);
    check(p.replace_yield && p.yield_item == 0x99 && p.yield_count == 2, "cost: Recycle replaced: 2 x 0x99 instead of the essence");
}
}  // namespace

// Owner's real-client check (2026-11-16, a client hosting its world): Gems.ReturnSocketedGems answered "no free backpack
// slot for the gem" twice with free backpack slots. The player's InventorySetup: links 0-1 Equipment (2), 2-6 Generic
// (4) pages all full, 7 a Virtual (5) link to the worn backpack's own InventorySetup with four Generic pages (1, 3, 8, 8
// free), 8 Currency (3). The precheck read only top-level Generic links; the game's unslot fills the backpack's pages too
// (as Items.Read lists them: engine.cpp walk).
void backpack_room() {
    struct Fake {
        std::map<uint32_t, std::vector<uint8_t>> setups, inventories;
        std::vector<uint8_t>& setup(uint32_t e, std::initializer_list<std::pair<uint32_t, uint8_t>> links) {
            auto& b = setups[e];
            b.assign(84, 0);
            int k = 0;
            for (auto [id, cat] : links) {
                std::memcpy(b.data() + 4 * k, &id, 4);
                b[0x40 + k] = cat;
                ++k;
            }
            return b;
        }
        void page(uint32_t e, int free) {
            auto& b = inventories[e];
            b.assign(96, 0);
            for (int j = 0; j < 8 - free; ++j) {
                const uint32_t item = 0x6a40e93a, count = 7;
                std::memcpy(b.data() + 12 * j, &item, 4);
                std::memcpy(b.data() + 12 * j + 4, &count, 4);
            }
        }
        bool room(uint32_t player) {
            return generic_room(
                setups.at(player).data(),
                [&](uint32_t e) -> const uint8_t* { auto it = setups.find(e); return it == setups.end() ? nullptr : it->second.data(); },
                [&](uint32_t e) -> const uint8_t* { auto it = inventories.find(e); return it == inventories.end() ? nullptr : it->second.data(); });
        }
    };
    Fake f;
    f.setup(1, {{2, 2}, {6577, 2}, {6590, 4}, {6591, 4}, {6592, 4}, {6593, 4}, {6594, 4}, {9000, 5}, {6600, 3}});
    f.page(2, 0), f.page(6577, 7), f.page(6600, 8);  // equipment and currency: never where a gem goes
    for (uint32_t e = 6590; e <= 6594; ++e) f.page(e, 0);
    f.setup(9000, {{6576, 4}, {6597, 4}, {6598, 4}, {6599, 4}});  // the worn backpack's own setup: its pages
    f.page(6576, 1), f.page(6597, 3), f.page(6598, 8), f.page(6599, 8);
    check(f.room(1), "room: free slots on the pages of a backpack behind a Virtual link count (the owner's player)");
    f.page(6576, 0), f.page(6597, 0), f.page(6598, 0), f.page(6599, 0);
    check(!f.room(1), "room: a full backpack and full Generic links: no room");
    f.page(6599, 1);
    check(f.room(1), "room: one free slot on the backpack's last page is room");
    f.page(6599, 0), f.page(6577, 8), f.page(6600, 8);
    check(!f.room(1), "room: free Equipment and Currency slots are not room for a gem");
    f.page(6592, 1);
    check(f.room(1), "room: a free slot on a top-level Generic link is room (as before)");
    f.page(6592, 0);
    f.setups.erase(9000);
    check(!f.room(1), "room: a Virtual link whose setup cannot be read gives no room of its own");
    f.setup(9000, {{9001, 5}});  // nested Virtual links, as the walk follows them (depth 3)
    f.setup(9001, {{9002, 5}});
    f.setup(9002, {{6599, 4}});
    f.page(6599, 2);
    check(f.room(1), "room: a page behind nested Virtual links counts");
    f.setup(9002, {{9000, 5}});  // a cycle ends at the depth limit
    check(!f.room(1), "room: a cycle of Virtual links ends (no room, no endless walk)");
}

// ---- 0.2.0-dev.29.15 (GitHub issue 61): separate holders, payers, the return, gem sources ----
void holders_and_payers() {
    // an older caller's 56-byte request: the appended fields read 0, nothing past its size is read
    struct Old {
        KilnGemRequest r;
        uint8_t poison[32];
    } old{};
    old.r = req(KILN_GEM_ACTION_SLOT);
    old.r.size = KILN_GEM_REQUEST_V1_SIZE;
    old.r.carrier = 2001;
    old.r.gem_slot = KILN_SLOT_ID(2100, 3);
    old.r.gem_holder = 777;  // inside the struct but past the caller's size: must not be read
    old.r.pay_from = 9;
    KilnGemRequest n{};
    check(normalize_request(&old.r, &n) && n.size == sizeof n && n.gem_holder == 0 && n.pay_from == 0 && request_valid(n),
          "request: a 56-byte (dev.29.11..14) request is kept, its appended fields 0");
    KilnGemRequest tiny = req(KILN_GEM_ACTION_SLOT);
    tiny.size = KILN_GEM_REQUEST_V1_SIZE - 4;
    check(!normalize_request(&tiny, &n), "request: shorter than the first version is refused");
    check(!normalize_request(nullptr, &n), "request: no request is refused");

    KilnGemRequest r = req(KILN_GEM_ACTION_SLOT);
    r.carrier = 2001;
    r.gem_slot = KILN_SLOT_ID(2100, 3);
    r.gem_holder = 5000;
    r.carrier_holder = 0;
    check(request_valid(r) && !request_routed(r), "holders: a gem in a chest and the carrier with the player is a plain request");
    r.pay_from = KILN_GEM_PAY_HOLDER;
    check(!request_valid(r), "payers: KILN_GEM_PAY_HOLDER without pay_holder is refused");
    r.pay_holder = 5000;
    check(request_valid(r) && request_routed(r), "payers: a paying container is a routed request");
    KilnGemRequest x = r;
    x.pay_from = KILN_GEM_PAY_PLAYER;
    check(!request_valid(x), "payers: a pay_holder without KILN_GEM_PAY_HOLDER is refused");
    x = r;
    x.pay_from = 4;
    x.pay_holder = 0;
    check(!request_valid(x), "payers: an unknown pay_from is refused");
    x = r;
    x.pay_from = KILN_GEM_PAY_BASE;
    x.pay_holder = 0;
    check(request_valid(x) && request_routed(x), "payers: base storage is a routed request");
    x.flags = KILN_GEM_FREE;
    check(request_valid(x) && !request_routed(x), "payers: a free action pays nothing, so it routes nothing");
    x.flags = KILN_GEM_COST;
    x.cost_item = 7;
    x.cost_count = 3;
    check(!request_valid(x), "payers: a replaced cost with another payer is refused (the replacement is the player's)");
    x = r;
    x.reserved2 = 1;
    check(!request_valid(x), "request: reserved2 must be 0");
    KilnGemRequest u = req(KILN_GEM_ACTION_UNSLOT);
    u.carrier = 2001;
    u.return_to = 5000;
    check(request_valid(u) && request_routed(u), "return: an Unslot into a chest is a routed request");
    u.return_to = u.player;
    check(!request_routed(u), "return: return_to = the player routes nothing");
    KilnGemRequest rc = req(KILN_GEM_ACTION_RECYCLE);
    rc.gem_slot = KILN_SLOT_ID(5001, 0);
    rc.pay_from = KILN_GEM_PAY_BASE;
    check(request_valid(rc) && !request_routed(rc), "payers: Recycle pays nothing, its pay_from routes nothing");
    rc.return_to = 5000;
    check(request_routed(rc), "return: Recycle's essence into a chest is routed");
    KilnGemRequest ret = req(KILN_GEM_ACTION_UNSLOT);
    ret.flags = KILN_GEM_RETURN;
    ret.forge = 0;
    ret.carrier = 2001;
    check(request_valid(ret), "return: the forgeless return stays valid");
    ret.return_to = 5000;
    check(!request_valid(ret), "return: the forgeless return refuses a return_to (and every appended field)");

    // the payment spread over payers on the game's own transaction
    using P = std::vector<std::pair<uint32_t, uint32_t>>;
    PayPlan p = plan_payment(P{{10, 5}, {11, 20}, {12, 3}}, 12);
    check(p.before.size() == 1 && p.before[0].payer == 10 && p.before[0].take == 5 && p.last == 11 && p.last_take == 7 &&
              p.short_by == 0, "payers: 12 from 5 + 20: Kiln takes 5 from the first, the game's call 7 from the second");
    p = plan_payment(P{{10, 30}, {11, 20}}, 12);
    check(p.before.empty() && p.last == 10 && p.last_take == 12, "payers: one payer covers it: only the game's own call, at that payer");
    p = plan_payment(P{{10, 2}, {11, 3}}, 12);
    check(p.short_by == 7 && p.last == 11 && p.last_take == 3 + 7 && p.before.size() == 1,
          "payers: short by 7: the game's call asks the last payer for its 3 and the 7 missing (the game then rolls back)");
    p = plan_payment(P{{10, 0}, {11, 0}}, 4);
    check(p.before.empty() && p.last == 10 && p.last_take == 4 && p.short_by == 4, "payers: nobody has any: the game's call finds all of it missing");
    p = plan_payment(P{{10, 0}, {11, 9}, {12, 9}}, 9);
    check(p.before.empty() && p.last == 11 && p.last_take == 9, "payers: an empty payer is skipped");
    p = plan_payment(P{}, 9);
    check(p.last == 0 && p.before.empty(), "payers: no payers: no plan (the request ends NOT_ENOUGH before the game runs)");
    check(payers_have(P{{1, 4}, {2, 6}}) == 10, "payers: the sum of what they have");
}

KilnGemSource src(uint32_t player, uint32_t page, uint32_t index, uint32_t gem) {
    KilnGemSource s{};
    s.size = sizeof s;
    s.player = player;
    s.holder = 5000;
    s.slot = KILN_SLOT_ID(page, index);
    s.gem = gem;
    s.item = 0x1234;
    s.level = 3;
    s.lifetime_steps = 100;
    return s;
}

void sources() {
    uint64_t next = 0x1000;
    auto rng = [&] { return next++; };
    SourceBook b;
    const char* why = "";
    KilnGemSource a[2] = {src(1, 5001, 2, 900), src(1, 5001, 3, 901)};
    check(!b.used() && b.offer(7, a, 2, 50, rng, &why) && b.used(), "sources: an offer of two gems is taken; the book is in use");
    check(a[0].token == 0x1000 && a[1].token == 0x1001 && a[0].expires_step == 150, "sources: each gets a token and its expiry");
    const SourceEntry* e = b.find(1, KILN_SLOT_ID(5001, 2), 120);
    check(e && e->s.gem == 900 && e->owner == 7, "sources: the player's request naming the slot finds its offer");
    check(!b.find(2, KILN_SLOT_ID(5001, 2), 120), "sources: another player naming the same slot finds nothing (forged)");
    check(!b.find(1, KILN_SLOT_ID(5001, 4), 120), "sources: a slot never offered finds nothing (forged)");
    check(!b.find(1, KILN_SLOT_ID(5001, 2), 151), "sources: an expired offer finds nothing (stale)");
    check(b.names_page(1, 5001) && !b.names_page(1, 5002) && !b.names_page(2, 5001), "sources: the pages offered to a player are known");
    check(foreign_verdict(KILN_GEM_ACTION_SLOT, e) == Foreign::Offered && foreign_verdict(KILN_GEM_ACTION_UPGRADE, e) == Foreign::Offered &&
              foreign_verdict(KILN_GEM_ACTION_RECYCLE, e) == Foreign::Offered, "sources: Insert, Upgrade and Recycle may use an offer");
    check(foreign_verdict(KILN_GEM_ACTION_UNSLOT, e) == Foreign::Refuse && foreign_verdict(KILN_GEM_ACTION_SLOT, nullptr) == Foreign::Refuse,
          "sources: an Unslot naming a foreign slot, or no offer, is refused");
    const uint64_t t = e->s.token;
    check(b.take(t) && !b.find(1, KILN_SLOT_ID(5001, 2), 120) && !b.take(t), "sources: an offer is used once (a duplicate request finds nothing)");
    // a newer offer of the same slot replaces the old one, with a new token
    KilnGemSource again = src(1, 5001, 3, 901);
    check(b.offer(7, &again, 1, 60, rng, &why) && b.size() == 1 && again.token != a[1].token &&
              b.find(1, KILN_SLOT_ID(5001, 3), 61)->s.token == again.token, "sources: offering a slot again replaces its offer and token");
    // all or nothing
    KilnGemSource bad[2] = {src(1, 5001, 5, 902), src(1, 5001, 6, 0)};
    check(!b.offer(7, bad, 2, 60, rng, &why) && b.size() == 1, "sources: one bad entry refuses the whole offer");
    KilnGemSource s = src(1, 5001, 1, 903);
    s.holder = 1;
    check(!source_valid(s), "sources: the player as the holder is refused (a container only)");
    s = src(1, 5001, 1, 903);
    s.slot = KILN_SLOT_ID(5001, 8);
    check(!source_valid(s), "sources: slot index 8 is refused");
    s = src(1, 5001, 1, 903);
    s.lifetime_steps = kMaxSourceLifetime + 1;
    check(!source_valid(s), "sources: a lifetime over 3000 steps is refused");
    s = src(1, 5001, 1, 903);
    s.pay_from = KILN_GEM_PAY_HOLDER;
    check(!source_valid(s), "sources: KILN_GEM_PAY_HOLDER needs its container");
    s.pay_holder = 5000;
    check(source_valid(s), "sources: a paying container is fine");
    // limits
    std::vector<KilnGemSource> many;
    for (uint32_t i = 0; i < kMaxSourcesPerPlayer + 1; ++i) many.push_back(src(3, 6000 + i / 8, i % 8, 2000 + i));
    check(!b.offer(7, many.data(), static_cast<uint32_t>(many.size()), 60, rng, &why), "sources: more than 256 live offers to one player are refused");
    check(b.offer(7, many.data(), kMaxSourcesPerPlayer, 60, rng, &why), "sources: 256 to one player are fine");
    // a broken generator (always 0 or repeating): no offer without a fresh token
    SourceBook z;
    auto zero = [] { return uint64_t{0}; };
    KilnGemSource zs = src(1, 5001, 1, 904);
    check(!z.offer(7, &zs, 1, 60, zero, &why), "sources: a generator that only gives 0 offers nothing");
    // withdraw, owners, expiry
    KilnGemSource o8 = src(1, 7001, 0, 905);
    b.offer(8, &o8, 1, 60, rng, &why);
    check(b.withdraw(7, 3, 0) == kMaxSourcesPerPlayer && b.find(1, KILN_SLOT_ID(7001, 0), 61), "sources: withdraw takes one owner's offers to one player");
    b.drop_owner(8);
    check(!b.find(1, KILN_SLOT_ID(7001, 0), 61), "sources: a mod's offers go with the mod");
    b.expire(60 + 100 + 10, 5);
    check(b.size() == 0, "sources: offers past their expiry (and the grace) are dropped");
    uint32_t live = b.live_for(1, 0, [](const KilnGemSource&) {});
    check(live == 0 && b.used(), "sources: an empty book stays in use for the world (forged requests stay refused)");
    b.clear();
    check(!b.used(), "sources: a new world starts unused");

    // own or foreign: the player's pages, a socket of the player's carrier
    const std::vector<uint32_t> pages{2, 6590, 6591, 6576};
    check(own_gem_slot(KILN_SLOT_ID(6591, 4), 0, false, pages), "foreign: a slot of the player's page is the player's own");
    check(own_gem_slot(KILN_SLOT_ID(6576, 0), 0, false, pages), "foreign: a worn backpack's page is the player's own");
    check(!own_gem_slot(KILN_SLOT_ID(5001, 2), 0, false, pages), "foreign: a chest's slot is not");
    check(own_gem_slot(KILN_SLOT_ID(2001, 0), 2001, true, pages), "foreign: the socket of a carrier in the player's pages is the player's own");
    check(!own_gem_slot(KILN_SLOT_ID(2001, 0), 2001, false, pages), "foreign: the socket of a carrier elsewhere (a chest's weapon) is not");
    check(own_gem_slot(0, 2001, false, pages), "foreign: no slot (Unslot) is judged by the carrier, not here");
}

int main() {
    requests();
    layout();
    hold_book();
    judging();
    prechecks();
    costs();
    gifts();
    setup_block();
    backpack_room();
    holders_and_payers();
    sources();
    std::printf("%s: %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
