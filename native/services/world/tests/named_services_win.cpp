// SPDX-License-Identifier: MIT
// Windows test (native on a Windows runner, or headless Wine; never shipped, never built by default): kiln.world's named
// services (named_services.cpp, named_service_hold.cpp) over the real Windows primitives of kiln.engine (memory_win.cpp:
// the SEH copy and write and the Interlocked words; resolve.cpp: the views; gate.cpp: the write gate), the real
// memory-range check (region_win.cpp, VirtualQuery) and a fake engine in VirtualAlloc'd memory laid out from the
// dedicated server's exact profile, with the game's named-service registry in it (named_fake_registry.h).
// Covered: the range check on real pages (read-write, read-only, guarded, no access, decommitted, the exe's own code,
// spanning two regions); open, snapshot and compare-exchange end to end; the service's page made PAGE_NOACCESS inside the
// hold before the read (the SEH copy refuses, nothing written, the process lives), read-only before the write (the SEH write
// faults part-way: rolled back and proven), PAGE_NOACCESS between the write and its readback (uncertain, latched); a
// decommitted record page (the binding does not resolve); a frame holding the Scene word (busy).
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "include/kiln_engine_native.h"
#include "memory.h"
#include "profiles.g.h"
#include "resolve.h"
#include "engine_state.h"
#include "gate.h"

#include "../named_services.h"
#include "named_fake_registry.h"

namespace kw {
bool region_writable(std::uintptr_t address, std::size_t bytes);
}

namespace {
using namespace kw::named;
using fake::get;
using fake::put;
std::atomic<std::uintptr_t> g_image{0};
const kiln::engine::Layout* g_layout = nullptr;
int checks = 0, failures = 0;
void check(bool ok, const std::string& what) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL %s\n", what.c_str());
    }
}

// kiln.internal.engine's members, the same bodies engine.cpp publishes, over the real primitives (as ecs_fault_win.cpp).
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
std::int32_t KILN_CALL component_member(const EcsGate* gate, std::uint16_t c) {
    return kiln::engine::gate_component_supported(*gate, c) ? 1 : 0;
}
KilnResult KILN_CALL write_member(const EcsGate* gate, std::uintptr_t address, const void* data, std::size_t bytes) {
    switch (kiln::engine::gate_write(*gate, address, data, bytes)) {
        case kiln::engine::GateWrite::Ok: return KILN_OK;
        case kiln::engine::GateWrite::TooLarge: return KILN_ERR_BUFFER_TOO_SMALL;
        case kiln::engine::GateWrite::NotHeld: return KILN_ERR_INVALID_ARGUMENT;
        default: return KILN_ERR_UNAVAILABLE;
    }
}
void KILN_CALL release_member(EcsGate* gate) { kiln::engine::gate_release(*gate); }

struct Rig {
    uint64_t session = 77, step = 1;
};
bool rig_server(void* u, uint64_t* s) {
    *s = static_cast<Rig*>(u)->session;
    return true;
}
uint64_t rig_step(void* u) { return static_cast<Rig*>(u)->step; }
bool real_writable(void*, std::uintptr_t a, std::size_t n) { return kw::region_writable(a, n); }

HoldPoint g_at = HoldPoint::Begin;
std::function<void()> g_action;
void hold_point(HoldPoint p) {
    if (g_action && p == g_at) {
        auto f = std::move(g_action);
        g_action = nullptr;
        f();
    }
}
DWORD protect(std::uintptr_t at, std::size_t n, DWORD how) {
    DWORD old = 0;
    VirtualProtect(reinterpret_cast<void*>(at & ~std::uintptr_t{4095}), (n + 4095) & ~std::size_t{4095}, how, &old);
    return old;
}

