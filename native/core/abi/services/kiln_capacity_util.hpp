// SPDX-License-Identifier: MIT
// Header-only helpers for services that answer with kiln_capacity.h (since 0.2.0-dev.28): filling a reply (checked
// accounting, the not-ready form), a revision counter, the stable registration id, and the pending-definition files
// (atomic write, deterministic replay order). No Windows dependency; C++20.
#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "kiln_capacity.h"

namespace kiln::capacity {

static_assert(sizeof(KilnCapacityInfo) == 48 && offsetof(KilnCapacityInfo, revision) == 40, "KilnCapacityInfo layout");
static_assert(sizeof(KilnDefinitionReply) == 64 && offsetof(KilnDefinitionReply, snapshot_revision) == 48,
              "KilnDefinitionReply layout");

// Where a budget lives. world(): kind is KILN_WORLD_SERVER (0) or KILN_WORLD_CLIENT (2); generation is the core
// session's counter of that kind (0 = there is no such world now).
struct Scope {
    bool world_scope = false;
    uint32_t world_kind = KILN_CAPACITY_NO_WORLD_KIND;
    uint32_t generation = 0;
    static Scope process() { return {}; }
    static Scope world(uint32_t kind, uint32_t generation) { return {true, kind, generation}; }
};

inline bool valid_out(const KilnCapacityInfo* out) { return out && out->size >= sizeof(KilnCapacityInfo); }

inline void fill_scope(KilnCapacityInfo* out, const Scope& s) {
    out->flags |= s.world_scope ? KILN_CAPACITY_WORLD : KILN_CAPACITY_PROCESS;
    out->world_kind = s.world_scope ? s.world_kind : KILN_CAPACITY_NO_WORLD_KIND;
    out->generation = s.world_scope ? s.generation : 0;
}

// A reply that is not ready: the four counts are 0, READY is clear. `readiness` is any value but VALID.
inline void not_ready(KilnCapacityInfo* out, uint32_t readiness, const Scope& scope, uint64_t revision,
                      bool expanded = false) {
    const uint32_t size = out->size;
    std::memset(out, 0, sizeof *out);
    out->size = size;
    out->readiness = readiness == KILN_CAPACITY_VALID ? KILN_CAPACITY_NOT_READY : readiness;
    if (expanded) out->flags |= KILN_CAPACITY_EXPANDED;
    if (out->readiness == KILN_CAPACITY_UNKNOWN_EXTERNAL) out->flags |= KILN_CAPACITY_EXTERNAL_UNKNOWN;
    fill_scope(out, scope);
    out->revision = revision;
}

// A ready reply, when the counts add up: game + kiln + reserved <= capacity in 64 bits. Otherwise the reply is
// ACCOUNTING_MISMATCH with zero counts (returns false): a budget is never clamped into something positive.
inline bool publish(KilnCapacityInfo* out, uint64_t capacity, uint64_t game_used, uint64_t kiln_used, uint64_t reserved,
                    const Scope& scope, uint64_t revision, bool expanded) {
    const uint64_t sum = game_used + kiln_used + reserved;
    const bool wraps = sum < game_used || game_used + kiln_used < game_used;
    if (wraps || sum > capacity || capacity > UINT32_MAX || (scope.world_scope && scope.generation == 0)) {
        not_ready(out, scope.world_scope && scope.generation == 0 ? KILN_CAPACITY_NO_WORLD : KILN_CAPACITY_ACCOUNTING_MISMATCH,
                  scope, revision, expanded);
        return false;
    }
    const uint32_t size = out->size;
    std::memset(out, 0, sizeof *out);
    out->size = size;
    out->capacity = static_cast<uint32_t>(capacity);
    out->game_used = static_cast<uint32_t>(game_used);
    out->kiln_used = static_cast<uint32_t>(kiln_used);
    out->reserved = static_cast<uint32_t>(reserved);
    out->readiness = KILN_CAPACITY_VALID;
    out->flags = KILN_CAPACITY_READY | (expanded ? KILN_CAPACITY_EXPANDED : 0u);
    fill_scope(out, scope);
    out->revision = revision;
    return true;
}

// A service's revision: starts at 1, rises by one with every published change (also a receipt-only one).
class Revision {
public:
    uint64_t current() const { return v_.load(std::memory_order_acquire); }
    uint64_t next() { return v_.fetch_add(1, std::memory_order_acq_rel) + 1; }

private:
    std::atomic<uint64_t> v_{1};
};

// ---- the registration id ----
// UUIDv5 (SHA-1) in Kiln's id namespace over the UTF-8 text
//   "<mod>:<kind>:<definition guid, 32 lower-case hex digits of its 16 bytes>:<sha256, 64 lower-case hex digits>"
// kind is "vfx", "buff" or "building-material-pool"; sha256 is the hash of the compiler's canonical definition (no
// absolute paths in it). Managed: Kiln.DefinitionRegistration.DeriveId gives the same 16 bytes (Guid.ToByteArray of a
// big-endian-read Guid differs: compare bytes through its own helper).
namespace detail {
inline std::array<uint8_t, 20> sha1(const uint8_t* data, size_t len) {
    uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
    std::vector<uint8_t> msg(data, data + len);
    msg.push_back(0x80);
    while (msg.size() % 64 != 56) msg.push_back(0);
    const uint64_t bits = static_cast<uint64_t>(len) * 8;
    for (int i = 7; i >= 0; --i) msg.push_back(static_cast<uint8_t>(bits >> (8 * i)));
    auto rol = [](uint32_t x, int n) { return (x << n) | (x >> (32 - n)); };
    for (size_t at = 0; at < msg.size(); at += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i)
            w[i] = (uint32_t{msg[at + 4 * i]} << 24) | (uint32_t{msg[at + 4 * i + 1]} << 16) |
                   (uint32_t{msg[at + 4 * i + 2]} << 8) | uint32_t{msg[at + 4 * i + 3]};
        for (int i = 16; i < 80; ++i) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20) f = (b & c) | (~b & d), k = 0x5A827999u;
            else if (i < 40) f = b ^ c ^ d, k = 0x6ED9EBA1u;
            else if (i < 60) f = (b & c) | (b & d) | (c & d), k = 0x8F1BBCDCu;
            else f = b ^ c ^ d, k = 0xCA62C1D6u;
            const uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d, d = c, c = rol(b, 30), b = a, a = t;
        }
        h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e;
    }
    std::array<uint8_t, 20> out{};
    for (int i = 0; i < 5; ++i)
        for (int k = 0; k < 4; ++k) out[static_cast<size_t>(4 * i + k)] = static_cast<uint8_t>(h[i] >> (24 - 8 * k));
    return out;
}
inline std::array<uint8_t, 32> sha256(const uint8_t* data, size_t len) {
    static constexpr uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
        0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
        0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
        0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
        0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
        0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::vector<uint8_t> msg(data, data + len);
    msg.push_back(0x80);
    while (msg.size() % 64 != 56) msg.push_back(0);
    const uint64_t bits = static_cast<uint64_t>(len) * 8;
    for (int i = 7; i >= 0; --i) msg.push_back(static_cast<uint8_t>(bits >> (8 * i)));
    auto ror = [](uint32_t x, int n) { return (x >> n) | (x << (32 - n)); };
    for (size_t at = 0; at < msg.size(); at += 64) {
        uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = (uint32_t{msg[at + 4 * i]} << 24) | (uint32_t{msg[at + 4 * i + 1]} << 16) |
                   (uint32_t{msg[at + 4 * i + 2]} << 8) | uint32_t{msg[at + 4 * i + 3]};
        for (int i = 16; i < 64; ++i) {
            const uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            const uint32_t t1 = hh + (ror(e, 6) ^ ror(e, 11) ^ ror(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
            const uint32_t t2 = (ror(a, 2) ^ ror(a, 13) ^ ror(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            hh = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
        }
        h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e, h[5] += f, h[6] += g, h[7] += hh;
    }
    std::array<uint8_t, 32> out{};
    for (int i = 0; i < 8; ++i)
        for (int j = 0; j < 4; ++j) out[static_cast<size_t>(4 * i + j)] = static_cast<uint8_t>(h[i] >> (24 - 8 * j));
    return out;
}
inline void hex(std::string& to, const uint8_t* bytes, size_t n) {
    static const char* const digits = "0123456789abcdef";
    for (size_t i = 0; i < n; ++i) to += digits[bytes[i] >> 4], to += digits[bytes[i] & 15];
}
}  // namespace detail

// SHA-256 of `bytes`: the hash registration_id takes over a definition's canonical text (since 0.2.0-dev.29).
inline std::array<uint8_t, 32> sha256(std::string_view bytes) {
    return detail::sha256(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
}

inline const char* kind_name(uint32_t kind) {
    switch (kind) {
        case KILN_DEFINITION_VFX: return "vfx";
        case KILN_DEFINITION_BUFF: return "buff";
        case KILN_DEFINITION_BUILDING_MATERIAL_POOL: return "building-material-pool";
        default: return nullptr;
    }
}

// False for an unknown kind. `out` receives the 16 bytes in network order (as the UUID is written).
inline bool registration_id(std::string_view mod, uint32_t kind, const uint8_t definition_guid[16], const uint8_t sha256[32],
                            uint8_t out[16]) {
    const char* k = kind_name(kind);
    if (!k) return false;
    static constexpr uint8_t kNamespace[16] = {0x6f, 0x6d, 0x2a, 0x44, 0x53, 0xc1, 0x5c, 0x52,
                                               0x9d, 0x51, 0x0a, 0x3c, 0x6e, 0x1f, 0x2b, 0x77};
    std::string text(reinterpret_cast<const char*>(kNamespace), 16);
    text += mod;
    text += ':';
    text += k;
    text += ':';
    detail::hex(text, definition_guid, 16);
    text += ':';
    detail::hex(text, sha256, 32);
    const auto h = detail::sha1(reinterpret_cast<const uint8_t*>(text.data()), text.size());
    std::memcpy(out, h.data(), 16);
    out[6] = static_cast<uint8_t>((out[6] & 0x0f) | 0x50);
    out[8] = static_cast<uint8_t>((out[8] & 0x3f) | 0x80);
    return true;
}

// ---- a define reply ----
inline void reply_init(KilnDefinitionReply* r, uint32_t kind, const uint8_t registration[16], const uint8_t definition[16]) {
    const uint32_t size = r->size;
    std::memset(r, 0, sizeof *r);
    r->size = size;
    r->kind = kind;
    std::memcpy(r->registration_id, registration, 16);
    std::memcpy(r->definition_guid, definition, 16);
}
// ok = 1 for REGISTERED and ACCEPTED_NEXT_START (the latter always with 0 slots); reason NONE.
inline void reply_ok(KilnDefinitionReply* r, uint32_t disposition, uint32_t slots, uint64_t revision) {
    r->ok = 1;
    r->disposition = disposition;
    r->slots_consumed = disposition == KILN_DEFINITION_ACCEPTED_NEXT_START ? 0 : slots;
    r->reason_code = KILN_DEFINE_NONE;
    r->snapshot_revision = revision;
}
// ok = 0: REFUSED (slots 0 unless a record could not be freed) or RETAINED with the slots it still occupies.
inline void reply_refused(KilnDefinitionReply* r, uint32_t reason_code, uint64_t revision,
                          uint32_t disposition = KILN_DEFINITION_REFUSED, uint32_t slots_still_occupied = 0) {
    r->ok = 0;
    r->disposition = disposition == KILN_DEFINITION_RETAINED ? KILN_DEFINITION_RETAINED : KILN_DEFINITION_REFUSED;
    r->slots_consumed = slots_still_occupied;
    r->reason_code = reason_code == KILN_DEFINE_NONE ? KILN_DEFINE_GAME_REFUSED : reason_code;
    r->snapshot_revision = revision;
}

// ---- pending definitions: <dir>/<mod>/<file> ----
// Writes `bytes` to `file` through a temporary file in the same folder and a rename, so a reader sees the old file or
// the new one, never a part. False when anything failed (the temporary file is removed).
inline bool write_atomic(const std::filesystem::path& file, std::string_view bytes) {
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    std::filesystem::path tmp = file;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        out.flush();
        if (!out) {
            out.close();
            std::filesystem::remove(tmp, ec);
            return false;
        }
    }
    std::filesystem::rename(tmp, file, ec);
    if (ec) {
        std::filesystem::remove(tmp, ec);
        return false;
    }
    return true;
}

struct PendingFile {
    std::string mod;   // the folder under `dir`
    std::string name;  // the file's name (a definition GUID plus an extension, by convention)
    std::filesystem::path path;
};

// The pending files of every mod in replay order: by mod folder name, then file name, both compared as bytes.
// Temporary files (".tmp") and folders inside a mod's folder are skipped. A missing `dir` gives an empty list.
inline std::vector<PendingFile> pending_files(const std::filesystem::path& dir) {
    std::vector<PendingFile> out;
    std::error_code ec;
    for (std::filesystem::directory_iterator mods(dir, ec), end; !ec && mods != end; mods.increment(ec)) {
        if (!mods->is_directory(ec)) continue;
        std::error_code ec2;
        for (std::filesystem::directory_iterator files(mods->path(), ec2), fend; !ec2 && files != fend; files.increment(ec2)) {
            if (!files->is_regular_file(ec2) || files->path().extension() == ".tmp") continue;
            out.push_back({mods->path().filename().string(), files->path().filename().string(), files->path()});
        }
    }
    std::sort(out.begin(), out.end(), [](const PendingFile& a, const PendingFile& b) {
        return a.mod != b.mod ? a.mod < b.mod : a.name < b.name;
    });
    return out;
}

}  // namespace kiln::capacity
