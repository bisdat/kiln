// SPDX-License-Identifier: MIT
// kiln.markers' icon catalog on synthetic resources (no game bytes): chapter 04 §6 M13, the client half (the registry
// and texture readers with their bounds checks, keys, aliases, the digest, resolve, warm-up, the fast-travel refusal and
// the build-pair table), and SHA-256 against the FIPS 180-4 examples.
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "catalog.h"
#include "check.h"
#include "sha256.h"
#include "store.h"

using namespace kmk;

namespace {

std::string hex(const std::array<uint8_t, 32>& d) {
    static const char x[] = "0123456789abcdef";
    std::string s;
    for (uint8_t b : d) s += x[b >> 4], s += x[b & 15];
    return s;
}

std::string sha(const std::string& s) {
    Sha256 h;
    h.update(s.data(), s.size());
    return hex(h.finish());
}

void put32(std::vector<uint8_t>& b, size_t at, uint32_t v) {
    for (int i = 0; i < 4; ++i) b[at + i] = static_cast<uint8_t>(v >> (8 * i));
}

// A made-up marker type (not game data).
struct Type {
    uint32_t id;
    uint8_t image[16];
    bool travel, custom, fog, label, range;
    std::string name;
};

Type type(uint32_t id, uint8_t seed, std::string name, bool travel = false, bool custom = false) {
    Type t{id, {}, travel, custom, true, false, true, std::move(name)};
    if (seed)
        for (int i = 0; i < 16; ++i) t.image[i] = static_cast<uint8_t>(seed + i);
    return t;
}

// The registry layout of catalog.h: 24-byte header (entries, keys, values BlobArrays), then the keys, then the values.
std::vector<uint8_t> registry(const std::vector<Type>& types) {
    const size_t keys = 24, values = keys + 4 * types.size();
    std::vector<uint8_t> b(values + 80 * types.size(), 0);
    put32(b, 0, 0);  // entries: none (the hash buckets are not read)
    put32(b, 4, 0);
    put32(b, 8, static_cast<uint32_t>(keys - 8));
    put32(b, 12, static_cast<uint32_t>(types.size()));
    put32(b, 16, static_cast<uint32_t>(values - 16));
    put32(b, 20, static_cast<uint32_t>(types.size()));
    for (size_t i = 0; i < types.size(); ++i) {
        const Type& t = types[i];
        put32(b, keys + 4 * i, t.id);
        uint8_t* v = b.data() + values + 80 * i;
        put32(b, values + 80 * i + 0x10, t.id);
        v[0x18] = t.travel;
        v[0x19] = t.custom;
        std::memcpy(v + 0x1c, t.image, 16);
        v[0x40] = t.fog;
        v[0x41] = t.label;
        v[0x42] = t.range;
    }
    return b;
}

std::vector<uint8_t> texture(const std::string& name, bool nul = false) {
    std::vector<uint8_t> b(56 + name.size() + (nul ? 1 : 0), 0);
    put32(b, 0x20, 56 - 0x20);
    put32(b, 0x24, static_cast<uint32_t>(name.size() + (nul ? 1 : 0)));
    std::memcpy(b.data() + 56, name.data(), name.size());
    return b;
}

struct Resources {
    std::map<std::string, std::vector<uint8_t>> by_key;
    static std::string key(const uint8_t g[16], uint32_t type) { return std::string(reinterpret_cast<const char*>(g), 16) + std::to_string(type); }
    void add(const uint8_t g[16], uint32_t type, std::vector<uint8_t> b) { by_key[key(g, type)] = std::move(b); }
    static bool read(const uint8_t g[16], uint32_t type, size_t limit, std::vector<uint8_t>* out, void* ctx) {
        auto& m = static_cast<Resources*>(ctx)->by_key;
        auto it = m.find(key(g, type));
        if (it == m.end() || it->second.size() > limit) return false;
        *out = it->second;
        return true;
    }
};

std::vector<Type> sample() {
    return {type(0xffe5e444u, 0x10, "mapmarker_Well_Plant"), type(0x0e6558ffu, 0x30, "icon_combat", false, true),
            type(0x1095684eu, 0x50, "mapmarker_fastTravel", true), type(0x41a86870u, 0, "no_image_type"),
            type(0x5af11f69u, 0x70, "MapMarker_Upper")};
}

Resources resources(const std::vector<Type>& types) {
    Resources r;
    r.add(kRegistryGuid, kRegistryType, registry(types));
    for (const Type& t : types) r.add(t.image, kUiTextureType, texture(t.name));
    return r;
}

void sha256_known_answers() {
    kmk_test::begin();
    CHECK(sha("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(sha("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(sha("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    Sha256 h;  // a million 'a', fed in uneven pieces
    std::string a(997, 'a');
    size_t left = 1000000;
    while (left) {
        const size_t n = left < a.size() ? left : a.size();
        h.update(a.data(), n);
        left -= n;
    }
    CHECK(hex(h.finish()) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    kmk_test::end("SHA-256 FIPS 180-4 examples");
}

void reads_rows_keys_aliases() {
    kmk_test::begin();
    auto types = sample();
    Resources res = resources(types);
    std::string why;
    auto c = read_catalog(&Resources::read, &res, &why);
    CHECK(c != nullptr);
    if (!c) {
        std::printf("  why: %s\n", why.c_str());
        kmk_test::end("catalog rows");
        return;
    }
    CHECK_EQ(c->rows.size(), 4u);  // the null-image type is not a row
    CHECK_EQ(c->map_rows(), 3u);   // the fast-travel row is listed without the map
    bool sorted = true;
    for (size_t i = 1; i < c->rows.size(); ++i) sorted &= c->rows[i - 1].id < c->rows[i].id;
    CHECK(sorted);
    const IconRow* well = c->by_id(0xffe5e444u);
    CHECK(well && well->alias == "well_plant" && well->key == "type:ffe5e444" && well->name == "mapmarker_Well_Plant");
    CHECK(well && well->surfaces == KILN_MARKER_MAP && well->flags == (KILN_MARKER_ICON_IMAGE | KILN_MARKER_ICON_ABOVE_FOG | KILN_MARKER_ICON_SHOW_RANGE));
    const IconRow* travel = c->by_id(0x1095684eu);
    CHECK(travel && travel->surfaces == 0 && (travel->flags & KILN_MARKER_ICON_FAST_TRAVEL) && travel->alias == "fasttravel");
    const IconRow* combat = c->by_id(0x0e6558ffu);
    CHECK(combat && combat->alias == "icon_combat" && (combat->flags & KILN_MARKER_ICON_CUSTOM));
    const IconRow* upper = c->by_id(0x5af11f69u);
    CHECK(upper && upper->alias == "mapmarker_upper");  // the prefix is removed only as written ("mapmarker_"), then lower-cased
    CHECK(c->by_id(0x41a86870u) == nullptr);
    // the digest, assembled independently: sorted id, image, flags, name length, name
    std::string bytes;
    for (const IconRow& r : c->rows) {
        for (int i = 0; i < 4; ++i) bytes += static_cast<char>(r.id >> (8 * i));
        bytes.append(reinterpret_cast<const char*>(r.image), 16);
        for (int i = 0; i < 4; ++i) bytes += static_cast<char>(r.flags >> (8 * i));
        bytes += static_cast<char>(r.name.size() & 0xff);
        bytes += static_cast<char>(r.name.size() >> 8);
        bytes += r.name;
    }
    CHECK(c->digest_hex() == sha(bytes));
    KilnMarkerIcon icon{};
    icon.size = sizeof icon;
    fill_icon(*well, &icon);
    CHECK(icon.id == 0xffe5e444u && std::strcmp(icon.key, "type:ffe5e444") == 0 && std::strcmp(icon.alias, "well_plant") == 0);
    CHECK(std::strcmp(icon.texture_name, "mapmarker_Well_Plant") == 0 && icon.texture_guid[0] == 0x10 && icon.size == sizeof icon);
    kmk_test::end("M13 client half: image-backed rows, keys, aliases, fast travel without the map, the digest");
}

void resolves() {
    kmk_test::begin();
    auto types = sample();
    types.push_back(type(0x00000007u, 0x90, "icon_combat"));  // a second type with the same alias
    Resources res = resources(types);
    std::string why;
    auto c = read_catalog(&Resources::read, &res, &why);
    CHECK(c != nullptr);
    if (!c) return kmk_test::end("resolve");
    const IconRow* row = nullptr;
    uint32_t n = 0;
    CHECK(resolve(*c, "well_plant", &row, &n) == Resolved::Ok && row && row->id == 0xffe5e444u);
    CHECK(resolve(*c, "WELL_Plant", &row, &n) == Resolved::Ok);  // input is ASCII lower-cased
    CHECK(resolve(*c, "type:FFE5E444", &row, &n) == Resolved::Ok && row->id == 0xffe5e444u);
    CHECK(resolve(*c, "type:deadbeef", &row, &n) == Resolved::NotFound && !row);
    CHECK(resolve(*c, "mapmarker_well_plant", &row, &n) == Resolved::NotFound);  // no prefix stripping of the input
    CHECK(resolve(*c, "well", &row, &n) == Resolved::NotFound);                  // no substring match
    CHECK(resolve(*c, "icon_combat", &row, &n) == Resolved::Ambiguous && n == 2 && !row);
    CHECK(resolve(*c, "type:0e6558ff", &row, &n) == Resolved::Ok);  // the canonical key still names it
    for (const char* bad : {"", " well_plant", "well_plant ", "type:12", "type:ffe5e44g", "type:ffe5e4444", "tab\there"})
        CHECK(resolve(*c, bad, &row, &n) == Resolved::BadSyntax);
    CHECK(resolve(*c, std::string("w\xc3\xa9ll"), &row, &n) == Resolved::BadSyntax);
    CHECK(resolve(*c, std::string(129, 'a'), &row, &n) == Resolved::BadSyntax);
    CHECK(resolve(*c, std::string(128, 'a'), &row, &n) == Resolved::NotFound);
    kmk_test::end("resolve: canonical key or exact alias, ambiguity, syntax");
}

void refuses_malformed() {
    kmk_test::begin();
    std::string why;
    std::vector<RegistryRow> rows;
    auto reg = registry(sample());
    CHECK(parse_registry(reg.data(), reg.size(), &rows, &why) && rows.size() == 4);
    CHECK(!parse_registry(reg.data(), 23, &rows, &why));  // shorter than the header
    auto bad = reg;
    put32(bad, 8, 0x7ffffff0);  // keys far outside
    CHECK(!parse_registry(bad.data(), bad.size(), &rows, &why));
    bad = reg;
    put32(bad, 8, static_cast<uint32_t>(-100));  // keys before the start
    CHECK(!parse_registry(bad.data(), bad.size(), &rows, &why));
    bad = reg;
    put32(bad, 20, 0x40000001);  // a count whose size overflows 32 bits
    CHECK(!parse_registry(bad.data(), bad.size(), &rows, &why));
    bad = reg;
    put32(bad, 12, 4);  // fewer keys than values
    CHECK(!parse_registry(bad.data(), bad.size(), &rows, &why));
    CHECK(!parse_registry(reg.data(), reg.size() - 1, &rows, &why));  // the last value cut short
    bad = reg;
    put32(bad, 24 + 4 * 5 + 0x10, 0x12345678);  // the first value's markerId is not its key
    CHECK(!parse_registry(bad.data(), bad.size(), &rows, &why));
    CHECK(why.find("layout") != std::string::npos);
    auto twice = sample();
    twice[4].id = twice[0].id;
    bad = registry(twice);
    CHECK(!parse_registry(bad.data(), bad.size(), &rows, &why));
    // textures
    std::string name;
    auto tex = texture("mapmarker_x");
    CHECK(parse_texture_name(tex.data(), tex.size(), &name, &why) && name == "mapmarker_x");
    auto nul = texture("icon_y", true);
    CHECK(parse_texture_name(nul.data(), nul.size(), &name, &why) && name == "icon_y");
    CHECK(!parse_texture_name(tex.data(), 55, &name, &why));
    auto out = tex;
    put32(out, 0x24, 200);  // longer than the buffer
    CHECK(!parse_texture_name(out.data(), out.size(), &name, &why));
    auto ctl = texture("bad\x01name");
    CHECK(!parse_texture_name(ctl.data(), ctl.size(), &name, &why));
    auto lng = texture(std::string(129, 'n'));
    CHECK(!parse_texture_name(lng.data(), lng.size(), &name, &why));
    auto empty = texture("");
    CHECK(!parse_texture_name(empty.data(), empty.size(), &name, &why));
    // a missing texture: no catalog at all, not a partial one
    auto types = sample();
    Resources res = resources(types);
    res.by_key.erase(Resources::key(types[1].image, kUiTextureType));
    CHECK(read_catalog(&Resources::read, &res, &why) == nullptr);
    Resources none;
    CHECK(read_catalog(&Resources::read, &none, &why) == nullptr);
    kmk_test::end("bounds: every offset, count, multiplication and length is checked before a read");
}

void holder_and_store() {
    kmk_test::begin();
    CatalogHolder holder;
    CHECK(CatalogHolder::check(0xffe5e444u, &holder) == IconAnswer::NotReady);
    CHECK_EQ(holder.generation(), 0u);
    auto types = sample();
    Resources res = resources(types);
    std::string why;
    auto c = read_catalog(&Resources::read, &res, &why);
    // a store with the map row on, asking the holder
    Arena arena;
    Switches sw;
    sw.local = true;
    sw.map = true;
    sw.world = true;
    Store store(arena, sw);
    store.set_icon_check(&CatalogHolder::check, &holder);
    store.world_started(KILN_WORLD_CLIENT);
    CHECK_EQ(store.bind(1, KilnStr{"cat", 3}), KILN_OK);
    KilnMarkerSpec s{};
    s.size = sizeof s;
    s.surfaces = KILN_MARKER_MAP;
    s.key = KilnStr{"well", 4};
    s.position = {100, 50, 100};
    s.icon_type = 0xffe5e444u;
    KilnMarkerResult r{};
    r.size = sizeof r;
    CHECK_EQ(store.set_local(1, &s, &r), KILN_OK);
    CHECK_EQ(r.status, KILN_MARKER_CATALOG_NOT_READY);  // warm-up: refused, not an error
    // a world-only label needs no catalog
    KilnMarkerSpec plate = s;
    plate.surfaces = KILN_MARKER_WORLD;
    plate.icon_type = 0;
    plate.key = KilnStr{"plate", 5};
    CHECK_EQ(store.set_local(1, &plate, &r), KILN_OK);
    CHECK_EQ(r.status, KILN_MARKER_ACCEPTED);
    CHECK(holder.publish(c, true, ""));
    CHECK(!holder.publish(c, true, ""));  // the first catalog stays
    CHECK_EQ(holder.generation(), 1u);
    CHECK(holder.map_verified());
    CHECK_EQ(store.set_local(1, &s, &r), KILN_OK);
    CHECK_EQ(r.status, KILN_MARKER_ACCEPTED);
    s.icon_type = 0x1095684eu;  // fast travel: refused by the setter
    CHECK_EQ(store.set_local(1, &s, &r), KILN_ERR_INVALID_ARGUMENT);
    s.icon_type = 0x41a86870u;  // a type without an image is not in the catalog
    CHECK_EQ(store.set_local(1, &s, &r), KILN_ERR_INVALID_ARGUMENT);
    holder.latch_changed("the registry changed");
    CHECK(!holder.map_verified() && holder.changed() && holder.map_reason() == "the registry changed");
    // the verified pair of the dossier's builds
    const BuildPair* p = pair_of_client("5d9ef2f140b8ed49e30dbee655841b89d08baec20f799a1817e171a18a850c77");
    CHECK(p && std::strcmp(p->catalog_sha256, "a9d9390934bd614784fa9f82b2bed4eba6a9bad08cb78f262deb5810fda7cac9") == 0);
    CHECK(pair_of_server("f0d824be7fcd48ebfe2458ff5c28da26060c2074e3ec1fcac587e5a1d508aeb2") == p);
    CHECK(pair_of_client("f0d824be7fcd48ebfe2458ff5c28da26060c2074e3ec1fcac587e5a1d508aeb2") == nullptr);
    CHECK(pair_of_client("") == nullptr);
    kmk_test::end("M13: warm-up refusal, world-only without a catalog, fast travel refused, the build pair");
}

}  // namespace

int main() {
    sha256_known_answers();
    reads_rows_keys_aliases();
    resolves();
    refuses_malformed();
    holder_and_store();
    return kmk_test::finish("catalog_test");
}
