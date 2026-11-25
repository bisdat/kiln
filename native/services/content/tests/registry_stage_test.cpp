// SPDX-License-Identifier: MIT
// kiln.content registry stage: the stage runner (registry/stage.cpp, slice S11) and the boot back-end's bridge
// (registry/boot_bridge.cpp, slice S10) on the host, with ASan/UBSan, over a fake tree view of the game's data and an
// in-memory data folder: a mod's data section in load order, refusals of fields this build does not map (loud, in
// registry.json and the log), an old boot patch below a registry edit of the same field (the registry wins, reported), a
// game recipe removed with the item it needs, registry.json and its phases, the id table and the plan written to the data
// folder, a second start from the cache giving the same registry.json, Kiln.Host's order, a native stage callback's edits,
// the registry's writes on a resource's bytes after the old documents.
#include <cstdio>
#include <cstring>
#include <stdexcept>

#include "services/kiln_caps.h"
#include "../registry/boot_hooks.h"
#include "../registry/schema_ir.h"
#include "../registry/stage.h"

namespace kc {
std::string guid_text(const uint8_t g[16]);
bool guid_parse(const std::string& s, uint8_t out[16]);
namespace rbtest {
void balance_type();  // keen::BalancingTable {playerLevelCap u32 +0, skillPointsPerLevel u32 +4}, 8 bytes
}
}  // namespace kc

