// SPDX-License-Identifier: MIT
// kiln.markers' icon catalog (chapter 04 §4.3, §2.7): the game's map marker types that have an icon image, read on a client
// from copies of two kinds of resources through kiln.assets (resource_read: the bytes are copied before they are parsed),
// with every offset, count and length checked against the copy. Pure apart from the read callback.
//
// MapMarkerRegistryResource {d165e819-f603-42a3-8f7d-827e9e8cb97a, type 0x316be6f1, part 0}: at +0 a MapMarkerMap of three
// BlobArrays (entries +0, keys +8, values +16; a BlobArray is {i32 offset from its own header, u32 count}); keys are u32
// type ids, values 80-byte MapMarkerType records (markerId +0x10, fast travel +0x18, custom +0x19, icon image GUID +0x1c,
// above fog +0x40, show label +0x41, show range +0x42). Rows with a null image are left out. Each row's image names a
// UiTextureResource {image, type 0xf9e86997, part 0} of 56 bytes whose debugName BlobString {i32 offset, u32 length} is
// at +0x20.
//
// The digest (SHA-256, the catalog's identity for one game build): rows sorted by unsigned id, each u32le id, the 16 image
// bytes (game order), u32le flags, u16le name length, the name's UTF-8 bytes.
#ifndef KILN_MARKERS_CATALOG_H
#define KILN_MARKERS_CATALOG_H
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "services/kiln_markers.h"
#include "store.h"

namespace kmk {

// {d165e819-f603-42a3-8f7d-827e9e8cb97a} in the game's byte order (.NET Guid.ToByteArray())
inline constexpr uint8_t kRegistryGuid[16] = {0x19, 0xe8, 0x65, 0xd1, 0x03, 0xf6, 0xa3, 0x42, 0x8f, 0x7d, 0x82, 0x7e, 0x9e, 0x8c, 0xb9, 0x7a};
inline constexpr uint32_t kRegistryType = 0x316be6f1u;   // keen::MapMarkerRegistryResource
inline constexpr uint32_t kUiTextureType = 0xf9e86997u;  // keen::UiTextureResource
inline constexpr size_t kMaxRegistryRows = 1024;
inline constexpr size_t kMaxRegistryBytes = size_t{1} << 20;
inline constexpr size_t kMaxTextureBytes = size_t{1} << 16;
inline constexpr size_t kNameMax = 128;

struct IconRow {
    uint32_t id = 0;
    uint32_t flags = 0;     // KILN_MARKER_ICON_*
    uint32_t surfaces = 0;  // KILN_MARKER_MAP, or 0 for a fast-travel type
    uint8_t image[16] = {};
    std::string name;   // the texture's debug name
    std::string alias;  // name without a leading "mapmarker_", ASCII lower-cased
    std::string key;    // "type:" + eight lower-case hex digits
};

// One row of the registry before its texture is read.
struct RegistryRow {
    uint32_t id = 0;
    uint32_t flags = 0;
    uint8_t image[16] = {};
};

class Catalog {
public:
    std::vector<IconRow> rows;          // sorted by id
    std::array<uint8_t, 32> digest{};
    uint32_t map_rows() const;
    std::string digest_hex() const;
    const IconRow* by_id(uint32_t id) const;
};

// Reads a resource (a copy) into `out`; false when it is missing or larger than `limit`.
using ReadResource = bool (*)(const uint8_t guid[16], uint32_t type, size_t limit, std::vector<uint8_t>* out, void* ctx);

// The registry's image-backed rows (unsorted, in registry order). False and `why` on anything malformed.
bool parse_registry(const uint8_t* p, size_t n, std::vector<RegistryRow>* out, std::string* why);
// A UiTextureResource's debug name: strict UTF-8, 1 to 128 bytes, no control characters (one trailing NUL is dropped).
bool parse_texture_name(const uint8_t* p, size_t n, std::string* name, std::string* why);
// "type:%08x" and the alias rule.
std::string canonical_key(uint32_t id);
std::string alias_of(std::string_view name);
std::array<uint8_t, 32> digest_of(const std::vector<IconRow>& sorted_rows);

// The whole read. nullptr and `why` when the registry or a texture is missing or malformed (the catalog is then not
// offered at all: no partial catalog).
std::shared_ptr<const Catalog> read_catalog(ReadResource read, void* ctx, std::string* why);

// catalog_resolve's rule. BadSyntax: not 1-128 printable ASCII bytes without surrounding spaces, or "type:" not followed
// by exactly eight hex digits.
enum class Resolved { Ok, NotFound, Ambiguous, BadSyntax };
Resolved resolve(const Catalog& c, std::string_view name, const IconRow** out, uint32_t* matches);

// Fills a KilnMarkerIcon from a row (out->size is kept).
void fill_icon(const IconRow& row, KilnMarkerIcon* out);

// The game builds whose catalog Kiln has verified (dossier header and chapter 04 §4.3): native map icons are offered only
// when this process's exe and the catalog's digest are one of these pairs. A game update needs a new row: the new
// client's digest from tests/catalog_offline.cpp with both new exe hashes (until a build record and a tool hold it).
struct BuildPair {
    const char* server_exe;
    const char* client_exe;
    const char* catalog_sha256;
};
const BuildPair* pair_of_client(std::string_view exe_sha256);
const BuildPair* pair_of_server(std::string_view exe_sha256);

// The process's catalog: the first one read (or, on a server, accepted) is kept for the life of the process.
class CatalogHolder {
public:
    // Publishes the first catalog; false (and nothing changes) when one is published already.
    bool publish(std::shared_ptr<const Catalog> c, bool map_verified, std::string map_reason);
    std::shared_ptr<const Catalog> get() const;
    uint64_t generation() const;
    bool map_verified() const;
    std::string map_reason() const;
    // The once-a-second check found other metadata than the published catalog's: native map icons stay off until restart.
    void latch_changed(std::string reason);
    bool changed() const;

    // Store::set_icon_check's callback (ctx: the holder).
    static IconAnswer check(uint32_t id, void* ctx);

private:
    mutable std::mutex mu_;
    std::shared_ptr<const Catalog> catalog_;
    uint64_t generation_ = 0;
    bool map_verified_ = false;
    bool changed_ = false;
    std::string map_reason_;
};

}  // namespace kmk
#endif
