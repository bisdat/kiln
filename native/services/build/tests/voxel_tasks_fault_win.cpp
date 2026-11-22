// SPDX-License-Identifier: MIT
// Windows test (native on a Windows runner, or headless Wine; never shipped, never built by default): kiln.build's
// task-thread voxel grid (voxel_tasks.cpp, voxel_tasks_hold.cpp) over the real Windows primitives of kiln.engine
// (memory_win.cpp: the SEH copy and the Interlocked words; resolve.cpp: the views; gate.cpp: the write gate) and a fake
// engine with a fake voxel world in VirtualAlloc'd memory, laid out from each exact profile (fake_engine.h). Pages under
// the hold's copies (the grid's interface, the wrapper, the VoxelWorld's two pages, the store) are made PAGE_NOACCESS,
// PAGE_GUARD or decommitted: every call must come back unreadable with the caller's record untouched and both engine words
// free, and the process must live on; with the pages back the same call succeeds. A page spoiled between the hold's two
// copies is unreadable too. A frame holding the Scene word is busy. A thread flips the VoxelWorld's first page while three
// callers call: no crash, every answer ok, busy or unreadable, the words free at the end. And the call's time with real
// memory.
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "engine_state.h"
#include "fake_engine.h"
#include "gate.h"
#include "include/kiln_engine_native.h"
#include "memory.h"
#include "profiles.g.h"
#include "resolve.h"

#include "../voxel_tasks.h"

namespace {
using kb::tasks::HoldPoint;
std::atomic<std::uintptr_t> g_image{0};
const kiln::engine::Layout* g_layout = nullptr;
constexpr KilnOwner kOwner = 9;  // the calling mod's owner
int checks = 0, failures = 0;
std::string g_case;
void check(bool ok, const std::string& what) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL [%s] %s\n", g_case.c_str(), what.c_str());
    }
}

// kiln.internal.engine's members, the same bodies engine.cpp publishes, over the real primitives, for a caller off the
// world threads.
const kiln::engine::Layout* KILN_CALL layout_member() { return g_layout; }
KilnResult KILN_CALL copy_member(std::uintptr_t address, void* out, std::size_t bytes) {
    if (!bytes) return KILN_OK;
    if (!out) return KILN_ERR_INVALID_ARGUMENT;
    return kiln::engine::mem::read_span(address, out, bytes) ? KILN_OK : KILN_ERR_UNAVAILABLE;
}
KilnResult KILN_CALL view_member(std::uint32_t kind, kiln::engine::EcsView* out, std::uint32_t* reason) {
    kiln::engine::EcsView v{};
    const auto r = kiln::engine::ecs_view(g_layout, static_cast<kiln::engine::ViewKind>(kind), v, g_image.load());
    *reason = static_cast<std::uint32_t>(r);
    if (r != kiln::engine::Reason::None) return KILN_ERR_UNAVAILABLE;
    *out = v;
    return KILN_OK;
}
using kiln::engine::EcsGate;
KilnResult KILN_CALL acquire_member(const kiln::engine::EcsView* expected, EcsGate* gate, std::uint32_t* reason) {
    const auto r = kiln::engine::gate_acquire(g_layout, *expected, *gate, kiln::engine::CallerThread::Other, g_image.load());
    *reason = static_cast<std::uint32_t>(r);
    return r == kiln::engine::Reason::None ? KILN_OK : KILN_ERR_UNAVAILABLE;
}
std::int32_t KILN_CALL validate_member(const EcsGate* gate) {
    return kiln::engine::gate_validate(g_layout, *gate, g_image.load()) ? 1 : 0;
}
void KILN_CALL release_member(EcsGate* gate) { kiln::engine::gate_release(*gate); }

// One page (or more) made unreadable in one of three ways, and put back.
struct Spoil {
    void* at;
    size_t size;
    int how;  // 0 PAGE_NOACCESS, 1 PAGE_GUARD, 2 decommitted
    std::vector<uint8_t> saved;
    Spoil(uintptr_t address, size_t bytes, int mode)
        : at(reinterpret_cast<void*>(address & ~uintptr_t{4095})), size((bytes + 4095) & ~size_t{4095}), how(mode) {
        DWORD old = 0;
        if (how == 2) {
            saved.resize(size);
            std::memcpy(saved.data(), at, size);
            VirtualFree(at, size, MEM_DECOMMIT);
        } else {
            VirtualProtect(at, size, how == 0 ? PAGE_NOACCESS : (PAGE_READWRITE | PAGE_GUARD), &old);
        }
    }
    ~Spoil() {
        DWORD ignored = 0;
        if (how == 2) {
            VirtualAlloc(at, size, MEM_COMMIT, PAGE_READWRITE);
            std::memcpy(at, saved.data(), size);
        } else {
            VirtualProtect(at, size, PAGE_READWRITE, &ignored);
        }
    }
};
const char* mode_name(int how) { return how == 0 ? "PAGE_NOACCESS" : how == 1 ? "PAGE_GUARD" : "decommitted"; }

