// SPDX-License-Identifier: MIT
// kiln#130: an item tag reference (ItemInfo.tags[i].tag, an ItemTagReference) through gd::parse_leaf, the leaf encoder of
// every itemdata rule and data-pack item path. The game data cooks no keen::ItemTag resource: the tags it knows are the
// item registry's list (ItemRegistryResource.itemTags {tagGuid, labelId}). So a tag the list holds is accepted, any other
// GUID (a root item's identity among them) is refused by name, and the list is read through the reflection only. Hand-made
// types and bytes on the host; no game.
#include "../gamedata_walk.cpp"

#include <cstdarg>
#include <cstdio>
#include <stdexcept>

namespace kc {
const KilnApi* g_api = nullptr;
void logf(int, const char*, ...) {}
bool rd(uintptr_t address, void* out, size_t n) {
    if (!address) return false;
    std::memcpy(out, reinterpret_cast<const void*>(address), n);
    return true;
}
std::string read_blob_string(uintptr_t) { return {}; }
std::string data_dir() { return {}; }
bool path_exists(const std::string&) { return false; }
std::string lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}
bool guid_parse(const std::string& s, uint8_t out[16]) {
    unsigned a, b, c, d[8];
    if (s.size() != 36 || std::sscanf(s.c_str(), "%8x-%4x-%4x-%2x%2x-%2x%2x%2x%2x%2x%2x", &a, &b, &c, &d[0], &d[1], &d[2], &d[3], &d[4], &d[5], &d[6], &d[7]) != 11) return false;
    const uint32_t aa = a;
    const uint16_t bb = static_cast<uint16_t>(b), cc = static_cast<uint16_t>(c);
    std::memcpy(out, &aa, 4), std::memcpy(out + 4, &bb, 2), std::memcpy(out + 6, &cc, 2);
    for (int i = 0; i < 8; ++i) out[8 + i] = static_cast<uint8_t>(d[i]);
    return true;
}
std::string guid_text(const uint8_t g[16]) {
    char t[40];
    std::snprintf(t, sizeof t, "%02x%02x%02x%02x", g[3], g[2], g[1], g[0]);
    return t;
}
uint32_t fnv1a(const void* data, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; ++i) h = (h ^ static_cast<const uint8_t*>(data)[i]) * 16777619u;
    return h;
}
// the scalar half of parse_leaf: not reached by a reference
const Field* Schema::find(const std::string&) const { return nullptr; }
KilnContentValue decode(const Field&, const uint8_t*) { return {}; }
bool encode(const Field&, const KilnContentValue&, uint8_t*, std::string* e) { if (e) *e = "scalar"; return false; }
double as_double(const KilnContentValue&) { return 0; }
const Entry* Registry::lookup(const std::string&) const { return nullptr; }
Registry& reg(uint32_t) { static Registry r; return r; }
namespace gd {
const TType* TType::resolved() const { auto* t = this; for (int i = 0; i < 16 && t && t->kind == K_TYPEDEF; ++i) t = t->inner(); return t; }
const TType* TType::inner() const { return reinterpret_cast<const TType*>(inner_rec); }
const TType* TField::type() const { return reinterpret_cast<const TType*>(type_rec); }
const TField* TType::field(const std::string& n) const {
    for (const TField& f : fields)
        if (f.name == n) return &f;
    return nullptr;
}
const GEntry* GRegistry::get(uint32_t) const { return nullptr; }
const GEntry* GRegistry::lookup(const std::string&) const { return nullptr; }
GRegistry& greg(uint32_t) { static GRegistry r; return r; }
bool buff_guid(uint32_t, uint8_t[16]) { return false; }
}  // namespace gd
}  // namespace kc

