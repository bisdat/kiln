// SPDX-License-Identifier: MIT
// #108 on the host: the actual program code (add_program, retry_waiting, refresh_programs, check_programs_ready) and
// the server world's start/stop (on_world) across world changes, against a fake ImpactRegistry. A world change keeps
// Kiln's programs and loses the game's registry, which may come back at the same address with fresh content (the
// report: 00000300BBD4C6D0 both times) or at another one; a stop may also never be seen (no world.lifecycle hook).
// After each change ApplyBuff's and RemoveBuff's programs (as EnsureBuffProgram asks: Info, then Register), a mod's
// program and kiln.assets' program must be in the live registry, and registering any of them again must not fail.
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "engine.h"
#include "kiln_api.h"
#include "services/kiln_abilities.h"
#include "services/kiln_assets.h"
#include "verify.h"
using namespace kab;

static int g_failed = 0, g_passed = 0;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (c) ++g_passed;                                               \
        else {                                                           \
            ++g_failed;                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);     \
        }                                                                \
    } while (0)

// ---- the log: refusals are what the report saw ----
std::mutex g_log_mu;
std::vector<std::string> g_warnings;
static void logv(bool warn, const char* fmt, va_list ap) {
    char b[512];
    std::vsnprintf(b, sizeof b, fmt, ap);
    std::lock_guard l(g_log_mu);
    if (warn) g_warnings.emplace_back(b);
    if (std::getenv("PROGRAMS_TEST_LOG")) std::printf("%s %s\n", warn ? "[W]" : "[I]", b);
}
void logw(const char* fmt, ...) { va_list ap; va_start(ap, fmt); logv(true, fmt, ap); va_end(ap); }
void logi(const char* fmt, ...) { va_list ap; va_start(ap, fmt); logv(false, fmt, ap); va_end(ap); }
static size_t warnings_with(const char* s) {
    std::lock_guard l(g_log_mu);
    size_t n = 0;
    for (auto& w : g_warnings) n += w.find(s) != std::string::npos;
    return n;
}

// ---- Windows memory (kept reachable: Kiln never frees a program copy) ----
#define MEM_COMMIT 0x1000
#define MEM_RESERVE 0x2000
#define MEM_RELEASE 0x8000
#define PAGE_READWRITE 0x04
std::mutex g_alloc_mu;
std::vector<void*>* g_allocs = new std::vector<void*>;  // never destroyed: the copies stay reachable at exit
void* VirtualAlloc(void*, size_t n, int, int) {
    void* p = std::calloc(1, n);
    std::lock_guard l(g_alloc_mu);
    g_allocs->push_back(p);
    return p;
}
int VirtualFree(void* p, size_t, int) {
    std::lock_guard l(g_alloc_mu);
    for (auto& a : *g_allocs)
        if (a == p) a = nullptr;
    std::free(p);
    return 1;
}

// ---- the fake ImpactRegistry (the game's map: id -> program) ----
struct FakeReg {
    std::unordered_map<uint32_t, const void*> map;
    uint32_t count = 0;
    void fresh();  // what the game fills while a world loads: its own programs only
};
std::mutex g_reg_mu;  // the fake's own (a real registry is only touched on the tick thread)
const uint8_t kVanillaBlob[64] = {};
constexpr uint32_t kVanillaA = 0x0badf00du, kVanillaB = 0x0badf11du;
void FakeReg::fresh() {
    std::lock_guard l(g_reg_mu);
    map.clear();
    map[kVanillaA] = kVanillaBlob;
    map[kVanillaB] = kVanillaBlob;
    count = static_cast<uint32_t>(map.size());
}
namespace kab {
Engine E;
bool read_bytes(const void* p, void* out, size_t n) {
    std::memcpy(out, p, n);
    return true;
}
const uint8_t* get_program(uintptr_t reg, uint32_t id) {
    if (!reg) return nullptr;
    auto* r = reinterpret_cast<FakeReg*>(reg);
    std::lock_guard l(g_reg_mu);
    auto it = r->map.find(id);
    return it == r->map.end() ? nullptr : static_cast<const uint8_t*>(it->second);
}
bool program_map(uintptr_t reg, MapView* m) {
    if (!reg) return false;
    m->count = &reinterpret_cast<FakeReg*>(reg)->count;
    return true;
}
bool program_map_insert(uintptr_t reg, uint32_t id, const void* program, bool* existed) {
    auto* r = reinterpret_cast<FakeReg*>(reg);
    std::lock_guard l(g_reg_mu);
    *existed = r->map.count(id) != 0;
    if (!*existed) r->map[id] = program;
    r->count = static_cast<uint32_t>(r->map.size());
    return true;
}
}  // namespace kab

