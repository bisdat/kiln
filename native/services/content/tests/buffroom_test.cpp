// SPDX-License-Identifier: MIT
// kiln.content gamedata, host test: the room of Kiln's copy of the game's buff collection (chapter 17.2).
// Compiles the real gamedata_clones.cpp over a made-up collection in one arena of this process (so every self-relative
// offset reaches), with a near allocator that can refuse: the 4,096-clone reservation, its 1,024 fallback, a refusal
// of both, the checked sizes, the map the game's own lookup reads (an emulation of getBuffType, server 0x933700), the
// slot checks (full, id 0, a game buff's id, two clones with one id) and a header that cannot be allocated.
// Game-memory paths this test does not take are discarded at link time (buffroom_test.sh).
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>
#include <unordered_set>
#include "../gamedata.h"
#undef __try
#define __try if (true)
#define __except(x) else if (false)
inline long long _InterlockedOr64(volatile long long* p, long long v) { auto old = *p; *p = old | v; return old; }
inline long long _InterlockedExchange64(volatile long long* p, long long v) { auto old = *p; *p = v; return old; }
#include "../gamedata_clones.cpp"

#include <cstdarg>
#include <stdexcept>
#include <string>

namespace {
// One arena for the made-up game data and everything the near allocator hands out: offsets between them stay small.
alignas(64) uint8_t g_arena[48u << 20];
size_t g_arena_used = 0;
size_t g_alloc_limit = SIZE_MAX;  // near_alloc refuses a request larger than this
uint32_t g_alloc_fail_after = UINT32_MAX;  // ... and every request after this many
uint32_t g_allocs = 0;
uint8_t* arena(size_t n) {
    const size_t at = (g_arena_used + 15) & ~size_t(15);
    if (at + n > sizeof g_arena) throw std::runtime_error("test arena full");
    g_arena_used = at + n;
    std::memset(g_arena + at, 0, n);
    return g_arena + at;
}
std::string g_last_log;
}  // namespace

namespace kc {
const KilnApi* g_api = nullptr;
KilnOwner g_owner = 0;
StoreMutex g_store;
StoreMutex& store_mutex() { return g_store; }
bool rd(uintptr_t p, void* out, size_t n) {
    if (!p) return false;
    std::memcpy(out, reinterpret_cast<void*>(p), n);
    return true;
}
uint32_t fnv1a(const void* p, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) h = (h ^ static_cast<const uint8_t*>(p)[i]) * 16777619u;
    return h;
}
std::string guid_text(const uint8_t*) { return "11111111-2222-4333-8444-555555555555"; }
std::string owner_label(KilnOwner) { return "mod test"; }
void logf(int, const char* fmt, ...) {
    char b[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    g_last_log = b;
}
void* near_alloc(uintptr_t, size_t n) {
    if (n > g_alloc_limit || ++g_allocs > g_alloc_fail_after) return nullptr;
    return arena(n);
}
void near_alloc_add(uintptr_t, size_t) {}
namespace gd {
TType coll_type, map_type, buff_type;
const TType* TField::type() const { return type_rec == 1 ? &map_type : nullptr; }
const TType* TType::resolved() const { return this; }
const TType* TType::inner() const { return nullptr; }
const TField* TType::field(const std::string& n) const {
    for (const TField& f : fields)
        if (f.name == n) return &f;
    return nullptr;
}
GRegistry buffs;
GRegistry& greg(uint32_t) { return buffs; }
const TType* type_named(const std::string& n) { return n == "keen::BuffTypeCollectionResource" ? &coll_type : nullptr; }
uintptr_t g_coll = 0;
bool res_list(uint32_t, std::vector<std::array<uint8_t, 16>>* out) {
    out->assign(1, std::array<uint8_t, 16>{1});
    return true;
}
uintptr_t res_data(const uint8_t*, uint32_t, uint64_t*) { return g_coll; }
bool blob_array(uintptr_t addr, uintptr_t* data, uint32_t* count) {
    int32_t rel = 0;
    std::memcpy(&rel, reinterpret_cast<void*>(addr), 4);
    std::memcpy(count, reinterpret_cast<void*>(addr + 4), 4);
    *data = rel ? addr + rel : 0;
    return true;
}
}  // namespace gd
}  // namespace kc

