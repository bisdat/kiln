// SPDX-License-Identifier: MIT
// kiln.decisions, the death producer (kind 1): what the hook at the death boundary does with the entity it stopped at,
// and how an answer is carried out, with nothing of the game or of Windows in it. death.cpp gives it the game (the
// "ports"); tests/death_test.cpp runs it on the host.
//
// The hook sits inside the game's death program, after an entity's Health was read and before the program marks its
// Actor dead (chapter 07 §1 "Death", §2 "Death and pickup targets"). It runs for every entity the program visits, on
// the world thread or on a worker, inside the simulation: it reads nothing but its own registers, the program's
// declared Actor slot and the published metadata table, and it never calls managed code. A visit ends one of two ways:
//   Pass  the game goes on as without Kiln (the entity dies when its health says so);
//   Skip  this entity's remainder of the death program is skipped in this step (the probe's fixed form: jump to the
//         program's next-entity code, never return from the whole program).
//
// Order of a visit with health 0 or less (§4.1, ruling for 07 in PLAN.md):
//   0. not the server world's own invocation of the program (a hosting client's client or prediction world, whose ids
//      overlap the server world's; a joined client, which has no server world): Pass, counted, before anything else
//      is read (world_gate.h);
//   1. the native guard (19d-pets): SKIP skips at once. Asked BEFORE any interest, so a guarded entity is safe while
//      no mod listens;
//   2. nobody listens for any death: Pass (the zero-interest exit; no lock, no lookup);
//   3. an Actor that is already dead or in state 12 is no new transition: Pass;
//   4. a hold of this entity: still pending -> Skip; released (allowed, timed out, failed) -> Pass once and forget it;
//      kept alive (Prevented, AlreadyAlive) -> it is a new lethal visit: forget the old hold, go on;
//   5. not this entity (a filter that does not name it) -> Pass;
//   6. metadata over budget, outcomes nobody reads -> Pass (fail open);
//   7. no metadata row (an entity seen first this step, one without a usable identity) -> Pass, counted;
//   8. file a request; filed -> Skip and remember the hold; queue full or no consumer -> Pass.
//
// The answer (Pre, tick thread, inside the broker's answer: §4.2 "Death"): the life must be the one that was held
// (StaleEntity), live health positive again wins (AlreadyAlive, nothing written), allow releases (Allowed), an Actor
// another program marked dead while held is not resurrected (SourceChanged), a maximum that is not positive cannot be
// kept alive (NoPositiveRange), else the combat service's own health setter writes the survival health clamped to
// 1..max and the value read back decides (Prevented, or WriteRefused).
#pragma once
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "metadata.h"
#include "queue.h"

namespace kiln::decisions::death {

enum class Visit { Pass, Skip };

struct Stats {
    std::atomic<uint64_t> guarded{0}, filed{0}, held{0}, released{0}, misses{0}, dropped{0}, blocked{0};
    std::atomic<uint64_t> foreign{0};  // visits of another world (client, prediction) or with no server world: passed
};

// What one live read of the entity gives the answer.
struct Live {
    int32_t current = 0, max = 0;
    bool actor_dead = false;  // the Actor's effective dead state (7) or state 12, raw, not derived from health
};

// The hold of one entity: the request filed for its lethal visit.
struct Hold {
    uint64_t request_id = 0, epoch = 0;
    uint64_t terminal_seen = 0;  // the window at which housekeeping first saw it terminal
};

class Holds {
public:
    // One visit of the death boundary. P (the ports) gives, lazily and in this order of need:
    //   int32_t health(); bool armed(); bool server_world(); uint32_t entity(); uint64_t epoch(); uint64_t step();
    //   uint32_t guard(uint64_t epoch, uint32_t entity, int32_t health);
    //   bool any_interest(); bool interested(uint32_t entity); bool actor_blocked(); bool holding();
    //   bool row(uint32_t entity, uint64_t epoch, Row* out); void filed(uint32_t entity, uint64_t request_id);
    template <class P>
    Visit visit(P& p, Broker& broker) {
        const int32_t hp = p.health();
        if (hp > 0 || !p.armed()) return Visit::Pass;
        if (!p.server_world()) {
            stats.foreign.fetch_add(1, std::memory_order_relaxed);
            return Visit::Pass;
        }
        const uint32_t e = p.entity();
        if (e == 0) return Visit::Pass;
        const uint64_t epoch = p.epoch();
        if (p.guard(epoch, e, hp) == KILN_DEATH_GUARD_SKIP) {
            stats.guarded.fetch_add(1, std::memory_order_relaxed);
            return Visit::Skip;
        }
        if (!p.any_interest()) return Visit::Pass;
        if (p.actor_blocked()) return Visit::Pass;  // a corpse, or state 12: not a new transition
        std::lock_guard lock(m_);
        if (auto it = holds_.find(e); it != holds_.end()) {
            if (it->second.epoch == epoch) {
                const uint32_t st = broker.status_of(it->second.request_id);
                if (st == 0) {
                    stats.held.fetch_add(1, std::memory_order_relaxed);
                    return Visit::Skip;
                }
                if (st != KILN_DECISION_PREVENTED && st != KILN_DECISION_ALREADY_ALIVE) {
                    holds_.erase(it);  // released: the game goes on, once
                    stats.released.fetch_add(1, std::memory_order_relaxed);
                    return Visit::Pass;
                }
            }
            holds_.erase(it);  // it lived (or the hold is of an older world): this visit is a new death
        }
        if (!p.interested(e) || !p.holding()) return Visit::Pass;
        Row row;
        if (!p.row(e, epoch, &row)) {
            stats.misses.fetch_add(1, std::memory_order_relaxed);
            broker.count_metadata_miss();
            return Visit::Pass;
        }
        KilnDecisionRequestV1 r{};
        r.size = sizeof r;
        r.version = 1;
        r.kind = KILN_DECISION_DEATH;
        r.payload_bytes = sizeof(KilnDeathRequestV1);
        r.subject = row.key;
        if (row.has_position) r.flags |= KILN_DECISION_FLAG_HAS_POSITION;
        r.payload.death.health = hp;
        r.payload.death.maximum_health = 0;  // not in the program's declared reads: the answer reads it live in Pre
        r.payload.death.effective_actor_states = 0;  // dead (7) and 12 were tested clear above
        r.payload.death.suggested_survival_health = 1;
        uint64_t id = 0;
        const KilnResult rc = broker.file(r, epoch, p.step(), &id);
        if (rc != KILN_OK) {
            if (rc == KILN_ERR_BUFFER_TOO_SMALL) stats.dropped.fetch_add(1, std::memory_order_relaxed);
            return Visit::Pass;  // fail open: the queue is full, or nobody consumes
        }
        holds_[e] = Hold{id, epoch, 0};
        stats.filed.fetch_add(1, std::memory_order_relaxed);
        p.filed(e, id);
        return Visit::Skip;
    }

