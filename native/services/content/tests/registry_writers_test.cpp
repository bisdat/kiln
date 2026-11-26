// SPDX-License-Identifier: MIT
// Host test of the registry's archive-writer attribution (registry/archive_journal.cpp, commitment 7 / D1): kiln-eml's
// startup change journal read against the start's live generation (the schema's own valid fixture, a stale one, broken
// ones), its packages named on the entries whose resources they wrote, the named rows below every Kiln writer in the
// chain (certainty "named"), the conflict line that says the package wrote the entry before Kiln and Kiln's edit wins, and
// registry.json's base.journal. argv[1]: the repository root (for the schema fixture); argv[2] (optional): where to write
// the document it builds, for a schema check against schema/state-registry.schema.json.
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "../registry/archive_journal.h"
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
    items.fields[ms.name] = ms;
    items.fields[perks.name] = perks;
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
    const size_t named = name_archive_writers(j, kIds, &bm);
    CHECK(named == 2, "two entries are named (the written one and the created one): " + std::to_string(named));
    const auto& stone = bm.registries["items"][0].archive;
    CHECK(stone == std::vector<std::string>({"shroudforge:alpha", "eml:bravo", "shroudforge:delta"}),
          "every package that wrote the stone, in run order");
    CHECK(bm.registries["items"][1].archive.empty(), "an entry no package wrote is not named");
    CHECK(bm.registries["items"][2].archive == std::vector<std::string>({"shroudforge:alpha", "shroudforge:delta"}),
          "the package that created an entry is named on it, and a later one that wrote it");
    BaseModel none = base();
    CHECK(name_archive_writers(stale, kIds, &none) == 0 && none.registries["items"][0].archive.empty(), "a stale journal names nothing");

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
    // a later entry point keeps the named rows of the start and names new places too
    const Result later = compose_after(s, bm, r, {mod("late", {batch(s, "late", "alter", R"([{"registry": "items", "select": {"id": "game:alpha_item"}, "field": "perks", "list": {"append": ["game:perk_x"]}}])")})});
    const auto& lp = later.fields.at({"items", kNew, "perks"}).chain;
    CHECK(lp.size() == 3 && lp[0].kind == WriterKind::ArchiveWriter && lp[2].kind == WriterKind::Registry, "a later pass names the package on a field it touches first");

    // ---- registry.json ----
    ReportMeta meta;
    meta.written = "2026-11-25T20:00:00Z", meta.generation = 1, meta.base.writer = "kiln-eml";
    meta.base.journal = {j.state, j.text, j.packages.size(), named, j.truncated};
    const json doc = report_json(s, r, meta);
    CHECK(doc["base"]["journal"]["state"] == "read" && doc["base"]["journal"]["packages"] == 4 && doc["base"]["journal"]["entries"] == 2 &&
              doc["base"]["journal"]["truncated"] == false,
          "base.journal: read, its packages and the entries it named: " + doc["base"].dump());
    const json* row = nullptr;
    for (const json& f : doc["fields"])
        if (f["id"] == kStone) row = &f;
    CHECK(row && (*row)["writers"][0]["kind"] == "archiveWriter" && (*row)["writers"][0]["who"] == "shroudforge:alpha" &&
              (*row)["writers"][0]["certainty"] == "named" && (*row)["writers"][0]["op"] == 25 && !(*row)["writers"][0].contains("mod"),
          "the field row names the package, certainty named");
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

    std::printf("registry writers: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
