// SPDX-License-Identifier: MIT
// Host test of the stage's reference check (registry/compose.cpp, mod-dev feedback adopted for P1): an entry whose reference
// (a recipe's input or output item, any id field) names a refused, removed, retired or missing entry is refused too, per
// entry, with the cause chain in the refusal ("recipes a:lantern_recipe refused: outputs[0].item a:lantern refused:
// Unmapped ..."), so a recipe never stays active when its output item failed. A mod's added entry is refused whole; an edit
// that brings such a reference into an existing entry is refused there; a game entry whose own data names an entry a mod
// removed is removed with it, reported.
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

static FieldSpec spec(const char* name, FieldType t, const char* ref = "") {
    FieldSpec f;
    f.name = name, f.type = t, f.ref = ref, f.applies = Applies::Now;
    return f;
}

static Schema schema() {
    Schema s;
    RegistrySpec items;
    items.name = "items";
    FieldSpec power = spec("power", FieldType::Int);
    power.min = 0, power.max = 1000;
    FieldSpec up = spec("upgradesTo", FieldType::Id, "items");
    FieldSpec perks = spec("perks", FieldType::List, "perks");  // a registry this schema does not have: not checked here
    perks.element = FieldType::Id;
    for (const FieldSpec& f : {power, up, perks}) items.fields[f.name] = f;
    s.registries["items"] = items;
    RegistrySpec recipes;
    recipes.name = "recipes";
    for (const char* list : {"inputs", "outputs"}) {
        FieldSpec l = spec(list, FieldType::List);
        FieldSpec item = spec((std::string(list) + "[].item").c_str(), FieldType::Id, "items");
        FieldSpec count = spec((std::string(list) + "[].count").c_str(), FieldType::Int);
        count.min = 1, count.max = 999;
        for (const FieldSpec& f : {l, item, count}) recipes.fields[f.name] = f;
    }
    s.registries["recipes"] = recipes;
    link_elements(&s);
    return s;
}

static Value stack(const char* item, int64_t count) {
    Value v;
    v.kind = Value::Struct;
    v.keys = {"count", "item"};
    v.list = {Value::integer(count), Value::text(item)};
    return v;
}

static BaseModel base() {
    BaseModel b;
    for (const char* id : {"game:wood", "game:plank", "game:flint", "game:arrow_item"}) {
        BaseModel::Entry e;
        e.id = id;
        e.fields["power"] = Value::integer(1);
        b.registries["items"].push_back(e);
    }
    BaseModel::Entry gone;
    gone.id = "game:old_ore";
    gone.removed_in = "26430253";
    b.registries["items"].push_back(gone);
    b.retired.insert("items|zz:retired_thing");
    BaseModel::Entry plank;
    plank.id = "game:plank_recipe";
    plank.fields["inputs"] = Value::of_list({stack("game:wood", 2)});
    plank.fields["outputs"] = Value::of_list({stack("game:plank", 4)});
    BaseModel::Entry arrow;
    arrow.id = "game:arrow";
    arrow.fields["inputs"] = Value::of_list({stack("game:flint", 1), stack("game:plank", 1)});
    arrow.fields["outputs"] = Value::of_list({stack("game:arrow_item", 5)});
    b.registries["recipes"] = {plank, arrow};
    return b;
}

static Batch batch(const Schema& s, const std::string& mod, const std::string& stage, const std::string& edits, std::vector<Refusal>* refused = nullptr) {
    Batch b;
    std::vector<Refusal> r;
    std::string err;
    if (!parse_batch(R"({"ir": 1, "mod": ")" + mod + R"(", "stage": ")" + stage + R"(", "source": "content/)" + mod + R"(.json", "edits": )" + edits + "}", s, &b, &r, &err))
        std::printf("batch: %s\n", err.c_str());
    if (refused) *refused = r;
    else
        for (const Refusal& x : r) std::printf("unexpected refusal %s #%u: %s\n", x.mod.c_str(), x.n, x.reason.c_str());
    return b;
}

static ModInput mod(const std::string& id, std::vector<Batch> batches, std::vector<Refusal> pre = {}) {
    ModInput m;
    m.id = id;
    m.batches = std::move(batches);
    m.refused = std::move(pre);
    return m;
}

static const Refusal* refusal(const Result& r, const std::string& text) {
    for (const Refusal& x : r.refused)
        if (x.reason.find(text) != std::string::npos) return &x;
    return nullptr;
}

static bool added(const Result& r, const std::string& reg, const std::string& id) {
    for (const EntryRef& e : r.added)
        if (e.registry == reg && e.id == id) return true;
    return false;
}

