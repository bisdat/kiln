// SPDX-License-Identifier: MIT
// kiln.hooks' read_original and code_state: the exe file's code, and a record of exactly what Kiln's own detours and
// patches wrote over it. Kiln's writers of game code (hooks.cpp: patch, unpatch, write, revoke_owner; hooks_chain.cpp:
// install, revoke, revoke_owner, sweep) hold lock() while they read the old bytes, write and record what they wrote.
// Lock order: the chain or patch mutex, then this one. code_state takes only this one, and holds it for two guarded
// copies of the live bytes and the comparison: no callback, no log, no wait. hooks.json's foreignBefore and
// foreignOver are never used here.
// The same registry keeps the code ranges Kiln's own services protect for their engine calls (kiln.internal.hooks,
// ../hooks_protect_internal.h): every writer above asks protected_overlap_locked() under lock(), before it reads the old
// bytes or writes, and protect() checks that the ranges are still ORIGINAL, with no detour or patch recorded over them,
// under the same lock. An install and a protect that race therefore come in one order: the install lands and protect
// sees it, or protect lands and the install is refused. The undo paths (unpatch, revoke, revoke_owner) are not checked:
// they only touch bytes that have a record, and a range with a record is never protected.
#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../hooks_protect_internal.h"
#include "services/kiln_hooks.h"

namespace kiln::hooks::code {

#ifdef KILN_HOOKS_TEST_LOCK_PROBE
// Host tests only (hooks_v2_test.cpp; never defined for kiln-core): called by lock() before it takes the mutex, so a
// test that holds the lock knows the thread it watches has reached it and waits, instead of sleeping and hoping it has.
void test_lock_probe();
#endif
struct Image {
    uintptr_t base;
    size_t size;
    uintptr_t text;
    size_t text_size;
    bool (*file)(uintptr_t, uint8_t*, size_t);
    bool (*live)(uintptr_t, uint8_t*, size_t);
};
inline bool valid_range(const Image& image, uintptr_t address, size_t size) {
    // Subtraction-based bounds checks also refuse wraparound in malformed image metadata. The size limit comes first:
    // both members check the range before they allocate or read anything sized by it.
    const auto max = std::numeric_limits<uintptr_t>::max();
    return size && size <= KILN_HOOKS_CODE_RANGE_MAX && image.base && image.size <= max - image.base &&
        image.text >= image.base && image.text - image.base <= image.size &&
        image.text_size <= image.size - (image.text - image.base) && address >= image.text &&
        address - image.text < image.text_size && size <= image.text_size - (address - image.text);
}
inline KilnResult read_original(const Image& image, uintptr_t address, void* out, size_t size) {
    if (!out || !valid_range(image, address, size)) return KILN_ERR_INVALID_ARGUMENT;
    // Stage the whole copy: a short/unavailable file reader must not publish a partial result.
    std::vector<uint8_t> bytes(size);
    if (!image.file || !image.file(address, bytes.data(), size)) return KILN_ERR_UNAVAILABLE;
    std::memcpy(out, bytes.data(), size);
    return KILN_OK;
}

// What protect() found, for the caller's log line.
struct ProtectOutcome {
    size_t added = 0, added_bytes = 0;  // ranges (and their bytes) newly protected
    uint32_t state = KILN_CODE_ORIGINAL; // KILN_ERR_UNAVAILABLE: the state of the first range that is not ORIGINAL
    uintptr_t address = 0;               // ... and its address (0 when no range was the reason)
    bool full = false;                   // KILN_ERR_UNAVAILABLE: the process-wide bounds would be passed
};

class Registry {
public:
    // A code range a Kiln service protects for an engine call (kiln.internal.hooks).
    struct Protection {
        uint64_t owner;
        uintptr_t address;
        size_t size;
        std::string reason;
        // Refusals of Kiln writers over this range, per requesting owner, for kiln.log's rate limit (under lock()): the
        // first 8 requesters count on their own, later ones share `others`.
        struct Requester {
            uint64_t owner = 0;
            uint64_t refused = 0;  // 0: a free slot
        };
        std::array<Requester, 8> requesters{};
        uint64_t others = 0;
    };
private:
    struct Replacement {
        uint64_t token;
        uintptr_t address;
        std::vector<uint8_t> original, installed;
        bool continuation;
    };
    std::mutex mutex_;
    std::vector<Replacement> replacements_;
    uint64_t next_token_ = 1;
    std::atomic<uint64_t> generation_{1};
    // Only the last range asked about is kept, so a query for another range moves the generation (kiln_hooks.h says
    // so). A map of every range callers ever asked about would grow for the life of the process.
    uintptr_t observed_address_ = 0;
    size_t observed_size_ = 0;
    uint32_t observed_state_ = KILN_CODE_UNKNOWN;
    bool observed_readable_ = false;
    std::vector<uint8_t> observed_;
    std::vector<Protection> protections_;
    size_t protected_bytes_ = 0;

