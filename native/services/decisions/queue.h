// SPDX-License-Identifier: MIT
// kiln.decisions: the broker (requests, outcomes, batches, deadlines, limits) with nothing of the game or of Windows
// in it. decisions.cpp wraps it into the service table; tests/queue_test.cpp runs it on the host.
//
// One mutex guards everything. Producers call file() (hooks, any thread) or enqueue_prepared() (Pre adapters); the
// consumer's calls come from the world thread in a world-safe phase (decisions.cpp checks that, not this file).
// Nothing here calls out while the mutex is held except the finisher of a kind (a producer's own code that applies
// an answer, e.g. the health write of a canceled death), which runs on the consumer's thread in Pre. The sinks of
// requests a native Pre adapter filed (file_native, kind 18) are collected under the mutex and called by
// drain_sinks() after it is released.
#pragma once
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "services/kiln_decisions.h"

namespace kiln::decisions {

inline uint32_t payload_size(uint32_t kind) {
    switch (kind) {
        case KILN_DECISION_DEATH: return sizeof(KilnDeathRequestV1);
        case KILN_DECISION_WALKOVER: return sizeof(KilnPickupRequestV1);
        case KILN_DECISION_EQUIP_SINGLE: return sizeof(KilnSingleEquipRequestV1);
        case KILN_DECISION_EQUIP_COLLECTION: return sizeof(KilnCollectionEquipRequestV1);
        case KILN_DECISION_EQUIP_SET: return sizeof(KilnSetDecisionRequestV1);
        case KILN_DECISION_BUILD_PIECE: return sizeof(KilnBuildRequestV1);
        default: return 0;  // unknown and reserved kinds (33) are refused
    }
}

// Client-input kinds keep kiln.playerstate's one-Pre hold: enqueue, dispatch and answer in the same Pre.
inline bool one_pre(uint32_t kind) { return kind == KILN_DECISION_EQUIP_SINGLE || kind == KILN_DECISION_EQUIP_COLLECTION; }

struct Counters {
    uint64_t filed = 0, queue_dropped = 0, timeouts = 0, outcomes = 0, outcome_gap = 0, metadata_misses = 0;
    uint64_t last_complete_step = 0;  // the last step whose outcomes all fit the outbox
};

// What a producer's finisher gets and fills when an answer arrives. It returns the terminal status.
struct Finish {
    const KilnDecisionRequestV1* request;
    const KilnDecisionAnswerV1* answer;
    int32_t health_before = 0, applied_health = 0;
    bool health_known = false;
    char reason[192] = {};
};
using Finisher = uint32_t (*)(Finish& f, void* user);
// The end of a request filed with file_native (kiln_decisions.h, enqueue_native).
using Sink = void (*)(const KilnDecisionOutcomeV1* outcome, void* user);

class Broker {
public:
    Broker() {
        slots_.resize(KILN_DECISIONS_MAX_ACTIVE);
        free_.reserve(KILN_DECISIONS_MAX_ACTIVE);
        for (uint32_t i = KILN_DECISIONS_MAX_ACTIVE; i-- > 0;) free_.push_back(i);
        arrivals_.reserve(KILN_DECISIONS_MAX_ACTIVE);
        active_.reserve(KILN_DECISIONS_MAX_ACTIVE * 2);
    }

    // ---- consumer ----
    KilnResult attach(KilnOwner owner) {
        std::lock_guard lock(m_);
        if (attached_ && owner_ != owner) return KILN_ERR_UNAVAILABLE;
        attached_ = true;
        owner_ = owner;
        return KILN_OK;
    }

    // Releases every pending hold (CAPABILITY_RELEASED) and forgets undelivered request copies. The outcomes stay
    // readable for the next consumer.
    KilnResult detach(KilnOwner owner, uint64_t step) {
        std::lock_guard lock(m_);
        if (!attached_ || owner_ != owner) return KILN_ERR_UNAVAILABLE;
        release_all(KILN_DECISION_CAPABILITY_RELEASED, "the decision consumer detached", step);
        attached_ = false;
        owner_ = 0;
        arrivals_.clear();
        req_batch_ = Batch<KilnDecisionRequestV1>{};
        return KILN_OK;
    }

