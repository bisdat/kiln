// SPDX-License-Identifier: MIT
// The "gear" section's expansion and "gearRules"' lowering (gear.cpp), on the host: the tooltip's formulas solved and read back,
// ops relative to the game's values, the refusals with their reasons. No game, no store.
#include "../gear.h"

#include <cmath>
#include <iostream>
#include <map>

using kc::gear::json;

namespace {
int checks = 0, failed = 0;
void check(bool ok, const std::string& what) {
    ++checks;
    if (!ok) {
        ++failed;
        std::cerr << "FAIL " << what << '\n';
    }
}
bool near(double a, double b) { return std::fabs(a - b) < 1e-9; }
bool has(const std::vector<std::string>& v, const std::string& part) {
    for (const auto& s : v)
        if (s.find(part) != std::string::npos) return true;
    return false;
}
// An item's original values: a weapon at level 5 (dmgMod 1, speedMod 1, cut 1) or armour at level 10 (physical 0.18, magical 0.10)
kc::gear::Reader item(std::map<std::string, double> m) {
    return [m](const std::string& p) -> std::optional<double> {
        auto it = m.find(p);
        if (it == m.end()) return std::nullopt;
        return it->second;
    };
}
const std::map<std::string, double> kSword = {{"itemLevelRange.minLevel", 5},  {"itemLevelRange.maxLevel", 5},
                                              {"damageSetup.isSet", 1},        {"damageSetup.dmgMod", 1},
                                              {"damageSetup.speedMod", 1},     {"damageSetup.distribution.physicalCutDamage", 1},
                                              {"armorSetup.isSet", 0},         {"blockSetup.quality", 0.5}};
const std::map<std::string, double> kChest = {{"itemLevelRange.minLevel", 10}, {"itemLevelRange.maxLevel", 10},
                                              {"damageSetup.isSet", 0},         {"armorSetup.isSet", 1},
                                              {"armorSetup.distribution.physical", 0.18}, {"armorSetup.distribution.magical", 0.10}};
kc::gear::Expansion ex(const char* text, const std::map<std::string, double>& m) { return kc::gear::expand(json::parse(text), item(m)); }
}  // namespace

