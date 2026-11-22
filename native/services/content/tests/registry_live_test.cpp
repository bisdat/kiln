// SPDX-License-Identifier: MIT
// kiln.content, host test: the registry's live back-end (registry_live.cpp, registry P1 S9) over the real store (store.cpp),
// the real IR / composition (registry/ir.cpp, compose.cpp), the real formulas and the committed regmap (regmap/common + the
// server record), on a made-up game memory: the fields of a composed stage written as evaluated layers above packs and below
// code, relative edits re-evaluated over a pack's new value, hot reload (a changed section recomposed, a removed one leaving
// nothing behind, a new field refused "needs a restart"), loud refusals, compare-before-write in the report.
// registry_live_test.sh builds and runs it.
#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include "../content.h"
#include "../registry/formulas.h"
#include "../registry/report.h"
#include "../registry/schema_ir.h"
#include "../registry_live.h"
#include "../store.cpp"
#include "../registry_live.cpp"

namespace kc {
const KilnApi* g_api = nullptr;
Engine E;
RecipeLayout RL;
DropLayout DL;
std::unordered_map<uint32_t, std::vector<uint8_t>> g_category_guid;
namespace {
alignas(16) uint8_t g_arena[1 << 20];  // the made-up game memory (real addresses, so near memory is within reach)
size_t g_near = 1 << 19;
Registry g_regs[5];
std::map<std::string, std::string> g_files;  // pack file rel -> its text
}  // namespace
uintptr_t A(size_t off) { return reinterpret_cast<uintptr_t>(g_arena) + off; }
void logf(int, const char* fmt, ...) {
    va_list a;
    va_start(a, fmt);
    std::vfprintf(stderr, fmt, a);
    va_end(a);
    std::fputc('\n', stderr);
}
bool rd(uintptr_t address, void* out, size_t n) {
    if (address < A(0) || address + n > A(sizeof g_arena)) return false;
    std::memcpy(out, reinterpret_cast<const void*>(address), n);
    return true;
}
size_t write_batch(std::vector<Write>& writes, uintptr_t*) {
    for (const Write& w : writes) std::memcpy(reinterpret_cast<void*>(w.addr), w.bytes, w.n);
    return writes.size();
}
void* near_alloc(uintptr_t, size_t n) {
    void* p = g_arena + g_near;
    g_near += (n + 15) & ~size_t(15);
    return p;
}
Registry& reg(uint32_t kind) { return g_regs[kind]; }
const Entry* Registry::get(uint32_t id) const {
    auto it = by_id.find(id);
    return it == by_id.end() ? nullptr : &entries[it->second];
}
const Entry* Registry::lookup(const std::string& key) const {
    for (const Entry& e : entries)
        if (lower(guid_text(e.guid)) == lower(key) || e.name == key) return &e;
    return nullptr;
}
std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
std::string guid_text(const uint8_t g[16]) {
    char b[40];
    std::snprintf(b, sizeof b, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", g[0], g[1], g[2], g[3], g[4], g[5],
                  g[6], g[7], g[8], g[9], g[10], g[11], g[12], g[13], g[14], g[15]);
    return b;
}
bool walk_drop_table(uintptr_t resource, std::vector<DropStackRef>* out) {
    out->clear();
    if (resource != A(0x30000)) return false;
    out->push_back({A(0x31000), 0});
    out->push_back({A(0x31040), 0});
    return true;
}
bool patch_code_byte(uintptr_t, uint8_t, uint8_t) { return false; }
uintptr_t type_by_hash(uint32_t) { return 0; }
std::string read_text(uintptr_t, uint64_t) { return {}; }
bool guid_parse(const std::string& s, uint8_t out[16]) {
    std::string h;
    for (char c : s)
        if (c != '-') h += c;
    if (h.size() != 32) return false;
    for (int i = 0; i < 16; ++i) out[i] = static_cast<uint8_t>(std::stoul(h.substr(2 * i, 2), nullptr, 16));
    return true;
}
uint32_t fnv1a(const void* data, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; ++i) h = (h ^ static_cast<const uint8_t*>(data)[i]) * 16777619u;
    return h;
}
// kiln.gamedata's item configs: the sword has a ManaCost config (float at +0x40) with a tooltip row (+0x44, Normal format)
bool gd_item_config_slot(const Entry& e, const std::string& path, GdValueSlot* out, std::string* why) {
    if (e.id != 0x222 || path != "impactValues.0xaf236ece") return *why = "the item has no such configuration (Kiln's live back-end does not add one)", false;
    out->addr = e.addr + 0x40, out->kind = 1000, out->id = e.id, out->field = 7;
    out->f.path = "impactValues.0xaf236ece.value", out->f.offset = 0, out->f.bytes = 4, out->f.vt = KILN_CV_FLOAT, out->f.writable = true;
    out->tip = e.addr + 0x44, out->tip_format = 0;
    return true;
}
bool gd_ui_scale(uint8_t, double v, int64_t* out) { return *out = std::llround(v), true; }
const registry::Schema& registry_schema() {
    static const registry::Schema s = registry::generated_schema();
    return s;
}
std::vector<RegistrySection> registry_sections(const std::vector<PackFile>& files, const registry::Schema& schema) {
    std::vector<RegistrySection> out;
    for (const PackFile& f : files) {
        const registry::json doc = registry::json::parse(g_files[f.rel], nullptr, false, true);
        if (doc.is_discarded() || !doc.contains("registry")) continue;
        RegistrySection s;
        s.mod = f.mod, s.version = f.version, s.rel = f.rel, s.source = f.rel.substr(f.rel.find("/content/") + 1);
        s.hash = std::hash<std::string>{}(doc["registry"].dump());
        registry::lower_section(doc["registry"], f.mod, f.version, s.source, schema, &s.batches, &s.refused, &s.error);
        out.push_back(std::move(s));
    }
    return out;
}

void apply_registry_files(const std::vector<PackFile>& files) {
    const auto schema = registry_live_schema();
    const auto sections = registry_sections(files, *schema);
    {
        std::lock_guard lock(store_mutex());
        registry_live_apply(sections, schema.get());
    }
    registry_live_publish();
}
}  // namespace kc

namespace {
using namespace kc;
int checks = 0;
void check(bool yes, const std::string& name) {
    if (!yes) throw std::runtime_error(name);
    ++checks;
}

constexpr KilnOwner PACK = 11, CODE = 44;
constexpr uint64_t kPackSeq = (1ull << 44) | (1ull << 32) | 1;
const uint8_t G1[16] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1}, G2[16] = {2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2},
              G3[16] = {3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3}, G4[16] = {4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4};
