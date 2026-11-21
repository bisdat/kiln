// SPDX-License-Identifier: MIT
// A boot generation's resource patches checked against a base view of the game data the start reads (gamedata_bootview.h):
// the real collector and apply (gamedata_bootpatch.cpp) with fake kiln.boot / kiln.assets tables, a base view that
// serves made-up decoded values, a few made-up types and a resource of hand-made bytes. No game or Wine.
//
// The data: the boot snapshot was made before the start (value 50, list [1, 2, 3]); in this start an archive writer
// changed the resource before kiln.boot published (value 75 and the list made longer, or shorter). The base view holds
// what the writer committed; kiln.assets serves the same bytes once the game's resources are up.
#include "bootview_fakes.hpp"

// Without a newer view (no source, the default) nothing changes: what the snapshot would say is never asked, and the
// bytes decide when the patch is written.
void no_view() {
    bootview::set_source(nullptr);
    serve(75, {1, 2, 3, 4, 5});
    run(docs_of({{"a", "k", set1("list[4]", 9)}}));
    check(faults.empty() && put_count == 1 && patched.list[4] == 9, "no source: the patch is written as before");
    check(last_digest.empty() && last_source.empty(), "no source: the plan names no base");
    serve(75, {1, 2});
    run(docs_of({{"a", "k", set1("list[4]", 9)}}));
    check(faults.size() == 1 && faults[0].mod == "a" && has(faults[0].reason, "the list has 2 entries") && put_count == 0,
          "no source: an index past the list is an activation fault when it is written, as before");
    check(!has(faults[0].reason, "base"), "no source: the fault names no base");
    bootview::set_source(&kSource);
    g_view = nullptr;
    serve(75, {1, 2});
    run(docs_of({{"a", "k", set1("list[4]", 9)}}));
    check(faults.size() == 1 && logged("the test start has no newer view"), "a source without a view: as without one, and the log says why");
}

// An archive writer of this start made the list longer: the new index is valid, and an old snapshot (three elements)
// would have refused it.
void longer_list() {
    FakeView v;
    v.values[kFixture] = json({{"value", 75}, {"flag", true}, {"list", {1, 2, 3, 4, 5}}});
    g_view = &v;
    serve(75, {1, 2, 3, 4, 5});
    run(docs_of({{"a", "k", set1("list[4]", 9)}}));
    check(faults.empty() && put_count == 1 && patched.list[4] == 9, "the new index is accepted and written");
    check(last_digest == v.digest_ && last_source == "committed", "the plan names the base it was checked against");
    check(v.trees > 0, "the base view was asked");
    FakeView stale;
    stale.values[kFixture] = json({{"value", 50}, {"flag", true}, {"list", {1, 2, 3}}});
    stale.name = "snapshot";
    g_view = &stale;
    check(accepted(docs_of({{"a", "k", set1("list[4]", 9)}})) && stale.trees == 0, "the boot snapshot is never asked");
    check(logged("the boot snapshot is made before the start"), "the log says why the snapshot is not used");
    g_view = nullptr;
}

// The writer made the list shorter (or the resource is gone): an optional mod is refused before the start and the
// collectors prepare without it; a required one is refused as well, and kiln.boot stops the start before the entry point.
void shorter_list() {
    FakeView v;
    v.values[kFixture] = json({{"value", 75}, {"flag", true}, {"list", {1, 2}}});
    g_view = &v;
    serve(75, {1, 2});
    json e = docs_of({{"a", "k1", set1("list[4]", 9)}, {"a", "k2", templ({{"set", {{"Alpha.x", 1}}}})}, {"b", "k3", set1("value", 5)}});
    check(refused(e, "a", "IndexPastEnd") && has(issue.reason, "list[4]") && has(issue.key, "k1"), "an index past the shorter list refuses its mod");
    e["excluded"] = {"a"};  // kiln.boot drops the optional mod whole and prepares again
    auto p = prepare(e);
    check(p.ok, "without the refused mod the plan is accepted");
    const auto* plan = static_cast<const BootPlan*>(p.plan);
    bool any_a = false;
    for (const auto& rp : plan->resources)
        for (const auto& d : rp.docs) any_a = any_a || d.mod == "a";
    check(!any_a && plan->resources.size() == 1, "nothing of the dropped mod is in the plan (its other document either)");
    active = p.plan;
    faults.clear();
    put_count = 0;
    apply(nullptr);
    active = nullptr;
    p.destroy(p.plan);
    check(faults.empty() && put_count == 1 && patched.value == 5, "the other mod's patch is written");
    check(refused(docs_of({{"r", "k", set1("list[2]", 9)}}), "r", "the list has 2 entries"), "a required mod is refused before the start");
    g_view = nullptr;
}

