// SPDX-License-Identifier: MIT
// resource patches in a kiln-backend start (gamedata_bootview_eml.h): the real collector and apply
// (gamedata_bootpatch.cpp), checked before the start through kiln.boot's real committed view (boot_archive_eml.h: the
// binding, the pins and the lossless tree's translation) over a fake kiln.eml.archive table that serves the archive
// generation a ShroudForge startup pass committed. No game, no kiln-eml.
//  - the pass made a list longer: the OnBoot patch of the new index passes and is written;
//  - the pass made it shorter, or changed a variant: an optional boot mod is refused (kiln.boot leaves it out whole and
//    prepares again), a required one is refused before the entry point (kiln.boot stops the start);
//  - a package the pass skipped is no boot owner: it is never named and makes nothing required;
//  - a resource the generation has not got (one Kiln, a boot mod or a data pack registers in this start) is not refused,
//    a required mod's either: the bytes say when it is written;
//  - hosted starts, a missing identity (until kiln.eml.startup hands it), an unavailable kiln.caps row, no table: no
//    view, nothing refused, the bytes decide as before; a view revoked under the check refuses nothing;
//  - the view is bound once and released once, at apply, also with finished() racing a binding (TSan).
// PRIVATE (tools/sfi/public_cut.py). Built with ASan/UBSan and with TSan by bootview_eml_test.sh.
#include "../gamedata_bootview_eml.h"
#include "bootview_fakes.hpp"

using kiln::boot_view::LaunchFacts;

// ---- tagged values (the lossless tree of resource_json) ----
json tv_u64(uint64_t v) {
    char s[24];
    std::snprintf(s, sizeof s, "u64:%016llx", static_cast<unsigned long long>(v));
    return {{"tag", "u64"}, {"type", "uint32"}, {"value", s}};
}
json tv_bool(bool b) { return {{"tag", "bool"}, {"type", "bool"}, {"value", b}}; }
json tv_str(const std::string& s) { return {{"tag", "string"}, {"value", "string:" + kiln::boot_view::hex_bytes(s.data(), s.size())}}; }
json tv_map(const std::vector<std::pair<std::string, json>>& fields, const char* type = "keen::Fixture") {
    json entries = json::array();
    for (const auto& [k, v] : fields) entries.push_back({{"key", tv_str(k)}, {"value", v}});
    return {{"tag", "map"}, {"type", type}, {"entries", entries}};
}
json tv_list(std::initializer_list<uint64_t> xs) {
    json items = json::array();
    for (uint64_t x : xs) items.push_back(tv_u64(x));
    return {{"tag", "array"}, {"type", "keen::BlobArray<uint32>"}, {"items", items}};
}
json tv_variant(const json& map) { return {{"tag", "variant"}, {"value", map}}; }
json fixture_value(std::initializer_list<uint64_t> list, const char* effect = "keen::Burn") {
    return tv_map({{"value", tv_u64(75)},
                   {"flag", tv_bool(true)},
                   {"list", tv_list(list)},
                   {"effect", tv_variant(tv_map({{std::string(effect) == "keen::Burn" ? "damage" : "amount", tv_u64(3)}}, effect))}});
}

// ---- a fake kiln.eml.archive: the generation the startup pass committed ----
struct FakeArchive {
    std::mutex m;
    KilnEmlArchiveIdentityV1 granted{};
    std::map<std::string, std::string> replies;  // query -> resource_json reply
    std::set<std::string> revoked;               // queries answered REVOKED (the generation retired under the view)
    std::set<uint64_t> live, released;
    uint64_t next_view = 0x5000;
    std::atomic<int> acquires{0}, releases{0}, reads{0}, late{0};
};
FakeArchive* F = nullptr;
KilnResult KILN_CALL fa_acquire(KilnOwner owner, const KilnEmlArchiveIdentityV1* expected, KilnEmlArchiveView* out) {
    std::lock_guard lk(F->m);
    ++F->acquires;
    if (!owner || !expected || !out) return KILN_ERR_INVALID_ARGUMENT;
    if (std::memcmp(expected->launch_id, F->granted.launch_id, 16) || expected->generation != F->granted.generation) return KILN_ERR_REVOKED;
    *out = F->next_view++;
    F->live.insert(*out);
    return KILN_OK;
}
KilnResult KILN_CALL fa_identity(KilnEmlArchiveView view, KilnEmlArchiveIdentityV1* out) {
    std::lock_guard lk(F->m);
    if (!F->live.count(view)) return KILN_ERR_REVOKED;
    const uint32_t size = out->size;
    *out = F->granted;
    out->size = size;
    return KILN_OK;
}
KilnResult KILN_CALL fa_release(KilnEmlArchiveView view) {
    std::lock_guard lk(F->m);
    ++F->releases;
    if (F->live.erase(view)) F->released.insert(view);
    return KILN_OK;
}
KilnResult KILN_CALL fa_resource_json(KilnEmlArchiveView view, KilnStr query, char* out, size_t capacity, size_t* required, KilnArchiveErrorV1*) {
    ++F->reads;
    std::lock_guard lk(F->m);
    if (F->released.count(view)) ++F->late;
    if (!F->live.count(view)) return KILN_ERR_REVOKED;
    const std::string q(query.ptr, query.len);
    if (F->revoked.count(q)) return KILN_ERR_REVOKED;
    const auto r = F->replies.find(q);
    if (r == F->replies.end()) return KILN_ERR_NOT_FOUND;
    *required = r->second.size();
    if (capacity < r->second.size()) return KILN_ERR_BUFFER_TOO_SMALL;
    std::memcpy(out, r->second.data(), r->second.size());
    return KILN_OK;
}
KilnEmlArchiveApiV1 t_table{};
const KilnEmlArchiveApiV1* table_of() { return &t_table; }