namespace {
using namespace kc::gd;
int checks = 0, failures = 0;
void check(bool yes, const std::string& what) {
    ++checks;
    if (!yes) {
        ++failures;
        std::printf("FAIL %s\n", what.c_str());
    }
}

constexpr uint32_t kGameBuffs = 274, kStride = 184, kBuckets0 = 512;

// The game's collection: 274 records of 184 bytes (buffTypeId at +0), a map of 512 buckets, as the game has it.
void make_game() {
    g_arena_used = 0;
    g_allocs = 0;
    g_alloc_limit = SIZE_MAX;
    g_alloc_fail_after = UINT32_MAX;
    BM.~BuffMirror();
    new (&BM) BuffMirror();
    coll_type = TType{};
    coll_type.name = "keen::BuffTypeCollectionResource";
    coll_type.size = 32;
    coll_type.hash = 0x1be67cef;
    coll_type.fields = {TField{"buffMap", 1, 0}, TField{"buffs", 0, 0x18}};
    map_type = TType{};
    map_type.fields = {TField{"entries", 0, 0}, TField{"keys", 0, 8}, TField{"values", 0, 0x10}};
    buff_type = TType{};
    buff_type.size = kStride;
    buff_type.fields = {TField{"buffTypeId", 0, 0}};
    buffs = GRegistry{};
    buffs.type = &buff_type;
    buffs.available = true;
    uint8_t* coll = arena(32);
    uint8_t* recs = arena(size_t(kGameBuffs) * kStride);
    uint8_t* entries = arena(size_t(kBuckets0) * 8);
    for (uint32_t i = 0; i < kGameBuffs; ++i) {
        const uint32_t id = 0x9e3779b9u * (i + 1);
        std::memcpy(recs + size_t(i) * kStride, &id, 4);
    }
    auto blob = [&](size_t field, const uint8_t* data, uint32_t n) {
        const int32_t rel = static_cast<int32_t>(data - (coll + field));
        std::memcpy(coll + field, &rel, 4);
        std::memcpy(coll + field + 4, &n, 4);
    };
    blob(0, entries, kBuckets0);
    blob(0x18, recs, kGameBuffs);
    g_coll = reinterpret_cast<uintptr_t>(coll);
}

uint32_t room() { return BM.cap - BM.n; }

void test_room() {
    std::string err;
    make_game();
    check(mirror_init(&err), "the reservation succeeds when near memory has it: " + err);
    check(room() == 4096, "the room is 4,096 clones (it is " + std::to_string(room()) + ")");
    check(BM.n == kGameBuffs, "the game's 274 records are mirrored");

    make_game();
    g_alloc_limit = size_t(kGameBuffs + 1024) * kStride;  // near memory has room for the fallback only
    err.clear();
    check(mirror_init(&err), "a refused 4,096 reservation falls back: " + err);
    check(room() == 1024, "the fallback room is 1,024 clones (it is " + std::to_string(room()) + ")");

    make_game();
    g_alloc_limit = 64;  // nothing fits
    err.clear();
    check(!mirror_init(&err), "no reservation at all: no copy");
    check(err.find("no memory near the buff collection") != std::string::npos, "and the reason says so: " + err);
}
// The game's getBuffType over a header (server 0x933700, docs/re/gamedata.md "Buffs"): bucket = id & (entries.count - 1),
// entries[bucket] one u64 {start, count}, keys[start..start+count), index = values[k], refused only when index > buffs.count
// (`ja`), record = buffs + index * stride. 0 = not found.
uintptr_t game_lookup(uintptr_t coll, uint32_t id, uint32_t stride, bool* index_eq_count = nullptr) {
    auto u32 = [](uintptr_t a) { uint32_t v; std::memcpy(&v, reinterpret_cast<void*>(a), 4); return v; };
    auto rel = [&](uintptr_t a) { int32_t r; std::memcpy(&r, reinterpret_cast<void*>(a), 4); return r ? a + r : uintptr_t{0}; };
    if (!id || !coll) return 0;
    const uint32_t buckets = u32(coll + 4);
    if (!buckets) return 0;
    uint64_t q;
    std::memcpy(&q, reinterpret_cast<void*>(rel(coll) + (id & (buckets - 1)) * 8ull), 8);
    const uint32_t start = static_cast<uint32_t>(q), count = static_cast<uint32_t>(q >> 32);
    const uintptr_t keys = rel(coll + 8), values = rel(coll + 0x10);
    for (uint32_t i = 0; i < count; ++i)
        if (u32(keys + (start + i) * 4ull) == id) {
            const uint32_t index = u32(values + (start + i) * 4ull);
            if (index > u32(coll + 0x1c)) return 0;
            if (index_eq_count && index == u32(coll + 0x1c)) *index_eq_count = true;
            return rel(coll + 0x18) + uint64_t{index} * stride;
        }
    return 0;
}

void test_sizes() {
    uint32_t cap = 0;
    size_t bytes = 0;
    std::string why;
    check(buff_mirror_size(274, 4096, 184, &cap, &bytes, &why) && cap == 4370 && bytes == 804080,
          "4,096 clones next to 274 game records of 184 bytes: 4,370 records, 804,080 bytes (chapter 17.2 section 4)");
    check(!buff_mirror_size(UINT32_MAX - 100, 4096, 184, &cap, &bytes, &why) && why.find("32 bits") != std::string::npos,
          "a record count over 32 bits is refused: " + why);
    check(!buff_mirror_size(400000, 4096, 184, &cap, &bytes, &why), "records over one block are refused: " + why);
    check(!buff_mirror_size(0xFFFFF000u, 4095, 0xFFFFFFF8u, &cap, &bytes, &why), "a product past 64 bits never wraps into a small one");
    check(!buff_mirror_size(274, 4096, 0, &cap, &bytes, &why) && !buff_mirror_size(274, 4096, 12, &cap, &bytes, &why),
          "a zero or unaligned record size is refused");
    BuffHeaderSize hs;
    check(buff_header_size(32, 32, 4370, 512, &hs, &why) && hs.buckets == 8192 && hs.total == 100528,
          "one header for 4,370 records: 8,192 buckets, 100,528 bytes (chapter 17.2 section 4)");
    check(buff_header_size(32, 32, 1298, 512, &hs, &why) && hs.buckets == 2048, "1,298 records: 2,048 buckets (load under 3/4)");
    check(!buff_header_size(32, 32, 100, 500, &hs, &why), "a bucket count that is not a power of two is refused");
    check(!buff_header_size(24, 32, 100, 512, &hs, &why), "header fields past the struct are refused");
    check(!buff_header_size(32, 32, 0x80000000u, 512, &hs, &why) && why.find("would take") != std::string::npos,
          "a header over one block is refused: " + why);
}

void clone_guid(uint32_t i, uint8_t out[16]) {
    const uint8_t base[16] = {0x6b, 0x69, 0x6c, 0x6e};
    char salt[32];
    std::snprintf(salt, sizeof salt, "buffroom-%u", i);
    derive_guid(base, salt, out);
}

// Clones take records until the room is full; every header built over them is read right by the game's lookup.
void test_slots_and_map() {
    std::string err;
    make_game();
    check(mirror_init(&err), "mirror for the slot test: " + err);
    g_defs.clear();
    g_defs.reserve(kBuffRoomExpanded + 8);
    std::vector<uint32_t> ids;
    uint32_t salt = 0;
    while (g_defs.size() < kBuffRoomExpanded) {
        Def d;
        d.section = KILN_GD_BUFFS;
        clone_guid(salt++, d.guid);
        const uint32_t id = kc::fnv1a(d.guid, 16);
        if (!id || BM.by_id.count(id)) continue;  // the fixture keeps its own ids unique; the guard is tested below
        d.name = "Room_" + std::to_string(g_defs.size());
        g_defs.push_back(std::move(d));
        if (!take_buff_slot(g_defs.back(), id)) check(false, "clone " + std::to_string(g_defs.size()) + ": " + g_defs.back().problem);
        ids.push_back(id);
        if (g_defs.size() == 1 || g_defs.size() == 1024 || g_defs.size() == 1025) {
            check(build_header(&err), "a header over " + std::to_string(g_defs.size()) + " clones: " + err);
        }
    }
    check(BM.used == kBuffRoomExpanded, "4,096 clones took 4,096 records");
    Def over;
    over.section = KILN_GD_BUFFS;
    clone_guid(999999, over.guid);
    check(!take_buff_slot(over, kc::fnv1a(over.guid, 16)) && over.refusal == KILN_DEFINE_CAPACITY_EXCEEDED &&
              over.problem == "Kiln's buff collection is full (4096 clones)",
          "the 4,097th is refused, full, with its reason: " + over.problem);
    check(BM.used == kBuffRoomExpanded, "a refused clone takes no record");
    check(take_buff_slot(g_defs[7], ids[7]) && g_defs[7].slot == BM.n + 7 && BM.used == kBuffRoomExpanded,
          "a clone that has its record keeps it (a redefinition takes no second one)");
    Def zero, game, twin;
    zero.section = game.section = twin.section = KILN_GD_BUFFS;
    check(!take_buff_slot(zero, 0) && zero.refusal == KILN_DEFINE_ID_COLLISION, "buff id 0 is refused (the game never finds it)");
    check(!take_buff_slot(game, BM.ids[0]) && game.problem.find("game buff") != std::string::npos, "a game buff's id is refused");
    check(!take_buff_slot(twin, ids[3]) && twin.refusal == KILN_DEFINE_ID_COLLISION && twin.problem.find("Room_3") != std::string::npos,
          "a second clone with one clone's id is refused and names it: " + twin.problem);

    check(build_header(&err), "the header over 4,370 records: " + err);
    check(BM.header_count == kGameBuffs + kBuffRoomExpanded, "it lists every record");
    uint32_t found = 0, wrong = 0;
    bool eq = false;
    for (uint32_t i = 0; i < BM.n + BM.used; ++i) {
        const uintptr_t r = game_lookup(BM.header, BM.ids[i], kStride, &eq);
        if (r == BM.array + uintptr_t{i} * kStride) ++found;
        else ++wrong;
    }
    check(found == kGameBuffs + kBuffRoomExpanded && !wrong, "the game's lookup finds every game buff and clone in Kiln's copy (" +
                                                                std::to_string(found) + " found, " + std::to_string(wrong) + " wrong)");
    check(!eq, "no map entry has index == count (getBuffType's ja would take it)");
    uint32_t unknown = 0x12345678u;
    while (BM.by_id.count(unknown)) ++unknown;
    check(game_lookup(BM.header, unknown, kStride) == 0, "an id nobody has finds nothing");
    uint32_t c = 0;
    std::memcpy(&c, reinterpret_cast<void*>(BM.header + 0x1c), 4);
    check(c == BM.n + BM.used, "the header's buff count is the record count");
    check(BM.headers.size() == 4 && BM.header_bytes > 0, "four headers built, all kept (" + std::to_string(BM.header_bytes) + " bytes)");

    // near memory gone for the next header: a named refusal, and the registries keep the header they read
    const uintptr_t before = BM.header;
    g_alloc_fail_after = g_allocs;
    err.clear();
    check(!build_header(&err) && err.find("no memory near the buff collection for its header") != std::string::npos,
          "a header near memory cannot hold is refused with its reason: " + err);
    check(BM.header == before, "and the published header stays");
}
}  // namespace

int main() {
    try {
        test_room();
        test_sizes();
        test_slots_and_map();
    } catch (const std::exception& e) {
        std::printf("FAIL threw: %s\n", e.what());
        ++failures;
    }
    std::printf("buffroom-test: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
