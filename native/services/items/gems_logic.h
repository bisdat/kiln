// SPDX-License-Identifier: MIT
// kiln.gemcraft (gem wave G3): the parts of the forge actions that need no game memory, so the host test can run them:
// request checks, the handler's iterator layout and its check against equipment_actions' declaration, the hold book of
// players' actions, what a snapshot before and after an action says happened, and what a free or replaced cost needs
// around the game's action. docs/re/gems-forge.md; the ABI is native/core/abi/services/kiln_gemcraft.h.
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "services/kiln_gemcraft.h"
#include "services/kiln_items.h"

namespace kitems::gems {

constexpr uint32_t kMaxCostCount = 100000;
constexpr uint32_t kHoldTimeoutSteps = 8;  // a held action nobody answered runs after this many steps
constexpr uint32_t kMaxHoldSteps = 250;
constexpr uint32_t kWeaponGemCategory = 15;  // ItemCategory WeaponGem

// ---- requests ----
// A caller's request as Kiln keeps it: the caller's bytes (at most sizeof), the appended fields 0 when its size does not
// cover them (a 0.2.0-dev.29.11..14 caller). False: shorter than the first version (nothing is read past `size`).
inline bool normalize_request(const KilnGemRequest* in, KilnGemRequest* out) {
    std::memset(out, 0, sizeof *out);
    if (!in || in->size < KILN_GEM_REQUEST_V1_SIZE) return false;
    std::memcpy(out, in, in->size < sizeof *out ? in->size : sizeof *out);
    out->size = sizeof *out;
    return true;
}

// The request needs Kiln's routing of the handler's payment or return (gems.payers): a payer or a return that is not the
// player. A free cost pays nothing, so its pay_from routes nothing.
inline bool request_routed(const KilnGemRequest& r) {
    const bool pays = r.action != KILN_GEM_ACTION_RECYCLE && !(r.flags & (KILN_GEM_FREE | KILN_GEM_COST));
    return (pays && r.pay_from != KILN_GEM_PAY_PLAYER) || (r.return_to != 0 && r.return_to != r.player);
}

inline bool request_valid(const KilnGemRequest& r) {
    if (r.size < sizeof(KilnGemRequest) || r.reserved[0] || r.reserved[1] || r.reserved2 || r.player == 0) return false;
    if (r.action < KILN_GEM_ACTION_SLOT || r.action > KILN_GEM_ACTION_RECYCLE) return false;
    if (r.flags & ~(KILN_GEM_FREE | KILN_GEM_COST | KILN_GEM_RETURN)) return false;
    if (r.pay_from > KILN_GEM_PAY_PLAYER_THEN_BASE || ((r.pay_from == KILN_GEM_PAY_HOLDER) != (r.pay_holder != 0)))
        return false;
    if (r.flags & KILN_GEM_RETURN)  // the forgeless return: alone, an Unslot of a carrier in the player's own inventories
        return r.flags == KILN_GEM_RETURN && r.action == KILN_GEM_ACTION_UNSLOT && r.carrier != 0 && r.gem_slot == 0 &&
               r.forge == 0 && r.holder == 0 && r.cost_item == 0 && r.cost_count == 0 && r.gem_holder == 0 &&
               r.carrier_holder == 0 && r.pay_from == 0 && r.pay_holder == 0 && r.return_to == 0;
    if ((r.flags & KILN_GEM_FREE) && (r.flags & KILN_GEM_COST)) return false;
    if (r.flags & KILN_GEM_COST) {
        if (r.cost_count > kMaxCostCount || (r.cost_count != 0 && r.cost_item == 0)) return false;
        // a replaced price is taken from the player by item id; only the game's own price goes to other payers
        if (r.action != KILN_GEM_ACTION_RECYCLE && r.pay_from != KILN_GEM_PAY_PLAYER) return false;
    } else if (r.cost_item != 0 || r.cost_count != 0) {
        return false;
    }
    if (r.gem_slot && ((r.gem_slot >> 40) != 0 || KILN_SLOT_INDEX(r.gem_slot) > 7 || KILN_SLOT_INVENTORY(r.gem_slot) == 0))
        return false;
    switch (r.action) {
        case KILN_GEM_ACTION_SLOT: return r.carrier != 0 && r.gem_slot != 0;
        case KILN_GEM_ACTION_UNSLOT: return r.carrier != 0 && r.gem_slot == 0;
        default: return (r.carrier != 0) != (r.gem_slot != 0);  // a socketed gem (carrier) or a loose one (slot)
    }
}

// ---- the handler's iterator: equipment_actions' unpacked iterator (0xf0 bytes), as the handler reads it ----
// [0] ctx | [1] PlayerInput | [2..7] Equipment write record | [8..13] ServerConsumedPlayerInput write record |
// [14..17] BalancingRegistry, ItemRegistry, RecipeRegistry, AttributeRegistry | [18] KnowledgeTriggerQueue |
// [19] ShroudForge template accessor | [20..27] Inventory, InventoryEmptyState, InventorySetup, ItemState, Level,
// OwnerRelationship, SlotSelection, WeaponGemSlot | [28] GemCraftingEvent sender | [29] InventoryFull sender
enum Slot : int {
    I_CTX = 0, I_INPUT = 1, I_EQUIPMENT = 2, I_CONSUMED = 8, I_BALANCING = 14, I_ITEMS = 15, I_RECIPES = 16,
    I_ATTRIBUTES = 17, I_KNOWLEDGE = 18, I_FORGE_TPL = 19, I_INVENTORY = 20, I_EMPTY = 21, I_SETUP = 22, I_ITEMSTATE = 23,
    I_LEVEL = 24, I_OWNER = 25, I_SLOTSEL = 26, I_GEMSLOT = 27, I_EV_GEM = 28, I_EV_FULL = 29, I_COUNT = 30,
};
constexpr uint32_t kIterBytes = I_COUNT * 8;
constexpr uint32_t kWriteRecordQwords = 6;
constexpr uint32_t kWriteTrackerQword = 5;  // record +0x28: the tracker (0: the write is not marked)

// The handler's other-entity accessors in iterator order (type index order = name order).
inline const char* const kOtherWrite[8] = {"Inventory", "InventoryEmptyState", "InventorySetup", "ItemState",
                                           "Level", "OwnerRelationship", "SlotSelection", "WeaponGemSlot"};

// equipment_actions' declaration, list by list (KILN_GAME_LIST_* order), as the iterator above needs it. `sorted`: the
// list's iterator order is by type index, so only the set matters.
struct ListWant {
    int list;
    std::vector<std::string> names;
    bool sorted;
};
inline std::vector<ListWant> expected_lists() {
    return {
        {0, {}, true}, {1, {}, true},  // tpl, tpl optional
        {2, {"PlayerInput"}, true}, {3, {}, true},
        {4, {"Equipment", "ServerConsumedPlayerInput"}, true}, {5, {}, true},
        {6, {"BalancingRegistry", "g38_items::ItemRegistry", "g38_items::RecipeRegistry", "impact::AttributeRegistry"}, false},
        {7, {"KnowledgeTriggerQueue"}, false},
        {8, {"ShroudForge"}, true}, {9, {}, true},
        {10, {kOtherWrite[0], kOtherWrite[1], kOtherWrite[2], kOtherWrite[3], kOtherWrite[4], kOtherWrite[5], kOtherWrite[6],
              kOtherWrite[7]}, true},
        {14, {}, true},
        {15, {"GemCraftingEvent", "InventoryFull"}, true},
    };
}

// The declaration read from the game's directory (each list's names in descriptor order; "keen::ecs::" stripped) matches
// what the handler's iterator layout assumes. False with why[] otherwise.
inline bool lists_match(const std::vector<std::vector<std::string>>& lists, char* why, size_t cap) {
    auto strip = [](std::string s) {
        static const char p[] = "keen::ecs::";
        if (s.compare(0, sizeof p - 1, p) == 0) s.erase(0, sizeof p - 1);
        return s;
    };
    for (const ListWant& w : expected_lists()) {
        if (w.list < 0 || static_cast<size_t>(w.list) >= lists.size()) {
            std::snprintf(why, cap, "equipment_actions' list %d is missing", w.list);
            return false;
        }
        std::vector<std::string> got;
        for (const std::string& n : lists[static_cast<size_t>(w.list)]) got.push_back(strip(n));
        std::vector<std::string> want = w.names;
        if (w.sorted) {
            std::sort(got.begin(), got.end());
            std::sort(want.begin(), want.end());
        }
        if (got != want) {
            std::string g;
            for (const std::string& n : got) g += (g.empty() ? "" : ",") + n;
            std::snprintf(why, cap, "equipment_actions' list %d is [%s], not the one Kiln builds the handler's iterator for",
                          w.list, g.c_str());
            return false;
        }
    }
    return true;
}

// ---- the hold book: players' actions held for a decision (one per player: a newer version replaces an older one) ----
enum class Verdict : uint8_t { None, Allow, Cancel };
struct Held {
    uint32_t player = 0, version = 0;
    uint64_t since = 0;        // the step it was first held in
    bool reported = false;     // handed to held()
    Verdict verdict = Verdict::None;
    KilnGemCraft info{};       // what it would do, read when it was first held
};

class HoldBook {
public:
    // Hook side, for a player's unconsumed action `version` in step `step`: what to do now.
    // Hold: keep it (first time: recorded with `info`). Run / Cancel: the verdict (the entry goes).
    // `hold_new` false (nobody holds now): nothing new is held or recorded; a verdict already given still stands (a
    // cancel stays a cancel when the handler's mod stopped listening or holding lapsed before the hook saw the answer).
    // A version that already ran and comes back unconsumed (the game's handler leaves it so when the gem has no readable
    // Level; another hook in the chain may too) runs again without a new hold: one GemCraftingEvent per action, no loop.
    enum class Now : uint8_t { Hold, Run, Cancel };
    Now on_action(uint32_t player, uint32_t version, uint64_t step, const KilnGemCraft* info, bool hold_new = true) {
        for (Ran& r : ran_)
            if (r.player == player && r.version == version) {
                r.seen = step;
                return Now::Run;
            }
        for (size_t i = 0; i < held_.size(); ++i) {
            Held& h = held_[i];
            if (h.player != player) continue;
            if (h.version != version) {  // a newer action replaces the older one (the game only sees the newest)
                held_.erase(held_.begin() + static_cast<ptrdiff_t>(i));
                break;
            }
            if (h.verdict == Verdict::Allow || (h.verdict == Verdict::None && (!hold_new || step >= h.since + kHoldTimeoutSteps))) {
                timed_out_ += h.verdict == Verdict::None && hold_new;
                held_.erase(held_.begin() + static_cast<ptrdiff_t>(i));
                remember(player, version, step);
                return Now::Run;
            }
            if (h.verdict == Verdict::Cancel) {  // the forge-0 call always consumes it: nothing to remember
                held_.erase(held_.begin() + static_cast<ptrdiff_t>(i));
                return Now::Cancel;
            }
            return Now::Hold;
        }
        if (!hold_new) return Now::Run;
        Held h;
        h.player = player;
        h.version = version;
        h.since = step;
        if (info) h.info = *info;
        held_.push_back(h);
        ++total_;
        return Now::Hold;
    }
    // Tick side: the held actions not reported yet (each once). One that already has a verdict (holding stopped and
    // released it) is not handed out: a handler could no longer cancel it.
    template <class F>
    uint32_t report(F&& f, uint32_t max) {
        uint32_t n = 0;
        for (Held& h : held_) {
            if (h.reported || n >= max) continue;
            h.reported = true;
            if (h.verdict != Verdict::None) continue;
            f(h);
            ++n;
        }
        return n;
    }
    bool waiting_reports() const {
        for (const Held& h : held_)
            if (!h.reported && h.verdict == Verdict::None) return true;
        return false;
    }
    // Tick side: the answer. False: no such held action.
    bool decide(uint32_t player, uint32_t version, bool cancel) {
        for (Held& h : held_)
            if (h.player == player && h.version == version && h.verdict == Verdict::None) {
                h.verdict = cancel ? Verdict::Cancel : Verdict::Allow;
                canceled_ += cancel;
                return true;
            }
        return false;
    }
    // Holding stopped: everything held runs (its verdict becomes Allow unless it was canceled).
    void release_all() {
        for (Held& h : held_)
            if (h.verdict == Verdict::None) h.verdict = Verdict::Allow;
    }
    // Entries nobody's hook asked about for a long time (the player left): dropped; so is the memory of a version that
    // ran and has not come back for as long.
    void expire(uint64_t step, uint64_t age) {
        for (size_t i = held_.size(); i-- > 0;)
            if (step > held_[i].since + age) held_.erase(held_.begin() + static_cast<ptrdiff_t>(i));
        for (size_t i = ran_.size(); i-- > 0;)
            if (step > ran_[i].seen + age) ran_.erase(ran_.begin() + static_cast<ptrdiff_t>(i));
    }
    void clear() {
        held_.clear();
        ran_.clear();
    }
    // Entries the hook must look at even while nothing is held (a verdict waiting, a version that ran): any thread reads
    // this through the owner's atomic copy.
    size_t size() const { return held_.size(); }
    size_t remembered() const { return ran_.size(); }
    uint32_t total() const { return total_; }
    uint32_t canceled() const { return canceled_; }
    uint32_t timed_out() const { return timed_out_; }

private:
    struct Ran {
        uint32_t player, version;
        uint64_t seen;
    };
    // one per player: its last version that ran
    void remember(uint32_t player, uint32_t version, uint64_t step) {
        for (Ran& r : ran_)
            if (r.player == player) {
                r.version = version;
                r.seen = step;
                return;
            }
        ran_.push_back({player, version, step});
    }
    std::vector<Held> held_;
    std::vector<Ran> ran_;
    uint32_t total_ = 0, canceled_ = 0, timed_out_ = 0;
};

// ---- what an action did ----
// The state an action is judged by, read right before and right after it (through the handler's accessors).
struct Snapshot {
    bool socket = false;          // the carrier has a WeaponGemSlot
    uint32_t socket_item = 0, socket_pide = 0;
    uint32_t source_item = 0, source_count = 0, source_pide = 0;  // the gem slot's stack (Slot, a loose gem)
    uint32_t gem_pide = 0;        // the gem the action works on (0: none found)
    bool level_known = false;
    uint32_t level = 0, cap = 0;
    uint32_t material = 0;        // count of the cost item in the player's inventories
    uint32_t essence = 0;         // count of the yield item in the player's inventories
    bool gem_in_player = false;   // the gem's entity is in one of the player's inventories
    uint8_t gem_category = 0xFF;  // the gem's ItemInfo category (0xFF: not read); a gem is kWeaponGemCategory
};

struct Outcome {
    int32_t status = KILN_GEM_REFUSED;
    uint32_t level_after = 0;
    uint32_t paid = 0;      // material: before - after
    uint32_t got = 0;       // essence: after - before
};

inline Outcome judge(uint32_t action, const Snapshot& b, const Snapshot& a) {
    Outcome o;
    o.level_after = a.level_known ? a.level : b.level;
    o.paid = b.material > a.material ? b.material - a.material : 0;
    o.got = a.essence > b.essence ? a.essence - b.essence : 0;
    bool done = false;
    switch (action) {
        case KILN_GEM_ACTION_SLOT:
            done = b.source_pide != 0 && a.socket && a.socket_pide == b.source_pide && a.socket_item == b.source_item;
            break;
        case KILN_GEM_ACTION_UNSLOT:
            done = b.socket_pide != 0 && a.socket && a.socket_pide == 0 && a.gem_in_player;
            break;
        case KILN_GEM_ACTION_UPGRADE:
            done = b.level_known && a.level_known && a.level == b.level + 1;
            break;
        case KILN_GEM_ACTION_RECYCLE:
            done = b.gem_pide != 0 && a.gem_pide != b.gem_pide;  // the slot may hold the essence now
            if (done) o.level_after = 0;
            break;
        default: break;
    }
    o.status = done ? KILN_GEM_DONE : KILN_GEM_REFUSED;
    return o;
}

// Unslot's own checks (the forge's Unslot and the forgeless return): a socket that holds something, room for it.
inline int32_t precheck_unslot(const Snapshot& b, bool backpack_room, const char** why) {
    if (!b.socket) return *why = "the carrier has no gem socket", KILN_GEM_NOT_FOUND;
    if (b.socket_pide == 0) return *why = "the socket is empty", KILN_GEM_OCCUPIED;
    if (!backpack_room) return *why = "no free backpack slot for the gem", KILN_GEM_NO_ROOM;
    return KILN_GEM_DONE;
}

// Why an action would be refused before the game runs it (KILN_GEM_DONE: nothing Kiln can see stops it). The game's
// own checks still run; these give the caller a reason instead of "refused". The game's handler takes any stack with an
// item entity as "the gem" (it reads no category: Recycle would destroy a sword, Upgrade raise its Level), so Slot,
// Upgrade and Recycle need a WeaponGem, and an item whose category cannot be read is refused.
inline int32_t precheck(uint32_t action, const Snapshot& b, uint32_t forge_max, uint32_t cost, uint32_t pays_from_have,
                        bool backpack_room, const char** why) {
    *why = "";
    if (forge_max == 0) return *why = "the forge entity is no Shroud Forge (no template, or maxGemLevel 0)", KILN_GEM_NO_FORGE;
    const char* not_gem = "the item is not a gem (or its category cannot be read)";
    switch (action) {
        case KILN_GEM_ACTION_SLOT:
            if (!b.socket) return *why = "the carrier has no gem socket", KILN_GEM_NOT_FOUND;
            if (b.socket_pide != 0) return *why = "the socket holds a gem already", KILN_GEM_OCCUPIED;
            if (b.source_pide == 0) return *why = "the gem slot holds no gem", KILN_GEM_NOT_FOUND;
            if (b.gem_category != kWeaponGemCategory) return *why = not_gem, KILN_GEM_NOT_FOUND;
            break;
        case KILN_GEM_ACTION_UNSLOT: {
            const int32_t st = precheck_unslot(b, backpack_room, why);
            if (st != KILN_GEM_DONE) return st;
            break;
        }
        case KILN_GEM_ACTION_UPGRADE:
            if (b.gem_pide == 0) return *why = "no gem there", KILN_GEM_NOT_FOUND;
            if (b.gem_category != kWeaponGemCategory) return *why = not_gem, KILN_GEM_NOT_FOUND;
            if (!b.level_known) return *why = "the gem's level cannot be read", KILN_GEM_NOT_FOUND;
            if (b.level >= b.cap || b.level >= forge_max) return *why = "the gem is at its cap or the forge's maximum level", KILN_GEM_MAX_LEVEL;
            break;
        case KILN_GEM_ACTION_RECYCLE:
            if (b.gem_pide == 0) return *why = "no gem there", KILN_GEM_NOT_FOUND;
            if (b.gem_category != kWeaponGemCategory) return *why = not_gem, KILN_GEM_NOT_FOUND;
            break;
        default: return *why = "unknown action", KILN_GEM_NOT_FOUND;
    }
    if (cost > pays_from_have) return *why = "the player cannot pay the forge's cost", KILN_GEM_NOT_ENOUGH;
    return KILN_GEM_DONE;
}

// ---- room for an unslotted gem ----
// Is there a free slot where the game's unslot puts the gem (gems/UnslotMove: the transaction's add into the player's
// inventories, 0x1b4d10)? Its Generic (4) pages: the ones a player's InventorySetup links directly, and the ones behind a
// Virtual (5) link, another InventorySetup (a worn backpack's pages), followed up to 3 levels deep as kiln.items' page walk
// does (engine.cpp walk, what Items.Read lists). Equipment, Currency and other categories never take a gem. A slot is
// free when its stack has no item or no count. `setup` is the player's InventorySetup (84 bytes: 16 u32 links, then 16
// u8 link categories at +0x40); `setup_of(entity)` gives another entity's InventorySetup and `inventory_of(entity)` an
// Inventory (8 x ItemStack {u32 item, u32 count, u32 pide}), each nullptr when unreadable. Kiln's precheck only: the
// game's own add still decides (the handler commits only when everything fit), and a return is judged afterwards.
// Owner's client 2026-11-16: a backpack behind a Virtual link was skipped, so a player with free backpack slots and full
// top-level pages was told "no free backpack slot".
template <class SetupOf, class InventoryOf>
bool generic_room(const uint8_t* setup, SetupOf setup_of, InventoryOf inventory_of, int depth = 0) {
    for (int k = 0; k < 16; ++k) {
        uint32_t link = 0;
        std::memcpy(&link, setup + 4 * k, 4);
        if (link == 0) break;
        const uint8_t category = setup[0x40 + k];
        if (category == 5) {
            if (depth < 3)
                if (const uint8_t* inner = setup_of(link))
                    if (generic_room(inner, setup_of, inventory_of, depth + 1)) return true;
            continue;
        }
        if (category != 4) continue;
        if (const uint8_t* inv = inventory_of(link))
            for (uint32_t j = 0; j < 8; ++j) {
                uint32_t item = 0, count = 0;
                std::memcpy(&item, inv + 12 * j, 4);
                std::memcpy(&count, inv + 12 * j + 4, 4);
                if (item == 0 || count == 0) return true;
            }
    }
    return false;
}

// KILN_GEM_RETURN: no forge, no cost; the socket and the room as for Unslot.
inline int32_t precheck_return(const Snapshot& b, bool backpack_room, const char** why) {
    *why = "";
    return precheck_unslot(b, backpack_room, why);
}

// ---- a free or replaced cost around the game's action ----
// Before: give the player the game's cost `game_cost` of `material` (the game takes it back) and take the replacement.
// After a Recycle: take the game's yield and give the replacement. After a refusal: undo what "before" did.
struct CostPlan {
    uint32_t give_material = 0;           // before the action: the game's cost, given (KILN_GEM_FREE / COST, not Recycle)
    KilnItemId take_item = 0;             // before the action: the replacement, taken (KILN_GEM_COST, not Recycle)
    uint32_t take_count = 0;
    bool replace_yield = false;           // Recycle with FREE / COST: after the action, the game's yield goes
    KilnItemId yield_item = 0;            // ... and this is given instead (0 x 0: nothing)
    uint32_t yield_count = 0;
};
inline CostPlan plan_cost(const KilnGemRequest& r, uint32_t game_cost) {
    CostPlan p;
    if (!(r.flags & (KILN_GEM_FREE | KILN_GEM_COST))) return p;
    if (r.action == KILN_GEM_ACTION_RECYCLE) {
        p.replace_yield = true;
        if (r.flags & KILN_GEM_COST) {
            p.yield_item = r.cost_item;
            p.yield_count = r.cost_count;
        }
        return p;
    }
    p.give_material = game_cost;
    if ((r.flags & KILN_GEM_COST) && r.cost_count) {
        p.take_item = r.cost_item;
        p.take_count = r.cost_count;
    }
    return p;
}

// What is left of the game's price Kiln handed over (`give`, for a free or replaced cost) once the game's action ran:
// the material count right before the handler (`before`, the gift included) and right after it (`after`). The game
// takes its price or nothing (one transaction), but Upgrade commits the payment before it raises the Level, and a
// commit can roll back by itself: so the take-back is measured, never assumed. 0: the game took all of it.
inline uint32_t gift_left(uint32_t give, uint32_t before, uint32_t after) {
    if (!give) return 0;
    const uint32_t without = before >= give ? before - give : 0;  // the count without the gift
    if (after <= without) return 0;
    const uint32_t left = after - without;
    return left > give ? give : left;
}

// ---- the forgeless return (KILN_GEM_RETURN): the InventoryTx setup block the gem handler builds ----
// Server 0x144c1c..0x144d03, client 0x3858ec..0x3859d3 (docs/re/gems-forge.md): flags 4 (InventoryFull to the
// operator), the iterator's context, ItemRegistry, BalancingRegistry (no AttributeRegistry), the seven inventory
// accessors, WeaponGemSlot at +0x60 (so a socket {carrier, 0} is a slot of the transaction), the InventoryFull sender, the
// player as operator; everything else zero. `h` is the handler's iterator (I_* layout), `operator_off` the setup's
// operator offset (sigdb items/setup.operator, 0xC8).
inline void handler_setup(const uint64_t* h, uint32_t player, uint32_t operator_off, uint8_t* s) {
    std::memset(s, 0, 0xF0);
    s[0x00] = 0x04;
    auto put = [s](int off, uint64_t v) { std::memcpy(s + off, &v, 8); };
    put(0x08, h[I_CTX]);
    put(0x10, h[I_ITEMS]);
    put(0x18, h[I_BALANCING]);
    put(0x28, h[I_SETUP]);
    put(0x30, h[I_INVENTORY]);
    put(0x38, h[I_SLOTSEL]);
    put(0x40, h[I_ITEMSTATE]);
    put(0x48, h[I_OWNER]);
    put(0x50, h[I_SETUP]);
    put(0x58, h[I_EMPTY]);
    put(0x60, h[I_GEMSLOT]);
    put(0xA8, h[I_EV_FULL]);
    if (operator_off + 4 <= 0xF0) std::memcpy(s + operator_off, &player, 4);
}

// ---- payers (gems.payers): the handler's payment spread over several inventories on its own transaction ----
// The game's payment is one removeItems(owner = the player, material, cost) on the action's transaction; with payers Kiln
// takes from the payers in order: the first ones itself (removeItems on the same transaction, before the game's call),
// and the game's own call takes the rest from the last payer that has some (its missing count then decides, as the
// game's does: anything missing and the handler rolls the whole transaction back). `have` is each payer's count of the
// material, read right before the handler runs.
struct PayStep {
    uint32_t payer = 0;  // entity
    uint32_t take = 0;
};
struct PayPlan {
    std::vector<PayStep> before;  // Kiln's own removeItems, in order
    uint32_t last = 0;            // the payer the game's call takes from
    uint32_t last_take = 0;       // what the game's call asks for (the rest; with a shortfall the missing part too)
    uint32_t short_by = 0;        // what no payer has
};
inline PayPlan plan_payment(const std::vector<std::pair<uint32_t, uint32_t>>& payers, uint32_t cost) {
    PayPlan p;
    if (payers.empty()) return p;
    uint32_t left = cost;
    std::vector<PayStep> steps;
    for (const auto& [payer, have] : payers) {
        if (!left) break;
        const uint32_t take = have < left ? have : left;
        if (take) steps.push_back({payer, take});
        left -= take;
    }
    p.short_by = left;
    if (steps.empty()) {  // nobody has any: the game's call asks the first payer for all of it and finds it missing
        p.last = payers.front().first;
        p.last_take = cost;
        return p;
    }
    p.last = steps.back().payer;
    p.last_take = steps.back().take + left;
    steps.pop_back();
    p.before = std::move(steps);
    return p;
}
inline uint64_t payers_have(const std::vector<std::pair<uint32_t, uint32_t>>& payers) {
    uint64_t n = 0;
    for (const auto& pr : payers) n += pr.second;
    return n;
}

// ---- gem sources: offers of one gem in a container to one player, found again by the player's request ----
constexpr uint32_t kMaxSourcesPerPlayer = 256;
constexpr uint32_t kMaxSources = 4096;
constexpr uint32_t kMaxSourceLifetimeMs = 120000;  // 2 minutes, real time (GetTickCount64)
constexpr uint32_t kNoSession = 0xFFFFFFFFu;        // session_of(): the entity is a player slot nobody is in game in

inline bool source_valid(const KilnGemSource& s) {
    return s.size >= sizeof(KilnGemSource) && s.player != 0 && s.holder != 0 && s.holder != s.player && s.gem != 0 &&
           s.item != 0 && s.slot != 0 && (s.slot >> 40) == 0 && KILN_SLOT_INDEX(s.slot) <= 7 &&
           KILN_SLOT_INVENTORY(s.slot) != 0 && s.pay_from <= KILN_GEM_PAY_PLAYER_THEN_BASE &&
           ((s.pay_from == KILN_GEM_PAY_HOLDER) == (s.pay_holder != 0)) && s.lifetime_ms >= 1 &&
           s.lifetime_ms <= kMaxSourceLifetimeMs && s.reserved == 0;
}

struct SourceEntry {
    KilnOwner owner = 0;
    uint32_t session = 0;  // the player's session when it was offered (kiln.players' handle; 0: not a player slot)
    KilnGemSource s{};
};

// The book of live offers. `token` is Kiln's random source (tests pass their own); a token is never 0 and never repeats in
// the book. Tick thread (the owner holds the service's lock around every call).
class SourceBook {
public:
    // All or nothing: false (with why) and no change when an entry is bad, a player is not in game (session_of gives
    // kNoSession) or would hold too many. `now` is real time in ms; each offer keeps its player's session.
    template <class SessionOf, class Rng>
    bool offer(KilnOwner owner, KilnGemSource* in, uint32_t count, uint64_t now, SessionOf&& session_of, Rng&& rng, const char** why) {
        *why = "";
        for (uint32_t i = 0; i < count; ++i)
            if (!source_valid(in[i])) return *why = "an offered gem is not valid (size, player, holder, slot, gem, item, payer, lifetime)", false;
        std::vector<std::pair<uint32_t, uint32_t>> sessions;  // player, session
        for (uint32_t i = 0; i < count; ++i) {
            bool seen = false;
            for (auto& pr : sessions) seen |= pr.first == in[i].player;
            if (seen) continue;
            const uint32_t sn = session_of(in[i].player);
            if (sn == kNoSession) return *why = "a player the gems are offered to is not in game", false;
            sessions.push_back({in[i].player, sn});
        }
        // the count per player after replacing the same slots
        std::vector<std::pair<uint32_t, uint32_t>> per;  // player, live after
        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t pl = in[i].player;
            bool seen = false;
            for (auto& pr : per) seen |= pr.first == pl;
            if (seen) continue;
            uint32_t n = 0;
            for (const SourceEntry& e : book_)
                if (e.s.player == pl) {
                    bool replaced = false;
                    for (uint32_t j = 0; j < count; ++j) replaced |= in[j].player == pl && in[j].slot == e.s.slot;
                    n += !replaced;
                }
            for (uint32_t j = 0; j < count; ++j)
                if (in[j].player == pl) {
                    bool dup = false;
                    for (uint32_t k = 0; k < j; ++k) dup |= in[k].player == pl && in[k].slot == in[j].slot;
                    n += !dup;
                }
            per.push_back({pl, n});
        }
        size_t others = 0, total = 0;  // the book's entries of players not in this call, plus every player's count after
        for (const SourceEntry& e : book_) {
            bool listed = false;
            for (auto& pr : per) listed |= pr.first == e.s.player;
            others += !listed;
        }
        total = others;
        for (auto& pr : per) {
            if (pr.second > kMaxSourcesPerPlayer) return *why = "a player would hold more than 256 offered gems", false;
            total += pr.second;
        }
        if (total > kMaxSources) return *why = "more than 4096 offered gems would be live", false;
        for (uint32_t i = 0; i < count; ++i) {
            KilnGemSource& s = in[i];
            for (size_t k = book_.size(); k-- > 0;)
                if (book_[k].s.player == s.player && book_[k].s.slot == s.slot) book_.erase(book_.begin() + static_cast<ptrdiff_t>(k));
            uint64_t t = 0;
            for (int tries = 0; tries < 16 && (t == 0 || find_token(t)); ++tries) t = rng();
            if (t == 0 || find_token(t)) return *why = "no fresh token", false;  // a broken generator: nothing more offered
            s.token = t;
            s.expires_ms = now + s.lifetime_ms;
            SourceEntry e;
            e.owner = owner;
            for (auto& pr : sessions)
                if (pr.first == s.player) e.session = pr.second;
            e.s = s;
            book_.push_back(e);
        }
        used_ = true;
        return true;
    }
    // The live offer for a player's request naming `slot`, made in the player's current `session`; nullptr when none,
    // expired at `now` (ms), or made to an earlier session of the entity (the player left; another may hold the slot).
    const SourceEntry* find(uint32_t player, uint64_t slot, uint64_t now, uint32_t session) const {
        for (const SourceEntry& e : book_)
            if (e.s.player == player && e.s.slot == slot && now <= e.s.expires_ms && e.session == session && session != kNoSession) return &e;
        return nullptr;
    }
    const SourceEntry* find_token(uint64_t token) const {
        for (const SourceEntry& e : book_)
            if (e.s.token == token) return &e;
        return nullptr;
    }
    // Removes the offer with this token (a request used it). True when it was there.
    bool take(uint64_t token) {
        for (size_t k = 0; k < book_.size(); ++k)
            if (book_[k].s.token == token) {
                book_.erase(book_.begin() + static_cast<ptrdiff_t>(k));
                return true;
            }
        return false;
    }
    // A page entity some offer (live or lapsed but still in the book) to the player names: what the hook refuses without
    // reading the world when the forge actions are off.
    bool names_page(uint32_t player, uint32_t page) const {
        for (const SourceEntry& e : book_)
            if (e.s.player == player && KILN_SLOT_INVENTORY(e.s.slot) == page) return true;
        return false;
    }
    uint32_t withdraw(KilnOwner owner, uint32_t player, uint32_t holder) {
        uint32_t n = 0;
        for (size_t k = book_.size(); k-- > 0;) {
            const SourceEntry& e = book_[k];
            if ((!owner || e.owner == owner) && (!player || e.s.player == player) && (!holder || e.s.holder == holder)) {
                book_.erase(book_.begin() + static_cast<ptrdiff_t>(k));
                ++n;
            }
        }
        return n;
    }
    // A mod unloaded: its offers go.
    void drop_owner(KilnOwner owner) { withdraw(owner, 0, 0); }
    // A player left (or a new one took the slot): every owner's offers to the entity go.
    uint32_t drop_player(uint32_t player) { return player ? withdraw(0, player, 0) : 0; }
    // Offers expired at `now` (ms) go (kept `grace` ms longer, so a request that arrives right after still finds its
    // page in names_page and is refused rather than passed on).
    void expire(uint64_t now, uint64_t grace) {
        for (size_t k = book_.size(); k-- > 0;)
            if (now > book_[k].s.expires_ms + grace) book_.erase(book_.begin() + static_cast<ptrdiff_t>(k));
    }
    // The offers `player` can use now: live at `now` (ms) and of the player's `session`.
    template <class F>
    uint32_t live_for(uint32_t player, uint64_t now, uint32_t session, F&& f) const {
        uint32_t n = 0;
        for (const SourceEntry& e : book_)
            if (e.s.player == player && now <= e.s.expires_ms && e.session == session && session != kNoSession) {
                f(e.s);
                ++n;
            }
        return n;
    }
    // Offers were made in this world: from now on a player's request naming a gem outside the player's own inventories
    // runs only through a live offer.
    bool used() const { return used_; }
    size_t size() const { return book_.size(); }
    void clear() {
        book_.clear();
        used_ = false;
    }

private:
    std::vector<SourceEntry> book_;
    bool used_ = false;
};

