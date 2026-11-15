// SPDX-License-Identifier: MIT
// Real collector and apply, with fake boot/assets tables and a one-field resource. No game or Wine.
#include "../gamedata_bootpatch.cpp"
#include <cstdarg>
#include <cstdio>
#include <stdexcept>
namespace kc {
const KilnApi* g_api = nullptr;
KilnOwner g_owner = 7;
void logf(int, const char*, ...) {}
std::vector<std::filesystem::path> mod_folder_list() { return {}; }
bool guid_parse(const std::string&, uint8_t out[16]) { std::memset(out, 1, 16); return true; }
std::string guid_text(const uint8_t*) { return "fixture"; }
uint32_t fnv1a(const void*, size_t) { return 1; }
namespace gd {
TType scalar, resource;
const TType* TType::resolved() const { return this; }
const TType* TType::inner() const { return nullptr; }
const TField* TType::field(const std::string&) const { return nullptr; }
const TType* TField::type() const { return &scalar; }
const TType* type_named(const std::string& name) { return name == "fixture" ? &resource : nullptr; }
bool leaf_of(const TType*, Field* f) { f->bytes = 4; f->vt = KILN_CV_INT; return true; }
bool resolve(Root, Node root, const std::string&, std::vector<Resolved>* out, std::string*, bool*) {
    Resolved r{}; r.node = {&scalar, root.addr}; r.path = "value"; out->push_back(r); return true;
}
bool parse_leaf(const Field&, const gjson& v, const uint8_t*, uint8_t* out, bool* reset, std::string*) {
    uint32_t n = v.get<uint32_t>(); std::memcpy(out, &n, 4); *reset = false; return true;
}
}
}
namespace {
using namespace kc;
int checks = 0, put_count = 0;
uint32_t vanilla = 10, patched = 0;
const void* active = nullptr;
std::string row_text;
KilnBootApiV1 boot{};
KilnAssetsApi asset{};
KilnCapsApi caps{};
KilnApi api{};
void check(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); ++checks; }
std::string str(KilnStr s) { return std::string(s.ptr, s.len); }
const void* KILN_CALL service(KilnStr name, uint32_t) {
    if (str(name) == KILN_BOOT_SERVICE) return &boot;
    if (str(name) == KILN_ASSETS_SERVICE) return &asset;
    if (str(name) == KILN_CAPS_SERVICE) return &caps;
    return nullptr;
}
KilnBootPreparedV1 prepare(nlohmann::json ir, KilnBootIssueV1* issue) {
    const std::string text = ir.dump();
    KilnBootPrepareInputV1 in{sizeof(in), 1, 1, {text.data(), text.size()}};
    KilnBootPreparedV1 out{}; out.size = sizeof(out);
    check(prepare_boot(nullptr, &in, &out, issue) == KILN_OK, "collector result");
    return out;
}
nlohmann::json envelope() {
    return {{"owners", {{{"id", "a"}}, {{"id", "b"}}, {{"id", "empty"}}}},
        {"collectors", {{"patches", {
            {{"owner", "a"}, {"key", "one"}, {"document", {{"resource", "fixture"}, {"type", "fixture"}, {"set", {{"value", 42}}}}}},
            {{"owner", "b"}, {"key", "two"}, {"document", {{"resource", "fixture"}, {"type", "fixture"}, {"set", {{"value", 42}}}}}}
        }}}}};
}
}
int main() {
    try {
        gd::resource.kind = gd::K_STRUCT; gd::resource.size = 4;
        boot.header.size = sizeof(boot);
        boot.active_plan = [](KilnStr, uint64_t*) -> const void* { return active; };
        asset.header.size = sizeof(asset);
        asset.when_resources_ready = [](void(KILN_CALL*)(void*), void*) -> KilnResult { return KILN_OK; };
        asset.resource_data = [](const KilnAssetKey*, const void** data, size_t* len) -> KilnResult { *data = &vanilla; *len = 4; return KILN_OK; };
        asset.resource_put = [](KilnOwner, const KilnAssetKey*, const void* data, size_t len, uint32_t) -> KilnResult {
            if (len == 4) std::memcpy(&patched, data, 4);
            ++put_count; return KILN_OK;
        };
        caps.report = [](KilnOwner, KilnStr, uint32_t, uint32_t, KilnStr text) -> KilnResult { row_text = str(text); return KILN_OK; };
        api.service = &service; g_api = &api;
        KilnBootIssueV1 issue{}; issue.size = sizeof(issue);
        auto ir = envelope();
        auto plan = prepare(ir, &issue);
        check(plan.ok, "normal prepare succeeds"); active = plan.plan;
        apply(nullptr);
        check(put_count == 1 && patched == 42 && vanilla == 10, "published readiness writes the override, preserves vanilla");
        plan.destroy(plan.plan); active = nullptr;
        // Readiness without a published plan (also the immediate callback at a late bind).
        apply(nullptr);
        for (const char* mod : {"a", "b"}) {
            issue = {}; issue.size = sizeof(issue);
            plan = prepare(ir, &issue);
            if (plan.ok) plan.destroy(plan.plan);
            check(!plan.ok, "early readiness refuses a writer");
            check(std::string(issue.mod_id) == mod && std::string(issue.kind) == "resourcePatches", "refusal names writer");
            check(std::string(issue.reason).find("resource service was already up before the boot generation was published") != std::string::npos,
                  "refusal explains missed deadline");
            ir["excluded"].push_back(mod); // kiln.boot retries collectors without each refused optional owner
        }
        plan = prepare(ir, &issue);
        check(plan.ok && static_cast<const BootPlan*>(plan.plan)->sets == 0, "no remaining writes is accepted");
        plan.destroy(plan.plan);
        check(row_text.find("resource service was already up") != std::string::npos, "capability row explains missed deadline");
        check(put_count == 1, "missed readiness never retries writes");
    } catch (const std::exception& e) { std::printf("bootpatch-test: FAIL %s (after %d checks)\n", e.what(), checks); return 1; }
    std::printf("bootpatch-test: %d checks passed\n", checks);
}
