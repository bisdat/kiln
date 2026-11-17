// SPDX-License-Identifier: MIT
// kiln.stations: the service's logic without game memory (host test tests/stations_test.cpp): request checks, the fill's
// arithmetic (the game's own: AutoTransferRecipe, docs/re/stations.md §3.1), a base's id from its altars, the state diff
// behind the events and the event ring. The game side is stations.cpp.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <vector>

#include "services/kiln_stations.h"

namespace kitems::stations {

constexpr uint32_t kMaxCrafts = 1000;
constexpr uint32_t kGameMaxCount = 0xFFFF;  // the game clamps what one input may move to a u16
constexpr uint32_t kMaxPlayerEntity = 1023;  // player entities, as kiln.items (engine.h kMaxPlayer)
constexpr uint32_t kWaterItem = 0;           // InputNow.item of a recipe's water (a pseudo input)
constexpr uint32_t kWaterUnitsPerCharge = 64;  // Fuel units per water charge (kiln_items.h)

// FILL's player: 0, or a player entity. Another entity would make the game's fill take from that entity's inventories.
inline bool player_ok(uint32_t p) { return p == 0 || p <= kMaxPlayerEntity; }

// ---- requests ----
inline bool any_choice(const KilnStationChoice* c) {
    for (int i = 0; i < 4; ++i)
        if (c[i].category != 0) return true;
    return false;
}

inline bool request_valid(const KilnStationRequest& r) {
    if (r.size < sizeof(KilnStationRequest) || r.station == 0) return false;
    for (uint32_t x : r.reserved)
        if (x) return false;
    for (const auto& c : r.choices)
        if (c.reserved[0] || c.reserved[1] || c.reserved[2]) return false;
    constexpr uint32_t all = KILN_STATION_FILL_ADD | KILN_STATION_FILL_EXACT | KILN_STATION_DRY_RUN | KILN_STATION_NO_WATER |
                             KILN_STATION_AFTER_CRAFT | KILN_STATION_KEEP_STOPPED | KILN_STATION_ALLOW_LOCKED;
    if (r.flags & ~all) return false;
    switch (r.action) {
    case KILN_STATION_FILL: {
        constexpr uint32_t ok = KILN_STATION_FILL_ADD | KILN_STATION_FILL_EXACT | KILN_STATION_DRY_RUN | KILN_STATION_NO_WATER;
        if (r.flags & ~ok) return false;
        if (r.crafts < 1 || r.crafts > kMaxCrafts) return false;
        if (r.keep_count > KILN_STATIONS_MAX_KEEP || (r.keep_count && !r.keep)) return false;
        if (any_choice(r.choices) && r.recipe == 0) return false;  // choices name a recipe's categories
        return player_ok(r.player);
    }
    case KILN_STATION_STOP:
        return (r.flags & ~KILN_STATION_AFTER_CRAFT) == 0 && r.crafts == 0 && r.recipe == 0 && r.keep_count == 0 && r.player == 0;
    case KILN_STATION_RESUME:
        return r.flags == 0 && r.crafts == 0 && r.recipe == 0 && r.keep_count == 0 && r.player == 0;
    case KILN_STATION_SELECT:
        return (r.flags & ~(KILN_STATION_KEEP_STOPPED | KILN_STATION_ALLOW_LOCKED)) == 0 && r.crafts == 0 && r.keep_count == 0 &&
               r.player == 0;  // recipe 0 clears the selection
    default:
        return false;
    }
}

// ---- the fill's arithmetic ----
// One resolved input: `count` per craft, `have` in the station (every link, partial stacks too), `stock` what the fill may
// take (the base's stock containers after the reserves, plus the named player's inventories), `kept` what the reserves
// held back, `room` the most the station can hold of it (water: its capacity; items: not modelled). The recipe's water is
// an input too (item kWaterItem, count = units per craft): the game counts it in "loaded" (docs/re/stations.md §3.1).
struct InputNow {
    uint32_t item = 0;
    uint32_t count = 0;
    uint64_t have = 0;
    uint64_t stock = 0;
    uint64_t kept = 0;
    uint64_t room = UINT64_MAX;
};

// The water input of a recipe with `charges` per craft (none when 0, or when the request pours no water).
inline bool water_input(uint32_t charges, bool no_water, uint64_t station_water, uint64_t station_max, uint64_t base_water,
                        InputNow* out) {
    if (charges == 0 || no_water) return false;
    *out = InputNow{};
    out->item = kWaterItem;
    out->count = charges * kWaterUnitsPerCharge;
    out->have = station_water;
    out->stock = base_water;
    out->room = station_max;
    return true;
}

// Inputs naming the same item more than once (an item and a category option that is that item) become one: the station
// holds one pile of it, and the stock is taken once.
inline void merge_inputs(std::vector<InputNow>* in) {
    std::vector<InputNow> out;
    for (const InputNow& i : *in) {
        auto it = std::find_if(out.begin(), out.end(), [&](const InputNow& o) { return o.item == i.item; });
        if (it == out.end()) out.push_back(i);
        else it->count += i.count;
    }
    in->swap(out);
}

// Complete crafts the station holds: min over inputs of have / count; 0 when an input's count is 0 (as the game).
inline uint32_t crafts_loaded(const std::vector<InputNow>& in) {
    if (in.empty()) return 0;
    uint64_t m = UINT64_MAX;
    for (const auto& i : in) {
        if (i.count == 0) return 0;
        m = std::min<uint64_t>(m, i.have / i.count);
    }
    return static_cast<uint32_t>(std::min<uint64_t>(m, UINT32_MAX));
}

// What the game moves for input i when asked for n more crafts: (n + loaded) * count - have, clamped 0..0xFFFF.
inline uint32_t game_need(const InputNow& i, uint32_t n, uint32_t loaded) {
    const int64_t want = static_cast<int64_t>(static_cast<uint64_t>(n) + loaded) * i.count - static_cast<int64_t>(i.have);
    if (want <= 0) return 0;
    return static_cast<uint32_t>(std::min<int64_t>(want, kGameMaxCount));
}

// More crafts the stock covers on top of `loaded`: the largest n with game_need(i, n) <= stock for every input.
inline uint32_t crafts_affordable(const std::vector<InputNow>& in, uint32_t loaded) {
    if (in.empty()) return 0;
    uint64_t best = UINT64_MAX;
    for (const auto& i : in) {
        if (i.count == 0) continue;  // the game moves nothing for it
        const uint64_t total = (i.have + i.stock) / i.count;
        const uint64_t more = total > loaded ? total - loaded : 0;
        // the u16 clamp: n never asks more than 0xFFFF of one input per call
        const uint64_t cap = (static_cast<uint64_t>(kGameMaxCount) + i.have) / i.count;
        uint64_t m = std::min<uint64_t>(more, cap > loaded ? cap - loaded : 0);
        if (i.room != UINT64_MAX) {  // the station holds no more than this of it
            const uint64_t r = i.room / i.count;
            m = std::min<uint64_t>(m, r > loaded ? r - loaded : 0);
        }
        best = std::min<uint64_t>(best, m);
    }
    return best == UINT64_MAX ? 0 : static_cast<uint32_t>(std::min<uint64_t>(best, UINT32_MAX));
}

// How many more crafts a request asks for: UpTo (default) = crafts - loaded, Add = crafts.
inline uint32_t crafts_wanted(uint32_t flags, uint32_t crafts, uint32_t loaded) {
    if (flags & KILN_STATION_FILL_ADD) return crafts;
    return crafts > loaded ? crafts - loaded : 0;
}

// After the game refused n (no room in the station, or a short input it saw and the plan did not): the next n to try, 0
// to stop. Halves, at most kMaxTries calls in all.
constexpr uint32_t kMaxTries = 6;
inline uint32_t crafts_next_try(uint32_t flags, uint32_t n, uint32_t tries) {
    if ((flags & KILN_STATION_FILL_EXACT) || tries >= kMaxTries || n <= 1) return 0;
    return n / 2;
}

// The first n a fill runs the game's function with: what is wanted, cut to what the stock covers. EXACT: all of it, or 0
// (no call) when the stock after the reserves does not cover it: the game itself knows no reserves.
inline uint32_t crafts_first_try(uint32_t flags, uint32_t wanted, uint32_t affordable) {
    if (flags & KILN_STATION_FILL_EXACT) return wanted <= affordable ? wanted : 0;
    return std::min(wanted, affordable);
}

// What allows the fewest more crafts, and why: the item and KILN_STATION_LIMIT_*.
struct Limit {
    uint32_t item = 0;
    uint8_t why = KILN_STATION_LIMIT_NONE;
};
inline Limit limit_of(const std::vector<InputNow>& in, uint32_t loaded) {
    Limit l;
    uint64_t best = UINT64_MAX;
    for (const auto& i : in) {
        if (i.count == 0) continue;
        const uint64_t total = (i.have + i.stock) / i.count;
        const uint64_t more = total > loaded ? total - loaded : 0;
        uint64_t room_more = UINT64_MAX;
        if (i.room != UINT64_MAX) {
            const uint64_t r = i.room / i.count;
            room_more = r > loaded ? r - loaded : 0;
        }
        const uint64_t m = std::min(more, room_more);
        if (m >= best) continue;
        best = m;
        if (i.item == kWaterItem) l = {0, KILN_STATION_LIMIT_WATER};
        else if (room_more < more) l = {i.item, KILN_STATION_LIMIT_ROOM};
        else if (i.kept && (i.have + i.stock + i.kept) / i.count > total) l = {i.item, KILN_STATION_LIMIT_RESERVE};
        else l = {i.item, KILN_STATION_LIMIT_STOCK};
    }
    return l;
}

// The plan's arithmetic for a FILL request over its inputs (water and duplicates already folded in).
struct PlanMath {
    int32_t status = KILN_STATION_DONE;
    uint32_t loaded = 0, affordable = 0, wanted = 0, planned = 0;
    Limit limit;
};
inline PlanMath plan_math(uint32_t flags, uint32_t crafts, const std::vector<InputNow>& in) {
    PlanMath p;
    p.loaded = crafts_loaded(in);
    p.affordable = crafts_affordable(in, p.loaded);
    p.wanted = std::min<uint32_t>(crafts_wanted(flags, crafts, p.loaded), kGameMaxCount);
    p.planned = crafts_first_try(flags, p.wanted, p.affordable);
    if (p.wanted == 0) {
        p.status = KILN_STATION_UNCHANGED;
        return p;
    }
    if (p.planned < p.wanted) p.limit = limit_of(in, p.loaded);
    if (p.planned == 0) p.status = p.limit.why == KILN_STATION_LIMIT_ROOM ? KILN_STATION_NO_ROOM : KILN_STATION_NOT_ENOUGH;
    else p.status = p.planned < p.wanted ? KILN_STATION_PARTIAL : KILN_STATION_DONE;
    return p;
}

// The game's function, tried with n and then less (crafts_next_try) until it takes it. `attempt(n)` returns the game's
// code (0: done) or kAttemptFault (stop at once).
constexpr int32_t kAttemptFault = -0x7fffffff;
struct Attempts {
    bool ok = false;
    uint32_t n = 0, tries = 0;
    int32_t code = 0;
};
template <class F>
Attempts run_attempts(uint32_t flags, uint32_t first, F attempt) {
    Attempts a;
    uint32_t n = first;
    while (n > 0) {
        ++a.tries;
        a.code = attempt(n);
        if (a.code == 0) {
            a.ok = true;
            a.n = n;
            return a;
        }
        if (a.code == kAttemptFault) return a;
        n = crafts_next_try(flags, n, a.tries);
    }
    return a;
}
// A FILL's status from its attempts: DONE / PARTIAL, else the game's last code (2 no room, 4 short), else REFUSED.
inline int32_t fill_status(const Attempts& a, uint32_t wanted) {
    if (a.ok) return a.n < wanted ? KILN_STATION_PARTIAL : KILN_STATION_DONE;
    if (a.code == 2) return KILN_STATION_NO_ROOM;
    if (a.code == 4) return KILN_STATION_NOT_ENOUGH;
    return KILN_STATION_REFUSED;
}

// Stock after a reserve: what may be taken of `have` when `keep` must stay.
inline uint64_t after_keep(uint64_t stock, uint64_t keep) { return stock > keep ? stock - keep : 0; }

// ---- stop after the running craft ----
// A cycle is in progress while running != 0 in Running, Paused (it resumes where it paused) or OutputFull (its outputs
// wait for room); a stop then makes the game refund the inputs and lose the cycle. Due: no cycle in progress, or a new
// cycle began (Running with another start, younger than kFreshNs: a resume from a pause also moves the start, to
// now - the time it had run). `start0` is recipeStart when the request was taken; now_ns the world's program time.
constexpr int64_t kFreshNs = 2'000'000'000;
inline bool cycle_in_progress(uint8_t state, uint32_t running) {
    return running != 0 && (state == KILN_STATION_STATE_RUNNING || state == KILN_STATION_STATE_PAUSED ||
                            state == KILN_STATION_STATE_OUTPUT_FULL);
}
inline bool after_craft_due(uint8_t state, uint32_t running, int64_t start0, int64_t start_now, int64_t now_ns = INT64_MIN) {
    if (!cycle_in_progress(state, running)) return true;
    if (state != KILN_STATION_STATE_RUNNING || start_now == start0) return false;
    return now_ns == INT64_MIN || now_ns - start_now < kFreshNs;
}

// A waiting STOP (after craft), looked at each step.
enum class StopWait { Wait, Write, TimedOut, Canceled, Unchanged, Gone };
struct StopSeen {
    bool present = false;  // the station still has its FactoryStation
    uint8_t state = 0, stopped = 0;
    uint32_t running = 0, selected = 0;
    int64_t start = 0, now = 0;
};
inline StopWait waiting_stop(const StopSeen& s, uint32_t selected0, int64_t start0, uint64_t waited, uint64_t max_steps) {
    if (!s.present) return StopWait::Gone;
    if (s.stopped) return StopWait::Unchanged;            // someone stopped it meanwhile: nothing left to do
    if (s.selected != selected0) return StopWait::Canceled;  // its recipe was changed (a player's or a mod's select)
    if (after_craft_due(s.state, s.running, start0, s.start, s.now)) return StopWait::Write;
    return waited > max_steps ? StopWait::TimedOut : StopWait::Wait;
}

// A later action on a station withdraws a STOP waiting there: RESUME, SELECT, and a STOP that is not after-craft.
inline bool withdraws_waiting(const KilnStationRequest& r) {
    return r.action == KILN_STATION_RESUME || r.action == KILN_STATION_SELECT ||
           (r.action == KILN_STATION_STOP && !(r.flags & KILN_STATION_AFTER_CRAFT));
}

// ---- the request queue ----
struct Req {
    uint64_t ticket = 0;
    KilnOwner owner = 0;
    KilnStationRequest r{};
    KilnStationStack keep[KILN_STATIONS_MAX_KEEP]{};
    uint64_t submitted = 0;
    // STOP after craft: what the station showed when it was taken
    bool waiting = false;
    int64_t start0 = 0;
    uint32_t selected0 = 0;
    uint64_t waiting_since = 0;
    uint8_t last_state = 0xff;  // the state seen the step before (a resume from Paused moves the start)
};
// Removes the requests of `list` that match and returns them, in order.
template <class P>
std::vector<Req> take_if(std::vector<Req>* list, P pred) {
    std::vector<Req> out;
    for (auto i = list->begin(); i != list->end();) {
        if (pred(*i)) {
            out.push_back(*i);
            i = list->erase(i);
        } else {
            ++i;
        }
    }
    return out;
}

inline bool is_deferred_stop(const KilnStationRequest& r) {
    return r.action == KILN_STATION_STOP && (r.flags & KILN_STATION_AFTER_CRAFT);
}

inline KilnStationResult result_for(const Req& q) {
    KilnStationResult r{};
    r.size = sizeof r;
    r.action = q.r.action;
    r.flags = q.r.flags;
    r.station = q.r.station;
    r.recipe = q.r.recipe;
    r.ticket = q.ticket;
    r.status = KILN_STATION_DONE;
    return r;
}

// One step of the program's requests, in order, then the stops that waited from before. act(q, res) runs q (it may
// change q: a stop's first look) and returns true when q is a stop that still waits; done(q, res) records an answer.
// A RESUME, SELECT or STOP now withdraws the stops waiting on its station (from before, or earlier in this step) once
// it has passed its checks (DONE or UNCHANGED), and says so in its message; a second stop after craft on a station
// with one waiting is UNCHANGED. Returns the stops still waiting, the older first.
template <class Act, class Done>
std::vector<Req> run_batch(const std::vector<Req>& batch, std::vector<Req> after, Act act, Done done) {
    auto on = [](KilnEntity st) { return [st](const Req& x) { return x.r.station == st; }; };
    std::vector<Req> fresh;  // this step's stops after craft that wait
    for (Req q : batch) {
        const KilnEntity st = q.r.station;
        if (is_deferred_stop(q.r) &&
            (std::any_of(after.begin(), after.end(), on(st)) || std::any_of(fresh.begin(), fresh.end(), on(st)))) {
            KilnStationResult res = result_for(q);
            res.status = KILN_STATION_UNCHANGED;
            std::snprintf(res.message, sizeof res.message, "a stop after the current craft already waits on station %u", st);
            done(q, res);
            continue;
        }
        KilnStationResult res = result_for(q);
        if (act(q, &res)) {
            fresh.push_back(q);
            continue;
        }
        if (withdraws_waiting(q.r) && (res.status == KILN_STATION_DONE || res.status == KILN_STATION_UNCHANGED)) {
            uint32_t withdrawn = 0;
            std::vector<Req> gone = take_if(&after, on(st));
            for (const Req& w : take_if(&fresh, on(st))) gone.push_back(w);
            for (const Req& w : gone) {
                KilnStationResult c = result_for(w);
                c.status = KILN_STATION_CANCELED;
                std::snprintf(c.message, sizeof c.message, "withdrawn by a later action on the station: nothing written");
                done(w, c);
                ++withdrawn;
            }
            if (withdrawn) {
                const size_t n = std::strlen(res.message);
                std::snprintf(res.message + n, sizeof res.message - n, "%swithdrew the stop that waited for the cycle's end",
                              n ? "; " : "");
            }
        }
        done(q, res);
    }
    std::vector<Req> waiting;
    for (Req q : after) {
        KilnStationResult res = result_for(q);
        if (act(q, &res)) waiting.push_back(q);
        else done(q, res);
    }
    waiting.insert(waiting.end(), fresh.begin(), fresh.end());
    return waiting;
}

// The watch's station scan: the first, every kScanEvery steps, and at once after the bases were rebuilt (a station's
// base index changes with them).
inline bool scan_due(bool scanned, uint64_t step, uint64_t last_scan, uint64_t every, bool bases_changed) {
    return !scanned || bases_changed || step - last_scan >= every;
}

// A waiting stop's look at its station this step (updates what it remembers).
inline StopWait waiting_stop_step(Req* q, const StopSeen& s, uint64_t waited, uint64_t max_steps) {
    // Paused -> Running is the same cycle resuming: the game moved its start (now - what it had run); a new cycle
    // follows a state with no cycle in progress, which this stop answers first (Write)
    if (s.present && q->last_state == KILN_STATION_STATE_PAUSED && s.state == KILN_STATION_STATE_RUNNING) q->start0 = s.start;
    const StopWait w = waiting_stop(s, q->selected0, q->start0, waited, max_steps);
    q->last_state = s.state;
    return w;
}

// ---- a base's id ----
// The smallest of the altars' ids (16 bytes compared in memory order); all zero ids are skipped.
inline KilnBaseId smallest_id(const KilnBaseId* ids, uint32_t n) {
    KilnBaseId best{};
    bool have = false;
    static const KilnBaseId zero{};
    for (uint32_t i = 0; i < n; ++i) {
        if (std::memcmp(ids[i].guid, zero.guid, 16) == 0) continue;
        if (!have || std::memcmp(ids[i].guid, best.guid, 16) < 0) best = ids[i];
        have = true;
    }
    return best;
}

// ---- the state diff behind the events ----
struct Snap {
    uint8_t state = 0, stopped = 0;
    uint32_t selected = 0, running = 0, workshop = 0;
    int32_t base = -1;
    KilnVec3 pos{};
    bool operator==(const Snap& o) const {
        return state == o.state && stopped == o.stopped && selected == o.selected && running == o.running;
    }
};

inline KilnStationEvent state_event(KilnEntity e, const Snap& before, const Snap& after) {
    KilnStationEvent ev{};
    ev.size = sizeof ev;
    ev.kind = KILN_STATION_EV_STATE;
    ev.station = e;
    ev.workshop = after.workshop;
    ev.state_before = before.state;
    ev.state_after = after.state;
    ev.stopped_before = before.stopped;
    ev.stopped_after = after.stopped;
    ev.selected_before = before.selected;
    ev.selected_after = after.selected;
    ev.running_before = before.running;
    ev.running_after = after.running;
    ev.base_index = after.base;
    ev.position = after.pos;
    return ev;
}

// The last 1024 events, numbered from 1.
class EventRing {
public:
    static constexpr uint32_t kCap = 1024;
    void push(KilnStationEvent ev) {
        ev.seq = ++seq_;
        buf_.push_back(ev);
        if (buf_.size() > kCap) buf_.pop_front();
    }
    uint64_t last() const { return seq_; }
    // Events with seq > after, oldest first; returns the number written; *more when more wait.
    uint32_t read(uint64_t after, KilnStationEvent* out, uint32_t capacity, bool* more) const {
        *more = false;
        uint32_t n = 0;
        if (buf_.empty()) return 0;
        const uint64_t first = buf_.front().seq;
        size_t i = after + 1 > first ? static_cast<size_t>(after + 1 - first) : 0;
        for (; i < buf_.size(); ++i) {
            if (n == capacity) {
                *more = true;
                break;
            }
            out[n++] = buf_[i];
        }
        return n;
    }
    void clear() { buf_.clear(); }  // seq_ keeps counting: a reader's `after` stays valid

private:
    std::deque<KilnStationEvent> buf_;
    uint64_t seq_ = 0;
};

// ---- status ----
// What kiln.stations reports as on. `live`: a server world runs here; `program`: kiln_items_stations is registered and
// not broken. The reads and the actions need the world; the events do not: a mod subscribes in OnLoad, before any world
// runs (and a game client that hosts later gets its world after that), and the watch only diffs while a world runs.
struct StatusBits {
    bool read = false, fill = false, control = false, events = false;
};
inline StatusBits status_bits(bool read_ok, bool fill_ok, bool control_ok, bool events_ok, bool live, bool program) {
    StatusBits b;
    b.read = read_ok && live;
    b.fill = fill_ok && program && live;
    b.control = control_ok && program && live;
    b.events = events_ok && read_ok;
    return b;
}

// ---- SELECT's unlock check ----
// The verdict from the answers: the world's (-1 unknown / not applicable, 0, 1) and the players' in game.
enum class Unlock { Yes, No, Unknown };
inline Unlock unlock_verdict(bool always, int world, const std::vector<int>& players) {
    if (always) return Unlock::Yes;
    if (world == 1) return Unlock::Yes;
    bool known = world == 0;
    for (int p : players) {
        if (p == 1) return Unlock::Yes;
        if (p == 0) known = true;
    }
    return known ? Unlock::No : Unlock::Unknown;
}

}  // namespace kitems::stations
