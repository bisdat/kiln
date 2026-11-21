// SPDX-License-Identifier: MIT
// A new recipe's water (a data pack's recipe definition, "water"; C# sets Fields["waterCharges"]): the game's own recipe water,
// RecipeInfo.waterCharges, in charges per craft. One charge is the game's water unit times 64 (BalancingRegistry
// settings +0xFC); the recipe screen shows the charges as its "Water" row ("Water 92 / 2": 92 charges at hand, 2 a craft).
// The game's recipe payment (rules/RecipeApply, server 0x19a1d0) takes charges x units x crafts after the inputs: a factory
// station's start (flag 0x08, its own stock only) from the station's own tank (Fuel), a player's craft (stock mode 2)
// from the crafter and then the base's water sources (wells, barrels). RecipeInfo.waterAmount is not read there and is 0
// in all 2,542 recipes of the game: Kiln leaves it alone. Evidence: docs/re/stations.md section 3.6.
// Header only, so the host test (tests/recipe_water_test.cpp) checks what newcontent.cpp runs.
#pragma once

#include <cstdint>
#include <cstring>
#include <string>

namespace kc::recipe_water {

// The largest water a recipe may ask: 1,023 charges is the most a container shows players exactly (NetworkFuel is 16
// bits of units: docs/re/items.md section 11). The game's own recipes ask 1, 2, 5 or 10; its tanks hold 50 to 150.
inline constexpr int64_t kMax = 1023;

// "water" of a recipe definition: absent = -1 (the template's water: the base recipe's, none for a station's), else
// 0..kMax. false and *err on anything else.
template <class Json>
bool parse(const Json& j, int64_t* out, std::string* err) {
    *out = -1;
    if (!j.is_object() || !j.contains("water")) return true;
    const auto& w = j["water"];
    if (!w.is_number_integer() || w.template get<int64_t>() < 0 || w.template get<int64_t>() > kMax) {
        *err = "\"water\" must be a whole number of water charges per craft, 0..1023 (the station's tank water, as the "
               "recipe screen shows it)";
        return false;
    }
    *out = w.template get<int64_t>();
    return true;
}

// Writes the definition's water into a built RecipeInfo, after the template's requirements were copied or cleared.
// water < 0 leaves the template's. offset < 0: this build's RecipeInfo has no 4-byte waterCharges.
inline bool write(uint8_t* recipe, int64_t water, int64_t offset, uint32_t bytes, std::string* err) {
    if (water < 0) return true;
    if (offset < 0 || bytes != 4) {
        *err = "this game build's recipes have no 4-byte waterCharges: a recipe cannot ask for water";
        return false;
    }
    const uint32_t v = static_cast<uint32_t>(water);
    std::memcpy(recipe + offset, &v, 4);
    return true;
}

}  // namespace kc::recipe_water
