// SPDX-License-Identifier: MIT
// kiln.component_changes (abi/services/kiln_component_changes.h): the watches, their budgets, the pairs and their
// baselines, the tokens and the origin rule, the two-frame queue and its batches, with nothing of the game or of
// Windows in it. component_changes.cpp wraps it into the service table and gives it the world through Ports;
// tests/watch_test.cpp runs it on the host.
//
// One bounded mutex guards everything (record_write is the only call from other threads: it never waits for ever).
// The window calls and the table calls run on the world thread in a world-safe phase (component_changes.cpp checks
// that): only they read the world, through Ports, under the lock. Nothing here calls managed code.
#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "bounded_mutex.h"
#include "services/kiln_component_changes.h"

namespace kiln::decisions::watch {

// What the service reads of the world. World thread, world-safe phase, inside the service's calls only.
struct Ports {
    virtual ~Ports() = default;
    // The bytes of component `component` (1..6) of `entity`: true and *size (1..256) when present. False: absent (no
    // such entity or component), or larger than 256 bytes (*size says how large; such a pair is never present).
    virtual bool read(uint32_t entity, uint32_t component, uint8_t* out, uint32_t* size) = 0;
    // The entity's InventorySetup (84 bytes: 16 link ids at +0, 16 categories at +0x40). False: it has none.
    virtual bool setup(uint32_t entity, uint8_t* out84) = 0;
    // kiln.decisions' key of `entity` (kiln_decisions.h). False: no entity (*exists false) or no usable identity.
    virtual bool key(uint32_t entity, KilnEventEntityKey* out, bool* exists) = 0;
    // The players in the game now (entity ids). Returns how many were written.
    virtual uint32_t players(uint32_t* out, uint32_t capacity) = 0;
    // The typed current and maximum of an attribute component (1 Health, 2 Stamina, 3 Mana, 4 Fuel) now. False: not known.
    virtual bool typed(uint32_t entity, uint32_t component, int32_t* current, int32_t* maximum) = 0;
    // Whether the owner of a watch still exists (a mod that unloaded has not).
    virtual bool owner_alive(KilnOwner owner) = 0;
    // kiln.world's session of the server world now (0: not known).
    virtual uint64_t session() = 0;
};

constexpr uint32_t kSetupSize = 84, kInventorySize = 96;
constexpr uint32_t kMaxSetups = 64, kMaxLeaves = 32, kMaxDepth = 3;
constexpr uint64_t kLinkedSerialBit = 1ull << 63;  // life serials of linked entities (never kiln.decisions')
constexpr size_t kMaxTokensPerInterval = 8192;     // over every pair: more are lost (and the records say so)
constexpr size_t kGraveyard = 64;                  // statuses of ended watches kept for status()

inline uint32_t bit(uint32_t component) { return 1u << (component - 1); }
inline uint64_t pair_id(uint32_t entity, uint32_t component) { return (static_cast<uint64_t>(entity) << 8) | component; }
inline bool same_life(const KilnEventEntityKey& a, const KilnEventEntityKey& b) { return std::memcmp(&a, &b, sizeof a) == 0; }

struct Token {
    uint64_t token = 0;
    std::string source;
    bool valid = false;
    uint32_t bytes = 0;
    std::array<uint8_t, KILN_WATCH_MAX_BYTES> before{}, after{};
};

// One change record before it is copied out (the wire record is 1808 bytes; this keeps what is used).
struct Change {
    uint32_t component = 0, origin = 0, flags = 0, attribute = 0;
    uint64_t epoch = 0, step = 0, since = 0, session = 0;
    KilnEventEntityKey key{};
    uint32_t before_bytes = 0, after_bytes = 0;
    std::array<uint8_t, KILN_WATCH_MAX_BYTES> before{}, after{};
    std::vector<uint32_t> roots;
    std::vector<uint64_t> watches, tokens;
    std::string source;
    int32_t value_before = 0, value_after = 0, max_before = 0, max_after = 0;
};

struct Counters {
    uint64_t windows = 0, changes = 0, queue_overflows = 0, frames_dropped = 0;
    uint64_t tokens_recorded = 0, tokens_unwatched = 0, tokens_refused = 0, tokens_lost = 0, tokens_truncated = 0,
             tokens_off_world_thread = 0;
};

// The origin of a change from its tokens (header: "Origin"). Pure: the tests drive it directly.
struct OriginResult {
    uint32_t origin = KILN_CHANGE_ORIGIN_UNKNOWN;
    std::string source;  // the valid sources, distinct, in order, joined by ", "
};
inline OriginResult origin_of(const uint8_t* base, uint32_t base_bytes, bool base_present, const uint8_t* final_bytes,
                              uint32_t final_size, bool final_present, const std::vector<Token>& tokens, bool truncated,
                              bool lost) {
    OriginResult r;
    if (tokens.empty()) return r;  // no token: Unknown, never Game
    std::vector<std::string> sources;
    for (const Token& t : tokens) {
        if (!t.valid) return OriginResult{};  // a source Kiln cannot vouch for: Unknown
        if (std::find(sources.begin(), sources.end(), t.source) == sources.end()) sources.push_back(t.source);
    }
    for (size_t i = 0; i < sources.size(); ++i) r.source += (i ? ", " : "") + sources[i];
    // In the order the tokens were recorded: a token continues the chain when its "before" is a state the chain has
    // reached (a nested write recorded after the write inside it still connects). Absent states never connect.
    std::vector<const uint8_t*> reached;
    std::vector<uint32_t> reached_size;
    if (base_present) reached.push_back(base), reached_size.push_back(base_bytes);
    auto in = [&](const uint8_t* p, uint32_t n) {
        for (size_t i = 0; i < reached.size(); ++i)
            if (reached_size[i] == n && std::memcmp(reached[i], p, n) == 0) return true;
        return false;
    };
    for (const Token& t : tokens)
        if (in(t.before.data(), t.bytes)) reached.push_back(t.after.data()), reached_size.push_back(t.bytes);
    const bool chain = final_present && in(final_bytes, final_size);
    r.origin = sources.size() == 1 && chain && !truncated && !lost ? KILN_CHANGE_ORIGIN_KILN : KILN_CHANGE_ORIGIN_MIXED;
    return r;
}

// The inventory tree of a root: its 8-slot inventories (leaves) in link order, Virtual (5) links followed. Pure on
// Ports.setup. False with *why when the tree cannot be walked whole (it is never used in part).
inline bool walk_tree(Ports& p, uint32_t root, std::vector<uint32_t>* leaves, std::string* why) {
    leaves->clear();
    uint8_t setup[kSetupSize];
    if (!p.setup(root, setup)) return true;  // no InventorySetup: nothing linked
    uint32_t setups = 1;
    struct Path {
        uint32_t ids[kMaxDepth + 2] = {};
        uint32_t n = 0;
        uint32_t* begin() { return ids; }
        uint32_t* end() { return ids + n; }
        void push_back(uint32_t id) { ids[n++] = id; }
        void pop_back() { --n; }
    } path;
    path.push_back(root);
    struct Walk {
        Ports& p;
        std::vector<uint32_t>* leaves;
        std::string* why;
        uint32_t* setups;
        Path* path;
        bool go(const uint8_t* s, uint32_t depth) {
            for (int k = 0; k < 16; ++k) {
                uint32_t id = 0;
                std::memcpy(&id, s + 4 * k, 4);
                if (id == 0) break;
                const uint8_t cat = s[0x40 + k];
                if (cat == 5) {  // Virtual: another InventorySetup
                    if (depth >= kMaxDepth) return *why = "its inventory links nest deeper than 3", false;
                    if (std::find(path->begin(), path->end(), id) != path->end())
                        return *why = "its inventory links form a cycle (entity " + std::to_string(id) + ")", false;
                    if (++*setups > kMaxSetups) return *why = "more than 64 inventory setups are linked", false;
                    uint8_t inner[kSetupSize];
                    if (!p.setup(id, inner))
                        return *why = "a linked inventory setup is missing (entity " + std::to_string(id) + ")", false;
                    path->push_back(id);
                    if (!go(inner, depth + 1)) return false;
                    path->pop_back();
                    continue;
                }
                if (std::find(leaves->begin(), leaves->end(), id) != leaves->end()) continue;  // linked twice: once
                if (leaves->size() >= kMaxLeaves) return *why = "more than 32 inventories are linked", false;
                leaves->push_back(id);
            }
            return true;
        }
    } w{p, leaves, why, &setups, &path};
    return w.go(setup, 0);
}

// FNV-1a 64 over what a watch's plan depends on (watch_core.h Watcher::observe).
struct Fingerprint {
    uint64_t h = 1469598103934665603ull;
    void add(const void* p, size_t n) {
        const auto* b = static_cast<const uint8_t*>(p);
        for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 1099511628211ull;
    }
    template <typename T>
    void add(const T& v) {
        add(&v, sizeof v);
    }
};

class Watcher {
public:
    explicit Watcher(Ports* ports) : p_(ports) {}