    // window_begin (world thread, no world job live): a pending hold whose entity nobody listens for any more is
    // released (CAPABILITY_RELEASED); terminal holds the hook did not come back to for three windows (the entity was
    // removed) are forgotten; holds of another world go.
    template <class Covers>
    void housekeeping(uint64_t epoch, uint64_t step, Broker& broker, Covers covers) {
        std::lock_guard lock(m_);
        for (auto it = holds_.begin(); it != holds_.end();) {
            Hold& h = it->second;
            if (h.epoch != epoch) {
                it = holds_.erase(it);
                continue;
            }
            const uint32_t st = broker.status_of(h.request_id);
            if (st == 0) {
                if (!covers(it->first))
                    broker.complete(h.request_id, KILN_DECISION_CAPABILITY_RELEASED, "nobody listens for this death any more; it was released",
                                    step);
                ++it;
                continue;
            }
            if (st == UINT32_MAX) {
                it = holds_.erase(it);
                continue;
            }
            if (h.terminal_seen == 0) h.terminal_seen = step;
            if (step >= h.terminal_seen + 3) it = holds_.erase(it);
            else ++it;
        }
    }

    void clear() {
        std::lock_guard lock(m_);
        holds_.clear();
    }
    size_t size() const {
        std::lock_guard lock(m_);
        return holds_.size();
    }

    Stats stats;

private:
    mutable std::mutex m_;
    std::unordered_map<uint32_t, Hold> holds_;
};

// Carries out an answer for kind 1 (the broker's finisher). P gives:
//   bool same_life(const KilnEventEntityKey& key)   the entity is there and is still the life that was held
//   bool live(uint32_t entity, Live* out)           its health, maximum and raw Actor state now
//   KilnResult set_health(uint32_t entity, int32_t value, int32_t* applied)   the combat service's setter
template <class P>
uint32_t finish(Finish& f, P& p) {
    const KilnDecisionRequestV1& r = *f.request;
    const KilnDecisionAnswerV1& a = *f.answer;
    const uint32_t e = r.subject.entity;
    auto say = [&](const char* fmt, auto... args) { std::snprintf(f.reason, sizeof f.reason, fmt, args...); };
    if (!p.same_life(r.subject)) {
        say("entity %u is gone, or its id belongs to another life now", e);
        return KILN_DECISION_STALE_ENTITY;
    }
    Live l;
    if (!p.live(e, &l)) {
        say("entity %u has no health to read any more", e);
        return KILN_DECISION_STALE_ENTITY;
    }
    f.health_known = true;
    f.health_before = l.current;
    f.applied_health = l.current;
    if (l.current > 0) {
        say("entity %u had %d health again before the answer; nothing was written", e, l.current);
        return KILN_DECISION_ALREADY_ALIVE;
    }
    if (a.allow) return KILN_DECISION_ALLOWED;
    if (l.actor_dead) {
        say("another program marked entity %u dead while its death was held; it is not brought back", e);
        return KILN_DECISION_SOURCE_CHANGED;
    }
    if (l.max <= 0) {
        say("entity %u has a maximum health of %d: it cannot be kept alive", e, l.max);
        return KILN_DECISION_NO_POSITIVE_RANGE;
    }
    const int32_t want = std::clamp(a.survival_health > 0 ? a.survival_health : 1, 1, l.max);
    int32_t applied = 0;
    const KilnResult rc = p.set_health(e, want, &applied);
    if (rc != KILN_OK) {
        say("kiln.combat refused the health write for entity %u (result %d)", e, static_cast<int>(rc));
        return KILN_DECISION_WRITE_REFUSED;
    }
    f.applied_health = applied;
    if (applied <= 0) {
        say("entity %u's health stayed at %d after the write of %d", e, applied, want);
        return KILN_DECISION_WRITE_REFUSED;
    }
    return KILN_DECISION_PREVENTED;
}

}  // namespace kiln::decisions::death
