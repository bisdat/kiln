// SPDX-License-Identifier: MIT
// The kiln.tasks pool: real Kiln-owned worker threads that run the work the model (tasks_model.h) hands
// out, each body and done guarded by the platform's fault boundary, with module references held for the callbacks'
// lifetime, first-fault owner quarantine with OS-thread retirement, LongRunning workers, a watchdog clock and off-tick
// maintenance joins. services/tasks.cpp binds the public kiln.tasks table to this pool; the managed host's leases run
// on it too. No user callback ever runs under the pool/model mutex.
//
// Lifetime: all pool state lives in a heap Core that the Pool and every worker thread co-own. A worker that will not
// exit at shutdown (an uncatchable stack overflow under Wine) is detached after a bound and keeps the Core alive, so it
// never touches freed state if it ever resumes; the Core is freed when that thread exits, or never if it does not
// (leaked by design). Nothing in the Core refers back to a worker, so there is no cycle. The Platform must outlive the
// Core. The production pool lives for the whole process, so this only matters at exit.
//
// Done deliveries count toward an owner's normal running cap (2), as in the model: an owner whose two normal bodies
// never return also holds back its queued-cancelled dones, so its drain reports outstanding > 0 until the timeout.
// That is bounded and reported (the watchdog names the long-running bodies), not a leak.
//
// A body that never returns (a stuck mod, or a stack overflow the platform cannot contain) holds only its own owner's
// running slot: other owners keep running, the watchdog names it, and shutdown detaches its worker within the bound.
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include "kiln_api.h"
#include "tasks_model.h"
#include "tasks_platform.h"

namespace kiln::tasks {

// The pool-facing body/done callback shapes (the public kiln_tasks.h mirrors these exactly; private here, no ABI).
struct TaskRun {
    uint32_t size, reserved;
    KilnHandle task;
    KilnOwner owner;
    uint32_t(KILN_CALL* cancelled)(const TaskRun*);  // poll, any time during the body
    void* internal;                                  // the pool's per-segment context; never a game pointer
};
using TaskBodyFn = void(KILN_CALL*)(const TaskRun* run, void* user);
using TaskDoneFn = void(KILN_CALL*)(KilnHandle task, uint32_t state, void* user);

struct PoolConfig {
    Limits limits;
    uint32_t normal_workers = 0;        // 0 = clamp(cores / 4, 1, 4)
    uint32_t long_running_workers = 0;  // 0 = limits.long_running_global
    uint64_t running_watchdog_ns = 30ull * 1000 * 1000 * 1000;
    uint64_t queued_watchdog_ns = 30ull * 1000 * 1000 * 1000;
    uint64_t shutdown_join_ns = 2ull * 1000 * 1000 * 1000;  // per worker at shutdown; past it the worker is detached
    uint64_t restart_retry_ns = 1ull * 1000 * 1000 * 1000;  // a worker that could not start is tried again after this
};

// Watchdog reports (delivered to an injected sink; services/tasks.cpp logs them). Report only, never a kill.
struct Watch {
    enum class Kind : uint32_t { RunningTooLong, QueuedTooLong };
    Kind kind;
    KilnOwner owner;
    KilnHandle task;
    std::string name;
    uint64_t age_ns;
};
using WatchFn = void (*)(const Watch&, void* user);

class Pool {
public:
    explicit Pool(PoolConfig config = {}, Platform* platform = nullptr);
    ~Pool();  // stops the pool; joins every worker within the bound, detaches one that will not exit
    Pool(const Pool&) = delete;
    Pool& operator=(const Pool&) = delete;

    // ---- owners ----
    KilnResult open_owner(KilnOwner owner, std::string_view name);
    KilnResult quarantine(KilnOwner owner);
    // drain_owner: seal, cancel queued, signal running, then wait up to timeout_ns for quiescence. Refused
    // (WRONG_THREAD) on a pool worker for any timeout, so a body or done never drains (the binding adds the tick/world rule).
    // *outstanding = body + done still in flight (0 = quiescent).
    KilnResult drain_owner(KilnOwner owner, uint64_t timeout_ns, uint32_t* outstanding);
    KilnResult retire_owner(KilnOwner owner, uint32_t* outstanding);

    // ---- task operations (reentrant: safe to call from inside a body or done) ----
    KilnResult submit(KilnOwner owner, const SubmitArgs& args, TaskBodyFn body, TaskDoneFn done, void* user,
                      KilnHandle* task, Refusal* why);
    KilnResult cancel(KilnOwner owner, KilnHandle task, State* state);
    KilnResult status(KilnOwner owner, KilnHandle task, State* state) const;
    KilnResult release(KilnOwner owner, KilnHandle task);

    // ---- managed leases (the managed host's tasks) ----
    KilnResult arm(KilnHandle task, uint64_t* registration);
    KilnResult resume(KilnHandle task, uint64_t registration);
    void edge(uint64_t session, uint64_t edge);
    void end_session(uint64_t session);

    // A watchdog sweep against the platform clock: one report per task over its age, with owner and name, delivered
    // to `fn` outside every lock. Gated on the model's real state, so a cancelled or finished task never reports.
    void run_watchdog(WatchFn fn, void* user);

    // Diagnostics / test seams.
    Counts counts() const;
    std::size_t tracked_tasks() const;  // tasks whose metadata (module references, watchdog clocks) is still held
    bool is_pool_worker() const;  // the calling thread is one of this pool's workers
    uint32_t normal_workers() const;
    uint32_t long_running_workers() const;
    // Whether work of `slot` can run now: the maintenance thread started and at least one worker of that slot (and,
    // for LongRunning, also a normal worker for its done) is live. submit refuses CAPABILITY otherwise (the
    // tasks.pool capability row reads it). start_failures() counts worker starts that failed (each retried later).
    bool can_run(Slot slot) const;
    uint64_t start_failures() const;
    enum class Point : uint32_t {
        SubmitPublished,     // inside submit, after the model published, before it returns (under the pool mutex)
        MaintenanceStopped,  // inside shutdown, after the maintenance thread was joined, before the workers are
    };
    void set_test_hook(std::function<void(Point)> hook);
    static int live_cores();  // pool cores not yet freed (a detached worker keeps its core alive)

    struct Core;  // all state; co-owned by the Pool and its worker threads

private:
    std::shared_ptr<Core> core_;
};

}  // namespace kiln::tasks