constexpr uint64_t kLaunch = 0x0123456789abcdefull;
LaunchFacts facts() {
    LaunchFacts l;
    l.launch_id = kLaunch;
    for (int i = 0; i < 32; ++i) l.plan_sha256[static_cast<size_t>(i)] = static_cast<uint8_t>(0xa0 + i);
    return l;
}
KilnEmlArchiveIdentityV1 identity_of(const LaunchFacts& l) {
    KilnEmlArchiveIdentityV1 id{};
    id.size = sizeof id;
    id.phase = KILN_EML_ARCHIVE_PUBLISHED;
    for (int i = 0; i < 8; ++i) id.launch_id[8 + i] = static_cast<uint8_t>(l.launch_id >> (56 - 8 * i));
    for (int i = 0; i < 16; ++i) id.plan_id[i] = static_cast<uint8_t>(0x40 + i);
    id.generation = 7;
    id.reload_epoch = 1;
    std::memcpy(id.plan_sha256, l.plan_sha256.data(), 32);
    for (int i = 0; i < 32; ++i) id.archive_sha256[i] = static_cast<uint8_t>(0x10 + i), id.registry_sha256[i] = static_cast<uint8_t>(0x70 + i);
    id.registry_id = 3;
    return id;
}
void commit_value(const std::string& query, const json& value) {
    F->replies[query] = json{{"schema_version", 1},
                             {"pin", kiln::boot_view::pin_of(F->granted)},
                             {"resourceId", "guid:0101010101010101" "0101010101010101"},
                             {"typeHash", 0x2a},
                             {"part", 0},
                             {"value", value}}
                            .dump();
}

// ---- what the start hands kiln.content ----
bool t_kiln = true, t_identity = true, t_table_there = true;
std::set<std::string> t_rows_off;
bool in_kiln_mode(LaunchFacts* out) { return t_kiln ? (*out = facts(), true) : false; }
const KilnEmlArchiveApiV1* in_table() { return t_table_there ? table_of() : nullptr; }
KilnResult in_identity(const LaunchFacts& l, KilnEmlArchiveIdentityV1* out, std::string* why) {
    if (!t_identity) return *why = "kiln-eml does not hand this start's committed archive generation to kiln.content yet", KILN_ERR_UNAVAILABLE;
    const uint32_t size = out->size;
    *out = identity_of(l);
    out->size = size;
    return KILN_OK;
}
bool in_row(const char* name, std::string* text) { return t_rows_off.count(name) ? (*text = "off for this test", false) : true; }
KilnOwner in_owner() { return 9; }
const bootview_eml::Inputs t_inputs{&in_kiln_mode, &in_table, &in_identity, &in_row, &in_owner};
bootview_eml::Backing* t_backing = nullptr;
// the kiln-backend source gives no final_for answer (kiln-eml cannot say yet which resources nothing else in the start
// creates or overrides); a test that checks what a final view refuses answers it here
bool t_final = false;
const bootview::Source t_source{[](std::string* why) { return t_backing->view(why); }, [] { t_backing->finished(); },
                                [](const KilnAssetKey&) { return t_final; }};