const uint8_t P1[16] = {9, 1, 9, 1, 9, 1, 9, 1, 9, 1, 9, 1, 9, 1, 9, 1}, P2[16] = {9, 2, 9, 2, 9, 2, 9, 2, 9, 2, 9, 2, 9, 2, 9, 2};
constexpr uint32_t WOOD = 0x111, SWORD = 0x222, PLANK = 0x333, STONE = 0x555;
constexpr size_t WOOD_AT = 0x10000, SWORD_AT = 0x10100, STONE_AT = 0x10200, PLANK_AT = 0x20000;

template <class T>
T at(size_t off) {
    T v{};
    std::memcpy(&v, g_arena + off, sizeof v);
    return v;
}
template <class T>
void mem(size_t off, T v) {
    std::memcpy(g_arena + off, &v, sizeof v);
}

Field fld(const char* path, uint32_t off, uint8_t bytes, uint8_t vt, bool f64 = false) {
    Field f;
    f.path = path, f.offset = off, f.bytes = bytes, f.vt = vt, f.f64 = f64, f.writable = true;
    return f;
}
void add_field(Schema& s, Field f) {
    s.by_path[f.path] = static_cast<uint32_t>(s.fields.size());
    s.by_lower[lower(f.path)] = static_cast<uint32_t>(s.fields.size());
    s.fields.push_back(std::move(f));
}
void add_entry(Registry& r, uint32_t id, const char* name, size_t at_off, const uint8_t g[16]) {
    Entry e;
    e.id = id, e.name = name, e.addr = A(at_off);
    std::memcpy(e.guid, g, 16);
    r.by_id[id] = static_cast<uint32_t>(r.entries.size());
    r.entries.push_back(e);
}

std::shared_ptr<registry::map::RegMap> g_regmap;
registry::Result g_last_report;
int g_reports = 0;

