// SPDX-License-Identifier: MIT
// Host test of the registry's archive-writer attribution (registry/archive_journal.cpp, commitment 7 / D1): kiln-eml's
// startup change journal read against the start's live generation (the schema's own valid fixture, a stale one, broken
// ones), its packages named on the entries whose resources they wrote, the named rows below every Kiln writer in the
// chain (certainty "named"), the conflict line that says the package wrote the entry before Kiln and Kiln's edit wins, and
// registry.json's base.journal. argv[1]: the repository root (for the schema fixture); argv[2] (optional): where to write
// the document it builds, for a schema check against schema/state-registry.schema.json.
#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>
#include <string>

#include "../registry/archive_journal.h"
#include "../registry/drift.h"
#include "../registry/report.h"

using namespace kc::registry;

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, what)                                                       \
    do {                                                                        \
        if (cond) ++g_pass;                                                     \
        else {                                                                  \
            ++g_fail;                                                           \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, std::string(what).c_str()); \
        }                                                                       \
    } while (0)

static const std::string kGen = std::string(64, 'a');
static const std::string kStoneGuid = "01234567-89ab-cdef-0123-456789abcdef";  // the fixture's resource alpha, bravo and delta write
static const std::string kCreated = "5d2f8a90-1c3b-4e7f-9a12-6b4c8d0e2f13";     // alpha's first creation
static const std::string kStone = "game:material_t1_stone", kLogs = "game:material_t1_wood_logs", kNew = "game:alpha_item";

static Schema schema() {
    Schema s;
    RegistrySpec items;
    items.name = "items", items.version = "1.0.0";
    FieldSpec ms;
    ms.name = "maxStack", ms.type = FieldType::Int, ms.min = 1, ms.max = 65535, ms.applies = Applies::WorldLoad;
    FieldSpec perks;
    perks.name = "perks", perks.type = FieldType::List, perks.element = FieldType::Id, perks.max_items = 5, perks.applies = Applies::Now;
    FieldSpec up;
    up.name = "upgradesTo", up.type = FieldType::Id, up.ref = "items", up.applies = Applies::Now;
    items.fields[ms.name] = ms;
    items.fields[perks.name] = perks;
    items.fields[up.name] = up;
    s.registries["items"] = items;
    return s;
}

static BaseModel base() {
    BaseModel b;
    auto item = [](const std::string& id, int64_t stack) {
        BaseModel::Entry e;
        e.id = id;
        e.fields["maxStack"] = Value::integer(stack);
        e.fields["perks"] = Value::of_list({});
        e.fields["upgradesTo"] = Value::text("game:material_t1_wood_logs");
        return e;
    };
    b.registries["items"] = {item(kStone, 25), item(kLogs, 10), item(kNew, 3)};
    return b;
}

static const std::map<std::string, std::string> kIds = {
    {"items|" + kStone, kStoneGuid}, {"items|" + kLogs, "aaaaaaaa-0000-4000-8000-000000000001"}, {"items|" + kNew, kCreated}};

static Batch batch(const Schema& s, const std::string& mod, const std::string& stage, const std::string& edits) {
    Batch b;
    std::vector<Refusal> r;
    std::string err;
    if (!parse_batch(std::string(R"({"ir": 1, "mod": ")") + mod + R"(", "stage": ")" + stage + R"(", "source": "content/)" + mod +
                         R"(.json", "edits": )" + edits + "}",
                     s, &b, &r, &err))
        std::printf("batch: %s\n", err.c_str());
    return b;
}
static ModInput mod(const std::string& id, std::vector<Batch> batches) {
    ModInput m;
    m.id = id;
    m.batches = std::move(batches);
    return m;
}

static std::string fixture(const std::string& root) {
    std::ifstream f(root + "/schema/sfi/eml-journal/fixtures/valid.json");
    std::stringstream ss;
    ss << f.rdbuf();
    const json all = json::parse(ss.str(), nullptr, false);
    if (all.is_discarded() || !all.contains("startup-journal")) return "{}";
    return all["startup-journal"]["mixed_retry"].dump();
}

