// SPDX-License-Identifier: MIT
// kiln.content, host test: newcontent.cpp's pack journal (nc_pack_mark / nc_pack_rollback / nc_pack_unmark, content.h;
// 0.2.0-dev.31.14). A mod's pack that throws partway leaves nothing of what its "add" lists defined or changed: packs.cpp
// marks the mod's owner before its first pack and rolls it back at the throw. Over the real newcontent.cpp (definitions,
// their index by GUID and name, nc_packs_begin / nc_packs_end, the listing), with no world (nothing is synced), a fake
// kiln API and owner labels:
//  - an item the marked owner's pack adds: orphaned at the rollback, as a removed pack's item (its id stays registered)
//  - an item it redefines: its definition from before (text, name in the index, display name), as a new version, with the
//    clone the game had (so the layers other packs put on its fields reach the game again); a recipe its RecipeInfo, an
//    item its random-loot entry
//  - an item it takes over from another mod's pack: that pack's again (owner, label, source, its clone)
//  - an orphan it adopts: an orphan again
//  - a rollback, then more, then a rollback: the mark's state; the journal is empty after each (a third does nothing)
//  - another owner's rollback, and definitions after nc_pack_unmark: nothing is journaled or touched
// packundo_test.sh builds and runs it.
#include <windows.h>

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

// What newcontent.cpp takes from <windows.h> and <intrin.h> beyond host/windows.h (as text_test.cpp).
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
std::vector<std::string> lines;
std::map<KilnOwner, std::string> owner_labels;
const void* KILN_CALL service(KilnStr, uint32_t) { return nullptr; }
uint32_t KILN_CALL side() { return KILN_SIDE_SERVER; }
bool KILN_CALL on_world_thread(uint32_t) { return true; }
uintptr_t KILN_CALL world(uint32_t) { return 0; }
KilnResult KILN_CALL post_world(KilnOwner, uint32_t, KilnTaskFn, void*) { return KILN_OK; }  // every owner is alive
KilnApi api = [] {
    KilnApi a{};
    a.size = sizeof a;
    a.service = &service;
    a.side = &side;
    a.world = &world;
    a.on_world_thread = &on_world_thread;
    a.post_world = &post_world;
    return a;
}();
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
    if (std::getenv("PACKUNDO_TEST_LOG")) std::printf("  log: %s\n", b);
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
std::string owner_label(KilnOwner o) {
    if (const auto found = fake::owner_labels.find(o); found != fake::owner_labels.end()) return found->second;
    return "kiln";
}
void set_owner_label(KilnOwner o, const std::string& label) { fake::owner_labels[o] = label; }
std::string data_dir() { return "D:\\kiln\\data"; }
bool path_exists(const std::string&) { return false; }
const std::string& current_pack_dir() {
    static const std::string none;
    return none;
}
uintptr_t active_world() { return 0; }
uint32_t active_kind() { return KILN_WORLD_SERVER; }
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

constexpr KilnOwner kA = 11, kB = 12, kC = 13;  // pack a, pack b, mod c (code)

std::string item(const char* guid, const char* name, const char* display) {
    return std::string(R"({"guid": ")") + guid + R"(", "name": ")" + name + R"(", "base": "Wood", "displayName": ")" + display + "\"}";
}
// A pack's "add" entry, as nc_pack_add hands one to nc_define.
bool define(KilnOwner owner, const std::string& text, bool pack = true, const std::string& source = "content/x.json") {
    std::string report;
    return kc::nc_define(owner, KILN_CONTENT_ITEMS, text, pack, source, &report) == KILN_OK;
}
kc::DefPtr def(const char* guid) {
    uint8_t g[16];
    kc::guid_parse(guid, g);
    return kc::find_def(KILN_CONTENT_ITEMS, g);
}
bool define_recipe(KilnOwner owner, const char* guid, const char* name, const char* input) {
    std::string report;
    const std::string text = std::string(R"({"guid": ")") + guid + R"(", "name": ")" + name + R"(", "base": "Recipe_Rope", "inputs": [")" +
                             input + R"("], "outputs": ["Rope"]})";
    return kc::nc_define(owner, KILN_CONTENT_RECIPES, text, true, "content/x.json", &report) == KILN_OK;
}
kc::DefPtr recipe(const char* guid) {
    uint8_t g[16];
    kc::guid_parse(guid, g);
    return kc::find_def(KILN_CONTENT_RECIPES, g);
}
bool named(const std::string& name, const kc::DefPtr& d) {
    const auto* v = kc::g_index.named(KILN_CONTENT_ITEMS, name);
    return v && std::find(v->begin(), v->end(), d) != v->end();
}