void world() {
    std::string e;
    store_reset(false, &e);
    std::memset(g_arena, 0, sizeof g_arena);
    g_near = 1 << 19;
    for (Registry& r : g_regs) r = Registry{};
    Registry& I = g_regs[KILN_CONTENT_ITEMS];
    I.kind = KILN_CONTENT_ITEMS, I.label = "items", I.available = true;
    add_field(I.schema, fld("maxStackSize", 0x00, 4, KILN_CV_INT));
    Field rar = fld("rarity", 0x04, 4, KILN_CV_INT);
    rar.enum_kind = KILN_CONTENT_ENUM;
    rar.en = std::make_shared<EnumInfo>();
    rar.en->values = {{"Common", 0}, {"Uncommon", 1}, {"Rare", 2}, {"Epic", 3}, {"Legendary", 4}};
    add_field(I.schema, rar);
    add_field(I.schema, fld("damageSetup.dmgMod", 0x08, 4, KILN_CV_FLOAT));
    add_field(I.schema, fld("damageSetup.speedMod", 0x0C, 4, KILN_CV_FLOAT));
    add_field(I.schema, fld("armorSetup.distribution.physical", 0x90, 4, KILN_CV_FLOAT));
    add_field(I.schema, fld("itemLevelRange.minLevel", 0x10, 4, KILN_CV_INT));
    add_field(I.schema, fld("itemLevelRange.maxLevel", 0x14, 4, KILN_CV_INT));
    for (int i = 0; i < 5; ++i) {
        Field pr = fld(("perkReferences[" + std::to_string(i) + "]").c_str(), 0x30 + 16 * i, 16, KILN_CV_GUID);
        pr.ref = true;
        add_field(I.schema, pr);
        add_field(I.schema, fld(("perkIds[" + std::to_string(i) + "]").c_str(), 0x80 + 4 * i, 4, KILN_CV_INT));
    }
    add_entry(I, WOOD, "Material_T1_Wood_Logs", WOOD_AT, G1);
    add_entry(I, SWORD, "Sword_T2", SWORD_AT, G2);
    add_entry(I, STONE, "Material_Stone", STONE_AT, G4);
    mem<uint32_t>(WOOD_AT, 10);
    std::memcpy(g_arena + WOOD_AT + 0x30, P1, 16), mem<uint32_t>(WOOD_AT + 0x80, 0x901);  // perk slot 0: perk_a
    mem<float>(SWORD_AT + 0x40, 10.0f), mem<int32_t>(SWORD_AT + 0x44, 10);            // ManaCost 10, its tooltip 10
    Registry& L = g_regs[KILN_CONTENT_LOOT];
    L.kind = KILN_CONTENT_LOOT, L.label = "loot", L.available = true;
    add_field(L.schema, fld("stackSizeMin", 0x00, 4, KILN_CV_INT));
    add_field(L.schema, fld("stackSizeMax", 0x04, 4, KILN_CV_INT));
    add_entry(L, WOOD, "Material_T1_Wood_Logs", 0x40000, G1);
    mem<uint32_t>(0x40000, 1), mem<uint32_t>(0x40004, 5);
    mem<uint32_t>(STONE_AT, 50);
    mem<float>(SWORD_AT + 0x08, 1.0f), mem<float>(SWORD_AT + 0x0C, 1.0f), mem<uint32_t>(SWORD_AT + 0x10, 10), mem<uint32_t>(SWORD_AT + 0x14, 15);
    mem<float>(SWORD_AT + 0x90, 1.0f);
    Registry& R = g_regs[KILN_CONTENT_RECIPES];
    R.kind = KILN_CONTENT_RECIPES, R.label = "recipes", R.available = true;
    add_field(R.schema, fld("craftingDuration", 0x00, 8, KILN_CV_TIME));
    R.schema.special["knowledgeRequirement"] = 0x50;
    add_entry(R, PLANK, "Plank_Recipe", PLANK_AT, G3);
    mem<uint32_t>(PLANK_AT + 0x50, 0x12345678), mem<uint32_t>(PLANK_AT + 0x58, 7);  // a knowledge requirement
    RL.input = 0x40, RL.output = 0x48;
    mem<int64_t>(PLANK_AT, 3'000'000'000ll);  // 3 s
    // inputs: 2 wood (one stack of 48 bytes at +0x100), outputs: 1 sword
    const size_t in_list = PLANK_AT + 0x100, out_list = PLANK_AT + 0x200;
    mem<int32_t>(PLANK_AT + 0x40, static_cast<int32_t>(in_list - (PLANK_AT + 0x40))), mem<uint32_t>(PLANK_AT + 0x44, 1);
    std::memcpy(g_arena + in_list + RL.in_item_ref, G1, 16), mem<uint32_t>(in_list + RL.in_item, WOOD), mem<uint32_t>(in_list + RL.in_count, 2);
    mem<int32_t>(PLANK_AT + 0x48, static_cast<int32_t>(out_list - (PLANK_AT + 0x48))), mem<uint32_t>(PLANK_AT + 0x4C, 1);
    std::memcpy(g_arena + out_list + RL.out_item_ref, G2, 16), mem<uint32_t>(out_list + RL.out_item, SWORD), mem<uint32_t>(out_list + RL.out_count, 1);
    Registry& D = g_regs[KILN_CONTENT_DROPS];
    D.kind = KILN_CONTENT_DROPS, D.label = "drops", D.available = true;
    add_field(D.schema, fld("countMin", 0x14, 4, KILN_CV_INT));
    add_field(D.schema, fld("countMax", 0x18, 4, KILN_CV_INT));
    add_field(D.schema, fld("spawnProbability", 0x1C, 4, KILN_CV_FLOAT));
    add_field(D.schema, fld("weight", 0x20, 4, KILN_CV_FLOAT));
    for (size_t s : {size_t(0x31000), size_t(0x31040)}) {
        mem<uint32_t>(s + 0x10, WOOD), mem<uint32_t>(s + 0x14, 1), mem<uint32_t>(s + 0x18, 3), mem<float>(s + 0x1C, 0.5f), mem<float>(s + 0x20, 1.0f);
    }
}
void add_drop_table() { add_entry(g_regs[KILN_CONTENT_DROPS], 0x444, "DT_Wolf", 0x30000, G3); }

registry::BaseModel base() {
    registry::BaseModel b;
    auto& items = b.registries["items"];
    registry::BaseModel::Entry w;
    w.id = "game:material_t1_wood_logs";
    w.fields["maxStack"] = registry::Value::integer(10);
    w.fields["rarity"] = registry::Value::text("common");
    w.fields["perks"] = registry::Value::of_list({registry::Value::text("game:perk_a")});
    items.push_back(w);
    registry::BaseModel::Entry s;
    s.id = "game:sword_t2";
    double power = 0;
    registry::formulas::read("gear.power", {1.0, 1.0, 10}, registry::formulas::constants_of({}), &power);
    s.fields["power"] = registry::Value::integer(std::llround(power));
    s.fields["armor.physical"] = registry::Value::integer(
        registry::formulas::armor_read(1, 10, registry::formulas::constants_of({})));
    s.fields["maxStack"] = registry::Value::integer(1);
    s.fields["manaCost"] = registry::Value::number(10);
    items.push_back(s);
    registry::BaseModel::Entry st;
    st.id = "game:material_stone";
    st.fields["maxStack"] = registry::Value::integer(50);
    items.push_back(st);
    registry::BaseModel::Entry rc;
    rc.id = "game:plank_recipe";
    rc.fields["duration"] = registry::Value::number(3);
    rc.fields["unlock"] = registry::Value::text("game");
    auto stru = [](std::vector<std::pair<std::string, registry::Value>> kv) {
        registry::Value x;
        x.kind = registry::Value::Struct;
        std::sort(kv.begin(), kv.end(), [](auto& a, auto& b) { return a.first < b.first; });
        for (auto& [k, v] : kv) x.keys.push_back(k), x.list.push_back(v);
        return x;
    };
    rc.fields["inputs"] = registry::Value::of_list({stru({{"count", registry::Value::integer(2)}, {"item", registry::Value::text("game:material_t1_wood_logs")}})});
    rc.fields["outputs"] = registry::Value::of_list({stru({{"count", registry::Value::integer(1)}, {"item", registry::Value::text("game:sword_t2")}})});
    b.registries["recipes"].push_back(rc);
    registry::BaseModel::Entry dt;
    dt.id = "game:dt_wolf";
    std::vector<registry::Value> st2;
    for (int i = 0; i < 2; ++i)
        st2.push_back(stru({{"chance", registry::Value::number(0.5)}, {"count.max", registry::Value::integer(3)}, {"count.min", registry::Value::integer(1)},
                            {"item", registry::Value::text("game:material_t1_wood_logs")}, {"weight", registry::Value::number(1)}}));
    dt.fields["entries"] = registry::Value::of_list(st2);
    b.registries["drops"].push_back(dt);
    registry::BaseModel::Entry lt;
    lt.id = "game:material_t1_wood_logs";
    lt.fields["stack.min"] = registry::Value::integer(1);
    lt.fields["stack.max"] = registry::Value::integer(5);
    b.registries["loot"].push_back(lt);
    return b;
}

std::vector<PackFile> files_of(const std::vector<std::pair<std::string, std::string>>& mods_files) {
    std::vector<PackFile> out;
    for (auto& [mod, rel] : mods_files) {
        PackFile f;
        f.mod = mod, f.rel = rel, f.version = "1.0.0";
        out.push_back(f);
    }
    return out;
}

// What S11 does at resources-ready: lower the sections, compose, install.
void stage(const std::vector<PackFile>& files, const std::vector<registry::Batch>& code = {}) {
    const registry::Schema& sc = registry_schema();
    std::vector<registry::ModInput> mods;
    for (const RegistrySection& s : registry_sections(files, sc)) {
        auto it = std::find_if(mods.begin(), mods.end(), [&](const registry::ModInput& m) { return m.id == s.mod; });
        if (it == mods.end()) mods.push_back({}), it = mods.end() - 1, it->id = s.mod;
        for (auto& b : s.batches) it->batches.push_back(b);
        for (auto& r : s.refused) it->refused.push_back(r);
    }
    // Runner hands over data first, queued code next, and recorded callback turns last.
    for (const auto& batch : code) {
        auto it = std::find_if(mods.begin(), mods.end(), [&](const auto& m) { return m.id == batch.mod; });
        if (it == mods.end()) mods.push_back({}), it = mods.end() - 1, it->id = batch.mod;
        it->batches.push_back(batch);
    }
    RegistryLive in;
    in.schema = std::make_shared<const registry::Schema>(sc);
    in.regmap = g_regmap;
    in.base = base();
    in.mods = mods;
    in.result = registry::compose(sc, in.base, {}, mods);
    in.ids = {{"items|game:material_t1_wood_logs", guid_text(G1)}, {"items|game:sword_t2", guid_text(G2)}, {"items|game:material_stone", guid_text(G4)},
              {"recipes|game:plank_recipe", guid_text(G3)}, {"drops|game:dt_wolf", guid_text(G3)},
              {"loot|game:material_t1_wood_logs", guid_text(G1)}, {"perks|game:perk_a", guid_text(P1)}, {"perks|game:perk_b", guid_text(P2)}};
    in.nums = {{"items|game:material_t1_wood_logs", WOOD}, {"items|game:sword_t2", SWORD}, {"items|game:material_stone", STONE},
               {"recipes|game:plank_recipe", PLANK}, {"drops|game:dt_wolf", 0x444},
               {"loot|game:material_t1_wood_logs", WOOD}, {"perks|game:perk_a", 0x901}, {"perks|game:perk_b", 0x902}};
    in.decoded = {"items.perks", "items.manaCost", "items.staminaCost", "recipes.unlock", "loot.stack.max", "items.maxStack", "items.rarity", "items.power", "recipes.duration", "recipes.inputs", "recipes.outputs", "drops.entries"};
    in.report = [](const registry::Result& r, bool) {
        g_last_report = r;
        ++g_reports;
    };
    registry_live_install(std::move(in));
}

bool reported(const std::string& needle) {
    for (const registry::Refusal& r : g_last_report.refused)
        if (r.reason.find(needle) != std::string::npos) return true;
    return false;
}

void put_pack(size_t off, uint32_t v) {
    uint8_t b[4];
    std::memcpy(b, &v, 4);
    set_owner_label(PACK, "pack oldstacks");
    store_put(PACK, kPackSeq, KILN_CONTENT_ITEMS, WOOD, 0, -1, A(off), 4, b, "oldstacks: content/a.json");
}

registry::json json_field(const registry::FieldRef& ref) {
    const auto doc = registry::report_json(registry_schema(), g_last_report, {});
    for (const auto& row : doc.at("fields"))
        if (row.at("registry") == ref.registry && row.at("id") == ref.id && row.at("field") == ref.field) return row;
    throw std::runtime_error("missing report row for " + ref.text());
}

bool conflict(const registry::FieldRef& ref, registry::LineKind kind, const std::string& pack) {
    return std::any_of(g_last_report.lines.begin(), g_last_report.lines.end(), [&](const registry::Line& line) {
        return line.field == ref && line.kind == kind && line.winner == "m" && line.loser == pack;
    });
}

void callback_order_survives_reloads() {
    for (bool queued : {false, true}) {
        world();
        g_files = {{"a/content/a.json", R"({"registry":{"alter":[{"registry":"items","select":"game:material_t1_wood_logs","field":"maxStack","op":{"mul":4}}]}})"},
                   {"b/content/b.json", R"({"registry":{"alter":[{"registry":"items","select":"game:material_stone","field":"maxStack","op":{"mul":2}}]}})"}};
        const auto files = files_of({{"a", "a/content/a.json"}, {"b", "b/content/b.json"}});
        std::vector<registry::Batch> code;
        auto record = [&](const char* op, const char* source) {
            registry::Batch batch; std::vector<registry::Refusal> refused; std::string why;
            const std::string ir = std::string(R"({"ir":1,"mod":"a","stage":"alter","edits":[{"registry":"items","select":"game:material_t1_wood_logs","field":"maxStack","op":)") + op + "}]}";
            check(registry::parse_batch(ir, registry_schema(), &batch, &refused, &why) && refused.empty(), "parse recorded code batch");
            batch.source = source; code.push_back(std::move(batch));
        };
        if (queued) record(R"({"mul":2})", "code:a");
        record(R"({"add":5})", "code");
        stage(files, code); apply_registry_files(files);
        const uint32_t factor = queued ? 2 : 1;
        check(at<uint32_t>(WOOD_AT) == 40 * factor + 5, "start applies data, queued code, then the callback");
        g_files["b/content/b.json"] = R"({"registry":{"alter":[{"registry":"items","select":"game:material_stone","field":"maxStack","op":{"mul":3}}]}})";
        apply_registry_files(files);
        check(at<uint32_t>(WOOD_AT) == 40 * factor + 5, "an unrelated section reload keeps data before recorded code");
        g_files["a/content/a.json"] = R"({"registry":{"alter":[{"registry":"items","select":"game:material_t1_wood_logs","field":"maxStack","op":{"mul":6}}]}})";
        apply_registry_files(files);
        check(at<uint32_t>(WOOD_AT) == 60 * factor + 5, "a same-mod reload also keeps the callback last, exactly once");
        g_files["a/content/a.json"] = "{}"; apply_registry_files(files);
        check(at<uint32_t>(WOOD_AT) == 10 * factor + 5, "removing data leaves queued code and the callback exactly once");
        registry_live_owner_gone(77); registry_live_publish();
        check(at<uint32_t>(WOOD_AT) == 10 * factor + 5, "owner retirement does not discard already-taken start edits");
    }
}

void live_recipe_report() {
    world();
    g_files = {{"m/content/a.json", R"({"registry": {"alter": [
        {"registry": "recipes", "select": {"id": "game:plank_recipe"}, "field": "inputs[0].count", "op": {"add": 1}},
        {"registry": "recipes", "select": {"id": "game:plank_recipe"}, "field": "outputs", "list": {"replace": [{"item": "game:sword_t2", "count": 3}]}}
    ]}})"}};
    const auto files = files_of({{"m", "m/content/a.json"}});
    stage(files);
    const Entry& recipe = *reg(KILN_CONTENT_RECIPES).get(PLANK);
    const std::string source = "oldrecipes: content/a.json";
    set_owner_label(PACK, "pack oldrecipes");
    auto pack_lists = [&](uint32_t count) {
        std::string why;
        KilnContentStack input{}, output{};
        input.item = WOOD, input.count = count;
        output.item = SWORD, output.count = 7;
        check(set_recipe_stacks(PACK, kPackSeq, recipe, KILN_CONTENT_INPUTS, {input}, source, &why) == KILN_OK, "old recipe inputs: " + why);
        check(set_recipe_stacks(PACK, kPackSeq, recipe, KILN_CONTENT_OUTPUTS, {output}, source, &why) == KILN_OK, "old recipe outputs: " + why);
    };
    const registry::FieldRef inputs{"recipes", "game:plank_recipe", "inputs"}, outputs{"recipes", "game:plank_recipe", "outputs"};
    pack_lists(5);
    apply_registry_files(files);
    auto row = json_field(inputs);
    check(row["result"].at(0)["count"] == 6, "recipe input report uses the old pack's live count");
    check(row["writers"].size() == 2 && row["writers"][0]["kind"] == "oldPack" &&
              row["writers"][0]["who"] == source && row["writers"][0]["list"]["replace"][0]["count"] == 5 &&
              row["writers"][1]["value"][0]["count"] == 6, "recipe input writers use the live list");
    check(conflict(inputs, registry::LineKind::ComposedOn, source), "recipe input composedOn names the old pack");
    row = json_field(outputs);
    check(row["result"][0]["count"] == 3 && row["writers"][0]["value"][0]["count"] == 7, "recipe output replacement shows both lists");
    check(conflict(outputs, registry::LineKind::Overridden, source), "recipe output overridden names the old pack");
    store_drop_owner(PACK);
    pack_lists(9);
    apply_registry_files(files);
    check(json_field(inputs)["result"][0]["count"] == 10, "recipe input report follows an old pack reload");
    KilnContentStack code{};
    code.item = WOOD, code.count = 12;
    std::string why;
    check(set_recipe_stacks(CODE, next_code_seq(), recipe, KILN_CONTENT_INPUTS, {code}, "code recipe", &why) == KILN_OK,
          "code recipe input writer: " + why);
    apply_registry_files(files);
    row = json_field(inputs);
    check(row["result"][0]["count"] == 12 && row["winner"] == "code recipe" && row["writers"].back()["kind"] == "code" &&
              row["writers"][1]["value"][0]["count"] == 10, "code above the registry wins and both live list values are shown");
    check(!row.contains("resultEntriesLeftOut"), "a short result list has no omission count");
}