// ---- what the extracted code uses from the rest of abilities.cpp ----
std::mutex g_mu, g_ev_mu;
std::atomic<uint64_t> g_step{1}, c_refused{0};
std::atomic<uintptr_t> g_system{0}, g_prog_registry{0}, g_native_registry{0};
std::atomic<bool> g_programs_added{false};
bool g_have_programs = true, g_have_natives = false;
uintptr_t g_cap_registry = 0, g_setup_world = 0, g_setup_registry = 0;
int argc_of(uint32_t) { return -1; }
TypedArgs typed_of(uint32_t) { return {}; }
std::string guid_text(const uint8_t* b) {
    char s[40];
    std::snprintf(s, sizeof s, "%02x%02x%02x%02x-...", b[3], b[2], b[1], b[0]);
    return s;
}
void selftest_programs(uintptr_t) {}
void check_attachments(uintptr_t) {}
const KilnAssetsApi* g_assets = nullptr;
// on_world's
std::vector<KilnAbilityEvent> g_events, g_batch;
std::atomic<uint32_t> g_inflight{0}, g_ecs_state{0}, g_ecs_reason{0};
std::atomic<void*> g_ecs{nullptr};
struct Slot { uintptr_t registered_in = 0; };
std::vector<Slot> g_slots(4);
std::unordered_set<uint32_t> g_held;
std::unordered_map<uint32_t, uint64_t> g_canceled;
std::unordered_map<uint32_t, int> g_impact_ovr, g_entity_ovr;
struct Tracked { uint32_t program, self; };
std::unordered_map<uint32_t, Tracked> g_tracked;
struct Call { int state = 0; };
std::unordered_map<uint64_t, Call> g_calls;
std::unordered_map<uint64_t, uint64_t> g_call_key;
constexpr int64_t kRetireWaitNs = 2'000'000;
int64_t now_ns() { return 0; }
void SwitchToThread() { std::this_thread::yield(); }
void holds_clear() {}
void recount_pending() {}

#include "programs_under_test.h"

// ---- programs (tools/impact's layout; the verifier is the real one) ----
static std::vector<uint8_t> program(const char* tag, uint32_t marker = 0) {
    uint8_t guid[16] = {};
    std::memcpy(guid, tag, std::min<size_t>(16, std::strlen(tag)));
    const size_t codeOff = 64, shutOff = codeOff + 8, layOff = shutOff + 4;
    std::vector<uint8_t> b(layOff + 24 + 4);
    const uint32_t id = fnv1a(guid, 16);
    std::memcpy(b.data(), &id, 4);
    std::memcpy(b.data() + 4, guid, 16);
    const uint16_t stack = 256;
    std::memcpy(b.data() + 0x14, &stack, 2);
    auto arr = [&](size_t field, size_t target, uint32_t count) {
        const int32_t off = count ? static_cast<int32_t>(target - field) : 0;
        std::memcpy(b.data() + field, &off, 4);
        std::memcpy(b.data() + field + 4, &count, 4);
    };
    const uint32_t halt = 30;  // code and shutdown: HALT (marker changes the data, not the code)
    arr(0x18, codeOff, 1);
    arr(0x20, shutOff, 1);
    arr(0x28, layOff, 1);
    arr(0x30, layOff + 24, 4);
    std::memcpy(b.data() + codeOff, &halt, 4);
    std::memcpy(b.data() + shutOff, &halt, 4);
    ImpactVariable v{};
    v.type = 0x737c8f86u;
    v.size = 4;
    std::memcpy(b.data() + layOff, &v, 24);
    std::memcpy(b.data() + layOff + 24, &marker, 4);  // the variable's initial value
    return b;
}
static uint32_t id_of(const std::vector<uint8_t>& b) { return fnv1a(b.data() + 4, 16); }

