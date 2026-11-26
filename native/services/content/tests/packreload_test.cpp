// SPDX-License-Identifier: MIT
// kiln.content, host test: a mod's data packs follow the mod's load, not only the set of loaded mod ids (live server,
// Kiln 0.2.0-dev.31.10: a data-only mod loaded with `load`, its kiln.mod.json changed, then `reload <id>`: no line, the
// old layer stayed). content.cpp's Pre, pack_mods and pack_mod_reloaded over the real packs.cpp (discovery in a
// temporary kiln/mods, the apply), store.cpp (layers over the game's original bytes) and refl.cpp, with an item
// registry of two items in a made-up memory:
//  - world load applies the pack; a reload of the same id (pack_mods unchanged) re-applies the current files, with one
//    line naming the mod and the reason; the old layer comes off (a field the new version no longer sets goes back)
//  - the generation grows (the content sync rebuilds the clients' state from it); reload_pending says it is queued
//  - a reload whose new files cannot all be read keeps the mod's previous packs whole (old layer, never half)
//  - unload reverts, an unload and a load before the next Pre apply the current files, as before
//  - a reload of a mod without data packs, or before the world is ready, applies nothing
//  - kiln.mod.json changing without a reload (a hot edit) is a pack change too: the scan re-applies
//  - a pack that throws while it applies: its mod's layers of that application come off, its packs from before apply
//    again whole (also when they had ordinary field errors), with their registry sections, else none of its packs apply
//  - 300 reloads (good, broken, throwing): no new owners, nothing left queued
// packreload_test.sh builds and runs it.
#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

// What content.cpp takes from <windows.h> beyond host/windows.h; libstdc++ already defines __try as `try`.
static unsigned long long g_now_ms = 1000;
inline unsigned long long GetTickCount64() { return g_now_ms; }
inline unsigned long GetExceptionCode() { return 0; }
#define EXCEPTION_EXECUTE_HANDLER 1
#define __except(...) catch (...)
#define __declspec(x)
struct MEMORY_BASIC_INFORMATION {
    void *BaseAddress, *AllocationBase;
    unsigned long Protect, Type;
    size_t RegionSize;
};
#define MEM_PRIVATE 0x20000
#define MEM_IMAGE 0x1000000
inline size_t VirtualQuery(const void*, MEMORY_BASIC_INFORMATION*, size_t) { return 0; }

#include "services/kiln_gamedata.h"
#include "../content.cpp"

namespace fs = std::filesystem;

// ============================================================================================= the fake game =====
namespace fake {
std::map<uintptr_t, uint8_t> mem;  // the game's memory, byte by byte
std::vector<std::string> lines;    // kiln.log
std::string mods;                  // <temp>/kiln/mods
KilnOwner next_owner = 100;

void KILN_CALL log(KilnOwner, int32_t, KilnStr text) { lines.emplace_back(text.ptr, text.len); }
bool KILN_CALL on_world_thread(uint32_t) { return true; }
const void* KILN_CALL service(KilnStr, uint32_t) { return nullptr; }  // no kiln.profile: kiln/mods in folder order
KilnOwner KILN_CALL owner_create(KilnStr) { return next_owner++; }
KilnResult KILN_CALL post_world(KilnOwner, uint32_t, KilnTaskFn, void*) { return KILN_OK; }
KilnResult KILN_CALL path(int32_t kind, char* buf, size_t cap, size_t* len) {
    const std::string p = kind == KILN_PATH_MODS ? mods : fs::path(mods).parent_path().string() + "/data";
    if (p.size() >= cap) return KILN_ERR_BUFFER_TOO_SMALL;
    std::memcpy(buf, p.data(), p.size());
    *len = p.size();
    return KILN_OK;
}
KilnApi api = [] {
    KilnApi a{};
    a.size = sizeof a;
    a.log = &log;
    a.on_world_thread = &on_world_thread;
    a.service = &service;
    a.owner_create = &owner_create;
    a.post_world = &post_world;
    a.path = &path;
    return a;
}();

uint32_t read32(uintptr_t a) {
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= uint32_t(mem[a + i]) << (8 * i);
    return v;
}
void write32(uintptr_t a, uint32_t v) {
    for (int i = 0; i < 4; ++i) mem[a + i] = uint8_t(v >> (8 * i));
}
}  // namespace fake

