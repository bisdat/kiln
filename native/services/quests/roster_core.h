// SPDX-License-Identifier: MIT
// kiln.knowledge's roster (package 17c, chapter 17.3, docs/re/npc-roster.md) without Windows or game memory, so
// tests/roster_test.cpp runs it on the host:
//  - Limit: the one runtime maximum the builder, the catalog, the sidecar and the budget use: 64, or 128 once the
//    second page is installed on this side (decision I7). The catalog's 4,096 stays a corruption guard only.
//  - eval_pages: the game's evaluator run once per page of at most 64 entries (the game's own BTS takes an index
//    modulo 64), the result put back into the game's 16-byte ABI.
//  - PendingStack: what a paged evaluation left for the SetUnlocks that follows it on the same thread.
//  - Sidecars: the extra page of each registry, valid only while the registry still holds the page-0 value, version
//    and collection it was computed with.
//  - Snapshots and the three reads (roster_world, roster_budget, npc_unlock_at): immutable per-world snapshots,
//    answered from any thread, the world named by the core session's per-kind counter.
#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "kiln_knowledge.h"
#include "services/kiln_capacity_util.hpp"

namespace kiln::roster {

constexpr uint32_t kPage = 64;                  // bits in the game's unlock value
constexpr uint32_t kPages = 2;                  // pages Kiln keeps per registry (decision I7)
constexpr uint32_t kExpanded = kPage * kPages;  // 128 entries, the game's included
constexpr uint32_t kReadGuard = 4096;           // a collection this long is damaged, never a capacity
constexpr uint32_t kKindUnknown = 0xffffffffu;

inline bool valid_kind(uint32_t kind) { return kind == KILN_WORLD_SERVER || kind == KILN_WORLD_CLIENT; }

// ---------------------------------------------------------------------------------------------- the maximum ------
class Limit {
public:
    uint32_t max() const { return expanded() ? kExpanded : kPage; }
    bool expanded() const { return expanded_.load(std::memory_order_acquire); }
    void expand() { expanded_.store(true, std::memory_order_release); }
    void reset_for_tests() { expanded_.store(false); }

private:
    std::atomic<bool> expanded_{false};
};

// The builder's refusal text (questbuild.cpp's guard, kept word for word at 64).
inline std::string refusal_text(uint32_t max, uint32_t game, size_t added) {
    char t[200];
    if (max <= kPage)
        std::snprintf(t, sizeof t, "the roster holds at most %u NPCs (a 64-bit unlock mask): %u of the game's + %zu new", max, game, added);
    else
        std::snprintf(t, sizeof t, "the roster holds at most %u NPCs (two 64-bit unlock pages): %u of the game's + %zu new", max, game, added);
    return t;
}

// ---------------------------------------------------------------------------------------------- paging ---------
// The game's result: u8 status at +0 (0 valid), u32 at +4 (0), u64 payload at +8.
struct Result16 {
    uint8_t status;
    uint8_t pad[3];
    uint32_t zero;
    uint64_t payload;
};
static_assert(sizeof(Result16) == 16 && offsetof(Result16, payload) == 8, "the game's 16-byte result");

// The evaluator's span: the registry's NpcResource array.
struct Span {
    uintptr_t ptr;
    uint64_t count;
};
static_assert(sizeof(Span) == 16, "the game's span");

struct PageOutcome {
    bool paged = false;     // more than one page: the original ran once per page
    bool complete = false;  // every page answered status 0 at one version: masks and version are the evaluation
    uint32_t pages = 0;
    uint64_t masks[kPages] = {};
    uint16_t version = 0;
};

// Runs `eval(Result16* out, void* ctx, uint16_t* seen, const Span* span)` (the game's evaluator, through the hook's
// continuation) on pages of at most 64 entries, each with its own copy of the incoming version and its own result,
// and writes the game's ABI answer into *out:
//  - every page status 0 at one version: page 0's result (status 0, page 0's mask); *seen = that version;
//  - a page that answered anything else (6 unchanged, 1 failed): that status with payload 0, *seen untouched (the
//    game then keeps its cache, and so does the sidecar);
//  - pages that answered status 0 at different versions (the knowledge moved between two pages): `error_status`
//    (the evaluator's own failure code) with payload 0, *seen untouched: the next step computes again.
// Never calls eval with more than 64 entries. Spans of 64 or fewer entries, and spans longer than kExpanded (a
// collection Kiln did not build and cannot page), go to eval once, as they are.
template <class Eval>
PageOutcome eval_pages(Result16* out, void* ctx, uint16_t* seen, const Span& span, uint64_t stride, uint8_t error_status,
                       Eval&& eval) {
    PageOutcome o;
    if (span.count <= kPage || span.count > kExpanded || stride == 0) {
        Span s = span;
        eval(out, ctx, seen, &s);
        return o;
    }
    o.paged = true;
    o.pages = static_cast<uint32_t>((span.count + kPage - 1) / kPage);
    Result16 first{};
    bool versions_agree = true;
    for (uint32_t p = 0; p < o.pages; ++p) {
        Span page{span.ptr + static_cast<uintptr_t>(p) * kPage * stride, std::min<uint64_t>(kPage, span.count - uint64_t{p} * kPage)};
        uint16_t version = *seen;
        Result16 r{};
        eval(&r, ctx, &version, &page);
        if (p == 0) first = r;
        if (r.status != 0) {
            std::memcpy(out, &r, sizeof r);
            out->zero = 0;
            out->payload = 0;
            return o;  // not complete: the cache stays
        }
        o.masks[p] = r.payload;
        if (p == 0) o.version = version;
        else if (version != o.version) versions_agree = false;
    }
    if (!versions_agree) {
        std::memcpy(out, &first, sizeof first);
        out->status = error_status;
        out->zero = 0;
        out->payload = 0;
        return o;
    }
    std::memcpy(out, &first, sizeof first);
    *seen = o.version;
    o.complete = true;
    return o;
}

// ---------------------------------------------------------------------------------------------- pairing --------
// One thread's paged evaluations waiting for their SetUnlocks (the program calls Eval, then SetUnlocks with Eval's
// page-0 payload and version, on one thread). A scoped stack, so a nested evaluation pairs with its own setter.
struct Pending {
    uint64_t epochs[3] = {};  // every world kind's lifecycle epoch when Eval began (Sidecars::epochs): an evaluation
                              // its own world stopped during is never attached (another world's stop does not matter)
    uintptr_t ctx = 0;        // the knowledge context the game passed to its evaluator (what a recomputation uses)
    uintptr_t span_ptr = 0;
    uint64_t count = 0;
    uint32_t pages = 0;
    uint64_t masks[kPages] = {};
    uint16_t version = 0;
};

class PendingStack {
public:
    static constexpr uint32_t kDepth = 4;
    void push(const Pending& p) {
        if (n_ == kDepth) {  // the oldest goes: its setter never came
            for (uint32_t i = 1; i < kDepth; ++i) s_[i - 1] = s_[i];
            --n_;
        }
        s_[n_++] = p;
    }
    // The newest entry the setter's arguments belong to (same span, page 0's mask, the pages' version). It and every
    // newer entry leave the stack. False: no entry matches (a setter of an evaluation that was not paged).
    bool take(uintptr_t span_ptr, uint64_t count, uint64_t mask0, uint16_t version, Pending* out) {
        for (uint32_t i = n_; i-- > 0;) {
            const Pending& p = s_[i];
            if (p.span_ptr == span_ptr && p.count == count && p.masks[0] == mask0 && p.version == version) {
                *out = p;
                n_ = i;
                return true;
            }
        }
        return false;
    }
    uint32_t size() const { return n_; }
    void clear() { n_ = 0; }

private:
    std::array<Pending, kDepth> s_{};
    uint32_t n_ = 0;
};

// ---------------------------------------------------------------------------------------------- sidecars -------
// What the registry holds now, read by the caller (hook or Post) from the game's memory.
struct LiveCache {
    uint8_t status = 0xff;  // +0x98: 0 valid
    uint16_t version = 0;   // +0x90
    uint64_t payload = 0;   // +0xa0
    uintptr_t span_ptr = 0; // the collection's entries
    uint64_t count = 0;
    bool operator==(const LiveCache&) const = default;
};

// One world's second page. A page belongs to the world that owned the registry when it was attached, from that moment:
// (kind, session, registry) is its key, and only that world's stop removes it.
struct Sidecar {
    uintptr_t registry = 0;
    uintptr_t span_ptr = 0;
    uint64_t count = 0;
    uint16_t version = 0;
    uint64_t masks[kPages] = {};
    uint32_t kind = kKindUnknown;  // the world kind it was attached for
    uint32_t generation = 0;       // that world's core session (KilnSessionInfo.sessions[kind])
    uint64_t serial = 0;           // rises with every attach
};

// A lock no hook waits on for ever: a few hundred tries over at most about a millisecond, then the caller fails
// closed (a query answers the game's "cache invalid") or skips (a setter leaves the old page, which no longer
// matches the registry and so is never used).
class BoundedLock {
public:
    bool try_for(std::chrono::microseconds limit = std::chrono::microseconds(1000)) {
        const auto end = std::chrono::steady_clock::now() + limit;
        for (unsigned n = 0; n < 512; ++n) {
            if (m_.try_lock()) return true;
            if (std::chrono::steady_clock::now() >= end) break;
            std::this_thread::yield();
        }
        busy_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    void unlock() { m_.unlock(); }
    uint64_t busy() const { return busy_.load(std::memory_order_relaxed); }

private:
    std::mutex m_;
    std::atomic<uint64_t> busy_{0};
};

// The second pages and which live world owns which registry.
//
// Ownership: each world binds its own NpcRegistry on its own thread (its STARTED event and its Pre: the world is alive
// and nothing of its step runs), as (kind, session). Nothing here ever reads a registry: a hook compares the pointer the
// game passed it with the bound ones, so a registry belongs to the world whose update runs the producer. A world's stop
// unbinds it on the destroying thread, removes the pages of that session only, and advances that kind's epoch, so an
// evaluation its world stopped during is not attached afterwards (Pending::epochs).
//
// Reads that borrow the game's registry (the IsUnlocked hook) go: owner (kind, session) first, the registry's fields,
// the page, then the owner and the fields again (Moved when anything changed in between: the answer is discarded).
class Sidecars {
public:
    static constexpr uint32_t kSlots = 8;  // a hosting client has two registries; a few spare for replaced ones
    enum class Bit { Unlocked, Locked, NoPage, Stale, Busy, Unowned, Moved };
    enum class Attach { Attached, Busy, Unowned, Old };
    enum class Owner { Found, None, Busy };

