// SPDX-License-Identifier: MIT
// kiln.content, since 0.2.0-dev.30.5: a recipe definition's "menuGroup" (a tab of a crafting menu the game has, "0x<workshop>/<tab GUID>[/<list
// GUID>]", parse_menu_group) and "trackCrafted" (the game's "crafted once" completion query on the recipe's own id, crafted_query), on the host:
// newcontent.cpp over the fakes of itemfind_test.cpp, with the game's reflection of keen::KnowledgeQueryData and its enums (client 26430625).
#include <windows.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

// What newcontent.cpp takes from <windows.h> and <intrin.h> beyond host/windows.h. libstdc++ already defines __try as
// `try` (bits/exception_defines.h), so an SEH guard reads as try / catch (...) here.
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
bool client = true;              // side()
bool early_test = false;         // <kiln>/data/content/early-test
bool no_unknown = false;         // <kiln>/data/content/no-unknown-items
uintptr_t server_world = 0;      // the session's live server world (0: this game joined someone else's server)
uintptr_t client_world = 0;      // the session's live client world (0: still loading)
bool enter_during_install = false;  // raw_detour calls the new link before it returns (a job thread on the fresh detour)
bool refuse_checked = false;     // mid2_checked: a thread could not be checked
std::vector<std::string> installs;  // raw links' labels, in order
std::vector<std::string> checked;   // mid2_checked calls on the lookup: "placed" or "refused"
bool placed = false;             // the lookup has a physical detour that the checked installer wrote
int unchecked_patches = 0;       // raw links that wrote the lookup's detour themselves (the plain installer)
KilnHandle placer = 0;           // the checked placer's mid link on the lookup
bool placer_revoked = false;
std::vector<std::string> lines;     // kiln.log
KilnHandle next_handle = 1;

constexpr uint32_t kKnown = 0x11112222;  // an item of the registry
uint8_t known_item[64];
std::unordered_map<uint32_t, uintptr_t> registry = {{kKnown, reinterpret_cast<uintptr_t>(known_item)}};
constexpr uintptr_t kReg = 0x5000, kReg2 = 0x6000;  // two client worlds' item registries

using Find = uintptr_t (*)(uint32_t, uintptr_t);
// The game's ItemRegistryFind, and its entry: once hooked, the entry leads to the chain (the hook), whose
// continuation is the real function.
uintptr_t real_find(uint32_t id, uintptr_t) {
    const auto it = registry.find(id);
    return it == registry.end() ? 0 : it->second;
}
Find detour = nullptr;
uintptr_t entry(uint32_t id, uintptr_t reg) { return detour ? detour(id, reg) : real_find(id, reg); }
// RegisterSingleton and ItemRegistryUpdate (distinct addresses; never called here)
uintptr_t other_fn(uintptr_t, uintptr_t, uintptr_t, uintptr_t) { return 0; }
uintptr_t other_fn2(uintptr_t, uintptr_t, uintptr_t, uintptr_t) { return 1; }
uintptr_t entered = 0;  // what the link answered when entered inside raw_detour

KilnResult KILN_CALL raw_detour(KilnOwner, uintptr_t target, const KilnHookOrder* o, void* fn, void** original, KilnHandle* out) {
    installs.emplace_back(o && o->label.ptr ? std::string(o->label.ptr, o->label.len) : "?");
    if (target == reinterpret_cast<uintptr_t>(&entry)) {
        if (!placed && !detour) ++unchecked_patches;      // the first link on the target writes its detour
        *original = reinterpret_cast<void*>(&real_find);  // the continuation is published first, as kiln.hooks does
        detour = reinterpret_cast<Find>(fn);              // from here the link is reachable
        if (enter_during_install) entered = detour(kKnown, kReg);
    } else {
        *original = reinterpret_cast<void*>(&other_fn);
    }
    *out = next_handle++;
    return KILN_OK;
}
// The checked mid installation: places the target's detour, or refuses before writing anything.
KilnResult KILN_CALL mid2_checked(KilnOwner, uintptr_t target, const KilnHookOrder*, KilnMidFn, void*, KilnHandle* out, uint32_t* unchecked) {
    if (target == reinterpret_cast<uintptr_t>(&entry)) checked.emplace_back(refuse_checked ? "refused" : "placed");
    if (refuse_checked) {
        if (unchecked) *unchecked = 1;
        return KILN_HOOKS_ERR_UNCHECKED;
    }
    if (target == reinterpret_cast<uintptr_t>(&entry)) placed = true, placer = next_handle;
    *out = next_handle++;
    return KILN_OK;
}
KilnResult KILN_CALL revoke(KilnHandle h) {
    if (h && h == placer) placer_revoked = true;
    return KILN_OK;
}
uintptr_t KILN_CALL caller(KilnHandle) { return 0; }