namespace kc {
// game.cpp's memory access, over fake::mem
bool rd(uintptr_t address, void* out, size_t n) {
    auto* o = static_cast<uint8_t*>(out);
    for (size_t i = 0; i < n; ++i) {
        auto it = fake::mem.find(address + i);
        if (it == fake::mem.end()) return false;
        o[i] = it->second;
    }
    return true;
}
size_t write_batch(std::vector<Write>& writes, uintptr_t*) {
    for (const Write& w : writes)
        for (uint8_t i = 0; i < w.n; ++i) fake::mem[w.addr + i] = w.bytes[i];
    return writes.size();
}
WriteStats write_stats() { return {}; }
bool patch_code_byte(uintptr_t, uint8_t, uint8_t) { return false; }
void* near_alloc(uintptr_t, size_t) { return nullptr; }
uint32_t fnv1a(const void* p, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) h = (h ^ static_cast<const uint8_t*>(p)[i]) * 16777619u;
    return h;
}
std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
std::string guid_text(const uint8_t*) { return "00000000-0000-0000-0000-000000000000"; }

// registries.cpp: one item registry (items Wood and Stone; field maxStackSize, u32 at +0)
Registry g_regs[5];
Registry& reg(uint32_t kind) { return g_regs[valid_kind(kind) ? kind : 0]; }
bool valid_kind(uint32_t k) { return k >= KILN_CONTENT_ITEMS && k <= KILN_CONTENT_DROPS; }
void Registry::add(Entry e) {
    const uint32_t i = static_cast<uint32_t>(entries.size());
    by_id[e.id] = i, by_name[e.name] = i, by_lower[lower(e.name)] = i, by_addr[e.addr] = i;
    entries.push_back(std::move(e));
}
const Entry* Registry::get(uint32_t id) const {
    const auto it = by_id.find(id);
    return it == by_id.end() ? nullptr : &entries[it->second];
}
const Entry* Registry::lookup(const std::string& key) const {
    if (auto it = by_name.find(key); it != by_name.end()) return &entries[it->second];
    if (auto it = by_lower.find(lower(key)); it != by_lower.end()) return &entries[it->second];
    return nullptr;
}
bool read_recipe_stacks(const Entry&, uint32_t, std::vector<KilnContentStack>*) { return false; }
bool walk_drop_table(uintptr_t, std::vector<DropStackRef>*) { return false; }
uint32_t map_count(uintptr_t) { return 0; }
void read_drop_stack(uintptr_t, uint32_t, KilnContentStack*) {}
bool build_registries(uintptr_t, std::string* why) {
    *why = "not in this test";
    return false;
}
Engine E;
RecipeLayout RL;
DropLayout DL;
std::unordered_map<uint32_t, std::vector<uint8_t>> g_category_guid;