    // ---- service state ----
    void set_on(bool on) { on_.store(on); }
    bool on() const { return on_.load(std::memory_order_relaxed); }
    bool stopped() const { return stopped_.load(std::memory_order_relaxed); }
    // Lock-free: is anything watched (record_write and window_wanted ask it first).
    bool any() const { return count_.load(std::memory_order_relaxed) != 0; }

    uint32_t window_wanted() const {
        if (!on() || stopped()) return 0;
        return any() ? 1u : 0u;
    }

    // Lock-free, any thread (a writer asks it before it copies a component): the components (mask, bit(index)) that may
    // be watched on `entity` in this interval. A filter of the pairs (65536 slots by a hash of the id): it can name a
    // component that is not watched, never miss one that is. 0: copy nothing, tell nothing.
    uint32_t watched(uint32_t entity) const {
        if (!on() || stopped() || !any()) return 0;
        const uint32_t s = slot_of(entity);
        return static_cast<uint32_t>(filter_[s >> 3].load(std::memory_order_acquire) >> ((s & 7u) * 8u)) & KILN_WATCH_COMPONENTS_ALL;
    }

    // ---- watches (world thread, world-safe phase) ----
    // Creates a watch. KILN_OK with st->watch_id set: created (Ready). KILN_OK with watch_id 0: refused, st->status and
    // st->reason say why (budget, metadata). KILN_ERR_INVALID_ARGUMENT: a malformed request.
    KilnResult create(KilnOwner owner, const KilnComponentWatchRequestV1& rq, uint64_t epoch, uint64_t step, KilnComponentWatchStatusV1* st) {
        *st = blank();
        if (owner == 0 || rq.size < sizeof(KilnComponentWatchRequestV1)) return KILN_ERR_INVALID_ARGUMENT;
        if (rq.components == 0 || (rq.components & ~KILN_WATCH_COMPONENTS_ALL) || rq.expansion > KILN_WATCH_EXPAND_INVENTORY ||
            rq.scope > KILN_WATCH_SCOPE_PLAYERS)
            return KILN_ERR_INVALID_ARGUMENT;
        std::vector<uint32_t> roots;
        if (rq.scope == KILN_WATCH_SCOPE_ENTITIES) {
            if (rq.entity_count == 0 || rq.entity_count > KILN_WATCH_MAX_ROOTS || !rq.entities) return KILN_ERR_INVALID_ARGUMENT;
            roots.assign(rq.entities, rq.entities + rq.entity_count);
            for (uint32_t e : roots)
                if (e == 0) return KILN_ERR_INVALID_ARGUMENT;
            auto sorted = roots;
            std::sort(sorted.begin(), sorted.end());
            if (std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end()) return KILN_ERR_INVALID_ARGUMENT;
        } else if (rq.entity_count != 0) {
            return KILN_ERR_INVALID_ARGUMENT;
        }
        std::lock_guard lock(m_);
        if (stopped()) return refuse(st, KILN_WATCH_SUSPENDED_METADATA, "kiln.component_changes stopped after a fault"), KILN_OK;
        if (watches_.size() >= KILN_WATCH_MAX_WATCHES)
            return refuse(st, KILN_WATCH_SUSPENDED_BUDGET, "32 watches exist already (dispose one first)"), KILN_OK;
        Watch w;
        w.owner = owner;
        w.scope = rq.scope;
        w.components = rq.components;
        w.expansion = rq.expansion;
        w.roots = roots;
        w.epoch = epoch;
        w.session = p_->session();
        // The roots' lives now: a root that cannot be told from a later entity of its id is refused.
        for (uint32_t e : roots) {
            KilnEventEntityKey k{};
            bool exists = false;
            if (!p_->key(e, &k, &exists))
                return refuse(st, KILN_WATCH_SUSPENDED_METADATA,
                              exists ? "entity " + std::to_string(e) + " has no identity Kiln can follow (no SpawnTime, and not a player in the game)"
                                     : "entity " + std::to_string(e) + " does not exist"),
                       KILN_OK;
            w.root_keys.push_back(k);
        }
        Plan plan;
        std::string why;
        uint32_t why_status = 0;
        if (!expand(w, step, &plan, &why, &why_status)) return refuse(st, why_status, why), KILN_OK;
        if (!fits(plan, &why)) {
            st->pairs = plan.present;
            return refuse(st, KILN_WATCH_SUSPENDED_BUDGET, why), KILN_OK;
        }
        w.id = ++next_id_;
        w.status = KILN_WATCH_READY;
        w.pairs = plan.present;
        w.fresh = true;
        w.reserved = reservation(plan);
        watches_.push_back(std::move(w));
        count_.store(static_cast<uint32_t>(watches_.size()));
        replan_ = true;
        fill(watches_.back(), st);
        return KILN_OK;
    }

    // Resume: KILN_OK and the status (Ready, or the reason it stays suspended); NOT_FOUND: no such watch of `owner`.
    KilnResult resume(KilnOwner owner, uint64_t id, uint64_t step, KilnComponentWatchStatusV1* st) {
        *st = blank();
        std::lock_guard lock(m_);
        Watch* w = find(id);
        if (!w || w->owner != owner) {
            if (const auto* g = grave(id); g && g->owner == owner) return *st = g->status, KILN_OK;  // Disposed, with why
            return KILN_ERR_NOT_FOUND;
        }
        if (w->status == KILN_WATCH_READY) return fill(*w, st), KILN_OK;
        if (w->session != p_->session()) {
            end_watch(*w, "its server world stopped; create a new watch");
            return *st = grave(id)->status, KILN_OK;
        }
        // From scratch: the roots must still be the lives the watch was made for.
        for (size_t i = 0; i < w->roots.size(); ++i) {
            KilnEventEntityKey k{};
            bool exists = false;
            if (!p_->key(w->roots[i], &k, &exists) || !same_life(k, w->root_keys[i])) {
                suspend(*w, KILN_WATCH_SUSPENDED_METADATA, "entity " + std::to_string(w->roots[i]) +
                                                               (exists ? " is another entity now (or has no identity)" : " is gone"));
                return fill(*w, st), KILN_OK;
            }
        }
        Plan plan;
        std::string why;
        uint32_t why_status = 0;
        const uint32_t was = w->status;
        w->status = KILN_WATCH_READY;  // so that fits() does not count it twice
        if (!expand(*w, step, &plan, &why, &why_status)) {
            w->status = was;
            suspend(*w, why_status, why);
            return fill(*w, st), KILN_OK;
        }
        if (!fits(plan, &why, w->id)) {
            w->status = was;
            suspend(*w, KILN_WATCH_SUSPENDED_BUDGET, why);
            return fill(*w, st), KILN_OK;
        }
        w->pairs = plan.present;
        w->reason.clear();
        w->fresh = true;  // a fresh baseline at the next window_begin, no catch-up change
        w->reserved = reservation(plan);
        replan_ = true;
        return fill(*w, st), KILN_OK;
    }

