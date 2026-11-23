// SPDX-License-Identifier: MIT
// kiln.camera's lease model (services/kiln_camera.h): who holds which endpoint, what waits, and in which order the
// answers and change events go out. No game and no Windows in here: the host tests drive it with a fake backend
// (tests/state_test.cpp), the service with the real ones (camera.cpp).
//
// Words. A slot is one endpoint of one player (SERVER_MODE and REMOTE_VISUAL per player, LOCAL_VISUAL one). A slot
// has at most one holder (an applied lease) and one candidate (a lease whose apply the backend has not decided), and
// at most one apply or update in flight. An operation is an accepted apply/update/end whose `done` is still owed;
// KILN_CAMERA_MAX_PENDING of them may wait in the process.
//
// Threads. One mutex guards the model. It is never held while author code runs (`done`, a change subscriber): a
// delivery is taken from its queue under the lock and called outside it, after a last look at whether its owner is
// still alive. Backend methods run under the lock and must not call the core back from inside them; a backend
// reports from its own step (committed, rejected, released, ended), before Core::pre of that world.
// owner_retiring and world_stopping may come on any thread: they change the model first and call backends only
// through world_stopped (atomics only).
//
// The owner-retiring barrier (DECISIONS.md "Camera owner-retiring barrier", a deviation from chapter 03 4.6's
// "the observer never waits"): every callback into an owner is reserved under the lock (a per-owner count) before it
// is called outside it, and released after it returned. owner_retiring closes the gate first (no reservation for the
// owner from then on), then waits, holding no lock, until the owner's calls on other threads have returned, so a host
// that frees the owner's code after the observer returned never frees code that runs. A retirement from inside one of
// the owner's own callbacks does not wait for that call (it returns before its host frees anything; the owner's state
// is cleaned up at each world's next Pre in any case). The wait is bounded (set_retire_wait_ms, 2 s): on a timeout it
// logs an error and returns; the owner stays retired, and kiln.camera frees nothing those calls can reach (they hold
// copies of their delivery and subscriber).
#ifndef KILN_CAMERA_STATE_H
#define KILN_CAMERA_STATE_H

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "services/kiln_camera.h"

namespace kcam {

// The world an endpoint lives in. Each has its own epoch (0: not running) and its own delivery queue.
enum World : uint32_t { kServer = 0, kClient = 1, kWorlds = 2 };
// Which worlds the calling thread steps (the `threads` argument of apply/update/end).
constexpr uint32_t kOnServer = 1u << kServer, kOnClient = 1u << kClient;

inline World world_of(uint32_t endpoint) { return endpoint == KILN_CAMERA_LOCAL_VISUAL ? kClient : kServer; }
inline bool is_mode(uint32_t parts) { return (parts & KILN_CAMERA_MODE) != 0; }

constexpr uint32_t kMaxPlayerSlots = 16;   // slots of SERVER_MODE, and of REMOTE_VISUAL
constexpr uint32_t kMaxSubscribers = 256;
constexpr uint32_t kMaxTombstones = 256;
// end() is admitted beyond KILN_CAMERA_MAX_PENDING, up to this: the queue it clears must not starve it.
constexpr uint32_t kMaxPendingWithEnds = 2 * KILN_CAMERA_MAX_PENDING;

// A request as the service keeps it: checked, quaternions normalized, keys copied.
struct Stored {
    KilnCameraRequest req{};  // req.keys points into `keys` (or is null)
    std::vector<KilnCameraKey> keys;
};

// The request's shape and numbers (chapter 03, 4.4 and 5.2). False: KILN_ERR_INVALID_ARGUMENT. Reads `r` and its keys
// once; nothing of the caller's memory is kept.
bool validate(const KilnCameraRequest* r, Stored* out);

// The capability bits a request needs on its endpoint. 0 for a request its endpoint refuses anyway (WRONG_ENDPOINT).
uint64_t required_caps(const KilnCameraRequest& r);

const char* reason_text(uint32_t reason_code);

// What the core hands a backend to decide: one revision of one lease.
struct Staged {
    KilnCameraLease lease = 0;
    uint64_t revision = 0;
    KilnOwner owner = 0;
    uint32_t epoch = 0;
    bool update = false;          // false: the lease's first revision
    uint32_t timing = 0;          // update: KILN_CAMERA_PRESERVE_TIMELINE / RESTART_TIMELINE
    KilnCameraLease replaces = 0; // apply: the holder this lease supersedes when it commits (0: the slot is free)
    std::shared_ptr<std::atomic<bool>> alive;  // invalidated before owner/world/lease cleanup, retained by queued jobs
    const Stored* request = nullptr;  // valid for the call; copy what is kept
};

// One endpoint's physical side: the Mode maintainer, the local renderer, the transport bridge.
class Backend {
public:
    virtual ~Backend() = default;
    // KILN_CAMERA_CAP_* this backend serves now. Any thread.
    virtual uint64_t caps() const = 0;
    // The same for one player (capabilities() with a player): REMOTE_VISUAL adds the part bits that player's client
    // offered. Any thread, never under the core's lock (it may call the transport bridge).
    virtual uint64_t player_caps(KilnEntity player) const {
        (void)player;
        return caps();
    }
    // Before anything is queued. KILN_OK with *reason 0: go on. KILN_OK with a reason: a refusal, answered through
    // `done`. Anything else is the call's immediate return (KILN_ERR_NOT_FOUND: no such player here). May fill in
    // request.req.player_entity (LOCAL_VISUAL: 0 becomes the local player).
    virtual KilnResult admit(Stored& request, uint32_t* reason, const char** text) = 0;
    // Take a revision. True: staged; exactly one of Core::committed / Core::rejected follows for it, unless the core
    // withdraws it first (cancel_update, drop, world_stopped). False: refused now, with *reason.
    virtual bool stage(const Staged& staged, uint32_t* reason, const char** text) = 0;
    // The staged update is withdrawn; the lease's active revision stays. Nothing is reported for it afterwards.
    virtual void cancel_update(KilnCameraLease lease, uint64_t revision) = 0;
    // end(): let go of the active revision. True: released now. False: Core::released follows (a blend out).
    virtual bool release(KilnCameraLease lease, uint64_t revision) = 0;
    // Forget the lease at once, whatever its state (owner gone, cancelled before its apply). Nothing is reported.
    virtual void drop(KilnCameraLease lease) = 0;
    // The world of this endpoint is going: everything of `epoch` is void. Any thread: atomics only, no game.
    virtual void world_stopped(uint32_t epoch) = 0;
    // An owner retired (or faulted) and its leases of this endpoint's world were dropped. Under the lock: note it only.
    virtual void owner_gone(KilnOwner owner) { (void)owner; }
};

// How the core reaches author code and the log. call_*: false when the callback faulted.
class Env {
public:
    virtual ~Env() = default;
    virtual bool call_done(KilnCameraDone fn, const KilnCameraResult& result, void* user) = 0;
    virtual bool call_changed(KilnCameraChanged fn, const KilnCameraChange& change, void* user) = 0;
    virtual void log(int32_t level, const std::string& text) = 0;
};

class Core {
public:
    explicit Core(Env* env) : env_(env) {}

