// SPDX-License-Identifier: MIT
// kiln.content: a kitbash reference that resolves late (a mod's resource kiln.assets serves after the clone was built) is
// written into an item clone only by the thread of a world that put that clone into its registry, and a clone the
// server world has is written on the server world's thread alone (SAUDIT-V1). On the host: newcontent.cpp's own
// registration (sync_world), client Pre (on_client_pre) and server Pre (on_server_pre) over a fake game: two worlds'
// item registries (the engine's HashMap), base ItemInfos, a fake kitbash whose model reference is "not loaded" until
// the case serves it.
//  - hosted, one clone (both worlds load the same base, as the game does): the client world's retry runs first and
//    leaves the clone alone; the server world's Pre writes it, once
//  - hosted, two clones (the client world's base moved): each world's retry writes its own clone, the client world's
//    never the server world's, and both resolve (each clone counts its own problems)
//  - a join (no server world here): the client world's retry writes its clone, as before
//  - host, leave, join: the server world's claim ends with a client world set up while no server world runs
//  - re-host: the next server session claims the clone again, and the client world's retry leaves it to the server
//  - a definition replaced: the old clone, still in the server world's registry, is never written again
//  - the server world's STOPPING (its world still intact, the destructor still to run) while the client world goes on:
//    that client session leaves the clone; the next client world, set up after the server world is gone, writes it
//  - no destroy hook: a joined client world leaves the last server world's clone until a newer server session begins
//  - the server world registers between the client registration's session snapshot and its claims: its claim stays
//  - the worlds resolve a reference differently (a bare GUID only a client-world item uses): the server world's clone
//    is only ever the server world's to write
// With KITBASH_TSAN (lateref_test.sh's second build, ThreadSanitizer): a reader thread copies the server world's clone while the client
// world's retry runs, as the GameServer thread reads its items while the main thread steps the client world.
// Each case runs in a child process: definitions and clones live as long as the process, as in the game.
#include <windows.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>

// What newcontent.cpp takes from <windows.h> and <intrin.h> beyond host/windows.h (as itemfind_test.cpp).
inline unsigned long long GetTickCount64() { return 1000; }
inline unsigned long GetExceptionCode() { return 0; }
#define EXCEPTION_EXECUTE_HANDLER 1
#define __except(x) catch (...)
#define __declspec(x)
inline long long _InterlockedOr64(volatile long long* p, long long v) {
    const long long old = *p;
    *p = old | v;
    return old;
}

#include "../newcontent.cpp"

