// SPDX-License-Identifier: MIT
#define main registry_live_baseline_main
#include "registry_live_test.cpp"
#undef main
int main() {
    int failures = 0, entry_checks = 0;
    auto entry_check = [&](bool yes, const char* text) { ++entry_checks; if (!yes) { ++failures; std::printf("FAIL %s\n", text); } };
    std::vector<std::string> errors;
    g_regmap = std::make_shared<registry::map::RegMap>();
    if (!registry::map::load_dir(KILN_REGMAP_DIR, "server", "f0d824be7fcd48ebfe2458ff5c28da26060c2074e3ec1fcac587e5a1d508aeb2", g_regmap.get(), &errors)) return 2;
    world(); stage({});
    const auto original = reg(KILN_CONTENT_ITEMS).get(WOOD);
    entry_check(original != nullptr, "base item exists");
    Entry clone = *original; clone.id = 0x991; clone.addr = A(0x41000);
    std::memcpy(reinterpret_cast<void*>(clone.addr), reinterpret_cast<const void*>(original->addr), 0x100);
    std::string why;
    entry_check(registry_live_initialize_entry(KILN_CONTENT_ITEMS, clone, "a:logs", {{"maxStack", 99}}, &why), "unpublished clone is initialized through the stable field mapper");
    entry_check(at<uint32_t>(0x41000) == 99 && at<uint32_t>(WOOD_AT) == 10, "clone value changes without touching its base");
    entry_check(registry_live_status().active == 0, "initializing a definition creates no live transaction");
    entry_check(!registry_live_initialize_entry(KILN_CONTENT_ITEMS, clone, "a:logs", {{"notAField", 99}}, &why), "unknown field refuses the unpublished definition");
    entry_check(why.find("notAField") != std::string::npos, "initialization failure names the Kiln field");
    entry_check(!registry_live_initialize_entry(KILN_CONTENT_ITEMS, clone, "a:logs", {{"fuel.burnTime", 30}}, &why), "unproven field does not bypass mapping guards");
    entry_check(!registry_live_initialize_entry(KILN_CONTENT_ITEMS, clone, "a:logs", {{"hasDamage", true}}, &why), "read-only initialization is refused");
    Entry sword_clone = *reg(KILN_CONTENT_ITEMS).get(SWORD);
    // The fake config resolver recognizes this id and resolves slots from this clone's own address.
    sword_clone.addr = A(0x42000);
    std::memcpy(reinterpret_cast<void*>(sword_clone.addr), reinterpret_cast<const void*>(reg(KILN_CONTENT_ITEMS).get(SWORD)->addr), 0x100);
    g_integer_impacts.insert("manaCost");
    entry_check(!registry_live_initialize_entry(KILN_CONTENT_ITEMS, sword_clone, "a:sword", {{"manaCost", 1.5}}, &why), "inexact whole-number config refuses the clone");
    entry_check(why.starts_with("not_exact: "), "whole-number refusal keeps the stable reason token");
    g_integer_impacts.clear();
    RegistryLive in = *g.in;
    in.added_guid = [](const auto&, const auto&) { return "12345678-1234-5678-9123-123456789abc"; };
    registry::EntryDefinition d;
    d.mod = "a"; d.like = d.base = "game:material_t1_wood_logs";
    d.values["maxStack"] = registry::Value::integer(99);
    d.constructor_values = d.values;
    auto body = registry_entry_definition({"items", "a:logs"}, d, in, &why);
    entry_check(body.is_object(), "stable added item lowers through the existing definition shape");
    if (body.is_object()) {
        entry_check(body.value("guid", "") == in.added_guid("items", "a:logs") && body.value("base", "") == guid_text(G1), "definition binds existing stable identity and canonical base GUID");
        entry_check(body["registryFields"]["maxStack"] == 99, "definition keeps stable effective values for initialization and persistence");
        entry_check(body.value("name", "").find(':') == std::string::npos && body.value("registryId", "") == "a:logs", "internal legal debug name does not replace the public Kiln identity");
    }
    d.like.clear();
    entry_check(registry_entry_definition({"items", "a:logs"}, d, in, &why).is_null(), "add without a clone source is honestly refused");
    d.like = d.base = "game:material_t1_wood_logs";
    entry_check(registry_entry_definition({"items", "other:logs"}, d, in, &why).is_null(), "definition cannot mint another mod's stable id");
    in.nums["items|game:collision"] = fnv1a([] { static uint8_t guid[16]; guid_parse("12345678-1234-5678-9123-123456789abc", guid); return guid; }(), 16);
    entry_check(registry_entry_definition({"items", "a:logs"}, d, in, &why).is_null() && why.find("collision") != std::string::npos, "numeric collision refuses instead of reminting the stable id");
    in = *g.in; in.added_guid = [](const auto& reg, const auto&) { return reg == "items" ? "12345678-1234-5678-9123-123456789abc" : "22345678-1234-5678-9123-123456789abc"; };
    registry::ModInput mod; mod.id = "a"; registry::Batch batch; std::vector<registry::Refusal> refusals;
    const registry::json adds = {{"ir", 1}, {"mod", "a"}, {"stage", "add"}, {"edits", {
        {{"registry", "items"}, {"entry", "add"}, {"id", "a:logs"}, {"like", "game:material_t1_wood_logs"}},
        {{"registry", "recipes"}, {"entry", "add"}, {"id", "a:recipe"}, {"like", "game:plank_recipe"}, {"fields", {{"outputs", {{{"item", "a:logs"}, {"count", 1}}}}}}}
    }}};
    entry_check(registry::parse_batch(adds, *in.schema, &batch, &refusals, &why) && refusals.empty(), "definition integration fixture parses");
    mod.batches.push_back(batch); in.mods = {mod}; in.result = registry::compose(*in.schema, in.base, {}, in.mods);
    std::map<registry::EntryRef, registry::json> queued;
    in.define_entry = [&](const auto& ref, const auto& body, auto*) { queued[ref] = body; return true; };
    in.forget_entry = [&](const auto& ref) { queued.erase(ref); };
    in.pin_definition = [](const auto&, const auto&, auto*) { return true; }; // isolated definition-binding fixture
    registry_entries_prepare(&in);
    entry_check(queued.size() == 2 && in.result.definitions.size() == 2, "both stable entries queue through the definition adapter");
    entry_check(queued.at({"recipes", "a:recipe"})["outputs"][0]["item"] == in.added_guid("items", "a:logs"), "recipe references bind a new item's stable GUID");
    in = *g.in; in.added_guid = [](const auto& reg, const auto&) { return reg == "items" ? "12345678-1234-5678-9123-123456789abc" : "22345678-1234-5678-9123-123456789abc"; };
    in.mods = {mod}; in.result = registry::compose(*in.schema, in.base, {}, in.mods); queued.clear();
    in.define_entry = [&](const auto& ref, const auto& body, auto* error) {
        if (ref.registry == "items") { *error = "injected item definition refusal"; return false; }
        queued[ref] = body; return true;
    };
    in.forget_entry = [&](const auto& ref) { queued.erase(ref); };
    in.pin_definition = [](const auto&, const auto&, auto*) { return true; }; // isolated definition-binding fixture
    registry_entries_prepare(&in);
    entry_check(queued.empty() && in.result.added.empty(), "definition failure refuses dependent recipes and forgets their unpublished definitions");
    entry_check(std::any_of(in.result.refused.begin(), in.result.refused.end(), [](const auto& r) { return r.registry == "recipes" && r.reason.find("injected item definition refusal") != std::string::npos; }), "definition failure root reaches dependent recipe refusal");
    RegistryLive revised = *g.in; revised.added_guid = in.added_guid;
    registry::ModInput revising; revising.id = "a"; registry::Batch first, later;
    registry::parse_batch(registry::json{{"ir", 1}, {"mod", "a"}, {"stage", "add"}, {"edits", {
        {{"registry", "recipes"}, {"entry", "add"}, {"id", "a:recipe"}, {"like", "game:plank_recipe"}},
        {{"registry", "items"}, {"entry", "add"}, {"id", "a:logs"}, {"like", "game:material_t1_wood_logs"}}
    }}}, *revised.schema, &first, &refusals, &why);
    registry::parse_batch(std::string(R"({"ir":1,"mod":"a","stage":"alter","edits":[{"registry":"recipes","select":"a:recipe","field":"outputs","list":{"replace":[{"item":"a:logs","count":1}]}}]})"), *revised.schema, &later, &refusals, &why);
    revising.batches = {first, later}; revised.mods = {revising}; revised.result = registry::compose(*revised.schema, revised.base, {}, revised.mods); queued.clear();
    revised.pin_definition = [](const auto&, const auto&, auto*) { return true; };
    revised.define_entry = [&](const auto& ref, const auto& definition, auto* error) {
        if (ref.registry == "items") { *error = "injected later definition failure"; return false; }
        queued[ref] = definition; return true;
    };
    revised.forget_entry = [&](const auto& ref) { queued.erase(ref); };
    registry_entries_prepare(&revised);
    entry_check(revised.result.added.size() == 1 && revised.result.definitions.count({"recipes", "a:recipe"}), "a refused field reference preserves the independently valid recipe definition");
    entry_check(queued.at({"recipes", "a:recipe"}) == registry_entry_definition({"recipes", "a:recipe"}, revised.result.definitions.at({"recipes", "a:recipe"}), revised, &why) &&
                revised.entry_bodies.at({"recipes", "a:recipe"})["registryFields"]["outputs"][0]["item"] == "game:sword_t2",
                "recomposition retains a valid native constructor and a separate restored effective reference baseline after a later definition failure");
    registry::Batch removal; refusals.clear();
    entry_check(registry::parse_batch(registry::json{{"ir", 1}, {"mod", "a"}, {"stage", "alter"}, {"edits", {{{"registry", "items"}, {"entry", "remove"}, {"select", "game:material_t1_wood_logs"}}}}}, *in.schema, &removal, &refusals, &why), "removal gate fixture parses");
    auto proof = *g_regmap;
    auto unproven = removal; proof.build.rows["drops.entries[].chance"] = registry::map::BuildRow::Unproven;
    registry::refuse_unmapped_edits(proof, *in.schema, &unproven, &refusals, true);
    entry_check(unproven.edits.empty() && refusals.size() == 1 && refusals.front().reason.find("drop tables not proven on this build") != std::string::npos, "unproven drop rows refuse the whole item removal with the named reason");
    proof.build.rows["drops.entries[].chance"] = registry::map::BuildRow::Ok; refusals.clear();
    registry::refuse_unmapped_edits(proof, *in.schema, &removal, &refusals, true);
    entry_check(removal.edits.size() == 1 && refusals.empty(), "proven drop rows admit removal to the group-safety preflight");
    world(); stage({}); registry_live_world_begin(); apply_registry_files({});
    registry::json recorded;
    g.in->entry_bodies[{"items", "game:material_t1_wood_logs"}] = {{"registryFields", {{"maxStack", 10}}}};
    g.in->record_entry = [&](const auto&, const auto& fields) { recorded = fields; };
    uint64_t handle = 0; RegistryLiveReply reply;
    entry_check(registry_live_begin(91, "recorder", 4, &handle) == KILN_OK, "effective-record change begins");
    registry_live_submit(91, handle, R"({"ir":1,"mod":"recorder","stage":"alter","edits":[{"registry":"items","select":"game:material_t1_wood_logs","field":"maxStack","op":77}]})", &reply, nullptr);
    registry_live_commit(91, handle, &reply);
    entry_check(recorded.is_object() && recorded.value("maxStack", 0) == 77, "definition export records actual effective values before a save");
    registry_live_revert(91, handle, &reply);
    entry_check(recorded.is_object() && recorded.value("maxStack", 0) == 10, "reverting a world layer restores the recorded baseline rather than retaining stale effective values");
    world(); stage({}); registry_live_world_begin(); apply_registry_files({});
    auto timing_doc = R"({"ir":1,"mod":"timing","stage":"alter","edits":[{"registry":"recipes","entry":"remove","select":"game:plank_recipe"},{"registry":"items","select":"game:material_t1_wood_logs","field":"maxStack","op":77}]})";
    registry_live_begin(92, "timing", 4, &handle); registry_live_submit(92, handle, timing_doc, &reply, nullptr); registry_live_commit(92, handle, &reply);
    entry_check(reply.outcome == RegistryLiveOutcome::RestartRequired && at<uint32_t>(WOOD_AT) == 10, "Manual with a restart-only entry op writes none of its otherwise valid fields");
    registry_live_begin(92, "timing", 1, &handle); registry_live_submit(92, handle, timing_doc, &reply, nullptr); registry_live_commit(92, handle, &reply);
    entry_check(reply.outcome == RegistryLiveOutcome::Applied && reply.refused == 1 && at<uint32_t>(WOOD_AT) == 77, "automatic later passes keep unrelated fields and count the entry refusal");
    entry_check(reply.reason.find("RestartRequired") != std::string::npos, "later entry refusal has the explicit RestartRequired reason");
    registry_live_begin(93, "lateadd", 4, &handle);
    registry_live_submit(93, handle, R"({"ir":1,"mod":"lateadd","stage":"add","edits":[{"registry":"items","entry":"add","id":"lateadd:logs","like":"game:material_t1_wood_logs"}]})", &reply, nullptr);
    registry_live_commit(93, handle, &reply);
    entry_check(reply.outcome == RegistryLiveOutcome::RestartRequired, "a later Add also reports RestartRequired, not generic unmapped refusal");
    world(); stage({});
    RegistryLive additions = *g.in; additions.mods = {mod}; additions.result = registry::compose(*additions.schema, additions.base, {}, additions.mods);
    additions.ids["items|a:logs"] = "12345678-1234-5678-9123-123456789abc"; additions.nums["items|a:logs"] = 0x991;
    Entry actual_clone = *reg(KILN_CONTENT_ITEMS).get(WOOD); actual_clone.id = 0x991; actual_clone.addr = A(0x41000);
    std::memcpy(reinterpret_cast<void*>(actual_clone.addr), reinterpret_cast<const void*>(reg(KILN_CONTENT_ITEMS).get(WOOD)->addr), 0x100);
    auto& native_items = reg(KILN_CONTENT_ITEMS); native_items.by_id[actual_clone.id] = static_cast<uint32_t>(native_items.entries.size()); native_items.entries.push_back(actual_clone);
    registry_live_install(std::move(additions));
    std::string value;
    entry_check(registry_live_read("items", "a:logs", "maxStack", &value) == KILN_OK && value == "10", "new entries expose inherited stable values before a world exists");
    registry_live_world_begin(); apply_registry_files({});
    registry_live_begin(94, "editor", 4, &handle);
    registry_live_submit(94, handle, R"({"ir":1,"mod":"editor","stage":"alter","edits":[{"registry":"items","select":"a:logs","field":"maxStack","op":88}]})", &reply, nullptr);
    registry_live_commit(94, handle, &reply);
    entry_check(reply.outcome == RegistryLiveOutcome::Applied && at<uint32_t>(0x41000) == 88, "later mapped edits bind newly added stable ids through prepared composition");
    world(); stage({});
    RegistryLive lifecycle = *g.in; lifecycle.mods = {mod}; lifecycle.result = registry::compose(*lifecycle.schema, lifecycle.base, {}, lifecycle.mods);
    bool registration_ok = false;
    lifecycle.entry_registered = [&](const auto& ref, auto* error) {
        if (ref.registry == "items" && !registration_ok) { *error = "injected native registration failure"; return false; }
        return true;
    };
    registry_live_install(std::move(lifecycle)); registry_live_world_begin(); apply_registry_files({});
    auto lifecycle_result = registry_live_result();
    entry_check(lifecycle_result && lifecycle_result->added.empty(), "native registration failure refuses the entry and dependent recipe for this world");
    entry_check(lifecycle_result && std::any_of(lifecycle_result->refused.begin(), lifecycle_result->refused.end(), [](const auto& r) { return r.registry == "recipes" && r.reason.find("injected native registration failure") != std::string::npos; }), "native registration root reason reaches the dependent recipe");
    registry_live_world_stop(); registration_ok = true; registry_live_world_begin(); apply_registry_files({});
    lifecycle_result = registry_live_result();
    entry_check(lifecycle_result && lifecycle_result->added.size() == 2, "process GameStart definitions retry in the next world without losing their original intent");
    world(); stage({});
    RegistryLive hiding = *g.in; hiding.mods = {{}}; hiding.mods.front().id = "hide";
    registry::Batch hide_batch; refusals.clear();
    registry::parse_batch(registry::json{{"ir", 1}, {"mod", "hide"}, {"stage", "alter"}, {"edits", {{{"registry", "recipes"}, {"entry", "remove"}, {"select", "game:plank_recipe"}}}}}, *hiding.schema, &hide_batch, &refusals, &why);
    hiding.mods.front().batches = {hide_batch}; hiding.result = registry::compose(*hiding.schema, hiding.base, {}, hiding.mods);
    bool visibility_ok = false, published_hidden = false;
    hiding.prepare_visibility = [&](const auto& result, const auto&) {
        std::map<registry::EntryRef, std::string> failed;
        published_hidden = false;
        if (!visibility_ok) for (const auto& ref : result.removed) failed[ref] = "injected visibility preparation failure";
        return failed;
    };
    hiding.publish_visibility = [&] { published_hidden = !g.in->result.removed.empty(); };
    registry_live_install(std::move(hiding)); registry_live_world_begin(); apply_registry_files({});
    auto hidden_result = registry_live_result();
    entry_check(hidden_result && hidden_result->removed.empty() && !published_hidden, "visibility preparation failure does not publish or report an unapplied removal");
    entry_check(hidden_result && std::any_of(hidden_result->refused.begin(), hidden_result->refused.end(), [](const auto& r) { return r.reason.find("injected visibility preparation failure") != std::string::npos; }), "visibility preparation failure names the refused Kiln entry");
    registry_live_world_stop(); visibility_ok = true; registry_live_world_begin(); apply_registry_files({});
    hidden_result = registry_live_result();
    entry_check(hidden_result && hidden_result->removed.size() == 1 && published_hidden, "visibility is published after successful application and retries in the next world");
    world(); g_files.clear();
    const auto reload_files = files_of({{"frozen", "frozen/content/entries.json"}});
    g_files["frozen/content/entries.json"] = R"({"registry":{"add":[{"registry":"items","entry":"add","id":"frozen:logs","like":"game:material_t1_wood_logs","fields":{"maxStack":21}}],"alter":[{"registry":"recipes","entry":"remove","select":"game:plank_recipe"},{"registry":"items","select":"game:material_stone","field":"maxStack","op":60}]}})";
    stage(reload_files); registry_live_world_begin(); apply_registry_files(reload_files);
    g_files["frozen/content/entries.json"] = R"({"registry":{"add":[{"registry":"items","entry":"add","id":"frozen:logs","like":"game:material_t1_wood_logs","fields":{"maxStack":99}}],"alter":[{"registry":"items","select":"game:material_stone","field":"maxStack","op":70}]}})";
    apply_registry_files(reload_files);
    auto frozen = registry_live_result();
    entry_check(frozen && frozen->definitions.at({"items", "frozen:logs"}).values.at("maxStack").i == 21 && frozen->removed.size() == 1, "hot reload freezes changed additions and deleted removals at their GameStart intent");
    entry_check(frozen && std::any_of(frozen->refused.begin(), frozen->refused.end(), [](const auto& r) { return r.reason.find("RestartRequired") != std::string::npos; }) && at<uint32_t>(STONE_AT) == 70, "hot reload reports entry intent changes while applying unrelated live fields");
    apply_registry_files({}); frozen = registry_live_result();
    entry_check(frozen && frozen->added.size() == 1 && frozen->removed.size() == 1, "deleting an entire pack retains committed GameStart entry intent until restart");
    world(); add_drop_table(); stage({});
    RegistryLive drop_failure = *g.in;
    auto& drop_base = drop_failure.base.registries.at("drops").front(); drop_base.drop_groups = {{0, 2}};
    auto& survivor = drop_base.fields.at("entries").list[1];
    survivor.list[static_cast<size_t>(std::find(survivor.keys.begin(), survivor.keys.end(), "item") - survivor.keys.begin())] = registry::Value::text("game:sword_t2");
    mem<uint32_t>(0x31040 + 0x10, SWORD);
    registry::ModInput removal_mod; removal_mod.id = "a"; removal_mod.batches = {removal}; drop_failure.mods = {removal_mod};
    drop_failure.result = registry::compose(*drop_failure.schema, drop_failure.base, {}, drop_failure.mods);
    entry_check(drop_failure.result.removed.size() == 3, "proven two-stack removal fixture hides item, its loot and its dependent recipe");
    registry_live_install(std::move(drop_failure)); registry_live_world_begin(); g_write_limits = {0}; apply_registry_files({});
    const auto failed_drop = registry_live_result();
    entry_check(failed_drop && failed_drop->removed.empty(), "failed drop-disable write refuses whole item removal without partial recipe or loot hiding");
    entry_check(failed_drop && std::any_of(failed_drop->refused.begin(), failed_drop->refused.end(), [](const auto& row) { return row.registry == "items" && row.reason.find("drop exclusion") != std::string::npos; }), "failed drop-disable write reports the root item rather than a false hidden outcome");
    auto removal_world = [&] (bool table_loaded) {
        RegistryLive fresh = *g.in;
        fresh.result = registry::compose(*fresh.schema, fresh.base, {}, fresh.mods);
        world(); if (table_loaded) add_drop_table(); mem<uint32_t>(0x31040 + 0x10, SWORD);
        registry_live_install(std::move(fresh)); registry_live_world_begin(); apply_registry_files({});
    };
    removal_world(true);
    auto removed_live = registry_live_result();
    entry_check(removed_live && removed_live->removed.size() == 3 && at<float>(0x31000 + 0x1C) == 0 && at<float>(0x31000 + 0x20) == 0 && at<float>(0x31040 + 0x1C) == 0.5f, "proven removal disables only its physical stack and retains surviving indices");
    entry_check(reg(KILN_CONTENT_ITEMS).get(WOOD) && reg(KILN_CONTENT_RECIPES).get(PLANK), "hidden item and recipe records remain addressable for already-owned copies and jobs");
    mem<float>(0x31000 + 0x1C, 0.75f); apply_registry_files({}); removed_live = registry_live_result();
    entry_check(removed_live && removed_live->removed.empty() && at<float>(0x31000 + 0x1C) == 0.75f, "foreign drop writer refuses the whole hide and is never overwritten");
    removal_world(false);
    entry_check(registry_live_result()->removed.size() == 3, "unloaded drop table waits for its resource rather than falsely failing an otherwise proven removal");
    add_drop_table(); registry_live_table(*reg(KILN_CONTENT_DROPS).get(0x444));
    g_write_limits = {0}; std::vector<uintptr_t> missed; store_flush_checked(&why, &missed); registry_live_publish();
    entry_check(registry_live_result()->removed.empty(), "late table disable failure revokes the whole item hide before publishing its refreshed outcome");
    std::printf("registry-entry-binding-test: %d checks, %d failed\n", entry_checks, failures);
    return failures ? 1 : 0;
}