// A resource the data has not got: Kiln, a boot mod or a data pack may register it with a new GUID in this start (served
// once the game's resources are up), so the bytes say; only when the source says its view of it is final for this start
// is the patch refused before the start.
void missing_resource() {
    FakeView v;
    v.values[kFixture] = json({{"value", 75}, {"flag", true}, {"list", {1, 2}}});
    v.values[kTemplate] = {{"name", "T"}, {"components", {{{"$type", "Alpha"}, {"x", 1}}}}};
    g_view = &v;
    serve(75, {1, 2});
    const json made = {{"resource", "made-by-a-mod"}, {"type", "fixture"}, {"set", {{"value", 5}}}};
    const json copy = templ({{"copyComponents", {{"Stock", "new-template"}}}});
    g_final = false;
    final_asked = 0;
    run(docs_of({{"r", "k", made}}));
    check(faults.empty() && put_count == 1 && patched.value == 5, "a resource this start creates is not refused (a required mod's either) and is written");
    check(logged("this start may create or override") && final_asked > 0, "the source was asked, and the log says the bytes decide");
    missing_assets.insert(gid("made-by-a-mod"));
    run(docs_of({{"a", "k", made}}));
    check(faults.size() == 1 && has(faults[0].reason, "MissingResource") && put_count == 0, "not served when it is written: an activation fault, as before");
    missing_assets.clear();
    check(accepted(docs_of({{"a", "k", copy}})), "a copyComponents source this start may create: the bytes say");
    g_final = true;
    check(refused(docs_of({{"b", "k", made}}), "b", "MissingResource") && has(issue.reason, "nothing in this start creates or overrides"),
          "final for this start: refused before the start");
    check(refused(docs_of({{"r", "k", made}}), "r", "MissingResource"), "a required mod's as well (kiln.boot stops the start)");
    check(refused(docs_of({{"a", "k", copy}}), "a", "copyComponents") && has(issue.reason, "MissingResource"), "and a copyComponents source");
    final_asked = 0;
    check(accepted(docs_of({{"a", "k", set1("list[1]", 3)}})) && final_asked == 0, "nothing to refuse: the source is not asked");
    g_view = nullptr;
}