    // ---- setup (before the first call below; not guarded) ----
    void set_backend(uint32_t endpoint, Backend* backend);
    // Whether the owner-retiring observer is there. Without it no write is admitted (capabilities() hides the bits).
    void set_writes(bool on) { writes_.store(on, std::memory_order_release); }
    bool writes() const { return writes_.load(std::memory_order_acquire); }
    // A bit that does not come from a backend (RENDERED_SNAPSHOT), for LOCAL_VISUAL.
    void set_local_read_caps(uint64_t bits) { local_read_caps_.store(bits, std::memory_order_release); }
    // How long owner_retiring waits for the owner's running callbacks on other threads (default 2000 ms).
    void set_retire_wait_ms(uint32_t ms) { retire_wait_ms_.store(ms, std::memory_order_relaxed); }

    // ---- lifecycle ----
    void world_started(World world, uint32_t epoch);   // that world's thread, before its first Pre
    void world_stopping(World world, uint32_t epoch);  // any thread: ends everything of that world and delivers it here
    // Any thread: no callback into `owner` starts from now on, and on return none runs on another thread (bounded wait).
    void owner_retiring(KilnOwner owner);
    void pre(World world);                             // that world's Pre, after the backends' own step

    // ---- the table ----
    KilnResult capabilities(uint32_t endpoint, KilnEntity player, uint64_t* mask) const;
    KilnResult apply(KilnOwner owner, const KilnCameraRequest* request, KilnCameraLease* out_lease, KilnCameraDone done, void* user, uint32_t threads);
    KilnResult update(KilnOwner owner, KilnCameraLease lease, uint32_t timing, const KilnCameraRequest* request, KilnCameraDone done, void* user, uint32_t threads);
    KilnResult end(KilnOwner owner, KilnCameraLease lease, KilnCameraDone done, void* user, uint32_t threads);
    KilnResult subscribe(KilnOwner owner, KilnCameraChanged fn, void* user, KilnHandle* out);
    KilnResult unsubscribe(KilnHandle handle);

    // ---- from a backend, on its world's thread, outside its own methods. False: stale (nothing changed) ----
    bool committed(KilnCameraLease lease, uint64_t revision, uint64_t frame);
    bool rejected(KilnCameraLease lease, uint64_t revision, uint32_t reason_code, uint32_t flags, const char* text);
    bool released(KilnCameraLease lease, uint64_t frame);
    bool ended(KilnCameraLease lease, uint64_t revision, uint32_t end_reason, uint32_t reason_code, uint64_t frame, const char* text);

