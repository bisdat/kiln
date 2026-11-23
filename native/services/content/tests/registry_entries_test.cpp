// SPDX-License-Identifier: MIT
// Entry descriptors and dependency closure before the real definition/hiding backend is admitted.
#include <cstdio>
#include "../registry/compose.h"
using namespace kc::registry;
static int passed = 0, failed = 0;
#define CHECK(c, text) do { if (c) ++passed; else { ++failed; std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, text); } } while (0)
static Schema schema() {
    Schema s;
    for (const char* reg : {"items", "recipes", "loot"}) s.registries[reg].name = reg;
    FieldSpec f; f.name = "maxStack"; f.type = FieldType::Int; f.min = 1; f.max = 65535;
    s.registries["items"].fields[f.name] = f;
    f = {}; f.name = "outputs"; f.type = FieldType::List; f.element = FieldType::Id; f.ref = "items";
    s.registries["recipes"].fields[f.name] = f;
    return s;
}
static BaseModel base() {
    BaseModel b;
    b.registries["items"] = {{"game:wood", {{"maxStack", Value::integer(10)}}, {"stackable"}}};
    b.registries["loot"] = {{"game:wood", {}}};
    b.registries["recipes"] = {{"game:board", {{"outputs", Value::of_list({Value::text("game:wood")})}}}};
    return b;
}
static ModInput input(const Schema& s, json edits, Stage phase = Stage::Add) {
    ModInput m; m.id = "a"; Batch b; std::string why;
    CHECK(parse_batch(json{{"ir", 1}, {"mod", "a"}, {"stage", stage_name(phase)}, {"source", "content/test.json"}, {"edits", edits}}, s, &b, &m.refused, &why), "batch is usable");
    m.batches.push_back(std::move(b)); return m;
}
static bool has(const std::vector<EntryRef>& rows, const char* reg, const char* id) {
    return std::find(rows.begin(), rows.end(), EntryRef{reg, id}) != rows.end();
}
static bool reason(const Result& r, const char* text) {
    return std::any_of(r.refused.begin(), r.refused.end(), [&](const auto& x) { return x.reason.find(text) != std::string::npos; });
}
template<class R> static void descriptor(const R& r) {
    if constexpr (requires { r.definitions; }) {
        const auto it = r.definitions.find({"items", "a:logs"});
        CHECK(it != r.definitions.end(), "added item has a binding descriptor");
        if (it != r.definitions.end()) {
            CHECK(it->second.like == "game:wood" && it->second.base == "game:wood", "descriptor keeps stable like and terminal base");
            CHECK(it->second.mod == "a" && it->second.source == "content/test.json" && it->second.n == 0, "descriptor keeps bound author provenance");
            CHECK(it->second.values.at("maxStack") == Value::integer(20), "descriptor includes final effective values");
            CHECK(it->second.traits.count("stackable") == 1, "descriptor keeps inherited traits");
        }
    } else CHECK(false, "added item has a binding descriptor");
}
int main() {
    const auto s = schema(); const auto b = base();
    for (const auto& bad : {"a:x:y", "a:x/y", "a:x~12345678", "a:", "a:x y"}) {
        auto m = input(s, json::array({{{"registry", "items"}, {"entry", "add"}, {"id", bad}, {"like", "game:wood"}}}));
        CHECK(m.refused.size() == 1 && m.batches.front().edits.empty(), "added IDs enforce the complete mod-entry grammar");
    }
    auto m = input(s, json::array({{{"registry", "items"}, {"entry", "add"}, {"id", "a:logs"}, {"like", "game:wood"}}}));
    m.batches.push_back(input(s, json::array({{{"registry", "items"}, {"select", "a:logs"}, {"field", "maxStack"}, {"op", 20}}}), Stage::Alter).batches.front());
    descriptor(compose(s, b, {}, {m}));
    auto remove = input(s, json::array({{{"registry", "items"}, {"entry", "remove"}, {"select", "game:wood"}}}), Stage::Alter);
    auto r = compose(s, b, {}, {remove});
    CHECK(has(r.removed, "loot", "game:wood"), "hiding an item disables its implicit loot entry");
    CHECK(has(r.removed, "recipes", "game:board"), "hiding an item hides dependent game recipes");
    auto chain = input(s, json::array({
        {{"registry", "items"}, {"entry", "add"}, {"id", "a:first"}, {"like", "game:wood"}},
        {{"registry", "items"}, {"entry", "add"}, {"id", "a:second"}, {"like", "a:first"}},
        {{"registry", "recipes"}, {"entry", "add"}, {"id", "a:recipe"}, {"fields", {{"outputs", {"a:second"}}}}}
    }));
    r = compose(s, b, {}, {chain, remove});
    CHECK(!has(r.added, "items", "a:first") && !has(r.added, "items", "a:second") && !has(r.added, "recipes", "a:recipe"), "removed clone base refuses the full dependent chain");
    CHECK(reason(r, "like game:wood was removed by a") && reason(r, "a:second refused: like a:first refused:"), "clone refusal chains name their root cause");
    std::printf("registry-entries-test: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