int main(int argc, char** argv) {
    const std::string root = argc > 1 ? argv[1] : "../../../..";
    const std::string text = fixture(root);
    const Schema s = schema();

    // ---- the reader ----
    const ArchiveJournal j = parse_archive_journal(text, kGen);
    CHECK(j.state == "read" && j.dialect == "shroudforge" && !j.truncated, "the schema's valid fixture reads against its generation: " + j.state + " " + j.text);
    CHECK(j.packages.size() == 4 && j.packages[0].id == "alpha" && j.packages[1].kind == "eml" && j.packages[3].id == "delta",
          "its packages in run order, with their kind");
    CHECK(j.packages.size() == 4 && j.packages[0].changed.size() == 1 && j.packages[0].changed[0] == kStoneGuid &&
              j.packages[0].created.size() == 2 && j.packages[0].created[0] == kCreated,
          "a package's written and created resources, GUIDs lower case");
    const ArchiveJournal stale = parse_archive_journal(text, std::string(64, 'c'));
    CHECK(stale.state == "stale" && stale.packages.empty() && stale.text.find("another generation") != std::string::npos,
          "a journal of another generation names nobody: " + stale.text);
    CHECK(parse_archive_journal("{", kGen).state == "unreadable", "not JSON: unreadable");
    json other = json::parse(text);
    other["format"] = 2;
    CHECK(parse_archive_journal(other.dump(), kGen).state == "unreadable", "another format: unreadable");
    json broken = json::parse(text);
    broken["packages"][0]["changed"][0]["guid"] = 7;
    const ArchiveJournal b = parse_archive_journal(broken.dump(), kGen);
    CHECK(b.state == "unreadable" && b.packages.empty(), "a malformed package: unreadable, nothing named: " + b.text);
    json big = json::parse(text);
    big["truncated"] = true;
    big["packages"][1]["truncated"] = true;
    const ArchiveJournal t = parse_archive_journal(big.dump(), kGen);
    CHECK(t.state == "read" && t.truncated && t.packages[1].truncated, "a truncated journal still names what it lists, and says it is truncated");
    CHECK(archive_writer_who("shroudforge", "alpha") == "shroudforge:alpha" && archive_writer_who("eml", "bravo") == "eml:bravo",
          "who: <kind>:<package>");
    CHECK(archive_writer_label("shroudforge:alpha") == "alpha (ShroudForge package)" && archive_writer_label("eml:bravo") == "bravo (EML package)",
          "a package's label in messages");

    // ---- naming entries ----
    BaseModel bm = base();
    const std::set<std::string> whole = {"items"};  // the registries whose entries are each a whole resource
    const size_t named = name_archive_writers(j, kIds, whole, &bm);
    CHECK(named == 2, "two entries are named (the written one and the created one): " + std::to_string(named));
    const auto& stone = bm.registries["items"][0].archive;
    CHECK(stone == std::vector<std::string>({"shroudforge:alpha", "eml:bravo", "shroudforge:delta"}),
          "every package that wrote the stone, in run order");
    CHECK(bm.registries["items"][1].archive.empty(), "an entry no package wrote is not named");
    CHECK(bm.registries["items"][2].archive == std::vector<std::string>({"shroudforge:alpha", "shroudforge:delta"}),
          "the package that created an entry is named on it, and a later one that wrote it");
    BaseModel none = base();
    CHECK(name_archive_writers(stale, kIds, whole, &none) == 0 && none.registries["items"][0].archive.empty(), "a stale journal names nothing");
    // review: an element of a shared resource (a recipe) is never named, even when its GUID member is a resource the journal lists
    BaseModel shared = base();
    BaseModel::Entry recipe;
    recipe.id = "game:craft_stone";
    recipe.fields["duration"] = Value::number(2);
    shared.registries["recipes"] = {recipe};
    std::map<std::string, std::string> ids = kIds;
    ids["recipes|game:craft_stone"] = kStoneGuid;
    CHECK(name_archive_writers(j, ids, whole, &shared) == 2 && shared.registries["recipes"][0].archive.empty() && !shared.registries["items"][0].archive.empty(),
          "a registry whose entries share a resource is not named from the journal's resource keys");

    // ---- the chain and the conflict line ----
    const Result r = compose(s, bm, {},
                             {mod("bigstacks", {batch(s, "bigstacks", "alter", R"([{"registry": "items", "select": {"id": "game:material_t1_*"}, "field": "maxStack", "op": {"mul": 4}},
                                                                                     {"registry": "items", "select": {"id": "game:alpha_item"}, "field": "maxStack", "op": 50}])")})});
    const FieldOutcome& fo = r.fields.at({"items", kStone, "maxStack"});
    CHECK(fo.chain.size() == 4 && fo.chain[0].kind == WriterKind::ArchiveWriter && fo.chain[0].who == "shroudforge:alpha" &&
              fo.chain[2].who == "shroudforge:delta" && fo.chain[3].kind == WriterKind::Registry,
          "the packages lie below the registry edit, in run order");
    CHECK(fo.chain.size() == 4 && fo.chain[0].after.i == 25 && fo.chain[0].op.set && fo.chain[0].mod.empty(),
          "an archive row's value is the base (archive bytes are the base, D1)");
    CHECK(fo.result.i == 100 && fo.winner == "bigstacks", "a relative registry op composes on the archive's value and wins");
    CHECK(r.fields.at({"items", kLogs, "maxStack"}).chain.size() == 1, "an entry no package wrote has no archive row");
    const Line* line = nullptr;
    for (const Line& l : r.lines)
        if (l.field.id == kStone) line = &l;
    CHECK(line && line->kind == LineKind::ComposedOn && line->certainty == Certainty::Named && line->loser == "shroudforge:delta" && line->winner == "bigstacks",
          "one composedOn line, named, against the last package");
    CHECK(line && line->text.find("delta (ShroudForge package)") != std::string::npos && line->text.find("before Kiln") != std::string::npos &&
              line->text.find("bigstacks's edit wins (D1)") != std::string::npos,
          "the line says the package wrote it before Kiln and the registry edit wins: " + (line ? line->text : std::string()));
    const Line* created = nullptr;
    for (const Line& l : r.lines)
        if (l.field.id == kNew) created = &l;
    CHECK(created && created->kind == LineKind::Overridden && created->certainty == Certainty::Named && created->loser == "shroudforge:delta",
          "an absolute edit over a created entry: overridden, named");
    // exclusive and yield are not about the archive (its value is the base)
    const Result y = compose(s, bm, {}, {mod("y", {batch(s, "y", "alter", R"([{"registry": "items", "select": {"id": "game:material_t1_stone"}, "field": "maxStack", "op": 9, "yield": true}])")})});
    CHECK(y.fields.at({"items", kStone, "maxStack"}).result.i == 9, "a yield edit still applies over the archive's packages (they are the base)");
    const Result ex = compose(s, bm, {}, {mod("x", {batch(s, "x", "alter", R"([{"registry": "items", "select": {"id": "game:material_t1_stone"}, "field": "maxStack", "op": 9, "exclusive": true}])")})});
    CHECK(ex.refused.empty() && ex.fields.at({"items", kStone, "maxStack"}).result.i == 9, "an exclusive edit is never refused because of the archive's packages");
    // an old Kiln path on a named entry: the packages first, the old path above them
    OldWrite pack;
    pack.kind = WriterKind::OldPack, pack.mod = "oldstacks", pack.who = "oldstacks: content/a.json", pack.field = {"items", kStone, "maxStack"};
    pack.op.set = true, pack.op.value = Value::integer(30);
    const Result o = compose(s, bm, {pack}, {});
    const auto& oc = o.fields.at({"items", kStone, "maxStack"}).chain;
    CHECK(oc.size() == 4 && oc[2].kind == WriterKind::ArchiveWriter && oc[3].kind == WriterKind::OldPack && oc[3].after.i == 30,
          "an old pack lies above the archive's packages");
    // review: a package is never a field's winner (per entry, not per field): an edit the reference check takes out leaves
    // the field to the base, not to the last package, and a field only packages are left on is not reported as touched
    const Result dropped = compose(s, bm, {}, {mod("ghost", {batch(s, "ghost", "alter", R"([{"registry": "items", "select": {"id": "game:material_t1_stone"}, "field": "upgradesTo", "op": "zz:ghost"}])")})});
    const auto up = dropped.fields.find({"items", kStone, "upgradesTo"});
    CHECK(up == dropped.fields.end() || (up->second.winner.empty() && up->second.winner.rfind("shroudforge:", 0) != 0),
          "a dropped edit's field is not won by a package: " + (up == dropped.fields.end() ? std::string("gone") : up->second.winner));
    CHECK(up == dropped.fields.end(), "a field only archive rows are left on is not a touched field");
    bool refused_ghost = false;
    for (const Refusal& x : dropped.refused) refused_ghost = refused_ghost || x.mod == "ghost";
    CHECK(refused_ghost, "the edit naming no entry is refused");
    // a later entry point keeps the named rows of the start and names new places too
    const Result later = compose_after(s, bm, r, {mod("late", {batch(s, "late", "alter", R"([{"registry": "items", "select": {"id": "game:alpha_item"}, "field": "perks", "list": {"append": ["game:perk_x"]}}])")})});
    const auto& lp = later.fields.at({"items", kNew, "perks"}).chain;
    CHECK(lp.size() == 3 && lp[0].kind == WriterKind::ArchiveWriter && lp[2].kind == WriterKind::Registry, "a later pass names the package on a field it touches first");

    // ---- registry.json ----
    ReportMeta meta;
    meta.written = "2026-11-25T20:00:00Z", meta.generation = 1, meta.base.writer = "kiln-eml";
    meta.regmap = {"server", std::string(64, 'f'), "", false};  // a document the schema check accepts
    meta.base.journal = {j.state, j.text, j.packages.size(), named, j.truncated};
    const json doc = report_json(s, r, meta);
    CHECK(doc["base"]["journal"]["state"] == "read" && doc["base"]["journal"]["packages"] == 4 && doc["base"]["journal"]["entries"] == 2 &&
              doc["base"]["journal"]["truncated"] == false,
          "base.journal: read, its packages and the entries it named: " + doc["base"].dump());
    const json* row = nullptr;
    for (const json& f : doc["fields"])
        if (f["id"] == kStone) row = &f;
    CHECK(row && (*row)["writers"][0]["kind"] == "archiveWriter" && (*row)["writers"][0]["who"] == "shroudforge:alpha" &&
              (*row)["writers"][0]["certainty"] == "named" && (*row)["writers"][0]["value"] == 25 && !(*row)["writers"][0].contains("mod"),
          "the field row names the package, certainty named");
    // review: the journal names the entry, not the field: an archive row claims no operation on the field (no op, no list)
    CHECK(row && !(*row)["writers"][0].contains("op") && !(*row)["writers"][0].contains("list"),
          "an archive row has no op: what the package did to this field is not known: " + (row ? (*row)["writers"].dump() : std::string()));
    bool named_line = false;
    for (const json& c : doc["conflicts"]) named_line = named_line || (c["loser"] == "shroudforge:delta" && c["certainty"] == "named");
    CHECK(named_line, "the conflict line is named");
    ReportMeta absent;
    absent.written = meta.written, absent.base.writer = "shroudforge-hosted";
    absent.base.journal.text = "hosted ShroudForge keeps no change journal";
    const json doc2 = report_json(s, compose(s, base(), {}, {}), absent);
    CHECK(doc2["base"]["journal"]["state"] == "none" && doc2["base"]["journal"]["text"] == "hosted ShroudForge keeps no change journal",
          "without a journal base.journal says so: " + doc2["base"].dump());
    if (argc > 2) std::ofstream(argv[2]) << doc.dump(2);

    // ---- the drift probe's suspects (drift.h) ----
    const json map = json::parse(R"({"foreign": [{"symbol": "a", "rva": "0x10", "module": "xhl-core.dll", "offset": "0x1", "own": false},
                                                 {"symbol": "b", "rva": "0x20", "module": "kiln-content.dll", "offset": "0x2", "own": true},
                                                 {"symbol": "c", "rva": "0x30", "module": "xhl-core.dll", "offset": "0x3", "own": false},
                                                 {"symbol": "d", "rva": "0x40", "module": "", "offset": "0x4", "own": false}]})");
    CHECK(drift_suspects(map, false) == std::vector<std::string>({"xhl-core.dll", "code outside any module"}),
          "suspects: other modules that patched game code, each once, never Kiln's own");
    CHECK(drift_suspects(map, true).front() == "ShroudForge (hosted)", "hosted ShroudForge is a suspect when it runs");
    CHECK(drift_suspects(json(), false).empty() && drift_suspects(json::parse("{\"foreign\": 3}"), false).empty(), "no hook map: no suspect");
    json many = {{"foreign", json::array()}};
    for (int i = 0; i < 12; ++i) many["foreign"].push_back({{"module", "m" + std::to_string(i) + ".dll"}, {"own", false}});
    const auto capped = drift_suspects(many, false);
    CHECK(capped.size() == 9 && capped.back() == "+4 more", "at most 8 suspects, then how many more");
    CHECK(drift_who({}) == "outside Kiln (best effort; no other loader's hook seen)" &&
              drift_who({"xhl-core.dll", "emf.dll"}) == "outside Kiln (best effort; suspects: xhl-core.dll, emf.dll)",
          "a drift row's writer says best effort and names the suspects");

    std::printf("registry writers: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
