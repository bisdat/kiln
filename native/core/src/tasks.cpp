// SPDX-License-Identifier: MIT
// The kiln.tasks pool: see tasks.h. Worker threads run the model's dequeued work under the platform's
// fault boundary; bodies and done callbacks never run under the pool or model mutex, and the callbacks' modules stay
// referenced until their task is quiescent. The first structured native fault in an owner's callback quarantines that
// owner and retires the faulting OS thread; a replacement starts only after the old thread has exited.
//
// Locks: the pool mutex may be held while calling into the model (the model is a leaf that never calls out); nothing
// holds the model mutex and then takes the pool mutex. No callback runs under either.
#include "tasks.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace kiln::tasks {

namespace {
std::atomic<int> g_live_cores{0};
std::size_t slot_index_of(Slot s) { return s == Slot::Normal ? 0 : 1; }

uint32_t default_normal_workers() {
    const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    return std::clamp(hw / 4, 1u, 4u);
}
}  // namespace

// Per-accepted-task metadata beside the model's record: the callbacks' module references (held until done returns),
// the owner and a copy of the name (thread description, watchdog), and the watchdog clocks. Registered in the same
// pool critical section that publishes the task, so a worker can never see the task without it.
struct Meta {
    ModuleRef body, done;
    KilnOwner owner = 0;
    std::string name;
    uint64_t queued_since = 0;   // when it was accepted (the watchdog checks the model's state too)
    uint64_t running_since = 0;  // when its current body started (valid while `running`)
    bool running = false;
};

struct Worker {
    std::shared_ptr<Pool::Core> core;  // the thread co-owns the pool state
    Slot slot = Slot::Normal;
    uint32_t index = 0;
    uint64_t serial = 0;  // this incarnation, assigned by the pool: an OS thread id/handle value may be reused
    ThreadHandle thread{};
    bool faulted = false;  // guarded by core->mutex: set when this worker must retire
    bool maintenance = false;
};

// The PODs a callback runs with (GuardedHeader first): no destructor is live across the fault boundary.
struct Segment {
    GuardedHeader header;  // header.callback = the body address
    TaskRun run;
};
struct DoneCall {
    GuardedHeader header;  // header.callback = the done address
    TaskDoneFn done;
    KilnHandle task;
    uint32_t state;
    void* user;
};

struct Pool::Core : std::enable_shared_from_this<Pool::Core> {
    Core(PoolConfig c, Platform& p) : platform(p), model(c.limits), config(c) { g_live_cores.fetch_add(1); }
    ~Core() { g_live_cores.fetch_sub(1); }

    Platform& platform;
    Model model;
    PoolConfig config;
    uint32_t normal_workers = 0, long_running_workers = 0;

    mutable std::mutex mutex;  // wake generation, metadata, roster, retirement
    std::condition_variable work_cv, exit_cv, maintenance_cv;
    uint64_t generation = 0;
    bool stopping = false;

    std::unordered_map<KilnHandle, std::shared_ptr<Meta>> meta;
    std::vector<std::weak_ptr<Worker>> roster;  // one slot per logical worker (weak: threads own their Worker)
    // Faulted worker threads awaiting join by maintenance. Plain data, never a Worker: a Worker co-owns this Core, so
    // holding one here would be a reference cycle that leaks the Core when a fault lands during or after shutdown.
    struct Retired {
        ThreadHandle thread;
        uint64_t serial;
        Slot slot;
        uint32_t index;
    };
    std::deque<Retired> retired;
    struct Live {
        ThreadHandle thread;
        uint64_t serial;
    };
    std::vector<Live> live_threads;               // every started worker thread not yet joined (one join set)
    std::unordered_set<uint64_t> exited;          // serials of worker incarnations that returned from their loop
    uint64_t next_serial = 0;
    uint32_t healthy[2] = {0, 0};                 // started, not faulted workers per slot (Normal, LongRunning)
    std::vector<std::pair<Slot, uint32_t>> pending_starts;  // workers that could not start: retried by maintenance
    uint64_t start_failures = 0;
    ThreadHandle maintenance_thread{};
    bool maintenance_started = false;
    uint32_t stuck_workers = 0;
    std::function<void(Point)> hook;

    bool start_worker(Slot slot, uint32_t index);  // mutex held; false: queued in pending_starts for a retry
    void start_maintenance();                      // mutex held
    static void thread_entry(void* context);
    void worker_loop(Worker& self);
    bool run_one(Worker& self);
    void deliver_body(Worker& self, Work& work);
    void deliver_done(Worker& self, Work& work);
    void on_fault(Worker& self, KilnOwner owner);
    void maintenance_loop();
    void wake_all();
    void release_meta(KilnHandle task);
    std::shared_ptr<Meta> find_meta(KilnHandle task);
    void shutdown();
};

