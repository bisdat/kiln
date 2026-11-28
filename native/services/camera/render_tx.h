// SPDX-License-Identifier: MIT
// kiln.camera's LOCAL_VISUAL backend (chapter 03, 4.1-4.4): the render-only camera of this client's own player.
//
// One commit point. Everything a visual request changes is written in the renderer's camera selection
// (camera/RenderFovSelected, snapshot.h): the selected pose's 52 value bytes and/or the low float of xmm6 (the field of
// view the game is about to store), nothing else. The player's input camera, the game's camera object, the local
// player's camera transform and every component stay as they are, so aim and input never move.
//
// A request is a Plan: an immutable copy of one revision (the request, its keys, its timing) plus a few atomics that
// the render path sets for the client Pre to report (its first frame, a refusal, an end, the release done). The client
// Pre (step()) owns the plans and hands the render path the live one and the staged one through a mailbox that the
// render path try-locks once per frame: a busy mailbox keeps last frame's plans; a stop (end, owner gone, world gone)
// is an atomic on the plan and needs no mailbox. Plans the render path may still hold are freed only after it has taken
// a newer mailbox, so nothing is freed on the render path (unless it took none of the last 64: see step()).
//
// Applied only after a frame showed it: the selection writes the plan's values and tells the capture what it wrote
// (Frames::expect); the capture compares the final frame with that (position exact in 32.32, rotation and field of view
// within 1e-5) and the next client Pre reports APPLIED (or UPDATED) with that frame, or FRAME_FAULT and
// MAY_HAVE_APPLIED when something after us changed it. No frame within 1 s: NO_CAMERA_FRAME.
//
// Timeline (4.3): T0 is the first committed frame (a Preserve update keeps it, a Restart restarts it), the clock is
// QueryPerformanceCounter (integer nanoseconds subtracted first), the lease expires at T0 + duration and then nothing
// is written. Blend in from the view shown before (the natural one on a fresh apply), blend out over the duration's
// last part toward the fresh natural view, End blends the last shown view out to the natural one.
//
// Threads. Backend methods: the core's lock is held, the client world's thread (world_stopped: any thread, atomics
// only). step(): the client Pre, outside the core's lock; it reports to the core after letting its own lock go.
// on_select(): the render path; atomics, the mailbox's try-lock, the renderer borrow; no game call, no author code.
#ifndef KILN_CAMERA_RENDER_TX_H
#define KILN_CAMERA_RENDER_TX_H

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "profile.h"
#include "sampler.h"
#include "shake.h"
#include "snapshot.h"
#include "state.h"
#include "timeline.h"

namespace kcam {

// No distance bound from the local player (owner, 0.2.0-dev.31.25): a key or a derived position may be anywhere inside
// the world bound (cam_math.h, kMaxWorldMetres). A path and a shake depend on no sample at all; only a follow profile
// reads one, its target's, and stops when that copy is stale.
constexpr int64_t kSampleValidNs = 250'000'000;  // a follow target's copy older than this is stale: the view would freeze or jump
constexpr int64_t kFrameDeadlineNs = 1'000'000'000; // no confirming frame within this: NO_CAMERA_FRAME

// What the backend needs of the client (production: kiln.client; the host tests: a fake).
class LocalWorld {
public:
    virtual ~LocalWorld() = default;
    virtual uint32_t local_player() = 0;  // 0: none. Any thread
};

// One revision of one lease, as the render path sees it.
struct Plan {
    uint64_t gen = 0;  // unique in the process, rising
    KilnCameraLease lease = 0;
    uint64_t revision = 0;
    KilnOwner owner = 0;
    uint32_t epoch = 0;
    bool update = false;
    bool restart = false;   // a Restart update (or an apply): T0 is this revision's first frame
    int64_t t0_ns = 0;      // a Preserve update: the lease's T0
    int64_t staged_ns = 0;  // when it was staged (the 1 s frame deadline)
    Stored request;
    std::shared_ptr<std::atomic<bool>> alive;  // the core's: false once the lease or its owner is gone

    bool pose() const { return (request.req.parts & (KILN_CAMERA_PATH | KILN_CAMERA_PROFILE | KILN_CAMERA_SHAKE)) != 0; }
    bool lens() const { return (request.req.parts & KILN_CAMERA_LENS) != 0; }
    // A profile's target: its follow entity, or the local player for 0. 0 when the plan has no profile.
    uint32_t follow() const {
        if (!(request.req.parts & KILN_CAMERA_PROFILE)) return 0;
        return request.req.profile.follow_entity ? request.req.profile.follow_entity : request.req.player_entity;
    }

    // ---- set by the render path, read by the client Pre ----
    std::atomic<int64_t> first_ns{0};          // T0 actually used (set with the first frame written)
    std::atomic<uint64_t> first_selection{0};  // the selection of that frame
    std::atomic<uint32_t> refusal{0};          // before it was shown: why it cannot be (KILN_CAMERA_REASON_*)
    std::atomic<uint32_t> end_reason{0};       // after it was shown: why it ended (KILN_CAMERA_END_*)
    std::atomic<uint32_t> end_code{0};         //   and the reason code with it
    std::atomic<bool> released{false};         // End's blend out has reached the natural view
    // ---- set by the client Pre, read by the render path ----
    std::atomic<bool> stop{false};             // write nothing for it from now on (bypasses the mailbox)
    std::atomic<int64_t> release_ns{0};        // End: blend out from this time
};

class LocalBackend final : public Backend {
public:
    LocalBackend(Core* core, Frames* frames, SampleBoard* samples, const Clock* clock, LocalWorld* world)
        : core_(core), frames_(frames), samples_(samples), clock_(clock), world_(world) {}

