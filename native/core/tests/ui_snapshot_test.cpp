// SPDX-License-Identifier: MIT
// Host test of kiln.ui.snapshot v1 (src/services/ui_snapshot.cpp, abi/services/kiln_ui_snapshot.h) against stub worlds:
// a fake kiln.client session, a fake GetEntityTransform, a fake kiln.time and 05a's pre-provider stage driven by hand.
// Chapter 09 §6's regressions: an undersized copy writes no header and no record and the retry returns the same
// publication; a world change revokes the token; a stage and a Post of different steps publish unavailable, not a mix;
// more than 512 slots is Budget; a joined world gives HOST_TIME DeferredSource; zero demand reads nothing; one read per
// dependency per Post; fields older than 1000 ms are refused. Run by test-ui-snapshot.sh (ASan/UBSan).
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "../src/services/ui_snapshot.cpp"

using namespace kiln::uisnapshot;
namespace uiv = kiln::uivalues;

namespace {

int g_fail = 0, g_pass = 0;
void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    ok ? ++g_pass : ++g_fail;
}

std::atomic<uint64_t> g_ms{10000};  // the fake GetTickCount64 (reader threads read it too)

struct Fake {
    WorldNow w{};
    int transform_rc = kReadOk;
    int64_t xyz[3] = {5ll << 32, 7ll << 32, -(3ll << 32)};
    float orient[4] = {0, 0, 0, 1};
    float scale[3] = {1, 1, 1};
    uint64_t transform_calls = 0;
    uintptr_t last_world = 0;
    uint32_t last_entity = 0;
    int host_rc = kReadOk;
    double hours = 13.25;
    uint64_t host_calls = 0;
    KilnResult stage_rc = KILN_OK;
    uiv::StageFn fn = nullptr;
    void* user = nullptr;
    int subs = 0, unsubs = 0;
    std::set<KilnOwner> dead;
    std::vector<std::string> off;
};
Fake F;
std::mutex g_fake_mu;  // the concurrency case's acquire threads read F.w while the main thread steps

void wire() {
    Env& e = env();
    e.tick_ms = []() -> uint64_t { return g_ms.load(); };
    e.client_side = [] { return true; };
    e.owner_alive = [](KilnOwner o) { return F.dead.count(o) == 0; };
    e.now = [](WorldNow* out) {
        std::lock_guard lk(g_fake_mu);
        *out = F.w;
        return true;
    };
    e.read_transform = [](uintptr_t world, uint32_t entity, uint8_t* out) -> int {
        ++F.transform_calls;
        F.last_world = world;
        F.last_entity = entity;
        if (F.transform_rc != kReadOk) return F.transform_rc;
        std::memcpy(out, F.xyz, 24);
        std::memcpy(out + 24, F.orient, 16);
        std::memcpy(out + 40, F.scale, 12);
        std::memset(out + 52, 0, 4);
        return kReadOk;
    };
    e.host_hours = [](double* out) -> int {
        ++F.host_calls;
        if (F.host_rc == kReadOk) *out = F.hours;
        return F.host_rc;
    };
    e.stage_subscribe = [](uiv::StageFn fn, void* user, KilnHandle* out) {
        ++F.subs;
        if (F.stage_rc != KILN_OK) return F.stage_rc;
        F.fn = fn;
        F.user = user;
        *out = 0x77;
        return KilnResult(KILN_OK);
    };
    e.stage_unsubscribe = [](KilnHandle h) {
        ++F.unsubs;
        F.fn = nullptr;
        return h == 0x77 ? KilnResult(KILN_OK) : KilnResult(KILN_ERR_NOT_FOUND);
    };
    e.row_off = [](const char* n, uint32_t, const char*) { F.off.push_back(n); };
    e.log = [](int, const char*) {};
}

bool has_off(const char* name) {
    for (const auto& n : F.off)
        if (n == name) return true;
    return false;
}

void fresh(uint32_t state = KILN_SESSION_HOST) {
    reset_for_tests();
    F = Fake{};
    g_ms = 10000;
    wire();
    F.w.state = state;
    F.w.client_session = 1;
    F.w.server_session = state == KILN_SESSION_HOST ? 1 : 0;
    F.w.local_player = 7;
    F.w.client_world = 0x1000;
    on_world_event(KILN_WORLD_STARTED, KILN_WORLD_CLIENT, 1);
}