constexpr const char* kX = "11111111-0000-4000-8000-000000000001";  // added by a's pack that throws
constexpr const char* kY = "11111111-0000-4000-8000-000000000002";  // a's, redefined by the pack that throws
constexpr const char* kZ = "11111111-0000-4000-8000-000000000003";  // b's, taken over by a's pack that throws
constexpr const char* kQ = "11111111-0000-4000-8000-000000000004";  // an orphan the pack that throws adopts
constexpr const char* kW = "11111111-0000-4000-8000-000000000005";  // added after a first rollback
constexpr const char* kK = "11111111-0000-4000-8000-000000000006";  // c's code definition (no pack)
constexpr const char* kR = "11111111-0000-4000-8000-000000000007";  // a's recipe, redefined by the pack that throws

void run() {
    kc::N.on = true;
    fake::owner_labels = {{kA, "pack a"}, {kB, "pack b"}, {kC, "mod c"}};

    // ---- the application before: a's pack defines Y and Q, b's pack Z; c's code K
    kc::nc_packs_begin();
    check(define(kA, item(kY, "Yew", "Yew Bow")) && define(kA, item(kQ, "Quill", "Quill")) && define(kB, item(kZ, "Zinc", "Zinc Bar")),
          "before: a's pack defines Y and Q, b's pack Z");
    check(define(kC, item(kK, "Kettle", "Kettle"), false, ""), "before: c's code defines K");
    kc::nc_packs_end();
    const std::string y_text = def(kY)->text, z_text = def(kZ)->text;
    const uint64_t y_version = def(kY)->version;

    // ---- Q's pack goes away for one application: an orphan
    kc::nc_packs_begin();
    define(kA, item(kY, "Yew", "Yew Bow"));
    define(kB, item(kZ, "Zinc", "Zinc Bar"));
    kc::nc_packs_end();
    check(def(kQ)->orphan, "Q is in no pack: an orphan");

    // ---- the world had built Y's and Z's clones and R's recipe before the reload (sync_world does it; no world here). The
    // game's maps point at them, and so do the layers other packs put on their fields: b's rules on its own Z, an earlier
    // mod's rule that matches Y (an "items *" stack size rule), since a layer is an address in the clone.
    def(kY)->clones[0x7000] = kc::CloneRec{0x7100};
    def(kZ)->clones[0x7000] = kc::CloneRec{0x7200};
    {
        kc::nc_packs_begin();
        define(kA, item(kY, "Yew", "Yew Bow"));
        define(kB, item(kZ, "Zinc", "Zinc Bar"));
        check(define_recipe(kA, kR, "Rope", "Wood"), "before: a's pack defines the recipe R");
        kc::nc_packs_end();
        const kc::DefPtr r = recipe(kR);
        r->info = 0x7300, r->info_version = r->version;
        r->loot_entry = 0, r->loot_version = ~0ull;
        def(kY)->loot_entry = 0x7400, def(kY)->loot_version = def(kY)->version;
    }
    const uint64_t r_version = recipe(kR)->version;

    // ---- the application with the pack that throws: b applies first, then a (marked) adds X, redefines Y with another
    // name and display name, takes Z over, adopts Q, redefines R, and throws
    kc::nc_packs_begin();
    define(kB, item(kZ, "Zinc", "Zinc Bar"));
    kc::nc_pack_mark(kA);
    check(define(kA, item(kX, "Xylo", "Xylophone")), "throw: a's pack adds X");
    check(define(kA, item(kY, "YewLong", "Long Yew Bow")), "throw: a's pack redefines Y (another name)");
    check(define(kA, item(kZ, "Zinc", "Zinc Bar (a)")), "throw: a's pack takes Z over from b's pack");
    check(define(kA, item(kQ, "Quill", "Quill")) && !def(kQ)->orphan, "throw: a's pack adopts the orphan Q");
    check(define_recipe(kA, kR, "Rope", "Stone"), "throw: a's pack redefines R (another input)");
    check(def(kY)->name == "YewLong" && named("YewLong", def(kY)), "throw: Y carries the new name in the index");
    check(def(kY)->clones.empty() && def(kZ)->clones.empty() && recipe(kR)->info == 0 && def(kY)->loot_entry == 0,
          "throw: the redefinitions dropped the clones, the recipe and the loot entry built for the version before");
    check(kc::nc_pack_rollback(kB) == 0 && def(kZ)->owner == kA, "another owner's rollback does nothing");
    check(kc::nc_pack_rollback(kA) == 5, "rollback: five definitions come back");
    const kc::DefPtr x = def(kX), y = def(kY), z = def(kZ), q = def(kQ), r = recipe(kR);
    check(x && x->orphan && !x->seen, "rollback: X, which the pack added, is orphaned (its id stays registered)");
    check(y->text == y_text && y->name == "Yew" && y->display == "Yew Bow" && y->owner == kA, "rollback: Y is its definition from before");
    check(named("Yew", y) && !named("YewLong", y), "rollback: the index names Y as before");
    check(y->version == y_version + 2, "rollback: Y is a new version (the maps are re-pointed at the next sync)");
    // the game gets back the clone it had (its memory stays valid): the layers other packs put on it reach the game again,
    // where a clone built anew from the definition would have none of them until the next application
    check(y->clones.size() == 1 && y->clones.count(0x7000) && y->clones.at(0x7000).at == 0x7100,
          "rollback: Y gets the clone it had before the reload back, with the layers on it (an earlier mod's rule), not a new one");
    check(z->clones.size() == 1 && z->clones.count(0x7000) && z->clones.at(0x7000).at == 0x7200,
          "rollback: Z, taken over, gets b's pack's clone back, with b's rules on it");
    check(r->version == r_version + 2 && r->info == 0x7300 && r->info_version == r->version,
          "rollback: R gets the recipe it had back (built for this version: not built again)");
    check(y->loot_entry == 0x7400 && y->loot_version == y->version, "rollback: Y gets its random-loot entry back");
    check(!y->seen && !y->orphan, "rollback: Y not seen in this application yet (the packs from before define it again, or it is orphaned)");
    check(z->owner == kB && z->owner_label == "pack b" && z->text == z_text && z->seen, "rollback: Z is b's pack's again, seen in this application");
    check(q->orphan, "rollback: Q is an orphan again");
    check(def(kK)->owner == kC && !def(kK)->orphan, "rollback: c's code definition is untouched");
    check(kc::nc_pack_rollback(kA) == 0, "a second rollback with nothing new does nothing");

    // ---- the packs from before apply again in a's place (Y only), then a later throw rolls back to the mark again
    check(define(kA, item(kY, "Yew", "Yew Bow")) && def(kY)->seen, "again: a's packs from before define Y (unchanged)");
    check(define(kA, item(kW, "Wand", "Wand")), "again: a pack adds W");
    check(kc::nc_pack_rollback(kA) == 2 && def(kW)->orphan && !def(kY)->seen, "a rollback after the packs from before: the mark's state again");
    kc::nc_pack_unmark();
    check(define(kA, item(kY, "Yew", "Yew Bow")), "after the mark: a defines Y");
    check(kc::nc_pack_rollback(kA) == 0 && def(kY)->seen, "unmarked: nothing is journaled, a rollback does nothing");
    kc::nc_packs_end();
    const std::string listing = kc::nc_definitions_json();
    const auto entries = nlohmann::json::parse(listing);
    std::map<std::string, std::string> state;
    for (const auto& e : entries) state[e["name"].get<std::string>()] = e["state"].get<std::string>();
    check(state["Xylo"] == "orphaned" && state["Wand"] == "orphaned" && state["Quill"] == "orphaned",
          "listing: X, W and Q are orphaned");
    check(state["Yew"] == "pending" && state["Zinc"] == "pending" && state["Kettle"] == "pending",
          "listing: Y, Z and K stay (pending: no world in this test)");
}

}  // namespace

int main() {
    run();
    if (failures) std::printf("packundo-test: %d check(s) FAILED\n", failures);
    else std::printf("packundo-test: all checks passed\n");
    return failures ? 1 : 0;
}
