// SPDX-License-Identifier: MIT
// Production EMF terrain translation with fake Kiln services, without a game.
#include "emf_internal.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <map>
#include <set>
#include <thread>

static int checks = 0, failures = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::printf("FAIL %d: %s\n", __LINE__, #x); } } while (0)
namespace kemf {
const KilnApi* g_api;
uint32_t g_side = KILN_SIDE_SERVER;
Services S;
thread_local Context t_ctx;
static Mod mod;
Mod* caller_mod(const void*) { return &mod; }
void unsupported(const Mod*, const char*, const char*) {}
void refuse(const Mod*, const char*, const char*, bool) {}
void note_call(const char*) {}
void mark_status_dirty() {}
static uint64_t clock_ms = 100;
uint64_t now_ms() { return clock_ms; }
bool guarded(Mod*, const char*, ThunkFn fn, void* p) { try { fn(p); return true; } catch (...) { return false; } }
}
using namespace kemf;
static bool tick_thread = true;
static uint64_t session = 7;
static uintptr_t world_ptr = 0x1000;
static uintptr_t KILN_CALL world(uint32_t k) { return k == KILN_WORLD_SERVER ? world_ptr : 0x2000; }
static bool KILN_CALL tick() { return tick_thread; }
static uint64_t KILN_CALL world_session(uint32_t) { return session; }
static std::map<std::string, bool> capabilities;
static KilnResult KILN_CALL cap(KilnStr name, KilnCapRow* out) {
    auto i = capabilities.find(std::string(name.ptr, name.len));
    if (i == capabilities.end()) return KILN_ERR_NOT_FOUND;
    out->state = i->second ? KILN_CAP_AVAILABLE : KILN_CAP_UNAVAILABLE;
    out->reason = i->second ? KILN_CAP_REASON_NONE : KILN_CAP_REASON_SYMBOL_MISSING;
    return KILN_OK;
}
static KilnResult query_rc = KILN_OK, break_rc = KILN_OK;
static uint32_t break_refusal = 0, break_result_flags = KILN_BUILD_BREAK_R_VERIFIED;
static unsigned query_calls = 0, break_calls = 0;
static KilnBuildBlockInfo block{};
static KilnVec3 captured_point{}, captured_from{}, captured_to{};
static uint32_t find_flags = 0;
static std::vector<KilnBuildBreak> requests;
static std::vector<std::vector<uint8_t>> masks;
static std::string origin;
static KilnResult KILN_CALL block_at(KilnVec3 p, KilnBuildBlockInfo* out) {
    ++query_calls;
    captured_point = p;
    *out = block;
    return query_rc;
}
static KilnResult KILN_CALL find_wall(KilnVec3 from, KilnVec3 to, uint32_t flags, KilnBuildBlockInfo* out) {
    captured_from = from; captured_to = to; find_flags = flags;
    if ((flags & KILN_BUILD_FIND_PLAYER_TERRAIN) && !capabilities["build.block_provenance"]) {
        out->refusal = KILN_BUILD_BLOCK_SYMBOL_MISSING;
        return KILN_ERR_UNAVAILABLE;
    }
    return block_at(from, out);
}
static KilnResult KILN_CALL grid(KilnBuildGrid* out) {
    out->origin = {-10, 0, -20}; out->cell_size = 0.5;
    return KILN_OK;
}
static KilnResult KILN_CALL break_box(const KilnBuildBreak* r, KilnBuildBreakResult* out) {
    CHECK(tick_thread);
    ++break_calls;
    requests.push_back(*r);
    masks.emplace_back(r->selection, r->selection + r->dims[0] * r->dims[1] * r->dims[2]);
    origin.assign(r->origin_mod.ptr, r->origin_mod.len);
    out->damaged = 3; out->removed = 2; out->dropped = r->flags & KILN_BUILD_BREAK_DROPS ? 1 : 0;
    out->refusal = break_refusal;
    out->flags = break_result_flags;
    return break_rc;
}
struct Answer { EmfWorld* world; uint32_t id; EmfBreakResult result; void* user; };
static std::vector<Answer> answers;
static void callback(EmfWorld* w, uint32_t id, const EmfBreakResult* r, void* user) {
    CHECK(tick_thread && t_ctx.mod == &mod && t_ctx.world_cb && t_ctx.world == w);
    CHECK(r && r->size == 20);
    answers.push_back({w, id, *r, user});
}
static EmfBreakBlocks request() {
    EmfBreakBlocks r{};
    r.size = sizeof r; r.flags = EMF_BREAK_INSTANT; r.x = 1.25; r.y = 2.25; r.z = 3.25;
    return r;
}
int main() {
    KilnApi api{}; api.world = &world; api.on_tick_thread = &tick; g_api = &api;
    KilnBuildApi build{}; build.header.size = sizeof build;
    build.block_at = &block_at; build.find_wall = &find_wall; build.break_box = &break_box; build.grid = &grid;
    KilnCapsApi caps{}; caps.header.size = sizeof caps; caps.find = &cap;
    KilnWorldApi worlds{}; worlds.header.size = sizeof worlds; worlds.game_world_session = &world_session;
    S.build = &build; S.caps = &caps; S.world = &worlds;
    for (const char* c : {"build.break_building", "build.block_metadata", "build.block_provenance", "build.break_damage", "build.break_drops", "build.break_terrain"}) capabilities[c] = true;
    mod.id = "terrain-test";
    auto* w = reinterpret_cast<EmfWorld*>(world_ptr);
    ContextScope scope(&mod, w, true);
    EmfApi t{}; fill_terrain(&t);
    CHECK(t.break_blocks && t.block_at && t.find_wall && t.find_wall_ex);
    if (!t.break_blocks || !t.block_at || !t.find_wall || !t.find_wall_ex) return 1;
    block.size = sizeof block; block.kind = KILN_BUILD_BLOCK_BUILDING; block.material = 135; block.fill = 0;
    block.hardness = 3; block.health = 1200; block.damage = 42; block.provenance = KILN_BUILD_PROVENANCE_PLAYER;
    block.center = {1.25, 2.25, 3.25};
    EmfBlockInfo info{}; info.size = sizeof info;
    CHECK(t.block_at(w, 1, 2, 3, &info));
    CHECK(captured_point.x == 1 && captured_point.y == 2 && captured_point.z == 3);
    CHECK(info.kind == EMF_BLOCK_BUILDING && info.material == 7 && info.health == 1200 && info.damage == 42);
    CHECK(info.fill == 0 && info.hardness == 3 && info.world == 0 && info.x == 1.25 && info.y == 2.25 && info.z == 3.25);
    for (int provenance : {-1, 0, 1}) {
        block.provenance = provenance;
        CHECK(t.block_at(w, 1, 2, 3, &info) && info.world == provenance);
    }
    block.kind = KILN_BUILD_BLOCK_TERRAIN; block.material = 7; block.fill = 123; block.health = 5;
    CHECK(t.block_at(w, 1, 2, 3, &info) && info.material == 7 && info.fill == 123 && info.health == 5 && info.damage == -1);
    block.hardness = block.health = block.damage = -1;
    CHECK(t.block_at(w, 1, 2, 3, &info) && info.hardness == 0 && info.health == 0 && info.damage == -1);
    block.kind = KILN_BUILD_BLOCK_AIR;
    CHECK(t.block_at(w, 1, 2, 3, &info) && info.kind == EMF_BLOCK_EMPTY && info.material == 0 && info.hardness == 0 && info.health == 0 && info.world == 0);
    // Prefix-sized output keeps size, never writes v45's tail or bytes after the known record.
    struct Extended { EmfBlockInfo info; uint32_t tail; } ext{};
    ext.info.size = 40; ext.info.world = 123; ext.tail = 0x12345678;
    CHECK(t.block_at(w, 1, 2, 3, &ext.info) && ext.info.size == 40 && ext.info.world == 123 && ext.tail == 0x12345678);
    ext.info.size = sizeof ext;
    CHECK(t.block_at(w, 1, 2, 3, &ext.info) && ext.info.size == sizeof ext && ext.tail == 0x12345678);
    float from[3] = {1, 2, 3}, to[3] = {4, 5, 6};
    CHECK(t.find_wall(w, from, to, &info) && find_flags == 0);
    CHECK(t.find_wall_ex(w, from, to, 0, &info) && find_flags == 0);
    CHECK(t.find_wall_ex(w, from, to, EMF_FIND_TERRAIN, &info) && find_flags == KILN_BUILD_FIND_PLAYER_TERRAIN);
    // EMF skips unknown terrain but still finds buildings if its shipped-world reader is unavailable.
    capabilities["build.block_provenance"] = false;
    block.kind = KILN_BUILD_BLOCK_BUILDING; block.material = 135; block.provenance = -1;
    CHECK(t.find_wall_ex(w, from, to, EMF_FIND_TERRAIN, &info) && find_flags == 0 && info.kind == EMF_BLOCK_BUILDING && info.world == -1);
    capabilities["build.block_provenance"] = true;
    CHECK(captured_from.x == 1 && captured_from.y == 2 && captured_from.z == 3 && captured_to.x == 4 && captured_to.y == 5 && captured_to.z == 6);
    // Every refusal leaves the complete output untouched.
    auto saved = info;
    for (KilnResult rc : {KILN_ERR_NOT_FOUND, KILN_ERR_UNAVAILABLE, KILN_ERR_INTERNAL}) {
        query_rc = rc;
        CHECK(!t.block_at(w, 1, 2, 3, &info) && std::memcmp(&info, &saved, sizeof info) == 0);
        CHECK(!t.find_wall(w, from, to, &info) && std::memcmp(&info, &saved, sizeof info) == 0);
        CHECK(!t.find_wall_ex(w, from, to, EMF_FIND_TERRAIN, &info) && std::memcmp(&info, &saved, sizeof info) == 0);
    }
    query_rc = KILN_OK;
    CHECK(!t.find_wall_ex(w, from, to, 2, &info) && std::memcmp(&info, &saved, sizeof info) == 0);
    for (uint32_t size : {0u, 39u}) {
        info.size = size; saved = info;
        CHECK(!t.block_at(w, 1, 2, 3, &info) && std::memcmp(&info, &saved, sizeof info) == 0);
        CHECK(!t.find_wall(w, from, to, &info) && !t.find_wall_ex(w, from, to, 0, &info));
    }
    info.size = sizeof info;
    CHECK(!t.block_at(w, 1, 2, 3, nullptr) && !t.find_wall(w, nullptr, to, &info));
    CHECK(!t.find_wall_ex(w, from, nullptr, 0, &info));
    for (float bad : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()}) {
        CHECK(!t.block_at(w, bad, 2, 3, &info));
        from[0] = bad;
        CHECK(!t.find_wall(w, from, to, &info));
    }
    from[0] = 1;
    tick_thread = false;
    CHECK(!t.block_at(w, 1, 2, 3, &info) && !t.find_wall(w, from, to, &info));
    tick_thread = true; t_ctx.world_cb = false;
    CHECK(!t.block_at(w, 1, 2, 3, &info));
    t_ctx.world_cb = true;
    CHECK(!t.block_at(nullptr, 1, 2, 3, &info) && !t.block_at(reinterpret_cast<EmfWorld*>(0x2000), 1, 2, 3, &info));
    { ContextScope other(&mod, nullptr, true); CHECK(!t.block_at(w, 1, 2, 3, &info)); }
    build.header.size = offsetof(KilnBuildApi, block_at);
    CHECK(!t.block_at(w, 1, 2, 3, &info));
    build.header.size = offsetof(KilnBuildApi, find_wall);
    CHECK(!t.find_wall(w, from, to, &info) && !t.find_wall_ex(w, from, to, 0, &info));
    build.header.size = offsetof(KilnBuildApi, break_box);
    auto old_service_request = request();
    CHECK(!t.break_blocks(w, &old_service_request, &callback, nullptr) && std::string(old_service_request.why) == "not available on this game version");
    build.header.size = sizeof build;
    // Queue validation: short requests untouched, exact upstream reason strings, no callbacks.
    auto r = request(); r.size = 159; std::memset(r.why, 'x', sizeof r.why); auto saved_r = r;
    CHECK(t.break_blocks(w, &r, &callback, nullptr) == 0 && std::memcmp(&r, &saved_r, sizeof r) == 0);
    CHECK(t.break_blocks(w, nullptr, &callback, nullptr) == 0);
    r = request(); CHECK(!t.break_blocks(nullptr, &r, nullptr, nullptr) && std::string(r.why).find("not a server world") == 0);
    for (int test = 0; test < 12; ++test) {
        r = request(); uint8_t material = 128;
        switch (test) {
        case 0: r.flags = 16; break;
        case 1: r.x = std::numeric_limits<float>::quiet_NaN(); break;
        case 2: r.radius = std::numeric_limits<float>::infinity(); break;
        case 3: r.radius = .24f; break;
        case 4: r.radius = 3.51f; break;
        case 5: r.height = -1; break;
        case 6: r.height = 4.1f; break;
        case 7: r.radius = .25f; r.height = 4; break;
        case 8: r.damage[0] = -1; break;
        case 9: r.damage[4] = 100001; break;
        case 10: r.material_count = 1; r.materials = &material; break;
        case 11: r.material_count = 1; break;
        }
        CHECK(!t.break_blocks(w, &r, &callback, nullptr) && r.why[0]);
    }
    for (const char* c : {"build.block_metadata", "build.block_provenance", "build.break_damage", "build.break_drops", "build.break_terrain"}) {
        capabilities[c] = false; r = request();
        if (std::string(c) == "build.break_damage") r.flags = 0;
        if (std::string(c) == "build.break_drops") r.flags |= EMF_BREAK_DROPS;
        if (std::string(c) == "build.break_terrain") r.flags |= EMF_BREAK_TERRAIN;
        CHECK(!t.break_blocks(w, &r, &callback, nullptr) && r.why[0]);
        capabilities[c] = true;
    }
    capabilities["build.block_provenance"] = false;
    r = request(); r.flags |= EMF_BREAK_TERRAIN;
    CHECK(!t.break_blocks(w, &r, nullptr, nullptr) && std::string(r.why) == "breaking terrain is not available on this game version");
    r = request(); r.flags |= EMF_BREAK_WORLD;
    CHECK(t.break_blocks(w, &r, nullptr, nullptr) != 0); // WORLD can include unknown buildings without provenance
    r.flags |= EMF_BREAK_TERRAIN;
    CHECK(!t.break_blocks(w, &r, nullptr, nullptr) && std::string(r.why) == "breaking terrain is not available on this game version");
    capabilities["build.block_provenance"] = true;
    terrain_world_stopping(); terrain_world_started(); terrain_after_step(w, EMF_WORLD_SERVER);
    CHECK(answers.empty() && break_calls == 0);
    // Request owns the material mask, flags and callback context, even from another thread.
    uint8_t materials[] = {0, 7, 127}; r = request(); r.material_count = 3; r.materials = materials;
    r.flags |= EMF_BREAK_TERRAIN | EMF_BREAK_WORLD | EMF_BREAK_DROPS;
    int token = 42; uint32_t id = 0;
    std::thread worker([&] { id = t.break_blocks(w, &r, &callback, &token); }); worker.join();
    CHECK(id != 0 && r.why[0] == 0 && answers.empty() && break_calls == 0);
    std::fill(std::begin(materials), std::end(materials), 33);
    terrain_server_pre();
    CHECK(break_calls == 1 && answers.empty());
    CHECK(requests.back().flags == 31 && requests.back().build_flags == KILN_BUILD_F_IN_BASES && requests.back().actor == 0);
    CHECK(requests.back().materials[16] == 129 && requests.back().materials[31] == 128);
    CHECK(requests.back().materials[0] == 0xff && origin == "emf:terrain-test");
    CHECK(requests.back().size == 184 && requests.back().terrain_health_scale == 250 && requests.back().terrain_heal_seconds == 20);
    CHECK(requests.back().sphere_center.x == r.x && requests.back().sphere_center.y == r.y && requests.back().sphere_center.z == r.z && requests.back().sphere_radius == 1 && requests.back().sphere_margin == .01 && requests.back().sphere_minimum == .25);
    CHECK(requests.back().damage[0] == 100 && requests.back().damage[2] == 95 && requests.back().damage[4] == 50);
    // Rasterization uses Kiln's actual grid origin, cell centres and x-fastest layout; corners are excluded.
    const auto& q = requests.back(); const auto& mask = masks.back();
    size_t n = 0, selected = 0;
    for (uint32_t z = 0; z < q.dims[2]; ++z) for (uint32_t y = 0; y < q.dims[1]; ++y) for (uint32_t x = 0; x < q.dims[0]; ++x) {
        double dx = -10 + (q.origin[0] + x + .5)*.5 - r.x;
        double dy = (q.origin[1] + y + .5)*.5 - r.y;
        double dz = -20 + (q.origin[2] + z + .5)*.5 - r.z;
        CHECK(bool(mask[n++]) == (dx*dx + dy*dy + dz*dz <= 1)); selected += mask[n-1] != 0;
    }
    CHECK(selected > 0 && selected < mask.size());
    terrain_after_step(w, EMF_WORLD_CLIENT); CHECK(answers.empty());
    terrain_after_step(w, EMF_WORLD_SERVER);
    CHECK(answers.size() == 1 && answers.back().id == id && answers.back().user == &token && answers.back().world == w);
    CHECK(answers.back().result.status == EMF_BREAK_DONE && answers.back().result.damaged == 3 && answers.back().result.removed == 2 && answers.back().result.dropped == 1);
    terrain_after_step(w, EMF_WORLD_SERVER); CHECK(answers.size() == 1);
    r = request(); r.radius = 3.5f;
    CHECK(t.break_blocks(w, &r, nullptr, nullptr));
    terrain_server_pre(); terrain_after_step(w, EMF_WORLD_SERVER);
    CHECK(requests.back().dims[0] == 15 && requests.back().dims[1] == 15 && requests.back().dims[2] == 15 && masks.back().size() == 3375);
    // Non-instant terrain damage forwards the caller's five entries, including zero entries, unchanged.
    r = request(); r.flags = EMF_BREAK_TERRAIN; r.damage[0] = 12; r.damage[2] = 34; r.damage[4] = 56;
    const auto damage_id = t.break_blocks(w, &r, &callback, nullptr);
    CHECK(damage_id != 0);
    terrain_server_pre(); terrain_after_step(w, EMF_WORLD_SERVER);
    CHECK(!(requests.back().flags & KILN_BUILD_BREAK_INSTANT) && requests.back().damage[0] == 12 && requests.back().damage[1] == 0 && requests.back().damage[2] == 34 && requests.back().damage[3] == 0 && requests.back().damage[4] == 56);
    CHECK(answers.back().id == damage_id && answers.back().result.status == EMF_BREAK_DONE && answers.back().result.dropped == 0);
    // Whole columns are FIFO, max eight spheres per step, max 64 queued requests.
    r = request(); r.radius = .25; r.height = 1.75; // eight spheres
    auto column = t.break_blocks(w, &r, &callback, nullptr);
    r = request(); auto next = t.break_blocks(w, &r, &callback, nullptr);
    auto before = break_calls; terrain_server_pre(); CHECK(break_calls == before + 8);
    terrain_after_step(w, EMF_WORLD_SERVER); CHECK(answers.back().id == column && answers.back().result.damaged == 24 && answers.back().result.dropped == 0);
    terrain_server_pre(); terrain_after_step(w, EMF_WORLD_SERVER); CHECK(answers.back().id == next);
    for (int i = 0; i < 64; ++i) { r = request(); CHECK(t.break_blocks(w, &r, &callback, nullptr) != 0); }
    r = request(); CHECK(!t.break_blocks(w, &r, &callback, nullptr) && std::string(r.why) == "64 break requests are already waiting");
    auto count = answers.size(); terrain_world_stopping(); CHECK(answers.size() == count);
    r = request(); CHECK(!t.break_blocks(w, &r, nullptr, nullptr));
    terrain_world_started(); ++session;
    terrain_after_step(w, EMF_WORLD_SERVER); CHECK(answers.size() == count + 64 && answers.back().result.status == EMF_BREAK_CANCELLED);
    // Multiple producers share the same 64-request limit and never receive duplicate ids.
    std::vector<uint32_t> ids[8];
    std::vector<std::thread> producers;
    for (int tno = 0; tno < 8; ++tno) producers.emplace_back([&,tno] {
        for (int i = 0; i < 10; ++i) { auto local = request(); ids[tno].push_back(t.break_blocks(w, &local, nullptr, nullptr)); }
    });
    for (auto& p : producers) p.join();
    std::set<uint32_t> unique; size_t rejected = 0;
    for (const auto& producer : ids) for (auto id : producer) { if (id) unique.insert(id); else ++rejected; }
    CHECK(unique.size() == 64 && rejected == 16);
    terrain_world_stopping(); terrain_world_started(); terrain_after_step(w, EMF_WORLD_SERVER);
    // Session change without a lifecycle notification still cancels, and never edits a replacement world.
    r = request(); CHECK(t.break_blocks(w, &r, &callback, nullptr)); ++session; before = break_calls;
    terrain_server_pre(); terrain_after_step(w, EMF_WORLD_SERVER);
    CHECK(break_calls == before && answers.back().result.status == EMF_BREAK_CANCELLED);
    // Watchdog cancels after 3 seconds without a runner; it delivers on Post, never the caller's thread.
    r = request(); CHECK(t.break_blocks(w, &r, &callback, nullptr)); clock_ms += 3001;
    terrain_after_step(w, EMF_WORLD_SERVER); CHECK(answers.back().result.status == EMF_BREAK_CANCELLED);
    // EMF skips an unloaded sphere without damage, rather than failing the accepted request.
    r = request(); CHECK(t.break_blocks(w, &r, &callback, nullptr));
    break_rc = KILN_ERR_UNAVAILABLE; break_refusal = KILN_BUILD_BLOCK_NOT_LOADED; break_result_flags = 0;
    terrain_server_pre(); terrain_after_step(w, EMF_WORLD_SERVER);
    CHECK(answers.back().result.status == EMF_BREAK_DONE);
    CHECK(answers.back().result.damaged == 0 && answers.back().result.removed == 0 && answers.back().result.dropped == 0);
    break_rc = KILN_OK; break_refusal = 0; break_result_flags = KILN_BUILD_BREAK_R_VERIFIED;
    // Failure after admission: no retries; later requests in the batch fail without running.
    r = request(); auto failed = t.break_blocks(w, &r, &callback, nullptr); auto skipped = t.break_blocks(w, &r, &callback, nullptr);
    break_rc = KILN_ERR_UNAVAILABLE; break_refusal = KILN_BUILD_BLOCK_SYMBOL_MISSING; before = break_calls;
    terrain_server_pre(); terrain_after_step(w, EMF_WORLD_SERVER);
    CHECK(break_calls == before + 1 && answers[answers.size()-2].id == failed && answers.back().id == skipped && answers.back().result.status == EMF_BREAK_FAILED);
    break_rc = KILN_OK; break_refusal = 0;
    // A mod callback fault cannot suppress other callbacks.
    CHECK(t.break_blocks(w, &r, [](EmfWorld*, uint32_t, const EmfBreakResult*, void*) { throw 1; }, nullptr));
    CHECK(t.break_blocks(w, &r, &callback, nullptr));
    count = answers.size(); terrain_server_pre(); terrain_after_step(w, EMF_WORLD_SERVER); CHECK(answers.size() == count + 1);
    // Partial writer faults retain verified/lower-bound counts, explicitly log uncertainty and latch breaks off.
    for (int i = 0; i < 9; ++i) CHECK(t.break_blocks(w, &r, &callback, nullptr));
    before = break_calls;
    break_rc = KILN_ERR_UNAVAILABLE; break_refusal = KILN_BUILD_BLOCK_NOT_LOADED; break_result_flags = KILN_BUILD_BREAK_R_MAY_HAVE_CHANGED;
    terrain_server_pre(); terrain_after_step(w, EMF_WORLD_SERVER);
    CHECK(answers[answers.size()-8].result.status == EMF_BREAK_FAILED && answers[answers.size()-8].result.removed == 2);
    terrain_server_pre(); terrain_after_step(w, EMF_WORLD_SERVER);
    CHECK(break_calls == before + 1 && answers.back().result.status == EMF_BREAK_FAILED);
    CHECK(!t.break_blocks(w, &r, nullptr, nullptr) && std::string(r.why).find("off") != std::string::npos);
    // Once a write fault latched breaking off, the restart reason takes priority over unavailable capabilities.
    capabilities["build.break_building"] = false;
    CHECK(!t.break_blocks(w, &r, nullptr, nullptr) && std::string(r.why).find("off (") == 0);
    capabilities["build.break_building"] = true;
    CHECK(terrain_status_json().find("\"partial\":1") != std::string::npos);
    std::printf("terrain_test: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
