// SPDX-License-Identifier: MIT
// kiln.repl.net's sidecar tracking, the parts that are plain data (track.cpp wires them to the game; the host tests run
// them as they are): which events one send-callback batch of a client channel gives (CREATED for every create the session
// accepted, DESTROYED for a destroy list's list A, LEFT for its list B; messages beyond batch->sent were refused and give
// none; the prediction channel gives none), the per-machine record that carries them from the job to the tick thread
// (rings.h: one record per batch, no allocation), its reading with GAP for records a full ring dropped, and the table of
// tracks (an owner's revoke marks its tracks from any thread; the tick thread frees them between frames).
#pragma once
#include <atomic>
#include <cstdint>
#include <cstring>

#include "layout.h"
#include "rings.h"
#include "services/kiln_repl.h"
#include "wire.h"

namespace repl::track {

// One event of a record (12 bytes; the ring's payloads are 8-aligned).
struct Rec {
    uint32_t entity, tick;
    uint8_t kind, pad[3];  // KILN_REPL_TRACK_CREATED / _DESTROYED / _LEFT
};
static_assert(sizeof(Rec) == 12);
constexpr uint16_t kRecTrack = 1;

// The events of one message of a client channel (a MsgEntry's bytes; on a client, what ClientStore is handed): a create
// gives CREATED with the header's tick (the entity's generation for that peer), a destroy list one DESTROYED per id of list
// A and one LEFT per id of list B, both with the list's tick; anything else none. Writes at most cap - at events into
// out + at (out may be null to count). Returns how many it gives.
inline uint32_t scan_message(const uint8_t* d, uint64_t n, Rec* out, uint32_t at, uint32_t cap) {
    if (!d || n < 4) return 0;
    const uint32_t kind = d[0] & 7;
    if (kind == kKindCreate) {
        Header h{};
        if (!parse_header(d, n, &h)) return 0;
        if (out && at < cap) out[at] = Rec{h.entity, h.tick, KILN_REPL_TRACK_CREATED, {}};
        return 1;
    }
    if (kind != kKindDestroyList) return 0;
    DestroyList l{};
    if (!parse_destroy(d, n, &l)) return 0;
    const uint32_t total = l.na + l.nb;
    if (out)
        for (uint32_t k = 0; k < total && at + k < cap; ++k)
            out[at + k] = Rec{u32_at(l.ids + 4 * k), l.tick,
                              static_cast<uint8_t>(k < l.na ? KILN_REPL_TRACK_DESTROYED : KILN_REPL_TRACK_LEFT), {}};
    return total;
}

// The events of a send-callback batch: only msgs[0 .. sent) (the session took them in order and refused the rest), only
// entity messages (type 4) and destroy lists (type 5).
inline uint32_t scan_batch(const MsgEntry* msgs, uint32_t count, uint32_t sent, Rec* out, uint32_t cap) {
    if (!msgs) return 0;
    const uint32_t ok = sent < count ? sent : count;
    uint32_t n = 0;
    for (uint32_t i = 0; i < ok; ++i) {
        const MsgEntry& m = msgs[i];
        if (!m.data || m.size < 4) continue;
        if (m.type == kMsgEntity && (m.data[0] & 7) != kKindCreate) continue;
        if (m.type == kMsgList && (m.data[0] & 7) != kKindDestroyList) continue;
        if (m.type != kMsgEntity && m.type != kMsgList) continue;
        n += scan_message(m.data, m.size, out, n, cap);
    }
    return n;
}

// The job's part (the SendCallback hook, inside its guard): one record with the batch's events, or nothing. The prediction
// channel is not tracked. False: nothing to write, or the ring dropped it (the next record carries gap).
inline bool push_batch(Ring& r, const Batch* b) {
    if (!b || (b->pred & 1) || !b->msgs || !r.ready()) return false;
    const uint32_t n = scan_batch(b->msgs, b->count, b->sent, nullptr, 0);
    if (!n) return false;
    uint8_t* p = r.reserve(kRecTrack, static_cast<uint32_t>(n * sizeof(Rec)));
    if (!p) return false;
    const uint32_t w = scan_batch(b->msgs, b->count, b->sent, reinterpret_cast<Rec*>(p), n);
    r.commit(static_cast<uint32_t>((w < n ? w : n) * sizeof(Rec)));
    return true;
}

// The same for one message a client stored (ClientStore, main thread).
inline bool push_message(Ring& r, const uint8_t* d, uint64_t size) {
    if (!r.ready()) return false;
    const uint32_t n = scan_message(d, size, nullptr, 0, 0);
    if (!n) return false;
    uint8_t* p = r.reserve(kRecTrack, static_cast<uint32_t>(n * sizeof(Rec)));
    if (!p) return false;
    const uint32_t w = scan_message(d, size, reinterpret_cast<Rec*>(p), 0, n);
    r.commit(static_cast<uint32_t>((w < n ? w : n) * sizeof(Rec)));
    return true;
}

// The consumer (tick thread / client main thread): emit(kind, entity, tick) for every event of every record, oldest
// first; a record that follows dropped ones is preceded by emit(KILN_REPL_TRACK_GAP, 0, 0). Returns the events emitted.
template <class Emit>
inline uint32_t drain(Ring& r, Emit&& emit) {
    uint32_t n = 0;
    r.drain([&](const RecordHeader& h, const uint8_t* p) {
        if (h.type != kRecTrack) return;
        if (h.gap) {
            emit(static_cast<uint8_t>(KILN_REPL_TRACK_GAP), 0u, 0u);
            ++n;
        }
        for (uint32_t k = 0; k < h.len / sizeof(Rec); ++k) {
            Rec e;
            std::memcpy(&e, p + k * sizeof(Rec), sizeof e);
            emit(e.kind, e.entity, e.tick);
            ++n;
        }
    });
    return n;
}

// ---- the tracks ----
struct Slot {
    std::atomic<uint64_t> handle{0};  // 0: free
    std::atomic<bool> removed{false};
    KilnOwner owner = 0;
    uint32_t direction = 0;           // KILN_REPL_SEND / KILN_REPL_RECEIVE
    KilnReplTrackFn fn = nullptr;
    void* user = nullptr;
};

template <uint32_t N>
struct Table {
    Slot at[N];
    uint64_t serial = 0;

