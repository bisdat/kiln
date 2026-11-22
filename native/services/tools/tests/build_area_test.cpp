// SPDX-License-Identifier: MIT
#include "../hooks/build_area.h"
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

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
