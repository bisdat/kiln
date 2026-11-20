// SPDX-License-Identifier: MIT
// kiln.content registry stage: the Kiln formulas between registry fields and the game's numbers (registry P1 slice S6; plan
// section 9.4). A regmap row with conv "formula" names one of them (regmap/common/items.json: items.power is "gear.power" over
// [damageSetup.dmgMod, damageSetup.speedMod, itemLevelRange.minLevel], items.armor.<kind> is "gear.armor" over
// [armorSetup.distribution.<kind>, itemLevelRange.minLevel]). They call gear.h's solver (one implementation, the one the
// data packs' "gear" section uses), parameterised by the balance registry's resolved gear.* constants instead of the
// game's values hard-coded there, so an edit of a constant moves the solver with it. Kiln.Content's GearMath (C#) gives the
// same numbers for the game's constants (pinned by tests on both sides). Plain functions: no game, no store.
#pragma once
#include <map>
#include <string>
#include <vector>

#include "../gear.h"

namespace kc::reg::formulas {

// The constants from the balance registry's resolved values ("gear.seed", "gear.creep", "gear.armorOffset",
// "gear.armorGradient"); a missing one keeps the game's own value.
gear::Constants constants_of(const std::map<std::string, double>& balance);

// items.power: the tooltip's damage at the item's lowest level.
int power_read(double dmg_mod, double speed_mod, int min_level, const gear::Constants& c);
double power_write(int power, int min_level, double speed_mod, const gear::Constants& c);
// items.armor.<kind>: the tooltip's armour of one kind at the item's lowest level.
int armor_read(double share, int min_level, const gear::Constants& c);
double armor_write(int armor, int min_level, const gear::Constants& c);

// By the regmap's formula name, the inputs in the row's path order (the first path is the member written).
bool known(const std::string& formula);
// The Kiln value from the game's numbers. false: an unknown formula or inputs it cannot use.
bool read(const std::string& formula, const std::vector<double>& inputs, const gear::Constants& c, double* kiln_value);
// The game's value of the first path for a Kiln value (the other inputs as they will be). false: an unknown formula or
// inputs that give no answer (speed 0, a level outside the game's item levels).
bool write(const std::string& formula, double kiln_value, const std::vector<double>& inputs, const gear::Constants& c, double* game_value);

}  // namespace kc::reg::formulas
