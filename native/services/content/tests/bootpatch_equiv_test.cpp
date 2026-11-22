// SPDX-License-Identifier: MIT
// The resource patches' prepare (gamedata_bootpatch.cpp) against its version before the claim index (registry P1 S10): the
// same generated boot generations through both, every outcome printed (accepted with its counts, or refused with the
// mod, key and the refusal's text); bootpatch_equiv_test.sh builds this file twice (BOOTPATCH_SRC: the old file, then the
// current one) and diffs the outputs. "bench": one prepare of 4,096 changes laid out to make the old scans quadratic, timed.
#include BOOTPATCH_SRC
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <stdexcept>
namespace kc {
const KilnApi* g_api = nullptr;
KilnOwner g_owner = 7;
void logf(int, const char*, ...) {}
std::vector<std::filesystem::path> g_mod_dirs;  // gear_rule_files() gives one
std::vector<std::filesystem::path> mod_folder_list() { return g_mod_dirs; }
// "fixture" is the patched resource; any other text a GUID of its own (by its length), "bad" none
bool guid_parse(const std::string& s, uint8_t out[16]) {
    if (s == "bad") return false;
    std::memset(out, s == "fixture" ? 1 : static_cast<int>(2 + s.size() % 200), 16);
    return true;
}
std::string guid_text(const uint8_t* g) { return g[0] == 1 ? "fixture" : "g" + std::to_string(g[0]); }
uint32_t fnv1a(const void*, size_t) { return 1; }
namespace gd {
// The types (g_types: the exe's reflection is readable). "fixture" {value: u32 at 0, list: BlobArray<u32>}; a template
// {name, components}; components Alpha {x, steps: BlobArray<u32>}, Beta, CurrentTransform below keen::ecs::Component.
bool g_types = false;
TType scalar, resource, list_u32, tmpl, var_list, variant, comp_base, alpha, beta, transform;
const TType* TType::resolved() const { return this; }
const TType* TType::inner() const { return reinterpret_cast<const TType*>(inner_rec); }
const TField* TType::field(const std::string& n) const {
    for (const TField& f : fields) if (f.name == n) return &f;
    for (const TField& f : fields) if (folded(f.name) == folded(n)) return &f;
    return nullptr;
}
const TType* TField::type() const { return reinterpret_cast<const TType*>(type_rec); }
const TType* type_named(const std::string& name) {
    if (name == "fixture") return &resource;
    if (name == "keen::ecs::TemplateResource") return &tmpl;
    if (!g_types) return nullptr;
    if (name == "keen::ItemInfo") return &resource;
    for (const TType* t : {&comp_base, &alpha, &beta, &transform}) if (t->name == name) return t;
    return nullptr;
}
bool leaf_of(const TType* t, Field* f) {
    if (t != &scalar) return false;
    f->bytes = 4; f->vt = KILN_CV_INT; return true;
}
// "value" and every other path: the u32 at the resource's start; "missing...": no such member; "past...": no such element;
// "empty...[*]": an empty list
bool resolve(Root, Node root, const std::string& path, std::vector<Resolved>* out, std::string* err, bool*) {
    if (path.rfind("missing", 0) == 0) return *err = "no member missing", false;
    if (path.rfind("past", 0) == 0 || path.rfind("empty", 0) == 0) return true;
    Resolved r{}; r.node = {&scalar, root.addr}; r.path = path; out->push_back(r); return true;
}
bool parse_leaf(const Field&, const gjson& v, const uint8_t*, uint8_t* out, bool* reset, std::string* err) {
    if (!v.is_number()) return *err = "not a number", false;
    uint32_t n = v.get<uint32_t>(); std::memcpy(out, &n, 4); *reset = false; return true;
}
// The structural edits have their own test (bootstruct_test.cpp). Here each leaves a mark in the resource's second u32:
// removing Alpha +100, adding Beta +1000, appending to "list" +10000; removing "Stuck", adding "Broken" and appending to "bad" fail, "huge" grows the copy over 64 MiB.
std::vector<std::string> trace;  // the structural edits in the order they were made
void mark(std::vector<uint8_t>* b, uint32_t add) { uint32_t v; std::memcpy(&v, b->data() + 4, 4); v += add; std::memcpy(b->data() + 4, &v, 4); }
bool boot_remove_components(const TType*, const std::vector<std::string>& names, std::vector<uint8_t>* b, size_t* n, std::string* err) {
    *n = 0;
    trace.push_back("remove");
    for (const auto& x : names) {
        if (x == "Stuck") return *err = "\"removeComponents\": Stuck cannot go", false;
        if (x == "Alpha") mark(b, 100), ++*n;
    }
    return true;
}
bool boot_add_components(const TType*, const std::vector<std::string>& names, std::vector<uint8_t>* b, size_t* n, std::string* err) {
    *n = 0;
    trace.push_back("add");
    for (const auto& x : names) {
        if (x == "Broken") return *err = "\"addComponents\": \"Broken\" cannot be added", false;
        if (x == "Beta") mark(b, 1000), ++*n;
    }
    return true;
}
bool boot_copy_components(const TType*, const std::vector<CopySource>& copies, std::vector<uint8_t>* b, size_t* n, std::string* err, size_t) {
    *n = 0;
    for (const auto& c : copies) {
        trace.push_back("copy " + c.brief);
        if (c.brief == "Broken") return *err = "\"copyComponents\": Broken: the template has no Broken", false;
        if (!c.data) return *err = "\"copyComponents\": MissingResource", false;
        mark(b, 100000), ++*n;
    }
    return true;
}
bool boot_clear(Root, const TType*, const std::string& path, std::vector<uint8_t>* b, size_t* n, std::string* err) {
    trace.push_back("clear " + path);
    if (path == "bad") return *err = "\"clear\": bad is not a list", false;
    mark(b, 1000000), *n = 1;
    return true;
}
bool boot_replace(Root, const TType*, const std::string& path, const std::vector<gjson>& v, std::vector<uint8_t>* b, size_t* n, std::string* err) {
    trace.push_back("replace " + path);
    if (path == "bad") return *err = "\"replace\": bad is not a list", false;
    mark(b, 10000000), *n = v.size();
    return true;
}
bool boot_append(Root, const TType*, const std::string& path, const std::vector<gjson>&, bool, std::vector<uint8_t>* b, size_t* n, std::string* err,
                 size_t* first) {
    if (first) *first = 2;  // "list" holds 2 elements: an append adds element 2
    trace.push_back("append " + path);
    *n = 0;
    if (path == "bad") return *err = "\"append\": bad: not a list", false;
    if (path == "huge") b->resize((64u << 20) + 16), *n = 1;
    else if (path == "list") mark(b, 10000), *n = 1;
    return true;
}
}
}