void live_formula_report() {
    world();
    g_files = {{"m/content/a.json", R"({"registry": {"alter": [
        {"registry": "items", "select": {"id": "game:sword_t2"}, "field": "power", "op": {"mul": 2}},
        {"registry": "items", "select": {"id": "game:sword_t2"}, "field": "armor.physical", "op": {"add": 4}}
    ]}})"}};
    const auto files = files_of({{"m", "m/content/a.json"}});
    stage(files);
    const std::string source = "oldgear: content/a.json";
    set_owner_label(PACK, "pack oldgear");
    const uint32_t level = 20;
    store_put(PACK, kPackSeq, KILN_CONTENT_ITEMS, SWORD, -1, -1, A(SWORD_AT + 0x10), 4,
              reinterpret_cast<const uint8_t*>(&level), source);
    apply_registry_files(files);
    const auto c = registry::formulas::constants_of({});
    for (const auto& field : {"power", "armor.physical"}) {
        const bool power = std::string(field) == "power";
        const registry::FieldRef ref{"items", "game:sword_t2", field};
        const auto row = json_field(ref);
        const int below = power ? registry::formulas::power_read(1, 1, level, c) : registry::formulas::armor_read(1, level, c);
        const int expected = power ? below * 2 : below + 4;
        check(row["result"] == expected, std::string(field) + " report uses the live formula result");
        const registry::json inputs = power ? registry::json::array({at<float>(SWORD_AT + 8), 1, level})
                                            : registry::json::array({at<float>(SWORD_AT + 0x90), level});
        check(row["formulaInputs"] == inputs, std::string(field) + " report includes the effective inputs");
        check(row["writers"].size() == 2 && row["writers"][0]["who"] == source && row["writers"][0]["value"] == below &&
                  row["writers"][1]["value"] == expected, std::string(field) + " writers use live formula values");
        check(conflict(ref, registry::LineKind::ComposedOn, source), std::string(field) + " composedOn names the old pack");
    }
}

