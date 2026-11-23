// SPDX-License-Identifier: MIT
#include "motionpack.h"
#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <map>
#include <utility>

namespace kiln::qb::motionpack {
namespace {
bool fail(std::string* err, const std::string& reason) {
    if (err) *err = reason;
    return false;
}
void success(std::string* err) { if (err) err->clear(); }
bool supported(EventType type) {
    switch (type) {
        case EventType::SfxNotifier: case EventType::VoiceoverNotifier: case EventType::DisableAbility:
        case EventType::EnableAbility: case EventType::Jump: case EventType::KeepAlive:
        case EventType::LerpMovementFactor: case EventType::LerpRotationFactor: case EventType::Loop:
        case EventType::RotationLerp: case EventType::SetAbortByMovement: case EventType::SetAnimation:
        case EventType::SetState: case EventType::ToggleInteractionAction: case EventType::TriggerKnowledge:
        case EventType::TriggerNoise: return true;
        default: return false;
    }
}
bool valid_span(const Span& span, Time length, const std::string& path, std::string* err) {
    if (span.time < 0 || span.time > length) return fail(err, path + ".time: outside the SubSequence");
    // Subtract first, so even an untrusted INT64_MAX duration cannot overflow the endpoint check.
    if (span.duration < 0 || span.duration > length - span.time)
        return fail(err, path + ".duration: negative or ends beyond the SubSequence");
    return true;
}
bool valid_map(const TimeMap& map, std::string* err) {
    if (map.knots.size() < 2 || map.knots.front().source != 0 || map.knots.front().target != 0)
        return fail(err, "time map: must start at zero and contain at least two knots");
    for (size_t i = 1; i < map.knots.size(); ++i)
        if (map.knots[i].source <= map.knots[i - 1].source || map.knots[i].target <= map.knots[i - 1].target)
            return fail(err, "time map: source and target intervals must be strictly monotone");
    return true;
}
// Only called on a validated map with time in [0,D]. Preserve integer endpoints exactly, including values above
// double's exact-integer range. Interpolate a relative offset, then add the integral base without losing low bits.
Time mapped(const TimeMap& map, Time time) {
    const auto hi = std::lower_bound(map.knots.begin(), map.knots.end(), time,
                                     [](const Knot& k, Time t) { return k.source < t; });
    if (hi->source == time) return hi->target;
    const auto& lo = *(hi - 1);
    const Time source_span = hi->source - lo.source, target_span = hi->target - lo.target;
    if (source_span == target_span) return lo.target + (time - lo.source);  // lead-in / identity stays exact
    const double fraction = static_cast<double>(time - lo.source) / static_cast<double>(source_span);
    const double offset = std::round(fraction * static_cast<double>(target_span));  // half away from zero
    // Rounded doubles can hit 2^63 when a valid target span is INT64_MAX. Clamp BEFORE converting to int64_t.
    if (offset >= static_cast<double>(target_span)) return hi->target;
    return lo.target + static_cast<Time>(offset);
}
Span mapped_span(const TimeMap& map, const Span& span) {
    const Time start = mapped(map, span.time);
    return {start, mapped(map, span.time + span.duration) - start};
}
}  // namespace

bool build_time_map(Time length, const std::vector<Event>& events, const std::vector<Binding>& bindings,
                    TimeMap* out, std::string* err) {
    if (!out) return fail(err, "time map: output is null");
    if (length <= 0) return fail(err, "SubSequence.length: must be positive");
    for (size_t i = 0; i < events.size(); ++i) {
        const auto& e = events[i];
        const std::string path = "events[" + std::to_string(i) + "]";
        if (!supported(e.type)) return fail(err, path + ".type: outside the verified event whitelist");
        if (!valid_span(e.span, length, path, err)) return false;
        if (e.type == EventType::LerpMovementFactor &&
            (e.lerp_duration < 0 || e.lerp_duration > length - e.span.time))
            return fail(err, path + ".lerpDuration: negative or ends beyond the SubSequence");
        for (size_t j = 0; j < e.local_intervals.size(); ++j)
            if (!valid_span(e.local_intervals[j], length, path + ".localIntervals[" + std::to_string(j) + "]", err)) return false;
    }
    std::vector<bool> bound(events.size(), false);
    std::map<Time, Time> starts;
    for (size_t i = 0; i < bindings.size(); ++i) {
        const auto& b = bindings[i];
        const std::string path = "bindings[" + std::to_string(i) + "]";
        if (b.event >= events.size()) return fail(err, path + ".event: ordinal outside the SubSequence");
        if (events[b.event].type != EventType::SetAnimation) return fail(err, path + ".event: not a SetAnimation event");
        if (bound[b.event]) return fail(err, path + ".event: duplicate binding");
        if (b.duration <= 0) return fail(err, path + ".duration: cooked durationNs must be positive");
        switch (b.phase) {
            case Phase::Enter: case Phase::Exit:
                if (b.loop) return fail(err, path + ".loop: entry and exit must not loop");
                break;
            case Phase::Loop:
                if (!b.loop) return fail(err, path + ".loop: Sit/Sleep middle variants must loop");
                break;
            case Phase::Any: break;
            default: return fail(err, path + ".phase: unknown phase");
        }
        const Time start = events[b.event].span.time;
        if (start >= length) return fail(err, path + ".event: final animation knot must be before SubSequence.length");
        bound[b.event] = true;
        starts[start] = std::max(starts[start], b.duration);
    }
    for (size_t i = 0; i < events.size(); ++i)
        if (events[i].type == EventType::SetAnimation && !bound[i])
            return fail(err, "events[" + std::to_string(i) + "]: missing animation binding");

    TimeMap next;
    next.knots.push_back({0, 0});
    if (starts.empty()) next.knots.push_back({length, length});
    else {
        Time target = starts.begin()->first;  // unchanged lead-in
        if (target) next.knots.push_back({target, target});
        for (auto it = starts.begin(); it != starts.end(); ++it) {
            const auto after = std::next(it);
            const Time source_end = after == starts.end() ? length : after->first;
            if (it->second > std::numeric_limits<Time>::max() - target)
                return fail(err, "SubSequence.length: retimed duration overflow");
            target += it->second;
            next.knots.push_back({source_end, target});
        }
    }
    *out = std::move(next);
    success(err);
    return true;
}

bool map_time(const TimeMap& map, Time time, Time* out, std::string* err) {
    if (!out) return fail(err, "time map: output is null");
    if (!valid_map(map, err)) return false;
    if (time < 0 || time > map.knots.back().source) return fail(err, "time map: local time outside the SubSequence");
    *out = mapped(map, time);
    success(err);
    return true;
}

bool retime(Time length, const std::vector<Event>& events, const std::vector<Binding>& bindings,
            Retimed* out, std::string* err) {
    if (!out) return fail(err, "retiming: output is null");
    Retimed next;
    if (!build_time_map(length, events, bindings, &next.map, err)) return false;
    next.length = next.map.knots.back().target;
    next.events = events;  // keep serialized order, flags and all non-local numeric values
    for (auto& e : next.events) {
        if (e.type == EventType::LerpMovementFactor)
            e.lerp_duration = mapped_span(next.map, {e.span.time, e.lerp_duration}).duration;
        e.span = mapped_span(next.map, e.span);
        for (auto& span : e.local_intervals) span = mapped_span(next.map, span);
    }
    *out = std::move(next);
    success(err);
    return true;
}
}  // namespace kiln::qb::motionpack
