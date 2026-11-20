// SPDX-License-Identifier: MIT
// kiln.markers' bridge (dossier chapter 04 §4.4, §5): the internal kiln.markers.bridge table the managed built-in receiver
// (Kiln.Markers) drives, and the pieces both link roles share: the bounded inbound queue of each role (receive is any
// thread and only copies and enqueues; the role applies it at its next step), token buckets, and the packets a role hands
// to pump. Route 2 (the hosted loopback) never reaches the managed send callback: pump moves its packets into the other
// role's inbound queue, rewritten to that role's peer and the same generation.
#ifndef KILN_MARKERS_BRIDGE_H
#define KILN_MARKERS_BRIDGE_H
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <utility>
#include <vector>

#include "services/kiln_markers.h"

namespace kmk {

constexpr uint32_t kRouteNetwork = 1, kRouteLoopback = 2;
constexpr uint32_t kLoopbackServerPeer = 0;            // the server role's peer for the hosting player's own client
constexpr uint32_t kServerPeer = 0xffffffffu;           // the client role's peer: its server (route 1 or 2)
enum PeerFlag : uint32_t { kHasPlayer = 1, kNetworkReady = 2, kHasMarkersChannel = 4, kVanilla = 8, kRejected = 16 };

// A refilling budget: rate per second, at most burst. The first use starts full.
struct Bucket {
    double rate = 0, burst = 0, tokens = 0;
    uint64_t at = 0;
    bool started = false;
    Bucket() = default;
    Bucket(double r, double b) : rate(r), burst(b) {}
    void refill(uint64_t now_ms);
    bool take(double n, uint64_t now_ms);
    double peek(uint64_t now_ms) {
        refill(now_ms);
        return tokens;
    }
};

// One copied envelope waiting for its role's step.
struct Inbound {
    uint32_t peer = 0;
    uint64_t generation = 0;
    uint32_t message = 0;
    std::vector<uint8_t> bytes;
};

// A role's inbound queue: at most 128 envelopes and 256 KiB; any thread. A full queue drops the new envelope and notes
// it for its peer (the role then asks for a full resync, never applies part of a transaction).
class Inbox {
public:
    static constexpr size_t kEnvelopes = 128;
    static constexpr size_t kBytes = size_t{256} << 10;
    bool push(uint32_t peer, uint64_t generation, uint32_t message, const uint8_t* bytes, size_t length);
    std::deque<Inbound> take();
    // Envelopes a role could not apply yet (over a budget) go back to the front, in their order.
    void put_back(std::deque<Inbound>&& front);
    // Peers that lost an envelope since the last call: (peer, generation) -> count.
    std::map<std::pair<uint32_t, uint64_t>, uint32_t> take_drops();
    size_t size() const;
    void clear();

private:
    mutable std::mutex mu_;
    std::deque<Inbound> q_;
    size_t bytes_ = 0;
    std::map<std::pair<uint32_t, uint64_t>, uint32_t> drops_;
};

// A packet a role hands to pump (a copy). tag: the role's id of the packet, for sent().
struct Outgoing {
    uint32_t peer = 0;
    uint64_t generation = 0;
    uint32_t route = kRouteNetwork;
    uint32_t message = 0;
    std::vector<uint8_t> bytes;
    uint64_t tag = 0;
};

// A role as pump sees it.
class Role {
public:
    virtual ~Role() = default;
    // Up to max_packets packets due now, at most 4 per peer, within the peers' byte budgets and the process's budget
    // (both roles). They stay queued until sent() says each went.
    virtual void collect(uint32_t max_packets, Bucket& process, uint64_t now_ms, std::vector<Outgoing>* out) = 0;
    // The transport took the packet (ok), or did not (it is tried again at the next pump, before anything after it).
    virtual void sent(const Outgoing& packet, bool ok, uint64_t now_ms) = 0;
    virtual KilnResult peer_up(const KilnMarkerPeer& peer) = 0;
    virtual KilnResult peer_down(uint32_t peer, uint64_t generation) = 0;
};

class Bridge {
public:
    Bridge(Role* server, Role* client, Inbox* server_inbox, Inbox* client_inbox) : roles_{server, client}, inbox_{server_inbox, client_inbox} {}
    KilnResult attach(KilnOwner owner);
    KilnOwner attached() const;
    KilnResult peer_up(const KilnMarkerPeer* peer);
    KilnResult peer_down(uint32_t role, uint32_t peer, uint64_t generation);
    KilnResult receive(const KilnMarkerEnvelope* envelope);
    KilnResult pump(uint32_t role, uint32_t max_packets, KilnMarkerSendFn send, void* user, uint32_t* processed, uint64_t now_ms);

private:
    Role* roles_[2];
    Inbox* inbox_[2];
    mutable std::mutex mu_;
    KilnOwner owner_ = 0;
    Bucket process_{512.0 * 1024, 256.0 * 1024};  // bytes per second, both roles
};

}  // namespace kmk
#endif