// 05a's stage of one step: rows (page 0, index i, item 100 + i), count = the step (a row tells its stage's step)
std::vector<uiv::StageRow> g_rows;
void stage(uint64_t step, uint32_t health = 413, uint32_t nrows = 3, uint32_t total = 0,
           uint32_t avail = uiv::kStageVitals | uiv::kStageInventory, uint64_t session = 1, uint32_t local = 7) {
    g_rows.clear();
    for (uint32_t i = 0; i < nrows; ++i)
        g_rows.push_back(uiv::StageRow{local, 0, i, 100 + i, static_cast<uint32_t>(step), 9000 + i});
    uiv::Stage s{};
    s.size = sizeof s;
    s.available = avail;
    s.local_player = local;
    s.health = health;
    s.health_max = 500;
    s.stamina = 90;
    s.stamina_max = 100;
    s.mana = 30;
    s.mana_max = 60;
    s.rows_total = total ? total : nrows;
    s.row_count = nrows;
    s.rows = g_rows.data();
    s.session = session;
    s.step = step;
    s.capture_ms = g_ms;
    if (F.fn) F.fn(&s, F.user);
}

KilnUiDemandResult demand_r(uint32_t mask, KilnOwner owner, KilnResult* rc) {
    KilnUiDemandResult r{};
    r.size = sizeof r;
    *rc = table()->demand_add(owner, mask, &r);
    return r;
}
KilnHandle demand(uint32_t mask, KilnOwner owner = 11) {
    KilnResult rc;
    const auto r = demand_r(mask, owner, &rc);
    return rc == KILN_OK && r.ok ? r.handle : 0;
}

struct Read {
    KilnResult rc = -1;
    KilnUiSnapshotAcquire a{};
    KilnUiWorldValues v{};
    std::vector<KilnUiInventoryRow> rows;
};
Read read(KilnOwner owner = 11) {
    Read r;
    r.a.size = sizeof r.a;
    r.rc = table()->acquire(owner, &r.a);
    if (r.rc != KILN_OK || !r.a.ok) return r;
    r.v.size = sizeof r.v;
    uint32_t req = 0;
    r.rows.resize(600);
    r.rc = table()->copy(r.a.token, &r.v, r.rows.data(), 600, &req);
    r.rows.resize(r.rc == KILN_OK ? req : 0);
    table()->release(r.a.token);
    return r;
}

void post(uint64_t step, uint64_t advance = 100) {
    g_ms += advance;
    on_client_post(step);
}

// One frame whose player-UI update runs before the client world's Post (a collection is due: 100 ms later).
void frame(uint64_t step, uint32_t health, uint32_t nrows, uint32_t total = 0,
           uint32_t avail = uiv::kStageVitals | uiv::kStageInventory) {
    g_ms += 100;
    stage(step, health, nrows, total, avail);
    on_client_post(step);
}

// ---------------------------------------------------------------------------------------------------------------------
void test_table_and_arguments() {
    fresh();
    const KilnUiSnapshotApi* t = table();
    check(t->header.size == sizeof(KilnUiSnapshotApi) && t->header.size == 48 && t->header.version == 1 &&
              t->demand_add && t->demand_remove && t->acquire && t->copy && t->release,
          "table: kiln.ui.snapshot v1, 48 bytes, five slots");
    KilnResult rc;
    check(table()->demand_add(11, KUW_LOCAL_VITALS, nullptr) == KILN_ERR_INVALID_ARGUMENT, "demand_add: null result");
    KilnUiDemandResult small{};
    small.size = sizeof small - 1;
    check(table()->demand_add(11, KUW_LOCAL_VITALS, &small) == KILN_ERR_INVALID_ARGUMENT, "demand_add: size below v1");
    demand_r(0, 11, &rc);
    check(rc == KILN_ERR_INVALID_ARGUMENT, "demand_add: mask 0");
    demand_r(16, 11, &rc);
    check(rc == KILN_ERR_INVALID_ARGUMENT, "demand_add: an unknown bit");
    check(table()->demand_remove(0) == KILN_ERR_INVALID_ARGUMENT && table()->demand_remove(12345) == KILN_ERR_NOT_FOUND,
          "demand_remove: 0 and an unknown handle");
    check(table()->acquire(11, nullptr) == KILN_ERR_INVALID_ARGUMENT, "acquire: null");
    KilnUiSnapshotAcquire a{};
    a.size = 8;
    check(table()->acquire(11, &a) == KILN_ERR_INVALID_ARGUMENT, "acquire: size below v1");
    KilnUiWorldValues v{};
    v.size = sizeof v;
    KilnUiInventoryRow row{};
    check(table()->copy(0, &v, &row, 1, nullptr) == KILN_ERR_INVALID_ARGUMENT &&
              table()->copy(5, nullptr, &row, 1, nullptr) == KILN_ERR_INVALID_ARGUMENT &&
              table()->copy(5, &v, nullptr, 1, nullptr) == KILN_ERR_INVALID_ARGUMENT,
          "copy: token 0, values null, rows null with capacity");
    v.size = sizeof v - 8;
    check(table()->copy(5, &v, &row, 1, nullptr) == KILN_ERR_INVALID_ARGUMENT, "copy: values size below v1");
    check(table()->release(0) == KILN_ERR_INVALID_ARGUMENT && table()->release(99) == KILN_ERR_NOT_FOUND,
          "release: 0 and an unknown token");
    env().client_side = [] { return false; };
    demand_r(KUW_LOCAL_VITALS, 11, &rc);
    a.size = sizeof a;
    check(rc == KILN_ERR_UNAVAILABLE && table()->acquire(11, &a) == KILN_ERR_UNAVAILABLE, "not a client: UNAVAILABLE");
}