void report_caps_lists() {
    registry::Result result;
    auto& field = result.fields[{"drops", "game:dt_wolf", "entries"}];
    std::vector<registry::Value> entries;
    for (int i = 0; i < 70; ++i) entries.push_back(registry::Value::integer(i));
    field.base = field.result = registry::Value::of_list(entries);
    auto& writer = field.chain.emplace_back();
    writer.after = field.result;
    writer.list = registry::ListOp::Replace;
    writer.list_values = entries;
    auto& set_writer = field.chain.emplace_back();
    set_writer.after = field.result;
    set_writer.op.set = true;
    set_writer.op.value = field.result;
    const auto doc = registry::report_json(registry_schema(), result, {});
    const auto& row = doc["fields"][0];
    check(row["result"].size() == 64 && row["result"][63] == 63 && row["resultEntriesLeftOut"] == 6,
          "long result list keeps its first 64 entries and counts the rest");
    check(row["base"]["value"].size() == 64 && row["base"]["entriesLeftOut"] == 6, "long base list is capped");
    check(row["writers"][0]["value"].size() == 64 && row["writers"][0]["valueEntriesLeftOut"] == 6, "long writer value is capped");
    check(row["writers"][0]["list"]["replace"].size() == 64 && row["writers"][0]["listEntriesLeftOut"] == 6, "long list operand is capped");
    check(row["writers"][1]["op"].size() == 64 && row["writers"][1]["opEntriesLeftOut"] == 6, "long list passed to op is capped");
}