namespace {
thread_local Pool::Core* t_core = nullptr;   // the pool core whose worker this thread is, else nullptr
thread_local bool t_segment_armed = false;  // the running body armed a resumption (managed lease): it suspends

uint32_t KILN_CALL run_cancelled(const TaskRun* run) {
    // `internal` is the Work* of the running segment, valid only during the callback.
    const auto* work = static_cast<const Work*>(run->internal);
    return work && work->cancel_requested() ? 1 : 0;
}

void invoke_body(void* context) {
    auto* seg = static_cast<Segment*>(context);
    const auto body = reinterpret_cast<TaskBodyFn>(seg->header.callback);
    const auto* work = static_cast<const Work*>(seg->run.internal);
    body(&seg->run, reinterpret_cast<void*>(work->payload.user));
}

void invoke_done(void* context) {
    auto* call = static_cast<DoneCall*>(context);
    call->done(call->task, call->state, call->user);
}
}  // namespace

// ---- threads ----

bool Pool::Core::start_worker(Slot slot, uint32_t index) {
    auto w = std::make_shared<Worker>();
    w->core = shared_from_this();
    w->slot = slot;
    w->index = index;
    w->serial = ++next_serial;
    if (index >= roster.size()) roster.resize(index + 1);
    auto* context = new std::shared_ptr<Worker>(w);  // the thread's own reference, released when its loop returns
    w->thread = platform.start_thread(slot == Slot::Normal ? "kiln-task-N" : "kiln-task-L", &Core::thread_entry, context);
    if (w->thread.id == 0) {  // it does not exist: counted, and maintenance tries again (submit refuses meanwhile
        delete context;       // when no worker of the slot is live, so accepted work is never stranded silently)
        ++start_failures;
        pending_starts.emplace_back(slot, index);
        maintenance_cv.notify_all();
        return false;
    }
    live_threads.push_back({w->thread, w->serial});
    roster[index] = w;
    ++healthy[slot_index_of(slot)];
    return true;
}

void Pool::Core::start_maintenance() {
    auto w = std::make_shared<Worker>();
    w->core = shared_from_this();
    w->maintenance = true;
    auto* context = new std::shared_ptr<Worker>(w);
    maintenance_thread = platform.start_thread("kiln-task-maint", &Core::thread_entry, context);
    maintenance_started = maintenance_thread.id != 0;
    if (!maintenance_started) delete context;
}

void Pool::Core::thread_entry(void* context) {
    std::unique_ptr<std::shared_ptr<Worker>> own(static_cast<std::shared_ptr<Worker>*>(context));
    const std::shared_ptr<Worker> self = *own;
    const std::shared_ptr<Core> core = self->core;  // keeps the state alive for as long as this thread runs
    if (self->maintenance) core->maintenance_loop();
    else core->worker_loop(*self);
}

void Pool::Core::worker_loop(Worker& self) {
    t_core = this;
    platform.lower_current_priority();
    uint64_t seen = 0;
    bool done = false;
    while (!done) {
        {
            std::unique_lock lock(mutex);
            work_cv.wait(lock, [&] { return stopping || self.faulted || generation != seen; });
            if (stopping || self.faulted) done = true;
            seen = generation;
        }
        while (!done && run_one(self)) {  // drain the ready work for this slot before sleeping again
            std::lock_guard l(mutex);
            if (stopping || self.faulted) done = true;
            seen = generation;
        }
    }
    {
        std::lock_guard lock(mutex);
        exited.insert(self.serial);
    }
    exit_cv.notify_all();
    t_core = nullptr;
}

bool Pool::Core::run_one(Worker& self) {
    Work work;
    if (!model.dequeue(self.slot, &work)) return false;
    {
        // submit registers the metadata in the same critical section that publishes the task, so it is here
        std::lock_guard lock(mutex);
        const auto it = meta.find(work.task);
        if (it != meta.end() && work.kind != Work::Kind::Done) {
            it->second->running_since = platform.now_ns();
            it->second->running = true;
        }
    }
    if (work.kind == Work::Kind::Done) deliver_done(self, work);
    else deliver_body(self, work);
    return true;
}

std::shared_ptr<Meta> Pool::Core::find_meta(KilnHandle task) {
    std::lock_guard lock(mutex);
    const auto it = meta.find(task);
    return it == meta.end() ? nullptr : it->second;
}