void test_zero_demand_reads_nothing() {
    fresh();
    for (uint64_t s = 1; s <= 30; ++s) {
        post(s);
        on_server_post(s);
        stage(s);
    }
    const Read r = read();
    check(F.transform_calls == 0 && F.host_calls == 0 && F.subs == 0 && stats().stage_copies == 0 &&
              stats().publications == 0,
          "zero demand: no transform read, no kiln.time get, no stage observer, nothing published");
    check(r.rc == KILN_OK && r.a.ok == 0 && r.a.reason == KUW_NO_SNAPSHOT && r.a.token == 0,
          "zero demand: acquire answers NO_SNAPSHOT, token 0");
}

void test_demands() {
    fresh();
    KilnHandle hs[64];
    bool all = true;
    for (int i = 0; i < 64; ++i) all &= (hs[i] = demand(KUW_LOCAL_VITALS, 21)) != 0;
    KilnResult rc;
    const auto over = demand_r(KUW_LOCAL_VITALS, 21, &rc);
    check(all && rc == KILN_OK && over.ok == 0 && over.reason == KUW_BUDGET && over.handle == 0,
          "demand: 64 per owner, the 65th is KILN_OK + ok 0 + BUDGET");
    for (KilnOwner o = 100; o < 115; ++o)
        for (int i = 0; i < 64; ++i) demand(KUW_LOCAL_POSITION, o);
    const auto global = demand_r(KUW_HOST_TIME, 999, &rc);
    check(rc == KILN_OK && global.ok == 0 && global.reason == KUW_BUDGET, "demand: 1024 in all, then BUDGET");
    check(demand_union() == (KUW_LOCAL_VITALS | KUW_LOCAL_POSITION), "demand: the union of the live masks");
    for (KilnOwner o = 100; o < 115; ++o) {
        F.dead.insert(o);
        owner_retired(o);
    }
    check(demand_union() == KUW_LOCAL_VITALS, "demand: a revoked owner's handles go with it");
    for (int i = 0; i < 64; ++i) table()->demand_remove(hs[i]);
    check(demand_union() == 0 && table()->demand_remove(hs[0]) == KILN_ERR_NOT_FOUND,
          "demand_remove: the union empties; a second remove is NOT_FOUND");
    F.dead.insert(31);
    demand_r(KUW_LOCAL_VITALS, 31, &rc);
    check(rc == KILN_ERR_REVOKED, "demand_add: a revoked owner is REVOKED");
}

void test_position() {
    fresh();
    demand(KUW_LOCAL_POSITION);
    post(1);
    const Read r = read();
    check(F.transform_calls == 1 && F.last_world == 0x1000 && F.last_entity == 7,
          "position: GetEntityTransform(client world, local player) once");
    check(r.rc == KILN_OK && r.a.ok == 1 && r.v.available_mask == KUW_LOCAL_POSITION &&
              r.v.requested_mask == KUW_LOCAL_POSITION && r.v.x_fixed == (5ll << 32) && r.v.y_fixed == (7ll << 32) &&
              r.v.z_fixed == -(3ll << 32) && r.v.local_step == 1 && r.v.local_session == 1 && r.v.local_player == 7 &&
              r.v.local_capture_ms == g_ms && r.v.publication == r.a.publication && r.v.size == sizeof r.v,
          "position: published 32.32 x/y/z with the Post's session, step and capture time");
    check(r.v.unavailable_reason[0] == KUW_NO_SNAPSHOT && r.v.unavailable_reason[1] == KUW_NONE &&
              r.v.unavailable_reason[2] == KUW_NO_SNAPSHOT && r.v.unavailable_reason[3] == KUW_NO_SNAPSHOT &&
              r.v.health == 0 && r.v.inventory_count == 0 && r.rows.empty(),
          "position: undemanded groups are NO_SNAPSHOT and zero");
    F.transform_rc = kReadAbsent;
    post(2);
    check(read().v.unavailable_reason[1] == KUW_MISSING_COMPONENT, "position: a null transform is MISSING_COMPONENT");
    F.transform_rc = kReadOk;
    F.scale[1] = std::numeric_limits<float>::quiet_NaN();
    post(3);
    check(read().v.available_mask == 0 && read().v.unavailable_reason[1] == KUW_MISSING_COMPONENT,
          "position: a transform with a non-finite scale is refused (not a transform)");
    F.scale[1] = 1;
    F.w.local_player = 0;
    const uint64_t before = F.transform_calls;
    post(4);
    check(F.transform_calls == before && read().v.unavailable_reason[1] == KUW_NO_WORLD,
          "position: no local player: no read, NO_WORLD");
    F.w.local_player = 7;
    F.transform_rc = kReadFault;
    post(5);
    const uint64_t after = F.transform_calls;
    post(6);
    post(7);
    check(read().v.unavailable_reason[1] == KUW_LAYOUT_OFF && F.transform_calls == after && has_off("world.position"),
          "position: a faulting reader is switched off (LAYOUT_OFF, world.position reported, no further read)");
}