// a relative registry edit over the game's value, over a pack's value, re-evaluated when the pack is applied again
void relative_over_packs_and_reload() {
    world();
    g_files = {{"bigstacks/content/a.json",
                R"({"registry": {"alter": [{"registry": "items", "select": {"id": "game:material_t1_wood_logs"}, "field": "maxStack", "op": {"mul": 4}}]}})"}};
    const auto files = files_of({{"bigstacks", "bigstacks/content/a.json"}});
    stage(files);
    apply_registry_files(files);
    check(at<uint32_t>(WOOD_AT) == 40, "x4 over the game's 10: " + std::to_string(at<uint32_t>(WOOD_AT)));
    // world load with an old pack (apply_all_packs: packs first, then the registry)
    put_pack(WOOD_AT, 30);
    apply_registry_files(files);
    check(at<uint32_t>(WOOD_AT) == 120, "x4 over the pack's 30");
    {
        const registry::FieldOutcome& fo = g_last_report.fields.at({"items", "game:material_t1_wood_logs", "maxStack"});
        check(fo.result == registry::Value::integer(120), "registry.json at world load: the live result 120 (" + fo.result.text_of() + ")");
        check(fo.chain.size() == 2 && fo.chain[0].kind == registry::WriterKind::OldPack && fo.chain[0].who == "oldstacks: content/a.json" &&
                  fo.chain[0].after == registry::Value::integer(30),
              "registry.json at world load: the old pack is a writer below the registry");
        bool line = false;
        for (const registry::Line& l : g_last_report.lines)
            line = line || (l.kind == registry::LineKind::ComposedOn && l.winner == "bigstacks" && l.loser == "oldstacks: content/a.json");
        check(line, "registry.json at world load: composedOn names the old pack");
    }
    store_drop_owner(PACK);  // the pack edited: a reload
    put_pack(WOOD_AT, 50);
    apply_registry_files(files);
    check(at<uint32_t>(WOOD_AT) == 200, "x4 over the reloaded pack's 50");
    check(g_reports == 3 && g_last_report.refused.empty(), "registry.json written at every application, nothing refused");
    uint8_t b[4] = {7, 0, 0, 0};
    store_put(CODE, next_code_seq(), KILN_CONTENT_ITEMS, WOOD, 0, -1, A(WOOD_AT), 4, b, "code");
    store_flush(nullptr);
    check(at<uint32_t>(WOOD_AT) == 7, "code wins over the registry");
    const auto cs = store_conflicts();
    bool named = false;
    for (const Conflict& c : cs) named = named || c.loser.find("bigstacks: content/a.json (registry items[game:material_t1_wood_logs].maxStack)") == 0;
    check(named, "conflicts name the registry layer's mod, file and field");
}

// set values of every live kind: enum (Kiln name -> the game's), time, formula (power), recipe lists, drop-entry leaves
void every_live_kind() {
    world();
    g_files = {{"m/content/a.json", R"({"registry": {"alter": [
        {"registry": "items", "select": {"id": "game:material_t1_wood_logs"}, "field": "rarity", "op": "epic"},
        {"registry": "items", "select": {"id": "game:sword_t2"}, "field": "power", "op": {"mul": 2}},
        {"registry": "recipes", "select": {"id": "game:plank_recipe"}, "field": "duration", "op": 5},
        {"registry": "recipes", "select": {"id": "game:plank_recipe"}, "field": "inputs", "list": {"append": [{"item": "game:material_stone", "count": 3}]}},
        {"registry": "recipes", "select": {"id": "game:plank_recipe"}, "field": "outputs[0].count", "op": {"add": 1}},
        {"registry": "drops", "select": {"id": "game:dt_wolf"}, "field": "entries[*].count.max", "op": {"mul": 2}}
    ]}})"}};
    const auto files = files_of({{"m", "m/content/a.json"}});
    stage(files);
    apply_registry_files(files);
    check(at<uint32_t>(WOOD_AT + 4) == 3, "rarity epic -> the game's Epic (3)");
    check(at<int64_t>(PLANK_AT) == 5'000'000'000ll, "duration 5 s");
    const gear::Constants c = registry::formulas::constants_of({});
    double p0 = 0, p1 = 0;
    registry::formulas::read("gear.power", {1.0, 1.0, 10}, c, &p0);
    registry::formulas::read("gear.power", {at<float>(SWORD_AT + 8), at<float>(SWORD_AT + 0x0C), 10}, c, &p1);
    check(std::llround(p1) == 2 * std::llround(p0), "power x2 through the formula: " + std::to_string(p0) + " -> " + std::to_string(p1));
    // inputs: wood x2, stone x3
    const int32_t off = at<int32_t>(PLANK_AT + 0x40);
    const uint32_t n = at<uint32_t>(PLANK_AT + 0x44);
    check(n == 2, "two inputs now");
    const size_t list = PLANK_AT + 0x40 + off;
    check(at<uint32_t>(list + RL.in_item) == WOOD && at<uint32_t>(list + RL.in_count) == 2, "first input unchanged");
    check(at<uint32_t>(list + RL.in_stride + RL.in_item) == STONE && at<uint32_t>(list + RL.in_stride + RL.in_count) == 3, "stone x3 appended");
    check(std::memcmp(g_arena + list + RL.in_stride + RL.in_item_ref, G4, 16) == 0, "with the stone's GUID");
    const size_t olist = PLANK_AT + 0x48 + at<int32_t>(PLANK_AT + 0x48);
    check(at<uint32_t>(olist + RL.out_count) == 2, "outputs[0].count + 1");
    // the drop table is not loaded yet: its layers wait for it
    check(at<uint32_t>(0x31018) == 3, "no table yet");
    add_drop_table();
    registry_live_table(g_regs[KILN_CONTENT_DROPS].entries[0]);
    store_flush(nullptr);
    check(at<uint32_t>(0x31018) == 6 && at<uint32_t>(0x31058) == 6, "every stack's count.max x2 once the table appears");
    check(at<uint32_t>(0x31014) == 1, "count.min unchanged");
    check(g_last_report.refused.empty(), "nothing refused");
}

