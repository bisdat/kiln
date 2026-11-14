// SPDX-License-Identifier: MIT
#include "catalog.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "sha256.h"
#include "text.h"

namespace kmk {

namespace {

uint32_t u32(const uint8_t* p) { return uint32_t{p[0]} | uint32_t{p[1]} << 8 | uint32_t{p[2]} << 16 | uint32_t{p[3]} << 24; }
int32_t i32(const uint8_t* p) { return static_cast<int32_t>(u32(p)); }

// A BlobArray (or BlobString) header {i32 offset from the header, u32 count} at `at` of a buffer of n bytes: the range
// [start, start + count * stride) it points at, checked against the buffer with 64-bit arithmetic.
bool blob(const uint8_t* p, size_t n, size_t at, size_t stride, size_t max_count, size_t* start, uint32_t* count) {
    if (at > n || n - at < 8) return false;
    const int64_t s = static_cast<int64_t>(at) + i32(p + at);
    const uint64_t c = u32(p + at + 4);
    if (c > max_count) return false;
    if (s < 0 || static_cast<uint64_t>(s) > n) return false;
    if (c * stride > n - static_cast<uint64_t>(s)) return false;
    *start = static_cast<size_t>(s);
    *count = static_cast<uint32_t>(c);
    return true;
}

bool zero16(const uint8_t* p) {
    for (int i = 0; i < 16; ++i)
        if (p[i]) return false;
    return true;
}

}  // namespace

// ---- parsing ------------------------------------------------------------------------------------------------------------

bool parse_registry(const uint8_t* p, size_t n, std::vector<RegistryRow>* out, std::string* why) {
    out->clear();
    if (!p || n < 24) return *why = "the marker registry is shorter than its header", false;
    size_t keys = 0, values = 0;
    uint32_t nkeys = 0, nvalues = 0;
    if (!blob(p, n, 8, 4, kMaxRegistryRows, &keys, &nkeys)) return *why = "the marker registry's keys point outside the resource", false;
    if (!blob(p, n, 16, 80, kMaxRegistryRows, &values, &nvalues)) return *why = "the marker registry's values point outside the resource", false;
    if (nkeys != nvalues) return *why = "the marker registry has not as many keys as values", false;
    for (uint32_t i = 0; i < nkeys; ++i) {
        const uint32_t id = u32(p + keys + size_t{i} * 4);
        const uint8_t* v = p + values + size_t{i} * 80;
        if (u32(v + 0x10) != id) return *why = "a marker type's id is not its key (the registry's layout changed)", false;
        if (zero16(v + 0x1c)) continue;  // no icon image: not a catalog row
        RegistryRow r;
        r.id = id;
        auto bit = [](uint8_t set, uint32_t flag) { return set ? flag : 0u; };
        r.flags = KILN_MARKER_ICON_IMAGE | bit(v[0x41], KILN_MARKER_ICON_SHOW_LABEL) | bit(v[0x42], KILN_MARKER_ICON_SHOW_RANGE) |
                  bit(v[0x40], KILN_MARKER_ICON_ABOVE_FOG) | bit(v[0x19], KILN_MARKER_ICON_CUSTOM) | bit(v[0x18], KILN_MARKER_ICON_FAST_TRAVEL);
        std::memcpy(r.image, v + 0x1c, 16);
        for (const RegistryRow& o : *out)
            if (o.id == id) return *why = "the marker registry names a type id twice", false;
        out->push_back(r);
    }
    return true;
}

bool parse_texture_name(const uint8_t* p, size_t n, std::string* name, std::string* why) {
    if (!p || n < 56) return *why = "a UiTextureResource is shorter than 56 bytes", false;
    size_t start = 0;
    uint32_t len = 0;
    if (!blob(p, n, 0x20, 1, kNameMax + 1, &start, &len)) return *why = "a texture's debug name points outside its resource or is too long", false;
    if (len && p[start + len - 1] == 0) --len;  // a C string's terminator
    if (!len) return *why = "a texture has no debug name", false;
    if (len > kNameMax) return *why = "a texture's debug name is longer than 128 bytes", false;
    if (text_problem(reinterpret_cast<const char*>(p + start), len, kNameMax)) return *why = "a texture's debug name is not plain UTF-8 text", false;
    name->assign(reinterpret_cast<const char*>(p + start), len);
    return true;
}

std::string canonical_key(uint32_t id) {
    char k[16];
    std::snprintf(k, sizeof k, "type:%08x", id);
    return k;
}

std::string alias_of(std::string_view name) {
    constexpr std::string_view prefix = "mapmarker_";
    if (name.substr(0, prefix.size()) == prefix) name.remove_prefix(prefix.size());
    std::string a(name);
    for (char& c : a)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return a;
}

std::array<uint8_t, 32> digest_of(const std::vector<IconRow>& rows) {
    Sha256 h;
    for (const IconRow& r : rows) {
        uint8_t head[4];
        for (int i = 0; i < 4; ++i) head[i] = static_cast<uint8_t>(r.id >> (8 * i));
        h.update(head, 4);
        h.update(r.image, 16);
        for (int i = 0; i < 4; ++i) head[i] = static_cast<uint8_t>(r.flags >> (8 * i));
        h.update(head, 4);
        const uint8_t len[2] = {static_cast<uint8_t>(r.name.size()), static_cast<uint8_t>(r.name.size() >> 8)};
        h.update(len, 2);
        h.update(r.name.data(), r.name.size());
    }
    return h.finish();
}

std::shared_ptr<const Catalog> read_catalog(ReadResource read, void* ctx, std::string* why) {
    std::vector<uint8_t> bytes;
    if (!read(kRegistryGuid, kRegistryType, kMaxRegistryBytes, &bytes, ctx)) {
        *why = "the game's marker registry resource could not be read";
        return nullptr;
    }
    std::vector<RegistryRow> raw;
    if (!parse_registry(bytes.data(), bytes.size(), &raw, why)) return nullptr;
    auto c = std::make_shared<Catalog>();
    c->rows.reserve(raw.size());
    for (const RegistryRow& r : raw) {
        std::vector<uint8_t> tex;
        if (!read(r.image, kUiTextureType, kMaxTextureBytes, &tex, ctx)) {
            char text[96];
            std::snprintf(text, sizeof text, "the icon texture of marker type 0x%08x could not be read", r.id);
            *why = text;
            return nullptr;
        }
        IconRow row;
        row.id = r.id;
        row.flags = r.flags;
        row.surfaces = (r.flags & KILN_MARKER_ICON_FAST_TRAVEL) ? 0u : static_cast<uint32_t>(KILN_MARKER_MAP);
        std::memcpy(row.image, r.image, 16);
        if (!parse_texture_name(tex.data(), tex.size(), &row.name, why)) return nullptr;
        row.alias = alias_of(row.name);
        row.key = canonical_key(row.id);
        c->rows.push_back(std::move(row));
    }
    std::sort(c->rows.begin(), c->rows.end(), [](const IconRow& a, const IconRow& b) { return a.id < b.id; });
    c->digest = digest_of(c->rows);
    return c;
}

// ---- the catalog --------------------------------------------------------------------------------------------------------

uint32_t Catalog::map_rows() const {
    uint32_t n = 0;
    for (const IconRow& r : rows) n += (r.surfaces & KILN_MARKER_MAP) != 0;
    return n;
}

std::string Catalog::digest_hex() const {
    static const char hex[] = "0123456789abcdef";
    std::string s(64, '0');
    for (size_t i = 0; i < 32; ++i) {
        s[2 * i] = hex[digest[i] >> 4];
        s[2 * i + 1] = hex[digest[i] & 15];
    }
    return s;
}

const IconRow* Catalog::by_id(uint32_t id) const {
    auto it = std::lower_bound(rows.begin(), rows.end(), id, [](const IconRow& r, uint32_t v) { return r.id < v; });
    return it != rows.end() && it->id == id ? &*it : nullptr;
}

Resolved resolve(const Catalog& c, std::string_view name, const IconRow** out, uint32_t* matches) {
    *out = nullptr;
    *matches = 0;
    if (name.empty() || name.size() > kNameMax || name.front() == ' ' || name.back() == ' ') return Resolved::BadSyntax;
    std::string low(name);
    for (char& ch : low) {
        const unsigned char u = static_cast<unsigned char>(ch);
        if (u < 0x20 || u > 0x7e) return Resolved::BadSyntax;
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    }
    constexpr std::string_view type = "type:";
    if (std::string_view(low).substr(0, type.size()) == type) {
        if (low.size() != type.size() + 8) return Resolved::BadSyntax;
        uint32_t id = 0;
        for (size_t i = type.size(); i < low.size(); ++i) {
            const char ch = low[i];
            const int d = ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : -1;
            if (d < 0) return Resolved::BadSyntax;
            id = id << 4 | static_cast<uint32_t>(d);
        }
        *out = c.by_id(id);
        *matches = *out ? 1 : 0;
        return *out ? Resolved::Ok : Resolved::NotFound;
    }
    for (const IconRow& r : c.rows)
        if (r.alias == low) {
            if (!*out) *out = &r;
            ++*matches;
        }
    if (*matches > 1) {
        *out = nullptr;
        return Resolved::Ambiguous;
    }
    return *out ? Resolved::Ok : Resolved::NotFound;
}

void fill_icon(const IconRow& row, KilnMarkerIcon* out) {
    const uint32_t size = out->size;
    std::memset(out, 0, sizeof(KilnMarkerIcon));
    out->size = size;
    out->id = row.id;
    out->flags = row.flags;
    out->surfaces = row.surfaces;
    std::memcpy(out->texture_guid, row.image, 16);
    std::snprintf(out->key, sizeof out->key, "%s", row.key.c_str());
    std::snprintf(out->alias, sizeof out->alias, "%s", row.alias.c_str());
    std::snprintf(out->texture_name, sizeof out->texture_name, "%s", row.name.c_str());
}

// ---- verified build pairs (G4 of the dev.28 plan: one compiled-in table until a record and a tool exist) ---------------

namespace {
constexpr BuildPair kPairs[] = {
    // server Steam 26430253, client Steam 26430625 (dossier header); 43 catalog rows, 37 with the map surface
    {"f0d824be7fcd48ebfe2458ff5c28da26060c2074e3ec1fcac587e5a1d508aeb2", "5d9ef2f140b8ed49e30dbee655841b89d08baec20f799a1817e171a18a850c77",
     "a9d9390934bd614784fa9f82b2bed4eba6a9bad08cb78f262deb5810fda7cac9"},
};
}  // namespace

const BuildPair* pair_of_client(std::string_view exe) {
    for (const BuildPair& p : kPairs)
        if (exe == p.client_exe) return &p;
    return nullptr;
}

const BuildPair* pair_of_server(std::string_view exe) {
    for (const BuildPair& p : kPairs)
        if (exe == p.server_exe) return &p;
    return nullptr;
}

// ---- the holder ---------------------------------------------------------------------------------------------------------

bool CatalogHolder::publish(std::shared_ptr<const Catalog> c, bool map_verified, std::string map_reason) {
    std::lock_guard g(mu_);
    if (catalog_ || !c) return false;
    catalog_ = std::move(c);
    generation_ = 1;
    map_verified_ = map_verified;
    map_reason_ = std::move(map_reason);
    return true;
}

std::shared_ptr<const Catalog> CatalogHolder::get() const {
    std::lock_guard g(mu_);
    return catalog_;
}

uint64_t CatalogHolder::generation() const {
    std::lock_guard g(mu_);
    return generation_;
}

bool CatalogHolder::map_verified() const {
    std::lock_guard g(mu_);
    return map_verified_ && !changed_;
}

std::string CatalogHolder::map_reason() const {
    std::lock_guard g(mu_);
    return map_reason_;
}

void CatalogHolder::latch_changed(std::string reason) {
    std::lock_guard g(mu_);
    if (changed_) return;
    changed_ = true;
    map_reason_ = std::move(reason);
}

bool CatalogHolder::changed() const {
    std::lock_guard g(mu_);
    return changed_;
}

IconAnswer CatalogHolder::check(uint32_t id, void* ctx) {
    const auto c = static_cast<const CatalogHolder*>(ctx)->get();
    if (!c) return IconAnswer::NotReady;
    const IconRow* r = c->by_id(id);
    if (!r) return IconAnswer::Unknown;
    return (r->surfaces & KILN_MARKER_MAP) ? IconAnswer::Ok : IconAnswer::NotForMap;
}

}  // namespace kmk