// ============================================================================================= the fake game =====
namespace fake {
constexpr uint32_t kItemSize = 0x200;  // keen::ItemInfo here: id +0, GUID +4, debugName (BlobString) +0x1f0
constexpr uint32_t kDbg = 0x1f0;
constexpr uint32_t kModel = 0x100;   // equipment.visualModel: an ObjectReference<keen::Model> (16 bytes)
constexpr uint32_t kModels = 0x120;  // equipment.visualModels: a BlobArray header (8 bytes)

// Kiln's near memory and every fake game object in one arena: a clone's self-relative blob offsets reach them all.
alignas(16) uint8_t arena[1 << 20];
size_t used = 0;
void* alloc(size_t n) {
    used = (used + 15) & ~size_t{15};
    if (used + n > sizeof arena) return nullptr;
    void* p = arena + used;
    used += n;
    return p;
}

struct Registry {  // a world's g38_items::ItemRegistry: loading +0, started +1, the id map at +0x100
    uint8_t* obj = nullptr;
    uint64_t* bits = nullptr;
    uint32_t* keys = nullptr;
    uint64_t* values = nullptr;
    static constexpr uint64_t kCap = 64;
    uintptr_t addr() const { return reinterpret_cast<uintptr_t>(obj); }
    void make() {
        obj = static_cast<uint8_t*>(alloc(0x200));
        bits = static_cast<uint64_t*>(alloc(8));
        keys = static_cast<uint32_t*>(alloc(4 * kCap));
        values = static_cast<uint64_t*>(alloc(8 * kCap));
        obj[0] = 0, obj[1] = 1;
        uint8_t* m = obj + 0x100;
        const uintptr_t b = reinterpret_cast<uintptr_t>(bits), k = reinterpret_cast<uintptr_t>(keys), v = reinterpret_cast<uintptr_t>(values);
        const uint64_t cap = kCap;
        std::memcpy(m + kc::E.hm_bits, &b, 8);
        std::memcpy(m + kc::E.hm_cap, &cap, 8);
        std::memcpy(m + kc::E.hm_keys, &k, 8);
        std::memcpy(m + kc::E.hm_values, &v, 8);
    }
    void put(uint32_t key, uintptr_t value) {  // the game's own insert at load (linear probing, as map_put reads it)
        uint64_t i = key & (kCap - 1);
        while ((bits[i >> 6] >> (i & 63)) & 1) i = (i + 1) & (kCap - 1);
        keys[i] = key, values[i] = value, bits[i >> 6] |= 1ull << (i & 63);
        uint32_t count = 0;
        std::memcpy(&count, obj + 0x100 + kc::E.hm_count, 4);
        ++count;
        std::memcpy(obj + 0x100 + kc::E.hm_count, &count, 4);
    }
    uintptr_t get(uint32_t key) const {
        for (uint64_t i = key & (kCap - 1), n = 0; n < kCap && ((bits[i >> 6] >> (i & 63)) & 1); ++n, i = (i + 1) & (kCap - 1))
            if (keys[i] == key) return values[i];
        return 0;
    }
};

// A base ItemInfo of the game: id, GUID, debug name.
uintptr_t make_item(uint32_t id, uint8_t guid_byte, const char* name) {
    auto* p = static_cast<uint8_t*>(alloc(kItemSize + 64));
    std::memcpy(p, &id, 4);
    std::memset(p + 4, guid_byte, 16);
    std::memset(p + kModel, 0xBA, 16);  // the base's own model
    const size_t n = std::strlen(name);
    std::memcpy(p + kItemSize, name, n);
    const int32_t off = static_cast<int32_t>(kItemSize - kDbg);
    const uint32_t len = static_cast<uint32_t>(n);
    std::memcpy(p + kDbg, &off, 4);
    std::memcpy(p + kDbg + 4, &len, 4);
    return reinterpret_cast<uintptr_t>(p);
}

constexpr uint32_t kBaseId = 0x0BA5E001;
const char* const kBase = "Weapon_Base_Sword";
const char* const kModelGuid = "3f2504e0-4f89-41d3-9a0c-0305e82c3301";  // the mod's model, served late
uint8_t model_guid[16];
uint8_t* model_list = nullptr;  // the per-body model list the late reference points the clone's header at

bool model_served = false;  // kiln.assets serves the mod's model now
// As refs.cpp validates a bare GUID: the model resolves where a loaded item of that world uses it (the pools come from
// the world's items), else "... is not a Model the game has loaded ...", which is retried. The two worlds of one process
// differ only where an item registered in one of them alone.
bool model_from_item = false;
const char* const kSource = "Mod_Model_Source";
bool lifecycle = true;  // the core sees world stops (its destroy hook resolved)
std::function<void()> before_client_registry;  // runs once inside the client world's registration, after its session snapshot
uintptr_t server_world = 0, client_world = 0;
uint32_t server_session = 0, client_session = 0;
Registry server_reg, client_reg;
std::vector<std::string> lines;  // kiln.log

KilnAssetsApi assets = [] {
    KilnAssetsApi a{};
    a.header.size = sizeof a;
    return a;
}();
const void* KILN_CALL service(KilnStr name, uint32_t) {
    return std::string(name.ptr, name.len) == KILN_ASSETS_SERVICE ? &assets : nullptr;
}
uint32_t KILN_CALL side() { return KILN_SIDE_CLIENT; }
uintptr_t KILN_CALL world(uint32_t kind) { return kind == KILN_WORLD_SERVER ? server_world : kind == KILN_WORLD_CLIENT ? client_world : 0; }
uintptr_t KILN_CALL server_world_fn() { return server_world; }
bool KILN_CALL on_world_thread(uint32_t) { return false; }
KilnResult KILN_CALL session(KilnSessionInfo* s) {
    const uint32_t size = s->size;
    std::memset(s, 0, size);
    s->size = size;
    s->side = KILN_SIDE_CLIENT;
    s->worlds[KILN_WORLD_SERVER] = server_world;
    s->worlds[KILN_WORLD_CLIENT] = client_world;
    s->sessions[KILN_WORLD_SERVER] = server_session;
    s->sessions[KILN_WORLD_CLIENT] = client_session;
    s->lifecycle = lifecycle ? 1 : 0;
    return KILN_OK;
}
uintptr_t KILN_CALL singleton(uintptr_t w, KilnStr name) {
    if (std::string(name.ptr, name.len) != "g38_items::ItemRegistry") return 0;
    if (w && w == client_world && before_client_registry) {
        const auto run = std::move(before_client_registry);
        before_client_registry = nullptr;
        run();  // the server world's thread, between the client registration's session snapshot and its claims
    }
    return w && w == server_world ? server_reg.addr() : w && w == client_world ? client_reg.addr() : 0;
}
KilnResult KILN_CALL post_world(KilnOwner, uint32_t, KilnTaskFn, void*) { return KILN_OK; }

KilnApi api = [] {
    KilnApi a{};
    a.size = sizeof a;
    a.service = &service;
    a.side = &side;
    a.world = &world;
    a.server_world = &server_world_fn;
    a.on_world_thread = &on_world_thread;
    a.session = &session;
    a.singleton = &singleton;
    a.post_world = &post_world;
    return a;
}();

size_t logged(const std::string& part) {
    size_t n = 0;
    for (const auto& l : lines) n += l.find(part) != std::string::npos;
    return n;
}
}  // namespace fake

