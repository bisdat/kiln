// SPDX-License-Identifier: MIT
// The structural edits of a boot resource patch (gamedata_bootstruct.cpp) on hand-made bytes and types. No game.
#include "../gamedata_bootstruct.cpp"
#include <cmath>
#include <cstdio>
#include <map>
#include <stdexcept>
namespace kc {
namespace gd {
// types: a template with a components list, three component types, CurrentTransform, a struct with a list of floats
TType comp_base, alpha, beta, gammaT, transform, tmpl, f32, steps_list, holder;
std::map<uint32_t, const TType*> by_hash;
const TType* TType::resolved() const { return this; }
const TType* TType::inner() const {
    if (this == &alpha || this == &beta || this == &gammaT || this == &transform) return &comp_base;
    if (this == &steps_list) return &f32;
    return nullptr;
}
const TField* TType::field(const std::string& n) const {
    for (const TField& f : fields) if (f.name == n) return &f;
    return nullptr;
}
const TType* TField::type() const { return nullptr; }
const TType* type_hash(uint32_t h) { auto it = by_hash.find(h); return it == by_hash.end() ? nullptr : it->second; }
const TType* type_named(const std::string& n) {
    for (const TType* t : {&alpha, &beta, &gammaT, &transform}) if (t->name == n) return t;
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
// "steps" names holder's list at offset 0
bool resolve(Root, Node root, const std::string& path, std::vector<Resolved>* out, std::string* err, bool*) {
    if (path != "steps") return *err = "no such member", false;
    Resolved r{};
    r.node = {&steps_list, root.addr};
    out->push_back(r);
    return true;
}
}  // namespace gd
}  // namespace kc

namespace {
using namespace kc;
using namespace kc::gd;
int checks = 0;
void check(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); ++checks; }
template <class T> T at(const std::vector<uint8_t>& b, size_t o) { T v; std::memcpy(&v, b.data() + o, sizeof v); return v; }
template <class T> void put(std::vector<uint8_t>& b, size_t o, T v) { std::memcpy(b.data() + o, &v, sizeof v); }

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
    tmpl.name = "keen::ecs::TemplateResource", tmpl.kind = K_STRUCT, tmpl.size = 0x10;
    tmpl.fields.push_back({"components", 0, 8});
    f32.kind = K_F32, f32.size = 4;
    steps_list.kind = K_BLOBARRAY, steps_list.size = 8;
    holder.kind = K_STRUCT, holder.size = 8;
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
    check(boot_append(Root::Plain, &holder, "steps", vals, false, &d, &n, &err), "append all");
    check(n == 4 && floats_of(d).size() == 6, "every value appended");
    check(!boot_append(Root::Plain, &holder, "nope", vals, false, &d, &n, &err), "an unknown path is refused");
    std::vector<uint8_t> empty(0x10, 0);
    check(boot_append(Root::Plain, &holder, "steps", vals, true, &empty, &n, &err) && n == 3, "an empty list: 1.5, 2.5, 3 then 2.75 is below");
}
}  // namespace

int main() {
    try {
        types();
        add_components();
        remove_components();
        append();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "bootstruct-test: FAIL after %d checks: %s\n", checks, e.what());
        return 1;
    }
    std::printf("bootstruct-test: %d checks passed\n", checks);
    return 0;
}