// perks (refList: references and ids in step), manaCost (impactConfig, its tooltip follows), unlock ("always" clears the
// knowledge requirement, "game" keeps it), loot stack sizes
void perks_costs_unlock_loot() {
    world();
    g_registry_type_size = [](const char* q) -> uint32_t { return std::string(q) == "keen::KnowledgeQueryData" ? 12u : 0u; };
    g_files = {{"m/content/a.json", R"({"registry": {"alter": [
        {"registry": "items", "select": {"id": "game:material_t1_wood_logs"}, "field": "perks", "list": {"append": ["game:perk_b"]}},
        {"registry": "items", "select": {"id": "game:sword_t2"}, "field": "manaCost", "op": {"mul": 1.5}},
        {"registry": "items", "select": {"id": "game:material_t1_wood_logs"}, "field": "staminaCost", "op": 3},
        {"registry": "recipes", "select": {"id": "game:plank_recipe"}, "field": "unlock", "op": "always"},
        {"registry": "loot", "select": {"id": "game:material_t1_wood_logs"}, "field": "stack.max", "op": {"mul": 2}}
    ]}})"}};
    const auto files = files_of({{"m", "m/content/a.json"}});
    stage(files);
    const std::string source = "oldperks: content/a.json";
    set_owner_label(PACK, "pack oldperks");
    const uint8_t empty[16] = {};
    store_put(PACK, kPackSeq, KILN_CONTENT_ITEMS, WOOD, -1, -1, A(WOOD_AT + 0x30), 16, empty, source);
    store_put(PACK, kPackSeq, KILN_CONTENT_ITEMS, WOOD, -1, -1, A(WOOD_AT + 0x40), 16, P1, source);
    apply_registry_files(files);
    for (const auto& r : g_last_report.refused) std::fprintf(stderr, "  refused: %s\n", r.reason.c_str());
    check(std::memcmp(g_arena + WOOD_AT + 0x30, P1, 16) == 0 && at<uint32_t>(WOOD_AT + 0x80) == 0x901, "perk slot 0 kept");
    check(std::memcmp(g_arena + WOOD_AT + 0x40, P2, 16) == 0 && at<uint32_t>(WOOD_AT + 0x84) == 0x902, "perk_b appended into slot 1, its id with it");
    check(at<float>(SWORD_AT + 0x40) == 15.0f, "manaCost x1.5: 15");
    check(at<int32_t>(SWORD_AT + 0x44) == 15, "its tooltip row follows: 15");
    check(reported("items[game:material_t1_wood_logs].staminaCost: the item has no such configuration"), "staminaCost on an item without the config: refused, loud");
    check(at<uint32_t>(PLANK_AT + 0x50) == 0 && at<uint32_t>(PLANK_AT + 0x58) == 0, "unlock always: the knowledge requirement cleared");
    check(at<uint32_t>(0x40004) == 10, "loot stack.max x2: 10");
    const registry::FieldRef perks{"items", "game:material_t1_wood_logs", "perks"};
    const auto row = json_field(perks);
    check(row["result"] == registry::json::array({"game:perk_a", "game:perk_b"}) &&
              row["writers"].size() == 2 && row["writers"][0]["who"] == source &&
              row["writers"][0]["value"] == registry::json::array({"game:perk_a"}), "perk reference slots report one writer and one list");
    store_drop_owner(PACK);
    // back to the game's: the section removed on a reload
    g_files["m/content/a.json"] = R"({"registry": {"alter": [{"registry": "recipes", "select": {"id": "game:plank_recipe"}, "field": "unlock", "op": "game"}]}})";
    apply_registry_files(files);
    check(at<uint32_t>(PLANK_AT + 0x50) == 0x12345678 && at<uint32_t>(PLANK_AT + 0x58) == 7, "unlock game: the game's requirement again");
    check(std::memcmp(g_arena + WOOD_AT + 0x40, "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0", 16) == 0 && at<uint32_t>(WOOD_AT + 0x84) == 0, "perk slot 1 back to none");
    check(at<float>(SWORD_AT + 0x40) == 10.0f && at<int32_t>(SWORD_AT + 0x44) == 10, "manaCost and its tooltip back");
    check(at<uint32_t>(0x40004) == 5, "loot back");
}

// fields the live back-end does not take are refused loudly; restart fields are not touched and not refused
void refusals_and_restart_fields() {
    world();
    g_files = {{"m/content/a.json", R"({"registry": {"alter": [
        {"registry": "items", "select": {"id": "game:material_t1_wood_logs"}, "field": "maxStack", "op": 99999},
        {"registry": "balance", "select": {"id": "*"}, "field": "levelCap", "op": 30}
    ]}})"}};
    const auto files = files_of({{"m", "m/content/a.json"}});
    stage(files);
    apply_registry_files(files);
    for (const auto& r : g_last_report.refused) std::fprintf(stderr, "  refused: %s\n", r.reason.c_str());
    check(!reported("levelCap"), "a restart field is the boot back-end's: not refused here");
    check(at<uint32_t>(WOOD_AT) == 10, "an out-of-range value is not written");
}

// a changed section is composed again; a removed pack's registry layer leaves nothing behind; a new field needs a restart
void hot_reload() {
    world();
    g_files = {{"a/content/a.json", R"({"registry": {"alter": [{"registry": "items", "select": {"id": "game:material_t1_wood_logs"}, "field": "maxStack", "op": {"mul": 4}}]}})"},
               {"b/content/b.json", R"({"registry": {"alter": [{"registry": "items", "select": {"id": "game:material_t1_wood_logs"}, "field": "maxStack", "op": {"mul": 2}}]}})"}};
    auto files = files_of({{"a", "a/content/a.json"}, {"b", "b/content/b.json"}});
    stage(files);
    apply_registry_files(files);
    check(at<uint32_t>(WOOD_AT) == 80, "x4 then x2 stack: 80");
    // b's section edited: x2 -> x3
    g_files["b/content/b.json"] = R"({"registry": {"alter": [{"registry": "items", "select": {"id": "game:material_t1_wood_logs"}, "field": "maxStack", "op": {"mul": 3}}]}})";
    apply_registry_files(files);
    check(at<uint32_t>(WOOD_AT) == 120, "recomposed: x4 then x3 = 120");
    const auto r = registry_live_result();
    check(r && r->fields.count({"items", "game:material_t1_wood_logs", "maxStack"}) &&
              r->fields.at({"items", "game:material_t1_wood_logs", "maxStack"}).result == registry::Value::integer(120),
          "registry.json has the new result");
    // a's pack removed
    files = files_of({{"b", "b/content/b.json"}});
    apply_registry_files(files);
    check(at<uint32_t>(WOOD_AT) == 30, "a removed pack's x4 leaves nothing behind: x3 over 10");
    // b removed too: back to the game's value, no registry layer left
    files.clear();
    apply_registry_files(files);
    check(at<uint32_t>(WOOD_AT) == 10, "no residue");
    check(!owner_has_layers(registry_owner()), "no registry layer left");
    // a new field the start never decoded needs a restart
    files = files_of({{"b", "b/content/b.json"}});
    g_files["b/content/b.json"] = R"({"registry": {"alter": [{"registry": "items", "select": {"id": "game:material_t1_wood_logs"}, "field": "levels.min", "op": 5}]}})";
    apply_registry_files(files);
    check(reported("needs a restart"), "a field the start never decoded: refused, needs a restart");
}