    bool attached() const {
        std::lock_guard lock(m_);
        return attached_;
    }
    bool is_consumer(KilnOwner owner) const {
        std::lock_guard lock(m_);
        return attached_ && owner_ == owner;
    }

    // The finisher of a kind (set once by its producer, before the first request). Without one an answer ends
    // Allowed or Prevented as given.
    void set_finisher(uint32_t kind, Finisher fn, void* user) {
        std::lock_guard lock(m_);
        if (kind < 64) finishers_[kind] = {fn, user};
    }

    // ---- producers ----
    // Files a request: copies it, assigns request_id, world_epoch, capture_step, deadline_step. KILN_OK and *id on
    // success. Queue full: an outcome QUEUE_DROPPED is recorded under a fresh id (*id) and
    // KILN_ERR_BUFFER_TOO_SMALL is returned: the caller lets the action happen. No consumer: KILN_ERR_UNAVAILABLE.
    KilnResult file(const KilnDecisionRequestV1& in, uint64_t epoch, uint64_t step, uint64_t* id) {
        if (id) *id = 0;
        if (!well_formed(in)) return KILN_ERR_INVALID_ARGUMENT;
        std::lock_guard lock(m_);
        return file_locked(in, epoch, step, id);
    }

    // A native Pre adapter's request (enqueue_native): file() and, once it ends, one call of `sink` with its outcome
    // record, from drain_sinks(). Queue full: the QUEUE_DROPPED outcome goes to the sink as well.
    KilnResult file_native(const KilnDecisionRequestV1& in, uint64_t epoch, uint64_t step, Sink sink, void* user, uint64_t* id) {
        if (id) *id = 0;
        if (!sink || !well_formed(in)) return KILN_ERR_INVALID_ARGUMENT;
        std::lock_guard lock(m_);
        if (!attached_) return KILN_ERR_UNAVAILABLE;
        sinks_[next_id_ + 1] = {sink, user};  // the id file_locked gives it, set first so a QUEUE_DROPPED outcome reaches it too
        return file_locked(in, epoch, step, id);  // attached: KILN_OK or KILN_ERR_BUFFER_TOO_SMALL
    }

    // Calls the sinks of the requests that ended since the last drain, outside the lock, in the order they ended.
    // World thread (the caller of the call that ended them). Returns how many were called.
    uint32_t drain_sinks() {
        std::vector<PendingSink> calls;
        {
            std::lock_guard lock(m_);
            calls.swap(pending_sinks_);
        }
        for (const PendingSink& c : calls) c.sink(&c.outcome, c.user);
        return static_cast<uint32_t>(calls.size());
    }

private:
    static bool well_formed(const KilnDecisionRequestV1& in) {
        const uint32_t want = payload_size(in.kind);
        return in.size == sizeof(KilnDecisionRequestV1) && in.version == 1 && want != 0 && in.payload_bytes == want && in.subject.entity != 0;
    }