    // The thread of the direction (tick / client main). 0: no free slot.
    uint64_t add(KilnOwner owner, uint32_t direction, KilnReplTrackFn fn, void* user, uint64_t tag) {
        for (Slot& s : at) {
            if (s.handle.load(std::memory_order_acquire)) continue;
            s.owner = owner;
            s.direction = direction;
            s.fn = fn;
            s.user = user;
            s.removed.store(false, std::memory_order_relaxed);
            const uint64_t h = tag | (++serial & 0xffffffffffffull);
            s.handle.store(h, std::memory_order_release);
            return h;
        }
        return 0;
    }
    // Any thread: the track is called no more from the next delivery on. The handle's direction, or 0 when it is not a
    // live track.
    uint32_t remove(uint64_t h) {
        if (!h) return 0;
        for (Slot& s : at)
            if (s.handle.load(std::memory_order_acquire) == h && !s.removed.exchange(true, std::memory_order_acq_rel))
                return s.direction;
        return 0;
    }
    // Any thread (an owner's revoke): the directions (bits) it had a live track in.
    uint32_t retire(KilnOwner owner) {
        uint32_t dirs = 0;
        for (Slot& s : at)
            if (s.handle.load(std::memory_order_acquire) && s.owner == owner && !s.removed.exchange(true, std::memory_order_acq_rel))
                dirs |= s.direction;
        return dirs;
    }
    // The direction's own thread, between deliveries: frees its removed slots; returns its live tracks.
    uint32_t apply(uint32_t direction) {
        uint32_t live = 0;
        for (Slot& s : at) {
            if (!s.handle.load(std::memory_order_acquire) || s.direction != direction) continue;
            if (s.removed.load(std::memory_order_acquire)) {
                s.fn = nullptr;
                s.user = nullptr;
                s.owner = 0;
                s.handle.store(0, std::memory_order_release);
                continue;
            }
            ++live;
        }
        return live;
    }
    // fn(slot) for every live track of the direction that was not removed.
    template <class F>
    void each(uint32_t direction, F&& fn) {
        for (Slot& s : at)
            if (s.handle.load(std::memory_order_acquire) && s.direction == direction && !s.removed.load(std::memory_order_acquire)) fn(s);
    }
};

}  // namespace repl::track