// compare-before-write in the report: another writer's value stays
void another_writer_in_the_report() {
    world();
    g_files = {{"bigstacks/content/a.json",
                R"({"registry": {"alter": [{"registry": "items", "select": {"id": "game:material_t1_wood_logs"}, "field": "maxStack", "op": {"mul": 4}}]}})"}};
    const auto files = files_of({{"bigstacks", "bigstacks/content/a.json"}});
    stage(files);
    apply_registry_files(files);
    mem<uint32_t>(WOOD_AT, 777);  // a ShroudForge run-time write in kiln mode
    apply_registry_files(files);   // a pack reload
    check(at<uint32_t>(WOOD_AT) == 777, "the other writer's value stays");
    check(reported("was written by another writer after Kiln's layer: Kiln's edit not applied"), "and registry.json says so");
    check(json_field({"items", "game:material_t1_wood_logs", "maxStack"})["result"] == 777, "the report shows the other writer's effective value");
}
// P2: an explicit retained live change, reverted by handle, must compose over packs and never overtake code.
void live_transactions() {
    world();
    g_files.clear();
    stage({});
    registry_live_world_begin();
    apply_registry_files({});
    uint64_t h = 0;
    RegistryLiveReply reply;
    check(registry_live_begin(77, "change", 4, &h) == KILN_OK && h, "begin an owner-scoped live transaction");
    const std::string ir = R"({"ir":1,"mod":"change","stage":"alter","edits":[{"registry":"items","select":"game:material_t1_wood_logs","field":"maxStack","op":{"mul":3}}]})";
    check(registry_live_submit(77, h, ir, &reply, nullptr) == KILN_OK, "submit Kiln fields");
    check(registry_live_commit(77, h, &reply) == KILN_OK && reply.outcome == RegistryLiveOutcome::Applied, "commit the live transaction");
    check(at<uint32_t>(WOOD_AT) == 30, "the live change writes through the real store");
    check(registry::report_json(registry_schema(), g_last_report, {}).dump().find("live:") != std::string::npos, "the live owner and transaction are in the report");
    check(registry_live_revert(78, h, &reply) == KILN_ERR_INVALID_ARGUMENT, "another owner cannot revert it");
    check(registry_live_revert(77, h, &reply) == KILN_OK && at<uint32_t>(WOOD_AT) == 10, "revert restores the layer below");
    check(registry_live_revert(77, h, &reply) == KILN_ERR_NOT_FOUND, "revert does not reuse stale handles");
}

void live_transaction_foreign_revert() {
    world(); g_files.clear(); stage({}); registry_live_world_begin(); apply_registry_files({});
    uint64_t h = 0; RegistryLiveReply reply;
    registry_live_begin(77, "change", 4, &h);
    registry_live_submit(77, h, R"({"ir":1,"mod":"change","stage":"alter","edits":[{"registry":"items","select":"game:material_t1_wood_logs","field":"maxStack","op":90}]})", &reply, nullptr);
    registry_live_commit(77, h, &reply);
    mem<uint32_t>(WOOD_AT, 777);
    check(registry_live_revert(77, h, &reply) == KILN_OK && reply.outcome == RegistryLiveOutcome::Skipped, "a foreign write makes the revert explicitly skipped");
    check(at<uint32_t>(WOOD_AT) == 777, "revert never overwrites the foreign writer");
}

void live_transaction_points_and_revoke() {
    world(); g_files.clear(); stage({});
    uint64_t h = 0; RegistryLiveReply reply;
    registry_live_begin(77, "change", 1, &h);
    registry_live_submit(77, h, R"({"ir":1,"mod":"change","stage":"alter","edits":[{"registry":"items","select":"game:material_t1_wood_logs","field":"maxStack","op":90}]})", &reply, nullptr);
    check(registry_live_commit(77, h, &reply) == KILN_OK && reply.outcome == RegistryLiveOutcome::PendingWorld, "mod-load changes can wait for the first world");
    check(at<uint32_t>(WOOD_AT) == 10, "no premature game write before world readiness");
    registry_live_world_begin(); apply_registry_files({});
    check(at<uint32_t>(WOOD_AT) == 90, "the queued mod-load change applies at world load");
    registry_live_owner_gone(77); registry_live_publish();
    check(at<uint32_t>(WOOD_AT) == 10, "revoked live owners lose their live layers on the world thread");
    check(registry_live_begin(77, "change", 2, &h) == KILN_ERR_REVOKED, "a revoked owner cannot create more transactions");
    registry_live_begin(88, "later", 3, &h);
    registry_live_submit(88, h, R"({"ir":1,"mod":"later","stage":"alter","edits":[{"registry":"balance","select":"game:balance","field":"levelCap","op":60}]})", &reply, nullptr);
    check(registry_live_commit(88, h, &reply) == KILN_OK && reply.outcome == RegistryLiveOutcome::RestartRequired, "a restart field is never silently written from hot reload");
}
}  // namespace

int main() {
    std::vector<std::string> errs;
    g_regmap = std::make_shared<registry::map::RegMap>();
    if (!registry::map::load_dir(KILN_REGMAP_DIR, "server", "f0d824be7fcd48ebfe2458ff5c28da26060c2074e3ec1fcac587e5a1d508aeb2", g_regmap.get(), &errs) || !g_regmap->has_build) {
        std::printf("FAIL cannot load the regmap at %s\n", KILN_REGMAP_DIR);
        return 1;
    }
    const std::pair<const char*, void (*)()> tests[] = {
        {"relative_over_packs_and_reload", relative_over_packs_and_reload},
        {"callback_order_survives_reloads", callback_order_survives_reloads},
        {"live_recipe_report", live_recipe_report},
        {"live_formula_report", live_formula_report},
        {"report_caps_lists", report_caps_lists},
        {"every_live_kind", every_live_kind},
        {"perks_costs_unlock_loot", perks_costs_unlock_loot},
        {"refusals_and_restart_fields", refusals_and_restart_fields},
        {"hot_reload", hot_reload},
        {"another_writer_in_the_report", another_writer_in_the_report},
        {"live_transactions", live_transactions},
        {"live_transaction_foreign_revert", live_transaction_foreign_revert},
        {"live_transaction_points_and_revoke", live_transaction_points_and_revoke},
    };
    int failed = 0;
    for (auto& [name, fn] : tests) {
        try {
            fn();
            std::printf("ok   %s\n", name);
        } catch (const std::exception& e) {
            ++failed;
            std::printf("FAIL %s: %s\n", name, e.what());
        }
    }
    std::printf("registry live: %d checks, %d failed tests\n", checks, failed);
    return failed ? 1 : 0;
}