    KilnResult status(uint64_t id, KilnComponentWatchStatusV1* st) {
        *st = blank();
        std::lock_guard lock(m_);
        if (Watch* w = find(id)) return fill(*w, st), KILN_OK;
        if (const auto* g = grave(id)) return *st = g->status, KILN_OK;
        return KILN_ERR_NOT_FOUND;
    }

    KilnResult remove(KilnOwner owner, uint64_t id) {
        std::lock_guard lock(m_);
        Watch* w = find(id);
        if (!w || w->owner != owner) return KILN_ERR_NOT_FOUND;
        end_watch(*w, "disposed");
        return KILN_OK;
    }

    uint32_t release_owner(KilnOwner owner, const char* why = "its owner went away") {
        std::lock_guard lock(m_);
        return release_locked(owner, why);
    }

    // ---- consumer (one) ----
    KilnResult attach(KilnOwner d) {
        std::lock_guard lock(m_);
        if (d == 0) return KILN_ERR_INVALID_ARGUMENT;
        if (consumer_ && consumer_ != d) return KILN_ERR_UNAVAILABLE;
        consumer_ = d;
        return KILN_OK;
    }
    KilnResult detach(KilnOwner d) {
        std::lock_guard lock(m_);
        if (consumer_ != d) return KILN_ERR_UNAVAILABLE;
        consumer_ = 0;
        batch_.clear();
        batch_id_ = 0;
        return KILN_OK;
    }
    bool is_consumer(KilnOwner d) {
        std::lock_guard lock(m_);
        return d != 0 && consumer_ == d;
    }
    KilnResult begin_events(KilnOwner d, uint64_t* batch, uint32_t* records) {
        std::lock_guard lock(m_);
        if (d == 0 || consumer_ != d) return KILN_ERR_UNAVAILABLE;
        if (batch_id_ == 0) {  // freeze what has arrived
            if (frames_.empty()) return *batch = 0, *records = 0, KILN_OK;
            batch_frames_ = static_cast<uint32_t>(frames_.size());
            for (auto& f : frames_)
                for (auto& c : f) batch_.push_back(std::move(c));
            frames_.clear();
            batch_id_ = ++next_batch_;
        }
        *batch = batch_id_;
        *records = static_cast<uint32_t>(batch_.size());
        return KILN_OK;
    }
    KilnResult read_events(KilnOwner d, uint64_t batch, KilnComponentChangeV1* out, uint32_t capacity, uint32_t stride, uint32_t* required) {
        std::lock_guard lock(m_);
        if (d == 0 || consumer_ != d) return KILN_ERR_UNAVAILABLE;
        if (batch == 0 || batch != batch_id_) return KILN_ERR_NOT_FOUND;
        if (required) *required = static_cast<uint32_t>(batch_.size());
        if (stride < sizeof(KilnComponentChangeV1)) return KILN_ERR_INVALID_ARGUMENT;
        if (!out || capacity < batch_.size()) return KILN_ERR_BUFFER_TOO_SMALL;  // writes nothing, consumes nothing
        auto* at = reinterpret_cast<uint8_t*>(out);
        for (const Change& c : batch_) {
            auto* r = reinterpret_cast<KilnComponentChangeV1*>(at);
            to_wire(c, r);
            at += stride;
        }
        return KILN_OK;
    }
    KilnResult ack_events(KilnOwner d, uint64_t batch) {
        std::lock_guard lock(m_);
        if (d == 0 || consumer_ != d) return KILN_ERR_UNAVAILABLE;
        if (batch != 0 && batch == last_acked_) return KILN_OK;  // idempotent
        if (batch == 0 || batch != batch_id_) return KILN_ERR_NOT_FOUND;
        batch_.clear();
        batch_frames_ = 0;
        last_acked_ = batch_id_;
        batch_id_ = 0;
        return KILN_OK;
    }

    // ---- writers (any thread) ----
    // KILN_OK: counted (or nothing is watched); INVALID_ARGUMENT: malformed; NOT_FOUND: another world or life.
    KilnResult record_write(KilnOwner owner, const KilnKnownWriteV1& w, bool on_world_thread) {
        if (!on() || stopped() || !any()) return KILN_OK;  // nothing watched: nothing to do
        if (owner == 0 || w.size < sizeof(KilnKnownWriteV1) || w.component < 1 || w.component > 6 || !w.before || !w.after ||
            w.bytes == 0 || w.bytes > KILN_WATCH_MAX_BYTES || w.entity.entity == 0)
            return KILN_ERR_INVALID_ARGUMENT;
        std::unique_lock lock(m_, std::defer_lock);
        try {
            lock.lock();
        } catch (const LockUnavailable&) {
            lost_.store(true);
            ++lost_count_;
            return KILN_OK;  // the interval's records say a token was lost
        }
        if (w.world_epoch != 0 && w.world_epoch != epoch_) return ++c_.tokens_refused, KILN_ERR_NOT_FOUND;
        auto it = pairs_.find(pair_id(w.entity.entity, w.component));
        if (it == pairs_.end()) return ++c_.tokens_unwatched, KILN_OK;
        Pair& pr = it->second;
        if (w.entity.life_serial != 0 && w.entity.life_serial != pr.key.life_serial) return ++c_.tokens_refused, KILN_ERR_NOT_FOUND;
        ++c_.tokens_recorded;
        if (!on_world_thread) ++c_.tokens_off_world_thread;
        if (pr.tokens.size() >= KILN_WATCH_MAX_TOKENS) {
            pr.truncated = true;
            ++c_.tokens_truncated;
            return KILN_OK;
        }
        if (tokens_in_interval_ >= kMaxTokensPerInterval) {
            pr.lost = true;
            ++c_.tokens_lost;
            return KILN_OK;
        }
        Token t;
        t.token = w.operation_token ? w.operation_token : ++next_token_;
        t.bytes = w.bytes;
        std::memcpy(t.before.data(), w.before, w.bytes);
        std::memcpy(t.after.data(), w.after, w.bytes);
        const size_t n = w.source_mod.len;
        t.valid = w.source_mod.ptr && n >= 1 && n <= 127 && valid_utf8(w.source_mod.ptr, n);
        if (t.valid) t.source.assign(w.source_mod.ptr, n);
        pr.tokens.push_back(std::move(t));
        ++tokens_in_interval_;
        return KILN_OK;
    }