// A fresh start: a new backing (one per process in production) over a fresh fake generation.
std::unique_ptr<FakeArchive> t_archive;
std::unique_ptr<bootview_eml::Backing> t_backing_owned;
void start() {
    t_archive = std::make_unique<FakeArchive>();
    F = t_archive.get();
    F->granted = identity_of(facts());
    t_table = KilnEmlArchiveApiV1{};
    t_table.header = {sizeof(KilnEmlArchiveApiV1), KILN_EML_ARCHIVE_VERSION};
    t_table.acquire = &fa_acquire;
    t_table.identity = &fa_identity;
    t_table.resource_json = &fa_resource_json;
    t_table.release = &fa_release;
    t_kiln = t_identity = t_table_there = true;
    t_final = false;
    t_rows_off.clear();
    t_backing_owned = std::make_unique<bootview_eml::Backing>(&t_inputs);
    t_backing = t_backing_owned.get();
    bootview::set_source(&t_source);
    std::lock_guard lk(g_log_mutex);
    g_log.clear();
}
// documents of boot mods a (optional) and r (required): the envelope's owners. The ShroudForge package the pass skipped,
// "sf-skipped", is no boot mod: it is in no envelope.
json boot_docs(std::initializer_list<std::tuple<const char*, const char*, json>> docs) { return docs_of(docs); }
bool named(const std::string& id) {
    std::lock_guard lk(g_log_mutex);
    for (const auto& l : g_log)
        if (has(l, id)) return true;
    return has(issue.reason, id) || has(issue.mod_id, id) || has(issue.key, id);
}

// The pass made the list longer (a package appended two entries): the new index passes and is written.
void longer() {
    start();
    commit_value(kFixture, fixture_value({1, 2, 3, 4, 5}));
    serve(75, {1, 2, 3, 4, 5});
    run(boot_docs({{"a", "k", set1("list[4]", 9)}, {"r", "k2", set1("value", {{"add", 5}})}}));
    check(faults.empty() && put_count == 1 && patched.list[4] == 9 && patched.value == 80, "the new index passes, the add starts from 75");
    check(last_source == "committed" && last_digest.size() == 64, "the plan names the committed generation's digest");
    {
        // kiln.boot's OnBoot reads bind their own view of the same generation (boot_archive_eml.h, kiln.boot's owner): its
        // digest is the plan's, so the patches were checked against exactly the data their authors read
        kiln::boot_view::CommittedView boot_view;
        std::string why;
        check(boot_view.bind(5, facts(), table_of(), &in_identity, &why) && boot_view.digest() == last_digest, "kiln.boot's view of the generation has the plan's digest");
        boot_view.release();
    }
    check(F->acquires == 2 && F->releases == 2 && F->late == 0, "one view for the patches, released at apply (and kiln.boot's own), no read after them");
}

// The pass made the list shorter, or chose another variant: an optional boot mod is refused before the start and the
// collectors prepare without it; a required one is refused before the entry point. A skipped package is never named.
void shorter() {
    start();
    commit_value(kFixture, fixture_value({1, 2}, "keen::Heal"));
    check(accepted(boot_docs({{"r", "k", set1("list[3]", 9)}})), "without a final answer (the kiln-backend source gives none yet) nothing is refused");
    start();
    t_final = true;
    commit_value(kFixture, fixture_value({1, 2}, "keen::Heal"));
    serve(75, {1, 2});
    json e = boot_docs({{"a", "k", set1("list[3]", 9)}, {"b", "k2", set1("list[1]", 4)}});
    check(refused(e, "a", "IndexPastEnd") && has(issue.reason, "the list has 2 entries"), "an index past the shorter list refuses the optional mod");
    e["excluded"] = {"a"};
    run(e);
    check(faults.empty() && put_count == 1 && patched.list[1] == 4, "the collectors prepare again without it, the other mod is written");
    check(F->acquires == 1 && F->releases == 1, "the view was bound once across the prepares and released at apply");
    start();  // apply ended this start's checks: the next cases are another start
    t_final = true;
    commit_value(kFixture, fixture_value({1, 2}, "keen::Heal"));
    check(refused(boot_docs({{"r", "k", set1("list[2]", 1)}}), "r", "IndexPastEnd"), "a required mod is refused before the entry point");
    check(refused(boot_docs({{"a", "k", set1("effect.damage", 1)}}), "a", "Heal"), "another variant: its member is not there");
    check(!named("sf-skipped"), "the skipped package is named nowhere");
    // only the envelope's required owners are strict: the optional mod stays optional, whatever the pass skipped
    check(!issue.mod_id[0] || std::string(issue.mod_id) == "a", "the refusal is the boot mod's own");
    check(accepted(boot_docs({{"a", "k", set1("list[1]", 4)}})), "a patch the generation allows passes while a package was skipped");
    check(F->acquires == 1 && F->releases == 0, "the view is bound once across prepares, and held until apply");
}