namespace {
using namespace kc::gd;
int checks = 0;
void check(bool ok, const std::string& why) {
    if (!ok) throw std::runtime_error(why);
    ++checks;
}
uintptr_t rec(const TType& t) { return reinterpret_cast<uintptr_t>(&t); }
// The client's reflection (26430625): ItemTagReference -> ObjectReference<ItemTag>; ItemRegistryResource.itemTags at +8,
// a BlobArray of 20-byte ItemTagInfo {tagGuid +0, labelId +16}.
TType guid_t, u32, item_tag, obj_ref, tag_ref, info, info_list, registry;
std::vector<uint8_t> resource(4096, 0);
bool registry_listed = true;
const uint8_t registry_key[16] = {0x72, 0xab, 0x1c, 0xb6};
void types() {
    guid_t.name = "keen::Guid", guid_t.kind = K_GUID, guid_t.size = 16;
    u32.name = "keen::LocaTagId", u32.kind = K_U32, u32.size = 4;
    item_tag.name = "keen::ItemTag", item_tag.kind = K_STRUCT, item_tag.size = 16;
    obj_ref.name = "keen::ObjectReference<keen::ItemTag>", obj_ref.kind = K_REF, obj_ref.size = 16, obj_ref.inner_rec = rec(item_tag);
    tag_ref.name = "keen::ItemTagReference", tag_ref.kind = K_TYPEDEF, tag_ref.size = 16, tag_ref.inner_rec = rec(obj_ref);
    info.name = "keen::ItemTagInfo", info.kind = K_STRUCT, info.size = 20, info.fields = {{"tagGuid", rec(guid_t), 0}, {"labelId", rec(u32), 16}};
    info_list.name = "keen::BlobArray<keen::ItemTagInfo>", info_list.kind = K_BLOBARRAY, info_list.size = 8, info_list.inner_rec = rec(info);
    registry.name = "keen::ItemRegistryResource", registry.kind = K_STRUCT, registry.size = 72, registry.hash = 0x72ab1cb6u;
    registry.fields = {{"itemRefs", rec(info_list), 0}, {"itemTags", rec(info_list), 8}};
}
// itemTags: three tags (vanilla Overgrowth's brush tag among them), labels after each GUID
void lay_out(uint32_t n) {
    std::fill(resource.begin(), resource.end(), 0);
    const int32_t rel = 64 - 8;
    std::memcpy(resource.data() + 8, &rel, 4);
    std::memcpy(resource.data() + 12, &n, 4);
    const char* tags[] = {"dbb7fc02-04c8-43ad-acc3-0fa22233d71d", "ee3b266d-83e7-4239-8bdd-8e58fe12e9d1", "08aea6cf-e0e8-4d6c-8a29-a21f4693e006"};
    for (uint32_t i = 0; i < n && i < 3; ++i) {
        kc::guid_parse(tags[i], resource.data() + 64 + 20 * i);
        const uint32_t label = 0x69691905u;
        std::memcpy(resource.data() + 64 + 20 * i + 16, &label, 4);
    }
}
}  // namespace

namespace kc::gd {
const TType* type_hash(uint32_t h) { return h == registry.hash ? &registry : nullptr; }
const TType* type_named(const std::string& n) {
    for (const TType* t : {&registry, &tag_ref, &obj_ref, &info, &item_tag})
        if (t->name == n) return t;
    return nullptr;
}
bool res_list(uint32_t type, std::vector<std::array<uint8_t, 16>>* out) {
    out->clear();
    if (type != registry.hash || !registry_listed) return true;
    std::array<uint8_t, 16> k{};
    std::memcpy(k.data(), registry_key, 16);
    out->push_back(k);
    return true;
}
uintptr_t res_data(const uint8_t g[16], uint32_t type, uint64_t* size) {
    if (type != registry.hash || std::memcmp(g, registry_key, 16)) return 0;  // no keen::ItemTag resource exists
    if (size) *size = resource.size();
    return reinterpret_cast<uintptr_t>(resource.data());
}
}  // namespace kc::gd

namespace {
std::string err;
bool encode(const std::string& value, uint8_t out[16]) {
    kc::Field f;
    f.path = "tags[0].tag", f.type_name = "keen::ItemTagReference", f.bytes = 16, f.vt = KILN_CV_GUID, f.ref = true;
    const uint8_t original[16] = {};
    bool reset = false;
    err.clear();
    return parse_leaf(f, kc::gjson(value), original, out, &reset, &err) && !reset;
}
void tags() {
    uint8_t out[16], want[16];
    lay_out(3);
    check(encode("08aea6cf-e0e8-4d6c-8a29-a21f4693e006", out) && kc::guid_parse("08aea6cf-e0e8-4d6c-8a29-a21f4693e006", want) && !std::memcmp(out, want, 16),
          "a tag the item registry lists is accepted and written as its GUID (it was refused as an unloaded keen::ItemTag resource)");
    check(encode("dbb7fc02-04c8-43ad-acc3-0fa22233d71d", out), "and so is the first one");
    check(!encode("c585cd2f-1111-4222-8333-444455556666", out) && err.find("is not an item tag the game knows") != std::string::npos,
          "a root item's GUID is no tag of the registry: refused, saying why");
    check(encode("none", out) && !std::any_of(out, out + 16, [](uint8_t b) { return b != 0; }), "none clears the reference");
    lay_out(2);
    check(!encode("08aea6cf-e0e8-4d6c-8a29-a21f4693e006", out), "only the entries the list counts: a third GUID past its count is not read");
    registry_listed = false;
    lay_out(3);
    check(!encode("08aea6cf-e0e8-4d6c-8a29-a21f4693e006", out), "no item registry resource: nothing to check against, refused");
    registry_listed = true;
    registry.fields = {{"itemRefs", rec(info_list), 0}};
    check(!encode("08aea6cf-e0e8-4d6c-8a29-a21f4693e006", out), "a build whose registry has no itemTags field: refused, no offset guessed");
    registry.fields = {{"itemRefs", rec(info_list), 0}, {"itemTags", rec(info_list), 8}};
}
}  // namespace

int main() {
    types();
    try {
        tags();
        std::printf("itemtag-test: %d checks passed, 0 failed\n", checks);
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "itemtag-test: FAIL: %s\n", e.what());
        return 1;
    }
}