void Pool::Core::deliver_body(Worker& self, Work& work) {
    const std::shared_ptr<Meta> m = find_meta(work.task);
    const std::string desc = model.owner_name(work.owner) + "/" + (m ? m->name : std::string());
    platform.set_description(desc.c_str());
    Segment seg{};
    seg.header.callback = reinterpret_cast<uintptr_t>(work.payload.body);
    seg.run.size = sizeof(TaskRun);
    seg.run.task = work.task;
    seg.run.owner = work.owner;
    seg.run.cancelled = &run_cancelled;
    seg.run.internal = &work;
    t_segment_armed = false;
    FaultInfo info{};
    if (work.payload.body) platform.guarded_call(&invoke_body, &seg, &info);
    const bool suspended = t_segment_armed;  // the body armed a resumption (managed lease): it is not terminal
    t_segment_armed = false;
    platform.set_description(self.slot == Slot::Normal ? "kiln-task-N" : "kiln-task-L");
    if (m) {
        std::lock_guard lock(mutex);
        m->running = false;
    }
    if (info.faulted) {
        model.segment_ended(work, false, Outcome::Faulted);  // publish Faulted, queue its done for a healthy worker
        on_fault(self, work.owner);
        wake_all();
        return;
    }
    // A native body returns void: it cannot say it stopped because of the cancel, so a normal return is Completed
    // (Completed wins over a late cancel). OwnTokenCancelled comes only from the managed host's explicit outcome.
    model.segment_ended(work, suspended, Outcome::Completed);
    wake_all();
}

void Pool::Core::deliver_done(Worker& self, Work& work) {
    DoneCall call{};
    call.header.callback = work.payload.done;
    call.done = reinterpret_cast<TaskDoneFn>(work.payload.done);
    call.task = work.task;
    call.state = static_cast<uint32_t>(work.final_state);
    call.user = reinterpret_cast<void*>(work.payload.user);
    FaultInfo info{};
    if (call.done) platform.guarded_call(&invoke_done, &call, &info);
    // the one done attempt has happened (faulted or not): quiescent; drop its metadata and module references
    model.done_returned(work);
    release_meta(work.task);
    wake_all();
    if (info.faulted) on_fault(self, work.owner);
}

void Pool::Core::on_fault(Worker& self, KilnOwner owner) {
    model.quarantine(owner);  // first fault: no new roots, queued roots cancelled
    std::lock_guard lock(mutex);
    self.faulted = true;  // this thread retires; maintenance joins it and starts a replacement
    for (auto& weak : roster) {
        if (auto w = weak.lock(); w.get() == &self) {
            retired.push_back({w->thread, w->serial, w->slot, w->index});
            --healthy[slot_index_of(w->slot)];
            weak.reset();
        }
    }
    maintenance_cv.notify_all();
}

void Pool::Core::maintenance_loop() {
    for (;;) {
        std::vector<Retired> to_join;
        std::vector<std::pair<Slot, uint32_t>> to_start;
        {
            std::unique_lock lock(mutex);
            const auto ready = [&] { return stopping || !retired.empty(); };
            // a worker that could not start is tried again after restart_retry_ns (or with the next retirement)
            if (pending_starts.empty()) maintenance_cv.wait(lock, ready);
            else maintenance_cv.wait_for(lock, std::chrono::nanoseconds(config.restart_retry_ns), ready);
            if (stopping && retired.empty()) return;
            for (const Retired& w : retired) {
                to_join.push_back(w);
                // out of the join set under the lock, so shutdown never joins it a second time
                live_threads.erase(std::remove_if(live_threads.begin(), live_threads.end(),
                                                  [&](const Live& l) { return l.serial == w.serial; }),
                                   live_threads.end());
                if (!stopping) to_start.emplace_back(w.slot, w.index);
            }
            retired.clear();
            if (!stopping) {
                to_start.insert(to_start.end(), pending_starts.begin(), pending_starts.end());
                pending_starts.clear();
            }
        }
        for (const Retired& w : to_join)
            if (w.thread.id) platform.join_thread(w.thread);  // replace only after the old thread has exited
        std::lock_guard lock(mutex);
        for (const Retired& w : to_join) exited.erase(w.serial);
        if (stopping) return;
        bool started = false;
        for (auto [slot, index] : to_start) started |= start_worker(slot, index);
        if (started) {
            ++generation;  // a new worker: let it look for work accepted while none could run
            work_cv.notify_all();
        }
    }
}

void Pool::Core::wake_all() {
    {
        std::lock_guard lock(mutex);
        ++generation;
    }
    work_cv.notify_all();
}

