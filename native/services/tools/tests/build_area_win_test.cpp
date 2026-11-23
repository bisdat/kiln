// SPDX-License-Identifier: MIT
// Native Windows proof of the actual guard's guarded copies and layout/capability refusal; no game process.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "kiln_api.h"
#include "../hooks/build_area.h"
#include "../include/kiln_tools_policy.h"
#include <array>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <utility>

namespace {
int total = 0, failed = 0;
void check(bool value, const char* name) {
    ++total;
    if (!value) { ++failed; std::fprintf(stderr, "FAIL %s\n", name); }
}
bool feature_on = true;
const char* missing = nullptr;
const char* wrong = nullptr;
constexpr std::array<std::pair<const char*, int64_t>, 8> layout{{
    {"tools/area.access_zones", 0x88}, {"tools/area.bases", 0x10}, {"tools/area.count", 0x18},
    {"tools/area.shape", 0x44}, {"tools/area.owner_kind", 0x45}, {"tools/area.abandoned", 0x46},
    {"build/zone.stride", 0x48}, {"build/zone.entity", 0x40}
}};
bool KILN_CALL feature(KilnStr name) { return feature_on && std::string_view(name.ptr, name.len) == "tools.build_area"; }
KilnResult KILN_CALL constant(KilnStr name, int64_t* out) {
    const std::string_view key(name.ptr, name.len);
    if (missing && key == missing) return KILN_ERR_NOT_FOUND;
    for (const auto& [n, value] : layout) {
        if (key == n) { *out = value + ((wrong && key == wrong) ? 1 : 0); return KILN_OK; }
    }
    return KILN_ERR_NOT_FOUND;
}
}
int main() {
    using kiln::tools::area::Base;
    std::array<uint8_t, 0x30> zones{};
    Base base{};
    for (int a = 0; a < 3; ++a) { base.min[a] = -INT64_C(42949672960); base.max[a] = INT64_C(42949672960); }
    base.altar = 42;
    uintptr_t data = reinterpret_cast<uintptr_t>(&base);
    uint64_t count = 1;
    std::memcpy(zones.data() + 0x10, &data, sizeof data);
    std::memcpy(zones.data() + 0x18, &count, sizeof count);
    const auto current = reinterpret_cast<uintptr_t>(zones.data());
    const double center[] = {0, 0, 0}, outside[] = {9.5, 0, 0};
    check(!kiln_tools_check_build_area(current, 1, center, 1), "before init refuses");
    check(!kiln_tools_build_area_init(nullptr), "missing core refuses");
    check(!kiln_tools_hook_installed(KILN_TOOL_HOOK_EDGE_CHECK), "missing core cannot report installed");
    KilnApi api{}; api.feature = &feature; api.constant = &constant;
    feature_on = false;
    check(!kiln_tools_build_area_init(&api), "missing mapping feature refuses");
    feature_on = true;
    for (size_t i = 0; i < layout.size(); ++i) {
        missing = layout[i].first;
        check(!kiln_tools_build_area_init(&api), "missing layout capture refuses");
        check(!kiln_tools_check_build_area(current, 1, center, 1), "missing layout cannot allow");
        missing = nullptr; wrong = layout[i].first;
        check(!kiln_tools_build_area_init(&api), "changed layout refuses");
        check(!kiln_tools_hook_installed(KILN_TOOL_HOOK_EDGE_CHECK), "changed layout withdraws installed hook");
        wrong = nullptr;
    }
    check(kiln_tools_build_area_init(&api), "qualified layout ready");
    check(kiln_tools_hook_installed(KILN_TOOL_HOOK_EDGE_CHECK), "qualified guard reported");
    check(kiln_tools_check_build_area(current, 1, center, 1), "current contained sphere admitted");
    check(!kiln_tools_check_build_area(current, 1, outside, 1), "edge-crossing sphere refuses");
    check(!kiln_tools_check_build_area(current, 0, center, 1), "missing actor refuses");
    check(!kiln_tools_check_build_area(current, 1, nullptr, 1), "missing center refuses");
    base.abandoned = 1;
    check(!kiln_tools_check_build_area(current, 1, center, 1), "new abandonment immediately refuses");
    base.abandoned = 0; base.max[0] = 0;
    check(!kiln_tools_check_build_area(current, 1, center, 1), "changed current bounds immediately refuse");
    void* unreadable = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_NOACCESS);
    check(unreadable != nullptr, "unreadable memory fixture allocated");
    if (unreadable) {
        check(!kiln_tools_check_build_area(reinterpret_cast<uintptr_t>(unreadable), 1, center, 1), "unreadable zones refuse without exception escape");
        data = reinterpret_cast<uintptr_t>(unreadable);
        std::memcpy(zones.data() + 0x10, &data, sizeof data);
        check(!kiln_tools_check_build_area(current, 1, center, 1), "unreadable base record refuses");
        check(!kiln_tools_check_build_area(current, 1, static_cast<const double*>(unreadable), 1), "unreadable center refuses");
        VirtualFree(unreadable, 0, MEM_RELEASE);
    }
    std::printf("building area Windows: %d passed, %d failed\n", total - failed, failed);
    return failed ? 1 : 0;
}
