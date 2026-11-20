// SPDX-License-Identifier: MIT
// kiln.content registry stage: the Kiln formulas (formulas.h), over gear.h's solver.
#include "formulas.h"

#include <cmath>

#include "schema.g.h"

namespace kc::reg::formulas {

gear::Constants constants_of(const std::map<std::string, double>& balance) {
    gear::Constants c;
    const auto take = [&](const char* k, float* v) {
        const auto it = balance.find(k);
        if (it != balance.end() && std::isfinite(it->second)) *v = static_cast<float>(it->second);
    };
    take("gear.seed", &c.seed);
    take("gear.creep", &c.creep);
    take("gear.armorOffset", &c.armor_offset);
    take("gear.armorGradient", &c.armor_gradient);
    return c;
}

int power_read(double dmg_mod, double speed_mod, int min_level, const gear::Constants& c) { return gear::power_of(dmg_mod, speed_mod, min_level, c); }
double power_write(int power, int min_level, double speed_mod, const gear::Constants& c) { return gear::dmg_mod_for(power, min_level, speed_mod, c); }
int armor_read(double share, int min_level, const gear::Constants& c) { return gear::armor_of(share, min_level, c); }
double armor_write(int armor, int min_level, const gear::Constants& c) { return gear::armor_share_for(armor, min_level, c); }

namespace {
// The items registry's level range (schema: levels.min / levels.max, 1..105: the game's gems reach 105), not the old gear
// section's 1..55: the formula holds for every level the field allows.
double level_limit(bool high) {
    for (const gen::Field& f : gen::kFields)
        if (std::string(f.registry) == "items" && std::string(f.path) == "levels.min") return high ? f.max : f.min;
    return high ? gear::kMaxLevel : gear::kMinLevel;
}
bool level_of(double v, int* out) {
    if (!std::isfinite(v) || v != std::floor(v) || v < level_limit(false) || v > level_limit(true)) return false;
    *out = static_cast<int>(v);
    return true;
}
bool whole(double v, int* out) {
    if (!std::isfinite(v) || v != std::floor(v) || v < 0 || v > 1e9) return false;
    *out = static_cast<int>(v);
    return true;
}
}  // namespace

bool known(const std::string& f) { return f == "gear.power" || f == "gear.armor"; }

bool read(const std::string& f, const std::vector<double>& in, const gear::Constants& c, double* out) {
    int level = 0;
    if (f == "gear.power") {
        if (in.size() != 3 || !level_of(in[2], &level) || !std::isfinite(in[0]) || !std::isfinite(in[1])) return false;
        return *out = power_read(in[0], in[1], level, c), true;
    }
    if (f == "gear.armor") {
        if (in.size() != 2 || !level_of(in[1], &level) || !std::isfinite(in[0])) return false;
        return *out = armor_read(in[0], level, c), true;
    }
    return false;
}

bool write(const std::string& f, double kiln, const std::vector<double>& in, const gear::Constants& c, double* out) {
    int level = 0, n = 0;
    if (!whole(kiln, &n)) return false;
    if (f == "gear.power") {
        if (in.size() != 3 || !level_of(in[2], &level) || !std::isfinite(in[1]) || in[1] <= 0) return false;
        if (gear::base_power(level, c) <= 0) return false;
        return *out = power_write(n, level, in[1], c), true;
    }
    if (f == "gear.armor") {
        if (in.size() != 2 || !level_of(in[1], &level)) return false;
        if (c.armor_gradient * static_cast<float>(level) + c.armor_offset <= 0) return false;
        return *out = armor_write(n, level, c), true;
    }
    return false;
}

}  // namespace kc::reg::formulas