const auto kBuff = program("kiln.applyBuff"), kRemove = program("kiln.removeBuff"), kMod = program("mod.Program"),
           kAsset = program("assets.Program");

// kiln.assets: one ImpactProgram resource (a mod's assets.json, mode "new")
KilnResult KILN_CALL fake_list(uint32_t type, KilnAssetKey* out, size_t cap, size_t* n) {
    *n = type == kTypeImpactProgram ? 1 : 0;
    if (*n && out && cap) {
        *out = KilnAssetKey{};
        std::memcpy(out->guid, kAsset.data() + 4, 16);
        out->type = type;
    }
    return KILN_OK;
}
KilnResult KILN_CALL fake_read(const KilnAssetKey*, void* buf, size_t cap, size_t* len) {
    *len = kAsset.size();
    if (!buf || cap < kAsset.size()) return KILN_ERR_BUFFER_TOO_SMALL;
    std::memcpy(buf, kAsset.data(), kAsset.size());
    return KILN_OK;
}

// What Kiln.Abilities' EnsureBuffProgram / EnsureRemoveProgram do: Programs.Info (the live registry), then
// Programs.RegisterCore when it has nothing (OK + an error text = waiting).
static KilnResult ensure(const std::vector<uint8_t>& blob, std::string* why = nullptr) {
    if (get_program(g_prog_registry.load(), id_of(blob))) return KILN_OK;
    std::string err;
    bool waiting = false;
    uint32_t id = 0;
    const KilnResult rc = add_program(blob.data(), blob.size(), 0, "ApplyBuff", &id, err, &waiting);
    if (why) *why = err;
    if (rc == KILN_OK && waiting) return KILN_ERR_UNAVAILABLE;
    return rc;
}
static KilnResult mod_register(const std::vector<uint8_t>& blob, bool* waiting) {
    std::string err;
    uint32_t id = 0;
    return add_program(blob.data(), blob.size(), 7, "a mod (program_register)", &id, err, waiting);
}

uint32_t g_session = 0;
// The server world's lifecycle as kiln.abilities sees it: STARTED, world_setup's part (the registry, readiness reset),
// the game filling its registry, and the Pre that finds it ready (check_programs_ready: refresh_programs).
static void start_world(FakeReg& reg, uintptr_t world, bool fill = true) {
    KilnWorldEvent ev{sizeof(KilnWorldEvent), KILN_WORLD_STARTED, KILN_WORLD_SERVER, ++g_session, world};
    on_world(&ev, nullptr);
    reg.fresh();
    g_prog_registry.store(reinterpret_cast<uintptr_t>(&reg));
    g_setup_world = world;
    g_programs_ready.store(false);
    g_last_count = 0;
    if (fill) check_programs_ready();
}
static void stop_world(uintptr_t world) {
    KilnWorldEvent ev{sizeof(KilnWorldEvent), KILN_WORLD_STOPPING, KILN_WORLD_SERVER, g_session, world};
    on_world(&ev, nullptr);
}
static bool live(const std::vector<uint8_t>& b) {
    const uint8_t* p = get_program(g_prog_registry.load(), id_of(b));
    return p && std::memcmp(p, b.data(), b.size()) == 0;
}
static size_t kiln_programs() {
    std::lock_guard l(g_mu);
    return g_programs.size();
}