namespace {
using namespace kc::registry;
int checks = 0;
void check(bool ok, const std::string& why) {
    if (!ok) throw std::runtime_error(why);
    ++checks;
}
std::string g(int n) {
    char b[40];
    std::snprintf(b, sizeof b, "%08x-0000-4000-8000-%012x", n, n);
    return b;
}
KilnAssetKey key_of(const std::string& guid, const std::string& type) {
    KilnAssetKey k{};
    kc::guid_parse(guid, k.guid);
    k.type = kiln::boot_gd::fnv1a32(type);
    return k;
}
const char* kBalance = "82706b40-61b1-4b8f-8b23-dcec6971bda1";
const std::string kExe = "f0d824be7fcd48ebfe2458ff5c28da26060c2074e3ec1fcac587e5a1d508aeb2";
const std::string kClientExe = "5d9ef2f140b8ed49e30dbee655841b89d08baec20f799a1817e171a18a850c77";

void registry_caps(const std::string& dir) {
    struct Row {
        uint32_t state, reason;
        std::string detail;
    };
    using Rows = std::map<std::string, Row>;
    constexpr std::string_view name_chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._/-";
    auto run = [&](const std::string& side, const std::string& exe,
                   const std::function<void(map::RegMap&)>& adjust = {}) {
        Rows rows;
        stage::Env e;
        e.side = side;
        e.exe_sha256 = exe;
        unsigned loads = 0;
        e.regmap = [&](map::RegMap* rm, std::vector<std::string>* errs) {
            ++loads;
            const bool ok = map::load_dir(dir, side, exe, rm, errs);
            if (adjust) adjust(*rm);
            return ok;
        };
        e.report_cap = [&](const std::string& name, uint32_t state, uint32_t reason, const std::string& detail) {
            check(loads == 1, "reports use the loaded regmap");
            check(!name.empty() && name.size() <= KILN_CAP_NAME_MAX && name.find_first_not_of(name_chars) == std::string::npos,
                  "valid kiln.caps name: " + name);
            check(rows.size() < KILN_CAP_OWNER_MAX, "fits kiln.caps owner limit");
            check(rows.emplace(name, Row{state, reason, detail}).second, "row reported once: " + name);
            return KILN_OK;
        };
        stage::Runner runner(std::move(e));
        // Schema queries can load the regmap before resources-ready.
        if (side == "server" && exe == kExe) runner.schema_json();
        runner.run({});
        runner.run({});
        check(rows.size() == std::size(gen::kRegistries) + std::size(gen::kFields), "one capability per registry and field");
        return rows;
    };
    const auto server = run("server", kExe);
    for (const auto& r : gen::kRegistries)
        check(server.at(std::string("registry.") + r.name).state == KILN_CAP_AVAILABLE, "verified registry: " + std::string(r.name));
    for (const auto& f : gen::kFields) {
        std::string path = f.path;
        if (const auto pos = path.find("[]"); pos != std::string::npos) path.erase(pos, 2);
        const auto& row = server.at(std::string("registry.") + f.registry + "." + path);
        const char* applies = f.applies == gen::Applies::Now ? "now" : f.applies == gen::Applies::WorldLoad ? "worldLoad" : "restart";
        check(row.detail.find(std::string("applies: ") + applies) != std::string::npos, "schema applies in " + path + " detail");
    }
    auto expect = [&](const Rows& rows, const char* name, uint32_t reason, const char* detail) {
        const Row& row = rows.at(name);
        check(row.state == (reason == KILN_CAP_REASON_NONE ? KILN_CAP_AVAILABLE : KILN_CAP_UNAVAILABLE) &&
                  row.reason == reason && row.detail.find(detail) != std::string::npos,
              std::string(name) + " reports " + detail + ": " + row.detail);
    };
    expect(server, "registry.items.power", KILN_CAP_REASON_NONE, "applies: now");
    expect(server, "registry.items.maxStack", KILN_CAP_REASON_NONE, "applies: worldLoad");
    expect(server, "registry.balance.levelCap", KILN_CAP_REASON_NONE, "applies: restart");
    expect(server, "registry.recipes.inputs.count", KILN_CAP_REASON_NONE, "applies: now");
    expect(server, "registry.items.fuel.burnTime", KILN_CAP_REASON_SYMBOL_MISSING, "unverified");
    expect(server, "registry.recipes.unlock", KILN_CAP_REASON_WRONG_SIDE, "client");

    const auto client = run("client", kClientExe);
    expect(client, "registry.recipes.unlock", KILN_CAP_REASON_NONE, "applies: now");
    const auto missing_client_unlock = run("client", kClientExe, [](map::RegMap& rm) {
        rm.build.rows.erase("recipes.unlock");
    });
    expect(missing_client_unlock, "registry.recipes.unlock", KILN_CAP_REASON_SYMBOL_MISSING, "unmapped");

    const auto unknown = run("server", std::string(64, '0'));
    expect(unknown, "registry.items", KILN_CAP_REASON_POLICY, "unverified");
    expect(unknown, "registry.items.power", KILN_CAP_REASON_POLICY, "unverified");
    const auto unverified_build = run("server", kExe, [](map::RegMap& rm) { rm.build.verified = false; });
    expect(unverified_build, "registry.items.power", KILN_CAP_REASON_POLICY, "unverified");
    // review3 S4: a record proven on another regmap/common enables nothing and is not called a verified one
    const auto other_mapping = run("server", kExe, [](map::RegMap& rm) { rm.build.regmap_digest = std::string(64, 'b'); });
    expect(other_mapping, "registry.items.power", KILN_CAP_REASON_POLICY, "unverified regmap record");
    expect(other_mapping, "registry.items", KILN_CAP_REASON_POLICY, "unverified regmap record");
    const auto incomplete = run("server", kExe, [](map::RegMap& rm) {
        rm.build.rows["items.power"] = map::BuildRow::Changed;
        rm.registries.erase("balance");
    });
    expect(incomplete, "registry.items.power", KILN_CAP_REASON_SYMBOL_MISSING, "unmapped");
    expect(incomplete, "registry.items", KILN_CAP_REASON_NONE, "verified");
    expect(incomplete, "registry.balance", KILN_CAP_REASON_SYMBOL_MISSING, "unmapped");
    expect(incomplete, "registry.balance.levelCap", KILN_CAP_REASON_SYMBOL_MISSING, "unmapped");
}

struct TreeView final : kiln::boot_view::BaseView {
    std::map<std::string, json> values;
    std::map<std::string, std::vector<std::string>> types;
    std::set<std::string> served;
    bool released = false;
    const char* source() const override { return "live"; }
    bool ready(std::string*) override { return true; }
    std::string digest() override {
        std::string d;
        for (const std::string& s : served) d += s + values[s].dump();
        return kiln::boot_gd::hex_of(kiln::boot_gd::sha256(d));
    }
    KilnResult tree(std::string_view q, size_t cap, size_t, uint64_t, std::string* out, size_t* req, size_t*, std::string* why) override {
        const std::string guid(q.substr(9, 36));
        const auto it = values.find(guid);
        if (it == values.end()) return *why = "none", KILN_ERR_NOT_FOUND;
        const std::string text = json{{"value", it->second}}.dump();
        if (text.size() + 1 > cap) return *req = text.size() + 1, KILN_ERR_BUFFER_TOO_SMALL;
        served.insert(guid);
        *out = text;
        return KILN_OK;
    }
    KilnResult bytes(const KilnAssetKey&, size_t, std::string*, std::string* why) override { return *why = "trees", KILN_ERR_UNAVAILABLE; }
    void release() override { released = true; }
    void add(const std::string& type, const std::string& guid, json v) {
        types[type].push_back(guid);
        values[guid] = std::move(v);
    }
};

TreeView world() {
    TreeView v;
    auto item = [&](int n, const std::string& name, int stack) {
        v.add("keen::ItemInfo", g(n), {{"itemId", 1000 + n}, {"objectId", g(n)}, {"debugName", name}, {"maxStackSize", stack}, {"rarity", "Common"},
                                       {"damageSetup", {{"dmgMod", 0.0}, {"speedMod", 1.0}, {"isSet", false}}}, {"itemLevelRange", {{"minLevel", 1}, {"maxLevel", 1}}}});
    };
    item(1, "Material_T1_Wood_Logs", 10);
    item(2, "Plank", 20);
    json in = {{"itemStack", {{"item", 1001}, {"itemRef", g(1)}, {"count", 3}}}, {"inputItemCategory", {{"category", 0}, {"categoryRef", "none"}, {"count", 0}}}};
    v.add("keen::RecipeRegistryResource", g(100),
          {{"recipes", json::array({{{"debugName", "Craft_Planks"}, {"recipeGuid", g(101)}, {"recipeId", 5001}, {"input", json::array({in})},
                                     {"output", json::array({{{"item", 1002}, {"itemRef", g(2)}, {"count", 1}}})}, {"craftingDuration", 2.0}}})},
           {"inputCategories", json::array()}});
    v.add("keen::LootableItemsResource", g(200), {{"items", json::array({{{"itemId", 1002}, {"stackSizeMin", 1}, {"stackSizeMax", 2}}})}});
    v.add("keen::ecs::DefaultInventoryResource", g(300), {{"rootGroup", {{"stacks", json::array({{{"item", 1002}, {"itemRef", g(2)}, {"countMin", 1}, {"countMax", 1}, {"spawnProbability", 1.0}, {"weight", 1.0}}})}}}});
    v.add("keen::ecs::TemplateResource", g(400), {{"name", "Chest_A"}, {"components", json::array({{{"$type", "DefaultInventory"}, {"defaultInventory", g(300)}}})}});
    v.add("keen::BalancingTable", kBalance, {{"playerLevelCap", 50}, {"skillPointsPerLevel", 2}, {"genericWeaponSeed", 10.0}, {"powerCreepWeapon", 0.15},
                                             {"armorSetup", {{"offset", 23.0}, {"gradient", 15.0}}}});
    return v;
}

struct Files {
    std::map<std::string, std::string> data;
    std::string report;
    std::vector<std::string> log;
};

stage::Env env_of(TreeView* view, Files* files, const std::string& regmap_dir, std::vector<stage::Env::DataFile> (*data)(const Schema&)) {
    stage::Env e;
    e.side = "server", e.exe_sha256 = kExe, e.kiln_version = "0.2.0-test", e.pid = 7;
    e.now = [] { return std::string("2026-11-21T00:00:00Z"); };
    e.source = [view](std::string*) {
        return model::tree_source(*view, [view](const std::string& type, std::vector<KilnAssetKey>* out, std::string*) {
            for (const std::string& gu : view->types[type]) out->push_back(key_of(gu, type));
            return true;
        });
    };
    e.regmap = [regmap_dir](map::RegMap* out, std::vector<std::string>* errs) { return map::load_dir(regmap_dir, "server", kExe, out, errs); };
    e.data = data;
    e.folders = [] { return std::vector<std::pair<std::string, std::string>>{{"oldmod", "1.0.0"}, {"bigmod", "2.0.0"}, {"late", "1.0.0"}}; };
    e.write_report = [files](const std::string& t) { return files->report = t, true; };
    e.write_data = [files](const std::string& rel, const std::string& t) { return files->data[rel] = t, true; };
    e.read_data = [files](const std::string& rel) -> std::optional<std::string> {
        const auto it = files->data.find(rel);
        if (it == files->data.end()) return std::nullopt;
        return it->second;
    };
    e.log = [files](int level, const std::string& l) { files->log.push_back(std::to_string(level) + " " + l); };
    return e;
}

std::vector<stage::Env::DataFile> sections(const Schema& schema) {
    const std::string big = R"({"registry": {"alter": [
        {"registry": "items", "select": {"id": "game:plank"}, "field": "maxStack", "op": {"mul": 4}},
        {"registry": "items", "select": {"id": "game:plank"}, "field": "fuel.burnTime", "op": 30},
        {"registry": "balance", "select": {"id": "game:balance"}, "field": "levelCap", "op": 60},
        {"registry": "items", "entry": "remove", "select": {"id": "game:material_t1_wood_logs"}}]}})";
    const std::string late = R"({"registry": {"alter": [{"registry": "items", "select": {"id": "game:plank"}, "field": "maxStack", "op": {"mul": 2}}]}})";
    auto out = stage::read_sections("bigmod", "2.0.0", {{"content/b.json", big}, {"content/a.json", "{\"items\": {}}"}}, schema);
    auto l = stage::read_sections("late", "1.0.0", {{"content/x.json", late}}, schema);
    out.insert(out.end(), l.begin(), l.end());
    return out;
}