void Pool::Core::release_meta(KilnHandle task) {
    std::shared_ptr<Meta> m;
    {
        std::lock_guard lock(mutex);
        const auto it = meta.find(task);
        if (it == meta.end()) return;
        m = it->second;
        meta.erase(it);  // the watchdog clocks go with it: nothing outlives the task
    }
    platform.release_module(m->body);
    platform.release_module(m->done);
}

void Pool::Core::shutdown() {
    {
        std::lock_guard lock(mutex);
        stopping = true;
    }
    work_cv.notify_all();
    maintenance_cv.notify_all();
    if (maintenance_started) platform.join_thread(maintenance_thread);
    std::vector<Live> to_join;
    std::function<void(Point)> test_hook;
    {
        std::lock_guard lock(mutex);
        to_join.swap(live_threads);
        test_hook = hook;
    }
    if (test_hook) test_hook(Point::MaintenanceStopped);
    // A worker that returned from its loop is joined; one that will not exit within the bound is detached. It keeps
    // this Core alive through its own reference, so it never touches freed state if it resumes.
    // Exits are tracked per incarnation (serial), never by the OS id: a handle value of a joined worker may be reused.
    for (const Live& l : to_join) {
        if (!l.thread.id) continue;
        std::unique_lock lock(mutex);
        const bool exited_in_time = exit_cv.wait_for(lock, std::chrono::nanoseconds(config.shutdown_join_ns),
                                                     [&] { return exited.count(l.serial) != 0; });
        if (!exited_in_time) ++stuck_workers;
        lock.unlock();
        if (exited_in_time) platform.join_thread(l.thread);
        else platform.detach_thread(l.thread);
    }
}

// ---- Pool: forwards to the core ----

Pool::Pool(PoolConfig config, Platform* platform)
    : core_(std::make_shared<Core>(config, platform ? *platform : default_platform())) {
    Core& c = *core_;
    c.normal_workers = config.normal_workers ? config.normal_workers : default_normal_workers();
    c.long_running_workers = config.long_running_workers ? config.long_running_workers : config.limits.long_running_global;
    std::lock_guard lock(c.mutex);
    c.start_maintenance();
    uint32_t index = 0;
    for (uint32_t i = 0; i < c.normal_workers; ++i) c.start_worker(Slot::Normal, index++);
    for (uint32_t i = 0; i < c.long_running_workers; ++i) c.start_worker(Slot::LongRunning, index++);
}

Pool::~Pool() { core_->shutdown(); }

KilnResult Pool::open_owner(KilnOwner owner, std::string_view name) { return core_->model.open_owner(owner, name); }

KilnResult Pool::submit(KilnOwner owner, const SubmitArgs& in, TaskBodyFn body, TaskDoneFn done, void* user,
                        KilnHandle* task, Refusal* why) {
    Core& c = *core_;
    if (!body || !done) {
        if (task) *task = 0;
        if (why) *why = Refusal::Argument;
        return KILN_ERR_INVALID_ARGUMENT;
    }
    if (!can_run(in.long_running ? Slot::LongRunning : Slot::Normal)) {  // never accept work no worker can run
        if (task) *task = 0;
        if (why) *why = Refusal::Capability;
        return KILN_ERR_UNAVAILABLE;
    }
    // Module references before acceptance; a refused submit releases them (it transfers nothing).
    auto m = std::make_shared<Meta>();
    m->body = c.platform.acquire_module(reinterpret_cast<uintptr_t>(body));
    m->done = c.platform.acquire_module(reinterpret_cast<uintptr_t>(done));
    m->owner = owner;
    m->name = std::string(in.name);
    SubmitArgs args = in;
    args.payload = {reinterpret_cast<uintptr_t>(body), reinterpret_cast<uintptr_t>(done), reinterpret_cast<uintptr_t>(user)};
    KilnHandle handle = 0;
    KilnResult r = KILN_OK;
    {
        // Publish and register in ONE pool critical section: a worker that dequeues the task at once (from its drain
        // loop, without waiting for a wake) blocks on this mutex in run_one until the metadata is in place.
        std::lock_guard lock(c.mutex);
        r = c.model.submit(owner, args, &handle, why);
        if (r == KILN_OK) {
            if (c.hook) c.hook(Point::SubmitPublished);
            m->queued_since = c.platform.now_ns();
            c.meta.emplace(handle, m);
        }
    }
    if (r != KILN_OK) {
        c.platform.release_module(m->body);
        c.platform.release_module(m->done);
        if (task) *task = 0;
        return r;
    }
    if (task) *task = handle;
    c.wake_all();
    return KILN_OK;
}