// Another mod overrides a resource in this start: mod A's assets.json puts a template with a component the game's has not
// got, and kiln.assets serves it before the patches are written; mod B patches that component. The view (the game's data)
// has no such component, but it is not final for this start, so B is not refused and its patch is written on A's template.
// Only when the source says its view of the template is final is B refused before the start. No data-based refusal holds
// without that answer.
void overridden() {
    FakeView v;
    v.values[kFixture] = json({{"value", 75}, {"flag", true}, {"list", {1, 2}}, {"effect", {{"$type", "Heal"}, {"amount", 2}}}});
    v.values[kTemplate] = {{"name", "T"}, {"components", {{{"$type", "Alpha"}, {"x", 1}}}}};
    g_view = &v;
    serve(75, {1, 2});  // what kiln.assets serves when the patches are written: mod A's template, Beta in it
    const json b = templ({{"set", {{"Beta.y", 4}}}});
    g_final = false;
    final_asked = 0;
    {
        std::lock_guard lk(g_log_mutex);
        g_log.clear();
    }
    run(docs_of({{"r", "b", b}}));
    check(faults.empty() && put_count == 1 && patched.value == 4, "not final: B's patch of the component A's override adds is written (a required mod's too)");
    check(final_asked > 0 && logged("no component Beta") && logged("may create or override"), "the source was asked, and the log says the bytes decide");
    check(accepted(docs_of({{"a", "k", set1("list[5]", 1)}, {"b", "k2", set1("effect.damage", 1)}, {"b", "k3", set1("flag", {{"mul", 2}})}})),
          "not final: no data-based refusal (an index past a list, another variant's member, an operation on a bool)");
    check(accepted(docs_of({{"a", "k", templ({{"copyComponents", {{"Beta", "tmpl2"}}}})}})), "nor of a copyComponents source");
    static const bootview::Source no_answer{[](std::string* why) -> kiln::boot_view::BaseView* {
                                                if (!g_view) *why = g_view_why;
                                                return g_view;
                                            },
                                            nullptr};
    bootview::set_source(&no_answer);
    check(accepted(docs_of({{"r", "b", b}})), "a source without the answer: as not final");
    bootview::set_source(&kSource);
    g_final = true;
    check(refused(docs_of({{"r", "b", b}}), "r", "no component Beta") && has(issue.reason, "nothing in this start creates or overrides"),
          "final for this start: B is refused before the start");
    g_view = nullptr;
}

// Forms of the view that the bytes read otherwise: a variant of one scalar (the bytes follow it to the scalar), a config
// shown by its alias, a config named by its id below a Configurations variant. None of them refuses.
void view_forms() {
    FakeView v;
    v.values[kFixture] = json({{"value", 1},
                               {"flag", true},
                               {"list", {1}},
                               {"key", {{"$type", "HashKey32"}, {"value", 7}}},
                               {"when", {{"$type", "Time"}, {"nanoseconds", 5}}},
                               {"pair", {{"$type", "Pair"}, {"a", 1}, {"b", 2}}},
                               {"impactValues", {{"ManaCost", {{"$config", "ScaledConfig"}, {"value", {{"value", 3}, {"scaleFactor", 1}}}}}}},
                               {"cfgs", {{"$type", "Configurations"}, {"simple", json::array()}}}});
    g_view = &v;
    check(accepted(docs_of({{"a", "k", set1("key", 9)}})), "a variant of one scalar: the bytes take it as the scalar");
    check(accepted(docs_of({{"a", "k", set1("when", {{"add", 1}})}})), "a keen::Time in a variant, with an operation");
    check(refused(docs_of({{"a", "k", set1("pair", 9)}}), "a", "NotScalar"), "a variant of two members is a struct");
    check(accepted(docs_of({{"a", "k", set1("impactValues.ManaCost", {{"mul", 2}})}})), "a config shown by its alias: its value is set");
    check(accepted(docs_of({{"a", "k", set1("impactValues.ManaCost.scaleFactor", 2)}})), "and a member of it");
    check(accepted(docs_of({{"a", "k", set1("cfgs[3]", 2)}})), "a config by its id below a Configurations variant");
    check(refused(docs_of({{"a", "k", set1("pair[3]", 2)}}), "a", "is not a list"), "another variant has no index");
    g_view = nullptr;
}

// [*] over an empty list changes nothing and is not an error (resource-patches.md); a concrete index is.
void wildcard() {
    FakeView v;
    v.values[kFixture] = json({{"value", 75}, {"flag", true}, {"list", json::array()}});
    g_view = &v;
    check(accepted(docs_of({{"a", "k", set1("list[*]", 1)}})), "[*] over an empty list is accepted");
    serve(75, {});
    {
        std::lock_guard lk(g_log_mutex);
        g_log.clear();
    }
    run(docs_of({{"a", "k", set1("list[*]", 1)}, {"b", "k2", set1("value", 5)}}));
    check(faults.empty() && put_count == 1 && patched.value == 5 && patched.n == 0 && logged("the list is empty"),
          "and when it is written it changes nothing and is no fault");
    check(refused(docs_of({{"a", "k", set1("list[0]", 1)}}), "a", "IndexPastEnd"), "[0] of an empty list is refused");
    v.values[kFixture]["list"] = {4, 5};
    serve(75, {4, 5});
    run(docs_of({{"a", "k", set1("list[*]", 3)}}));
    check(faults.empty() && patched.list[0] == 3 && patched.list[1] == 3, "[*] over the list sets each element");
    g_view = nullptr;
}

