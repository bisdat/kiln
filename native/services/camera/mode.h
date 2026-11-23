// SPDX-License-Identifier: MIT
// kiln.camera's SERVER_MODE backend (chapter 03, 4.7): a mode lease holds a player's stock camera mode by writing the
// player entity's NetworkCameraControl (3 bytes: on, camera id, transition) through kiln.world's tracked writes, every
// step, after the game's own writer of that component (network_camera_control). Vanilla clients follow it: the
// component is replicated.
//
// One slot per player. A slot has the active revision (the one being held), at most one staged revision (an apply or
// update waiting for its first write), and at most one write ticket out at a time: a step's write is submitted in the
// server Pre (step()) or from the previous write's result (kiln.world delivers results in its Pre, before the step), so
// a held player gets exactly one write per step and never two. The ticket's change callback runs inside kiln.world's
// write program (the sim step: the tick thread on a dedicated server, maybe a game worker on a client host) and
// writes only when its (lease, revision, epoch) is still the slot's current one and its deadline has not passed: the
// slot's guard is atomics, set before a ticket goes out and cleared before write_cancel (the tombstone comes first).
//
// What the game does with a held mode is the game's: an applied lease is "the server state holds it" (commit scope
// SERVER_STATE), never "the client showed it". An Unchanged write (the component already held the bytes) counts the
// same.
//
// Threads. Backend methods run under the core's lock on the server world's thread (state.h). step() and the write
// result run on the tick thread outside it; they report to the core after their own lock is let go. The change callback
// reads the ticket and atomics only. world_stopped may come on any thread: atomics only; step() forgets the rest.
#ifndef KILN_CAMERA_MODE_H
#define KILN_CAMERA_MODE_H

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "services/kiln_world.h"
#include "state.h"

namespace kcam {

// Who a player is now: two numbers that change when the entity id comes to name someone else (production: kiln.players'
// joined_step and connection handle; the lab fixture: the spawn step of its stand-in).
struct Incarnation {
    uint64_t a = 0, b = 0;
    bool operator==(const Incarnation& o) const { return a == o.a && b == o.b; }
    bool operator!=(const Incarnation& o) const { return !(*this == o); }
};

// What the backend needs of the game: kiln.world's tracked writes, the players, the server clock. The service gives the
// real ones (camera.cpp); the host tests and the lab fixture their own.
class ModeWorld {
public:
    virtual ~ModeWorld() = default;
    // kiln.world write_submit / write_cancel. Tick thread, outside a program update.
    virtual KilnResult submit(const KilnWorldWriteRequest& request, uint64_t* out_ticket) = 0;
    virtual KilnResult cancel(uint64_t ticket) = 0;
    // A player of the running server world, and who they are now. False: no such player in game. Tick thread.
    virtual bool player(KilnEntity entity, Incarnation* out) = 0;
    // The monotonic server clock in milliseconds (any thread, no game calls), and the server step (tick thread).
    virtual uint64_t now_ms() = 0;
    // Exact origin/deadline comparisons. Fixtures may expose an integer-millisecond clock; production retains QPC precision.
    virtual uint64_t now_ns() { return now_ms() * UINT64_C(1000000); }
    virtual uint64_t step() = 0;
    virtual void log(int32_t level, const std::string& text) = 0;
};

class ModeBackend final : public Backend {
public:
    static constexpr uint32_t kNccSize = 3;
    static constexpr uint32_t kMaxDrops = 20;  // DROPPED answers in a row before a lease gives up (no write program runs)

    // `component`: NetworkCameraControl's hash; `who`: the name write_declare took the component under (kept as is).
    ModeBackend(Core* core, ModeWorld* world, uint32_t component, std::string who);

    // The component was declared and tracked writes work: the backend serves (caps() has SERVER_MODE).
    void set_ready(bool on) { ready_.store(on, std::memory_order_release); }

    // ---- Backend ----
    uint64_t caps() const override;
    KilnResult admit(Stored& request, uint32_t* reason, const char** text) override;
    bool stage(const Staged& staged, uint32_t* reason, const char** text) override;
    void cancel_update(KilnCameraLease lease, uint64_t revision) override;
    bool release(KilnCameraLease lease, uint64_t revision) override;
    void drop(KilnCameraLease lease) override;
    void world_stopped(uint32_t epoch) override;

