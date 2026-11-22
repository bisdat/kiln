// SPDX-License-Identifier: MIT
#include "../hooks/build_area.h"
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <utility>

using namespace kiln::tools::area;
namespace {
int total = 0, failed = 0;
void check(bool value, const char* name) {
    ++total;
    if (!value) { ++failed; std::fprintf(stderr, "FAIL %s\n", name); }
}
Base box(double lx, double ly, double lz, double hx, double hy, double hz) {
    Base b{};
    const double lo[] = {lx, ly, lz}, hi[] = {hx, hy, hz};
    for (int a = 0; a < 3; ++a) {
        b.min[a] = static_cast<int64_t>(lo[a] * 4294967296.0);
        b.max[a] = static_cast<int64_t>(hi[a] * 4294967296.0);
    }
    b.altar = 42;
    return b;
}
bool copy(void* dst, uintptr_t src, size_t size) {
    if (!src) return false;
    std::memcpy(dst, reinterpret_cast<const void*>(src), size);
    return true;
}
std::array<uint8_t, 0x30> zones{};
std::array<Base, 2> bases{};
void publish(size_t count) {
    const uintptr_t data = reinterpret_cast<uintptr_t>(bases.data());
    std::memcpy(zones.data() + 0x10, &data, sizeof data);
    std::memcpy(zones.data() + 0x18, &count, sizeof count);
}
bool allowed(Sphere s, uint32_t actor = 1) {
    return check_current(reinterpret_cast<uintptr_t>(zones.data()), actor, s, &copy);
}
enum class Change { None, Abandon, Shrink, Replace, Count, Altar, Identity, FailBase, FailHeader };
Change change = Change::None;
int base_reads = 0, header_reads = 0;
bool changing_copy(void* dst, uintptr_t src, size_t size) {
    const bool base = src == reinterpret_cast<uintptr_t>(bases.data());
    const bool header = src == reinterpret_cast<uintptr_t>(zones.data()) + 0x10;
    if (base && ++base_reads == 2 && change == Change::FailBase) return false;
    if (header && ++header_reads == 2 && change == Change::FailHeader) return false;
    if (!copy(dst, src, size)) return false;
    if (base && base_reads == 1) {
        switch (change) {
        case Change::Abandon: bases[0].abandoned = 1; break;
        case Change::Shrink: bases[0].max[0] = 0; break;
        case Change::Replace: {
            const uintptr_t replacement = reinterpret_cast<uintptr_t>(&bases[1]);
            std::memcpy(zones.data() + 0x10, &replacement, sizeof replacement);
            break;
        }
        case Change::Count: publish(0); break;
        case Change::Altar: bases[0].altar = 99; break;
        case Change::Identity: bases[0].id[0] = 1; break;
        default: break;
        }
    }
    return true;
}
bool counted_copy(void*, uintptr_t, size_t) { ++header_reads; return false; }
}
int main() {
    bases[0] = box(-10, -10, -10, 10, 10, 10); publish(1);
    check(allowed({{0, 0, 0}, 1}), "inside");
    check(!allowed({{20, 0, 0}, 1}), "outside");
    check(!allowed({{9.5, 0, 0}, 1}), "center inside but sphere crosses edge");
    check(allowed({{9, 0, 0}, 1}), "exact positive tangent");
    check(allowed({{-9, 0, 0}, 1}), "exact negative tangent");
    check(!allowed({{std::nextafter(9.0, 10.0), 0, 0}, 1}), "no epsilon beyond tangent");
    check(!allowed({{9, 0, 0}, std::nextafter(1.0, 2.0)}), "sub-ULP positive crossing refused");
    check(!allowed({{-9, 0, 0}, std::nextafter(1.0, 2.0)}), "sub-ULP negative crossing refused");
    check(!allowed({{0, 9.5, 0}, 1}), "vertical upper crossing");
    check(!allowed({{0, -9.5, 0}, 1}), "vertical lower crossing");
    check(allowed({{0, 9, 0}, 1}), "vertical tangent");
    check(!allowed({{0, 0, 9.5}, 1}), "z crossing");
    check(!allowed({{0, 0, -9.5}, 1}), "negative z crossing");
    bases[0] = box(-10, -10, -10, 1, 10, 10);
    bases[1] = box(-1, -10, -10, 10, 10, 10); publish(2);
    check(!allowed({{0, 0, 0}, 2}), "overlap union is not one base");
    check(allowed({{5, 0, 0}, 2}), "second base may contain whole sphere");
    bases[1].abandoned = 1;
    check(!allowed({{5, 0, 0}, 2}), "abandoned base refused");
    bases[0] = box(-10, -10, -10, 10, 10, 10); publish(1);
    check(allowed({{5, 0, 0}, 2}), "request initially contained");
    bases[0] = box(-20, -10, -10, 0, 10, 10);
    check(!allowed({{5, 0, 0}, 2}), "altar moved before impact rechecked");
    bases[0] = box(-10, -10, -10, 10, 10, 10);
    check(!allowed({{0, 0, 0}, 1}, 0), "unowned impact refused");
    bases[0].shape = 1;
    check(!allowed({{0, 0, 0}, 1}), "unqualified non-box geometry refused");
    bases[0].shape = 0; bases[0].altar = 0;
    check(!allowed({{0, 0, 0}, 1}), "missing altar refused");
    bases[0].altar = 42; bases[0].min[0] = bases[0].max[0] + 1;
    check(!allowed({{0, 0, 0}, 1}), "inverted bounds refused");
    bases[0] = box(-10, -10, -10, 10, 10, 10);
    check(!allowed({{0, 0, 0}, -1}), "negative radius refused");
    check(!allowed({{0, 0, 0}, 0}), "zero radius refused");
    check(!allowed({{0, 0, 0}, std::numeric_limits<double>::infinity()}), "infinite radius refused");
    check(!allowed({{NAN, 0, 0}, 1}), "nonfinite center refused");
    check(!allowed({{0, 0, 0}, NAN}), "nan radius refused");
    check(!allowed({{std::numeric_limits<double>::max(), 0, 0}, 1}), "extreme center refused");
    bases[0].min[0] = (INT64_C(1) << 62) - 4096;
    bases[0].max[0] = (INT64_C(1) << 62) + 1;
    check(allowed({{1073741824.0, 0, 0}, 0x1p-32}), "exact fixed-point upper tangent beyond double precision");
    check(!allowed({{1073741824.0, 0, 0}, 0x1p-31}), "crossing fractional upper bound refused");
    bases[0].min[0] = -(INT64_C(1) << 62) - 1;
    bases[0].max[0] = -(INT64_C(1) << 62) + 4096;
    check(allowed({{-1073741824.0, 0, 0}, 0x1p-32}), "exact fixed-point lower tangent beyond double precision");
    check(!allowed({{-1073741824.0, 0, 0}, 0x1p-31}), "crossing fractional lower bound refused");
    for (const auto& scenario : {
             std::pair{Change::None, "unchanged validation snapshot admitted"},
             std::pair{Change::Abandon, "mid-read abandonment refused"},
             std::pair{Change::Shrink, "mid-read bounds shrink refused"},
             std::pair{Change::Replace, "mid-read list replacement refused even with identical geometry"},
             std::pair{Change::Count, "mid-read count change refused"},
             std::pair{Change::Altar, "mid-read altar replacement refused"},
             std::pair{Change::Identity, "mid-read base identity replacement refused"},
             std::pair{Change::FailBase, "unreadable base validation refused"},
             std::pair{Change::FailHeader, "unreadable header validation refused"}}) {
        bases[0] = box(-10, -10, -10, 10, 10, 10);
        bases[1] = bases[0]; publish(1);
        change = scenario.first; base_reads = 0; header_reads = 0;
        const bool result = check_current(reinterpret_cast<uintptr_t>(zones.data()), 1, {{0, 0, 0}, 1}, &changing_copy);
        check(result == (scenario.first == Change::None), scenario.second);
    }
    header_reads = 0;
    check(!check_current(std::numeric_limits<uintptr_t>::max() - 0x17, 1, {{0, 0, 0}, 1}, &counted_copy) &&
          header_reads == 0, "wrapping list header refused before read");
    for (int axis = 0; axis < 3; ++axis) {
        bases[0] = box(-10, -10, -10, 10, 10, 10); publish(1);
        for (int sign : {-1, 1}) {
            Sphere s{{0, 0, 0}, 1}; s.center[axis] = sign * 9;
            check(allowed(s), "tangent on every signed axis admitted");
            s.radius = std::nextafter(1.0, 2.0);
            check(!allowed(s), "sub-ULP crossing on every signed axis refused");
            s.center[axis] = sign * std::numeric_limits<double>::infinity();
            check(!allowed(s), "infinite center on every signed axis refused");
            s.center[axis] = NAN;
            check(!allowed(s), "NaN center on every axis refused");
        }
        bases[0].min[axis] = INT64_MIN; bases[0].max[axis] = INT64_MAX;
        Sphere s{{0, 0, 0}, 1}; s.center[axis] = -2147483647.0;
        check(allowed(s), "INT64_MIN exact lower tangent admitted");
        s.center[axis] = 2147483647.0;
        check(!allowed(s), "crossing INT64_MAX by one fixed unit refused");
    }
    bases[0] = box(-10, -10, -10, 10, 10, 10);
    check(!allowed({{0, 0, 0}, std::numeric_limits<double>::max()}), "huge finite radius refused");
    check(allowed({{0, 0, 0}, std::numeric_limits<double>::denorm_min()}), "positive subnormal radius inside admitted");
    check(!allowed({{10, 0, 0}, std::numeric_limits<double>::denorm_min()}), "subnormal crossing refused");
    bases[0].owner_kind = 7;
    check(allowed({{0, 0, 0}, 1}, 99), "base owner kind does not grant or replace native actor permission");
    bases[0].owner_kind = 8;
    check(!allowed({{0, 0, 0}, 1}), "unknown owner kind refused");
    publish(0); check(!allowed({{0, 0, 0}, 1}), "no bases refused");
    publish(4097); check(!allowed({{0, 0, 0}, 1}), "unbounded count refused");
    publish(1);
    uintptr_t null_data = 0;
    std::memcpy(zones.data() + 0x10, &null_data, sizeof null_data);
    check(!allowed({{0, 0, 0}, 1}), "missing base list refused");
    check(!check_current(0, 1, {{0, 0, 0}, 1}, &copy), "missing zones refused");
    check(!check_current(reinterpret_cast<uintptr_t>(zones.data()), 1, {{0, 0, 0}, 1}, nullptr), "missing reader refused");
    check(!check_current(std::numeric_limits<uintptr_t>::max(), 1, {{0, 0, 0}, 1}, &copy), "address overflow refused");
    std::printf("building area: %d passed, %d failed\n", total - failed, failed);
    return failed ? 1 : 0;
}