// Arithmetic starts from the value this start's data holds (75), not the snapshot's (50); an operation needs a number.
void arithmetic() {
    FakeView v;
    v.values[kFixture] = json({{"value", 75}, {"flag", true}, {"list", {1}}});
    g_view = &v;
    serve(75, {1});
    run(docs_of({{"a", "k", set1("value", {{"add", 5}})}}));
    check(faults.empty() && patched.value == 80, "add 5 to the generation's 75 is 80");
    check(refused(docs_of({{"a", "k", set1("flag", {{"mul", 2}})}}), "a", "OperationOnNonNumber"), "an operation on a bool is refused");
    check(accepted(docs_of({{"a", "k", set1("flag", false)}})), "a plain value on a bool is fine");
    g_view = nullptr;
}

// Two mods' claims, the file's precedence and what a set may write stay as they are with a view.
void current_policy() {
    FakeView v;
    v.values[kFixture] = json({{"value", 75}, {"flag", true}, {"list", {1, 2, 3}}, {"inner", {{"x", 1}}}});
    g_view = &v;
    check(accepted(docs_of({{"a", "k1", set1("value", 42)}, {"b", "k2", set1("value", 42)}})), "the same value from two mods is combined");
    check(refused(docs_of({{"a", "k1", set1("value", 42)}, {"b", "k2", set1("value", 43)}}), "b", "Conflict"), "two values: the later mod is refused");
    check(refused(docs_of({{"a", "k1", set1("inner", 1)}}), "a", "NotScalar"), "a struct is not set (the view does not widen what a set may write)");
    check(refused(docs_of({{"a", "k1", set1("list", 1)}}), "a", "NotScalar"), "a list is not set");
    // the file's document wins over the same key staged by code: the file's is what is checked
    namespace fs = std::filesystem;
    const char* base = std::getenv("KILN_OUT");
    const fs::path root = fs::path(base ? base : fs::temp_directory_path().string()) / "content-tests" / ("bootview-mods-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::remove_all(root);
    fs::create_directories(root / "a" / "blocks");
    auto put = [](const fs::path& p, const std::string& s) { std::ofstream(p, std::ios::binary) << s; };
    put(root / "a" / "kiln.mod.json", R"({"id": "a", "boot": true})");
    put(root / "a" / "blocks" / "patches.json", R"({"resourcePatches": {"one": {"resource": "fixture", "type": "fixture", "set": {"list[1]": 7}}}})");
    g_mod_dirs = {root / "a"};
    check(accepted(docs_of({{"a", "one", set1("list[9]", 7)}})), "the file's patch is checked, the staged one with the same key is not");
    put(root / "a" / "blocks" / "patches.json", R"({"resourcePatches": {"one": {"resource": "fixture", "type": "fixture", "set": {"list[9]": 7}}}})");
    check(refused(docs_of({{"a", "one", set1("list[1]", 7)}}), "a", "list[9]"), "the file's patch is what the view refuses");
    g_mod_dirs.clear();
    fs::remove_all(root);
    // with the game's types, what they refuse stays refused whatever the view shows
    gd::g_types = true;
    v.values[kTemplate] = {{"name", "T"}, {"components", {{{"$type", "Alpha"}, {"x", 1}, {"steps", 5}}}}};
    check(refused(docs_of({{"a", "k1", templ({{"set", {{"Alpha.steps", 1}}}})}}), "a", "NotScalar"), "the types still refuse a list");
    check(accepted(docs_of({{"a", "k1", templ({{"set", {{"Alpha.x", 1}}}})}})), "and accept the member the view has");
    gd::g_types = false;
    g_view = nullptr;
}

// After the publication the bytes still decide: a resource another provider changed after the check is an activation
// fault, which names the base the patch was checked against and claims no undoing of the game's data.
void after_publication() {
    FakeView v;
    v.values[kFixture] = json({{"value", 75}, {"flag", true}, {"list", {1, 2, 3, 4, 5}}});
    g_view = &v;
    serve(75, {1, 2, 3});
    run(docs_of({{"a", "k", set1("list[4]", 9)}, {"b", "k2", set1("value", 5)}}));
    check(faults.size() == 1 && faults[0].mod == "a" && has(faults[0].reason, "the list has 3 entries"), "an activation fault for the patch");
    check(has(faults[0].reason, "nothing of this patch was applied") && has(faults[0].reason, std::string(16, 'a')), "it names the base");
    check(!has(faults[0].reason, "roll") && !has(faults[0].reason, "undo"), "and claims no rollback");
    check(put_count == 1 && patched.value == 5, "the other patch is written");
    g_view = nullptr;
}

// What only the data can say beyond lists: a variant holds the member, an optional is there, a template has the component,
// copyComponents' source has it.
void variants_optionals_components() {
    FakeView v;
    v.values[kFixture] = json({{"value", 1}, {"flag", true}, {"list", {1}}, {"effect", {{"$type", "Burn"}, {"damage", 3}}}, {"opt", nullptr},
                          {"some", {{"x", 1}}}});
    v.values[kTemplate] = {{"name", "T"}, {"components", {{{"$type", "Alpha"}, {"x", 1}, {"steps", {1, 2}}}, {{"$type", "Gamma"}}}}};
    v.values[kChest] = {{"name", "Chest"}, {"components", {{{"$type", "Stock"}, {"items", json::array()}}}}};
    g_view = &v;
    check(accepted(docs_of({{"a", "k", set1("effect.damage", 4)}})), "the variant holds the member");
    v.values[kFixture]["effect"] = {{"$type", "Heal"}, {"amount", 2}};
    check(refused(docs_of({{"a", "k", set1("effect.damage", 4)}}), "a", "Heal"), "another variant: the member is not there");
    check(refused(docs_of({{"a", "k", set1("opt.x", 4)}}), "a", "is empty"), "an optional without a value");
    check(accepted(docs_of({{"a", "k", set1("some.x", 4)}})), "a struct's member");
    check(accepted(docs_of({{"a", "k", set1("nope.x", 4)}})), "a member the value does not show outside a variant: the types and bytes say");
    check(accepted(docs_of({{"a", "k", templ({{"set", {{"Alpha.x", 1}, {"alpha.steps[1]", 3}}}})}})), "the template's component and its list");
    check(refused(docs_of({{"a", "k", templ({{"set", {{"Beta.y", 1}}}})}}), "a", "no component Beta"), "a component the template has not got");
    check(refused(docs_of({{"a", "k", templ({{"set", {{"Alpha#1.x", 1}}}})}}), "a", "Alpha#1"), "the second Alpha of a template with one");
    check(accepted(docs_of({{"a", "k", templ({{"set", {{"Beta.y", 1}}}})}, {"b", "k2", templ({{"addComponents", {"Beta"}}})}})),
          "a component another patch adds: the bytes say");
    check(refused(docs_of({{"a", "k", templ({{"set", {{"Alpha.steps[2]", 1}}}})}}), "a", "IndexPastEnd"), "an index past a component's list");
    check(accepted(docs_of({{"a", "k", templ({{"copyComponents", {{"Stock", "magic-chest"}}}})}})), "the source has the component");
    check(refused(docs_of({{"a", "k", templ({{"copyComponents", {{"Beta", "magic-chest"}}}})}}), "a", "no Beta"), "the source has not got it");
    check(refused(docs_of({{"a", "k", templ({{"copyComponents", {{"Stock", "gone-template"}}}})}}), "a", "MissingResource"), "the source is not there");
    g_view = nullptr;
}

// The structural edits of the generation change what a later path finds: such a place is the bytes' to say.
void structural() {
    FakeView v;
    v.values[kFixture] = json({{"value", 1}, {"flag", true}, {"list", {1, 2}}, {"effect", {{"$type", "Burn"}, {"damage", 3}}}, {"opt", nullptr}});
    g_view = &v;
    serve(1, {1, 2});
    run(docs_of({{"a", "k", plain({{"append", {{"list", {{"values", {3}}}}}}, {"set", {{"list[2]", 9}}}})}}));
    check(faults.empty() && patched.n == 3 && patched.list[2] == 9, "a mod's set of the element its own append adds");
    check(accepted(docs_of({{"a", "k", plain({{"replace", {{"list", {{"values", {1, 2, 3, 4}}}}}}, {"set", {{"list[3]", 9}}}})}})),
          "a set inside a list the generation replaces");
    check(refused(docs_of({{"a", "k", plain({{"append", {{"value", {{"values", {3}}}}}}})}}), "a", "NotAList"), "an append to a number");
    check(refused(docs_of({{"a", "k", plain({{"clear", {"effect"}}})}}), "a", "NotAList"), "a clear of a variant's struct");
    check(refused(docs_of({{"a", "k", plain({{"clear", {"opt.items"}}})}}), "a", "is empty"), "a list inside an empty optional");
    check(accepted(docs_of({{"a", "k", plain({{"clear", {"other"}}})}})), "a list the value does not show: the bytes say");
    g_view = nullptr;
}

// What the view cannot say is left to the bytes; a check never runs on another generation than the one it started on.
void unknowns() {
    FakeView v;
    v.values[kFixture] = json({{"value", 1}, {"flag", true}, {"list", "..."}});
    g_view = &v;
    check(accepted(docs_of({{"a", "k", set1("list[40]", 1)}})), "a value cut at the view's depth");
    v.unavailable.insert(kFixture);
    check(accepted(docs_of({{"a", "k", set1("list[40]", 1)}})), "a resource the view cannot decode");
    v.unavailable.clear();
    v.values[kFixture] = {{"list", json::array()}};
    v.ready_ = false;
    v.trees = 0;
    check(accepted(docs_of({{"a", "k", set1("list[4]", 1)}})) && v.trees == 0 && logged("the test view is not ready"), "a view that is not ready");
    v.ready_ = true;
    v.digests = 0;
    v.digest_after = std::string(64, 'b');
    check(accepted(docs_of({{"a", "k", set1("list[4]", 1)}})), "the digest changed under the check: nothing is refused");
    check(logged("changed while the patches were checked"), "and the log says so");
    g_view = nullptr;
}

// kiln.boot's own backings as sources: the boot snapshot is never used for the check, the live game data is not up before
// the entry point; either way the bytes decide, as without a view.
void real_views() {
    static kiln::boot_gd::Reader reader;  // no snapshot opened: it would not be asked anyway
    static kiln::boot_view::SnapshotView snapshot(reader);
    static kiln::boot_view::LiveView live([]() -> const KilnAssetsApi* { return nullptr; });
    static kiln::boot_view::BaseView* which = nullptr;
    static const bootview::Source source{[](std::string*) { return which; }, nullptr};
    bootview::set_source(&source);
    for (kiln::boot_view::BaseView* v : {static_cast<kiln::boot_view::BaseView*>(&snapshot), static_cast<kiln::boot_view::BaseView*>(&live)}) {
        which = v;
        {
            std::lock_guard lk(g_log_mutex);
            g_log.clear();
        }
        serve(75, {1, 2, 3, 4, 5});
        run(docs_of({{"a", "k", set1("list[4]", 9)}}));
        check(faults.empty() && put_count == 1 && patched.list[4] == 9 && last_digest.empty(), "kiln.boot's backing: no check, the patch is written");
        check(logged(v == &snapshot ? "the boot snapshot is made before the start" : "kiln.assets is not running"), "the log says why");
    }
    bootview::set_source(&kSource);
}

// The base view's reads end once: when the game's resources are up, not before.
void release_once() {
    FakeView v;
    v.values[kFixture] = json({{"value", 1}, {"flag", true}, {"list", {1}}});
    g_view = &v;
    finished_calls = 0;
    auto p = prepare(docs_of({{"a", "k", set1("value", 3)}}));
    check(p.ok && v.released == 0 && finished_calls == 0, "prepare does not end the reads (kiln.boot may prepare again)");
    serve(1, {1});
    active = p.plan;
    std::thread t([] { apply(nullptr); });  // the resource service's thread
    t.join();
    active = nullptr;
    p.destroy(p.plan);
    check(finished_calls == 1 && v.released == 1, "apply ends them once");
    g_view = nullptr;
}
// The resources up before the generation was published (the latch the bootpatch tests cover): the view's reads end as
// well, since no prepare after it may write anything.
void early_readiness() {
    FakeView v;
    v.values[kFixture] = json({{"value", 1}, {"flag", true}, {"list", {1}}});
    g_view = &v;
    finished_calls = 0;
    active = nullptr;
    apply(nullptr);
    check(finished_calls == 1 && v.released == 1, "readiness without a published plan ends the view's reads");
    check(refused(docs_of({{"a", "k", set1("value", 3)}}), "a", "already up"), "and a later prepare refuses the writer, as before");
    g_view = nullptr;
}
}  // namespace

