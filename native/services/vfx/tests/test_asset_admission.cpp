// SPDX-License-Identifier: MIT
// Runs the boot cook's actual option assignments with a tiny combined registry.
#include "cook/vfxcook.h"

#include <cstdio>

int main() {
    kvb::GameCounts g_game{};
    g_game.vfx = 1575;
    struct { struct { uint32_t admission, reserved; } b; } lb{{2048, 1}};
#include "asset_admission.inc"
    kvb::Budget budget(opt.game, opt.capacity);
    kvb::BudgetNeed pack;
    pack.vfx = 473;
    std::string reason;
    const bool accepted = budget.try_add(pack, &reason);
    const uint32_t type_list_count = g_game.vfx + lb.b.reserved + budget.used_vfx();
    if (accepted || type_list_count > lb.b.admission) {
        std::fprintf(stderr, "FAIL: %u original + %u raw asset + %u cooked = %u VFX reach a %u-entry registry\n",
                     g_game.vfx, lb.b.reserved, budget.used_vfx(), type_list_count, lb.b.admission);
        return 1;
    }
    pack.vfx = 472;
    if (reason != "VFX: needs 473, 472 left" || !budget.try_add(pack, nullptr) ||
        g_game.vfx + lb.b.reserved + budget.used_vfx() != lb.b.admission) {
        std::fprintf(stderr, "FAIL: a fitting pack must fill the registry, refusal was %s\n", reason.c_str());
        return 1;
    }
    std::puts("PASS VFX boot admission: 473 refused, 472 fit with one external VFX resource");
}