    // [a, a + n) and [b, b + m) share a byte (n, m nonzero); never forms a + n or b + m, so no wraparound.
    static bool overlaps(uintptr_t a, size_t n, uintptr_t b, size_t m) { return a >= b ? a - b < m : b - a < n; }
    // lock() held: a Kiln detour or patch is recorded over some byte of [address, address + size).
    bool recorded_locked(uintptr_t address, size_t size) const {
        for (const Replacement& x : replacements_)
            if (!x.installed.empty() && overlaps(address, size, x.address, x.installed.size())) return true;
        return false;
    }

    uint32_t classify(uintptr_t address, const uint8_t* file, const uint8_t* live, size_t size) const {
        bool managed = false, foreign = false;
        for (size_t i = 0; i < size; ++i) {
            if (live[i] == file[i]) continue;
            const uintptr_t at = address + i;
            uint8_t value = live[i];
            // Unwind overlapping managed layers, newest first. An entry's range alone cannot bless foreign bytes
            // written over it, or a foreign prologue that Kiln's trampoline merely chained over.
            for (auto it = replacements_.rbegin(); it != replacements_.rend(); ++it) {
                if (at < it->address || at - it->address >= it->installed.size()) continue;
                if (!it->continuation || it->original.size() != it->installed.size()) return KILN_CODE_UNKNOWN;
                const size_t offset = at - it->address;
                if (value != it->installed[offset]) break;
                value = it->original[offset];
            }
            if (value == file[i]) managed = true;
            else foreign = true;
        }
        return foreign ? KILN_CODE_FOREIGN : managed ? KILN_CODE_MANAGED : KILN_CODE_ORIGINAL;
    }
public:
    auto lock() {
#ifdef KILN_HOOKS_TEST_LOCK_PROBE
        test_lock_probe();
#endif
        return std::unique_lock(mutex_);
    }
    // Writers call these ONLY while holding lock(), including the physical write. Prepare all allocations before
    // changing code. Tokens are never reused; a detour's fact is process-lifetime, as is its trampoline.
    uint64_t add_locked(uintptr_t address, const std::vector<uint8_t>& original,
                        const std::vector<uint8_t>& installed, bool continuation) {
        const auto token = next_token_++;
        replacements_.push_back({token, address, original, installed, continuation});
        touch_locked();
        return token;
    }
    void remove_locked(uint64_t token) {
        const auto it = std::find_if(replacements_.begin(), replacements_.end(),
                                    [=](const auto& r) { return r.token == token; });
        if (it != replacements_.end()) { replacements_.erase(it); touch_locked(); }
    }
    void touch_locked() { generation_.fetch_add(1, std::memory_order_relaxed); }
    // Dispatch cannot take a mutex. Disabling a faulted link changes no physical replacement facts; it only
    // invalidates cached ownership observations. Its later sweep publishes the new chain under lock().
    void note_chain_fault() { generation_.fetch_add(1, std::memory_order_relaxed); }