    // ---- windows (core: world thread, no world job live) ----
    void window_begin(uint64_t epoch, uint64_t step) {
        std::lock_guard lock(m_);
        epoch_ = epoch;
        step_ = step;
        if (!on() || stopped()) return;
        ++c_.windows;
        // owners that went away (about once a second)
        if (step >= next_owner_check_) {
            next_owner_check_ = step + 25;
            std::vector<KilnOwner> gone;
            for (const Watch& w : watches_)
                if (std::find(gone.begin(), gone.end(), w.owner) == gone.end() && !p_->owner_alive(w.owner)) gone.push_back(w.owner);
            for (KilnOwner o : gone) release_locked(o, "its mod unloaded");
        }
        const uint64_t session = p_->session();
        for (size_t i = 0; i < watches_.size();) {
            if (watches_[i].session != session) {
                end_watch(watches_[i], "its server world stopped; create a new watch");
                continue;  // end_watch removed it
            }
            ++i;
        }
        // Steady state: no watch was made, resumed, suspended or ended since the last refresh, every root is the same
        // life, and nothing a watch's plan depends on (its players, their links, the item entities in their
        // inventories) changed: the pairs stay as they are, nothing is planned or allocated. A pair that became
        // present still counts against the budget: past it, the full refresh below suspends the watch it brings over.
        bool steady = !replan_;
        for (Watch& w : watches_) {
            if (w.status != KILN_WATCH_READY) continue;
            std::string why;
            uint32_t why_status = 0;
            if (!observe(w, &w.observed, &why, &why_status)) {
                suspend(w, why_status, why);
                steady = false;
                continue;
            }
            if (!w.planned || w.fresh || w.observed != w.fingerprint) steady = false;
        }
        if (steady) {
            uint32_t present = 0;
            for (const auto& [id, pr] : pairs_) present += pr.present ? 1u : 0u;
            if (present <= KILN_WATCH_MAX_PAIRS) {
                present_ = present;
                return;
            }
        }
        replan_ = false;
        // The pairs of this interval, watch by watch in the order they were made; a watch that does not fit is
        // suspended whole before anything of it is compared.
        std::unordered_map<uint64_t, Pair> next;
        next.reserve(pairs_.size() + 64);
        uint32_t present = 0;
        std::unordered_set<uint64_t> fresh_watches;
        for (Watch& w : watches_) {
            if (w.status != KILN_WATCH_READY) continue;
            Plan plan;
            std::string why;
            uint32_t why_status = 0;
            if (!expand(w, step, &plan, &why, &why_status)) {
                suspend(w, why_status, why);
                continue;
            }
            // what this watch adds to the interval's set
            uint32_t added = 0;
            bool fits_roots = true;
            for (const auto& [id, want] : plan.pairs) {
                auto have = next.find(id);
                if (have == next.end()) {
                    if (want.present) ++added;
                    continue;
                }
                if (!same_life(have->second.key, want.key)) {
                    fits_roots = false;
                    why = "entity " + std::to_string(want.key.entity) + " is reached as two different lives";
                    break;
                }
                size_t roots = have->second.roots.size();
                for (uint32_t r : want.roots)
                    if (std::find(have->second.roots.begin(), have->second.roots.end(), r) == have->second.roots.end()) ++roots;
                if (roots > KILN_WATCH_MAX_ROOTS) {
                    fits_roots = false;
                    why = "a component would be reached from more than 128 roots";
                    break;
                }
            }
            if (!fits_roots) {
                suspend(w, KILN_WATCH_SUSPENDED_BUDGET, why);
                continue;
            }
            if (present + added > KILN_WATCH_MAX_PAIRS) {
                suspend(w, KILN_WATCH_SUSPENDED_BUDGET,
                        "it would take the watched pairs past 4096 (" + std::to_string(present + added) + ")");
                continue;
            }
            present += added;
            w.pairs = plan.present;
            w.reserved = reservation(plan);
            if (w.fresh) fresh_watches.insert(w.id);
            w.fresh = false;
            w.planned = true;
            w.fingerprint = w.observed;
            for (auto& [id, want] : plan.pairs) {
                auto [at, inserted] = next.try_emplace(id);
                Pair& pr = at->second;
                if (inserted) {
                    auto old = pairs_.find(id);
                    if (old != pairs_.end() && same_life(old->second.key, want.key)) {
                        pr = std::move(old->second);  // its baseline and the tokens of its interval so far
                        pr.prev_roots = std::move(pr.roots);
                        pr.watches.clear();
                        pr.roots.clear();
                        pr.carried = true;
                    } else {
                        pr = Pair{};
                        pr.key = want.key;
                        pr.component = want.component;
                        baseline(pr, want, step);
                    }
                }
                if (std::find(pr.watches.begin(), pr.watches.end(), w.id) == pr.watches.end()) pr.watches.push_back(w.id);
                for (uint32_t r : want.roots)
                    if (std::find(pr.roots.begin(), pr.roots.end(), r) == pr.roots.end()) pr.roots.push_back(r);
            }
        }
        // A carried pair that gains a watch (one it did not have, or one made or resumed since): what changed since its
        // baseline goes to the watches it had and keeps; the baseline moves to now, so that the new watch sees no
        // change from before it began.
        for (auto& [id, pr] : next) {
            if (pr.carried) {
                bool gains = false;
                std::vector<uint64_t> kept;
                for (uint64_t wid : pr.watches) {
                    const bool had = std::find(pr.prev_watches.begin(), pr.prev_watches.end(), wid) != pr.prev_watches.end();
                    if (!had || fresh_watches.count(wid)) gains = true;
                    else kept.push_back(wid);
                }
                if (gains) split(pr, kept, step);
            }
            pr.carried = false;
            pr.prev_watches = pr.watches;
            pr.prev_roots.clear();
        }
        tokens_in_interval_ = 0;
        for (auto& [id, pr] : next) tokens_in_interval_ += pr.tokens.size();
        pairs_ = std::move(next);
        // A linked life lasts only while at least one ready watch reaches it.
        std::erase_if(linked_, [&](const auto& entry) {
            for (uint32_t c : {KILN_WATCH_FUEL, KILN_WATCH_INVENTORY}) {
                const auto at = pairs_.find(pair_id(entry.first, c));
                if (at != pairs_.end() && at->second.key.life_serial == entry.second.serial) return false;
            }
            return true;
        });
        present_ = present;
        rebuilt();
    }

    void window_end(uint64_t epoch, uint64_t step) {
        std::lock_guard lock(m_);
        if (!on() || stopped() || epoch != epoch_) return;
        // One read of every pair: what differs from its baseline is kept (a reused scratch, nothing is changed yet), and
        // how many pairs are present now. Nothing is emitted before the identity and budget checks below.
        diffs_.clear();
        uint32_t live = 0;
        uint8_t now[KILN_WATCH_MAX_BYTES];
        for (auto& [id, pr] : pairs_) {
            uint32_t size = 0;
            const bool present = p_->read(pr.key.entity, pr.component, now, &size) && size >= 1 && size <= KILN_WATCH_MAX_BYTES;
            pr.live = present;
            live += present ? 1u : 0u;
            if (present == pr.present && (!present || (size == pr.size && std::memcmp(now, pr.base.data(), size) == 0))) continue;
            Diff& d = diffs_.emplace_back();  // the vector keeps its capacity from step to step
            d.pr = &pr;
            d.present = present;
            d.size = present ? size : 0;
            if (present) std::memcpy(d.now.data(), now, size);
        }
        // A root can be destroyed and its id reused inside Sim, after window_begin checked it: the replacement's bytes
        // are never labelled with the old life. Each root entity once (root_lives_, kept with the pairs). An absent root
        // still reports its components disappearing; a live root of another life (or of none) suspends its watches.
        for (const RootLife& r : root_lives_) {
            KilnEventEntityKey key{};
            bool exists = false;
            const bool known = p_->key(r.entity, &key, &exists);
            if (!exists || (known && same_life(key, r.key))) continue;
            for (uint32_t c = KILN_WATCH_HEALTH; c <= KILN_WATCH_SLOT_SELECTION; ++c) {
                const auto at = pairs_.find(pair_id(r.entity, c));
                if (at == pairs_.end()) continue;
                for (uint64_t wid : at->second.watches)
                    if (Watch* w = find(wid); w && w->status == KILN_WATCH_READY)
                        suspend(*w, KILN_WATCH_SUSPENDED_METADATA, "entity " + std::to_string(r.entity) +
                                " is another entity now (or has no identity)");
            }
        }
        // Components absent at the last compare can appear during Sim: past 4096 present pairs, the watches that bring
        // them over are suspended (in the order they were made, a shared pair counted once) before anything is emitted.
        // A component that disappeared frees its place in the same step, so an exchange at the bound fits.
        if (live > KILN_WATCH_MAX_PAIRS) {
            std::unordered_set<uint64_t> accepted;
            for (Watch& w : watches_) {
                if (w.status != KILN_WATCH_READY) continue;
                std::vector<uint64_t> added;
                for (const auto& [id, pr] : pairs_)
                    if (pr.live && !accepted.count(id) && std::find(pr.watches.begin(), pr.watches.end(), w.id) != pr.watches.end())
                        added.push_back(id);
                if (accepted.size() + added.size() > KILN_WATCH_MAX_PAIRS)
                    suspend(w, KILN_WATCH_SUSPENDED_BUDGET,
                            "it would take the watched pairs past 4096 (" + std::to_string(accepted.size() + added.size()) + ")");
                else accepted.insert(added.begin(), added.end());
            }
        }
        auto inactive = [&](uint64_t wid) { const Watch* w = find(wid); return !w || w->status != KILN_WATCH_READY; };
        uint32_t present = 0;
        for (auto& [id, pr] : pairs_) {
            std::erase_if(pr.watches, inactive);
            if (pr.live && !pr.watches.empty()) ++present;
        }
        present_ = present;
        for (Change& c : pending_) std::erase_if(c.watches, inactive);
        std::erase_if(pending_, [](const Change& c) { return c.watches.empty(); });
        const bool lost = lost_.exchange(false);
        std::vector<Change> frame = std::move(pending_);
        pending_.clear();
        for (Diff& d : diffs_) {
            Pair& pr = *d.pr;
            int32_t cur = 0, max = 0;
            const bool typed = d.present && typed_of(pr, &cur, &max);
            if (!pr.watches.empty()) frame.push_back(make_change(pr, d.present, d.now.data(), d.size, step, lost || pr.lost, typed, cur, max));
            pr.present = d.present;
            pr.size = d.size;
            if (d.present) std::memcpy(pr.base.data(), d.now.data(), d.size);
            pr.typed_ok = typed;
            pr.cur = cur;
            pr.max = max;
        }
        diffs_.clear();
        for (auto& [id, pr] : pairs_) {
            pr.since = step;
            pr.tokens.clear();
            pr.truncated = pr.lost = false;
        }
        tokens_in_interval_ = 0;
        if (!frame.empty()) {
            if (frames_.size() + batch_frames_ >= KILN_WATCH_MAX_FRAMES) {
                // the consumer is behind: nothing is advertised as complete that was not delivered
                ++c_.queue_overflows;
                ++c_.frames_dropped;
                for (Watch& w : watches_)
                    if (w.status == KILN_WATCH_READY)
                        suspend(w, KILN_WATCH_SUSPENDED_BUDGET, "the change queue is full (two steps were not read); resume it");
                return;
            }
            c_.changes += frame.size();
            frames_.push_back(std::move(frame));
        }
        for (Watch& w : watches_)
            if (w.status == KILN_WATCH_READY) w.last_complete_step = step;
        last_complete_step_ = step;
    }

