// SPDX-License-Identifier: MIT
// Host test: kiln.build's task-thread voxel grid (voxel_tasks.cpp, voxel_tasks_hold.cpp) over the real kiln.engine
// (booted through kiln_plugin_main with a fake core: its views, its write gate and its guarded copies) and a fake engine
// laid out from each exact profile, with a fake voxel world (ecs::VoxelGrid's interface, its wrapper, the VoxelWorld,
// its store) in anonymous memory. Every outcome is forced: invalid arguments, an unknown grid, off on this build, no
// server world, a stale session, nothing published yet, a frame holding the Scene word, the busy latch (the caller's
// own: another owner and the dialect owner are not latched) and its next step, a call on the server thread, a client joined to another machine's world (no local authority), fields that are
// not ready, an interface that is not sigdb's, a voxel world that moves between the two copies, a faulting copy, a
// session that ends during the call. Inside the hold: no allocation and no call into the core. Callers racing a
// publishing tick and a frame under ThreadSanitizer.
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <new>
#include <string>
#include <thread>
#include <vector>

#include "engine_state.h"
#include "fake_engine.h"
#include "gate.h"
#include "include/kiln_engine_native.h"
#include "profiles.g.h"
#include "services/kiln_caps.h"

#include "../voxel_tasks.h"

extern "C" KilnResult KILN_CALL kiln_plugin_main(const KilnApi*, KilnOwner, KilnPluginInfo*);
namespace kiln::engine::mem {
extern std::atomic<std::uintptr_t> test_image;
}

namespace {
thread_local bool g_in_hold = false;
std::atomic<long> g_allocations_in_hold{0}, g_core_calls_in_hold{0};
}  // namespace