    uint64_t epoch(uint32_t kind) const { return valid_kind(kind) ? epoch_[kind].load(std::memory_order_acquire) : 0; }
    void epochs(uint64_t out[3]) const {
        for (uint32_t k = 0; k < 3; ++k) out[k] = epoch_[k].load(std::memory_order_acquire);
    }

    // World `kind` of session `session` owns `registry` (0: its registry is not known yet). On that world's thread.
    // Another session of the kind than the bound one: the old session's pages go (a stop that was not seen).
    // A session that already stopped is never bound again (false).
    bool bind(uint32_t kind, uint32_t session, uintptr_t registry) {
        if (!valid_kind(kind) || !session) return false;
        lock_blocking();
        bool ok = session > stopped_[kind];
        Bound& b = bound_[kind];
        if (ok && (b.session != session || b.registry != registry)) {
            if (b.session != session) {
                if (b.session) {  // a stop that was not seen (no world.lifecycle): as unbind would have done
                    epoch_[kind].fetch_add(1, std::memory_order_acq_rel);
                    stopped_[kind] = std::max(stopped_[kind], b.session);
                }
                drop_locked(kind, b.session, true);
                b = Bound{};
            } else {
                drop_locked(kind, session, false);  // the world has another registry now: the old one's pages go
                b.ctx = 0;                          // and the context is learnt again from the game's next evaluation
            }
            b.session = session;
            b.registry = registry;
        }
        lock_.unlock();
        return ok;
    }
    // World `kind` of session `session` stops (or stopped): its pages go, its binding goes, its epoch moves. Blocking:
    // on the destroying thread or a world's thread, never from a hook.
    void unbind(uint32_t kind, uint32_t session) {
        if (!valid_kind(kind)) return;
        lock_blocking();
        epoch_[kind].fetch_add(1, std::memory_order_acq_rel);
        drop_locked(kind, session, false);
        if (bound_[kind].session == session || session == 0) bound_[kind] = Bound{};
        stopped_[kind] = std::max(stopped_[kind], session);
        lock_.unlock();
    }
    // The live world whose registry this is. None: no world bound it (an unknown or stale address).
    Owner owner(uintptr_t registry, uint32_t* kind, uint32_t* session) const {
        if (!registry) return Owner::None;
        if (!lock_.try_for()) return Owner::Busy;
        Owner o = Owner::None;
        for (uint32_t k = 0; k < 3 && o == Owner::None; ++k)
            if (bound_[k].session && bound_[k].registry == registry) *kind = k, *session = bound_[k].session, o = Owner::Found;
        lock_.unlock();
        return o;
    }
    // The knowledge context the game passed when it last evaluated world (kind, session)'s registry (0: none seen yet).
    uintptr_t context(uint32_t kind, uint32_t session) const {
        if (!valid_kind(kind) || !lock_.try_for()) return 0;
        const uintptr_t c = bound_[kind].session == session ? bound_[kind].ctx : 0;
        lock_.unlock();
        return c;
    }