std::vector<boothook::OldDoc> old_patch() {
    boothook::OldDoc d;
    d.mod = "oldmod", d.key = "cap";
    d.res = key_of(kBalance, "keen::BalancingTable");
    d.type = "keen::BalancingTable";
    d.sets = {{"playerLevelCap", 55}, {"skillPointsPerLevel", {{"mul", 2}}}};
    return {d};
}

void one_start(const std::string& dir) {
    TreeView view = world();
    Files files;
    stage::Runner r(env_of(&view, &files, dir, &sections));
    std::function<void(const Result&, bool)> live_report;
    const Result* live_result = nullptr;
    r.set_live([&](const stage::LiveHandoff& h) { live_report = h.report, live_result = h.result; });
    check(r.phase() == stage::Phase::None, "phase none before the stage");
    const auto docs = old_patch();
    const auto boot = r.run([&](model::Model& m) { return boothook::old_writes(docs, m); });
    check(boot.size() == 1 && boot[0].type == "keen::BalancingTable" && boot[0].fields.size() == 1 && boot[0].fields[0].field.field == "levelCap",
          "the boot back-end gets the balance table's levelCap (restart) only");
    check(view.released, "the base view is released once the base is decoded");
    r.finish({{boot[0].fields[0].field, true, Value::integer(60), ""}});
    check(r.phase() == stage::Phase::Start && r.generation() == 1, "phase start, generation 1");
    const json rep = json::parse(files.report);
    check(rep["phase"] == "start" && rep["orderSource"] == "folders" && rep["regmap"]["verified"] == true, "registry.json: phase, order source, the verified record");
    check(rep["order"].size() == 2 && rep["order"][0]["mod"] == "bigmod" && rep["order"][1]["mod"] == "late", "the order: the mod folders' (late after bigmod)");
    const std::string text = rep.dump();  // compact: the file itself is indented
    check(text.find("\"field\":\"maxStack\"") != std::string::npos && text.find("\"result\":160") != std::string::npos, "maxStack: x4 then x2 stack (20 -> 160)");
    check(text.find("fuel.burnTime") != std::string::npos && text.find("Unproven") != std::string::npos, "an unproven field's edit is refused with its reason: " + text.substr(0, 400));
    bool loud = false;
    for (const std::string& l : files.log) loud = loud || (l.rfind("1 ", 0) == 0 && l.find("fuel.burnTime") != std::string::npos);
    check(loud, "and said in the log as a warning");
    // review finding 1 (ruling 2026-11-21): entry add and remove have no back-end in this release: refused per edit, loud, and
    // nothing is removed with them (a game recipe needing the item stays)
    check(text.find("entry add and remove have no back-end in this release") != std::string::npos, "the entry remove is refused with its reason");
    check(rep["removed"].empty() && text.find("recipes game:craft_planks removed") == std::string::npos, "nothing is removed: " + rep["removed"].dump());
    bool warned = false;
    for (const std::string& l : files.log) warned = warned || (l.rfind("1 ", 0) == 0 && l.find("no back-end in this release") != std::string::npos);
    check(warned, "the refused entry edit is a warning in kiln.log");
    bool no_adds = false;
    for (const auto& [k, v] : files.data)
        if (k.rfind("plans/", 0) == 0) no_adds = json::parse(v)["adds"].empty();
    check(no_adds, "the plan adds no entry");
    check(text.find("\"oldPatch\"") != std::string::npos && text.find("oldmod/cap") != std::string::npos, "the old boot patch is a writer of levelCap below the registry");
    bool over = false;
    for (const auto& c : rep["conflicts"]) over = over || (c["kind"] == "overridden" && c["winner"] == "bigmod" && c["field"].get<std::string>().find("levelCap") != std::string::npos);
    check(over, "the registry's set over the old patch wins, reported as overridden: " + rep["conflicts"].dump());
    check(text.find("\"result\":60") != std::string::npos, "levelCap 60 in the report");
    check(files.data.count("ids-server-" + kExe + ".json") && json::parse(files.data["ids-server-" + kExe + ".json"])["registries"]["items"][g(2)] == "game:plank",
          "the id table in the data folder");
    bool plan = false;
    for (const auto& [k, v] : files.data) plan = plan || k.rfind("plans/", 0) == 0;
    check(plan && rep["plan"].is_object(), "the plan is written and named in registry.json");
    std::string out;
    check(r.read(2, "balance", "game:balance", "levelCap", &out) == KILN_OK && out == "60", "read resolved");
    check(r.read(0, "items", "game:plank", "maxStack", &out) == KILN_OK && out == "20", "read base");
    check(r.select(2, "items", "\"game:*\"", &out) == KILN_OK && out == "[\"game:material_t1_wood_logs\",\"game:plank\"]", "select resolved: the item whose removal was refused is still there: " + out);
    uint32_t a = 0, b = 0;
    std::string rp;
    check(r.submit(1, "x", R"({"ir":1,"mod":"x","stage":"alter","edits":[]})", &a, &b, &rp) == KILN_ERR_UNAVAILABLE, "no submit after the stage");
    check(live_report != nullptr, "the live back-end got its handoff with a report callback");
    const std::string start_report = files.report;
    live_report(*live_result, true);
    check(r.phase() == stage::Phase::WorldLoad && r.generation() == 2 && json::parse(files.report)["phase"] == "worldLoad",
          "the live back-end's report: phase worldLoad, generation 2");

    // a second start: the cache gives the same base and the same registry.json
    TreeView view2 = world();
    Files files2;
    files2.data = files.data;
    stage::Runner r2(env_of(&view2, &files2, dir, &sections));
    const auto boot2 = r2.run([&](model::Model& m) { return boothook::old_writes(docs, m); });
    r2.finish({{boot2.at(0).fields.at(0).field, true, Value::integer(60), ""}});
    bool hit = false;
    for (const std::string& l : files2.log) hit = hit || l.find("the cache holds") != std::string::npos;
    check(hit, "the second start takes its base from the cache");
    check(files2.report == start_report, "and writes the same registry.json");
}