    KilnResult file_locked(const KilnDecisionRequestV1& in, uint64_t epoch, uint64_t step, uint64_t* id) {
        if (!attached_) return KILN_ERR_UNAVAILABLE;
        const uint64_t rid = ++next_id_;
        if (id) *id = rid;
        if (free_.empty()) {
            ++counters_.queue_dropped;
            KilnDecisionRequestV1 r = in;
            r.request_id = rid;
            r.world_epoch = epoch;
            push_outcome(r, KILN_DECISION_QUEUE_DROPPED, "the decision queue is full; the action was not held", step, nullptr);
            return KILN_ERR_BUFFER_TOO_SMALL;
        }
        const uint32_t slot = free_.back();
        free_.pop_back();
        KilnDecisionRequestV1& r = slots_[slot];
        r = in;
        r.request_id = rid;
        r.world_epoch = epoch;
        r.capture_step = step;
        r.deadline_step = one_pre(in.kind) ? step : step + 2;
        active_[rid] = slot;
        // Deadlines/answers free slots even when no request batch is read. Keep
        // their obsolete delivery ids from growing without bound; preserve the
        // arrival order of every request that is still active.
        if (arrivals_.size() >= KILN_DECISIONS_MAX_ACTIVE)
            std::erase_if(arrivals_, [&](uint64_t pending) { return !active_.count(pending); });
        arrivals_.push_back(rid);
        ++counters_.filed;
        return KILN_OK;
    }

public:
    // ---- request batches ----
    KilnResult begin_requests(uint64_t* batch, uint32_t* records) {
        std::lock_guard lock(m_);
        if (!req_batch_.open) {
            req_batch_.items.clear();
            for (uint64_t rid : arrivals_)
                if (auto it = active_.find(rid); it != active_.end()) req_batch_.items.push_back(slots_[it->second]);
            arrivals_.clear();
            if (!req_batch_.items.empty()) {
                req_batch_.open = true;
                req_batch_.id = ++batch_seq_;
            }
        }
        *batch = req_batch_.open ? req_batch_.id : 0;
        *records = req_batch_.open ? static_cast<uint32_t>(req_batch_.items.size()) : 0;
        return KILN_OK;
    }
    KilnResult read_requests(uint64_t batch, KilnDecisionRequestV1* out, uint32_t capacity, uint32_t stride, uint32_t* required) {
        std::lock_guard lock(m_);
        return read(req_batch_, batch, out, capacity, stride, required);
    }
    KilnResult ack_requests(uint64_t batch, uint64_t step) {
        std::lock_guard lock(m_);
        return ack(req_batch_, batch, step);
    }

    // ---- outcome batches ----
    KilnResult begin_outcomes(uint64_t* batch, uint32_t* records) {
        std::lock_guard lock(m_);
        if (!out_batch_.open && !outbox_.empty()) {
            out_batch_.items.assign(outbox_.begin(), outbox_.end());
            outbox_.clear();
            out_batch_.open = true;
            out_batch_.id = ++batch_seq_;
        }
        *batch = out_batch_.open ? out_batch_.id : 0;
        *records = out_batch_.open ? static_cast<uint32_t>(out_batch_.items.size()) : 0;
        return KILN_OK;
    }
    KilnResult read_outcomes(uint64_t batch, KilnDecisionOutcomeV1* out, uint32_t capacity, uint32_t stride, uint32_t* required) {
        std::lock_guard lock(m_);
        return read(out_batch_, batch, out, capacity, stride, required);
    }
    KilnResult ack_outcomes(uint64_t batch, uint64_t step) {
        std::lock_guard lock(m_);
        return ack(out_batch_, batch, step);
    }

    // ---- answers ----
    KilnResult answer(const KilnDecisionAnswerV1& a, KilnDecisionReceiptV1* receipt, uint64_t step) {
        if (a.size != sizeof(KilnDecisionAnswerV1) || a.request_id == 0) return KILN_ERR_INVALID_ARGUMENT;
        std::lock_guard lock(m_);
        if (receipt) {
            *receipt = KilnDecisionReceiptV1{};
            receipt->size = sizeof *receipt;
        }
        auto it = active_.find(a.request_id);
        if (it == active_.end()) {
            // Terminal already (idempotent: the status again, nothing else), or never known.
            const auto done = terminal_.find(a.request_id);
            if (done == terminal_.end() || done->second.epoch != a.world_epoch || done->second.kind != a.kind) return KILN_ERR_NOT_FOUND;
            if (receipt) receipt->terminal_status = done->second.status;
            return KILN_OK;
        }
        const uint32_t slot = it->second;
        const KilnDecisionRequestV1 r = slots_[slot];  // a copy: the slot is freed below
        if (r.world_epoch != a.world_epoch || r.kind != a.kind) return KILN_ERR_INVALID_ARGUMENT;
        Finish f{&r, &a};
        uint32_t status = a.allow ? KILN_DECISION_ALLOWED : KILN_DECISION_PREVENTED;
        if (r.kind < 64 && finishers_[r.kind].fn) status = finishers_[r.kind].fn(f, finishers_[r.kind].user);
        retire(a.request_id, slot);
        push_outcome(r, status, f.reason, step, &f);
        if (receipt) {
            receipt->accepted = 1;
            receipt->terminal_status = status;
            std::snprintf(receipt->reason, sizeof receipt->reason, "%s", f.reason);
        }
        return KILN_OK;
    }

