// SPDX-License-Identifier: MIT
// gd::resolve (gamedata_walk.cpp), the walker every path of kiln.gamedata, data packs and resource patches goes through,
// over hand-made bytes and types on the host: "[*]" names every element of a list, and none of an empty one, which is no
// error (resource-patches.md: "[*] on an empty list changes nothing"); an index past the list, a member an element has
// not got, and an empty list on the way do not hide each other's errors. No game, no Wine.
#include "../gamedata_walk.cpp"

#include <cstdarg>
#include <cstdio>
#include <stdexcept>

namespace kc {
void logf(int, const char*, ...) {}
bool rd(uintptr_t address, void* out, size_t n) {
    std::memcpy(out, reinterpret_cast<const void*>(address), n);
    return true;
}
std::string read_blob_string(uintptr_t) { return {}; }
std::string lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}
bool guid_parse(const std::string&, uint8_t[16]) { return false; }
uint32_t fnv1a(const void* data, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; ++i) h = (h ^ static_cast<const uint8_t*>(data)[i]) * 16777619u;
    return h;
}
namespace gd {
const TType* TType::resolved() const { return this; }
const TType* TType::inner() const { return reinterpret_cast<const TType*>(inner_rec); }
const TType* TField::type() const { return reinterpret_cast<const TType*>(type_rec); }
const TField* TType::field(const std::string& n) const {
    for (const TField& f : fields)
        if (f.name == n) return &f;
    for (const TField& f : fields)
        if (lower(f.name) == lower(n)) return &f;
    return nullptr;
}
const TType* type_hash(uint32_t) { return nullptr; }
const TType* type_named(const std::string&) { return nullptr; }
bool res_list(uint32_t, std::vector<std::array<uint8_t, 16>>*) { return false; }
uintptr_t res_data(const uint8_t[16], uint32_t, uint64_t*) { return 0; }
}  // namespace gd
}  // namespace kc