void test_one_read_per_dependency_per_post() {
    fresh();
    demand(KUW_LOCAL_POSITION | KUW_HOST_TIME);
    bool one = true, cadence = true;
    for (uint64_t s = 1; s <= 10; ++s) {
        const uint64_t t0 = F.transform_calls, h0 = F.host_calls;
        post(s);
        on_server_post(100 + s);
        one &= F.transform_calls == t0 + 1 && F.host_calls == h0 + 1;
        on_client_post(s);  // the same step again, within 100 ms: nothing
        on_server_post(100 + s);
        cadence &= F.transform_calls == t0 + 1 && F.host_calls == h0 + 1;
    }
    check(one, "one read per dependency per Post: one transform read per due client Post, one get per server Post");
    check(cadence, "cadence: a second Post within 100 ms reads nothing");
    uint64_t t0 = F.transform_calls;
    for (uint64_t s = 20; s < 30; ++s) post(s, 16);  // 60 Hz for 160 ms
    check(F.transform_calls - t0 <= 2 && F.transform_calls - t0 >= 1, "cadence: at most every 100 ms at 60 Hz");
    check(stats().publications == F.transform_calls, "every collection published once");
}

void test_steps_never_mix() {
    fresh();
    demand(KUW_LOCAL_POSITION | KUW_LOCAL_VITALS | KUW_LOCAL_INVENTORY);
    post(5);  // subscribes the stage; the sample of step 5 waits for its stage
    check(F.subs == 1 && stats().publications == 0, "steps: the Post's sample waits for the stage of its step");
    stage(4, 111);  // an older step's stage
    check(stats().publications == 0, "steps: a stage of another step does not complete the sample");
    g_ms += 300;
    on_client_post(6);  // still no stage of step 6; the grace (250 ms since due) has run out
    Read r = read();
    check(r.a.ok == 1 && r.v.available_mask == KUW_LOCAL_POSITION && r.v.local_step == 6 &&
              r.v.unavailable_reason[0] == KUW_STALE_SOURCE && r.v.unavailable_reason[2] == KUW_STALE_SOURCE &&
              r.v.health == 0 && r.v.inventory_count == 0 && r.rows.empty(),
          "steps: a stage and a Post of different steps publish vitals/inventory unavailable (STALE_SOURCE), not a mix");
    g_ms += 100;
    stage(10, 450);  // the update ran before the client world's Post in this frame
    on_client_post(10);
    r = read();
    check(r.v.available_mask == (KUW_LOCAL_POSITION | KUW_LOCAL_VITALS | KUW_LOCAL_INVENTORY) && r.v.local_step == 10 &&
              r.v.health == 450 && r.v.health_max == 500 && r.v.mana_max == 60 && r.v.inventory_count == 3 &&
              r.rows.size() == 3 && r.rows[0].count == 10 && r.rows[2].item == 102,
          "steps: stage and Post of the same step (stage first) publish one composite");
    post(11);
    check(read().v.local_step == 10, "steps: the Post of step 11 waits for its stage (nothing new yet)");
    stage(11, 460);  // the update ran after the Post
    r = read();
    check(r.v.local_step == 11 && r.v.health == 460 && r.rows.size() == 3 && r.rows[0].count == 11 &&
              (r.v.available_mask & KUW_LOCAL_POSITION),
          "steps: stage and Post of the same step (Post first) publish one composite");
    post(12);
    stage(13, 470);  // a later step's stage: not this sample's
    check(read().v.local_step == 11, "steps: a later step's stage does not complete an earlier sample");
    g_ms += 16;
    on_client_post(13);
    r = read();
    check(r.v.local_step == 13 && r.v.health == 470 && r.rows[0].count == 13,
          "steps: the next Post takes the waiting step over and matches its stage");
    g_ms += 100;
    stage(14, 480, 3, 0, uiv::kStageVitals | uiv::kStageInventory, 1, 8);  // another local player in the stage
    on_client_post(14);
    g_ms += 300;
    on_client_post(15);
    r = read();
    check(!(r.v.available_mask & KUW_LOCAL_VITALS) && r.v.unavailable_reason[0] == KUW_STALE_SOURCE,
          "steps: a stage of another local player never completes the sample");
    stage(15, 490, 3, 0, uiv::kStageVitals | uiv::kStageInventory, 2);  // another session's stage: ignored
    check(read().v.health != 490, "steps: a stage of another client session is ignored");
}