void range_cases() {
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    const std::size_t page = si.dwPageSize;
    auto* rw = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, page * 4, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    const auto a = reinterpret_cast<std::uintptr_t>(rw);
    check(kw::region_writable(a, page * 4) && kw::region_writable(a + 8, 144), "committed private read-write pages");
    DWORD old = 0;
    VirtualProtect(rw + page, page, PAGE_READONLY, &old);
    check(!kw::region_writable(a + page, 16) && !kw::region_writable(a + page - 8, 16) && kw::region_writable(a, page),
          "a read-only page, also when the range only reaches into it");
    VirtualProtect(rw + page, page, PAGE_READWRITE | PAGE_GUARD, &old);
    check(!kw::region_writable(a + page, 16), "a guard page");
    VirtualProtect(rw + page, page, PAGE_NOACCESS, &old);
    check(!kw::region_writable(a + page, 16), "a no-access page");
    VirtualProtect(rw + page, page, PAGE_READWRITE, &old);
    check(kw::region_writable(a, page * 4), "read-write again: one range over regions of the same kind");
    VirtualFree(rw + page * 2, page, MEM_DECOMMIT);
    check(!kw::region_writable(a + page * 2, 16) && !kw::region_writable(a, page * 4), "a decommitted page");
    VirtualFree(rw, 0, MEM_RELEASE);
    const auto code = reinterpret_cast<std::uintptr_t>(&range_cases);
    check(!kw::region_writable(code, 16), "the exe's own code (an image page)");
    check(!kw::region_writable(0, 16) && !kw::region_writable(UINTPTR_MAX - 4, 16), "null and an overflowing range");
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
    range_cases();
    const kiln::engine::Profile* server = nullptr;
    for (const auto& p : kiln::engine::generated::kProfiles)
        if (p.layout.dedicated_server) server = &p;
    if (!server) {
        std::printf("FAIL no dedicated-server profile\n");
        return 1;
    }
    g_layout = &server->layout;
    fake::Engine e(server->layout);
    g_image = e.image.at();
    namedfake::Registry reg(server->layout, e);
    const kiln::engine::ServiceContract& C = server->layout.services.contracts[0];
    KilnEngineNativeApi native{};
    native.header = {sizeof native, KILN_ENGINE_NATIVE_VERSION};
    native.layout_size = sizeof(kiln::engine::Layout);
    native.profile_size = sizeof(kiln::engine::Profile);
    native.layout = &layout_member;
    native.copy = &copy_member;
    native.ecs_view = &view_member;
    native.ecs_gate_acquire = &acquire_member;
    native.ecs_gate_validate = &validate_member;
    native.ecs_gate_component_supported = &component_member;
    native.ecs_gate_write = &write_member;
    native.ecs_gate_release = &release_member;
    Rig rig;
    Source s;
    s.engine = &native;
    s.user = &rig;
    s.server = &rig_server;
    s.server_step = &rig_step;
    s.writable = &real_writable;
    Services services(s);
    check(services.available(), "named services over the real primitives");
    g_hold_test_point = &hold_point;
    constexpr KilnOwner kOwner = 5;
    KilnHandle h = 0;
    check(services.open(kOwner, KilnStr{C.key.data(), C.key_size}, 0, &h, nullptr) == KILN_OK && h, "open");
    for (size_t i = 0; i < C.size; ++i) put<uint8_t>(reg.service() + i, static_cast<uint8_t>(i + 3));
    std::vector<uint8_t> out(C.size), now(C.size);
    std::memcpy(now.data(), reinterpret_cast<const void*>(reg.service()), C.size);
    check(services.snapshot(kOwner, h, out.data(), C.size, nullptr) == KILN_OK && out == now, "snapshot");
    auto cx = [&](const std::vector<uint8_t>& expected, const std::vector<uint8_t>& desired, KilnMutationReply& r) {
        r = KilnMutationReply{};
        r.size = sizeof r;
        return services.compare_exchange(kOwner, h, expected.data(), nullptr, desired.data(), C.size, nullptr, &r);
    };
    std::vector<uint8_t> next = now;
    next[0] ^= 0xff;
    KilnMutationReply r{};
    const KilnResult first = cx(now, next, r);
    check(first == KILN_OK && r.flags == KILN_MUTATION_WRITTEN &&  // no confirmed buffer: CONFIRMED is not claimed
              get<uint8_t>(reg.service()) == next[0],
          "compare-exchange written and read back (" + std::to_string(first) + " status " + std::to_string(r.status) +
              " why " + std::to_string(r.why_code) + " flags " + std::to_string(r.flags) + " reason " +
              std::to_string(r.engine_reason) + ")");
    now = next;
    // the service's page turns PAGE_NOACCESS after the range check: the SEH copy refuses, nothing is written
    rig.step++;
    g_at = HoldPoint::AfterResolve;
    DWORD old = 0;
    g_action = [&] { old = protect(reg.service(), C.size, PAGE_NOACCESS); };
    next[1] ^= 0xff;
    const KilnResult faulted = cx(now, next, r);
    protect(reg.service(), C.size, old);
    check(faulted == KILN_ECS_ERR_UNREADABLE && r.why_code == KILN_ECS_WRITE_WHY_FAULT &&
              std::memcmp(reinterpret_cast<const void*>(reg.service()), now.data(), C.size) == 0,
          "a fault on the first read: unreadable, nothing written, the process lives");
    g_at = HoldPoint::AfterResolve;
    g_action = [&] { old = protect(reg.service(), C.size, PAGE_NOACCESS); };
    const KilnResult snap = services.snapshot(kOwner, h, out.data(), C.size, nullptr);
    protect(reg.service(), C.size, old);
    check(snap == KILN_ECS_ERR_UNREADABLE, "a fault under a snapshot: unreadable");
    // read-only before the write: the SEH write faults, the rollback is proven
    g_at = HoldPoint::AfterCompare;
    g_action = [&] { old = protect(reg.service(), C.size, PAGE_READONLY); };
    const KilnResult ro = cx(now, next, r);
    protect(reg.service(), C.size, old);
    check(ro == KILN_ECS_ERR_UNREADABLE && r.flags == KILN_MUTATION_ROLLED_BACK && r.why_code == KILN_ECS_WRITE_WHY_FAULT &&
              std::memcmp(reinterpret_cast<const void*>(reg.service()), now.data(), C.size) == 0,
          "a write that faults on a read-only page: rolled back and proven");
    // no access between the write and its readback: uncertain, latched for the session
    g_at = HoldPoint::AfterWrite;
    g_action = [&] { old = protect(reg.service(), C.size, PAGE_NOACCESS); };
    const KilnResult lost = cx(now, next, r);
    protect(reg.service(), C.size, old);
    check(lost == KILN_ECS_ERR_UNCERTAIN && (r.flags & KILN_MUTATION_TARGET_LATCHED), "a write that cannot be read back: uncertain");
    std::memcpy(now.data(), reinterpret_cast<const void*>(reg.service()), C.size);
    check(cx(now, now, r) == KILN_ECS_ERR_UNCERTAIN && r.why_code == KILN_ECS_WRITE_WHY_TARGET_LATCHED, "then latched");
    rig.session = 78;
    KilnHandle h2 = 0;
    check(services.open(kOwner, KilnStr{C.key.data(), C.key_size}, 0, &h2, nullptr) == KILN_OK, "a new session opens");
    h = h2;
    // a decommitted record page: the binding does not resolve, nothing crashes
    VirtualFree(reinterpret_cast<void*>(reg.rows.at()), 4096, MEM_DECOMMIT);
    const KilnResult gone = services.snapshot(kOwner, h, out.data(), C.size, nullptr);
    VirtualAlloc(reinterpret_cast<void*>(reg.rows.at()), 4096, MEM_COMMIT, PAGE_READWRITE);
    reg.wire();
    check(gone == KILN_ECS_ERR_UNREADABLE && services.snapshot(kOwner, h, out.data(), C.size, nullptr) == KILN_OK,
          "a decommitted record page: unreadable; recommitted: reads");
    // a frame holds the Scene word
    rig.step++;
    *fake::word(e.scene_word()) = 1;
    std::memcpy(now.data(), reinterpret_cast<const void*>(reg.service()), C.size);
    const KilnResult busy = cx(now, now, r);
    *fake::word(e.scene_word()) = 0;
    check(busy == KILN_ECS_ERR_BUSY && r.engine_reason == static_cast<uint32_t>(kiln::engine::Reason::EcsSceneFrameBusy),
          "a frame holds the Scene word: busy");
    std::printf("named_services_win: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
