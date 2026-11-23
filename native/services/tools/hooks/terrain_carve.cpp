// SPDX-License-Identifier: MIT
// Scoped edits inside vanilla terraforming, never Build.Dig/the cell writer. Mining yield, replication and building
// history remain the game's own operation. Engine evidence: docs/re/terrain-tool-carve.md.
#ifdef _WIN32
#include <windows.h>
#endif
#include "terrain_carve.h"
#include "../include/kiln_tools_policy.h"
#include "../action_trace.h"
#include "kiln_hook.hpp"
#include "services/kiln_caps.h"
#include <atomic>

namespace {
using namespace kiln::tools::carve;
kiln::RawHook kernel_hook, dispatch_hook, buildings_hook;
const KilnApi* carve_api = nullptr;
const KilnHooksApi* carve_hooks = nullptr;
const KilnCapsApi* carve_caps = nullptr;
KilnOwner carve_owner = 0, carve_parent = 0;
// The removal report's layout for the action trace, from sigdb at install (-1: unresolved, strokes stay unmeasured).
std::atomic<int64_t> report_at{-1}, report_total_at{-1};
constexpr KilnStr carve_capability{"build.filtered_terrain", sizeof("build.filtered_terrain") - 1};

void report_capability(const KilnCapsApi* caps, KilnOwner owner, uint32_t state, uint32_t reason, const char* text) {
    if (caps) caps->report(owner, carve_capability, state, reason, {text, std::strlen(text)});
}
using Kernel = bool (*)(void*, void*, void*, const int32_t*, const uint32_t*);
using Buildings = uint8_t (*)(void*, void*, uint32_t);
using Dispatch = bool (*)(void*, void*, void*, const int32_t*, const uint32_t*, const uint8_t*, void*);

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
    bool removable = false;
    if (!filter_brush(brush, policy->remove_cells, owned, coefficients, &copy_memory, &removable)) {
        kiln::tools::note_brush_fault();
        return false;
    }
    // The stroke's trace (action_trace.h): whether an allowed cell is minable, and whether the kernel's own removal
    // report grew. Two guarded 4-byte reads; the report and the cells stay the game's.
    const ReportLayout layout{report_at.load(std::memory_order_relaxed), report_total_at.load(std::memory_order_relaxed)};
    float before = 0, after = 0;
    const bool measured = report_total(owned, layout, &copy_memory, before);
    const bool result = kernel_hook.original<Kernel>()(owned.bytes.data(), cells, unknown, origin, dims);
    kiln::tools::note_kernel(removable, measured && report_total(owned, layout, &copy_memory, after) ? (after > before ? 1 : 0) : -1);
    return result;
}