    // The world stops (KILN_WORLD_STOPPING): every watch ends (its entity ids mean nothing in the next world), what
    // was not read is dropped (counted). Also after a fault (stop()).
    struct Stopped {
        uint32_t watches = 0, frames = 0, records = 0;
    };
    Stopped world_stopped(const char* why) {
        std::lock_guard lock(m_);
        Stopped out;
        out.watches = static_cast<uint32_t>(watches_.size());
        out.frames = static_cast<uint32_t>(frames_.size()) + batch_frames_;
        for (const auto& f : frames_) out.records += static_cast<uint32_t>(f.size());
        out.records += static_cast<uint32_t>(batch_.size() + pending_.size());
        while (!watches_.empty()) end_watch(watches_.front(), why);
        pairs_.clear();
        linked_.clear();
        pending_.clear();
        rebuilt();
        replan_ = true;
        c_.frames_dropped += frames_.size();
        frames_.clear();
        // a batch frozen but not acked yet goes too: its entity ids would mean the next world's entities
        c_.frames_dropped += batch_frames_;
        batch_.clear();
        batch_id_ = 0;
        batch_frames_ = 0;
        present_ = 0;
        return out;
    }
    void stop(const char* why) {
        stopped_.store(true);
        std::unique_lock lock(m_, std::defer_lock);
        if (!lock.try_lock()) return;  // a lock left held: nothing more is touched
        stop_reason_ = why;
        while (!watches_.empty()) end_watch(watches_.front(), why);
        pairs_.clear();
        rebuilt();
        frames_.clear();
        pending_.clear();
        batch_.clear();
        batch_id_ = 0;
        count_.store(0);
    }

    void counters(KilnComponentChangesCountersV1* out) {
        const uint32_t size = out->size;
        std::memset(out, 0, sizeof *out);
        out->size = size;
        out->state = stopped() ? 2u : on() ? 1u : 0u;
        std::unique_lock lock(m_, std::defer_lock);
        if (!lock.try_lock()) return;
        out->watches = static_cast<uint32_t>(watches_.size());
        out->pairs = static_cast<uint32_t>(pairs_.size());
        out->present_pairs = present_;
        uint32_t queued = 0;
        for (const auto& f : frames_) queued += static_cast<uint32_t>(f.size());
        out->queued_records = queued + static_cast<uint32_t>(batch_.size());
        out->queued_frames = static_cast<uint32_t>(frames_.size()) + batch_frames_;
        out->world_epoch = epoch_;
        out->last_complete_step = last_complete_step_;
        out->windows = c_.windows;
        out->changes = c_.changes;
        out->queue_overflows = c_.queue_overflows;
        out->frames_dropped = c_.frames_dropped;
        out->tokens_recorded = c_.tokens_recorded;
        out->tokens_unwatched = c_.tokens_unwatched;
        out->tokens_refused = c_.tokens_refused;
        out->tokens_lost = c_.tokens_lost + lost_count_.load();
        out->tokens_truncated = c_.tokens_truncated;
        out->tokens_off_world_thread = c_.tokens_off_world_thread;
        auto pct = [](std::vector<uint64_t> v, double q) -> uint64_t {
            if (v.empty()) return 0;
            std::sort(v.begin(), v.end());
            return v[std::min(v.size() - 1, static_cast<size_t>(q * static_cast<double>(v.size() - 1) + 0.5))];
        };
        out->begin_ns_p50 = pct(begin_ns_, 0.5);
        out->begin_ns_p99 = pct(begin_ns_, 0.99);
        out->end_ns_p50 = pct(end_ns_, 0.5);
        out->end_ns_p99 = pct(end_ns_, 0.99);
        out->samples = std::min(begin_ns_.size(), end_ns_.size());
        for (uint64_t v : begin_ns_) out->begin_ns_max = std::max(out->begin_ns_max, v);
        for (uint64_t v : end_ns_) out->end_ns_max = std::max(out->end_ns_max, v);
    }
    // The service measures its window calls (only those with pairs) and gives them here.
    void timed(bool begin, uint64_t ns) {
        std::unique_lock lock(m_, std::defer_lock);
        if (!lock.try_lock() || pairs_.empty()) return;
        auto& v = begin ? begin_ns_ : end_ns_;
        auto& at = begin ? begin_at_ : end_at_;
        if (v.size() < 1024) v.push_back(ns);
        else v[at++ % 1024] = ns;
    }

