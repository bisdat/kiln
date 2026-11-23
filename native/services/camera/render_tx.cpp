// SPDX-License-Identifier: MIT
// kiln.camera's LOCAL_VISUAL backend: render_tx.h says what it is and which thread may do what.
#include "render_tx.h"

#include <cmath>

namespace kcam {

namespace {

bool lens_ok(double radians) { return std::isfinite(radians) && radians >= 0.01 && radians <= kPi - 0.01; }

bool scale_ok(const float s[3]) {
    for (int i = 0; i < 3; ++i)
        if (!std::isfinite(s[i]) || std::fabs(s[i] - 1.0f) > 0.0001f) return false;
    return true;
}

constexpr uint64_t kLocalBits = KILN_CAMERA_CAP_LOCAL_POSE | KILN_CAMERA_CAP_LOCAL_FOV | KILN_CAMERA_CAP_LOCAL_PROFILE | KILN_CAMERA_CAP_LOCAL_SHAKE;
constexpr uint32_t kGraveyardMax = 64;

// The local player's sample when it is fresh and of this world, else null.
const Sample* fresh_local(const SampleSet& samples, uint32_t player, uint32_t epoch, int64_t now) {
    const Sample& s = samples.s[0];
    if (s.state != kSamplePresent || !player || s.entity != player || s.epoch != epoch || now - s.ns > kSampleValidNs || now < s.ns) return nullptr;
    return &s;
}

}  // namespace

// ---- Backend (the core's lock is held; the client world's thread) -------------------------------------------------------

uint64_t LocalBackend::caps() const {
    if (write_fault_.load(std::memory_order_acquire)) return 0;
    return serving_.load(std::memory_order_acquire) & kLocalBits;
}

KilnResult LocalBackend::admit(Stored& request, uint32_t* reason, const char** text) {
    if (!caps()) return KILN_ERR_UNAVAILABLE;
    const uint32_t me = world_->local_player();
    if (!me) return KILN_ERR_NOT_FOUND;  // no local player: no frame will ever show it (managed: NoCameraFrame)
    if (!request.req.player_entity) {
        request.req.player_entity = me;
    } else if (request.req.player_entity != me) {
        *reason = KILN_CAMERA_REASON_WRONG_ENDPOINT;
        *text = "a local camera is for this client's own player";
    }
    return KILN_OK;
}

bool LocalBackend::stage(const Staged& st, uint32_t* reason, const char** text) {
    if (!st.request) return *reason = KILN_CAMERA_REASON_BUSY, false;
    const int64_t now = clock_->now_ns();
    auto p = std::make_shared<Plan>();
    p->lease = st.lease;
    p->revision = st.revision;
    p->owner = st.owner;
    p->epoch = st.epoch;
    p->update = st.update;
    p->restart = !st.update || st.timing == KILN_CAMERA_RESTART_TIMELINE;
    p->staged_ns = now;
    p->request = *st.request;
    p->request.req.keys = p->request.keys.empty() ? nullptr : p->request.keys.data();
    p->alive = st.alive;
    const KilnCameraRequest& q = p->request.req;

    std::lock_guard<std::mutex> lock(m_);
    if (st.update) {
        if (!active_ || active_->lease != st.lease) return *reason = KILN_CAMERA_REASON_UNKNOWN_LEASE, false;
        if (!p->restart) {
            // Preserve: the lease's clock goes on; the new duration counts from its T0
            const int64_t t0 = active_->first_ns.load(std::memory_order_acquire);
            const int64_t remaining = (t0 ? t0 : now) + static_cast<int64_t>(q.duration_ms) * 1'000'000 - now;
            if (remaining <= 0) {
                *text = "the lease's clock goes on (Preserve) and the new duration is already over";
                return *reason = KILN_CAMERA_REASON_EXPIRED_UPDATE, false;
            }
            if (remaining < (static_cast<int64_t>(q.blend_in_ms) + q.blend_out_ms) * 1'000'000) {
                *text = "the time left (Preserve) is shorter than the new blends";
                return *reason = KILN_CAMERA_REASON_INSUFFICIENT_LIFETIME, false;
            }
            p->t0_ns = t0 ? t0 : now;
        }
    }
    if (p->pose() && (q.parts & KILN_CAMERA_PATH)) {
        // every authored key within 250 m of the local player, when the player's position is known now (the render
        // path checks every derived position again)
        SampleSet s;
        if (samples_->try_read(&s)) {
            if (const Sample* me = fresh_local(s, q.player_entity, st.epoch, now)) {
                const Vec3 at{me->position[0], me->position[1], me->position[2]};
                for (const KilnCameraKey& k : p->request.keys)
                    if (length(vec(k.pose.position) - at) > kMaxSeparationMetres) {
                        *text = "a key is more than 250 m from the player";
                        return *reason = KILN_CAMERA_REASON_BOUNDS, false;
                    }
            }
        }
    }
    p->gen = next_gen_++;
    if (staged_) {
        staged_->stop.store(true, std::memory_order_release);
        retire(staged_);
    }
    staged_ = std::move(p);
    publish_mail();
    return true;
}

void LocalBackend::cancel_update(KilnCameraLease lease, uint64_t revision) {
    std::lock_guard<std::mutex> lock(m_);
    if (!staged_ || staged_->lease != lease || staged_->revision != revision) return;
    staged_->stop.store(true, std::memory_order_release);
    retire(staged_);
    publish_mail();
}

bool LocalBackend::release(KilnCameraLease lease, uint64_t) {
    std::lock_guard<std::mutex> lock(m_);
    if (!active_ || active_->lease != lease) return true;
    const uint32_t out = active_->request.req.blend_out_ms;
    // nothing to blend out: no blend, never drawn, writes off, or the render path has ended it already
    if (!out || !active_->first_ns.load(std::memory_order_acquire) || write_fault_.load(std::memory_order_acquire) ||
        active_->end_reason.load(std::memory_order_acquire) || active_->refusal.load(std::memory_order_acquire)) {
        active_->stop.store(true, std::memory_order_release);  // the next frame is the game's own
        retire(active_);
        publish_mail();
        return true;
    }
    active_->release_ns.store(clock_->now_ns(), std::memory_order_release);  // the render path blends out; step() reports
    return false;
}

void LocalBackend::drop(KilnCameraLease lease) {
    std::lock_guard<std::mutex> lock(m_);
    bool changed = false;
    for (std::shared_ptr<Plan>* p : {&active_, &staged_})
        if (*p && (*p)->lease == lease) {
            (*p)->stop.store(true, std::memory_order_release);  // at once: no blend that could keep dead state
            retire(*p);
            changed = true;
        }
    if (changed) publish_mail();
}

void LocalBackend::world_stopped(uint32_t epoch) {
    stopped_epoch_.store(epoch, std::memory_order_release);
    frames_->world_stopping();  // the render path compares a plan's epoch with this: nothing of that world is written
}

// ---- the client Pre ---------------------------------------------------------------------------------------------------------

void LocalBackend::retire(std::shared_ptr<Plan>& p) {
    if (!p) return;
    graveyard_.emplace_back(mail_count_ + 1, std::move(p));  // the render path holds it until it took a later mailbox
    p.reset();
}

void LocalBackend::publish_mail() {
    ++mail_count_;
    std::lock_guard<std::mutex> lock(mail_m_);
    mail_active_ = active_;
    mail_staged_ = staged_;
    mail_gen_ = mail_count_;
}

bool LocalBackend::step() {
    std::vector<Report> reports;
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(m_);
        const int64_t now = clock_->now_ns();
        const uint32_t epoch = frames_->epoch();
        bool mail = false;
        if (epoch != epoch_) {
            // another world: the core ended the old one's leases (WorldStopped); forget their plans without a report
            for (std::shared_ptr<Plan>* p : {&active_, &staged_})
                if (*p && (*p)->epoch != epoch) {
                    (*p)->stop.store(true, std::memory_order_release);
                    retire(*p);
                    mail = true;
                }
            if (epoch) write_fault_.store(false, std::memory_order_release);
            epoch_ = epoch;
        }

        // what the frames showed
        Frames::Outcome o;
        while (frames_->next_outcome(&outcome_cursor_, &o)) {
            if (staged_ && o.gen == staged_->gen) {
                if (o.matched) {
                    reports.push_back({Report::Committed, staged_->lease, staged_->revision, 0, 0, 0, o.frame, ""});
                    if (active_) {
                        active_->stop.store(true, std::memory_order_release);  // superseded, or the older revision
                        retire(active_);
                    }
                    active_ = std::move(staged_);
                    staged_.reset();
                } else {
                    reports.push_back({Report::Rejected, staged_->lease, staged_->revision, KILN_CAMERA_REASON_FRAME_FAULT, 0, KILN_CAMERA_MAY_HAVE_APPLIED, o.frame,
                                       "the frame did not show the camera that was written (another writer after kiln.camera?)"});
                    staged_->stop.store(true, std::memory_order_release);
                    retire(staged_);
                }
                mail = true;
            } else if (active_ && o.gen == active_->gen && !o.matched) {
                reports.push_back({Report::Ended, active_->lease, active_->revision, KILN_CAMERA_END_SAFETY_REJECTED, KILN_CAMERA_REASON_FRAME_FAULT, 0, o.frame,
                                   "the frame did not show the camera that was written (another writer after kiln.camera?)"});
                active_->stop.store(true, std::memory_order_release);
                retire(active_);
                mail = true;
            }
        }

        // a guarded write faulted: no visual write for the rest of this world
        if (write_fault_.load(std::memory_order_acquire)) {
            if (staged_) {
                reports.push_back({Report::Rejected, staged_->lease, staged_->revision, KILN_CAMERA_REASON_FRAME_FAULT, 0, KILN_CAMERA_MAY_HAVE_APPLIED, 0,
                                   "writing the renderer's camera faulted"});
                staged_->stop.store(true, std::memory_order_release);
                retire(staged_);
                mail = true;
            }
            if (active_) {
                reports.push_back({Report::Ended, active_->lease, active_->revision, KILN_CAMERA_END_SAFETY_REJECTED, KILN_CAMERA_REASON_FRAME_FAULT, 0, 0,
                                   "writing the renderer's camera faulted"});
                active_->stop.store(true, std::memory_order_release);
                retire(active_);
                mail = true;
            }
        }

        if (staged_) {
            Plan& s = *staged_;
            const bool shown = s.first_selection.load(std::memory_order_acquire) != 0;
            uint32_t refusal = s.refusal.load(std::memory_order_acquire);
            if (const uint32_t follow = s.follow(); follow && !shown && !refusal) {
                // this Pre's sampler asked for the target and the client world does not have it: TargetUnavailable
                SampleSet set;
                const Sample* t = samples_->try_read(&set) ? set.find(follow == s.request.req.player_entity ? 0 : follow) : nullptr;
                if (t && t->epoch == epoch && t->state == kSampleAbsent) refusal = KILN_CAMERA_REASON_TARGET_UNAVAILABLE;
            }
            if (refusal || now - s.staged_ns > kFrameDeadlineNs) {
                // refused by the render path, or no frame confirmed it in time; a frame may have shown it already
                reports.push_back({Report::Rejected, s.lease, s.revision, refusal ? refusal : static_cast<uint32_t>(KILN_CAMERA_REASON_NO_CAMERA_FRAME), 0,
                                   shown ? static_cast<uint32_t>(KILN_CAMERA_MAY_HAVE_APPLIED) : 0u, 0,
                                   refusal ? "" : "no camera frame confirmed it within 1 s"});
                s.stop.store(true, std::memory_order_release);
                retire(staged_);
                mail = true;
            }
        }

        if (active_) {
            Plan& a = *active_;
            const KilnCameraRequest& q = a.request.req;
            const int64_t t0 = a.first_ns.load(std::memory_order_acquire);
            const bool expired = t0 && now - t0 >= static_cast<int64_t>(q.duration_ms) * 1'000'000;
            const uint64_t frame = frames_->published();
            bool over = true;
            if (a.released.load(std::memory_order_acquire) || (expired && a.release_ns.load(std::memory_order_acquire))) {
                reports.push_back({Report::Released, a.lease, a.revision, 0, 0, 0, frame, ""});  // End's blend out is done
            } else if (const uint32_t why = a.end_reason.load(std::memory_order_acquire)) {
                reports.push_back({Report::Ended, a.lease, a.revision, why, a.end_code.load(std::memory_order_acquire), 0, frame, ""});
            } else if (expired) {
                reports.push_back({Report::Ended, a.lease, a.revision, KILN_CAMERA_END_EXPIRED, 0, 0, frame, ""});
            } else {
                over = false;
            }
            if (over) {
                a.stop.store(true, std::memory_order_release);
                retire(active_);
                mail = true;
            }
        }

        if (mail) publish_mail();
        // plans the render path has moved past
        const uint64_t adopted = adopted_gen_.load(std::memory_order_acquire);
        for (size_t i = graveyard_.size(); i-- > 0;)
            if (graveyard_[i].first <= adopted) graveyard_.erase(graveyard_.begin() + static_cast<std::ptrdiff_t>(i));
        while (graveyard_.size() > kGraveyardMax) graveyard_.erase(graveyard_.begin());  // a renderer that never draws

        const uint64_t c = caps();
        changed = c != reported_caps_;
        reported_caps_ = c;
    }
    flush(reports);
    return changed;
}

