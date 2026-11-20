// SPDX-License-Identifier: MIT
// The fakes of the base-view tests of resource patches (bootview_test.cpp): the real collector and apply
// (gamedata_bootpatch.cpp) with fake kiln.boot / kiln.assets tables, a base view that serves made-up decoded values, a few
// made-up types and a resource of hand-made bytes. Included once per test program; it leaves an anonymous namespace
// open for the test's own cases.
#pragma once
#include "../gamedata_bootpatch.cpp"
#include <cstdarg>
#include <cstdio>
#include <stdexcept>
#include <thread>
#include <chrono>
namespace kc {
const KilnApi* g_api = nullptr;
KilnOwner g_owner = 7;
std::vector<std::string> g_log;
std::mutex g_log_mutex;
void logf(int, const char* fmt, ...) {
    char line[2048];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    std::lock_guard lk(g_log_mutex);
    g_log.emplace_back(line);
}
std::vector<std::filesystem::path> g_mod_dirs;
std::vector<std::filesystem::path> mod_folder_list() { return g_mod_dirs; }
// "fixture" is the patched resource (GUID byte 1); "tmpl" a template; any other text a GUID of its own (by its length), "bad" none
bool guid_parse(const std::string& s, uint8_t out[16]) {
    if (s == "bad") return false;
    std::memset(out, s == "fixture" ? 1 : static_cast<int>(2 + s.size() % 200), 16);
    return true;
}
std::string guid_text(const uint8_t* g) { return g[0] == 1 ? "fixture" : "g" + std::to_string(g[0]); }
uint32_t fnv1a(const void*, size_t) { return 0x2a; }
namespace gd {
// The types. "fixture" {value u32, flag bool, list BlobArray<u32>}; a template {name, components}; components Alpha
// {x, steps BlobArray<u32>}, Beta {y}, Gamma below keen::ecs::Component. g_types: the exe's reflection is readable.
bool g_types = false;
TType scalar, boolean, list_u32, resource, tmpl, var_list, variant, comp_base, alpha, beta, gammaT;
const TType* TType::resolved() const { return this; }
const TType* TType::inner() const { return reinterpret_cast<const TType*>(inner_rec); }
const TType* TField::type() const { return reinterpret_cast<const TType*>(type_rec); }
const TField* TType::field(const std::string& n) const {
    for (const TField& f : fields) if (f.name == n) return &f;
    for (const TField& f : fields) if (folded(f.name) == folded(n)) return &f;
    return nullptr;
}
const TType* type_named(const std::string& name) {
    if (name == "fixture") return &resource;
    if (name == "keen::ecs::TemplateResource") return &tmpl;
    if (!g_types) return nullptr;
    if (name == "keen::ItemInfo") return &resource;
    for (const TType* t : {&comp_base, &alpha, &beta, &gammaT}) if (t->name == name) return t;
    return nullptr;
}
bool leaf_of(const TType* t, Field* f) {
    if (t == &scalar) return f->bytes = 4, f->vt = KILN_CV_INT, true;
    if (t == &boolean) return f->bytes = 4, f->vt = KILN_CV_BOOL, true;
    return false;
}
// The resource's bytes: {value u32 +0, flag u32 +4, n u32 +8, list u32[16] +12}. The walker of these bytes: "value",
// "flag", "list[i]" (an element the list has), "list[*]" (every element; none of an empty list, no error, as gd::resolve), and on
// a template "<Component>.x" / ".y" (the u32 at +0). Anything else has no field.
struct Res {
    uint32_t value, flag, n, list[16];
};
bool resolve(Root kind, Node root, const std::string& path, std::vector<Resolved>* out, std::string* err, bool*) {
    const auto* r = reinterpret_cast<const Res*>(root.addr);
    auto at = [&](uintptr_t a, const TType* t) { Resolved x{}; x.node = {t, a}; x.path = path; out->push_back(x); };
    if (path == "value") return at(root.addr, &scalar), true;
    if (path == "flag") return at(root.addr + 4, &boolean), true;
    if (path == "list[*]") {
        for (uint32_t i = 0; i < r->n; ++i) at(root.addr + 12 + 4 * i, &scalar);
        return true;
    }
    if (path.rfind("list[", 0) == 0 && path.back() == ']') {
        const unsigned long i = std::stoul(path.substr(5));
        if (i >= r->n) return *err = "list[" + std::to_string(i) + "]: the list has " + std::to_string(r->n) + " entries", false;
        return at(root.addr + 12 + 4 * i, &scalar), true;
    }
    if (kind == Root::Template && (path == "Alpha.x" || path == "Beta.y" || path == "alpha.x")) return at(root.addr, &scalar), true;
    return *err = path + ": has no field", false;
}
bool parse_leaf(const Field& f, const gjson& v, const uint8_t* original, uint8_t* out, bool* reset, std::string* err) {
    *reset = false;
    uint32_t n = 0;
    if (f.vt == KILN_CV_BOOL) {
        if (!v.is_boolean()) return *err = "a bool", false;
        n = v.get<bool>();
    } else if (v.is_object()) {
        std::memcpy(&n, original, 4);
        double x = n;
        if (v.contains("mul")) x *= v["mul"].get<double>();
        if (v.contains("add")) x += v["add"].get<double>();
        if (v.contains("min")) x = std::max(x, v["min"].get<double>());
        if (v.contains("max")) x = std::min(x, v["max"].get<double>());
        n = static_cast<uint32_t>(x);
    } else if (v.is_number()) {
        n = v.get<uint32_t>();
    } else {
        return *err = "not a number", false;
    }
    std::memcpy(out, &n, 4);
    return true;
}
Res& res_of(std::vector<uint8_t>* b) { return *reinterpret_cast<Res*>(b->data()); }
bool boot_remove_components(const TType*, const std::vector<std::string>&, std::vector<uint8_t>*, size_t* n, std::string*) { return *n = 0, true; }
bool boot_add_components(const TType*, const std::vector<std::string>& names, std::vector<uint8_t>*, size_t* n, std::string*) {
    return *n = names.size(), true;
}
bool boot_copy_components(const TType*, const std::vector<CopySource>& copies, std::vector<uint8_t>*, size_t* n, std::string* err, size_t) {
    for (const auto& c : copies)
        if (!c.data) return *err = "\"copyComponents\": MissingResource: the template " + c.from + " is not in this game build", false;
    return *n = copies.size(), true;
}
bool boot_clear(Root, const TType*, const std::string& path, std::vector<uint8_t>* b, size_t* n, std::string* err) {
    if (path != "list") return *err = "\"clear\": " + path + " is not a list", false;
    *n = res_of(b).n;
    res_of(b).n = 0;
    return true;
}
bool boot_replace(Root, const TType*, const std::string& path, const std::vector<gjson>& v, std::vector<uint8_t>* b, size_t* n, std::string* err) {
    if (path != "list") return *err = "\"replace\": " + path + " is not a list", false;
    res_of(b).n = static_cast<uint32_t>(v.size());
    for (size_t i = 0; i < v.size(); ++i) res_of(b).list[i] = v[i].get<uint32_t>();
    return *n = v.size(), true;
}
bool boot_append(Root, const TType*, const std::string& path, const std::vector<gjson>& v, bool, std::vector<uint8_t>* b, size_t* n, std::string* err,
                 size_t* first) {
    if (path != "list") return *err = "\"append\": " + path + " is not a list", false;
    Res& r = res_of(b);
    if (first) *first = r.n;
    for (const auto& x : v) r.list[r.n++] = x.get<uint32_t>();
    return *n = v.size(), true;
}
}  // namespace gd
}  // namespace kc

