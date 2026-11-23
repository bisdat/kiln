// SPDX-License-Identifier: MIT
// Exercise the real native publisher through the existing host fakes, not a second publication model.
#define main legacy_native_pack_main
#include "native_pack_test.cpp"
#undef main

#include <algorithm>
#include <map>
#if defined(__SANITIZE_ADDRESS__)
#include <sanitizer/asan_interface.h>
#endif

namespace {
struct Layer { KilnOwner owner; KilnAssetKey key; kiln::qb::Bytes bytes; };
std::vector<Layer> layers;
std::map<KilnOwner, std::string> owners;
std::map<KilnOwner, unsigned> puts;
std::vector<KilnOwner> reverted;
std::vector<std::pair<KilnOwner, std::string>> definitions;
std::map<uint32_t, std::string> texts;
std::map<KilnOwner, std::map<uint32_t, std::string>> before_texts;
KilnOwner next_owner = 100;
std::string refused_mod, abort_failure;
bool existing_template = false, missing_override = false;
unsigned put_calls = 0;
bool fail_legacy = false, broken_rollback = false;
KilnOwner broken_owner = 0;
const uint8_t base_npcs[] = {5, 0};

bool key_equal(const KilnAssetKey& a, const KilnAssetKey& b) {
    return a.type == b.type && a.part == b.part && std::memcmp(a.guid, b.guid, 16) == 0;
}
KilnOwner KILN_CALL create_owner(KilnStr label) {
    const auto owner = ++next_owner;
    owners[owner] = std::string(label.ptr, label.len);
    return owner;
}
KilnResult KILN_CALL layered_data(const KilnAssetKey* key, const void** data, size_t* size) {
    if (!t_service_up) return KILN_ERR_NOT_FOUND;
    if (existing_template && key->type == kiln::qb::kTypeTemplate) {
        static const uint8_t occupied[] = {7, 8};
        *data = occupied; *size = sizeof occupied; return KILN_OK;
    }
    if (missing_override && key->type == kiln::qb::kTypeNpcs) return KILN_ERR_NOT_FOUND;
    for (auto it = layers.rbegin(); it != layers.rend(); ++it)
        if (key_equal(it->key, *key)) { *data = it->bytes.data(); *size = it->bytes.size(); return KILN_OK; }
    if (key->type == kiln::qb::kTypeKnowledge) { *data = t_res; *size = sizeof t_res; return KILN_OK; }
    if (key->type == kiln::qb::kTypeNpcs) { *data = base_npcs; *size = sizeof base_npcs; return KILN_OK; }
    return KILN_ERR_NOT_FOUND;
}
KilnResult KILN_CALL layered_put(KilnOwner owner, const KilnAssetKey* key, const void* data, size_t size, uint32_t) {
    ++put_calls;
    const bool target = owners[owner] == "bootpack:" + refused_mod || (fail_legacy && owner == g_owner);
    if (target && ++puts[owner] == 2) return KILN_ERR_UNAVAILABLE;
    for (auto& layer : layers)
        if (layer.owner == owner && key_equal(layer.key, *key)) {
            layer.bytes.assign(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + size);
            return KILN_OK;
        }
    layers.push_back({owner, *key, kiln::qb::Bytes(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + size)});
    return KILN_OK;
}
void KILN_CALL layered_revert(KilnOwner owner) {
    reverted.push_back(owner);
    if (!broken_rollback && owner != broken_owner) std::erase_if(layers, [&](const Layer& layer) { return layer.owner == owner; });
}
KilnResult reply(const std::string& text, char* out, size_t capacity, size_t* size, KilnResult result = KILN_OK) {
    if (size) *size = text.size();
    if (out && capacity) {
        const size_t count = std::min(capacity - 1, text.size());
        std::memcpy(out, text.data(), count);
        out[count] = 0;
    }
    return result;
}
KilnResult KILN_CALL define(KilnOwner owner, uint32_t, KilnStr document, char* out, size_t capacity, size_t* size) {
    const auto item = kiln::qb::ojson::parse(std::string(document.ptr, document.len));
    if (item.value("name", "") == "refuse") return reply("injected content refusal", out, capacity, size, KILN_ERR_INVALID_ARGUMENT);
    definitions.push_back({owner, item.value("name", "")});
    if (item.contains("displayName")) texts[42] = item["displayName"].get<std::string>();
    return reply("{\"ok\":true}", out, capacity, size);
}
uint32_t KILN_CALL forget(KilnOwner owner) {
    const size_t count = definitions.size();
    std::erase_if(definitions, [&](const auto& definition) { return definition.first == owner; });
    return static_cast<uint32_t>(count - definitions.size());
}
KilnContentApi content = [] {
    KilnContentApi api{};
    api.header = {sizeof(KilnContentApi), KILN_CONTENT_VERSION};
    api.define = &define;
    api.undefine_owner = &forget;
    return api;
}();

// The private DLL helper's opcodes are intentionally not a mod-facing service table.
KilnResult KILN_CALL boot_content(uint32_t op, KilnOwner owner, uint32_t kind, const char* json, size_t len,
                                char* out, size_t capacity, size_t* size) {
    if (op == 1) before_texts[owner] = texts;
    else if (op == 2) return define(owner, kind, KilnStr{json, len}, out, capacity, size);
    else if (op == 3) {
        const auto value = kiln::qb::ojson::parse(std::string(json, len));
        texts[value["tag"].get<uint32_t>()] = value["texts"].dump();
    } else if (op == 5) {
        if (owners[owner] == "bootpack:" + abort_failure)
            return reply("injected content rollback invariant: text tag 42 changed", out, capacity, size, KILN_ERR_INTERNAL);
        forget(owner);
        texts = before_texts.at(owner);
        before_texts.erase(owner);
    }
    return reply("ok", out, capacity, size);
}
void* find_proc(const char* name) {
    return std::string(name) == "kc_nc_boot_pack_v1" ? reinterpret_cast<void*>(&boot_content) : nullptr;
}
const void* KILN_CALL services(KilnStr name, uint32_t version) {
    if (std::string(name.ptr, name.len) == KILN_CONTENT_SERVICE) return &content;
    return f_service(name, version);
}
void reset(const char* name) {
    start(name);
    layers.clear(); puts.clear(); definitions.clear(); before_texts.clear(); reverted.clear();
    texts = {{42, "original text"}};
    refused_mod.clear(); fail_legacy = false; broken_rollback = false; broken_owner = 0;
    abort_failure.clear(); existing_template = false; missing_override = false; put_calls = 0;
    kiln::qb::t_bootpack_test = true;
    t_assets_api.resource_data = &layered_data;
    t_assets_api.resource_put = &layered_put;
    t_assets_api.revert_owner = &layered_revert;
    t_api.service = &services;
    t_api.owner_create = &create_owner;
    kiln_test_module = reinterpret_cast<void*>(1);
    kiln_test_proc = &find_proc;
    g_pack_failure = {}; // the fixture starts a new process-equivalent boot state
}
std::vector<uint8_t> knowledge() {
    KilnAssetKey key{};
    kiln::qb::Guid guid;
    kiln::qb::parse_guid(kiln::qb::kGuidKnowledge, &guid);
    std::memcpy(key.guid, guid.b, 16);
    key.type = kiln::qb::kTypeKnowledge;
    const void* data = nullptr;
    size_t size = 0;
    if (layered_data(&key, &data, &size) != KILN_OK) return {};
    return {static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + size};
}
void test_mid_pack() {
    reset("16f: refusal mid-pack leaves no resources or shared knowledge behind");
    mod("mid-bad", kGood);
    refused_mod = "mid-bad"; fail_legacy = true;
    boot_and_resource_init();
    CHECK(layers.empty());
    CHECK(knowledge() == std::vector<uint8_t>(t_res, t_res + sizeof t_res));
    CHECK(g_names.count("mid-bad:k1") == 0);
    CHECK(t_stops.empty());  // a cleanly rolled-back optional mod does not stop its independent neighbours
}
void test_survivors() {
    reset("16f: a later refusal does not erase an earlier mod; the next mod compiles on the survivors");
    mod("a-survives", kGood); mod("b-refused", kGood); mod("c-after", kGood);
    refused_mod = "b-refused";
    boot_and_resource_init();
    CHECK(t_stops.empty());
    CHECK(g_names.count("a-survives:k1") == 1);
    CHECK(g_names.count("b-refused:k1") == 0);
    CHECK(g_names.count("c-after:k1") == 1);
    const auto final = knowledge();
    CHECK(std::find(final.begin(), final.end(), uint8_t('a')) != final.end());
    CHECK(std::find(final.begin(), final.end(), uint8_t('b')) == final.end());
    CHECK(std::find(final.begin(), final.end(), uint8_t('c')) != final.end());
    bool compiled_on_survivors = false;
    for (const auto& [name, base] : kiln::qb::t_compile_bases)
        if (name == "c-after") compiled_on_survivors = std::find(base.begin(), base.end(), uint8_t('a')) != base.end() &&
            std::find(base.begin(), base.end(), uint8_t('b')) == base.end();
    CHECK(compiled_on_survivors);
    CHECK(std::all_of(layers.begin(), layers.end(), [](const Layer& layer) { return layer.owner != g_owner && owners[layer.owner].rfind("bootpack:", 0) == 0; }));
}
void test_text_restore() {
    reset("16f: refusal after a definition restores its text-tag before-image");
    mod("text-bad", R"({"knowledge":{"met":{}},"bootItems":[{"name":"first","displayName":"changed text"},{"name":"refuse"}]})");
    boot_and_resource_init();
    CHECK(texts[42] == "original text");
    CHECK(definitions.empty());
    CHECK(layers.empty());
    CHECK(g_names.count("text-bad:k1") == 0);
}
void test_rollback_fault_is_named() {
    reset("16f: a rollback invariant failure stops startup with the mod and reason in the existing notice");
    mod("rollback-bad", kGood);
    refused_mod = "rollback-bad"; fail_legacy = true; broken_rollback = true;
    boot_and_resource_init();
    CHECK(t_stops.size() == 1);
    if (t_stops.size() == 1) {
        CHECK(t_stops[0].kind == StartStop::Content);
        CHECK(t_stops[0].mods == std::vector<std::string>{"rollback-bad"});
        CHECK(t_stops[0].why.find("rollback") != std::string::npos);
        const auto notice = stop::box(t_stops[0].kind, t_stops[0].why, t_stops[0].mods);
        CHECK(notice.find("rollback-bad") != std::string::npos);
        CHECK(notice.find(t_stops[0].why) != std::string::npos);
    }
    CHECK(logged("rollback"));
}
void test_dependencies_and_cycles() {
    reset("16f: failed required native dependencies are excluded before compilation");
    mod("a-dependent", kGood, R"(,"dependencies":{"b-refused":"*"})");
    mod("b-refused", kGood); mod("c-independent", kGood);
    refused_mod = "b-refused";
    boot_and_resource_init();
    CHECK(t_stops.empty());
    CHECK(g_names.count("a-dependent:k1") == 0 && g_names.count("b-refused:k1") == 0);
    CHECK(g_names.count("c-independent:k1") == 1);
    CHECK(std::none_of(kiln::qb::t_compile_bases.begin(), kiln::qb::t_compile_bases.end(),
        [](const auto& row) { return row.first == "a-dependent"; }));
    CHECK(logged("required dependency b-refused did not commit"));

    reset("16f: a required dependency cycle is refused without affecting an independent mod");
    mod("a-cycle", kGood, R"(,"dependencies":{"b-cycle":"*"})");
    mod("b-cycle", kGood, R"(,"dependencies":{"a-cycle":"*"})");
    mod("c-independent", kGood);
    boot_and_resource_init();
    CHECK(t_stops.empty());
    CHECK(g_names.count("a-cycle:k1") == 0 && g_names.count("b-cycle:k1") == 0);
    CHECK(g_names.count("c-independent:k1") == 1);
    CHECK(logged("required native dependency cycle"));
}
void test_body_pack_exclusion() {
    for (bool helper : {false, true}) for (bool malformed_like : {false, true}) {
        reset(helper ? "16f: no authored body lowerer excludes the entire dependent native pack" :
                       "16f: a missing rollback helper excludes the entire body-dependent native pack");
        if (!helper) kiln_test_module = nullptr;
        mod("a-body", kGood); mod("b-ordinary", kGood);
        write(t_root / "mods/a-body/blocks/body.json", malformed_like
            ? R"({"npcs":{"n":{"name":"Authored","body":{"like":"not-a-body","model":"model"}}}})"
            : R"({"npcs":{"n":{"name":"Authored","body":{"model":"model"},"motions":"poses"}}})");
        boot_and_resource_init();
        CHECK(t_stops.empty());
        CHECK(g_names.count("a-body:k1") == 0);
        CHECK(g_names.count("b-ordinary:k1") == 1);
        CHECK(logged(helper ? "authored roster-body lowering is not installed" : "whole body-dependent native pack is excluded"));
    }
}
void test_local_body_manifest_excludes_the_whole_pack() {
    for (bool helper : {false, true}) {
        reset("16f: a machine-local cooked body manifest keeps the whole native pack body-dependent");
        if (!helper) kiln_test_module = nullptr;
        mod("a-body", kGood); mod("b-ordinary", kGood);
        write(t_root / "mods/a-body/assets/cooked/npc-bodies.v1.json", R"({"format":1,"bodies":{"n":{}}})");
        boot_and_resource_init();
        CHECK(t_stops.empty());
        CHECK(g_names.count("a-body:k1") == 0);
        CHECK(g_names.count("b-ordinary:k1") == 1);
        CHECK(logged(helper ? "authored roster-body lowering is not installed" : "whole body-dependent native pack is excluded"));
    }
}
void test_early_abort() {
    reset("16f: enclosing OnBoot abort withdraws successful packs in reverse order");
    mod("a-first", kGood); mod("b-second", kGood);
    boot_and_resource_init();
    CHECK(g_pack_applied.size() == 2);
    CHECK(!layers.empty());
    std::vector<KilnOwner> expected_reverse;
    for (auto it = g_pack_applied.rbegin(); it != g_pack_applied.rend(); ++it) expected_reverse.push_back(it->owner);
    auto* plan = new BootPlan;
    plan->built_early = true;
    destroy_boot(plan);
    CHECK(t_stops.empty());
    CHECK(layers.empty() && definitions.empty());
    CHECK(reverted == expected_reverse);
    CHECK(knowledge() == std::vector<uint8_t>(t_res, t_res + sizeof t_res));
    CHECK(g_names.empty() && !g_built && g_pack_applied.empty());
    CHECK(std::none_of(reverted.begin(), reverted.end(), [](KilnOwner owner) { return owner == g_owner; }));

    reset("16f: an empty early plan never withdraws the shared knowledge owner");
    boot_and_resource_init();
    plan = new BootPlan;
    plan->built_early = true;
    destroy_boot(plan);
    CHECK(reverted.empty());
    CHECK(t_stops.empty());
}
void test_content_rollback_failure_is_named() {
    reset("16f: content-side Abort failure stops startup with the mod, reason, notice and log");
    mod("content-rollback-bad", kGood);
    refused_mod = "content-rollback-bad";
    abort_failure = refused_mod;
    boot_and_resource_init();
    CHECK(content_stop({"content-rollback-bad"}, "injected content rollback invariant"));
    CHECK(logged("injected content rollback invariant: text tag 42 changed"));
    if (t_stops.size() == 1) {
        const auto notice = stop::box(t_stops[0].kind, t_stops[0].why, t_stops[0].mods);
        CHECK(notice.find("content-rollback-bad") != std::string::npos);
        CHECK(notice.find("text tag 42 changed") != std::string::npos);
    }
}
void test_preflight_refuses_before_publication() {
    for (bool new_conflict : {true, false}) {
        reset(new_conflict ? "16f: a new key already present is refused before publication" :
                             "16f: a missing override key is refused before publication");
        mod("preflight", R"({"knowledge":{"met":{}},"template":"collision"})");
        existing_template = new_conflict;
        missing_override = !new_conflict;
        boot_and_resource_init();
        CHECK(t_stops.empty());
        CHECK(put_calls == 0);
        CHECK(layers.empty() && definitions.empty() && before_texts.empty());
        CHECK(g_pack_applied.empty() && g_names.empty());
        CHECK(logged(new_conflict ? "new key already exists" : "override key is missing"));
    }
}
void check_plan_deleted(const void* plan) {
#if defined(__SANITIZE_ADDRESS__)
    CHECK(__asan_address_is_poisoned(plan));
#else
    (void)plan;
#endif
}
void test_partial_teardown_failure() {
    reset("16f: a returning fatal teardown deletes its plan and retains only pending withdrawals");
    mod("a-first", kGood); mod("b-second", kGood); mod("c-third", kGood);
    boot_and_resource_init();
    CHECK(g_pack_applied.size() == 3);
    if (g_pack_applied.size() != 3) return;
    const auto b = g_pack_applied[1].owner, c = g_pack_applied[2].owner;
    broken_owner = b;
    auto* plan = new BootPlan;
    plan->built_early = true;
    const void* address = plan;
    destroy_boot(plan);
    check_plan_deleted(address);
    CHECK(content_stop({"b-second"}, "rollback invariant"));
    CHECK(reverted == (std::vector<KilnOwner>{c, b}));
    CHECK(g_pack_applied.size() == 2);
    CHECK(std::none_of(g_pack_applied.begin(), g_pack_applied.end(), [&](const auto& pending) { return pending.owner == c; }));
    CHECK(!g_built && g_names.empty() && g_served.empty());
    CHECK(kiln::qb::ojson::parse(g_manifest).contains("failed"));
    const int compiles = kiln::qb::t_compiles;
    const auto withdrawals = reverted;
    t_stops.clear();
    build_once();
    CHECK(kiln::qb::t_compiles == compiles && reverted == withdrawals);
    CHECK(content_stop({"b-second"}, "rollback invariant")); // restart required, not a fresh build over half-withdrawn state
}
void test_disappeared_helper_teardown() {
    reset("16f: a disappeared helper also deletes the aborted plan and latches the fault");
    mod("a-first", kGood);
    boot_and_resource_init();
    auto* plan = new BootPlan;
    plan->built_early = true;
    kiln::qb::ModInput shipped;
    shipped.id = "a-first";
    shipped.documents = {kiln::qb::ojson::parse(kGood)};
    plan->mods.push_back(std::move(shipped));
    const void* address = plan;
    kiln_test_module = nullptr;
    destroy_boot(plan);
    check_plan_deleted(address);
    CHECK(content_stop({"a-first"}, "content helper disappeared"));
    CHECK(!g_built && g_names.empty() && g_served.empty());
    CHECK(g_pack_applied.size() == 1); // none was withdrawn
    CHECK(reverted.empty());
    CHECK(kiln::qb::ojson::parse(g_manifest).contains("failed"));
    const int compiles = kiln::qb::t_compiles;
    t_stops.clear();
    build_once();
    CHECK(kiln::qb::t_compiles == compiles && reverted.empty());
    CHECK(content_stop({"a-first"}, "content helper disappeared"));
}
}  // namespace

#ifndef KILN_BOOTPACK_NATIVE_NO_MAIN  // body_native_test.cpp reuses these fakes with its own main
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    t_root = fs::path(argv[1]) / "bootpack-native-world";
    g_api = &t_api;
    test_mid_pack();
    test_survivors();
    test_text_restore();
    test_rollback_fault_is_named();
    test_dependencies_and_cycles();
    test_body_pack_exclusion();
    test_local_body_manifest_excludes_the_whole_pack();
    test_early_abort();
    test_content_rollback_failure_is_named();
    test_preflight_refuses_before_publication();
    test_partial_teardown_failure();
    test_disappeared_helper_teardown();
    std::error_code ec;
    fs::remove_all(t_root, ec);
    std::printf("bootpack native: %d checks, %d failed\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
#endif
