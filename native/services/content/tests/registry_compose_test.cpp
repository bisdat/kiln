// SPDX-License-Identifier: MIT
// Host test of the registry stage's edit IR and composition (registry/ir.cpp, registry/compose.cpp): the owner's public
// commitments as rules (later wins and is reported, relative changes stack, exclusive, yield, add/alter/final across all
// mods, final sees add+alter only, old paths below the registry and never refused, ids never reused).
#include <cstdio>
#include <string>

#include "../registry/compose.h"

using namespace kc::registry;

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
    items.name = "items";
    FieldSpec ms;
    ms.name = "maxStack", ms.type = FieldType::Int, ms.min = 1, ms.max = 65535, ms.applies = Applies::WorldLoad;
    FieldSpec pw;
    pw.name = "power", pw.type = FieldType::Float, pw.min = 0, pw.max = 100000, pw.applies = Applies::Now;
    FieldSpec rolled;
    rolled.name = "rolled", rolled.type = FieldType::Bool, rolled.applies = Applies::Now;
    FieldSpec rarity;
    rarity.name = "rarity", rarity.type = FieldType::Enum, rarity.enum_values = {"Common", "Uncommon", "Rare", "Epic", "Legendary"};
    FieldSpec perks;
    perks.name = "perks", perks.type = FieldType::List, perks.element = FieldType::Id, perks.max_items = 3, perks.applies = Applies::Now;
    FieldSpec ro;
    ro.name = "category", ro.type = FieldType::Int, ro.read_only = true;
    for (const FieldSpec& f : {ms, pw, rolled, rarity, perks, ro}) items.fields[f.name] = f;
    s.registries["items"] = items;
    RegistrySpec bal;
    bal.name = "balance";
    bal.add = bal.remove = false;
    FieldSpec cap;
    cap.name = "levelCap", cap.type = FieldType::Int, cap.min = 1, cap.max = 100, cap.applies = Applies::Restart;
    bal.fields[cap.name] = cap;
    s.registries["balance"] = bal;
    return s;
}

static BaseModel base() {
    BaseModel b;
    auto item = [](const char* id, int64_t stack, std::set<std::string> traits = {}) {
        BaseModel::Entry e;
        e.id = id;
        e.fields["maxStack"] = Value::integer(stack);
        e.fields["power"] = Value::number(10);
        e.fields["perks"] = Value::of_list({});
        e.traits = std::move(traits);
        return e;
    };
    b.registries["items"] = {item("game:material_t1_wood_logs", 10, {"material"}), item("game:material_t1_stone", 20, {"material"}),
                             item("game:weapon_t1_sword", 1, {"weapon"})};
    BaseModel::Entry gone = item("game:old_thing", 5);
    gone.removed_in = "26430253";
    b.registries["items"].push_back(gone);
    BaseModel::Entry bal;
    bal.id = "game:balance";
    bal.fields["levelCap"] = Value::integer(50);
    b.registries["balance"] = {bal};
    b.retired.insert("items|zz:retired");
    return b;
}

