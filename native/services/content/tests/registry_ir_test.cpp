// SPDX-License-Identifier: MIT
// Host test of the edit IR's shapes the generated schema needs (registry/ir.cpp, registry/compose.cpp): lists whose elements
// have fields of their own (recipes.inputs {item, count}), element paths ("inputs[2].count", "inputs[*].count",
// "altarsPerFlameLevel[2]") that edit inside the list's own chain, flags (a set of the schema's names), field() reading
// "[3]" and "[*]" as "[]", and the data pack section {"add" | "alter" | "final": [edits]} lowered to IR batches.
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

static FieldSpec spec(const char* name, FieldType t, Applies a = Applies::Now) {
    FieldSpec f;
    f.name = name, f.type = t, f.applies = a;
    return f;
}

static Schema schema() {
    Schema s;
    RegistrySpec recipes;
    recipes.name = "recipes";
    FieldSpec inputs = spec("inputs", FieldType::List);
    inputs.max_items = 8;
    FieldSpec item = spec("inputs[].item", FieldType::Id);
    FieldSpec count = spec("inputs[].count", FieldType::Int);
    count.min = 1, count.max = 999;
    FieldSpec cat = spec("inputs[].category", FieldType::Int);
    cat.read_only = true;
    FieldSpec dur = spec("duration", FieldType::Float);
    dur.min = 0, dur.max = 3600;
    for (const FieldSpec& f : {inputs, item, count, cat, dur}) recipes.fields[f.name] = f;
    s.registries["recipes"] = recipes;
    RegistrySpec items;
    items.name = "items";
    FieldSpec tags = spec("tags", FieldType::Flags);
    tags.enum_values = {"storage", "fuel", "quest"};
    FieldSpec perks = spec("perks", FieldType::List);
    perks.element = FieldType::Id, perks.max_items = 3;
    for (const FieldSpec& f : {tags, perks}) items.fields[f.name] = f;
    s.registries["items"] = items;
    RegistrySpec bal;
    bal.name = "balance", bal.add = bal.remove = false;
    FieldSpec altars = spec("altarsPerFlameLevel", FieldType::List, Applies::Restart);
    altars.element = FieldType::Int, altars.min = 0, altars.max = 64;
    bal.fields[altars.name] = altars;
    s.registries["balance"] = bal;
    link_elements(&s);
    return s;
}

static Value input(const char* item, int64_t count) {
    Value v;
    v.kind = Value::Struct;
    v.keys = {"count", "item"};
    v.list = {Value::integer(count), Value::text(item)};
    return v;
}

static BaseModel base() {
    BaseModel b;
    BaseModel::Entry r;
    r.id = "game:arrow";
    r.fields["inputs"] = Value::of_list({input("game:wood", 2), input("game:flint", 3)});
    r.fields["duration"] = Value::number(1.0);
    b.registries["recipes"] = {r};
    BaseModel::Entry it;
    it.id = "game:chest";
    it.fields["tags"] = Value::of_list({Value::text("storage")});
    it.fields["perks"] = Value::of_list({});
    b.registries["items"] = {it};
    BaseModel::Entry bal;
    bal.id = "game:balance";
    bal.fields["altarsPerFlameLevel"] = Value::of_list({Value::integer(1), Value::integer(2), Value::integer(3)});
    b.registries["balance"] = {bal};
    return b;
}

