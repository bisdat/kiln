// SPDX-License-Identifier: MIT
// kiln.content: Kiln's hook on the game's item lookup (ItemRegistryFind, the "[Unknown item]" placeholder) goes in only
// when it is needed (GitHub issue 39), on the host: newcontent.cpp's own early_boot, unknown_enable and hk_itemfind over
// a fake kiln.hooks table and a fake lookup whose entry jumps to the hook once it is "installed".
//  - boot: the lookup is not hooked (the placeholder is defined); the lab's early-test file hooks it at boot
//  - a join (no server world in this process): hooked when the client world's item registry finishes loading, and an id
//    the registry lacks answers the placeholder
//  - a host without item mods: never hooked; an item defined while its client world runs: a line, and the next world's
//    registry load hooks it
//  - a host with item mods: hooked at the registry load
//  - the handler entered while install() has not returned yet (a job thread on the fresh detour): it answers through the
//    chain and never through an unset pointer
//  - <kiln>/data/content/no-unknown-items: never hooked
// Each case runs in a child process: the hook and the definitions live as long as the process, as in the game.
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
bool enter_during_install = false;  // raw_detour calls the new link before it returns (a job thread on the fresh detour)
std::vector<std::string> installs;  // labels, in order
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
        *original = reinterpret_cast<void*>(&real_find);  // the continuation is published first, as kiln.hooks does
        detour = reinterpret_cast<Find>(fn);              // from here the link is reachable
        if (enter_during_install) entered = detour(kKnown, kReg);
    } else {
        *original = reinterpret_cast<void*>(&other_fn);
    }
    *out = next_handle++;
    return KILN_OK;
}
KilnResult KILN_CALL revoke(KilnHandle) { return KILN_OK; }
uintptr_t KILN_CALL caller(KilnHandle) { return 0; }