namespace {
using namespace kc::gd;
int checks = 0;
void check(bool ok, const std::string& why) {
    if (!ok) throw std::runtime_error(why);
    ++checks;
}

// The types: Root {value u32 +0, list BlobArray<u32> +4, groups BlobArray<Group> +12}; Group {vals BlobArray<u32> +0}.
TType u32, list_u32, group, list_group, root;
uintptr_t rec(const TType& t) { return reinterpret_cast<uintptr_t>(&t); }
void types() {
    u32.name = u32.brief = "uint32", u32.kind = K_U32, u32.size = 4;
    list_u32.name = list_u32.brief = "keen::BlobArray<uint32>", list_u32.kind = K_BLOBARRAY, list_u32.size = 8, list_u32.inner_rec = rec(u32);
    group.name = "keen::Group", group.brief = "Group", group.kind = K_STRUCT, group.size = 8;
    group.fields = {{"vals", rec(list_u32), 0}};
    list_group.name = list_group.brief = "keen::BlobArray<keen::Group>", list_group.kind = K_BLOBARRAY, list_group.size = 8, list_group.inner_rec = rec(group);
    root.name = "keen::Root", root.brief = "Root", root.kind = K_STRUCT, root.size = 20;
    root.fields = {{"value", rec(u32), 0}, {"list", rec(list_u32), 4}, {"groups", rec(list_group), 12}};
}

// A resource's bytes as the game keeps them: the struct, then each list's data behind it (self-relative offsets).
struct Bytes {
    std::vector<uint8_t> b = std::vector<uint8_t>(4096, 0);
    size_t end = 64;
    uintptr_t at(size_t off) { return reinterpret_cast<uintptr_t>(b.data()) + off; }
    void u32_at(size_t off, uint32_t v) { std::memcpy(b.data() + off, &v, 4); }
    // a BlobArray at `field` holding n elements of `stride` bytes; `rel0`: an empty list with a zero offset
    size_t list(size_t field, uint32_t n, uint32_t stride, bool rel0 = false) {
        const size_t data = end;
        end += n * stride + 8;
        const int32_t rel = rel0 ? 0 : static_cast<int32_t>(data - field);
        std::memcpy(b.data() + field, &rel, 4);
        u32_at(field + 4, n);
        return data;
    }
};

std::vector<Resolved> rs;
std::string err;
bool missing = false;
bool walk(Bytes& m, const std::string& path) {
    rs.clear();
    err.clear();
    missing = false;
    return resolve(Root::Plain, Node{&root, m.at(0)}, path, &rs, &err, &missing);
}

void lists() {
    Bytes m;
    const size_t d = m.list(4, 3, 4);
    for (uint32_t i = 0; i < 3; ++i) m.u32_at(d + 4 * i, 10 + i);
    check(walk(m, "list[*]") && rs.size() == 3 && rs[2].node.addr == m.at(d + 8) && rs[2].path == "list[2]", "[*] names every element");
    check(walk(m, "list[1]") && rs.size() == 1 && rs[0].node.addr == m.at(d + 4), "[i] names one element");
    check(!walk(m, "list[3]") && rs.empty() && err.find("the list has 3 entries") != std::string::npos, "an index past the list is an error");
    check(!walk(m, "nope[*]") && missing && !err.empty(), "a member the struct has not got is an error");
}

void empty_lists() {
    for (const bool rel0 : {true, false}) {
        Bytes m;
        m.list(4, 0, 4, rel0);
        m.list(12, 0, 8, rel0);
        const std::string how = rel0 ? " (no data offset)" : " (a data offset, no elements)";
        check(walk(m, "list[*]") && rs.empty() && err.empty() && !missing, "[*] over an empty list names nothing and is no error" + how);
        check(walk(m, "groups[*].vals[*]") && rs.empty() && err.empty(), "nor is a path on below it" + how);
        check(!walk(m, "list[0]") && err.find("the list has 0 entries") != std::string::npos, "[0] of an empty list is an error" + how);
        check(walk(m, "value") && rs.size() == 1, "the rest of the resource is unchanged" + how);
    }
}

void nested() {
    Bytes m;
    const size_t g = m.list(12, 3, 8);
    m.list(g, 0, 4);                        // groups[0].vals: empty
    const size_t v1 = m.list(g + 8, 2, 4);  // groups[1].vals: two
    m.u32_at(v1, 7), m.u32_at(v1 + 4, 8);
    m.list(g + 16, 0, 4, true);             // groups[2].vals: empty, no data offset
    check(walk(m, "groups[*].vals[*]") && rs.size() == 2 && rs[0].path == "groups[1].vals[0]" && rs[1].node.addr == m.at(v1 + 4),
          "the elements there are, whatever lists on the way are empty");
    check(!walk(m, "groups[*].vals[*].x") && rs.empty() && err.find("has no field x") != std::string::npos,
          "an element without the member is an error, though other lists on the way are empty");
    check(!walk(m, "groups[*].nope[*]") && err.find("has no field nope") != std::string::npos, "and so is a member no element has");
    Bytes e;
    const size_t ge = e.list(12, 2, 8);
    e.list(ge, 0, 4), e.list(ge + 8, 0, 4);
    check(walk(e, "groups[*].vals[*]") && rs.empty() && err.empty(), "every list on the way empty: nothing named, no error");
}
}  // namespace

int main() {
    types();
    int failed = 0;
    const std::pair<const char*, void (*)()> groups[] = {{"lists", lists}, {"empty_lists", empty_lists}, {"nested", nested}};
    for (const auto& [name, fn] : groups) {
        try {
            fn();
        } catch (const std::exception& e) {
            ++failed;
            std::printf("walk-test: FAIL %s: %s (after %d checks)\n", name, e.what(), checks);
        }
    }
    if (failed) {
        std::printf("walk-test: %d group(s) failed, %d checks passed\n", failed, checks);
        return 1;
    }
    std::printf("walk-test: %d checks passed\n", checks);
}
