// SPDX-License-Identifier: MIT
// kiln.content: the "gear" section and "gearRules" (gear.h). docs/kiln/services/content.md "Gear values".
#include "gear.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <iterator>

namespace kc::gear {

int base_power(int level) {
    const float x = kSeed * kCreep * static_cast<float>(level) + kSeed;  // float, as the game (and Kiln.Blocks) computes it
    return static_cast<int>(static_cast<long long>(x));
}

int power_of(double dmg_mod, double speed_mod, int level) {
    const float x = static_cast<float>(base_power(level)) * static_cast<float>(dmg_mod) * static_cast<float>(speed_mod);
    return x <= 0 ? 0 : static_cast<int>(x);
}

double dmg_mod_for(int power, int level, double speed_mod) { return (power + 0.5) / (base_power(level) * speed_mod); }

int armor_of(double share, int level) {
    return static_cast<int>(std::lround((kArmorGradient * static_cast<float>(level) + kArmorOffset) * static_cast<float>(share)));
}

double armor_share_for(int armor, int level) { return armor / (static_cast<double>(kArmorGradient) * level + kArmorOffset); }

namespace {

struct Kind {
    const char* key;
    const char* field;
    bool share;  // a share of the damage (float); else a whole number the game keeps per kind (wood, stone, ...)
    bool hit;    // counts in the shares that make up a hit
};
// keen::impact::DamageDistribution (the game's reflection), under damageSetup.distribution
constexpr Kind kDamage[] = {
    {"cut", "physicalCutDamage", true, true},         {"pierce", "physicalPierceDamage", true, true},
    {"blunt", "physicalBluntDamage", true, true},     {"fire", "magicalFireDamage", true, true},
    {"ice", "magicalIceDamage", true, true},          {"shroud", "magicalFogDamage", true, true},
    {"fog", "magicalFogDamage", true, true},          {"lightning", "magicalLightningDamage", true, true},
    {"poison", "poisonDamage", true, true},           {"healing", "healing", true, false},
    {"wood", "woodDamage", false, false},             {"stone", "stoneDamage", false, false},
    {"metal", "metalDamage", false, false},           {"explosion", "explosionDamage", false, false},
};
// keen::ItemArmorSetup's distribution (Kiln.Blocks' armour block writes the same paths)
constexpr Kind kArmor[] = {
    {"physical", "physical", true, false}, {"magical", "magical", true, false}, {"cut", "cut", true, false},
    {"pierce", "pierce", true, false},     {"blunt", "blunt", true, false},     {"fire", "fire", true, false},
    {"ice", "ice", true, false},           {"shroud", "fog", true, false},      {"fog", "fog", true, false},
    {"lightning", "lightning", true, false},
};
constexpr const char* kRarities[] = {"Common", "Uncommon", "Rare", "Epic", "Legendary"};
constexpr const char* kRarityKeys[] = {"common", "uncommon", "rare", "epic", "legendary"};

constexpr const char* kGearKeys[] = {"power", "damage", "speed", "armor",   "block",  "staminaCost", "rarity",
                                     "levels", "rolled", "perks", "socket", "maxStack", "recipe"};

std::string lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

std::string kinds_list(const Kind* k, size_t n) {
    std::string s;
    for (size_t i = 0; i < n; ++i) {
        if (!std::string(k[i].key).compare("fog")) continue;  // an alias of shroud
        s += (s.empty() ? "" : ", ") + std::string(k[i].key);
    }
    return s;
}

// A number, or an op relative to base: {"set": n} | {"mul", "add", "min", "max", "raiseOnly", "lowerOnly"}. have_base false:
// only plain numbers and "set" (the item has no value of the game's to change).
bool number(const json& v, std::optional<double> base, double* out, std::string* err) {
    if (v.is_number()) {
        *out = v.get<double>();
        return true;
    }
    if (!v.is_object()) {
        *err = "a number or an op ({\"mul\": 1.2}, {\"add\": 5}, {\"set\": 30}, \"min\", \"max\", \"raiseOnly\", \"lowerOnly\")";
        return false;
    }
    for (auto& [k, x] : v.items())
        if (k != "set" && k != "mul" && k != "add" && k != "min" && k != "max" && k != "raiseOnly" && k != "lowerOnly") {
            *err = "unknown op \"" + k + "\"";
            return false;
        }
    if (v.contains("set")) {
        if (v.size() != 1 || !v["set"].is_number()) {
            *err = "\"set\" takes a number and stands alone";
            return false;
        }
        *out = v["set"].get<double>();
        return true;
    }
    if (!base) {
        *err = "an op needs the game's value, and this item has none: give a number";
        return false;
    }
    double r = *base, mul = 1, add = 0, lo = -INFINITY, hi = INFINITY;
    for (const char* k : {"mul", "add", "min", "max"})
        if (v.contains(k) && !v[k].is_number()) {
            *err = std::string("\"") + k + "\" must be a number";
            return false;
        }
    for (const char* k : {"raiseOnly", "lowerOnly"})
        if (v.contains(k) && !v[k].is_boolean()) {
            *err = std::string("\"") + k + "\" must be true or false";
            return false;
        }
    if (v.contains("mul")) mul = v["mul"].get<double>();
    if (v.contains("add")) add = v["add"].get<double>();
    if (v.contains("min")) lo = v["min"].get<double>();
    if (v.contains("max")) hi = v["max"].get<double>();
    r = std::clamp(r * mul + add, lo, hi);
    if (v.value("raiseOnly", false)) r = std::max(r, *base);
    if (v.value("lowerOnly", false)) r = std::min(r, *base);
    *out = r;
    return true;
}

// An op passed on to a field as it is (the field's own parse applies it), checked for its shape only.
bool op_shape(const json& v, std::string* err) {
    double d = 0;
    return number(v, 0.0, &d, err);
}

std::string fmt(double d) {
    char b[32];
    std::snprintf(b, sizeof b, "%.3g", d);
    return b;
}

}  // namespace

bool is_gear_key(const std::string& key) {
    for (const char* k : kGearKeys)
        if (key == k) return true;
    return false;
}

Expansion expand(const json& rule, const Reader& original) {
    Expansion x;
    if (!rule.is_object()) {
        x.errors.push_back("a gear rule is an object of values");
        return x;
    }
    const auto orig = [&](const std::string& p) -> std::optional<double> { return original ? original(p) : std::nullopt; };
    const auto fail = [&](const std::string& key, const std::string& why) { x.errors.push_back(key + ": " + why); };

    // raw item paths first, as in "items": the typed keys below win where both name one field
    for (auto& [k, v] : rule.items())
        if (!k.empty() && k[0] != '@' && !is_gear_key(k)) x.fields[k] = v;

    // levels: [min, max], [level] or a number (both)
    const int level0 = std::clamp(static_cast<int>(orig("itemLevelRange.minLevel").value_or(kMinLevel)), kMinLevel, 255);
    int level = level0;
    if (rule.contains("levels")) {
        const json& v = rule["levels"];
        std::vector<int64_t> ls;
        bool ok = true;
        if (v.is_number_integer()) ls = {v.get<int64_t>(), v.get<int64_t>()};
        else if (v.is_array() && (v.size() == 1 || v.size() == 2)) {
            for (const json& e : v) {
                if (!e.is_number_integer()) ok = false;
                else ls.push_back(e.get<int64_t>());
            }
            if (ok && ls.size() == 1) ls.push_back(ls[0]);
        } else
            ok = false;
        if (!ok) fail("levels", "a level (5) or [lowest, highest] ([1, 55]), whole numbers");
        else if (ls[0] < kMinLevel || ls[1] > kMaxLevel || ls[0] > ls[1])
            fail("levels", "[" + std::to_string(ls[0]) + ", " + std::to_string(ls[1]) + "] is not within the game's item levels 1..55, lowest first");
        else {
            level = static_cast<int>(ls[0]);
            x.fields["itemLevelRange.minLevel"] = ls[0];
            x.fields["itemLevelRange.maxLevel"] = ls[1];
        }
    }
    if (rule.contains("rolled")) {
        if (!rule["rolled"].is_boolean()) fail("rolled", "true (the game rolls the rarity of each new copy) or false");
        else x.fields["generateRarity"] = rule["rolled"];
    }
    if (rule.contains("rarity")) {
        const json& v = rule["rarity"];
        int r = -1;
        if (v.is_string())
            for (int i = 0; i < 5; ++i)
                if (lower(v.get<std::string>()) == kRarityKeys[i]) r = i;
        if (v.is_number_integer() && v.get<int64_t>() >= 0 && v.get<int64_t>() <= 4) r = static_cast<int>(v.get<int64_t>());
        if (r < 0) fail("rarity", "one of common, uncommon, rare, epic, legendary (the game has no rarity above legendary)");
        else x.fields["rarity"] = kRarities[r];
    }

    // speed and power: the tooltip's damage is int(base_power(level) * dmgMod * speedMod)
    const bool has_damage = orig("damageSetup.isSet").value_or(1) != 0;
    const double speed0 = orig("damageSetup.speedMod").value_or(1);
    double speed = speed0;
    if (rule.contains("speed")) {
        double s = 0;
        std::string e;
        if (!has_damage) fail("speed", "this item deals no damage of its own (damageSetup.isSet is off)");
        else if (!number(rule["speed"], speed0, &s, &e)) fail("speed", e);
        else if (!(s > 0 && s <= 100)) fail("speed", fmt(s) + " is outside 0..100 (more than 0)");
        else {
            speed = s;
            x.fields["damageSetup.speedMod"] = s;
        }
    }
    if (rule.contains("power")) {
        double p = 0;
        std::string e;
        const int power0 = power_of(orig("damageSetup.dmgMod").value_or(1), speed0, level0);
        if (!has_damage) fail("power", "this item deals no damage of its own (damageSetup.isSet is off)");
        else if (!number(rule["power"], static_cast<double>(power0), &p, &e)) fail("power", e);
        else if (std::llround(p) < 0 || std::llround(p) > 100000) fail("power", fmt(p) + " is outside 0..100000");
        else if (!(speed > 0)) fail("power", "the item's speedMod is 0: give \"speed\" too");
        else x.fields["damageSetup.dmgMod"] = dmg_mod_for(static_cast<int>(std::llround(p)), level, speed);
    }
    if (rule.contains("damage")) {
        const json& v = rule["damage"];
        if (!v.is_object()) fail("damage", "an object of kinds and shares ({\"cut\": 0.7, \"fire\": 0.3})");
        else if (!has_damage) fail("damage", "this item deals no damage of its own (damageSetup.isSet is off)");
        else {
            bool hit_given = false;
            for (auto& [k, val] : v.items()) {
                const Kind* kind = nullptr;
                for (const Kind& d : kDamage)
                    if (k == d.key) kind = &d;
                if (!kind) {
                    fail("damage." + k, "not a kind of damage; there are " + kinds_list(kDamage, std::size(kDamage)));
                    continue;
                }
                const std::string path = std::string("damageSetup.distribution.") + kind->field;
                double d = 0;
                std::string e;
                if (!number(val, orig(path), &d, &e)) {
                    fail("damage." + k, e);
                    continue;
                }
                if (d < 0 || (kind->share && d > 10) || (!kind->share && d > 1000000)) {
                    fail("damage." + k, fmt(d) + " is outside " + (kind->share ? "0..10 (a share: 0.5 is half)" : "0..1000000"));
                    continue;
                }
                hit_given = hit_given || kind->hit;
                if (kind->share) x.fields[path] = d;
                else x.fields[path] = static_cast<int64_t>(std::llround(d));
            }
            if (hit_given) {  // the shares a hit is made of, after this rule
                double sum = 0;
                for (const Kind& d : kDamage) {
                    if (!d.hit || !std::string(d.key).compare("fog")) continue;
                    const std::string path = std::string("damageSetup.distribution.") + d.field;
                    sum += x.fields.contains(path) ? x.fields[path].get<double>() : orig(path).value_or(0);
                }
                if (std::fabs(sum - 1) > 0.01)
                    x.warnings.push_back("damage: the shares of cut, pierce, blunt, fire, ice, shroud, lightning and poison add up to " +
                                         fmt(sum) + ", not 1 (Kiln's weapon blocks write shares that add up to 1)");
            }
        }
    }
    if (rule.contains("armor")) {
        const json& v = rule["armor"];
        if (!v.is_object()) fail("armor", "an object of kinds and the tooltip's armour ({\"physical\": 30, \"magical\": 12})");
        else if (orig("armorSetup.isSet").value_or(1) == 0) fail("armor", "this item has no armour values (armorSetup.isSet is off)");
        else
            for (auto& [k, val] : v.items()) {
                const Kind* kind = nullptr;
                for (const Kind& a : kArmor)
                    if (k == a.key) kind = &a;
                if (!kind) {
                    fail("armor." + k, "not a kind of armour; there are " + kinds_list(kArmor, std::size(kArmor)));
                    continue;
                }
                const std::string path = std::string("armorSetup.distribution.") + kind->field;
                const std::optional<double> share0 = orig(path);
                double a = 0;
                std::string e;
                if (!number(val, share0 ? std::optional<double>(armor_of(*share0, level0)) : std::nullopt, &a, &e)) {
                    fail("armor." + k, e);
                    continue;
                }
                if (std::llround(a) < 0 || std::llround(a) > 100000) {
                    fail("armor." + k, fmt(a) + " is outside 0..100000");
                    continue;
                }
                x.fields[path] = armor_share_for(static_cast<int>(std::llround(a)), level);
            }
    }
    if (rule.contains("block")) {
        double b = 0;
        std::string e;
        if (!number(rule["block"], orig("blockSetup.quality"), &b, &e)) fail("block", e);
        else if (b < 0 || b > 1) fail("block", fmt(b) + " is outside 0..1");
        else x.fields["blockSetup.quality"] = b;
    }
    if (rule.contains("staminaCost")) {
        const json& v = rule["staminaCost"];
        std::string e;
        if (v.is_number() && v.get<double>() < 0) fail("staminaCost", fmt(v.get<double>()) + " is less than 0");
        else if (!op_shape(v, &e)) fail("staminaCost", e);
        else x.fields["impactValues.StaminaCost"] = v;  // the item's own config value, as weapon blocks write it
    }
    if (rule.contains("maxStack")) {
        const json& v = rule["maxStack"];
        std::string e;
        if (v.is_number() && (v.get<double>() < 1 || v.get<double>() > 65535)) fail("maxStack", fmt(v.get<double>()) + " is outside 1..65535");
        else if (!op_shape(v, &e)) fail("maxStack", e);
        else x.fields["maxStackSize"] = v;
    }
    if (rule.contains("socket")) {
        if (!rule["socket"].is_boolean()) fail("socket", "true (new copies get a gem socket) or false");
        else x.fields["gemSlotOverride"] = rule["socket"];
    }
    if (rule.contains("perks")) {
        const json& v = rule["perks"];
        const bool ok = (v.is_array() && v.size() <= 5) || (v.is_string() && v.get<std::string>().rfind("item:", 0) == 0);
        if (!ok) fail("perks", "a list of up to 5 perk slots (a perk GUID or name, item:<name>, \"none\", null = keep), or \"item:<name>\"");
        else x.fields["perks"] = v;
    }
    if (rule.contains("recipe")) {
        const json& v = rule["recipe"];
        if (!v.is_object()) fail("recipe", "{\"inputs\": [...] or {\"mul\": x}, \"duration\": seconds}");
        else {
            json r = json::object();
            for (auto& [k, val] : v.items()) {
                if (k == "inputs") {
                    if (!val.is_array() && !val.is_object()) fail("recipe.inputs", "a list [{\"item\": ..., \"count\": n}] or {\"mul\": x}");
                    else r["inputs"] = val;
                } else if (k == "duration") {
                    std::string e;
                    if (val.is_number() && val.get<double>() < 0) fail("recipe.duration", "less than 0 seconds");
                    else if (!op_shape(val, &e)) fail("recipe.duration", e);
                    else r["craftingDuration"] = val;
                } else
                    fail("recipe." + k, "unknown; a gear rule's recipe has \"inputs\" and \"duration\" (other recipe fields: the recipes section)");
            }
            if (!r.empty()) x.recipe = std::move(r);
        }
    }
    return x;
}

json lower_rules(const json& rules, std::vector<std::string>* errors) {
    json set = json::object();
    const auto fail = [&](const std::string& key, const std::string& why) { errors->push_back("gearRules." + key + ": " + why); };
    if (!rules.is_object()) {
        errors->push_back("gearRules: an object (rarityDurability, socketChance, durability, unlockCost)");
        return json::object();
    }
    // a value as a resource patch takes it: a plain number or {mul, add, min, max, raiseOnly, lowerOnly}; {"set": n} is n
    const auto value = [&](const std::string& key, const json& v, double lo, double hi, bool whole, const std::string& path) {
        if (v.is_null()) return;  // keep the game's
        std::string e;
        double d = 0;
        if (!number(v, 0.0, &d, &e)) return fail(key, e);
        if (v.is_object() && !v.contains("set")) {
            set[path] = v;
            return;
        }
        if (d < lo || d > hi) return fail(key, fmt(d) + " is outside " + fmt(lo) + ".." + fmt(hi));
        if (whole && d != std::floor(d)) return fail(key, fmt(d) + " is not a whole number");
        set[path] = whole ? json(static_cast<int64_t>(d)) : json(d);
    };
    // five values, one per rarity: a list (null = keep) or {"common": ..., "legendary": ...}
    const auto per_rarity = [&](const std::string& key, const json& v, double lo, double hi, const std::string& member) {
        if (v.is_array()) {
            if (v.size() > 5) return fail(key, "five values at most, common to legendary");
            for (size_t i = 0; i < v.size(); ++i) value(key + "[" + std::to_string(i) + "]", v[i], lo, hi, false, member + "." + kRarityKeys[i]);
            return;
        }
        if (!v.is_object()) return fail(key, "a list of five values, common to legendary, or {\"common\": ..., \"legendary\": ...}");
        for (auto& [k, x] : v.items()) {
            int r = -1;
            for (int i = 0; i < 5; ++i)
                if (lower(k) == kRarityKeys[i]) r = i;
            if (r < 0) fail(key + "." + k, "not a rarity (common, uncommon, rare, epic, legendary)");
            else value(key + "." + k, x, lo, hi, false, member + "." + kRarityKeys[r]);
        }
    };
    for (auto& [k, v] : rules.items()) {
        if (k.empty() || k[0] == '@' || k == "comment") continue;
        if (k == "rarityDurability") per_rarity(k, v, 0.01, 100, "rarityDurabilityMod");
        else if (k == "socketChance") per_rarity(k, v, 0, 1, "gemSlotProbability");
        else if (k == "durability") {
            if (!v.is_object()) {
                fail(k, "{\"start\": 145, \"growth\": 2}");
                continue;
            }
            for (auto& [m, x] : v.items()) {
                if (m == "start") value(k + ".start", x, 1, 100000, false, "durabilityStart");
                else if (m == "growth") value(k + ".growth", x, 0, 100000, false, "durabilityGrowth");
                else fail(k + "." + m, "unknown; there are start (durability at level 1) and growth (more per level)");
            }
        } else if (k == "unlockCost") {
            if (!v.is_object()) {
                fail(k, "{\"seed\", \"creep\", \"weapons\", \"armor\", \"rings\", \"advanced\", \"advancedFrom\"}");
                continue;
            }
            for (auto& [m, x] : v.items()) {
                if (m == "seed") value(k + ".seed", x, 0, 100000, true, "perkUpgradeCostSeed");
                else if (m == "creep") value(k + ".creep", x, 0, 100, false, "perkCostCreep");
                else if (m == "weapons") value(k + ".weapons", x, 0, 100, false, "perkCostFactorWeaponsShield");
                else if (m == "armor") value(k + ".armor", x, 0, 100, false, "perkCostFactorArmor");
                else if (m == "rings") value(k + ".rings", x, 0, 100, false, "perkCostFactorRings");
                else if (m == "advanced") value(k + ".advanced", x, 0, 100000, false, "advancedPerkCostNominator");
                else if (m == "advancedFrom") value(k + ".advancedFrom", x, 0, 5, true, "advancedPerkCostStartLevel");
                else fail(k + "." + m, "unknown; there are seed, creep, weapons, armor, rings, advanced, advancedFrom");
            }
        } else if (k == "lootRarityRates")
            fail(k, "not supported yet: Kiln has not mapped the game resource that holds the loot rarity rates");
        else
            fail(k, "unknown; there are rarityDurability, socketChance, durability, unlockCost");
    }
    if (set.empty() && errors->empty()) errors->push_back("gearRules: nothing to change");
    if (!errors->empty()) return json::object();  // whole or not at all, as a resource patch is applied
    return json{{"format", 1}, {"resource", kBalancingTable}, {"type", kBalancingType}, {"set", std::move(set)}};
}

}  // namespace kc::gear
