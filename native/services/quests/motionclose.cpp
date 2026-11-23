// SPDX-License-Identifier: MIT
// 16f S4 motion closure (motionclose.h). Offsets are the verified layouts listed in bodypack.cpp body_layout().
#include "motionclose.h"

#include <algorithm>
#include <cstring>
#include <set>

namespace kiln::qb::motionclose {
namespace {
using motionpack::EventType;
using motionpack::Time;

uint32_t hash(const char* name) { return fnv1a(name, std::strlen(name)); }
uint32_t read32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
int64_t read64(const uint8_t* p) { int64_t v; std::memcpy(&v, p, 8); return v; }
void put64(Bytes& b, size_t at, int64_t v) { std::memcpy(b.data() + at, &v, 8); }
bool range(size_t len, size_t at, size_t size) { return at <= len && size <= len - at; }
// A BlobArray field: rel32 from the field, count. Returns the element base.
bool blob_array(const uint8_t* b, size_t len, size_t field, size_t stride, uint32_t max, size_t* at, uint32_t* count) {
    if (!range(len, field, 8)) return false;
    *count = read32(b + field + 4);
    if (!*count) { *at = 0; return true; }
    const int64_t target = static_cast<int64_t>(field) + static_cast<int32_t>(read32(b + field));
    if (*count > max || target < 0 || target > static_cast<int64_t>(len)) return false;
    *at = static_cast<size_t>(target);
    return *count <= (len - *at) / stride;
}

struct EventKind { const char* name; uint32_t size; EventType type; };
// Chapter 16f section 5.1's verified whitelist, with the reflected sizes of client 5d9ef2f1 / server f0d824be.
constexpr EventKind kEvents[] = {
    {"keen::SfxNotifierEvent", 168, EventType::SfxNotifier}, {"keen::VoiceoverNotifierEvent", 168, EventType::VoiceoverNotifier},
    {"keen::actor::DisableAbilityEvent", 120, EventType::DisableAbility}, {"keen::actor::EnableAbilityEvent", 120, EventType::EnableAbility},
    {"keen::actor::JumpEvent", 96, EventType::Jump}, {"keen::actor::KeepAliveEvent", 112, EventType::KeepAlive},
    {"keen::actor::LerpMovementFactorEvent", 120, EventType::LerpMovementFactor},
    {"keen::actor::LerpRotationFactorEvent", 104, EventType::LerpRotationFactor}, {"keen::actor::LoopEvent", 120, EventType::Loop},
    {"keen::actor::RotationLerpEvent", 104, EventType::RotationLerp}, {"keen::actor::SetAbortByMovement", 104, EventType::SetAbortByMovement},
    {"keen::actor::SetAnimationEvent", 120, EventType::SetAnimation}, {"keen::actor::SetStateEvent", 104, EventType::SetState},
    {"keen::actor::ToggleInteractionActionEvent", 96, EventType::ToggleInteractionAction},
    {"keen::actor::TriggerKnowledge", 104, EventType::TriggerKnowledge}, {"keen::actor::TriggerNoiseEvent", 112, EventType::TriggerNoise},
};
constexpr size_t kAnimationName = 0x60, kLerpDuration = 0x70;  // SetAnimationEvent.animationName, LerpMovementFactorEvent.lerpDuration
constexpr size_t kSubSequence = 72, kSourceSubSequenceId = 0x30, kLength = 8;  // ActorSubSequenceResource : SequenceResource

constexpr Source kClosure[] = {
    {"2328fd07-2fa3-40fb-8b11-6e5240ab67eb", "sit", 3}, {"a7f06962-3245-4ec9-90ee-e276d3e137fc", "sleep", 3},
    {"1894e958-fafe-4117-b075-9bc6782e0d37", "inspect", 1}, {"9a9eb210-caeb-4127-b844-0154470d329b", "greet", 1},
    {"6a6e9bfc-7f01-446c-8180-95974cca991f", "talk", 1}, {"34aadfa4-45b3-4e95-b9a0-a8643d2a4e2c", "reaction0", 1},
    {"e5961c9f-a80b-4822-b45e-c7cd92f6c384", "reaction1", 1}, {"d4ce1c4b-da12-42f7-9578-f3fca76a2d26", "refuse", 1},
    {"99b2e7de-abfd-4ed0-9c5f-d1a3fa4d12b9", "idleGesture0", 1}, {"c6717834-835d-40fb-a388-ace17b91ad67", "idleGesture1", 1},
    {"0fe65220-fbed-49ec-a018-83a939898bb6", "interaction", 1}, {"ff01e568-4c43-4449-8c8c-2a05a3ba7f56", "greetingExtra", 1},
    {"992bc2ea-e3a3-4dba-a2b3-6d71dab33f62", "farewell", 1}, {"ae956af7-0e66-455d-bc7e-224ba33e3562", "dialogOption", 1},
    {"017a6e8f-1913-4749-99fd-59d0196e2780", "attentionGreeting", 1}, {"2a641062-db27-47b7-bb52-2f6d2dabd2b2", "turnLeftSmall", 1},
    {"fca98e03-62e4-415c-9650-88dca7c5a323", "turnLeftBig", 1}, {"e014aaa1-2443-4012-b044-484a318c591b", "turnRightSmall", 1},
    {"81384ea0-18cd-4f79-96ea-a932e96382a8", "turnRightBig", 1}, {"dc8df8a9-844f-42ed-afd9-b3778df94fff", "jump", 1},
    {"241feba6-9ea8-43b2-b10f-e93f3573a697", "door", 3},
};

// The template's sequence references (verified offsets, bodypack.cpp body_layout()): direct fields and BlobArrays.
struct Direct { const char* path; size_t offset; };
struct Nested { const char* path; size_t field; size_t stride; size_t ref; };
struct Holder { const char* component; uint32_t size; std::vector<Direct> direct; std::vector<Nested> nested; };
const std::vector<Holder>& holders() {
    static const std::vector<Holder> h{
        {"keen::ecs::ActiveNpcState", 456, {{"sitSequence", 0x4}, {"sleepSequence", 0x14}, {"inspectSequence", 0x24}},
         {{"socializingSetup.sequences.greetings", 0x1b0, 16, 0}, {"socializingSetup.sequences.talking", 0x1b8, 16, 0},
          {"socializingSetup.sequences.reactions", 0x1c0, 16, 0}}},
        {"keen::ecs::NpcState", 128, {{"defaultGreetingSequence", 0}, {"attentionGreetingSequence", 0x10}, {"turnSetup.turnLeftSmallSequence", 0x20},
                                      {"turnSetup.turnLeftBigSequence", 0x30}, {"turnSetup.turnRightSmallSequence", 0x40},
                                      {"turnSetup.turnRightBigSequence", 0x50}}, {}},
        {"keen::ecs::NpcActionSequences", 32, {{"defaultInteractionSequence", 0}},
         {{"idleSequences.sequences", 0x10, 16, 0}, {"interactionSequences.sequences", 0x18, 32, 0x10}}},
        {"keen::ecs::NpcDialog", 120, {{"dialogResource.defaultSequence", 0x28}, {"dialogResource.missingRequirementSequence", 0x38}},
         {{"dialogResource.initLines", 0x48, 68, 8}, {"dialogResource.transitionLines", 0x50, 64, 8},
          {"dialogResource.farewellLines", 0x58, 64, 8}, {"dialogResource.dialogOptions", 0x60, 68, 8}}},
        {"keen::ecs::PathWalker", 92, {{"jumpSequence", 0}, {"openDoorSequence", 0x10}, {"teleportSequence", 0x24}}, {}},
    };
    return h;
}
}  // namespace

std::span<const Source> fisher_closure() { return kClosure; }

bool parse_bindings(const nlohmann::ordered_json& rows, Bindings* out, std::string* err) {
    const auto fail = [&](const std::string& why) { if (err) *err = "motions: " + why; return false; };
    if (!out) return fail("no output");
    if (!rows.is_array() || rows.size() != 27) return fail("the admitted binding vector must have the 27 rows of the Fisher manifest");
    Bindings next;
    for (const auto& s : kClosure) next[s.role].assign(s.variants, Bound{});
    std::set<std::pair<std::string, uint32_t>> seen;
    for (size_t i = 0; i < rows.size(); ++i) {
        const auto& r = rows[i];
        const std::string at = "bindings[" + std::to_string(i) + "]";
        if (!r.is_object() || !r.contains("role") || !r["role"].is_string() || !r.contains("variant") || !r["variant"].is_number_integer() ||
            r["variant"].get<int64_t>() < 0 || r["variant"].get<int64_t>() > 255)
            return fail(at + ": role and variant are required");
        const std::string role = r["role"].get<std::string>();
        const uint32_t variant = r["variant"].get<uint32_t>();
        const auto slot = next.find(role);
        if (slot == next.end() || variant >= slot->second.size()) return fail(at + ": " + role + "[" + std::to_string(variant) + "] is not in the Fisher manifest");
        if (!seen.emplace(role, variant).second) return fail(at + ": duplicate " + role + "[" + std::to_string(variant) + "]");
        Bound b;
        if (!r.contains("state") || !r["state"].is_string() || !parse_guid(r["state"].get<std::string>(), &b.state) || b.state.zero())
            return fail(at + ".state: a nonzero cooked state GUID is required");
        if (!r.contains("durationNs") || !r["durationNs"].is_number_integer() || r["durationNs"].get<int64_t>() <= 0)
            return fail(at + ".durationNs: a positive cooked duration is required");
        b.duration = r["durationNs"].get<int64_t>();
        b.loop = r.value("loop", false) == true;
        b.clamp = r.value("clamp", false) == true;
        slot->second[variant] = b;
    }
    *out = std::move(next);
    if (err) err->clear();
    return true;
}

bool clone_sequence(const uint8_t* res, size_t len, const Guid& clone, const Source& source, const std::vector<Bound>& variants,
                    const std::function<Guid(const Guid&)>& sub_id, Bytes* out, std::string* err) {
    const std::string where = std::string("motions.") + source.role + " (" + source.guid + ")";
    const auto fail = [&](const std::string& why) { if (err) *err = where + ": " + why; return false; };
    if (!out || !res || len < 24 || len > kMaxSequenceBytes) return fail("the sequence must be 24 bytes to 16 MiB");
    if (variants.size() != source.variants) return fail("the binding vector has the wrong number of variants");
    const bool phased = std::strcmp(source.role, "sit") == 0 || std::strcmp(source.role, "sleep") == 0;
    Bytes b(res, res + len);
    size_t subs = 0;
    uint32_t n_subs = 0;
    if (!blob_array(res, len, 16, kSubSequence, 64, &subs, &n_subs) || !n_subs) return fail("its sub-sequence array is invalid");
    std::memcpy(b.data(), clone.b, 16);
    size_t ordinal = 0;
    for (uint32_t i = 0; i < n_subs; ++i) {
        const size_t base = subs + size_t{i} * kSubSequence;
        size_t headers = 0;
        uint32_t n_events = 0;
        if (!blob_array(res, len, base, 12, 1024, &headers, &n_events)) return fail("sub-sequence " + std::to_string(i) + ": its event array is invalid");
        Guid old_sub;
        std::memcpy(old_sub.b, res + base + kSourceSubSequenceId, 16);
        if (!old_sub.zero()) {
            const Guid next = sub_id(old_sub);
            std::memcpy(b.data() + base + kSourceSubSequenceId, next.b, 16);
        }
        std::vector<motionpack::Event> events;
        std::vector<motionpack::Binding> bindings;
        std::vector<size_t> payloads;
        std::vector<std::pair<size_t, Guid>> names;
        for (uint32_t j = 0; j < n_events; ++j) {
            const size_t h = headers + size_t{j} * 12;
            const uint32_t type = read32(res + h), size = read32(res + h + 8);
            const int64_t at = static_cast<int64_t>(h) + 4 + static_cast<int32_t>(read32(res + h + 4));
            const auto kind = std::find_if(std::begin(kEvents), std::end(kEvents), [&](const EventKind& k) { return hash(k.name) == type; });
            const std::string ev = "sub-sequence " + std::to_string(i) + " event " + std::to_string(j);
            if (kind == std::end(kEvents)) return fail(ev + ": an event type outside the verified whitelist");
            if (size != kind->size) return fail(ev + ": " + kind->name + " changed size");
            if (at < 0 || !range(len, static_cast<size_t>(at), size)) return fail(ev + ": its payload is outside the sequence");
            const size_t p = static_cast<size_t>(at);
            motionpack::Event e;
            e.type = kind->type;
            e.span = {read64(res + p), read64(res + p + 8)};
            if (e.type == EventType::LerpMovementFactor) e.lerp_duration = read64(res + p + kLerpDuration);
            if (e.type == EventType::SetAnimation) {
                if (ordinal >= variants.size()) return fail("it has more animation events than the manifest's " + std::to_string(variants.size()));
                const Bound& v = variants[ordinal];
                const motionpack::Phase phase = !phased ? motionpack::Phase::Any
                                              : ordinal == 0 ? motionpack::Phase::Enter : ordinal == 1 ? motionpack::Phase::Loop : motionpack::Phase::Exit;
                bindings.push_back({j, v.duration, v.loop, phase});
                names.emplace_back(p + kAnimationName, v.state);
                ++ordinal;
            }
            events.push_back(e);
            payloads.push_back(p);
        }
        motionpack::Retimed retimed;
        std::string why;
        if (!motionpack::retime(read64(res + base + kLength), events, bindings, &retimed, &why))
            return fail("sub-sequence " + std::to_string(i) + ": " + why);
        put64(b, base + kLength, retimed.length);
        for (size_t j = 0; j < payloads.size(); ++j) {
            put64(b, payloads[j], retimed.events[j].span.time);
            put64(b, payloads[j] + 8, retimed.events[j].span.duration);
            if (retimed.events[j].type == EventType::LerpMovementFactor) put64(b, payloads[j] + kLerpDuration, retimed.events[j].lerp_duration);
        }
        for (const auto& [at, state] : names) std::memcpy(b.data() + at, state.b, 16);
    }
    if (ordinal != variants.size())
        return fail("it has " + std::to_string(ordinal) + " animation events, the manifest " + std::to_string(variants.size()));
    *out = std::move(b);
    if (err) err->clear();
    return true;
}

bool remap_template(Bytes* tpl, const std::map<std::string, Guid>& map, size_t* replaced, std::string* err) {
    const auto fail = [&](const std::string& why) { if (err) *err = "motions: " + why; return false; };
    if (!tpl || tpl->size() < 24) return fail("no template");
    Bytes b = *tpl;
    const uint8_t* r = tpl->data();
    const size_t len = tpl->size();
    size_t headers = 0;
    uint32_t count = 0;
    if (!blob_array(r, len, 16, 12, 512, &headers, &count)) return fail("the template's component array is invalid");
    size_t n = 0;
    const auto one = [&](const std::string& path, size_t at) {
        if (!range(len, at, 16)) return fail(path + ": outside the template");
        Guid g;
        std::memcpy(g.b, r + at, 16);
        if (g.zero()) return true;
        const auto found = map.find(guid_text(g));
        if (found == map.end()) return fail(path + ": sequence " + guid_text(g) + " is not in the verified Fisher motion closure");
        std::memcpy(b.data() + at, found->second.b, 16);
        ++n;
        return true;
    };
    for (const auto& holder : holders()) {
        size_t at = 0, found = 0;
        for (uint32_t i = 0; i < count; ++i) {
            const size_t h = headers + size_t{i} * 12;
            if (read32(r + h) != hash(holder.component)) continue;
            const int64_t p = static_cast<int64_t>(h) + 4 + static_cast<int32_t>(read32(r + h + 4));
            if (read32(r + h + 8) != holder.size || p < 0 || !range(len, static_cast<size_t>(p), holder.size))
                return fail(std::string(holder.component) + ": missing or changed");
            at = static_cast<size_t>(p);
            ++found;
        }
        if (found != 1) return fail(std::string(holder.component) + ": the chassis must have exactly one");
        const std::string name = std::string(holder.component).substr(std::strlen("keen::ecs::"));
        for (const auto& d : holder.direct)
            if (!one(name + "." + d.path, at + d.offset)) return false;
        for (const auto& a : holder.nested) {
            size_t elements = 0;
            uint32_t m = 0;
            if (!blob_array(r, len, at + a.field, a.stride, 4096, &elements, &m)) return fail(name + "." + a.path + ": an invalid array");
            for (uint32_t k = 0; k < m; ++k)
                if (!one(name + "." + a.path + "[" + std::to_string(k) + "]", elements + size_t{k} * a.stride + a.ref)) return false;
        }
    }
    // A source id anywhere else (a path a game update added, a component outside the verified five) refuses.
    for (const auto& [source, cloned] : map) {
        Guid g;
        parse_guid(source, &g);
        const auto hit = std::search(b.begin(), b.end(), std::begin(g.b), std::end(g.b));
        if (hit != b.end())
            return fail("a reference to sequence " + source + " remains at byte " + std::to_string(hit - b.begin()) + " outside the verified fields");
    }
    *tpl = std::move(b);
    if (replaced) *replaced = n;
    if (err) err->clear();
    return true;
}
}  // namespace kiln::qb::motionclose