int main() {
    try {
        types();
        boot.header.size = sizeof(boot);
        boot.active_plan = [](KilnStr, uint64_t*) -> const void* { return active; };
        boot.activation_fault = [](KilnOwner, KilnStr mod, KilnStr, KilnStr key, KilnStr reason) -> KilnResult {
            faults.push_back({str(mod), str(key), str(reason)});
            return KILN_OK;
        };
        asset.header.size = sizeof(asset);
        asset.when_resources_ready = [](void(KILN_CALL*)(void*), void*) -> KilnResult { return KILN_OK; };
        asset.resource_data = [](const KilnAssetKey* key, const void** data, size_t* len) -> KilnResult {
            if (missing_assets.count(guid_text(key->guid))) return KILN_ERR_NOT_FOUND;
            *data = &served;
            *len = sizeof served;
            return KILN_OK;
        };
        asset.resource_put = [](KilnOwner, const KilnAssetKey*, const void* data, size_t len, uint32_t) -> KilnResult {
            if (len >= sizeof patched) std::memcpy(&patched, data, sizeof patched);
            ++put_count;
            return KILN_OK;
        };
        caps.report = [](KilnOwner, KilnStr, uint32_t, uint32_t, KilnStr) -> KilnResult { return KILN_OK; };
        api.service = &service;
        g_api = &api;
    } catch (const std::exception& e) {
        std::printf("bootview-test: FAIL %s\n", e.what());
        return 1;
    }
    // each group on its own, so one run shows every group that fails
    int failed = 0;
    const std::pair<const char*, void (*)()> groups[] = {{"no_view", no_view},
                                                         {"longer_list", longer_list},
                                                         {"shorter_list", shorter_list},
                                                         {"missing_resource", missing_resource},
                                                         {"overridden", overridden},
                                                         {"view_forms", view_forms},
                                                         {"wildcard", wildcard},
                                                         {"arithmetic", arithmetic},
                                                         {"current_policy", current_policy},
                                                         {"after_publication", after_publication},
                                                         {"variants_optionals_components", variants_optionals_components},
                                                         {"structural", structural},
                                                         {"unknowns", unknowns},
                                                         {"release_once", release_once},
                                                         {"real_views", real_views},
                                                         {"early_readiness", early_readiness}};  // last: it sets a process latch
    for (const auto& [name, fn] : groups) {
        bootview::set_source(&kSource);
        g_view = nullptr;
        g_final = true;  // the groups below test what a final view refuses; missing_resource and overridden what it does not
        gd::g_types = false;
        g_mod_dirs.clear();
        try {
            fn();
        } catch (const std::exception& e) {
            ++failed;
            std::printf("bootview-test: FAIL %s: %s (after %d checks)\n", name, e.what(), checks);
        }
    }
    if (failed) {
        std::printf("bootview-test: %d group(s) failed, %d checks passed\n", failed, checks);
        return 1;
    }
    std::printf("bootview-test: %d checks passed\n", checks);
}