namespace {
using namespace kc;
struct Rng {
    uint64_t s;
    uint32_t next() {
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<uint32_t>(s >> 33);
    }
    uint32_t below(uint32_t n) { return next() % n; }
    template <class T>
    const T& pick(const std::vector<T>& v) { return v[below(static_cast<uint32_t>(v.size()))]; }
};

std::string outcome(const nlohmann::json& envelope) {
    const std::string text = envelope.dump();
    KilnBootPrepareInputV1 in{sizeof(in), 1, 1, {text.data(), text.size()}};
    KilnBootPreparedV1 out{};
    out.size = sizeof out;
    KilnBootIssueV1 issue{};
    issue.size = sizeof issue;
    if (prepare_boot(nullptr, &in, &out, &issue) != KILN_OK) return "ERR";
    if (!out.ok) return std::string("NO ") + issue.mod_id + "/" + issue.key + " " + issue.kind + ": " + issue.reason;
    const auto* plan = static_cast<const BootPlan*>(out.plan);
    std::string s = "OK " + std::to_string(plan->sets) + " " + std::to_string(plan->resources.size());
    for (const auto& rp : plan->resources) s += " " + guid_text(rp.res.guid) + ":" + std::to_string(rp.docs.size());
    out.destroy(out.plan);
    return s;
}

std::string path(Rng& r, bool tmpl, bool star) {
    static const std::vector<std::string> members = {"list", "a", "b", "List", "steps", "c"};
    static const std::vector<std::string> comps = {"Alpha", "Beta", "Alpha#1", "alpha#0", "components"};
    std::string p = tmpl ? r.pick(comps) : r.pick(members);
    const uint32_t n = r.below(3);
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t k = r.below(6);
        if (k == 0) p += "[0]";
        else if (k == 1) p += "[1]";
        else if (k == 2) p += star ? "[*]" : "[01]";
        else if (k == 3) p += "[01]";
        else p += "." + r.pick(members);
    }
    if (r.below(400) == 0)  // deeper than the claim index keeps
        for (int i = 0; i < 70; ++i) p += ".d";
    return p;
}