    // Ends one active request from its producer's side (the entity went away, the source changed, ...). False when
    // the id is not active.
    bool complete(uint64_t request_id, uint32_t status, const char* reason, uint64_t step) {
        std::lock_guard lock(m_);
        auto it = active_.find(request_id);
        if (it == active_.end()) return false;
        const uint32_t slot = it->second;
        const KilnDecisionRequestV1 r = slots_[slot];
        retire(request_id, slot);
        push_outcome(r, status, reason, step, nullptr);
        return true;
    }

    // 0 while the request is pending; its terminal status afterwards (kept for the last 8192 ids); UINT32_MAX when
    // the id is unknown. For producers that poll their hold.
    uint32_t status_of(uint64_t request_id) const {
        std::lock_guard lock(m_);
        if (active_.count(request_id)) return 0;
        const auto done = terminal_.find(request_id);
        return done == terminal_.end() ? UINT32_MAX : done->second.status;
    }

    // ---- step windows ----
    // After the last Pre subscriber of `step`: holds whose answers were due in the two Pres before are released.
    void window_begin(uint64_t epoch, uint64_t step) {
        std::lock_guard lock(m_);
        expire(epoch, step, [](const KilnDecisionRequestV1& r, uint64_t s) { return !one_pre(r.kind) && s > r.deadline_step; });
        if (!gap_this_step_) counters_.last_complete_step = step ? step - 1 : 0;
        gap_this_step_ = false;
    }
    // After the world's jobs of `step`: an unanswered client input ran under kiln.playerstate's own rule.
    void window_end(uint64_t epoch, uint64_t step) {
        std::lock_guard lock(m_);
        expire(epoch, step, [](const KilnDecisionRequestV1& r, uint64_t s) { return one_pre(r.kind) && s >= r.deadline_step; });
    }
    // The service shut down after a fault (shutdown.h): every open request ends `status` (fail open), as at a detach,
    // but only if the lock is free: a fault can leave it held (an SEH unwind runs no destructor). False: it was not,
    // and nothing was done; the caller then never calls this broker again.
    bool try_release_all(uint32_t status, const char* reason, uint64_t step) {
        std::unique_lock lock(m_, std::try_to_lock);
        if (!lock.owns_lock()) return false;
        release_all(status, reason, step);
        arrivals_.clear();
        req_batch_ = Batch<KilnDecisionRequestV1>{};
        return true;
    }
    // A rollback (reset_watch.h): every open request ends `status`; undelivered copies go, the consumer stays.
    void release_open(uint32_t status, const char* reason, uint64_t step) {
        std::lock_guard lock(m_);
        release_all(status, reason, step);
        arrivals_.clear();
        req_batch_ = Batch<KilnDecisionRequestV1>{};
    }
    // Tests: the lock, held by the caller (a fault that left it held).
    std::unique_lock<std::mutex> lock_for_tests() { return std::unique_lock(m_); }

    // The world stopped: nothing is held any more.
    void world_ended(uint64_t step) {
        std::lock_guard lock(m_);
        release_all(KILN_DECISION_WORLD_ENDED, "the world stopped", step);
        arrivals_.clear();
        req_batch_ = Batch<KilnDecisionRequestV1>{};
    }