void test_no_position_uses_the_stage_step() {
    fresh();
    demand(KUW_LOCAL_VITALS);
    post(1);
    stage(1, 300);
    Read r = read();
    check(r.v.available_mask == KUW_LOCAL_VITALS && r.v.health == 300 && r.v.local_step == 1 && F.transform_calls == 0,
          "vitals only: published from the stage, no transform read");
    post(2);  // the stage of step 1 is the last one: no position to mix with, the stage stamps the local part
    r = read();
    check(r.v.local_step == 1 && r.v.health == 300, "vitals only: the stage's own step and capture stamp the local part");
    g_ms += 100;
    stage(3, 310, 0, 0, uiv::kStageInventory);  // vitals not in the stage
    on_client_post(3);
    check(read().v.unavailable_reason[0] == KUW_MISSING_COMPONENT, "vitals only: a stage without vitals is MISSING_COMPONENT");
}

void test_budget() {
    fresh();
    demand(KUW_LOCAL_INVENTORY);
    post(1);
    stage(1, 413, 512, 600);
    Read r = read();
    check(r.a.ok == 1 && !(r.v.available_mask & KUW_LOCAL_INVENTORY) && r.v.unavailable_reason[2] == KUW_BUDGET &&
              r.v.inventory_count == 0 && r.rows.empty(),
          "inventory: more than 512 slots is BUDGET with no partial list");
    frame(2, 413, 512, 512);
    r = read();
    check((r.v.available_mask & KUW_LOCAL_INVENTORY) && r.v.inventory_count == 512 && r.rows.size() == 512 &&
              r.rows[511].slot_index == 511,
          "inventory: 512 slots are published whole");
    frame(3, 413, 0, 0, uiv::kStageVitals);
    check(read().v.unavailable_reason[2] == KUW_MISSING_COMPONENT, "inventory: a stage without the models' layout");
    frame(4, 413, 0, 0);
    r = read();
    check((r.v.available_mask & KUW_LOCAL_INVENTORY) && r.v.inventory_count == 0,
          "inventory: an empty inventory is available with 0 rows");
}

void test_undersized_copy() {
    fresh();
    demand(KUW_LOCAL_INVENTORY);
    post(1);
    stage(1, 413, 3);
    KilnUiSnapshotAcquire a{};
    a.size = sizeof a;
    check(table()->acquire(11, &a) == KILN_OK && a.ok == 1 && a.token != 0, "copy: acquired a token");
    const uint64_t pinned = a.publication;
    KilnUiWorldValues v;
    std::memset(&v, 0xAB, sizeof v);
    v.size = sizeof v;
    KilnUiWorldValues untouched = v;
    KilnUiInventoryRow rows[8];
    std::memset(rows, 0xCD, sizeof rows);
    KilnUiInventoryRow rows_before[8];
    std::memcpy(rows_before, rows, sizeof rows);
    uint32_t required = 0;
    const KilnResult small = table()->copy(a.token, &v, rows, 2, &required);
    check(small == KILN_ERR_BUFFER_TOO_SMALL && required == 3, "copy: capacity 2 for 3 records: BUFFER_TOO_SMALL, required 3");
    check(std::memcmp(&v, &untouched, sizeof v) == 0 && std::memcmp(rows, rows_before, sizeof rows) == 0,
          "copy: an undersized copy writes neither the header nor any record");
    check(table()->copy(a.token, &v, nullptr, 0, nullptr) == KILN_ERR_BUFFER_TOO_SMALL &&
              std::memcmp(&v, &untouched, sizeof v) == 0,
          "copy: capacity 0 without required: BUFFER_TOO_SMALL and nothing written");
    post(2);
    stage(2, 499, 5);  // the collector publishes another one in between
    KilnUiSnapshotAcquire b{};
    b.size = sizeof b;
    table()->acquire(12, &b);
    check(b.ok == 1 && b.publication > pinned, "copy: a newer publication exists now");
    table()->release(b.token);
    const KilnResult again = table()->copy(a.token, &v, rows, 3, &required);
    check(again == KILN_OK && v.publication == pinned && v.inventory_count == 3 && required == 3 && rows[0].count == 1 &&
              rows[2].item == 102 && std::memcmp(&rows[3], &rows_before[3], sizeof rows[3]) == 0,
          "copy: the retry with the same token returns the same publication (header and records), nothing past them");
    check(table()->copy(a.token, &v, rows, 8, &required) == KILN_OK && v.publication == pinned,
          "copy: the pin holds for every copy of the token");
    check(table()->release(a.token) == KILN_OK && table()->release(a.token) == KILN_ERR_NOT_FOUND &&
              table()->copy(a.token, &v, rows, 8, &required) == KILN_ERR_NOT_FOUND,
          "release: once; a released token is NOT_FOUND");
    // the caller's size is kept; bytes past v1 untouched
    struct Bigger {
        KilnUiWorldValues v;
        uint64_t extra;
    } big;
    std::memset(&big, 0xEE, sizeof big);
    big.v.size = sizeof big;
    table()->acquire(11, &a);
    check(table()->copy(a.token, &big.v, rows, 8, &required) == KILN_OK && big.v.size == sizeof big &&
              big.extra == 0xEEEEEEEEEEEEEEEEull,
          "copy: a bigger record keeps its size and its bytes past v1");
    table()->release(a.token);
}