    uint64_t epoch() const { return epoch_; }

private:
    struct Reserved {
        uint64_t id = 0;
        bool present = false;
        std::vector<uint32_t> roots;
    };
    struct Watch {
        uint64_t id = 0;
        KilnOwner owner = 0;
        uint32_t scope = 0, components = 0, expansion = 0, status = KILN_WATCH_READY, pairs = 0;
        std::vector<uint32_t> roots;
        std::vector<KilnEventEntityKey> root_keys;
        std::string reason;
        uint64_t last_complete_step = 0, epoch = 0, session = 0;
        bool fresh = true;
        std::vector<Reserved> reserved;  // its pairs as last planned (the budget counts them before they are compared)
        bool planned = false;            // its pairs are in pairs_ as planned from `fingerprint`
        uint64_t fingerprint = 0, observed = 0;
    };
    struct Pair {
        KilnEventEntityKey key{};
        uint32_t component = 0;
        bool present = false, typed_ok = false, carried = false, truncated = false, lost = false;
        bool live = false;  // window_end's scratch: present at its read
        uint32_t size = 0;
        int32_t cur = 0, max = 0;
        uint64_t since = 0;
        std::array<uint8_t, KILN_WATCH_MAX_BYTES> base{};
        std::vector<uint64_t> watches, prev_watches;
        std::vector<uint32_t> roots, prev_roots;
        std::vector<Token> tokens;
    };
    // window_end's scratch: a pair whose bytes or presence differ from its baseline, and what they are now.
    struct Diff {
        Pair* pr = nullptr;
        bool present = false;
        uint32_t size = 0;
        std::array<uint8_t, KILN_WATCH_MAX_BYTES> now;
    };
    // A root entity of the pairs (not a linked one) and the life its pairs were planned for: window_end asks each once.
    struct RootLife {
        uint32_t entity = 0;
        KilnEventEntityKey key{};
    };
    struct Want {
        KilnEventEntityKey key{};
        uint32_t component = 0;
        bool present = false;
        uint32_t size = 0;
        std::vector<uint8_t> bytes;  // read for this plan: a pair new to the service (empty otherwise: no copy per step)
        std::vector<uint32_t> roots;
    };
    struct Plan {
        std::map<uint64_t, Want> pairs;
        uint32_t present = 0;
    };
    struct Grave {
        uint64_t id = 0;
        KilnOwner owner = 0;
        KilnComponentWatchStatusV1 status{};
    };
    struct Linked {
        uint64_t serial = 0;
        uint32_t generation = 0;  // an item entity: the item id in its slot
        uint64_t seen = 0;
    };

    static KilnComponentWatchStatusV1 blank() {
        KilnComponentWatchStatusV1 s{};
        s.size = sizeof s;
        s.limit = KILN_WATCH_MAX_PAIRS;
        return s;
    }
    static void copy_reason(char* dst, const std::string& s) { std::snprintf(dst, 192, "%s", s.c_str()); }
    static void refuse(KilnComponentWatchStatusV1* st, uint32_t status, const std::string& why) {
        st->status = status;
        st->watch_id = 0;
        copy_reason(st->reason, why);
    }
    void fill(const Watch& w, KilnComponentWatchStatusV1* st) const {
        *st = blank();
        st->status = w.status;
        st->pairs = w.pairs;
        st->watch_id = w.id;
        st->last_complete_step = w.last_complete_step;
        copy_reason(st->reason, w.reason);
    }
    Watch* find(uint64_t id) {
        for (Watch& w : watches_)
            if (w.id == id) return &w;
        return nullptr;
    }
    const Grave* grave(uint64_t id) const {
        for (const Grave& g : graves_)
            if (g.id == id) return &g;
        return nullptr;
    }
    void suspend(Watch& w, uint32_t status, const std::string& why) {
        w.status = status;
        w.reason = why;
        w.pairs = 0;
        w.reserved.clear();
        w.planned = false;
        replan_ = true;
    }
    void end_watch(Watch& w, const std::string& why) {
        Grave g;
        g.id = w.id;
        g.owner = w.owner;
        w.status = KILN_WATCH_DISPOSED;
        w.reason = why;
        fill(w, &g.status);
        g.status.pairs = 0;
        graves_.push_back(g);
        if (graves_.size() > kGraveyard) graves_.pop_front();
        const uint64_t id = w.id;
        watches_.erase(std::remove_if(watches_.begin(), watches_.end(), [&](const Watch& x) { return x.id == id; }), watches_.end());
        count_.store(static_cast<uint32_t>(watches_.size()));
        replan_ = true;
        // what it was told and nobody has read goes with it: no later watch (its mod loaded again) is told of a change
        // from before it existed, and a record names only watches that are still there. A batch already frozen is left
        // as it is (a read of it gives the same records until it is acked).
        auto purge = [id](std::vector<Change>& v) {
            for (Change& c : v) c.watches.erase(std::remove(c.watches.begin(), c.watches.end(), id), c.watches.end());
            v.erase(std::remove_if(v.begin(), v.end(), [](const Change& c) { return c.watches.empty(); }), v.end());
        };
        for (auto it = frames_.begin(); it != frames_.end();) {
            purge(*it);
            if (it->empty()) {
                ++c_.frames_dropped;
                it = frames_.erase(it);
            } else {
                ++it;
            }
        }
        purge(pending_);
        if (watches_.empty()) {  // nothing watched: nothing kept (no window call comes to clear it)
            pairs_.clear();
            linked_.clear();
            pending_.clear();
            present_ = 0;
            rebuilt();
        }
    }
    static uint32_t slot_of(uint32_t entity) { return (entity * 0x9E3779B1u) >> 16; }
    // pairs_ was rebuilt (or cleared): the root lives window_end checks and the writers' filter follow it. The filter
    // word by word: the new bits are set before the old ones are cleared, so a pair watched before and after never
    // reads as unwatched, from any thread.
    void rebuilt() {
        root_lives_.clear();
        filter_next_.fill(0);
        for (const auto& [id, pr] : pairs_) {
            const uint32_t s = slot_of(pr.key.entity);
            filter_next_[s >> 3] |= static_cast<uint64_t>(bit(pr.component)) << ((s & 7u) * 8u);
            if (!(pr.key.life_serial & kLinkedSerialBit)) root_lives_.push_back(RootLife{pr.key.entity, pr.key});
        }
        std::sort(root_lives_.begin(), root_lives_.end(), [](const RootLife& a, const RootLife& b) { return a.entity < b.entity; });
        root_lives_.erase(std::unique(root_lives_.begin(), root_lives_.end(), [](const RootLife& a, const RootLife& b) { return a.entity == b.entity; }),
                          root_lives_.end());
        for (size_t i = 0; i < filter_next_.size(); ++i) {
            const uint64_t want = filter_next_[i];
            if (filter_[i].load(std::memory_order_relaxed) == want) continue;
            filter_[i].fetch_or(want, std::memory_order_release);
            filter_[i].store(want, std::memory_order_release);
        }
    }
    uint32_t release_locked(KilnOwner owner, const char* why) {
        uint32_t n = 0;
        for (size_t i = 0; i < watches_.size();) {
            if (watches_[i].owner == owner) {
                end_watch(watches_[i], why);
                ++n;
                continue;
            }
            ++i;
        }
        return n;
    }