    // SERVER Pre, before Core::pre(kServer): forget a stopped world, end leases whose player left or changed or whose
    // time is up, and send the write of each held player that has none out.
    void step();

    // ---- for tests and the log (any thread unless said) ----
    struct Guard {
        KilnCameraLease lease = 0;
        uint64_t revision = 0;
    };
    // What the change callback of `player` may write now (lease 0: nothing).
    Guard guard(KilnEntity player) const;
    struct Counters {
        uint64_t submits = 0, cancels = 0, writes = 0, refusals = 0;  // refusals: the change callback said no
        uint64_t results[8] = {};                                    // by KILN_WORLD_WRITE_* status
    };
    Counters counters() const;
    uint32_t tickets() const;  // write tickets out now (takes the backend's lock: not from inside a ModeWorld call)
    // Whether the ticket whose `user` this is may still write (its slot's guard names its lease and revision). Lock-free:
    // a ModeWorld::cancel can ask it about the ticket being cancelled (the lab fixture proves the tombstone comes first).
    static bool ticket_current(const void* user);

private:
    struct Rev {
        KilnCameraLease lease = 0;
        uint64_t revision = 0;
        KilnOwner owner = 0;
        uint32_t epoch = 0;
        uint8_t camera_id = 0, transition = 0;
        uint64_t start_ns = 0, deadline_ns = 0;
        uint32_t duration_ms = 0;
        bool start_on_commit = false;
        std::shared_ptr<std::atomic<bool>> alive;
    };
    struct Ticket {
        ModeBackend* self = nullptr;
        uint32_t slot = 0;
        KilnEntity player = 0;
        KilnCameraLease lease = 0;
        uint64_t revision = 0;
        uint32_t epoch = 0;
        uint64_t deadline_ns = 0, wrote_ns = 0;
        std::shared_ptr<std::atomic<bool>> alive;
        uint8_t bytes[kNccSize] = {};
        uint64_t id = 0;
    };
    struct Slot {
        KilnEntity player = 0;  // 0: free
        Incarnation who;
        Rev active, staged;     // lease 0: none
        Ticket* ticket = nullptr;
        uint32_t drops = 0;
        // the change callback's guard: set before a ticket goes out, cleared before it is cancelled
        std::atomic<uint64_t> cur_lease{0}, cur_revision{0};
    };
    struct Report {
        enum Kind { Committed, Rejected, Ended } kind = Committed;
        KilnCameraLease lease = 0;
        uint64_t revision = 0;
        uint32_t code = 0;  // Rejected: reason; Ended: end reason
        uint32_t reason = 0;  // Ended: reason code
        uint64_t frame = 0;
        std::string text;
    };

    static bool KILN_CALL on_change(KilnEntity entity, void* data, uint32_t size, void* user);
    static void KILN_CALL on_result(const KilnWorldWriteResult* result, void* user);
    void result(Ticket* t, const KilnWorldWriteResult& r);

    // the lock is held below
    Slot* find(KilnEntity player);
    uint32_t index(const Slot* s) const { return static_cast<uint32_t>(s - slots_); }
    // the revision the slot writes next (the staged one first), or null
    const Rev* next(const Slot& s) const;
    void set_guard(Slot& s, const Rev* r);
    // Clears the guard first, then cancels the slot's ticket: no change callback of it starts after this.
    void cut(Slot& s);
    void submit(Slot& s);
    void free_if_idle(Slot& s);
    void flush(std::vector<Report>& reports);  // the lock is NOT held

    Core* core_;
    ModeWorld* world_;
    const uint32_t component_;
    const std::string who_;
    std::atomic<bool> ready_{false};
    std::atomic<bool> broken_{false};
    std::atomic<uint32_t> live_epoch_{0};   // the change callback's epoch guard (0: no world)
    std::atomic<uint64_t> now_ns_{0};       // the change callback's clock (the last step's)
    std::atomic<bool> stopped_{false};      // world_stopped came (step() forgets that world's work by its epoch)

    mutable std::mutex m_;
    Slot slots_[kMaxPlayerSlots];

    std::atomic<uint64_t> submits_{0}, cancels_{0}, writes_{0}, refusals_{0};
    std::atomic<uint64_t> results_[8]{};
};

}  // namespace kcam
#endif
