// SPDX-License-Identifier: MIT
// kiln.decisions, the walk-over pickup producer (kind 2): sampled exposures, what the hook at the range pickup helper
// does with a visit, and the explicit reset, with nothing of the game or of Windows in it. pickup.cpp gives it the
// game (the "ports"); tests/pickup_test.cpp runs it on the host.
//
// The hook sits at the entry of the helper the game's walk-over program (loot_pickup_in_range) calls for a drop whose
// DynamicIsPlayerInRange names a receiver (chapter 07 §1 "Walk-over pickup", §2). It never touches the interaction
// helper (LootEvent covers interaction loot). Only the server world's own invocations are looked at (world_gate.h):
// a hosting client's client and prediction worlds, and a joined client, pass untouched. A visit ends one of two ways:
//   Pass    the helper runs as without Kiln;
//   Refuse  the helper returns its own ordinary "nothing added" result ({error 0x15, zero}, 8 bytes) without running:
//           inventory and drop stay as they are, and the game tries again at its next attempt.
//
// Exposure (§4.3). One per drop: (world epoch, drop and its key, receiver and its key, exposure epoch, item, raw
// amount). A permit or a refusal never crosses drops, receivers, lives, worlds or item/amount changes: a visit whose
// receiver, item or amount differ from the drop's exposure closes it and starts a new one. Both window calls read the
// drop's DynamicIsPlayerInRange and PickupItem legally (no world job is live): receiver 0, another receiver, the drop
// gone or another item/amount close the exposure; window_begin also checks that both lives are still the ones keyed.
// What happens wholly between two samples (leaving and coming back) is not seen: the exposure is SAMPLED.
//
// States of an exposure: Open (no request yet) -> Pending (a request is filed; visits are refused) -> Suppressed (a
// handler canceled; visits are refused until the exposure closes or Decisions.ReevaluatePickup reopens it) or
// Released (allowed, or failed open: visits pass for the rest of this exposure). An allow is therefore a permit for
// exactly this key: a partial grant changes the amount, which is a new exposure and a new decision.
#pragma once
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <unordered_map>

#include "metadata.h"
#include "queue.h"

