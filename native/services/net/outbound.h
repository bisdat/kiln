// SPDX-License-Identifier: MIT
// kiln.net's outbound side with no game code in it (host tests: tests/outbound_test.cpp):
//  - the gate: once the game's shutdown begins (its "Trigger gameflow shutdown" line), kiln.net sends nothing more, in
//    either role, and drops what waits; from the server world's stop until the next one starts, the server role sends
//    nothing (the GameServer and its sockets are going away);
//  - the retry queue: frames the game refused for now (its bandwidth budget, a full session allocator), in order,
//    bounded in frames and in age, retried on the server thread.
// Thread rule: any thread; the send callback of flush is called without Outbound's lock held.
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <vector>

namespace knet {

class Outbound {
public:
    Outbound(size_t max_frames, uint64_t max_age_ms) : max_frames_(max_frames), max_age_ms_(max_age_ms) {}

    // The game's shutdown began: from now on nothing is sent and every queued frame is dropped. True the first time;
    // *dropped = the frames that were waiting then.
    bool begin_shutdown(size_t* dropped = nullptr);
    bool shutting_down() const { return shutdown_.load(std::memory_order_acquire); }
    // The server world stops (drops what waits; returns how many) / the next one starts.
    size_t server_world_stopped();
    void server_world_started();
    // Whether the server role may send now, and the client role.
    bool server_open() const { return !shutting_down() && !server_down_.load(std::memory_order_acquire); }
    bool client_open() const { return !shutting_down(); }
    bool server_world_down() const { return server_down_.load(std::memory_order_acquire); }

    enum class Queued { Yes, Full, Closed };
    // Puts a frame the game refused for now behind the others (or behind the peer's earlier ones).
    Queued queue(uint32_t peer, const uint8_t* data, size_t len, uint64_t now_ms);
    bool pending_for(uint32_t peer) const;
    size_t pending() const;

    enum class Sent { Ok, Later, Never };  // the game took it / refused it for now / the peer has no socket any more
    using SendFn = std::function<Sent(uint32_t peer, const uint8_t* data, size_t len)>;
    using DropFn = std::function<void(uint32_t peer)>;
    struct Flush {
        size_t sent = 0, dropped = 0, kept = 0;
    };
    // Sends what waits, in order: the first frame refused for now stops the rest (they keep their order); a frame that
    // waited longer than the age bound, or whose peer is gone, is dropped (dropped(peer) once per frame); once the gate
    // is closed nothing more is sent and what waits is dropped.
    Flush flush(uint64_t now_ms, const SendFn& send, const DropFn& dropped);

private:
    struct Pending {
        uint32_t peer;
        std::vector<uint8_t> data;
        uint64_t since;
    };
    size_t drop_all_locked(const DropFn* dropped);

    const size_t max_frames_;
    const uint64_t max_age_ms_;
    mutable std::mutex mu_;
    std::deque<Pending> q_;
    std::atomic<bool> shutdown_{false}, server_down_{false};
};

}  // namespace knet