static bool removed(const Result& r, const std::string& reg, const std::string& id) {
    for (const EntryRef& e : r.removed)
        if (e.registry == reg && e.id == id) return true;
    return false;
}

static bool touched(const Result& r, const std::string& reg, const std::string& id) {
    for (const auto& [ref, fo] : r.fields)
        if (ref.registry == reg && ref.id == id) return true;
    return false;
}

static const std::string kLanternRecipe = R"([{"registry": "recipes", "entry": "add", "id": "a:lantern_recipe",
                                              "fields": {"inputs": [{"item": "game:wood", "count": 3}], "outputs": [{"item": "a:lantern", "count": 1}]}}])";

int main() {
    const Schema s = schema();
    const BaseModel b = base();

    // ---- the IR names a refused add's id ----
    {
        std::vector<Refusal> rf;
        batch(s, "a", "add", R"([{"registry": "items", "entry": "add", "id": "a:lantern", "fields": {"power": 5000}}])", &rf);
        CHECK(rf.size() == 1 && rf[0].entry == "items|a:lantern", "a refused add carries the entry it would have made");
    }

    // ---- a recipe whose output item's add was refused is refused too, with the cause chain ----
    {
        Refusal pre;
        pre.mod = "a", pre.source = "content/a.json", pre.stage = Stage::Add, pre.n = 0, pre.entry = "items|a:lantern";
        pre.reason = "Unmapped: items.power is not mapped for game build 1287369 yet";
        const Result r = compose(s, b, {}, {mod("a", {batch(s, "a", "add", kLanternRecipe)}, {pre})});
        const Refusal* x = refusal(r, "recipes a:lantern_recipe refused: outputs[0].item a:lantern refused: Unmapped: items.power");
        CHECK(x && x->mod == "a" && x->stage == Stage::Add && x->n == 0 && x->source == "content/a.json", "the recipe's add is refused, naming the chain and its own edit");
        CHECK(!added(r, "recipes", "a:lantern_recipe") && !touched(r, "recipes", "a:lantern_recipe"), "the refused recipe is not added and leaves no field behind");
        CHECK(r.refused.size() == 1, "the item's own refusal is not repeated (it was the IR's or the mapping's)");
    }

    // ---- a reference to an entry that does not exist, was removed by the game build, or is retired ----
    {
        const Result miss = compose(s, b, {}, {mod("a", {batch(s, "a", "add", kLanternRecipe)})});
        CHECK(refusal(miss, "outputs[0].item a:lantern does not exist") && !added(miss, "recipes", "a:lantern_recipe"), "a missing output item refuses the recipe");
        const Result old = compose(s, b, {}, {mod("a", {batch(s, "a", "add", R"([{"registry": "recipes", "entry": "add", "id": "a:ore_recipe", "fields": {"inputs": [{"item": "game:old_ore", "count": 1}]}}])")})});
        CHECK(refusal(old, "inputs[0].item game:old_ore was removed by game build 26430253"), "an input the game build removed refuses the recipe");
        const Result ret = compose(s, b, {}, {mod("a", {batch(s, "a", "add", R"([{"registry": "recipes", "entry": "add", "id": "a:z", "fields": {"outputs": [{"item": "zz:retired_thing", "count": 1}]}}])")})});
        CHECK(refusal(ret, "zz:retired_thing is retired"), "a retired id refuses the recipe");
        const Result fine = compose(s, b, {}, {mod("a", {batch(s, "a", "add", R"([{"registry": "items", "entry": "add", "id": "a:lantern"}])" ),
                                                        batch(s, "a", "add", kLanternRecipe)})});
        CHECK(fine.refused.empty() && added(fine, "recipes", "a:lantern_recipe"), "the same recipe with its item added is taken");
        const Result perks = compose(s, b, {}, {mod("a", {batch(s, "a", "alter", R"([{"registry": "items", "select": {"id": "game:wood"}, "field": "perks", "list": {"append": ["game:no_such_perk"]}}])")})});
        CHECK(perks.refused.empty(), "a reference into a registry this schema does not have is not checked here (S5's model does)");
    }

    // ---- the chain goes on: item -> item -> recipe ----
    {
        Refusal pre;
        pre.mod = "a", pre.source = "content/a.json", pre.stage = Stage::Add, pre.n = 0, pre.entry = "items|a:lantern";
        pre.reason = "Unmapped: items.power is not mapped for game build 1287369 yet";
        const Result r = compose(s, b, {}, {mod("a", {batch(s, "a", "add", R"([{"registry": "items", "entry": "add", "id": "a:lantern2", "fields": {"upgradesTo": "a:lantern"}}])")}, {pre}),
                                            mod("c", {batch(s, "c", "add", R"([{"registry": "recipes", "entry": "add", "id": "c:r", "fields": {"outputs": [{"item": "a:lantern2", "count": 1}]}}])")})});
        CHECK(refusal(r, "recipes c:r refused: outputs[0].item a:lantern2 refused: upgradesTo a:lantern refused: Unmapped"), "the cause chain runs through every link");
        CHECK(!added(r, "items", "a:lantern2") && !added(r, "recipes", "c:r") && r.refused.size() == 2, "both links refused, each once");
    }

    // ---- an edit that brings a bad reference into an existing entry is refused there; the others stand ----
    {
        const Result r = compose(s, b, {}, {mod("c", {batch(s, "c", "alter", R"([{"registry": "recipes", "select": {"id": "game:arrow"}, "field": "inputs", "list": {"append": [{"item": "c:ghost", "count": 1}]}}])")}),
                                            mod("d", {batch(s, "d", "alter", R"([{"registry": "recipes", "select": {"id": "game:arrow"}, "field": "inputs[*].count", "op": {"add": 1}}])")})});
        const auto it = r.fields.find({"recipes", "game:arrow", "inputs"});
        CHECK(it != r.fields.end() && it->second.result.list.size() == 2 && it->second.result.list[0].leaf("count")->i == 2 && it->second.result.list[1].leaf("count")->i == 2,
              "c's append is taken out; d's change applies over what is left");
        const Refusal* x = refusal(r, "inputs[2].item c:ghost does not exist");
        CHECK(x && x->mod == "c" && !removed(r, "recipes", "game:arrow"), "c's edit is refused for game:arrow, naming the reference; the recipe stays");
        CHECK(it != r.fields.end() && it->second.chain.size() == 1 && it->second.chain[0].mod == "d", "the chain keeps only the rows that stand");
    }

    // ---- a game entry acts only on what the stage took away (review of ee378645, work-bb's rulings): in a registry whose
    // entries are removed it goes with the entry a mod removed; a refused mod entry never takes a game entry with it ----
    {
        const Result r = compose(s, b, {}, {mod("b", {batch(s, "b", "alter", R"([{"registry": "items", "entry": "remove", "select": {"id": "game:wood"}}])")})});
        CHECK(removed(r, "items", "game:wood") && removed(r, "recipes", "game:plank_recipe") && !removed(r, "recipes", "game:arrow"), "the plank recipe is removed with its input");
        bool line = false;
        for (const Line& l : r.lines) line = line || (l.kind == LineKind::Dropped && l.text.find("recipes game:plank_recipe removed: inputs[0].item game:wood was removed by b") != std::string::npos);
        CHECK(line && r.refused.empty(), "...and reported with its cause, nothing refused");
        const Result two = compose(s, b, {}, {mod("b", {batch(s, "b", "alter", R"([{"registry": "items", "entry": "remove", "select": {"id": "game:plank"}}])")})});
        CHECK(removed(two, "recipes", "game:plank_recipe") && removed(two, "recipes", "game:arrow"), "every recipe naming the removed item, as input or output");
        const Result mine = compose(s, b, {}, {mod("b", {batch(s, "b", "alter", R"([{"registry": "items", "entry": "remove", "select": {"id": "game:wood"}}])")}),
                                               mod("a", {batch(s, "a", "add", R"([{"registry": "recipes", "entry": "add", "id": "a:board", "fields": {"inputs": [{"item": "game:wood", "count": 1}]}}])")})});
        CHECK(refusal(mine, "recipes a:board refused: inputs[0].item game:wood was removed by b") && !added(mine, "recipes", "a:board"),
              "a mod's new recipe naming a removed item is refused, with the cause");
        Refusal pre;
        pre.mod = "a", pre.stage = Stage::Add, pre.entry = "items|a:lantern", pre.reason = "Unmapped: items.power";
        ModInput a = mod("a", {batch(s, "a", "alter", R"([{"registry": "recipes", "select": {"id": "game:arrow"}, "field": "outputs", "list": {"replace": [{"item": "a:lantern", "count": 1}]}}])")}, {pre});
        const Result kept = compose(s, b, {}, {a});
        CHECK(!removed(kept, "recipes", "game:arrow") && kept.fields.find({"recipes", "game:arrow", "outputs"}) == kept.fields.end() && refusal(kept, "a:lantern refused: Unmapped"),
              "a game recipe given a refused mod item keeps its own output: only the edit is taken out");
    }

    std::printf("registry-refs-test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
