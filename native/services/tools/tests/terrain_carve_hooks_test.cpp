// SPDX-License-Identifier: MIT
// Real production detours and installation checks over a fake kiln.hooks registry; no target/game execution.
#include "../hooks/terrain_carve.cpp"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

namespace {
int checks = 0;
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); std::exit(1); } } while (0)
thread_local const KilnToolPolicyView* active = nullptr;
int reported_kernel = -1, reported_buildings = -1;
int installs = 0, revoked = 0, building_calls = 0, kernel_calls = 0;
bool refuse_kernel = false, refuse_buildings = false;
std::string scenario;
std::array<uint8_t, 64> kernel_entry{}, building_entry{};
Kernel installed_kernel = nullptr;
Buildings installed_buildings = nullptr;
void* expected_brush = nullptr;
bool expect_original_brush = false;
uint32_t props = 0xabcdef01;
struct Cell { uint8_t material, fill; };
struct Cells { std::vector<Cell> cells; };

bool vanilla_kernel(void* brush, void* data, void* unknown, const int32_t* origin, const uint32_t* dims) {
    ++kernel_calls;
    CHECK(expect_original_brush ? brush == expected_brush : brush != expected_brush);
    CHECK(unknown == &props && origin[0] == 7 && dims[0] == 256);
    const float* coefficients;
    uint64_t count;
    std::memcpy(&coefficients, static_cast<uint8_t*>(brush) + coefficient_offset, sizeof coefficients);
    std::memcpy(&count, static_cast<uint8_t*>(brush) + coefficient_count_offset, sizeof count);
    // Fake kernel: coefficient zero must preserve *every* fill value, including material-bearing air.
    // Its deliberately distinct partial boundary behaviour must be passed through, not reconstructed by Kiln.
    for (auto& cell : static_cast<Cells*>(data)->cells)
        if (cell.material > 0 && cell.material < 128 && cell.material < count && coefficients[cell.material] > 0)
            cell.fill = cell.fill > 31 ? uint8_t(cell.fill - 31) : 0;
    return false; // The wrapper must preserve the game's return, not turn it into unconditional success.
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
        (n == "tools/RemoveBuildingVoxels" && scenario == "missing-buildings")) return KILN_ERR_NOT_FOUND;
    *out = reinterpret_cast<uintptr_t>(n == "tools/TerrainCarveKernel" ? kernel_entry.data() : building_entry.data());
#ifdef _WIN32
    if (scenario == "unreadable" && n == "tools/TerrainCarveKernel") *out = 1;
#endif
    return KILN_OK;
}
KilnResult KILN_CALL raw_detour(KilnOwner owner, uintptr_t target, const KilnHookOrder*, void* handler,
                               void** continuation, KilnHandle* handle) {
    CHECK(owner == 9);
    ++installs;
    if (target == reinterpret_cast<uintptr_t>(kernel_entry.data())) {
        if (refuse_kernel) return KILN_ERR_UNAVAILABLE;
        *continuation = reinterpret_cast<void*>(&vanilla_kernel);
        installed_kernel = reinterpret_cast<Kernel>(handler);
    } else {
        CHECK(target == reinterpret_cast<uintptr_t>(building_entry.data()));
        if (refuse_buildings) return KILN_ERR_UNAVAILABLE;
        *continuation = reinterpret_cast<void*>(&vanilla_buildings);
        installed_buildings = reinterpret_cast<Buildings>(handler);
    }
    *handle = installs;
    return KILN_OK;
}
KilnResult KILN_CALL revoke(KilnHandle) { ++revoked; return KILN_OK; }
KilnHooksApi hooks{};
const void* KILN_CALL service(KilnStr name, uint32_t version) {
    CHECK(std::string(name.ptr, name.len) == "kiln.hooks" && version == 2);
    return scenario == "no-hooks" ? nullptr : &hooks;
}
void set_cell(KilnToolPolicyView& policy, unsigned cell) { policy.remove_cells[cell / 8] |= uint8_t(1u << (cell % 8)); }
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
    if (scenario == "bad-kernel") kernel_entry[12] ^= 1;
    if (scenario == "bad-buildings") building_entry[20] ^= 1;
    refuse_kernel = scenario == "refuse-kernel";
    refuse_buildings = scenario == "refuse-buildings";
    hooks.header = {sizeof hooks, 2}; hooks.raw_detour = &raw_detour; hooks.revoke = &revoke;
    KilnApi api{}; api.size = sizeof api; api.build_info = &build_info; api.symbol = &symbol; api.service = &service;
    kiln_tools_carve_install(&api, 9);
    if (scenario != "client" && scenario != "server") {
        CHECK(reported_kernel == 0 && reported_buildings == 0);
        CHECK(installs == (refuse_kernel ? 1 : refuse_buildings ? 2 : 0));
        CHECK(revoked == (refuse_buildings ? 1 : 0));
        std::printf("terrain hooks %s: %d checks passed\n", scenario.c_str(), checks);
        return 0;
    }
    CHECK(reported_kernel == 1 && reported_buildings == 1 && installs == 2);
    // Every byte in either guarded prefix is load-bearing, not merely the first opcode.
    for (size_t i = 0; i < sizeof kernel_prologue; ++i) {
        kernel_entry[i] ^= 1;
        CHECK(!prologue(reinterpret_cast<uintptr_t>(kernel_entry.data()), kernel_prologue));
        kernel_entry[i] ^= 1;
    }
    for (size_t i = 0; i < sizeof building_prologue; ++i) {
        building_entry[i] ^= 1;
        CHECK(!prologue(reinterpret_cast<uintptr_t>(building_entry.data()), building_prologue));
        building_entry[i] ^= 1;
    }
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
#ifdef _WIN32
    // The production SEH copy refuses a bad native span without reaching any continuation.
    const int calls_before_fault = kernel_calls;
    CHECK(!installed_kernel(reinterpret_cast<void*>(1), &cells, &props, origin, dims));
    CHECK(kernel_calls == calls_before_fault);
#endif
    active = nullptr;
    std::printf("terrain hooks %s: %d checks passed\n", scenario.c_str(), checks);
}
