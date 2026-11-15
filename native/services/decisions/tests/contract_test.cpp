// SPDX-License-Identifier: MIT
// Wire layout of kiln.decisions, kiln.component_changes and kiln.window_clock (tests/run.sh). Sizes and offsets are
// the contract with the managed binding and with every native consumer: a change here is an ABI break.
#include <cstddef>
#include <cstdio>
#include <cstring>

#include "services/kiln_component_changes.h"
#include "services/kiln_decisions.h"
#include "services/kiln_window_clock.h"

static int g_pass = 0, g_fail = 0;
#define CHECK(x) ((x) ? ++g_pass : (++g_fail, std::printf("FAIL %s\n", #x)))

int main() {
    CHECK(sizeof(KilnWindowStateV1) == 48);
    CHECK(offsetof(KilnWindowStateV1, process_session) == 24);
    CHECK(offsetof(KilnWindowStateV1, world_thread) == 40);
    CHECK(sizeof(KilnEventEntityKey) == 40);
    CHECK(offsetof(KilnEventEntityKey, life_serial) == 16);
    CHECK(sizeof(KilnSetOpV1) == 40);
    CHECK(offsetof(KilnSetOpV1, instance_stamp) == 24);
    CHECK(sizeof(KilnDeathRequestV1) == 24);
    CHECK(sizeof(KilnPickupRequestV1) == 40);
    CHECK(sizeof(KilnSingleEquipRequestV1) == 56);
    CHECK(sizeof(KilnCollectionEquipRequestV1) == 56);
    CHECK(sizeof(KilnSetDecisionRequestV1) == 824);
    CHECK(sizeof(KilnBuildRequestV1) <= 1024);
    CHECK(sizeof(KilnDecisionRequestV1) == 1168);
    CHECK(offsetof(KilnDecisionRequestV1, request_id) == 16);
    CHECK(offsetof(KilnDecisionRequestV1, subject) == 48);
    CHECK(offsetof(KilnDecisionRequestV1, actor) == 88);
    CHECK(offsetof(KilnDecisionRequestV1, exposure_epoch) == 128);
    CHECK(offsetof(KilnDecisionRequestV1, payload) == 144);
    CHECK(sizeof(KilnDecisionInterestV1) == 40);
    CHECK(sizeof(KilnDecisionAnswerV1) == 32);
    CHECK(sizeof(KilnDecisionReceiptV1) == 208);
    CHECK(sizeof(KilnDecisionOutcomeV1) == 328);
    CHECK(offsetof(KilnDecisionOutcomeV1, subject) == 40);
    CHECK(offsetof(KilnDecisionOutcomeV1, requested_health) == 120);
    CHECK(offsetof(KilnDecisionOutcomeV1, reason) == 136);
    CHECK(sizeof(KilnDecisionsApiV1) == 8 + 22 * sizeof(void*));
    CHECK(offsetof(KilnDecisionsApiV1, window_begin) == 8 + 14 * sizeof(void*));
    // appended in dev.28 after window_end: readers of the first table (136 bytes) stop before it
    CHECK(offsetof(KilnDecisionsApiV1, set_death_guard) == 8 + 16 * sizeof(void*));
    CHECK(offsetof(KilnDecisionsApiV1, enqueue_native) == 8 + 17 * sizeof(void*));  // appended after set_death_guard
    // appended in dev.29 after enqueue_native: lazy producers and the core's window calls
    CHECK(offsetof(KilnDecisionsApiV1, prepare_kind) == 8 + 18 * sizeof(void*));
    CHECK(offsetof(KilnDecisionsApiV1, window_wanted) == 8 + 19 * sizeof(void*));
    CHECK(offsetof(KilnDecisionsApiV1, window_faulted) == 8 + 20 * sizeof(void*));
    CHECK(offsetof(KilnDecisionsApiV1, is_stopped) == 176);
    CHECK(sizeof(KilnComponentChangeV1) <= 2048);
    CHECK(sizeof(KilnComponentWatchStatusV1) == 224);
    CHECK(sizeof(KilnComponentChangesApiV1) == 8 + 13 * sizeof(void*));
    // two drops with the same SpawnTime are different keys
    KilnEventEntityKey a{}, b{};
    a.entity = 1024;
    b.entity = 1025;
    a.spawn_time = b.spawn_time = 99;
    CHECK(std::memcmp(&a, &b, sizeof a) != 0);
    std::printf("contract_test: %d PASS, %d FAIL\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
