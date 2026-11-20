// SPDX-License-Identifier: MIT
// kiln.gameplay: which calls of the game's craft core become CRAFTED (ItemCraftedEvent). An automated station's cycle
// calls the core twice (start: inputs, flag 0x08; end: outputs, flag 0x10): it must be reported once, at the end.
#include "../craft_phase.h"

#include <cstddef>
#include <cstdio>

namespace {
int checks = 0, failed = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failed; std::printf("FAIL %d %s\n", __LINE__, #x); } } while (0)

struct Ctx {
    uint32_t op, station, workshop, pad;
    uint8_t flags;
    uint8_t rest[0x100];
};

Ctx make(uint32_t op, uint32_t station, uint8_t flags) {
    Ctx c{};
    c.op = op;
    c.station = station;
    c.workshop = 0x2546ae49u;  // charcoal kiln
    c.flags = flags;
    return c;
}
}  // namespace

int main() {
    const kg::CraftPhase phase{0x10, 0x10};  // the captured values of 26430253 / 26430625
    static_assert(offsetof(Ctx, flags) == 0x10);

    // an automated station (operator == station, not a player): one report per cycle, at its end
    Ctx start = make(1318, 1318, 0x08);
    Ctx start_all = make(1318, 1318, 0x08 | 0x40);  // "use all ingredient options": RecipePayAlt
    Ctx end = make(1318, 1318, 0x10);
    CHECK(!kg::craft_reported(&start.op, 0, phase));      // inputs taken: not a craft that made anything
    CHECK(!kg::craft_reported(&start_all.op, 0, phase));
    CHECK(kg::craft_reported(&end.op, 0, phase));          // outputs given: the cycle's one report
    CHECK(!kg::craft_reported(&start.op, 4, phase));       // missing materials, retried every step: never reported
    CHECK(!kg::craft_reported(&end.op, 2, phase));         // output full: retried, not reported

    // a player's craft at a bench and a request (operator chest, station bench): every outcome, whatever the flags
    Ctx player = make(3, 1318, 0x00);
    Ctx chest = make(1126, 1318, 0x00);
    CHECK(kg::craft_reported(&player.op, 0, phase));
    CHECK(kg::craft_reported(&player.op, 4, phase));
    CHECK(kg::craft_reported(&chest.op, 0, phase));
    CHECK(kg::craft_reported(&chest.op, 2, phase));
    // a player crafting by hand at itself (operator == station, a player id) is no factory
    Ctx hand = make(5, 5, 0x00);
    CHECK(kg::craft_reported(&hand.op, 0, phase));

    // an older sigdb without the phase: a factory's successes are reported as before (both calls)
    const kg::CraftPhase unknown{};
    CHECK(kg::craft_reported(&start.op, 0, unknown));
    CHECK(kg::craft_reported(&end.op, 0, unknown));
    CHECK(!kg::craft_reported(&start.op, 4, unknown));
    CHECK(!kg::craft_reported(nullptr, 0, phase));

    std::printf("craft_phase: %d checks, %d failures\n", checks, failed);
    return failed ? 1 : 0;
}
