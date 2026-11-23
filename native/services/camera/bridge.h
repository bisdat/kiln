// SPDX-License-Identifier: MIT
// kiln.camera's REMOTE_VISUAL side (chapter 03 §4.6): the framework's transport bridge and the backend that hands it
// the server's visual requests for players whose client runs Kiln.
//
// Who does what. Kiln.Camera (managed, the persistent framework module CameraStartup) owns the transport: the channel
// kiln.camera v1, encoding, negotiation, retries, ACKs, the host's own player (a local queue, no packets). It binds a
// KilnCameraTransport table here (bind_transport). This side owns the leases (state.h) and turns each staged revision,
// end and drop into one submit() of that table; the bridge answers through `done` and reports a lease its client ended
// by itself through transport_ended. camera.remote is on exactly while a bridge is bound.
//
// Threads. Backend methods run under the core's lock and only queue (no bridge call, no managed code there). step() runs
// in the server's Pre, before Core::pre(kServer), outside every lock: it reports what the bridge answered to the core and
// sends what was queued. `done` and transport_ended may come on any thread (normally the server tick, a world-stop
// thread when the bridge closes): they only record. caps() of a player calls the bridge (any thread, never under the
// core's lock: Core::capabilities holds none).
#ifndef KILN_CAMERA_BRIDGE_H
#define KILN_CAMERA_BRIDGE_H

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "services/kiln_camera.h"
#include "state.h"

namespace kcam {

class TransportBinding {
    struct State;
public:
    // A reservation of the copied bridge table. Use and destroy it on the thread that acquired it; no callback from
    // this table may outlive the reservation. Unbind closes admission first and waits for all such calls to return.
    class Call {
    public:
        Call() = default;
        ~Call();
        Call(const Call&) = delete;
        Call& operator=(const Call&) = delete;
        Call(Call&& other) noexcept;
        Call& operator=(Call&&) = delete;
        explicit operator bool() const { return static_cast<bool>(state_); }
        const KilnCameraTransport* table() const;
        KilnOwner owner() const;
        uint64_t generation() const;
    private:
        friend class TransportBinding;
        explicit Call(std::shared_ptr<State> state);
        std::shared_ptr<State> state_;
    };

    TransportBinding();
    TransportBinding(const TransportBinding&) = delete;
    TransportBinding& operator=(const TransportBinding&) = delete;
    // bind copies the v1 prefix synchronously. One nonzero framework owner; no replacement while bound or draining.
    KilnResult bind(KilnOwner owner, const KilnCameraTransport* table);
    // Any thread. Success is the delegate-root release barrier. Wrong owner, self-unbind and a timeout are failures:
    // the framework MUST retain all delegates and retry after the calls drain. Timeout/self-unbind still close the
    // gate, so no new call can start. The caller logs an error on timeout; a failure must never free managed roots.
    KilnResult unbind(KilnOwner owner);
    // Ready is explicitly set only after startup owns the channel and both routes exist; binding alone is not ready.
    KilnResult set_ready(KilnOwner owner, bool ready);
    bool ready() const;
    // The bound owner (0: none), whatever its readiness.
    KilnOwner owner() const;
    // Generation of the binding now (0 before the first bind). A new bind is a new generation.
    uint64_t generation() const;
    Call acquire() const;
    // For deterministic host tests. Production default is 2000 ms, matching camera's owner-retiring barrier.
    void set_drain_wait_ms(uint32_t ms);
    uint32_t in_flight() const;
private:
    std::shared_ptr<State> state_;
};

// The REMOTE_VISUAL backend over the bound bridge. One per process (the bridge's `done` is a plain C function that
// finds it); the host tests make one at a time.
class RemoteBackend final : public Backend {
public:
    explicit RemoteBackend(Core* core);
    ~RemoteBackend() override;
    RemoteBackend(const RemoteBackend&) = delete;
    RemoteBackend& operator=(const RemoteBackend&) = delete;

