// SPDX-License-Identifier: MIT
#pragma once
namespace kiln::rules {
// Tool history already owns the exact charged/yielded list. Applying a player's building multipliers a second time
// on undo would duplicate refunds or waive yield takeback. Vanilla history keeps the existing multiplier policy.
inline bool scale_materials(float multiplier, bool building, bool exact_history) {
    return multiplier != 1.0f && building && !exact_history;
}
}
