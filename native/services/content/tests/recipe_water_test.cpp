// SPDX-License-Identifier: MIT
// A new recipe's "water" (recipe_water.h): what newcontent.cpp's define accepts, and where build_recipe writes it.
#include "../recipe_water.h"

#include <cstdio>
#include <cstring>
#include <vector>

#include "nlohmann/json.hpp"

static int fails = 0;
#define CHECK(c)                                                    \
    do {                                                            \
        if (!(c)) {                                                 \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            ++fails;                                                \
        }                                                           \
    } while (0)

using json = nlohmann::ordered_json;
namespace rw = kc::recipe_water;

static int64_t parsed(const char* text, bool* ok, std::string* err) {
    int64_t w = -2;
    err->clear();
    *ok = rw::parse(json::parse(text), &w, err);
    return w;
}

int main() {
    std::string err;
    bool ok = false;
    // absent: the template's water
    CHECK(parsed(R"({"station": "0xba67134f"})", &ok, &err) == -1 && ok);
    // whole charges 0..1023
    CHECK(parsed(R"({"water": 0})", &ok, &err) == 0 && ok);
    CHECK(parsed(R"({"water": 2})", &ok, &err) == 2 && ok);
    CHECK(parsed(R"({"water": 1023})", &ok, &err) == 1023 && ok);
    // refused: negative, too much, fractions, text, null
    for (const char* bad : {R"({"water": -1})", R"({"water": 1024})", R"({"water": 1.5})", R"({"water": "5"})", R"({"water": null})",
                            R"({"water": true})", R"({"water": 4294967296})"}) {
        parsed(bad, &ok, &err);
        CHECK(!ok);
        CHECK(err.find("\"water\"") != std::string::npos);
    }

    // build_recipe's order: the station template's requirements are cleared (waterAmount, waterCharges among them), then
    // the definition's water is written into waterCharges only. Offsets as on the game's 168-byte RecipeInfo.
    const int64_t amount_at = 0x94, charges_at = 0x98;
    std::vector<uint8_t> recipe(168, 0xAB);
    std::memset(recipe.data() + amount_at, 0, 4);
    std::memset(recipe.data() + charges_at, 0, 4);
    CHECK(rw::write(recipe.data(), 5, charges_at, 4, &err));
    uint32_t charges = 0, amount = 1;
    std::memcpy(&charges, recipe.data() + charges_at, 4);
    std::memcpy(&amount, recipe.data() + amount_at, 4);
    CHECK(charges == 5);
    CHECK(amount == 0);  // waterAmount: the game never reads a recipe's; Kiln leaves it as cleared or copied
    CHECK(recipe[charges_at + 4] == 0xAB && recipe[charges_at - 1] == 0);  // nothing else written
    // "water": 0 clears a base recipe's water
    std::memset(recipe.data() + charges_at, 0, 4);
    recipe[charges_at] = 10;
    CHECK(rw::write(recipe.data(), 0, charges_at, 4, &err));
    std::memcpy(&charges, recipe.data() + charges_at, 4);
    CHECK(charges == 0);
    // absent: the copied base water stays
    recipe[charges_at] = 10;
    CHECK(rw::write(recipe.data(), -1, charges_at, 4, &err));
    CHECK(recipe[charges_at] == 10);
    // a build without the field (or another size) refuses a recipe that asks for water, and accepts one that does not
    err.clear();
    CHECK(!rw::write(recipe.data(), 3, -1, 0, &err) && !err.empty());
    CHECK(!rw::write(recipe.data(), 3, charges_at, 2, &err));
    CHECK(rw::write(recipe.data(), -1, -1, 0, &err));

    if (fails) {
        std::printf("recipe-water-test: %d failure(s)\n", fails);
        return 1;
    }
    std::printf("recipe-water-test: ok\n");
    return 0;
}
