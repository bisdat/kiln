// SPDX-License-Identifier: MIT
// kiln.content registry stage over the GENERATED schema (registry/schema.g.h through schema_ir.h), not a hand-filled
// descriptor: the IR reader, composition with its reference check, and registry.json for a realistic set of edits on the
// R1 registries: items and recipes with references (a mod's added item in an added recipe, an input whose item another mod
// removes), loot rarities as flags, the balance table's fixed-length lists edited per element, an optional reference
// cleared; and the edits the generated schema must refuse (a whole fixed-length list, a list op on it, clearing a reference
// that is not optional, a field that does not exist, a value outside the generated range).
#include <cstdio>
#include <set>
#include <string>

#include "../registry/compose.h"
#include "../registry/report.h"
#include "../registry/schema_ir.h"

using namespace kc::reg;

static int g_fail = 0, g_pass = 0;
#define CHECK(c)                                                                  \
    do {                                                                          \
        if (c) ++g_pass;                                                          \
        else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } \
    } while (0)

static Value stack(const char* item, int64_t count) {  // a recipe input: {category, count, item}, keys sorted
    Value v;
    v.kind = Value::Struct;
    v.keys = {"category", "count", "item"};
    v.list = {Value{}, Value::integer(count), Value::text(item)};
    return v;
}
static Value out_stack(const char* item, int64_t count) {  // an output: {count, item}
    Value v;
    v.kind = Value::Struct;
    v.keys = {"count", "item"};
    v.list = {Value::integer(count), Value::text(item)};
    return v;
}
static Value ints(std::initializer_list<int64_t> xs) {
    std::vector<Value> v;
    for (int64_t x : xs) v.push_back(Value::integer(x));
    return Value::of_list(v);
}

static BaseModel base() {
    BaseModel b;
    for (const char* id : {"game:wood", "game:flint", "game:plank", "game:sword"}) {
        BaseModel::Entry e;
        e.id = id;
        e.fields["maxStack"] = Value::integer(10);
        e.fields["perks"] = Value::of_list({});
        b.registries["items"].push_back(e);
    }
    BaseModel::Entry arrow, plank;
    arrow.id = "game:arrow";
    arrow.fields["inputs"] = Value::of_list({stack("game:flint", 1), stack("game:plank", 1)});
    arrow.fields["outputs"] = Value::of_list({out_stack("game:sword", 5)});
    plank.id = "game:plank_recipe";
    plank.fields["inputs"] = Value::of_list({stack("game:wood", 2)});
    plank.fields["outputs"] = Value::of_list({out_stack("game:plank", 4)});
    b.registries["recipes"] = {arrow, plank};
    BaseModel::Entry loot;
    loot.id = "game:wood";
    loot.fields["rarities"] = Value::of_list({Value::text("common")});
    b.registries["loot"] = {loot};
    BaseModel::Entry bal;
    bal.id = "game:balance";
    bal.fields["xpPerLevel"] = ints({1000, 2000, 3000, 4000, 5000});
    bal.fields["altarsPerFlameLevel"] = ints({1, 1, 2, 2, 3, 3});
    bal.fields["flameLevelMax"] = Value::integer(5);
    b.registries["balance"] = {bal};
    return b;
}

static Batch batch(const Schema& s, const std::string& mod, const std::string& stage, const std::string& edits, std::vector<Refusal>* refused) {
    Batch b;
    std::string err;
    if (!parse_batch(R"({"ir": 1, "mod": ")" + mod + R"(", "stage": ")" + stage + R"(", "source": "content/)" + mod + R"(.json", "edits": )" + edits + "}", s, &b, refused, &err))
        std::printf("batch refused whole: %s\n", err.c_str());
    return b;
}

static const FieldOutcome* field(const Result& r, const char* reg, const char* id, const char* f) {
    const auto it = r.fields.find({reg, id, f});
    return it == r.fields.end() ? nullptr : &it->second;
}

static bool has_ref(const Result& r, const std::vector<EntryRef>& v, const char* reg, const char* id) {
    for (const EntryRef& e : v)
        if (e.registry == reg && e.id == id) return true;
    (void)r;
    return false;
}

static bool refused_with(const std::vector<Refusal>& v, uint32_t n, const std::string& text) {
    for (const Refusal& x : v)
        if (x.n == n && x.reason.find(text) != std::string::npos) return true;
    return false;
}