// ============================================================================================= the service's other files
namespace kc {
const KilnApi* g_api = &fake::api;
KilnOwner g_owner = 1;
Engine E;
void logf(int, const char* fmt, ...) {
    char b[2048];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    fake::lines.emplace_back(b);
    if (std::getenv("LATEREF_TEST_LOG")) std::printf("  log: %s\n", b);
}
uint32_t fnv1a(const void* p, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) h = (h ^ static_cast<const uint8_t*>(p)[i]) * 16777619u;
    return h;
}
std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
bool guid_parse(const std::string& s, uint8_t out[16]) {
    std::string hex;
    for (char c : s)
        if (c != '-') hex += c;
    if (hex.size() != 32) return false;
    for (int i = 0; i < 16; ++i) out[i] = static_cast<uint8_t>(std::strtoul(hex.substr(i * 2, 2).c_str(), nullptr, 16));
    return true;
}
std::string guid_text(const uint8_t*) { return "00000000-0000-0000-0000-000000000000"; }
bool rd(uintptr_t p, void* out, size_t n) {
    if (!p) return false;
    std::memcpy(out, reinterpret_cast<const void*>(p), n);
    return true;
}
std::string read_blob_string(uintptr_t field) {
    int32_t off = 0;
    uint32_t len = 0;
    if (!rdv(field, off) || !rdv(field + 4, len) || !off || !len || len > 512) return {};
    return std::string(reinterpret_cast<const char*>(field + off), len);
}
bool map_read(uintptr_t map, std::vector<uint32_t>* keys, std::vector<uint64_t>* values) {
    MapView m;
    if (!map_view(map, &m)) return false;
    keys->clear(), values->clear();
    for (uint64_t i = 0; i < m.cap; ++i)
        if ((m.bits[i >> 6] >> (i & 63)) & 1) keys->push_back(m.keys[i]), values->push_back(reinterpret_cast<uint64_t*>(m.values)[i]);
    return keys->size() == m.count;
}
uint32_t map_count(uintptr_t map) {
    MapView m;
    return map_view(map, &m) ? m.count : 0;
}
void* near_alloc(uintptr_t, size_t n) { return fake::alloc(n ? n : 1); }
void StoreMutex::lock() { m_.lock(); }
void StoreMutex::unlock() { m_.unlock(); }
bool StoreMutex::try_lock() { return m_.try_lock(); }
StoreMutex g_store;
StoreMutex& store_mutex() { return g_store; }
PhaseTimer::PhaseTimer(Phase) {}
PhaseTimer::~PhaseTimer() {}
std::string owner_label(KilnOwner o) { return o == 1 ? "kiln" : "mod test-items"; }
std::string data_dir() { return "D:\\kiln\\data"; }
bool path_exists(const std::string&) { return false; }
const std::string& current_pack_dir() {
    static const std::string none;
    return none;
}
uintptr_t active_world() { return 0; }
uint32_t active_kind() { return KILN_WORLD_SERVER; }
DropLayout DL;
void boot_late_check() {}
void barber_sync(const std::vector<BarberItem>&) {}
// reflection: the ItemInfo / RecipeInfo layouts newcontent.cpp's layouts() asks for
bool build_schema(const char* q, const std::vector<std::string>&, Schema* out, std::string* err) {
    const std::string t = q;
    out->type = t;
    if (t == "keen::ItemInfo") {
        out->size = fake::kItemSize;
        out->special = {{"itemId", 0}, {"objectId", 4}, {"name", 0x48}, {"description", 0x68}, {"lore", 0x78}, {"debugName", fake::kDbg}};
        return true;
    }
    if (t == "keen::RecipeInfo") {
        out->size = 0xa8;
        out->special = {{"recipeGuid", 0},     {"recipeId", 0x10},          {"workshopGuid", 0x14},
                        {"workshopId", 0x24},  {"input", 0x28},             {"output", 0x30},
                        {"craftingDuration", 0x38}, {"requiredProps", 0x40}, {"requiredPropsMask", 0x48},
                        {"knowledgeRequirement", 0x50}, {"completionRequirementQuery", 0x5c}, {"recipeName", 0x68},
                        {"debugName", 0x78}};
        return true;
    }
    *err = "not in this fake reflection";
    return false;
}
int64_t field_offset(const char*, const char*) { return 0; }
uint32_t type_size(const char* q) {
    const std::string t = q;
    return t == "keen::RecipeInputResource" ? 48 : t == "keen::RecipeItemStackResource" ? 24 : t == "keen::KnowledgeQueryData" ? 12 : 4;
}
bool enum_values(const char*, std::vector<std::pair<std::string, int64_t>>*) { return false; }
bool self_relative_fields(const char*, std::vector<uint32_t>* out, std::string*) {
    out->clear();
    return true;
}
bool encode(const Field&, const KilnContentValue&, uint8_t*, std::string*) { return false; }
const Field* Schema::find(const std::string&) const { return nullptr; }
const Entry* Registry::lookup(const std::string&) const { return nullptr; }
Registry& reg(uint32_t) {
    static Registry r;
    return r;
}
void drops_apply_rules(const Entry&) {}
// refs.cpp: "model" names the mod's model, which kiln.assets serves once fake::model_served is set (the real kitbash
// says "... is not a Model the game has loaded ..." until then, which is what makes the definition retry)
const Field kModelField = [] {
    Field f;
    f.path = "equipment.visualModel";
    f.type_name = "keen::ObjectReference<keen::Model>";
    f.offset = fake::kModel;
    f.bytes = 16;
    f.ref = true;
    return f;
}();
void kitbash(const Schema&, const std::string& key, const nlohmann::ordered_json&, const ItemFind&, const std::vector<uintptr_t>& items,
             KitbashOut* out, uintptr_t) {
    if (key != "model") return;
    const bool used = std::any_of(items.begin(), items.end(), [](uintptr_t i) {
        return std::memcmp(reinterpret_cast<const uint8_t*>(i) + fake::kModel, fake::model_guid, 16) == 0;
    });
    if (fake::model_from_item ? !used : !fake::model_served) {
        out->problems.push_back(std::string("equipment.visualModel: ") + fake::kModelGuid +
                                " is not a Model the game has loaded (no loaded item uses it, and no mod serves it through kiln.assets)");
        return;
    }
    RefWrite w;
    w.f = &kModelField;
    std::memcpy(w.bytes, fake::model_guid, 16);
    out->writes.push_back(w);
    BlobWrite b;
    b.path = "equipment.visualModels";
    b.offset = fake::kModels;
    b.data = reinterpret_cast<uintptr_t>(fake::model_list);
    b.count = 3;
    out->blobs.push_back(b);
}
void set_kitbash_mod(const std::string*) {}
void set_kitbash_item(const uint8_t*) {}
size_t placeholder_clear_programs(KitbashOut*) { return 0; }  // no placeholder here
std::string near_alloc_report() { return "-"; }
uint64_t next_code_seq() { return 1; }
bool store_put(KilnOwner, uint64_t, uint32_t, uint32_t, int32_t, int32_t, uintptr_t, uint8_t, const uint8_t*, const std::string&) {
    return false;
}
bool store_drop(KilnOwner, uintptr_t) { return false; }
bool store_original(uintptr_t, uint8_t, uint8_t*) { return false; }
void store_originals(uintptr_t, size_t, uint8_t*) {}
size_t store_flush(std::string*) { return 0; }
}  // namespace kc