// After a world change: every program back, ApplyBuff/RemoveBuff/a mod's registration again succeed, nothing refused.
static void after_change(const char* what) {
    const size_t refused = warnings_with("refused");
    CHECK(g_programs_ready.load());
    CHECK(live(kBuff) && live(kRemove) && live(kMod) && live(kAsset));  // refresh_programs put them back
    CHECK(ensure(kBuff) == KILN_OK && ensure(kRemove) == KILN_OK);
    bool waiting = true;
    CHECK(mod_register(kMod, &waiting) == KILN_OK && !waiting);  // a mod that registers at every world start
    std::string err;
    uint32_t id = 0;
    CHECK(add_program(kBuff.data(), kBuff.size(), 0, "again", &id, err, &waiting) == KILN_OK && !waiting && err.empty());
    CHECK(kiln_programs() == 4);
    CHECK(warnings_with("refused") == refused && warnings_with("registered already") == 0);
    std::printf("programs_test: %s: the four programs are in the registry, registering again succeeds\n", what);
}

int main() {
    KilnAssetsApi assets{};
    assets.resource_list = &fake_list;
    assets.resource_read = &fake_read;
    g_assets = &assets;
    g_vanilla_programs = {kVanillaA, kVanillaB};
    for (auto* b : {&kBuff, &kRemove, &kMod, &kAsset}) {
        const VerifyResult r = verify_program(b->data(), b->size(), [](uint32_t) { return -1; });
        CHECK(r.ok && r.unknown_natives.empty());
        if (!r.ok) std::printf("verifier: %s\n", r.error.c_str());
    }

    static FakeReg first, other;
    // World A: kiln.assets' program goes in at readiness; ApplyBuff, RemoveBuff and a mod register theirs.
    start_world(first, 0xA000);
    CHECK(live(kAsset));
    CHECK(ensure(kBuff) == KILN_OK && ensure(kRemove) == KILN_OK);
    bool waiting = true;
    CHECK(mod_register(kMod, &waiting) == KILN_OK && !waiting);
    CHECK(live(kBuff) && live(kRemove) && live(kMod) && kiln_programs() == 4);
    // the same bytes in the same world: a no-op; other bytes under a live GUID: refused
    {
        const auto changed = program("mod.Program", 5);
        CHECK(id_of(changed) == id_of(kMod));
        CHECK(mod_register(kMod, &waiting) == KILN_OK && !waiting);
        CHECK(mod_register(changed, &waiting) == KILN_ERR_INVALID_ARGUMENT && live(kMod));
    }
    // a game program's GUID stays refused
    {
        const auto vanilla = program("game.Program");
        {
            std::lock_guard l(g_reg_mu);
            first.map[id_of(vanilla)] = kVanillaBlob;
            first.count = static_cast<uint32_t>(first.map.size());
        }
        std::string err;
        uint32_t id = 0;
        CHECK(add_program(vanilla.data(), vanilla.size(), 0, "a mod", &id, err, &waiting) == KILN_ERR_INVALID_ARGUMENT);
        CHECK(err.find("program the game has") != std::string::npos);
    }

    // The report: World B's registry at World A's address, with fresh content.
    stop_world(0xA000);
    {
        std::lock_guard l(g_mu);
        for (auto& p : g_programs) CHECK(p.inserted_in == 0);  // the stop forgets where they were
    }
    CHECK(g_prog_registry.load() == 0);
    start_world(first, 0xB000);
    after_change("world B, same registry address");

    // World C at another address.
    stop_world(0xB000);
    start_world(other, 0xC000);
    CHECK(get_program(reinterpret_cast<uintptr_t>(&first), id_of(kBuff)) != nullptr);  // (the old map is not touched)
    after_change("world C, another registry address");

    // World D at C's address with no stop seen (a build without the world.lifecycle hook): STARTED forgets C.
    g_setup_registry = 0x5e7;
    g_slots[0].registered_in = 0x5e7;
    start_world(other, 0xD000, false);
    CHECK(g_setup_registry == 0 && g_slots[0].registered_in == 0);  // natives go into the new registry again
    CHECK(!g_programs_ready.load());
    // before the game has loaded its registry ApplyBuff says "not loaded yet" (waiting) and queues one entry, however often
    std::string why;
    CHECK(ensure(kBuff, &why) == KILN_ERR_UNAVAILABLE && ensure(kBuff) == KILN_ERR_UNAVAILABLE);
    {
        std::lock_guard l(g_mu);
        size_t n = 0;
        for (auto& w : g_waiting) n += w.bytes.size() >= 20 && std::memcmp(w.bytes.data() + 4, kBuff.data() + 4, 16) == 0;
        CHECK(n == 1);
    }
    check_programs_ready();
    {
        std::lock_guard l(g_mu);
        CHECK(g_waiting.empty());
    }
    after_change("world D, same address, no stop seen");

    // The old bug's state by hand: inserted_in names this registry, the registry has lost them (no stop, no start).
    {
        {
            std::lock_guard l(g_mu);
            for (auto& p : g_programs) p.inserted_in = g_prog_registry.load();
        }
        other.fresh();
        CHECK(!live(kBuff));
        CHECK(ensure(kBuff) == KILN_OK && live(kBuff));  // Info finds nothing: Register puts it back, no throw
        CHECK(!live(kRemove));
        uint32_t added = 0;
        added = refresh_programs();
        CHECK(added == 0);  // kiln.assets' program was put back by the first loop, not registered anew
        CHECK(live(kRemove) && live(kMod) && live(kAsset));
        CHECK(warnings_with("registered already") == 0);
        std::printf("programs_test: a stale inserted_in is not trusted (ensure and refresh both put the programs back)\n");
    }

    // A new world, and the mod brings other bytes under its GUID: they go in (no instance runs the old copy).
    stop_world(0xD000);
    start_world(first, 0xE000);
    {
        const auto changed = program("mod.Program", 9);
        CHECK(live(kMod));  // refresh put the old bytes back first
        CHECK(mod_register(changed, &waiting) == KILN_ERR_INVALID_ARGUMENT);  // live now: refused as in any world
        stop_world(0xE000);
        start_world(other, 0xF000, false);
        CHECK(mod_register(changed, &waiting) == KILN_OK && waiting);  // the registry is loading: it waits
        check_programs_ready();  // the queued new bytes go in first, then the other programs come back
        CHECK(live(changed) && live(kBuff) && live(kRemove) && live(kAsset));
        CHECK(warnings_with("registered already") == 0);
        stop_world(0xF000);
        start_world(first, 0x10000, false);
        {
            std::lock_guard l(g_mu);
            for (auto& p : g_programs) CHECK(get_program(g_prog_registry.load(), p.id) == nullptr);
        }
        g_programs_ready.store(true);  // ready, but no refresh yet: the mod's registration comes first
        CHECK(mod_register(kMod, &waiting) == KILN_OK && !waiting && live(kMod));  // back to the first bytes
        CHECK(kiln_programs() == 4);
    }

    // Threads: the world stops on another thread (the destroying thread) while the tick thread registers and refreshes.
    {
        stop_world(0x10000);
        start_world(other, 0x11000);
        std::atomic<bool> go{true};
        std::thread destroyer([&] {
            for (int i = 0; i < 200; ++i) {
                KilnWorldEvent ev{sizeof(KilnWorldEvent), KILN_WORLD_STOPPING, KILN_WORLD_SERVER, g_session, 0x11000};
                on_world(&ev, nullptr);
                std::lock_guard l(g_mu);
                for (auto& p : g_programs) (void)p.inserted_in;
            }
            go.store(false);
        });
        int calls = 0;
        while (go.load()) {
            g_prog_registry.store(reinterpret_cast<uintptr_t>(&other));
            g_programs_ready.store(true);
            (void)ensure(kBuff);
            (void)refresh_programs();
            ++calls;
        }
        destroyer.join();
        CHECK(calls > 0);
        start_world(first, 0x12000);
        after_change("after a stop raced the tick thread");
    }

    std::printf("programs_test: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
