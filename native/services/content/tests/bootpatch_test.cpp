// SPDX-License-Identifier: MIT
// Real collector and apply, with fake boot/assets tables, a few made-up types and an 8-byte resource. No game or Wine.
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
void mark(std::vector<uint8_t>* b, uint32_t add) { uint32_t v; std::memcpy(&v, b->data() + 4, 4); v += add; std::memcpy(b->data() + 4, &v, 4); }
bool boot_remove_components(const TType*, const std::vector<std::string>& names, std::vector<uint8_t>* b, size_t* n, std::string* err) {
    *n = 0;
    for (const auto& x : names) {
        if (x == "Stuck") return *err = "\"removeComponents\": Stuck cannot go", false;
        if (x == "Alpha") mark(b, 100), ++*n;
    }
    return true;
}
bool boot_add_components(const TType*, const std::vector<std::string>& names, std::vector<uint8_t>* b, size_t* n, std::string* err) {
    *n = 0;
    for (const auto& x : names) {
        if (x == "Broken") return *err = "\"addComponents\": \"Broken\" cannot be added", false;
        if (x == "Beta") mark(b, 1000), ++*n;
    }
    return true;
}
bool boot_append(Root, const TType*, const std::string& path, const std::vector<gjson>&, bool, std::vector<uint8_t>* b, size_t* n, std::string* err) {
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
int checks = 0, put_count = 0;
bool put_fail = false;
struct Res { uint32_t value, marks; } vanilla{10, 0}, patched{};
size_t patched_len = 0;
const void* active = nullptr;
std::string row_text;
struct Fault { std::string mod, key, reason; };
std::vector<Fault> faults;
KilnBootApiV1 boot{};
KilnAssetsApi asset{};
KilnCapsApi caps{};
KilnApi api{};
void check(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); ++checks; }
std::string str(KilnStr s) { return std::string(s.ptr, s.len); }
bool has(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }
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
// documents of mods a and b (in that order), each {owner, key, document}
nlohmann::json docs_of(std::initializer_list<std::tuple<const char*, const char*, nlohmann::json>> docs) {
    nlohmann::json list = nlohmann::json::array();
    for (const auto& [owner, key, doc] : docs) list.push_back({{"owner", owner}, {"key", key}, {"document", doc}});
    return {{"owners", {{{"id", "a"}}, {{"id", "b"}}}}, {"collectors", {{"patches", list}}}};
}
nlohmann::json plain(nlohmann::json body) { body["resource"] = "fixture"; body["type"] = "fixture"; return body; }
nlohmann::json templ(nlohmann::json body) { body["resource"] = "fixture"; body["type"] = "keen::ecs::TemplateResource"; return body; }
KilnBootIssueV1 issue{};
// prepare only: true when the plan is accepted (issue holds the refusal otherwise)
bool accepted(const nlohmann::json& e) {
    issue = {}; issue.size = sizeof(issue);
    auto p = prepare(e, &issue);
    if (p.ok) p.destroy(p.plan);
    return p.ok;
}
bool refused(const nlohmann::json& e, const char* mod, const std::string& part) {
    return !accepted(e) && std::string(issue.mod_id) == mod && has(issue.reason, part);
}
// prepare and apply: the resource the game would read is in `patched` (put_count counts the puts)
void run(const nlohmann::json& e) {
    faults.clear(); put_count = 0; patched = {}; patched_len = 0;
    issue = {}; issue.size = sizeof(issue);
    auto p = prepare(e, &issue);
    if (!p.ok) throw std::runtime_error(std::string("the plan is refused: ") + issue.mod_id + "/" + issue.key + ": " + issue.reason);
    ++checks;
    active = p.plan;
    apply(nullptr);
    active = nullptr;
    p.destroy(p.plan);
}

