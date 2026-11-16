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
inline bool request_valid(const KilnGemRequest& r) {
    if (r.size < sizeof(KilnGemRequest) || r.reserved[0] || r.reserved[1] || r.player == 0) return false;
    if (r.action < KILN_GEM_ACTION_SLOT || r.action > KILN_GEM_ACTION_RECYCLE) return false;
    if (r.flags & ~(KILN_GEM_FREE | KILN_GEM_COST)) return false;
    if ((r.flags & KILN_GEM_FREE) && (r.flags & KILN_GEM_COST)) return false;
    if (r.flags & KILN_GEM_COST) {
        if (r.cost_count > kMaxCostCount || (r.cost_count != 0 && r.cost_item == 0)) return false;
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
    // Hook side, for a player's unconsumed action `version` in step `step` while holding: what to do now.
    // Hold: keep it (first time: recorded with `info`). Allow / Cancel: the verdict (the entry goes).
    enum class Now : uint8_t { Hold, Run, Cancel };
    Now on_action(uint32_t player, uint32_t version, uint64_t step, const KilnGemCraft* info) {
        for (size_t i = 0; i < held_.size(); ++i) {
            Held& h = held_[i];
            if (h.player != player) continue;
            if (h.version != version) {  // a newer action replaces the older one (the game only sees the newest)
                held_.erase(held_.begin() + static_cast<ptrdiff_t>(i));
                break;
            }
            if (h.verdict == Verdict::Allow || (h.verdict == Verdict::None && step >= h.since + kHoldTimeoutSteps)) {
                timed_out_ += h.verdict == Verdict::None;
                held_.erase(held_.begin() + static_cast<ptrdiff_t>(i));
                return Now::Run;
            }
            if (h.verdict == Verdict::Cancel) {
                held_.erase(held_.begin() + static_cast<ptrdiff_t>(i));
                return Now::Cancel;
            }
            return Now::Hold;
        }
        Held h;
        h.player = player;
        h.version = version;
        h.since = step;
        if (info) h.info = *info;
        held_.push_back(h);
        ++total_;
        return Now::Hold;
    }
    // Tick side: the held actions not reported yet (each once).
    template <class F>
    uint32_t report(F&& f, uint32_t max) {
        uint32_t n = 0;
        for (Held& h : held_) {
            if (h.reported || n >= max) continue;
            h.reported = true;
            f(h);
            ++n;
        }
        return n;
    }
    bool waiting_reports() const {
        for (const Held& h : held_)
            if (!h.reported) return true;
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
    // Entries nobody's hook asked about for a long time (the player left): dropped.
    void expire(uint64_t step, uint64_t age) {
        for (size_t i = held_.size(); i-- > 0;)
            if (step > held_[i].since + age) held_.erase(held_.begin() + static_cast<ptrdiff_t>(i));
    }
    void clear() { held_.clear(); }
    size_t size() const { return held_.size(); }
    uint32_t total() const { return total_; }
    uint32_t canceled() const { return canceled_; }
    uint32_t timed_out() const { return timed_out_; }

private:
    std::vector<Held> held_;
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

// Why an action would be refused before the game runs it (KILN_GEM_DONE: nothing Kiln can see stops it). The game's
// own checks still run; these give the caller a reason instead of "refused".
inline int32_t precheck(uint32_t action, const Snapshot& b, uint32_t forge_max, uint32_t cost, uint32_t pays_from_have,
                        bool backpack_room, const char** why) {
    *why = "";
    if (forge_max == 0) return *why = "the forge entity is no Shroud Forge (no template, or maxGemLevel 0)", KILN_GEM_NO_FORGE;
    switch (action) {
        case KILN_GEM_ACTION_SLOT:
            if (!b.socket) return *why = "the carrier has no gem socket", KILN_GEM_NOT_FOUND;
            if (b.socket_pide != 0) return *why = "the socket holds a gem already", KILN_GEM_OCCUPIED;
            if (b.source_pide == 0) return *why = "the gem slot holds no gem", KILN_GEM_NOT_FOUND;
            break;
        case KILN_GEM_ACTION_UNSLOT:
            if (!b.socket) return *why = "the carrier has no gem socket", KILN_GEM_NOT_FOUND;
            if (b.socket_pide == 0) return *why = "the socket is empty", KILN_GEM_OCCUPIED;
            if (!backpack_room) return *why = "no free backpack slot for the gem", KILN_GEM_NO_ROOM;
            break;
        case KILN_GEM_ACTION_UPGRADE:
            if (b.gem_pide == 0) return *why = "no gem there", KILN_GEM_NOT_FOUND;
            if (!b.level_known) return *why = "the gem's level cannot be read", KILN_GEM_NOT_FOUND;
            if (b.level >= b.cap || b.level >= forge_max) return *why = "the gem is at its cap or the forge's maximum level", KILN_GEM_MAX_LEVEL;
            break;
        case KILN_GEM_ACTION_RECYCLE:
            if (b.gem_pide == 0) return *why = "no gem there", KILN_GEM_NOT_FOUND;
            break;
        default: return *why = "unknown action", KILN_GEM_NOT_FOUND;
    }
    if (cost > pays_from_have) return *why = "the player cannot pay the forge's cost", KILN_GEM_NOT_ENOUGH;
    return KILN_GEM_DONE;
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
