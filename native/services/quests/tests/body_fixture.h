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
#include "../motionclose.h"
#include "../questpack.h"

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
        {"ServerAnimationGraphSlotComponent", 1760}, {"ActiveNpcState", 456}, {"LocaTagComponent", 16}, {"NpcDialog", 120},
        {"NpcState", 128}, {"NpcActionSequences", 32}};
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
    // 16f S4: the Fisher's sequence references (motionclose.cpp offsets), the rest of each holder zeroed
    const auto source = [](const char* role) {
        for (const auto& s : kiln::qb::motionclose::fisher_closure())
            if (std::string(s.role) == role) { kiln::qb::Guid g; kiln::qb::parse_guid(s.guid, &g); return g; }
        return kiln::qb::Guid{};
    };
    const auto ref = [&](size_t at, const char* role) { const auto g = source(role); std::memcpy(b.data() + at, g.b, 16); };
    const auto refs = [&](size_t field, size_t stride, size_t offset, std::vector<const char*> roles) {
        const size_t at = allocate(b, stride * roles.size());
        setarr(b, field, at, static_cast<uint32_t>(roles.size()));
        for (size_t i = 0; i < roles.size(); ++i) ref(at + i * stride + offset, roles[i]);
    };
    const size_t active = component(b, "ActiveNpcState"), state = component(b, "NpcState"), actions = component(b, "NpcActionSequences");
    const size_t dialog = component(b, "NpcDialog"), walker = component(b, "PathWalker");
    std::memset(b.data() + active + 4, 0, 0x30);
    std::memset(b.data() + active + 0x1b0, 0, 0x18);
    std::memset(b.data() + state, 0, 0x60);
    std::memset(b.data() + actions, 0, 32);
    std::memset(b.data() + dialog + 0x28, 0, 0x20);
    std::memset(b.data() + walker, 0, 0x34);
    ref(active + 4, "sit"); ref(active + 0x14, "sleep"); ref(active + 0x24, "inspect");
    refs(active + 0x1b0, 16, 0, {"greet"}); refs(active + 0x1b8, 16, 0, {"talk"}); refs(active + 0x1c0, 16, 0, {"reaction0", "reaction1", "refuse"});
    ref(state, "greet"); ref(state + 0x10, "attentionGreeting");
    ref(state + 0x20, "turnLeftSmall"); ref(state + 0x30, "turnLeftBig"); ref(state + 0x40, "turnRightSmall"); ref(state + 0x50, "turnRightBig");
    ref(actions, "inspect");
    refs(actions + 0x10, 16, 0, {"idleGesture0", "idleGesture1"});
    refs(actions + 0x18, 32, 0x10, {"interaction"});
    ref(dialog + 0x28, "talk"); ref(dialog + 0x38, "refuse");
    ref(walker, "jump"); ref(walker + 0x10, "door");
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
// A synthetic ActorSequenceResource for one closure source: one sub-sequence whose events are a SetState at 0, the
// role's SetAnimationEvents (at 0 and then every `step` ns), an Sfx between them, a Loop over the middle phase and a
// LerpMovementFactor with a lerpDuration. Times in integer ns; nonzero source sub-sequence id.
inline Bytes sequence(const kiln::qb::motionclose::Source& source, int64_t step = 1'000'000'000) {
    struct Ev { const char* type; uint32_t size; int64_t time, duration, lerp; };
    std::vector<Ev> events{{"keen::actor::SetStateEvent", 104, 0, 0, 0}};
    for (uint32_t v = 0; v < source.variants; ++v) {
        events.push_back({"keen::actor::SetAnimationEvent", 120, step * v, 0, 0});
        if (v == 0) events.push_back({"keen::SfxNotifierEvent", 168, step / 2, 0, 0});
    }
    if (source.variants == 3) events.push_back({"keen::actor::LoopEvent", 120, step + step / 4, step / 2, 0});
    events.push_back({"keen::actor::LerpMovementFactorEvent", 120, step / 4, step / 8, step / 2});
    Bytes b(24, 0);
    kiln::qb::Guid g;
    kiln::qb::parse_guid(source.guid, &g);
    std::memcpy(b.data(), g.b, 16);
    const size_t sub = allocate(b, 72);
    setarr(b, 16, sub, 1);
    const size_t headers = allocate(b, events.size() * 12);
    setarr(b, sub, headers, static_cast<uint32_t>(events.size()));
    const int64_t length = step * source.variants + step / 3;
    std::memcpy(b.data() + sub + 8, &length, 8);
    b[sub + 0x10] = source.variants == 3 ? 1 : 0;
    for (size_t k = 0; k < 16; ++k) b[sub + 0x30 + k] = static_cast<uint8_t>(0xa0 + k);
    for (size_t i = 0; i < events.size(); ++i) {
        const auto& e = events[i];
        const size_t at = allocate(b, e.size);
        for (uint32_t k = 16; k < e.size; ++k) b[at + k] = static_cast<uint8_t>(0x30 + (i + k) % 7);  // opaque, copied as is
        std::memcpy(b.data() + at, &e.time, 8);
        std::memcpy(b.data() + at + 8, &e.duration, 8);
        if (std::string(e.type) == "keen::actor::LerpMovementFactorEvent") std::memcpy(b.data() + at + 0x70, &e.lerp, 8);
        if (std::string(e.type) == "keen::actor::SetAnimationEvent") std::memset(b.data() + at + 0x60, 0x11, 16);  // the Fisher's state
        const size_t h = headers + i * 12;
        put32(b, h, hash(e.type));
        put32(b, h + 4, static_cast<uint32_t>(static_cast<int64_t>(at) - static_cast<int64_t>(h) - 4));
        put32(b, h + 8, e.size);
    }
    return b;
}
// The admitted 27-row vector: state GUIDs derived from the role/variant, cooked durations, Sit/Sleep middle loops.
inline kiln::qb::ojson bindings(int64_t duration = 2'000'000'000) {
    kiln::qb::ojson rows = kiln::qb::ojson::array();
    for (const auto& s : kiln::qb::motionclose::fisher_closure())
        for (uint32_t v = 0; v < s.variants; ++v) {
            const bool phased = std::string(s.role) == "sit" || std::string(s.role) == "sleep";
            rows.push_back({{"role", s.role}, {"variant", v}, {"state", kiln::qb::guid_text(kiln::qb::derive_guid("fixture", "state", std::string(s.role) + std::to_string(v)))},
                            {"durationNs", duration + v}, {"loop", phased && v == 1}, {"clamp", false}});
        }
    return rows;
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