// A resource the committed generation has not got: Kiln, a boot mod or a data pack may register it with a new GUID in this
// start (the archive holds none of those), so it is not refused before the start, a required mod's either; kiln.assets
// serves it when the patch is written. The generation's own resources are still checked.
void created() {
    start();
    commit_value(kFixture, fixture_value({1, 2}));
    serve(75, {1, 2});
    const json made = {{"resource", "made-by-a-mod"}, {"type", "fixture"}, {"set", {{"value", 5}}}};
    run(boot_docs({{"r", "k", made}}));
    check(faults.empty() && put_count == 1 && patched.value == 5, "a resource this start creates is not refused, a required mod's either, and is written");
    check(last_source == "committed" && F->reads > 0 && logged("this start may create or override"), "it was looked for in the committed generation");
    start();
    t_final = true;  // a source that says its view is final for this start
    commit_value(kFixture, fixture_value({1, 2}));
    check(refused(boot_docs({{"r", "k", made}}), "r", "MissingResource"), "a final view: the missing resource is refused");
    check(refused(boot_docs({{"a", "k2", set1("list[3]", 1)}}), "a", "IndexPastEnd"), "and an index past a list the generation has");
}

// No committed generation for kiln.content: nothing is refused, the bytes decide as in any other start; the boot
// snapshot is not read here at all.
void no_generation() {
    for (int which = 0; which < 4; ++which) {
        start();
        commit_value(kFixture, fixture_value({1, 2}));
        const char* why = nullptr;
        if (which == 0) t_kiln = false, why = "not a kiln-backend start";
        if (which == 1) t_identity = false, why = "does not hand this start's committed archive generation";
        if (which == 2) t_rows_off.insert("eml.archive.read.resource_json"), why = "eml.archive.read.resource_json";
        if (which == 3) t_table_there = false, why = "offers no kiln.eml.archive table";
        serve(75, {1, 2, 3, 4, 5});
        run(boot_docs({{"a", "k", set1("list[4]", 9)}}));
        check(faults.empty() && put_count == 1 && patched.list[4] == 9 && last_digest.empty(), "no view: the patch is written as before");
        check(logged(why), "the log says why there is no view");
        check(F->acquires == 0 && F->reads == 0, "and nothing was read from kiln-eml");
    }
}

// The generation retired under the view while the patches were checked: nothing is refused on it.
void revoked() {
    start();
    commit_value(kFixture, fixture_value({1, 2}));
    F->revoked.insert(kFixture);
    check(accepted(boot_docs({{"a", "k", set1("list[4]", 9)}})), "a revoked view refuses nothing");
    check(logged("changed while the patches were checked"), "the log says the view changed under the check");
}

// finished() racing the first binding: the view is released once and nothing reads after it.
void race() {
    for (int round = 0; round < 50; ++round) {
        start();
        commit_value(kFixture, fixture_value({1, 2}));
        std::atomic<bool> go{false};
        std::thread binder([&] {
            while (!go.load()) std::this_thread::yield();
            std::string why;
            if (auto* v = t_backing->view(&why)) bootview::read(*v, kFixture, 1 << 20);
        });
        go = true;
        t_backing->finished();
        binder.join();
        std::string why;
        check(t_backing->view(&why) == nullptr, "after finished() no view is served");
        check(F->releases <= 1 && F->acquires == F->releases && F->live.empty() && F->late == 0,
              "a bound view is released once, and no read reaches kiln-eml after its release");
    }
}
}  // namespace

int main() {
    types();
    boot.header.size = sizeof(boot);
    boot.active_plan = [](KilnStr, uint64_t*) -> const void* { return active; };
    boot.activation_fault = [](KilnOwner, KilnStr mod, KilnStr, KilnStr key, KilnStr reason) -> KilnResult {
        faults.push_back({str(mod), str(key), str(reason)});
        return KILN_OK;
    };
    asset.header.size = sizeof(asset);
    asset.when_resources_ready = [](void(KILN_CALL*)(void*), void*) -> KilnResult { return KILN_OK; };
    asset.resource_data = [](const KilnAssetKey*, const void** data, size_t* len) -> KilnResult {
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
    int failed = 0;
    const std::pair<const char*, void (*)()> groups[] = {
        {"longer", longer}, {"shorter", shorter}, {"created", created}, {"no_generation", no_generation}, {"revoked", revoked}, {"race", race}};
    for (const auto& [name, fn] : groups) {
        try {
            fn();
        } catch (const std::exception& e) {
            ++failed;
            std::printf("bootview-eml-test: FAIL %s: %s (after %d checks)\n", name, e.what(), checks);
        }
    }
    bootview::set_source(nullptr);
    if (failed) {
        std::printf("bootview-eml-test: %d group(s) failed, %d checks passed\n", failed, checks);
        return 1;
    }
    std::printf("bootview-eml-test: %d checks passed\n", checks);
}