// ============================================================================================= the cases =========
namespace {

int failures = 0;
void check(bool ok, const std::string& what) {
    std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) ++failures;
}

constexpr const char* kGuid = "0b7d2c1e-5a44-4c1b-9e0f-1c2d3e4f5a6b";
uint32_t item_id() {
    uint8_t g[16];
    kc::guid_parse(kGuid, g);
    return kc::fnv1a(g, 16);
}

uintptr_t server_base = 0, client_base = 0;  // the base ItemInfo each world's registry has

void boot() {
    kc::N.on = true;  // content.new
    kc::E.items_map = 0x100, kc::E.items_loading = 0, kc::E.items_started = 1;
    kc::guid_parse(fake::kModelGuid, fake::model_guid);
    fake::model_list = static_cast<uint8_t*>(fake::alloc(64));
    fake::server_reg.make();
    fake::client_reg.make();
}

// The game's worlds: `shared` = both registries hold the same base ItemInfo (the hosted game's one resource memory).
void worlds(bool host, bool shared) {
    server_base = fake::make_item(fake::kBaseId, 0x5B, fake::kBase);
    client_base = shared ? server_base : fake::make_item(fake::kBaseId, 0x5B, fake::kBase);
    if (host) {
        fake::server_world = 0x40000, fake::server_session = 1;
        fake::server_reg.put(fake::kBaseId, server_base);
    }
    fake::client_world = 0x80000, fake::client_session = 1;
    fake::client_reg.put(fake::kBaseId, client_base);
}