    KilnResult code_state(const Image& image, uintptr_t address, size_t size, uint32_t* state, uint64_t* generation) {
        if (!state || !generation || !valid_range(image, address, size)) return KILN_ERR_INVALID_ARGUMENT;
        std::vector<uint8_t> file(size), live(size), second(size);
        if (!image.file || !image.file(address, file.data(), size)) return KILN_ERR_UNAVAILABLE;
        auto guard = lock();
        const bool readable = image.live && image.live(address, live.data(), size) &&
            image.live(address, second.data(), size) && live == second;
        const uint32_t result = readable ? classify(address, file.data(), live.data(), size) : KILN_CODE_UNKNOWN;
        if (observed_address_ != address || observed_size_ != size || observed_readable_ != readable ||
            observed_state_ != result || (readable && observed_ != live)) touch_locked();
        observed_address_ = address;
        observed_size_ = size;
        observed_readable_ = readable;
        observed_state_ = result;
        if (readable) observed_ = std::move(live);
        else observed_.clear();
        *state = result;
        *generation = generation_.load(std::memory_order_relaxed);
        return KILN_OK;
    }

    // ---- protected ranges (kiln.internal.hooks, ../hooks_protect_internal.h) ----
    // Writers call this ONLY while holding lock(), before they read the old bytes or write: the protection that
    // [address, address + size) overlaps, or nullptr. Adjacent ranges do not overlap; size 0 overlaps nothing.
    Protection* protected_overlap_locked(uintptr_t address, size_t size) {
        if (!size) return nullptr;
        for (Protection& p : protections_)
            if (overlaps(address, size, p.address, p.size)) return &p;
        return nullptr;
    }
    // A writer refused over `p` (lock() held): how many times `requester` has been refused over it, this one included.
    static uint64_t count_refusal_locked(Protection& p, uint64_t requester) {
        for (auto& r : p.requesters)
            if (r.refused && r.owner == requester) return ++r.refused;
        for (auto& r : p.requesters)
            if (!r.refused) {
                r.owner = requester;
                return r.refused = 1;
            }
        return ++p.others;
    }
    // Writers log the 1st, 2nd, 4th, 8th, ... refusal of one requester over one protected range: a mod that retries a
    // refused hook every tick cannot flood kiln.log.
    static bool log_refusal(uint64_t count) { return count && !(count & (count - 1)); }
    // The tail of a logged refusal from the 2nd one on (empty for the 1st).
    static std::string refusal_tail(uint64_t count) {
        return count > 1 ? " [refusal " + std::to_string(count) + " of this owner over this range; logged at 1, 2, 4, 8, ...]"
                         : std::string();
    }