    // What a watch's plan depends on, read now and hashed (no plan is built): the roots' lives (Entities: suspended when
    // one is gone or another life; Players: the players and their sessions), and with the expansion the inventories
    // linked from each root and the item entities in them. False with the reason when a root cannot be followed.
    bool observe(Watch& w, uint64_t* out, std::string* why, uint32_t* why_status) {
        Fingerprint f;
        uint32_t ids[1024];
        uint32_t n = 0;
        if (w.scope == KILN_WATCH_SCOPE_ENTITIES) {
            for (size_t i = 0; i < w.roots.size(); ++i) {
                KilnEventEntityKey k{};
                bool exists = false;
                if (!p_->key(w.roots[i], &k, &exists) || !same_life(k, w.root_keys[i])) {
                    *why = "entity " + std::to_string(w.roots[i]) + (exists ? " is another entity now (or has no identity)" : " is gone");
                    *why_status = KILN_WATCH_SUSPENDED_METADATA;
                    return false;
                }
                ids[n++] = w.roots[i];
            }
        } else {
            n = p_->players(ids, 1024);
            for (uint32_t i = 0; i < n; ++i) {
                KilnEventEntityKey k{};
                bool exists = false;
                if (p_->key(ids[i], &k, &exists)) f.add(k);
                else f.add(ids[i]);
            }
        }
        f.add(n);
        const bool inv = (w.components & bit(KILN_WATCH_INVENTORY)) != 0, fuel = (w.components & bit(KILN_WATCH_FUEL)) != 0;
        if (w.expansion == KILN_WATCH_EXPAND_INVENTORY && (inv || fuel)) {
            for (uint32_t i = 0; i < n; ++i) {
                if (!walk_tree(*p_, ids[i], &leaves_, why)) {
                    *why = "entity " + std::to_string(ids[i]) + ": " + *why;
                    *why_status = KILN_WATCH_SUSPENDED_METADATA;
                    return false;
                }
                f.add(ids[i]);
                f.add(static_cast<uint32_t>(leaves_.size()));
                for (uint32_t leaf : leaves_) {
                    f.add(leaf);
                    if (!fuel) continue;
                    uint8_t stacks[KILN_WATCH_MAX_BYTES];
                    uint32_t size = 0;
                    if (!p_->read(leaf, KILN_WATCH_INVENTORY, stacks, &size) || size < kInventorySize) {
                        f.add(0u);
                        continue;
                    }
                    for (uint32_t k = 0; k < 8; ++k) {  // what the plan takes of a stack: its item and item entity
                        uint32_t item = 0, count = 0, pide = 0;
                        std::memcpy(&item, stacks + 12 * k, 4);
                        std::memcpy(&count, stacks + 12 * k + 4, 4);
                        std::memcpy(&pide, stacks + 12 * k + 8, 4);
                        if (pide == 0xFFFFFFFFu) pide = 0;
                        if (!item || !count || !pide) continue;
                        f.add(item);
                        f.add(pide);
                    }
                }
            }
        }
        *out = f.h;
        return true;
    }

    // The pairs of one watch now. False with the reason (and the status it means) when a root or its links cannot be
    // followed whole.
    bool expand(Watch& w, uint64_t step, Plan* plan, std::string* why, uint32_t* why_status) {
        std::vector<std::pair<uint32_t, KilnEventEntityKey>> roots;
        if (w.scope == KILN_WATCH_SCOPE_ENTITIES) {
            for (size_t i = 0; i < w.roots.size(); ++i) roots.emplace_back(w.roots[i], w.root_keys[i]);
        } else {
            uint32_t ids[1024];
            const uint32_t n = p_->players(ids, 1024);
            for (uint32_t i = 0; i < n; ++i) {
                KilnEventEntityKey k{};
                bool exists = false;
                if (p_->key(ids[i], &k, &exists)) roots.emplace_back(ids[i], k);  // a player without a session yet: next step
            }
        }
        const uint32_t root_mask = w.expansion == KILN_WATCH_EXPAND_INVENTORY
                                       ? w.components & ~(bit(KILN_WATCH_INVENTORY) | bit(KILN_WATCH_FUEL))
                                       : w.components;
        for (const auto& [root, key] : roots) {
            for (uint32_t c = 1; c <= 6; ++c)
                if (root_mask & bit(c)) add(plan, key, c, root);
            if (w.expansion != KILN_WATCH_EXPAND_INVENTORY) continue;
            const bool inv = (w.components & bit(KILN_WATCH_INVENTORY)) != 0, fuel = (w.components & bit(KILN_WATCH_FUEL)) != 0;
            if (!inv && !fuel) continue;
            std::vector<uint32_t> leaves;
            if (!walk_tree(*p_, root, &leaves, why)) {
                *why = "entity " + std::to_string(root) + ": " + *why;
                *why_status = KILN_WATCH_SUSPENDED_METADATA;
                return false;
            }
            for (uint32_t leaf : leaves) {
                const KilnEventEntityKey lk = linked(leaf, 0, step);
                if (inv) add(plan, lk, KILN_WATCH_INVENTORY, root);
                if (!fuel) continue;
                uint8_t stacks[KILN_WATCH_MAX_BYTES];
                uint32_t size = 0;
                if (!p_->read(leaf, KILN_WATCH_INVENTORY, stacks, &size) || size < kInventorySize) continue;
                for (uint32_t k = 0; k < 8; ++k) {
                    uint32_t item = 0, count = 0, pide = 0;
                    std::memcpy(&item, stacks + 12 * k, 4);
                    std::memcpy(&count, stacks + 12 * k + 4, 4);
                    std::memcpy(&pide, stacks + 12 * k + 8, 4);
                    if (pide == 0xFFFFFFFFu) pide = 0;  // "no entity"
                    if (!item || !count || !pide) continue;  // an empty stack, or a stack without an item entity
                    add(plan, linked(pide, item, step), KILN_WATCH_FUEL, root);
                }
            }
        }
        // presence: from the pair the service has, or read now (the baseline of a pair new to the service)
        for (auto& [id, want] : plan->pairs) {
            auto have = pairs_.find(id);
            if (have != pairs_.end() && same_life(have->second.key, want.key)) {
                want.present = have->second.present;
            } else {
                uint8_t now[KILN_WATCH_MAX_BYTES];
                want.present = p_->read(want.key.entity, want.component, now, &want.size) && want.size >= 1 &&
                               want.size <= KILN_WATCH_MAX_BYTES;
                if (want.present) want.bytes.assign(now, now + want.size);
                else want.size = 0;
            }
            if (want.present) ++plan->present;
        }
        return true;
    }
    static void add(Plan* plan, const KilnEventEntityKey& key, uint32_t component, uint32_t root) {
        Want& want = plan->pairs[pair_id(key.entity, component)];
        want.key = key;
        want.component = component;
        if (std::find(want.roots.begin(), want.roots.end(), root) == want.roots.end()) want.roots.push_back(root);
    }
    KilnEventEntityKey linked(uint32_t entity, uint32_t generation, uint64_t step) {
        Linked& l = linked_[entity];
        if (l.serial == 0 || l.generation != generation) l = Linked{kLinkedSerialBit | ++next_linked_, generation, step};
        l.seen = step;
        KilnEventEntityKey k{};
        k.entity = entity;
        k.life_serial = l.serial;
        return k;
    }
    // Would `plan` fit beside the ready watches (other than `except`)? Their reservations count, also those of
    // watches made since the last window_begin.
    bool fits(const Plan& plan, std::string* why, uint64_t except = 0) {
        std::unordered_set<uint64_t> present;
        std::unordered_map<uint64_t, std::vector<uint32_t>> roots;
        auto take = [&](uint64_t id, bool is_present, const std::vector<uint32_t>& rs) {
            if (is_present) present.insert(id);
            auto& r = roots[id];
            for (uint32_t x : rs)
                if (std::find(r.begin(), r.end(), x) == r.end()) r.push_back(x);
            return r.size() <= KILN_WATCH_MAX_ROOTS;
        };
        for (const Watch& w : watches_)
            if (w.id != except && w.status == KILN_WATCH_READY)
                for (const Reserved& r : w.reserved) take(r.id, r.present, r.roots);
        for (const auto& [id, want] : plan.pairs)
            if (!take(id, want.present, want.roots)) return *why = "a component would be reached from more than 128 roots", false;
        if (present.size() > KILN_WATCH_MAX_PAIRS)
            return *why = "it needs " + std::to_string(plan.present) + " present (entity, component) pairs and the watches would hold " +
                          std::to_string(present.size()) + " of at most 4096",
                   false;
        return true;
    }
    static std::vector<Reserved> reservation(const Plan& plan) {
        std::vector<Reserved> v;
        v.reserve(plan.pairs.size());
        for (const auto& [id, want] : plan.pairs) v.push_back(Reserved{id, want.present, want.roots});
        return v;
    }
    void baseline(Pair& pr, const Want& want, uint64_t step) {
        pr.present = want.present;
        pr.size = want.present ? want.size : 0;
        if (want.present) std::memcpy(pr.base.data(), want.bytes.data(), want.size);
        pr.typed_ok = want.present && typed_of(pr, &pr.cur, &pr.max);
        pr.since = step;
        pr.tokens.clear();  // writes before the baseline do not belong to its interval
        pr.truncated = pr.lost = false;
    }
    bool typed_of(const Pair& pr, int32_t* cur, int32_t* max) {
        return pr.component <= KILN_WATCH_FUEL && p_->typed(pr.key.entity, pr.component, cur, max);
    }
    // A carried pair gains a watch: compare now. A change goes to the watches it had and keeps (`kept`), with its
    // old roots; the baseline moves here either way.
    void split(Pair& pr, const std::vector<uint64_t>& kept, uint64_t step) {
        uint8_t now[KILN_WATCH_MAX_BYTES];
        uint32_t size = 0;
        const bool present = p_->read(pr.key.entity, pr.component, now, &size) && size >= 1 && size <= KILN_WATCH_MAX_BYTES;
        const bool changed = present != pr.present || (present && (size != pr.size || std::memcmp(now, pr.base.data(), size) != 0));
        int32_t cur = 0, max = 0;
        const bool typed = present && typed_of(pr, &cur, &max);
        if (changed && !kept.empty()) {
            Change c = make_change(pr, present, now, size, step, lost_.load() || pr.lost, typed, cur, max);
            c.watches = kept;
            c.roots = pr.prev_roots.empty() ? pr.roots : pr.prev_roots;
            pending_.push_back(std::move(c));
        }
        if (changed) {
            pr.present = present;
            pr.size = present ? size : 0;
            if (present) std::memcpy(pr.base.data(), now, size);
            pr.typed_ok = typed;
            pr.cur = cur;
            pr.max = max;
        }
        pr.since = step;
        pr.tokens.clear();
        pr.truncated = pr.lost = false;
    }
    Change make_change(const Pair& pr, bool present, const uint8_t* now, uint32_t size, uint64_t step, bool lost, bool typed,
                       int32_t cur, int32_t max) {
        Change c;
        c.component = pr.component;
        c.epoch = epoch_;
        c.step = step;
        c.since = pr.since;
        c.session = p_->session();
        c.key = pr.key;
        c.before_bytes = pr.present ? pr.size : 0;
        c.after_bytes = present ? size : 0;
        if (pr.present) std::memcpy(c.before.data(), pr.base.data(), pr.size);
        if (present) std::memcpy(c.after.data(), now, size);
        c.flags = (pr.present ? KILN_CHANGE_BEFORE_PRESENT : 0u) | (present ? KILN_CHANGE_AFTER_PRESENT : 0u) |
                  (pr.truncated ? KILN_CHANGE_TOKENS_TRUNCATED : 0u) | (lost ? KILN_CHANGE_TOKENS_LOST : 0u);
        const OriginResult o = origin_of(pr.base.data(), pr.size, pr.present, now, size, present, pr.tokens, pr.truncated, lost);
        c.origin = o.origin;
        c.source = o.source;
        for (const Token& t : pr.tokens) c.tokens.push_back(t.token);
        c.watches = pr.watches;
        c.roots = pr.roots;
        if (pr.typed_ok && typed) {
            c.attribute = pr.component;  // 1 Health, 2 Stamina, 3 Mana, 4 Fuel: the same numbers
            c.value_before = pr.cur;
            c.max_before = pr.max;
            c.value_after = cur;
            c.max_after = max;
        }
        return c;
    }
    static void to_wire(const Change& c, KilnComponentChangeV1* r) {
        std::memset(r, 0, sizeof *r);
        r->size = sizeof *r;
        r->version = 1;
        r->kind = 1;
        r->component = c.component;
        r->world_epoch = c.epoch;
        r->step = c.step;
        r->entity = c.key;
        r->before_bytes = c.before_bytes;
        r->after_bytes = c.after_bytes;
        r->origin = c.origin;
        r->flags = c.flags;
        r->root_count = static_cast<uint32_t>(std::min<size_t>(c.roots.size(), 128));
        r->watch_count = static_cast<uint32_t>(std::min<size_t>(c.watches.size(), 32));
        r->token_count = static_cast<uint32_t>(std::min<size_t>(c.tokens.size(), 32));
        for (uint32_t i = 0; i < r->root_count; ++i) r->roots[i] = c.roots[i];
        for (uint32_t i = 0; i < r->watch_count; ++i) r->watches[i] = c.watches[i];
        for (uint32_t i = 0; i < r->token_count; ++i) r->tokens[i] = c.tokens[i];
        std::memcpy(r->before, c.before.data(), c.before_bytes);
        std::memcpy(r->after, c.after.data(), c.after_bytes);
        std::snprintf(r->source_mod, sizeof r->source_mod, "%s", c.source.c_str());
        r->world_kind = 0;  // KILN_WORLD_SERVER
        r->attribute = c.attribute;
        r->value_before = c.value_before;
        r->value_after = c.value_after;
        r->maximum_before = c.max_before;
        r->maximum_after = c.max_after;
        r->since_step = c.since;
        r->world_session = c.session;
    }
    static bool valid_utf8(const char* s, size_t n) {
        for (size_t i = 0; i < n;) {
            const auto b = static_cast<uint8_t>(s[i]);
            if (b == 0) return false;
            const size_t len = b < 0x80 ? 1 : (b >> 5) == 6 ? 2 : (b >> 4) == 14 ? 3 : (b >> 3) == 30 ? 4 : 0;
            if (len == 0 || i + len > n) return false;
            uint32_t scalar = b & (len == 1 ? 0x7fu : len == 2 ? 0x1fu : len == 3 ? 0x0fu : 0x07u);
            for (size_t k = 1; k < len; ++k) {
                const auto next = static_cast<uint8_t>(s[i + k]);
                if ((next >> 6) != 2) return false;
                scalar = (scalar << 6) | (next & 0x3f);
            }
            if ((len == 2 && scalar < 0x80) || (len == 3 && scalar < 0x800) || (len == 4 && scalar < 0x10000) ||
                scalar > 0x10ffff || (scalar >= 0xd800 && scalar <= 0xdfff)) return false;
            i += len;
        }
        return true;
    }