// The fake voxel world (offsets as sigdb's build.json on the current builds: docs/re/build.md section 2).
constexpr int64_t kGridIface = 0x28, kWrapperWorld = 8, kInit = 0x14, kOrigin = 0x30, kLevels = 0x40, kLevel = 6,
                  kStride = 0x28, kStore = 0x1220, kStoreUnavailable = 0x2c3, kResources = 0x160;
struct Voxels {
    fake::Region grid{4096}, wrapper{4096}, world{0x2000}, store{4096}, resources{4096}, handles{80 * 80 * 4};
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

std::atomic<uintptr_t> g_world{0};
std::atomic<uint64_t> g_session{55}, g_step{1};
bool server_fact(void*, uintptr_t* world, uint64_t* session) {
    *world = g_world.load();
    *session = g_session.load();
    return true;
}
uint64_t server_step(void*) { return g_step.load(); }

std::function<void()> g_between;
void hold_point(HoldPoint p) {
    if (p == HoldPoint::BetweenCopies && g_between) {
        auto f = std::move(g_between);
        g_between = nullptr;
        f();
    }
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
double now_us() {
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) * 1e6 / static_cast<double>(f.QuadPart);
}

void exact(const kiln::engine::Profile& p) {
    g_case = p.id;
    kiln::engine::gate_reset_for_tests();
    g_layout = &p.layout;
    fake::Engine e(p.layout);
    g_image = e.image.at();
    Voxels v(e);
    KilnEngineNativeApi native{};
    native.header = {sizeof native, KILN_ENGINE_NATIVE_VERSION};
    native.layout_size = sizeof(kiln::engine::Layout);
    native.profile_size = sizeof(kiln::engine::Profile);
    native.layout = &layout_member;
    native.copy = &copy_member;
    native.ecs_view = &view_member;
    native.ecs_gate_acquire = &acquire_member;
    native.ecs_gate_validate = &validate_member;
    native.ecs_gate_release = &release_member;
    kb::tasks::Source src;
    src.engine = &native;
    src.server = &server_fact;
    src.server_step = &server_step;
    kb::tasks::Offsets off;
    off.grid_iface = kGridIface, off.wrapper_world = kWrapperWorld, off.world_init = kInit, off.world_origin = kOrigin;
    off.world_levels = kLevels, off.level = kLevel, off.level_stride = kStride, off.world_store = kStore;
    off.store_unavailable = kStoreUnavailable, off.world_resources = kResources, off.read = v.read, off.ready = v.ready;
    off.cell = 0.5;
    kb::tasks::Grid grid;
    grid.configure(src, off, nullptr);
    g_world = e.exec.at();
    g_session = 55;
    grid.publish({55, e.exec.at(), v.grid.at(), v.wrapper.at(), v.world.at()});
    const KilnStr voxel{"voxel", 5};
    const auto words_free = [&] { return *fake::word(e.scene_word()) == 0 && *fake::word(e.registry_word()) == 0; };
    const auto next_step = [] { g_step.fetch_add(1); };

    KilnBuildGridSpec s = blank();
    check(grid.grid_for_session(kOwner, 55, voxel, &s) == KILN_OK && s.maximum[0] == 20480 && s.cell_size[1] == 0.5 &&
              s.origin[1] == -4.0 && words_free(),
          "the grid over the real SEH copy and the real gate");
    // Spoiled pages under the hold's copies.
    const struct {
        const char* what;
        uintptr_t at;
    } places[] = {{"the grid's interface", v.grid.at(kGridIface)}, {"the wrapper", v.wrapper.at(kWrapperWorld)},
                  {"the VoxelWorld's first page", v.world.at(kInit)}, {"the VoxelWorld's store field", v.world.at(kStore)},
                  {"the store", v.store.at(kStoreUnavailable)}};
    for (const auto& place : places)
        for (int how = 0; how < 3; ++how) {
            next_step();
            s = blank();
            KilnResult rc;
            {
                Spoil spoil(place.at, 1, how);
                rc = grid.grid_for_session(kOwner, 55, voxel, &s);
            }
            check(rc == KILN_ECS_ERR_UNREADABLE && untouched(s) && words_free(),
                  std::string(place.what) + " " + mode_name(how) + ": unreadable, the record untouched, the words free (" +
                      std::to_string(rc) + ")");
            s = blank();
            check(grid.grid_for_session(kOwner, 55, voxel, &s) == KILN_OK && s.maximum[2] == 20480,
                  std::string(place.what) + " " + mode_name(how) + " put back: ok");
        }
    // A page spoiled between the hold's two copies.
    for (int how = 0; how < 3; ++how) {
        next_step();
        std::unique_ptr<Spoil> spoil;
        g_between = [&] { spoil = std::make_unique<Spoil>(v.world.at(kOrigin), 1, how); };
        s = blank();
        const KilnResult rc = grid.grid_for_session(kOwner, 55, voxel, &s);
        spoil.reset();
        check(rc == KILN_ECS_ERR_UNREADABLE && untouched(s) && words_free(),
              std::string("the VoxelWorld spoiled between the two copies, ") + mode_name(how) + ": unreadable");
    }
    // A frame holding the Scene word.
    next_step();
    *fake::word(e.scene_word()) = 1;
    s = blank();
    check(grid.grid_for_session(kOwner, 55, voxel, &s) == KILN_ECS_ERR_BUSY && untouched(s), "a frame holds the Scene word: busy");
    *fake::word(e.scene_word()) = 0;
    next_step();
    check(grid.grid_for_session(kOwner, 55, voxel, &s) == KILN_OK, "the next step: ok");
    // Racing callers while a thread flips the VoxelWorld's first page.
    std::atomic<bool> stop{false};
    std::atomic<long> ok{0}, busy{0}, unreadable{0}, other{0};
    std::thread flipper([&] {
        void* page = reinterpret_cast<void*>(v.world.at());
        while (!stop.load()) {
            DWORD old = 0;
            VirtualProtect(page, 4096, PAGE_NOACCESS, &old);
            SwitchToThread();
            VirtualProtect(page, 4096, PAGE_READWRITE, &old);
            SwitchToThread();
        }
    });
    std::thread stepper([&] {  // steps much faster than a server's, so the busy latch clears between collisions
        while (!stop.load()) {
            next_step();
            SwitchToThread();
        }
    });
    std::vector<std::thread> callers;
    for (int t = 0; t < 3; ++t)
        callers.emplace_back([&] {
            for (int i = 0; i < 2000; ++i) {
                KilnBuildGridSpec r = blank();
                switch (grid.grid_for_session(kOwner, 55, voxel, &r)) {
                    case KILN_OK: (r.maximum[0] == 20480 ? ok : other)++; break;
                    case KILN_ECS_ERR_BUSY: ++busy; break;
                    case KILN_ECS_ERR_UNREADABLE: ++unreadable; break;
                    default: ++other;
                }
            }
        });
    for (auto& c : callers) c.join();
    stop = true;
    flipper.join();
    stepper.join();
    {
        DWORD old = 0;
        VirtualProtect(reinterpret_cast<void*>(v.world.at()), 4096, PAGE_READWRITE, &old);
    }
    check(other.load() == 0 && ok.load() > 0 && words_free(),
          "a flipping page under three callers: " + std::to_string(ok.load()) + " ok, " + std::to_string(busy.load()) +
              " busy, " + std::to_string(unreadable.load()) + " unreadable, nothing else, the words free");
    // The call's time with real memory (between frames, one caller).
    double total = 0, worst = 0;
    int n = 0;
    for (int i = 0; i < 1000; ++i) {
        next_step();
        s = blank();
        const double t0 = now_us();
        const KilnResult rc = grid.grid_for_session(kOwner, 55, voxel, &s);
        const double dt = now_us() - t0;
        if (rc != KILN_OK) continue;
        total += dt;
        worst = dt > worst ? dt : worst;
        ++n;
    }
    check(n == 1000, "1000 calls between frames: ok");
    std::printf("[%s] grid_for_session: %d calls, %.1f us on average, %.1f us at most (the gate's whole hold included)\n",
                g_case.c_str(), n, n ? total / n : 0.0, worst);
}

}  // namespace

namespace kiln::engine {
Stats& stats() {
    static Stats s;
    return s;
}
}  // namespace kiln::engine
namespace kiln::engine::mem {
std::uintptr_t image_base() { return g_image.load(); }
}  // namespace kiln::engine::mem

int main() {
    kb::tasks::g_hold_test_point = &hold_point;
    for (const auto& p : kiln::engine::generated::kProfiles) exact(p);
    std::printf("voxel_tasks_fault: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
