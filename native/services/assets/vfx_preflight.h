// SPDX-License-Identifier: MIT
// KFC4 metadata only, never resource/content payloads. Layout: tools/kfc/kfc/archive.py.
#pragma once
#include <filesystem>
#include <fstream>
#include <unordered_set>

#include "assets.h"

namespace ka {
constexpr uint32_t kVfxType = 0xe64ee036u;
constexpr uint32_t kVfxMetadataMaxKeys = 1u << 20;
constexpr uint32_t kVfxMaxListedKeys = 65536;
using VfxKeys = std::unordered_set<Key, KeyHash>;

inline bool read_vfx_census(const std::filesystem::path& path, VfxKeys* out, std::string* why) {
    out->clear();
    auto bad = [&] { *why = "original VFX KFC metadata missing, truncated or unsupported"; return false; };
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return bad();
    const auto end = f.tellg();
    if (end < 0x90) return bad();
    const uint64_t bytes = static_cast<uint64_t>(end);
    uint8_t h[0x90];
    f.seekg(0);
    if (!f.read(reinterpret_cast<char*>(h), sizeof h) || std::memcmp(h, "KFC4", 4)) return bad();
    const auto word = [](const uint8_t* p) { uint32_t n; std::memcpy(&n, p, 4); return n; };
    const uint64_t tables = word(h + 4);
    if (tables < sizeof h || tables > bytes) return bad();
    struct Loc { uint64_t off; uint32_t count; };
    const auto loc = [&](size_t at) { return Loc{at + uint64_t(word(h + at)), word(h + at + 4)}; };
    const Loc order = loc(0x38), keys = loc(0x60), types = loc(0x78), bundles = loc(0x80);
    const auto fits = [&](Loc l, uint32_t stride, uint32_t max) {
        return l.count <= max && (!l.count || l.off >= sizeof h) && l.off <= tables && l.count <= (tables - l.off) / stride;
    };
    if (!fits(order, 4, kVfxMetadataMaxKeys) || !fits(keys, 32, kVfxMetadataMaxKeys) ||
        !fits(types, 4, 16384) || !fits(bundles, 12, 16384) || types.count != bundles.count) return bad();
    const auto read = [&](uint64_t at, void* dst, size_t n) {
        f.seekg(static_cast<std::streamoff>(at));
        return bool(f.read(static_cast<char*>(dst), static_cast<std::streamsize>(n)));
    };
    // Only the small type table is read whole; follow just this type's ordered key references.
    std::vector<uint32_t> ts(types.count);
    if (!read(types.off, ts.data(), ts.size() * 4)) return bad();
    bool found = false;
    VfxKeys stock;
    for (uint32_t i = 0; i < types.count; ++i) {
        if (ts[i] != kVfxType) continue;
        if (found) return bad();
        found = true;
        uint32_t b[3];
        if (!read(bundles.off + i * 12ull, b, sizeof b) || b[1] > order.count ||
            b[2] > order.count - b[1] || b[2] > kVfxMaxListedKeys) return bad();
        std::vector<uint32_t> ids(b[2]);
        if (!read(order.off + b[1] * 4ull, ids.data(), ids.size() * 4)) return bad();
        for (uint32_t id : ids) {
            uint8_t raw[32];
            if (id >= keys.count || !read(keys.off + id * 32ull, raw, sizeof raw)) return bad();
            Key k;
            std::memcpy(&k, raw, sizeof k);
            if (k.type != kVfxType) return bad();
            stock.insert(k);  // repeated aliases in the bundle consume one slot
        }
    }
    *out = std::move(stock);  // a valid archive with no VFX bundle has zero stock VFX keys
    return true;
}

// The caller safely copies the original list first. Preserve all distinct original/other-loader keys;
// if the final merged list would overflow, publish none of our additions. No store lock/ReadGuard inversion.
inline bool vfx_merge(const std::vector<Key>& original, const std::vector<Key>& added, uint32_t capacity,
                      std::vector<Key>* out) {
    VfxKeys seen;
    out->clear();
    for (const Key& k : original)
        if (seen.insert(k).second) out->push_back(k);
    const size_t originals = out->size();
    for (const Key& k : added)
        if (seen.insert(k).second) out->push_back(k);
    if (out->size() > capacity && out->size() != originals) {
        out->resize(originals);
        return false;
    }
    return true;
}

// Capture identities while the snapshot is guarded; resolve mod provenance only after leaving the reader
// and merged mutex. Keys retained by the original/foreign enumeration were not omitted.
inline void vfx_omitted(const ResView& view, const std::vector<Key>& added, const std::vector<Key>& kept,
                        std::vector<ResourceRegistration>* out) {
    const VfxKeys present(kept.begin(), kept.end());
    out->clear();
    for (const Key& k : added)
        if (!present.count(k))
            if (const Served* sv = view.key(k); sv && sv->is_new) out->push_back({k, sv->registration});
}
}  // namespace ka