    Counters counters() const {
        std::lock_guard lock(m_);
        return counters_;
    }
    void count_metadata_miss() {
        std::lock_guard lock(m_);
        ++counters_.metadata_misses;
    }
    uint32_t active_count() const {
        std::lock_guard lock(m_);
        return static_cast<uint32_t>(active_.size());
    }
    // Delivery ids waiting for begin_requests (including retired ids awaiting compaction).
    size_t undelivered_count() const {
        std::lock_guard lock(m_);
        return arrivals_.size();
    }
    // True while outcomes were lost because nobody read them: producers stop holding until the consumer catches up.
    bool saturated() const {
        std::lock_guard lock(m_);
        return outbox_.size() >= KILN_DECISIONS_MAX_OUTCOMES;
    }

private:
    template <typename T>
    struct Batch {
        bool open = false;
        uint64_t id = 0;
        std::vector<T> items;
        uint64_t last_acked = 0, last_acked_step = 0;
    };
    struct Done {
        uint64_t epoch;
        uint32_t kind, status;
    };
    struct FinisherSlot {
        Finisher fn = nullptr;
        void* user = nullptr;
    };
    struct SinkSlot {
        Sink sink = nullptr;
        void* user = nullptr;
    };
    struct PendingSink {
        Sink sink;
        void* user;
        KilnDecisionOutcomeV1 outcome;
    };

    static KilnDecisionOutcomeV1 outcome_of(const KilnDecisionRequestV1& r, uint64_t rid, uint64_t epoch, uint32_t status, const char* reason,
                                            uint64_t step, const Finish* f) {
        KilnDecisionOutcomeV1 o{};
        o.size = sizeof o;
        o.version = 1;
        o.kind = r.kind;
        o.status = status;
        o.request_id = rid;
        o.world_epoch = epoch;
        o.step = step;
        o.subject = r.subject;
        o.actor = r.actor;
        if (f && f->answer) o.requested_health = f->answer->survival_health;
        if (f && f->health_known) {
            o.flags |= KILN_DECISION_OUTCOME_HEALTH_KNOWN;
            o.health_before = f->health_before;
            o.applied_health = f->applied_health;
        }
        if (reason) std::snprintf(o.reason, sizeof o.reason, "%s", reason);
        return o;
    }

    template <typename T>
    static KilnResult read(const Batch<T>& b, uint64_t batch, T* out, uint32_t capacity, uint32_t stride, uint32_t* required) {
        if (!required) return KILN_ERR_INVALID_ARGUMENT;
        *required = 0;
        if (!b.open || batch != b.id) return KILN_ERR_NOT_FOUND;
        const uint32_t n = static_cast<uint32_t>(b.items.size());
        *required = n;
        if (!out || capacity < n) return KILN_ERR_BUFFER_TOO_SMALL;  // nothing written, nothing consumed
        if (stride < sizeof(T)) return KILN_ERR_INVALID_ARGUMENT;
        auto* p = reinterpret_cast<uint8_t*>(out);
        for (uint32_t i = 0; i < n; ++i) std::memcpy(p + static_cast<size_t>(i) * stride, &b.items[i], sizeof(T));
        return KILN_OK;
    }
    template <typename T>
    static KilnResult ack(Batch<T>& b, uint64_t batch, uint64_t step) {
        if (batch == 0) return KILN_OK;  // the empty batch
        if (b.open && batch == b.id) {
            b.open = false;
            b.items.clear();
            b.last_acked = batch;
            b.last_acked_step = step;
            return KILN_OK;
        }
        // The same ack again is accepted for two Pres.
        if (batch == b.last_acked && step <= b.last_acked_step + 2) return KILN_OK;
        return KILN_ERR_NOT_FOUND;
    }