int main() {
    using namespace kc::gear;
    // the game's curve (the same numbers as Kiln.Blocks' Compiler.BasePower)
    check(base_power(1) == 11 && base_power(5) == 17 && base_power(10) == 25 && base_power(20) == 40 && base_power(55) == 92,
          "base power follows int(10 * 0.15 * level + 10)");
    bool round_trip = true;
    for (int level = 1; level <= 55; ++level)
        for (double speed : {0.8, 1.0, 1.25, 1.6})
            for (int p = 0; p <= 600; ++p) round_trip = round_trip && power_of(dmg_mod_for(p, level, speed), speed, level) == p;
    check(round_trip, "every power 0..600 at every level and speed reads back as written");
    bool armor_trip = true;
    for (int level = 1; level <= 55; ++level)
        for (int a = 0; a <= 400; ++a) armor_trip = armor_trip && armor_of(armor_share_for(a, level), level) == a;
    check(armor_trip, "every armour 0..400 at every level reads back as written");

    // power, at the item's level
    auto x = ex(R"({"power": 30})", kSword);
    check(x.errors.empty() && near(x.fields["damageSetup.dmgMod"].get<double>(), 30.5 / 17) && x.fields.size() == 1, "power 30 at level 5 is dmgMod 30.5 / 17");
    x = ex(R"({"power": {"mul": 2}})", kSword);
    check(x.errors.empty() && power_of(x.fields["damageSetup.dmgMod"].get<double>(), 1, 5) == 34, "power x2 of the game's 17 is 34");
    x = ex(R"({"power": {"add": 5, "max": 20}})", kSword);
    check(x.errors.empty() && power_of(x.fields["damageSetup.dmgMod"].get<double>(), 1, 5) == 20, "an op's max holds");
    x = ex(R"({"levels": [10, 12], "power": 30})", kSword);
    check(x.errors.empty() && x.fields["itemLevelRange.minLevel"] == 10 && x.fields["itemLevelRange.maxLevel"] == 12 &&
              power_of(x.fields["damageSetup.dmgMod"].get<double>(), 1, 10) == 30,
          "with levels the power is solved at the new lowest level");
    x = ex(R"({"levels": 20})", kSword);
    check(x.errors.empty() && x.fields["itemLevelRange.minLevel"] == 20 && x.fields["itemLevelRange.maxLevel"] == 20 && !x.fields.contains("damageSetup.dmgMod"),
          "a single level is both ends; without power the game's curve scales the damage");
    x = ex(R"({"speed": 1.25, "power": 30})", kSword);
    check(x.errors.empty() && near(x.fields["damageSetup.speedMod"].get<double>(), 1.25) &&
              power_of(x.fields["damageSetup.dmgMod"].get<double>(), 1.25, 5) == 30,
          "power is solved with the new speed");
    x = ex(R"({"power": 30})", kChest);
    check(has(x.errors, "power: this item deals no damage") && x.fields.empty(), "power on an item without damage is refused with the reason");

    // damage shares
    x = ex(R"({"damage": {"cut": 0.7, "fire": 0.3}})", kSword);
    check(x.errors.empty() && x.warnings.empty() && near(x.fields["damageSetup.distribution.physicalCutDamage"].get<double>(), 0.7) &&
              near(x.fields["damageSetup.distribution.magicalFireDamage"].get<double>(), 0.3),
          "damage shares go to the distribution");
    x = ex(R"({"damage": {"fire": 0.5}})", kSword);
    check(x.errors.empty() && has(x.warnings, "add up to 1.5"), "shares that do not add up to 1 are a warning (the game's cut 1 stays)");
    x = ex(R"({"damage": {"shroud": 1, "cut": 0, "wood": 3}})", kSword);
    check(x.errors.empty() && x.warnings.empty() && near(x.fields["damageSetup.distribution.magicalFogDamage"].get<double>(), 1) &&
              x.fields["damageSetup.distribution.woodDamage"] == 3,
          "shroud is the game's fog; wood is a whole number");
    x = ex(R"({"damage": {"frost": 1}})", kSword);
    check(has(x.errors, "damage.frost: not a kind of damage; there are cut, pierce"), "an unknown kind names the kinds there are");
    x = ex(R"({"damage": {"cut": -1}})", kSword);
    check(has(x.errors, "damage.cut: -1 is outside"), "a negative share is refused");

    // armour, in the tooltip's numbers at the item's level (gradient 15, offset 23: 173 at level 10)
    x = ex(R"({"armor": {"physical": 30, "magical": {"mul": 2}}})", kChest);
    check(x.errors.empty() && near(x.fields["armorSetup.distribution.physical"].get<double>(), 30.0 / 173) &&
              armor_of(x.fields["armorSetup.distribution.magical"].get<double>(), 10) == 34,
          "armour 30, and the game's magical 17 doubled");
    x = ex(R"({"levels": 20, "armor": {"physical": 30}})", kChest);
    check(x.errors.empty() && armor_of(x.fields["armorSetup.distribution.physical"].get<double>(), 20) == 30, "armour is solved at the new level");
    x = ex(R"({"armor": {"physical": 30}})", kSword);
    check(has(x.errors, "armor: this item has no armour values"), "armour on a weapon is refused");

    // the simple keys
    x = ex(R"({"rarity": "rare", "rolled": false, "socket": true, "maxStack": 1, "block": {"mul": 1.5}, "staminaCost": 12})", kSword);
    check(x.errors.empty() && x.fields["rarity"] == "Rare" && x.fields["generateRarity"] == false && x.fields["gemSlotOverride"] == true &&
              x.fields["maxStackSize"] == 1 && near(x.fields["blockSetup.quality"].get<double>(), 0.75) && x.fields["impactValues.StaminaCost"] == 12,
          "rarity, rolled, socket, maxStack, block, staminaCost map to their fields");
    x = ex(R"({"rarity": "Mythic"})", kSword);
    check(has(x.errors, "rarity: one of common") && has(x.errors, "no rarity above legendary"), "no rarity above legendary");
    x = ex(R"({"levels": [0, 60]})", kSword);
    check(has(x.errors, "levels: [0, 60] is not within the game's item levels 1..55"), "levels outside 1..55 are refused");
    x = ex(R"({"levels": [12, 10]})", kSword);
    check(has(x.errors, "lowest first"), "levels lowest first");
    x = ex(R"({"block": 2})", kSword);
    check(has(x.errors, "block: 2 is outside 0..1"), "block outside 0..1 is refused");
    x = ex(R"({"power": {"times": 2}})", kSword);
    check(has(x.errors, "unknown op \"times\""), "an unknown op is named");
    x = ex(R"({"perks": [null, null, "Perk_Damage_Ice", null, null]})", kSword);
    check(x.errors.empty() && x.fields["perks"].size() == 5, "perks go to the item's perk slots as in items");
    x = ex(R"({"perks": [1, 2, 3, 4, 5, 6]})", kSword);
    check(has(x.errors, "perks: a list of up to 5"), "six perks are refused");

    // raw paths pass through first; typed keys win
    x = ex(R"({"maxStackSize": 5, "damageSetup.dmgMod": 9, "power": 30, "@where": {"rarity": "Rare"}})", kSword);
    check(x.errors.empty() && x.fields.begin().key() == "maxStackSize" && near(x.fields["damageSetup.dmgMod"].get<double>(), 30.5 / 17) &&
              !x.fields.contains("@where"),
          "raw item paths pass through, the typed key wins, @-keys stay the caller's");

    // the recipe that makes the item
    x = ex(R"({"recipe": {"inputs": [{"item": "Material_T1_Twigs", "count": 2}], "duration": 5}})", kSword);
    check(x.errors.empty() && x.recipe["inputs"].size() == 1 && x.recipe["craftingDuration"] == 5 && x.fields.empty(), "recipe inputs and duration");
    x = ex(R"({"recipe": {"outputs": []}})", kSword);
    check(has(x.errors, "recipe.outputs: unknown"), "a recipe key it does not take is named");

    // gearRules -> a BalancingTable patch
    std::vector<std::string> errs;
    json p = lower_rules(json::parse(R"({"rarityDurability": [1, null, 1, 1.2, 1.5], "socketChance": {"Legendary": 0.5},
        "durability": {"start": 200, "growth": {"mul": 2}}, "unlockCost": {"seed": 4, "rings": 0.25, "advancedFrom": 3}})"), &errs);
    check(errs.empty() && p["resource"] == kBalancingTable && p["type"] == "keen::BalancingTable" && p["format"] == 1 &&
              p["set"]["rarityDurabilityMod.common"] == 1 && !p["set"].contains("rarityDurabilityMod.uncommon") &&
              p["set"]["rarityDurabilityMod.legendary"] == 1.5 && p["set"]["gemSlotProbability.legendary"] == 0.5 &&
              p["set"]["durabilityStart"] == 200 && p["set"]["durabilityGrowth"]["mul"] == 2 && p["set"]["perkUpgradeCostSeed"] == 4 &&
              p["set"]["perkCostFactorRings"] == 0.25 && p["set"]["advancedPerkCostStartLevel"] == 3,
          "gearRules lower to BalancingTable members (null keeps, ops pass on)");
    errs.clear();
    p = lower_rules(json::parse(R"({"lootRarityRates": {"common": 50}, "socketChance": [0, 0, 0, 0, 1.5], "speed": 1, "unlockCost": {"seed": 2.5}})"), &errs);
    check(has(errs, "gearRules.lootRarityRates: not supported yet") && has(errs, "gearRules.socketChance[4]: 1.5 is outside 0..1") &&
              has(errs, "gearRules.speed: unknown") && has(errs, "gearRules.unlockCost.seed: 2.5 is not a whole number") && p.empty(),
          "unsupported, out-of-range and unknown rules are refused with their names");
    errs.clear();
    p = lower_rules(json::parse(R"({"comment": "nothing"})"), &errs);
    check(p.empty() && has(errs, "nothing to change"), "an empty gearRules says so");

    std::cout << "gear: " << checks << " checks, " << failed << " failures\n";
    return failed ? 1 : 0;
}
