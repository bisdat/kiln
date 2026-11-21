// SPDX-License-Identifier: MIT
// Host tests of kiln.client's building input (src/services/client_building_input.cpp):
// the members' refusals and argument rules, latest-id ownership, the producer's press and release on a stand-in
// ClientPlayerInput, cancel after press or dispatch, the exact-target dismantle, the 5 s timeout, producer stop and
// reload, session end, owner retirement, and the producer's thread rules (it never waits for a caller, allocates nothing,
// and callers only ever get a copied state). Run by test-client-building-input.sh (asan, tsan).
#include "services/client_building_input.h"

#include <sanitizer/allocator_interface.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace bi = kiln::building_input;
namespace {
int passed = 0, failed = 0;
void check(bool ok, const char* text, const std::string& detail = "") {
    std::printf("%s %s%s%s\n", ok ? "PASS" : "FAIL", text, detail.empty() ? "" : ": ", detail.c_str());
    ok ? ++passed : ++failed;
}

// The fake clock (ms).
std::atomic<uint64_t> g_now{1000};
uint64_t fake_clock() { return g_now.load(std::memory_order_relaxed); }

// Allocations on the calling thread while t_counting is set (the sanitizers' hook sees operator new too).
thread_local bool t_counting = false;
thread_local unsigned t_allocs = 0;
__attribute__((no_sanitize("address", "thread", "undefined"))) void on_malloc(const volatile void*, size_t) {
    if (t_counting) ++t_allocs;
}
__attribute__((no_sanitize("address", "thread", "undefined"))) void on_free(const volatile void*) {}

// The layout of keen::ecs::ClientPlayerInput on client 26430625 as its reflection describes it (sigdb/types/client/
// 5d9ef2f1...: ClientPlayerInputData.cursorInput 760, digitalInput 936, createBuildingItemAction 500,
// buildingStockCycleAction 160; PlayerInputType MainhandAction 0, _Tap 1, _Hold 2, _Release 3, SecondaryBuildingAction 4,
// ContextualAction 5, ContextualAction_Hold 7, BuildingUndo 40): the values sigdb's client.input_observation fixes.
constexpr bi::Layout kLayout{760, 936, 500, 160, 0, 1, 2, 3, 4, 5, 7, 40};
constexpr size_t kRecord = 1536;
constexpr uint64_t bit(unsigned n) { return uint64_t{1} << n; }

struct Transform {
    int64_t position[3];
    float rotation[4];
    float scale[3];
    uint32_t padding;
};
static_assert(sizeof(Transform) == 56);

// A stand-in ClientPlayerInput.
struct Record {
    alignas(8) std::array<uint8_t, kRecord> bytes{};
    uintptr_t base() const { return reinterpret_cast<uintptr_t>(bytes.data()); }
    uint64_t digital() const {
        uint64_t v;
        std::memcpy(&v, bytes.data() + kLayout.digital, 8);
        return v;
    }
    void set_digital(uint64_t v) { std::memcpy(bytes.data() + kLayout.digital, &v, 8); }
    Transform transform(int which) const {
        Transform t;
        std::memcpy(&t, bytes.data() + kLayout.cursor + 56 * which, sizeof t);
        return t;
    }
    uint32_t u32(size_t off) const {
        uint32_t v;
        std::memcpy(&v, bytes.data() + off, 4);
        return v;
    }
};
// The Memory given to the producer: plain copies with a write log and switchable faults.
struct Mem {
    bool fail_read = false, fail_write = false;
    std::vector<std::pair<size_t, size_t>> writes;  // (offset in the record, size), in order
    uintptr_t base = 0;
    bool log = false;
};
bool mem_read(void* ctx, void* dst, uintptr_t src, size_t n) {
    auto* m = static_cast<Mem*>(ctx);
    if (m && m->fail_read) return false;
    std::memcpy(dst, reinterpret_cast<const void*>(src), n);
    return true;
}
bool mem_write(void* ctx, uintptr_t dst, const void* src, size_t n) {
    auto* m = static_cast<Mem*>(ctx);
    if (m && m->fail_write) return false;
    if (m && m->log) m->writes.emplace_back(dst - m->base, n);
    std::memcpy(reinterpret_cast<void*>(dst), src, n);
    return true;
}

constexpr KilnOwner A = 11, B = 12;
constexpr KilnEntity kPlayer = 77;
constexpr uint32_t kSession = 3;
const bi::Env kEnv{true, true, true, kSession, nullptr};

KilnBuildingInput request(uint32_t action) {
    KilnBuildingInput in{};
    in.size = sizeof in;
    in.action = action;
    in.player = kPlayer;
    if (action == KILN_BUILDING_ACTION_SELECT) in.item_id = 4242;
    if (action == KILN_BUILDING_ACTION_DISMANTLE) in.target_entity_id = 991;
    in.position[0] = 1.5;
    in.position[1] = -0.25;
    in.position[2] = 100.0;
    in.rotation[3] = 2.0;  // normalized to (0, 0, 0, 1)
    in.scale[0] = in.scale[1] = in.scale[2] = 1.0;
    return in;
}

void fresh() {
    bi::reset_for_tests();
    bi::set_clock_for_tests(&fake_clock);
    g_now = 1000;
    bi::bind(&kLayout);
}

uint32_t state_of(KilnOwner owner, uint32_t id) {
    uint32_t s = 0xffff;
    return bi::status(kEnv, owner, id, &s) == KILN_OK ? s : 0xdead;
}
uint32_t submit_ok(KilnOwner owner, const KilnBuildingInput& in) {
    uint32_t id = 0;
    return bi::submit(kEnv, owner, 0, &in, &id) == KILN_OK ? id : 0;
}
void visit(Record& r, uint32_t version, Mem* mem = nullptr, KilnEntity entity = kPlayer, uint32_t session = kSession) {
    Mem local;
    Mem& m = mem ? *mem : local;
    m.base = r.base();
    bi::observe({r.base(), entity, session, version}, {&mem_read, &mem_write, &m});
}

void refusals() {
    fresh();
    KilnBuildingInput in = request(KILN_BUILDING_ACTION_PLACE);
    uint32_t id = 99;
    check(bi::submit(kEnv, A, 0, nullptr, &id) == KILN_ERR_INVALID_ARGUMENT && id == 0, "submit: no record is invalid, id 0");
    check(bi::submit(kEnv, A, 0, &in, nullptr) == KILN_ERR_INVALID_ARGUMENT, "submit: no id out is invalid");
    in.size = sizeof in - 1;
    id = 99;
    check(bi::submit(kEnv, A, 0, &in, &id) == KILN_ERR_INVALID_ARGUMENT && id == 0, "submit: a short record is invalid");
    in.size = sizeof in;
    check(bi::submit(kEnv, 0, 0, &in, &id) == KILN_ERR_INVALID_ARGUMENT, "submit: owner 0 is invalid");
    bi::Env env = kEnv;
    env.owner_alive = false;
    check(bi::submit(env, A, 0, &in, &id) == KILN_ERR_REVOKED, "submit: a revoked owner is refused");
    env = kEnv;
    env.client_side = false;
    check(bi::submit(env, A, 0, &in, &id) == KILN_ERR_UNAVAILABLE && id == 0, "submit: a dedicated server is unavailable");
    env = kEnv;
    env.hooks = false;
    check(bi::submit(env, A, 0, &in, &id) == KILN_ERR_UNAVAILABLE, "submit: safe idle is unavailable");
    bi::bind(nullptr);
    const char* why = nullptr;
    KilnBuildingInput bad = in;
    bad.action = 9;
    check(!bi::bound(&why) && why && std::strlen(why) > 20 && bi::submit(kEnv, A, 0, &in, &id) == KILN_ERR_UNAVAILABLE &&
              bi::submit(kEnv, A, 0, &bad, &id) == KILN_ERR_UNAVAILABLE,
          "submit: no producer bound is unavailable before any field rule, with a reason", why ? why : "");
    bi::bind(&kLayout);
    check(bi::bound(nullptr) && bi::submit(kEnv, A, 0, &bad, &id) == KILN_ERR_INVALID_ARGUMENT, "submit: bound, the field rules apply");
    env = kEnv;
    env.live_session = 0;
    check(bi::submit(env, A, 0, &in, &id) == KILN_CLIENT_ERR_BUILDING_STALE && id == 0, "submit: no live client world is stale");
    check(bi::submit(kEnv, A, kSession + 1, &in, &id) == KILN_CLIENT_ERR_BUILDING_STALE, "submit: another expected session is stale");
    check(bi::submit(kEnv, A, kSession, &in, &id) == KILN_OK && id == 1, "submit: the live session named explicitly is accepted");
    uint32_t second = 99;
    check(bi::submit(kEnv, B, 0, &in, &second) == KILN_CLIENT_ERR_BUILDING_BUSY && second == 0 &&
              bi::submit(kEnv, A, 0, &in, &second) == KILN_CLIENT_ERR_BUILDING_BUSY,
          "submit: one open request at a time, for every owner");

    uint32_t s = 7, c = 7;
    check(bi::status(kEnv, A, 1, nullptr) == KILN_ERR_INVALID_ARGUMENT && bi::cancel(kEnv, A, 1, nullptr) == KILN_ERR_INVALID_ARGUMENT,
          "status/cancel: no out is invalid");
    check(bi::status(kEnv, 0, 1, &s) == KILN_ERR_INVALID_ARGUMENT && s == 0 && bi::cancel(kEnv, 0, 1, &c) == KILN_ERR_INVALID_ARGUMENT &&
              c == 0,
          "status/cancel: owner 0 is invalid, outs zeroed");
    env = kEnv;
    env.client_side = false;
    s = c = 7;
    check(bi::status(env, A, 1, &s) == KILN_ERR_UNAVAILABLE && s == 0 && bi::cancel(env, A, 1, &c) == KILN_ERR_UNAVAILABLE && c == 0,
          "status/cancel: a dedicated server is unavailable");
    env = kEnv;
    env.hooks = false;
    check(bi::status(env, A, 1, &s) == KILN_ERR_UNAVAILABLE && bi::cancel(env, A, 1, &c) == KILN_ERR_UNAVAILABLE,
          "status/cancel: safe idle is unavailable");
    env = kEnv;
    env.owner_alive = false;
    check(bi::status(env, A, 1, &s) == KILN_ERR_REVOKED && bi::cancel(env, A, 1, &c) == KILN_ERR_REVOKED,
          "status/cancel: a revoked owner is refused");
    check(state_of(A, 1) == KILN_BUILDING_INPUT_QUEUED, "status: the refusals changed nothing");
}

void field_rules() {
    fresh();
    struct Case {
        const char* name;
        void (*edit)(KilnBuildingInput&);
        uint32_t action;
    };
    const Case invalid[] = {
        {"player 0", [](KilnBuildingInput& in) { in.player = 0; }, KILN_BUILDING_ACTION_PLACE},
        {"action 5", [](KilnBuildingInput& in) { in.action = 5; }, KILN_BUILDING_ACTION_PLACE},
        {"slot 256", [](KilnBuildingInput& in) { in.slot = 256; }, KILN_BUILDING_ACTION_SELECT},
        {"select without an item", [](KilnBuildingInput& in) { in.item_id = 0; }, KILN_BUILDING_ACTION_SELECT},
        {"dismantle without a target", [](KilnBuildingInput& in) { in.target_entity_id = 0; }, KILN_BUILDING_ACTION_DISMANTLE},
        {"a target on place", [](KilnBuildingInput& in) { in.target_entity_id = 5; }, KILN_BUILDING_ACTION_PLACE},
        {"a target on select", [](KilnBuildingInput& in) { in.target_entity_id = 5; }, KILN_BUILDING_ACTION_SELECT},
        {"reserved set", [](KilnBuildingInput& in) { in.reserved = 1; }, KILN_BUILDING_ACTION_PLACE},
        {"NaN rotation", [](KilnBuildingInput& in) { in.rotation[1] = std::nan(""); }, KILN_BUILDING_ACTION_PLACE},
        {"infinite rotation", [](KilnBuildingInput& in) { in.rotation[0] = INFINITY; }, KILN_BUILDING_ACTION_REMOVE},
        {"zero quaternion", [](KilnBuildingInput& in) { in.rotation[3] = 0; }, KILN_BUILDING_ACTION_PLACE},
        {"quaternion norm below 1e-12", [](KilnBuildingInput& in) { in.rotation[3] = 9.9e-7; }, KILN_BUILDING_ACTION_PLACE},
        {"quaternion norm overflow", [](KilnBuildingInput& in) { in.rotation[3] = 1e200; in.rotation[2] = 1e200; }, KILN_BUILDING_ACTION_PLACE},
        {"scale 0", [](KilnBuildingInput& in) { in.scale[2] = 0; }, KILN_BUILDING_ACTION_PLACE},
        {"scale above 1024", [](KilnBuildingInput& in) { in.scale[0] = 1024.0001; }, KILN_BUILDING_ACTION_DISMANTLE},
        {"NaN scale", [](KilnBuildingInput& in) { in.scale[1] = std::nan(""); }, KILN_BUILDING_ACTION_PLACE},
        {"NaN position", [](KilnBuildingInput& in) { in.position[1] = std::nan(""); }, KILN_BUILDING_ACTION_REMOVE},
        {"position 2^31 m (Q32.32 overflow)", [](KilnBuildingInput& in) { in.position[0] = 2147483648.0; }, KILN_BUILDING_ACTION_PLACE},
        {"position -2^31 m (refused at the minimum, as the source does)", [](KilnBuildingInput& in) { in.position[2] = -2147483648.0; }, KILN_BUILDING_ACTION_PLACE},
    };
    for (const Case& c : invalid) {
        KilnBuildingInput in = request(c.action);
        c.edit(in);
        uint32_t id = 99;
        check(bi::submit(kEnv, A, 0, &in, &id) == KILN_ERR_INVALID_ARGUMENT && id == 0, "rule refuses", c.name);
    }
    const Case valid[] = {
        {"slot 255", [](KilnBuildingInput& in) { in.slot = 255; }, KILN_BUILDING_ACTION_SELECT},
        {"select ignores a NaN transform", [](KilnBuildingInput& in) { in.position[0] = std::nan(""); in.rotation[3] = 0; in.scale[0] = 0; }, KILN_BUILDING_ACTION_SELECT},
        {"undo ignores a NaN transform", [](KilnBuildingInput& in) { in.position[0] = std::nan(""); in.rotation[3] = 0; }, KILN_BUILDING_ACTION_UNDO},
        {"scale 1024", [](KilnBuildingInput& in) { in.scale[0] = 1024; }, KILN_BUILDING_ACTION_PLACE},
        {"norm just above 1e-12", [](KilnBuildingInput& in) { in.rotation[3] = 1.0001e-6; }, KILN_BUILDING_ACTION_PLACE},
        {"position just below 2^31 m", [](KilnBuildingInput& in) { in.position[0] = 2147483647.75; }, KILN_BUILDING_ACTION_PLACE},
    };
    for (const Case& c : valid) {
        KilnBuildingInput in = request(c.action);
        c.edit(in);
        uint32_t id = 0;
        const bool ok = bi::submit(kEnv, A, 0, &in, &id) == KILN_OK && id != 0;
        uint32_t cancelled = 0;
        bi::cancel(kEnv, A, id, &cancelled);
        check(ok && cancelled == 1, "rule accepts", c.name);
    }
}

void conversion() {
    fresh();
    KilnBuildingInput in = request(KILN_BUILDING_ACTION_PLACE);
    in.position[0] = 1.5;
    in.position[1] = -0.25;
    in.position[2] = std::ldexp(1.0, -33);  // half a Q32.32 unit: llround rounds half away from zero
    in.rotation[0] = in.rotation[1] = in.rotation[2] = in.rotation[3] = 3.0;
    in.scale[0] = 0.1;
    in.scale[1] = 2.0;
    in.scale[2] = 1024.0;
    const uint32_t id = submit_ok(A, in);
    Record r;
    visit(r, 10);
    const Transform t = r.transform(0), u = r.transform(1);
    check(state_of(A, id) == KILN_BUILDING_INPUT_PRESSED, "convert: pressed");
    check(t.position[0] == 6442450944ll && t.position[1] == -1073741824ll && t.position[2] == 1,
          "convert: position is Q32.32, rounded half away from zero",
          std::to_string(t.position[0]) + " " + std::to_string(t.position[1]) + " " + std::to_string(t.position[2]));
    check(t.rotation[0] == 0.5f && t.rotation[1] == 0.5f && t.rotation[2] == 0.5f && t.rotation[3] == 0.5f,
          "convert: the quaternion is normalized");
    check(t.scale[0] == 0.1f && t.scale[1] == 2.0f && t.scale[2] == 1024.0f && t.padding == 0, "convert: scale as float, padding 0");
    check(std::memcmp(&t, &u, sizeof t) == 0, "convert: primary and secondary transforms are the same");
}

void latest_id() {
    fresh();
    const KilnBuildingInput in = request(KILN_BUILDING_ACTION_PLACE);
    const uint32_t id1 = submit_ok(A, in);
    check(id1 == 1 && state_of(A, id1) == KILN_BUILDING_INPUT_QUEUED, "latest: the first id is 1, queued");
    uint32_t c = 7;
    check(state_of(B, id1) == KILN_BUILDING_INPUT_UNKNOWN && bi::cancel(kEnv, B, id1, &c) == KILN_OK && c == 0 &&
              state_of(A, id1) == KILN_BUILDING_INPUT_QUEUED,
          "latest: another owner sees unknown and cannot cancel it");
    check(state_of(A, 0) == KILN_BUILDING_INPUT_UNKNOWN && state_of(A, id1 + 1) == KILN_BUILDING_INPUT_UNKNOWN &&
              bi::cancel(kEnv, A, 0, &c) == KILN_OK && c == 0,
          "latest: id 0 and an id never given are unknown");
    check(bi::cancel(kEnv, A, id1, &c) == KILN_OK && c == 1, "latest: its owner cancels it");
    const uint32_t id2 = submit_ok(B, in);
    check(id2 == id1 + 1 && state_of(B, id2) == KILN_BUILDING_INPUT_QUEUED, "latest: the next request takes the next id");
    check(state_of(A, id1) == KILN_BUILDING_INPUT_UNKNOWN && bi::cancel(kEnv, A, id1, &c) == KILN_OK && c == 0,
          "latest: an id that is no longer the latest is unknown, even to its owner");
    bi::cancel(kEnv, B, id2, &c);
    bi::set_next_id_for_tests(0xffffffffu);
    const uint32_t wrapped = submit_ok(A, in);
    check(wrapped == 1, "latest: ids wrap past 0 to 1", std::to_string(wrapped));
}

void place_sequence() {
    fresh();
    const uint32_t id = submit_ok(A, request(KILN_BUILDING_ACTION_PLACE));
    Record r;
    const auto before = r.bytes;
    visit(r, 5, nullptr, kPlayer + 1);
    visit(r, 5, nullptr, kPlayer, kSession + 1);
    check(r.bytes == before && state_of(A, id) == KILN_BUILDING_INPUT_QUEUED,
          "place: another entity's or another session's input is never touched");
    r.set_digital(bit(9));  // an unrelated bit the player holds stays
    visit(r, 5);
    check(state_of(A, id) == KILN_BUILDING_INPUT_PRESSED && r.digital() == (bit(9) | bit(0) | bit(1)),
          "place: pressed MainhandAction and _Tap, other bits kept");
    const Transform t = r.transform(0);
    check(t.position[0] == 6442450944ll && t.rotation[3] == 1.0f && t.scale[1] == 1.0f, "place: the cursor transform is the request's");
    visit(r, 5);
    check(state_of(A, id) == KILN_BUILDING_INPUT_PRESSED && r.digital() == (bit(9) | bit(0) | bit(1)),
          "place: a second visit at the same input version keeps the press");
    r.set_digital(r.digital() | bit(2));  // the game derived _Hold
    visit(r, 6);
    check(state_of(A, id) == KILN_BUILDING_INPUT_DISPATCHED && r.digital() == (bit(9) | bit(3)),
          "place: a later version releases every building bit and sets MainhandAction_Release");
    const auto after = r.bytes;
    visit(r, 7);
    visit(r, 8);
    check(r.bytes == after && state_of(A, id) == KILN_BUILDING_INPUT_DISPATCHED, "place: nothing more is written");
    check(submit_ok(A, request(KILN_BUILDING_ACTION_PLACE)) == id + 1, "place: a dispatched request frees the slot");
}

void physical_action_first() {
    fresh();
    const uint32_t id = submit_ok(A, request(KILN_BUILDING_ACTION_REMOVE));
    Record r;
    r.set_digital(bit(4));  // the player's own secondary action
    const auto before = r.bytes;
    visit(r, 1);
    check(r.bytes == before && state_of(A, id) == KILN_BUILDING_INPUT_QUEUED, "physical: the player's own building action finishes first");
    r.set_digital(0);
    visit(r, 2);
    check(state_of(A, id) == KILN_BUILDING_INPUT_PRESSED && r.digital() == bit(4), "remove: pressed SecondaryBuildingAction only");
    visit(r, 3);
    check(state_of(A, id) == KILN_BUILDING_INPUT_DISPATCHED && r.digital() == 0, "remove: released without MainhandAction_Release");
}

void select_sequence() {
    fresh();
    KilnBuildingInput in = request(KILN_BUILDING_ACTION_SELECT);
    in.slot = 7;
    in.material_item_id = 31;
    const uint32_t id = submit_ok(A, in);
    Record r;
    r.bytes[kLayout.create + 5] = 0xab;                                      // a byte the game owns stays
    std::memcpy(r.bytes.data() + kLayout.stock + 12, "\x11\x22\x33\x44", 4);  // the roof material stays
    Mem m;
    m.log = true;
    visit(r, 0x55, &m);
    check(state_of(A, id) == KILN_BUILDING_INPUT_DISPATCHED, "select: dispatched at once");
    check(r.bytes[kLayout.create + 4] == 7 && r.u32(kLayout.create + 8) == 4242 && r.u32(kLayout.create) == 0x55 &&
              r.bytes[kLayout.create + 5] == 0xab,
          "select: slot, item and the version stamp");
    check(r.u32(kLayout.stock + 4) == 31 && r.u32(kLayout.stock + 8) == 31 && r.u32(kLayout.stock) == 0x55 &&
              r.u32(kLayout.stock + 12) == 0x44332211u,
          "select: terrain and default material with the version stamp, roof untouched");
    bool order = m.writes.size() == 5 && m.writes[2] == std::pair<size_t, size_t>{kLayout.create, 4} &&
                 m.writes[4] == std::pair<size_t, size_t>{kLayout.stock, 4};
    for (const auto& w : m.writes)
        order = order && (w.first < kLayout.digital || w.first >= kLayout.digital + 8) &&
                (w.first < kLayout.cursor || w.first >= kLayout.cursor + 112);
    check(order, "select: each version stamp is written after its data; no bits, no transform");
    fresh();
    in.material_item_id = 0;
    submit_ok(A, in);
    Record q;
    visit(q, 9);
    check(q.u32(kLayout.stock) == 0 && q.u32(kLayout.stock + 4) == 0 && q.u32(kLayout.create) == 9,
          "select: no material leaves the stock action alone");
    fresh();
    submit_ok(A, in);
    Record busy;
    busy.set_digital(bit(0));
    visit(busy, 9);
    check(busy.u32(kLayout.create) == 0, "select: waits for the player's own building action too");
}

void undo_and_layout_bits() {
    fresh();
    const uint32_t id = submit_ok(A, request(KILN_BUILDING_ACTION_UNDO));
    Record r;
    visit(r, 1);
    const Transform zero{};
    check(state_of(A, id) == KILN_BUILDING_INPUT_PRESSED && r.digital() == bit(40) &&
              std::memcmp(&zero, r.bytes.data() + kLayout.cursor, 56) == 0,
          "undo: the layout's BuildingUndo bit (40 on 26430625), no transform");
    visit(r, 2);
    check(state_of(A, id) == KILN_BUILDING_INPUT_DISPATCHED && r.digital() == 0, "undo: released");
    // Another build's numbering: the bits follow the layout, never a constant.
    bi::Layout other = kLayout;
    other.undo = 37;
    other.mainhand = 11;
    other.mainhand_tap = 12;
    bi::bind(&other);
    const uint32_t undo = submit_ok(A, request(KILN_BUILDING_ACTION_UNDO));
    Record s;
    visit(s, 1);
    const bool undo37 = undo != 0 && s.digital() == bit(37);
    visit(s, 2);
    const uint32_t place = submit_ok(A, request(KILN_BUILDING_ACTION_PLACE));
    visit(s, 3);
    check(undo37 && place != 0 && s.digital() == (bit(11) | bit(12)), "layout: another build's bit numbers are used as given");
}

void cancels() {
    fresh();
    uint32_t c = 0;
    uint32_t id = submit_ok(A, request(KILN_BUILDING_ACTION_PLACE));
    check(bi::cancel(kEnv, A, id, &c) == KILN_OK && c == 1 && state_of(A, id) == KILN_BUILDING_INPUT_CANCELLED,
          "cancel: a queued request is cancelled");
    Record r;
    const auto before = r.bytes;
    visit(r, 1);
    check(r.bytes == before && state_of(A, id) == KILN_BUILDING_INPUT_CANCELLED, "cancel: the producer never presses it afterwards");

    id = submit_ok(A, request(KILN_BUILDING_ACTION_PLACE));
    visit(r, 2);
    check(state_of(A, id) == KILN_BUILDING_INPUT_PRESSED, "cancel after press: pressed first");
    check(bi::cancel(kEnv, A, id, &c) == KILN_OK && c == 1 && state_of(A, id) == KILN_BUILDING_INPUT_CANCELLED,
          "cancel after press: answers cancelled at once");
    const KilnBuildingInput in = request(KILN_BUILDING_ACTION_PLACE);
    uint32_t other = 9;
    check(bi::submit(kEnv, B, 0, &in, &other) == KILN_CLIENT_ERR_BUILDING_BUSY, "cancel after press: the slot stays busy until the release");
    visit(r, 2);
    check(r.digital() == (bit(0) | bit(1)), "cancel after press: no release at the same input version");
    visit(r, 3);
    check(r.digital() == bit(3) && state_of(A, id) == KILN_BUILDING_INPUT_CANCELLED,
          "cancel after press: released at the next version, still cancelled");
    check(submit_ok(B, in) != 0, "cancel after press: the release frees the slot");

    fresh();
    id = submit_ok(A, request(KILN_BUILDING_ACTION_PLACE));
    Record d;
    visit(d, 1);
    visit(d, 2);
    const auto done = d.bytes;
    check(bi::cancel(kEnv, A, id, &c) == KILN_OK && c == 1 && state_of(A, id) == KILN_BUILDING_INPUT_DISPATCHED,
          "cancel after dispatch: acknowledged, the request stays dispatched");
    visit(d, 3);
    check(d.bytes == done, "cancel after dispatch: nothing is undone or written");
}

void dismantle() {
    fresh();
    KilnBuildingInput in = request(KILN_BUILDING_ACTION_DISMANTLE);
    in.position[0] = 2.0;
    const uint32_t id = submit_ok(A, in);
    Record r;
    g_now = 2000;
    visit(r, 1);
    check(state_of(A, id) == KILN_BUILDING_INPUT_PRESSED && r.digital() == (bit(5) | bit(7)) && r.transform(0).position[0] == (2ll << 32),
          "dismantle: ContextualAction and _Hold pressed at the target's transform");
    r.set_digital(0);  // the game's input sampling cleared the bits and moved the cursor between visits
    std::memset(r.bytes.data() + kLayout.cursor, 0, 112);
    g_now = 2000 + 1199;
    visit(r, 2);  // a later version inside the hold
    check(state_of(A, id) == KILN_BUILDING_INPUT_PRESSED && r.digital() == (bit(5) | bit(7)) && r.transform(1).position[0] == (2ll << 32),
          "dismantle: held 1.2 s, the bits and the transform written again at every visit");
    g_now = 2000 + 1200;
    visit(r, 1);
    check(state_of(A, id) == KILN_BUILDING_INPUT_PRESSED && r.digital() == (bit(5) | bit(7)),
          "dismantle: after the hold, no release at the press's version");
    visit(r, 3);
    check(state_of(A, id) == KILN_BUILDING_INPUT_DISPATCHED && r.digital() == 0, "dismantle: released, no MainhandAction_Release");

    // The release goes only to the input object that took the press.
    fresh();
    const uint32_t place = submit_ok(A, request(KILN_BUILDING_ACTION_PLACE));
    Record first, second;
    visit(first, 1);
    second.set_digital(bit(0) | bit(1));
    const auto untouched = second.bytes;
    visit(second, 2);
    check(second.bytes == untouched && state_of(A, place) == KILN_BUILDING_INPUT_PRESSED,
          "exact target: another ClientPlayerInput of the same entity is never released or pressed");
    visit(first, 2);
    check(first.digital() == bit(3) && state_of(A, place) == KILN_BUILDING_INPUT_DISPATCHED, "exact target: the pressing input is released");
}

void timeouts() {
    fresh();
    g_now = 10000;
    uint32_t id = submit_ok(A, request(KILN_BUILDING_ACTION_PLACE));
    g_now = 15000;
    check(state_of(A, id) == KILN_BUILDING_INPUT_QUEUED, "timeout: still queued at exactly 5 s");
    g_now = 15001;
    check(state_of(A, id) == KILN_BUILDING_INPUT_TIMEOUT, "timeout: a queued request times out after 5 s (status)");
    Record r;
    const auto before = r.bytes;
    visit(r, 1);
    check(r.bytes == before, "timeout: never pressed afterwards");

    g_now = 20000;
    id = submit_ok(A, request(KILN_BUILDING_ACTION_PLACE));
    g_now = 25001;
    visit(r, 1);
    check(r.bytes == before && state_of(A, id) == KILN_BUILDING_INPUT_TIMEOUT, "timeout: the producer times out a queued request too");

    g_now = 30000;
    id = submit_ok(A, request(KILN_BUILDING_ACTION_PLACE));
    visit(r, 1);
    g_now = 35001;
    check(state_of(A, id) == KILN_BUILDING_INPUT_TIMEOUT, "timeout: a pressed request answers timeout after 5 s");
    const KilnBuildingInput in = request(KILN_BUILDING_ACTION_PLACE);
    uint32_t other = 0;
    check(bi::submit(kEnv, B, 0, &in, &other) == KILN_CLIENT_ERR_BUILDING_BUSY, "timeout: a pressed one keeps the slot until released");
    visit(r, 2);
    check(r.digital() == bit(3) && state_of(A, id) == KILN_BUILDING_INPUT_TIMEOUT && submit_ok(B, in) != 0,
          "timeout: a pressed request is still released; it stays timeout");

    // As the source's status and Observe: the 5 s rule overwrites a pressed request's pending answer, so one cancelled
    // after its press and not released within 5 s answers cancelled, then timeout (the one answer that moves back).
    fresh();
    g_now = 40000;
    id = submit_ok(A, in);
    Record p;
    visit(p, 1);
    uint32_t c = 0;
    bi::cancel(kEnv, A, id, &c);
    const bool cancelled_first = c == 1 && state_of(A, id) == KILN_BUILDING_INPUT_CANCELLED;
    g_now = 45001;
    check(cancelled_first && state_of(A, id) == KILN_BUILDING_INPUT_TIMEOUT,
          "timeout: a pressed request cancelled and not released within 5 s answers timeout, as the source does");
    visit(p, 2);
    check(p.digital() == bit(3) && state_of(A, id) == KILN_BUILDING_INPUT_TIMEOUT, "timeout: and it is still released");

    // A press whose input never comes back for its release (the local player's entity or its ClientPlayerInput changed
    // between the press and the release) ends 10 s after the press, unreleased, and frees the slot. Before, it kept the
    // slot busy for the rest of the session: cancel only set its answer and the 5 s rule only rewrote it.
    fresh();
    g_now = 50000;
    id = submit_ok(A, in);
    Record pressed;
    visit(pressed, 1);
    Record moved;
    const auto untouched = moved.bytes;
    g_now = 59000;
    visit(moved, 2);  // the same entity's input at another address: never released there
    check(bi::submit(kEnv, B, 0, &in, &other) == KILN_CLIENT_ERR_BUILDING_BUSY && moved.bytes == untouched,
          "unreleased: 9 s after its press it still keeps the slot; another input is not released");
    g_now = 60001;
    visit(moved, 3, nullptr, kPlayer + 1);  // the local player is another entity now
    check(moved.bytes == untouched && state_of(A, id) == KILN_BUILDING_INPUT_TIMEOUT,
          "unreleased: 10 s after its press it ends timeout, nothing written");
    check(submit_ok(B, in) != 0, "unreleased: the slot is free again");
    // the same end without a visit or a status: the next submit finds it over
    fresh();
    g_now = 70000;
    id = submit_ok(A, in);
    Record alone;
    visit(alone, 1);
    g_now = 80001;
    check(bi::submit(kEnv, B, 0, &in, &other) == KILN_OK && other != 0 && alone.digital() == bit(0) + bit(1),
          "unreleased: a submit 10 s after the press takes the slot (nothing visited or asked meanwhile)");
}

void stop_and_reload() {
    fresh();
    const uint32_t id = submit_ok(A, request(KILN_BUILDING_ACTION_PLACE));
    bi::bind(nullptr);
    check(state_of(A, id) == KILN_BUILDING_INPUT_CANCELLED, "stop: a queued request ends cancelled when the producer stops");
    Record r;
    const auto before = r.bytes;
    visit(r, 1);
    const KilnBuildingInput in = request(KILN_BUILDING_ACTION_PLACE);
    uint32_t next = 9;
    check(r.bytes == before && bi::submit(kEnv, A, 0, &in, &next) == KILN_ERR_UNAVAILABLE && next == 0,
          "stop: a stopped producer writes nothing and takes nothing");
    bi::bind(&kLayout);
    next = submit_ok(A, in);
    check(next == id + 1 && state_of(A, id) == KILN_BUILDING_INPUT_UNKNOWN && state_of(A, next) == KILN_BUILDING_INPUT_QUEUED,
          "reload: a new producer takes requests, ids are never reused");
    visit(r, 1);
    bi::bind(nullptr);
    check(state_of(A, next) == KILN_BUILDING_INPUT_CANCELLED, "stop: a pressed request ends cancelled too (the input path is gone)");
    bi::bind(&kLayout);
    check(submit_ok(A, in) != 0, "reload: the slot is free after a stop");
}

void session_end() {
    fresh();
    const uint32_t id = submit_ok(A, request(KILN_BUILDING_ACTION_PLACE));
    bi::session_ended(kSession);
    check(state_of(A, id) == KILN_BUILDING_INPUT_CANCELLED, "session: a queued request ends cancelled at its world's stop");
    const KilnBuildingInput in = request(KILN_BUILDING_ACTION_PLACE);
    uint32_t next = 9;
    check(bi::submit(kEnv, A, 0, &in, &next) == KILN_CLIENT_ERR_BUILDING_STALE && next == 0,
          "session: no later submit binds to an ended session");
    bi::Env env = kEnv;
    env.live_session = kSession + 1;
    check(bi::submit(env, A, 0, &in, &next) == KILN_OK, "session: the next session takes requests");
    Record r;
    Mem m;
    m.base = r.base();
    bi::observe({r.base(), kPlayer, kSession + 1, 1}, {&mem_read, &mem_write, &m});
    uint32_t s = 0;
    bi::status(env, A, next, &s);
    check(s == KILN_BUILDING_INPUT_PRESSED, "session: pressed in the new session");
    bi::session_ended(kSession + 1);
    bi::status(env, A, next, &s);
    check(s == KILN_BUILDING_INPUT_CANCELLED, "session: a pressed request ends cancelled too");
    env.live_session = kSession + 2;
    check(bi::submit(env, A, 0, &in, &next) == KILN_OK, "session: and the slot is free");
    bi::session_ended(kSession);  // an older session's stop changes nothing now
    bi::status(env, A, next, &s);
    check(s == KILN_BUILDING_INPUT_QUEUED, "session: an older session's stop leaves a newer request alone");
}

void owners_gone() {
    fresh();
    uint32_t id = submit_ok(A, request(KILN_BUILDING_ACTION_PLACE));
    bi::owner_gone(B);
    check(state_of(A, id) == KILN_BUILDING_INPUT_QUEUED, "owner: another owner's retirement leaves it alone");
    bi::owner_gone(A);
    check(state_of(A, id) == KILN_BUILDING_INPUT_CANCELLED, "owner: a retired owner's queued request is cancelled");
    id = submit_ok(B, request(KILN_BUILDING_ACTION_PLACE));
    Record r;
    visit(r, 1);
    bi::owner_gone(B);
    check(state_of(B, id) == KILN_BUILDING_INPUT_CANCELLED, "owner: a retired owner's pressed request answers cancelled");
    visit(r, 2);
    check(r.digital() == bit(3), "owner: and its press is still released (no stuck input after a reload)");
    check(submit_ok(A, request(KILN_BUILDING_ACTION_PLACE)) != 0, "owner: the slot is free again");
}

// The owner's revoke runs between submit's first owner check and its slot lock: owner_gone() finds nothing to cancel, so
// submit must not leave that request queued for the producer.
std::atomic<bool> g_alive_now{true};
bool alive_now(KilnOwner) { return g_alive_now.load(); }

void owner_revoked_during_submit() {
    fresh();
    bi::Env env = kEnv;  // owner_alive was true when submit started
    env.owner_alive_now = &alive_now;
    g_alive_now = false;  // ... and the revoke (with its owner_gone) happened before the request reached the slot
    bi::owner_gone(A);
    const KilnBuildingInput in = request(KILN_BUILDING_ACTION_PLACE);
    uint32_t id = 99;
    const KilnResult rc = bi::submit(env, A, 0, &in, &id);
    check(rc == KILN_ERR_REVOKED && id == 0, "owner revoked during submit: answers revoked, id 0", std::to_string(rc));
    Record r;
    const auto before = r.bytes;
    visit(r, 1);
    check(r.bytes == before, "owner revoked during submit: the producer never presses its request");
    check(submit_ok(B, in) != 0, "owner revoked during submit: the slot is free for another owner");
    g_alive_now = true;
    fresh();
    env.owner_alive_now = &alive_now;
    check(bi::submit(env, A, 0, &in, &id) == KILN_OK && id == 1 && state_of(A, id) == KILN_BUILDING_INPUT_QUEUED,
          "owner alive at the second look: queued as usual");
}

void faults() {
    fresh();
    uint32_t id = submit_ok(A, request(KILN_BUILDING_ACTION_PLACE));
    Record r;
    Mem m;
    m.fail_read = true;
    const auto f0 = bi::stats().faults;
    visit(r, 1, &m);
    check(state_of(A, id) == KILN_BUILDING_INPUT_CANCELLED && bi::stats().faults == f0 + 1, "fault: an unreadable input ends cancelled");
    id = submit_ok(A, request(KILN_BUILDING_ACTION_PLACE));
    m.fail_read = false;
    m.fail_write = true;
    visit(r, 1, &m);
    check(state_of(A, id) == KILN_BUILDING_INPUT_CANCELLED && bi::stats().faults == f0 + 2, "fault: an unwritable input ends cancelled");
    check(submit_ok(A, request(KILN_BUILDING_ACTION_PLACE)) != 0, "fault: the slot is free");
}

void producer_never_waits() {
    fresh();
    const uint32_t id = submit_ok(A, request(KILN_BUILDING_ACTION_PLACE));
    std::mutex m;
    std::condition_variable cv;
    bool held = false, done = false;
    std::thread holder([&] {
        std::lock_guard slot(bi::mutex_for_tests());
        std::unique_lock l(m);
        held = true;
        cv.notify_all();
        cv.wait(l, [&] { return done; });
    });
    {
        std::unique_lock l(m);
        cv.wait(l, [&] { return held; });
    }
    Record r;
    const auto before = r.bytes;
    const auto s0 = bi::stats().skipped;
    visit(r, 1);  // would deadlock here if the producer waited for the slot
    const bool skipped = bi::stats().skipped == s0 + 1 && r.bytes == before;
    {
        std::lock_guard l(m);
        done = true;
    }
    cv.notify_all();
    holder.join();
    check(skipped, "producer: a caller holding the slot makes it skip the visit, never wait");
    visit(r, 1);
    check(state_of(A, id) == KILN_BUILDING_INPUT_PRESSED, "producer: the next visit presses");
}

void no_allocation() {
    fresh();
    __sanitizer_install_malloc_and_free_hooks(&on_malloc, &on_free);
    unsigned total = 0;
    for (uint32_t action : {KILN_BUILDING_ACTION_PLACE, KILN_BUILDING_ACTION_SELECT, KILN_BUILDING_ACTION_DISMANTLE,
                            KILN_BUILDING_ACTION_UNDO}) {
        KilnBuildingInput in = request(action);
        in.material_item_id = action == KILN_BUILDING_ACTION_SELECT ? 5 : 0;
        const uint32_t id = submit_ok(A, in);
        Record r;
        Mem mem;
        mem.base = r.base();
        const bi::Memory m{&mem_read, &mem_write, &mem};
        g_now += 10;
        t_allocs = 0;
        t_counting = true;
        bi::observe({r.base(), kPlayer, kSession, 1}, m);
        g_now += 2000;
        bi::observe({r.base(), kPlayer, kSession, 2}, m);
        t_counting = false;
        total += t_allocs;
        uint32_t c;
        bi::cancel(kEnv, A, id, &c);
    }
    check(total == 0, "producer: no heap allocation on the producer thread", std::to_string(total));
}

// The producer thread's Memory: plain copies that count releases, the visits whose digital write holds no pressed bit
// (a place's release writes MainhandAction_Release alone, a dismantle's or a remove's writes none).
struct ReleaseCount {
    uintptr_t base = 0;
    uint32_t releases = 0;
};
bool plain_read(void*, void* dst, uintptr_t src, size_t n) {
    std::memcpy(dst, reinterpret_cast<const void*>(src), n);
    return true;
}
bool counting_write(void* ctx, uintptr_t dst, const void* src, size_t n) {
    auto* c = static_cast<ReleaseCount*>(ctx);
    std::memcpy(reinterpret_cast<void*>(dst), src, n);
    if (dst == c->base + kLayout.digital && n == 8) {
        uint64_t v;
        std::memcpy(&v, src, 8);
        if (!(v & (bit(0) | bit(1) | bit(2) | bit(4) | bit(5) | bit(7) | bit(40)))) ++c->releases;
    }
    return true;
}

void concurrent_copied_status() {
    fresh();
    std::atomic<bool> stop{false};
    std::atomic<uint32_t> regressions{0}, submitted{0};
    ReleaseCount released;  // the producer thread's own; read after it is joined
    std::thread producer([&] {
        Record r;
        released.base = r.base();
        uint32_t version = 1;
        while (!stop.load(std::memory_order_relaxed)) {
            // The clock moves with the producer's visits (100 ms each), not with the callers' spinning: a dismantle's
            // hold ends after 12 visits and nothing times out unvisited. In review a clock moved by the callers stood
            // still (both owners busy) or raced ahead of a starved producer (every request timed out unpressed).
            g_now.fetch_add(100, std::memory_order_relaxed);
            r.set_digital(0);  // the game samples the input anew
            bi::observe({r.base(), kPlayer, kSession, version++}, {&plain_read, &counting_write, &released});
            std::this_thread::yield();
        }
    });
    // Each caller reads its own request's state while the producer presses and releases it. Whether a caller ever reads
    // DISPATCHED itself is timing (the other owner's next request replaces a finished one at once, and under the slot's
    // low priority the threads may share one CPU), so the full sequence is counted where it happens: at the producer.
    auto caller = [&](KilnOwner owner) {
        const KilnBuildingInput in = request(owner == A ? KILN_BUILDING_ACTION_PLACE : KILN_BUILDING_ACTION_DISMANTLE);
        for (int round = 0; round < 3000; ++round, std::this_thread::yield()) {
            uint32_t id = 0;
            if (bi::submit(kEnv, owner, 0, &in, &id) != KILN_OK) continue;
            submitted.fetch_add(1, std::memory_order_relaxed);
            uint32_t last = KILN_BUILDING_INPUT_QUEUED;
            for (int i = 0; i < 400; ++i, std::this_thread::yield()) {
                uint32_t s = 0;
                bi::status(kEnv, owner, id, &s);
                if (s == KILN_BUILDING_INPUT_UNKNOWN) break;  // it ended and another owner's request replaced it
                if (s < last) regressions.fetch_add(1, std::memory_order_relaxed);
                if (s >= KILN_BUILDING_INPUT_DISPATCHED) break;
                last = s;
            }
            if ((round & 3) == 0) {
                uint32_t c;
                bi::cancel(kEnv, owner, id, &c);
            }
        }
    };
    std::thread a(caller, A), b(caller, B);
    a.join();
    b.join();
    stop = true;
    producer.join();
    check(regressions == 0 && submitted > 0 && released.releases > 0, "threads: each request's copied state only moves forward",
          std::to_string(submitted.load()) + " submitted, " + std::to_string(released.releases) + " releases, " +
              std::to_string(regressions.load()) + " regressions");
}
// bound()'s answer while unbound: the default (no observation point), the binder's reason, and back.
void unbound_reasons() {
    bi::reset_for_tests();
    const char* why = nullptr;
    const bool unbound = !bi::bound(&why);
    const std::string def = why ? why : "";
    check(unbound && def.find("client.input_observation") != std::string::npos,
          "reason: unbound by default, naming the missing sigdb feature", def);
    static const char* const kHeld = "held by the client gate";
    bi::unbound_reason(kHeld);
    why = nullptr;
    check(!bi::bound(&why) && why == kHeld, "reason: the binder's reason while unbound");
    bi::bind(&kLayout);
    why = nullptr;
    check(bi::bound(&why) && why == nullptr, "reason: a bound producer gives none");
    bi::bind(nullptr);
    check(!bi::bound(&why) && why == kHeld, "reason: unbound again, the last reason stands");
    bi::unbound_reason(nullptr);
    check(!bi::bound(&why) && def == why, "reason: nullptr is the default again");
    bi::unbound_reason(kHeld);
    bi::reset_for_tests();
    check(!bi::bound(&why) && def == why, "reason: reset_for_tests restores the default");
}
}  // namespace

