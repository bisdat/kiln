// SPDX-License-Identifier: MIT
// kiln.abilities: the decisions of the native thunks (abilities.cpp) with nothing of Windows in them, so the host tests
// (tests/natives_test.cpp) run them with made-up calls. Since 0.2.0-dev.29: synchronous natives.
#pragma once
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <string_view>
#include <vector>

#include "engine.h"
#include "services/kiln_abilities.h"

namespace kab {

// The interpreter's error codes a Kiln native answers with (docs/re/impact.md §2; keen::ErrorId).
constexpr uint8_t kErrInternal = 0x11;      // a handler fault: the instance stops with Error, the game goes on
constexpr uint8_t kErrNoConnection = 0x18;  // a required argument is not connected
constexpr uint8_t kErrTypeMismatch = 0x2C;  // an argument's variable has another type or size
constexpr uint8_t kErrUnknownNative = 0x1F; // a Kiln native that was taken off (the interpreter's own "unknown native")
constexpr uint8_t kUndecided = 0xFF;        // KilnNativeCall.out_error before anyone answered

// "<modid>.<Name>": a non-empty mod id, a dot, Name in [A-Za-z0-9_], 90 bytes at most.
inline bool valid_native_name(std::string_view n) {
    const size_t dot = n.rfind('.');
    if (dot == std::string_view::npos || dot == 0 || dot + 1 >= n.size()) return false;
    // Mod ids may contain dots; only the suffix is the native's Name.
    for (size_t i = 0; i < dot; ++i) {
        const char c = n[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')) return false;
    }
    for (size_t i = dot + 1; i < n.size(); ++i) {
        const char c = n[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
    }
    return n.size() <= 90;
}

// The game's native registry is a hash map of capacity 256 whose RegisterNative takes no new key once it holds half of
// that (measured in the lab on server 26430253: the 129th key was refused; 107 keys are the game's own). Kiln refuses
// first, with the reason, and never fills the map (its lookups stop at an empty entry). count/cap as the map says; cap 0
// = unreadable.
inline bool registry_has_room(uint32_t count, uint64_t cap) { return cap != 0 && static_cast<uint64_t>(count) + 1 <= cap / 2; }

// What a sync native's call answers once its fn returned: what fn set, or error 0 value 0 when it set nothing.
inline void sync_result(const KilnNativeCall& c, uint8_t* error, uint8_t* value) {
    if (c.out_error == kUndecided) {
        *error = 0;
        *value = 0;
    } else {
        *error = c.out_error;
        *value = c.out_value;
    }
}

// A fault caught by a thunk's guard. The game's own function runs at most once per call: a wrapped native whose
// original did not run yet runs it now (fail open); one whose original ran keeps the result it gave (the latch);
// a sync native answers Internal; anything else (or a fault inside the game's own native) answers "unknown native".
enum class FaultAnswer { RunOriginal, KeepResult, Internal, Unknown };
enum class ThunkKind { Wrapped, Sync, Other };
inline FaultAnswer fault_answer(ThunkKind kind, bool in_game, bool original_ran, bool have_original) {
    if (kind == ThunkKind::Wrapped && !in_game) {
        if (original_ran) return FaultAnswer::KeepResult;
        if (have_original) return FaultAnswer::RunOriginal;
    }
    if (kind == ThunkKind::Sync && !in_game) return FaultAnswer::Internal;
    return FaultAnswer::Unknown;
}

// The 0.2 ms budget of a sync native (a log line, not a cut-off). ticks/frequency as QueryPerformanceCounter gives.
inline bool over_budget(int64_t ticks, int64_t frequency) {
    return frequency > 0 && ticks * 5000 > frequency;  // ticks / frequency > 0.0002 s
}

// ---------------------------------------------------------------------------------------------- attachments ----
// since 0.2.0-dev.29: handlers attached to one vanilla native's calls, filtered here (in the thunk) on the calling
// program and call site, so a call that matches none never enters managed code.
struct Attachment {
    uint64_t id = 0;
    uint32_t hash = 0, program = 0, site = 0;  // program/site 0 = any
    int32_t stage = KILN_NATIVE_BEFORE, priority = 0;
    uint64_t order = 0;                       // registration order (ties)
    KilnNativeWrapFn fn = nullptr;
    void* user = nullptr;
    KilnOwner owner = 0;
    bool active = true;                       // false: its program or site was not found when the world's programs loaded
    bool checked = false;                     // its program and site were checked against the world's programs
    struct OwnerGate* gate = nullptr;         // its owner's admission (every call enters it first)
};

struct AttachSet {
    std::vector<Attachment> before, instead, after;
    bool empty() const { return before.empty() && instead.empty() && after.empty(); }
};

inline bool filter_matches(const Attachment& a, uint32_t program, uint32_t site) {
    return a.active && (a.program == 0 || a.program == program) && (a.site == 0 || a.site == site);
}

// The set a thunk reads: Before by priority high to low, Instead high to low, After low to high; ties in registration
// order.
inline AttachSet build_set(const std::vector<Attachment>& all, uint32_t hash) {
    AttachSet s;
    for (const auto& a : all) {
        if (a.hash != hash) continue;
        (a.stage == KILN_NATIVE_BEFORE ? s.before : a.stage == KILN_NATIVE_INSTEAD ? s.instead : s.after).push_back(a);
    }
    auto high = [](const Attachment& x, const Attachment& y) { return x.priority != y.priority ? x.priority > y.priority : x.order < y.order; };
    auto low = [](const Attachment& x, const Attachment& y) { return x.priority != y.priority ? x.priority < y.priority : x.order < y.order; };
    std::stable_sort(s.before.begin(), s.before.end(), high);
    std::stable_sort(s.instead.begin(), s.instead.end(), high);
    std::stable_sort(s.after.begin(), s.after.end(), low);
    return s;
}

// One Instead per (native, program, site).
inline bool instead_taken(const std::vector<Attachment>& all, uint32_t hash, uint32_t program, uint32_t site) {
    for (const auto& a : all)
        if (a.stage == KILN_NATIVE_INSTEAD && a.hash == hash && a.program == program && a.site == site) return true;
    return false;
}

// One call through the stages: every matching Before; then, if nothing decided the call, the first matching Instead;
// then, if still undecided, the game's native (once); then every matching After. enter(a, stage) runs one attachment;
// original() runs the game's native; decided() says whether a handler answered or the native ran.
template <class Enter, class Original, class Decided>
void run_stages(const AttachSet& set, uint32_t program, uint32_t site, Enter&& enter, Original&& original,
                Decided&& decided, uint64_t* entered, uint64_t* skipped) {
    for (const auto& a : set.before) {
        if (!filter_matches(a, program, site)) { ++*skipped; continue; }
        ++*entered;
        enter(a, KILN_NATIVE_BEFORE);
    }
    if (!decided())
        for (const auto& a : set.instead) {
            if (!filter_matches(a, program, site)) { ++*skipped; continue; }
            ++*entered;
            enter(a, KILN_NATIVE_INSTEAD);
            break;
        }
    if (!decided()) original();
    for (const auto& a : set.after) {
        if (!filter_matches(a, program, site)) { ++*skipped; continue; }
        ++*entered;
        enter(a, KILN_NATIVE_AFTER);
    }
}

// ------------------------------------------------------------------------------------------- typed arguments ----
// since 0.2.0-dev.29: the per-call check of a typed declaration (native_declare_typed), before the native's handler
// sees the call. args[i]: a variable handle 0xFFFF0000|index, 0 (not connected) or a raw value. layout: the program's
// ImpactVariable entries (24 B each), data_size: the instance data's bytes. Answers 0, kErrTypeMismatch (another
// type or size, a raw value for a variable parameter, a handle out of range) or kErrNoConnection (a required argument
// not connected).
inline uint8_t check_typed_args(const KilnNativeArg* decl, uint32_t n, const uint32_t* args, uint64_t argc,
                                const uint8_t* layout, uint64_t layout_count, uint64_t data_size) {
    if (argc < n || (n && !args)) return kErrTypeMismatch;
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t h = args[i];
        const KilnNativeArg& d = decl[i];
        if ((h & 0xFFFF0000u) != 0xFFFF0000u) {
            if (d.flags & KILN_NATIVE_ARG_RAW) continue;  // [Raw] uint: a literal (0 included)
            if (h == 0) {
                if (d.flags & KILN_NATIVE_ARG_OPTIONAL) continue;
                return kErrNoConnection;
            }
            return kErrTypeMismatch;  // an untagged literal for a variable parameter
        }
        const uint32_t idx = h & 0xFFFFu;
        if (idx >= layout_count || !layout) return kErrTypeMismatch;
        ImpactVariable v;
        std::memcpy(&v, layout + static_cast<size_t>(idx) * sizeof(ImpactVariable), sizeof v);
        if ((d.type && v.type != d.type) || (d.size && v.size != d.size)) return kErrTypeMismatch;
        if (static_cast<uint64_t>(v.offset) + v.size > data_size) return kErrTypeMismatch;
    }
    return 0;
}

// Impact bytecode: words of operands after an opcode (the verifier's table, verify.cpp).
inline int operand_words(uint32_t op) {
    switch (op) {
        case 8: case 9: case 10: case 11: case 21: case 22: case 23: case 24: return 1;  // BR BRT BRF ICONST LOAD GLOAD STORE GSTORE
        case 16: case 18: case 19: return 2;                                              // COPY CALL ECALL
        default: return 0;
    }
}

// The program's code calls `hash` (at `site`; site 0 = at any site): an ECALL site, hash on an instruction start.
inline bool code_calls(const uint32_t* code, size_t n, uint32_t hash, uint32_t site) {
    for (size_t pc = 0; pc < n;) {
        const uint32_t op = code[pc];
        if (op < 1 || op > 30) return false;  // not a program the verifier would take: no match
        const int w = operand_words(op);
        if (op == 19 && pc + 2 < n && code[pc + 2] == hash && (site == 0 || code[pc + 1] == site)) return true;
        pc += 1 + static_cast<size_t>(w);
    }
    return false;
}


// ------------------------------------------------------------------------------------------ owner admission ----
// since 0.2.0-dev.29 (review P1): every call into a mod's sync native or attachment enters its owner's gate first.
// Retiring an owner closes the gate (no new entry), then waits a bounded time for the calls inside to leave; a call
// still inside when the bound expires keeps the owner's bindings alive (a deferred retirement, finished in a later
// Pre when the gate is quiet). Nothing a running handler can still read is freed before its gate is quiet.
struct OwnerGate {
    KilnOwner owner = 0;
    std::atomic<uint32_t> inflight{0};
    std::atomic<bool> closed{false};
};

// A call enters (true) or the owner is retired (false: answer as a removed native). The increment comes before the
// check, so a closer that sees no call inside can trust that a late caller sees the gate closed (both seq_cst).
inline bool gate_enter(OwnerGate& g) {
    g.inflight.fetch_add(1, std::memory_order_seq_cst);
    if (!g.closed.load(std::memory_order_seq_cst)) return true;
    g.inflight.fetch_sub(1, std::memory_order_seq_cst);
    return false;
}
inline void gate_leave(OwnerGate& g) { g.inflight.fetch_sub(1, std::memory_order_seq_cst); }

// Closes the gate and waits until no call is inside or `budget` is spent; yield() between looks (never a lock).
// True: quiet. now() and yield() are the caller's clock and yield (tests pass fakes).
template <class Now, class Yield>
bool gate_close_and_wait(OwnerGate& g, int64_t budget, Now&& now, Yield&& yield) {
    g.closed.store(true, std::memory_order_seq_cst);
    if (g.inflight.load(std::memory_order_seq_cst) == 0) return true;
    const int64_t end = now() + budget;
    while (now() < end) {
        yield();
        if (g.inflight.load(std::memory_order_seq_cst) == 0) return true;
    }
    return g.inflight.load(std::memory_order_seq_cst) == 0;
}

// What a retirement keeps alive until its gate is quiet (bindings, attachment sets: freed by `release`).
template <class Payload>
struct Deferred {
    OwnerGate* gate = nullptr;
    Payload payload{};
    uint64_t since = 0;  // the step it was deferred in
};

// The retirements whose gate is quiet now: released (`release(payload)`, `done(gate, steps)`) and removed.
template <class Payload, class Release, class Done>
size_t finish_quiet(std::vector<Deferred<Payload>>& list, uint64_t step, Release&& release, Done&& done) {
    size_t n = 0;
    for (auto it = list.begin(); it != list.end();) {
        if (it->gate && it->gate->inflight.load(std::memory_order_seq_cst) != 0) {
            ++it;
            continue;
        }
        release(it->payload);
        done(it->gate, step - it->since);
        it = list.erase(it);
        ++n;
    }
    return n;
}

// ---------------------------------------------------------------------------------------------- watchdog -------
// A handler that has been inside longer than the watchdog's limit is named once per call (a hung server says why).
// Records are claimed by CAS at entry and released at exit; the watchdog thread only reads them.
struct InsideRecord {
    std::atomic<int64_t> since{0};       // 0 = free; else the entry time (the caller's clock)
    std::mutex mu;  // protects a copied name and record reuse; claim/scan never wait
    char what[96]{};
    std::atomic<KilnOwner> owner{0};
    std::atomic<bool> reported{false};
};
template <size_t N>
struct InsideTable {
    InsideRecord rec[N];
    // A free record for this call, or -1 when all are taken (the call is then not watched).
    int claim(int64_t now, const char* what, KilnOwner owner) {
        for (size_t i = 0; i < N; ++i) {
            std::unique_lock lock(rec[i].mu, std::try_to_lock);
            if (!lock.owns_lock()) continue;
            int64_t free = 0;
            if (rec[i].since.compare_exchange_strong(free, -1, std::memory_order_acq_rel)) {
                std::snprintf(rec[i].what, sizeof rec[i].what, "%s", what ? what : "?");
                rec[i].owner.store(owner, std::memory_order_relaxed);
                rec[i].reported.store(false, std::memory_order_relaxed);
                rec[i].since.store(now, std::memory_order_release);
                return static_cast<int>(i);
            }
        }
        return -1;
    }
    void release(int i) {
        if (i >= 0) rec[i].since.store(0, std::memory_order_release);
    }
    // Calls inside for longer than `limit`, each once: report(what, owner, elapsed).
    template <class Report>
    size_t scan(int64_t now, int64_t limit, Report&& report) {
        size_t n = 0;
        for (size_t i = 0; i < N; ++i) {
            std::unique_lock lock(rec[i].mu, std::try_to_lock);
            if (!lock.owns_lock()) continue;
            const int64_t s = rec[i].since.load(std::memory_order_acquire);
            if (s <= 0 || now - s < limit || rec[i].reported.exchange(true, std::memory_order_acq_rel)) continue;
            char what[96];
            std::memcpy(what, rec[i].what, sizeof what);
            const auto owner = rec[i].owner.load(std::memory_order_relaxed);
            lock.unlock();  // logging may block: do not keep the record locked
            report(what, owner, now - s);
            ++n;
        }
        return n;
    }
};

// The immutable callback of a sync native: published as one unit (a reader never sees one field of another
// registration), retired only when its owner's gate is quiet.
struct SyncBinding {
    KilnNativeWrapFn fn = nullptr;
    void* user = nullptr;
    OwnerGate* gate = nullptr;
    std::string name;  // immutable across same-slot reloads
};

// One gate per owner, made on its first registration and never freed (an owner id is never reused: a mod's reload
// is a new owner). Under the caller's lock.
class OwnerGates {
public:
    OwnerGate* of(KilnOwner owner) {
        auto& g = map_[owner];
        if (!g) {
            g = std::make_unique<OwnerGate>();
            g->owner = owner;
        }
        return g.get();
    }
    OwnerGate* find(KilnOwner owner) const {
        auto it = map_.find(owner);
        return it == map_.end() ? nullptr : it->second.get();
    }

private:
    std::unordered_map<KilnOwner, std::unique_ptr<OwnerGate>> map_;
};

// ------------------------------------------------------------------------------------------- the ECS bridge ----
// since 0.2.0-dev.29 (chapter 06c 9.1): the first Kiln native call of each world checks the iterator it holds against
// kiln.world's view of impact_simulation: the job's group counts are the view's (counts_ok), and the qword at
// impact::ImpactSystem's computed slot is the native registry the RegisterNative hook captured. A reason, or NONE.
inline uint32_t ecs_verdict(bool counts_ok, int32_t system_slot, uint64_t at_slot, uint64_t registry) {
    if (!counts_ok) return KILN_NATIVE_ECS_REASON_COUNTS;
    if (system_slot < 1 || !registry || at_slot != registry) return KILN_NATIVE_ECS_REASON_REGISTRY;
    return KILN_NATIVE_ECS_REASON_NONE;
}

// The bridge's serial condition (review round 3): impact_simulation runs its slices one after another on the world's
// thread, so no two calls can touch one entity at once. It holds while the world's pin byte (storage +
// abilities/impact.pin, copied from world config +0x23: CHUNKED programs get affinity 1, the world thread) is set and
// the call runs on the world's thread; it cannot be known on a build where sigdb does not resolve the pin. Checked at
// the self-check and at every access. A reason, or NONE.
inline uint32_t ecs_serial(bool pin_known, uint8_t pin, bool on_world_thread) {
    if (!pin_known) return KILN_NATIVE_ECS_REASON_PIN_UNKNOWN;
    if (pin == 0 || !on_world_thread) return KILN_NATIVE_ECS_REASON_NOT_SERIAL;
    return KILN_NATIVE_ECS_REASON_NONE;
}

}  // namespace kab