KilnResult Pool::cancel(KilnOwner owner, KilnHandle task, State* state) {
    const KilnResult r = core_->model.cancel(owner, task, state);
    core_->wake_all();  // a queued cancellation produced a done delivery
    return r;
}

KilnResult Pool::status(KilnOwner owner, KilnHandle task, State* state) const {
    return core_->model.status(owner, task, state);
}

KilnResult Pool::release(KilnOwner owner, KilnHandle task) { return core_->model.release(owner, task); }

KilnResult Pool::arm(KilnHandle task, uint64_t* registration) {
    const KilnResult r = core_->model.arm(task, registration);
    if (r == KILN_OK) t_segment_armed = true;  // read by deliver_body when this body returns (same thread)
    return r;
}

KilnResult Pool::resume(KilnHandle task, uint64_t registration) {
    const KilnResult r = core_->model.resume(task, registration);
    core_->wake_all();
    return r;
}

void Pool::edge(uint64_t session, uint64_t edge) {
    core_->model.edge(session, edge);
    core_->wake_all();
}

void Pool::end_session(uint64_t session) {
    core_->model.end_session(session);
    core_->wake_all();  // the cancelled tasks' done deliveries
}

KilnResult Pool::quarantine(KilnOwner owner) {
    const KilnResult r = core_->model.quarantine(owner);
    core_->wake_all();
    return r;
}

KilnResult Pool::drain_owner(KilnOwner owner, uint64_t timeout_ns, uint32_t* outstanding) {
    if (outstanding) *outstanding = 0;
    if (is_pool_worker()) return KILN_ERR_WRONG_THREAD;  // any timeout: a body or done never drains (no self-drain)
    Core& c = *core_;
    uint32_t left = 0;
    const KilnResult sealed = c.model.seal(owner, &left);
    c.wake_all();
    if (sealed != KILN_OK && sealed != KILN_ERR_REVOKED) {
        if (outstanding) *outstanding = left;
        return sealed;
    }
    if (timeout_ns > 0) {
        const uint64_t deadline = c.platform.now_ns() + timeout_ns;
        while (left > 0 && c.platform.now_ns() < deadline) {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
            c.model.outstanding(owner, &left);
        }
    }
    if (outstanding) *outstanding = left;
    return KILN_OK;
}

KilnResult Pool::retire_owner(KilnOwner owner, uint32_t* outstanding) {
    const KilnResult r = core_->model.retire(owner, outstanding);
    core_->wake_all();
    return r;
}

void Pool::run_watchdog(WatchFn fn, void* user) {
    Core& c = *core_;
    std::vector<Watch> reports;
    {
        std::lock_guard lock(c.mutex);
        const uint64_t now = c.platform.now_ns();
        for (const auto& [task, m] : c.meta) {
            // by handle, not the owner-scoped public status: a body that outlives its retired owner is still named
            const State s = c.model.diag_state(task);  // pool -> model lock order; the model never calls back
            if (s == State::Running && m->running && now - m->running_since >= c.config.running_watchdog_ns)
                reports.push_back({Watch::Kind::RunningTooLong, m->owner, task, m->name, now - m->running_since});
            else if (s == State::Queued && now - m->queued_since >= c.config.queued_watchdog_ns)
                reports.push_back({Watch::Kind::QueuedTooLong, m->owner, task, m->name, now - m->queued_since});
        }
    }
    for (const Watch& r : reports) fn(r, user);  // outside every lock
}

Counts Pool::counts() const { return core_->model.counts(); }
std::size_t Pool::tracked_tasks() const {
    std::lock_guard lock(core_->mutex);
    return core_->meta.size();
}
bool Pool::is_pool_worker() const { return t_core == core_.get(); }
uint32_t Pool::normal_workers() const { return core_->normal_workers; }
bool Pool::can_run(Slot slot) const {
    std::lock_guard lock(core_->mutex);
    const Core& c = *core_;
    // a LongRunning task's done runs on a normal worker
    return c.maintenance_started && c.healthy[0] > 0 && (slot == Slot::Normal || c.healthy[1] > 0);
}
uint64_t Pool::start_failures() const {
    std::lock_guard lock(core_->mutex);
    return core_->start_failures;
}
uint32_t Pool::long_running_workers() const { return core_->long_running_workers; }
void Pool::set_test_hook(std::function<void(Point)> hook) {
    std::lock_guard lock(core_->mutex);
    core_->hook = std::move(hook);
}
int Pool::live_cores() { return g_live_cores.load(); }

}  // namespace kiln::tasks
