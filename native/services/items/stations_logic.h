// SPDX-License-Identifier: MIT
// kiln.stations: the service's logic without game memory (host test tests/stations_test.cpp): request checks, the fill's
// arithmetic (the game's own: AutoTransferRecipe, docs/re/stations.md §3.1), a base's id from its altars, the state diff
// behind the events and the event ring. The game side is stations.cpp.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "services/kiln_stations.h"

namespace kitems::stations {

constexpr uint32_t kMaxCrafts = 1000;
constexpr uint32_t kGameMaxCount = 0xFFFF;  // the game clamps what one input may move to a u16
constexpr uint32_t kMaxPlayerEntity = 1023;  // player entities, as kiln.items (engine.h kMaxPlayer)
constexpr uint32_t kWaterItem = 0;           // InputNow.item of a recipe's water (a pseudo input)
constexpr uint32_t kWaterUnitsPerCharge = 64;  // Fuel units per water charge (kiln_items.h)

// FILL's player: 0, or a player entity. Another entity would make the game's fill take from that entity's inventories.
inline bool player_ok(uint32_t p) { return p == 0 || p <= kMaxPlayerEntity; }

// ---- requests ----
inline bool any_choice(const KilnStationChoice* c) {
    for (int i = 0; i < 4; ++i)
        if (c[i].category != 0) return true;
    return false;
}

// The request names ordinary chests only when it is the full struct. A KILN_STATION_REQUEST_V1 request has none, and
// those fields are not read (the caller may not have them).
inline bool request_names_chests(const KilnStationRequest& r) { return r.size >= sizeof(KilnStationRequest); }

inline bool fill_chests_ok(const KilnStationRequest& r) {
    if (!request_names_chests(r)) return true;
    if (r.chest_reserved) return false;
    if (r.chest_count > KILN_STATIONS_MAX_CHESTS) return false;
    if (r.chest_count == 0) return r.chests == nullptr;
    if (!r.chests) return false;
    for (uint32_t i = 0; i < r.chest_count; ++i) {
        const uint32_t e = r.chests[i];
        if (e == 0 || e == r.station || e <= kMaxPlayerEntity) return false;  // not the station, not a player
    }
    return true;
}

inline bool request_valid(const KilnStationRequest& r) {
    if (r.size < KILN_STATION_REQUEST_V1 || r.size > sizeof(KilnStationRequest) || r.station == 0) return false;
    if (r.size != KILN_STATION_REQUEST_V1 && r.size < sizeof(KilnStationRequest)) return false;
    for (uint32_t x : r.reserved)
        if (x) return false;
    for (const auto& c : r.choices)
        if (c.reserved[0] || c.reserved[1] || c.reserved[2]) return false;
    constexpr uint32_t all = KILN_STATION_FILL_ADD | KILN_STATION_FILL_EXACT | KILN_STATION_DRY_RUN | KILN_STATION_NO_WATER |
                             KILN_STATION_AFTER_CRAFT | KILN_STATION_KEEP_STOPPED | KILN_STATION_ALLOW_LOCKED |
                             KILN_STATION_FILL_NO_OUTPUTS;
    if (r.flags & ~all) return false;
    switch (r.action) {
    case KILN_STATION_FILL: {
        constexpr uint32_t ok = KILN_STATION_FILL_ADD | KILN_STATION_FILL_EXACT | KILN_STATION_DRY_RUN | KILN_STATION_NO_WATER |
                                KILN_STATION_FILL_NO_OUTPUTS;
        if (r.flags & ~ok) return false;
        if (r.crafts < 1 || r.crafts > kMaxCrafts) return false;
        if (r.keep_count > KILN_STATIONS_MAX_KEEP || (r.keep_count && !r.keep)) return false;
        if (any_choice(r.choices) && r.recipe == 0) return false;  // choices name a recipe's categories
        return player_ok(r.player) && fill_chests_ok(r);
    }
    case KILN_STATION_STOP:
        return (r.flags & ~KILN_STATION_AFTER_CRAFT) == 0 && r.crafts == 0 && r.recipe == 0 && r.keep_count == 0 && r.player == 0 &&
               fill_chests_ok(r) && (!request_names_chests(r) || r.chest_count == 0);
    case KILN_STATION_RESUME:
        return r.flags == 0 && r.crafts == 0 && r.recipe == 0 && r.keep_count == 0 && r.player == 0 && fill_chests_ok(r) &&
               (!request_names_chests(r) || r.chest_count == 0);
    case KILN_STATION_SELECT:
        return (r.flags & ~(KILN_STATION_KEEP_STOPPED | KILN_STATION_ALLOW_LOCKED)) == 0 && r.crafts == 0 && r.keep_count == 0 &&
               r.player == 0 && fill_chests_ok(r) && (!request_names_chests(r) || r.chest_count == 0);  // recipe 0 clears
    default:
        return false;
    }
}

// ---- the fill's arithmetic ----
// One resolved input: `count` per craft, `have` in the station (every link, partial stacks too), `stock` what the fill may
// take (the base's Magic chests after the reserves, plus the named player's inventories, plus named ordinary chests, plus
// opted-in station outputs), `kept` what the reserves held back, `room` the most the station can hold of it (water: its
// capacity; items: not modelled). The recipe's water is an input too (item kWaterItem, count = units per craft): the game
// counts it in "loaded" (docs/re/stations.md §3.1).
struct InputNow {
    uint32_t item = 0;
    uint32_t count = 0;
    uint64_t have = 0;
    uint64_t stock = 0;
    uint64_t kept = 0;
    uint64_t room = UINT64_MAX;
    uint64_t chests = 0;   // of `stock`: named ordinary chests, after the player and the Magic chests, before outputs
    uint64_t outputs = 0;  // of `stock`: what opted-in stations of the base lend (their Outputs), taken last
};

// The water input of a recipe with `charges` per craft (none when 0, or when the request pours no water).
inline bool water_input(uint32_t charges, bool no_water, uint64_t station_water, uint64_t station_max, uint64_t base_water,
                        InputNow* out) {
    if (charges == 0 || no_water) return false;
    *out = InputNow{};
    out->item = kWaterItem;
    out->count = charges * kWaterUnitsPerCharge;
    out->have = station_water;
    out->stock = base_water;
    out->room = station_max;
    return true;
}

// Inputs naming the same item more than once (an item and a category option that is that item) become one: the station
// holds one pile of it, and the stock is taken once.
inline void merge_inputs(std::vector<InputNow>* in) {
    std::vector<InputNow> out;
    for (const InputNow& i : *in) {
        auto it = std::find_if(out.begin(), out.end(), [&](const InputNow& o) { return o.item == i.item; });
        if (it == out.end()) out.push_back(i);
        else it->count += i.count;
    }
    in->swap(out);
}

// Complete crafts the station holds: min over inputs of have / count; 0 when an input's count is 0 (as the game).
inline uint32_t crafts_loaded(const std::vector<InputNow>& in) {
    if (in.empty()) return 0;
    uint64_t m = UINT64_MAX;
    for (const auto& i : in) {
        if (i.count == 0) return 0;
        m = std::min<uint64_t>(m, i.have / i.count);
    }
    return static_cast<uint32_t>(std::min<uint64_t>(m, UINT32_MAX));
}

// What the game moves for input i when asked for n more crafts: (n + loaded) * count - have, clamped 0..0xFFFF.
inline uint32_t game_need(const InputNow& i, uint32_t n, uint32_t loaded) {
    const int64_t want = static_cast<int64_t>(static_cast<uint64_t>(n) + loaded) * i.count - static_cast<int64_t>(i.have);
    if (want <= 0) return 0;
    return static_cast<uint32_t>(std::min<int64_t>(want, kGameMaxCount));
}

// More crafts the stock covers on top of `loaded`: the largest n with game_need(i, n) <= stock for every input.
inline uint32_t crafts_affordable(const std::vector<InputNow>& in, uint32_t loaded) {
    if (in.empty()) return 0;
    uint64_t best = UINT64_MAX;
    for (const auto& i : in) {
        if (i.count == 0) continue;  // the game moves nothing for it
        const uint64_t total = (i.have + i.stock) / i.count;
        const uint64_t more = total > loaded ? total - loaded : 0;
        // the u16 clamp: n never asks more than 0xFFFF of one input per call
        const uint64_t cap = (static_cast<uint64_t>(kGameMaxCount) + i.have) / i.count;
        uint64_t m = std::min<uint64_t>(more, cap > loaded ? cap - loaded : 0);
        if (i.room != UINT64_MAX) {  // the station holds no more than this of it
            const uint64_t r = i.room / i.count;
            m = std::min<uint64_t>(m, r > loaded ? r - loaded : 0);
        }
        best = std::min<uint64_t>(best, m);
    }
    return best == UINT64_MAX ? 0 : static_cast<uint32_t>(std::min<uint64_t>(best, UINT32_MAX));
}

// How many more crafts a request asks for: UpTo (default) = crafts - loaded, Add = crafts.
inline uint32_t crafts_wanted(uint32_t flags, uint32_t crafts, uint32_t loaded) {
    if (flags & KILN_STATION_FILL_ADD) return crafts;
    return crafts > loaded ? crafts - loaded : 0;
}

// After the game refused n (no room in the station, or a short input it saw and the plan did not): the next n to try, 0
// to stop. Halves, at most kMaxTries calls in all.
constexpr uint32_t kMaxTries = 6;
inline uint32_t crafts_next_try(uint32_t flags, uint32_t n, uint32_t tries) {
    if ((flags & KILN_STATION_FILL_EXACT) || tries >= kMaxTries || n <= 1) return 0;
    return n / 2;
}

// The first n a fill runs the game's function with: what is wanted, cut to what the stock covers. EXACT: all of it, or 0
// (no call) when the stock after the reserves does not cover it: the game itself knows no reserves.
inline uint32_t crafts_first_try(uint32_t flags, uint32_t wanted, uint32_t affordable) {
    if (flags & KILN_STATION_FILL_EXACT) return wanted <= affordable ? wanted : 0;
    return std::min(wanted, affordable);
}

// What allows the fewest more crafts, and why: the item and KILN_STATION_LIMIT_*.
struct Limit {
    uint32_t item = 0;
    uint8_t why = KILN_STATION_LIMIT_NONE;
};
inline Limit limit_of(const std::vector<InputNow>& in, uint32_t loaded) {
    Limit l;
    uint64_t best = UINT64_MAX;
    for (const auto& i : in) {
        if (i.count == 0) continue;
        const uint64_t total = (i.have + i.stock) / i.count;
        const uint64_t more = total > loaded ? total - loaded : 0;
        uint64_t room_more = UINT64_MAX;
        if (i.room != UINT64_MAX) {
            const uint64_t r = i.room / i.count;
            room_more = r > loaded ? r - loaded : 0;
        }
        const uint64_t m = std::min(more, room_more);
        if (m >= best) continue;
        best = m;
        if (i.item == kWaterItem) l = {0, KILN_STATION_LIMIT_WATER};
        else if (room_more < more) l = {i.item, KILN_STATION_LIMIT_ROOM};
        else if (i.kept && (i.have + i.stock + i.kept) / i.count > total) l = {i.item, KILN_STATION_LIMIT_RESERVE};
        else l = {i.item, KILN_STATION_LIMIT_STOCK};
    }
    return l;
}

// The plan's arithmetic for a FILL request over its inputs (water and duplicates already folded in).
struct PlanMath {
    int32_t status = KILN_STATION_DONE;
    uint32_t loaded = 0, affordable = 0, wanted = 0, planned = 0;
    Limit limit;
};
inline PlanMath plan_math(uint32_t flags, uint32_t crafts, const std::vector<InputNow>& in) {
    PlanMath p;
    p.loaded = crafts_loaded(in);
    p.affordable = crafts_affordable(in, p.loaded);
    p.wanted = std::min<uint32_t>(crafts_wanted(flags, crafts, p.loaded), kGameMaxCount);
    p.planned = crafts_first_try(flags, p.wanted, p.affordable);
    if (p.wanted == 0) {
        p.status = KILN_STATION_UNCHANGED;
        return p;
    }
    if (p.planned < p.wanted) p.limit = limit_of(in, p.loaded);
    if (p.planned == 0) p.status = p.limit.why == KILN_STATION_LIMIT_ROOM ? KILN_STATION_NO_ROOM : KILN_STATION_NOT_ENOUGH;
    else p.status = p.planned < p.wanted ? KILN_STATION_PARTIAL : KILN_STATION_DONE;
    return p;
}

// The game's function, tried with n and then less (crafts_next_try) until it takes it. `attempt(n)` returns the game's
// code (0: done) or kAttemptFault (stop at once).
constexpr int32_t kAttemptFault = -0x7fffffff;
struct Attempts {
    bool ok = false;
    uint32_t n = 0, tries = 0;
    int32_t code = 0;
};
template <class F>
Attempts run_attempts(uint32_t flags, uint32_t first, F attempt) {
    Attempts a;
    uint32_t n = first;
    while (n > 0) {
        ++a.tries;
        a.code = attempt(n);
        if (a.code == 0) {
            a.ok = true;
            a.n = n;
            return a;
        }
        if (a.code == kAttemptFault) return a;
        n = crafts_next_try(flags, n, a.tries);
    }
    return a;
}
// A FILL's status from its attempts: DONE / PARTIAL, else the game's last code (2 no room, 4 short), else REFUSED.
inline int32_t fill_status(const Attempts& a, uint32_t wanted) {
    if (a.ok) return a.n < wanted ? KILN_STATION_PARTIAL : KILN_STATION_DONE;
    if (a.code == 2) return KILN_STATION_NO_ROOM;
    if (a.code == 4) return KILN_STATION_NOT_ENOUGH;
    return KILN_STATION_REFUSED;
}

// Stock after a reserve: what may be taken of `have` when `keep` must stay.
inline uint64_t after_keep(uint64_t stock, uint64_t keep) { return stock > keep ? stock - keep : 0; }

// ---- station outputs as crafting stock (GitHub #68; docs/re/stations.md section 8) ----
// A station's recipes as the classification needs them: the outputs, and every item that is an input (a category
// input: all its options). Of the selected and the running recipe together, as Kiln.Blocks' SlotRoles.
struct SourceRecipes {
    std::vector<uint32_t> outputs;
    std::vector<uint32_t> inputs;
};
// Item `item` is an Output of that station: an output of its recipes and no input of them. Then every stack of the item
// in it is an Output stack, so the game's per-item take from the station can touch nothing else.
inline bool lends_item(const SourceRecipes& r, uint32_t item) {
    if (item == 0) return false;
    if (std::find(r.inputs.begin(), r.inputs.end(), item) != r.inputs.end()) return false;
    return std::find(r.outputs.begin(), r.outputs.end(), item) != r.outputs.end();
}

// One opted-in station as a fill sees it: the base it stands in now (-1: none) and whether it is still a station.
struct SourceCandidate {
    uint32_t entity = 0;
    int32_t base = -1;
    bool station = false;
};
// A station slot's role, the same split as Kiln.Blocks' SlotRoles. `known` is false when the station has no recipe:
// leftovers are then none (an output that is also an input is neither).
enum class SlotRole : uint8_t { None, Output, Leftover };
inline SlotRole slot_role(const SourceRecipes& r, uint32_t item, bool known) {
    if (item == 0) return SlotRole::None;
    const bool input = std::find(r.inputs.begin(), r.inputs.end(), item) != r.inputs.end();
    const bool output = std::find(r.outputs.begin(), r.outputs.end(), item) != r.outputs.end();
    if (output && !input) return SlotRole::Output;
    if (known && !input && !output) return SlotRole::Leftover;
    return SlotRole::None;
}

// Collect's result. `finished` is stacks whose move completed (a partial move is not finished). `any_moved` is set when
// some items left a stack that was not finished: that is PARTIAL, not DONE and not NO_ROOM.
inline int32_t collect_status(uint32_t stacks, uint32_t finished, bool any_moved = false) {
    if (stacks == 0) return KILN_STATION_UNCHANGED;
    if (finished >= stacks) return KILN_STATION_DONE;
    if (finished > 0 || any_moved) return KILN_STATION_PARTIAL;
    return KILN_STATION_NO_ROOM;
}

// CollectToBase when the station stands in no base. The managed call answered NoRoom: every stack's target was null.
inline int32_t collect_without_base() { return KILN_STATION_NO_ROOM; }

// A move of a collect faulted. Stacks moved before it stay moved (each move is its own transaction): that is PARTIAL,
// so the caller sees that something changed. Nothing moved yet: UNAVAILABLE.
inline int32_t collect_fault_status(bool any_moved_before) {
    return any_moved_before ? KILN_STATION_PARTIAL : KILN_STATION_UNAVAILABLE;
}

// DynamicInventoryCraftingStock when the override is set (byte 0, byte 1), else the template's
// InventoryCraftingStock.canReceiveAreaDeposit. Unknown (no template bit) stays allowed, as the managed TakesDeposits.
struct DepositRead {
    bool component = false;
    bool override_set = false;
    bool override_on = false;
    bool template_known = false;
    bool template_on = false;
};
inline bool takes_deposits(const DepositRead& d) {
    if (d.component && d.override_set) return d.override_on;
    if (d.template_known) return d.template_on;
    return true;
}

// FindRoomFor's order: a chest that already holds the item with room, else one with a free slot. A chest with neither
// is not a destination.
struct ChestOffer {
    uint32_t entity = 0;
    bool holds_with_room = false;
    bool free_slot = false;
};
inline int chest_rank(const ChestOffer& c) {
    if (c.holds_with_room) return 0;
    if (c.free_slot) return 1;
    return 2;
}
inline std::vector<uint32_t> collect_chest_order(const std::vector<ChestOffer>& chests) {
    std::vector<uint32_t> idx(chests.size());
    for (uint32_t i = 0; i < static_cast<uint32_t>(idx.size()); ++i) idx[i] = i;
    std::stable_sort(idx.begin(), idx.end(), [&](uint32_t a, uint32_t b) { return chest_rank(chests[a]) < chest_rank(chests[b]); });
    std::vector<uint32_t> out;
    for (uint32_t i : idx)
        if (chest_rank(chests[i]) < 2) out.push_back(chests[i].entity);
    return out;
}

// One stack across chests in order. `room[i]` is how many of the remaining count chest i takes (0: this chest takes
// nothing, try the next). Finished only when nothing of the stack remains.
struct StackCollect {
    bool finished = false;
    bool any = false;
    uint32_t left = 0;
};
inline StackCollect collect_stack(uint32_t count, const uint32_t* room, uint32_t n) {
    StackCollect s;
    s.left = count;
    for (uint32_t i = 0; i < n && s.left; ++i) {
        uint32_t take = room[i];
        if (take > s.left) take = s.left;
        if (!take) continue;
        s.any = true;
        s.left -= take;
    }
    s.finished = count > 0 && s.left == 0;
    return s;
}

inline bool collect_valid(const KilnStationCollect& c) {
    if (c.size < sizeof(KilnStationCollect) || c.station == 0 || c.reserved != 0) return false;
    constexpr uint32_t what = KILN_STATION_COLLECT_OUTPUTS | KILN_STATION_COLLECT_LEFTOVERS;
    constexpr uint32_t all = what | KILN_STATION_COLLECT_TO_BASE;
    if ((c.flags & ~all) != 0 || (c.flags & what) == 0) return false;
    if (c.flags & KILN_STATION_COLLECT_TO_BASE) return c.to == 0;
    return c.to != 0 && c.to != c.station;  // a player's bag too, as Items.Move (run_move guards its own slots)
}

// The stations a fill of `target` in base `base` may use, in entity order: opted in, still a station, in that base, not the
// target itself. A station of another base is never used.
inline std::vector<uint32_t> eligible_sources(const std::vector<SourceCandidate>& all, uint32_t target, int32_t base) {
    std::vector<uint32_t> out;
    if (base < 0) return out;
    for (const SourceCandidate& c : all)
        if (c.entity && c.entity != target && c.station && c.base == base) out.push_back(c.entity);
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

// The resolved inputs repeat an item: the game then counts and tops up each entry on its own, which Kiln's per-item
// shortfall call cannot mirror. Such a fill uses no station outputs and ignores named ordinary chests (the plain fill
// still runs).
inline bool inputs_repeat(const uint32_t* items, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i)
        for (uint32_t k = i + 1; k < n; ++k)
            if (items[i] == items[k]) return true;
    return false;
}
inline bool use_named_chests(const uint32_t* items, uint32_t n) { return n != 0 && !inputs_repeat(items, n); }

// What one named entity is, once the world has been read. Ordinary: an initialized inventory in the station's base, not a
// production station, not a player's or an NPC's inventory, not already in the base's Magic-chest list.
enum class ChestKind : uint8_t { Ordinary, Missing, OtherBase, Magic, Station, NoInventory, Player, IsTarget, Person };

// InventorySetup's flags byte (kiln.items' KILN_ITEMS_FIND_* split): bit 0 a player's inventory, bit 1 an NPC's.
inline bool setup_is_person(uint8_t flags) { return (flags & 3u) != 0; }

struct ChestFacts {
    int32_t base = -1;
    bool present = false;
    bool station = false;
    bool magic = false;
    bool inventory = false;
    bool person = false;  // the inventory is a player's or an NPC's (InventorySetup flags), not a storage container
};

struct NamedChest {
    uint32_t entity = 0;
    int32_t base = -1;
    ChestKind kind = ChestKind::Missing;
};

inline ChestKind kind_of(const ChestFacts& f, uint32_t entity, uint32_t target, int32_t station_base) {
    if (entity == 0) return ChestKind::Missing;
    if (entity == target) return ChestKind::IsTarget;
    if (entity <= kMaxPlayerEntity) return ChestKind::Player;
    if (!f.present) return ChestKind::Missing;
    if (f.station) return ChestKind::Station;
    if (f.magic) return ChestKind::Magic;
    if (!f.inventory) return ChestKind::NoInventory;
    if (f.person) return ChestKind::Person;
    if (station_base < 0 || f.base != station_base) return ChestKind::OtherBase;
    return ChestKind::Ordinary;
}

inline const char* chest_kind_why(ChestKind k) {
    switch (k) {
    case ChestKind::IsTarget: return "entity %u is the station being filled";
    case ChestKind::Player: return "entity %u is a player, not a chest";
    case ChestKind::Station: return "entity %u is a production station, not an ordinary chest";
    case ChestKind::Magic: return "entity %u is already one of the base's Magic chests";
    case ChestKind::NoInventory: return "entity %u has no inventory";
    case ChestKind::Person: return "entity %u is a player's or an NPC's inventory, not a storage chest";
    case ChestKind::OtherBase: return "entity %u is not in this base";
    case ChestKind::Missing: return "entity %u is not in this base";
    case ChestKind::Ordinary: return "";
    }
    return "entity %u is not an ordinary chest in this base";
}

// Every distinct named entity must be an ordinary chest. Duplicates are one entity.
inline bool check_named_chests(const std::vector<NamedChest>& named, uint32_t* bad, ChestKind* kind) {
    std::vector<uint32_t> seen;
    for (const NamedChest& c : named) {
        if (std::find(seen.begin(), seen.end(), c.entity) != seen.end()) continue;
        seen.push_back(c.entity);
        if (c.kind != ChestKind::Ordinary) {
            *bad = c.entity;
            *kind = c.kind;
            return false;
        }
    }
    return true;
}

// The chests a fill may take from, in the order the request names them, once each (a repeat keeps its first place).
inline std::vector<uint32_t> eligible_chests(const std::vector<NamedChest>& all, uint32_t target, int32_t base) {
    std::vector<uint32_t> out;
    if (base < 0) return out;
    for (const NamedChest& c : all)
        if (c.kind == ChestKind::Ordinary && c.entity != target && c.base == base &&
            std::find(out.begin(), out.end(), c.entity) == out.end())
            out.push_back(c.entity);
    return out;
}

// For a fill of n more crafts on top of `loaded`: what ordinary chests pay, then what station outputs pay. The player's
// bag and the Magic chests (stock - chests - outputs, after the reserves) pay what they can first; chests pay the next
// slice; outputs pay only the rest. Water comes from neither. False when those two cannot cover the rest (the plan
// should not have asked for n). With chests == 0 this is the station-output share alone.
inline bool pay_layers(const std::vector<InputNow>& in, uint32_t n, uint32_t loaded, std::vector<uint32_t>* chest_shares,
                       std::vector<uint32_t>* output_shares) {
    chest_shares->assign(in.size(), 0);
    output_shares->assign(in.size(), 0);
    for (size_t k = 0; k < in.size(); ++k) {
        const InputNow& i = in[k];
        if (i.item == kWaterItem || i.count == 0) continue;
        const uint64_t need = game_need(i, n, loaded);
        const uint64_t extra = std::min(i.stock, i.chests + i.outputs);
        const uint64_t first = i.stock - extra;
        if (need <= first) continue;
        const uint64_t after = need - first;
        const uint64_t from_chests = std::min(i.chests, after);
        const uint64_t rest = after - from_chests;
        if (rest > i.outputs) return false;
        (*chest_shares)[k] = static_cast<uint32_t>(from_chests);
        (*output_shares)[k] = static_cast<uint32_t>(rest);
    }
    return true;
}

inline bool output_shares(const std::vector<InputNow>& in, uint32_t n, uint32_t loaded, std::vector<uint32_t>* shares) {
    std::vector<uint32_t> chests;
    return pay_layers(in, n, loaded, &chests, shares);
}

inline std::vector<uint32_t> share_sum(const std::vector<uint32_t>& a, const std::vector<uint32_t>& b) {
    std::vector<uint32_t> o(std::max(a.size(), b.size()), 0);
    for (size_t i = 0; i < o.size(); ++i) {
        const uint32_t x = i < a.size() ? a[i] : 0;
        const uint32_t y = i < b.size() ? b[i] : 0;
        o[i] = x + y;
    }
    return o;
}
inline bool any_share(const std::vector<uint32_t>& shares) {
    return std::any_of(shares.begin(), shares.end(), [](uint32_t s) { return s != 0; });
}
inline uint64_t shares_total(const std::vector<uint32_t>& shares) {
    uint64_t t = 0;
    for (uint32_t s : shares) t += s;
    return t;
}

// One line of a crafting payment (docs/re/stations.md §10). `game` is what the game's own sources can pay
// (the owner and the base's Magic chests, already in the game's order). `outputs` is what opted-in stations
// can lend of this item. `need` is the line's cost. Outputs pay only the shortfall, and only when every line
// is covered: otherwise Kiln takes nothing and the game's own payment runs.
struct CraftCost {
    uint32_t item = 0;
    uint32_t need = 0;
    uint64_t game = 0;
    uint64_t outputs = 0;
};
// False when some line is still short with the outputs counted in: `*from_outputs` is then all zero.
// True when every line is covered. A line the game's sources already cover takes nothing from outputs.
inline bool craft_output_takes(const std::vector<CraftCost>& lines, std::vector<uint32_t>* from_outputs) {
    from_outputs->assign(lines.size(), 0);
    for (size_t i = 0; i < lines.size(); ++i) {
        const CraftCost& c = lines[i];
        if (c.need == 0 || c.game >= c.need) continue;
        const uint64_t rest = static_cast<uint64_t>(c.need) - c.game;
        if (rest > c.outputs) {
            from_outputs->assign(lines.size(), 0);
            return false;
        }
        (*from_outputs)[i] = static_cast<uint32_t>(rest);
    }
    return true;
}

// ---- a craft's payment, without game memory (stations.cpp craft_pay_impl; docs/re/stations.md §10) ----
constexpr uint64_t kMaxCraftStock = 4096;  // a longer stock list than this is left to the game

// Who pays a craft before any station, in the game's order (InventoryPay): stock mode 2 pays the owner first and then
// the base's stock list, mode 1 the list and then the owner. Each entity once: the owner can be one of the list's
// chests, and counting it twice would hide a shortfall. False (the game's own payment) for any other mode (0 has no
// base stock) and for a list longer than kMaxCraftStock, so outputs never pay before a chest the game would ask.
inline bool craft_order(uint8_t mode, uint32_t owner, const uint32_t* list, uint64_t n, std::vector<uint32_t>* out) {
    out->clear();
    if ((mode != 1 && mode != 2) || n > kMaxCraftStock || (n && !list)) return false;
    std::unordered_set<uint32_t> seen;
    auto push = [&](uint32_t e) {
        if (e && seen.insert(e).second) out->push_back(e);
    };
    if (mode == 2) push(owner);
    for (uint64_t i = 0; i < n; ++i) push(list[i]);
    if (mode == 1) push(owner);
    return true;
}

// One opted-in station as a craft's payment reads it, at the payment.
struct CraftLender {
    uint32_t entity = 0;
    int32_t base = -1;      // the base it stands in now (-1: none, or its place was not read)
    bool station = false;   // still a station, and its recipes were read
    SourceRecipes recipes;  // its selected and running recipes
};
// What the lenders of one craft hold: per item, the count and the stations (in entity order).
struct CraftLent {
    std::unordered_map<uint32_t, uint64_t> count;
    std::unordered_map<uint32_t, std::vector<uint32_t>> by;
};
// What opted-in stations lend to a craft in base `base`, of `items`: only stations of that base (eligible_sources),
// never `exclude` (the station crafted at, or a non-player owner), never an entity the game's own sources already pay
// from (`order`), and of each station only an item that is an output and no input of its recipes (lends_item: its
// queued inputs, its Leftovers and an output that is also an input stay where they are). `count(entity, item)` is what
// that entity holds of the item.
template <class Count>
void craft_lenders(const std::vector<CraftLender>& all, uint32_t exclude, int32_t base, const std::vector<uint32_t>& order,
                   const std::vector<uint32_t>& items, Count count, CraftLent* out) {
    std::vector<SourceCandidate> cands;
    cands.reserve(all.size());
    for (const CraftLender& l : all) cands.push_back(SourceCandidate{l.entity, l.base, l.station});
    for (uint32_t e : eligible_sources(cands, exclude, base)) {
        if (std::find(order.begin(), order.end(), e) != order.end()) continue;
        const auto it = std::find_if(all.begin(), all.end(), [e](const CraftLender& l) { return l.entity == e; });
        if (it == all.end()) continue;
        for (uint32_t item : items) {
            if (!lends_item(it->recipes, item)) continue;
            const uint64_t n = count(e, item);
            if (!n) continue;
            out->count[item] += n;
            out->by[item].push_back(e);
        }
    }
}

// The items a Kiln client must have counted to raise a recipe's count with station outputs (GitHub #68, the crafting
// menu): every input (an item, or every option of a category input) of every recipe that takes one of `lent`, and the
// `lent` items themselves. A recipe that takes none of them keeps the game's own count, so its other inputs are not
// needed. `recipe_inputs`: per recipe, its input items and options. Sorted, unique.
inline std::vector<uint32_t> craft_stock_items(const std::vector<std::vector<uint32_t>>& recipe_inputs, const std::vector<uint32_t>& lent) {
    std::unordered_set<uint32_t> want(lent.begin(), lent.end());
    want.erase(0);
    std::unordered_set<uint32_t> out(want.begin(), want.end());
    if (!want.empty())
        for (const auto& inputs : recipe_inputs)
            if (std::any_of(inputs.begin(), inputs.end(), [&](uint32_t i) { return want.count(i) != 0; }))
                for (uint32_t i : inputs)
                    if (i) out.insert(i);
    std::vector<uint32_t> v(out.begin(), out.end());
    std::sort(v.begin(), v.end());
    return v;
}

// ---- the station screen's fill buttons on a Kiln client (GitHub #68, docs/re/stations.md §10.3) ----
// The game's UI action query (stations/client.UiActionStatus) with type 3 asks whether a recipe fill of `amount` more
// crafts would go through (the station screen's "+1" and "+10"; the client's own dry run of AutoTransferRecipe on its view
// of the bag and the Magic chests). It answers 0 (not run yet), 1 (it would: the button is enabled), or the fill's code:
// 2 no room in the station, 4 missing materials, 5 no inventory.
constexpr uint32_t kUiQueryFill = 3;
constexpr uint32_t kUiOk = 1, kUiMissing = 4;

// The key of a query as the screen passes it; a choice's reserved bytes are not part of it.
inline KilnStationFillKey fill_key(uint32_t station, uint32_t recipe, const KilnStationChoice* sel) {
    KilnStationFillKey k{};
    k.station = station;
    k.recipe = recipe;
    for (int i = 0; sel && i < 4; ++i) {
        k.choices[i].category = sel[i].category;
        k.choices[i].index = sel[i].index;
    }
    return k;
}
inline bool same_fill(const KilnStationFillKey& a, const KilnStationFillKey& b) {
    if (a.station != b.station || a.recipe != b.recipe) return false;
    for (int i = 0; i < 4; ++i)
        if (a.choices[i].category != b.choices[i].category || a.choices[i].index != b.choices[i].index) return false;
    return true;
}
// A row set_fill_answers takes: a station and a recipe, reserved bytes 0.
inline bool fill_answer_ok(const KilnStationFillAnswer& a) {
    if (!a.key.station || !a.key.recipe || a.reserved) return false;
    for (int i = 0; i < 4; ++i)
        if (a.key.choices[i].reserved[0] || a.key.choices[i].reserved[1] || a.key.choices[i].reserved[2]) return false;
    return true;
}

// What the query answers: the game's own status, except that a fill the game refused for missing materials is "can fill"
// when the server's answer for exactly this station, recipe and choices covers at least `amount` more crafts. Every other
// status (not run yet, no room, no inventory, ok) and every other query type stays the game's. *answered: an answer names
// this key (whatever its count).
inline uint32_t fill_ui_status(uint32_t game, uint32_t type, uint32_t amount, const KilnStationFillKey& k,
                               const std::vector<KilnStationFillAnswer>& answers, bool* answered) {
    if (answered) *answered = false;
    if (type != kUiQueryFill || game != kUiMissing) return game;
    for (const KilnStationFillAnswer& a : answers) {
        if (!same_fill(a.key, k)) continue;
        if (answered) *answered = true;
        return amount != 0 && amount <= a.crafts ? kUiOk : game;
    }
    return game;
}

// The fills to ask the server about: refused for missing materials, no answer yet. Oldest first, at most `cap` (the
// oldest goes when a new one comes). False: nothing changed.
inline bool note_fill_ask(std::vector<KilnStationFillKey>* asks, const KilnStationFillKey& k, size_t cap) {
    for (const KilnStationFillKey& a : *asks)
        if (same_fill(a, k)) return false;
    if (cap == 0) return false;
    if (asks->size() >= cap) asks->erase(asks->begin());
    asks->push_back(k);
    return true;
}
// New answers came: the asks they name are answered.
inline void drop_answered(std::vector<KilnStationFillKey>* asks, const std::vector<KilnStationFillAnswer>& answers) {
    asks->erase(std::remove_if(asks->begin(), asks->end(),
                               [&](const KilnStationFillKey& k) {
                                   return std::any_of(answers.begin(), answers.end(),
                                                      [&](const KilnStationFillAnswer& a) { return same_fill(a.key, k); });
                               }),
                asks->end());
}

enum CraftVerdict : int {
    kCraftGame = 0,      // Kiln took nothing: the game's own payment runs (`why` says why)
    kCraftPaid = 1,      // every line paid, station outputs last: InventoryPay's 0
    kCraftRollback = 2,  // a take faulted, or a line stayed short after a take: InventoryPay's 4 (the craft rolls back)
};
enum CraftWhy : uint32_t {
    kCraftWhyNone = 0,
    kCraftWhyCovered = 1,    // the game's own sources cover every line: no outputs needed
    kCraftWhyShort = 2,      // a line's shortfall is more than this base's opted-in outputs hold
    kCraftWhyFault = 3,      // a take faulted
    kCraftWhyRemainder = 4,  // a line was still short after every source paid (the counts did not hold)
};

// One craft paid as the game's InventoryPay pays it, with the opted-in outputs as the very last source. Every line is
// counted first: `have(entity, item)` for the game's own sources in `order`, `lent` for the stations. Kiln pays only
// when some line is short of the game's own sources and the outputs cover every line's shortfall (all or nothing);
// otherwise it takes nothing and the game pays (or refuses) as always. It pays line by line: the game's sources in their
// order as far as they pay, then that line's lenders for what is left. `pay(entity, item, &left)` takes up to *left of
// the item from the entity and lowers *left; false when it faulted. *from_outputs: what the lenders paid.
template <class Have, class Pay>
int craft_pay(std::vector<CraftCost>* lines, const std::vector<uint32_t>& order, const CraftLent& lent, Have have, Pay pay,
              uint64_t* from_outputs, uint32_t* why) {
    *from_outputs = 0;
    *why = kCraftWhyNone;
    for (CraftCost& line : *lines) {
        line.game = 0;
        uint64_t still = line.need;
        for (uint32_t e : order) {
            if (!still) break;
            const uint64_t take = std::min<uint64_t>(have(e, line.item), still);
            line.game += take;
            still -= take;
        }
        const auto it = lent.count.find(line.item);
        line.outputs = it == lent.count.end() ? 0 : it->second;
    }
    std::vector<uint32_t> takes;
    if (!craft_output_takes(*lines, &takes)) {
        *why = kCraftWhyShort;
        return kCraftGame;
    }
    if (!any_share(takes)) {
        *why = kCraftWhyCovered;
        return kCraftGame;
    }
    for (size_t i = 0; i < lines->size(); ++i) {
        const CraftCost& line = (*lines)[i];
        uint32_t left = line.need;
        for (uint32_t e : order) {
            if (!left) break;
            if (!pay(e, line.item, &left)) {
                *why = kCraftWhyFault;
                return kCraftRollback;  // a take may already be in the transaction: the caller rolls it back
            }
        }
        if (left && takes[i]) {
            const auto by = lent.by.find(line.item);
            if (by != lent.by.end())
                for (uint32_t e : by->second) {
                    if (!left) break;
                    const uint32_t before = left;
                    if (!pay(e, line.item, &left)) {
                        *why = kCraftWhyFault;
                        return kCraftRollback;
                    }
                    *from_outputs += before - left;
                }
        }
        if (left) {
            *why = kCraftWhyRemainder;
            return kCraftRollback;
        }
    }
    return kCraftPaid;
}

// Once the shares are in the station: the n the game's own call then needs so that every input ends where a plain fill
// of n would put it, n + loaded - loaded'. The game's call takes the rest from the player and the chests and pours the
// water, exactly as it would have.
inline uint32_t crafts_after_shares(const std::vector<InputNow>& in, const std::vector<uint32_t>& shares, uint32_t n, uint32_t loaded) {
    std::vector<InputNow> after = in;
    for (size_t k = 0; k < after.size() && k < shares.size(); ++k) after[k].have += shares[k];
    const uint32_t l2 = crafts_loaded(after);
    const uint64_t target = static_cast<uint64_t>(n) + loaded;
    return l2 >= target ? 0u : static_cast<uint32_t>(target - l2);
}

// ---- stop after the running craft ----
// A cycle is in progress while running != 0 in Running, Paused (it resumes where it paused) or OutputFull (its outputs
// wait for room); a stop then makes the game refund the inputs and lose the cycle. Due: no cycle in progress, or a new
// cycle began (Running with another start, younger than kFreshNs: a resume from a pause also moves the start, to
// now - the time it had run). `start0` is recipeStart when the request was taken; now_ns the world's program time.
constexpr int64_t kFreshNs = 2'000'000'000;
inline bool cycle_in_progress(uint8_t state, uint32_t running) {
    return running != 0 && (state == KILN_STATION_STATE_RUNNING || state == KILN_STATION_STATE_PAUSED ||
                            state == KILN_STATION_STATE_OUTPUT_FULL);
}
inline bool after_craft_due(uint8_t state, uint32_t running, int64_t start0, int64_t start_now, int64_t now_ns = INT64_MIN) {
    if (!cycle_in_progress(state, running)) return true;
    if (state != KILN_STATION_STATE_RUNNING || start_now == start0) return false;
    return now_ns == INT64_MIN || now_ns - start_now < kFreshNs;
}

// A waiting STOP (after craft), looked at each step.
enum class StopWait { Wait, Write, TimedOut, Canceled, Unchanged, Gone };
struct StopSeen {
    bool present = false;  // the station still has its FactoryStation
    uint8_t state = 0, stopped = 0;
    uint32_t running = 0, selected = 0;
    int64_t start = 0, now = 0;
};
inline StopWait waiting_stop(const StopSeen& s, uint32_t selected0, int64_t start0, uint64_t waited, uint64_t max_steps) {
    if (!s.present) return StopWait::Gone;
    if (s.stopped) return StopWait::Unchanged;            // someone stopped it meanwhile: nothing left to do
    if (s.selected != selected0) return StopWait::Canceled;  // its recipe was changed (a player's or a mod's select)
    if (after_craft_due(s.state, s.running, start0, s.start, s.now)) return StopWait::Write;
    return waited > max_steps ? StopWait::TimedOut : StopWait::Wait;
}

// A later action on a station withdraws a STOP waiting there: RESUME, SELECT, and a STOP that is not after-craft.
inline bool withdraws_waiting(const KilnStationRequest& r) {
    return r.action == KILN_STATION_RESUME || r.action == KILN_STATION_SELECT ||
           (r.action == KILN_STATION_STOP && !(r.flags & KILN_STATION_AFTER_CRAFT));
}

// ---- the request queue ----
struct Req {
    uint64_t ticket = 0;
    KilnOwner owner = 0;
    KilnStationRequest r{};
    KilnStationStack keep[KILN_STATIONS_MAX_KEEP]{};
    KilnEntity chests[KILN_STATIONS_MAX_CHESTS]{};
    uint32_t chest_n = 0;
    KilnEntity collect_to = 0;
    uint32_t collect_item = 0;
    uint32_t collect_flags = 0;
    uint64_t submitted = 0;
    // STOP after craft: what the station showed when it was taken
    bool waiting = false;
    int64_t start0 = 0;
    uint32_t selected0 = 0;
    uint64_t waiting_since = 0;
    uint8_t last_state = 0xff;  // the state seen the step before (a resume from Paused moves the start)
};
// Removes the requests of `list` that match and returns them, in order.
template <class P>
std::vector<Req> take_if(std::vector<Req>* list, P pred) {
    std::vector<Req> out;
    for (auto i = list->begin(); i != list->end();) {
        if (pred(*i)) {
            out.push_back(*i);
            i = list->erase(i);
        } else {
            ++i;
        }
    }
    return out;
}

inline bool is_deferred_stop(const KilnStationRequest& r) {
    return r.action == KILN_STATION_STOP && (r.flags & KILN_STATION_AFTER_CRAFT);
}

inline KilnStationResult result_for(const Req& q) {
    KilnStationResult r{};
    r.size = sizeof r;
    r.action = q.r.action;
    r.flags = q.r.flags;
    r.station = q.r.station;
    r.recipe = q.r.recipe;
    r.ticket = q.ticket;
    r.status = KILN_STATION_DONE;
    return r;
}

// One step of the program's requests, in order, then the stops that waited from before. act(q, res) runs q (it may
// change q: a stop's first look) and returns true when q is a stop that still waits; done(q, res) records an answer.
// A RESUME, SELECT or STOP now withdraws the stops waiting on its station (from before, or earlier in this step) once
// it has passed its checks (DONE or UNCHANGED), and says so in its message; a second stop after craft on a station
// with one waiting is UNCHANGED. Returns the stops still waiting, the older first.
template <class Act, class Done>
std::vector<Req> run_batch(const std::vector<Req>& batch, std::vector<Req> after, Act act, Done done) {
    auto on = [](KilnEntity st) { return [st](const Req& x) { return x.r.station == st; }; };
    std::vector<Req> fresh;  // this step's stops after craft that wait
    for (Req q : batch) {
        const KilnEntity st = q.r.station;
        if (is_deferred_stop(q.r) &&
            (std::any_of(after.begin(), after.end(), on(st)) || std::any_of(fresh.begin(), fresh.end(), on(st)))) {
            KilnStationResult res = result_for(q);
            res.status = KILN_STATION_UNCHANGED;
            std::snprintf(res.message, sizeof res.message, "a stop after the current craft already waits on station %u", st);
            done(q, res);
            continue;
        }
        KilnStationResult res = result_for(q);
        if (act(q, &res)) {
            fresh.push_back(q);
            continue;
        }
        if (withdraws_waiting(q.r) && (res.status == KILN_STATION_DONE || res.status == KILN_STATION_UNCHANGED)) {
            uint32_t withdrawn = 0;
            std::vector<Req> gone = take_if(&after, on(st));
            for (const Req& w : take_if(&fresh, on(st))) gone.push_back(w);
            for (const Req& w : gone) {
                KilnStationResult c = result_for(w);
                c.status = KILN_STATION_CANCELED;
                std::snprintf(c.message, sizeof c.message, "withdrawn by a later action on the station: nothing written");
                done(w, c);
                ++withdrawn;
            }
            if (withdrawn) {
                const size_t n = std::strlen(res.message);
                std::snprintf(res.message + n, sizeof res.message - n, "%swithdrew the stop that waited for the cycle's end",
                              n ? "; " : "");
            }
        }
        done(q, res);
    }
    std::vector<Req> waiting;
    for (Req q : after) {
        KilnStationResult res = result_for(q);
        if (act(q, &res)) waiting.push_back(q);
        else done(q, res);
    }
    waiting.insert(waiting.end(), fresh.begin(), fresh.end());
    return waiting;
}

// The watch's station scan: the first, every kScanEvery steps, and at once after the bases were rebuilt (a station's
// base index changes with them).
inline bool scan_due(bool scanned, uint64_t step, uint64_t last_scan, uint64_t every, bool bases_changed) {
    return !scanned || bases_changed || step - last_scan >= every;
}

// A waiting stop's look at its station this step (updates what it remembers).
inline StopWait waiting_stop_step(Req* q, const StopSeen& s, uint64_t waited, uint64_t max_steps) {
    // Paused -> Running is the same cycle resuming: the game moved its start (now - what it had run); a new cycle
    // follows a state with no cycle in progress, which this stop answers first (Write)
    if (s.present && q->last_state == KILN_STATION_STATE_PAUSED && s.state == KILN_STATION_STATE_RUNNING) q->start0 = s.start;
    const StopWait w = waiting_stop(s, q->selected0, q->start0, waited, max_steps);
    q->last_state = s.state;
    return w;
}

// ---- a base's id ----
// The smallest of the altars' ids (16 bytes compared in memory order); all zero ids are skipped.
inline KilnBaseId smallest_id(const KilnBaseId* ids, uint32_t n) {
    KilnBaseId best{};
    bool have = false;
    static const KilnBaseId zero{};
    for (uint32_t i = 0; i < n; ++i) {
        if (std::memcmp(ids[i].guid, zero.guid, 16) == 0) continue;
        if (!have || std::memcmp(ids[i].guid, best.guid, 16) < 0) best = ids[i];
        have = true;
    }
    return best;
}

// ---- the state diff behind the events ----
struct Snap {
    uint8_t state = 0, stopped = 0;
    uint32_t selected = 0, running = 0, workshop = 0;
    int32_t base = -1;
    KilnVec3 pos{};
    bool operator==(const Snap& o) const {
        return state == o.state && stopped == o.stopped && selected == o.selected && running == o.running;
    }
};

inline KilnStationEvent state_event(KilnEntity e, const Snap& before, const Snap& after) {
    KilnStationEvent ev{};
    ev.size = sizeof ev;
    ev.kind = KILN_STATION_EV_STATE;
    ev.station = e;
    ev.workshop = after.workshop;
    ev.state_before = before.state;
    ev.state_after = after.state;
    ev.stopped_before = before.stopped;
    ev.stopped_after = after.stopped;
    ev.selected_before = before.selected;
    ev.selected_after = after.selected;
    ev.running_before = before.running;
    ev.running_after = after.running;
    ev.base_index = after.base;
    ev.position = after.pos;
    return ev;
}

// The last 1024 events, numbered from 1.
class EventRing {
public:
    static constexpr uint32_t kCap = 1024;
    void push(KilnStationEvent ev) {
        ev.seq = ++seq_;
        buf_.push_back(ev);
        if (buf_.size() > kCap) buf_.pop_front();
    }
    uint64_t last() const { return seq_; }
    // Events with seq > after, oldest first; returns the number written; *more when more wait.
    uint32_t read(uint64_t after, KilnStationEvent* out, uint32_t capacity, bool* more) const {
        *more = false;
        uint32_t n = 0;
        if (buf_.empty()) return 0;
        const uint64_t first = buf_.front().seq;
        size_t i = after + 1 > first ? static_cast<size_t>(after + 1 - first) : 0;
        for (; i < buf_.size(); ++i) {
            if (n == capacity) {
                *more = true;
                break;
            }
            out[n++] = buf_[i];
        }
        return n;
    }
    void clear() { buf_.clear(); }  // seq_ keeps counting: a reader's `after` stays valid

private:
    std::deque<KilnStationEvent> buf_;
    uint64_t seq_ = 0;
};

// ---- status ----
// What kiln.stations reports as on. `live`: a server world runs here; `program`: kiln_items_stations is registered and
// not broken. The reads and the actions need the world; the events do not: a mod subscribes in OnLoad, before any world
// runs (and a game client that hosts later gets its world after that), and the watch only diffs while a world runs.
struct StatusBits {
    bool read = false, fill = false, control = false, events = false;
};
inline StatusBits status_bits(bool read_ok, bool fill_ok, bool control_ok, bool events_ok, bool live, bool program) {
    StatusBits b;
    b.read = read_ok && live;
    b.fill = fill_ok && program && live;
    b.control = control_ok && program && live;
    b.events = events_ok && read_ok;
    return b;
}

// ---- SELECT's unlock check ----
// The verdict from the answers: the world's (-1 unknown / not applicable, 0, 1) and the players' in game.
enum class Unlock { Yes, No, Unknown };
inline Unlock unlock_verdict(bool always, int world, const std::vector<int>& players) {
    if (always) return Unlock::Yes;
    if (world == 1) return Unlock::Yes;
    bool known = world == 0;
    for (int p : players) {
        if (p == 1) return Unlock::Yes;
        if (p == 0) known = true;
    }
    return known ? Unlock::No : Unlock::Unknown;
}

}  // namespace kitems::stations