void test_world_change_revokes() {
    fresh();
    demand(KUW_LOCAL_POSITION);
    post(1);
    KilnUiSnapshotAcquire a{};
    a.size = sizeof a;
    table()->acquire(11, &a);
    KilnUiWorldValues v{};
    v.size = sizeof v;
    on_world_event(KILN_WORLD_STOPPING, KILN_WORLD_CLIENT, 1);
    check(table()->copy(a.token, &v, nullptr, 0, nullptr) == KILN_ERR_REVOKED && v.publication == 0,
          "world change: the client world's stop revokes the token within its TTL");
    check(table()->release(a.token) == KILN_OK, "world change: a revoked token can still be released");
    post(2);
    table()->acquire(11, &a);
    {
        std::lock_guard lk(g_fake_mu);
        F.w.client_session = 2;  // a rejoin that no event announced yet
        F.w.local_player = 9;
    }
    check(table()->copy(a.token, &v, nullptr, 0, nullptr) == KILN_ERR_REVOKED, "rejoin: a new client session revokes the token");
    const Read r = read();
    check(r.a.ok == 0 && r.a.reason == KUW_NO_SNAPSHOT, "rejoin: the old session's publication is gone");
    post(1);
    const Read n = read();
    check(n.a.ok == 1 && n.v.local_session == 2 && n.v.local_player == 9, "rejoin: the new session publishes its own");
    {
        std::lock_guard lk(g_fake_mu);
        F.w.state = KILN_SESSION_NONE;
    }
    const Read m = read();
    check(m.a.ok == 0 && m.a.reason == KUW_NO_WORLD, "loading screen / menu: NO_WORLD");
    {
        std::lock_guard lk(g_fake_mu);
        F.w.state = KILN_SESSION_HOST;
    }
    check(read().a.reason == KUW_NO_SNAPSHOT, "menu: the publication was cleared, not kept for the next world");
    fresh();
    demand(KUW_LOCAL_POSITION);
    post(1);
    table()->acquire(11, &a);
    on_world_event(KILN_WORLD_STARTED, KILN_WORLD_SERVER, 2);
    check(table()->copy(a.token, &v, nullptr, 0, nullptr) == KILN_ERR_REVOKED,
          "world change: a server world's start (hosting begins) revokes too");
}

void test_joined_host_time() {
    fresh(KILN_SESSION_REMOTE);
    KilnResult rc;
    const auto d = demand_r(KUW_HOST_TIME, 11, &rc);
    check(rc == KILN_OK && d.ok == 1 && d.handle != 0 && d.reason == KUW_DEFERRED_SOURCE,
          "joined: HOST_TIME demand is accepted with DEFERRED_SOURCE");
    for (uint64_t s = 1; s <= 5; ++s) {
        on_server_post(s);
        post(s);
    }
    const Read r = read();
    check(F.host_calls == 0, "joined: no kiln.time get (no hosted server world), no subscription");
    check(r.a.ok == 1 && r.v.available_mask == 0 && r.v.requested_mask == KUW_HOST_TIME &&
              r.v.unavailable_reason[3] == KUW_DEFERRED_SOURCE && r.v.server_hours == 0,
          "joined: HOST_TIME is DEFERRED_SOURCE in the publication");
}