KilnHooksApi hooks = [] {
    KilnHooksApi h{};
    h.header.size = sizeof h;
    h.header.version = 2;
    h.raw_detour = &raw_detour;
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
uintptr_t KILN_CALL world(uint32_t kind) { return kind == KILN_WORLD_SERVER ? server_world : 0; }
bool KILN_CALL on_world_thread(uint32_t) { return false; }
KilnResult KILN_CALL session(KilnSessionInfo* s) {
    const uint32_t size = s->size;
    std::memset(s, 0, size);
    s->size = size;
    s->side = side();
    s->worlds[KILN_WORLD_SERVER] = server_world;
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
int64_t field_offset(const char*, const char*) { return -1; }
uint32_t type_size(const char*) { return 0; }
bool enum_values(const char*, std::vector<std::pair<std::string, int64_t>>*) { return false; }
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

const std::string kFind = "content/ItemRegistryFind";
uint8_t tmpl[64];  // Kiln_Unknown_Item's clone in the client world's registry

void boot() {
    kc::N.on = true;  // nc_boot's content.new (a game build with new content)
    kc::L.item.size = sizeof tmpl;
    kc::L.i_id = 0;
    kc::early_boot();
}

// The client world's item registry finished loading (sync_world_pass after its items: newcontent.cpp's unknown_enable).
void registry_loaded(uintptr_t ireg, uint32_t session) {
    kc::reset_ws(kc::g_early_kind, 0x9000 + ireg, session);
    kc::WorldState& ws = kc::g_ws[kc::g_early_kind];
    ws.ireg = ireg;
    if (kc::g_unknown_id) ws.items[kc::g_unknown_id] = reinterpret_cast<uintptr_t>(tmpl);
    kc::unknown_enable(ws);
}
// More definitions went in while the world runs (the next sync pass of the same world).
void synced_again() { kc::unknown_enable(kc::g_ws[kc::g_early_kind]); }

bool define_mod_item() {
    std::string rep;
    return kc::nc_define(7, KILN_CONTENT_ITEMS,
                         R"({"guid":"0b7d2c1e-5a44-4c1b-9e0f-1c2d3e4f5a6b","name":"Test_Sword","base":"Weapon_T1_1H_SwordEpic_01"})",
                         false, "test", &rep) == KILN_OK;
}

uint32_t id_of(uintptr_t item) {
    uint32_t id = 0;
    if (item) std::memcpy(&id, reinterpret_cast<const void*>(item + kc::L.i_id), 4);
    return id;
}

void case_boot() {
    boot();
    check(kc::g_unknown_id != 0, "boot defines the placeholder item Kiln_Unknown_Item");
    check(!fake::has(fake::installs, kFind), "boot does not hook the game's item lookup");
    check(fake::has(fake::installs, "RegisterSingleton") && fake::has(fake::installs, "content/ItemRegistryUpdate"),
          "boot still hooks the registry load (registration at registry load)");
    check(fake::entry(0x0badf00d, fake::kReg) == 0, "an unknown id stays NULL before anything needs the placeholder");
}

void case_join() {
    boot();
    fake::server_world = 0;  // joined someone else's server
    check(!fake::has(fake::installs, kFind), "a join: not hooked before the client world's registry loads");
    registry_loaded(fake::kReg, 1);
    check(fake::has(fake::installs, kFind), "a join: hooked when the client world's item registry finished loading");
    check(fake::logged("item lookup is hooked") && fake::logged("joined"), "a join: kiln.log says it is hooked and why");
    check(fake::logged("self-check through the game's lookup: ok"), "a join: the placeholder's self-check passes");
    const uintptr_t u = fake::entry(0x0badf00d, fake::kReg);
    check(u != 0 && id_of(u) == 0x0badf00d, "a join: an id the registry lacks answers a placeholder with that id");
    check(fake::entry(fake::kKnown, fake::kReg) == reinterpret_cast<uintptr_t>(fake::known_item), "a join: a known id answers the game's item");
    check(fake::entry(0x0badf00d, 0x7777) == 0, "a join: another registry's miss stays NULL");
}

void case_host_plain() {
    boot();
    fake::server_world = 0x4000;  // this game hosts its world
    registry_loaded(fake::kReg, 1);
    check(!fake::has(fake::installs, kFind), "a host without item mods: not hooked at the registry load");
    check(fake::logged("item lookup is not hooked"), "a host without item mods: kiln.log says it is not hooked and why");
    check(fake::entry(0x0badf00d, fake::kReg) == 0, "a host without item mods: an unknown id stays NULL, as without Kiln");
    // a mod defines an item while the client world runs: no hook in this world, a line saying when it starts
    check(define_mod_item(), "a mod item is defined while the world runs");
    synced_again();
    check(!fake::has(fake::installs, kFind), "an item defined while the world runs: still not hooked in this world");
    check(fake::logged("starts with the next world"), "an item defined while the world runs: kiln.log says the placeholder starts with the next world");
    const size_t said = fake::lines.size();
    synced_again();
    check(fake::lines.size() == said, "that line is written once per world");
    // the next world: mods define items now
    registry_loaded(fake::kReg2, 2);
    check(fake::has(fake::installs, kFind), "the next world's registry load hooks it (mods define items now)");
    const uintptr_t u = fake::entry(0x0badf00d, fake::kReg2);
    check(u != 0 && id_of(u) == 0x0badf00d, "the next world answers the placeholder");
}

void case_host_items() {
    boot();
    fake::server_world = 0x4000;
    check(define_mod_item(), "a mod defines an item before the world");
    check(!fake::has(fake::installs, kFind), "a host with item mods: not hooked before the registry loads");
    registry_loaded(fake::kReg, 1);
    check(fake::has(fake::installs, kFind), "a host with item mods: hooked at the registry load");
    check(fake::logged("item lookup is hooked") && fake::logged("define"), "a host with item mods: kiln.log says it is hooked and why");
    const uintptr_t u = fake::entry(0x0badf00d, fake::kReg);
    check(u != 0 && id_of(u) == 0x0badf00d, "a host with item mods: an id the registry lacks answers the placeholder");
}

void case_enter_during_install() {
    fake::enter_during_install = true;
    boot();
    fake::server_world = 0;
    registry_loaded(fake::kReg, 1);
    check(fake::has(fake::installs, kFind), "hooked (a join)");
    check(fake::entered == reinterpret_cast<uintptr_t>(fake::known_item),
          "the handler entered before install() returned answers through the chain (the hook's published continuation)");
}

void case_early_test() {
    fake::client = false;
    fake::early_test = true;
    boot();
    check(fake::has(fake::installs, kFind), "early-test (a lab server): hooked at boot");
}

void case_opt_out() {
    fake::no_unknown = true;
    boot();
    fake::server_world = 0;
    registry_loaded(fake::kReg, 1);
    check(!fake::has(fake::installs, kFind), "no-unknown-items: a join is not hooked either");
    check(fake::entry(0x0badf00d, fake::kReg) == 0, "no-unknown-items: an unknown id stays NULL");
}

}  // namespace

int main() {
    const std::pair<const char*, void (*)()> cases[] = {
        {"boot", &case_boot},
        {"join", &case_join},
        {"host without item mods", &case_host_plain},
        {"host with item mods", &case_host_items},
        {"handler entered during install", &case_enter_during_install},
        {"early-test", &case_early_test},
        {"no-unknown-items", &case_opt_out},
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
    if (failed.empty()) std::printf("itemfind-test: all %zu cases passed\n", std::size(cases));
    else std::printf("itemfind-test: FAILED: %s\n", failed.c_str());
    return failed.empty() ? 0 : 1;
}