static Batch batch(const Schema& s, const std::string& mod, const std::string& stage, const std::string& edits, std::vector<Refusal>* refused = nullptr) {
    Batch b;
    std::vector<Refusal> r;
    std::string err;
    const bool ok = parse_batch(std::string(R"({"ir": 1, "mod": ")") + mod + R"(", "stage": ")" + stage + R"(", "source": "content/t.json", "edits": )" + edits + "}", s, &b, &r, &err);
    if (!ok) std::printf("batch: %s\n", err.c_str());
    if (refused) *refused = r;
    else
        for (const Refusal& x : r) std::printf("unexpected refusal %s #%u: %s\n", x.mod.c_str(), x.n, x.reason.c_str());
    return b;
}

static ModInput mod(const std::string& id, std::vector<Batch> batches) {
    ModInput m;
    m.id = id;
    m.batches = std::move(batches);
    return m;
}

static Value result(const Result& r, const std::string& reg, const std::string& id, const std::string& field) {
    const auto it = r.fields.find({reg, id, field});
    return it == r.fields.end() ? Value{} : it->second.result;
}

static int lines(const Result& r, LineKind k) {
    int n = 0;
    for (const Line& l : r.lines) n += l.kind == k;
    return n;
}

static const Line* line(const Result& r, LineKind k) {
    for (const Line& l : r.lines)
        if (l.kind == k) return &l;
    return nullptr;
}

int main() {
    const Schema s = schema();
    const BaseModel b = base();
    const std::string logs = "game:material_t1_wood_logs", stone = "game:material_t1_stone";

    // ---- the IR's checks (per edit, all or nothing) ----
    {
        std::vector<Refusal> r;
        Batch x = batch(s, "a", "alter", R"([
            {"registry": "items", "select": {"id": "game:material_*"}, "field": "maxStack", "op": {"mul": 4, "max": 9999}},
            {"registry": "items", "select": {"id": "x"}, "field": "nope", "op": 1},
            {"registry": "items", "select": {"id": "x"}, "field": "rolled", "op": {"mul": 2}},
            {"registry": "items", "select": {"id": "x"}, "field": "maxStack", "op": {"min": 5, "max": 2}},
            {"registry": "items", "select": {"id": "x"}, "field": "category", "op": 3},
            {"registry": "items", "select": {"id": "x"}, "field": "perks", "op": 3},
            {"registry": "items", "entry": "add", "id": "a:new"},
            {"registry": "nope", "select": {"id": "x"}, "field": "f", "op": 1},
            {"registry": "items", "select": {"id": "x"}, "field": "rarity", "op": "legendary"},
            {"registry": "items", "select": {"id": "x"}, "field": "rarity", "op": "Mythic"},
            {"registry": "items", "select": {"id": "x"}, "field": "maxStack", "op": 70000},
            {"registry": "items", "select": {"id": "x"}, "field": "maxStack", "op": {"mul": 2}, "raw": {"a.b": 1}}
        ])", &r);
        CHECK(x.edits.size() == 2, "IR: two good edits kept (a relative op, an enum name in any case)");
        CHECK(x.edits.size() == 2 && x.edits[1].op.value.t == "Legendary", "IR: an enum name is taken in the schema's spelling");
        CHECK(r.size() == 10, "IR: ten edits refused, each on its own");
        auto has = [&](uint32_t n, const char* text) {
            for (const Refusal& f : r)
                if (f.n == n && f.reason.find(text) != std::string::npos) return true;
            return false;
        };
        CHECK(has(1, "has no field \"nope\""), "IR: an unknown field is named");
        CHECK(has(2, "works on numbers"), "IR: {mul} on a bool is refused");
        CHECK(has(3, "min must not be greater than max"), "IR: min > max is refused (std::clamp is undefined there)");
        CHECK(has(4, "read-only"), "IR: a read-only field is refused");
        CHECK(has(5, "is a list"), "IR: a list takes list ops");
        CHECK(has(6, "added in stage add"), "IR: an add outside stage add is refused");
        CHECK(has(7, "no registry"), "IR: an unknown registry is refused per edit");
        CHECK(has(9, "not one of"), "IR: an unknown enum name is refused");
        CHECK(has(10, "outside"), "IR: a value out of the field's range is refused");
        CHECK(has(11, "raw"), "IR: raw game paths are not taken by IR 1");
        Batch y;
        std::string err;
        CHECK(!parse_batch(std::string(R"({"ir": 2, "mod": "a", "stage": "alter", "edits": []})"), s, &y, &r, &err), "IR: an unknown IR version refuses the document");
        CHECK(!parse_batch(std::string(R"({"ir": 1, "mod": "a", "stage": "later", "edits": []})"), s, &y, &r, &err), "IR: an unknown stage refuses the document");
        std::vector<Refusal> r2;
        batch(s, "a", "add", R"([{"registry": "items", "entry": "add", "id": "b:x"}, {"registry": "items", "select": {"id": "x"}, "field": "maxStack", "op": 1},
                                 {"registry": "balance", "entry": "add", "id": "a:x"}])", &r2);
        CHECK(r2.size() == 3, "IR: an add of another mod's id, a field edit in stage add and an add to a fixed registry are refused");
    }

    // ---- later wins, reported; equal values agree ----
    {
        const Result r = compose(s, b, {}, {mod("a", {batch(s, "a", "alter", R"([{"registry": "items", "select": {"id": "game:material_t1_wood_logs"}, "field": "maxStack", "op": 100}])")}),
                                            mod("b", {batch(s, "b", "alter", R"([{"registry": "items", "select": {"id": "game:material_t1_wood_logs"}, "field": "maxStack", "op": 200}])")})});
        CHECK(result(r, "items", logs, "maxStack").i == 200, "later in load order wins");
        const Line* l = line(r, LineKind::Overridden);
        CHECK(l && l->winner == "b" && l->loser == "a", "the override is reported with winner and loser");
        CHECK(r.refused.empty(), "nobody is refused for an override");
        const Result agree = compose(s, b, {}, {mod("a", {batch(s, "a", "alter", R"([{"registry": "items", "select": {"id": "game:material_t1_wood_logs"}, "field": "maxStack", "op": 100}])")}),
                                                mod("b", {batch(s, "b", "alter", R"([{"registry": "items", "select": {"id": "game:material_t1_wood_logs"}, "field": "maxStack", "op": {"set": 100}}])")})});
        CHECK(agree.lines.empty() && result(agree, "items", logs, "maxStack").i == 100, "two mods setting the same value agree: no conflict line");
        const Result again = compose(s, b, {}, {mod("a", {batch(s, "a", "alter", R"([{"registry": "items", "select": {"id": "game:material_t1_wood_logs"}, "field": "maxStack", "op": 100}])")}),
                                                mod("b", {batch(s, "b", "alter", R"([{"registry": "items", "select": {"id": "game:material_t1_wood_logs"}, "field": "maxStack", "op": 200},
                                                                                     {"registry": "items", "select": {"id": "game:material_t1_wood_logs"}, "field": "maxStack", "op": 300}])")})});
        CHECK(lines(again, LineKind::Overridden) == 1 && result(again, "items", logs, "maxStack").i == 300, "a mod's second edit over its own value is not another conflict line");
    }

    // ---- relative changes stack (x4 then x2 = x8); relative on another's absolute composes on it ----
    {
        const Result r = compose(s, b, {}, {mod("a", {batch(s, "a", "alter", R"([{"registry": "items", "select": {"id": "game:material_*"}, "field": "maxStack", "op": {"mul": 4}}])")}),
                                            mod("b", {batch(s, "b", "alter", R"([{"registry": "items", "select": {"id": "game:material_*"}, "field": "maxStack", "op": {"mul": 2}}])")})});
        CHECK(result(r, "items", logs, "maxStack").i == 80 && result(r, "items", stone, "maxStack").i == 160, "x4 then x2 = x8");
        CHECK(lines(r, LineKind::Composed) == 2, "one composed line per field");
        const Result on = compose(s, b, {}, {mod("a", {batch(s, "a", "alter", R"([{"registry": "items", "select": {"id": "game:material_t1_wood_logs"}, "field": "maxStack", "op": 50}])")}),
                                             mod("b", {batch(s, "b", "alter", R"([{"registry": "items", "select": {"id": "game:material_t1_wood_logs"}, "field": "maxStack", "op": {"mul": 2, "max": 90}}])")})});
        CHECK(result(on, "items", logs, "maxStack").i == 90 && lines(on, LineKind::ComposedOn) == 1, "a relative edit composes on another mod's value (and clamps)");
    }

    // ---- D1: old paths lie below the registry; the registry wins, the old mod is never refused ----
    {
        OldWrite pack;
        pack.kind = WriterKind::OldPack, pack.mod = "oldstacks", pack.who = "oldstacks: content/a.json", pack.field = {"items", logs, "maxStack"};
        pack.op.set = true, pack.op.value = Value::integer(100);
        OldWrite sf;
        sf.kind = WriterKind::ArchiveWriter, sf.who = "shroudforge:betterstacks", sf.field = {"items", stone, "maxStack"};
        sf.op.mul = 3;
        const Result r = compose(s, b, {pack, sf}, {mod("new", {batch(s, "new", "alter", R"([{"registry": "items", "select": {"id": "game:material_*"}, "field": "maxStack", "op": {"mul": 4}}])")})});
        CHECK(result(r, "items", logs, "maxStack").i == 400, "a relative registry op composes on the old pack's value");
        CHECK(result(r, "items", stone, "maxStack").i == 240, "a relative registry op composes on the archive writer's value");
        CHECK(r.refused.empty(), "the old mod is never refused because of a registry edit");
        const auto& chain = r.fields.at({"items", logs, "maxStack"}).chain;
        CHECK(chain.size() == 2 && chain[0].kind == WriterKind::OldPack && chain[1].kind == WriterKind::Registry, "the chain keeps the old writer below the registry");
        CHECK(r.fields.at({"items", logs, "maxStack"}).base.i == 10, "the base value is kept beside the chain");
        const Result own = compose(s, b, {pack}, {mod("oldstacks", {batch(s, "oldstacks", "alter", R"([{"registry": "items", "select": {"id": "game:material_t1_wood_logs"}, "field": "maxStack", "op": 7}])")})});
        CHECK(own.lines.empty() && result(own, "items", logs, "maxStack").i == 7, "a mod's registry edit over its own old pack is no conflict");
    }

    // ---- exclusive: the later conflicting edit is refused whole ----
    {
        const Result r = compose(s, b, {}, {mod("a", {batch(s, "a", "alter", R"([{"registry": "items", "select": {"id": "game:material_t1_wood_logs"}, "field": "maxStack", "op": 100, "exclusive": true}])")}),
                                            mod("b", {batch(s, "b", "alter", R"([{"registry": "items", "select": {"id": "game:material_*"}, "field": "maxStack", "op": 200}])")})});
        CHECK(result(r, "items", logs, "maxStack").i == 100 && r.refused.size() == 1 && r.refused[0].mod == "b", "a later edit on an exclusive field is refused");
        CHECK(result(r, "items", stone, "maxStack").kind == Value::None, "a refused edit is all or nothing (its other entries are untouched too)");
        const Result later = compose(s, b, {}, {mod("a", {batch(s, "a", "alter", R"([{"registry": "items", "select": {"id": "game:material_t1_wood_logs"}, "field": "maxStack", "op": 100}])")}),
                                                mod("b", {batch(s, "b", "alter", R"([{"registry": "items", "select": {"id": "game:material_t1_wood_logs"}, "field": "maxStack", "op": 200, "exclusive": true}])")})});
        CHECK(result(later, "items", logs, "maxStack").i == 100 && later.refused.size() == 1 && later.refused[0].reason.find("exclusive") != std::string::npos,
              "an exclusive edit that comes later is the one refused");
        OldWrite patch;
        patch.kind = WriterKind::OldPatch, patch.mod = "old", patch.who = "old/caps", patch.field = {"balance", "game:balance", "levelCap"};
        patch.op.set = true, patch.op.value = Value::integer(60);
        const Result old = compose(s, b, {patch}, {mod("a", {batch(s, "a", "alter", R"([{"registry": "balance", "select": {"id": "game:balance"}, "field": "levelCap", "op": 70, "exclusive": true}])")})});
        CHECK(result(old, "balance", "game:balance", "levelCap").i == 60 && old.refused.size() == 1, "an exclusive registry edit over an old path is refused, the old path stays");
        const Result same = compose(s, b, {}, {mod("a", {batch(s, "a", "alter", R"([{"registry": "items", "select": {"id": "game:material_t1_wood_logs"}, "field": "maxStack", "op": 100, "exclusive": true}])")}),
                                               mod("b", {batch(s, "b", "alter", R"([{"registry": "items", "select": {"id": "game:material_t1_wood_logs"}, "field": "maxStack", "op": 100}])")})});
        CHECK(same.refused.empty(), "the same value from both is no exclusive conflict (today's strict rule allows it)");
    }

    // ---- yield: only where no earlier writer set the field ----
    {
        OldWrite pack;
        pack.kind = WriterKind::OldPack, pack.mod = "old", pack.who = "old: content/a.json", pack.field = {"items", logs, "maxStack"};
        pack.op.set = true, pack.op.value = Value::integer(100);
        const Result r = compose(s, b, {pack}, {mod("a", {batch(s, "a", "alter", R"([{"registry": "items", "select": {"id": "game:material_*"}, "field": "maxStack", "op": 5, "yield": true}])")})});
        CHECK(result(r, "items", logs, "maxStack").i == 100 && result(r, "items", stone, "maxStack").i == 5 && lines(r, LineKind::Yielded) == 1,
              "yield skips a field an earlier writer set and reports it");
    }

    // ---- all or nothing over a selection ----
    {
        const Result r = compose(s, b, {}, {mod("a", {batch(s, "a", "alter", R"([{"registry": "items", "select": {"id": "*"}, "field": "maxStack", "op": {"mul": 5000}}])")})});
        CHECK(r.refused.size() == 1 && r.fields.empty(), "one entry's result outside the range refuses the whole edit");
    }

    // ---- stages: every mod's add before any alter; alter sees adds and earlier alters ----
    {
        const Result r = compose(s, b, {}, {mod("a", {batch(s, "a", "alter", R"([{"registry": "items", "select": {"id": "*", "has": ["material"]}, "field": "maxStack", "op": {"mul": 2}}])")}),
                                            mod("b", {batch(s, "b", "add", R"([{"registry": "items", "entry": "add", "id": "b:crate", "like": "game:material_t1_wood_logs", "fields": {"maxStack": 30}}])")})});
        CHECK(r.added.size() == 1 && result(r, "items", "b:crate", "maxStack").i == 60, "an earlier mod's alter sees a later mod's add (and its like-copied traits)");
        const Result seq = compose(s, b, {}, {mod("a", {batch(s, "a", "alter", R"([{"registry": "items", "select": {"id": "game:material_t1_wood_logs"}, "field": "maxStack", "op": 50}])")}),
                                              mod("b", {batch(s, "b", "alter", R"([{"registry": "items", "select": {"id": "game:material_*", "where": {"maxStack": ">40"}}, "field": "maxStack", "op": {"add": 1}}])")})});
        CHECK(result(seq, "items", logs, "maxStack").i == 51 && result(seq, "items", stone, "maxStack").kind == Value::None, "a later alter's where sees an earlier alter");
    }

    // ---- final: sees add+alter only, never another mod's final; its own edits it does see; same rules as alter ----
    {
        const Result r = compose(s, b, {}, {mod("a", {batch(s, "a", "final", R"([{"registry": "items", "select": {"id": "game:material_t1_wood_logs"}, "field": "maxStack", "op": 100}])")}),
                                            mod("b", {batch(s, "b", "final", R"([{"registry": "items", "select": {"id": "game:material_*", "where": {"maxStack": ">50"}}, "field": "maxStack", "op": {"mul": 2}}])")}),
                                            mod("c", {batch(s, "c", "final", R"([{"registry": "items", "select": {"id": "game:material_t1_stone"}, "field": "maxStack", "op": 60},
                                                                                 {"registry": "items", "select": {"id": "game:material_*", "where": {"maxStack": ">50"}}, "field": "maxStack", "op": {"add": 1}}])")})});
        CHECK(result(r, "items", logs, "maxStack").i == 100, "b's final does not see a's final (its where saw 10)");
        CHECK(result(r, "items", stone, "maxStack").i == 61, "c's final sees its own final edit (read after write in one mod)");
        const Result rel = compose(s, b, {}, {mod("a", {batch(s, "a", "final", R"([{"registry": "items", "select": {"id": "game:material_t1_wood_logs"}, "field": "maxStack", "op": {"mul": 2}}])")}),
                                              mod("b", {batch(s, "b", "final", R"([{"registry": "items", "select": {"id": "game:material_t1_wood_logs"}, "field": "maxStack", "op": {"mul": 3}}])")})});
        CHECK(result(rel, "items", logs, "maxStack").i == 60 && lines(rel, LineKind::Composed) == 1, "relative finals stack, reported");
        const Result abs = compose(s, b, {}, {mod("a", {batch(s, "a", "final", R"([{"registry": "items", "select": {"id": "game:material_t1_wood_logs"}, "field": "maxStack", "op": 100}])")}),
                                              mod("b", {batch(s, "b", "final", R"([{"registry": "items", "select": {"id": "game:material_t1_wood_logs"}, "field": "maxStack", "op": 200}])")})});
        CHECK(result(abs, "items", logs, "maxStack").i == 200 && lines(abs, LineKind::Overridden) == 1, "competing finals: later wins, reported");
        const Result order = compose(s, b, {}, {mod("a", {batch(s, "a", "final", R"([{"registry": "items", "select": {"id": "game:material_t1_wood_logs"}, "field": "maxStack", "op": 7}])"),
                                                         batch(s, "a", "alter", R"([{"registry": "items", "select": {"id": "game:material_t1_wood_logs"}, "field": "maxStack", "op": 99}])")}),
                                                mod("b", {batch(s, "b", "alter", R"([{"registry": "items", "select": {"id": "game:material_t1_wood_logs"}, "field": "maxStack", "op": 98}])")})});
        CHECK(result(order, "items", logs, "maxStack").i == 7, "every alter runs before any final, whatever the batch order");
    }

    // ---- remove; dropped edits; ids never reused; removed by the game ----
    {
        const Result r = compose(s, b, {}, {mod("a", {batch(s, "a", "alter", R"([{"registry": "items", "entry": "remove", "select": {"id": "game:material_t1_wood_logs"}}])")}),
                                            mod("b", {batch(s, "b", "alter", R"([{"registry": "items", "select": {"id": "game:material_t1_wood_logs"}, "field": "maxStack", "op": 5},
                                                                                 {"registry": "items", "select": {"id": "game:material_*"}, "field": "maxStack", "op": 6}])")})});
        CHECK(r.removed.size() == 1 && lines(r, LineKind::Dropped) == 1 && r.refused.empty(), "an edit into a removed entry is dropped and reported, not refused");
        CHECK(result(r, "items", logs, "maxStack").kind == Value::None && result(r, "items", stone, "maxStack").i == 6, "a glob skips a removed entry");
        const Result twice = compose(s, b, {}, {mod("a", {batch(s, "a", "add", R"([{"registry": "items", "entry": "add", "id": "a:x"}, {"registry": "items", "entry": "add", "id": "a:x"}])")})});
        CHECK(twice.added.size() == 1 && twice.refused.size() == 1 && twice.refused[0].reason.find("never reused") != std::string::npos, "an id is added once");
        BaseModel retired = b;
        retired.retired.insert("items|zz:old");
        const Result ret = compose(s, retired, {}, {mod("zz", {batch(s, "zz", "add", R"([{"registry": "items", "entry": "add", "id": "zz:old"}])")})});
        CHECK(ret.added.empty() && ret.refused.size() == 1, "a retired id is never added again");
        const Result gone = compose(s, b, {}, {mod("a", {batch(s, "a", "alter", R"([{"registry": "items", "select": {"id": "game:old_thing"}, "field": "maxStack", "op": 5}])")})});
        CHECK(gone.refused.size() == 1 && gone.refused[0].reason.find("removed by game build 26430253") != std::string::npos, "an entry the game removed refuses the edit, saying which build");
        const Result missing = compose(s, b, {}, {mod("a", {batch(s, "a", "alter", R"([{"registry": "items", "select": {"id": "game:nothing"}, "field": "maxStack", "op": 5}])")})});
        CHECK(missing.refused.size() == 1 && missing.refused[0].reason.find("no entry") != std::string::npos, "an exact id that does not exist is refused");
    }

    // ---- D3: old debug-name selectors keep working ----
    {
        const Result r = compose(s, b, {}, {mod("a", {batch(s, "a", "alter", R"([{"registry": "items", "select": {"id": "Material_T1_Wood_Logs"}, "field": "maxStack", "op": 77}])")})});
        CHECK(result(r, "items", logs, "maxStack").i == 77, "a debug name selects its game: id");
    }

    // ---- lists ----
    {
        const Result r = compose(s, b, {}, {mod("a", {batch(s, "a", "alter", R"([{"registry": "items", "select": {"id": "game:weapon_t1_sword"}, "field": "perks", "list": {"append": ["game:perk_a"]}}])")}),
                                            mod("b", {batch(s, "b", "alter", R"([{"registry": "items", "select": {"id": "game:weapon_t1_sword"}, "field": "perks", "list": {"append": ["game:perk_b"]}}])")})});
        const Value v = result(r, "items", "game:weapon_t1_sword", "perks");
        CHECK(v.list.size() == 2 && v.list[0].t == "game:perk_a" && v.list[1].t == "game:perk_b", "both mods' appends are kept, in load order");
        const Result full = compose(s, b, {}, {mod("a", {batch(s, "a", "alter", R"([{"registry": "items", "select": {"id": "game:weapon_t1_sword"}, "field": "perks", "list": {"append": ["game:p1", "game:p2", "game:p3", "game:p4"]}}])")})});
        CHECK(full.refused.size() == 1, "a list over its size is refused");
        const Result cl = compose(s, b, {}, {mod("a", {batch(s, "a", "alter", R"([{"registry": "items", "select": {"id": "game:weapon_t1_sword"}, "field": "perks", "list": {"append": ["game:p1"]}}])")}),
                                             mod("b", {batch(s, "b", "alter", R"([{"registry": "items", "select": {"id": "game:weapon_t1_sword"}, "field": "perks", "list": {"clear": true}}])")}),
                                             mod("c", {batch(s, "c", "alter", R"([{"registry": "items", "select": {"id": "game:weapon_t1_sword"}, "field": "perks", "list": {"remove": ["game:none"]}}])")})});
        CHECK(result(cl, "items", "game:weapon_t1_sword", "perks").list.empty() && lines(cl, LineKind::Overridden) == 1 && lines(cl, LineKind::ComposedOn) == 1,
              "a clear overrides an append (reported); a remove after it composes on it");
    }

    // ---- code callbacks: alter reads the live state, final the frozen one ----
    {
        ModInput a;
        a.id = "a";
        a.code = [](Stage st, const View& v, Batch* out) {
            if (st != Stage::Alter) return;
            const auto x = v.get({"items", "game:material_t1_wood_logs", "maxStack"});
            Edit e;
            e.registry = "items", e.select.id = "game:material_t1_wood_logs", e.field = "maxStack";
            e.op.set = true, e.op.value = Value::integer(x ? x->i * 3 : 1);
            out->edits.push_back(e);
        };
        ModInput c;
        c.id = "c";
        c.code = [](Stage st, const View& v, Batch* out) {
            if (st != Stage::Final) return;
            for (const std::string& id : v.ids("items"))
                if (v.has_trait("items", id, "material")) {
                    Edit e;
                    e.registry = "items", e.select.id = id, e.field = "rolled";
                    e.op.set = true, e.op.value = Value::boolean(true);
                    out->edits.push_back(e);
                }
        };
        const Result r = compose(s, b, {}, {a, c});
        CHECK(result(r, "items", logs, "maxStack").i == 30 && result(r, "items", stone, "rolled").b, "code callbacks at their stage, on the stage's view");
        CHECK(r.refused.empty(), "code edits pass the same checks");
    }

    // ---- the live back-end re-evaluates the registry rows over another value below them ----
    {
        const Result r = compose(s, b, {}, {mod("a", {batch(s, "a", "alter", R"([{"registry": "items", "select": {"id": "game:material_t1_wood_logs"}, "field": "maxStack", "op": {"mul": 4, "max": 9999}}])")})});
        const FieldSpec& f = s.registries.at("items").fields.at("maxStack");
        CHECK(evaluate(r.fields.at({"items", logs, "maxStack"}).chain, Value::integer(3000), f).i == 9999, "a relative layer over a pack's new value");
    }

    // ---- determinism: the same inputs give the same result and the same lines ----
    {
        auto run = [&]() {
            return compose(s, b, {}, {mod("a", {batch(s, "a", "alter", R"([{"registry": "items", "select": {"id": "*"}, "field": "maxStack", "op": {"mul": 2}}])")}),
                                      mod("b", {batch(s, "b", "final", R"([{"registry": "items", "select": {"id": "game:material_*"}, "field": "maxStack", "op": {"add": 3}}])")})});
        };
        const Result x = run(), y = run();
        bool same = x.lines.size() == y.lines.size() && x.fields.size() == y.fields.size();
        for (size_t i = 0; same && i < x.lines.size(); ++i) same = x.lines[i].text == y.lines[i].text;
        CHECK(same, "two runs give the same lines");
    }

    std::printf("registry-compose-test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