// What the packs reach in the service's other files: no new content, no kitbash, no registry stage in this test.
void nc_packs_begin() {}
void nc_packs_end() {}
// An "add" entry {"throw": "<what>"} throws while its pack applies (a section's code meeting a value type it did not expect):
// the rule before it in the same pack has applied already.
void nc_pack_add(KilnOwner, uint32_t, const std::string& list, const std::string&, ApplyResult* out) {
    if (list.find("\"throw\"") != std::string::npos) throw std::runtime_error("[json.exception.type_error.302] type must be string, but is number");
    out->errors.push_back("no new content here");
}
bool nc_is_new(uint32_t, uint32_t) { return false; }
void nc_drops_table(const Entry&) {}
void nc_world_ready(uintptr_t, uint32_t) {}
void nc_world_stopping(uintptr_t) {}
KilnResult nc_define(KilnOwner, uint32_t, const std::string&, bool, const std::string&, std::string*) { return KILN_ERR_UNAVAILABLE; }
std::string nc_definitions_json() { return "[]"; }
bool nc_icon_alias(const uint8_t[16], const uint8_t[16]) { return false; }
std::string nc_info_json(const std::string&) { return "{}"; }
uint32_t nc_undefine_owner(KilnOwner) { return 0; }
uint32_t nc_forget_unbound(KilnOwner) { return 0; }
void nc_boot_failed(std::vector<BootFailed>*) {}
bool nc_lab_refuse(const std::string&, const std::string&) { return false; }
void nc_boot() {}
void nc_text_boot() {}
bool nc_menu_render() { return false; }
KilnResult KILN_CALL nc_text_api(uint32_t, char*, size_t, size_t*) { return KILN_ERR_UNAVAILABLE; }
KilnResult KILN_CALL nc_text_info_api(KilnContentTextInfo*) { return KILN_ERR_UNAVAILABLE; }
bool is_kitbash_key(const Schema&, const std::string&) { return false; }
void kitbash(const Schema&, const std::string&, const nlohmann::ordered_json&, const ItemFind&, const std::vector<uintptr_t>&, KitbashOut*, uintptr_t) {}
std::string ref_text(const uint8_t*) { return "none"; }
// The registry stage: a schema (so the packs' registry sections are lowered and handed over), and what it was handed.
std::shared_ptr<const registry::Schema> registry_live_schema() {
    static const auto schema = std::make_shared<const registry::Schema>();
    return schema;
}
std::vector<RegistrySection> g_staged;  // the sections of the last application
void registry_live_apply(const std::vector<RegistrySection>& sections, const registry::Schema*) { g_staged = sections; }
void registry_live_publish() {}
void registry_live_world_begin(uint32_t) {}
void registry_live_world_stop() {}
void drops_table_loaded(const Entry&) {}
void refs_reset() {}
std::string refs_info_json(const Schema&, const std::vector<uintptr_t>&) { return {}; }
const registry::Schema& registry_schema();
namespace registry {
bool lower_section(const json&, const std::string&, const std::string&, const std::string&, const Schema&, std::vector<Batch>*,
                   std::vector<Refusal>*, std::string* err) {
    *err = "no registry stage in this test";
    return false;
}
Schema generated_schema() { return {}; }
void link_elements(Schema*) {}
}  // namespace registry
namespace ml {
void probe_owners() {}
}  // namespace ml
}  // namespace kc

// ============================================================================================= the cases =========
namespace {

int failures = 0;
void check(bool ok, const std::string& what) {
    std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) ++failures;
}
bool logged(const std::string& part) {
    for (const auto& l : fake::lines)
        if (l.find(part) != std::string::npos) return true;
    return false;
}
size_t count_logged(const std::string& part) {
    size_t n = 0;
    for (const auto& l : fake::lines) n += l.find(part) != std::string::npos;
    return n;
}

constexpr uintptr_t kWood = 0x1000, kStone = 0x2000;
fs::path g_root;

void write(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary | std::ios::trunc) << text;
}
fs::path mod_dir(const std::string& folder) { return g_root / "kiln" / "mods" / folder; }
void manifest(const std::string& folder, const std::string& id, const std::string& version) {
    write(mod_dir(folder) / "kiln.mod.json", "{\"id\": \"" + id + "\", \"name\": \"" + id + "\", \"version\": \"" + version + "\"}");
}
void pack(const std::string& folder, const std::string& file, const std::string& text) {
    write(mod_dir(folder) / "content" / file, text);
}
// A file rewritten with the same size and modification time as before: the 2-s scan cannot see it (a deploy that
// keeps times, or two writes in one timestamp tick). Only the mod's load can say the files are new.
void same_stamp(const std::string& folder, const std::string& file, const std::string& text) {
    const fs::path p = mod_dir(folder) / "content" / file;
    const auto size = fs::file_size(p);
    const auto t = fs::last_write_time(p);
    std::string padded = text;
    if (padded.size() > size) {
        std::printf("  (test bug: %s longer than the file it replaces)\n", file.c_str());
        std::abort();
    }
    padded.append(size - padded.size(), ' ');
    write(p, padded);
    fs::last_write_time(p, t);
}
void pack_mods(const std::string& json) { t_pack_mods(KilnStr{json.data(), json.size()}); }
KilnResult reloaded(const std::string& mod, const std::string& reason) {
    return t_pack_mod_reloaded(KilnStr{mod.data(), mod.size()}, KilnStr{reason.data(), reason.size()});
}

