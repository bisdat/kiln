// SPDX-License-Identifier: MIT
// kiln.abilities: the impact pool watch's pure parts (since 0.2.0-dev.29.19; inbox 116). The game takes every impact's
// stack, hit memory and collision arrays from one 16 MiB allocator per ImpactSystem; once it is full each new impact
// fails ("[impact] Impact System could not allocate impact (collisions)"), hits are lost, the world crawls and the game
// can crash. The watch reads the pool's counters, warns in kiln.log at 50/75/90 % and names the programs that fill it
// (impact_watch.cpp; layout and evidence: docs/re/impact.md "The impact memory pool"). Everything here is plain C++ so
// a host test drives it (tests/impact_watch_test.cpp): no Windows, no game memory, no allocation.
#pragma once
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace kab::iw {

// ---- one reading of the pool: five numbers ----
struct Reading {
    uint64_t used;       // allocator bytes in use
    uint64_t peak;       // the allocator's peak bytes
    uint64_t capacity;   // the allocator's size (16 MiB on every build seen)
    uint64_t live;       // live impact instances (the slot pool's count)
    uint64_t live_peak;  // their peak
};

// The check made once per ImpactSystem before the watch trusts its offsets: a capacity that is a power of two between
// 1 MiB and 1 GiB, used <= peak <= capacity, live <= live_peak < 2^24. Anything else: the layout is not the one the
// offsets were read from, and the watch stays off for that world.
enum class Layout { Ok, BadCapacity, UsedOverCapacity, PeakUnderUsed, LiveOverPeak };
inline Layout check_layout(const Reading& r) {
    constexpr uint64_t kMin = 1ull << 20, kMax = 1ull << 30;
    if (r.capacity < kMin || r.capacity > kMax || (r.capacity & (r.capacity - 1)) != 0) return Layout::BadCapacity;
    if (r.used > r.capacity || r.peak > r.capacity) return Layout::UsedOverCapacity;
    if (r.peak < r.used) return Layout::PeakUnderUsed;
    if (r.live > r.live_peak || r.live_peak >= (1ull << 24)) return Layout::LiveOverPeak;
    return Layout::Ok;
}
inline const char* layout_text(Layout l) {
    switch (l) {
        case Layout::Ok: return "ok";
        case Layout::BadCapacity: return "the capacity is not a power of two between 1 MiB and 1 GiB";
        case Layout::UsedOverCapacity: return "bytes in use (or their peak) exceed the capacity";
        case Layout::PeakUnderUsed: return "the peak is below the bytes in use";
        case Layout::LiveOverPeak: return "live instances exceed their peak (or the peak is absurd)";
    }
    return "?";
}
inline double used_pct(const Reading& r) { return r.capacity ? static_cast<double>(r.used) * 100.0 / static_cast<double>(r.capacity) : 0.0; }
inline double peak_pct(const Reading& r) { return r.capacity ? static_cast<double>(r.peak) * 100.0 / static_cast<double>(r.capacity) : 0.0; }

// ---- warnings: each level once per session; one "recovered" line when it falls under 40 % after reaching 75 % ----
enum : uint32_t { kNone = 0, kWarn50 = 1, kWarn75 = 2, kWarn90 = 4, kRecovered = 8 };
struct Thresholds {
    bool w50 = false, w75 = false, w90 = false, recovered = false;
    // The events of one reading (several levels at once when the pool jumped past them between two readings).
    uint32_t step(double pct) {
        uint32_t ev = kNone;
        if (pct >= 50.0 && !w50) w50 = true, ev |= kWarn50;
        if (pct >= 75.0 && !w75) w75 = true, ev |= kWarn75;
        if (pct >= 90.0 && !w90) w90 = true, ev |= kWarn90;
        if (w75 && !recovered && pct < 40.0) recovered = true, ev |= kRecovered;
        return ev;
    }
    void reset() { *this = Thresholds{}; }
};
inline int highest(uint32_t ev) { return (ev & kWarn90) ? 90 : (ev & kWarn75) ? 75 : (ev & kWarn50) ? 50 : 0; }

// ---- live instances per program: a fixed open-addressing table (N a power of two), filled by one walk ----
struct Top {
    uint32_t id;
    uint32_t count;
};
template <size_t N>
struct ProgramCounts {
    static_assert(N >= 2 && (N & (N - 1)) == 0, "N: a power of two");
    uint32_t ids[N] = {};
    uint32_t counts[N] = {};  // 0 = free
    size_t n = 0;
    uint32_t sum = 0, over = 0;
    void clear() {
        std::fill(std::begin(counts), std::end(counts), 0u);
        n = 0;
        sum = over = 0;
    }
    void add(uint32_t id) {
        ++sum;
        size_t i = (id * 0x9E3779B1u) & (N - 1);
        for (size_t p = 0; p < N; ++p, i = (i + 1) & (N - 1)) {
            if (counts[i] && ids[i] == id) {
                ++counts[i];
                return;
            }
            if (!counts[i]) {
                if (n == N) break;
                ids[i] = id;
                counts[i] = 1;
                ++n;
                return;
            }
        }
        ++over;  // more distinct programs than the table holds
    }
    // The k with the most instances, most first; ties: the lower id first. Returns how many were written.
    size_t top(Top* out, size_t k) const {
        size_t got = 0;
        for (size_t i = 0; i < N; ++i) {
            if (!counts[i]) continue;
            const Top t{ids[i], counts[i]};
            size_t pos = got;
            while (pos > 0 && (out[pos - 1].count < t.count || (out[pos - 1].count == t.count && out[pos - 1].id > t.id))) {
                if (pos < k) out[pos] = out[pos - 1];
                --pos;
            }
            if (pos < k) {
                out[pos] = t;
                if (got < k) ++got;
            }
        }
        return got;
    }
    uint32_t total() const { return sum; }
    size_t distinct() const { return n; }
    uint32_t overflow() const { return over; }
};

// ---- the K oldest of a stream (smallest creation time first; unknown ages after every known one, first seen first) ----
constexpr int64_t kUnknownAge = INT64_MAX;
struct Aged {
    uint64_t key;  // what the caller needs to find the instance again (its slot address)
    int64_t t;     // creation time, or kUnknownAge
};
template <size_t K>
struct OldestK {
    Aged v[K] = {};
    size_t n = 0;
    void clear() { n = 0; }
    void offer(uint64_t key, int64_t t) {
        size_t pos = n;
        while (pos > 0 && v[pos - 1].t > t) --pos;  // strictly older moves ahead; equal times keep arrival order
        if (pos >= K) return;
        const size_t last = n < K ? n : K - 1;
        for (size_t i = last; i > pos; --i) v[i] = v[i - 1];
        v[pos] = Aged{key, t};
        if (n < K) ++n;
    }
    size_t size() const { return n; }
    Aged at(size_t i) const { return v[i]; }
};

// ---- creation times: a fixed, lock-free table the game's job threads write in createImpactInstance ----
// key: (system index << 32) | the id's slot part (the low 24 bits: block and slot; the top byte is the slot's
// generation), so a reused slot overwrites its entry and the table never holds more keys than the pools have slots.
// stamp: (creation time in ms, 32-bit wrapping) << 32 | the full id, one atomic word, so a reader never sees the time
// of one instance with the id of another. A reader trusts an entry only when its id is the instance's current id.
inline uint64_t key_of(uint32_t system_index, uint32_t id) { return (static_cast<uint64_t>(system_index) << 32) | (id & 0x00FFFFFFu); }
inline uint64_t make_stamp(uint32_t ms, uint32_t id) { return (static_cast<uint64_t>(ms) << 32) | id; }
inline uint32_t stamp_id(uint64_t s) { return static_cast<uint32_t>(s); }
inline uint32_t stamp_ms(uint64_t s) { return static_cast<uint32_t>(s >> 32); }
inline uint32_t age_ms(uint32_t then, uint32_t now) { return now - then; }  // modulo 2^32: right across the clock's wrap

template <size_t N, size_t Probe>
struct AgeTable {
    static_assert(N >= 2 && (N & (N - 1)) == 0 && Probe >= 1 && Probe <= N, "N: a power of two");
    static constexpr uint64_t kEmpty = 0, kTomb = ~0ull;  // system index 0 is never used, so no key is 0 or ~0
    struct Entry {
        std::atomic<uint64_t> key{kEmpty};
        std::atomic<uint64_t> stamp{0};
    };
    Entry e[N];
    std::atomic<uint64_t> refused{0};

    static size_t home(uint64_t k) {
        k ^= k >> 33;
        k *= 0xff51afd7ed558ccdull;
        k ^= k >> 33;
        return static_cast<size_t>(k & (N - 1));
    }
    // Any thread, never blocks: the key's entry gets the stamp (an existing entry, else the first tombstone or empty
    // slot of its probe window). False when the window is full of other keys (counted in full()).
    bool insert_id(uint64_t key, uint32_t id, uint32_t ms) { return put(key, make_stamp(ms, id)); }
    bool insert(uint64_t key, uint32_t ms) { return insert_id(key, static_cast<uint32_t>(key), ms); }  // tests: id = slot part
    const Entry* find(uint64_t key) const {
        size_t i = home(key);
        for (size_t p = 0; p < Probe; ++p, i = (i + 1) & (N - 1)) {
            const uint64_t k = e[i].key.load(std::memory_order_acquire);
            if (k == key) return &e[i];
            if (k == kEmpty) return nullptr;
        }
        return nullptr;
    }
    size_t slot_of(uint64_t key) const {
        const Entry* x = find(key);
        return x ? static_cast<size_t>(x - e) : N;
    }
    void remove(uint64_t key) {
        if (const Entry* x = find(key)) const_cast<Entry*>(x)->key.store(kTomb, std::memory_order_release);
    }
    // Tombstones every entry of one system (its world ended: a new session starts with nothing).
    void clear_system(uint32_t system_index) {
        for (auto& x : e) {
            uint64_t k = x.key.load(std::memory_order_acquire);
            if (k != kEmpty && k != kTomb && (k >> 32) == system_index) x.key.compare_exchange_strong(k, kTomb);
        }
    }
    size_t used() const {
        size_t c = 0;
        for (const auto& x : e) {
            const uint64_t k = x.key.load(std::memory_order_relaxed);
            c += k != kEmpty && k != kTomb;
        }
        return c;
    }
    uint64_t full() const { return refused.load(std::memory_order_relaxed); }

private:
    bool put(uint64_t key, uint64_t stamp) {
        size_t i = home(key), tomb = N;
        for (size_t p = 0; p < Probe; ++p, i = (i + 1) & (N - 1)) {
            uint64_t k = e[i].key.load(std::memory_order_acquire);
            if (k == key) {
                e[i].stamp.store(stamp, std::memory_order_release);
                return true;
            }
            if (k == kTomb) {
                if (tomb == N) tomb = i;
                continue;
            }
            if (k == kEmpty) {
                if (claim(tomb != N ? tomb : i, tomb != N ? kTomb : kEmpty, key, stamp)) return true;
                if (claim(i, kEmpty, key, stamp)) return true;  // the tombstone went to another writer: this slot then
                k = e[i].key.load(std::memory_order_acquire);
                if (k == key) {  // another writer claimed it for the same key just now
                    e[i].stamp.store(stamp, std::memory_order_release);
                    return true;
                }
            }
        }
        if (tomb != N && claim(tomb, kTomb, key, stamp)) return true;
        refused.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    bool claim(size_t i, uint64_t expect, uint64_t key, uint64_t stamp) {
        if (!e[i].key.compare_exchange_strong(expect, key, std::memory_order_acq_rel)) return false;
        e[i].stamp.store(stamp, std::memory_order_release);
        return true;
    }
};

// ---- <game>/kiln/control/impacts: "dump" (once, while the game runs) or "off" (no watch) ----
enum class Control { None, Dump, Off };
inline Control parse_control(std::string_view s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    auto is = [&](std::string_view w) {
        if (b - a != w.size()) return false;
        for (size_t i = 0; i < w.size(); ++i)
            if (std::tolower(static_cast<unsigned char>(s[a + i])) != w[i]) return false;
        return true;
    };
    return is("dump") ? Control::Dump : is("off") ? Control::Off : Control::None;
}

// ---- the Windows side (impact_watch.cpp), called by abilities.cpp ----
// is_vanilla(program, &known): whether the game's own ImpactRegistry list holds the program (known false: no list).
using VanillaFn = bool (*)(uint32_t program, bool* known);
}  // namespace kab::iw

#ifdef KILN_ABI_VERSION
namespace kab::iw {
// From kiln_plugin_main, after kab::E is filled: resolves the offsets and subscribes the server and client worlds' Pre.
void init(const KilnApi* api, KilnOwner owner, VanillaFn is_vanilla);
// createImpactInstance's detour (job threads), after the game returned the new instance's id.
void on_created(uintptr_t system, uint32_t id);
}  // namespace kab::iw
#endif
