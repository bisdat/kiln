// SPDX-License-Identifier: MIT
// kiln.content registry stage: the Kiln formulas between registry fields and the game's numbers (registry/formulas.h), on
// the host. They call gear.h's solver (one implementation, no copy), parameterised by the balance registry's constants:
// with the game's own constants they give gear.h's numbers bit for bit; written and read back they give the Kiln value;
// other constants move them as the game's formula does. The damage rows' member names are checked against the committed
// type dump (sigdb/types), and a hash over 10,000 generated inputs pins the numbers that Kiln.Content's GearMath (C#) must
// give too (managed/tests/Kiln.Blocks.Tests GearAgreementTests).
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

#include "../gear.h"
#include "../registry/formulas.h"
#include "nlohmann/json.hpp"

using namespace kc;

// The hash of the 10,000 vectors below with the game's constants; GearAgreementTests (C#) pins the same number.
#ifndef KILN_GEAR_VECTORS_HASH
#define KILN_GEAR_VECTORS_HASH 0xa40975297674a479ull
#endif

static int g_fail = 0, g_pass = 0;
#define CHECK(c)                                                                  \
    do {                                                                          \
        if (c) ++g_pass;                                                          \
        else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } \
    } while (0)

static uint64_t splitmix(uint64_t& s) {
    uint64_t z = (s += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}
static void fnv(uint64_t& h, const void* p, size_t n) {
    const auto* b = static_cast<const unsigned char*>(p);
    for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 0x100000001b3ull;
}

int main(int argc, char** argv) {
    const gear::Constants game;  // the BalancingTable's own values
    // ---- the same numbers as gear.h's solver with the game's constants
    for (int level = 1; level <= 55; ++level) {
        CHECK(gear::base_power(level, game) == gear::base_power(level));
        for (int power : {0, 1, 17, 30, 250, 999}) {
            const double dm = reg::formulas::power_write(power, level, 1.25, game);
            CHECK(dm == gear::dmg_mod_for(power, level, 1.25));
            CHECK(reg::formulas::power_read(dm, 1.25, level, game) == gear::power_of(dm, 1.25, level));
            CHECK(reg::formulas::power_read(dm, 1.25, level, game) == power);  // written, read back
        }
        for (int armor : {0, 3, 40, 777}) {
            const double share = reg::formulas::armor_write(armor, level, game);
            CHECK(share == gear::armor_share_for(armor, level));
            CHECK(reg::formulas::armor_read(share, level, game) == armor);
        }
    }
    // ---- other constants move the numbers as the game's formula does
    gear::Constants twice = game;
    twice.seed = 20.0f;
    CHECK(gear::base_power(10, twice) == 2 * gear::base_power(10, game));  // int(20 * 0.15 * 10 + 20) = 50 = 2 * 25
    CHECK(reg::formulas::power_read(1.0, 1.0, 10, twice) == static_cast<int>(static_cast<float>(int(20.0f * 0.15f * 10 + 20.0f))));
    gear::Constants steep = game;
    steep.armor_gradient = 30.0f;
    CHECK(reg::formulas::armor_read(1.0, 2, steep) == 83);  // round(30 * 2 + 23)
    CHECK(reg::formulas::armor_read(1.0, 2, game) == 53);   // round(15 * 2 + 23)
    // ---- the balance registry's read-only gear fields carry the constants
    {
        const auto c = reg::formulas::constants_of({{"gear.seed", 12.0}, {"gear.creep", 0.2}, {"gear.armorOffset", 20.0}, {"gear.armorGradient", 16.0}});
        CHECK(c.seed == 12.0f && c.creep == 0.2f && c.armor_offset == 20.0f && c.armor_gradient == 16.0f);
        const auto d = reg::formulas::constants_of({});
        CHECK(d.seed == game.seed && d.creep == game.creep && d.armor_offset == game.armor_offset && d.armor_gradient == game.armor_gradient);
    }
    // ---- by the regmap's formula names, the inputs in the row's path order
    {
        double v = 0;
        CHECK(reg::formulas::known("gear.power") && reg::formulas::known("gear.armor") && !reg::formulas::known("gear.other"));
        // power: paths [dmgMod, speedMod, minLevel]
        CHECK(reg::formulas::read("gear.power", {gear::dmg_mod_for(42, 5, 1.0), 1.0, 5}, game, &v) && v == 42);
        CHECK(reg::formulas::write("gear.power", 42, {0, 1.0, 5}, game, &v) && v == gear::dmg_mod_for(42, 5, 1.0));
        // armor: paths [share, minLevel]
        CHECK(reg::formulas::write("gear.armor", 60, {0, 3}, game, &v) && v == gear::armor_share_for(60, 3));
        CHECK(reg::formulas::read("gear.armor", {v, 3}, game, &v) && v == 60);
        CHECK(!reg::formulas::read("gear.power", {1.0}, game, &v));            // too few inputs
        CHECK(!reg::formulas::write("gear.power", 42, {0, 0.0, 5}, game, &v)); // speed 0: no dmgMod gives a power
        CHECK(!reg::formulas::write("gear.power", 42, {0, 1.0, 0}, game, &v)); // level outside 1..55
        CHECK(!reg::formulas::read("gear.nothing", {1, 1, 1}, game, &v));
    }
    // ---- 10,000 generated inputs: the numbers GearMath (C#) must give as well
    {
        uint64_t s = 0x6b696c6e2d726567ull, h = 0xcbf29ce484222325ull;
        for (int i = 0; i < 10000; ++i) {
            const int level = 1 + static_cast<int>(splitmix(s) % 55);
            const int power = static_cast<int>(splitmix(s) % 2000);
            const double speed = 0.5 + static_cast<double>(splitmix(s) % 200) / 100.0;
            const double share = static_cast<double>(splitmix(s) % 1000) / 1000.0;
            const int armor = static_cast<int>(splitmix(s) % 500);
            const int32_t bp = gear::base_power(level);
            const double dm = gear::dmg_mod_for(power, level, speed);
            const int32_t back = gear::power_of(dm, speed, level);
            const int32_t ar = gear::armor_of(share, level);
            const float as = static_cast<float>(gear::armor_share_for(armor, level));  // the game keeps a float
            fnv(h, &bp, 4), fnv(h, &dm, 8), fnv(h, &back, 4), fnv(h, &ar, 4), fnv(h, &as, 4);
        }
        std::printf("gear vectors: fnv64 0x%016llx\n", static_cast<unsigned long long>(h));
        CHECK(h == KILN_GEAR_VECTORS_HASH);
    }
    // ---- the damage rows' members against the committed type dump (structure only)
    if (argc > 2) {
        std::ifstream f(argv[1]);
        std::stringstream ss;
        ss << f.rdbuf();
        const auto dump = nlohmann::json::parse(ss.str(), nullptr, false);
        std::ifstream g(argv[2]);
        std::stringstream rs;
        rs << g.rdbuf();
        const auto rows = nlohmann::json::parse(rs.str(), nullptr, false)["rows"];
        CHECK(!dump.is_discarded() && rows.is_object());
        const nlohmann::json* dd = nullptr;
        for (const auto& t : dump["types"])
            if (t["qualifiedName"] == "keen::impact::DamageDistribution") dd = &t;
        CHECK(dd != nullptr);
        int seen = 0;
        for (const auto& [field, row] : rows.items()) {
            if (field.rfind("damage.", 0) != 0) continue;
            const std::string path = row["paths"][0];
            const std::string member = path.substr(path.rfind('.') + 1);
            const std::string conv = row["conv"];
            bool found = false;
            for (const auto& m : (*dd)["fields"])
                if (m["name"] == member) found = (conv == "float") == (m["type"] == "keen::float32"), ++seen;
            if (!found) std::printf("  damage row %s: %s is not a %s member of keen::impact::DamageDistribution\n", field.c_str(), member.c_str(), conv.c_str());
            CHECK(found);
        }
        CHECK(seen == 13);
    }
    std::printf("registry-formulas-test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
