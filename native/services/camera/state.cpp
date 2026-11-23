// SPDX-License-Identifier: MIT
// kiln.camera's lease model: state.h says what it is and which thread may do what.
#include "state.h"

#include <chrono>

#include <cmath>
#include <cstring>

namespace kcam {

namespace {

constexpr uint32_t kKnownParts = KILN_CAMERA_MODE | KILN_CAMERA_PATH | KILN_CAMERA_PROFILE | KILN_CAMERA_SHAKE | KILN_CAMERA_LENS;
constexpr double kMaxMetres = 1000000.0;

bool finite_position(const double* p) {
    for (int i = 0; i < 3; ++i)
        if (!std::isfinite(p[i]) || std::fabs(p[i]) > kMaxMetres) return false;
    return true;
}

// An authored quaternion: finite, within 0.001 of unit length (squared). Normalized in place.
bool unit_quaternion(float* q) {
    double n2 = 0;
    for (int i = 0; i < 4; ++i) {
        if (!std::isfinite(q[i])) return false;
        n2 += static_cast<double>(q[i]) * q[i];
    }
    if (!(n2 > 0) || std::fabs(n2 - 1.0) > 0.001) return false;
    const double n = std::sqrt(n2);
    for (int i = 0; i < 4; ++i) q[i] = static_cast<float>(q[i] / n);
    return true;
}

bool lens_degrees(float v) { return std::isfinite(v) && v >= 5.0f && v <= 160.0f; }

uint32_t scope_of(uint32_t endpoint) { return endpoint == KILN_CAMERA_SERVER_MODE ? KILN_CAMERA_COMMIT_SERVER_STATE : KILN_CAMERA_COMMIT_CLIENT_FRAME; }

}  // namespace

bool validate(const KilnCameraRequest* r, Stored* out) {
    if (!r || !out || r->size < sizeof(KilnCameraRequest)) return false;
    KilnCameraRequest q;
    std::memcpy(&q, r, sizeof q);  // read once: the caller's memory may change under us
    if (!q.parts || (q.parts & ~kKnownParts)) return false;
    if (q.endpoint < KILN_CAMERA_SERVER_MODE || q.endpoint > KILN_CAMERA_REMOTE_VISUAL) return false;
    if (q.endpoint != KILN_CAMERA_LOCAL_VISUAL && q.player_entity == 0) return false;
    if (q.input_policy > KILN_CAMERA_AIM_TOO) return false;
    if (q.priority < -1000 || q.priority > 1000) return false;
    if (q.duration_ms < 1 || q.duration_ms > KILN_CAMERA_MAX_DURATION_MS) return false;
    if (static_cast<uint64_t>(q.blend_in_ms) + q.blend_out_ms > q.duration_ms) return false;
    if (q.reserved[0] || q.reserved[1]) return false;
    if (q.lens_source > KILN_CAMERA_LENS_KEYS) return false;

    const bool path = (q.parts & KILN_CAMERA_PATH) != 0, lens = (q.parts & KILN_CAMERA_LENS) != 0;
    out->keys.clear();
    if (path) {
        if (!q.keys || q.key_count < 1 || q.key_count > KILN_CAMERA_MAX_KEYS) return false;
        out->keys.assign(q.keys, q.keys + q.key_count);
    } else if (q.keys || q.key_count) {
        return false;
    }

    if (is_mode(q.parts)) {
        // a mode lock stands alone, and the game picks its own blend from the transition
        if (q.parts != KILN_CAMERA_MODE || q.input_policy == KILN_CAMERA_VISUAL_ONLY) return false;
        if (q.camera_id > 45 || q.transition_override > KILN_CAMERA_TRANSITION_LERP) return false;
        if (q.blend_in_ms || q.blend_out_ms) return false;
        if (q.lens_source != KILN_CAMERA_LENS_NONE || q.fov_y_degrees != 0.0f) return false;
    } else {
        if (q.input_policy == KILN_CAMERA_STOCK_MODE) return false;
        if (path && (q.parts & KILN_CAMERA_PROFILE)) return false;
        if (lens) {
            if (q.lens_source == KILN_CAMERA_LENS_CONSTANT) {
                if (!lens_degrees(q.fov_y_degrees)) return false;
            } else if (q.lens_source == KILN_CAMERA_LENS_KEYS) {
                if (!path || q.fov_y_degrees != 0.0f) return false;
            } else {
                return false;
            }
        } else if (q.lens_source != KILN_CAMERA_LENS_NONE || q.fov_y_degrees != 0.0f) {
            return false;
        }
        const bool keyed_lens = lens && q.lens_source == KILN_CAMERA_LENS_KEYS;
        uint32_t previous = 0;
        for (size_t i = 0; i < out->keys.size(); ++i) {
            KilnCameraKey& k = out->keys[i];
            if (k.reserved || k.reserved2) return false;
            if (i == 0 ? k.at_ms != 0 : k.at_ms <= previous) return false;
            if (k.at_ms > q.duration_ms) return false;
            previous = k.at_ms;
            if (!finite_position(k.pose.position) || !unit_quaternion(k.pose.rotation)) return false;
            if (keyed_lens ? !lens_degrees(k.fov_y_degrees) : k.fov_y_degrees != 0.0f) return false;
        }
        if (q.parts & KILN_CAMERA_PROFILE) {
            KilnCameraProfile& p = q.profile;
            if (p.size < sizeof(KilnCameraProfile)) return false;
            if (p.rotation_mode > KILN_CAMERA_FIXED_WORLD || p.collision_policy != KILN_CAMERA_COLLISION_NONE) return false;
            if (!std::isfinite(p.distance) || p.distance < 0.25f || p.distance > 200.0f) return false;
            double n2 = 0;
            for (float v : p.offset_local) {
                if (!std::isfinite(v)) return false;
                n2 += static_cast<double>(v) * v;
            }
            if (n2 > 100.0 * 100.0) return false;
            if (!std::isfinite(p.offset_world_y) || std::fabs(p.offset_world_y) > 100.0f) return false;
            if (!unit_quaternion(p.rotation)) return false;
            if (p.rotation_mode == KILN_CAMERA_FREE_ORBIT &&
                (std::fabs(p.rotation[0]) > 1e-5f || std::fabs(p.rotation[1]) > 1e-5f || std::fabs(p.rotation[2]) > 1e-5f))
                return false;  // a free orbit keeps the game's orientation: only identity (either sign of w)
        }
        if (q.parts & KILN_CAMERA_SHAKE) {
            const KilnCameraShake& s = q.shake;
            if (!std::isfinite(s.amplitude_metres) || s.amplitude_metres < 0.0f || s.amplitude_metres > 0.5f) return false;
            if (!std::isfinite(s.frequency_hz) || s.frequency_hz < 0.1f || s.frequency_hz > 30.0f) return false;
            if (s.falloff_ms > q.duration_ms) return false;
        }
    }

    // What no selected part reads is zero in the stored copy: a backend never sees the caller's leftovers.
    if (!is_mode(q.parts)) q.camera_id = q.transition_override = 0;
    if (!(q.parts & KILN_CAMERA_PROFILE)) {
        q.profile = KilnCameraProfile{};
        q.profile.rotation[3] = 1.0f;
    }
    q.profile.size = sizeof(KilnCameraProfile);
    if (!(q.parts & KILN_CAMERA_SHAKE)) q.shake = KilnCameraShake{};
    q.size = sizeof(KilnCameraRequest);
    q.key_count = static_cast<uint32_t>(out->keys.size());
    q.keys = out->keys.empty() ? nullptr : out->keys.data();
    out->req = q;
    return true;
}

uint64_t required_caps(const KilnCameraRequest& r) {
    if (is_mode(r.parts)) return r.endpoint == KILN_CAMERA_SERVER_MODE ? KILN_CAMERA_CAP_SERVER_MODE : 0;
    if (r.endpoint == KILN_CAMERA_REMOTE_VISUAL) return KILN_CAMERA_CAP_REMOTE;  // the recipient's part bits: the bridge's admit
    if (r.endpoint != KILN_CAMERA_LOCAL_VISUAL) return 0;
    uint64_t need = 0;
    if (r.parts & KILN_CAMERA_PATH) need |= KILN_CAMERA_CAP_LOCAL_POSE;
    if (r.parts & KILN_CAMERA_PROFILE) need |= KILN_CAMERA_CAP_LOCAL_PROFILE;
    if (r.parts & KILN_CAMERA_LENS) need |= KILN_CAMERA_CAP_LOCAL_FOV;
    if (r.parts & KILN_CAMERA_SHAKE) need |= KILN_CAMERA_CAP_LOCAL_SHAKE;
    return need;
}

const char* reason_text(uint32_t code) {
    static const char* const kText[] = {
        "",                                                        // NONE
        "another request holds this camera or is being applied",   // BUSY
        "the caller already holds this camera: use update",        // ALREADY_OWNED
        "no such lease",                                           // UNKNOWN_LEASE
        "the lease belongs to another owner",                      // FOREIGN_OWNER
        "the request does not fit this endpoint",                  // WRONG_ENDPOINT
        "this input policy is not supported",                      // INPUT_POLICY
        "the player's client does not run Kiln",                   // REQUIRES_KILN_CLIENT
        "the player is not connected",                             // PEER_UNAVAILABLE
        "the player's client did not finish the camera handshake", // NEGOTIATION
        "the camera's target is not there",                        // TARGET_UNAVAILABLE
        "no camera frame",                                         // NO_CAMERA_FRAME
        "the world stopped",                                       // STALE_WORLD
        "the lease has no time left",                              // EXPIRED_UPDATE
        "collision is not supported",                              // COLLISION
        "no answer from the player's client",                      // TRANSPORT_TIMEOUT
        "a malformed camera message",                              // MALFORMED_REMOTE
        "the camera resource is in use",                           // RESOURCE_BUSY
        "the frame could not be confirmed",                        // FRAME_FAULT
        "the snapshot is being written",                           // SNAPSHOT_BUSY
        "the snapshot is too old",                                 // SNAPSHOT_STALE
        "a value is out of bounds",                                // BOUNDS
        "too many camera requests",                                // RATE_LIMIT
        "a mode lease stays a mode lease and a visual lease a visual one",  // LEASE_KIND
        "cancelled",                                               // CANCELLED
        "the player's client lacks a camera capability",           // CLIENT_CAPABILITY
        "the remaining time is shorter than the blends",           // INSUFFICIENT_LIFETIME
        "the request is too large to send",                        // BODY_TOO_LARGE
    };
    return code < sizeof kText / sizeof kText[0] ? kText[code] : "refused";
}

// ---- setup, small reads ---------------------------------------------------------------------------------------------

void Core::set_backend(uint32_t endpoint, Backend* b) {
    if (endpoint >= 1 && endpoint <= 3) backends_[endpoint] = b;
}

KilnResult Core::capabilities(uint32_t endpoint, KilnEntity, uint64_t* mask) const {
    if (!mask || endpoint < KILN_CAMERA_SERVER_MODE || endpoint > KILN_CAMERA_REMOTE_VISUAL) return KILN_ERR_INVALID_ARGUMENT;
    uint64_t bits = 0;
    if (const Backend* b = backend(endpoint)) bits = b->caps();
    // every write needs the owner-retiring observer: without it a lease could outlive the code it calls
    if (!writes()) bits = 0;
    if (endpoint == KILN_CAMERA_LOCAL_VISUAL) bits |= local_read_caps_.load(std::memory_order_acquire);
    *mask = bits & ~KILN_CAMERA_CAP_RESOURCE_PROFILE;
    return KILN_OK;
}

uint32_t Core::epoch(World w) const {
    std::lock_guard<std::mutex> lock(m_);
    return w < kWorlds ? epoch_[w] : 0;
}

uint32_t Core::pending() const {
    std::lock_guard<std::mutex> lock(m_);
    return pending_;
}

uint32_t Core::live_leases() const {
    std::lock_guard<std::mutex> lock(m_);
    return static_cast<uint32_t>(leases_.size());
}

uint64_t Core::active_revision(KilnCameraLease lease) const {
    std::lock_guard<std::mutex> lock(m_);
    const auto it = leases_.find(lease);
    return it == leases_.end() ? 0 : it->second.revision;
}

uint32_t Core::slots_of(uint32_t endpoint) const {
    uint32_t n = 0;
    for (const auto& [key, slot] : slots_)
        if (static_cast<uint32_t>(key >> 32) == endpoint) ++n;
    return n;
}

// ---- queueing (lock held) -------------------------------------------------------------------------------------------

void Core::queue_done(World world, const Op& op, bool ok, uint32_t reason, uint32_t flags, uint32_t scope, uint32_t endpoint, uint32_t player, uint32_t epoch,
                      uint64_t revision, uint64_t frame, const char* text) {
    Delivery d;
    d.owner = op.owner;
    d.done = op.done;
    d.user = op.user;
    d.result.size = sizeof(KilnCameraResult);
    d.result.ok = ok ? 1 : 0;
    d.result.reason_code = reason;
    d.result.commit_scope = ok ? scope : static_cast<uint32_t>(KILN_CAMERA_COMMIT_NONE);
    d.result.flags = flags;
    d.result.endpoint = endpoint;
    d.result.player_entity = player;
    d.result.local_generation = epoch;
    d.result.lease = op.lease;
    d.result.revision = revision;
    d.result.frame = frame;
    d.text = text && *text ? text : reason_text(reason);
    queue_[world].push_back(std::move(d));
}

void Core::queue_refusal(World world, KilnOwner owner, KilnCameraDone done, void* user, KilnCameraLease lease, uint64_t revision, uint32_t reason,
                         const KilnCameraRequest& req, uint32_t epoch, const char* text) {
    Op op;
    op.owner = owner;
    op.done = done;
    op.user = user;
    op.lease = lease;
    queue_done(world, op, false, reason, 0, KILN_CAMERA_COMMIT_NONE, req.endpoint, req.player_entity, epoch, revision, 0, text);
}

void Core::queue_event(const Lease& lease, uint32_t kind, uint32_t end_reason, uint32_t reason_code, uint64_t frame, const char* text) {
    Delivery d;
    d.change = true;
    d.event.size = sizeof(KilnCameraChange);
    d.event.kind = kind;
    d.event.endpoint = lease.endpoint;
    d.event.player_entity = lease.player;
    d.event.local_generation = lease.epoch;
    d.event.end_reason = end_reason;
    d.event.reason_code = reason_code;
    d.event.lease = lease.id;
    d.event.revision = lease.revision;
    d.event.frame = frame;
    d.text = text && *text ? text : reason_text(reason_code);
    queue_[world_of(lease.endpoint)].push_back(std::move(d));
}

void Core::discard(std::unique_ptr<Op>& op) {
    if (!op) return;
    if (pending_) --pending_;
    op.reset();
}

void Core::tombstone(const Lease& lease) {
    lease.alive->store(false, std::memory_order_release);
    tombs_.push_back(Tomb{lease.id, lease.owner, lease.endpoint});
    while (tombs_.size() > kMaxTombstones) tombs_.pop_front();
}

void Core::erase_lease(KilnCameraLease id) {
    const auto it = leases_.find(id);
    if (it == leases_.end()) return;
    const uint64_t key = slot_key(it->second.endpoint, it->second.player);
    leases_.erase(it);
    const auto s = slots_.find(key);
    if (s == slots_.end()) return;
    if (s->second.holder == id) s->second.holder = 0;
    if (s->second.candidate == id) s->second.candidate = 0;
    if (!s->second.holder && !s->second.candidate) slots_.erase(s);
}

void Core::finish_holder(Lease& lease, uint32_t end_reason, uint32_t reason_code, uint64_t frame, const char* text, uint32_t pending_reason, bool silent_owner) {
    const World w = world_of(lease.endpoint);
    Slot& slot = slots_[slot_key(lease.endpoint, lease.player)];
    if (slot.inflight && slot.inflight->type == Op::Update && slot.inflight->lease == lease.id) {
        if (silent_owner) discard(slot.inflight);
        else queue_done(w, *slot.inflight, false, pending_reason, 0, KILN_CAMERA_COMMIT_NONE, lease.endpoint, lease.player, lease.epoch, lease.revision, 0, nullptr);
        slot.inflight.reset();
    }
    if (lease.end_op) {
        if (silent_owner) discard(lease.end_op);
        else queue_done(w, *lease.end_op, true, KILN_CAMERA_REASON_NONE, 0, KILN_CAMERA_COMMIT_NONE, lease.endpoint, lease.player, lease.epoch, lease.revision, frame, nullptr);
        lease.end_op.reset();
    }
    for (auto& follower : lease.end_followers) {
        if (silent_owner) discard(follower);
        else queue_done(w, *follower, true, KILN_CAMERA_REASON_NONE, KILN_CAMERA_ALREADY_ENDED, KILN_CAMERA_COMMIT_NONE, lease.endpoint, lease.player, lease.epoch,
                        lease.revision, frame, nullptr);
    }
    lease.end_followers.clear();
    queue_event(lease, KILN_CAMERA_ENDED, end_reason, reason_code, frame, text);
    tombstone(lease);
    erase_lease(lease.id);  // `lease` is gone after this line
}

// ---- lifecycle --------------------------------------------------------------------------------------------------------

void Core::stop_world(World w) {
    const uint32_t old = epoch_[w];
    epoch_[w] = 0;  // first: nothing below, and no later call, takes the world for live
    for (uint32_t e = 1; e <= 3; ++e)
        if (world_of(e) == w && backends_[e]) backends_[e]->world_stopped(old);
    std::vector<KilnCameraLease> ids;
    for (const auto& [id, lease] : leases_)
        if (world_of(lease.endpoint) == w) ids.push_back(id);
    for (const KilnCameraLease id : ids) {
        Lease& lease = leases_.at(id);
        if (lease.state == Lease::Staged) {
            Slot& slot = slots_[slot_key(lease.endpoint, lease.player)];
            if (slot.inflight && slot.inflight->lease == id) {
                queue_done(w, *slot.inflight, false, KILN_CAMERA_REASON_STALE_WORLD, 0, KILN_CAMERA_COMMIT_NONE, lease.endpoint, lease.player, lease.epoch, 0, 0, nullptr);
                slot.inflight.reset();
            }
            tombstone(lease);
            erase_lease(id);
        } else {
            finish_holder(lease, KILN_CAMERA_END_WORLD_STOPPED, KILN_CAMERA_REASON_NONE, 0, nullptr, KILN_CAMERA_REASON_STALE_WORLD, false);
        }
    }
}

void Core::world_started(World w, uint32_t epoch) {
    if (w >= kWorlds || !epoch) return;
    std::lock_guard<std::mutex> lock(m_);
    if (epoch_[w]) stop_world(w);  // a start without the stop before it: the old world's leases end here
    epoch_[w] = epoch;
}

void Core::world_stopping(World w, uint32_t epoch) {
    if (w >= kWorlds) return;
    {
        std::lock_guard<std::mutex> lock(m_);
        if (!epoch_[w] || (epoch && epoch_[w] != epoch)) return;  // not the live world's stop
        stop_world(w);
    }
    deliver(w);  // no Pre of this world comes any more: its open answers go out on this thread
}

void Core::owner_retiring(KilnOwner owner) {
    if (!owner) return;
    std::unique_lock<std::mutex> lock(m_);
    dead_.emplace(owner, Dead{});  // the gate: from here no callback into it is reserved; each world drops its leases at its next Pre
    for (auto& [id, lease] : leases_)
        if (lease.owner == owner) lease.alive->store(false, std::memory_order_release);
    // Calls into it that started before the gate closed: wait (the lock is let go while waiting) until those of other
    // threads returned. This thread's own (a retirement from inside the owner's callback) are not waited for.
    const uint32_t own = mine(owner);
    const auto drained = [&] {
        const auto it = calls_.find(owner);
        return it == calls_.end() || it->second <= own;
    };
    const uint32_t ms = retire_wait_ms_.load(std::memory_order_relaxed);
    if (drained_.wait_for(lock, std::chrono::milliseconds(ms), drained)) return;
    const uint32_t left = calls_[owner] - own;
    dead_[owner].timed_out = true;
    lock.unlock();
    env_->log(KILN_LOG_ERROR, "camera: owner " + std::to_string(owner) + " retires while " + std::to_string(left) +
                                  " camera callback(s) into it still run on other threads after " + std::to_string(ms) +
                                  " ms: returning without them; the owner stays retired and kiln.camera frees nothing those calls hold");
}

// ---- the barrier ------------------------------------------------------------------------------------------------------

namespace {
// The callbacks this thread is inside, as (core, owner): a retirement from inside one does not wait for it.
thread_local std::vector<std::pair<const void*, KilnOwner>> t_inside;
}  // namespace

bool Core::reserve(KilnOwner owner) {
    if (dead(owner)) return false;
    if (owner) ++calls_[owner];
    t_inside.emplace_back(this, owner);
    return true;
}

void Core::unreserve(KilnOwner owner) {
    for (size_t i = t_inside.size(); i-- > 0;)
        if (t_inside[i].first == this && t_inside[i].second == owner) {
            t_inside.erase(t_inside.begin() + static_cast<std::ptrdiff_t>(i));
            break;
        }
    if (!owner) return;
    {
        std::lock_guard<std::mutex> lock(m_);
        const auto it = calls_.find(owner);
        if (it != calls_.end() && --it->second == 0) calls_.erase(it);
    }
    drained_.notify_all();
}

uint32_t Core::mine(KilnOwner owner) const {
    uint32_t n = 0;
    for (const auto& [core, o] : t_inside)
        if (core == this && o == owner) ++n;
    return n;
}

uint32_t Core::calls_in(KilnOwner owner) const {
    std::lock_guard<std::mutex> lock(m_);
    const auto it = calls_.find(owner);
    return it == calls_.end() ? 0 : it->second;
}

void Core::cleanup_owner(KilnOwner owner, World w) {
    std::vector<KilnCameraLease> ids;
    for (const auto& [id, lease] : leases_)
        if (lease.owner == owner && world_of(lease.endpoint) == w) ids.push_back(id);
    for (const KilnCameraLease id : ids) {
        Lease& lease = leases_.at(id);
        Backend* b = backend(lease.endpoint);
        if (lease.state == Lease::Staged) {
            Slot& slot = slots_[slot_key(lease.endpoint, lease.player)];
            if (slot.inflight && slot.inflight->lease == id) discard(slot.inflight);
            tombstone(lease);
            erase_lease(id);
            if (b) b->drop(id);
        } else {
            // the tombstone first, then the backend lets go: nothing of this lease can complete in between
            finish_holder(lease, KILN_CAMERA_END_OWNER_RETIRED, KILN_CAMERA_REASON_NONE, 0, nullptr, KILN_CAMERA_REASON_CANCELLED, true);
            if (b) b->drop(id);
        }
    }
    for (size_t i = subs_.size(); i-- > 0;)
        if (subs_[i].owner == owner) subs_.erase(subs_.begin() + static_cast<std::ptrdiff_t>(i));
}

void Core::fault(KilnOwner owner, World w, const char* what) {
    std::lock_guard<std::mutex> lock(m_);
    if (dead(owner)) return;
    Dead d;
    d.cleaned[w] = true;
    dead_.emplace(owner, d);
    env_->log(KILN_LOG_ERROR, std::string("camera: a ") + what + " callback of owner " + std::to_string(owner) +
                                  " faulted: it gets no more callbacks and its leases end");
    cleanup_owner(owner, w);
}

void Core::pre(World w) {
    if (w >= kWorlds) return;
    {
        std::lock_guard<std::mutex> lock(m_);
        for (auto& [owner, d] : dead_)
            if (!d.cleaned[w]) {
                d.cleaned[w] = true;
                cleanup_owner(owner, w);
            }
    }
    deliver(w);
}

void Core::deliver(World w) {
    for (;;) {
        Delivery d;
        std::vector<Subscriber> subs;
        {
            std::lock_guard<std::mutex> lock(m_);
            if (queue_[w].empty()) return;
            d = std::move(queue_[w].front());
            queue_[w].pop_front();
            if (d.change) {
                subs = subs_;
            } else {
                if (pending_) --pending_;
                if (!d.done || !reserve(d.owner)) continue;  // a retired owner gets nothing
            }
        }
        // author code from here: no lock. A reserved call is released after it returned.
        if (!d.change) {
            d.result.reason = KilnStr{d.text.data(), d.text.size()};
            const bool ok = env_->call_done(d.done, d.result, d.user);
            unreserve(d.owner);
            if (!ok) fault(d.owner, w, "done");
            continue;
        }
        d.event.reason = KilnStr{d.text.data(), d.text.size()};
        for (const Subscriber& s : subs) {
            {
                std::lock_guard<std::mutex> lock(m_);
                bool here = false;
                for (const Subscriber& now : subs_)
                    if (now.handle == s.handle) here = true;
                if (!here || !reserve(s.owner)) continue;  // unsubscribed or retired since the event was taken
            }
            const bool ok = env_->call_changed(s.fn, d.event, s.user);
            unreserve(s.owner);
            if (!ok) fault(s.owner, w, "change");
        }
    }
}

// ---- the table --------------------------------------------------------------------------------------------------------

KilnResult Core::apply(KilnOwner owner, const KilnCameraRequest* request, KilnCameraLease* out_lease, KilnCameraDone done, void* user, uint32_t threads) {
    // owner 0 is no mod: it never retires and every such caller would share one identity
    if (!out_lease || !owner) return KILN_ERR_INVALID_ARGUMENT;
    if (!writes()) return KILN_ERR_UNAVAILABLE;  // no owner-retiring observer: a `done` could outlive its code
    auto stored = std::make_unique<Stored>();
    if (!validate(request, stored.get())) return KILN_ERR_INVALID_ARGUMENT;
    KilnCameraRequest& r = stored->req;
    const World w = world_of(r.endpoint);
    if (!(threads & (1u << w))) return KILN_ERR_WRONG_THREAD;
    // a mode request on a visual endpoint, or the other way round, is answered WRONG_ENDPOINT: it needs no capability
    const bool fits = is_mode(r.parts) == (r.endpoint == KILN_CAMERA_SERVER_MODE);
    Backend* b = backend(r.endpoint);

    std::lock_guard<std::mutex> lock(m_);
    if (dead(owner)) return KILN_ERR_REVOKED;
    if (fits) {
        uint64_t have = 0;
        capabilities(r.endpoint, r.player_entity, &have);
        const uint64_t need = required_caps(r);
        if (!b || (have & need) != need) return KILN_ERR_UNAVAILABLE;
    }
    if (!epoch_[w]) return KILN_ERR_NOT_FOUND;
    if (pending_ >= KILN_CAMERA_MAX_PENDING) return KILN_ERR_BUFFER_TOO_SMALL;

    uint32_t reason = KILN_CAMERA_REASON_NONE;
    const char* text = nullptr;
    if (!fits) reason = KILN_CAMERA_REASON_WRONG_ENDPOINT;
    else if (r.input_policy == KILN_CAMERA_AIM_TOO) reason = KILN_CAMERA_REASON_INPUT_POLICY;
    else if (const KilnResult rc = b->admit(*stored, &reason, &text); rc != KILN_OK) return rc;

    const uint64_t key = slot_key(r.endpoint, r.player_entity);
    KilnCameraLease replaces = 0;
    if (!reason) {
        const auto it = slots_.find(key);
        if (it == slots_.end()) {
            if (r.endpoint != KILN_CAMERA_LOCAL_VISUAL && slots_of(r.endpoint) >= kMaxPlayerSlots) return KILN_ERR_BUFFER_TOO_SMALL;
        } else {
            const Slot& slot = it->second;
            const Lease* holder = slot.holder ? &leases_.at(slot.holder) : nullptr;
            const Lease* candidate = slot.candidate ? &leases_.at(slot.candidate) : nullptr;
            if ((holder && holder->owner == owner && holder->state != Lease::Ending) || (candidate && candidate->owner == owner))
                reason = KILN_CAMERA_REASON_ALREADY_OWNED;
            else if (slot.inflight || candidate) reason = KILN_CAMERA_REASON_BUSY;  // one apply or update in flight per endpoint
            else if (holder && r.priority <= holder->priority) reason = KILN_CAMERA_REASON_BUSY;
            else if (holder) replaces = holder->id;
        }
    }

    Lease lease;
    lease.id = next_lease_++;
    lease.owner = owner;
    lease.endpoint = r.endpoint;
    lease.player = r.player_entity;
    lease.epoch = epoch_[w];
    lease.priority = r.priority;
    const uint64_t revision = lease.next_revision++;
    if (!reason) {
        Staged st;
        st.lease = lease.id;
        st.revision = revision;
        st.owner = owner;
        st.epoch = lease.epoch;
        st.replaces = replaces;
        st.alive = lease.alive;
        st.request = stored.get();
        if (!b->stage(st, &reason, &text) && !reason) reason = KILN_CAMERA_REASON_BUSY;
    }
    *out_lease = lease.id;
    ++pending_;
    if (reason) {
        // refused before it was ever a lease: end() of it by its owner is answered ALREADY_ENDED, and there is no event
        tombstone(lease);
        queue_refusal(w, owner, done, user, lease.id, 0, reason, r, lease.epoch, text);
        return KILN_OK;
    }
    auto op = std::make_unique<Op>();
    op->type = Op::Apply;
    op->owner = owner;
    op->done = done;
    op->user = user;
    op->lease = lease.id;
    op->revision = revision;
    op->request = std::move(stored);
    Slot& slot = slots_[key];
    slot.candidate = lease.id;
    slot.inflight = std::move(op);
    leases_.emplace(lease.id, std::move(lease));
    return KILN_OK;
}

KilnResult Core::update(KilnOwner owner, KilnCameraLease id, uint32_t timing, const KilnCameraRequest* request, KilnCameraDone done, void* user, uint32_t threads) {
    if (!id || !owner || timing > KILN_CAMERA_RESTART_TIMELINE) return KILN_ERR_INVALID_ARGUMENT;
    if (!writes()) return KILN_ERR_UNAVAILABLE;
    auto stored = std::make_unique<Stored>();
    if (!validate(request, stored.get())) return KILN_ERR_INVALID_ARGUMENT;
    KilnCameraRequest& r = stored->req;

    std::lock_guard<std::mutex> lock(m_);
    const auto found = leases_.find(id);
    Lease* lease = found == leases_.end() ? nullptr : &found->second;
    const World w = world_of(lease ? lease->endpoint : r.endpoint);
    if (!(threads & (1u << w))) return KILN_ERR_WRONG_THREAD;
    if (dead(owner)) return KILN_ERR_REVOKED;

    uint32_t reason = KILN_CAMERA_REASON_NONE;
    const char* text = nullptr;
    if (!lease) reason = KILN_CAMERA_REASON_UNKNOWN_LEASE;
    else if (lease->owner != owner) reason = KILN_CAMERA_REASON_FOREIGN_OWNER;
    else if (r.endpoint != lease->endpoint || (r.endpoint == KILN_CAMERA_LOCAL_VISUAL ? r.player_entity != 0 && r.player_entity != lease->player : r.player_entity != lease->player))
        reason = KILN_CAMERA_REASON_WRONG_ENDPOINT;
    else if (is_mode(r.parts) != (lease->endpoint == KILN_CAMERA_SERVER_MODE)) reason = KILN_CAMERA_REASON_LEASE_KIND;

    Backend* b = lease ? backend(lease->endpoint) : nullptr;
    if (!reason) {
        // the rows of the new, whole request; a missing one leaves the old revision as it is
        uint64_t have = 0;
        capabilities(r.endpoint, lease->player, &have);
        const uint64_t need = required_caps(r);
        if (!b || (have & need) != need) return KILN_ERR_UNAVAILABLE;
    }
    if (!epoch_[w]) return KILN_ERR_NOT_FOUND;
    if (pending_ >= KILN_CAMERA_MAX_PENDING) return KILN_ERR_BUFFER_TOO_SMALL;

    uint64_t revision = 0;
    if (!reason) {
        r.player_entity = lease->player;
        Slot& slot = slots_[slot_key(lease->endpoint, lease->player)];
        if (r.input_policy == KILN_CAMERA_AIM_TOO) reason = KILN_CAMERA_REASON_INPUT_POLICY;
        else if (lease->state == Lease::Ending) reason = KILN_CAMERA_REASON_UNKNOWN_LEASE, text = "the lease is ending";
        else if (lease->state == Lease::Staged || slot.inflight) reason = KILN_CAMERA_REASON_BUSY;
        else if (const KilnResult rc = b->admit(*stored, &reason, &text); rc != KILN_OK) return rc;
        if (!reason) {
            revision = lease->next_revision++;
            Staged st;
            st.lease = id;
            st.revision = revision;
            st.owner = owner;
            st.epoch = lease->epoch;
            st.update = true;
            st.timing = timing;
            st.alive = lease->alive;
            st.request = stored.get();
            if (!b->stage(st, &reason, &text) && !reason) reason = KILN_CAMERA_REASON_BUSY;
        }
        if (!reason) {
            auto op = std::make_unique<Op>();
            op->type = Op::Update;
            op->owner = owner;
            op->done = done;
            op->user = user;
            op->lease = id;
            op->revision = revision;
            op->request = std::move(stored);
            slot.inflight = std::move(op);
            ++pending_;
            return KILN_OK;
        }
    }
    ++pending_;
    const bool mine = lease && lease->owner == owner;
    queue_refusal(w, owner, done, user, id, mine ? lease->revision : 0, reason, r, mine ? lease->epoch : epoch_[w], text);
    return KILN_OK;
}

KilnResult Core::end(KilnOwner owner, KilnCameraLease id, KilnCameraDone done, void* user, uint32_t threads) {
    if (!id || !owner) return KILN_ERR_INVALID_ARGUMENT;
    if (!writes()) return KILN_ERR_UNAVAILABLE;  // and so no lease was ever made
    std::lock_guard<std::mutex> lock(m_);
    const auto found = leases_.find(id);
    Lease* lease = found == leases_.end() ? nullptr : &found->second;
    const Tomb* tomb = nullptr;
    if (!lease)
        for (const Tomb& t : tombs_)
            if (t.id == id) tomb = &t;
    World w;
    if (lease) w = world_of(lease->endpoint);
    else if (tomb) w = world_of(tomb->endpoint);
    else if (threads & kOnServer) w = kServer;
    else w = kClient;
    if (!(threads & (1u << w))) return KILN_ERR_WRONG_THREAD;
    if (dead(owner)) return KILN_ERR_REVOKED;
    if (!epoch_[w]) return KILN_ERR_NOT_FOUND;  // no Pre of that world would bring the answer; its leases ended with it
    if (pending_ >= kMaxPendingWithEnds) return KILN_ERR_BUFFER_TOO_SMALL;
    ++pending_;

    auto op = std::make_unique<Op>();
    op->type = Op::End;
    op->owner = owner;
    op->done = done;
    op->user = user;
    op->lease = id;

    if (!lease) {
        const uint32_t endpoint = tomb ? tomb->endpoint : 0;
        if (tomb && tomb->owner == owner)
            queue_done(w, *op, true, KILN_CAMERA_REASON_NONE, KILN_CAMERA_ALREADY_ENDED, KILN_CAMERA_COMMIT_NONE, endpoint, 0, epoch_[w], 0, 0, nullptr);
        else
            queue_done(w, *op, false, tomb ? KILN_CAMERA_REASON_FOREIGN_OWNER : KILN_CAMERA_REASON_UNKNOWN_LEASE, 0, KILN_CAMERA_COMMIT_NONE, endpoint, 0, epoch_[w], 0, 0,
                       nullptr);
        return KILN_OK;
    }
    if (lease->owner != owner) {
        queue_done(w, *op, false, KILN_CAMERA_REASON_FOREIGN_OWNER, 0, KILN_CAMERA_COMMIT_NONE, lease->endpoint, lease->player, lease->epoch, 0, 0, nullptr);
        return KILN_OK;
    }
    Backend* b = backend(lease->endpoint);
    Slot& slot = slots_[slot_key(lease->endpoint, lease->player)];
    if (lease->state == Lease::Staged) {
        // ended before its apply: that apply is answered CANCELLED, the lease never shows
        if (slot.inflight && slot.inflight->lease == id) {
            queue_done(w, *slot.inflight, false, KILN_CAMERA_REASON_CANCELLED, 0, KILN_CAMERA_COMMIT_NONE, lease->endpoint, lease->player, lease->epoch, 0, 0, nullptr);
            slot.inflight.reset();
        }
        queue_done(w, *op, true, KILN_CAMERA_REASON_NONE, 0, KILN_CAMERA_COMMIT_NONE, lease->endpoint, lease->player, lease->epoch, 0, 0, nullptr);
        queue_event(*lease, KILN_CAMERA_ENDED, KILN_CAMERA_END_CANCELLED_BEFORE_APPLY, KILN_CAMERA_REASON_NONE, 0, nullptr);
        tombstone(*lease);
        erase_lease(id);  // the tombstone stands before the backend lets go
        if (b) b->drop(id);
        return KILN_OK;
    }
    if (lease->state == Lease::Ending) {
        lease->end_followers.push_back(std::move(op));  // answered ALREADY_ENDED after the first end()
        return KILN_OK;
    }
    // Active. A waiting update of this lease is cancelled first; another owner's candidate for the slot stays.
    if (slot.inflight && slot.inflight->type == Op::Update && slot.inflight->lease == id) {
        const uint64_t staged = slot.inflight->revision;
        queue_done(w, *slot.inflight, false, KILN_CAMERA_REASON_CANCELLED, 0, KILN_CAMERA_COMMIT_NONE, lease->endpoint, lease->player, lease->epoch, lease->revision, 0,
                   nullptr);
        slot.inflight.reset();
        if (b) b->cancel_update(id, staged);
    }
    // The lease is marked ending before the backend is asked to let go: a completion of it that arrives from now on
    // (committed, rejected, a stale ended) finds no revision to act on.
    lease->state = Lease::Ending;
    lease->end_op = std::move(op);
    const uint64_t revision = lease->revision;
    if (!b || b->release(id, revision)) finish_holder(*lease, KILN_CAMERA_END_RELEASED, KILN_CAMERA_REASON_NONE, 0, nullptr, KILN_CAMERA_REASON_CANCELLED, false);
    return KILN_OK;
}

KilnResult Core::subscribe(KilnOwner owner, KilnCameraChanged fn, void* user, KilnHandle* out) {
    if (!fn || !out) return KILN_ERR_INVALID_ARGUMENT;
    if (!writes()) return KILN_ERR_UNAVAILABLE;  // a subscriber is a callback into an owner, as a `done` is
    std::lock_guard<std::mutex> lock(m_);
    if (dead(owner)) return KILN_ERR_REVOKED;
    if (subs_.size() >= kMaxSubscribers) return KILN_ERR_BUFFER_TOO_SMALL;
    const KilnHandle h = next_handle_++;
    subs_.push_back(Subscriber{h, owner, fn, user});
    *out = h;
    return KILN_OK;
}

KilnResult Core::unsubscribe(KilnHandle handle) {
    std::lock_guard<std::mutex> lock(m_);
    for (size_t i = 0; i < subs_.size(); ++i)
        if (subs_[i].handle == handle) {
            subs_.erase(subs_.begin() + static_cast<std::ptrdiff_t>(i));  // no call of it starts after this; a running one is not waited for
            return KILN_OK;
        }
    return KILN_ERR_NOT_FOUND;
}

// ---- from a backend ---------------------------------------------------------------------------------------------------

bool Core::committed(KilnCameraLease id, uint64_t revision, uint64_t frame) {
    std::lock_guard<std::mutex> lock(m_);
    const auto found = leases_.find(id);
    if (found == leases_.end()) return false;
    Lease& lease = found->second;
    const World w = world_of(lease.endpoint);
    Slot& slot = slots_[slot_key(lease.endpoint, lease.player)];
    if (!slot.inflight || slot.inflight->lease != id || slot.inflight->revision != revision) return false;  // not the revision that waits
    std::unique_ptr<Op> op = std::move(slot.inflight);
    if (op->type == Op::Apply) {
        if (slot.holder && slot.holder != id) {
            // the old lease's ENDED goes out before the new one's answer and APPLIED
            Lease& old = leases_.at(slot.holder);
            finish_holder(old, old.state == Lease::Ending ? KILN_CAMERA_END_RELEASED : KILN_CAMERA_END_SUPERSEDED, KILN_CAMERA_REASON_NONE, frame, nullptr,
                          KILN_CAMERA_REASON_CANCELLED, false);
        }
        Slot& now = slots_[slot_key(lease.endpoint, lease.player)];
        now.holder = id;
        now.candidate = 0;
        lease.state = Lease::Active;
    }
    lease.revision = revision;
    lease.priority = op->request ? op->request->req.priority : lease.priority;
    queue_done(w, *op, true, KILN_CAMERA_REASON_NONE, 0, scope_of(lease.endpoint), lease.endpoint, lease.player, lease.epoch, revision, frame, nullptr);
    queue_event(lease, op->type == Op::Apply ? KILN_CAMERA_APPLIED : KILN_CAMERA_UPDATED, KILN_CAMERA_END_NONE, KILN_CAMERA_REASON_NONE, frame, nullptr);
    return true;
}

bool Core::rejected(KilnCameraLease id, uint64_t revision, uint32_t reason_code, uint32_t flags, const char* text) {
    std::lock_guard<std::mutex> lock(m_);
    const auto found = leases_.find(id);
    if (found == leases_.end()) return false;
    Lease& lease = found->second;
    const World w = world_of(lease.endpoint);
    Slot& slot = slots_[slot_key(lease.endpoint, lease.player)];
    if (!slot.inflight || slot.inflight->lease != id || slot.inflight->revision != revision) return false;
    std::unique_ptr<Op> op = std::move(slot.inflight);
    if (!reason_code) reason_code = KILN_CAMERA_REASON_BUSY;
    flags &= KILN_CAMERA_MAY_HAVE_APPLIED;
    queue_done(w, *op, false, reason_code, flags, KILN_CAMERA_COMMIT_NONE, lease.endpoint, lease.player, lease.epoch, lease.revision, 0, text);
    if (op->type == Op::Apply) {
        tombstone(lease);
        erase_lease(id);
    }
    return true;
}

bool Core::released(KilnCameraLease id, uint64_t frame) {
    std::lock_guard<std::mutex> lock(m_);
    const auto found = leases_.find(id);
    if (found == leases_.end() || found->second.state != Lease::Ending) return false;
    finish_holder(found->second, KILN_CAMERA_END_RELEASED, KILN_CAMERA_REASON_NONE, frame, nullptr, KILN_CAMERA_REASON_CANCELLED, false);
    return true;
}

bool Core::ended(KilnCameraLease id, uint64_t revision, uint32_t end_reason, uint32_t reason_code, uint64_t frame, const char* text) {
    if (end_reason == KILN_CAMERA_END_NONE || end_reason > KILN_CAMERA_END_CANCELLED_BEFORE_APPLY) return false;
    std::lock_guard<std::mutex> lock(m_);
    const auto found = leases_.find(id);
    if (found == leases_.end()) return false;
    Lease& lease = found->second;
    // only the applied revision can end by itself: a staged one is rejected(), an older one is history
    if (lease.state == Lease::Staged || lease.revision != revision) return false;
    finish_holder(lease, end_reason, reason_code, frame, text,
                  end_reason == KILN_CAMERA_END_EXPIRED ? KILN_CAMERA_REASON_EXPIRED_UPDATE : KILN_CAMERA_REASON_CANCELLED, false);
    return true;
}

}  // namespace kcam