uint64_t g_steps = 1;
void step() {
    ++g_steps;
    if (g_steps % 25 == 0 || g_steps % 250 == 0) ++g_steps;  // no owner probe or timing lines in these steps
    KilnTickInfo t{};
    t.size = sizeof t;
    t.step = g_steps;
    pre(&t, KILN_WORLD_SERVER, 0x5000);
}
uint64_t generation() { return g_generation.load(); }
bool pending() {
    KilnContentStatus s{};
    s.size = sizeof s;
    t_status(&s);
    return s.reload_pending != 0;
}

// The world's registries loaded: what try_ready does once build_registries succeeded, then apply_packs(false).
void world_load() {
    Registry& items = reg(KILN_CONTENT_ITEMS);
    items.kind = KILN_CONTENT_ITEMS;
    items.label = "items";
    items.available = true;
    Field f;
    f.path = "maxStackSize";
    f.type_name = "uint32";
    f.offset = 0;
    f.bytes = 4;
    f.vt = KILN_CV_INT;
    f.is_signed = false;
    items.schema.type = "keen::ItemInfo";
    items.schema.size = 16;
    items.schema.fields.push_back(f);
    items.schema.by_path["maxStackSize"] = 0;
    items.schema.by_lower["maxstacksize"] = 0;
    for (const auto& [name, addr] : {std::pair<const char*, uintptr_t>{"Wood", kWood}, {"Stone", kStone}}) {
        Entry e;
        e.name = name;
        e.id = fnv1a(e.name);
        e.addr = addr;
        items.add(e);
    }
    fake::write32(kWood, 10);
    fake::write32(kStone, 20);
    g_kind.store(KILN_WORLD_SERVER);
    g_world.store(0x5000);
    apply_packs(false);
    g_ready.store(true);
}

void setup() {
    g_root = fs::temp_directory_path() / ("kiln-packreload-" + std::to_string(::getpid()));
    fs::remove_all(g_root);
    fs::create_directories(g_root / "kiln" / "mods");
    fake::mods = (g_root / "kiln" / "mods").string();
    kc::g_api = &fake::api;
}