// ---- the lazy producer: an installer, called at the first submit that could be queued ----
std::atomic<int> g_install_calls{0};
std::atomic<int> g_install_script[8];  // per call: 0 Bound, 1 Again, 2 Failed
std::atomic<int> g_install_delay_ms{0};
std::atomic<int> g_entered{0};          // submits a test's threads have started
std::atomic<int> g_install_wait_for{0}; // the installer first waits until this many have started (0: no wait)
const char* const kInstallWhy = "the test installer failed";
bi::Install test_installer(bi::Layout* layout, const char** why) {
    const int n = g_install_calls.fetch_add(1);
    if (const int want = g_install_wait_for.load())
        for (int i = 0; i < 5000 && g_entered.load() < want; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (const int ms = g_install_delay_ms.load()) std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    const int script = n < 8 ? g_install_script[n].load() : 2;
    if (script == 3) throw std::runtime_error("the test installer threw");  // 3: an install that throws
    if (script == 0) {
        *layout = kLayout;
        return bi::Install::Bound;
    }
    *why = kInstallWhy;
    return script == 1 ? bi::Install::Again : bi::Install::Failed;
}
void lazy(std::initializer_list<int> script, int delay_ms = 0) {
    bi::reset_for_tests();
    bi::set_clock_for_tests(&fake_clock);
    g_now = 1000;
    g_install_calls = 0;
    g_install_delay_ms = delay_ms;
    g_entered = 0;
    g_install_wait_for = 0;
    int i = 0;
    for (auto& s : g_install_script) s = 2;
    for (int s : script) g_install_script[i++] = s;
    bi::set_installer(&test_installer);
}

void lazy_install() {
    lazy({0});
    const char* why = nullptr;
    bool installed = true;
    check(!bi::bound(nullptr) && bi::usable(&why, &installed) && !installed && g_install_calls == 0,
          "lazy: an installer given: usable, not bound, nothing installed");
    KilnBuildingInput in = request(KILN_BUILDING_ACTION_PLACE);
    KilnBuildingInput bad = in;
    bad.action = 9;
    uint32_t id = 99, s = 0, c = 0;
    bi::Env env = kEnv;
    env.owner_alive = false;
    check(bi::submit(kEnv, A, 0, &bad, &id) == KILN_ERR_INVALID_ARGUMENT && bi::submit(kEnv, 0, 0, &in, &id) == KILN_ERR_INVALID_ARGUMENT &&
              bi::submit(env, A, 0, &in, &id) == KILN_ERR_REVOKED && g_install_calls == 0,
          "lazy: a submit refused for its arguments or its owner installs nothing");
    env = kEnv;
    env.live_session = 0;
    check(bi::submit(env, A, 0, &in, &id) == KILN_CLIENT_ERR_BUILDING_STALE &&
              bi::submit(kEnv, A, kSession + 1, &in, &id) == KILN_CLIENT_ERR_BUILDING_STALE && g_install_calls == 0,
          "lazy: no live client world, or another session, is stale and installs nothing");
    env = kEnv;
    env.client_side = false;
    check(bi::submit(env, A, 0, &in, &id) == KILN_ERR_UNAVAILABLE && g_install_calls == 0,
          "lazy: a dedicated server installs nothing");
    check(bi::status(kEnv, A, 1, &s) == KILN_OK && bi::cancel(kEnv, A, 1, &c) == KILN_OK && g_install_calls == 0,
          "lazy: status and cancel install nothing");
    const uint64_t gen = bi::generation();
    check(bi::submit(kEnv, A, 0, &in, &id) == KILN_OK && id == 1 && g_install_calls == 1 && bi::bound(nullptr) &&
              bi::usable(nullptr, &installed) && installed && bi::generation() != gen && bi::stats().installs == 1,
          "lazy: the first submit that could be queued installs once, binds and is queued");
    Record r;
    visit(r, 10);
    check(r.digital() == (bit(kLayout.mainhand) | bit(kLayout.mainhand_tap)) && state_of(A, id) == KILN_BUILDING_INPUT_PRESSED,
          "lazy: the producer the install bound presses the request");
    uint32_t again = 0;
    check(bi::submit(kEnv, B, 0, &in, &again) == KILN_CLIENT_ERR_BUILDING_BUSY && g_install_calls == 1,
          "lazy: later submits never install again");
    visit(r, 11);
    check(bi::submit(kEnv, B, 0, &in, &again) == KILN_OK && g_install_calls == 1, "lazy: the next request needs no install");

    // concurrent first submits: one install, then one queued request and busy for the rest
    lazy({0}, 30);
    constexpr int kThreads = 8;
    std::atomic<int> ok{0}, busy{0}, other{0};
    std::atomic<bool> go{false};
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t)
        threads.emplace_back([&, t] {
            while (!go.load()) std::this_thread::yield();
            KilnBuildingInput mine = request(KILN_BUILDING_ACTION_PLACE);
            uint32_t got = 0;
            const KilnResult rc = bi::submit(kEnv, static_cast<KilnOwner>(100 + t), 0, &mine, &got);
            (rc == KILN_OK ? ok : rc == KILN_CLIENT_ERR_BUILDING_BUSY ? busy : other).fetch_add(1);
        });
    go = true;
    for (auto& t : threads) t.join();
    check(g_install_calls == 1 && ok == 1 && busy == kThreads - 1 && other == 0 && bi::bound(nullptr),
          "lazy: concurrent first submits: one install, one queued, the rest busy",
          std::to_string(g_install_calls.load()) + " installs, " + std::to_string(ok.load()) + " ok, " +
              std::to_string(busy.load()) + " busy, " + std::to_string(other.load()) + " other");

    // a failed install is final: that submit and every later one are unavailable, with the installer's reason
    lazy({2});
    why = nullptr;
    check(bi::submit(kEnv, A, 0, &in, &id) == KILN_ERR_UNAVAILABLE && id == 0 && !bi::usable(&why, nullptr) &&
              why == kInstallWhy && bi::submit(kEnv, A, 0, &in, &id) == KILN_ERR_UNAVAILABLE && g_install_calls == 1 &&
              !bi::bound(nullptr),
          "lazy: a failed install answers unavailable, says why, and is not tried again");
    // refused for threads it could not check: later submits try again, up to kInstallAttempts in all
    lazy({1, 1, 0});
    check(bi::submit(kEnv, A, 0, &in, &id) == KILN_ERR_UNAVAILABLE && bi::usable(nullptr, nullptr) &&
              bi::submit(kEnv, A, 0, &in, &id) == KILN_ERR_UNAVAILABLE && bi::submit(kEnv, A, 0, &in, &id) == KILN_OK &&
              g_install_calls == 3 && bi::bound(nullptr),
          "lazy: an install to try again leaves the producer usable, and a later submit installs it");
    lazy({1, 1, 1, 0});
    why = nullptr;
    check(bi::submit(kEnv, A, 0, &in, &id) == KILN_ERR_UNAVAILABLE && bi::submit(kEnv, A, 0, &in, &id) == KILN_ERR_UNAVAILABLE &&
              bi::submit(kEnv, A, 0, &in, &id) == KILN_ERR_UNAVAILABLE && !bi::usable(&why, nullptr) && why == kInstallWhy &&
              bi::submit(kEnv, A, 0, &in, &id) == KILN_ERR_UNAVAILABLE && g_install_calls == static_cast<int>(bi::kInstallAttempts),
          "lazy: the last attempt to try again is final");
    bi::set_installer(nullptr);
    bi::reset_for_tests();
}