void types() {
    using namespace gd;
    auto rec = [](const TType& t) { return reinterpret_cast<uintptr_t>(&t); };
    scalar.name = "uint32", scalar.kind = K_U32, scalar.size = 4;
    list_u32.name = "keen::BlobArray<uint32>", list_u32.kind = K_BLOBARRAY, list_u32.size = 8, list_u32.inner_rec = rec(scalar);
    resource.name = "fixture", resource.brief = "fixture", resource.kind = K_STRUCT, resource.size = 8;
    resource.fields = {{"value", rec(scalar), 0}, {"list", rec(list_u32), 0}};
    variant.name = "keen::BlobVariant", variant.kind = K_BLOBVAR, variant.size = 12;
    var_list.name = "keen::BlobArray<keen::BlobVariant>", var_list.kind = K_BLOBARRAY, var_list.size = 8, var_list.inner_rec = rec(variant);
    tmpl.name = "keen::ecs::TemplateResource", tmpl.brief = "TemplateResource", tmpl.kind = K_STRUCT, tmpl.size = 8;
    tmpl.fields = {{"name", 0, 0}, {"components", rec(var_list), 0}};
    comp_base.name = "keen::ecs::Component", comp_base.brief = "Component", comp_base.kind = K_STRUCT, comp_base.size = 1;
    auto comp = [&](TType& t, const char* brief, uint32_t hash) {
        t.name = std::string("keen::ecs::") + brief, t.brief = brief, t.kind = K_STRUCT, t.size = 8, t.hash = hash, t.inner_rec = rec(comp_base);
    };
    comp(alpha, "Alpha", 0xa1);
    alpha.fields = {{"x", rec(scalar), 0}, {"steps", rec(list_u32), 0}};
    comp(beta, "Beta", 0xb2);
    beta.fields = {{"y", rec(scalar), 0}};
    comp(transform, "CurrentTransform", 0xd4);
}

// A document is applied whole or not at all, and the resource is what the other documents make it.
void all_or_nothing() {
    // template: a's remove would succeed, its add fails: neither is kept; b's add is
    run(docs_of({{"a", "k1", templ({{"removeComponents", {"Alpha"}}, {"addComponents", {"Broken"}}})},
                 {"b", "k2", templ({{"addComponents", {"Beta"}}})}}));
    check(faults.size() == 1 && faults[0].mod == "a" && faults[0].key == "k1", "one fault, naming the mod and the key");
    check(has(faults[0].reason, "Broken") && has(faults[0].reason, "nothing of this patch was applied"), "the fault says why and that nothing was applied");
    check(put_count == 1 && patched.marks == 1000, "only b's add is in the served copy (a's remove is not)");
    // a set that fails takes the document's append with it; b's set stays
    run(docs_of({{"a", "k1", plain({{"append", {{"list", {{"values", {1}}}}}}, {"set", {{"missing.x", 5}}}})},
                 {"b", "k2", plain({{"set", {{"value", 42}}}})}}));
    check(faults.size() == 1 && faults[0].mod == "a" && has(faults[0].reason, "\"missing.x\""), "the failing set names its path");
    check(put_count == 1 && patched.value == 42 && patched.marks == 0, "b's set is served, a's append is not");
    // an index past the list's end at apply: the same
    run(docs_of({{"a", "k1", templ({{"removeComponents", {"Alpha"}}, {"set", {{"past[9]", 1}}}})}}));
    check(faults.size() == 1 && has(faults[0].reason, "IndexPastEnd") && put_count == 0, "nothing of the document, nothing to serve");
    // [*] over an empty list is not a failure
    run(docs_of({{"a", "k1", templ({{"removeComponents", {"Alpha"}}, {"set", {{"empty[*]", 1}}}})}}));
    check(faults.empty() && put_count == 1 && patched.marks == 100, "an empty [*] changes nothing and is no fault");
    // every document fails: the game's bytes stay
    run(docs_of({{"a", "k1", templ({{"addComponents", {"Broken"}}})}, {"b", "k2", templ({{"removeComponents", {"Stuck"}}})}}));
    check(faults.size() == 2 && faults[0].mod == "b" && faults[1].mod == "a" && put_count == 0, "two faults (removes are made first), nothing served");
    check(vanilla.value == 10 && vanilla.marks == 0, "the game's own bytes are never written");
}