    void retire(uint64_t rid, uint32_t slot) {
        active_.erase(rid);
        free_.push_back(slot);
    }

    void push_outcome(const KilnDecisionRequestV1& r, uint32_t status, const char* reason, uint64_t step, const Finish* f) {
        // Remember the terminal status of the last 8192 ids for repeated answers.
        terminal_[r.request_id] = Done{r.world_epoch, r.kind, status};
        terminal_order_.push_back(r.request_id);
        while (terminal_order_.size() > KILN_DECISIONS_MAX_OUTCOMES) {
            terminal_.erase(terminal_order_.front());
            terminal_order_.pop_front();
        }
        if (status == KILN_DECISION_TIMEOUT_RELEASED) ++counters_.timeouts;
        const KilnDecisionOutcomeV1 o = outcome_of(r, r.request_id, r.world_epoch, status, reason, step, f);
        // a native adapter's request: its sink hears of the end even when the outbox is full
        if (auto sink = sinks_.find(r.request_id); sink != sinks_.end()) {
            pending_sinks_.push_back({sink->second.sink, sink->second.user, o});
            sinks_.erase(sink);
        }
        if (outbox_.size() >= KILN_DECISIONS_MAX_OUTCOMES) {
            // Nobody reads: the record is lost and said to be lost. Never an unreported loss.
            ++counters_.outcome_gap;
            gap_this_step_ = true;
            return;
        }
        outbox_.push_back(o);
        ++counters_.outcomes;
    }

    template <typename Pred>
    void expire(uint64_t epoch, uint64_t step, Pred due) {
        expired_.clear();
        for (const auto& [rid, slot] : active_) {
            const KilnDecisionRequestV1& r = slots_[slot];
            if (r.world_epoch != epoch) expired_.push_back({rid, KILN_DECISION_WORLD_ENDED});
            else if (due(r, step)) expired_.push_back({rid, KILN_DECISION_TIMEOUT_RELEASED});
        }
        std::sort(expired_.begin(), expired_.end());  // outcomes in request order, not in hash order
        for (const auto& [rid, status] : expired_) {
            const uint32_t slot = active_[rid];
            const KilnDecisionRequestV1 r = slots_[slot];
            retire(rid, slot);
            push_outcome(r, status,
                         status == KILN_DECISION_WORLD_ENDED ? "the request belongs to a world that ended"
                                                             : "no answer in time; the action was released",
                         step, nullptr);
        }
    }

    void release_all(uint32_t status, const char* reason, uint64_t step) {
        expired_.clear();
        for (const auto& [rid, slot] : active_) expired_.push_back({rid, status});
        std::sort(expired_.begin(), expired_.end());
        for (const auto& [rid, st] : expired_) {
            const uint32_t slot = active_[rid];
            const KilnDecisionRequestV1 r = slots_[slot];
            retire(rid, slot);
            push_outcome(r, st, reason, step, nullptr);
        }
    }

    mutable std::mutex m_;
    bool attached_ = false;
    KilnOwner owner_ = 0;
    uint64_t next_id_ = 0, batch_seq_ = 0;
    std::vector<KilnDecisionRequestV1> slots_;
    std::vector<uint32_t> free_;
    std::unordered_map<uint64_t, uint32_t> active_;
    std::vector<uint64_t> arrivals_;
    Batch<KilnDecisionRequestV1> req_batch_;
    std::deque<KilnDecisionOutcomeV1> outbox_;
    Batch<KilnDecisionOutcomeV1> out_batch_;
    std::unordered_map<uint64_t, Done> terminal_;
    std::deque<uint64_t> terminal_order_;
    std::vector<std::pair<uint64_t, uint32_t>> expired_;
    std::array<FinisherSlot, 64> finishers_{};
    std::unordered_map<uint64_t, SinkSlot> sinks_;
    std::vector<PendingSink> pending_sinks_;
    Counters counters_;
    bool gap_this_step_ = false;
};

}  // namespace kiln::decisions
