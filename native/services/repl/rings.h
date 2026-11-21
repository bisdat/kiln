// SPDX-License-Identifier: MIT
// kiln.repl's record rings (the dossier's chapter 11 §4.1, "Rings, overflow and loss"): one per (machine, channel),
// sized when the first consumer needs it (default 256 KiB / 2,048 records), written by that (machine, channel)'s job
// (the queue reader's writer and delta hooks, then its send callback: one at a time) and read by the tick thread in the
// next Pre. A full ring drops the new record and counts it (`dropped`); the next record that fits carries gap = 1. The
// game's traffic is never changed and nothing is asked again.
//
// No allocation and no lock after init(): the producer reserves space, fills it and commits it; positions are 64-bit
// counters (an index is position % capacity), so full and empty never look alike. A record never wraps: one that does
// not fit before the end leaves a wrap marker and starts at 0. A second producer that reserves while one holds the ring
// (the job model says it cannot happen) drops its record instead of writing into the other's.
#pragma once
#include <atomic>
#include <cstdint>
#include <cstring>
#include <new>

namespace repl {

struct RecordHeader {
    uint32_t len;   // payload bytes after this header; kWrap: the rest of the buffer is unused
    uint16_t type;
    uint8_t gap;    // 1: the ring dropped records since the one before this
    uint8_t pad;
};
static_assert(sizeof(RecordHeader) == 8);

class Ring {
public:
    static constexpr uint32_t kWrap = 0xffffffffu;
    static constexpr uint32_t kDefaultBytes = 256u << 10, kDefaultRecords = 2048;

    Ring() = default;
    Ring(const Ring&) = delete;
    Ring& operator=(const Ring&) = delete;
    ~Ring() { delete[] buf_; }

    static constexpr uint64_t align8(uint64_t n) { return (n + 7) & ~uint64_t{7}; }

    // Tick thread, while no producer runs. `bytes` is rounded down to a multiple of 8 (at least 64).
    bool init(uint32_t bytes = kDefaultBytes, uint32_t records = kDefaultRecords) {
        release();
        bytes = bytes < 64 ? 64 : bytes & ~7u;
        buf_ = new (std::nothrow) uint8_t[bytes];
        if (!buf_) return false;
        cap_ = bytes;
        max_records_ = records ? records : 1;
        reset();
        return true;
    }
    // Tick thread, while no producer runs.
    void release() {
        delete[] buf_;
        buf_ = nullptr;
        cap_ = 0;
        reset();
    }
    bool ready() const { return buf_ != nullptr; }
    uint32_t capacity() const { return cap_; }