namespace kiln::decisions::pickup {

enum class Visit { Pass, Refuse };
enum class State : uint8_t { Open, Pending, Suppressed, Released };

struct Exposure {
    uint64_t world_epoch = 0, exposure_epoch = 0, request_id = 0;
    uint32_t receiver = 0, item = 0, amount = 0;
    KilnEventEntityKey drop_key{}, receiver_key{};
    uint32_t has_position = 0;
    double position[3] = {};
    State state = State::Open;
};

struct Stats {
    std::atomic<uint64_t> visits{0}, filed{0}, refused{0}, permitted{0}, released{0}, misses{0}, dropped{0}, closed{0}, reopened{0};
    std::atomic<uint64_t> foreign{0};  // visits of another world (client, prediction) or with no server world: passed
};

class Exposures {
public:
    // One visit of the range helper. P gives, lazily and in this order of need:
    //   bool armed(); bool server_world(); uint32_t receiver(); bool any_interest(); uint32_t entity();
    //   bool interested(uint32_t entity); bool item(uint32_t* item, uint32_t* amount); uint32_t feedback();
    //   uint64_t epoch(); uint64_t step(); bool holding(); bool row(uint32_t entity, uint64_t epoch, Row* out);
    template <class P>
    Visit visit(P& p, Broker& broker) {
        if (!p.armed()) return Visit::Pass;
        // Not the server world's own invocation (a hosting client's client or prediction world, ids overlapping the
        // server world's; a joined client without a server world): the game's own pickup, nothing read (world_gate.h).
        if (!p.server_world()) {
            stats.foreign.fetch_add(1, std::memory_order_relaxed);
            return Visit::Pass;
        }
        const uint32_t r = p.receiver();
        if (r == 0 || !p.any_interest()) return Visit::Pass;  // the zero-interest exit
        const uint32_t d = p.entity();
        if (d == 0 || !(p.interested(r) || p.interested(d))) return Visit::Pass;
        uint32_t item = 0, amount = 0;
        if (!p.item(&item, &amount)) return Visit::Pass;
        stats.visits.fetch_add(1, std::memory_order_relaxed);
        const uint64_t epoch = p.epoch();
        std::lock_guard lock(m_);
        auto it = map_.find(d);
        if (it != map_.end() && (it->second.world_epoch != epoch || it->second.receiver != r || it->second.item != item ||
                                 it->second.amount != amount)) {
            close(it, broker, KILN_DECISION_SOURCE_CHANGED, "the drop's receiver, item or amount changed under the hold", p.step());
            it = map_.end();
        }
        if (it == map_.end()) {
            if (!p.holding()) return Visit::Pass;
            Row dr, rr;
            if (!p.row(d, epoch, &dr) || !p.row(r, epoch, &rr)) {
                stats.misses.fetch_add(1, std::memory_order_relaxed);
                broker.count_metadata_miss();
                return Visit::Pass;  // a drop or receiver first seen this step: fail open until a sample exists
            }
            Exposure x;
            x.world_epoch = epoch;
            x.exposure_epoch = ++next_epoch_;
            x.receiver = r;
            x.item = item;
            x.amount = amount;
            x.drop_key = dr.key;
            x.receiver_key = rr.key;
            x.has_position = dr.has_position;
            std::memcpy(x.position, dr.position, sizeof x.position);
            it = map_.emplace(d, x).first;
        }
        Exposure& x = it->second;
        switch (x.state) {
            case State::Released: return Visit::Pass;
            case State::Suppressed: return refuse();
            case State::Pending: {
                const uint32_t st = broker.status_of(x.request_id);
                if (st == 0) return refuse();
                if (st == KILN_DECISION_PREVENTED) {
                    x.state = State::Suppressed;
                    return refuse();
                }
                x.state = State::Released;  // allowed (a permit for this key), or released by a timeout or a failure
                (st == KILN_DECISION_ALLOWED ? stats.permitted : stats.released).fetch_add(1, std::memory_order_relaxed);
                return Visit::Pass;
            }
            case State::Open: break;
        }
        KilnDecisionRequestV1 q{};
        q.size = sizeof q;
        q.version = 1;
        q.kind = KILN_DECISION_WALKOVER;
        q.payload_bytes = sizeof(KilnPickupRequestV1);
        q.subject = x.drop_key;
        q.actor = x.receiver_key;
        q.exposure_epoch = x.exposure_epoch;
        q.payload.pickup.item_id = item;
        q.payload.pickup.raw_count = amount;
        q.payload.pickup.feedback = p.feedback();
        q.payload.pickup.entry = 1;  // the range helper
        if (x.has_position) {
            q.flags |= KILN_DECISION_FLAG_HAS_POSITION;
            std::memcpy(q.payload.pickup.position, x.position, sizeof x.position);
        }
        uint64_t id = 0;
        const KilnResult rc = broker.file(q, epoch, p.step(), &id);
        if (rc != KILN_OK) {
            if (rc == KILN_ERR_BUFFER_TOO_SMALL) stats.dropped.fetch_add(1, std::memory_order_relaxed);
            x.state = State::Released;  // fail open for this exposure
            return Visit::Pass;
        }
        x.state = State::Pending;
        x.request_id = id;
        stats.filed.fetch_add(1, std::memory_order_relaxed);
        return refuse();
    }