    // protect_code_ranges (the rules are in hooks_protect_internal.h). Every range is checked ORIGINAL under lock(),
    // with the live bytes read twice as code_state does, and with no Kiln detour or patch recorded over it; then all of
    // them are protected, or none. `alive` is asked under lock() too, and release_owner() takes it: an owner retiring
    // meanwhile either finds these ranges or this call finds the owner gone. Ranges this owner already protects are not
    // added again. It moves no generation (no code changes).
    KilnResult protect(const Image& image, uint64_t owner, const KilnCodeRange* requested, size_t n, std::string_view reason,
                       bool (*alive)(uint64_t), ProtectOutcome* out = nullptr) {
        ProtectOutcome o;
        const auto done = [&](KilnResult rc) {
            if (out) *out = o;
            return rc;
        };
        if (!owner || !requested || n == 0 || n > KILN_HOOKS_PROTECT_MAX_PER_CALL || reason.empty() ||
            reason.size() > KILN_HOOKS_PROTECT_REASON_MAX || !alive)
            return done(KILN_ERR_INVALID_ARGUMENT);
        // One copy of the caller's ranges: the checks, the reads and the protection all use the same values.
        std::array<KilnCodeRange, KILN_HOOKS_PROTECT_MAX_PER_CALL> copy{};
        std::copy_n(requested, n, copy.begin());
        const KilnCodeRange* const ranges = copy.data();
        size_t total = 0;
        for (size_t i = 0; i < n; ++i) {
            if (!valid_range(image, ranges[i].address, ranges[i].size) ||
                ranges[i].size > KILN_HOOKS_CODE_RANGE_MAX - total)
                return done(KILN_ERR_INVALID_ARGUMENT);
            total += ranges[i].size;
        }
        // Everything that allocates, and the file's bytes (never patched), before the lock.
        std::vector<uint8_t> file(total), live(total), second(total);
        for (size_t i = 0, at = 0; i < n; at += ranges[i].size, ++i)
            if (!image.file || !image.file(ranges[i].address, file.data() + at, ranges[i].size))
                return done(KILN_ERR_UNAVAILABLE);
        std::vector<Protection> fresh;
        fresh.reserve(n);
        for (size_t i = 0; i < n; ++i) fresh.push_back({owner, ranges[i].address, ranges[i].size, std::string(reason)});
        std::vector<bool> keep(n, false);
        auto guard = lock();
        if (!alive(owner)) return done(KILN_ERR_REVOKED);
        size_t kept = 0, kept_bytes = 0;
        for (size_t i = 0, at = 0; i < n; at += ranges[i].size, ++i) {
            const KilnCodeRange& r = ranges[i];
            const bool readable = image.live && image.live(r.address, live.data() + at, r.size) &&
                image.live(r.address, second.data() + at, r.size) &&
                !std::memcmp(live.data() + at, second.data() + at, r.size);
            uint32_t state = readable ? classify(r.address, file.data() + at, live.data() + at, r.size) : KILN_CODE_UNKNOWN;
            // A Kiln detour or patch recorded over the range counts even when the live bytes are the file's (a patch that
            // put the original back over another one): unpatch, revoke and revoke_owner are not checked, and undoing it
            // would change the code.
            if (state == KILN_CODE_ORIGINAL && recorded_locked(r.address, r.size)) state = KILN_CODE_MANAGED;
            if (state != KILN_CODE_ORIGINAL) {
                o.state = state;
                o.address = r.address;
                return done(KILN_ERR_UNAVAILABLE);
            }
            const auto same = [&](const Protection& p) { return p.owner == owner && p.address == r.address && p.size == r.size; };
            bool known = std::any_of(protections_.begin(), protections_.end(), same);
            for (size_t j = 0; j < i && !known; ++j) known = keep[j] && same(fresh[j]);
            if (known) continue;
            keep[i] = true;
            ++kept;
            kept_bytes += r.size;
        }
        if (kept > KILN_HOOKS_PROTECT_MAX_RANGES - protections_.size() ||
            kept_bytes > KILN_HOOKS_PROTECT_MAX_BYTES - protected_bytes_) {
            o.full = true;
            return done(KILN_ERR_UNAVAILABLE);
        }
        protections_.reserve(protections_.size() + kept);  // the only allocation under the lock; nothing changed yet
        for (size_t i = 0; i < n; ++i)
            if (keep[i]) protections_.push_back(std::move(fresh[i]));
        protected_bytes_ += kept_bytes;
        o.added = kept;
        o.added_bytes = kept_bytes;
        return done(KILN_OK);
    }

    // The owner retired: its protections go. Returns how many.
    size_t release_owner(uint64_t owner) {
        auto guard = lock();
        size_t released = 0;
        for (auto it = protections_.begin(); it != protections_.end();) {
            if (it->owner != owner) { ++it; continue; }
            protected_bytes_ -= it->size;
            it = protections_.erase(it);
            ++released;
        }
        return released;
    }

    size_t protected_count() {
        auto guard = lock();
        return protections_.size();
    }
    size_t protected_bytes() {
        auto guard = lock();
        return protected_bytes_;
    }
};
inline Registry& registry() {
    static auto& instance = *new Registry; // same lifetime as the registry's detours
    return instance;
}
} // namespace kiln::hooks::code
