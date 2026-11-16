// SPDX-License-Identifier: MIT
// kiln.gemcraft host test (gem wave G3): the forge actions' logic without game memory (gems_logic.h): request checks,
// equipment_actions' declaration against the handler's iterator layout, the hold book of players' actions, what a
// snapshot before and after says an action did, the prechecks, and a free or replaced cost around the game's action.
#include <cstdio>
#include <cstring>

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
    g.level_known = true;
    g.level = 10;
    g.cap = 105;
    check(precheck(KILN_GEM_ACTION_UPGRADE, g, 10, 0, 0, true, &why) == KILN_GEM_MAX_LEVEL, "precheck: Upgrade at the forge's maximum (10)");
    check(precheck(KILN_GEM_ACTION_UPGRADE, g, 15, 30, 30, true, &why) == KILN_GEM_DONE, "precheck: Upgrade below the forge's maximum");
    g.cap = 10;
    check(precheck(KILN_GEM_ACTION_UPGRADE, g, 50, 0, 0, true, &why) == KILN_GEM_MAX_LEVEL, "precheck: Upgrade at the gem's own cap");
    Snapshot r;
    check(precheck(KILN_GEM_ACTION_RECYCLE, r, 10, 0, 0, true, &why) == KILN_GEM_NOT_FOUND, "precheck: Recycle without a gem");
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

int main() {
    requests();
    layout();
    hold_book();
    judging();
    prechecks();
    costs();
    std::printf("%s: %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