// The lazy producer's edges: a burst of first submits spends one refused attempt, an install that binds never answers a
// first submit that comes meanwhile unavailable, and an installer that throws is a final failure, never an exception out
// of submit.
void lazy_install_edges() {
    using namespace std::chrono_literals;
    KilnBuildingInput in = request(KILN_BUILDING_ACTION_PLACE);
    uint32_t id = 0;

    // eight first submits at once, the install refused for threads it could not check: the ones that waited for that
    // attempt take its answer (one attempt for the burst); a later submit tries again
    lazy({1, 0}, 100);
    constexpr int kThreads = 8;
    g_install_wait_for = kThreads;
    std::atomic<int> unavailable{0}, other{0};
    std::atomic<bool> go{false};
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t)
        threads.emplace_back([&, t] {
            while (!go.load()) std::this_thread::yield();
            KilnBuildingInput mine = request(KILN_BUILDING_ACTION_PLACE);
            uint32_t got = 0;
            g_entered.fetch_add(1);
            const KilnResult rc = bi::submit(kEnv, static_cast<KilnOwner>(100 + t), 0, &mine, &got);
            (rc == KILN_ERR_UNAVAILABLE ? unavailable : other).fetch_add(1);
        });
    go = true;
    for (auto& t : threads) t.join();
    check(g_install_calls == 1 && unavailable == kThreads && other == 0 && bi::usable(nullptr, nullptr),
          "lazy: concurrent first submits refused for threads it could not check spend one attempt, not all three",
          std::to_string(g_install_calls.load()) + " installer calls, " + std::to_string(unavailable.load()) +
              " unavailable, " + std::to_string(other.load()) + " other");
    g_install_wait_for = 0;
    check(bi::submit(kEnv, A, 0, &in, &id) == KILN_OK && g_install_calls == 2 && bi::bound(nullptr),
          "lazy: after a refused burst, a later submit tries again and installs");

    // an install that binds: a first submit that comes while it binds waits for it, and usable() stays true
    lazy({0});
    {
        std::atomic<int> a_rc{-1}, b_rc{-1};
        std::unique_lock slot(bi::mutex_for_tests());  // bind() takes the slot: the install is held right there
        std::thread a([&] {
            KilnBuildingInput mine = request(KILN_BUILDING_ACTION_PLACE);
            uint32_t got = 0;
            a_rc = bi::submit(kEnv, A, 0, &mine, &got);
        });
        for (int i = 0; i < 5000 && g_install_calls == 0; ++i) std::this_thread::sleep_for(1ms);
        std::this_thread::sleep_for(100ms);  // the installer returned: the install waits for the slot to bind
        const bool usable_while_binding = bi::usable(nullptr, nullptr);
        std::thread b([&] {
            KilnBuildingInput mine = request(KILN_BUILDING_ACTION_PLACE);
            uint32_t got = 0;
            b_rc = bi::submit(kEnv, B, 0, &mine, &got);
        });
        std::this_thread::sleep_for(100ms);
        const int b_early = b_rc.load();
        slot.unlock();
        a.join();
        b.join();
        const bool one_each = (a_rc == KILN_OK && b_rc == KILN_CLIENT_ERR_BUILDING_BUSY) ||
                              (a_rc == KILN_CLIENT_ERR_BUILDING_BUSY && b_rc == KILN_OK);
        check(usable_while_binding && b_early == -1 && one_each && g_install_calls == 1 && bi::bound(nullptr),
              "lazy: a first submit while the install binds waits for it (never unavailable); usable all along",
              std::string("usable while binding ") + (usable_while_binding ? "yes" : "no") + ", second submit early " +
                  std::to_string(b_early) + ", answers " + std::to_string(a_rc.load()) + " and " + std::to_string(b_rc.load()));
    }

    // an installer that throws: final, unavailable with a reason, and the install mutex is free again
    lazy({3});
    bool threw = false;
    KilnResult rc = KILN_OK;
    try {
        rc = bi::submit(kEnv, A, 0, &in, &id);
    } catch (...) {
        threw = true;
    }
    const char* why = nullptr;
    bool final_after = false;
    if (!threw) {
        try {
            final_after = !bi::usable(&why, nullptr) && why && bi::submit(kEnv, A, 0, &in, &id) == KILN_ERR_UNAVAILABLE &&
                          g_install_calls == 1;
        } catch (...) {
        }
    }
    check(!threw && rc == KILN_ERR_UNAVAILABLE && final_after,
          "lazy: an installer that throws is a final failure: unavailable with a reason, never an exception out of submit");
    bi::set_installer(nullptr);
    bi::reset_for_tests();
}

int main() {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);  // a hang (the script's time limit) still shows how far it got
    unbound_reasons();
    refusals();
    field_rules();
    conversion();
    latest_id();
    place_sequence();
    physical_action_first();
    select_sequence();
    undo_and_layout_bits();
    cancels();
    dismantle();
    timeouts();
    stop_and_reload();
    session_end();
    owners_gone();
    owner_revoked_during_submit();
    faults();
    producer_never_waits();
    no_allocation();
    concurrent_copied_status();
    lazy_install();
    lazy_install_edges();
    bi::set_clock_for_tests(nullptr);
    std::printf("client building input: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
