// SPDX-License-Identifier: MIT
// kiln.stations: the service's logic without game memory (host test tests/stations_test.cpp): request checks, the fill's
// arithmetic (the game's own: AutoTransferRecipe, docs/re/stations.md §3.1), a base's id from its altars, the state diff
// behind the events and the event ring. The game side is stations.cpp.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <deque>
#include <vector>

#include "services/kiln_stations.h"

namespace kitems::stations {

constexpr uint32_t kMaxCrafts = 1000;
constexpr uint32_t kGameMaxCount = 0xFFFF;  // the game clamps what one input may move to a u16

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
        return true;
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
// One resolved input: `count` per craft, `have` in the station (every link, partial stacks too), `stock` in the base's
// stock containers after the reserves.
struct InputNow {
    uint32_t item = 0;
    uint32_t count = 0;
    uint64_t have = 0;
    uint64_t stock = 0;
};

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
        best = std::min<uint64_t>(best, std::min<uint64_t>(more, cap > loaded ? cap - loaded : 0));
    }
    return best == UINT64_MAX ? 0 : static_cast<uint32_t>(std::min<uint64_t>(best, UINT32_MAX));
}

// How many more crafts a request asks for: UpTo (default) = crafts - loaded, Add = crafts.
inline uint32_t crafts_wanted(uint32_t flags, uint32_t crafts, uint32_t loaded) {
    if (flags & KILN_STATION_FILL_ADD) return crafts;
    return crafts > loaded ? crafts - loaded : 0;
}

// The first n a fill runs the game's function with: what is wanted, cut to what the stock covers unless EXACT.
inline uint32_t crafts_first_try(uint32_t flags, uint32_t wanted, uint32_t affordable) {
    if (flags & KILN_STATION_FILL_EXACT) return wanted;
    return std::min(wanted, affordable);
}

// After the game refused n (no room in the station, or a short input it saw and the plan did not): the next n to try, 0
// to stop. Halves, at most kMaxTries calls in all.
constexpr uint32_t kMaxTries = 6;
inline uint32_t crafts_next_try(uint32_t flags, uint32_t n, uint32_t tries) {
    if ((flags & KILN_STATION_FILL_EXACT) || tries >= kMaxTries || n <= 1) return 0;
    return n / 2;
}

// Stock after a reserve: what may be taken of `have` when `keep` must stay.
inline uint64_t after_keep(uint64_t stock, uint64_t keep) { return stock > keep ? stock - keep : 0; }

// ---- stop after the running craft ----
// Stop now when the station runs no cycle, or the cycle that ran at the request has ended (a new start, or a state that
// is not Running). `start0` is recipeStart when the request was taken.
inline bool after_craft_due(uint8_t state, uint32_t running, int64_t start0, int64_t start_now) {
    if (state != KILN_STATION_STATE_RUNNING || running == 0) return true;
    return start_now != start0;
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