    // After the game's SetUnlocks wrote (mask0, version) for `registry`: keep the pages with it, for the world that owns
    // it now, and only when that world did not stop since the evaluation began.
    Attach attach(uintptr_t registry, const Pending& p) {
        if (!lock_.try_for()) return Attach::Busy;
        uint32_t kind = kKindUnknown;
        for (uint32_t k = 0; k < 3; ++k)
            if (bound_[k].session && bound_[k].registry == registry) kind = k;
        if (kind == kKindUnknown) {
            lock_.unlock();
            return Attach::Unowned;
        }
        if (p.epochs[kind] != epoch_[kind].load(std::memory_order_acquire)) {
            lock_.unlock();
            return Attach::Old;
        }
        if (p.ctx) bound_[kind].ctx = p.ctx;
        Sidecar* s = find_or_claim(kind, bound_[kind].session, registry);
        s->registry = registry;
        s->kind = kind;
        s->generation = bound_[kind].session;
        s->span_ptr = p.span_ptr;
        s->count = p.count;
        s->version = p.version;
        for (uint32_t i = 0; i < kPages; ++i) s->masks[i] = i < p.pages ? p.masks[i] : 0;
        s->serial = ++serial_;
        lock_.unlock();
        return Attach::Attached;
    }
    // A SetUnlocks without a paged evaluation: the registry's extra page is gone (whoever owned it).
    bool invalidate(uintptr_t registry) {
        if (!lock_.try_for()) return false;
        for (Sidecar& s : s_)
            if (s.registry == registry) s = Sidecar{};
        lock_.unlock();
        return true;
    }
    // The state of entry `index` (64 or more) of `registry`, for a reader that found it owned by (kind, session) and took
    // `live` from it. Moved: the owner is not (kind, session) any more.
    Bit bit(uintptr_t registry, const LiveCache& live, uint64_t index, uint32_t kind, uint32_t session) const {
        if (index < kPage || index >= kExpanded) return Bit::NoPage;
        if (!valid_kind(kind)) return Bit::Unowned;
        if (!lock_.try_for()) return Bit::Busy;
        Bit b = Bit::NoPage;
        if (bound_[kind].session != session || bound_[kind].registry != registry) {
            b = Bit::Moved;
        } else if (const Sidecar* s = find(kind, session, registry)) {
            if (live.status != 0 || s->version != live.version || s->masks[0] != live.payload || s->span_ptr != live.span_ptr ||
                s->count != live.count || index >= s->count)
                b = Bit::Stale;
            else
                b = ((s->masks[index / kPage] >> (index % kPage)) & 1) ? Bit::Unlocked : Bit::Locked;
        }
        lock_.unlock();
        return b;
    }
    // A copy of world (kind, session)'s page for `registry` when it matches `live` (a snapshot), else false.
    bool read(uint32_t kind, uint32_t session, uintptr_t registry, const LiveCache& live, Sidecar* out) const {
        if (!valid_kind(kind) || !lock_.try_for()) return false;
        bool ok = false;
        if (bound_[kind].session == session && bound_[kind].registry == registry)
            if (const Sidecar* s = find(kind, session, registry))
                if (live.status == 0 && s->version == live.version && s->masks[0] == live.payload && s->span_ptr == live.span_ptr &&
                    s->count == live.count) {
                    *out = *s;
                    ok = true;
                }
        lock_.unlock();
        return ok;
    }
    // True when world (kind, session)'s registry, holding `live`, has more than 64 entries, a computed cache and no page
    // that matches it: a lost page, to be computed again at that world's next safe point.
    bool wants_page(uint32_t kind, uint32_t session, uintptr_t registry, const LiveCache& live) const {
        if (live.status != 0 || live.count <= kPage || live.count > kExpanded) return false;
        Sidecar s;
        return !read(kind, session, registry, live, &s);
    }
    // Lab: forget world kind's page as if it had been lost (kiln/control/roster.drop-page).
    void drop_page(uint32_t kind) {
        lock_blocking();
        for (Sidecar& s : s_)
            if (s.kind == kind) s = Sidecar{};
        lock_.unlock();
    }
    void drop_all() {
        lock_blocking();
        for (uint32_t k = 0; k < 3; ++k) {
            epoch_[k].fetch_add(1, std::memory_order_acq_rel);
            stopped_[k] = std::max(stopped_[k], bound_[k].session);
            bound_[k] = Bound{};
        }
        for (Sidecar& s : s_) s = Sidecar{};
        lock_.unlock();
    }
    uint64_t busy() const { return lock_.busy(); }
    uint32_t live_count() const {
        uint32_t n = 0;
        if (!lock_.try_for()) return 0;
        for (const Sidecar& s : s_) n += s.registry != 0;
        lock_.unlock();
        return n;
    }

private:
    struct Bound {
        uint32_t session = 0;
        uintptr_t registry = 0;
        uintptr_t ctx = 0;
    };
    void lock_blocking() const {
        while (!lock_.try_for()) {
        }
    }
    // the pages of (kind, session); `older_too`: and of every other session of the kind
    void drop_locked(uint32_t kind, uint32_t session, bool older_too) {
        for (Sidecar& s : s_)
            if (s.registry && s.kind == kind && (s.generation == session || older_too || session == 0)) s = Sidecar{};
    }
    const Sidecar* find(uint32_t kind, uint32_t session, uintptr_t registry) const {
        for (const Sidecar& s : s_)
            if (s.registry == registry && s.kind == kind && s.generation == session) return &s;
        return nullptr;
    }
    Sidecar* find_or_claim(uint32_t kind, uint32_t session, uintptr_t registry) {
        for (Sidecar& s : s_)
            if (s.registry == registry && s.kind == kind && s.generation == session) return &s;
        for (Sidecar& s : s_)
            if (s.registry == 0) return &s;
        // full: replace the oldest (a registry no longer written)
        Sidecar* oldest = &s_[0];
        for (Sidecar& s : s_)
            if (s.serial < oldest->serial) oldest = &s;
        *oldest = Sidecar{};
        return oldest;
    }

