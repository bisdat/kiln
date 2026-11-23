// SPDX-License-Identifier: MIT
// A synthetic Fisher-shaped TemplateResource (the C++ twin of tools/kfc/tests/test_bodypack.py fixture()): the
// verified component sizes, nonzero opaque payloads, the eleven collider modes with the Interaction box and Steering
// sphere shapes, and empty dialogue line arrays so edit_template can run after the body rewrite. No game data.
#pragma once
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "../bodypack.h"

namespace body_fixture {
using kiln::qb::Bytes;
inline uint32_t hash(const std::string& name) { return kiln::qb::fnv1a(name.data(), name.size()); }
inline void put32(Bytes& b, size_t at, uint32_t v) { std::memcpy(b.data() + at, &v, 4); }
inline void putf(Bytes& b, size_t at, float v) { std::memcpy(b.data() + at, &v, 4); }
inline uint32_t get32(const Bytes& b, size_t at) { uint32_t v; std::memcpy(&v, b.data() + at, 4); return v; }
inline void setarr(Bytes& b, size_t field, size_t at, uint32_t count) {
    put32(b, field, count ? static_cast<uint32_t>(static_cast<int64_t>(at) - static_cast<int64_t>(field)) : 0);
    put32(b, field + 4, count);
}
inline size_t allocate(Bytes& b, size_t size) {
    b.resize((b.size() + 7) & ~size_t{7});
    const size_t at = b.size();
    b.resize(at + size);
    return at;
}
inline const std::vector<std::pair<const char*, uint32_t>>& sizes() {
    static const std::vector<std::pair<const char*, uint32_t>> v{
        {"CharacterSetup", 44}, {"NewCharacterSetup", 1}, {"RenderEquipment", 788}, {"EquipmentCheck", 1}, {"SlotSelection", 216},
        {"ModelResource", 16}, {"AnimationGraphResourceComponent2", 40}, {"AnimationGraphComponent2", 44}, {"PathWalker", 92},
        {"MovementColliderConfig", 32}, {"UiOffsets", 80}, {"ColliderResourceComponent", 8}, {"AnimationGraphSlotComponent", 1760},
        {"ServerAnimationGraphSlotComponent", 1760}, {"ActiveNpcState", 456}, {"LocaTagComponent", 16}, {"NpcDialog", 120}};
    return v;
}
// Component payload offset by short name (keen::ecs::<name>), 0 when absent.
inline size_t component(const Bytes& b, const std::string& name) {
    const int32_t rel = static_cast<int32_t>(get32(b, 16));
    const uint32_t count = get32(b, 20);
    const size_t headers = static_cast<size_t>(16 + rel);
    for (uint32_t i = 0; i < count; ++i) {
        const size_t h = headers + i * 12;
        if (get32(b, h) == hash("keen::ecs::" + name)) return static_cast<size_t>(static_cast<int64_t>(h) + 4 + static_cast<int32_t>(get32(b, h + 4)));
    }
    return 0;
}
inline Bytes fisher() {
    const auto& entries = sizes();
    Bytes b(24 + entries.size() * 12, 0);
    setarr(b, 16, 24, static_cast<uint32_t>(entries.size()));
    for (size_t i = 0; i < entries.size(); ++i) {
        const auto& [name, size] = entries[i];
        const size_t at = allocate(b, size);
        for (uint32_t k = 0; k < size; ++k) b[at + k] = static_cast<uint8_t>((i * 11 + k) % 256);
        const size_t h = 24 + i * 12;
        put32(b, h, hash(std::string("keen::ecs::") + name));
        put32(b, h + 4, static_cast<uint32_t>(static_cast<int64_t>(at) - static_cast<int64_t>(h) - 4));
        put32(b, h + 8, size);
    }
    // edit_template reads the four dialogue line arrays even when only the headline changes
    for (size_t offset : {0x34, 0x3c, 0x44, 0x4c}) setarr(b, component(b, "NpcDialog") + 0x14 + offset, 0, 0);
    static constexpr uint32_t shapes[] = {2, 1, 0, 0, 0, 0, 0, 2, 2, 2, 2};
    const size_t modes = allocate(b, 11 * 56);
    setarr(b, component(b, "ColliderResourceComponent"), modes, 11);
    for (uint32_t i = 0; i < 11; ++i) {
        const size_t mode = modes + i * 56;
        const uint32_t n = shapes[i];
        const size_t data = allocate(b, n * 12);
        setarr(b, mode, data, n);
        put32(b, mode + 20, 0x12340000 + i);
        for (uint32_t j = 0; j < n; ++j) {
            const bool box = j == 0;
            const uint32_t size = box ? 96 : 88;
            const size_t at = allocate(b, size);
            const size_t h = data + j * 12;
            put32(b, h, hash(box ? "keen::ecs::BoxColliderData" : "keen::ecs::SphereColliderData"));
            put32(b, h + 4, static_cast<uint32_t>(static_cast<int64_t>(at) - static_cast<int64_t>(h) - 4));
            put32(b, h + 8, size);
            put32(b, at, (1u << (box ? 13 : 11)) | 0x10);
            put32(b, at + 4, 0x20);
            putf(b, at + 80, 10);
        }
    }
    return b;
}
// A fake type registry that answers exactly the verified layout (the game side's kiln.types after no update).
inline bool layout_lookup(const std::string& name, kiln::qb::BodyTypeFacts* out) {
    for (const auto& type : kiln::qb::body_layout()) {
        if (name != type.name) continue;
        out->size = type.size;
        out->inner = type.inner ? type.inner : "";
        for (const auto& f : type.fields) out->fields.push_back({f.name, f.type, f.offset});
        for (const auto& v : type.values) out->values.emplace_back(v.name, v.value);
        return true;
    }
    return false;
}
}  // namespace body_fixture
