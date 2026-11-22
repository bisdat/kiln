// SPDX-License-Identifier: MIT
// kiln.content: the "gear" section of data packs and the "gearRules" boot values (docs/kiln/services/content.md "Gear values").
// A typed face over item fields, in the numbers the game's own tooltip shows: expand() turns one gear rule into the item field
// paths kiln.content's "items" section takes, solved for one item (its level, its speed class); lower_rules() turns "gearRules"
// into a resource patch of the game's BalancingTable. Pure functions of the JSON and the item's original values: no game, no
// store, so they are tested on the host (tests/gear_test.cpp).
#pragma once
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

namespace kc::gear {

using json = nlohmann::ordered_json;

// The game's numbers the tooltip follows from (keen::BalancingTable 82706b40-...: +0 genericWeaponSeed, +0x28 powerCreepWeapon,
// +0x2a0 armorSetup offset / gradient). The same values as Kiln.Content's GearMath and Kiln.Blocks; checked against the running
// game by Kiln.Blocks (BlockRuntime, ItemChecks), which warns when an update changes them.
constexpr float kSeed = 10.0f, kCreep = 0.15f, kArmorOffset = 23.0f, kArmorGradient = 15.0f;
constexpr int kMinLevel = 1, kMaxLevel = 55;
constexpr char kBalancingTable[] = "82706b40-61b1-4b8f-8b23-dcec6971bda1";
constexpr char kBalancingType[] = "keen::BalancingTable";

// A gear rule's "perks" on a game item needs the references opt-in ("@refs": true or "options": {"refs": true}), as
// "perks" in the items section does: a vanilla client keeps the game's perks. One switch, so that the coordinator can lift it.
constexpr bool kPerksNeedRefs = true;

// The formulas' constants as one value: the game's own by default; the registry stage passes the balance registry's resolved
// gear.* values (registry/formulas.h), so that one solver serves both. The functions without it use the game's values.
struct Constants {
    float seed = kSeed, creep = kCreep, armor_offset = kArmorOffset, armor_gradient = kArmorGradient;
};

// The base of the game's damage power at a level: int(seed * creep * level + seed) (server 0x91e9d0), in float as the game.
int base_power(int level);
int base_power(int level, const Constants& c);
// The tooltip's damage: int(base_power(level) * dmgMod * speedMod).
int power_of(double dmg_mod, double speed_mod, int level);
int power_of(double dmg_mod, double speed_mod, int level, const Constants& c);
// The dmgMod that makes the tooltip show this power at this level and speed (half a point of margin, as Kiln.Blocks).
double dmg_mod_for(int power, int level, double speed_mod);
double dmg_mod_for(int power, int level, double speed_mod, const Constants& c);
// The tooltip's armour of one kind: round((gradient * level + offset) * share) (client 0xe6dfe0).
int armor_of(double share, int level);
int armor_of(double share, int level, const Constants& c);
double armor_share_for(int armor, int level);
double armor_share_for(int armor, int level, const Constants& c);

// The game's original number at an item field path; nullopt: this build's ItemInfo has no such field.
using Reader = std::function<std::optional<double>(const std::string& path)>;

struct Expansion {
    json fields = json::object();  // item field path -> value (a number, a bool, a name, an op, a list), applied as "items" applies it
    json recipe;                   // null, or a recipe rule ({"inputs", "craftingDuration"}) for every recipe that makes the item
    std::vector<std::string> errors, warnings;
};

// The keys a gear rule has besides raw item paths (which pass through as in "items").
bool is_gear_key(const std::string& key);

// One gear rule for one item. Keys starting with '@' are the caller's (@where, @refs).
Expansion expand(const json& rule, const Reader& original);

// "gearRules" -> a resource patch document {format, resource, type, set} of the BalancingTable. Whole or not at all: any error
// gives an empty object (and the errors), as a resource patch with one bad change is not applied.
json lower_rules(const json& rules, std::vector<std::string>* errors);

}  // namespace kc::gear