    std::atomic<uint64_t> epoch_[3] = {1, 1, 1};
    Bound bound_[3];
    uint32_t stopped_[3] = {};  // per kind, the newest session seen to stop
    mutable BoundedLock lock_;
    std::array<Sidecar, kSlots> s_{};
    uint64_t serial_ = 0;
};

// The IsUnlocked hook's answer for one id (any thread: the game's reader runs on a worker on a dedicated server).
// `read_live(LiveCache*)` reads the registry's fields (false: unreadable); `index_of(const LiveCache&, uint64_t*)` the
// id's index in that collection (false: not one of its entries). A first read only sorts the query (a page-0 entry, an
// unknown id, a cache the game itself calls invalid: the game answers; no lock is taken). For an entry of page 2 the
// order makes a read unable to span a world's end: the owner (kind, session), the fields again (they must be the
// first read's), the page (which checks the owner again under the lock), then the fields and the owner once more;
// anything that moved in between and the answer is not given (Moved).
struct QueryAnswer {
    enum class Kind { Game, Page, Kept };
    Kind kind = Kind::Game;  // Game: the game's own call answers; Page: from the page; Kept: the game's "cache invalid"
    bool unlocked = false;
    Sidecars::Bit why = Sidecars::Bit::NoPage;
};
template <class ReadLive, class IndexOf>
QueryAnswer page_query(const Sidecars& sc, uintptr_t registry, ReadLive&& read_live, IndexOf&& index_of) {
    QueryAnswer a;
    LiveCache live;
    uint64_t index = 0;
    if (!read_live(&live) || !index_of(live, &index) || index < kPage || live.status != 0) return a;
    a.kind = QueryAnswer::Kind::Kept;
    uint32_t kind = kKindUnknown, session = 0;
    const Sidecars::Owner o = sc.owner(registry, &kind, &session);
    if (o != Sidecars::Owner::Found) {
        a.why = o == Sidecars::Owner::Busy ? Sidecars::Bit::Busy : Sidecars::Bit::Unowned;
        return a;
    }
    LiveCache now;
    if (!read_live(&now) || !(now == live)) {
        a.why = Sidecars::Bit::Moved;
        return a;
    }
    a.why = sc.bit(registry, live, index, kind, session);
    if (a.why != Sidecars::Bit::Unlocked && a.why != Sidecars::Bit::Locked) return a;
    LiveCache again;
    uint32_t k2 = kKindUnknown, s2 = 0;
    if (!read_live(&again) || !(again == live) || sc.owner(registry, &k2, &s2) != Sidecars::Owner::Found || k2 != kind ||
        s2 != session) {
        a.why = Sidecars::Bit::Moved;
        return a;
    }
    a.kind = QueryAnswer::Kind::Page;
    a.unlocked = a.why == Sidecars::Bit::Unlocked;
    return a;
}

// A lost page computed again on its world's thread at a safe point (its Pre: nothing of its step runs), with the game's
// evaluator `eval` on the world's knowledge context, consistent with what the game's cache holds now: true and *out
// (its epochs left to the caller, taken before this call) when every page answered at the cache's version and page 0
// equals the cache's value; false otherwise (the knowledge moved: the producer computes in the step anyway).
template <class Eval>
bool recompute(const LiveCache& live, const Span& span, uint64_t stride, uint8_t error_status, void* ctx, Eval&& eval,
               Pending* out) {
    if (live.status != 0 || live.count <= kPage || live.count > kExpanded || span.count != live.count || span.ptr != live.span_ptr)
        return false;
    Result16 r{};
    uint16_t seen = static_cast<uint16_t>(live.version ^ 0x8000u);  // not the cache's version: the evaluator computes
    const PageOutcome o = eval_pages(&r, ctx, &seen, span, stride, error_status, eval);
    if (!o.complete || o.version != live.version || o.masks[0] != live.payload) return false;
    out->ctx = reinterpret_cast<uintptr_t>(ctx);
    out->span_ptr = span.ptr;
    out->count = span.count;
    out->pages = o.pages;
    for (uint32_t i = 0; i < kPages; ++i) out->masks[i] = o.masks[i];
    out->version = o.version;
    return true;
}

// ---------------------------------------------------------------------------------------------- snapshots ------
struct Entry {
    uint32_t npc_id = 0;
    uint8_t unlocked = 0;
};

struct Snapshot {
    uint32_t kind = 0, generation = 0;
    bool ready = false;                    // false: the roster cannot be answered (not computed, damaged, too long)
    uint16_t version = 0;                  // the engine's knowledge version the cache was computed at
    uint64_t revision = 0;                 // set by Snapshots::publish
    std::vector<Entry> by_id;              // sorted by NpcId
    uint32_t count = 0;                    // entries in the collection
    uint32_t game_used = 0, kiln_used = 0, reserved = 0;
};

// Which entries are whose: the collection Kiln found at boot (the game's, or what another loader served before
// Kiln's build) and the ids Kiln's build added. Set once at boot, read on world threads.
struct Owners {
    std::vector<uint32_t> boot_ids;  // sorted
    std::vector<uint32_t> kiln_ids;  // sorted
    bool known = false;              // false: no build looked at the collection: every entry counts as the game's
};

// The snapshot of a registry whose cache is `live` with entry ids `ids` (collection order).
inline Snapshot make_snapshot(uint32_t kind, uint32_t generation, const LiveCache& live, const std::vector<uint32_t>& ids,
                              const Sidecar* page, const Owners& owners, uint32_t max) {
    Snapshot s;
    s.kind = kind;
    s.generation = generation;
    s.version = live.version;
    s.count = static_cast<uint32_t>(std::min<uint64_t>(live.count, UINT32_MAX));
    s.ready = live.status == 0 && live.count == ids.size() && live.count <= max && live.count <= kReadGuard &&
              (live.count <= kPage || (page != nullptr && page->count == live.count));
    if (!s.ready) return s;
    s.by_id.reserve(ids.size());
    for (size_t i = 0; i < ids.size(); ++i) {
        const uint64_t mask = i < kPage ? live.payload : page->masks[i / kPage];
        s.by_id.push_back(Entry{ids[i], static_cast<uint8_t>((mask >> (i % kPage)) & 1)});
        const uint32_t id = ids[i];
        if (owners.known && std::binary_search(owners.kiln_ids.begin(), owners.kiln_ids.end(), id)) ++s.kiln_used;
        else if (!owners.known || std::binary_search(owners.boot_ids.begin(), owners.boot_ids.end(), id)) ++s.game_used;
        else ++s.reserved;
    }
    std::sort(s.by_id.begin(), s.by_id.end(), [](const Entry& a, const Entry& b) { return a.npc_id < b.npc_id; });
    return s;
}

// The snapshot of a world whose registry could not be read this step: no answer (never the last ready one).
inline Snapshot unreadable_snapshot(uint32_t kind, uint32_t generation) {
    Snapshot s;
    s.kind = kind;
    s.generation = generation;
    return s;
}

// What the core session says about one world kind now (any thread).
struct WorldNow {
    bool live = false;
    uint32_t generation = 0;  // sessions[kind]
};

struct Change {
    uint32_t kind, generation, npc_id;
    uint8_t before, after;
    uint16_t version;
    uint64_t revision;
};

class Snapshots {
public:
    // Publishes `s` for its kind. The revision rises when the answer changes (readiness, values, version, counts,
    // generation); an unchanged snapshot keeps the old one. `changes` gets one entry per NpcId whose state changed
    // since the previous ready snapshot of the same world (none for a world's first one).
    void publish(Snapshot s, std::vector<Change>* changes = nullptr) {
        if (!valid_kind(s.kind)) return;
        std::lock_guard lock(mu_);
        auto& cur = cur_[s.kind];
        if (cur && same(*cur, s)) return;
        s.revision = rev_.next();
        if (changes && cur && cur->ready && s.ready && cur->generation == s.generation) {
            size_t i = 0;
            for (const Entry& e : s.by_id) {
                while (i < cur->by_id.size() && cur->by_id[i].npc_id < e.npc_id) ++i;
                if (i < cur->by_id.size() && cur->by_id[i].npc_id == e.npc_id && cur->by_id[i].unlocked != e.unlocked)
                    changes->push_back(Change{s.kind, s.generation, e.npc_id, cur->by_id[i].unlocked, e.unlocked, s.version, s.revision});
            }
        }
        cur = std::make_shared<const Snapshot>(std::move(s));
    }
    // The world of `kind` stops: its generation's answers end here (WORLD_ENDED, never the next world's data).
    void end(uint32_t kind, uint32_t generation) {
        if (!valid_kind(kind)) return;
        std::lock_guard lock(mu_);
        if (cur_[kind] && cur_[kind]->generation == generation) cur_[kind].reset(), rev_.next();
        ended_[kind] = std::max(ended_[kind], generation);
    }
    std::shared_ptr<const Snapshot> get(uint32_t kind) const {
        if (!valid_kind(kind)) return {};
        std::lock_guard lock(mu_);
        return cur_[kind];
    }
    uint32_t ended(uint32_t kind) const {
        std::lock_guard lock(mu_);
        return valid_kind(kind) ? ended_[kind] : 0;
    }
    uint64_t revision() const { return rev_.current(); }
    void clear_for_tests() {
        std::lock_guard lock(mu_);
        for (auto& c : cur_) c.reset();
        for (auto& e : ended_) e = 0;
    }

private:
    static bool same(const Snapshot& a, const Snapshot& b) {
        if (a.kind != b.kind || a.generation != b.generation || a.ready != b.ready || a.version != b.version || a.count != b.count ||
            a.game_used != b.game_used || a.kiln_used != b.kiln_used || a.reserved != b.reserved || a.by_id.size() != b.by_id.size())
            return false;
        for (size_t i = 0; i < a.by_id.size(); ++i)
            if (a.by_id[i].npc_id != b.by_id[i].npc_id || a.by_id[i].unlocked != b.by_id[i].unlocked) return false;
        return true;
    }
    mutable std::mutex mu_;
    std::shared_ptr<const Snapshot> cur_[3];
    uint32_t ended_[3] = {};
    kiln::capacity::Revision rev_;
};

// ---------------------------------------------------------------------------------------------- the reads ------
// `available`: the reads are on for this kind on this side. `now`: the core session's view of the kind.

// The snapshot of the world `generation` names, or why there is none: VALID, NOT_READY, WORLD_ENDED.
inline uint32_t resolve(const Snapshots& snaps, uint32_t kind, uint32_t generation, WorldNow now,
                        std::shared_ptr<const Snapshot>* out) {
    if (!now.live || now.generation != generation || snaps.ended(kind) >= generation) return KILN_CAPACITY_WORLD_ENDED;
    auto s = snaps.get(kind);
    if (!s || s->generation != generation || !s->ready) return KILN_CAPACITY_NOT_READY;
    *out = std::move(s);
    return KILN_CAPACITY_VALID;
}

inline KilnResult world(const Snapshots& snaps, uint32_t kind, WorldNow now, bool available, uint32_t* out_generation,
                        uint32_t* out_readiness) {
    if (!valid_kind(kind) || !out_generation || !out_readiness) return KILN_ERR_INVALID_ARGUMENT;
    if (!available) return KILN_ERR_UNAVAILABLE;
    *out_generation = 0;
    if (!now.live || now.generation == 0 || snaps.ended(kind) >= now.generation) {
        *out_readiness = KILN_CAPACITY_NO_WORLD;
        return KILN_OK;
    }
    std::shared_ptr<const Snapshot> s;
    *out_readiness = resolve(snaps, kind, now.generation, now, &s);
    if (*out_readiness == KILN_CAPACITY_VALID) *out_generation = now.generation;
    else *out_readiness = KILN_CAPACITY_NOT_READY;
    return KILN_OK;
}

inline KilnResult budget(const Snapshots& snaps, uint32_t kind, uint32_t generation, WorldNow now, bool available,
                         uint32_t max, bool expanded, KilnCapacityInfo* out) {
    if (!valid_kind(kind) || generation == 0 || !kiln::capacity::valid_out(out)) return KILN_ERR_INVALID_ARGUMENT;
    if (!available) return KILN_ERR_UNAVAILABLE;
    const auto scope = kiln::capacity::Scope::world(kind, generation);
    std::shared_ptr<const Snapshot> s;
    const uint32_t r = resolve(snaps, kind, generation, now, &s);
    if (r != KILN_CAPACITY_VALID) {
        kiln::capacity::not_ready(out, r, scope, snaps.revision(), expanded);
        return KILN_OK;
    }
    kiln::capacity::publish(out, max, s->game_used, s->kiln_used, s->reserved, scope, s->revision, expanded);
    return KILN_OK;
}

inline KilnResult unlock_at(const Snapshots& snaps, uint32_t kind, uint32_t generation, uint32_t npc_id, WorldNow now,
                            bool available, KilnNpcUnlockReply* out) {
    if (!valid_kind(kind) || generation == 0 || npc_id == 0 || !out || out->size < sizeof(KilnNpcUnlockReply))
        return KILN_ERR_INVALID_ARGUMENT;
    if (!available) return KILN_ERR_UNAVAILABLE;
    const uint32_t size = out->size;
    std::memset(out, 0, sizeof *out);
    out->size = size;
    out->world_kind = kind;
    out->generation = generation;
    out->npc_id = npc_id;
    out->revision = snaps.revision();
    std::shared_ptr<const Snapshot> s;
    out->readiness = resolve(snaps, kind, generation, now, &s);
    if (out->readiness != KILN_CAPACITY_VALID) return KILN_OK;
    out->revision = s->revision;
    const auto it = std::lower_bound(s->by_id.begin(), s->by_id.end(), npc_id,
                                     [](const Entry& e, uint32_t id) { return e.npc_id < id; });
    if (it == s->by_id.end() || it->npc_id != npc_id) {
        out->readiness = KILN_NPC_UNKNOWN_ID;
        return KILN_OK;
    }
    out->unlocked = it->unlocked;
    out->knowledge_version = s->version;
    return KILN_OK;
}

}  // namespace kiln::roster
