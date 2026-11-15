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
    // kiln.component_changes (built in dev.29, 07 S7): the S1 prefixes unchanged, the appended members after them
    CHECK(sizeof(KilnComponentWatchRequestV1) == 32);
    CHECK(offsetof(KilnComponentWatchRequestV1, entities) == 16);
    CHECK(offsetof(KilnComponentWatchStatusV1, watch_id) == 16);
    CHECK(offsetof(KilnComponentWatchStatusV1, reason) == 32);
    CHECK(sizeof(KilnComponentChangeV1) == 1808);
    CHECK(offsetof(KilnComponentChangeV1, entity) == 32);
    CHECK(offsetof(KilnComponentChangeV1, before_bytes) == 72);
    CHECK(offsetof(KilnComponentChangeV1, roots) == 104);
    CHECK(offsetof(KilnComponentChangeV1, watches) == 616);
    CHECK(offsetof(KilnComponentChangeV1, tokens) == 872);
    CHECK(offsetof(KilnComponentChangeV1, before) == 1128);
    CHECK(offsetof(KilnComponentChangeV1, after) == 1384);
    CHECK(offsetof(KilnComponentChangeV1, source_mod) == 1640);
    CHECK(offsetof(KilnComponentChangeV1, world_kind) == 1768);
    CHECK(offsetof(KilnComponentChangeV1, value_before) == 1776);
    CHECK(offsetof(KilnComponentChangeV1, since_step) == 1792);
    CHECK(offsetof(KilnComponentChangeV1, world_session) == 1800);
    CHECK(sizeof(KilnKnownWriteV1) == 112);
    CHECK(offsetof(KilnKnownWriteV1, entity) == 32);
    CHECK(offsetof(KilnKnownWriteV1, before) == 72);
    CHECK(offsetof(KilnKnownWriteV1, bytes) == 88);
    CHECK(offsetof(KilnKnownWriteV1, source_mod) == 96);
    CHECK(sizeof(KilnComponentChangesCountersV1) == 184);
    CHECK(offsetof(KilnComponentChangesCountersV1, world_epoch) == 32);
    CHECK(offsetof(KilnComponentChangesCountersV1, begin_ns_p50) == 128);
    CHECK(sizeof(KilnComponentChangesApiV1) == 8 + 17 * sizeof(void*));
    CHECK(offsetof(KilnComponentChangesApiV1, record_write) == 8 + 10 * sizeof(void*));
    CHECK(offsetof(KilnComponentChangesApiV1, window_end) == 8 + 12 * sizeof(void*));
    CHECK(offsetof(KilnComponentChangesApiV1, window_wanted) == 8 + 13 * sizeof(void*));  // appended
    CHECK(offsetof(KilnComponentChangesApiV1, window_faulted) == 8 + 14 * sizeof(void*));
    CHECK(offsetof(KilnComponentChangesApiV1, counters) == 8 + 15 * sizeof(void*));
    CHECK(offsetof(KilnComponentChangesApiV1, watched) == 8 + 16 * sizeof(void*));  // appended with the writers' filter
    CHECK(kiln_component_index("Health", 6) == KILN_WATCH_HEALTH && kiln_component_index("SlotSelection", 13) == KILN_WATCH_SLOT_SELECTION);
    CHECK(kiln_component_index("Healt", 5) == 0 && kiln_component_index("Oxygen", 6) == 0);
    // two drops with the same SpawnTime are different keys
    KilnEventEntityKey a{}, b{};
    a.entity = 1024;
    b.entity = 1025;
    a.spawn_time = b.spawn_time = 99;
    CHECK(std::memcmp(&a, &b, sizeof a) != 0);
    std::printf("contract_test: %d PASS, %d FAIL\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