    // ---- the table's framework-private entries (any thread) ----
    // A bridge binds once Kiln.Camera owns the channel and its routes: bound means ready (camera.remote on).
    KilnResult bind(KilnOwner owner, const KilnCameraTransport* table);
    // After the bridge drained its work (its dones and transport_ended calls returned). Requests still out are refused
    // PEER_UNAVAILABLE (MAY_HAVE_APPLIED when they reached the bridge) and applied leases end CAPABILITY_LOST at the next
    // step. A failure keeps the table and its callbacks alive (see TransportBinding::unbind).
    KilnResult unbind(KilnOwner owner);
    // The client ended a lease by itself (expired, target lost, a safety refusal) or the bridge let it go (the peer left,
    // its binding was replaced, the heartbeat bookkeeping expired). Checked against the bridge owner here and against the
    // lease and its revision at the next step (Core::ended): a stale one changes nothing.
    KilnResult transport_ended(KilnOwner owner, KilnCameraLease lease, uint64_t revision, uint32_t end_reason, uint32_t reason_code, KilnStr reason);
    bool bound() const { return binding_.ready(); }
    KilnOwner bridge_owner() const { return binding_.owner(); }

    // SERVER Pre, before Core::pre(kServer), outside every lock: report the bridge's answers to the core, then send what
    // waits (in the order it was queued), then tell the bridge which authors are gone.
    void step();

    // ---- Backend (under the core's lock: queue only) ----
    uint64_t caps() const override;
    uint64_t player_caps(KilnEntity player) const override;
    KilnResult admit(Stored& request, uint32_t* reason, const char** text) override;
    bool stage(const Staged& staged, uint32_t* reason, const char** text) override;
    void cancel_update(KilnCameraLease lease, uint64_t revision) override;
    bool release(KilnCameraLease lease, uint64_t revision) override;
    void drop(KilnCameraLease lease) override;
    void world_stopped(uint32_t epoch) override;
    void owner_gone(KilnOwner owner) override;

    // ---- tests and the log ----
    void set_drain_wait_ms(uint32_t ms) { binding_.set_drain_wait_ms(ms); }
    uint32_t outstanding() const;  // submits the bridge has not answered yet
    uint32_t queued() const;       // operations waiting for the next step
    uint32_t active() const;       // applied leases the bridge carries

private:
    enum Op : uint32_t { kApply = 1, kUpdatePreserve = 2, kUpdateRestart = 3, kEnd = 4 };
    struct Out {
        uint32_t op = kApply;
        KilnCameraLease lease = 0, replaces = 0;
        uint64_t revision = 0;
        KilnOwner author = 0;
        uint32_t epoch = 0;
        bool report = true;  // End: tell the core when released (false: a drop, nobody waits)
        std::shared_ptr<std::atomic<bool>> alive;
        std::unique_ptr<Stored> request;  // Apply/Update
    };
    struct Job {
        uint32_t op = kApply;
        KilnCameraLease lease = 0, replaces = 0;
        uint64_t revision = 0;
        KilnOwner author = 0;
        uint32_t epoch = 0;
        uint64_t generation = 0;  // the binding it went to
        bool report = true;
    };
    struct Answer {
        uint64_t job = 0;
        uint32_t ok = 0, reason = 0, flags = 0;
        uint64_t frame = 0;
        std::string text;
    };
    struct EndedNote {
        KilnCameraLease lease = 0;
        uint64_t revision = 0;
        uint32_t end_reason = 0, reason_code = 0;
        std::string text;
    };
    struct Active {
        uint64_t revision = 0;
        KilnOwner author = 0;
        uint32_t epoch = 0;
    };
    // the author and epoch of a lease the bridge saw (lock held); false: it never left
    bool known(KilnCameraLease lease, KilnOwner* author, uint32_t* epoch, uint64_t* revision) const;

    static void KILN_CALL on_done(const KilnCameraResult* result, void* user);
    void record(uint64_t job, const KilnCameraResult& result);
    void forget_epoch(uint32_t epoch);

    Core* core_;
    TransportBinding binding_;
    mutable std::mutex m_;
    std::vector<Out> out_;
    std::map<uint64_t, Job> jobs_;
    std::vector<Answer> answers_;
    std::vector<EndedNote> ended_;
    std::vector<KilnOwner> gone_;
    std::map<KilnCameraLease, Active> active_;
    uint64_t next_job_ = 1;
    uint64_t seen_generation_ = 0;  // step(): the binding whose loss was handled
    std::atomic<uint32_t> stopped_{0};
};

}  // namespace kcam
#endif