void test_hosted_host_time() {
    fresh(KILN_SESSION_HOST);
    KilnResult rc;
    const auto d = demand_r(KUW_HOST_TIME | KUW_LOCAL_POSITION, 11, &rc);
    check(d.ok == 1 && d.reason == KUW_NONE, "hosted: HOST_TIME demand accepted");
    g_ms += 100;
    on_server_post(40);
    const uint64_t at = g_ms;
    post(3, 20);
    const Read r = read();
    check(F.host_calls == 1 && r.v.available_mask == (KUW_HOST_TIME | KUW_LOCAL_POSITION) && r.v.server_hours == 13.25 &&
              r.v.server_step == 40 && r.v.server_session == 1 && r.v.server_capture_ms == at && r.v.local_step == 3 &&
              r.v.local_capture_ms == at + 20,
          "hosted: the server world's hour with its own session, step and capture time");
    F.host_rc = kReadFault;
    g_ms += 100;
    on_server_post(41);
    post(4);
    check(read().v.unavailable_reason[3] == KUW_LAYOUT_OFF, "hosted: no kiln.time here is LAYOUT_OFF");
    F.host_rc = kReadAbsent;
    g_ms += 100;
    on_server_post(42);
    post(5);
    check(read().v.unavailable_reason[3] == KUW_NO_SNAPSHOT, "hosted: kiln.time refusing (no world yet) is NO_SNAPSHOT");
    F.host_rc = kReadOk;
    F.hours = std::numeric_limits<double>::infinity();
    g_ms += 100;
    on_server_post(43);
    post(6);
    check(read().v.unavailable_reason[3] == KUW_MISSING_COMPONENT, "hosted: a non-finite hour is never published");
    fresh(KILN_SESSION_NONE);
    demand(KUW_HOST_TIME);
    on_server_post(1);
    check(F.host_calls == 0, "menu: no server Post read");
}

void test_expiry() {
    fresh();
    demand(KUW_LOCAL_POSITION);
    post(1);
    check(read().a.ok == 1, "expiry: fresh publication readable");
    g_ms += 1000;
    check(read().a.ok == 1, "expiry: 1000 ms old is still readable");
    g_ms += 1;
    const Read r = read();
    check(r.a.ok == 0 && r.a.reason == KUW_EXPIRED && r.a.token == 0, "expiry: 1001 ms old is EXPIRED");
    fresh();
    demand(KUW_LOCAL_POSITION | KUW_HOST_TIME);
    g_ms += 100;
    on_server_post(10);
    post(1, 0);
    check(read().v.available_mask == (KUW_LOCAL_POSITION | KUW_HOST_TIME), "expiry: host time fresh");
    for (uint64_t s = 2; s <= 12; ++s) post(s);  // 1100 ms of client Posts, no server Post (the server world stalls)
    const Read h = read();
    check(h.a.ok == 1 && h.v.available_mask == KUW_LOCAL_POSITION && h.v.unavailable_reason[3] == KUW_EXPIRED &&
              h.v.server_hours == 0 && h.v.server_capture_ms == 0,
          "expiry: an hour older than 1000 ms is taken out of the view (EXPIRED), the local part stays");
}

void test_tokens() {
    fresh();
    demand(KUW_LOCAL_POSITION);
    post(1);
    KilnUiSnapshotAcquire t1{}, t2{}, t3{};
    t1.size = t2.size = t3.size = sizeof t1;
    table()->acquire(11, &t1);
    table()->acquire(11, &t2);
    table()->acquire(11, &t3);
    check(t1.ok == 1 && t2.ok == 1 && t3.ok == 0 && t3.reason == KUW_BUDGET, "tokens: 2 per owner, the third is BUDGET");
    table()->release(t1.token);
    table()->acquire(11, &t3);
    check(t3.ok == 1, "tokens: a release frees a place");
    table()->release(t2.token);
    table()->release(t3.token);
    std::vector<KilnHandle> held;
    for (KilnOwner o = 200; o < 264; ++o)
        for (int i = 0; i < 2; ++i) {
            KilnUiSnapshotAcquire a{};
            a.size = sizeof a;
            table()->acquire(o, &a);
            if (a.ok) held.push_back(a.token);
        }
    KilnUiSnapshotAcquire over{};
    over.size = sizeof over;
    table()->acquire(300, &over);
    check(held.size() == 128 && over.ok == 0 && over.reason == KUW_BUDGET, "tokens: 128 in all, then BUDGET");
    for (KilnHandle h : held) table()->release(h);
    KilnUiSnapshotAcquire a{};
    a.size = sizeof a;
    table()->acquire(11, &a);
    KilnUiWorldValues v{};
    v.size = sizeof v;
    g_ms += 2000;
    check(table()->copy(a.token, &v, nullptr, 0, nullptr) == KILN_OK, "tokens: 2000 ms old still copies");
    g_ms += 1;
    check(table()->copy(a.token, &v, nullptr, 0, nullptr) == KILN_ERR_NOT_FOUND,
          "tokens: after the 2 s TTL a copy is NOT_FOUND (an Expired refusal)");
    fresh();
    demand(KUW_LOCAL_POSITION);
    post(1);
    table()->acquire(11, &a);
    F.dead.insert(11);
    owner_retired(11);
    check(table()->copy(a.token, &v, nullptr, 0, nullptr) == KILN_ERR_REVOKED, "tokens: a revoked owner's token is REVOKED");
    check(table()->acquire(11, &a) == KILN_ERR_REVOKED, "acquire: a revoked owner is REVOKED");
    check(demand_union() == 0, "owner revoke: its demand went, the union is empty");
}

