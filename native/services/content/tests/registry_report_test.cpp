// SPDX-License-Identifier: MIT
// Host test of registry.json (registry/report.cpp): the document compose()'s Result gives (fields with every writer, conflict
// lines with their certainty, refused edits, raw members, added entries), its caps and kiln.log's lines. argv[1] (optional):
// where to write the document it builds, for a schema check against schema/state-registry.schema.json.
#include <cstdio>
#include <fstream>
#include <string>

#include "../registry/report.h"

using namespace kc::reg;

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, what)                                                       \
    do {                                                                        \
        if (cond) ++g_pass;                                                     \
        else {                                                                  \
            ++g_fail;                                                           \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, what);          \
        }                                                                       \
    } while (0)

static Schema schema() {
    Schema s;
    RegistrySpec items;
    items.name = "items", items.version = "1.0.0", items.raw = true;
    FieldSpec ms;
    ms.name = "maxStack", ms.type = FieldType::Int, ms.min = 1, ms.max = 65535, ms.applies = Applies::WorldLoad;
    FieldSpec perks;
    perks.name = "perks", perks.type = FieldType::List, perks.element = FieldType::Id, perks.max_items = 5, perks.applies = Applies::Now;
    items.fields[ms.name] = ms;
    items.fields[perks.name] = perks;
    s.registries["items"] = items;
    RegistrySpec bal;
    bal.name = "balance", bal.version = "1.1.0", bal.add = bal.remove = false;
    FieldSpec cap;
    cap.name = "levelCap", cap.type = FieldType::Int, cap.min = 1, cap.max = 100, cap.applies = Applies::Restart;
    bal.fields[cap.name] = cap;
    s.registries["balance"] = bal;
    return s;
}

static const std::string kLogs = "game:material_t1_wood_logs", kStone = "game:material_t1_stone", kSword = "game:weapon_t1_sword";

static BaseModel base() {
    BaseModel b;
    auto item = [](const std::string& id, int64_t stack) {
        BaseModel::Entry e;
        e.id = id;
        e.fields["maxStack"] = Value::integer(stack);
        e.fields["perks"] = Value::of_list({});
        return e;
    };
    b.registries["items"] = {item(kLogs, 10), item(kStone, 20), item(kSword, 1)};
    BaseModel::Entry bal;
    bal.id = "game:balance";
    bal.fields["levelCap"] = Value::integer(50);
    b.registries["balance"] = {bal};
    b.raw = [](const std::string&, const std::string&, const std::string& key, std::string* why) -> std::optional<RawMember> {
        if (key != "damagesetup.dmgmod") return *why = "no member " + key, std::nullopt;
        RawMember m;
        m.spec.name = "damageSetup.dmgMod", m.spec.type = FieldType::Float, m.spec.applies = Applies::Now;
        m.value = Value::number(1.0);
        return m;
    };
    return b;
}