// Whether a player's request names a gem of the player's own: its slot's page is one of the player's pages, or it is the
// socket {carrier, 0} of a carrier in the player's pages (Upgrade / Recycle of a socketed gem). `pages`: the player's
// page entities; `carrier_own`: the request's carrier is in them.
inline bool own_gem_slot(uint64_t slot, uint32_t weapon, bool carrier_own, const std::vector<uint32_t>& pages) {
    if (!slot) return true;  // no slot: a socketed gem through its carrier (Unslot), judged by the carrier
    const uint32_t page = KILN_SLOT_INVENTORY(slot);
    if (weapon && page == weapon && KILN_SLOT_INDEX(slot) == 0) return carrier_own;
    for (uint32_t p : pages)
        if (p == page) return true;
    return false;
}

// Once offers are in use: a request is foreign (only an offer may run it) when it names a gem slot outside the player's
// own pages; foreign by its carrier (refused, an offer never covers it) when it names a carrier the player's own pages
// do not hold. `carrier_own`: the player's pages hold the item entity `weapon`.
struct Ownership {
    bool foreign = false, foreign_carrier = false;
};
inline Ownership ownership(uint64_t slot, uint32_t weapon, bool carrier_own, const std::vector<uint32_t>& pages) {
    Ownership o;
    o.foreign_carrier = weapon != 0 && !carrier_own;
    o.foreign = o.foreign_carrier || !own_gem_slot(slot, weapon, carrier_own, pages);
    return o;
}

// What the hook does with a player's request that names a gem outside the player's own inventories.
enum class Foreign : uint8_t { Offered, Refuse };
inline Foreign foreign_verdict(uint32_t action, const SourceEntry* offer) {
    if (!offer) return Foreign::Refuse;
    // the GEMS grid offers loose gems: Insert, Upgrade, Recycle (Unslot names no gem slot)
    if (action != KILN_GEM_ACTION_SLOT && action != KILN_GEM_ACTION_UPGRADE && action != KILN_GEM_ACTION_RECYCLE) return Foreign::Refuse;
    return Foreign::Offered;
}

// ---- text ----
inline const char* action_name(uint32_t a) {
    switch (a) {
        case KILN_GEM_ACTION_SLOT: return "Slot";
        case KILN_GEM_ACTION_UNSLOT: return "Unslot";
        case KILN_GEM_ACTION_UPGRADE: return "Upgrade";
        case KILN_GEM_ACTION_RECYCLE: return "Recycle";
        default: return "?";
    }
}

}  // namespace kitems::gems