bool define(const char* name = "Test_Sword") {
    std::string rep;
    const std::string def = std::string(R"({"guid":")") + kGuid + R"(","name":")" + name + R"(","base":")" + fake::kBase +
                            R"(","model":")" + fake::kModelGuid + R"("})";
    const bool ok = kc::nc_define(7, KILN_CONTENT_ITEMS, def, false, "test", &rep) == KILN_OK;
    if (!ok) std::printf("  define: %s\n", rep.c_str());
    return ok;
}

// Each world's Pre at a step where the late-reference retry runs (step % 50 == 7).
uint64_t server_step = 0, client_step = 0;
void server_pre() {
    server_step += 50;
    KilnTickInfo t{};
    t.size = sizeof t, t.world_kind = KILN_WORLD_SERVER, t.step = server_step + 7;
    kc::on_server_pre(&t, nullptr);
}
void client_pre() {
    client_step += 50;
    KilnTickInfo t{};
    t.size = sizeof t, t.world_kind = KILN_WORLD_CLIENT, t.step = client_step + 7;
    kc::on_client_pre(&t, nullptr);
}

// What a clone holds of the late reference: its model GUID and its per-body list header (where it points, how many).
bool resolved(uintptr_t clone) {
    if (!clone) return false;
    const auto* p = reinterpret_cast<const uint8_t*>(clone);
    int32_t off = 0;
    uint32_t n = 0;
    std::memcpy(&off, p + fake::kModels, 4);
    std::memcpy(&n, p + fake::kModels + 4, 4);
    return std::memcmp(p + fake::kModel, fake::model_guid, 16) == 0 && n == 3 &&
           clone + fake::kModels + off == reinterpret_cast<uintptr_t>(fake::model_list);
}
std::vector<uint8_t> bytes(uintptr_t clone) {
    const auto* p = reinterpret_cast<const uint8_t*>(clone);
    return std::vector<uint8_t>(p, p + fake::kItemSize);
}
void stop(uint32_t kind, uintptr_t world, uint32_t session) {
    KilnWorldEvent ev{sizeof(KilnWorldEvent), KILN_WORLD_STOPPING, kind, session, world};
    kc::on_world(&ev, nullptr);
}

// A hosted game whose two worlds load the same base: one clone, in both registries.
void case_hosted_shared() {
    boot();
    worlds(true, true);
    check(define(), "a mod defines an item whose model its mod serves later");
    kc::sync_world(KILN_WORLD_SERVER, fake::server_world);  // the server world's registry load (server thread)
    kc::sync_world(KILN_WORLD_CLIENT, fake::client_world);  // the client world's (its registry load / Pre)
    const uintptr_t sc = fake::server_reg.get(item_id()), cc = fake::client_reg.get(item_id());
    check(sc && sc == cc, "both worlds' registries have the same clone (one base ItemInfo)");
    check(!resolved(sc), "the model is not served yet: the clone keeps the base's");
    fake::model_served = true;  // kiln.assets serves it now
    const std::vector<uint8_t> before = bytes(sc);
    client_pre();
    check(bytes(sc) == before, "the client world's retry ran first and left the server world's clone as it was (no byte written)");
    check(fake::logged("resolved late") == 0, "the client world's retry wrote nothing");
    server_pre();
    check(resolved(sc), "server Pre wrote the late model reference and its list header into the clone");
    check(fake::logged("resolved late") == 1, "written once");
    client_pre();
    server_pre();
    check(fake::logged("resolved late") == 1, "later retries of both worlds write nothing more");
}

// A hosted game whose client world's base moved: one clone per world.
void case_hosted_distinct() {
    boot();
    worlds(true, false);
    check(define(), "a mod defines an item whose model its mod serves later");
    kc::sync_world(KILN_WORLD_SERVER, fake::server_world);
    kc::sync_world(KILN_WORLD_CLIENT, fake::client_world);
    const uintptr_t sc = fake::server_reg.get(item_id()), cc = fake::client_reg.get(item_id());
    check(sc && cc && sc != cc, "each world's registry has its own clone (the bases differ)");
    fake::model_served = true;
    const std::vector<uint8_t> before = bytes(sc);
    client_pre();
    check(bytes(sc) == before, "the client world's retry left the server world's clone as it was");
    check(resolved(cc), "the client world's retry wrote its own clone");
    server_pre();
    check(resolved(sc), "server Pre wrote the server world's clone");
    check(fake::logged("resolved late") == 2, "each clone was written once (each counts its own problems)");
}

