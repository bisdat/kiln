// SPDX-License-Identifier: MIT
// The structural edits of a boot resource patch (gamedata_bootstruct.cpp) on hand-made bytes and types. No game.
#include "../gamedata_bootstruct.cpp"
#include <cmath>
#include <cstdio>
#include <map>
#include <stdexcept>
namespace kc {
const KilnApi* g_api = nullptr;
namespace gd {
// types: a template with a components list, three component types, CurrentTransform, an intermediate base below
// keen::ecs::Component that is no component of the game's (Middle), a struct with a list of floats; for copyComponents a
// component Stock with every heap part (a list of structs that hold lists, a string, an optional, a variant), Actor, and a
// component with a member of the editor's Ds kinds (Editor)
TType comp_base, middle, alpha, beta, gammaT, transform, tmpl, f32, steps_list, holder;
TType u8b, u32t, item, item_list, f32_list, str, opt_f32, variant, inner, stock, actorT, editor, ds_array;
std::map<uint32_t, const TType*> by_hash;
const TType* TType::resolved() const { return this; }
const TType* TType::inner() const {
    if (this == &alpha || this == &beta || this == &gammaT || this == &transform || this == &middle) return &comp_base;
    if (this == &steps_list) return &f32;
    return reinterpret_cast<const TType*>(inner_rec);
}
const TField* TType::field(const std::string& n) const {
    for (const TField& f : fields) if (f.name == n) return &f;
    return nullptr;
}
const TType* TField::type() const { return reinterpret_cast<const TType*>(type_rec); }
const TType* type_hash(uint32_t h) { auto it = by_hash.find(h); return it == by_hash.end() ? nullptr : it->second; }
const TType* type_named(const std::string& n) {
    for (const TType* t : {&alpha, &beta, &gammaT, &transform, &comp_base, &middle, &stock, &actorT, &editor}) if (t->name == n) return t;
    return nullptr;
}
bool leaf_of(const TType* t, Field* f) {
    if (t != &f32) return false;
    f->bytes = 4, f->vt = KILN_CV_FLOAT;
    return true;
}
bool parse_leaf(const Field&, const gjson& v, const uint8_t*, uint8_t* out, bool* reset, std::string* err) {
    if (!v.is_number()) return *err = "not a number", false;
    const float x = v.get<float>();
    std::memcpy(out, &x, 4);
    *reset = false;
    return true;
}
// "steps" names holder's list at offset 0, "items" a list of structs at offset 8, "scalar" a float at offset 0
bool resolve(Root, Node root, const std::string& path, std::vector<Resolved>* out, std::string* err, bool*) {
    Resolved r{};
    if (path == "steps") r.node = {&steps_list, root.addr};
    else if (path == "items") r.node = {&item_list, root.addr + 8};
    else if (path == "scalar") r.node = {&f32, root.addr};
    else return *err = "no such member", false;
    out->push_back(r);
    return true;
}
}  // namespace gd
}  // namespace kc

