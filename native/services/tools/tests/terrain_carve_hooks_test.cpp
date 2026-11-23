// SPDX-License-Identifier: MIT
// Real production detours and installation checks over a fake kiln.hooks registry; no target/game execution.
#include "../hooks/terrain_carve.cpp"
#include "services/kiln_caps.h"
#include <algorithm>
#include <atomic>
#include <barrier>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

namespace {
std::atomic<int> checks{0};
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); std::exit(1); } } while (0)
thread_local const KilnToolPolicyView* active = nullptr;
int reported_kernel = -1, reported_buildings = -1;
int installs = 0, revoked = 0;
std::atomic<int> building_calls{0}, kernel_calls{0};
std::barrier<>* concurrent_kernel = nullptr;
bool refuse_kernel = false, refuse_dispatch = false, refuse_buildings = false;
KilnOwner live_child = 0, next_child = 10;
int owner_revoke_calls = 0, hooks_revoke_owner_calls = 0;
uint32_t cap_state = KILN_CAP_UNAVAILABLE, cap_reason = KILN_CAP_REASON_SYMBOL_MISSING;
std::string scenario;
std::array<uint8_t, 64> kernel_entry{}, dispatch_entry{}, building_entry{}, kernel_file{}, dispatch_file{}, building_file{};
bool kernel_patched = false, dispatch_patched = false, building_patched = false;
std::vector<KilnHandle> revocations;
Kernel installed_kernel = nullptr;
Buildings installed_buildings = nullptr;
using DispatchFixture = bool (*)(void*, void*, void*, const int32_t*, const uint32_t*, const uint8_t*, void*);
bool vanilla_dispatch(void*, void*, void*, const int32_t*, const uint32_t*, const uint8_t*, void*);
DispatchFixture installed_dispatch = &vanilla_dispatch;
const uint8_t* expected_flags = nullptr;
bool expect_original_flags = false;
thread_local void* expected_brush = nullptr;
thread_local bool expect_original_brush = false;
#ifdef _WIN32
thread_local bool fault_in_kernel = false;
#endif
uint32_t props = 0xabcdef01;
struct Cell { uint8_t material, fill; };
struct Cells { std::vector<Cell> cells; };
// The game kernel's removal report: the descriptor's pointer at +0x80, the float total at +0x18 (both builds,
// @tools/TerrainCarveKernel.report). Kiln reads them only as the sigdb captures tools/carve.report(_total).
constexpr size_t game_report = 0x80, game_report_total = 0x18;