void LocalBackend::flush(std::vector<Report>& reports) {
    for (const Report& r : reports) {
        const char* text = r.text.empty() ? nullptr : r.text.c_str();
        switch (r.kind) {
        case Report::Committed: core_->committed(r.lease, r.revision, r.frame); break;
        case Report::Rejected: core_->rejected(r.lease, r.revision, r.code, r.flags, text); break;
        case Report::Ended: core_->ended(r.lease, r.revision, r.code, r.reason, r.frame, text); break;
        case Report::Released: core_->released(r.lease, r.frame); break;
        }
    }
}

uint32_t LocalBackend::targets(uint32_t* out, uint32_t max) const {
    std::lock_guard<std::mutex> lock(m_);
    uint32_t n = 0;
    for (const std::shared_ptr<Plan>* p : {&active_, &staged_}) {
        if (!*p || n >= max) continue;
        const uint32_t follow = (*p)->follow();
        if (follow && follow != (*p)->request.req.player_entity) out[n++] = follow;
    }
    return n;
}

LocalBackend::Counters LocalBackend::counters() const {
    return Counters{c_frames_.load(), c_pose_.load(), c_fov_.load(), c_faults_.load()};
}

uint32_t LocalBackend::plans_held() const {
    std::lock_guard<std::mutex> lock(m_);
    return (active_ ? 1u : 0u) + (staged_ ? 1u : 0u) + static_cast<uint32_t>(graveyard_.size());
}