KilnHooksApi hooks = [] {
    KilnHooksApi h{};
    h.header.size = sizeof h;
    h.header.version = 2;
    h.raw_detour = &raw_detour;
    h.mid2_checked = &mid2_checked;
    h.revoke = &revoke;
    h.caller = &caller;
    return h;
}();

const void* KILN_CALL service(KilnStr name, uint32_t) {
    return std::string(name.ptr, name.len) == KILN_HOOKS_SERVICE ? &hooks : nullptr;
}
KilnResult KILN_CALL symbol(KilnStr name, uintptr_t* out) {
    const std::string n(name.ptr, name.len);
    *out = n == "content/ItemRegistryFind"     ? reinterpret_cast<uintptr_t>(&entry)
           : n == "RegisterSingleton"          ? reinterpret_cast<uintptr_t>(&other_fn)
           : n == "content/ItemRegistryUpdate" ? reinterpret_cast<uintptr_t>(&other_fn2)
                                               : 0;
    return *out ? KILN_OK : KILN_ERR_NOT_FOUND;
}
KilnResult KILN_CALL constant(KilnStr name, int64_t* out) {
    const std::string n(name.ptr, name.len);
    *out = n == "world.storage" ? 0x10 : n == "storage.kind" ? 0x8 : 0;
    return KILN_OK;
}
uint32_t KILN_CALL side() { return client ? KILN_SIDE_CLIENT : KILN_SIDE_SERVER; }
uintptr_t KILN_CALL world(uint32_t kind) { return kind == KILN_WORLD_SERVER ? server_world : kind == KILN_WORLD_CLIENT ? client_world : 0; }
bool KILN_CALL on_world_thread(uint32_t) { return false; }
KilnResult KILN_CALL session(KilnSessionInfo* s) {
    const uint32_t size = s->size;
    std::memset(s, 0, size);
    s->size = size;
    s->side = side();
    s->worlds[KILN_WORLD_SERVER] = server_world;
    s->worlds[KILN_WORLD_CLIENT] = client_world;
    s->sessions[KILN_WORLD_SERVER] = server_world ? 1 : 0;
    return KILN_OK;
}

KilnApi api = [] {
    KilnApi a{};
    a.size = sizeof a;
    a.service = &service;
    a.symbol = &symbol;
    a.constant = &constant;
    a.side = &side;
    a.world = &world;
    a.on_world_thread = &on_world_thread;
    a.session = &session;
    return a;
}();