bool filtered_dispatch(void* terrain, void* box, void* buffers, const int32_t* begin, const uint32_t* end,
                       const uint8_t* flags, void* brush) {
    const auto* policy = kiln_tools_active_policy();
    if (!policy || policy->mode != KILN_TOOL_POLICY_REMOVE)
        return dispatch_hook.original<Dispatch>()(terrain, box, buffers, begin, end, flags, brush);
    uint8_t owned_flags[3];
    if (!flags || !copy_memory(owned_flags, flags, sizeof owned_flags)) return false;
    // These post-kernel passes remove isolated terrain / material-bearing air without consulting coefficients.
    // Keep snapshot/history flag 0, but disable both unfiltered cleanup passes for this invocation only.
    owned_flags[1] = owned_flags[2] = 0;
    return dispatch_hook.original<Dispatch>()(terrain, box, buffers, begin, end, owned_flags, brush);
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
constexpr uint8_t dispatch_prologue[] = {
    0x4c,0x89,0x4c,0x24,0x20,0x48,0x89,0x4c,0x24,0x08,0x55,0x53,0x56,0x57,0x41,0x54,
    0x41,0x55,0x41,0x56,0x48,0x8d,0xac,0x24,0xe0,0xfd,0xff,0xff,0x48,0x81,0xec,0x20,0x03,0x00,0x00};
constexpr uint8_t building_prologue[] = {
    0x48,0x8b,0xc4,0x48,0x89,0x58,0x08,0x48,0x89,0x70,0x10,0x48,0x89,0x78,0x18,0x4c,
    0x89,0x60,0x20,0x55,0x41,0x56,0x41,0x57,0x48,0x8d,0xa8,0x18,0xf8,0xff,0xff};

template<size_t N> bool prologue(const KilnHooksApi* hooks, uintptr_t address, const uint8_t (&expected)[N]) {
    uint8_t actual[N];
    uint32_t state = KILN_CODE_UNKNOWN;
    uint64_t generation = 0;
    // Revoking a link leaves the registry's physical detour in place. Check the shipped code, not the patched
    // entry, and accept live changes only when the registry can account for every changed byte.
    return address && hooks->read_original(address, actual, N) == KILN_OK &&
           std::memcmp(actual, expected, N) == 0 &&
           hooks->code_state(address, N, &state, &generation) == KILN_OK &&
           (state == KILN_CODE_ORIGINAL || state == KILN_CODE_MANAGED);
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

void kiln_tools_carve_uninstall() {
    // The service closes admission and drains active invocations before changing these links.
    kiln_tools_report_hook(KILN_TOOL_HOOK_KERNEL_FILTER, 0);
    kiln_tools_report_hook(KILN_TOOL_HOOK_BUILDING_SUPPRESS, 0);
    report_capability(carve_caps, carve_parent, KILN_CAP_UNAVAILABLE, KILN_CAP_REASON_POLICY,
                      "terrain removal hooks are not installed");
    buildings_hook.revoke();
    dispatch_hook.revoke();
    kernel_hook.revoke();
    if (carve_owner) {
        carve_hooks->revoke_owner(carve_owner);
        carve_api->owner_revoke(carve_owner);
    }
    carve_owner = carve_parent = 0;
    report_at.store(-1, std::memory_order_relaxed);
    report_total_at.store(-1, std::memory_order_relaxed);
    carve_api = nullptr;
    carve_hooks = nullptr;
    carve_caps = nullptr;
}

void kiln_tools_carve_install(const KilnApi* api, KilnOwner owner) {
    kiln_tools_carve_uninstall();
    uintptr_t kernel = 0, dispatch = 0, buildings = 0;
    const KilnStr dispatch_name{"tools/TerrainCarveDispatch", sizeof("tools/TerrainCarveDispatch") - 1};
    const KilnStr kernel_name{"tools/TerrainCarveKernel", sizeof("tools/TerrainCarveKernel") - 1};
    const KilnStr buildings_name{"tools/RemoveBuildingVoxels", sizeof("tools/RemoveBuildingVoxels") - 1};
    if (!api || api->size < sizeof(KilnApi) || !api->service) return;
    carve_parent = owner;
    const auto* caps = static_cast<const KilnCapsApi*>(api->service({KILN_CAPS_SERVICE, sizeof(KILN_CAPS_SERVICE) - 1},
                                                                 KILN_CAPS_VERSION));
    if (caps && caps->header.version >= KILN_CAPS_VERSION &&
        caps->header.size >= offsetof(KilnCapsApi, report) + sizeof(caps->report) && caps->report) carve_caps = caps;
    report_capability(carve_caps, owner, KILN_CAP_UNAVAILABLE, KILN_CAP_REASON_POLICY,
                      "terrain removal requires a qualified executable and enabled game hooks");
    if (!exact_build(api)) return;
    const auto* hooks = kiln::hooks_v2(api);
    if (!hooks || hooks->header.size < offsetof(KilnHooksApi, code_state) + sizeof(hooks->code_state) ||
        !hooks->read_original || !hooks->code_state || !hooks->raw_detour || !hooks->revoke || !hooks->revoke_owner ||
        !api->owner_create || !api->owner_revoke) {
        report_capability(carve_caps, owner, KILN_CAP_UNAVAILABLE, hooks ? KILN_CAP_REASON_SERVICE_OLDER :
                          KILN_CAP_REASON_SERVICE_MISSING, "hook provenance and child-owner lifecycle APIs are required");
        return;
    }
    report_capability(carve_caps, owner, KILN_CAP_UNAVAILABLE, KILN_CAP_REASON_SYMBOL_MISSING,
                      "terrain removal symbols or original entry bytes are not qualified");
    if (!api->symbol || api->symbol(kernel_name, &kernel) != KILN_OK ||
        api->symbol(dispatch_name, &dispatch) != KILN_OK || api->symbol(buildings_name, &buildings) != KILN_OK ||
        kernel == buildings || kernel == dispatch || dispatch == buildings ||
        !prologue(hooks, kernel, kernel_prologue) || !prologue(hooks, dispatch, dispatch_prologue) ||
        !prologue(hooks, buildings, building_prologue)) return;
    // Optional: the trace's removal report layout. Unresolved or unqualified, strokes are traced as unmeasured.
    ReportLayout layout;
    if (!api->constant || api->constant({"tools/carve.report", sizeof("tools/carve.report") - 1}, &layout.report) != KILN_OK ||
        api->constant({"tools/carve.report_total", sizeof("tools/carve.report_total") - 1}, &layout.total) != KILN_OK ||
        !report_layout_ok(layout)) layout = {};
    report_at.store(layout.report, std::memory_order_relaxed);
    report_total_at.store(layout.total, std::memory_order_relaxed);
    carve_owner = api->owner_create({"kiln.tools.carve", sizeof("kiln.tools.carve") - 1});
    if (!carve_owner || carve_owner == owner) {
        carve_owner = 0; // Never revoke the parent, even if an invalid owner factory returns it.
        report_capability(carve_caps, owner, KILN_CAP_UNAVAILABLE, KILN_CAP_REASON_FAULTED,
                          "could not create the terrain removal hook owner");
        return;
    }
    carve_api = api;
    carve_hooks = hooks;
    const bool installed =
        kernel_hook.install(api, carve_owner, kernel, reinterpret_cast<void*>(&filtered_kernel), "tools/TerrainCarveKernel") &&
        dispatch_hook.install(api, carve_owner, dispatch, reinterpret_cast<void*>(&filtered_dispatch), "tools/TerrainCarveDispatch") &&
        buildings_hook.install(api, carve_owner, buildings, reinterpret_cast<void*>(&filtered_buildings), "tools/RemoveBuildingVoxels");
    if (!installed) {
        const auto* reporter = carve_caps;
        kiln_tools_carve_uninstall();
        report_capability(reporter, owner, KILN_CAP_UNAVAILABLE, KILN_CAP_REASON_FAULTED,
                          "could not install all terrain removal hooks");
        return;
    }
    kiln_tools_report_hook(KILN_TOOL_HOOK_KERNEL_FILTER, 1);
    kiln_tools_report_hook(KILN_TOOL_HOOK_BUILDING_SUPPRESS, 1);
    report_capability(carve_caps, owner, KILN_CAP_AVAILABLE, KILN_CAP_REASON_NONE, "");
}