// ---- the render path ----------------------------------------------------------------------------------------------------------

bool LocalBackend::safe(const Plan& p, const Output& o, const SampleSet& samples) const {
    if (o.fov && !lens_ok(o.view.fov)) return false;
    if (!o.pose) return true;
    Quat q = o.view.rotation;
    if (!finite(o.view.position) || !engine_unit(&q)) return false;
    const Sample& me = samples.s[0];
    const Vec3 at{me.position[0], me.position[1], me.position[2]};
    const double gap = length(o.view.position - at);
    if (!(gap <= kMaxSeparationMetres)) return false;
    int64_t f;
    return metres_to_fixed(o.view.position.x, &f) && metres_to_fixed(o.view.position.y, &f) && metres_to_fixed(o.view.position.z, &f) && p.pose();
}

LocalBackend::Eval LocalBackend::evaluate(Plan& p, Run& run, const View& natural, bool natural_had_override, const SampleSet& samples, int64_t now,
                                          Output* out, uint32_t* code, uint32_t* end_reason) {
    const KilnCameraRequest& q = p.request.req;
    const bool shown = p.first_ns.load(std::memory_order_acquire) != 0;
    Run r = run;
    if (r.gen != p.gen) {
        // its first frame: blend in from what was on screen (the natural view on a fresh apply)
        r = Run{};
        r.gen = p.gen;
        r.lease = p.lease;
        r.follow = p.follow();
        r.mode = q.profile.rotation_mode;
        r.commit_ns = now;
        r.from = natural_had_override ? last_out_ : natural;
        // a Preserve update of the same lease with the same target and rotation mode keeps the heading it had
        if (p.update && !p.restart && run_active_.lease == p.lease && run_active_.follow == r.follow && run_active_.mode == r.mode) r.heading = run_active_.heading;
    }
    int64_t t0 = p.first_ns.load(std::memory_order_acquire);
    if (!t0) t0 = p.restart || !p.t0_ns ? now : p.t0_ns;
    const double elapsed = static_cast<double>(now - t0) / 1e6;

    out->pose = p.pose();
    out->fov = p.lens();
    const int64_t release = p.release_ns.load(std::memory_order_acquire);
    // an expired lease is skipped before anything else: the stock view wins and the client Pre reports it
    if (!release && elapsed >= q.duration_ms) return Eval::Nothing;
    if (out->pose) {
        // pose, profile and shake need the local player's fresh sample (the 250 m bound and their safety)
        if (!fresh_local(samples, q.player_entity, p.epoch, now)) {
            if (!shown) return Eval::Nothing;  // not yet: the 1 s deadline answers NO_CAMERA_FRAME if it never comes
            *end_reason = KILN_CAMERA_END_SAFETY_REJECTED;
            *code = KILN_CAMERA_REASON_NONE;
            return Eval::Ended;
        }
    }

    if (release) {
        // End: the last shown view out to the fresh natural one; no new synthetic motion
        if (!r.releasing) {
            r.releasing = true;
            r.release_start_ns = now;
            r.release_from = natural_had_override ? last_out_ : natural;
        }
        const double w = q.blend_out_ms ? smoothstep(static_cast<double>(now - r.release_start_ns) / 1e6 / q.blend_out_ms) : 1.0;
        if (w >= 1.0) {
            p.released.store(true, std::memory_order_release);
            run = r;
            return Eval::Nothing;
        }
        out->view = mix(r.release_from, natural, w);
    } else {
        View a = natural;
        if (q.parts & KILN_CAMERA_PATH) {
            const PathPoint pp = path_at(p.request.keys.data(), static_cast<uint32_t>(p.request.keys.size()), elapsed);
            a.position = pp.position;
            a.rotation = pp.rotation;
            if (q.lens_source == KILN_CAMERA_LENS_KEYS) a.fov = radians(pp.fov_degrees);
        }
        if (const uint32_t follow = p.follow()) {
            // the target's copy from the newest client Pre: one completed step behind, never older than 250 ms
            const Sample* t = samples.find(follow == q.player_entity ? 0 : follow);
            const bool usable = t && t->state == kSamplePresent && t->epoch == p.epoch && now >= t->ns && now - t->ns <= kSampleValidNs;
            if (!usable) {
                if (!shown) return Eval::Nothing;  // not yet: the client Pre refuses a target that is not there
                const bool gone = t && t->state == kSampleAbsent && t->epoch == p.epoch;
                *end_reason = gone ? KILN_CAMERA_END_TARGET_LOST : KILN_CAMERA_END_SAFETY_REJECTED;
                *code = gone ? KILN_CAMERA_REASON_TARGET_UNAVAILABLE : KILN_CAMERA_REASON_NONE;
                return Eval::Ended;
            }
            const Vec3 pt{t->position[0], t->position[1], t->position[2]};
            const Quat qt{t->rotation[0], t->rotation[1], t->rotation[2], t->rotation[3]};
            if (profile_view(q.profile, pt, qt, natural.rotation, &r.heading, &a.position, &a.rotation) != ProfileResult::Ok) {
                *code = KILN_CAMERA_REASON_TARGET_UNAVAILABLE;  // straight up or down, and no heading yet
                return Eval::Refused;
            }
        }
        if (q.parts & KILN_CAMERA_SHAKE) {
            // after the path, the profile or the game's pose; Q stays
            a.position = a.position + rotate(a.rotation, shake_offset(q.shake, elapsed, q.duration_ms));
        }
        if (q.lens_source == KILN_CAMERA_LENS_CONSTANT) a.fov = radians(q.fov_y_degrees);
        const View in = mix(r.from, a, blend_in_weight(static_cast<double>(now - r.commit_ns) / 1e6, q.blend_in_ms));
        out->view = mix(in, natural, blend_out_weight(elapsed, q.duration_ms, q.blend_out_ms));
    }
    if (!out->pose) out->view.position = natural.position, out->view.rotation = natural.rotation;
    if (!out->fov) out->view.fov = natural.fov;

    if (!safe(p, *out, samples)) {
        *code = KILN_CAMERA_REASON_BOUNDS;
        if (!shown) return Eval::Refused;  // a bad request leaves the previous one as it is
        *end_reason = KILN_CAMERA_END_SAFETY_REJECTED;
        return Eval::Ended;
    }
    run = r;
    return Eval::Write;
}