int main() {
    const Schema s = generated_schema();
    // ---- the glue: the generated rows as the IR's descriptor
    CHECK(s.format == 1 && s.registries.size() == 5);
    size_t specs = 0;
    for (const auto& [n, r] : s.registries) {
        specs += r.fields.size();
        CHECK(r.version == "0.1.0" && r.raw);
    }
    CHECK(specs == std::size(gen::kFields));  // every generated row, leaves of list elements kept
    const RegistrySpec& rec = *s.registry("recipes");
    const FieldSpec* inputs = rec.field("inputs");
    CHECK(inputs && inputs->type == FieldType::List && inputs->element == FieldType::Struct && inputs->element_fields.size() == 3);
    CHECK(inputs && inputs->element_field("item") && inputs->element_field("item")->ref == "items" && inputs->element_field("item")->optional);
    CHECK(inputs && inputs->element_field("category") && inputs->element_field("category")->ref == "inputCategories");
    CHECK(rec.field("outputs[3].item") && rec.field("outputs[3].item")->ref == "items" && !rec.field("outputs[3].item")->optional);
    CHECK(s.registry("items")->field("perks")->ref == "perks" && s.registry("items")->field("perks")->element == FieldType::Id);
    CHECK(s.registry("loot")->field("rarities")->type == FieldType::Flags && s.registry("loot")->field("rarities")->enum_values.size() == 5);
    const FieldSpec* xp = s.registry("balance")->field("xpPerLevel");
    CHECK(xp && xp->type == FieldType::List && xp->indexed && xp->fixed_length == 5);
    CHECK(s.registry("balance")->field("altarsPerFlameLevel")->indexed && s.registry("balance")->field("flameLevelMax")->read_only);
    CHECK(s.registry("drops")->field("entries")->element == FieldType::Struct && s.registry("drops")->field("entries[0].count.min"));

    // ---- what the generated schema refuses
    {
        std::vector<Refusal> rf;
        batch(s, "bad", "alter", R"([
            {"registry": "balance", "select": {"id": "game:balance"}, "field": "xpPerLevel", "op": [1, 2, 3, 4, 5]},
            {"registry": "balance", "select": {"id": "game:balance"}, "field": "xpPerLevel", "list": {"append": [6000]}},
            {"registry": "balance", "select": {"id": "game:balance"}, "field": "xpPerLevel[5]", "op": 1},
            {"registry": "recipes", "select": {"id": "game:arrow"}, "field": "outputs[0].item", "op": null},
            {"registry": "items", "select": {"id": "game:wood"}, "field": "maxStackSize", "op": 5},
            {"registry": "items", "select": {"id": "game:wood"}, "field": "maxStack", "op": 70000},
            {"registry": "loot", "select": {"id": "game:wood"}, "field": "rarities", "op": ["mythic"]},
            {"registry": "balance", "select": {"id": "game:balance"}, "field": "flameLevelMax", "op": 30}
        ])", &rf);
        for (const Refusal& x : rf) std::printf("  refused #%u: %s\n", x.n, x.reason.c_str());
        CHECK(rf.size() == 8);
        CHECK(refused_with(rf, 0, "fixed length") && refused_with(rf, 1, "fixed length") && refused_with(rf, 2, "5"));
        CHECK(refused_with(rf, 3, "outputs[0].item"));
    }

    // ---- a realistic set, composed
    std::vector<Refusal> r1, r2, r3;
    ModInput a, b, c;
    a.id = "newstuff", c.id = "tuner", b.id = "remover";
    a.batches.push_back(batch(s, "newstuff", "add", R"([
        {"registry": "items", "entry": "add", "id": "newstuff:crate", "like": "game:wood", "fields": {"maxStack": 50}},
        {"registry": "recipes", "entry": "add", "id": "newstuff:crate_recipe",
         "fields": {"inputs": [{"item": "game:wood", "count": 3}], "outputs": [{"item": "newstuff:crate", "count": 1}]}}
    ])", &r1));
    c.batches.push_back(batch(s, "tuner", "alter", R"([
        {"registry": "items", "select": {"id": "game:*"}, "field": "maxStack", "op": {"mul": 4}},
        {"registry": "items", "select": {"id": "game:sword"}, "field": "perks", "list": {"append": ["game:perk_fire"]}},
        {"registry": "loot", "select": {"id": "game:wood"}, "field": "rarities", "op": ["rare", "epic"]},
        {"registry": "balance", "select": {"id": "game:balance"}, "field": "xpPerLevel[2]", "op": 1500},
        {"registry": "balance", "select": {"id": "game:balance"}, "field": "altarsPerFlameLevel[3]", "op": {"add": 1}},
        {"registry": "recipes", "select": {"id": "game:plank_recipe"}, "field": "inputs[0].item", "op": null},
        {"registry": "recipes", "select": {"id": "game:plank_recipe"}, "field": "inputs[0].category", "op": "game:logs"}
    ])", &r2));
    b.batches.push_back(batch(s, "remover", "alter", R"([{"registry": "items", "entry": "remove", "select": {"id": "game:flint"}}])", &r3));
    for (const auto* v : {&r1, &r2, &r3})
        for (const Refusal& x : *v) std::printf("  unexpected refusal %s #%u: %s\n", x.mod.c_str(), x.n, x.reason.c_str());
    CHECK(r1.empty() && r2.empty() && r3.empty());
    const Result r = compose(s, base(), {}, {a, c, b});
    for (const Refusal& x : r.refused) std::printf("  refused in the stage %s #%u: %s\n", x.mod.c_str(), x.n, x.reason.c_str());
    CHECK(r.refused.empty());
    const FieldOutcome* ms = field(r, "items", "game:wood", "maxStack");
    CHECK(ms && ms->result.i == 40);
    CHECK(has_ref(r, r.added, "items", "newstuff:crate") && has_ref(r, r.added, "recipes", "newstuff:crate_recipe"));
    const FieldOutcome* crate = field(r, "items", "newstuff:crate", "maxStack");
    CHECK(crate && crate->result.i == 50);  // a mod's added entry is not a game:* entry
    CHECK(has_ref(r, r.removed, "items", "game:flint") && has_ref(r, r.removed, "recipes", "game:arrow"));  // its input is gone
    CHECK(!has_ref(r, r.removed, "recipes", "game:plank_recipe"));
    const FieldOutcome* perks = field(r, "items", "game:sword", "perks");
    CHECK(perks && perks->result.list.size() == 1 && perks->result.list[0].t == "game:perk_fire");  // perks: not checked here
    const FieldOutcome* rar = field(r, "loot", "game:wood", "rarities");
    CHECK(rar && rar->result.list.size() == 2 && rar->result.list[0].t == "rare" && rar->result.list[1].t == "epic");
    const FieldOutcome* xpo = field(r, "balance", "game:balance", "xpPerLevel");
    CHECK(xpo && xpo->result.list.size() == 5 && xpo->result.list[2].i == 1500 && xpo->result.list[3].i == 4000);
    const FieldOutcome* alt = field(r, "balance", "game:balance", "altarsPerFlameLevel");
    CHECK(alt && alt->result.list.size() == 6 && alt->result.list[3].i == 3);
    const FieldOutcome* pin = field(r, "recipes", "game:plank_recipe", "inputs");
    CHECK(pin && pin->result.list.size() == 1);
    if (pin && pin->result.list.size() == 1) {
        const Value& in0 = pin->result.list[0];
        CHECK(in0.leaf("item") && in0.leaf("item")->kind == Value::None);   // cleared
        CHECK(in0.leaf("category") && in0.leaf("category")->t == "game:logs");
        CHECK(in0.leaf("count") && in0.leaf("count")->i == 2);
    }

    // ---- registry.json over the generated schema
    ReportMeta meta;
    meta.pid = 1, meta.written = "2026-11-20T16:00:00Z", meta.kiln = "0.2.0-dev.31", meta.generation = 1;
    meta.regmap = {"server", "f0d824be7fcd48ebfe2458ff5c28da26060c2074e3ec1fcac587e5a1d508aeb2", "regmap/server/f0d824be7fcd48ebfe2458ff5c28da26060c2074e3ec1fcac587e5a1d508aeb2.json", true};
    meta.order = {{"newstuff", "1.0.0", ""}, {"tuner", "1.0.0", ""}, {"remover", "1.0.0", ""}};
    const json doc = report_json(s, r, meta);
    CHECK(doc["schema"]["registrySchema"] == 1 && doc["schema"]["registries"]["items"] == "0.1.0" && doc["schema"]["registries"].size() == 5);
    bool saw_ms = false, saw_xp = false;
    for (const json& f : doc["fields"]) {
        saw_ms = saw_ms || (f["registry"] == "items" && f["id"] == "game:wood" && f["field"] == "maxStack" && f["applies"] == "worldLoad");
        saw_xp = saw_xp || (f["registry"] == "balance" && f["field"] == "xpPerLevel" && f["applies"] == "restart");
    }
    CHECK(saw_ms && saw_xp);
    const auto log = report_log(r);
    CHECK(!log.empty());
    std::printf("registry-generated-test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