    // ---- producer ----
    // Space for a record of `len` payload bytes, not visible to the reader until commit(); nullptr when it does not fit
    // (the record is dropped and counted; the next committed one carries gap = 1) or another reservation is open.
    uint8_t* reserve(uint16_t type, uint32_t len) {
        if (!buf_) return nullptr;
        if (busy_.exchange(true, std::memory_order_acquire)) {  // another producer holds the ring: never share it
            drop();
            return nullptr;
        }
        const uint64_t rec = align8(sizeof(RecordHeader) + uint64_t{len});
        const uint64_t h = head_.load(std::memory_order_relaxed);
        const uint64_t t = tail_.load(std::memory_order_acquire);
        const uint64_t to_end = cap_ - h % cap_;
        const uint64_t pad = rec > to_end ? to_end : 0;
        if (rec > cap_ || h + pad + rec - t > cap_ ||
            written_.load(std::memory_order_relaxed) - read_.load(std::memory_order_acquire) >= max_records_) {
            drop();
            busy_.store(false, std::memory_order_release);
            return nullptr;
        }
        res_pos_ = h;
        res_pad_ = pad;
        res_len_ = len;
        res_type_ = type;
        return buf_ + (h + pad) % cap_ + sizeof(RecordHeader);
    }
    // Publishes the open reservation with `len` (<= the reserved length) payload bytes.
    void commit(uint32_t len) {
        if (!busy_.load(std::memory_order_relaxed)) return;
        if (len > res_len_) len = res_len_;
        if (res_pad_) {
            RecordHeader wrap{kWrap, 0, 0, 0};
            std::memcpy(buf_ + res_pos_ % cap_, &wrap, sizeof wrap);
        }
        const uint64_t at = res_pos_ + res_pad_;
        RecordHeader rh{len, res_type_, static_cast<uint8_t>(gap_.exchange(false, std::memory_order_relaxed) ? 1 : 0), 0};
        std::memcpy(buf_ + at % cap_, &rh, sizeof rh);
        written_.store(written_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
        head_.store(at + align8(sizeof(RecordHeader) + uint64_t{len}), std::memory_order_release);
        busy_.store(false, std::memory_order_release);
    }
    // Gives the open reservation back (nothing is published, nothing counted).
    void abort() { busy_.store(false, std::memory_order_release); }
    bool reserved() const { return busy_.load(std::memory_order_relaxed); }

    bool push(uint16_t type, const void* data, uint32_t len) {
        uint8_t* p = reserve(type, len);
        if (!p) return false;
        if (len) std::memcpy(p, data, len);
        commit(len);
        return true;
    }

    // ---- consumer (the tick thread) ----
    // fn(const RecordHeader&, const uint8_t* payload) for every committed record, oldest first. Returns the count.
    template <class Fn>
    uint32_t drain(Fn&& fn) {
        if (!buf_) return 0;
        uint64_t t = tail_.load(std::memory_order_relaxed);
        const uint64_t h = head_.load(std::memory_order_acquire);
        uint32_t n = 0;
        while (t < h) {
            const uint64_t idx = t % cap_;
            RecordHeader rh;
            std::memcpy(&rh, buf_ + idx, sizeof rh);
            if (rh.len == kWrap) {
                t += cap_ - idx;
                continue;
            }
            fn(static_cast<const RecordHeader&>(rh), static_cast<const uint8_t*>(buf_ + idx + sizeof rh));
            t += align8(sizeof(RecordHeader) + uint64_t{rh.len});
            ++n;
        }
        read_.store(read_.load(std::memory_order_relaxed) + n, std::memory_order_release);
        tail_.store(t, std::memory_order_release);
        return n;
    }
    // Drops every unread record without reading it (the machine's peer changed). Tick thread, no producer running.
    void discard() {
        tail_.store(head_.load(std::memory_order_acquire), std::memory_order_release);
        read_.store(written_.load(std::memory_order_acquire), std::memory_order_release);
        gap_.store(false, std::memory_order_relaxed);
    }

    uint64_t dropped() const { return dropped_.load(std::memory_order_relaxed); }
    uint64_t written() const { return written_.load(std::memory_order_relaxed); }
    // Tick thread, no producer running.
    void clear_counters() {
        dropped_.store(0, std::memory_order_relaxed);
    }

private:
    void reset() {
        head_.store(0, std::memory_order_relaxed);
        tail_.store(0, std::memory_order_relaxed);
        written_.store(0, std::memory_order_relaxed);
        read_.store(0, std::memory_order_relaxed);
        dropped_.store(0, std::memory_order_relaxed);
        busy_.store(false, std::memory_order_relaxed);
        gap_.store(false, std::memory_order_relaxed);
    }
    void drop() {
        dropped_.fetch_add(1, std::memory_order_relaxed);
        gap_.store(true, std::memory_order_relaxed);
    }

    uint8_t* buf_ = nullptr;
    uint64_t cap_ = 0;
    uint32_t max_records_ = 0;
    std::atomic<uint64_t> head_{0};     // producer: committed end
    std::atomic<uint64_t> tail_{0};     // consumer: read up to here
    std::atomic<uint64_t> written_{0};  // records committed (producer)
    std::atomic<uint64_t> read_{0};     // records read (consumer)
    std::atomic<uint64_t> dropped_{0};
    std::atomic<bool> busy_{false};     // a reservation is open
    // the open reservation (its producer only)
    uint64_t res_pos_ = 0, res_pad_ = 0;
    uint32_t res_len_ = 0;
    uint16_t res_type_ = 0;
    std::atomic<bool> gap_{false};      // set by a drop, taken by the next commit
};

}  // namespace repl
