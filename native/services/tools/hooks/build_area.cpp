// SPDX-License-Identifier: MIT
#include "build_area.h"
#include <cmath>
#include <cstring>
#include <limits>

namespace kiln::tools::area {
namespace {
bool valid(const Sphere& s) noexcept {
    return s.radius > 0 && std::isfinite(s.radius) && std::isfinite(s.center[0]) &&
           std::isfinite(s.center[1]) && std::isfinite(s.center[2]);
}
// The residual preserves crossings smaller than one ULP: 9 + nextafter(1, 2) rounds to 10, but crosses a bound at 10.
// Do not add an epsilon at the boundary. Tangency is allowed; any crossing is refused.
double sum_error(double a, double b, double sum) noexcept {
    const double z = sum - a;
    return (a - (sum - z)) + (b - z);
}
struct Bound { double rounded, residual; };
Bound exact_bound(int64_t fixed) noexcept {
    constexpr int64_t unit = INT64_C(4294967296);
    // Intentional integer quotient/remainder: both parts are exactly representable before the error-free sum.
    const int64_t integral = fixed / unit;
    const double whole = static_cast<double>(integral);
    const double fraction = std::ldexp(static_cast<double>(fixed % unit), -32);
    const double rounded = whole + fraction;
    return {rounded, sum_error(whole, fraction, rounded)};
}
bool add_address(uintptr_t base, size_t offset, uintptr_t& out) noexcept {
    if (!base || offset > std::numeric_limits<uintptr_t>::max() - base) return false;
    out = base + offset;
    return true;
}
bool read_bounded(Reader read, void* destination, uintptr_t source, size_t size) noexcept {
    return source && size && size - 1 <= std::numeric_limits<uintptr_t>::max() - source &&
           read(destination, source, size);
}
}
bool contains(const Base& b, const Sphere& s) noexcept {
    if (!valid(s) || !b.altar || b.abandoned || b.shape != 0 || b.owner_kind > 7) return false;
    for (int a = 0; a < 3; ++a) {
        if (b.min[a] > b.max[a]) return false;
        const double low = s.center[a] - s.radius, high = s.center[a] + s.radius;
        if (!std::isfinite(low) || !std::isfinite(high)) return false;
        const Bound min = exact_bound(b.min[a]), max = exact_bound(b.max[a]);
        // Rounded sums order the exact values; when equal, compare their exact residuals. Keeping the bound's
        // residual too preserves tangency to fixed-point values that do not fit in one double.
        if (low < min.rounded || high > max.rounded) return false;
        if (low == min.rounded && sum_error(s.center[a], -s.radius, low) < min.residual) return false;
        if (high == max.rounded && sum_error(s.center[a], s.radius, high) > max.residual) return false;
    }
    return true;
}
bool check_current(uintptr_t zones, uint32_t actor, const Sphere& sphere, Reader read) noexcept {
    if (!actor || !valid(sphere) || !read) return false;
    // The exact layout is gated at boot. Copy only the current base list, within the engine's Access; no tick-thread
    // singleton calls or retained pointers. A stale client-side preview can never authorise the impact.
    uintptr_t list = 0;
    if (!add_address(zones, 0x10, list)) return false;
    struct Entries { uintptr_t data; uint64_t count; } entries{};
    if (!read_bounded(read, &entries, list, sizeof entries) || !entries.data ||
        entries.count == 0 || entries.count > 4096) return false;
    for (uint64_t i = 0; i < entries.count; ++i) {
        uintptr_t address = 0;
        if (!add_address(entries.data, static_cast<size_t>(i) * sizeof(Base), address)) return false;
        Base base{};
        if (!read_bounded(read, &base, address, sizeof base)) return false;
        if (contains(base, sphere)) {
            // Refuse an observed rebuild, including in-place changes that leave the list header intact.
            // This is conservative validation, not a replacement for the engine's Access lifetime: no qualified
            // version/lock is available to exclude ABA or writes after our final read.
            Base current{};
            Entries after{};
            return read_bounded(read, &current, address, sizeof current) &&
                   read_bounded(read, &after, list, sizeof after) &&
                   after.data == entries.data && after.count == entries.count &&
                   std::memcmp(&base, &current, sizeof base) == 0;
        }
    }
    return false;
}
}

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "kiln_api.h"
#include "../include/kiln_tools_policy.h"

namespace {
bool area_ready = false; // immutable after init, which must precede hook installation
bool area_copy(void* destination, uintptr_t source, size_t bytes) {
    __try {
        std::memcpy(destination, reinterpret_cast<const void*>(source), bytes);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool constant_is(const KilnApi* api, const char* name, int64_t expected) {
    int64_t value = -1;
    return api->constant(KilnStr{name, std::strlen(name)}, &value) == KILN_OK && value == expected;
}
}
bool kiln_tools_build_area_init(const KilnApi* api) {
    area_ready = api && api->feature && api->constant &&
        api->feature(KilnStr{"tools.build_area", 16}) &&
        constant_is(api, "tools/area.access_zones", 0x88) &&
        constant_is(api, "tools/area.bases", 0x10) &&
        constant_is(api, "tools/area.count", 0x18) &&
        constant_is(api, "tools/area.shape", 0x44) &&
        constant_is(api, "tools/area.owner_kind", 0x45) &&
        constant_is(api, "tools/area.abandoned", 0x46) &&
        constant_is(api, "build/zone.stride", 0x48) &&
        constant_is(api, "build/zone.entity", 0x40);
    kiln_tools_report_hook(KILN_TOOL_HOOK_EDGE_CHECK, area_ready ? 1 : 0);
    return area_ready;
}
bool kiln_tools_check_build_area(uintptr_t zones, uint32_t actor, const double center[3], double radius) {
    if (!area_ready || !center) return false;
    double copied[3]{};
    if (!area_copy(copied, reinterpret_cast<uintptr_t>(center), sizeof copied)) return false;
    return kiln::tools::area::check_current(zones, actor, {{copied[0], copied[1], copied[2]}, radius}, &area_copy);
}
#endif