void run() {
    setup();
    // a data-only mod, hot-loaded with `load` (so it is the last loaded mod): content/skills.json
    manifest("skilltest", "skilltest", "1.0.0");
    pack("skilltest", "skills.json", R"({"format": 1, "items": {"Wood": {"maxStackSize": 100}}})");
    manifest("other", "other", "1.0.0");
    pack("other", "other.json", R"({"format": 1, "items": {"Stone": {"maxStackSize": 25}}})");
    pack_mods(R"(["other","skilltest"])");  // Kiln.Content's list, before the world
    world_load();
    check(fake::read32(kWood) == 100 && fake::read32(kStone) == 25, "world load: both mods' packs apply (Wood 100, Stone 25)");
    const uint64_t g0 = generation();
    step();
    check(generation() == g0, "nothing changed: no re-application");

    // ---- a reload: same id, same place in the load order (pack_mods gets the same list), new kiln.mod.json and files
    manifest("skilltest", "skilltest", "1.0.1");
    same_stamp("skilltest", "skills.json", R"({"format": 1, "items": {"Stone": {"maxStackSize": 50}}})");
    pack_mods(R"(["other","skilltest"])");
    check(reloaded("skilltest", "was reloaded (1.0.0 -> 1.0.1)") == KILN_OK, "pack_mod_reloaded takes the mod and the reason");
    check(pending(), "status: reload_pending says a re-application is queued");
    fake::lines.clear();
    step();
    check(logged("mod skilltest was reloaded (1.0.0 -> 1.0.1): re-applying data packs"),
          "one line names the mod and the reason");
    check(count_logged("re-applying data packs") == 1, "exactly one such line");
    check(!pending(), "nothing queued after the Pre");
    check(generation() == g0 + 1, "the generation grew by one (the content sync rebuilds the clients' state from it)");
    check(fake::read32(kWood) == 10, "the old layer came off: Wood, which the new version no longer sets, is back to the game's 10");
    check(fake::read32(kStone) == 50, "the new version's value applies over the earlier mod's (Stone 50, load order kept)");

    // ---- a reload whose new files cannot all be read: the mod's previous packs stay, whole
    pack("skilltest", "more.json", R"({"format": 1, "items": {"Wood": {"maxStackSize": 70}}})");
    reloaded("skilltest", "was reloaded");
    step();
    check(fake::read32(kWood) == 70 && fake::read32(kStone) == 50, "a second file of the mod applies with the first (Wood 70, Stone 50)");
    const uint64_t g1 = generation();
    same_stamp("skilltest", "skills.json", R"({"format": 1, "items": {"Stone": {"maxStackSize": 60}}})");
    pack("skilltest", "more.json", R"({"format": 1, "items": {"Wood": {"maxStackSize": 7)");  // cut off: the deploy was still writing
    reloaded("skilltest", "was reloaded (1.0.1 -> 1.0.2)");
    fake::lines.clear();
    step();
    check(generation() == g1 + 1, "the reload re-applied");
    check(fake::read32(kWood) == 70 && fake::read32(kStone) == 50,
          "one file of the new version is broken: the mod keeps its previous packs whole (Wood 70, Stone 50; not Stone 60)");
    check(logged("data packs of skilltest: ") && logged("the packs it had before stay applied, whole"),
          "a line says the mod's previous packs stay, and why");
    pack("skilltest", "more.json", R"({"format": 1, "items": {"Wood": {"maxStackSize": 80}}})");
    reloaded("skilltest", "was reloaded");
    step();
    check(fake::read32(kWood) == 80 && fake::read32(kStone) == 60, "once every file reads, the new version applies whole (Wood 80, Stone 60)");

    // ---- unload reverts; an unload and a load before the next Pre apply the current files
    pack_mods(R"(["other"])");
    step();
    check(fake::read32(kWood) == 10 && fake::read32(kStone) == 25, "unload: the mod's layer comes off (Wood 10, Stone back to other's 25)");
    std::filesystem::remove(mod_dir("skilltest") / "content" / "more.json");
    pack("skilltest", "skills.json", R"({"format": 1, "items": {"Wood": {"maxStackSize": 90}}})");
    reloaded("skilltest", "was reloaded");  // not in the loaded set: nothing to do for it
    const uint64_t g2 = generation();
    step();
    check(generation() == g2, "a reload line for a mod that is not loaded re-applies nothing");
    pack_mods(R"([])");
    pack_mods(R"(["other","skilltest"])");
    step();
    check(fake::read32(kWood) == 90 && fake::read32(kStone) == 25, "an unload and a load between two Pres apply the current files (Wood 90)");

    // ---- a mod without data packs, and before the world is ready
    const uint64_t g3 = generation();
    reloaded("codeonly", "was reloaded");
    step();
    check(generation() == g3, "a reload of a mod that has no data packs re-applies nothing");
    check(reloaded("", "was reloaded") == KILN_ERR_INVALID_ARGUMENT, "an empty mod id is refused");

    // ---- kiln.mod.json changing without a reload (a hot edit): the scan re-applies
    step();  // the first scan (records the signature)
    const uint64_t g4 = generation();
    manifest("skilltest", "skilltest", "1.0.3");
    g_now_ms += 2500;
    fake::lines.clear();
    step();
    check(generation() == g4 + 1 && logged("data pack files changed: re-applying"),
          "a kiln.mod.json edit (version) is a pack change: the 2-s scan re-applies");

    // ---- a pack that throws while it applies (nobody had tested this branch): none of that application's layers of the
    // mod stay; its packs from before apply again, whole, with their registry sections
    {
        // the version before: a field error (an ordinary problem, reported, not a throw) and a registry section
        pack("skilltest", "skills.json",
             R"({"format": 1, "items": {"Wood": {"maxStackSize": 90, "noSuchField": 1}}, "registry": {"v": "old"}})");
        reloaded("skilltest", "was reloaded");
        step();
        check(fake::read32(kWood) == 90 && fake::read32(kStone) == 25, "throw: the version before applies (Wood 90, Stone 25)");
        auto staged_of = [](const std::string& mod) {
            std::vector<uint64_t> h;
            for (const RegistrySection& s : g_staged)
                if (s.mod == mod) h.push_back(s.hash);
            return h;
        };
        const std::vector<uint64_t> old_sections = staged_of("skilltest");
        check(old_sections.size() == 1, "throw: the registry stage got the version's one registry section");
        // the new version: Stone 33 applies, then its "add" list throws
        pack("skilltest", "skills.json",
             R"({"format": 1, "items": {"Stone": {"maxStackSize": 33}, "add": [{"throw": 1}]}, "registry": {"v": "new"}})");
        reloaded("skilltest", "was reloaded (1.0.3 -> 1.0.4)");
        fake::lines.clear();
        step();
        check(fake::read32(kStone) == 25, "throw: the layer the throwing pack wrote before it threw comes off (Stone back to other's 25)");
        check(fake::read32(kWood) == 90, "throw: the packs from before apply again, whole, though they had an ordinary field error (Wood 90)");
        check(logged("a pack threw while it applied: the packs it had before apply again, whole"), "throw: a line says the packs from before apply again");
        check(staged_of("skilltest") == old_sections,
              "throw: the registry stage gets the section of the packs that apply (the one from before), not the throwing pack's");
        check(g_report.find("applying it failed") != std::string::npos && g_report.find("type_error") != std::string::npos,
              "throw: the report names the file and the throw");
        // and once more: the bookkeeping still holds the version before (not the one that threw)
        reloaded("skilltest", "was reloaded");
        step();
        check(fake::read32(kWood) == 90 && fake::read32(kStone) == 25, "throw, again: the version before still applies");
        // no version before (the mod's packs came off with an unload): the throwing pack's mod applies nothing
        pack_mods(R"(["other"])");
        step();
        pack_mods(R"(["other","skilltest"])");
        fake::lines.clear();
        step();
        check(fake::read32(kWood) == 10 && fake::read32(kStone) == 25, "throw without a version before: none of the mod's packs apply (Wood 10, Stone 25)");
        check(logged("a pack threw while it applied: none of its packs apply"), "throw without a version before: a line says none apply");
        check(staged_of("skilltest").empty(), "throw without a version before: no registry section of the mod reaches the stage");
        // fixed: the new version applies
        pack("skilltest", "skills.json", R"({"format": 1, "items": {"Wood": {"maxStackSize": 90}}})");
        reloaded("skilltest", "was reloaded");
        step();
        check(fake::read32(kWood) == 90 && fake::read32(kStone) == 25, "throw fixed: the new version applies (Wood 90)");
    }

    // ---- many reloads (good, broken, throwing, in turn): no new owners, nothing left queued, the last good version applies
    {
        const KilnOwner owners = fake::next_owner;
        const uint64_t g = generation();
        for (int i = 0; i < 300; ++i) {
            const int k = i % 3;
            if (k == 0) pack("skilltest", "more.json", R"({"format": 1, "items": {"Stone": {"maxStackSize": 4)");  // cut off
            else if (k == 1) pack("skilltest", "more.json", R"({"format": 1, "items": {"Stone": {"maxStackSize": 5}, "add": [{"throw": 1}]}})");
            else pack("skilltest", "more.json", R"({"format": 1, "items": {"Stone": {"maxStackSize": )" + std::to_string(100 + i) + "}}}");
            reloaded("skilltest", "was reloaded");
            reloaded("skilltest", "was reloaded");  // twice before one Pre: one application
            step();
        }
        check(generation() == g + 300, "300 reloads: one application each (a mod named twice before a Pre applies once)");
        check(fake::next_owner == owners, "300 reloads: no owner created after the first load (one pack owner per mod id)");
        check(!pending() && fake::read32(kWood) == 90 && fake::read32(kStone) == 399, "300 reloads: nothing queued; the last version applies (Wood 90, Stone 399)");
        fs::remove(mod_dir("skilltest") / "content" / "more.json");
        reloaded("skilltest", "was reloaded");
        step();
    }

    // ---- the world goes away: nothing stays queued for the next one
    reloaded("skilltest", "was reloaded");
    world_reset(true, "is stopping");
    check(!pending() && fake::read32(kWood) == 10 && fake::read32(kStone) == 20, "a world reset puts every original back and drops what was queued");
    check(reloaded("skilltest", "was reloaded") == KILN_OK && !pending(), "before the world is ready a reload queues nothing (the world load applies the files)");

    fs::remove_all(g_root);
}

