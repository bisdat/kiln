// SPDX-License-Identifier: MIT
// Shared by regions_test.cpp (Linux host) and regions_win_test.cpp (native Windows): the stand-ins regions.cpp needs from
// the rest of kiln.build when it is compiled alone (KILN_REGIONS_HOST_TEST), and a game-like region list to hand it.
// Include once before "../regions.cpp" with KILN_REGIONS_SUPPORT_STUBS defined, and once after it without.
#ifdef KILN_REGIONS_SUPPORT_STUBS
#include <cstdint>
#include <cstdio>
#include <string>
#define KILN_CALL
struct KilnWorldEvent { int kind, event; };
constexpr int KILN_WORLD_SERVER = 1, KILN_WORLD_STOPPING = 2;
namespace kb {
int voxel_closes = 0, transient_closes = 0, block_closes = 0, warnings = 0, errors = 0;
void block_world_closed() { ++block_closes; }
void terrain_budget_reset() {}
void vox_world_closed() { ++voxel_closes; }
void transient_world_closed() { ++transient_closes; }

struct { bool persist = true; } F;
std::string identity;
std::string detail;
void set_detail(const char* text) { detail = text; }
std::string current_world() { return identity; }
}
#define KILN_REGIONS_HOST_TEST
#define KB_I(...) ((void)0)
#define KB_W(...) ((void)++::kb::warnings)
#define KB_E(...) ((void)++::kb::errors)
#else
#include <array>
#include <cstring>
#include <set>
#include <vector>

namespace regions_test {
namespace {  // regions.cpp's types live in an anonymous namespace: so do these
// A game array {data, count, capacity, grow} over memory the test owns; grow is the game's contract: (array,
// -required count, mode) -> true once capacity >= the required count.
struct FakeList {
    kb::GameArray a{};                // first: grow() gets &a and finds the rest of the list from it
    std::vector<uint8_t>* store = nullptr;
    bool fail_grow = false;
};
bool fake_grow(kb::GameArray* a, int64_t neg_required, uint8_t) {
    auto* l = reinterpret_cast<FakeList*>(a);
    if (l->fail_grow) return false;
    const uint64_t need = static_cast<uint64_t>(-neg_required);
    if (need <= a->capacity) return true;
    const uint64_t cap = need > a->capacity * 2 ? need : a->capacity * 2;
    std::vector<uint8_t> next(static_cast<size_t>(cap) * 0x30);
    if (a->count) std::memcpy(next.data(), a->data, static_cast<size_t>(a->count) * 0x30);
    l->store->swap(next);
    a->data = l->store->data();
    a->capacity = cap;
    return true;
}
// One entry as the game's builder writes it: {BaseId[16], float3 min, pad, float3 max, pad}.
void add_entry(FakeList& l, const char id[17], const float lo[3], const float hi[3]) {
    if (l.a.count == l.a.capacity) fake_grow(&l.a, -static_cast<int64_t>(l.a.count + 1), 1);
    uint8_t* e = l.a.data + l.a.count * 0x30;
    std::memset(e, 0, 0x30);
    std::memcpy(e, id, 16);
    std::memcpy(e + 0x10, lo, 12);
    std::memcpy(e + 0x20, hi, 12);
    ++l.a.count;
}
FakeList make_list(std::vector<uint8_t>& store) {
    FakeList l;
    l.store = &store;
    l.a.grow = &fake_grow;
    return l;
}
bool is_kiln(const uint8_t* e) { return std::memcmp(e, "KILNBILD", 8) == 0; }
// The voxel serializer's rule (docs/re/build.md §12): a stored 32-cell chunk goes into the blob of every entry whose
// bounds its 16 m box touches (min <= max on every axis, edges included).
bool saved_by(const std::array<int32_t, 3>& c, const uint8_t* e) {
    float lo[3], hi[3];
    std::memcpy(lo, e + 0x10, 12);
    std::memcpy(hi, e + 0x20, 12);
    for (int k = 0; k < 3; ++k) {
        const float clo = static_cast<float>(c[k]) * 16.0f, chi = clo + 16.0f;
        if (!(clo <= hi[k] && lo[k] <= chi)) return false;
    }
    return true;
}
// Every kept chunk is in some entry's blob; with `no_redundant`, no Kiln entry touches a chunk a game entry saves.
bool every_chunk_saved(const kb::GameArray& a, const std::set<std::array<int32_t, 3>>& kept, bool no_redundant) {
    for (const auto& c : kept) {
        bool by_game = false, by_kiln = false;
        for (uint64_t i = 0; i < a.count; ++i) {
            const uint8_t* e = a.data + i * 0x30;
            if (!saved_by(c, e)) continue;
            (is_kiln(e) ? by_kiln : by_game) = true;
        }
        if (!by_game && !by_kiln) return false;
        if (no_redundant && by_game && by_kiln) return false;
    }
    return true;
}
uint64_t kiln_entries(const kb::GameArray& a) {
    uint64_t n = 0;
    for (uint64_t i = 0; i < a.count; ++i) n += is_kiln(a.data + i * 0x30);
    return n;
}
}  // namespace
}  // namespace regions_test
#endif
