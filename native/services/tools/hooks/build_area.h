// SPDX-License-Identifier: MIT
#pragma once
#include <cstddef>
#include <cstdint>

// Private tools-service guard, not an independent permission API. The game's original impact must still run its
// own actor, cursor, range and forbidden-area checks. The caller must hold the engine's program Access while reading
// BuildZoneSystem and must call this immediately before switching the impact, never cache a successful answer.
// Admission rechecks the containing record and list header and refuses observed changes. These reads are not a lock
// or a proven atomic snapshot; BZS reader/writer scheduling and changes after validation remain unqualified.
namespace kiln::tools::area {
struct Sphere {
    double center[3];
    double radius;
};
// BuildZoneSystem's base-list record (docs/re/build.md section 7). Only qualified box geometry is accepted.
struct Base {
    int64_t min[3];
    int64_t max[3];
    uint8_t id[16];
    uint32_t altar;
    uint8_t shape;
    uint8_t owner_kind;
    uint8_t abandoned;
    uint8_t reserved;
};
static_assert(sizeof(Base) == 0x48 && offsetof(Base, altar) == 0x40 && offsetof(Base, abandoned) == 0x46);
using Reader = bool (*)(void* destination, uintptr_t source, size_t bytes);
bool contains(const Base& base, const Sphere& sphere) noexcept;
bool check_current(uintptr_t build_zones, uint32_t actor, const Sphere& sphere, Reader read) noexcept;
}

// Resolve the exact layout and the impact Access slot before publishing EDGE_CHECK as installed. These are private
// same-DLL calls; no game pointers are exposed through the mod API. init runs once before any impact is hooked.
struct KilnApi;
bool kiln_tools_build_area_init(const KilnApi* api);
bool kiln_tools_check_build_area(uintptr_t build_zones, uint32_t actor, const double center[3], double radius);
