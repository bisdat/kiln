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
#include "services/kiln_capacity_util.hpp"  // libstdc++'s fstream has its own __try: parsed before the macro below
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
std::string guid_text(const uint8_t* g) {  // the game's byte order, as content.cpp writes it
    char b[40];
    std::snprintf(b, sizeof b, "%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x", static_cast<unsigned>(g[0] | g[1] << 8 | g[2] << 16 | uint32_t{g[3]} << 24),
                  static_cast<unsigned>(g[4] | g[5] << 8), static_cast<unsigned>(g[6] | g[7] << 8), g[8], g[9], g[10], g[11], g[12], g[13], g[14],
                  g[15]);
    return b;
}
std::string owner_label(KilnOwner) { return "mod test"; }
void logf(int, const char* fmt, ...) {
    char b[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    g_last_log = b;
    if (std::getenv("BUFFROOM_LOG")) std::fprintf(stderr, "log: %s\n", b);
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

// ---- the definition path (clones_define, register_def, gd_pack_add): what the buff path needs works, the rest throws ----
[[noreturn]] void unreachable(const char* what) { throw std::logic_error(std::string("the buff path reached ") + what); }
void StoreMutex::lock() { m_.lock(); }
void StoreMutex::unlock() { m_.unlock(); }
PhaseTimer::PhaseTimer(Phase) {}
PhaseTimer::~PhaseTimer() {}
LayerModScope::LayerModScope(const std::string&) {}
LayerModScope::~LayerModScope() {}
bool guid_parse(const std::string& s, uint8_t out[16]) {
    unsigned a, b, c, d[8];
    if (s.size() != 36 || std::sscanf(s.c_str(), "%8x-%4x-%4x-%2x%2x-%2x%2x%2x%2x%2x%2x", &a, &b, &c, &d[0], &d[1], &d[2], &d[3], &d[4],
                                      &d[5], &d[6], &d[7]) != 11)
        return false;
    const uint32_t aa = a;
    const uint16_t bb = b, cc = c;
    std::memcpy(out, &aa, 4);
    std::memcpy(out + 4, &bb, 2);
    std::memcpy(out + 6, &cc, 2);
    for (int i = 0; i < 8; ++i) out[8 + i] = static_cast<uint8_t>(d[i]);
    return true;
}
uintptr_t g_test_world = 0;
uintptr_t active_world() { return g_test_world; }
uint32_t active_kind() { return KILN_WORLD_SERVER; }
Engine E{};
const KilnAssetsApi* assets_api() { return nullptr; }
bool map_read(uintptr_t, std::vector<uint32_t>*, std::vector<uint64_t>*) { return false; }
const Entry* Registry::lookup(const std::string&) const { unreachable("Registry::lookup"); }
Registry& reg(uint32_t) { unreachable("reg"); }
const Field* Schema::find(const std::string&) const { unreachable("Schema::find"); }
bool store_drop(KilnOwner, uintptr_t) { return false; }
size_t store_flush(std::string*) { return 0; }
void store_for_each(const std::function<void(uintptr_t, const Slot&)>&) {}
void store_originals(uintptr_t, size_t, uint8_t*) {}
std::string data_dir() {
    const char* out = std::getenv("KILN_OUT");
    return std::string(out ? out : ".") + "/content-tests/buffroom-data";
}
void set_owner_label(KilnOwner, const std::string&) {}
namespace gd {
bool g_test_ready = true;
bool gd_ready() { return g_test_ready; }
uint32_t g_after = 0;
void after_changes() { ++g_after; }
uint32_t apply_value(KilnOwner, uint64_t, uint32_t, const GEntry&, const std::string&, const gjson&, const std::string&,
                     std::vector<std::string>*) {
    return 0;
}
void forget_added(KilnOwner, uintptr_t) {}
void GRegistry::add(GEntry e) {
    by_id[e.id] = static_cast<uint32_t>(entries.size());
    by_name[e.name] = static_cast<uint32_t>(entries.size());
    entries.push_back(std::move(e));
}
const GEntry* GRegistry::get(uint32_t id) const {
    auto it = by_id.find(id);
    return it == by_id.end() ? nullptr : &entries[it->second];
}
const GEntry* GRegistry::lookup(const std::string& key) const {
    if (auto it = by_name.find(key); it != by_name.end()) return &entries[it->second];
    if (key.rfind("0x", 0) == 0) return get(static_cast<uint32_t>(std::stoul(key.substr(2), nullptr, 16)));
    return nullptr;
}
bool resolve(Root, Node, const std::string&, std::vector<Resolved>*, std::string*, bool*) { unreachable("resolve"); }
bool res_ready() { return true; }
const char* section_name(uint32_t) { return "buffs"; }
void sequence_clone_entries(GRegistry&, const uint8_t*, const std::string&, uintptr_t, uint32_t, size_t) { unreachable("sequences"); }
const TType* type_hash(uint32_t) { return nullptr; }
}  // namespace gd
}  // namespace kc
extern "C" void kc_nc_set_texts(uint32_t, const char*, size_t) {}

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

// A made-up API: one server world on this thread, whose BuffRegistry holds the game's collection at +0x20.
uint8_t* g_registry = nullptr;
KilnApi g_fake_api{};
void make_world() {
    g_registry = arena(0x40);
    std::memcpy(g_registry + 0x20, &g_coll, 8);
    g_fake_api = KilnApi{};
    g_fake_api.world = [](uint32_t kind) -> uintptr_t { return kind == KILN_WORLD_SERVER ? 0x7000 : 0; };
    g_fake_api.on_world_thread = [](uint32_t) -> bool { return true; };
    g_fake_api.singleton = [](uintptr_t, KilnStr) -> uintptr_t { return reinterpret_cast<uintptr_t>(g_registry); };
    kc::g_api = &g_fake_api;
    // the game's buffs as build_sections lists them: name buff_<id>, its record
    uintptr_t recs = 0;
    uint32_t n = 0;
    blob_array(g_coll + 0x18, &recs, &n);
    for (uint32_t i = 0; i < n; ++i) {
        GEntry e;
        std::memcpy(&e.id, reinterpret_cast<void*>(recs + uintptr_t{i} * kStride), 4);
        char b[24];
        std::snprintf(b, sizeof b, "buff_%08x", e.id);
        e.name = b;
        e.addr = recs + uintptr_t{i} * kStride;
        buffs.add(std::move(e));
    }
}

std::string guid_of(uint32_t i) {
    uint8_t g[16];
    clone_guid(i, g);
    char b[40];
    std::snprintf(b, sizeof b, "%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x", static_cast<unsigned>(g[0] | g[1] << 8 | g[2] << 16 | uint32_t{g[3]} << 24),
                  static_cast<unsigned>(g[4] | g[5] << 8), static_cast<unsigned>(g[6] | g[7] << 8), g[8], g[9], g[10], g[11], g[12], g[13], g[14],
                  g[15]);
    return b;
}

kc::gjson clone_list(uint32_t from, uint32_t n, const std::string& base) {
    kc::gjson list = kc::gjson::array();
    for (uint32_t i = from; i < from + n; ++i) list.push_back({{"guid", guid_of(i)}, {"name", "Batch_" + std::to_string(i)}, {"base", base}});
    return list;
}

uint32_t registered() {
    uint32_t n = 0;
    for (const Def& d : g_defs) n += d.section == KILN_GD_BUFFS && d.state == "registered";
    return n;
}

// One data pack's list while a world runs, and a world's load: one header each, not one per clone (chapter 17.2 section 4).
void test_batches() {
    make_game();
    g_defs.clear();
    make_world();
    char base[16];
    std::snprintf(base, sizeof base, "0x%08x", 0x9e3779b9u);
    std::string err;
    kc::g_test_world = 0;  // no world yet: definitions wait, then the world's load registers them all
    {
        kc::ApplyResult r;
        gd_pack_add(1, KILN_GD_BUFFS, clone_list(0, 2000, base), "pack test/content/a.json", &r);
        check(r.errors.empty() && r.applied == 2000, "2,000 definitions accepted before a world (" + std::to_string(r.errors.size()) + " errors)");
    }
    check(BM.headers.empty() && registered() == 0, "no world: no header, nothing registered");
    kc::g_test_world = 0x7000;
    clones_world_ready(0x7000, KILN_WORLD_SERVER);
    check(registered() == 2000, "the world's load registers all 2,000 (" + std::to_string(registered()) + ")");
    check(BM.headers.size() == 1, "with one header (" + std::to_string(BM.headers.size()) + ")");
    uintptr_t slot = 0;
    std::memcpy(&slot, g_registry + 0x20, 8);
    check(slot == BM.header && BM.attaches == 1, "and one swap of the world's registry to it");

    const size_t headers = BM.headers.size();
    {
        kc::ApplyResult r;
        gd_pack_add(1, KILN_GD_BUFFS, clone_list(2000, 2096, base), "pack test/content/b.json", &r);
        check(r.errors.empty() && r.applied == 2096, "a second pack of 2,096 while the world runs: accepted");
    }
    check(registered() == 4096 && BM.headers.size() == headers + 1, "registered with one more header (" + std::to_string(BM.headers.size()) + ")");
    check(BM.used == 4096 && BM.header_count == kGameBuffs + 4096, "the room is used up exactly");
    {
        kc::ApplyResult r;
        gd_pack_add(1, KILN_GD_BUFFS, clone_list(4096, 1, base), "pack test/content/c.json", &r);
        check(r.errors.size() == 1 && r.errors[0].find("full (4096 clones)") != std::string::npos,
              "the 4,097th is refused with its reason: " + (r.errors.empty() ? std::string("none") : r.errors[0]));
    }
    check(BM.headers.size() == headers + 1, "a refused clone builds no header");
    uint32_t found = 0;
    for (const Def& d : g_defs)
        if (d.state == "registered" && game_lookup(BM.header, kc::fnv1a(d.guid, 16), kStride) == d.data) ++found;
    check(found == 4096, "the game's lookup finds all 4,096 clones (" + std::to_string(found) + ")");
    check(BM.header_bytes < (1u << 20), "two headers for 4,096 clones keep under 1 MiB (" + std::to_string(BM.header_bytes) + " bytes)");
}

KilnCapacityInfo budget() {
    KilnCapacityInfo b{};
    b.size = sizeof b;
    buff_budget(&b);
    return b;
}

struct Answer {
    KilnResult rc;
    KilnDefinitionReply r;
    kc::gjson report;
};
Answer define(KilnOwner owner, const kc::gjson& def, uint32_t timing) {
    Answer a{};
    a.r.size = sizeof a.r;
    std::string rep;
    a.rc = define_buff_result(owner, def.dump(), timing, &a.r, &rep);
    a.report = kc::gjson::parse(rep, nullptr, false);
    return a;
}
Answer receipt(const uint8_t id[16]) {
    Answer a{};
    a.r.size = sizeof a.r;
    std::string rep;
    a.rc = buff_receipt(id, &a.r, &rep);
    a.report = kc::gjson::parse(rep, nullptr, false);
    return a;
}
kc::gjson with(kc::gjson d, const char* key, kc::gjson v) {
    d[key] = std::move(v);
    return d;
}
std::string why(const Answer& a) { return a.report.is_object() && a.report["reason"].is_string() ? a.report["reason"].get<std::string>() : ""; }

// The result-bearing define (chapter 17 I1): registered now, the same again, refusals that take nothing, a definition
// stored for the next start and replayed there, receipts that follow what became of a definition, the budget.
void test_results() {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::remove_all(kc::data_dir(), ec);
    make_game();
    g_defs.clear();
    g_receipts.clear();
    g_receipt_age.clear();
    g_gone.clear();
    g_budget = KilnCapacityInfo{};
    make_world();
    g_fake_api.owner_create = [](KilnStr) -> KilnOwner { return 77; };
    char base[16];
    std::snprintf(base, sizeof base, "0x%08x", 0x9e3779b9u);
    auto def = [&](uint32_t i) { return kc::gjson{{"guid", guid_of(i)}, {"name", "Result_" + std::to_string(i)}, {"base", base}}; };

    KilnCapacityInfo b = budget();
    check(b.readiness == KILN_CAPACITY_NOT_READY && !b.capacity && (b.flags & KILN_CAPACITY_PROCESS), "before the first world the budget is NotReady");
    kc::g_test_world = 0;
    Answer a = define(5, def(1), KILN_DEFINE_REQUIRE_REGISTERED);
    check(a.rc == KILN_OK && !a.r.ok && a.r.reason_code == KILN_DEFINE_NO_WORLD && a.r.slots_consumed == 0 && !why(a).empty(),
          "RequireRegistered with no world: refused NoWorld, nothing taken: " + why(a));
    check(!fs::exists(kc::data_dir() + "/gamedata/pending"), "and no file written");
    check(g_defs.empty(), "and no definition kept");
    a = define(5, with(def(10), "description", std::string(70000, 'x')), KILN_DEFINE_ALLOW_NEXT_START);
    check(!a.r.ok && a.r.reason_code == KILN_DEFINE_STORAGE_FAILURE,
          "a pending definition over the replay limit is refused before accepting it");
    a = define(5, def(2), KILN_DEFINE_ALLOW_NEXT_START);
    check(a.rc == KILN_OK && a.r.ok && a.r.disposition == KILN_DEFINITION_ACCEPTED_NEXT_START && a.r.slots_consumed == 0,
          "AllowNextStart with no world: accepted for the next start, no slot");
    size_t files = 0;
    for (auto it = fs::recursive_directory_iterator(kc::data_dir(), ec); !ec && it != fs::recursive_directory_iterator(); ++it)
        files += it->is_regular_file();
    check(files == 1, "one file written (" + std::to_string(files) + ")");
    uint8_t stored[16];
    std::memcpy(stored, a.r.registration_id, 16);
    check(receipt(stored).r.disposition == KILN_DEFINITION_ACCEPTED_NEXT_START, "its receipt says so");
    check(define(5, def(12), KILN_DEFINE_ALLOW_NEXT_START).r.ok, "store a second boot request to test current-mod precedence");

    kc::g_test_world = 0x7000;
    clones_world_ready(0x7000, KILN_WORLD_SERVER);
    b = budget();
    check(b.readiness == KILN_CAPACITY_VALID && b.capacity == 4096 && b.game_used == 0 && b.kiln_used == 0 && b.reserved == 0 &&
              (b.flags & KILN_CAPACITY_EXPANDED),
          "the world's load reserved the room: Capacity=4096 GameUsed=0 KilnUsed=0 before the first clone, Expanded");
    const uint64_t rev0 = b.revision;
    a = define(5, def(3), KILN_DEFINE_REQUIRE_REGISTERED);
    check(a.rc == KILN_OK && a.r.ok && a.r.disposition == KILN_DEFINITION_REGISTERED && a.r.slots_consumed == 1 && a.r.reason_code == 0 &&
              a.report["reason"].is_null(),
          "RequireRegistered in a world: Registered, SlotsConsumed 1");
    g_get_buff = [](uint32_t id, uintptr_t reg) -> uintptr_t {
        uintptr_t header = 0; std::memcpy(&header, reinterpret_cast<void*>(reg + 0x20), 8);
        return game_lookup(header, id, kStride);
    };
    auto& looked = g_defs.front();
    looked.probed.clear(); probe_buff(looked, 0x7000);
    check(clones_status()["buffLookups"][kc::guid_text(looked.guid)]["server"] == true,
          "boundary proof names the GUID whose ordinary game lookup succeeded");
    g_get_buff = nullptr;
    uint8_t first[16];
    std::memcpy(first, a.r.registration_id, 16);
    b = budget();
    check(b.kiln_used == 1 && b.revision > rev0 && a.r.snapshot_revision == b.revision, "KilnUsed=1, the answer carries the budget's revision");
    Answer again = define(5, def(3), KILN_DEFINE_REQUIRE_REGISTERED);
    check(again.r.ok && again.r.slots_consumed == 1 && std::memcmp(again.r.registration_id, first, 16) == 0 && budget().kiln_used == 1 &&
              budget().revision == b.revision,
          "the same definition again: the same receipt, SlotsConsumed 1, nothing charged, the revision unchanged");
    a = define(5, with(def(3), "fields", {{"defaultLifeTime", 9}}), KILN_DEFINE_REQUIRE_REGISTERED);
    check(a.r.ok && std::memcmp(a.r.registration_id, first, 16) != 0 && budget().kiln_used == 1, "new content: a new receipt, the same record");
    Answer old = receipt(first);
    check(!old.r.ok && old.r.reason_code == KILN_DEFINE_SUPERSEDED, "the first receipt now says Superseded");
    a = define(5, kc::gjson{{"guid", guid_of(4)}, {"name", "Result_3"}, {"base", base}}, KILN_DEFINE_ALLOW_NEXT_START);
    check(!a.r.ok && a.r.reason_code == KILN_DEFINE_NAME_TAKEN && a.r.slots_consumed == 0, "a taken name: NameTaken, even with AllowNextStart");
    a = define(5, kc::gjson{{"guid", guid_of(5)}, {"name", "Result_5"}, {"base", "no_such_buff"}}, KILN_DEFINE_REQUIRE_REGISTERED);
    check(!a.r.ok && a.r.reason_code == KILN_DEFINE_UNKNOWN_BASE, "an unknown base: UnknownBase");
    a = define(5, kc::gjson{{"guid", guid_of(6)}, {"name", 7}, {"base", base}}, KILN_DEFINE_REQUIRE_REGISTERED);
    check(a.rc == KILN_ERR_INVALID_ARGUMENT && !why(a).empty(), "a malformed definition is a bad argument: " + why(a));
    a = define(5, def(6), 9);
    check(a.rc == KILN_ERR_INVALID_ARGUMENT, "a bad timing is a bad argument");
    a = define(5, with(def(6), "placeholder", {{"mod", "x"}}), KILN_DEFINE_REQUIRE_REGISTERED);
    check(a.rc == KILN_ERR_INVALID_ARGUMENT, "a placeholder is not a mod's definition");
    uint8_t unknown[16] = {9, 9, 9};
    a = receipt(unknown);
    check(a.rc == KILN_OK && !a.r.ok && a.r.reason_code == KILN_DEFINE_NOT_FOUND, "an unknown receipt: NotFound");
    uint8_t zero[16] = {};
    check(receipt(zero).rc == KILN_ERR_INVALID_ARGUMENT, "the empty receipt id is a bad argument");

    // the owner goes: its record stays, reserved; its receipt says OwnerInactive
    uint8_t latest[16];
    std::memcpy(latest, define(5, with(def(3), "fields", {{"defaultLifeTime", 9}}), KILN_DEFINE_REQUIRE_REGISTERED).r.registration_id, 16);
    clones_owner_revoked(5);
    b = budget();
    a = receipt(latest);
    check(b.kiln_used == 0 && b.reserved == 1 && !a.r.ok && a.r.disposition == KILN_DEFINITION_RETAINED && a.r.slots_consumed == 1 &&
              a.r.reason_code == KILN_DEFINE_OWNER_INACTIVE,
          "a revoked owner: KilnUsed 0, Reserved 1, its receipt Retained/OwnerInactive with its record");
    a = define(5, def(7), KILN_DEFINE_REQUIRE_REGISTERED);
    check(!a.r.ok && a.r.reason_code == KILN_DEFINE_OWNER_INACTIVE, "a revoked owner defines nothing");
    a = define(6, with(def(3), "fields", {{"defaultLifeTime", 9}}), KILN_DEFINE_REQUIRE_REGISTERED);
    check(a.r.ok && budget().kiln_used == 1 && budget().reserved == 0, "a loaded mod's definition of it takes the record back");

    // kiln.hooks says the owner is retiring (a mod unloads): its callback only queues; the next publication reserves the record
    on_owner_retiring(6, nullptr);
    check(budget().kiln_used == 1, "the retiring callback changes nothing by itself (it must not wait for the clone lock)");
    {
        std::lock_guard l(g_mu);
        publish_budget();
    }
    check(budget().kiln_used == 0 && budget().reserved == 1, "the next publication: the retired owner's record is reserved");
    check(define(7, with(def(3), "fields", {{"defaultLifeTime", 9}}), KILN_DEFINE_REQUIRE_REGISTERED).r.ok && budget().kiln_used == 1,
          "the reloaded mod's definition takes it back");

    const uint32_t used_before_retire = BM.used;
    on_owner_retiring(8, nullptr);
    a = define(8, def(9), KILN_DEFINE_REQUIRE_REGISTERED);
    check(!a.r.ok && a.r.reason_code == KILN_DEFINE_OWNER_INACTIVE && BM.used == used_before_retire,
          "retiring notice before Define: refuse without spending an immortal record");

    // full: CapacityExceeded before anything is taken, also for AllowNextStart
    BM.used = BM.room;
    a = define(7, def(8), KILN_DEFINE_ALLOW_NEXT_START);
    check(!a.r.ok && a.r.reason_code == KILN_DEFINE_CAPACITY_EXCEEDED && a.r.slots_consumed == 0, "full: CapacityExceeded, also with AllowNextStart");
    b = budget();
    check(b.readiness == KILN_CAPACITY_ACCOUNTING_MISMATCH || b.readiness == KILN_CAPACITY_VALID, "the budget stays a valid reply");
    std::string big(9000, 'x');
    check(report_json(big, std::vector<std::string>(40, big)).size() < kReportMax, "a report never exceeds 4096 bytes");

    // the next start: the stored definition is defined before mods, registers with the world's load, its file goes
    make_game();
    g_defs.clear();
    {
        std::lock_guard rl(g_rc_mu);
        g_receipts.clear();
        g_receipt_age.clear();
    }
    g_gone.clear();
    g_budget = KilnCapacityInfo{};
    make_world();
    g_fake_api.owner_create = [](KilnStr) -> KilnOwner { return 77; };
    kc::g_test_world = 0;
    const auto malformed = u8path(pending_dir()) / "test" / "malformed.json";
    fs::create_directories(malformed.parent_path());
    { std::ofstream out(malformed); out << R"({"format":"wrong"})"; }
    pending_replay();
    check(g_defs.empty(), "pending replay waits for the reliable loaded-mod filter");
    a = define(5, def(11), KILN_DEFINE_ALLOW_NEXT_START);
    check(a.r.ok, "a new OnLoad request is accepted for the next process start");
    const auto fresh_file = u8path(pending_file("test", a.r.definition_guid));
    g_defs.clear(); g_receipts.clear(); g_pending_owners.clear();
    g_pending_filter_known = true;
    g_pending_mods = {"another-mod"};
    pending_replay();
    check(g_defs.empty(), "disabled mod's pending file is not applied");
    std::string current_report;
    clones_define(77, KILN_GD_BUFFS, with(def(12), "description", "current OnLoad definition").dump(), false, "mod OnLoad", &current_report);
    g_pending_mods = {"test"};
    bool replay_threw = false;
    try { pending_replay(); } catch (const std::exception&) { replay_threw = true; }
    check(!replay_threw && fs::exists(malformed), "malformed pending field types are refused without throwing and left in place");
    auto resumed = std::find_if(g_defs.begin(), g_defs.end(), [](const Def& d) { return d.name == "Result_2"; });
    check(g_defs.size() == 2 && resumed != g_defs.end() && resumed->state != "registered", "the next start defines the stored clone before the world");
    auto current = std::find_if(g_defs.begin(), g_defs.end(), [](const Def& d) { return d.name == "Result_12"; });
    check(current != g_defs.end() && current->body.value("description", "") == "current OnLoad definition",
          "deferred replay never overwrites this start's newer OnLoad definition");
    check(receipt(stored).r.disposition == KILN_DEFINITION_ACCEPTED_NEXT_START, "its receipt: still AcceptedNextStart before the world");
    pending_owner_loaded(90, "test");
    check(resumed->owner == 90 && g_receipts.at(resumed->reg_id).owner == 90, "pending stand-in handed to real mod owner");
    kc::g_test_world = 0x7000;
    clones_world_ready(0x7000, KILN_WORLD_SERVER);
    a = receipt(stored);
    check(a.r.ok && a.r.disposition == KILN_DEFINITION_REGISTERED && a.r.slots_consumed == 1, "after the world's load: Registered");
    files = 0;
    for (auto it = fs::recursive_directory_iterator(kc::data_dir(), ec); !ec && it != fs::recursive_directory_iterator(); ++it)
        files += it->is_regular_file();
    fs::remove(malformed);
    check(files == 3 && fs::exists(fresh_file), "the refused file and new next-start request remain (" + std::to_string(files) + " left)");
    on_owner_retiring(90, nullptr);
    pending_owner_loaded(91, "test");
    check(budget().kiln_used == 0 && budget().reserved == 2, "real owner's retirement reserves replayed clone");
    pending_owner_loaded(91, "test");
    check(budget().kiln_used == 0 && budget().reserved == 2,
          "a later loaded owner does not resurrect retired clones without defining them again");
    fs::remove_all(kc::data_dir(), ec);
}

int main() {
    try {
        test_room();
        test_sizes();
        test_slots_and_map();
        test_batches();
        test_results();
    } catch (const std::exception& e) {
        std::printf("FAIL threw: %s\n", e.what());
        ++failures;
    }
    std::printf("buffroom-test: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
