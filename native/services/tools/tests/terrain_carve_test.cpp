// SPDX-License-Identifier: MIT
#include "../hooks/terrain_carve.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>

using namespace kiln::tools::carve;
static int checks = 0;
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); std::exit(1); } } while (0)
static bool copy_memory(void* dst, const void* src, size_t n) { std::memcpy(dst, src, n); return true; }
static bool unreadable(void*, const void*, size_t) { return false; }
static void set(uint8_t (&bits)[32], unsigned cell) { bits[cell / 8] |= uint8_t(1u << (cell % 8)); }
static BrushCopy descriptor(const float* coefficients, uint64_t count) {
    BrushCopy b;
    for (size_t i = 0; i < b.bytes.size(); ++i) b.bytes[i] = uint8_t(i * 13 + 1);
    std::memcpy(b.bytes.data() + coefficient_offset, &coefficients, sizeof coefficients);
    std::memcpy(b.bytes.data() + coefficient_count_offset, &count, sizeof count);
    return b;
}
int main() {
    // These are fixtures independently confirmed by the extracted terrain registry, not runtime defaults.
    constexpr unsigned materials[] = {8,66,69,74,81,87,102,107,64,23,36,43,44,61,62,92,14,33,65,72,48};
    uint8_t bits[32]{};
    for (auto m : materials) set(bits, m);
    std::array<float, 256> source, filtered;
    for (unsigned i = 0; i < 256; ++i) source[i] = float(i + 1) / 7;
    auto original = descriptor(source.data(), source.size());
    const auto saved = original;
    BrushCopy result;
    CHECK(filter_brush(original.bytes.data(), bits, result, filtered, copy_memory));
    CHECK(original.bytes == saved.bytes);
    for (unsigned i = 0; i < 256; ++i) {
        bool allowed = std::find(std::begin(materials), std::end(materials), i) != std::end(materials);
        CHECK(filtered[i] == (allowed ? source[i] : 0));
    }
    for (size_t i = 0; i < brush_size; ++i)
        if (i < coefficient_offset || i >= coefficient_count_offset + 8) CHECK(result.bytes[i] == saved.bytes[i]);

    // Optional entries are per policy: mushroom references and underwater mud are not hidden global defaults.
    for (unsigned optional : {37u,73u,91u}) {
        CHECK(filtered[optional] == 0);
        set(bits, optional);
        CHECK(filter_brush(original.bytes.data(), bits, result, filtered, copy_memory));
        CHECK(filtered[optional] == source[optional]);
        bits[optional / 8] &= uint8_t(~(1u << (optional % 8)));
    }
    // Even a hostile bit set cannot turn air or building materials into terrain.
    std::fill(std::begin(bits), std::end(bits), uint8_t(255));
    CHECK(filter_brush(original.bytes.data(), bits, result, filtered, copy_memory));
    for (unsigned i = 0; i < 256; ++i) CHECK(filtered[i] == (i > 0 && i < 128 ? source[i] : 0));

    // Each individual policy bit is independent. Allowed values are bit-exact, even signed zero and NaN
    // payloads: copying the game's coefficients must not perform floating-point arithmetic on them.
    std::array<uint32_t, 256> representations;
    constexpr uint32_t special[] = {0x80000000, 0x7fc12345, 0x7f812345, 0xff800000, 0x00000001, 0x3f800001};
    for (unsigned i = 0; i < 256; ++i) representations[i] = special[i % std::size(special)];
    std::memcpy(source.data(), representations.data(), sizeof source);
    const auto source_before = representations;
    for (unsigned selected = 0; selected < 256; ++selected) {
        std::fill(std::begin(bits), std::end(bits), uint8_t(0));
        set(bits, selected);
        CHECK(filter_brush(original.bytes.data(), bits, result, filtered, copy_memory));
        std::array<uint32_t, 256> actual;
        std::memcpy(actual.data(), filtered.data(), sizeof filtered);
        for (unsigned cell = 0; cell < 256; ++cell)
            CHECK(actual[cell] == (cell == selected && cell > 0 && cell < 128 ? representations[cell] : 0u));
    }
    CHECK(std::memcmp(source.data(), source_before.data(), sizeof source) == 0);

    // A partial source span is zero-extended, never read past its count. Allowed coefficients stay exact,
    // including zero/negative values the engine treats as unmineable. The brush geometry is not reimplemented.
    std::fill(std::begin(bits), std::end(bits), uint8_t(0));
    set(bits, 8); set(bits, 14);
    source[8] = -2; source[14] = 0;
    auto short_span = descriptor(source.data(), 15);
    CHECK(filter_brush(short_span.bytes.data(), bits, result, filtered, copy_memory));
    CHECK(filtered[8] == -2 && filtered[14] == 0);
    for (unsigned i = 15; i < 256; ++i) CHECK(filtered[i] == 0);
    auto empty = descriptor(nullptr, 0);
    CHECK(filter_brush(empty.bytes.data(), bits, result, filtered, copy_memory));
    CHECK(std::all_of(filtered.begin(), filtered.end(), [](float f) { return f == 0; }));
    auto bad = descriptor(nullptr, 1);
    CHECK(!filter_brush(bad.bytes.data(), bits, result, filtered, copy_memory));
    bad = descriptor(source.data(), 257);
    CHECK(!filter_brush(bad.bytes.data(), bits, result, filtered, copy_memory));
    CHECK(!filter_brush(nullptr, bits, result, filtered, copy_memory));
    CHECK(!filter_brush(original.bytes.data(), bits, result, filtered, unreadable));

    // Three different sphere radii and fractional boundaries: only coefficients change; every byte containing
    // radius, origin, cell size, rotations, direction and the game's report pointer survives unchanged.
    for (float radius : {1.0f, 2.0f, 4.0f}) {
        auto b = original;
        std::memcpy(b.bytes.data(), &radius, sizeof radius);
        const auto before = b;
        CHECK(filter_brush(b.bytes.data(), bits, result, filtered, copy_memory));
        CHECK(std::memcmp(result.bytes.data(), before.bytes.data(), coefficient_offset) == 0);
        CHECK(std::memcmp(result.bytes.data() + 0x80, before.bytes.data() + 0x80, 0x10) == 0);
    }
    std::printf("terrain carve: %d checks passed\n", checks);
}
