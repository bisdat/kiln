// SPDX-License-Identifier: MIT
// The selected Fisher contract, chapter 16f section 5.2. Layout metadata only, never copied game defaults.
#include "bodypack.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <set>

namespace kiln::qb {
namespace {
constexpr size_t kMaxBodyBytes = 64u << 20;
constexpr uint32_t kMaxComponents = 512;
constexpr uint32_t kInteraction = 1u << 13, kSteering = 1u << 11;
constexpr std::array<uint32_t, 11> kShapes{2, 1, 0, 0, 0, 0, 0, 2, 2, 2, 2};
uint32_t hash(const char* name) { return fnv1a(name, std::strlen(name)); }
uint32_t read32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
void put32(Bytes& b, size_t at, uint32_t v) { std::memcpy(b.data() + at, &v, 4); }
void putf(Bytes& b, size_t at, float v) { std::memcpy(b.data() + at, &v, 4); }
void vec3(Bytes& b, size_t at, float x, float y, float z) { putf(b, at, x); putf(b, at + 4, y); putf(b, at + 8, z); }
bool range(size_t len, size_t at, size_t size) { return at <= len && size <= len - at; }
bool overlap(size_t a, size_t an, size_t b, size_t bn) { return an && bn && a < b + bn && b < a + an; }
bool array(const uint8_t* b, size_t len, size_t field, size_t stride, size_t* at, uint32_t* count) {
    if (!range(len, field, 8)) return false;
    const int32_t rel = static_cast<int32_t>(read32(b + field));
    *count = read32(b + field + 4);
    if (!*count) { *at = 0; return true; }
    const int64_t target = static_cast<int64_t>(field) + rel;
    if (target < 24 || target > static_cast<int64_t>(len)) return false;
    *at = static_cast<size_t>(target);
    return *count <= (len - *at) / stride;
}
struct Variant { uint32_t type, size; size_t at; };
bool variant(const uint8_t* b, size_t len, size_t header, Variant* out) {
    if (!range(len, header, 12)) return false;
    out->type = read32(b + header); out->size = read32(b + header + 8);
    const int64_t target = static_cast<int64_t>(header) + 4 + static_cast<int32_t>(read32(b + header + 4));
    if (target < 24 || target > static_cast<int64_t>(len)) return false;
    out->at = static_cast<size_t>(target);
    return out->size && range(len, out->at, out->size);
}
size_t append(Bytes& b, size_t size) {
    const size_t at = (b.size() + 3) & ~size_t{3};
    b.resize(at + size);
    return at;
}
struct Component { const char* name; uint32_t size; bool remove; };
constexpr Component kComponents[] = {
    {"keen::ecs::CharacterSetup", 44, true}, {"keen::ecs::NewCharacterSetup", 1, true},
    {"keen::ecs::RenderEquipment", 788, true}, {"keen::ecs::EquipmentCheck", 1, true}, {"keen::ecs::SlotSelection", 216, true},
    {"keen::ecs::ModelResource", 16, false}, {"keen::ecs::AnimationGraphResourceComponent2", 40, false},
    {"keen::ecs::AnimationGraphComponent2", 44, false}, {"keen::ecs::PathWalker", 92, false},
    {"keen::ecs::MovementColliderConfig", 32, false}, {"keen::ecs::UiOffsets", 80, false},
    {"keen::ecs::ColliderResourceComponent", 8, false}, {"keen::ecs::AnimationGraphSlotComponent", 1760, false},
    {"keen::ecs::ServerAnimationGraphSlotComponent", 1760, false},
};
}  // namespace

std::span<const BodyLayoutType> body_layout() {
    // Validate each interpreted field and its containing size/base. The adapter also checks the enum values and
    // the BlobArray/variant element contracts. Uninterpreted bytes, including nested gameplay arrays, stay in place.
    static constexpr BodyLayoutField root[]{{"components", "keen::BlobArray<keen::BlobVariant<keen::ecs::Component>>", 16}};
    static constexpr BodyLayoutField model[]{{"model", "keen::ObjectReference<keen::Model>", 0}};
    static constexpr BodyLayoutField graph[]{{"animationGraph", "keen::ObjectReference<keen::anim_graph::tool_graph::AnimationGraph2_0>", 0}};
    static constexpr BodyLayoutField cloth[]{{"clothCollider", "keen::Guid", 8}, {"currentClothCollider", "keen::Guid", 24}};
    static constexpr BodyLayoutField path[]{{"horizontalRadius", "keen::float32", 56}, {"height", "keen::float32", 60}};
    static constexpr BodyLayoutField movement[]{{"width", "keen::float32", 0}, {"height", "keen::float32", 4}};
    static constexpr BodyLayoutField ui[]{{"healthBarOffset", "keen::ecs::Offset", 0}, {"interactionPromptOffset", "keen::ecs::Offset", 40}};
    static constexpr BodyLayoutField offset[]{{"localOffset", "keen::float3", 0}, {"worldOffset", "keen::float3", 12}};
    static constexpr BodyLayoutField slots[]{{"slotsInitialized", "keen::AttachmentSlotMask", 0}};
    static constexpr BodyLayoutField colliders[]{{"colliders", "keen::BlobArray<keen::ecs::ColliderResourceData>", 0}};
    static constexpr BodyLayoutField mode[]{{"dataArray", "keen::ecs::ColliderDataArray", 0}};
    static constexpr BodyLayoutField data[]{{"flags", "keen::ecs::ColliderFlagData", 0}, {"offset", "keen::float3", 8}, {"orientation", "keen::Quaternion", 20}};
    static constexpr BodyLayoutField flags[]{{"isA", "keen::ecs::ColliderMask", 0}};
    static constexpr BodyLayoutField box[]{{"shape", "keen::BoxColliderShape", 80}};
    static constexpr BodyLayoutField sphere[]{{"shape", "keen::SphereColliderShape", 80}};
    static constexpr BodyLayoutField half[]{{"halfSize", "keen::float3", 0}};
    static constexpr BodyLayoutField radius[]{{"radius", "keen::float32", 0}};
    static constexpr BodyLayoutField xyz[]{{"x", "keen::float32", 0}, {"y", "keen::float32", 4}, {"z", "keen::float32", 8}};
    static constexpr BodyLayoutField xyzw[]{{"x", "keen::float32", 0}, {"y", "keen::float32", 4}, {"z", "keen::float32", 8}, {"w", "keen::float32", 12}};
    static constexpr BodyLayoutValue masks[]{{"Interaction", 13}, {"Steering", 11}};
    static const BodyLayoutType types[]{
        {"keen::ecs::TemplateResource", 24, root},
        {"keen::ecs::CharacterSetup", 44, {}, "keen::ecs::Component"}, {"keen::ecs::NewCharacterSetup", 1, {}, "keen::ecs::Component"},
        {"keen::ecs::RenderEquipment", 788, {}, "keen::ecs::Component"}, {"keen::ecs::EquipmentCheck", 1, {}, "keen::ecs::Component"},
        {"keen::ecs::SlotSelection", 216, {}, "keen::ecs::Component"},
        {"keen::ecs::ModelResource", 16, model, "keen::ecs::Component"}, {"keen::ecs::AnimationGraphResourceComponent2", 40, graph, "keen::ecs::Component"},
        {"keen::ecs::AnimationGraphComponent2", 44, cloth, "keen::ecs::Component"}, {"keen::ecs::PathWalker", 92, path, "keen::ecs::Component"},
        {"keen::ecs::MovementColliderConfig", 32, movement, "keen::ecs::Component"}, {"keen::ecs::UiOffsets", 80, ui, "keen::ecs::Component"},
        {"keen::ecs::Offset", 40, offset}, {"keen::ecs::AnimationGraphSlotComponentBase", 1760, slots, "keen::ecs::Component"},
        {"keen::ecs::AnimationGraphSlotComponent", 1760, {}, "keen::ecs::AnimationGraphSlotComponentBase"},
        {"keen::ecs::ServerAnimationGraphSlotComponent", 1760, {}, "keen::ecs::AnimationGraphSlotComponentBase"},
        {"keen::ecs::ColliderResourceComponent", 8, colliders, "keen::ecs::Component"}, {"keen::ecs::ColliderResourceData", 56, mode},
        {"keen::ecs::ColliderData", 80, data}, {"keen::ecs::ColliderFlagData", 8, flags},
        {"keen::ecs::BoxColliderData", 96, box, "keen::ecs::ColliderData"}, {"keen::ecs::SphereColliderData", 88, sphere, "keen::ecs::ColliderData"},
        {"keen::BoxColliderShape", 12, half, "keen::ColliderShape"}, {"keen::SphereColliderShape", 4, radius, "keen::ColliderShape"},
        {"keen::float3", 12, xyz}, {"keen::Quaternion", 16, xyzw}, {"keen::ecs::ColliderFlags", 2, {}, "keen::uint16", masks},
        {"keen::ecs::ColliderDataArray", 8, {}, "keen::BlobArray<keen::BlobVariant<keen::ecs::ColliderData>>"},
        {"keen::BlobArray<keen::BlobVariant<keen::ecs::ColliderData>>", 8, {}, "keen::BlobVariant<keen::ecs::ColliderData>"},
        {"keen::BlobVariant<keen::ecs::ColliderData>", 12, {}, "keen::ecs::ColliderData"},
        {"keen::BlobArray<keen::ecs::ColliderResourceData>", 8, {}, "keen::ecs::ColliderResourceData"},
        {"keen::BlobArray<keen::BlobVariant<keen::ecs::Component>>", 8, {}, "keen::BlobVariant<keen::ecs::Component>"},
        {"keen::BlobVariant<keen::ecs::Component>", 12, {}, "keen::ecs::Component"},
        {"keen::ecs::ColliderMask", 4, {}, "keen::Bitmask32<keen::ecs::ColliderFlags>"},
        {"keen::Bitmask32<keen::ecs::ColliderFlags>", 4, {}, "keen::ecs::ColliderFlags"},
        {"keen::AttachmentSlotMask", 8}, {"keen::float32", 4}, {"keen::Guid", 16},
    };
    return types;
}

bool lower_body(const uint8_t* res, size_t len, const BodySpec& spec, Bytes* out, std::string* err) {
    const auto fail = [&](const std::string& why) { if (err) *err = why; return false; };
    if (!out) return fail("body: no output buffer");
    if (spec.model.zero()) return fail("body.model: a cooked model GUID is required");
    if (spec.graph.zero()) return fail("animation: a cooked graph GUID is required");
    if (!std::isfinite(spec.radius) || spec.radius < .05 || spec.radius > 4) return fail("body.size.radius: a finite radius in [0.05, 4] metres");
    if (!std::isfinite(spec.height) || spec.height < .1 || spec.height > 8) return fail("body.size.height: a finite height in [0.1, 8] metres");
    if (!res || len < 24 || len > kMaxBodyBytes) return fail("body.chassis: a template of 24 bytes to 64 MiB is required");
    size_t headers = 0;
    uint32_t count = 0;
    if (!array(res, len, 16, 12, &headers, &count) || !count || count > kMaxComponents)
        return fail("body.chassis: the component array is invalid or exceeds 512 entries");
    std::map<uint32_t, Variant> components;
    std::vector<Variant> retained;
    std::set<uint32_t> removed;
    for (const auto& c : kComponents) if (c.remove) removed.insert(hash(c.name));
    for (uint32_t i = 0; i < count; ++i) {
        Variant c;
        if (!variant(res, len, headers + i * 12, &c) || overlap(c.at, c.size, headers, size_t{count} * 12))
            return fail("body.chassis: a component payload is invalid or overlaps the component headers");
        for (const auto& [_, previous] : components)
            if (overlap(c.at, c.size, previous.at, previous.size)) return fail("body.chassis: overlapping component payloads");
        if (!components.emplace(c.type, c).second) return fail("body.chassis: a duplicate component type is ambiguous");
        if (!removed.contains(c.type)) retained.push_back(c);
    }
    for (const auto& expected : kComponents) {
        const auto found = components.find(hash(expected.name));
        if (found == components.end() && std::strcmp(expected.name, "keen::ecs::ModelResource") == 0) continue;
        if (found == components.end() || found->second.size != expected.size)
            return fail(std::string("body.chassis: missing or changed ") + expected.name);
    }
    const auto at = [&](const char* name) { return components.at(hash(name)).at; };
    size_t modes = 0;
    uint32_t n_modes = 0;
    if (!array(res, len, at("keen::ecs::ColliderResourceComponent"), 56, &modes, &n_modes) || n_modes != kShapes.size())
        return fail("body.chassis: the Fisher collider mode array changed");
    // Do not let a malformed nested array alias a component or header we intend to retain unchanged.
    const auto nested = [&](size_t where, size_t size) {
        if (overlap(where, size, headers, size_t{count} * 12)) return false;
        for (const auto& [_, c] : components) if (overlap(where, size, c.at, c.size)) return false;
        return true;
    };
    if (!nested(modes, size_t{n_modes} * 56)) return fail("body.chassis: collider modes alias a component");
    struct Shape { size_t at; bool box; };
    std::vector<Shape> shapes;
    std::vector<std::pair<size_t, size_t>> nested_ranges{{modes, size_t{n_modes} * 56}};
    const auto remember = [&](size_t where, size_t size) {
        if (!nested(where, size)) return false;
        for (const auto& [old, n] : nested_ranges) if (overlap(where, size, old, n)) return false;
        nested_ranges.emplace_back(where, size);
        return true;
    };
    for (uint32_t i = 0; i < n_modes; ++i) {
        size_t data = 0;
        uint32_t n = 0;
        if (!array(res, len, modes + i * 56, 12, &data, &n) || n != kShapes[i] || (n && !remember(data, size_t{n} * 12)))
            return fail("body.chassis: the Fisher collider shape array changed or aliases another payload");
        for (uint32_t j = 0; j < n; ++j) {
            Variant v;
            if (!variant(res, len, data + j * 12, &v)) return fail("body.chassis: an invalid collider shape payload");
            const bool box = j == 0;
            if (v.type != hash(box ? "keen::ecs::BoxColliderData" : "keen::ecs::SphereColliderData") || v.size != (box ? 96u : 88u) ||
                (read32(res + v.at) & (kInteraction | kSteering)) != (box ? kInteraction : kSteering) || !remember(v.at, v.size))
                return fail("body.chassis: expected only the verified Fisher Interaction box and Steering sphere shapes");
            shapes.push_back({v.at, box});
        }
    }
    Bytes b(res, res + len);
    if (!components.contains(hash("keen::ecs::ModelResource"))) {
        const size_t model = append(b, 16);
        retained.push_back({hash("keen::ecs::ModelResource"), 16, model});
        components.emplace(hash("keen::ecs::ModelResource"), retained.back());
    }
    std::memcpy(b.data() + at("keen::ecs::ModelResource"), spec.model.b, 16);
    std::memcpy(b.data() + at("keen::ecs::AnimationGraphResourceComponent2"), spec.graph.b, 16);
    std::memset(b.data() + at("keen::ecs::AnimationGraphComponent2") + 8, 0, 32);
    const float radius = static_cast<float>(spec.radius), height = static_cast<float>(spec.height);
    putf(b, at("keen::ecs::PathWalker") + 56, radius); putf(b, at("keen::ecs::PathWalker") + 60, height);
    putf(b, at("keen::ecs::MovementColliderConfig"), radius * 2); putf(b, at("keen::ecs::MovementColliderConfig") + 4, height);
    for (const char* name : {"keen::ecs::AnimationGraphSlotComponent", "keen::ecs::ServerAnimationGraphSlotComponent"})
        std::memset(b.data() + at(name), 0, 8);  // the authored graph initializes its own named slots
    const size_t ui = at("keen::ecs::UiOffsets");
    vec3(b, ui, 0, static_cast<float>(spec.height + .2), 0); vec3(b, ui + 12, 0, 0, 0);
    vec3(b, ui + 40, 0, static_cast<float>(spec.height * .6), 0); vec3(b, ui + 52, 0, 0, 0);
    for (const auto& shape : shapes) {
        vec3(b, shape.at + 8, 0, shape.box ? height / 2 : 0, 0);
        if (shape.box) {
            vec3(b, shape.at + 20, 0, 0, 0); putf(b, shape.at + 32, 1);
            vec3(b, shape.at + 80, radius, height / 2, radius);
        } else putf(b, shape.at + 80, radius);
    }
    const size_t new_headers = append(b, retained.size() * 12);
    for (size_t i = 0; i < retained.size(); ++i) {
        const auto& c = retained[i];
        const size_t h = new_headers + i * 12;
        put32(b, h, c.type); put32(b, h + 4, static_cast<uint32_t>(static_cast<int64_t>(c.at) - static_cast<int64_t>(h + 4)));
        put32(b, h + 8, c.size);
    }
    put32(b, 16, static_cast<uint32_t>(new_headers - 16)); put32(b, 20, static_cast<uint32_t>(retained.size()));
    *out = std::move(b);
    if (err) err->clear();
    return true;
}
}  // namespace kiln::qb
