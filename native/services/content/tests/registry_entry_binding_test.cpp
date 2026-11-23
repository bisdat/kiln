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
    entry_check(!registry_live_initialize_entry(KILN_CONTENT_ITEMS, clone, "a:logs", {{"block", 0.5}}, &why), "unproven field does not bypass mapping guards");
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
    registry_entries_prepare(&in);
    entry_check(queued.empty() && in.result.added.empty(), "definition failure refuses dependent recipes and forgets their unpublished definitions");
    entry_check(std::any_of(in.result.refused.begin(), in.result.refused.end(), [](const auto& r) { return r.registry == "recipes" && r.reason.find("injected item definition refusal") != std::string::npos; }), "definition failure root reaches dependent recipe refusal");
    registry::Batch removal; refusals.clear();
    entry_check(registry::parse_batch(registry::json{{"ir", 1}, {"mod", "a"}, {"stage", "alter"}, {"edits", {{{"registry", "items"}, {"entry", "remove"}, {"select", "game:material_t1_wood_logs"}}}}}, *in.schema, &removal, &refusals, &why), "removal gate fixture parses");
    auto proof = *g_regmap;
    auto unproven = removal; proof.build.rows["drops.entries[].chance"] = registry::map::BuildRow::Unproven;
    registry::refuse_unmapped_edits(proof, *in.schema, &unproven, &refusals, true);
    entry_check(unproven.edits.empty() && refusals.size() == 1 && refusals.front().reason.find("drop tables not proven on this build") != std::string::npos, "unproven drop rows refuse the whole item removal with the named reason");
    proof.build.rows["drops.entries[].chance"] = registry::map::BuildRow::Ok; refusals.clear();
    registry::refuse_unmapped_edits(proof, *in.schema, &removal, &refusals, true);
    entry_check(removal.edits.size() == 1 && refusals.empty(), "proven drop rows admit removal to the group-safety preflight");
    std::printf("registry-entry-binding-test: %d checks, %d failed\n", entry_checks, failures);
    return failures ? 1 : 0;
}
