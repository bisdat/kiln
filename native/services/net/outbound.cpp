// SPDX-License-Identifier: MIT
// kiln.net's outbound side (outbound.h).
#include "outbound.h"

namespace knet {

bool Outbound::begin_shutdown(size_t* dropped) {
    std::lock_guard<std::mutex> lk(mu_);
    const bool first = !shutdown_.exchange(true, std::memory_order_acq_rel);
    const size_t n = drop_all_locked(nullptr);
    if (dropped) *dropped = n;
    return first;
}

size_t Outbound::server_world_stopped() {
    std::lock_guard<std::mutex> lk(mu_);
    server_down_.store(true, std::memory_order_release);
    return drop_all_locked(nullptr);
}

void Outbound::server_world_started() {
    std::lock_guard<std::mutex> lk(mu_);
    server_down_.store(false, std::memory_order_release);  // the shutdown, once begun, still holds (server_open)
}

Outbound::Queued Outbound::queue(uint32_t peer, const uint8_t* data, size_t len, uint64_t now_ms) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!server_open()) return Queued::Closed;
    if (q_.size() >= max_frames_) return Queued::Full;  // also behind the peer's earlier frames: the bound holds
    q_.push_back(Pending{peer, std::vector<uint8_t>(data, data + len), now_ms});
    return Queued::Yes;
}

bool Outbound::pending_for(uint32_t peer) const {
    std::lock_guard<std::mutex> lk(mu_);
    for (const auto& p : q_)
        if (p.peer == peer) return true;
    return false;
}

size_t Outbound::pending() const {
    std::lock_guard<std::mutex> lk(mu_);
    return q_.size();
}

size_t Outbound::drop_all_locked(const DropFn* dropped) {
    const size_t n = q_.size();
    if (dropped)
        for (const auto& p : q_) (*dropped)(p.peer);
    q_.clear();
    return n;
}

Outbound::Flush Outbound::flush(uint64_t now_ms, const SendFn& send, const DropFn& dropped) {
    Flush f;
    std::deque<Pending> work;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (q_.empty()) return f;
        work.swap(q_);
    }
    std::deque<Pending> keep;
    bool blocked = false;
    for (auto& p : work) {
        if (!server_open()) {  // the shutdown began (or the world stopped) while this flush ran: nothing more goes out
            ++f.dropped;
            dropped(p.peer);
            continue;
        }
        if (!blocked) {
            const Sent s = send(p.peer, p.data.data(), p.data.size());
            if (s == Sent::Ok) {
                ++f.sent;
                continue;
            }
            if (s == Sent::Never) {
                ++f.dropped;
                dropped(p.peer);
                continue;
            }
            blocked = true;  // keep order: everything after waits too
        }
        if (now_ms - p.since > max_age_ms_) {
            ++f.dropped;
            dropped(p.peer);
            continue;
        }
        keep.push_back(std::move(p));
    }
    std::vector<uint32_t> closed;  // dropped(peer) is called without the lock: callers may take their own
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!server_open()) {  // closed after the loop: what was kept goes too
            for (const auto& p : keep) closed.push_back(p.peer);
            keep.clear();
        }
        f.kept = keep.size();
        // Frames queued while this flush ran came after these: they stay behind them (at most twice the bound for a
        // moment; queue refuses new ones until it is under the bound again).
        for (auto it = keep.rbegin(); it != keep.rend(); ++it) q_.push_front(std::move(*it));
    }
    f.dropped += closed.size();
    for (uint32_t peer : closed) dropped(peer);
    return f;
}

}  // namespace knet