// A structural-only document is faulted when kiln.assets refuses the copy, and when the copy would be over 64 MiB.
void put_and_size_faults() {
    put_fail = true;
    run(docs_of({{"a", "k1", templ({{"removeComponents", {"Alpha"}}})}, {"b", "k2", templ({{"set", {{"value", 3}}}})}}));
    put_fail = false;
    check(faults.size() == 2 && faults[0].mod == "a" && faults[0].key == "k1" && faults[1].mod == "b", "both documents are faulted, the structural one too");
    check(has(faults[0].reason, "kiln.assets refused"), "the fault says kiln.assets refused the copy");
    run(docs_of({{"a", "big", plain({{"append", {{"huge", {{"values", {1}}}}}}})}, {"b", "k2", plain({{"set", {{"value", 42}}}})}}));
    check(faults.size() == 1 && faults[0].mod == "a" && faults[0].key == "big" && has(faults[0].reason, "64 MiB"), "the append over 64 MiB is faulted");
    check(put_count == 1 && patched.value == 42 && patched_len == 8, "the other document is served, at the resource's own size");
}

// Two mods that would change one thing two ways: the later is refused, in prepare.
void conflicts() {
    // a component one mod removes, and another mod's path that starts at it (either order, any case, "#0")
    check(refused(docs_of({{"a", "k1", templ({{"removeComponents", {"Alpha"}}})}, {"b", "k2", templ({{"set", {{"alpha#0.x", 1}}}})}}), "b",
                  "removes the component alpha"), "a set on a component another mod removes is refused");
    check(refused(docs_of({{"a", "k1", templ({{"set", {{"Alpha.x", 1}}}})}, {"b", "k2", templ({{"removeComponents", {"Alpha"}}})}}), "b",
                  "\"Alpha.x\""), "removing a component another mod's set starts at is refused");
    check(refused(docs_of({{"a", "k1", templ({{"append", {{"Alpha.steps", {{"values", {1}}}}}}})}, {"b", "k2", templ({{"removeComponents", {"Alpha"}}})}}),
                  "b", "appends to"), "removing a component another mod appends to is refused");
    check(accepted(docs_of({{"a", "k1", templ({{"removeComponents", {"Alpha"}}})}, {"b", "k2", templ({{"set", {{"Beta.y", 1}}}})}})),
          "a set on another component is fine");
    // components[i] on a template whose components change: any mod, either order
    check(refused(docs_of({{"a", "k1", templ({{"addComponents", {"Beta"}}})}, {"b", "k2", templ({{"set", {{"components[1].y", 1}}}})}}), "b",
                  "by its place in the list"), "components[i] after another mod's add is refused");
    check(refused(docs_of({{"a", "k1", templ({{"set", {{"Components[0].x", 1}}}})}, {"b", "k2", templ({{"removeComponents", {"Alpha"}}})}}), "b",
                  "by its place in the list"), "a remove after another mod's components[i] is refused");
    check(refused(docs_of({{"a", "k1", templ({{"addComponents", {"Beta"}}, {"set", {{"components[2].y", 1}}}})}}), "a", "by its place in the list"),
          "one document that adds and indexes components is refused");
    check(accepted(docs_of({{"a", "k1", templ({{"set", {{"components[0].x", 1}}}})}})), "components[i] on a template nobody changes is fine");
    // append and another mod's [*] on the same list: the later is refused; [i] is not
    check(refused(docs_of({{"a", "k1", plain({{"append", {{"list", {{"values", {1}}}}}}})}, {"b", "k2", plain({{"set", {{"list[*]", 2}}}})}}), "b",
                  "appends to this list"), "[*] over a list another mod appends to is refused");
    check(refused(docs_of({{"a", "k1", plain({{"set", {{"List[*]", 2}}}})}, {"b", "k2", plain({{"append", {{"list.", {{"values", {1}}}}}}})}}), "b",
                  "sets every element"), "an append to a list another mod sets with [*] is refused");
    check(accepted(docs_of({{"a", "k1", plain({{"append", {{"list", {{"values", {1}}}}}}})}, {"b", "k2", plain({{"set", {{"list[0]", 2}}}})}})),
          "an element by index is fine");
    check(accepted(docs_of({{"a", "k1", plain({{"append", {{"list", {{"values", {1}}}}}}, {"set", {{"list[*]", 2}}}})}})),
          "one mod's own append and [*] are fine (the set sees the appended values)");
    // the keys are the normalised paths: case, a trailing dot, an index's zeros
    check(refused(docs_of({{"a", "k1", plain({{"append", {{"Steps", {{"values", {1}}}}}}})}, {"b", "k2", plain({{"append", {{"steps.", {{"values", {2}}}}}}})}}),
                  "b", "appends to this list"), "two appends to one list, written two ways, are refused");
    check(refused(docs_of({{"a", "k1", plain({{"set", {{"Value", 1}}}})}, {"b", "k2", plain({{"set", {{"value", 2}}}})}}), "b", "sets this field"),
          "two sets of one field, written two ways, are refused");
    check(refused(docs_of({{"a", "k1", plain({{"set", {{"list[03]", 1}}}})}, {"b", "k2", plain({{"set", {{"list[3]", 2}}}})}}), "b", "sets this field"),
          "[03] is [3]");
    check(refused(docs_of({{"a", "k1", plain({{"append", {{"list[*]", {{"values", {1}}}}}}})}}), "a", "one list"), "an append names one list");
    check(refused(docs_of({{"a", "k1", plain({{"set", {{"value", 1}}}, {"sett", {{"value", 1}}}})}}), "a", "unknown member \"sett\""),
          "an unknown member is refused");
}