void host_order_and_code(const std::string& dir) {
    TreeView view = world();
    Files files;
    stage::Runner r(env_of(&view, &files, dir, &sections));
    check(r.set_order(R"([{"mod":"late","version":"1.0.0"},{"mod":"bigmod","version":"2.0.0"}])") == KILN_OK, "set_order");
    check(r.set_order("[{\"mod\":\"a\"},{\"mod\":\"a\"}]") == KILN_ERR_INVALID_ARGUMENT, "a mod twice is refused");
    uint64_t h = 0;
    std::string seen;
    check(r.subscribe_stage(9, "codemod", [&](Stage s, const View& v) {
              if (s != Stage::Alter) return true;
              std::string out;
              r.read(1, "items", "game:plank", "maxStack", &out);
              seen = out;
              uint32_t a = 0, b = 0;
              std::string rep;
              r.submit(9, "codemod", R"({"ir":1,"mod":"codemod","stage":"alter","edits":[{"registry":"items","select":{"id":"game:plank"},"field":"maxStack","op":{"add":1}}]})", &a, &b, &rep);
              (void)v;
              return true;
          }, &h) == KILN_OK,
          "a native stage callback");
    uint32_t a = 0, b = 0;
    std::string rep;
    check(r.submit(9, "codemod", R"({"ir":1,"mod":"codemod","stage":"final","edits":[{"registry":"items","select":{"id":"game:plank"},"field":"maxStack","op":{"add":100}}]})", &a, &b, &rep) == KILN_OK && a == 1,
          "a batch submitted before the stage is queued");
    check(r.submit(9, "codemod", R"({"ir":1,"mod":"other","stage":"alter","edits":[]})", &a, &b, &rep) == KILN_ERR_INVALID_ARGUMENT, "a batch naming another mod's id is refused");
    r.run({});
    r.finish({});
    const json rep2 = json::parse(files.report);
    check(rep2["orderSource"] == "kiln.host" && rep2["order"][0]["mod"] == "late" && rep2["order"][1]["mod"] == "bigmod" && rep2["order"][2]["mod"] == "codemod",
          "Kiln.Host's order; a mod it does not name runs after: " + rep2["order"].dump());
    check(seen == "20", "the callback's stage view in alter: the previous stage's resolved view (add: the base 20), never the alters running: " + seen);
    check(rep2.dump().find("\"result\":261") != std::string::npos, "its alter (+1) and its queued final (+100) compose on them: 261");
}

