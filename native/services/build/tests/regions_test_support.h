// SPDX-License-Identifier: MIT
// Shared by regions_test.cpp (Linux host) and regions_win_test.cpp (native Windows): the stand-ins regions.cpp needs from
// the rest of kiln.build when it is compiled alone (KILN_REGIONS_HOST_TEST), and a game-like region list to hand it.
// Include once before "../regions.cpp" with KILN_REGIONS_SUPPORT_STUBS defined, and once after it without.
#ifdef KILN_REGIONS_SUPPORT_STUBS
#include <cstdarg>
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
// kiln.log: the newest line of each level as regions.cpp formatted it
std::string last_info, last_warning;
#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 3, 4)))
#endif
void log_line(std::string& line, int* count, const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    line = buf;
    if (count) ++*count;
}
// kiln.caps: the newest report of the row about the save-region builder's callers
int cap_reports = 0;
bool cap_available = false;
std::string cap_text;
void report_callers_row(bool available, const std::string& text) {
    ++cap_reports;
    cap_available = available;
    cap_text = text;
}
}
#define KILN_REGIONS_HOST_TEST
#define KB_I(...) ::kb::log_line(::kb::last_info, nullptr, __VA_ARGS__)
#define KB_W(...) ::kb::log_line(::kb::last_warning, &::kb::warnings, __VA_ARGS__)
#define KB_E(...) ((void)++::kb::errors)
#else
#include <algorithm>
#include <array>
#include <cmath>
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
// The 16 m chunk positions an entry's bounds touch (edges included), counted one by one on each axis with the game's
// float test: the most stored chunks the voxel writer can collect for its record.
uint64_t entry_positions(const uint8_t* e) {
    float lo[3], hi[3];
    std::memcpy(lo, e + 0x10, 12);
    std::memcpy(hi, e + 0x20, 12);
    uint64_t n = 1;
    for (int k = 0; k < 3; ++k) {
        uint64_t axis = 0;
        const int64_t first = static_cast<int64_t>(std::floor(lo[k] / 16.0f)) - 2;
        const int64_t last = static_cast<int64_t>(std::floor(hi[k] / 16.0f)) + 2;
        for (int64_t c = first; c <= last; ++c) {
            const float clo = static_cast<float>(c * 32) * 0.5f, chi = clo + 16.0f;
            axis += clo <= hi[k] && lo[k] <= chi;
        }
        n *= axis;
    }
    return n;
}
constexpr uint64_t kWriterChunks = 4096;  // stored chunks the game's voxel writer collects per record, at most
uint64_t max_kiln_positions(const kb::GameArray& a) {
    uint64_t most = 0;
    for (uint64_t i = 0; i < a.count; ++i)
        if (is_kiln(a.data + i * 0x30)) most = std::max(most, entry_positions(a.data + i * 0x30));
    return most;
}
// The voxel serializer's rule (docs/re/build.md §12), in floats as the game computes it: a stored 32-cell chunk goes
// into the blob of every entry whose bounds its 16 m box touches, but there only its 4 m blocks (8 cells a side, 64 per
// chunk, bit x + 4y + 16z) whose own box touches the bounds keep their cells (edges included); every other block is
// written empty, and the loader leaves an empty block as the world generates it. The writer collects at most 4,096 of
// the record's stored chunks and skips the rest in hash order: of an entry whose bounds touch more chunk positions than
// that, any chunk may be missing, so it counts as saving none.
uint64_t blocks_saved_by(const std::array<int32_t, 3>& c, const uint8_t* e) {
    if (entry_positions(e) > kWriterChunks) return 0;
    float lo[3], hi[3];
    std::memcpy(lo, e + 0x10, 12);
    std::memcpy(hi, e + 0x20, 12);
    for (int k = 0; k < 3; ++k) {
        const float clo = static_cast<float>(c[k] * 32) * 0.5f, chi = clo + 16.0f;
        if (!(clo <= hi[k] && lo[k] <= chi)) return 0;
    }
    uint64_t saved = 0;
    for (int b = 0; b < 64; ++b) {
        bool in = true;
        for (int k = 0; k < 3; ++k) {
            const float blo = static_cast<float>((b >> (2 * k)) & 3) * 4.0f + static_cast<float>(c[k] * 32) * 0.5f;
            if (!(blo <= hi[k] && lo[k] <= blo + 4.0f)) in = false;
        }
        if (in) saved |= uint64_t{1} << b;
    }
    return saved;
}
constexpr uint64_t kWholeChunk = ~uint64_t{0};
// Every block of every kept chunk is in some entry's blob. With `no_redundant`, no Kiln entry touches a chunk the
// game's entries save whole already (a chunk they save only in part needs Kiln's box for the rest).
bool every_chunk_saved(const kb::GameArray& a, const std::set<std::array<int32_t, 3>>& kept, bool no_redundant) {
    for (const auto& c : kept) {
        uint64_t by_game = 0, by_kiln = 0;
        for (uint64_t i = 0; i < a.count; ++i) {
            const uint8_t* e = a.data + i * 0x30;
            (is_kiln(e) ? by_kiln : by_game) |= blocks_saved_by(c, e);
        }
        if ((by_game | by_kiln) != kWholeChunk) return false;
        if (no_redundant && by_game == kWholeChunk && by_kiln) return false;
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