// Every change counts against the limit of 4096: sets, appends, components.
void limit() {
    nlohmann::json sets = nlohmann::json::object();
    for (int i = 0; i < 4094; ++i) sets["v" + std::to_string(i)] = 1;
    check(accepted(docs_of({{"a", "k1", templ({{"removeComponents", {"Alpha"}}, {"append", {{"list", {{"values", {1}}}}}}, {"set", sets}})}})),
          "4096 changes are accepted");
    check(refused(docs_of({{"a", "k1", templ({{"removeComponents", {"Alpha", "Gamma"}}, {"append", {{"list", {{"values", {1}}}}}}, {"set", sets}})}}), "a",
                  "more than 4096 changes"), "a component over the limit is refused");
    check(refused(docs_of({{"a", "k1", templ({{"append", {{"list", {{"values", {1}}}}, {"list2", {{"values", {1}}}}, {"list3", {{"values", {1}}}}}}, {"set", sets}})}}),
                  "a", "more than 4096 changes"), "appends count against the limit");
}

// With the game's types: what can be checked before the game starts is.
void typed_prepare() {
    gd::g_types = true;
    check(refused(docs_of({{"a", "k1", templ({{"addComponents", {"Component"}}})}}), "a", "not a component type"), "the base type Component is refused");
    check(refused(docs_of({{"a", "k1", templ({{"addComponents", {"Nope"}}})}}), "a", "not a component type"), "an unknown type is refused");
    check(accepted(docs_of({{"a", "k1", templ({{"addComponents", {"Beta"}}, {"set", {{"Beta.y", 2}}}})}})), "a component of the game, and its member");
    check(refused(docs_of({{"a", "k1", templ({{"removeComponents", {"CurrentTransform"}}})}}), "a", "CurrentTransform"),
          "CurrentTransform is refused in prepare");
    // a set-only template patch is checked as a plain one is
    check(refused(docs_of({{"a", "k1", templ({{"set", {{"Alpah.x", 1}}}})}}), "a", "not a component type"), "a misspelt component is refused");
    check(refused(docs_of({{"a", "k1", templ({{"set", {{"Alpha.nope", 1}}}})}}), "a", "has no member nope"), "a misspelt member is refused");
    check(refused(docs_of({{"a", "k1", templ({{"set", {{"Alpha", 1}}}})}}), "a", "names a member of a component"), "a path that stops at the component is refused");
    check(refused(docs_of({{"a", "k1", templ({{"set", {{"Alpha.steps", 1}}}})}}), "a", "NotScalar"), "a list is no scalar");
    check(refused(docs_of({{"a", "k1", templ({{"set", {{"Alpha#x.x", 1}}}})}}), "a", "is a number"), "a bad #k is refused");
    check(accepted(docs_of({{"a", "k1", templ({{"set", {{"Alpha#1.x", 1}, {"components[0]", 1}}}})}})), "#k and the template's own member pass to the bytes");
    // an append's path is a list of scalars, its values fit the element
    check(accepted(docs_of({{"a", "k1", templ({{"append", {{"Alpha.steps", {{"values", {1, 2}}}}}}})}})), "an append to a component's list");
    check(refused(docs_of({{"a", "k1", templ({{"append", {{"Alpha.x", {{"values", {1}}}}}}})}}), "a", "NotAList"), "an append to a scalar is refused");
    check(refused(docs_of({{"a", "k1", plain({{"append", {{"list", {{"values", {"Hard"}}}}}}})}}), "a", "not a number"), "a value that does not fit is refused");
    check(refused(docs_of({{"a", "k1", plain({{"append", {{"value", {{"values", {1}}}}}}})}}), "a", "NotAList"), "a plain resource's scalar is no list");
    gd::g_types = false;
}
}
int main() {
    try {
        types();
        boot.header.size = sizeof(boot);
        boot.active_plan = [](KilnStr, uint64_t*) -> const void* { return active; };
        boot.activation_fault = [](KilnOwner, KilnStr mod, KilnStr, KilnStr key, KilnStr reason) -> KilnResult {
            faults.push_back({str(mod), str(key), str(reason)}); return KILN_OK;
        };
        asset.header.size = sizeof(asset);
        asset.when_resources_ready = [](void(KILN_CALL*)(void*), void*) -> KilnResult { return KILN_OK; };
        asset.resource_data = [](const KilnAssetKey*, const void** data, size_t* len) -> KilnResult { *data = &vanilla; *len = sizeof vanilla; return KILN_OK; };
        asset.resource_put = [](KilnOwner, const KilnAssetKey*, const void* data, size_t len, uint32_t) -> KilnResult {
            if (put_fail) return KILN_ERR_UNAVAILABLE;
            if (len >= sizeof patched) std::memcpy(&patched, data, sizeof patched);
            patched_len = len;
            ++put_count; return KILN_OK;
        };
        caps.report = [](KilnOwner, KilnStr, uint32_t, uint32_t, KilnStr text) -> KilnResult { row_text = str(text); return KILN_OK; };
        api.service = &service; g_api = &api;
        auto ir = envelope();
        auto plan = prepare(ir, &issue);
        check(plan.ok, "normal prepare succeeds"); active = plan.plan;
        apply(nullptr);
        check(put_count == 1 && patched.value == 42 && vanilla.value == 10, "published readiness writes the override, preserves vanilla");
        plan.destroy(plan.plan); active = nullptr;
        // The structural edits' shapes (their bytes: bootstruct_test.cpp).
        auto one = [&](nlohmann::json doc) { return accepted(docs_of({{"a", "k", doc}})); };
        check(one({{"resource", "fixture"}, {"type", "fixture"}, {"append", {{"list", {{"values", {1, 2}}, {"aboveLast", true}}}}}}),
              "an append alone is a patch");
        check(!one({{"resource", "fixture"}, {"type", "fixture"}, {"addComponents", {"Alpha"}}}) &&
              std::string(issue.reason).find("only a template") != std::string::npos, "components on a non-template are refused");
        check(!one({{"resource", "fixture"}, {"type", "fixture"}, {"append", {{"list", {{"values", "x"}}}}}}), "a bad append is refused");
        check(!one({{"resource", "fixture"}, {"type", "fixture"}, {"append", {{"list", {{"values", {1}}, {"more", 1}}}}}}),
              "an unknown append member is refused");
        check(!one({{"resource", "fixture"}, {"type", "fixture"}, {"set", nlohmann::json::object()}}), "nothing to do is refused");
        check(one({{"resource", "fixture"}, {"type", "keen::ecs::TemplateResource"}, {"removeComponents", {"SpawnTime"}}}),
              "a template's components (types unchecked here)");
        check(!one({{"resource", "fixture"}, {"type", "keen::ecs::TemplateResource"}, {"removeComponents", {"currentTransform"}}}),
              "CurrentTransform is refused without the types too");
        all_or_nothing();
        put_and_size_faults();
        conflicts();
        limit();
        typed_prepare();
        // Readiness without a published plan (also the immediate callback at a late bind).
        put_count = 0;
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
        check(put_count == 0, "missed readiness never retries writes");
    } catch (const std::exception& e) { std::printf("bootpatch-test: FAIL %s (after %d checks)\n", e.what(), checks); return 1; }
    std::printf("bootpatch-test: %d checks passed\n", checks);
}