std::vector<stage::Env::DataFile> no_sections(const Schema&) { return {}; }

void id_table_without_content(const std::string& dir) {
    TreeView view = world();
    Files files;
    stage::Runner r(env_of(&view, &files, dir, &no_sections));
    r.run({});
    r.finish({});
    const std::string rel = "ids-server-" + kExe + ".json";
    check(files.data.count(rel) && json::parse(files.data[rel])["registries"]["items"][g(2)] == "game:plank" && !view.served.empty(),
          "no registry content, no id table for this build yet: the id table is written");
    check(json::parse(files.report)["fields"].empty(), "and registry.json holds nothing");
    TreeView view2 = world();
    Files files2;
    files2.data = files.data;
    stage::Runner r2(env_of(&view2, &files2, dir, &no_sections));
    r2.run({});
    r2.finish({});
    check(view2.served.empty(), "the next start with no registry content reads nothing");
}

void bridge_write() {
    kc::rbtest::balance_type();
    std::vector<uint8_t> bytes(8, 0);
    const uint32_t cap = 55, sp = 4;
    std::memcpy(bytes.data(), &cap, 4);
    std::memcpy(bytes.data() + 4, &sp, 4);
    const Schema schema = generated_schema();
    map::RegistryMap reg;
    reg.registry = "balance";
    map::Row row;
    row.field = "levelCap", row.paths = {"playerLevelCap"}, row.conv = "int", row.backend = map::Backend::Boot;
    reg.rows["levelCap"] = row;
    stage::BootResource rb;
    rb.key = key_of(kBalance, "keen::BalancingTable");
    rb.type = "keen::BalancingTable";
    ChainRow add;
    add.kind = WriterKind::Registry, add.mod = "bigmod";
    add.op.add = 5;
    rb.fields.push_back({{"balance", "game:balance", "levelCap"}, &reg, &reg.rows["levelCap"], *schema.registry("balance")->field("levelCap"), {add}});
    std::vector<stage::BootOutcome> out;
    std::vector<std::string> notes;
    const size_t n = boothook::write(rb.key, rb.type, &bytes, rb, &out, &notes);
    uint32_t now = 0;
    std::memcpy(&now, bytes.data(), 4);
    check(n == 1 && now == 60 && out.size() == 1 && out[0].ok && out[0].written == Value::integer(60),
          "the registry's +5 over what the old documents left (55) is written: 60");
    row.paths = {"noSuchMember"};
    reg.rows["levelCap"] = row;
    out.clear();
    check(boothook::write(rb.key, rb.type, &bytes, rb, &out, &notes) == 0 && out.size() == 1 && !out[0].ok && !out[0].why.empty(), "a member that is not there: not written, with why: " + (out.empty() ? "" : out[0].why));
}
}  // namespace