void LocalBackend::on_select(const Renderer& r, uint64_t selection, float* fov) {
    c_frames_.fetch_add(1, std::memory_order_relaxed);
    const uint32_t epoch = frames_->epoch();
    // the mailbox, tried once: a busy one keeps last frame's plans
    if (mail_m_.try_lock()) {
        if (mail_gen_ != r_gen_) {
            std::shared_ptr<Plan> a = mail_active_, s = mail_staged_;
            const uint64_t g = mail_gen_;
            mail_m_.unlock();
            r_active_.swap(a);
            r_staged_.swap(s);
            a.reset();  // the client side still holds them (graveyard) until it sees adopted_gen_
            s.reset();
            r_gen_ = g;
            if (r_active_ && run_staged_.gen == r_active_->gen) run_active_ = run_staged_;  // the staged plan was confirmed
            else if (!r_active_ || run_active_.gen != r_active_->gen) run_active_ = Run{};
            if (!r_staged_ || run_staged_.gen != r_staged_->gen) run_staged_ = Run{};
            adopted_gen_.store(g, std::memory_order_release);
        } else {
            mail_m_.unlock();
        }
    }
    SampleSet fresh;
    if (samples_->try_read(&fresh)) r_samples_ = fresh;

    const RenderLayout& l = r.layout();
    uint8_t raw[kPoseBytes] = {};
    bool fault = false;
    if (!r.read(l.pose, raw, kPoseValueBytes, &fault)) {
        if (fault) write_fault_.store(true, std::memory_order_release), c_faults_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    EnginePose np;
    std::memcpy(&np, raw, sizeof np);
    View natural;
    natural.position = {fixed_to_metres(np.position[0]), fixed_to_metres(np.position[1]), fixed_to_metres(np.position[2])};
    natural.rotation = {np.rotation[0], np.rotation[1], np.rotation[2], np.rotation[3]};
    natural.fov = *fov;
    Frames::Expected none;
    if (!lens_ok(natural.fov) || !scale_ok(np.scale) || !engine_unit(&natural.rotation)) {
        // the game's own view is one v1 does not take: the game is left alone (NO_CAMERA_FRAME by the deadline)
        frames_->expect(selection, none);
        last_overridden_ = false;
        return;
    }

    const int64_t now = clock_->now_ns();
    const auto usable = [&](const std::shared_ptr<Plan>& p) {
        return p && epoch && p->epoch == epoch && !p->stop.load(std::memory_order_acquire) && (!p->alive || p->alive->load(std::memory_order_acquire)) &&
               !p->refusal.load(std::memory_order_acquire) && !p->end_reason.load(std::memory_order_acquire) && !p->released.load(std::memory_order_acquire);
    };
    Output out;
    Plan* shown = nullptr;
    if (!write_fault_.load(std::memory_order_acquire)) {
        uint32_t code = 0, end = 0;
        // the staged revision first: it replaces the live one from the frame it can be shown
        if (usable(r_staged_)) {
            switch (evaluate(*r_staged_, run_staged_, natural, last_overridden_, r_samples_, now, &out, &code, &end)) {
            case Eval::Write: shown = r_staged_.get(); break;
            case Eval::Refused:
            case Eval::Ended:
                if (r_staged_->first_ns.load(std::memory_order_acquire)) {
                    // it was drawn, and a frame may have confirmed it already: should the client Pre commit it, it ends at
                    // once with this reason (it must not hold the camera on without being drawn)
                    r_staged_->end_code.store(code, std::memory_order_release);
                    r_staged_->end_reason.store(end ? end : static_cast<uint32_t>(KILN_CAMERA_END_SAFETY_REJECTED), std::memory_order_release);
                }
                r_staged_->refusal.store(code ? code : static_cast<uint32_t>(KILN_CAMERA_REASON_NO_CAMERA_FRAME), std::memory_order_release);
                break;
            case Eval::Nothing: break;
            }
        }
        if (!shown && usable(r_active_)) {
            code = end = 0;
            switch (evaluate(*r_active_, run_active_, natural, last_overridden_, r_samples_, now, &out, &code, &end)) {
            case Eval::Write: shown = r_active_.get(); break;
            case Eval::Refused:  // (a heading camera without a heading: not reached while its memory is kept)
            case Eval::Ended:
                r_active_->end_code.store(code, std::memory_order_release);
                r_active_->end_reason.store(end ? end : static_cast<uint32_t>(KILN_CAMERA_END_SAFETY_REJECTED), std::memory_order_release);
                break;
            case Eval::Nothing: break;
            }
        }
    }
    if (!shown) {
        frames_->expect(selection, none);
        last_out_ = natural;
        last_overridden_ = false;
        return;
    }

    Frames::Expected ex;
    ex.gen = shown->gen;
    if (out.pose) {
        EnginePose w = np;  // the scale and what follows the 52 value bytes stay the game's
        Quat q = out.view.rotation;
        engine_unit(&q);
        metres_to_fixed(out.view.position.x, &w.position[0]);
        metres_to_fixed(out.view.position.y, &w.position[1]);
        metres_to_fixed(out.view.position.z, &w.position[2]);
        w.rotation[0] = static_cast<float>(q.x), w.rotation[1] = static_cast<float>(q.y), w.rotation[2] = static_cast<float>(q.z),
        w.rotation[3] = static_cast<float>(q.w);
        uint8_t bytes[kPoseBytes];
        std::memcpy(bytes, &w, sizeof w);
        if (!r.write(l.pose, bytes, kPoseValueBytes, &fault)) {
            // nothing was written, or a part of it: the next stock extraction puts the game's view back
            write_fault_.store(true, std::memory_order_release);
            c_faults_.fetch_add(1, std::memory_order_relaxed);
            frames_->expect(selection, none);
            last_overridden_ = false;
            return;
        }
        c_pose_.fetch_add(1, std::memory_order_relaxed);
        ex.wrote |= Frames::kWrotePose;
        for (int i = 0; i < 3; ++i) ex.position[i] = w.position[i];
        for (int i = 0; i < 4; ++i) ex.rotation[i] = w.rotation[i];
    }
    if (out.fov) {
        *fov = static_cast<float>(out.view.fov);  // the original instruction stores it
        c_fov_.fetch_add(1, std::memory_order_relaxed);
        ex.wrote |= Frames::kWroteFov;
        ex.fov = *fov;
    }
    if (!shown->first_ns.load(std::memory_order_acquire)) {
        shown->first_ns.store(shown->restart || !shown->t0_ns ? now : shown->t0_ns, std::memory_order_release);
        shown->first_selection.store(selection, std::memory_order_release);
    }
    frames_->expect(selection, ex);
    last_out_ = out.view;
    last_overridden_ = true;
}

}  // namespace kcam