void test_stage_follows_demand() {
    fresh();
    const KilnHandle h = demand(KUW_LOCAL_VITALS);
    post(1);
    post(2);
    check(F.subs == 1 && F.fn != nullptr, "stage: subscribed once, when vitals are demanded");
    table()->demand_remove(h);
    post(3);
    check(F.unsubs == 1 && F.fn == nullptr, "stage: unsubscribed when the demand goes (05a walks no model for us)");
    fresh();
    F.stage_rc = KILN_ERR_UNAVAILABLE;
    demand(KUW_LOCAL_VITALS | KUW_LOCAL_INVENTORY | KUW_LOCAL_POSITION);
    post(1);
    post(2);
    const Read r = read();
    check(F.subs == 1 && r.v.unavailable_reason[0] == KUW_LAYOUT_OFF && r.v.unavailable_reason[2] == KUW_LAYOUT_OFF &&
              (r.v.available_mask & KUW_LOCAL_POSITION) && has_off("world.vitals") && has_off("world.inventory"),
          "stage refused (ui.values.hud off): vitals and inventory LAYOUT_OFF, rows reported, position still works");
}

void test_concurrent_readers_never_mix() {
    fresh();
    demand(KUW_LOCAL_POSITION | KUW_LOCAL_VITALS | KUW_LOCAL_INVENTORY);
    std::atomic<bool> stop{false};
    std::atomic<int> bad{0}, good{0};
    auto reader = [&](KilnOwner owner) {
        while (!stop.load()) {
            KilnUiSnapshotAcquire a{};
            a.size = sizeof a;
            if (table()->acquire(owner, &a) != KILN_OK || !a.ok) continue;
            KilnUiWorldValues v{};
            v.size = sizeof v;
            KilnUiInventoryRow rows[4];
            uint32_t req = 0;
            KilnResult rc = table()->copy(a.token, &v, rows, 1, &req);
            if (rc == KILN_ERR_BUFFER_TOO_SMALL) rc = table()->copy(a.token, &v, rows, 4, &req);
            table()->release(a.token);
            if (rc != KILN_OK) continue;
            bool ok = v.publication == a.publication && v.inventory_count == req;
            if (v.available_mask & KUW_LOCAL_INVENTORY)
                for (uint32_t i = 0; i < req; ++i) ok &= rows[i].count == static_cast<uint32_t>(v.local_step);
            if (v.available_mask & KUW_LOCAL_VITALS) ok &= v.health == 1000 + v.local_step;
            ok ? ++good : ++bad;
        }
    };
    std::thread a(reader, 51), b(reader, 52);
    for (uint64_t s = 1; s <= 3000; ++s) {
        g_ms += 40;
        if (s % 2) stage(s, static_cast<uint32_t>(1000 + s));
        on_client_post(s);
        if (!(s % 2)) stage(s, static_cast<uint32_t>(1000 + s));
    }
    stop = true;
    a.join();
    b.join();
    check(bad.load() == 0 && good.load() > 0, "concurrency: every copied publication is one step's (header = records)");
}

}  // namespace

int main() {
    test_table_and_arguments();
    test_zero_demand_reads_nothing();
    test_demands();
    test_position();
    test_one_read_per_dependency_per_post();
    test_steps_never_mix();
    test_no_position_uses_the_stage_step();
    test_budget();
    test_undersized_copy();
    test_world_change_revokes();
    test_joined_host_time();
    test_hosted_host_time();
    test_expiry();
    test_tokens();
    test_stage_follows_demand();
    test_concurrent_readers_never_mix();
    std::printf("ui_snapshot_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