static Batch batch(const Schema& s, const std::string& mod, const std::string& stage, const std::string& edits, std::vector<Refusal>* refused = nullptr) {
    Batch b;
    std::vector<Refusal> r;
    std::string err;
    if (!parse_batch(R"({"ir": 1, "mod": ")" + mod + R"(", "stage": ")" + stage + R"(", "source": "t", "edits": )" + edits + "}", s, &b, &r, &err))
        std::printf("batch: %s\n", err.c_str());
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

static std::string counts(const Value& list) {
    std::string out;
    for (const Value& e : list.list)
        if (const Value* c = e.leaf("count")) out += (out.empty() ? "" : ",") + std::to_string(c->i);
    return out;
}

static std::string names(const Value& list) {
    std::string out;
    for (const Value& e : list.list) out += (out.empty() ? "" : ",") + e.t;
    return out;
}

static std::string alter(const std::string& reg, const std::string& id, const std::string& field, const std::string& opkey, const std::string& op) {
    return R"([{"registry": ")" + reg + R"(", "select": {"id": ")" + id + R"("}, "field": ")" + field + R"(", ")" + opkey + R"(": )" + op + "}]";
}

int main() {
    const Schema s = schema();
    const BaseModel b = base();

    // ---- field(): an element's index reads as [] ----
    {
        CHECK(generic_path("inputs[3].count") == "inputs[].count" && generic_path("inputs[*].count") == "inputs[].count" && generic_path("a[1][2]") == "a[][]",
              "generic_path");
        const RegistrySpec& r = *s.registry("recipes");
        CHECK(r.field("inputs[2].count") && r.field("inputs[2].count")->name == "inputs[].count" && r.field("inputs[*].item"), "field() finds a leaf by an indexed path");
        CHECK(r.field("inputs")->element == FieldType::Struct && r.field("inputs")->element_fields.size() == 3 && r.field("inputs")->element_field("count"),
              "link_elements: the list's element has its leaves");
        CHECK(!r.field("nothing[1].count"), "field(): no such list");
    }

    // ---- lists of elements with fields ----
    {
        std::vector<Refusal> rf;
        Batch x = batch(s, "a", "alter", R"([
            {"registry": "recipes", "select": {"id": "game:arrow"}, "field": "inputs", "list": {"append": [{"item": "game:stone", "count": 4}]}},
            {"registry": "recipes", "select": {"id": "game:arrow"}, "field": "inputs", "list": {"append": [{"item": "game:stone", "colour": 4}]}},
            {"registry": "recipes", "select": {"id": "game:arrow"}, "field": "inputs", "list": {"append": [{"item": "game:stone", "count": 0}]}},
            {"registry": "recipes", "select": {"id": "game:arrow"}, "field": "inputs", "list": {"append": [{"category": 3}]}},
            {"registry": "recipes", "select": {"id": "game:arrow"}, "field": "inputs", "list": {"append": [5]}}
        ])", &rf);
        CHECK(x.edits.size() == 2 && rf.size() == 3, "an element is {leaf: value}: unknown leaves, out-of-range values and plain values refused; a read-only leaf is given in a new element");
        const Result r = compose(s, b, {}, {mod("a", {x})});
        const Value v = result(r, "recipes", "game:arrow", "inputs");
        CHECK(v.list.size() == 4 && counts(v) == "2,3,4" && v.list[2].leaf("item")->t == "game:stone" && v.list[3].leaf("category") && v.list[3].leaf("category")->i == 3,
              "appended elements with fields (a read-only leaf set when the element is made)");
    }

    // ---- element paths: inside the list's own chain ----
    {
        const Result one = compose(s, b, {}, {mod("a", {batch(s, "a", "alter", alter("recipes", "game:arrow", "inputs[1].count", "op", R"({"mul": 2})"))})});
        CHECK(counts(result(one, "recipes", "game:arrow", "inputs")) == "2,6" && one.refused.empty(), "inputs[1].count x2");
        CHECK(one.fields.count({"recipes", "game:arrow", "inputs"}) && one.fields.size() == 1, "an element edit is a row of the list's chain");
        const Result all = compose(s, b, {}, {mod("a", {batch(s, "a", "alter", R"([{"registry": "recipes", "select": {"id": "game:arrow"}, "field": "inputs", "list": {"append": [{"item": "game:stone", "count": 4}]}}])")}),
                                              mod("b", {batch(s, "b", "alter", alter("recipes", "game:arrow", "inputs[*].count", "op", R"({"add": 1})"))})});
        CHECK(counts(result(all, "recipes", "game:arrow", "inputs")) == "3,4,5" && all.lines.size() == 1 && all.lines[0].kind == LineKind::Composed,
              "inputs[*].count +1 after another mod's append: every element, composed and reported");
        const Result far = compose(s, b, {}, {mod("a", {batch(s, "a", "alter", alter("recipes", "game:arrow", "inputs[5].count", "op", "7"))})});
        CHECK(far.refused.size() == 1 && far.refused[0].reason.find("no element 5") != std::string::npos, "an element that is not there refuses the edit");
        const Result high = compose(s, b, {}, {mod("a", {batch(s, "a", "alter", alter("recipes", "game:arrow", "inputs[*].count", "op", R"({"mul": 400})"))})});
        CHECK(high.refused.size() == 1 && high.fields.empty(), "a leaf's range holds for every element (all or nothing)");
        const Result whole = compose(s, b, {}, {mod("a", {batch(s, "a", "alter", alter("recipes", "game:arrow", "inputs[0]", "op", R"({"set": {"item": "game:iron", "count": 1}})"))})});
        const Value w = result(whole, "recipes", "game:arrow", "inputs");
        CHECK(w.list.size() == 2 && w.list[0].leaf("item")->t == "game:iron" && counts(w) == "1,3", "a whole element set with {set: {...}}");
        const Result alt = compose(s, b, {}, {mod("a", {batch(s, "a", "alter", alter("balance", "game:balance", "altarsPerFlameLevel[2]", "op", R"({"add": 10})"))})});
        const Value av = result(alt, "balance", "game:balance", "altarsPerFlameLevel");
        CHECK(av.list.size() == 3 && av.list[2].i == 13, "a plain list's element: a relative change with the list's range");
        std::vector<Refusal> rf;
        batch(s, "a", "alter", R"([
            {"registry": "recipes", "select": {"id": "game:arrow"}, "field": "inputs[1].count", "list": {"clear": true}},
            {"registry": "recipes", "select": {"id": "game:arrow"}, "field": "inputs[1].colour", "op": 1},
            {"registry": "recipes", "select": {"id": "game:arrow"}, "field": "inputs[1].category", "op": 1},
            {"registry": "balance", "select": {"id": "game:balance"}, "field": "altarsPerFlameLevel[1].count", "op": 1},
            {"registry": "recipes", "select": {"id": "game:arrow"}, "field": "inputs[0]", "op": {"item": "game:iron"}},
            {"registry": "recipes", "select": {"id": "game:arrow"}, "field": "inputs[x].count", "op": 1},
            {"registry": "recipes", "select": {"id": "game:arrow"}, "field": "duration[1]", "op": 1},
            {"registry": "balance", "select": {"id": "game:balance"}, "field": "altarsPerFlameLevel[1]", "op": 99}
        ])", &rf);
        CHECK(rf.size() == 8, "element paths: a list op, an unknown or read-only leaf, a leaf of a plain list, a plain object, a bad index, a non-list, a value out of range: refused");
        std::vector<Refusal> rf2;
        Batch norm = batch(s, "a", "alter", alter("recipes", "game:arrow", "inputs[02].count", "op", "5"), &rf2);
        CHECK(norm.edits.size() == 1 && norm.edits[0].field == "inputs[2].count" && norm.edits[0].element.index == 2, "an element path is kept with a plain index");
    }

    // ---- flags: a set of the schema's names ----
    {
        const Result set = compose(s, b, {}, {mod("a", {batch(s, "a", "alter", alter("items", "game:chest", "tags", "op", R"(["Quest", "storage"])"))})});
        CHECK(names(result(set, "items", "game:chest", "tags")) == "storage,quest", "flags set: names in any case, held in the schema's order");
        std::vector<Refusal> rf;
        batch(s, "a", "alter", alter("items", "game:chest", "tags", "op", R"(["fuel", "FUEL"])") + "", &rf);
        CHECK(rf.size() == 1 && rf[0].reason.find("twice") != std::string::npos, "flags: a name twice is refused");
        std::vector<Refusal> rf2;
        batch(s, "a", "alter", alter("items", "game:chest", "tags", "op", R"({"mul": 2})"), &rf2);
        CHECK(rf2.size() == 1, "flags take no relative op");
        const Result un = compose(s, b, {}, {mod("a", {batch(s, "a", "alter", alter("items", "game:chest", "tags", "list", R"({"append": ["quest", "storage"]})"))}),
                                             mod("b", {batch(s, "b", "alter", alter("items", "game:chest", "tags", "list", R"({"append": ["fuel"]})"))}),
                                             mod("c", {batch(s, "c", "alter", alter("items", "game:chest", "tags", "list", R"({"remove": ["storage"]})"))})});
        CHECK(names(result(un, "items", "game:chest", "tags")) == "fuel,quest", "flags: appends are a union (each name once), remove takes out");
        const Result cl = compose(s, b, {}, {mod("a", {batch(s, "a", "alter", alter("items", "game:chest", "tags", "list", R"("clear")"))})});
        CHECK(result(cl, "items", "game:chest", "tags").list.empty(), "\"list\": \"clear\" (the section's short form)");
    }

    // ---- a plain list's whole value through op ----
    {
        const Result r = compose(s, b, {}, {mod("a", {batch(s, "a", "alter", alter("items", "game:chest", "perks", "op", R"(["game:p1", "game:p2"])"))})});
        CHECK(names(result(r, "items", "game:chest", "perks")) == "game:p1,game:p2", "op with a list sets a list field whole");
        std::vector<Refusal> rf;
        batch(s, "a", "alter", alter("items", "game:chest", "perks", "op", R"(["game:p1", "game:p2", "game:p3", "game:p4"])"), &rf);
        CHECK(rf.size() == 1, "...within its size");
    }

    // ---- the data pack section {"add" | "alter" | "final": [edits]} ----
    {
        const json section = json::parse(R"({
            "final": [{"registry": "recipes", "select": {"id": "game:arrow"}, "field": "duration", "op": {"mul": 0.5}}],
            "add": [{"registry": "recipes", "entry": "add", "id": "pack:bolt", "like": "game:arrow"}],
            "alter": [{"registry": "recipes", "select": {"id": "pack:bolt"}, "field": "inputs[0].count", "op": 9},
                      {"registry": "recipes", "select": {"id": "game:arrow"}, "field": "nope", "op": 1}]
        })");
        std::vector<Batch> out;
        std::vector<Refusal> rf;
        std::string err;
        const bool ok = lower_section(section, "pack", "1.0.0", "content/bolts.json", s, &out, &rf, &err);
        CHECK(ok && out.size() == 3 && out[0].stage == Stage::Add && out[1].stage == Stage::Alter && out[2].stage == Stage::Final, "one batch per stage, in stage order");
        CHECK(ok && out[1].mod == "pack" && out[1].source == "content/bolts.json" && out[1].mod_version == "1.0.0" && out[1].edits.size() == 1 && rf.size() == 1,
              "the batches carry mod, source and version; a bad edit is refused alone");
        if (ok) {
            const Result r = compose(s, b, {}, {mod("pack", out)});
            CHECK(r.added.size() == 1 && counts(result(r, "recipes", "pack:bolt", "inputs")) == "9,3" && result(r, "recipes", "game:arrow", "duration").d == 0.5,
                  "the lowered section composes: add, then alter of the new entry's element, then final");
        }
        std::vector<Batch> o2;
        CHECK(!lower_section(json::parse(R"({"later": []})"), "pack", "", "c", s, &o2, &rf, &err) && err.find("later") != std::string::npos, "an unknown stage refuses the section");
        CHECK(!lower_section(json::parse(R"({"alter": {}})"), "pack", "", "c", s, &o2, &rf, &err), "a stage is a list");
        CHECK(!lower_section(json::parse(R"({"alter": []})"), "game", "", "c", s, &o2, &rf, &err), "a section of a reserved mod id is refused");
        CHECK(lower_section(json::object(), "pack", "", "c", s, &o2, &rf, &err) && o2.empty(), "an empty section is no batch");
    }

    // ---- batch_json: the plan keeps a start's edits as IR that parse_batch reads back the same (registry-rollback) ----
    {
        const std::string alters = R"([
            {"n": 40, "registry": "recipes", "select": {"id": "game:arr*", "where": {"duration": ">=1"}, "has": ["t1"], "hasNot": ["old"]},
             "field": "duration", "op": {"mul": 0.5, "add": 1, "min": 0, "max": 60, "raiseOnly": true}, "exclusive": true},
            {"n": 70, "registry": "recipes", "select": {"id": "game:arrow"}, "field": "duration", "op": 2.5, "yield": true},
            {"registry": "recipes", "select": {"id": "game:arrow"}, "field": "inputs", "list": {"append": [{"item": "game:stone", "count": 4}]}},
            {"registry": "recipes", "select": {"id": "game:arrow"}, "field": "inputs", "list": {"remove": [{"item": "game:wood"}]}},
            {"registry": "recipes", "select": {"id": "game:arrow"}, "field": "inputs", "list": {"replace": []}},
            {"registry": "recipes", "select": {"id": "game:arrow"}, "field": "inputs", "list": "clear"},
            {"registry": "recipes", "select": {"id": "game:arrow"}, "field": "inputs[1].count", "op": {"mul": 2}},
            {"registry": "recipes", "select": {"id": "game:arrow"}, "field": "inputs[*].item", "op": "game:iron"},
            {"registry": "recipes", "select": {"id": "game:arrow"}, "field": "inputs[0]", "op": {"set": {"item": "game:iron", "count": 1}}},
            {"registry": "items", "select": {"id": "game:chest"}, "field": "tags", "op": ["quest", "fuel"]},
            {"registry": "items", "select": {"id": "game:chest"}, "field": "tags", "list": {"append": ["storage"]}},
            {"registry": "items", "select": {"id": "game:chest"}, "field": "perks[1]", "op": "game:perk_a"},
            {"registry": "items", "entry": "remove", "select": {"id": "game:chest"}},
            {"registry": "balance", "select": {"id": "game:balance"}, "field": "altarsPerFlameLevel[2]", "op": {"add": 10}}])";
        const std::string adds = R"([{"n": 3, "registry": "recipes", "entry": "add", "id": "a:bolt", "like": "game:arrow",
                                      "fields": {"duration": 3, "inputs": [{"item": "game:stone", "count": 2}]}}])";
        Batch alter_b = batch(s, "a", "alter", alters), add_b = batch(s, "a", "add", adds);
        alter_b.mod_version = "1.2.3";
        CHECK(alter_b.edits.size() == 14 && add_b.edits.size() == 1, "the fixture parses whole");
        for (const Batch* b0 : {&alter_b, &add_b}) {
            const json doc = batch_json(*b0);
            Batch back;
            std::vector<Refusal> rf;
            std::string err;
            const bool ok = parse_batch(doc.dump(), s, &back, &rf, &err);
            CHECK(ok && rf.empty() && back.edits.size() == b0->edits.size(), ("batch_json reads back whole: " + err + " " + doc.dump().substr(0, 300)).c_str());
            CHECK(ok && back.mod == b0->mod && back.mod_version == b0->mod_version && back.stage == b0->stage && back.source == b0->source,
                  "mod, version, stage and source kept");
            CHECK(ok && batch_json(back) == doc, "a second trip writes the same document");
            for (size_t i = 0; ok && i < back.edits.size() && i < b0->edits.size(); ++i) {
                const Edit &x = b0->edits[i], &y = back.edits[i];
                CHECK(x.n == y.n && x.registry == y.registry && x.field == y.field && x.select.id == y.select.id && x.select.where == y.select.where &&
                          x.select.has == y.select.has && x.select.has_not == y.select.has_not && x.op.set == y.op.set && x.op.value == y.op.value &&
                          x.op.mul == y.op.mul && x.op.add == y.op.add && x.op.min == y.op.min && x.op.max == y.op.max && x.op.raise_only == y.op.raise_only &&
                          x.list == y.list && x.list_values == y.list_values && x.entry == y.entry && x.add_id == y.add_id && x.like == y.like &&
                          x.add_fields == y.add_fields && x.exclusive == y.exclusive && x.yield == y.yield && x.element.on == y.element.on,
                      ("edit " + std::to_string(i) + " is the same after the trip").c_str());
            }
        }
        const Result r1 = compose(s, b, {}, {mod("a", {alter_b})});
        Batch again;
        std::vector<Refusal> rf;
        std::string err;
        parse_batch(batch_json(alter_b).dump(), s, &again, &rf, &err);
        const Result r2 = compose(s, b, {}, {mod("a", {again})});
        CHECK(r1.fields.size() == r2.fields.size() && r1.refused.size() == r2.refused.size(), "the read-back batch composes the same");
        for (const auto& [ref, fo] : r1.fields) {
            const auto it = r2.fields.find(ref);
            CHECK(it != r2.fields.end() && it->second.result == fo.result, ("the same result for " + ref.text()).c_str());
        }
    }

    std::printf("registry-ir-test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