namespace {
using namespace kc;
using namespace kc::gd;
int checks = 0;
std::string last_err;
void check(bool ok, const char* why) { if (!ok) throw std::runtime_error(std::string(why) + (last_err.empty() ? "" : " (" + last_err + ")")); ++checks; }
template <class T> T at(const std::vector<uint8_t>& b, size_t o) { T v; std::memcpy(&v, b.data() + o, sizeof v); return v; }
template <class T> void put(std::vector<uint8_t>& b, size_t o, T v) { std::memcpy(b.data() + o, &v, sizeof v); }
int32_t at_i32(const std::vector<uint8_t>& b, size_t o) { return at<int32_t>(b, o); }

void types() {
    comp_base.name = "keen::ecs::Component";
    auto comp = [](TType& t, const char* brief, uint32_t hash, uint32_t size) {
        t.name = std::string("keen::ecs::") + brief, t.brief = brief, t.hash = hash, t.size = size, t.kind = K_STRUCT;
        by_hash[hash] = &t;
    };
    comp(alpha, "Alpha", 0xa1, 8);
    comp(beta, "Beta", 0xb2, 12);
    comp(gammaT, "Gamma", 0xc3, 4);
    comp(transform, "CurrentTransform", 0xd4, 16);
    comp(middle, "Middle", 0xe5, 8);
    comp_base.kind = K_STRUCT, comp_base.size = 1, comp_base.hash = 0xf6;
    tmpl.name = "keen::ecs::TemplateResource", tmpl.kind = K_STRUCT, tmpl.size = 0x10;
    tmpl.fields.push_back({"components", 0, 8});
    f32.kind = K_F32, f32.size = 4;
    steps_list.kind = K_BLOBARRAY, steps_list.size = 8;
    holder.kind = K_STRUCT, holder.size = 8;
    auto rec = [](const TType& t) { return reinterpret_cast<uintptr_t>(&t); };
    u8b.name = "bool", u8b.kind = K_BOOL, u8b.size = 1;
    u32t.name = "uint32", u32t.kind = K_U32, u32t.size = 4;
    f32.name = "float";
    f32_list.name = "BlobArray<float>", f32_list.kind = K_BLOBARRAY, f32_list.size = 8, f32_list.inner_rec = rec(f32);
    // Item {id u32 +0, tags BlobArray<float> +8}, 16 bytes
    item.name = "Item", item.kind = K_STRUCT, item.size = 16, item.fields = {{"id", rec(u32t), 0}, {"tags", rec(f32_list), 8}};
    item_list.name = "BlobArray<Item>", item_list.kind = K_BLOBARRAY, item_list.size = 8, item_list.inner_rec = rec(item);
    str.name = "BlobString", str.kind = K_BLOBSTRING, str.size = 8;
    opt_f32.name = "BlobOptional<float>", opt_f32.kind = K_BLOBOPT, opt_f32.size = 4, opt_f32.inner_rec = rec(f32);
    variant.name = "BlobVariant", variant.kind = K_BLOBVAR, variant.size = 12;
    inner.name = "keen::Inner", inner.brief = "Inner", inner.kind = K_STRUCT, inner.size = 8, inner.hash = 0x99, inner.fields = {{"name", rec(str), 0}};
    by_hash[0x99] = &inner;
    comp(stock, "Stock", 0xa7, 40);
    stock.fields = {{"flag", rec(u8b), 0}, {"items", rec(item_list), 4}, {"label", rec(str), 12}, {"opt", rec(opt_f32), 20}, {"var", rec(variant), 24}};
    comp(actorT, "Actor", 0xe1, 4);
    ds_array.name = "keen::DsArray<float>", ds_array.kind = 0x14, ds_array.size = 16;
    comp(editor, "Editor", 0xe7, 16);
    editor.fields = {{"list", rec(ds_array), 0}};
    for (TType* t : {&stock, &actorT, &editor}) t->inner_rec = rec(comp_base);
}

// a template: components at +8 -> list at 0x40: Alpha (data 0x80, 8 bytes), Gamma (data 0x90, 4 bytes), CurrentTransform (0xa0)
std::vector<uint8_t> template_bytes() {
    std::vector<uint8_t> b(0xb0, 0);
    put<int32_t>(b, 8, 0x40 - 8);
    put<uint32_t>(b, 12, 3);
    auto var = [&](size_t i, uint32_t hash, size_t data, uint32_t size) {
        const size_t e = 0x40 + i * 12;
        put<uint32_t>(b, e, hash);
        put<int32_t>(b, e + 4, static_cast<int32_t>(data - (e + 4)));
        put<uint32_t>(b, e + 8, size);
    };
    var(0, 0xa1, 0x80, 8);
    var(1, 0xd4, 0xa0, 16);
    var(2, 0xc3, 0x90, 4);
    put<uint64_t>(b, 0x80, 0x1111111111111111ull);  // Alpha's data
    put<uint32_t>(b, 0x90, 0x33333333u);            // Gamma's data
    return b;
}

struct Entry { uint32_t hash, size; size_t data; };
std::vector<Entry> list_of(const std::vector<uint8_t>& b) {
    const size_t l = 8 + at<int32_t>(b, 8);
    std::vector<Entry> out;
    for (uint32_t i = 0; i < at<uint32_t>(b, 12); ++i) {
        const size_t e = l + i * 12;
        out.push_back({at<uint32_t>(b, e), at<uint32_t>(b, e + 8), static_cast<size_t>(static_cast<int64_t>(e) + 4 + at<int32_t>(b, e + 4))});
    }
    return out;
}

void add_components() {
    auto b = template_bytes();
    size_t n = 0;
    std::string err;
    check(boot_add_components(&tmpl, {"Beta", "Alpha"}, &b, &n, &err), "add Beta");
    check(n == 1, "only the missing component is added");
    const auto l = list_of(b);
    check(l.size() == 4, "four components");
    // sorted by type name: Alpha, Beta, CurrentTransform, Gamma
    check(l[0].hash == 0xa1 && l[1].hash == 0xb2 && l[2].hash == 0xd4 && l[3].hash == 0xc3, "sorted by name");
    check(l[0].data == 0x80 && at<uint64_t>(b, l[0].data) == 0x1111111111111111ull, "Alpha keeps its data");
    check(l[3].data == 0x90 && at<uint32_t>(b, l[3].data) == 0x33333333u, "Gamma keeps its data");
    check(l[1].size == 12 && l[1].data >= 0xb0 && l[1].data % 16 == 0 && l[1].data + 12 <= b.size(), "Beta's data behind the copy");
    for (size_t i = 0; i < 12; ++i) check(b[l[1].data + i] == 0, "Beta is zeroed");
    check(!boot_add_components(&tmpl, {"Nope"}, &b, &n, &err), "an unknown type is refused");
}

// The game's list of ECS components (kiln.types): Alpha, Beta, Gamma, CurrentTransform; not Middle, not the base.
const uint32_t kListed[] = {0xa1, 0xb2, 0xc3, 0xd4};
KilnTypesApi types_table{};
KilnApi api{};
const void* KILN_CALL service(KilnStr name, uint32_t) {
    return std::string(name.ptr, name.len) == KILN_TYPES_SERVICE ? &types_table : nullptr;
}

// addComponents takes a component of the game's: not keen::ecs::Component itself, nor a base below it the game's list
// of ECS components has not got (without that list, the base class is what says it)
void component_types() {
    std::string err;
    size_t n = 0;
    auto b = template_bytes();
    check(!boot_add_components(&tmpl, {"Component"}, &b, &n, &err) && err.find("not a component type") != std::string::npos,
          "the base type keen::ecs::Component is refused");
    check(component_type("Component", &err) == nullptr && component_type("", &err) == nullptr && component_type("keen::ecs::Alpha", &err) == nullptr,
          "the base, an empty name and a qualified name are no component names");
    check(component_type("Middle", &err) == &middle, "without kiln.types' list, a type below Component is one");
    types_table.header.size = sizeof types_table;
    types_table.component_type_count = []() -> uint32_t { return 4; };
    types_table.component_type_at = [](uint32_t i) -> const KilnType* { return reinterpret_cast<const KilnType*>(&kListed[i]); };
    types_table.type_info = [](const KilnType* t, KilnTypeInfo* out) -> KilnResult {
        out->hash = *reinterpret_cast<const uint32_t*>(t);
        return KILN_OK;
    };
    api.service = &service;
    g_api = &api;
    check(component_type("Beta", &err) == &beta, "a listed component is one");
    b = template_bytes();
    check(!boot_add_components(&tmpl, {"Middle"}, &b, &n, &err) && err.find("not one of the game's ECS components") != std::string::npos,
          "an intermediate type the game's list has not got is refused");
    check(boot_add_components(&tmpl, {"Beta"}, &b, &n, &err) && n == 1, "a listed one is added");
    g_api = nullptr;
}

void remove_components() {
    auto b = template_bytes();
    size_t n = 0;
    std::string err;
    check(boot_remove_components(&tmpl, {"Gamma", "Beta"}, &b, &n, &err), "remove Gamma");
    check(n == 1, "one removed, one the template hasn't");
    const auto l = list_of(b);
    check(l.size() == 2 && l[0].hash == 0xa1 && l[1].hash == 0xd4, "Alpha and CurrentTransform stay, in order");
    check(l[0].data == 0x80 && l[1].data == 0xa0, "their data stays where it was");
    check(b.size() == 0xb0, "nothing grows");
    check(!boot_remove_components(&tmpl, {"CurrentTransform"}, &b, &n, &err), "CurrentTransform is never removed");
    check(!boot_remove_components(&holder, {"Alpha"}, &b, &n, &err), "a struct without components is refused");
}

std::vector<float> floats_of(const std::vector<uint8_t>& b) {
    const size_t l = at<int32_t>(b, 0);
    std::vector<float> out;
    for (uint32_t i = 0; i < at<uint32_t>(b, 4); ++i) out.push_back(at<float>(b, l + i * 4));
    return out;
}

void append() {
    std::vector<uint8_t> b(0x30, 0);
    put<int32_t>(b, 0, 0x20);
    put<uint32_t>(b, 4, 2);
    put<float>(b, 0x20, 1.0f);
    put<float>(b, 0x24, 2.0f);
    size_t n = 0;
    std::string err;
    const std::vector<gjson> vals = {gjson(1.5), gjson(2.5), gjson(3), gjson(2.75)};
    auto c = b;
    check(boot_append(Root::Plain, &holder, "steps", vals, true, &c, &n, &err), "append above last");
    check(n == 2, "2.5 and 3 are above the last; 1.5 and 2.75 are not");
    const auto f = floats_of(c);
    check(f.size() == 4 && f[0] == 1 && f[1] == 2 && f[2] == 2.5f && f[3] == 3, "1, 2, 2.5, 3");
    check(at<int32_t>(c, 0) % 16 == 0 && static_cast<size_t>(at<int32_t>(c, 0)) >= 0x30, "the list moved behind the copy");
    auto d = b;
    size_t first = 99;
    check(boot_append(Root::Plain, &holder, "steps", vals, false, &d, &n, &err, &first) && first == 2, "append all, the first appended is element 2");
    check(n == 4 && floats_of(d).size() == 6, "every value appended");
    check(!boot_append(Root::Plain, &holder, "nope", vals, false, &d, &n, &err), "an unknown path is refused");
    std::vector<uint8_t> empty(0x10, 0);
    check(boot_append(Root::Plain, &holder, "steps", vals, true, &empty, &n, &err) && n == 3, "an empty list: 1.5, 2.5, 3 then 2.75 is below");
}
// A source template with Stock (every heap part), Gamma (other data) and Editor: components at +8 -> list at 0x40, Stock's
// data at 0x80, its heap behind it; returns the bytes.
std::vector<uint8_t> source_bytes() {
    std::vector<uint8_t> b(0x200, 0);
    put<int32_t>(b, 8, 0x40 - 8);
    put<uint32_t>(b, 12, 3);
    auto var = [&](size_t i, uint32_t hash, size_t data, uint32_t size) {
        const size_t e = 0x40 + i * 12;
        put<uint32_t>(b, e, hash);
        put<int32_t>(b, e + 4, static_cast<int32_t>(data - (e + 4)));
        put<uint32_t>(b, e + 8, size);
    };
    var(0, 0xe7, 0x70, 16);  // Editor: a Ds list (rel 0)
    var(1, 0xc3, 0x78, 4);   // Gamma
    var(2, 0xa7, 0x80, 40);  // Stock
    put<uint32_t>(b, 0x78, 0x77777777u);
    const size_t s = 0x80;
    b[s] = 1;                                  // flag
    put<int32_t>(b, s + 4, 0x100 - (s + 4));   // items -> 0x100, 2 elements of 16
    put<uint32_t>(b, s + 8, 2);
    put<int32_t>(b, s + 12, 0x140 - (s + 12)); // label -> 0x140, "Magic" (5)
    put<uint32_t>(b, s + 16, 5);
    std::memcpy(b.data() + 0x140, "Magic", 5);
    put<int32_t>(b, s + 20, 0x150 - (s + 20)); // opt -> 0x150: 2.5
    put<float>(b, 0x150, 2.5f);
    put<uint32_t>(b, s + 24, 0x99);            // var: Inner at 0x160, its name -> 0x170 "in" (2)
    put<int32_t>(b, s + 28, static_cast<int32_t>(0x160 - (s + 28)));
    put<uint32_t>(b, s + 32, 8);
    put<int32_t>(b, 0x160, 0x170 - 0x160);
    put<uint32_t>(b, 0x164, 2);
    std::memcpy(b.data() + 0x170, "in", 2);
    // items[0] {id 11, tags -> 0x180 [1, 2]}, items[1] {id 22, tags empty}
    put<uint32_t>(b, 0x100, 11);
    put<int32_t>(b, 0x108, 0x180 - 0x108);
    put<uint32_t>(b, 0x10c, 2);
    put<float>(b, 0x180, 1.0f);
    put<float>(b, 0x184, 2.0f);
    put<uint32_t>(b, 0x110, 22);
    return b;
}
// a self-relative pointer at `at` in b: its target
size_t ptr(const std::vector<uint8_t>& b, size_t at) { return static_cast<size_t>(static_cast<int64_t>(at) + at_i32(b, at)); }

void copy_components() {
    const auto src = source_bytes();
    auto b = template_bytes();
    const size_t before = b.size();
    size_t n = 0;
    std::string err;
    std::vector<CopySource> one = {{"Stock", "magic", src.data(), src.size()}};
    check(boot_copy_components(&tmpl, one, &b, &n, &err) && n == 1, "Stock copied");
    const auto l = list_of(b);
    check(l.size() == 4 && l[0].hash == 0xa1 && l[1].hash == 0xd4 && l[2].hash == 0xc3 && l[3].hash == 0xa7, "added, the list sorted by name");
    check(l[0].data == 0x80 && at<uint64_t>(b, 0x80) == 0x1111111111111111ull, "Alpha keeps its data");
    const size_t s = l[3].data;
    check(l[3].size == 40 && s >= before && s % 16 == 0, "Stock's data behind the copy");
    check(b[s] == 1, "flag copied");
    const size_t items = ptr(b, s + 4);
    check(at<uint32_t>(b, s + 8) == 2 && items >= before && items + 32 <= b.size(), "items: two, behind the copy");
    check(at<uint32_t>(b, items) == 11 && at<uint32_t>(b, items + 16) == 22, "the items' ids");
    const size_t tags = ptr(b, items + 8);
    check(at<uint32_t>(b, items + 12) == 2 && tags >= before && at<float>(b, tags) == 1.0f && at<float>(b, tags + 4) == 2.0f,
          "a list inside a list element is copied too, in the copy's own bytes");
    check(at<int32_t>(b, items + 16 + 8) == 0 && at<uint32_t>(b, items + 16 + 12) == 0, "an empty list stays empty");
    const size_t label = ptr(b, s + 12);
    check(at<uint32_t>(b, s + 16) == 5 && std::memcmp(b.data() + label, "Magic", 5) == 0 && b[label + 5] == 0, "the string, with a zero behind it");
    check(at<float>(b, ptr(b, s + 20)) == 2.5f, "the optional");
    check(at<uint32_t>(b, s + 24) == 0x99 && at<uint32_t>(b, s + 32) == 8, "the variant's type and size");
    const size_t var = static_cast<size_t>(static_cast<int64_t>(s + 28) + at_i32(b, s + 28));
    check(var >= before && std::memcmp(b.data() + ptr(b, var), "in", 2) == 0, "the variant's data and its string");
    // replacing: the template's own Gamma takes the source's data
    check(boot_copy_components(&tmpl, {{"Gamma", "magic", src.data(), src.size()}}, &b, &n, &err) && n == 1, "Gamma copied over");
    const auto l2 = list_of(b);
    check(l2.size() == 4 && l2[2].hash == 0xc3 && at<uint32_t>(b, l2[2].data) == 0x77777777u && l2[2].data != 0x90, "Gamma replaced, no second Gamma");
    check(at<uint32_t>(b, 0x90) == 0x33333333u, "the old data stays where it was, unused");
    // refusals
    auto c = template_bytes();
    check(!boot_copy_components(&tmpl, {{"Beta", "magic", src.data(), src.size()}}, &c, &n, &err) && err.find("has no Beta") != std::string::npos,
          "a component the source has not got is refused");
    check(!boot_copy_components(&tmpl, {{"CurrentTransform", "magic", src.data(), src.size()}}, &c, &n, &err) && err.find("needs to live") != std::string::npos,
          "CurrentTransform is never copied over");
    check(!boot_copy_components(&tmpl, {{"Stock", "gone", nullptr, 0}}, &c, &n, &err) && err.find("MissingResource") != std::string::npos,
          "a source the game build has not got is refused");
    check(!boot_copy_components(&tmpl, {{"Stock", "huge", nullptr, 0, "the template huge is 70000000 bytes, over the 64 MiB a copy is read from"}}, &c, &n, &err) &&
              err.find("over the 64 MiB") != std::string::npos && err.find("MissingResource") == std::string::npos,
          "a source too large to read says so, not that it is missing");
    check(!boot_copy_components(&tmpl, {{"Editor", "magic", src.data(), src.size()}}, &c, &n, &err) && err.find("does not take") != std::string::npos,
          "a member of a kind the copy does not know is refused, not guessed");
    auto broken = src;
    put<int32_t>(broken, 0x80 + 4, 0x7000);  // items point far outside the source
    check(!boot_copy_components(&tmpl, {{"Stock", "magic", broken.data(), broken.size()}}, &c, &n, &err) && err.find("outside") != std::string::npos,
          "a pointer outside the source's bytes is refused");
    check(c == template_bytes(), "a refused copy leaves the list as it was");
    // a layout that does not fit the bytes placed for it is refused, never written past (review: heap overflow probes)
    {
        const auto saved_size = inner.size;
        const auto saved_fields = inner.fields;
        inner.size = 48;  // the variant stores 8 bytes of an Inner that claims 48, with a string at 40
        inner.fields = {{"name", reinterpret_cast<uintptr_t>(&str), 40}};
        check(!boot_copy_components(&tmpl, one, &c, &n, &err) && err.find("smaller than its type") != std::string::npos,
              "a variant smaller than its reflected type is refused");
        inner.size = saved_size, inner.fields = saved_fields;
        const auto saved_item = item.size;
        item.size = 0;  // a list of structs whose size the reflection does not give
        check(!boot_copy_components(&tmpl, one, &c, &n, &err) && err.find("size of its elements is not known") != std::string::npos,
              "a list whose element size is not known is refused");
        item.size = saved_item;
        stock.fields.push_back({"beyond", reinterpret_cast<uintptr_t>(&str), 36});  // an 8-byte member at 36 of the 40 bytes copied
        check(!boot_copy_components(&tmpl, one, &c, &n, &err) && err.find("outside the bytes copied") != std::string::npos,
              "a member past the component's bytes is refused, not written");
        stock.fields.pop_back();
        check(c == template_bytes(), "the refused copies left the bytes as they were");
        check(boot_copy_components(&tmpl, one, &c, &n, &err) && n == 1, "the real layout copies again");
        c = template_bytes();
    }
    // one size cap for the whole call, padding counted, checked before anything is allocated (review)
    {
        const std::vector<CopySource> two = {{"Stock", "magic", src.data(), src.size()}, {"Gamma", "magic", src.data(), src.size()}};
        auto d = template_bytes();
        check(!boot_copy_components(&tmpl, one, &d, &n, &err, d.size()) && err.find("over its size cap") != std::string::npos && d == template_bytes(),
              "no room at all: refused, nothing grows");
        size_t cap = d.size();
        for (bool ok = false; !ok && cap < d.size() + 4096; cap += 16) {
            auto t = template_bytes();
            ok = boot_copy_components(&tmpl, one, &t, &n, &err, cap);
            if (ok) {
                const auto t2 = template_bytes();
                auto u = t2;
                check(!boot_copy_components(&tmpl, two, &u, &n, &err, cap) && err.find("Gamma") != std::string::npos && u == t2,
                      "two components share one budget: the second, past the cap, is refused and nothing is kept");
            }
        }
        check(cap < d.size() + 4096, "the smallest cap that holds Stock was found");
        // a fixed array of more than 65536 elements is refused, never copied with its tail left pointing at the source
        // (in a heap value: Stock's variant holds an Inner of 560,000 bytes that is one such array)
        TType big_array;
        big_array.name = "keen::StaticArray<BlobString,70000>", big_array.kind = K_ARRAY, big_array.count = 70000, big_array.size = 560000;
        big_array.inner_rec = reinterpret_cast<uintptr_t>(&str);
        auto big = src;
        big.resize(0x200 + 560000, 0);
        put<int32_t>(big, 0x80 + 28, static_cast<int32_t>(0x200 - (0x80 + 28)));
        put<uint32_t>(big, 0x80 + 32, 560000);
        const auto saved_size = inner.size;
        const auto saved_fields = inner.fields;
        inner.size = 560000, inner.fields = {{"many", reinterpret_cast<uintptr_t>(&big_array), 0}};
        auto e = template_bytes();
        check(!boot_copy_components(&tmpl, {{"Stock", "magic", big.data(), big.size()}}, &e, &n, &err) && err.find("too many elements") != std::string::npos,
              "an array over 65536 elements is refused");
        inner.size = saved_size, inner.fields = saved_fields;
    }
    // an actor: what it needs to live is not copied over
    auto actor = template_bytes();
    put<uint32_t>(actor, 0x40 + 2 * 12, 0xe1);  // Gamma's entry is an Actor now
    check(!boot_copy_components(&tmpl, {{"Faction", "magic", src.data(), src.size()}}, &actor, &n, &err) && err.find("an actor needs") != std::string::npos,
          "Faction is not copied over onto an actor");
}

void clear_and_replace() {
    std::vector<uint8_t> b(0x40, 0);
    put<int32_t>(b, 0, 0x20);
    put<uint32_t>(b, 4, 2);
    put<float>(b, 0x20, 1.0f);
    put<float>(b, 0x24, 2.0f);
    put<int32_t>(b, 8, 0x30 - 8);  // items: one Item at 0x30
    put<uint32_t>(b, 12, 1);
    size_t n = 0;
    std::string err;
    auto c = b;
    check(boot_clear(Root::Plain, &holder, "items", &c, &n, &err) && n == 1, "a list of structs cleared");
    check(at<int32_t>(c, 8) == 0 && at<uint32_t>(c, 12) == 0 && c.size() == b.size(), "empty, nothing grows");
    check(boot_clear(Root::Plain, &holder, "items", &c, &n, &err) && n == 0, "an empty list: nothing to clear, no error");
    check(!boot_clear(Root::Plain, &holder, "scalar", &c, &n, &err) && err.find("not a list") != std::string::npos, "a scalar is no list");
    check(!boot_clear(Root::Plain, &holder, "nope", &c, &n, &err), "an unknown path is refused");
    auto r = b;
    check(boot_replace(Root::Plain, &holder, "steps", {gjson(5), gjson(6.5), gjson(7)}, &r, &n, &err) && n == 3, "replaced by three");
    const auto f = floats_of(r);
    check(f.size() == 3 && f[0] == 5 && f[1] == 6.5f && f[2] == 7, "exactly 5, 6.5, 7");
    check(static_cast<size_t>(at<int32_t>(r, 0)) >= b.size(), "the new list behind the copy");
    auto r2 = b;
    check(boot_replace(Root::Plain, &holder, "steps", {gjson(9)}, &r2, &n, &err) && floats_of(r2) == std::vector<float>{9}, "a shorter list");
    check(!boot_replace(Root::Plain, &holder, "steps", {gjson("x")}, &r2, &n, &err), "a value that does not fit is refused");
    check(!boot_replace(Root::Plain, &holder, "steps", {}, &r2, &n, &err), "no values is refused");
    check(!boot_replace(Root::Plain, &holder, "items", {gjson(1)}, &r2, &n, &err) && err.find("not a list of numbers") != std::string::npos,
          "a list of structs takes no values");
}
}  // namespace

int main() {
    try {
        types();
        add_components();
        component_types();
        remove_components();
        append();
        copy_components();
        clear_and_replace();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "bootstruct-test: FAIL after %d checks: %s\n", checks, e.what());
        return 1;
    }
    std::printf("bootstruct-test: %d checks passed\n", checks);
    return 0;
}
