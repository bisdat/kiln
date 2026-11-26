// SPDX-License-Identifier: MIT
#define main registry_live_baseline_main
#include "registry_live_test.cpp"
#undef main
#include <thread>

namespace {
int review_checks = 0, review_failures = 0;
void expect(bool ok, const char* message) {
    ++review_checks;
    if (!ok) { ++review_failures; std::printf("FAIL %s\n", message); }
}
void publish_clone(const std::vector<PackFile>& files) {
    stage(files);
    RegistryLive in = *g.in;
    in.added_guid = [](const auto&, const auto&) { return "12345678-1234-5678-9123-123456789abc"; };
    in.pin_definition = [](const auto&, const auto&, auto*) { return true; };
    in.define_entry = [](const auto&, const auto&, auto*) { return true; };
    registry_entries_prepare(&in);
    const auto body = in.entry_bodies.at({"items", "a:logs"});
    Entry clone = *reg(KILN_CONTENT_ITEMS).get(WOOD);
    clone.id = in.nums.at("items|a:logs"); clone.addr = A(0x41000); guid_parse(body.at("guid"), clone.guid);
    registry_live_install(std::move(in));
    std::memcpy(reinterpret_cast<void*>(clone.addr), reinterpret_cast<const void*>(reg(KILN_CONTENT_ITEMS).get(WOOD)->addr), 0x100);
    std::string why;
    if (!registry_live_initialize_entry(KILN_CONTENT_ITEMS, clone, "a:logs", body.at("registryFields"), &why)) throw std::runtime_error(why);
    auto& items = reg(KILN_CONTENT_ITEMS);
    items.by_id[clone.id] = static_cast<uint32_t>(items.entries.size()); items.entries.push_back(clone);
}
void baseline_cases() {
    const auto files = files_of({{"a", "a/content/entries.json"}});
    for (const char* phase : {"alter", "final"}) {
        world(); g_files.clear();
        g_files["a/content/entries.json"] = std::string(R"({"registry":{"add":[{"registry":"items","entry":"add","id":"a:logs","like":"game:material_t1_wood_logs"}],")") + phase + R"(":[{"registry":"items","select":"a:logs","field":"maxStack","op":{"mul":2}}]}})";
        publish_clone(files);
        expect(at<uint32_t>(0x41000) == 10, "constructor keeps the inherited Add baseline, not a pending Alter/Final result");
        std::string value;
        expect(registry_live_read("items", "a:logs", "maxStack", &value) == KILN_OK && value == "20", "pre-world stable read retains the composed effective view");
        registry_live_world_begin(); apply_registry_files(files);
        expect(at<uint32_t>(0x41000) == 20, "relative Alter/Final is applied exactly once to native clone bytes");
        expect(registry_live_read("items", "a:logs", "maxStack", &value) == KILN_OK && value == "20", "world stable read matches one relative application");
        registry::json recorded;
        g.in->record_entry = [&](const auto&, const auto& fields) { recorded = fields; };
        apply_registry_files(files);
        expect(recorded.value("maxStack", 0) == 20, "persisted/synced definition exports the effective value separately from its constructor");
        g_files["a/content/entries.json"] = R"({"registry":{"add":[{"registry":"items","entry":"add","id":"a:logs","like":"game:material_t1_wood_logs"}]}})";
        apply_registry_files(files);
        expect(at<uint32_t>(0x41000) == 10 && registry_live_read("items", "a:logs", "maxStack", &value) == KILN_OK && value == "10", "removing Alter/Final on pack reload restores inherited native and stable values");
        expect(recorded.value("maxStack", 0) == 10, "reload also restores the persisted effective baseline");
    }
    world(); g_files.clear();
    g_files["a/content/entries.json"] = R"({"registry":{"add":[{"registry":"items","entry":"add","id":"a:logs","like":"game:material_t1_wood_logs","fields":{"maxStack":14}}],"alter":[{"registry":"items","select":"a:logs","field":"maxStack","op":{"mul":2}}],"final":[{"registry":"items","select":"a:logs","field":"maxStack","op":{"add":3}}]}})";
    publish_clone(files);
    expect(at<uint32_t>(0x41000) == 14, "explicit Add overrides belong to the constructor before later stages");
    registry_live_world_begin(); apply_registry_files(files);
    expect(at<uint32_t>(0x41000) == 31, "explicit Add override plus Alter and Final each take effect once");
    g_files["a/content/entries.json"] = R"({"registry":{"add":[{"registry":"items","entry":"add","id":"a:logs","like":"game:material_t1_wood_logs","fields":{"maxStack":14}}]}})";
    apply_registry_files(files);
    expect(at<uint32_t>(0x41000) == 14, "reload retains the explicit Add override after removing later stages");
}
void recipe_baseline_case() {
    world(); g_files.clear(); const auto files = files_of({{"a", "a/content/recipe.json"}});
    g_files["a/content/recipe.json"] = R"({"registry":{"add":[{"registry":"recipes","entry":"add","id":"a:recipe","like":"game:plank_recipe","fields":{"duration":4}}],"alter":[{"registry":"recipes","select":"a:recipe","field":"duration","op":{"mul":2}}]}})";
    stage(files); RegistryLive in = *g.in;
    in.added_guid = [](const auto&, const auto&) { return "12345678-1234-5678-9123-123456789abc"; };
    in.pin_definition = [](const auto&, const auto&, auto*) { return true; };
    in.define_entry = [](const auto&, const auto&, auto*) { return true; };
    registry_entries_prepare(&in);
    const auto body = in.entry_bodies.at({"recipes", "a:recipe"});
    Entry clone = *reg(KILN_CONTENT_RECIPES).get(PLANK);
    clone.id = in.nums.at("recipes|a:recipe"); clone.addr = A(0x43000); guid_parse(body.at("guid"), clone.guid);
    std::memcpy(reinterpret_cast<void*>(clone.addr), reinterpret_cast<const void*>(reg(KILN_CONTENT_RECIPES).get(PLANK)->addr), 0x300);
    mem<int64_t>(0x43000, static_cast<int64_t>(body.at("duration").get<double>() * 1e9));
    registry_live_install(std::move(in));
    auto& recipes = reg(KILN_CONTENT_RECIPES); recipes.by_id[clone.id] = static_cast<uint32_t>(recipes.entries.size()); recipes.entries.push_back(clone);
    registry::json recorded;
    g.in->record_entry = [&](const auto& ref, const auto& fields) { if (ref.registry == "recipes") recorded = fields; };
    registry_live_world_begin(); apply_registry_files(files);
    expect(at<int64_t>(0x43000) == 8'000'000'000ll, "recipe relative edits apply once over the explicit Add constructor");
    expect(recorded.value("duration", 0.0) == 8 && recorded.contains("inputs") && recorded.contains("outputs") && recorded.contains("unlock"),
           "recipe save/sync fields include effective constructor-owned values and stable references");
    g_files["a/content/recipe.json"] = R"({"registry":{"add":[{"registry":"recipes","entry":"add","id":"a:recipe","like":"game:plank_recipe","fields":{"duration":4}}]}})";
    apply_registry_files(files);
    expect(at<int64_t>(0x43000) == 4'000'000'000ll && recorded.value("duration", 0.0) == 4,
           "recipe reload restores both native and exported Add-only baselines");
}
void lazy_case() {
    world(); stage({}); RegistryLive lazy = *g.in;
    lazy.base = {}; lazy.ids.clear(); lazy.nums.clear();
    bool prepared = false, outside_lock = false;
    lazy.prepare = [&](registry::stage::LiveData* data, std::string*) {
        prepared = true;
        std::thread probe([&] { outside_lock = store_mutex().try_lock(); if (outside_lock) store_mutex().unlock(); });
        probe.join();
        data->base = base(); data->ids["perks|game:perk_a"] = guid_text(P1); data->nums["perks|game:perk_a"] = 0x901; return true;
    };
    registry_live_install(std::move(lazy));
    Entry clone = *reg(KILN_CONTENT_ITEMS).get(WOOD); clone.addr = A(0x41000);
    std::memcpy(reinterpret_cast<void*>(clone.addr), reinterpret_cast<const void*>(reg(KILN_CONTENT_ITEMS).get(WOOD)->addr), 0x100);
    std::string why;
    expect(registry_live_initialize_entry(KILN_CONTENT_ITEMS, clone, "a:logs", {{"maxStack", 27}, {"perks", registry::json::array()}}, &why) && !prepared,
           "ordinary warm construction with no perk refs does not prepare the copied base");
    {
        std::lock_guard lock(store_mutex());
        expect(!registry_live_initialize_entry(KILN_CONTENT_ITEMS, clone, "a:logs", {{"perks", {"game:perk_a"}}}, &why) && !prepared && why.find("store lock") != std::string::npos,
               "recursive registration defers cold identity work without decoding under the store lock");
    }
    registry_live_world_begin();
    g.in->on_world_thread = [](uint32_t) { return false; };
    expect(!registry_live_initialize_entry(KILN_CONTENT_ITEMS, clone, "a:logs", {{"perks", {"game:perk_a"}}}, &why) && !prepared,
           "wrong-thread identity preparation is refused before construction");
    g.in->on_world_thread = [](uint32_t) { return true; };
    const bool restored = registry_live_initialize_entry(KILN_CONTENT_ITEMS, clone, "a:logs", {{"perks", {"game:perk_a"}}}, &why);
    expect(restored && prepared, "missing-mod nonempty perk refs trigger lazy identity preparation before construction");
    expect(outside_lock, "cold identity decoding leaves the store mutex available to other threads");
    bool again = false;
    g.in->prepare = [&](registry::stage::LiveData*, std::string*) { again = true; return false; };
    expect(registry_live_initialize_entry(KILN_CONTENT_ITEMS, clone, "a:logs", {{"perks", {"game:perk_a"}}}, &why) && !again,
           "already prepared perk identities do not trigger another decode");
    expect(!registry_live_initialize_entry(KILN_CONTENT_ITEMS, clone, "a:logs", {{"perks", {"game:missing_perk"}}}, &why) && again && why.find("could not be prepared") != std::string::npos,
           "failed identity preparation refuses the placeholder with a named cause");
}
void drop_case() {
    for (uint32_t point : {1u, 2u, 3u, 4u}) for (const char* phase : {"alter", "final"})
    for (const char* field : {"entries[1].weight", "entries[1].chance", "entries[1].count.max", "entries[*].weight"}) {
    world(); add_drop_table(); stage({}); RegistryLive in = *g.in;
    auto& table = in.base.registries.at("drops").front(); table.drop_groups = {{0,2}};
    auto& survivor = table.fields.at("entries").list[1];
    survivor.list[static_cast<size_t>(std::find(survivor.keys.begin(), survivor.keys.end(), "item") - survivor.keys.begin())] = registry::Value::text("game:sword_t2");
    mem<uint32_t>(0x31040 + 0x10, SWORD);
    registry::ModInput mod; mod.id = "hide"; registry::Batch removal; std::vector<registry::Refusal> refused; std::string why;
    registry::parse_batch(std::string(R"({"ir":1,"mod":"hide","stage":"alter","edits":[{"registry":"items","entry":"remove","select":"game:material_t1_wood_logs"}]})"), *in.schema, &removal, &refused, &why);
    mod.batches = {removal}; in.mods = {mod}; in.result = registry::compose(*in.schema, in.base, {}, in.mods);
    registry_live_install(std::move(in)); registry_live_world_begin(); apply_registry_files({});
    uint64_t handle = 0; RegistryLiveReply reply;
    registry_live_begin(91, "editor", point, &handle);
    const registry::json ir = {{"ir", 1}, {"mod", "editor"}, {"stage", phase}, {"edits", {
        {{"registry", "drops"}, {"select", "game:dt_wolf"}, {"field", field}, {"op", 0}},
        {{"registry", "items"}, {"select", "game:material_stone"}, {"field", "maxStack"}, {"op", 77}}
    }}};
    registry_live_submit(91, handle, ir.dump(), &reply, nullptr);
    registry_live_commit(91, handle, &reply);
    expect(registry_live_result()->removed.size() == 3 && at<float>(0x31000 + 0x20) == 0 && at<float>(0x31040 + 0x20) > 0, "later field edits preserve an established removal and at least one viable candidate");
    expect(reply.refused > 0 && at<uint32_t>(STONE_AT) == 77, "unsafe field target is refused while unrelated Manual/automatic field edits retain existing semantics");
    expect(at<uint32_t>(0x31018) > 0 && at<uint32_t>(0x31058) > 0 && at<float>(0x31040 + 0x1c) > 0,
           "chance/count refusals leave a viable native survivor and physical count slots intact");
    }
    auto copied = g.in->base;
    auto other = copied.registries.at("drops").front(); other.id = "game:dt_other";
    other.fields.at("entries").list[0] = other.fields.at("entries").list[1];
    copied.registries["drops"].push_back(other);
    const auto previous = registry::compose(*g.in->schema, copied, {}, g.in->mods);
    registry::ModInput editor; editor.id = "editor";
    registry::Batch batch; batch.mod = editor.id; batch.stage = registry::Stage::Alter; batch.source = "review fixture";
    registry::Edit replace; replace.n = 1; replace.registry = "drops"; replace.select.id = "game:dt_wolf"; replace.field = "entries";
    replace.list = registry::ListOp::Replace;
    replace.list_values = previous.fields.at({"drops", "game:dt_wolf", "entries"}).result.list;
    auto& stack = replace.list_values[1];
    stack.list[static_cast<size_t>(std::find(stack.keys.begin(), stack.keys.end(), "weight") - stack.keys.begin())] = registry::Value::number(0);
    batch.edits = {replace}; editor.batches = {batch};
    const auto replaced = registry::compose_after(*g.in->schema, copied, previous, {editor});
    expect(replaced.removed == previous.removed && replaced.refused.size() > previous.refused.size() &&
           replaced.fields.at({"drops", "game:dt_wolf", "entries"}).result == previous.fields.at({"drops", "game:dt_wolf", "entries"}).result,
           "whole-list replacement cannot bypass established-removal viability");
    replace.list = registry::ListOp::None; replace.list_values.clear(); replace.select.id = "game:*"; replace.per_entry = true;
    replace.element = {true, "entries", false, 1, "weight"}; replace.op.set = true; replace.op.value = registry::Value::number(0);
    batch.edits = {replace}; editor.batches = {batch};
    const auto selected = registry::compose_after(*g.in->schema, copied, previous, {editor});
    expect(selected.refused.size() > previous.refused.size() && selected.fields.at({"drops", "game:dt_wolf", "entries"}).result.list[1].leaf("weight")->as_double() > 0 &&
           selected.fields.at({"drops", "game:dt_other", "entries"}).result.list[1].leaf("weight")->as_double() == 0,
           "per-entry selection refuses the unsafe target without losing an unrelated safe table edit");
    world(); add_drop_table(); g_files.clear();
    const auto files = files_of({{"hide", "hide/content/drop.json"}});
    g_files["hide/content/drop.json"] = R"({"registry":{"alter":[{"registry":"items","entry":"remove","select":"game:material_t1_wood_logs"}]}})";
    stage(files); RegistryLive reload = *g.in;
    auto& reloaded_table = reload.base.registries.at("drops").front(); reloaded_table.drop_groups = {{0,2}};
    auto& reloaded_survivor = reloaded_table.fields.at("entries").list[1];
    reloaded_survivor.list[static_cast<size_t>(std::find(reloaded_survivor.keys.begin(), reloaded_survivor.keys.end(), "item") - reloaded_survivor.keys.begin())] = registry::Value::text("game:sword_t2");
    mem<uint32_t>(0x31040 + 0x10, SWORD);
    reload.result = registry::compose(*reload.schema, reload.base, {}, reload.mods);
    registry_live_install(std::move(reload)); registry_live_world_begin(); apply_registry_files(files);
    g_files["hide/content/drop.json"] = R"({"registry":{"alter":[{"registry":"items","entry":"remove","select":"game:material_t1_wood_logs"},{"registry":"drops","select":"game:dt_wolf","field":"entries[1].weight","op":0},{"registry":"items","select":"game:material_stone","field":"maxStack","op":77}]}})";
    apply_registry_files(files);
    expect(registry_live_result()->removed.size() == 3 && at<float>(0x31000 + 0x20) == 0 && at<float>(0x31040 + 0x20) > 0 && at<uint32_t>(STONE_AT) == 77,
           "pack reload refuses an unsafe new field target while retaining established removals and unrelated fields");
}
}
int main(int argc, char** argv) {
    g_registry_stack_guard = &fake_guard;  // the host has no game code to patch: the guard as on a build that has it
    std::vector<std::string> errors; g_regmap = std::make_shared<registry::map::RegMap>();
    if (!registry::map::load_dir(KILN_REGMAP_DIR, "server", "f0d824be7fcd48ebfe2458ff5c28da26060c2074e3ec1fcac587e5a1d508aeb2", g_regmap.get(), &errors)) return 2;
    const std::string which = argc > 1 ? argv[1] : "all";
    if (which == "all" || which == "baseline") baseline_cases();
    if (which == "all" || which == "recipe") recipe_baseline_case();
    if (which == "all" || which == "lazy") lazy_case();
    if (which == "all" || which == "drop") drop_case();
    std::printf("registry-entry-review: %d checks, %d failed\n", review_checks, review_failures);
    return review_failures ? 1 : 0;
}