nlohmann::json doc(Rng& r) {
    const bool tmpl = r.below(4) == 0;
    static const std::vector<std::string> res = {"fixture", "x", "yy"};
    nlohmann::json d = {{"resource", tmpl ? std::string("fixture") : r.pick(res)}, {"type", tmpl ? "keen::ecs::TemplateResource" : "fixture"}};
    const uint32_t changes = 1 + r.below(3);
    for (uint32_t i = 0; i < changes; ++i) {
        const uint32_t k = r.below(tmpl ? 9 : 6);
        const std::vector<nlohmann::json> values = {1, 2, nlohmann::json{{"mul", 2}}};
        if (k <= 2) d["set"][path(r, tmpl, true)] = r.pick(values);
        else if (k == 3) d["clear"].push_back(path(r, tmpl, r.below(8) == 0));
        else if (k == 4) d["replace"][path(r, tmpl, false)] = {{"values", r.below(2) ? nlohmann::json::array({1}) : nlohmann::json::array({2})}};
        else if (k == 5) d["append"][path(r, tmpl, false)] = {{"values", nlohmann::json::array({1})}};
        else if (k == 6) d["addComponents"].push_back(r.below(2) ? "Alpha" : "Beta");
        else if (k == 7) d["removeComponents"].push_back(r.below(2) ? "Alpha" : "Beta");
        else d["copyComponents"][r.below(2) ? "Alpha" : "Beta"] = "x";
    }
    return d;
}

nlohmann::json envelope(const nlohmann::json& docs) {
    return {{"owners", {{{"id", "a"}}, {{"id", "b"}}, {{"id", "c"}}}}, {"collectors", {{"patches", docs}}}};
}
}  // namespace

int main(int argc, char** argv) {
    const bool bench = argc > 1 && std::string(argv[1]) == "bench";
    if (!bench) {
        Rng r{20261121};
        static const std::vector<std::string> mods = {"a", "b", "c"};
        for (int c = 0; c < 6000; ++c) {
            nlohmann::json docs = nlohmann::json::array();
            const uint32_t n = 2 + r.below(7);
            for (uint32_t i = 0; i < n; ++i) docs.push_back({{"owner", r.pick(mods)}, {"key", "k" + std::to_string(i)}, {"document", doc(r)}});
            std::printf("%d %s\n", c, outcome(envelope(docs)).c_str());
        }
        return 0;
    }
    // 4,096 changes: 2,048 sets of one list's elements by two mods (the same-field check), then 1,024 lists cleared by one
    // mod and 1,024 sets below other lists by another (the list checks walk every earlier claim of every resource)
    nlohmann::json docs = nlohmann::json::array();
    for (int i = 0; i < 2048; ++i)
        docs.push_back({{"owner", i % 2 ? "a" : "b"}, {"key", "f" + std::to_string(i)}, {"document", {{"resource", "fixture"}, {"type", "fixture"}, {"set", {{"list[" + std::to_string(i) + "]", 1}}}}}});
    for (int i = 0; i < 1024; ++i)
        docs.push_back({{"owner", "a"}, {"key", "c" + std::to_string(i)}, {"document", {{"resource", "x"}, {"type", "fixture"}, {"clear", {"l" + std::to_string(i)}}}}});
    for (int i = 0; i < 1024; ++i)
        docs.push_back({{"owner", "b"}, {"key", "s" + std::to_string(i)}, {"document", {{"resource", "yy"}, {"type", "fixture"}, {"set", {{"m" + std::to_string(i) + "[0].v", 1}}}}}});
    const nlohmann::json e = envelope(docs);
    double best = 1e9;
    std::string first;
    for (int k = 0; k < 3; ++k) {
        const auto t0 = std::chrono::steady_clock::now();
        const std::string o = outcome(e);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        best = std::min(best, ms);
        first = o.substr(0, 40);
    }
    std::printf("bench: 4096 changes, best of 3: %.1f ms (%s)\n", best, first.c_str());
}