    Ports* p_;
    mutable BoundedMutex m_;
    std::atomic<bool> on_{false}, stopped_{false}, lost_{false};
    std::atomic<uint32_t> count_{0};
    std::atomic<uint64_t> lost_count_{0};
    std::vector<Watch> watches_;
    std::deque<Grave> graves_;
    std::unordered_map<uint64_t, Pair> pairs_;
    std::unordered_map<uint32_t, Linked> linked_;
    std::deque<std::vector<Change>> frames_;
    std::vector<Change> pending_;  // split records of this step's window_begin: they join its frame
    std::vector<Change> batch_;
    uint32_t batch_frames_ = 0, present_ = 0;
    uint64_t batch_id_ = 0, next_batch_ = 0, last_acked_ = 0;
    KilnOwner consumer_ = 0;
    uint64_t next_id_ = 0, next_token_ = 0, next_linked_ = 0, epoch_ = 0, step_ = 0, last_complete_step_ = 0, next_owner_check_ = 0;
    size_t tokens_in_interval_ = 0;
    bool replan_ = true;              // the watch set changed since the last refresh
    std::vector<uint32_t> leaves_;    // observe()'s scratch (no allocation per step once grown)
    std::vector<Diff> diffs_;         // window_end's scratch
    std::vector<RootLife> root_lives_;  // the distinct root entities of pairs_, rebuilt with it
    static constexpr uint32_t kFilterSlots = 1u << 16;            // watched(): 8 slots of 8 bits per word, 64 KB
    std::array<std::atomic<uint64_t>, kFilterSlots / 8> filter_{};
    std::array<uint64_t, kFilterSlots / 8> filter_next_{};
    std::string stop_reason_;
    Counters c_;
    std::vector<uint64_t> begin_ns_, end_ns_;
    size_t begin_at_ = 0, end_at_ = 0;
};

}  // namespace kiln::decisions::watch