// review3 S4: registry.json does not call a record verified when it was proven on another regmap/common (every row is off)
void other_mapping_report(const std::string& dir) {
    TreeView view = world();
    Files files;
    stage::Env e = env_of(&view, &files, dir, [](const Schema&) { return std::vector<stage::Env::DataFile>{}; });
    e.regmap = [dir](map::RegMap* out, std::vector<std::string>* errs) {
        const bool ok = map::load_dir(dir, "server", kExe, out, errs);
        out->build.regmap_digest = std::string(64, 'b');
        return ok;
    };
    stage::Runner r(std::move(e));
    r.run([](model::Model& m) { return boothook::old_writes({}, m); });
    r.finish({});
    const json rep = json::parse(files.report);
    check(rep["regmap"]["verified"] == false && rep["regmap"]["record"].is_string(), "registry.json: a record of another mapping is not verified");
}

int main(int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : "regmap";
    int failed = 0;
    const std::pair<const char*, std::function<void()>> groups[] = {
        {"one_start", [&] { one_start(dir); }}, {"host_order_and_code", [&] { host_order_and_code(dir); }}, {"id_table_without_content", [&] { id_table_without_content(dir); }}, {"bridge_write", [] { bridge_write(); }},
        {"registry_caps", [&] { registry_caps(dir); }}, {"other_mapping_report", [&] { other_mapping_report(dir); }}};
    for (const auto& [name, fn] : groups) {
        try {
            fn();
        } catch (const std::exception& e) {
            ++failed;
            std::printf("registry-stage-test: FAIL %s: %s (after %d checks)\n", name, e.what(), checks);
        }
    }
    if (failed) {
        std::printf("registry-stage-test: %d group(s) failed, %d checks passed\n", failed, checks);
        return 1;
    }
    std::printf("registry-stage-test: %d checks passed\n", checks);
}