    // The capability bits the hooks and the sampler make possible (KILN_CAMERA_CAP_LOCAL_*). Setup, or the client Pre.
    void set_serving(uint64_t bits) { serving_.store(bits, std::memory_order_release); }
    // A write or the capture faulted in this client world: no visual write until the next one. Any thread.
    bool faulted() const { return write_fault_.load(std::memory_order_acquire) || frames_->faulted(); }

    // ---- Backend ----
    uint64_t caps() const override;
    KilnResult admit(Stored& request, uint32_t* reason, const char** text) override;
    bool stage(const Staged& staged, uint32_t* reason, const char** text) override;
    void cancel_update(KilnCameraLease lease, uint64_t revision) override;
    bool release(KilnCameraLease lease, uint64_t revision) override;
    void drop(KilnCameraLease lease) override;
    void world_stopped(uint32_t epoch) override;

    // The client Pre, after the sampler and before Core::pre(kClient). True: caps() changed (a fault, a new world).
    bool step();
    // The entities the next Pre's sampler should copy besides the local player (follow targets). Client thread.
    uint32_t targets(uint32_t* out, uint32_t max) const;

    // camera/RenderFovSelected, after Frames::select() gave `selection`. fov: xmm6's low float.
    void on_select(const Renderer& r, uint64_t selection, float* fov);

    // ---- tests and the log ----
    struct Counters {
        uint64_t frames = 0, wrote_pose = 0, wrote_fov = 0, faults = 0;
    };
    Counters counters() const;
    uint32_t plans_held() const;  // plans the client side still keeps (live, staged, waiting to be freed)
    // Tests: called on the render path after the plan to draw was picked, right before the world and owner are checked
    // again and the renderer is written (another thread ending them in between).
    void set_before_commit_for_test(void (*fn)(void*), void* user) {
        before_commit_ = fn;
        before_commit_user_ = user;
    }

private:
    // the render path's own state for one plan
    struct Run {
        uint64_t gen = 0;
        KilnCameraLease lease = 0;
        uint32_t follow = 0, mode = 0;  // the profile's target and rotation mode (the heading's memory goes with them)
        HeadingMemory heading;
        int64_t commit_ns = 0;  // its first frame
        View from;              // blend-in origin
        bool releasing = false;
        int64_t release_start_ns = 0;
        View release_from;
    };
    struct Output {
        View view;
        bool pose = false, fov = false;  // which channels are written
    };
    struct Report {
        enum Kind { Committed, Rejected, Ended, Released } kind = Committed;
        KilnCameraLease lease = 0;
        uint64_t revision = 0;
        uint32_t code = 0, reason = 0, flags = 0;
        uint64_t frame = 0;
        std::string text;
    };

    // render path
    enum class Eval { Write, Nothing, Refused, Ended };
    Eval evaluate(Plan& p, Run& run, const View& natural, bool natural_had_override, const SampleSet& samples, int64_t now, Output* out,
                  uint32_t* code, uint32_t* end_reason);
    static bool safe(const Output& o);

    // client side (lock held)
    void publish_mail();
    void retire(std::shared_ptr<Plan>& p);
    void flush(std::vector<Report>& reports);

    Core* core_;
    Frames* frames_;
    SampleBoard* samples_;
    const Clock* clock_;
    LocalWorld* world_;
    std::atomic<uint64_t> serving_{0};
    std::atomic<bool> write_fault_{false};  // a guarded write faulted: no visual write for the rest of this world

    // client side
    mutable std::mutex m_;
    std::shared_ptr<Plan> active_, staged_;
    std::vector<std::pair<uint64_t, std::shared_ptr<Plan>>> graveyard_;  // (mailbox generation it left, plan)
    uint64_t next_gen_ = 1;
    uint64_t mail_count_ = 0;  // mailboxes published (the render path's adopted_gen_ counts the same)
    uint64_t outcome_cursor_ = 0;
    uint64_t reported_caps_ = 0;
    uint32_t epoch_ = 0;

    // the mailbox
    std::mutex mail_m_;
    uint64_t mail_gen_ = 0;
    std::shared_ptr<Plan> mail_active_, mail_staged_;
    std::atomic<uint64_t> adopted_gen_{0};

    // the render path
    std::shared_ptr<Plan> r_active_, r_staged_;
    uint64_t r_gen_ = 0;
    Run run_active_, run_staged_;
    View last_out_;
    bool last_overridden_ = false;  // last frame's view was ours
    SampleSet r_samples_{};
    std::atomic<uint64_t> c_frames_{0}, c_pose_{0}, c_fov_{0}, c_faults_{0};
    void (*before_commit_)(void*) = nullptr;
    void* before_commit_user_ = nullptr;
};

}  // namespace kcam
#endif