// review2 S5 remainder: Content.Entry.Modified (KilnContentEntry.flags) follows registry layers. A registry write goes
// through the shared store, never through Content's setters, so only the store's generation tells fill_entry's cache
// (refresh_modified) that it changed: a registry layer makes the entry modified, its drop makes it unmodified again,
// with no Content call in between.
bool modified(const char* name) {
    KilnContentEntry e{};
    e.size = sizeof e;
    const KilnResult rc = t_find(KILN_CONTENT_ITEMS, KilnStr{name, std::strlen(name)}, &e);
    return rc == KILN_OK && (e.flags & KILN_CONTENT_MODIFIED) != 0;
}
void modified_follows_registry_layers() {
    setup();
    world_load();  // no mod has packs: nothing modified
    check(!modified("Wood") && !modified("Stone"), "Modified: nothing differs from the game's original yet");
    constexpr KilnOwner kRegistry = 0x7e9;  // the registry's owner (registry_live.cpp registry_owner)
    const Entry* wood = reg(KILN_CONTENT_ITEMS).get(fnv1a("Wood", 4));
    const uint32_t v = 40;
    uint8_t b[4];
    std::memcpy(b, &v, 4);
    std::string err;
    {
        std::lock_guard lock(store_mutex());
        check(wood && store_put(kRegistry, registry_seq(1), KILN_CONTENT_ITEMS, wood->id, 0, -1, kWood, 4, b, "registry"),
              "a registry layer on Wood's maxStackSize");
        store_flush(&err);
    }
    check(fake::read32(kWood) == 40, "the registry layer is written (40)");
    check(modified("Wood") && !modified("Stone"), "Modified: Wood is modified by the registry layer alone (the store's generation, no Content setter)");
    {
        std::lock_guard lock(store_mutex());
        store_drop_owner(kRegistry);
        store_flush(&err);
    }
    check(fake::read32(kWood) == 10 && !modified("Wood"), "Modified: the registry layer's drop puts Wood back, unmodified");
    {
        std::lock_guard lock(store_mutex());
        const uint32_t original = 10;
        std::memcpy(b, &original, 4);
        store_put(kRegistry, registry_seq(2), KILN_CONTENT_ITEMS, wood->id, 0, -1, kWood, 4, b, "registry");  // the original value
        store_flush(&err);
    }
    check(!modified("Wood"), "Modified: a registry layer that writes the original value leaves the entry unmodified");
    {
        std::lock_guard lock(store_mutex());
        store_drop_owner(kRegistry);
        store_flush(&err);
    }
    world_reset(true, "is stopping");
    reg(KILN_CONTENT_ITEMS) = Registry{};  // run() loads its own world
    fs::remove_all(g_root);
}

}  // namespace

int main() {
    modified_follows_registry_layers();
    run();
    if (failures) std::printf("packreload-test: %d check(s) FAILED\n", failures);
    else std::printf("packreload-test: all checks passed\n");
    return failures ? 1 : 0;
}
