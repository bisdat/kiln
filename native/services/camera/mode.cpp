// SPDX-License-Identifier: MIT
// kiln.camera's SERVER_MODE backend: mode.h says what it is and which thread may do what.
#include "mode.h"

#include <cstring>
#include <limits>

namespace kcam {

namespace {

bool good(uint32_t status) { return status == KILN_WORLD_WRITE_DONE || status == KILN_WORLD_WRITE_UNCHANGED; }

std::string text_of(const KilnWorldWriteResult& r, const char* fallback) {
    if (r.reason.ptr && r.reason.len) return std::string(r.reason.ptr, r.reason.len);
    return fallback;
}

}  // namespace

ModeBackend::ModeBackend(Core* core, ModeWorld* world, uint32_t component, std::string who)
    : core_(core), world_(world), component_(component), who_(std::move(who)) {}

uint64_t ModeBackend::caps() const {
    return ready_.load(std::memory_order_acquire) && !broken_.load(std::memory_order_acquire) ? KILN_CAMERA_CAP_SERVER_MODE : 0;
}

// ---- Backend (the core's lock is held; the server world's thread) -------------------------------------------------------

KilnResult ModeBackend::admit(Stored& request, uint32_t*, const char**) {
    if (!caps()) return KILN_ERR_UNAVAILABLE;
    Incarnation who;
    // not a player of the running server world: no Pre would ever bring an answer (managed: PeerUnavailable)
    if (!world_->player(request.req.player_entity, &who)) return KILN_ERR_NOT_FOUND;
    return KILN_OK;
}

bool ModeBackend::stage(const Staged& st, uint32_t* reason, const char** text) {
    if (!st.request) return *reason = KILN_CAMERA_REASON_BUSY, false;
    const KilnCameraRequest& q = st.request->req;
    std::lock_guard<std::mutex> lock(m_);
    Incarnation who;
    if (!world_->player(q.player_entity, &who)) return *reason = KILN_CAMERA_REASON_PEER_UNAVAILABLE, false;
    Slot* s = find(q.player_entity);
    if (s && s->who != who) {
        *reason = KILN_CAMERA_REASON_PEER_UNAVAILABLE;
        *text = "the player joined again: the old player's lease ends first";
        return false;
    }
    if (!s) {
        for (Slot& f : slots_)
            if (!f.player) {
                s = &f;
                break;
            }
        if (!s) return *reason = KILN_CAMERA_REASON_BUSY, false;
        s->player = q.player_entity;
        s->who = who;
        s->drops = 0;
    }
    const uint64_t now = world_->now_ms();
    Rev r;
    r.lease = st.lease;
    r.revision = st.revision;
    r.owner = st.owner;
    r.epoch = st.epoch;
    r.camera_id = static_cast<uint8_t>(q.camera_id);
    r.transition = static_cast<uint8_t>(q.transition_override);
    if (st.update) {
        if (s->active.lease != st.lease) {
            free_if_idle(*s);
            return *reason = KILN_CAMERA_REASON_UNKNOWN_LEASE, false;
        }
        r.start_ms = st.timing == KILN_CAMERA_RESTART_TIMELINE ? now : s->active.start_ms;
    } else {
        r.start_ms = now;
    }
    r.duration_ms = q.duration_ms;
    r.alive = st.alive;
    r.start_on_commit = !st.update || st.timing == KILN_CAMERA_RESTART_TIMELINE;
    r.deadline_ms = r.start_on_commit ? std::numeric_limits<uint64_t>::max() : r.start_ms + q.duration_ms;
    if (r.deadline_ms <= now) {
        free_if_idle(*s);
        *text = "the lease's clock goes on (Preserve) and the new duration is already over";
        return *reason = KILN_CAMERA_REASON_EXPIRED_UPDATE, false;
    }
    s->staged = r;
    return true;
}

void ModeBackend::cancel_update(KilnCameraLease lease, uint64_t revision) {
    std::lock_guard<std::mutex> lock(m_);
    for (Slot& s : slots_) {
        if (!s.player || s.staged.lease != lease || s.staged.revision != revision) continue;
        if (s.ticket && s.ticket->lease == lease && s.ticket->revision == revision) cut(s);
        s.staged = Rev{};
        submit(s);  // the active revision goes on without a step's gap
    }
}

bool ModeBackend::release(KilnCameraLease lease, uint64_t) {
    std::lock_guard<std::mutex> lock(m_);
    for (Slot& s : slots_) {
        if (!s.player || s.active.lease != lease) continue;
        if (s.ticket && s.ticket->lease == lease) cut(s);  // guard first, then write_cancel
        if (s.staged.lease == lease) s.staged = Rev{};
        s.active = Rev{};
        submit(s);  // another owner's candidate, if one waits
        free_if_idle(s);
    }
    // Nothing to blend out: the game's own writer puts the stock bytes back in the next step.
    return true;
}

void ModeBackend::drop(KilnCameraLease lease) {
    std::lock_guard<std::mutex> lock(m_);
    for (Slot& s : slots_) {
        if (!s.player || (s.active.lease != lease && s.staged.lease != lease)) continue;
        if (s.ticket && s.ticket->lease == lease) cut(s);
        if (s.staged.lease == lease) s.staged = Rev{};
        if (s.active.lease == lease) s.active = Rev{};
        submit(s);
        free_if_idle(s);
    }
}

void ModeBackend::world_stopped(uint32_t) {
    live_epoch_.store(0, std::memory_order_release);  // no change callback writes from here
    stopped_.store(true, std::memory_order_release);
}

// ---- the server Pre -----------------------------------------------------------------------------------------------------

void ModeBackend::step() {
    std::vector<Report> reports;
    const uint32_t epoch = core_->epoch(kServer);
    {
        std::lock_guard<std::mutex> lock(m_);
        const uint64_t now = world_->now_ms();
        now_ms_.store(now, std::memory_order_release);
        stopped_.store(false, std::memory_order_release);
        live_epoch_.store(epoch, std::memory_order_release);
        for (Slot& s : slots_) {
            if (!s.player) continue;
            // Another world's work is void: the core ended its leases (WorldStopped); cancel and forget, report nothing.
            if (s.ticket && s.ticket->epoch != epoch) cut(s);
            if (s.active.lease && s.active.epoch != epoch) s.active = Rev{};
            if (s.staged.lease && s.staged.epoch != epoch) s.staged = Rev{};
            if (!s.active.lease && !s.staged.lease) {
                free_if_idle(s);
                continue;
            }
            Incarnation who;
            if (!world_->player(s.player, &who) || who != s.who) {
                cut(s);
                if (s.staged.lease && s.staged.lease != s.active.lease)
                    reports.push_back({Report::Rejected, s.staged.lease, s.staged.revision, KILN_CAMERA_REASON_PEER_UNAVAILABLE, 0, 0, "the player left"});
                if (s.active.lease)
                    reports.push_back({Report::Ended, s.active.lease, s.active.revision, KILN_CAMERA_END_PLAYER_LEFT, KILN_CAMERA_REASON_PEER_UNAVAILABLE,
                                       world_->step(), "the player left"});
                s.active = s.staged = Rev{};
                free_if_idle(s);
                continue;
            }
            if (s.active.lease && now >= s.active.deadline_ms) {
                const KilnCameraLease id = s.active.lease;
                if (s.ticket && s.ticket->lease == id) cut(s);
                reports.push_back({Report::Ended, id, s.active.revision, KILN_CAMERA_END_EXPIRED, KILN_CAMERA_REASON_NONE, world_->step(), ""});
                if (s.staged.lease == id) s.staged = Rev{};  // the core answers its update EXPIRED_UPDATE
                s.active = Rev{};
            }
            if (s.staged.lease && now >= s.staged.deadline_ms) {
                if (s.ticket && s.ticket->lease == s.staged.lease && s.ticket->revision == s.staged.revision) cut(s);
                reports.push_back({Report::Rejected, s.staged.lease, s.staged.revision, KILN_CAMERA_REASON_EXPIRED_UPDATE, 0, 0, "the request's time ran out before it was written"});
                s.staged = Rev{};
            }
            submit(s);
            free_if_idle(s);
        }
    }
    flush(reports);
}

// ---- the write ----------------------------------------------------------------------------------------------------------

// In the write program, after the game's writer: the tick thread or a game worker. The ticket and atomics only.
bool KILN_CALL ModeBackend::on_change(KilnEntity, void* data, uint32_t size, void* user) {
    auto* t = static_cast<Ticket*>(user);
    ModeBackend* self = t->self;
    const Slot& s = self->slots_[t->slot];
    if (!data || size != kNccSize || s.cur_lease.load(std::memory_order_acquire) != t->lease ||
        s.cur_revision.load(std::memory_order_acquire) != t->revision || self->live_epoch_.load(std::memory_order_acquire) != t->epoch ||
        (t->alive && !t->alive->load(std::memory_order_acquire)) || self->world_->now_ms() >= t->deadline_ms) {
        self->refusals_.fetch_add(1, std::memory_order_relaxed);
        return false;  // ended, replaced, expired or another world: the component stays as the game left it
    }
    t->wrote_ms = self->world_->now_ms();
    std::memcpy(data, t->bytes, kNccSize);
    self->writes_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

// kiln.world's Pre, on the tick thread.
void KILN_CALL ModeBackend::on_result(const KilnWorldWriteResult* result, void* user) {
    auto* t = static_cast<Ticket*>(user);
    if (result) t->self->result(t, *result);
}

void ModeBackend::result(Ticket* t, const KilnWorldWriteResult& r) {
    std::vector<Report> reports;
    {
        std::lock_guard<std::mutex> lock(m_);
        results_[r.status < 8 ? r.status : 7].fetch_add(1, std::memory_order_relaxed);
        Slot& s = slots_[t->slot];
        if (s.ticket != t) {  // not the slot's any more (a cancel comes first, so this does not happen)
            delete t;
            return;
        }
        s.ticket = nullptr;
        if (t->alive && !t->alive->load(std::memory_order_acquire)) {
            delete t;  // retirement already invalidated it; Core::pre owns cancellation and its reason
            return;
        }
        const uint64_t now = world_->now_ms();
        now_ms_.store(now, std::memory_order_release);
        const bool staged = s.staged.lease == t->lease && s.staged.revision == t->revision;
        const bool active = !staged && s.active.lease == t->lease && s.active.revision == t->revision;
        const uint64_t frame = world_->step();
        uint32_t status = r.status;
        // the guard said no to a write that is still current and in time: nothing outside could have changed it
        if (status == KILN_WORLD_WRITE_REFUSED && (staged || active) && now < t->deadline_ms && t->epoch == live_epoch_.load()) status = KILN_WORLD_WRITE_FAULTED;
        bool again = true;
        if (good(status)) {
            s.drops = 0;
            if (staged) {
                // the first write of this revision is in the server state: Applied/Updated (an old holder ends first)
                reports.push_back({Report::Committed, s.staged.lease, s.staged.revision, 0, 0, frame, ""});
                if (s.staged.start_on_commit) {
                    s.staged.start_ms = t->wrote_ms;
                    s.staged.deadline_ms = t->wrote_ms + s.staged.duration_ms;
                    s.staged.start_on_commit = false;
                }
                s.active = s.staged;
                s.staged = Rev{};
            }
        } else if (status == KILN_WORLD_WRITE_REFUSED) {
            // a stale ticket (ended, replaced, expired): nothing to report
        } else if (status == KILN_WORLD_WRITE_NO_COMPONENT) {
            if (staged) {
                reports.push_back({Report::Rejected, s.staged.lease, s.staged.revision, KILN_CAMERA_REASON_PEER_UNAVAILABLE, 0, 0, text_of(r, "the player is gone")});
                s.staged = Rev{};
            } else if (active) {
                reports.push_back({Report::Ended, s.active.lease, s.active.revision, KILN_CAMERA_END_PLAYER_LEFT, KILN_CAMERA_REASON_PEER_UNAVAILABLE, frame,
                                   text_of(r, "the player is gone")});
                if (s.staged.lease == s.active.lease) s.staged = Rev{};
                s.active = Rev{};
            }
        } else if (status == KILN_WORLD_WRITE_DROPPED && ++s.drops < kMaxDrops) {
            again = false;  // no write program ran it (between worlds, or behind a full update): the next Pre tries again
        } else if (staged || active) {
            // a write that cannot work: not declared, does not fit, faulted, or never runs
            if (status == KILN_WORLD_WRITE_NOT_DECLARED || status == KILN_WORLD_WRITE_OUT_OF_RANGE) broken_.store(true, std::memory_order_release);
            const std::string why = text_of(r, "the tracked write failed");
            world_->log(KILN_LOG_ERROR, "camera: mode write of player " + std::to_string(s.player) + " failed (status " + std::to_string(r.status) + "): " + why);
            if (staged) {
                reports.push_back({Report::Rejected, s.staged.lease, s.staged.revision,
                                   status == KILN_WORLD_WRITE_DROPPED ? static_cast<uint32_t>(KILN_CAMERA_REASON_STALE_WORLD) : static_cast<uint32_t>(KILN_CAMERA_REASON_FRAME_FAULT),
                                   0, 0, why});
                s.staged = Rev{};
            } else {
                reports.push_back({Report::Ended, s.active.lease, s.active.revision, KILN_CAMERA_END_CAPABILITY_LOST, KILN_CAMERA_REASON_FRAME_FAULT, frame, why});
                if (s.staged.lease == s.active.lease) s.staged = Rev{};
                s.active = Rev{};
            }
            s.drops = 0;
        }
        delete t;
        // The next step's write, now (the game's writer puts the stock bytes back every step it runs).
        if (again) submit(s);
        free_if_idle(s);
    }
    flush(reports);
}

// ---- helpers (the lock is held) -----------------------------------------------------------------------------------------

ModeBackend::Slot* ModeBackend::find(KilnEntity player) {
    for (Slot& s : slots_)
        if (s.player && s.player == player) return &s;
    return nullptr;
}

const ModeBackend::Rev* ModeBackend::next(const Slot& s) const {
    if (s.staged.lease) return &s.staged;
    if (s.active.lease) return &s.active;
    return nullptr;
}

void ModeBackend::set_guard(Slot& s, const Rev* r) {
    s.cur_lease.store(r ? r->lease : 0, std::memory_order_release);
    s.cur_revision.store(r ? r->revision : 0, std::memory_order_release);
}

void ModeBackend::cut(Slot& s) {
    set_guard(s, nullptr);  // the tombstone: from here the ticket's change callback refuses
    if (!s.ticket) return;
    Ticket* t = s.ticket;
    s.ticket = nullptr;
    world_->cancel(t->id);  // not run after this returns; its result is not delivered (kiln_world.h)
    cancels_.fetch_add(1, std::memory_order_relaxed);
    delete t;
}

void ModeBackend::submit(Slot& s) {
    const Rev* r = next(s);
    if (!r || s.ticket || r->epoch != live_epoch_.load(std::memory_order_acquire) || !r->epoch) return;
    auto* t = new Ticket;
    t->self = this;
    t->slot = index(&s);
    t->player = s.player;
    t->lease = r->lease;
    t->revision = r->revision;
    t->epoch = r->epoch;
    t->deadline_ms = r->deadline_ms;
    t->alive = r->alive;
    t->bytes[0] = 1;
    t->bytes[1] = r->camera_id;
    t->bytes[2] = r->transition;
    set_guard(s, r);
    KilnWorldWriteRequest q{};
    q.size = sizeof q;
    q.entity = s.player;
    q.component = component_;
    q.change = &on_change;
    q.callback = &on_result;
    q.user = t;
    q.mod_id = KilnStr{who_.data(), who_.size()};
    uint64_t id = 0;
    const KilnResult rc = world_->submit(q, &id);
    if (rc != KILN_OK || !id) {
        delete t;  // the next Pre tries again (a full queue, or not between two steps)
        return;
    }
    t->id = id;
    s.ticket = t;
    submits_.fetch_add(1, std::memory_order_relaxed);
}

void ModeBackend::free_if_idle(Slot& s) {
    if (s.active.lease || s.staged.lease || s.ticket) return;
    set_guard(s, nullptr);
    s.player = 0;
    s.who = Incarnation{};
    s.drops = 0;
}

void ModeBackend::flush(std::vector<Report>& reports) {
    for (const Report& r : reports) {
        const char* text = r.text.empty() ? nullptr : r.text.c_str();
        switch (r.kind) {
        case Report::Committed: core_->committed(r.lease, r.revision, r.frame); break;
        case Report::Rejected: core_->rejected(r.lease, r.revision, r.code, 0, text); break;
        case Report::Ended: core_->ended(r.lease, r.revision, r.code, r.reason, r.frame, text); break;
        }
    }
}

// ---- tests and the log --------------------------------------------------------------------------------------------------

ModeBackend::Guard ModeBackend::guard(KilnEntity player) const {
    std::lock_guard<std::mutex> lock(m_);
    for (const Slot& s : slots_)
        if (s.player && s.player == player) return Guard{s.cur_lease.load(), s.cur_revision.load()};
    return Guard{};
}

bool ModeBackend::ticket_current(const void* user) {
    const auto* t = static_cast<const Ticket*>(user);
    const Slot& s = t->self->slots_[t->slot];
    return s.cur_lease.load(std::memory_order_acquire) == t->lease && s.cur_revision.load(std::memory_order_acquire) == t->revision;
}

ModeBackend::Counters ModeBackend::counters() const {
    Counters c;
    c.submits = submits_.load();
    c.cancels = cancels_.load();
    c.writes = writes_.load();
    c.refusals = refusals_.load();
    for (int i = 0; i < 8; ++i) c.results[i] = results_[i].load();
    return c;
}

uint32_t ModeBackend::tickets() const {
    std::lock_guard<std::mutex> lock(m_);
    uint32_t n = 0;
    for (const Slot& s : slots_)
        if (s.ticket) ++n;
    return n;
}

}  // namespace kcam
