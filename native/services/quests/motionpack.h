// SPDX-License-Identifier: MIT
// Internal NPC motion retiming, since 0.2.17. Pure values only: no game memory, resource parsing or mod ABI.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace kiln::qb::motionpack {
using Time = int64_t;  // integer nanoseconds

// The byte parser must validate each layout before projecting its local time fields here. This is the complete
// chapter 16f v1 whitelist, not permission to traverse arbitrary Time fields (cooldowns/world times are not local).
enum class EventType {
    SfxNotifier, VoiceoverNotifier, DisableAbility, EnableAbility, Jump, KeepAlive,
    LerpMovementFactor, LerpRotationFactor, Loop, RotationLerp, SetAbortByMovement,
    SetAnimation, SetState, ToggleInteractionAction, TriggerKnowledge, TriggerNoise, Unknown
};
struct Span {
    Time time = 0, duration = 0;
};
struct Event {
    EventType type = EventType::Unknown;
    Span span;
    Time lerp_duration = 0;  // local duration only for LerpMovementFactor; otherwise copied, not interpreted
    std::vector<Span> local_intervals;  // only additional local endpoints identified by the verified event layout
    uint32_t flags = 0;  // opaque to this helper, including loop flags; copied without interpretation
};
enum class Phase { Any, Enter, Loop, Exit };
struct Binding {
    size_t event = 0;  // serialized event ordinal in THIS SubSequence, never a target-state GUID lookup
    Time duration = 0;  // effective cooked durationNs: frameCount/fps/Speed, already rounded half-away
    bool loop = false;
    Phase phase = Phase::Any;  // Sit/Sleep variants 0/1/2 -> Enter/Loop/Exit; Door branches -> Any
};
struct Knot {
    Time source = 0, target = 0;
};
struct TimeMap {
    std::vector<Knot> knots;
};
struct Retimed {
    Time length = 0;
    TimeMap map;
    std::vector<Event> events;
};

// Simultaneous guarded animations share a knot and use the longest target duration. No animation -> identity.
// All functions refuse invalid input with a reason. Failure leaves the output untouched. Each SubSequence is
// processed independently; bindings must cover every SetAnimation event exactly once. Source/event order is kept.
bool build_time_map(Time length, const std::vector<Event>& events, const std::vector<Binding>& bindings,
                    TimeMap* out, std::string* err);
bool map_time(const TimeMap& map, Time time, Time* out, std::string* err);
// Durations are F(end) - F(start), never a global rate; zero, exact knots and the unchanged lead-in stay exact.
bool retime(Time length, const std::vector<Event>& events, const std::vector<Binding>& bindings,
            Retimed* out, std::string* err);
}  // namespace kiln::qb::motionpack