void* operator new(std::size_t n) {
    if (g_in_hold) g_allocations_in_hold.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

namespace {
using namespace kiln::engine;
using kb::tasks::Grid;
using kb::tasks::HoldPoint;
using kb::tasks::Offsets;
using kb::tasks::Published;
using kb::tasks::Source;

constexpr KilnOwner kOwner = 31, kOtherOwner = 32, kDialectOwner = 33;  // the callers' own owners
std::uint64_t KILN_CALL dialect_owner() { return kDialectOwner; }

int checks = 0, failures = 0;
std::string g_case;
void check(bool ok, const std::string& what) {
    ++checks;
    if (!ok) {
        ++failures;
        std::fprintf(stderr, "FAIL [%s] %s\n", g_case.c_str(), what.c_str());
    }
}

// ---- a fake core for the real kiln-engine entry (as native/services/engine/tests/plugin_host_test.cpp) ----
void core_call() {
    if (g_in_hold) g_core_calls_in_hold.fetch_add(1);
}
ImageFacts g_facts;
std::map<std::string, const void*> g_tables;
KilnCapsApi g_caps{};
KilnResult KILN_CALL cap_report(KilnOwner, KilnStr, uint32_t, uint32_t, KilnStr) { return KILN_OK; }
const void* KILN_CALL lookup(KilnStr n, uint32_t v) {
    core_call();
    const std::string name(n.ptr, n.len);
    if (name == KILN_CAPS_SERVICE) return &g_caps;
    const auto at = g_tables.find(name);
    if (at == g_tables.end()) return nullptr;
    return static_cast<const KilnServiceHeader*>(at->second)->version >= v ? at->second : nullptr;
}
KilnResult KILN_CALL publish_table(KilnOwner, KilnStr n, const void* t) {
    core_call();
    g_tables[std::string(n.ptr, n.len)] = t;
    return KILN_OK;
}
void KILN_CALL logger(KilnOwner, int32_t, KilnStr) { core_call(); }
KilnResult KILN_CALL build(KilnBuildInfo* out) {
    core_call();
    out->side = KILN_SIDE_CLIENT;
    return KILN_OK;
}
thread_local bool g_on_server_thread = false;
bool KILN_CALL on_world(uint32_t kind) {
    core_call();
    return kind == KILN_WORLD_SERVER && g_on_server_thread;
}
KilnResult KILN_CALL subscribe_world(KilnOwner, KilnWorldEventFn, void*, KilnHandle* out) {
    core_call();
    *out = 0;
    return KILN_OK;
}
KilnResult KILN_CALL subscribe_tick(KilnOwner, int32_t, KilnTickFn, void*, KilnHandle* out) {
    core_call();
    *out = 0;
    return KILN_OK;
}
KilnApi make_api() {
    KilnApi api{};
    api.size = sizeof api;
    api.abi_version = KILN_ABI_VERSION;
    api.service = &lookup;
    api.register_service = &publish_table;
    api.log = &logger;
    api.build_info = &build;
    api.on_world_thread = &on_world;
    api.subscribe_world = &subscribe_world;
    api.subscribe_tick = &subscribe_tick;
    return api;
}
const KilnEngineNativeApi* boot_engine(const Profile& p, KilnApi& api) {
    reset_for_tests();
    g_tables.clear();
    g_facts = {};
    g_facts.headers = true;
    g_facts.target = p.target;
    g_facts.sha256 = p.sha256;
    g_facts.timestamp = p.timestamp;
    g_facts.size_of_image = p.size_of_image;
    g_facts.archive_format = 4;
    g_facts.side = std::string(p.target) == "enshrouded.exe" ? KILN_SIDE_CLIENT : KILN_SIDE_SERVER;
    KilnPluginInfo info{};
    info.size = sizeof info;
    if (kiln_plugin_main(&api, 7, &info) != KILN_OK) return nullptr;
    char why[160]{};
    return kiln_engine_native(&api, 7, sizeof(KilnEngineNativeApi), why, sizeof why);
}

// ---- the engine's table with test hooks: a copy that can fault, a view that can be a client replica ----
const KilnEngineNativeApi* g_real = nullptr;
std::atomic<uintptr_t> g_fail_lo{0}, g_fail_hi{0};
std::atomic<bool> g_replica{false};
std::atomic<long> g_hold_copies{0};
KilnResult KILN_CALL test_copy(std::uintptr_t a, void* out, std::size_t n) {
    if (g_in_hold) g_hold_copies.fetch_add(1, std::memory_order_relaxed);
    if (n && a < g_fail_hi.load() && a + n > g_fail_lo.load()) return KILN_ERR_UNAVAILABLE;
    return g_real->copy(a, out, n);
}
KilnResult KILN_CALL test_view(std::uint32_t kind, EcsView* out, std::uint32_t* reason) {
    const KilnResult r = g_real->ecs_view(kind, out, reason);
    if (r == KILN_OK && g_replica.load()) out->role = Role::ClientReplica;
    return r;
}
KilnEngineNativeApi g_hooked{};

// ---- the fake voxel world (offsets as sigdb's build.json on the current builds: docs/re/build.md section 2) ----
constexpr int64_t kGridIface = 0x28, kWrapperWorld = 8, kInit = 0x14, kOrigin = 0x30, kLevels = 0x40, kLevel = 6,
                  kStride = 0x28, kStore = 0x1220, kStoreUnavailable = 0x2c3, kResources = 0x160;
struct Voxels {
    fake::Region grid{4096}, wrapper{4096}, world{0x2000}, store{4096}, resources{4096}, handles{80 * 80 * 4},
        decoy{0x2000};  // another, zeroed VoxelWorld
    std::uintptr_t read, ready;
    explicit Voxels(const fake::Engine& e) : read(e.image.at(0x1100)), ready(e.image.at(0x1140)) {
        fake::put<uint64_t>(grid.at(kGridIface), wrapper.at());
        fake::put<uint64_t>(grid.at(kGridIface + 8), read);
        fake::put<uint64_t>(grid.at(kGridIface + 16), e.image.at(0x1120));
        fake::put<uint64_t>(grid.at(kGridIface + 24), ready);
        fake::put<uint64_t>(wrapper.at(kWrapperWorld), world.at());
        fake::put<uint32_t>(world.at(kInit), 1);
        const float origin[3] = {0.f, -4.f, 0.f};
        std::memcpy(reinterpret_cast<void*>(world.at(kOrigin)), origin, sizeof origin);
        kb::tasks::Level l;
        l.tile_x = l.tile_z = 256;
        l.count_x = l.count_z = 80;
        l.allocator = 0x10;
        l.handles = handles.at();
        l.handle_count = 80 * 80;
        fake::put(world.at(kLevels + kLevel * kStride), l);
        fake::put<uint64_t>(world.at(kStore), store.at());
        fake::put<uint64_t>(world.at(kResources), resources.at());
        fake::put<uint8_t>(store.at(kStoreUnavailable), 0);
    }
};
Offsets offsets_of(const Voxels& v) {
    Offsets o;
    o.grid_iface = kGridIface, o.wrapper_world = kWrapperWorld, o.world_init = kInit, o.world_origin = kOrigin;
    o.world_levels = kLevels, o.level = kLevel, o.level_stride = kStride, o.world_store = kStore;
    o.store_unavailable = kStoreUnavailable, o.world_resources = kResources, o.read = v.read, o.ready = v.ready;
    o.cell = 32.0 / 64.0;
    return o;
}

// Kiln's own server world facts, scripted.
struct Server {
    std::atomic<uintptr_t> world{0};
    std::atomic<uint64_t> session{0}, step{100};
    std::atomic<int> change_after{-1};  // after this many more samples, the session moves on
    std::atomic<bool> moving{false};     // every sample sees a moving session (unreadable)
};
Server g_server;
bool server_fact(void*, uintptr_t* world, uint64_t* session) {
    if (g_server.moving.load()) return false;
    if (g_server.change_after.load() >= 0 && g_server.change_after.fetch_sub(1) == 0) g_server.session.fetch_add(1);
    *world = g_server.world.load();
    *session = g_server.session.load();
    return true;
}
uint64_t server_step(void*) { return g_server.step.load(); }

std::function<void(HoldPoint)> g_point;
void on_hold_point(HoldPoint p) {
    if (p == HoldPoint::Begin) g_in_hold = true;
    if (g_point) {
        const bool was = g_in_hold;
        g_in_hold = false;
        const auto point = g_point;
        point(p);
        g_in_hold = was;
    }
}
void on_release(bool begin) {
    if (begin) g_in_hold = false;
}

KilnBuildGridSpec blank() {
    KilnBuildGridSpec s;
    std::memset(&s, 0xab, sizeof s);
    s.size = sizeof s;
    return s;
}
bool untouched(const KilnBuildGridSpec& s) {
    const KilnBuildGridSpec b = blank();
    return std::memcmp(&s, &b, sizeof s) == 0;
}
KilnStr str(const char* s) { return KilnStr{s, std::strlen(s)}; }

void exact(const Profile& p) {
    g_case = p.id;
    g_hold_copies = 0;
    g_allocations_in_hold = 0;
    g_core_calls_in_hold = 0;
    KilnApi api = make_api();
    const KilnEngineNativeApi* native = boot_engine(p, api);
    check(native != nullptr, "the real kiln.internal.engine table");
    if (!native) return;
    g_real = native;
    g_hooked = *native;
    g_hooked.copy = &test_copy;
    g_hooked.ecs_view = &test_view;
    fake::Engine e(p.layout);
    kiln::engine::mem::test_image = e.image.at();
    Voxels v(e);
    const Offsets off = offsets_of(v);
    g_server.world = e.exec.at();
    g_server.session = 41;
    g_server.step = 100;
    Source src;
    src.engine = &g_hooked;
    src.server = &server_fact;
    src.server_step = &server_step;
    Grid grid;
    const Published pub{41, e.exec.at(), v.grid.at(), v.wrapper.at(), v.world.at()};
    const auto call = [&](uint64_t session, const char* id, KilnBuildGridSpec& out) {
        return grid.grid_for_session(kOwner, session, str(id), &out);
    };
    auto next_step = [] { g_server.step.fetch_add(1); };

    // ---- off, before anything ----
    {
        KilnBuildGridSpec s = blank();
        check(call(41, "voxel", s) == KILN_ERR_UNAVAILABLE && untouched(s), "before configure: unavailable");
        grid.configure(src, off, "kiln.build's voxel reads are off on this build (build.read)");
        check(!grid.available() && grid.unavailable_reason().find("build.read") != std::string::npos,
              "configured off: the reason is kept");
        check(call(41, "voxel", s) == KILN_ERR_UNAVAILABLE && untouched(s), "configured off: unavailable");
        Source no_gate = src;
        KilnEngineNativeApi short_table = g_hooked;
        short_table.header.size = offsetof(KilnEngineNativeApi, ecs_gate_acquire);
        no_gate.engine = &short_table;
        grid.configure(no_gate, off, nullptr);
        check(!grid.available() && call(41, "voxel", s) == KILN_ERR_UNAVAILABLE, "no gate members: unavailable");
        Offsets missing = off;
        missing.world_store = -1;
        grid.configure(src, missing, nullptr);
        check(!grid.available() && grid.unavailable_reason().find("layout") != std::string::npos,
              "an incomplete layout: unavailable");
        grid.configure(src, off, nullptr);
        check(grid.available(), "configured on");
    }
    // ---- arguments ----
    {
        KilnBuildGridSpec s = blank();
        check(grid.grid_for_session(kOwner, 41, str("voxel"), nullptr) == KILN_ERR_INVALID_ARGUMENT, "out NULL");
        KilnBuildGridSpec short_spec = blank();
        short_spec.size = sizeof short_spec - 1;
        check(grid.grid_for_session(kOwner, 41, str("voxel"), &short_spec) == KILN_ERR_INVALID_ARGUMENT, "short record");
        check(call(0, "voxel", s) == KILN_ERR_INVALID_ARGUMENT && untouched(s), "session 0");
        check(grid.grid_for_session(0, 41, str("voxel"), &s) == KILN_ERR_INVALID_ARGUMENT && untouched(s), "owner 0");
        check(grid.grid_for_session(kOwner, 41, KilnStr{nullptr, 0}, &s) == KILN_ERR_INVALID_ARGUMENT, "empty id");
        check(grid.grid_for_session(kOwner, 41, KilnStr{"vox\0l", 5}, &s) == KILN_ERR_INVALID_ARGUMENT, "a NUL in the id");
        check(call(41, "voxel-voxel-voxe", s) == KILN_ERR_INVALID_ARGUMENT, "an id of 16 bytes");
        check(call(41, "voxels", s) == KILN_ERR_NOT_FOUND && call(41, "Voxel", s) == KILN_ERR_NOT_FOUND &&
                  call(41, "fog", s) == KILN_ERR_NOT_FOUND && untouched(s),
              "another grid: not found");
    }
    // ---- the world and the session ----
    {
        KilnBuildGridSpec s = blank();
        g_server.world = 0;
        check(call(41, "voxel", s) == KILN_ERR_UNAVAILABLE && untouched(s), "no server world: unavailable");
        g_server.world = e.exec.at();
        check(call(40, "voxel", s) == KILN_ECS_ERR_STALE && call(42, "voxel", s) == KILN_ECS_ERR_STALE && untouched(s),
              "another session: stale");
        g_server.moving = true;
        check(call(41, "voxel", s) == KILN_ECS_ERR_BUSY && untouched(s), "a moving session sample: busy");
        g_server.moving = false;
        check(call(41, "voxel", s) == KILN_ECS_ERR_BUSY && untouched(s), "nothing published yet: busy");
        Published old = pub;
        old.session = 40;
        grid.publish(old);
        check(call(41, "voxel", s) == KILN_ECS_ERR_BUSY, "published for an older session: busy");
        grid.publish(pub);
    }
    // ---- the spec ----
    {
        KilnBuildGridSpec s = blank();
        const KilnResult rc = call(41, "voxel", s);
        check(rc == KILN_OK, "ok (" + std::to_string(rc) + ")");
        check(s.size == sizeof s && std::strcmp(s.id, "voxel") == 0 && s.origin[0] == 0 && s.origin[1] == -4 &&
                  s.origin[2] == 0 && s.cell_size[0] == 0.5 && s.cell_size[1] == 0.5 && s.cell_size[2] == 0.5 &&
                  s.maximum[0] == 20480 && s.maximum[1] == 0x7fffffffull - 7 && s.maximum[2] == 20480 && s.reserved == 0,
              "the spec: id, origin, cell size, maximum");
        check(g_allocations_in_hold.load() == 0 && g_core_calls_in_hold.load() == 0,
              "inside the hold: no allocation, no call into the core");
        check(g_hold_copies.load() == 16, "two snapshots of eight guarded copies each (" +
                                              std::to_string(g_hold_copies.load()) + ")");
        g_hold_copies = 0;
        check(*fake::word(e.scene_word()) == 0 && *fake::word(e.registry_word()) == 0, "both engine words released");
        KilnBuildGridSpec big = blank();
        big.size = sizeof big + 32;
        check(grid.grid_for_session(kOwner, 41, str("voxel"), &big) == KILN_OK && big.size == sizeof big + 32,
              "a longer record keeps its size");
    }
    // ---- the gate ----
    {
        KilnBuildGridSpec s = blank();
        next_step();
        *fake::word(e.scene_word()) = 1;  // a frame holds the Scene word Shared
        check(call(41, "voxel", s) == KILN_ECS_ERR_BUSY && untouched(s), "a frame holds the Scene word: busy");
        *fake::word(e.scene_word()) = 0;
        check(call(41, "voxel", s) == KILN_ECS_ERR_BUSY && untouched(s), "...and the owner is latched for the step");
        check(grid.grid_for_session(kOtherOwner, 41, str("voxel"), &s) == KILN_OK,
              "...but only that owner: another caller in the same step is not latched");
        next_step();
        check(call(41, "voxel", s) == KILN_OK, "the next step: ok");
        // The interpreter's owner (the ShroudForge-dialect owner the core designates) is never latched, as at its own
        // gate calls: a busy refusal leaves its next attempt in the same step free.
        next_step();
        kiln::engine::gate_set_dialect_owner(&dialect_owner);
        s = blank();
        *fake::word(e.scene_word()) = 1;
        check(grid.grid_for_session(kDialectOwner, 41, str("voxel"), &s) == KILN_ECS_ERR_BUSY && untouched(s),
              "the dialect owner during a frame: busy");
        *fake::word(e.scene_word()) = 0;
        check(grid.grid_for_session(kDialectOwner, 41, str("voxel"), &s) == KILN_OK,
              "...and not latched: the same step between frames is ok");
        kiln::engine::gate_set_dialect_owner(nullptr);
        next_step();
        g_on_server_thread = true;
        s = blank();
        check(call(41, "voxel", s) == KILN_ECS_ERR_BUSY && untouched(s), "on the server tick: busy (kiln-in-frame)");
        g_on_server_thread = false;
        next_step();
        g_replica = true;
        check(call(41, "voxel", s) == KILN_ERR_UNAVAILABLE && untouched(s), "not the local authority: unavailable");
        g_replica = false;
        check(call(41, "voxel", s) == KILN_OK, "the local authority again: ok");
    }
    // ---- the voxel world's fields ----
    const auto busy_when = [&](const std::string& what, auto&& set, auto&& reset) {
        KilnBuildGridSpec s = blank();
        set();
        check(call(41, "voxel", s) == KILN_ECS_ERR_BUSY && untouched(s), what + ": busy");
        reset();
        s = blank();
        check(call(41, "voxel", s) == KILN_OK, what + " undone: ok");
    };
    busy_when("not initialized", [&] { fake::put<uint32_t>(v.world.at(kInit), 0); },
              [&] { fake::put<uint32_t>(v.world.at(kInit), 1); });
    busy_when("the store unavailable", [&] { fake::put<uint8_t>(v.store.at(kStoreUnavailable), 1); },
              [&] { fake::put<uint8_t>(v.store.at(kStoreUnavailable), 0); });
    busy_when("no store", [&] { fake::put<uint64_t>(v.world.at(kStore), 0); },
              [&] { fake::put<uint64_t>(v.world.at(kStore), v.store.at()); });
    busy_when("no resources", [&] { fake::put<uint64_t>(v.world.at(kResources), 0); },
              [&] { fake::put<uint64_t>(v.world.at(kResources), v.resources.at()); });
    busy_when("a tile 7 cells wide", [&] { fake::put<uint32_t>(v.world.at(kLevels + kLevel * kStride), 7); },
              [&] { fake::put<uint32_t>(v.world.at(kLevels + kLevel * kStride), 256); });
    busy_when("a handle count that is not the tiles'",
              [&] { fake::put<uint64_t>(v.world.at(kLevels + kLevel * kStride + 32), 1); },
              [&] { fake::put<uint64_t>(v.world.at(kLevels + kLevel * kStride + 32), 6400); });
    busy_when("a non-finite origin", [&] { fake::put<float>(v.world.at(kOrigin + 4), NAN); },
              [&] { fake::put<float>(v.world.at(kOrigin + 4), -4.f); });
    busy_when("a read slot that is not sigdb's", [&] { fake::put<uint64_t>(v.grid.at(kGridIface + 8), v.read + 1); },
              [&] { fake::put<uint64_t>(v.grid.at(kGridIface + 8), v.read); });
    busy_when("a ready slot that is not sigdb's", [&] { fake::put<uint64_t>(v.grid.at(kGridIface + 24), 0); },
              [&] { fake::put<uint64_t>(v.grid.at(kGridIface + 24), v.ready); });
    busy_when("another wrapper than the published one",
              [&] { fake::put<uint64_t>(v.grid.at(kGridIface), v.store.at()); },
              [&] { fake::put<uint64_t>(v.grid.at(kGridIface), v.wrapper.at()); });
    busy_when("another VoxelWorld than the published one",
              [&] { fake::put<uint64_t>(v.wrapper.at(kWrapperWorld), v.decoy.at()); },
              [&] { fake::put<uint64_t>(v.wrapper.at(kWrapperWorld), v.world.at()); });
    busy_when("no wrapper", [&] { fake::put<uint64_t>(v.grid.at(kGridIface), 0); },
              [&] { fake::put<uint64_t>(v.grid.at(kGridIface), v.wrapper.at()); });
    // ---- inside the hold ----
    {
        KilnBuildGridSpec s = blank();
        g_point = [&](HoldPoint hp) {
            if (hp == HoldPoint::BetweenCopies) fake::put<float>(v.world.at(kOrigin), 1.f);
        };
        check(call(41, "voxel", s) == KILN_ECS_ERR_BUSY && untouched(s), "the origin moves between the copies: busy");
        g_point = nullptr;
        fake::put<float>(v.world.at(kOrigin), 0.f);
        g_point = [&](HoldPoint hp) {
            if (hp == HoldPoint::BetweenCopies)
                fake::put<uint32_t>(v.world.at(kLevels + kLevel * kStride + 8), 81);  // the tile count changes
        };
        check(call(41, "voxel", s) == KILN_ECS_ERR_BUSY && untouched(s), "the level moves between the copies: busy");
        g_point = nullptr;
        fake::put<uint32_t>(v.world.at(kLevels + kLevel * kStride + 8), 80);
        g_fail_lo = v.world.at(kOrigin);
        g_fail_hi = v.world.at(kOrigin + 12);
        check(call(41, "voxel", s) == KILN_ECS_ERR_UNREADABLE && untouched(s), "a faulting copy: unreadable");
        g_fail_lo = g_fail_hi = 0;
        const Published gone{41, e.exec.at(), fake::unmapped(), v.wrapper.at(), v.world.at()};
        grid.publish(gone);
        check(call(41, "voxel", s) == KILN_ECS_ERR_UNREADABLE && untouched(s), "an unmapped grid: unreadable");
        grid.publish(pub);
        check(*fake::word(e.scene_word()) == 0 && *fake::word(e.registry_word()) == 0, "every refusal released the gate");
        check(call(41, "voxel", s) == KILN_OK, "ok again");
    }
    // ---- the session ends during the call ----
    {
        KilnBuildGridSpec s = blank();
        g_server.change_after = 1;  // the first sample is the call's, the second (after the release) sees session 42
        check(call(41, "voxel", s) == KILN_ECS_ERR_STALE && untouched(s), "the session moved on during the call: stale");
        g_server.change_after = -1;
        check(call(41, "voxel", s) == KILN_ECS_ERR_STALE, "the old session stays stale");
        Published next = pub;
        next.session = 42;
        grid.publish(next);
        check(call(42, "voxel", s) == KILN_OK, "the new session once published: ok");
        grid.clear();
        check(call(42, "voxel", s) == KILN_ECS_ERR_BUSY, "cleared (the world stops): busy until the tick publishes");
        grid.publish(next);
    }
    // ---- racing callers, a publishing tick and a frame (ThreadSanitizer) ----
    {
        std::atomic<bool> stop{false};
        std::atomic<long> ok{0}, busy{0}, other{0};
        std::vector<std::thread> callers;
        for (int t = 0; t < 3; ++t)
            callers.emplace_back([&] {
                for (int i = 0; i < 300; ++i) {
                    KilnBuildGridSpec s = blank();
                    const KilnResult r = grid.grid_for_session(kOwner, 42, str("voxel"), &s);
                    if (r == KILN_OK && s.maximum[0] == 20480) ++ok;
                    else if (r == KILN_ECS_ERR_BUSY) ++busy;
                    else ++other;
                }
            });
        std::thread tick([&] {
            Published p = pub;
            p.session = 42;
            while (!stop.load()) {
                grid.publish(p);
                next_step();
                std::this_thread::yield();
            }
        });
        std::thread frame([&] {
            auto* word = fake::word(e.scene_word());
            while (!stop.load()) {
                std::int32_t free_word = 0;
                if (__atomic_compare_exchange_n(word, &free_word, 1, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
                    std::this_thread::yield();
                    __atomic_store_n(word, 0, __ATOMIC_SEQ_CST);
                }
                std::this_thread::yield();
            }
        });
        for (auto& c : callers) c.join();
        stop = true;
        tick.join();
        frame.join();
        check(other.load() == 0 && ok.load() > 0, "racing: every answer ok or busy (" + std::to_string(ok.load()) + " ok, " +
                                                      std::to_string(busy.load()) + " busy)");
        check(*fake::word(e.scene_word()) == 0 && *fake::word(e.registry_word()) == 0, "racing: the words are free");
    }
}

}  // namespace

namespace kiln::engine {
ImageFacts gather_facts(const KilnApi*) { return g_facts; }
}  // namespace kiln::engine

int main() {
    g_caps.header = {sizeof g_caps, KILN_CAPS_VERSION};
    g_caps.report = &cap_report;
    kb::tasks::g_hold_test_point = &on_hold_point;
    kiln::engine::g_gate_release_point = &on_release;
    for (const auto& p : generated::kProfiles) exact(p);
    std::printf("voxel_tasks: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