bool vanilla_kernel(void* brush, void* data, void* unknown, const int32_t* origin, const uint32_t* dims) {
    ++kernel_calls;
    CHECK(expect_original_brush ? brush == expected_brush : brush != expected_brush);
    CHECK(unknown == &props && origin[0] == 7 && dims[0] == 256);
    const float* coefficients;
    uint64_t count;
    std::memcpy(&coefficients, static_cast<uint8_t*>(brush) + coefficient_offset, sizeof coefficients);
    std::memcpy(&count, static_cast<uint8_t*>(brush) + coefficient_count_offset, sizeof count);
    if (concurrent_kernel) concurrent_kernel->arrive_and_wait();
#ifdef _WIN32
    if (fault_in_kernel) RaiseException(0xe0420001, 0, 0, nullptr);
#endif
    // Fake kernel: coefficient zero must preserve *every* fill value, including material-bearing air.
    // Its deliberately distinct partial boundary behaviour must be passed through, not reconstructed by Kiln.
    bool carved = false;
    for (auto& cell : static_cast<Cells*>(data)->cells)
        if (cell.material > 0 && cell.material < 128 && cell.material < count && coefficients[cell.material] > 0) {
            carved = carved || cell.fill;
            cell.fill = cell.fill > 31 ? uint8_t(cell.fill - 31) : 0;
        }
    // Like the game's kernel, add what it carved to the report the descriptor names (+0x80, total at +0x18).
    uintptr_t report = 0;
    std::memcpy(&report, static_cast<uint8_t*>(brush) + game_report, sizeof report);
    if (carved && report) *reinterpret_cast<float*>(report + game_report_total) += 1.5f;
    return false; // The wrapper must preserve the game's return, not turn it into unconditional success.
}
bool vanilla_dispatch(void*, void*, void* data, const int32_t* origin, const uint32_t* dims,
                      const uint8_t* flags, void* brush) {
    CHECK(expect_original_flags ? flags == expected_flags : flags != expected_flags);
    CHECK(flags[0] == 7); // Native snapshot/history behavior is not a cleanup flag.
    installed_kernel(brush, data, &props, origin, dims);
    // Fixture for the *separate* post-kernel passes in client A88945..A88ADE / server A331E5..A3337E.
    // Material 14 is an isolated ordinary-terrain cell; material-bearing air exercises the other cleanup flag.
    for (auto& cell : static_cast<Cells*>(data)->cells) {
        if ((flags[1] && cell.material == 14) || (flags[2] && !cell.material)) cell = {0,0};
    }
    return true;
}
uint8_t vanilla_buildings(void*, void*, uint32_t item) { ++building_calls; CHECK(item == 42); return 0; }
KilnResult KILN_CALL build_info(KilnBuildInfo* out) {
    out->verified = scenario != "unverified";
    out->hooks_enabled = scenario != "safe-idle";
    out->side = scenario == "server" ? KILN_SIDE_SERVER : KILN_SIDE_CLIENT;
    const char* hash = out->side == KILN_SIDE_SERVER ?
        "f0d824be7fcd48ebfe2458ff5c28da26060c2074e3ec1fcac587e5a1d508aeb2" :
        "5d9ef2f140b8ed49e30dbee655841b89d08baec20f799a1817e171a18a850c77";
    std::memcpy(out->exe_sha256, hash, 65);
    if (scenario == "unknown-build") out->exe_sha256[0] = '0';
    return KILN_OK;
}
KilnResult KILN_CALL symbol(KilnStr name, uintptr_t* out) {
    const std::string n(name.ptr, name.len);
    if ((n == "tools/TerrainCarveKernel" && scenario == "missing-kernel") ||
        (n == "tools/TerrainCarveDispatch" && scenario == "missing-dispatch") ||
        (n == "tools/RemoveBuildingVoxels" && scenario == "missing-buildings")) return KILN_ERR_NOT_FOUND;
    *out = reinterpret_cast<uintptr_t>(n == "tools/TerrainCarveKernel" ? kernel_entry.data() :
                                       n == "tools/TerrainCarveDispatch" ? dispatch_entry.data() : building_entry.data());
#ifdef _WIN32
    if (scenario == "unreadable" && n == "tools/TerrainCarveKernel") *out = 1;
#endif
    return KILN_OK;
}
// The report layout as sigdb captures it; missing (or naming the coefficient table) in the report-layout scenarios.
KilnResult KILN_CALL constant(KilnStr name, int64_t* out) {
    const std::string n(name.ptr, name.len);
    if (n != "tools/carve.report" && n != "tools/carve.report_total") return KILN_ERR_NOT_FOUND;
    if (scenario == "no-report-layout") return KILN_ERR_NOT_FOUND;
    *out = n == "tools/carve.report" ? (scenario == "bad-report-layout" ? int64_t(coefficient_offset) : int64_t(game_report))
                                     : int64_t(game_report_total);
    return KILN_OK;
}
bool report_layout_scenario() { return scenario == "no-report-layout" || scenario == "bad-report-layout"; }
KilnResult KILN_CALL raw_detour(KilnOwner owner, uintptr_t target, const KilnHookOrder*, void* handler,
                               void** continuation, KilnHandle* handle) {
    CHECK(owner != 9 && owner == live_child);
    ++installs;
    if (target == reinterpret_cast<uintptr_t>(kernel_entry.data())) {
        if (refuse_kernel) return KILN_ERR_UNAVAILABLE;
        *continuation = reinterpret_cast<void*>(&vanilla_kernel);
        installed_kernel = reinterpret_cast<Kernel>(handler);
        // kiln.hooks retains its physical detour even when the last link is revoked.
        kernel_entry[0] = 0xe9;
        kernel_patched = true;
    } else if (target == reinterpret_cast<uintptr_t>(dispatch_entry.data())) {
        if (refuse_dispatch) return KILN_ERR_UNAVAILABLE;
        *continuation = reinterpret_cast<void*>(&vanilla_dispatch);
        installed_dispatch = reinterpret_cast<DispatchFixture>(handler);
        dispatch_entry[0] = 0xe9;
        dispatch_patched = true;
    } else {
        CHECK(target == reinterpret_cast<uintptr_t>(building_entry.data()));
        if (refuse_buildings) return KILN_ERR_UNAVAILABLE;
        *continuation = reinterpret_cast<void*>(&vanilla_buildings);
        installed_buildings = reinterpret_cast<Buildings>(handler);
        building_entry[0] = 0xe9;
        building_patched = true;
    }
    *handle = installs;
    return KILN_OK;
}
KilnResult KILN_CALL revoke(KilnHandle handle) {
    // Admission must close before either link disappears; the physical entry deliberately remains patched.
    CHECK(reported_kernel == 0 && reported_buildings == 0);
    revocations.push_back(handle);
    ++revoked;
    return KILN_OK;
}
KilnResult KILN_CALL read_original(uintptr_t address, void* out, size_t size) {
    if (scenario == "original-error") return KILN_ERR_UNAVAILABLE;
    if (size > kernel_file.size()) return KILN_ERR_INVALID_ARGUMENT;
    if (address == reinterpret_cast<uintptr_t>(kernel_entry.data())) std::memcpy(out, kernel_file.data(), size);
    else if (address == reinterpret_cast<uintptr_t>(dispatch_entry.data())) std::memcpy(out, dispatch_file.data(), size);
    else if (address == reinterpret_cast<uintptr_t>(building_entry.data())) std::memcpy(out, building_file.data(), size);
    else return KILN_ERR_INVALID_ARGUMENT;
    return KILN_OK;
}
KilnResult KILN_CALL code_state(uintptr_t address, size_t size, uint32_t* state, uint64_t* generation) {
    if (scenario == "state-error") return KILN_ERR_UNAVAILABLE;
    *generation = 1;
    if (scenario == "unknown-code") { *state = KILN_CODE_UNKNOWN; return KILN_OK; }
    const bool kernel = address == reinterpret_cast<uintptr_t>(kernel_entry.data());
    const bool dispatch = address == reinterpret_cast<uintptr_t>(dispatch_entry.data());
    const auto& live = kernel ? kernel_entry : dispatch ? dispatch_entry : building_entry;
    const auto& file = kernel ? kernel_file : dispatch ? dispatch_file : building_file;
    const bool patched = kernel ? kernel_patched : dispatch ? dispatch_patched : building_patched;
    *state = KILN_CODE_ORIGINAL;
    for (size_t i = 0; i < size; ++i) {
        if (live[i] == file[i]) continue;
        if (i == 0 && patched && live[i] == 0xe9) *state = KILN_CODE_MANAGED;
        else { *state = KILN_CODE_FOREIGN; break; }
    }
    return KILN_OK;
}
KilnOwner KILN_CALL owner_create(KilnStr name) {
    CHECK(std::string(name.ptr, name.len) == "kiln.tools.carve" && !live_child);
    if (scenario == "owner-failure") return 0;
    if (scenario == "parent-owner") return 9;
    return live_child = next_child++;
}
KilnResult KILN_CALL revoke_owner(KilnOwner owner) {
    CHECK(owner != 9 && owner == live_child);
    CHECK(reported_kernel == 0 && reported_buildings == 0 && cap_state == KILN_CAP_UNAVAILABLE);
    ++hooks_revoke_owner_calls;
    return KILN_OK;
}
void KILN_CALL owner_revoke(KilnOwner owner) {
    CHECK(owner != 9 && owner == live_child);
    CHECK(hooks_revoke_owner_calls == owner_revoke_calls + 1);
    ++owner_revoke_calls;
    live_child = 0;
}
KilnResult KILN_CALL cap_report(KilnOwner owner, KilnStr name, uint32_t state, uint32_t reason, KilnStr) {
    CHECK(owner == 9 && std::string(name.ptr, name.len) == "build.filtered_terrain");
    CHECK((reason == KILN_CAP_REASON_NONE) == (state == KILN_CAP_AVAILABLE));
    if (state == KILN_CAP_AVAILABLE) CHECK(reported_kernel == 1 && reported_buildings == 1 && live_child);
    cap_state = state; cap_reason = reason;
    return KILN_OK;
}
KilnHooksApi hooks{};
KilnCapsApi caps{};
const void* KILN_CALL service(KilnStr name, uint32_t version) {
    const std::string n(name.ptr, name.len);
    if (n == "kiln.caps") { CHECK(version == 1); return &caps; }
    CHECK(n == "kiln.hooks" && version == 2);
    return scenario == "no-hooks" ? nullptr : &hooks;
}
void set_cell(KilnToolPolicyView& policy, unsigned cell) { policy.remove_cells[cell / 8] |= uint8_t(1u << (cell % 8)); }
#ifdef _WIN32
bool catch_kernel_fault(void* brush, void* cells, const int32_t* origin, const uint32_t* dims) {
    __try { installed_kernel(brush, cells, &props, origin, dims); }
    __except (GetExceptionCode() == 0xe0420001 ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return true;
    }
    return false;
}
#endif
}
const KilnToolPolicyView* kiln_tools_active_policy() { return active; }
void kiln_tools_report_hook(uint32_t hook, int installed) {
    if (hook == KILN_TOOL_HOOK_KERNEL_FILTER) reported_kernel = installed;
    else if (hook == KILN_TOOL_HOOK_BUILDING_SUPPRESS) reported_buildings = installed;
    else CHECK(false);
}
int main(int argc, char** argv) {
    scenario = argc > 1 ? argv[1] : "client";
    std::copy(std::begin(kernel_prologue), std::end(kernel_prologue), kernel_entry.begin());
    std::copy(std::begin(building_prologue), std::end(building_prologue), building_entry.begin());
    std::copy(std::begin(dispatch_prologue), std::end(dispatch_prologue), dispatch_entry.begin());
    if (scenario == "bad-dispatch") dispatch_entry[20] ^= 1;
    if (scenario == "bad-kernel") kernel_entry[12] ^= 1;
    if (scenario == "bad-buildings") building_entry[20] ^= 1;
    kernel_file = kernel_entry;
    dispatch_file = dispatch_entry;
    building_file = building_entry;
    if (scenario == "foreign-kernel") kernel_entry[0] = 0xe9;
    if (scenario == "foreign-dispatch") dispatch_entry[0] = 0xe9;
    if (scenario == "foreign-buildings") building_entry[0] = 0xe9;
    if (scenario == "managed-code") {
        kernel_entry[0] = dispatch_entry[0] = building_entry[0] = 0xe9;
        kernel_patched = dispatch_patched = building_patched = true;
    }
    refuse_kernel = scenario == "refuse-kernel";
    refuse_dispatch = scenario == "refuse-dispatch";
    refuse_buildings = scenario == "refuse-buildings" || scenario == "retry-buildings";
    hooks.header = {sizeof hooks, 2}; hooks.raw_detour = &raw_detour; hooks.revoke = &revoke;
    hooks.revoke_owner = &revoke_owner;
    caps.header = {sizeof caps, 1}; caps.report = &cap_report;
    hooks.read_original = &read_original; hooks.code_state = &code_state;
    if (scenario == "short-hooks") hooks.header.size = offsetof(KilnHooksApi, code_state);
    if (scenario == "no-original") hooks.read_original = nullptr;
    if (scenario == "no-code-state") hooks.code_state = nullptr;
    KilnApi api{}; api.size = sizeof api; api.build_info = &build_info; api.symbol = &symbol; api.service = &service;
    api.constant = &constant;
    api.owner_create = &owner_create; api.owner_revoke = &owner_revoke;
    if (scenario == "no-owner-create") api.owner_create = nullptr;
    if (scenario == "no-owner-revoke") api.owner_revoke = nullptr;
    if (scenario == "no-revoke-owner") hooks.revoke_owner = nullptr;
    kiln_tools_carve_install(&api, 9);
    if (scenario == "retry-buildings") {
        CHECK(reported_kernel == 0 && reported_buildings == 0 && revoked == 2);
        CHECK(kernel_patched && kernel_entry[0] == 0xe9);
        kiln_tools_carve_uninstall(); // Idempotent even after a partial installation rolled back.
        CHECK(revoked == 2 && !kernel_hook && !dispatch_hook && !buildings_hook);
        refuse_buildings = false;
        kiln_tools_carve_install(&api, 9);
        CHECK(reported_kernel == 1 && reported_buildings == 1 && installs == 6);
        kiln_tools_carve_uninstall();
        CHECK(reported_kernel == 0 && reported_buildings == 0 && revoked == 5);
        CHECK(!live_child && owner_revoke_calls == 2 && hooks_revoke_owner_calls == 2);
        CHECK(cap_state == KILN_CAP_UNAVAILABLE);
        std::printf("terrain hooks %s: %d checks passed\n", scenario.c_str(), checks.load());
        return 0;
    }
    if (scenario != "client" && scenario != "server" && scenario != "managed-code" && !report_layout_scenario()) {
        CHECK(reported_kernel == 0 && reported_buildings == 0);
        CHECK(installs == (refuse_kernel ? 1 : refuse_dispatch ? 2 : refuse_buildings ? 3 : 0));
        CHECK(revoked == (refuse_buildings ? 2 : refuse_dispatch ? 1 : 0));
        CHECK(!live_child && cap_state == KILN_CAP_UNAVAILABLE);
        CHECK(owner_revoke_calls == (refuse_kernel || refuse_dispatch || refuse_buildings ? 1 : 0));
        if (refuse_kernel || refuse_dispatch || refuse_buildings || scenario == "owner-failure" || scenario == "parent-owner")
            CHECK(cap_reason == KILN_CAP_REASON_FAULTED);
        kiln_tools_carve_uninstall();
        CHECK(!kernel_hook && !dispatch_hook && !buildings_hook);
        CHECK(revoked == (refuse_buildings ? 2 : refuse_dispatch ? 1 : 0));
        std::printf("terrain hooks %s: %d checks passed\n", scenario.c_str(), checks.load());
        return 0;
    }
    CHECK(reported_kernel == 1 && reported_buildings == 1 && installs == 3);
    // Every byte in the original guarded prefix is load-bearing, even with a managed live detour.
    for (size_t i = 0; i < sizeof kernel_prologue; ++i) {
        kernel_file[i] ^= 1;
        CHECK(!prologue(&hooks, reinterpret_cast<uintptr_t>(kernel_entry.data()), kernel_prologue));
        kernel_file[i] ^= 1;
    }
    for (size_t i = 0; i < sizeof building_prologue; ++i) {
        building_file[i] ^= 1;
        CHECK(!prologue(&hooks, reinterpret_cast<uintptr_t>(building_entry.data()), building_prologue));
        building_file[i] ^= 1;
    }
    for (size_t i = 0; i < sizeof dispatch_prologue; ++i) {
        dispatch_file[i] ^= 1;
        CHECK(!prologue(&hooks, reinterpret_cast<uintptr_t>(dispatch_entry.data()), dispatch_prologue));
        dispatch_file[i] ^= 1;
    }
    // A subsequent refusal removes old links too, not merely their readiness flags.
    const auto good_build = scenario;
    scenario = "unknown-build";
    kiln_tools_carve_install(&api, 9);
    CHECK(reported_kernel == 0 && reported_buildings == 0 && !kernel_hook && !dispatch_hook && !buildings_hook);
    CHECK(revocations == std::vector<KilnHandle>({3, 2, 1}));
    scenario = good_build;
    kiln_tools_carve_install(&api, 9);
    CHECK(reported_kernel == 1 && reported_buildings == 1 && installs == 6);
    kiln_tools_carve_uninstall();
    CHECK(reported_kernel == 0 && reported_buildings == 0 && !kernel_hook && !dispatch_hook && !buildings_hook);
    CHECK(revocations == std::vector<KilnHandle>({3, 2, 1, 6, 5, 4}));
    kiln_tools_carve_uninstall();
    CHECK(revoked == 6);
    CHECK(kernel_hook.original<Kernel>() == &vanilla_kernel);
    CHECK(buildings_hook.original<Buildings>() == &vanilla_buildings);
    kiln_tools_carve_install(&api, 9);
    CHECK(reported_kernel == 1 && reported_buildings == 1 && installs == 9);
    KilnToolPolicyView policy{}; policy.size = sizeof policy; policy.mode = 1;
    constexpr unsigned materials[] = {8,66,69,74,81,87,102,107,64,23,36,43,44,61,62,92,14,33,65,72,48};
    for (auto cell : materials) set_cell(policy, cell);
    std::array<float, 256> source; source.fill(1);
    BrushCopy brush{};
    const float* ptr = source.data(); const uint64_t count = 256;
    std::memcpy(brush.bytes.data() + coefficient_offset, &ptr, sizeof ptr);
    std::memcpy(brush.bytes.data() + coefficient_count_offset, &count, sizeof count);
    expected_brush = brush.bytes.data();
    int32_t origin[3]{7,8,9}; uint32_t dims[3]{256,1,1};
    // All 256 material values at all 256 fill levels; includes unrelated terrain and partial boundaries.
    for (float radius : {1.0f,2.0f,4.0f}) {
        std::memcpy(brush.bytes.data(), &radius, sizeof radius);
        auto saved = brush.bytes;
        Cells cells;
        for (unsigned mat = 0; mat < 256; ++mat) for (unsigned fill = 0; fill < 256; ++fill)
            cells.cells.push_back({uint8_t(mat),uint8_t(fill)});
        active = &policy;
        CHECK(!installed_kernel(brush.bytes.data(), &cells, &props, origin, dims));
        CHECK(installed_buildings(nullptr, nullptr, 42) == 1);
        CHECK(building_calls == 0 && props == 0xabcdef01);
        CHECK(brush.bytes == saved && std::all_of(source.begin(), source.end(), [](float f) {return f == 1;}));
        for (unsigned mat = 0; mat < 256; ++mat) for (unsigned fill = 0; fill < 256; ++fill) {
            const bool allowed = std::find(std::begin(materials), std::end(materials), mat) != std::end(materials);
            const auto& cell = cells.cells[mat * 256 + fill];
            CHECK(cell.material == mat && cell.fill == (allowed ? (fill > 31 ? fill - 31 : 0) : fill));
        }
    }
    // No policy, and Add, must be the original call with the original pointer and return value.
    expect_original_brush = true;
    Cells cells;
    for (const auto* p : {static_cast<const KilnToolPolicyView*>(nullptr), static_cast<const KilnToolPolicyView*>(&policy)}) {
        policy.mode = 2; active = p;
        cells.cells = {{8,255}};
        CHECK(!installed_kernel(brush.bytes.data(), &cells, &props, origin, dims));
        CHECK(cells.cells[0].fill == 224);
        CHECK(installed_buildings(nullptr, nullptr, 42) == 0);
    }
    CHECK(building_calls == 2);
    // A malformed active Remove view refuses; never an unrestricted fallback.
    policy.mode = 1; policy.size = 1; active = &policy;
    const int before = kernel_calls;
    CHECK(!installed_kernel(brush.bytes.data(), &cells, &props, origin, dims));
    CHECK(kernel_calls == before && installed_buildings(nullptr, nullptr, 42) == 1);
    policy.size = sizeof policy;
    expect_original_brush = false;
    for (unsigned optional : {37u,73u,91u}) {
        cells.cells = {{uint8_t(optional),255}};
        CHECK(!installed_kernel(brush.bytes.data(), &cells, &props, origin, dims));
        CHECK(cells.cells[0].fill == 255);
        set_cell(policy, optional);
        CHECK(!installed_kernel(brush.bytes.data(), &cells, &props, origin, dims));
        CHECK(cells.cells[0].fill == 224);
        policy.remove_cells[optional / 8] &= uint8_t(~(1u << (optional % 8)));
    }
    // Both owned tables remain live concurrently, with different policies; an unrelated thread sees no policy.
    std::barrier gate(3);
    concurrent_kernel = &gate;
    std::vector<std::thread> workers;
    for (unsigned selected : {8u, 14u, 0u}) workers.emplace_back([&, selected] {
        CHECK(active == nullptr);
        KilnToolPolicyView local{}; local.size = sizeof local; local.mode = 1;
        set_cell(local, selected);
        std::array<float, 256> table; table.fill(1);
        BrushCopy descriptor = brush;
        const float* data = table.data();
        std::memcpy(descriptor.bytes.data() + coefficient_offset, &data, sizeof data);
        const auto saved = descriptor.bytes;
        expected_brush = descriptor.bytes.data();
        expect_original_brush = selected == 0;
        active = selected ? &local : nullptr;
        Cells own{{{8,255}, {14,255}}};
        CHECK(!installed_kernel(descriptor.bytes.data(), &own, &props, origin, dims));
        CHECK(own.cells[0].fill == (selected == 8 || !selected ? 224 : 255));
        CHECK(own.cells[1].fill == (selected == 14 || !selected ? 224 : 255));
        CHECK(descriptor.bytes == saved && table[8] == 1 && table[14] == 1);
        active = nullptr;
    });
    for (auto& worker : workers) worker.join();
    concurrent_kernel = nullptr;
    CHECK(active == &policy);
    // The stroke's trace (kiln#133): inside a stroke the filtered kernel notes whether an allowed cell is minable and
    // whether the game's own report grew; outside one it notes nothing. The report stays the game's to write. Without
    // the report layout from sigdb (or with one naming the coefficient table) every stroke stays unmeasured.
    {
        const bool layout = !report_layout_scenario();
        struct Report { float* data; uint64_t capacity; uint8_t material, pad[7]; float total, tail; } report{nullptr, 256, 0, {}, 0, 0};
        static_assert(offsetof(Report, total) == game_report_total);
        BrushCopy reporting = brush;
        const auto* named = &report;
        std::memcpy(reporting.bytes.data() + game_report, &named, sizeof named);
        expected_brush = reporting.bytes.data();
        Cells rubble{{{8,255}}}, ground{{{9,255}}};
        CHECK(!installed_kernel(reporting.bytes.data(), &rubble, &props, origin, dims)); // no stroke: nothing noted
        CHECK(report.total == 1.5f && kiln::tools::stroke_tally == nullptr);
        kiln::tools::StrokeTally removed, unmatched, unmeasured;
        {kiln::tools::StrokeScope stroke(&removed);rubble.cells = {{8,255}};
         CHECK(!installed_kernel(reporting.bytes.data(), &rubble, &props, origin, dims));}
        CHECK(removed.kernels == 1 && removed.removable && removed.removed == layout && removed.measured == layout &&
              report.total == 3.0f && rubble.cells[0].fill == 224);
        {kiln::tools::StrokeScope stroke(&unmatched);
         CHECK(!installed_kernel(reporting.bytes.data(), &ground, &props, origin, dims));}
        CHECK(unmatched.kernels == 1 && unmatched.removable && !unmatched.removed && unmatched.measured == layout &&
              ground.cells[0].fill == 255);
        expected_brush = brush.bytes.data();
        {kiln::tools::StrokeScope stroke(&unmeasured);rubble.cells = {{8,255}};
         CHECK(!installed_kernel(brush.bytes.data(), &rubble, &props, origin, dims));}
        CHECK(unmeasured.kernels == 1 && unmeasured.removable && !unmeasured.measured && !unmeasured.removed);
        CHECK(kiln::tools::stroke_tally == nullptr);
    }
#ifdef _WIN32
    // The production SEH copy refuses bad descriptors/tables without reaching any continuation.
    const int calls_before_fault = kernel_calls;
    CHECK(!installed_kernel(reinterpret_cast<void*>(1), &cells, &props, origin, dims));
    const float* unreadable_table = reinterpret_cast<const float*>(1);
    std::memcpy(brush.bytes.data() + coefficient_offset, &unreadable_table, sizeof unreadable_table);
    CHECK(!installed_kernel(brush.bytes.data(), &cells, &props, origin, dims));
    CHECK(kernel_calls == calls_before_fault);
    std::memcpy(brush.bytes.data() + coefficient_offset, &ptr, sizeof ptr);
    // A continuation fault escapes to the caller: this hook owns no TLS and must not swallow the fault or clear
    // its caller's scope. It has not written the borrowed descriptor/table. Impact-scope unwinding is separate.
    const auto before_fault = brush.bytes;
    fault_in_kernel = true;
    CHECK(catch_kernel_fault(brush.bytes.data(), &cells, origin, dims));
    fault_in_kernel = false;
    CHECK(active == &policy && brush.bytes == before_fault && source[8] == 1);
#endif
    // The native dispatcher's cleanup happens AFTER the filtered kernel. Coefficients alone cannot protect it.
    std::fill(std::begin(policy.remove_cells), std::end(policy.remove_cells), uint8_t(0));
    set_cell(policy, 8);
    const uint8_t cleanup_flags[3]{7,1,1};
    expected_flags = cleanup_flags;
    Cells cleanup{{{8,255}, {14,255}, {0,90}}};
    CHECK(installed_dispatch(nullptr, nullptr, &cleanup, origin, dims, cleanup_flags, brush.bytes.data()));
    CHECK(cleanup.cells[0].fill == 224 && cleanup.cells[1].material == 14 && cleanup.cells[1].fill == 255);
    CHECK(cleanup.cells[2].material == 0 && cleanup.cells[2].fill == 90);
    CHECK(cleanup_flags[1] == 1 && cleanup_flags[2] == 1);
#ifdef _WIN32
    const int before_bad_flags = kernel_calls;
    CHECK(!installed_dispatch(nullptr, nullptr, &cleanup, origin, dims, reinterpret_cast<const uint8_t*>(1), brush.bytes.data()));
    CHECK(kernel_calls == before_bad_flags);
#endif
    expect_original_flags = expect_original_brush = true;
    for (const auto* p : {static_cast<const KilnToolPolicyView*>(nullptr), static_cast<const KilnToolPolicyView*>(&policy)}) {
        policy.mode = KILN_TOOL_POLICY_ADD; active = p;
        cleanup.cells = {{8,255}, {14,255}, {0,90}};
        CHECK(installed_dispatch(nullptr, nullptr, &cleanup, origin, dims, cleanup_flags, brush.bytes.data()));
        CHECK(cleanup.cells[0].fill == 224 && cleanup.cells[1].material == 0 && cleanup.cells[1].fill == 0);
        CHECK(cleanup.cells[2].material == 0 && cleanup.cells[2].fill == 0);
    }
    active = nullptr;
    kiln_tools_carve_uninstall();
    CHECK(reported_kernel == 0 && reported_buildings == 0 && !kernel_hook && !dispatch_hook && !buildings_hook);
    CHECK(revocations == std::vector<KilnHandle>({3,2,1,6,5,4,9,8,7}));
    CHECK(!live_child && owner_revoke_calls == 3 && hooks_revoke_owner_calls == 3);
    CHECK(cap_state == KILN_CAP_UNAVAILABLE);
    std::printf("terrain hooks %s: %d checks passed\n", scenario.c_str(), checks.load());
}
