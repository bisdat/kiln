// SPDX-License-Identifier: MIT
// Synthetic timings only. The sit/sleep boundaries are the chapter 16f structural contract, not game asset bytes.
#include "motionpack.h"
#include <algorithm>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

using namespace kiln::qb::motionpack;
namespace {
int checks = 0, failures = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::fprintf(stderr, "%s:%d: FAIL %s\n", __FILE__, __LINE__, #x); } } while (0)
Event event(EventType type, Time time, Time duration = 0) {
    Event e;
    e.type = type;
    e.span = {time, duration};
    return e;
}
Time mapped(const TimeMap& map, Time t) {
    Time out = -1;
    std::string err;
    CHECK(map_time(map, t, &out, &err));
    CHECK(err.empty());
    return out;
}
void three_phases(Time enter, Time exit, Time length, Time first, Time middle, Time last) {
    std::vector<Event> events{event(EventType::SetAnimation, 0), event(EventType::SetAnimation, enter),
                              event(EventType::SetAnimation, exit), event(EventType::Loop, enter, exit - enter),
                              event(EventType::KeepAlive, 0, length)};
    events[3].flags = 0xa5u;
    events[3].local_intervals = {{enter, exit - enter}, {exit, 0}};
    const std::vector<Binding> bindings{{0, first, false, Phase::Enter}, {1, middle, true, Phase::Loop},
                                        {2, last, false, Phase::Exit}};
    Retimed out;
    std::string err;
    CHECK(retime(length, events, bindings, &out, &err));
    CHECK(err.empty());
    if (out.events.size() != events.size() || out.map.knots.size() != 4) { CHECK(false); return; }
    CHECK(out.length == first + middle + last);
    const Time source[]{0, enter, exit, length}, target[]{0, first, first + middle, first + middle + last};
    for (size_t i = 0; i < 4; ++i) {
        CHECK(out.map.knots[i].source == source[i]);
        CHECK(out.map.knots[i].target == target[i]);
        CHECK(mapped(out.map, source[i]) == target[i]);
    }
    CHECK(out.events[0].span.time == 0 && out.events[0].span.duration == 0);
    CHECK(out.events[1].span.time == first && out.events[2].span.time == first + middle);
    CHECK(out.events[3].type == EventType::Loop && out.events[3].flags == 0xa5u);
    CHECK(out.events[3].span.duration == middle);
    CHECK(out.events[3].local_intervals[0].time == first && out.events[3].local_intervals[0].duration == middle);
    CHECK(out.events[3].local_intervals[1].time == first + middle && out.events[3].local_intervals[1].duration == 0);
    CHECK(out.events[4].span.duration == out.length);
    CHECK(events[1].span.time == enter && events[3].span.duration == exit - enter);  // input untouched
}
void sit_sleep() {
    three_phases(1383333361, 7833333490, 8316666833, 2000000000, 3000000000, 1000000000);
    three_phases(2466666716, 12983333593, 13766666942, 1500000000, 4000000000, 750000000);
}
void intervals_and_rounding() {
    std::vector<Event> events{event(EventType::SetAnimation, 0), event(EventType::SetAnimation, 4),
                              event(EventType::LerpMovementFactor, 2, 4), event(EventType::Loop, 4, 6),
                              event(EventType::SetState, 4), event(EventType::SetState, 4)};
    events[2].lerp_duration = 4;
    events[2].local_intervals = {{3, 3}, {0, 10}};
    events[3].flags = 0x80000001u;
    events[4].lerp_duration = -17;  // not a local duration on this event, so must not be interpreted
    const std::vector<Binding> bindings{{0, 2, false}, {1, 12, false}};
    Retimed out;
    std::string err;
    CHECK(retime(10, events, bindings, &out, &err));
    if (out.events.size() != events.size()) { CHECK(false); return; }
    CHECK(out.length == 14);
    CHECK(out.events[2].span.time == 1 && out.events[2].span.duration == 5);  // crosses the knot
    CHECK(out.events[2].lerp_duration == 5);
    CHECK(out.events[2].local_intervals[0].time == 2 && out.events[2].local_intervals[0].duration == 4);
    CHECK(out.events[2].local_intervals[1].time == 0 && out.events[2].local_intervals[1].duration == 14);
    CHECK(out.events[3].span.time == 2 && out.events[3].span.duration == 12 && out.events[3].flags == 0x80000001u);
    CHECK(out.events[4].lerp_duration == -17 && out.events[4].type == events[4].type);
    CHECK(out.events[4].span.time == out.events[5].span.time);  // original equal-time event order kept
    CHECK(mapped(out.map, 1) == 1);  // 0.5 -> 1, not ties-to-even
    CHECK(mapped(out.map, 3) == 2);  // 1.5 -> 2
    CHECK(mapped(out.map, 4) == 2);  // exact knot
    CHECK(mapped(out.map, 0) == 0);
    Time previous = -1;
    for (Time t = 0; t <= 10; ++t) { const Time current = mapped(out.map, t); CHECK(current >= previous); previous = current; }
}
void lead_in_and_guarded_branches() {
    // Door's alternatives share one start, not three consecutive phases. Serialized order and binding order differ.
    std::vector<Event> events{event(EventType::SetState, 2), event(EventType::SetAnimation, 3),
                              event(EventType::SetAnimation, 3), event(EventType::SetAnimation, 3)};
    const std::vector<Binding> bindings{{3, 4, false}, {1, 8, true}, {2, 2, false}};
    Retimed out, again;
    std::string err;
    CHECK(retime(10, events, bindings, &out, &err));
    if (out.events.size() != events.size()) { CHECK(false); return; }
    CHECK(out.map.knots.size() == 3 && out.length == 11);
    for (Time t = 0; t <= 3; ++t) CHECK(mapped(out.map, t) == t);
    for (size_t i = 1; i < events.size(); ++i) CHECK(out.events[i].span.time == 3);
    auto reverse = bindings;
    std::reverse(reverse.begin(), reverse.end());
    CHECK(retime(10, events, reverse, &again, &err));
    CHECK(out.map.knots.size() == again.map.knots.size());
    for (size_t i = 0; i < std::min(out.map.knots.size(), again.map.knots.size()); ++i)
        CHECK(out.map.knots[i].source == again.map.knots[i].source && out.map.knots[i].target == again.map.knots[i].target);
    // Sorting knots must never reorder the event vector itself.
    std::vector<Event> unsorted{event(EventType::SetAnimation, 6), event(EventType::SetAnimation, 0)};
    CHECK(retime(10, unsorted, {{0, 5, false}, {1, 3, false}}, &again, &err));
    if (again.events.size() == 2) CHECK(again.events[0].span.time == 3 && again.events[1].span.time == 0);
    else CHECK(false);
}
void identity_and_whitelist() {
    std::vector<Event> events;
    for (int type = static_cast<int>(EventType::SfxNotifier); type < static_cast<int>(EventType::Unknown); ++type)
        if (static_cast<EventType>(type) != EventType::SetAnimation) events.push_back(event(static_cast<EventType>(type), 2, 3));
    Retimed out;
    std::string err;
    CHECK(retime(10, events, {}, &out, &err));
    CHECK(out.events.size() == events.size() && out.length == 10);
    for (const auto& e : out.events) CHECK(e.span.time == 2 && e.span.duration == 3);
    CHECK(retime(10, {}, {}, &out, &err));
    CHECK(out.length == 10 && out.events.empty());
}
void refused(Time length, std::vector<Event> events, std::vector<Binding> bindings, const char* reason) {
    Retimed out;
    out.length = 123;
    out.events = {event(EventType::Jump, 7)};
    out.map.knots = {{0, 0}, {123, 123}};
    std::string err;
    CHECK(!retime(length, events, bindings, &out, &err));
    CHECK(err.find(reason) != std::string::npos);
    CHECK(out.length == 123 && out.events.size() == 1 && out.events[0].span.time == 7 && out.map.knots.back().target == 123);
    TimeMap map;
    map.knots = {{0, 0}, {123, 123}};
    CHECK(!build_time_map(length, events, bindings, &map, &err));
    CHECK(map.knots.size() == 2 && map.knots.back().target == 123);
}
void invalid_inputs() {
    const auto anim = event(EventType::SetAnimation, 0);
    refused(0, {}, {}, "length");
    refused(-1, {}, {}, "length");
    refused(10, {event(EventType::Unknown, 0)}, {}, "events[0].type");
    refused(10, {event(static_cast<EventType>(-1), 0)}, {}, "events[0].type");
    refused(10, {event(EventType::Jump, -1)}, {}, "events[0].time");
    refused(10, {event(EventType::Jump, 0, -1)}, {}, "events[0].duration");
    refused(10, {event(EventType::Jump, 11)}, {}, "events[0].time");
    refused(10, {event(EventType::Jump, 9, 2)}, {}, "events[0].duration");
    refused(10, {event(EventType::Jump, 1, std::numeric_limits<Time>::max())}, {}, "events[0].duration");
    refused(10, {anim}, {}, "binding");
    refused(10, {anim}, {{0, 0, false}}, "duration");
    refused(10, {anim}, {{0, -1, false}}, "duration");
    refused(10, {anim}, {{1, 2, false}}, "event");
    refused(10, {event(EventType::Jump, 0)}, {{0, 2, false}}, "SetAnimation");
    refused(10, {anim}, {{0, 2, false}, {0, 3, false}}, "duplicate");
    refused(10, {event(EventType::SetAnimation, 10)}, {{0, 2, false}}, "final animation knot");
    refused(10, {anim}, {{0, 2, true, Phase::Enter}}, "loop");
    refused(10, {anim}, {{0, 2, true, Phase::Exit}}, "loop");
    refused(10, {anim}, {{0, 2, false, Phase::Loop}}, "loop");
    refused(10, {anim}, {{0, 2, false, static_cast<Phase>(-1)}}, "phase");
    auto bad = event(EventType::LerpMovementFactor, 2);
    bad.lerp_duration = -1;
    refused(10, {bad}, {}, "lerpDuration");
    bad.lerp_duration = 9;
    refused(10, {bad}, {}, "lerpDuration");
    bad = event(EventType::Loop, 0);
    bad.local_intervals = {{-1, 2}};
    refused(10, {bad}, {}, "localIntervals[0].time");
    bad.local_intervals = {{2, 9}};
    refused(10, {bad}, {}, "localIntervals[0].duration");
    const Time max = std::numeric_limits<Time>::max();
    refused(10, {anim, event(EventType::SetAnimation, 5)}, {{0, max, false}, {1, 1, false}}, "overflow");
    refused(10, {event(EventType::SetAnimation, 1)}, {{0, max, false}}, "overflow");
}
void public_helper_validation_and_limits() {
    std::string err = "stale";
    Time out = 77;
    for (const auto& map : std::vector<TimeMap>{{}, {{{0, 0}}}, {{{1, 0}, {2, 1}}}, {{{0, 1}, {2, 2}}},
                                               {{{0, 0}, {0, 1}}}, {{{0, 0}, {2, 0}}}, {{{0, 0}, {2, 2}, {1, 3}}}}) {
        CHECK(!map_time(map, 0, &out, &err));
        CHECK(out == 77 && !err.empty());
    }
    CHECK(!map_time({{{0, 0}, {10, 10}}}, -1, &out, &err));
    CHECK(!map_time({{{0, 0}, {10, 10}}}, 11, &out, &err));
    CHECK(!map_time({{{0, 0}, {10, 10}}}, 0, nullptr, &err));
    CHECK(!retime(10, {}, {}, nullptr, &err));
    CHECK(!build_time_map(10, {}, {}, nullptr, &err));
    CHECK(map_time({{{0, 0}, {10, 10}}}, 2, &out, &err));
    CHECK(out == 2 && err.empty());
    CHECK(map_time({{{0, 0}, {10, 10}}}, 3, &out, nullptr));
    const Time max = std::numeric_limits<Time>::max();
    TimeMap map;
    CHECK(build_time_map(10, {event(EventType::SetAnimation, 0)}, {{0, max, false}}, &map, &err));
    if (!map.knots.empty()) {
        CHECK(mapped(map, 10) == max);  // exact endpoint must not pass through a lossy double conversion
        const Time middle = mapped(map, 5);
        CHECK(middle > 0 && middle < max);
    }
    CHECK(build_time_map(max, {}, {}, &map, &err));
    if (!map.knots.empty()) CHECK(mapped(map, max) == max);
    // Large absolute lead-in: interpolate an offset, not a lossy double absolute nanosecond timestamp.
    map.knots = {{0, 0}, {max - 10, max - 10}, {max, max}};
    CHECK(mapped(map, max - 5) == max - 5);
}
}  // namespace
int main() {
    sit_sleep();
    intervals_and_rounding();
    lead_in_and_guarded_branches();
    identity_and_whitelist();
    invalid_inputs();
    public_helper_validation_and_limits();
    std::printf("motionpack: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