// A join: no server world in this process.
void case_join() {
    boot();
    worlds(false, true);
    check(define(), "a mod defines an item whose model its mod serves later");
    kc::sync_world(KILN_WORLD_CLIENT, fake::client_world);
    const uintptr_t cc = fake::client_reg.get(item_id());
    check(cc && !resolved(cc), "the client world's registry has the clone, model not served yet");
    fake::model_served = true;
    client_pre();
    check(resolved(cc), "a join: the client world's retry writes the client world's clone");
}

// Host, leave to the menu, join someone else's server: the clone the server world had is the client world's alone now.
void case_host_then_join() {
    boot();
    worlds(true, true);
    check(define(), "a mod defines an item whose model its mod serves later");
    kc::sync_world(KILN_WORLD_SERVER, fake::server_world);
    kc::sync_world(KILN_WORLD_CLIENT, fake::client_world);
    const uintptr_t clone = fake::server_reg.get(item_id());
    stop(KILN_WORLD_SERVER, fake::server_world, 1);
    stop(KILN_WORLD_CLIENT, fake::client_world, 1);
    fake::server_world = 0, fake::client_world = 0;
    // the join's client world (a new session) gets the same clone: the base is the same resource memory
    fake::client_world = 0x90000, fake::client_session = 2;
    fake::client_reg.make();
    fake::client_reg.put(fake::kBaseId, client_base);
    kc::sync_world(KILN_WORLD_CLIENT, fake::client_world);
    check(fake::client_reg.get(item_id()) == clone, "the joined client world's registry has the clone the hosted game had");
    fake::model_served = true;
    client_pre();
    check(resolved(clone), "after hosting, the joined client world's retry writes it (the server world is gone)");
}

// Host again: the next server session claims the clone again; the client world leaves it to the server world.
void case_rehost() {
    boot();
    worlds(true, true);
    check(define(), "a mod defines an item whose model its mod serves later");
    kc::sync_world(KILN_WORLD_SERVER, fake::server_world);
    kc::sync_world(KILN_WORLD_CLIENT, fake::client_world);
    const uintptr_t clone = fake::server_reg.get(item_id());
    stop(KILN_WORLD_SERVER, fake::server_world, 1);
    stop(KILN_WORLD_CLIENT, fake::client_world, 1);
    // the next hosted game: its client world is set up before the server world's first step, then the server world
    fake::server_world = 0, fake::client_world = 0x90000, fake::client_session = 2;
    fake::client_reg.make();
    fake::client_reg.put(fake::kBaseId, client_base);
    kc::sync_world(KILN_WORLD_CLIENT, fake::client_world);
    fake::server_world = 0x50000, fake::server_session = 2;
    fake::server_reg.make();
    fake::server_reg.put(fake::kBaseId, server_base);
    kc::sync_world(KILN_WORLD_SERVER, fake::server_world);
    check(fake::server_reg.get(item_id()) == clone && fake::client_reg.get(item_id()) == clone,
          "the next hosted game's two registries have the same clone");
    fake::model_served = true;
    const std::vector<uint8_t> before = bytes(clone);
    client_pre();
    check(bytes(clone) == before, "re-hosted: the client world's retry leaves the clone of the new server session alone");
    server_pre();
    check(resolved(clone), "re-hosted: server Pre writes it");
}

// The definition changes while the worlds run: its old clone stays in the server world's registry until the server
// world takes the new one, and nothing writes the old one again.
void case_redefined() {
    boot();
    worlds(true, true);
    check(define(), "a mod defines an item whose model its mod serves later");
    kc::sync_world(KILN_WORLD_SERVER, fake::server_world);
    kc::sync_world(KILN_WORLD_CLIENT, fake::client_world);
    const uintptr_t old = fake::server_reg.get(item_id());
    check(define("Test_Sword_Two"), "the mod redefines it (a reload with another name)");
    const std::vector<uint8_t> before = bytes(old);
    fake::model_served = true;
    client_pre();  // registers the new version in the client world first, then retries
    const uintptr_t now = fake::client_reg.get(item_id());
    check(now && now != old, "the client world took the new version's clone");
    check(bytes(old) == before, "the old clone, still the server world's, was not written by the client world");
    server_pre();
    check(fake::server_reg.get(item_id()) == now, "the server world took the new clone in its Pre");
    check(bytes(old) == before, "the old clone is never written again");
}