static Batch batch(const Schema& s, const std::string& mod, const std::string& stage, const std::string& edits) {
    Batch b;
    std::vector<Refusal> r;
    std::string err;
    if (!parse_batch(std::string(R"({"ir": 1, "mod": ")") + mod + R"(", "stage": ")" + stage + R"(", "source": "content/)" + mod + R"(.json", "edits": )" + edits + "}", s, &b, &r, &err))
        std::printf("batch: %s\n", err.c_str());
    return b;
}

static ModInput mod(const std::string& id, std::vector<Batch> batches) {
    ModInput m;
    m.id = id;
    m.batches = std::move(batches);
    return m;
}

static const json* row(const json& doc, const std::string& reg, const std::string& id, const std::string& field) {
    if (!doc.contains("fields")) return nullptr;
    for (const json& f : doc["fields"])
        if (f.value("registry", "") == reg && f.value("id", "") == id && f.value("field", "") == field) return &f;
    return nullptr;
}

static int count_kind(const json& doc, const char* kind) {
    int n = 0;
    if (doc.contains("conflicts"))
        for (const json& c : doc["conflicts"]) n += c.value("kind", "") == kind;
    return n;
}

int main(int argc, char** argv) {
    const Schema s = schema();
    const BaseModel b = base();
    OldWrite pack;
    pack.kind = WriterKind::OldPack, pack.mod = "oldstacks", pack.who = "oldstacks: content/a.json", pack.field = {"items", kLogs, "maxStack"};
    pack.op.set = true, pack.op.value = Value::integer(100);
    OldWrite sf;
    sf.kind = WriterKind::ArchiveWriter, sf.who = "shroudforge:betterstacks", sf.field = {"items", kStone, "maxStack"};
    sf.op.set = true, sf.op.value = Value::integer(25);
    const Result r = compose(s, b, {pack, sf},
                             {mod("bigstacks", {batch(s, "bigstacks", "alter", R"([{"registry": "items", "select": {"id": "game:material_*"}, "field": "maxStack", "op": {"mul": 4}},
                                                                                   {"registry": "balance", "select": {"id": "game:balance"}, "field": "levelCap", "op": 60, "exclusive": true}])")}),
                              mod("caps", {batch(s, "caps", "alter", R"([{"registry": "balance", "select": {"id": "game:balance"}, "field": "levelCap", "op": 70},
                                                                         {"registry": "items", "select": {"id": "game:nothing"}, "field": "maxStack", "op": 3}])")}),
                              mod("blades", {batch(s, "blades", "add", R"([{"registry": "items", "entry": "add", "id": "blades:edge", "like": "game:weapon_t1_sword", "fields": {"maxStack": 2}}])"),
                                             batch(s, "blades", "alter", R"([{"registry": "items", "select": {"id": "game:weapon_t1_sword"}, "raw": {"DamageSetup.DmgMod": {"mul": 2}}},
                                                                             {"registry": "items", "select": {"id": "game:weapon_t1_sword"}, "field": "perks", "list": {"append": ["game:perk_sharp"]}}])")})});
    ReportMeta meta;
    meta.pid = 1234, meta.written = "2026-11-20T16:00:00Z", meta.kiln = "0.2.0-dev.31", meta.generation = 1;
    meta.regmap = {"server", std::string(64, 'f'), "regmap/server/" + std::string(64, 'f') + ".json", true};
    meta.order = {{"bigstacks", "1.2.0", "9c1f"}, {"caps", "1.0.0", "77aa"}, {"blades", "0.3.0", "0b0b"}};
    const json doc = report_json(s, r, meta);

    // ---- the head ----
    CHECK(doc.value("format", 0) == 1 && doc.value("pid", 0) == 1234 && doc.value("written", "") == "2026-11-20T16:00:00Z" && doc.value("phase", "") == "start" &&
              doc.value("generation", 0) == 1 && doc.value("kiln", "") == "0.2.0-dev.31",
          "head: format, pid, written, phase start, generation, kiln");
    CHECK(doc.contains("schema") && doc["schema"].value("registrySchema", 0) == 1 && doc["schema"]["registries"].value("balance", "") == "1.1.0",
          "schema: the format and each registry's version");
    CHECK(doc.contains("regmap") && doc["regmap"].value("side", "") == "server" && doc["regmap"].value("verified", false), "regmap: side, record, verified");
    CHECK(doc.contains("base") && doc["base"].value("origin", "") == "live" && doc["base"].value("writer", "") == "none", "base: origin and archive writer (S5/S10 fill them)");
    CHECK(doc.contains("order") && doc["order"].size() == 3 && doc["order"][0].value("mod", "") == "bigstacks" && doc["order"][0].value("editsHash", "") == "9c1f" &&
              doc.value("orderSource", "") == "kiln.host",
          "order: the mods as the stage ran them, and where the order came from");

    // ---- field rows: base, writers below and above, result, winner ----
    const json* logs = row(doc, "items", kLogs, "maxStack");
    CHECK(logs && (*logs).value("applies", "") == "worldLoad" && (*logs)["base"].value("value", 0) == 10 && (*logs)["base"].value("origin", "") == "archive",
          "a field row: applies from the schema, the base value and its origin");
    CHECK(logs && (*logs)["writers"].size() == 2 && (*logs)["writers"][0].value("kind", "") == "oldPack" && (*logs)["writers"][0].value("mod", "") == "oldstacks" &&
              (*logs)["writers"][0].value("value", 0) == 100 && (*logs)["writers"][1].value("kind", "") == "registry" &&
              (*logs)["writers"][1].value("stage", "") == "alter" && (*logs)["writers"][1]["op"].value("mul", 0.0) == 4 &&
              (*logs)["writers"][1].value("source", "") == "content/bigstacks.json" && (*logs)["writers"][1].value("n", 99) == 0,
          "writers in the order they apply: the old pack below, the registry edit with its stage, op, source and ordinal");
    CHECK(logs && (*logs).value("result", 0) == 400 && (*logs).value("winner", "") == "bigstacks", "result and winner");
    const json* stone = row(doc, "items", kStone, "maxStack");
    CHECK(stone && (*stone)["writers"][0].value("kind", "") == "archiveWriter" && (*stone)["writers"][0].value("certainty", "") == "named" &&
              (*stone)["writers"][0].value("who", "") == "shroudforge:betterstacks" && (*stone)["writers"][1].value("certainty", "") == "certain",
          "an archive-writer package is named, Kiln's own writers are certain");
    const json* raw = row(doc, "items", kSword, "DamageSetup.DmgMod");
    CHECK(raw && (*raw).value("raw", false) && (*raw).value("applies", "") == "now" && (*raw).value("result", 0.0) == 2.0 &&
              (*raw)["writers"][0].value("raw", "") == "DamageSetup.DmgMod",
          "a raw member's row: the path as written, raw: true, applies from the member");
    const json* perks = row(doc, "items", kSword, "perks");
    CHECK(perks && (*perks)["writers"][0].contains("list") && (*perks)["writers"][0]["list"].contains("append") && (*perks)["result"].size() == 1,
          "a list row: the list op and the resulting list");
    const json* added = row(doc, "items", "blades:edge", "maxStack");
    CHECK(added && (*added)["base"].value("origin", "") == "added" && (*added).value("result", 0) == 2, "an added entry's base is reported as added");
    CHECK(doc.contains("added") && doc["added"].size() == 1 && doc["added"][0].value("id", "") == "blades:edge", "added entries listed");

    // ---- conflicts and refusals ----
    CHECK(count_kind(doc, "composedOn") == 2 && count_kind(doc, "exclusive") == 1, "conflicts: composed on the old pack and the archive writer's value; one exclusive");
    bool exclusive_ok = false, named_ok = false;
    for (const json& c : doc["conflicts"]) {
        if (c.value("kind", "") == "exclusive")
            exclusive_ok = c.value("winner", "") == "bigstacks" && c.value("loser", "") == "caps" && c.value("field", "") == "balance[game:balance].levelCap" &&
                           c.value("certainty", "") == "certain" && !c.value("text", "").empty();
        if (c.value("kind", "") == "composedOn" && c.value("loser", "") == "shroudforge:betterstacks") named_ok = c.value("certainty", "") == "named";
    }
    CHECK(exclusive_ok, "the exclusive conflict names winner, loser, field and text");
    CHECK(named_ok, "a conflict with an archive-writer package is named");
    CHECK(doc.contains("refused") && doc["refused"].size() == 2, "both refusals listed (the exclusive loser and the missing entry)");
    bool missing_ok = false;
    for (const json& x : doc["refused"])
        missing_ok = missing_ok || (x.value("mod", "") == "caps" && x.value("n", 0) == 1 && x.value("stage", "") == "alter" &&
                                    x.value("source", "") == "content/caps.json" && x.value("reason", "").find("no entry") != std::string::npos);
    CHECK(missing_ok, "a refusal names mod, source, stage, ordinal and reason");
    CHECK(doc.contains("counts") && doc["counts"].value("fields", 0) == 6 && doc["counts"].value("conflicts", 0) == 3 && doc["counts"].value("refused", 0) == 2 &&
              doc["counts"].value("added", 0) == 1,
          "counts: the totals");
    CHECK(doc.contains("truncated") && doc["truncated"].value("fields", 1) == 0 && doc["truncated"].value("conflicts", 1) == 0, "nothing cut");

    // ---- world load: same rows, the phase says it ----
    ReportMeta wl = meta;
    wl.world_load = true, wl.generation = 2;
    const json doc2 = report_json(s, r, wl);
    CHECK(doc2.value("phase", "") == "worldLoad" && doc2.value("generation", 0) == 2 && doc2["fields"].size() == doc["fields"].size(), "phase worldLoad");

    // ---- an origin from S5 / S10 ----
    ReportMeta o = meta;
    o.origin = [](const FieldRef& f) { return f.id == kStone ? std::string("archiveWriter") : std::string("archive"); };
    const json doc3 = report_json(s, r, o);
    const json* st3 = row(doc3, "items", kStone, "maxStack");
    CHECK(st3 && (*st3)["base"].value("origin", "") == "archiveWriter", "the base origin comes from the model when it says");

    // ---- caps: rows past a cap are left out and counted ----
    {
        ReportCaps small;
        small.fields = 2, small.conflicts = 1, small.refused = 1, small.entries = 0;
        const json cut = report_json(s, r, meta, small);
        CHECK(cut["fields"].size() == 2 && cut["truncated"].value("fields", 0) == 4 && cut["conflicts"].size() == 1 && cut["truncated"].value("conflicts", 0) == 2 &&
                  cut["refused"].size() == 1 && cut["truncated"].value("refused", 0) == 1 && cut["added"].empty() && cut["truncated"].value("added", 0) == 1 &&
                  cut["counts"].value("fields", 0) == 6,
              "caps: rows cut, counted under truncated, totals kept");
    }

    // ---- kiln.log: one line per conflict, then refusals, at most caps.log, then a total ----
    {
        const std::vector<std::string> log = report_log(r);
        CHECK(log.size() == 6 && log.back().find("3 conflicts") != std::string::npos && log.back().find("2 refused") != std::string::npos,
              "kiln.log: three conflict lines, two refusals, one total");
        CHECK(!log.empty() && log[0].rfind("registry: ", 0) == 0, "kiln.log lines start with registry:");
        ReportCaps two;
        two.log = 2;
        const std::vector<std::string> cut = report_log(r, two);
        CHECK(cut.size() == 3 && cut.back().find("3 not in kiln.log") != std::string::npos, "kiln.log: at most caps.log lines plus the total, which says how many were left out");
        CHECK(report_log(Result{}).empty(), "kiln.log: nothing to say, no line");
    }

    // ---- many conflicts: the log stays at 50 lines plus the total ----
    {
        BaseModel big = b;
        for (int i = 0; i < 120; ++i) {
            BaseModel::Entry e;
            e.id = "game:thing_" + std::to_string(i);
            e.fields["maxStack"] = Value::integer(5);
            big.registries["items"].push_back(e);
        }
        const Result many = compose(s, big, {}, {mod("a", {batch(s, "a", "alter", R"([{"registry": "items", "select": {"id": "game:thing_*"}, "field": "maxStack", "op": 7}])")}),
                                                 mod("b", {batch(s, "b", "alter", R"([{"registry": "items", "select": {"id": "game:thing_*"}, "field": "maxStack", "op": 8}])")})});
        CHECK(many.lines.size() == 120 && report_log(many).size() == 51, "120 conflicts: 50 lines and the total");
    }

    if (argc > 1) {
        std::ofstream out(argv[1]);
        out << doc.dump(2) << "\n";
    }
    std::printf("registry-report-test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