    // Both window calls (world thread, no world job live). S gives:
    //   bool read(uint32_t drop, uint32_t* receiver, uint32_t* item, uint32_t* amount)   false: the drop or its
    //        DynamicIsPlayerInRange / PickupItem is gone
    //   bool same_life(const KilnEventEntityKey& key)    asked only when `lives` (window_begin)
    //   bool interested(uint32_t entity)
    template <class S>
    void sample(uint64_t epoch, uint64_t step, Broker& broker, S& s, bool lives) {
        std::lock_guard lock(m_);
        for (auto it = map_.begin(); it != map_.end();) {
            Exposure& x = it->second;
            uint32_t r = 0, item = 0, amount = 0;
            const char* why = nullptr;
            uint32_t status = KILN_DECISION_SOURCE_CHANGED;
            if (x.world_epoch != epoch) why = "the world ended", status = KILN_DECISION_WORLD_ENDED;
            else if (!s.read(it->first, &r, &item, &amount)) why = "the drop is gone", status = KILN_DECISION_STALE_ENTITY;
            else if (r != x.receiver) why = r == 0 ? "the receiver left the drop's range" : "another receiver is in the drop's range";
            else if (item != x.item || amount != x.amount) why = "the drop's item or amount changed";
            else if (lives && (!s.same_life(x.drop_key) || !s.same_life(x.receiver_key)))
                why = "the drop or the receiver is another life now", status = KILN_DECISION_STALE_ENTITY;
            else if (!s.interested(x.receiver) && !s.interested(it->first)) {
                // nobody listens for this pair any more: a pending hold is released, a suppression is forgotten
                why = "nobody listens for this pickup any more", status = KILN_DECISION_CAPABILITY_RELEASED;
            }
            if (why) {
                it = close(it, broker, status, why, step);
                continue;
            }
            ++it;
        }
    }

    // Decisions.ReevaluatePickup: reopens the exposures of (drop, receiver) (0 = any) that are not pending, each with a
    // new exposure epoch, so the next visit asks again. Returns how many were reopened.
    uint32_t reopen(uint32_t drop, uint32_t receiver) {
        std::lock_guard lock(m_);
        uint32_t n = 0;
        for (auto& [d, x] : map_) {
            if ((drop && d != drop) || (receiver && x.receiver != receiver) || x.state == State::Pending || x.state == State::Open) continue;
            x.state = State::Open;
            x.exposure_epoch = ++next_epoch_;
            x.request_id = 0;
            ++n;
        }
        stats.reopened.fetch_add(n, std::memory_order_relaxed);
        return n;
    }

    // World-safe reset: consult the broker even before the next helper visit.
    uint32_t reopen(uint32_t drop, uint32_t receiver, Broker& broker) {
        {
            std::lock_guard lock(m_);
            for (auto& [d, x] : map_) {
                if ((drop && d != drop) || (receiver && x.receiver != receiver) || x.state != State::Pending) continue;
                const uint32_t status = broker.status_of(x.request_id);
                if (status == 0) continue;
                x.state = status == KILN_DECISION_PREVENTED ? State::Suppressed : State::Released;
            }
        }
        return reopen(drop, receiver);
    }

    void clear() {
        std::lock_guard lock(m_);
        map_.clear();
    }
    size_t size() const {
        std::lock_guard lock(m_);
        return map_.size();
    }
    // A copy of one exposure (tests, diagnostics); false when the drop has none.
    bool get(uint32_t drop, Exposure* out) const {
        std::lock_guard lock(m_);
        auto it = map_.find(drop);
        if (it == map_.end()) return false;
        *out = it->second;
        return true;
    }

    Stats stats;

private:
    using Map = std::unordered_map<uint32_t, Exposure>;

    Visit refuse() {
        stats.refused.fetch_add(1, std::memory_order_relaxed);
        return Visit::Refuse;
    }

    Map::iterator close(Map::iterator it, Broker& broker, uint32_t status, const char* why, uint64_t step) {
        if (it->second.state == State::Pending) broker.complete(it->second.request_id, status, why, step);
        stats.closed.fetch_add(1, std::memory_order_relaxed);
        return map_.erase(it);
    }

    mutable std::mutex m_;
    Map map_;
    uint64_t next_epoch_ = 0;  // process-wide, never 0 for an exposure
};

}  // namespace kiln::decisions::pickup