// The server world's STOPPING comes while its world is still intact: the core delivers it, clears its pointer, and only
// then calls the game's destructor (tick.cpp hk_world_destroy), which may still read the clone. A client world that
// steps on through that (not the game's order: it destroys the client world first) leaves the clone alone for the rest
// of its session; the next client world, set up after the server world is gone, writes it.
void case_server_stops_client_goes_on() {
    boot();
    worlds(true, true);
    check(define(), "a mod defines an item whose model its mod serves later");
    kc::sync_world(KILN_WORLD_SERVER, fake::server_world);
    kc::sync_world(KILN_WORLD_CLIENT, fake::client_world);
    const uintptr_t clone = fake::client_reg.get(item_id());
    fake::model_served = true;
    client_pre();
    check(!resolved(clone), "while the server world runs, the client world's retry leaves the shared clone alone");
    stop(KILN_WORLD_SERVER, fake::server_world, 1);  // STOPPING, on the server world's thread; the world is intact
    client_pre();
    check(!resolved(clone), "after STOPPING, while the server world is still there: the client world's retry leaves it");
    fake::server_world = 0;  // the core clears its pointer, then the game's destructor runs
    client_pre();
    check(!resolved(clone), "the core's pointer gone but the destructor may still run: the same client session leaves it");
    stop(KILN_WORLD_CLIENT, fake::client_world, 1);
    fake::client_world = 0x90000, fake::client_session = 2;  // the next game's client world, after the server world is gone
    fake::client_reg.make();
    fake::client_reg.put(fake::kBaseId, client_base);
    kc::sync_world(KILN_WORLD_CLIENT, fake::client_world);
    client_pre();
    check(resolved(clone), "the next client world, set up after the server world was destroyed, writes the clone");
}

// No destroy hook (world.lifecycle off): no STOPPING is ever seen and the session keeps the old server world's pointer.
// After hosting, a joined client world leaves the clone the last server world claimed (nothing synchronised says that
// world is gone: without a step for a while it may only be stalled), until a newer server session shows it is.
void case_no_destroy_hook() {
    boot();
    fake::lifecycle = false;
    worlds(true, true);
    check(define(), "a mod defines an item whose model its mod serves later");
    kc::sync_world(KILN_WORLD_SERVER, fake::server_world);
    kc::sync_world(KILN_WORLD_CLIENT, fake::client_world);
    const uintptr_t clone = fake::server_reg.get(item_id());
    // leave and join: the core saw no stop; the server pointer stays in the session
    fake::client_world = 0x90000, fake::client_session = 2;
    fake::client_reg.make();
    fake::client_reg.put(fake::kBaseId, client_base);
    kc::sync_world(KILN_WORLD_CLIENT, fake::client_world);
    check(fake::client_reg.get(item_id()) == clone, "the joined client world's registry has the clone the hosted game had");
    fake::model_served = true;
    client_pre();
    check(!resolved(clone), "no stop seen: the joined client world leaves the clone the last server world claimed");
    // a newer server world began (the core starts a session only for a new world): the old one is gone, and the new one
    // has not put the clone into its registry yet
    fake::server_world = 0x50000, fake::server_session = 2;
    client_pre();
    check(resolved(clone), "a newer server session began: the client world's retry writes the clone");
}

// The server world registers between the client registration's session snapshot ("no server world") and its claims:
// the client registration must not take the server world's fresh claim away.
void case_concurrent_publication() {
    boot();
    worlds(true, true);
    fake::server_world = 0, fake::server_session = 0;  // not stepping yet when the client registration starts
    check(define(), "a mod defines an item whose model its mod serves later");
    fake::before_client_registry = [] {
        fake::server_world = 0x40000, fake::server_session = 1;
        kc::sync_world(KILN_WORLD_SERVER, fake::server_world);
    };
    kc::sync_world(KILN_WORLD_CLIENT, fake::client_world);
    const uintptr_t sc = fake::server_reg.get(item_id()), cc = fake::client_reg.get(item_id());
    check(sc && sc == cc, "both registries have the one clone (the server world published it first)");
    fake::model_served = true;
    const std::vector<uint8_t> before = bytes(sc);
    client_pre();
    check(bytes(sc) == before, "the client world's retry leaves the clone the server world published meanwhile");
    server_pre();
    check(resolved(sc), "server Pre writes it");
}