bool has(const std::vector<std::string>& v, const std::string& s) {
    for (const auto& x : v)
        if (x == s) return true;
    return false;
}
bool logged(const std::string& part) {
    for (const auto& l : lines)
        if (l.find(part) != std::string::npos) return true;
    return false;
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
    if (std::getenv("ITEMFIND_TEST_LOG")) std::printf("  log: %s\n", b);
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
void* near_alloc(uintptr_t, size_t n) { return std::calloc(1, n ? n : 1); }
void StoreMutex::lock() { m_.lock(); }
void StoreMutex::unlock() { m_.unlock(); }
bool StoreMutex::try_lock() { return m_.try_lock(); }
StoreMutex g_store;
StoreMutex& store_mutex() { return g_store; }
PhaseTimer::PhaseTimer(Phase) {}
PhaseTimer::~PhaseTimer() {}
std::string owner_label(KilnOwner o) { return o == 1 ? "kiln" : "mod test-items"; }
std::string data_dir() { return "D:\\kiln\\data"; }
bool path_exists(const std::string& p) {
    return (fake::early_test && p == "D:\\kiln\\data\\content\\early-test") ||
           (fake::no_unknown && p == "D:\\kiln\\data\\content\\no-unknown-items");
}
const std::string& current_pack_dir() {
    static const std::string none;
    return none;
}
uintptr_t active_world() { return 0; }
uint32_t active_kind() { return KILN_WORLD_CLIENT; }
// Reached from newcontent.cpp's other paths (the clone builder, recipes, loot, the store); never called by these cases.
DropLayout DL;
void boot_late_check() {}
void barber_sync(const std::vector<BarberItem>&) {}
bool build_schema(const char*, const std::vector<std::string>&, Schema*, std::string*) { return false; }
int64_t field_offset(const char* type, const char* field) {
    if (std::string(type) != "keen::KnowledgeQueryData") return -1;
    const std::string f(field);  // the game's reflection, client 26430625
    return f == "knowledgeOrQueryId" ? 0 : f == "compareValue" ? 4 : f == "compareOperator" ? 8 : f == "type" ? 9 : f == "isExplicitPlayerKnowledgeQuery" ? 10 : -1;
}
uint32_t type_size(const char*) { return 0; }
bool enum_values(const char* type, std::vector<std::pair<std::string, int64_t>>* out) {
    const std::string t(type);
    if (t == "keen::CompareOperator") *out = {{"GreaterThan", 0}, {"LessThan", 1}, {"Equals", 2}};
    else if (t == "keen::KnowledgeQueryDataType") *out = {{"Extern", 0}, {"SimpleBool", 1}, {"SimpleCount", 2}, {"SimpleFlag", 3}};
    else return false;
    return true;
}
bool self_relative_fields(const char*, std::vector<uint32_t>*, std::string*) { return false; }
bool encode(const Field&, const KilnContentValue&, uint8_t*, std::string*) { return false; }
const Field* Schema::find(const std::string&) const { return nullptr; }
const Entry* Registry::lookup(const std::string&) const { return nullptr; }
Registry& reg(uint32_t) {
    static Registry r;
    return r;
}
void drops_apply_rules(const Entry&) {}
void kitbash(const Schema&, const std::string&, const nlohmann::ordered_json&, const ItemFind&, const std::vector<uintptr_t>&,
             KitbashOut*, uintptr_t) {}
void set_kitbash_mod(const std::string*) {}
uint32_t map_count(uintptr_t) { return 0; }
bool map_read(uintptr_t, std::vector<uint32_t>*, std::vector<uint64_t>*) { return false; }
std::string near_alloc_report() { return "-"; }
std::string read_blob_string(uintptr_t) { return ""; }
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

void menu_groups() {
    std::printf("== menuGroup\n");
    uint32_t w = 0;
    uint8_t g[16] = {}, s[16] = {};
    check(kc::parse_menu_group("0xbddd74e6/8c91ca52-be38-468c-ae74-bc6a5ffab28e", &w, g, s), "the Carpenter's Essentials tab parses");
    uint8_t want[16];
    kc::guid_parse("8c91ca52-be38-468c-ae74-bc6a5ffab28e", want);
    static const uint8_t zero[16] = {};
    check(w == 0xbddd74e6 && !std::memcmp(g, want, 16) && !std::memcmp(s, zero, 16), "workshop, tab and the first list");
    check(kc::parse_menu_group("0x0/8c91ca52-be38-468c-ae74-bc6a5ffab28e/51be23a7-616d-4158-96ed-815a47e1d507", &w, g, s) && w == 0, "the player's own menu, a list of a tab");
    kc::guid_parse("51be23a7-616d-4158-96ed-815a47e1d507", want);
    check(!std::memcmp(s, want, 16), "the list's GUID");
    for (const char* bad : {"", "0xbddd74e6", "bddd74e6/8c91ca52-be38-468c-ae74-bc6a5ffab28e", "0x/8c91ca52-be38-468c-ae74-bc6a5ffab28e",
                            "0x1bddd74e6/8c91ca52-be38-468c-ae74-bc6a5ffab28e", "0xbddd74eg/8c91ca52-be38-468c-ae74-bc6a5ffab28e",
                            "0xbddd74e6/00000000-0000-0000-0000-000000000000", "0xbddd74e6/8c91ca52-be38-468c-ae74-bc6a5ffab28e/x",
                            "0xbddd74e6/8c91ca52"})
        check(!kc::parse_menu_group(bad, &w, g, s), std::string("refused: \"") + bad + "\"");
}

void crafted() {
    std::printf("== trackCrafted\n");
    uint8_t q[12];
    std::memset(q, 0xAA, sizeof q);
    std::string why;
    check(kc::crafted_query(q, sizeof q, 3289458456u, &why), "written");
    uint32_t id = 0, value = 0;
    std::memcpy(&id, q, 4);
    std::memcpy(&value, q + 4, 4);
    // as the game's own recipes have it (Prop_Decoration_T2_Bucket: its id, 2, Equals, SimpleFlag, explicit player)
    check(id == 3289458456u && value == 2 && q[8] == 2 && q[9] == 3 && q[10] == 1, "{recipe id, 2, Equals, SimpleFlag, explicit}");
    check(!kc::crafted_query(q, 8, 1, &why) && !why.empty(), "a smaller KnowledgeQueryData than the reflection says is refused");
}

}  // namespace

int main() {
    menu_groups();
    crafted();
    std::printf(failures ? "menugroup-test: %d FAILED\n" : "menugroup-test: all passed\n", failures);
    return failures ? 1 : 0;
}
