// SPDX-License-Identifier: MIT
// Scoped edits inside vanilla terraforming, never Build.Dig/the cell writer. Mining yield, replication and building
// history remain the game's own operation. Engine evidence: docs/re/terrain-tool-carve.md.
#ifdef _WIN32
#include <windows.h>
#endif
#include "terrain_carve.h"
#include "../include/kiln_tools_policy.h"
#include "kiln_hook.hpp"

namespace {
using namespace kiln::tools::carve;
kiln::RawHook kernel_hook, buildings_hook;
using Kernel = bool (*)(void*, void*, void*, const int32_t*, const uint32_t*);
using Buildings = uint8_t (*)(void*, void*, uint32_t);

bool copy_memory(void* dst, const void* src, size_t n) {
#ifdef _WIN32
    __try { std::memcpy(dst, src, n); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
#else
    std::memcpy(dst, src, n);
    return true;
#endif
}

bool filtered_kernel(void* brush, void* cells, void* unknown, const int32_t* origin, const uint32_t* dims) {
    const auto* policy = kiln_tools_active_policy();
    if (!policy || policy->mode != 1) return kernel_hook.original<Kernel>()(brush, cells, unknown, origin, dims);
    // Never repair a bad active policy by running an unrestricted kernel.
    if (policy->size < offsetof(KilnToolPolicyView, remove_cells) + sizeof policy->remove_cells) return false;
    BrushCopy owned;
    std::array<float, 256> coefficients;
    if (!filter_brush(brush, policy->remove_cells, owned, coefficients, &copy_memory)) return false;
    return kernel_hook.original<Kernel>()(owned.bytes.data(), cells, unknown, origin, dims);
}

uint8_t filtered_buildings(void* operation, void* brush, uint32_t item) {
    const auto* policy = kiln_tools_active_policy();
    // Returning success is essential: the outer game operation must finish its terrain edit/yield/history.
    // Calling this continuation, even with zero terrain coefficients, would still enter building removal.
    if (policy && policy->mode == 1) return 1;
    return buildings_hook.original<Buildings>()(operation, brush, item);
}

constexpr uint8_t kernel_prologue[] = {
    0x48,0x8b,0xc4,0x48,0x89,0x58,0x18,0x4c,0x89,0x48,0x20,0x55,0x56,0x57,0x41,0x54,
    0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x81,0xec,0x00,0x09,0x00,0x00};
constexpr uint8_t building_prologue[] = {
    0x48,0x8b,0xc4,0x48,0x89,0x58,0x08,0x48,0x89,0x70,0x10,0x48,0x89,0x78,0x18,0x4c,
    0x89,0x60,0x20,0x55,0x41,0x56,0x41,0x57,0x48,0x8d,0xa8,0x18,0xf8,0xff,0xff};

template<size_t N> bool prologue(uintptr_t address, const uint8_t (&expected)[N]) {
    uint8_t actual[N];
    return address && copy_memory(actual, reinterpret_cast<const void*>(address), N) &&
           std::memcmp(actual, expected, N) == 0;
}
bool exact_build(const KilnApi* api) {
    KilnBuildInfo build{};
    build.size = sizeof build;
    if (!api || !api->build_info || api->build_info(&build) != KILN_OK || !build.verified || !build.hooks_enabled)
        return false;
    // Layout qualification is exact-build, not merely a coincidentally matching function prologue.
    constexpr char client[] = "5d9ef2f140b8ed49e30dbee655841b89d08baec20f799a1817e171a18a850c77";
    constexpr char server[] = "f0d824be7fcd48ebfe2458ff5c28da26060c2074e3ec1fcac587e5a1d508aeb2";
    return (build.side == KILN_SIDE_CLIENT && std::memcmp(build.exe_sha256, client, sizeof client) == 0) ||
           (build.side == KILN_SIDE_SERVER && std::memcmp(build.exe_sha256, server, sizeof server) == 0);
}
} // namespace

void kiln_tools_carve_install(const KilnApi* api, KilnOwner owner) {
    kiln_tools_report_hook(KILN_TOOL_HOOK_KERNEL_FILTER, 0);
    kiln_tools_report_hook(KILN_TOOL_HOOK_BUILDING_SUPPRESS, 0);
    uintptr_t kernel = 0, buildings = 0;
    const KilnStr kernel_name{"tools/TerrainCarveKernel", sizeof("tools/TerrainCarveKernel") - 1};
    const KilnStr buildings_name{"tools/RemoveBuildingVoxels", sizeof("tools/RemoveBuildingVoxels") - 1};
    if (!exact_build(api) || !api->symbol || api->symbol(kernel_name, &kernel) != KILN_OK ||
        api->symbol(buildings_name, &buildings) != KILN_OK || kernel == buildings ||
        !prologue(kernel, kernel_prologue) || !prologue(buildings, building_prologue)) return;
    const bool installed =
        kernel_hook.install(api, owner, kernel, reinterpret_cast<void*>(&filtered_kernel), "tools/TerrainCarveKernel") &&
        buildings_hook.install(api, owner, buildings, reinterpret_cast<void*>(&filtered_buildings), "tools/RemoveBuildingVoxels");
    if (!installed) { kernel_hook.revoke(); buildings_hook.revoke(); return; }
    kiln_tools_report_hook(KILN_TOOL_HOOK_KERNEL_FILTER, 1);
    kiln_tools_report_hook(KILN_TOOL_HOOK_BUILDING_SUPPRESS, 1);
}