// The reference resolves in the client world only: its bare model GUID is used by an item in the client world's registry
// alone (an item that registered there only). An "item:<name>" reference to an item a world lacks is "item ... not
// found" there (refs.cpp item_spec), which is not retried, so it cannot reach the retry at all.
void case_resolves_differently(bool shared) {
    boot();
    worlds(true, shared);
    fake::model_from_item = true;
    const uintptr_t source = fake::make_item(0x50C0001, 0x5C, fake::kSource);
    std::memcpy(reinterpret_cast<uint8_t*>(source) + fake::kModel, fake::model_guid, 16);  // a loaded item that uses the model
    fake::client_reg.put(0x50C0001, source);  // in the client world's registry only
    check(define(), "a mod defines an item whose model GUID only a client-world item uses");
    kc::sync_world(KILN_WORLD_SERVER, fake::server_world);
    kc::sync_world(KILN_WORLD_CLIENT, fake::client_world);
    const uintptr_t sc = fake::server_reg.get(item_id()), cc = fake::client_reg.get(item_id());
    const std::vector<uint8_t> before = bytes(sc);
    client_pre();
    server_pre();
    check(bytes(sc) == before, "the server world's clone keeps the base's model: the server world cannot resolve it, "
                               "and the client world never writes the server world's clone");
    if (shared) check(sc == cc, "(one clone: the client world sees what the server world has)");
    else check(resolved(cc), "a clone of the client world's own resolves in the client world");
}
void case_resolves_differently_shared() { case_resolves_differently(true); }
void case_resolves_differently_distinct() { case_resolves_differently(false); }

#ifdef KITBASH_TSAN
// ThreadSanitizer: the server world's clone is read on another thread (the GameServer thread's programs read their
// ItemInfos) while the client world's retry runs on this one (the main thread). Any write to it from here is a race.
int run_tsan() {
    boot();
    worlds(true, true);
    if (!define()) return 2;
    kc::sync_world(KILN_WORLD_SERVER, fake::server_world);
    kc::sync_world(KILN_WORLD_CLIENT, fake::client_world);
    const uintptr_t clone = fake::server_reg.get(item_id());
    if (!clone) return 2;
    fake::model_served = true;
    std::atomic<bool> stop_reading{false};
    std::atomic<uint64_t> reads{0}, sum{0};
    std::thread reader([&] {  // relaxed only: nothing here orders the reads before the retry's writes
        uint8_t copy[fake::kItemSize];
        while (!stop_reading.load(std::memory_order_relaxed)) {
            std::memcpy(copy, reinterpret_cast<const void*>(clone), sizeof copy);
            uint64_t s = 0;
            for (uint8_t b : copy) s += b;  // used, so the copy is not optimized away
            sum.fetch_add(s, std::memory_order_relaxed);
            reads.fetch_add(1, std::memory_order_relaxed);
        }
    });
    while (reads.load(std::memory_order_relaxed) < 1000) std::this_thread::yield();
    for (int i = 0; i < 3; ++i) client_pre();
    stop_reading.store(true, std::memory_order_relaxed);
    reader.join();
    const bool untouched = !resolved(clone);
    server_pre();  // no reader now: the server world's own retry
    std::printf("  %s the client world's retry wrote nothing into the server world's clone while it was read\n", untouched ? "ok  " : "FAIL");
    std::printf("  %s server Pre wrote it afterwards\n", resolved(clone) ? "ok  " : "FAIL");
    std::printf("lateref-tsan: %s (a ThreadSanitizer report above is a failure too)\n", untouched && resolved(clone) ? "passed" : "FAILED");
    return untouched && resolved(clone) ? 0 : 1;
}
#endif

}  // namespace

int main() {
#ifdef KITBASH_TSAN
    return run_tsan();
#else
    const std::pair<const char*, void (*)()> cases[] = {
        {"hosted, one clone in both worlds", &case_hosted_shared},
        {"hosted, a clone per world", &case_hosted_distinct},
        {"join", &case_join},
        {"host, leave, join", &case_host_then_join},
        {"host again", &case_rehost},
        {"definition replaced while hosting", &case_redefined},
        {"the server world stops, the client world goes on", &case_server_stops_client_goes_on},
        {"host, leave, join without the destroy hook", &case_no_destroy_hook},
        {"server registration between the client's snapshot and its claim", &case_concurrent_publication},
        {"the worlds resolve differently, one clone", &case_resolves_differently_shared},
        {"the worlds resolve differently, a clone per world", &case_resolves_differently_distinct},
    };
    std::string failed;
    for (const auto& [name, fn] : cases) {
        std::printf("== %s\n", name);
        std::fflush(stdout);
        const pid_t pid = fork();
        if (pid == 0) {
            fn();
            std::fflush(stdout);
            _exit(failures ? 1 : 0);
        }
        int status = 0;
        waitpid(pid, &status, 0);
        if (WIFSIGNALED(status) || WEXITSTATUS(status) != 0) {
            if (WIFSIGNALED(status)) std::printf("  FAIL the case crashed (signal %d)\n", WTERMSIG(status));
            failed += std::string(failed.empty() ? "" : ", ") + name;
        }
    }
    if (failed.empty()) std::printf("lateref-test: all %zu cases passed\n", std::size(cases));
    else std::printf("lateref-test: FAILED: %s\n", failed.c_str());
    return failed.empty() ? 0 : 1;
#endif
}
