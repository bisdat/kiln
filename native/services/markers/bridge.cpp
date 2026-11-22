// SPDX-License-Identifier: MIT
#include "bridge.h"

#include <algorithm>
#include <cstring>

#include "wire.h"

namespace kmk {

void Bucket::refill(uint64_t now) {
    if (!started) {
        started = true;
        tokens = burst;
        at = now;
        return;
    }
    if (now > at) tokens = std::min(burst, tokens + static_cast<double>(now - at) * rate / 1000.0);
    at = now;
}

bool Bucket::take(double n, uint64_t now) {
    refill(now);
    if (tokens < n) return false;
    tokens -= n;
    return true;
}

bool Inbox::push(uint32_t peer, uint64_t generation, uint32_t message, const uint8_t* bytes, size_t length) {
    std::lock_guard g(mu_);
    if (q_.size() >= kEnvelopes || bytes_ + length > kBytes || (per_peer_ && queued_[{peer, generation}] >= per_peer_)) {
        ++drops_[{peer, generation}];
        return false;
    }
    ++queued_[{peer, generation}];
    Inbound m;
    m.peer = peer;
    m.generation = generation;
    m.message = message;
    if (length) m.bytes.assign(bytes, bytes + length);
    bytes_ += length;
    q_.push_back(std::move(m));
    return true;
}

std::deque<Inbound> Inbox::take() {
    std::lock_guard g(mu_);
    std::deque<Inbound> out;
    out.swap(q_);
    bytes_ = 0;
    queued_.clear();
    return out;
}

void Inbox::put_back(std::deque<Inbound>&& front) {
    if (front.empty()) return;
    std::lock_guard g(mu_);
    for (const Inbound& m : front) {
        bytes_ += m.bytes.size();
        ++queued_[{m.peer, m.generation}];
    }
    for (auto it = front.rbegin(); it != front.rend(); ++it) q_.push_front(std::move(*it));
}

std::map<std::pair<uint32_t, uint64_t>, uint32_t> Inbox::take_drops() {
    std::lock_guard g(mu_);
    std::map<std::pair<uint32_t, uint64_t>, uint32_t> out;
    out.swap(drops_);
    return out;
}

size_t Inbox::size() const {
    std::lock_guard g(mu_);
    return q_.size();
}

void Inbox::clear() {
    std::lock_guard g(mu_);
    q_.clear();
    bytes_ = 0;
    drops_.clear();
    queued_.clear();
}

void Inbox::set_per_peer(size_t n) {
    std::lock_guard g(mu_);
    per_peer_ = n;
}

// ---- the table's members ------------------------------------------------------------------------------------------------

KilnResult Bridge::attach(KilnOwner owner) {
    if (!owner) return KILN_ERR_INVALID_ARGUMENT;
    std::lock_guard g(mu_);
    owner_ = owner;  // startup only; a second attach (a test kit's next harness) takes over
    return KILN_OK;
}

KilnOwner Bridge::attached() const {
    std::lock_guard g(mu_);
    return owner_;
}

KilnResult Bridge::peer_up(const KilnMarkerPeer* p) {
    if (!p || p->size < sizeof(KilnMarkerPeer) || p->role > 1 || !p->generation) return KILN_ERR_INVALID_ARGUMENT;
    if (p->route != kRouteNetwork && p->route != kRouteLoopback) return KILN_ERR_INVALID_ARGUMENT;
    if (!attached()) return KILN_ERR_UNAVAILABLE;
    Role* r = roles_[p->role];
    return r ? r->peer_up(*p) : KILN_ERR_UNAVAILABLE;
}

KilnResult Bridge::peer_down(uint32_t role, uint32_t peer, uint64_t generation) {
    if (role > 1 || !generation) return KILN_ERR_INVALID_ARGUMENT;
    if (!attached()) return KILN_ERR_UNAVAILABLE;
    Role* r = roles_[role];
    return r ? r->peer_down(peer, generation) : KILN_ERR_UNAVAILABLE;
}

KilnResult Bridge::receive(const KilnMarkerEnvelope* e) {
    if (!e || e->size < sizeof(KilnMarkerEnvelope) || e->role > 1 || !e->peer_generation) return KILN_ERR_INVALID_ARGUMENT;
    if (e->length && !e->bytes) return KILN_ERR_INVALID_ARGUMENT;
    if (!attached() || !inbox_[e->role]) return KILN_ERR_UNAVAILABLE;
    // A body over the wire's limit is still queued (cut one byte past the limit), so the role counts it as a bad
    // message of that peer instead of it vanishing here.
    const size_t n = e->length > wire::kMaxBody + 1 ? wire::kMaxBody + 1 : e->length;
    return inbox_[e->role]->push(e->peer, e->peer_generation, e->message, e->bytes, n) ? KILN_OK : KILN_ERR_BUFFER_TOO_SMALL;
}

KilnResult Bridge::pump(uint32_t role, uint32_t max_packets, KilnMarkerSendFn send, void* user, uint32_t* processed, uint64_t now) {
    if (processed) *processed = 0;
    if (role > 1) return KILN_ERR_INVALID_ARGUMENT;
    if (!attached()) return KILN_ERR_UNAVAILABLE;
    Role* r = roles_[role];
    if (!r) return KILN_ERR_UNAVAILABLE;
    const uint32_t cap = std::min<uint32_t>(max_packets, 16);  // 16 packets per role and pump at most
    std::vector<Outgoing> out;
    {
        std::lock_guard g(mu_);  // the process budget; the role takes its own lock inside collect
        r->collect(cap, process_, now, &out);
    }
    uint32_t done = 0;
    std::vector<uint32_t> blocked;  // peers whose transport said "later": nothing after that packet goes this pump
    for (const Outgoing& o : out) {
        bool ok = false;
        if (std::find(blocked.begin(), blocked.end(), o.peer) == blocked.end()) {
            if (o.route == kRouteLoopback) {
                // the hosted loopback: the same bytes into the other role's queue, as that role's peer
                Inbox* other = inbox_[role ^ 1];
                const uint32_t to = role == 0 ? kServerPeer : kLoopbackServerPeer;
                ok = other && other->push(to, o.generation, o.message, o.bytes.data(), o.bytes.size());
            } else if (send) {
                KilnMarkerEnvelope e{};
                e.size = sizeof e;
                e.role = role;
                e.peer = o.peer;
                e.message = o.message;
                e.peer_generation = o.generation;
                e.bytes = o.bytes.data();
                e.length = static_cast<uint32_t>(o.bytes.size());
                ok = send(&e, user) == 1;  // outside every lock of the service
            }
            if (!ok) blocked.push_back(o.peer);
        }
        r->sent(o, ok, now);
        if (ok) ++done;
    }
    if (processed) *processed = done;
    return KILN_OK;
}

}  // namespace kmk