    // ---- for tests and the log ----
    uint32_t epoch(World world) const;
    uint32_t pending() const;
    uint32_t live_leases() const;
    // The active revision of a live lease, 0 when it has none (not applied yet, or not live).
    uint64_t active_revision(KilnCameraLease lease) const;
    // Callbacks into `owner` that are reserved and have not returned yet.
    uint32_t calls_in(KilnOwner owner) const;

private:
    struct Op {
        enum Type { Apply, Update, End } type = Apply;
        KilnOwner owner = 0;
        KilnCameraDone done = nullptr;
        void* user = nullptr;
        KilnCameraLease lease = 0;
        uint64_t revision = 0;  // Apply/Update: the revision that was staged
        std::unique_ptr<Stored> request;
    };
    struct Lease {
        enum State { Staged, Active, Ending };
        KilnCameraLease id = 0;
        std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>>(true);
        KilnOwner owner = 0;
        uint32_t endpoint = 0, player = 0, epoch = 0;
        int32_t priority = 0;
        State state = Staged;
        uint64_t revision = 0;       // the applied revision; 0 before the first commit
        uint64_t next_revision = 1;  // never handed out twice
        std::unique_ptr<Op> end_op;  // Ending
        std::vector<std::unique_ptr<Op>> end_followers;  // end() again while Ending
    };
    struct Slot {
        KilnCameraLease holder = 0, candidate = 0;
        std::unique_ptr<Op> inflight;  // the candidate's Apply, or the holder's Update
    };
    struct Tomb {
        KilnCameraLease id;
        KilnOwner owner;
        uint32_t endpoint;
    };
    struct Subscriber {
        KilnHandle handle;
        KilnOwner owner;
        KilnCameraChanged fn;
        void* user;
    };
    struct Delivery {
        bool change = false;
        KilnOwner owner = 0;  // done: whose callback
        KilnCameraDone done = nullptr;
        void* user = nullptr;
        KilnCameraResult result{};
        KilnCameraChange event{};
        std::string text;
    };
    struct Dead {
        bool cleaned[kWorlds] = {false, false};
        bool timed_out = false;  // owner_retiring returned while calls into it still ran (never pruned)
    };

    static uint64_t slot_key(uint32_t endpoint, uint32_t player) {
        return (static_cast<uint64_t>(endpoint) << 32) | (endpoint == KILN_CAMERA_LOCAL_VISUAL ? 0u : player);
    }
    Backend* backend(uint32_t endpoint) const { return endpoint >= 1 && endpoint <= 3 ? backends_[endpoint] : nullptr; }
    // capabilities() without a player: what apply/update check under the lock (no backend may call out there).
    uint64_t endpoint_caps(uint32_t endpoint) const;
    bool dead(KilnOwner owner) const { return dead_.count(owner) != 0; }
    uint32_t slots_of(uint32_t endpoint) const;

    // everything below: the lock is held
    void queue_done(World world, const Op& op, bool ok, uint32_t reason, uint32_t flags, uint32_t scope, uint32_t endpoint, uint32_t player, uint32_t epoch,
                    uint64_t revision, uint64_t frame, const char* text);
    void queue_refusal(World world, KilnOwner owner, KilnCameraDone done, void* user, KilnCameraLease lease, uint64_t revision, uint32_t reason,
                       const KilnCameraRequest& req, uint32_t epoch, const char* text);
    void queue_event(const Lease& lease, uint32_t kind, uint32_t end_reason, uint32_t reason_code, uint64_t frame, const char* text);
    void discard(std::unique_ptr<Op>& op);  // an operation whose owner is gone: no `done`
    void tombstone(const Lease& lease);
    void erase_lease(KilnCameraLease id);
    // Ends a holder (Active or Ending): its waiting work is answered, it becomes a tombstone, one ENDED is queued.
    void finish_holder(Lease& lease, uint32_t end_reason, uint32_t reason_code, uint64_t frame, const char* text, uint32_t pending_reason, bool silent_owner);
    void cleanup_owner(KilnOwner owner, World world);
    void stop_world(World world);
    void fault(KilnOwner owner, World world, const char* what);
    void deliver(World world);
    // The barrier (the lock is held for reserve): false when the owner is dead (the gate is closed).
    bool reserve(KilnOwner owner);
    void unreserve(KilnOwner owner);  // takes the lock
    uint32_t mine(KilnOwner owner) const;  // calls into `owner` this thread is inside

    Env* env_;
    Backend* backends_[4] = {nullptr, nullptr, nullptr, nullptr};
    std::atomic<bool> writes_{false};
    std::atomic<uint64_t> local_read_caps_{0};

    mutable std::mutex m_;
    uint32_t epoch_[kWorlds] = {0, 0};
    uint64_t next_lease_ = 1;
    uint64_t next_handle_ = 1;
    uint32_t pending_ = 0;
    std::map<KilnCameraLease, Lease> leases_;  // live, in the order they were made
    std::map<uint64_t, Slot> slots_;
    std::deque<Tomb> tombs_;
    std::vector<Subscriber> subs_;
    std::map<KilnOwner, Dead> dead_;
    std::deque<Delivery> queue_[kWorlds];
    std::map<KilnOwner, uint32_t> calls_;  // reserved callbacks per owner (owner 0, Kiln's own, is not counted)
    std::condition_variable drained_;      // a count went down
    std::atomic<uint32_t> retire_wait_ms_{2000};
};

}  // namespace kcam
#endif