namespace {
using namespace kc;
using json = nlohmann::json;
int checks = 0, put_count = 0;
gd::Res served{}, patched{};
const void* active = nullptr;
struct Fault { std::string mod, key, reason; };
std::vector<Fault> faults;
KilnBootApiV1 boot{};
KilnAssetsApi asset{};
KilnCapsApi caps{};
KilnApi api{};
KilnBootIssueV1 issue{};
std::set<std::string> missing_assets;  // GUID texts kiln.assets has not got
void check(bool ok, const char* why) {
    if (!ok) throw std::runtime_error(std::string(why) + " [" + issue.mod_id + "/" + issue.key + ": " + issue.reason + "]");
    ++checks;
}
std::string str(KilnStr s) { return std::string(s.ptr, s.len); }
bool has(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }
const void* KILN_CALL service(KilnStr name, uint32_t) {
    if (str(name) == KILN_BOOT_SERVICE) return &boot;
    if (str(name) == KILN_ASSETS_SERVICE) return &asset;
    if (str(name) == KILN_CAPS_SERVICE) return &caps;
    return nullptr;
}

// ---- the base view: decoded values by query, as kiln.boot's resource: query answers them ----
struct FakeView final : kiln::boot_view::BaseView {
    std::string name = "committed";
    std::map<std::string, json> values;  // query -> value
    std::set<std::string> unavailable;   // queries it cannot answer
    bool ready_ = true;
    std::string digest_ = std::string(64, 'a');
    std::string digest_after;  // set: the digest it gives from the second call on (another generation under the check)
    std::atomic<int> trees{0}, digests{0}, released{0};
    const char* source() const override { return name.c_str(); }
    bool ready(std::string* why) override {
        if (!ready_) *why = "the test view is not ready";
        return ready_;
    }
    std::string digest() override { return !ready_ ? std::string() : digests++ && !digest_after.empty() ? digest_after : digest_; }
    KilnResult tree(std::string_view query, size_t capacity, size_t result_max, uint64_t, std::string* out, size_t* required, size_t* value_bytes,
                    std::string* why) override {
        ++trees;
        const std::string q(query);
        if (unavailable.count(q)) return *why = "the test view cannot decode it", KILN_ERR_UNAVAILABLE;
        const auto it = values.find(q);
        if (it == values.end()) return *why = "not in the test generation", KILN_ERR_NOT_FOUND;
        const std::string text = it->second.dump();
        const std::string whole = R"({"resourceId":"x","typeHash":"0000002a","part":0,"value":)" + text + "}";
        if (text.size() > result_max) return *why = "the record is larger than one result may be", KILN_ERR_UNAVAILABLE;
        if (capacity < whole.size() + 1) return *required = whole.size() + 1, KILN_ERR_BUFFER_TOO_SMALL;
        *out = whole;
        *value_bytes = text.size();
        return KILN_OK;
    }
    KilnResult bytes(const KilnAssetKey&, size_t, std::string*, std::string* why) override {
        *why = "decoded values only";
        return KILN_ERR_UNAVAILABLE;
    }
    void release() override { ++released; }
};
FakeView* g_view = nullptr;  // what the source answers (null: none, with g_view_why)
std::string g_view_why = "the test start has no newer view";
std::atomic<int> finished_calls{0};
const bootview::Source kSource{
    [](std::string* why) -> kiln::boot_view::BaseView* {
        if (!g_view) *why = g_view_why;
        return g_view;
    },
    [] {
        ++finished_calls;
        if (g_view) g_view->release();
    }};
const std::string kFixture = "resource:fixture/0000002a/0", kTemplate = "resource:g6/0000002a/0", kChest = "resource:g13/0000002a/0";

// documents of mods a, b and r (r's boot content is required), each {owner, key, document}
json docs_of(std::initializer_list<std::tuple<const char*, const char*, json>> docs) {
    json list = json::array();
    for (const auto& [owner, key, doc] : docs) list.push_back({{"owner", owner}, {"key", key}, {"document", doc}});
    return {{"owners", {{{"id", "a"}}, {{"id", "b"}}, {{"id", "r"}, {"required", true}}}}, {"collectors", {{"patches", list}}}};
}
json plain(json body) { return body["resource"] = "fixture", body["type"] = "fixture", body; }
[[maybe_unused]] json templ(json body) { return body["resource"] = "tmpl", body["type"] = "keen::ecs::TemplateResource", body; }
json set1(const char* path, json v) { return plain({{"set", {{path, v}}}}); }
KilnBootPreparedV1 prepare(const json& ir) {
    const std::string text = ir.dump();
    KilnBootPrepareInputV1 in{sizeof(in), 1, 1, {text.data(), text.size()}};
    KilnBootPreparedV1 out{};
    out.size = sizeof(out);
    issue = {};
    issue.size = sizeof(issue);
    if (prepare_boot(nullptr, &in, &out, &issue) != KILN_OK) throw std::runtime_error("the collector broke its contract");
    return out;
}
bool accepted(const json& e) {
    auto p = prepare(e);
    if (p.ok) p.destroy(p.plan);
    return p.ok;
}
bool refused(const json& e, const char* mod, const std::string& part) {
    return !accepted(e) && std::string(issue.mod_id) == mod && has(issue.reason, part) && std::string(issue.phase) == "prepare" &&
           issue.code == KILN_BOOT_ISSUE_PREPARE_REFUSED;
}
// prepare and apply: the resource the game would read is in patched
std::string last_digest, last_source;
void run(const json& e) {
    faults.clear();
    put_count = 0;
    patched = {};
    auto p = prepare(e);
    if (!p.ok) throw std::runtime_error(std::string("the plan is refused: ") + issue.mod_id + "/" + issue.key + ": " + issue.reason);
    ++checks;
    last_digest = static_cast<const BootPlan*>(p.plan)->base_digest;
    last_source = static_cast<const BootPlan*>(p.plan)->base_source;
    active = p.plan;
    apply(nullptr);
    active = nullptr;
    p.destroy(p.plan);
}
bool logged(const std::string& part) {
    std::lock_guard lk(g_log_mutex);
    for (const auto& l : g_log)
        if (has(l, part)) return true;
    return false;
}
void serve(uint32_t value, std::initializer_list<uint32_t> list) {
    served = {};
    served.value = value;
    served.flag = 1;
    for (uint32_t x : list) served.list[served.n++] = x;
}

void types() {
    using namespace gd;
    auto rec = [](const TType& t) { return reinterpret_cast<uintptr_t>(&t); };
    scalar.name = "uint32", scalar.kind = K_U32, scalar.size = 4;
    boolean.name = "bool", boolean.kind = K_BOOL, boolean.size = 4;
    list_u32.name = "keen::BlobArray<uint32>", list_u32.kind = K_BLOBARRAY, list_u32.size = 8, list_u32.inner_rec = rec(scalar);
    resource.name = "fixture", resource.brief = "fixture", resource.kind = K_STRUCT, resource.size = 8;
    resource.fields = {{"value", rec(scalar), 0}, {"flag", rec(boolean), 4}, {"list", rec(list_u32), 8}};
    variant.name = "keen::BlobVariant", variant.kind = K_BLOBVAR, variant.size = 12;
    var_list.name = "keen::BlobArray<keen::BlobVariant>", var_list.kind = K_BLOBARRAY, var_list.size = 8, var_list.inner_rec = rec(variant);
    tmpl.name = "keen::ecs::TemplateResource", tmpl.brief = "TemplateResource", tmpl.kind = K_STRUCT, tmpl.size = 8;
    tmpl.fields = {{"name", 0, 0}, {"components", rec(var_list), 0}};
    comp_base.name = "keen::ecs::Component", comp_base.brief = "Component", comp_base.kind = K_STRUCT, comp_base.size = 1;
    auto comp = [&](TType& t, const char* brief) { t.name = std::string("keen::ecs::") + brief, t.brief = brief, t.kind = K_STRUCT, t.size = 8, t.inner_rec = rec(comp_base); };
    comp(alpha, "Alpha");
    alpha.fields = {{"x", rec(scalar), 0}, {"steps", rec(list_u32), 4}};
    comp(beta, "Beta");
    beta.fields = {{"y", rec(scalar), 0}};
    comp(gammaT, "Gamma");
}
