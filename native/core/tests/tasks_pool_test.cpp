// SPDX-License-Identifier: MIT
// kiln.tasks pool (src/tasks.cpp) over the host test platform (tasks_platform_host.h): real worker
// threads run bodies off the caller thread, module references live until done returns, a faulting callback quarantines
// its owner and retires its OS thread while other owners keep running, drains are off-tick only, suspension frees a
// worker, one LongRunning runner serialises a queue, and the watchdog reports against an injected
// clock. Built twice by test-tasks.sh (ASan/UBSan and TSan); `tasks_pool_test <name>` runs one test.
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <functional>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <vector>

#include "tasks.h"
#include "tasks_platform_host.h"
#include "tasks_test_support.h"

using namespace kiln::tasks;
using kiln::tasks::test::check;
using kiln::tasks::test::Gate;
using kiln::tasks::test::HostPlatform;

// default_platform() is never called by these tests (each passes an explicit HostPlatform); it only has to link.
// Immortal like the production one.
namespace kiln::tasks {
Platform& default_platform() {
    static auto* host = new HostPlatform;
    return *host;
}
}  // namespace kiln::tasks

namespace {

// The shared `user` of a task: body and done both receive it.
struct Job {
    HostPlatform* platform = nullptr;
    Pool* pool = nullptr;
    std::atomic<int> body_runs{0};
    std::atomic<uint64_t> body_thread{0};
    Gate release;  // the body returns when this is open
    std::atomic<bool> arm_on_run{false};
    std::atomic<uint64_t> registration{0};
    std::atomic<int> done_calls{0};
    std::atomic<uint32_t> done_state{0};
    std::atomic<uint64_t> done_thread{0};
    std::function<void(const TaskRun*)> extra;  // optional extra work inside the body, before it blocks
    std::function<void()> after_release;        // optional, after the body is released and before it returns
};

void KILN_CALL job_body(const TaskRun* run, void* user) {
    auto* j = static_cast<Job*>(user);
    j->body_thread.store(j->platform->current_thread_id());
    j->body_runs.fetch_add(1);
    if (j->arm_on_run.load() && j->pool) {
        uint64_t reg = 0;
        j->pool->arm(run->task, &reg);
        j->registration.store(reg);
    }
    if (j->extra) j->extra(run);
    j->release.wait();
    if (j->after_release) j->after_release();
}

void KILN_CALL job_done(KilnHandle, uint32_t state, void* user) {
    auto* j = static_cast<Job*>(user);
    j->done_thread.store(j->platform->current_thread_id());
    j->done_state.store(state);
    j->done_calls.fetch_add(1);
}

SubmitArgs named(const char* name, Priority p = Priority::Normal, bool long_running = false) {
    SubmitArgs a;
    a.name = name;
    a.priority = p;
    a.long_running = long_running;
    return a;
}

template <class F>
bool until(F pred, int ms = 5000) {
    for (int i = 0; i < ms * 10 && !pred(); ++i) std::this_thread::sleep_for(std::chrono::microseconds(100));
    return pred();
}

PoolConfig config(uint32_t normal, uint32_t long_running = 2) {
    PoolConfig c;
    c.normal_workers = normal;
    c.long_running_workers = long_running;
    c.limits.owner_capacity = 128;
    return c;
}

KilnHandle submit(Pool& pool, KilnOwner owner, const SubmitArgs& a, Job& job) {
    KilnHandle h = 0;
    Refusal why = Refusal::None;
    check(pool.submit(owner, a, &job_body, &job_done, &job, &h, &why) == KILN_OK && h != 0,
          std::string("submit ") + std::string(a.name));
    return h;
}

// ---------------------------------------------------------------------------------------------------------------
// Bodies run on worker threads, not the submitter; idle descriptions restored; module refs held until done returns.
KILN_TASKS_TEST(runs_off_thread) {
    HostPlatform platform;
    Pool pool(config(2), &platform);
    pool.open_owner(1, "mod.one");
    Job job;
    job.platform = &platform;  // the body blocks until released, so the references can be observed in flight
    const KilnHandle h = submit(pool, 1, named("work"), job);
    check(until([&] { return job.body_runs.load() == 1; }), "the body is running");
    check(platform.module_refcount(reinterpret_cast<uintptr_t>(&job_body)) >= 1 &&
              platform.module_refcount(reinterpret_cast<uintptr_t>(&job_done)) >= 1,
          "the body's and done's module references are held while the task is in flight");
    job.release.open();
    check(until([&] { return job.done_calls.load() == 1; }), "the done callback ran");
    check(job.body_runs.load() == 1 && job.body_thread.load() != 0 && job.body_thread.load() != platform.current_thread_id(),
          "the body ran once on a worker thread, not the submitter");
    check(job.done_state.load() == static_cast<uint32_t>(State::Completed), "done saw Completed");
    check(job.done_thread.load() != 0 && job.done_thread.load() != platform.current_thread_id(),
          "done also ran on a worker, not the submitter");
    // the worker releases them just after the done returned (it counted the done first): wait for that, not timing
    check(until([&] {
        return platform.module_refcount(reinterpret_cast<uintptr_t>(&job_body)) == 0 &&
               platform.module_refcount(reinterpret_cast<uintptr_t>(&job_done)) == 0;
    }), "module references released after done returned");
    check(platform.acquires() == 2 && platform.releases() == 2, "every acquire was released");
    State s = State::None;
    check(pool.status(1, h, &s) == KILN_OK && s == State::Completed && pool.release(1, h) == KILN_OK, "terminal, released");
    check(platform.lowered() >= 2, "workers run below normal");
    bool active = false;
    for (auto& [id, text] : platform.description_log()) active |= text == "mod.one/work";
    check(active, "the worker described itself <mod>/<task> while running");
}

// A refused submit transfers no module reference.
KILN_TASKS_TEST(refused_submit_keeps_no_reference) {
    HostPlatform platform;
    Pool pool(config(1), &platform);
    pool.open_owner(1, "a");
    Job job;
    job.platform = &platform;
    KilnHandle h = 0;
    Refusal why = Refusal::None;
    check(pool.submit(2, named("w"), &job_body, &job_done, &job, &h, &why) == KILN_ERR_REVOKED, "unknown owner refused");
    check(platform.acquires() == platform.releases(), "a refused submit releases the references it took");
    check(pool.submit(1, named("w"), nullptr, &job_done, &job, &h, &why) == KILN_ERR_INVALID_ARGUMENT &&
              why == Refusal::Argument,
          "a null body refuses");
    check(pool.submit(1, named("w"), &job_body, nullptr, &job, &h, &why) == KILN_ERR_INVALID_ARGUMENT, "a null done refuses");
    check(platform.acquires() == platform.releases(), "and neither took a net reference");
}

// Per-owner running cap and worker use: at most 2 bodies of one owner run at once; all workers are used across owners.
KILN_TASKS_TEST(fairness_and_caps) {
    HostPlatform platform;
    PoolConfig c = config(4, 3);
    c.limits.owner_running = 2;
    c.limits.long_running_per_owner = 2;
    c.limits.long_running_global = 3;
    Pool pool(c, &platform);
    for (KilnOwner o : {1, 2, 3}) pool.open_owner(o, "o");
    std::vector<std::unique_ptr<Job>> jobs;
    std::atomic<int> running_per_owner[4] = {};
    std::atomic<int> max_per_owner[4] = {};
    for (KilnOwner o : {1, 2, 3})
        for (int i = 0; i < 4; ++i) {
            auto j = std::make_unique<Job>();
            j->platform = &platform;
            j->extra = [o, &running_per_owner, &max_per_owner](const TaskRun*) {
                const int now = ++running_per_owner[o];
                int prev = max_per_owner[o].load();
                while (now > prev && !max_per_owner[o].compare_exchange_weak(prev, now)) {
                }
            };
            j->after_release = [o, &running_per_owner] { --running_per_owner[o]; };
            KilnHandle h = 0;
            Refusal why = Refusal::None;
            check(pool.submit(o, named("t"), &job_body, &job_done, j.get(), &h, &why) == KILN_OK, "submit");
            jobs.push_back(std::move(j));
        }
    // let the bodies pile up, then release everything; each body decrements on return via a wrapper
    // bodies block (not released): once four are blocked, every started body is still counted in running_per_owner,
    // so the per-owner maximum reflects true concurrency. The cap holds it to 2 per owner even with 4 free workers.
    check(until([&] {
        int busy = 0;
        for (KilnOwner o : {1, 2, 3}) busy += running_per_owner[o].load();
        return busy >= 4;
    }), "four bodies run at once across owners");
    for (KilnOwner o : {1, 2, 3}) check(max_per_owner[o].load() <= 2, "at most two bodies of one owner ran at once");
    for (auto& j : jobs) j->release.open();
    check(until([&] { return pool.counts().outstanding == 0; }), "all complete");
    for (KilnOwner o : {1, 2, 3}) check(max_per_owner[o].load() <= 2, "the per-owner cap held throughout");
}

// drain_owner with a timeout is refused on a pool worker; off-tick it returns within the budget.
KILN_TASKS_TEST(drain_thread_rules) {
    HostPlatform platform;
    Pool pool(config(2), &platform);
    pool.open_owner(1, "a");
    std::atomic<KilnResult> from_worker{KILN_OK};
    std::atomic<bool> ran{false};
    Job job;
    job.platform = &platform;
    job.release.open();
    job.extra = [&](const TaskRun*) {
        uint32_t out = 0;
        from_worker.store(pool.drain_owner(2, 1000, &out));
        ran.store(true);
    };
    submit(pool, 1, named("w"), job);
    check(until([&] { return ran.load(); }), "the body ran");
    check(from_worker.load() == KILN_ERR_WRONG_THREAD, "drain with a timeout is refused on a pool worker");
    check(until([&] { return pool.counts().outstanding == 0; }), "quiescent");
    pool.open_owner(3, "c");
    uint32_t out = 7;
    check(pool.drain_owner(3, 1000, &out) == KILN_OK && out == 0, "off-tick drain of an idle owner returns 0");
}

// A faulting body quarantines its owner, retires its worker thread; another owner keeps running on a fresh thread.
KILN_TASKS_TEST(fault_quarantines_owner) {
    HostPlatform platform;
    Job faulter, other, queued;  // before the pool: the test ends on a state, while `other`'s done may still run
    faulter.platform = other.platform = queued.platform = &platform;
    Pool pool(config(2), &platform);
    pool.open_owner(1, "bad");
    pool.open_owner(2, "good");
    platform.fault_next_call_of(reinterpret_cast<uintptr_t>(&job_body));
    faulter.release.open();
    other.release.open();
    const KilnHandle fh = submit(pool, 1, named("faulter"), faulter);
    check(until([&] {
        State s = State::None;
        pool.status(1, fh, &s);
        return s == State::Faulted;
    }), "the faulting task becomes Faulted");
    KilnHandle qh = 0;
    Refusal why = Refusal::None;
    check(pool.submit(1, named("after"), &job_body, &job_done, &queued, &qh, &why) == KILN_ERR_UNAVAILABLE &&
              why == Refusal::OwnerQuarantined,
          "the quarantined owner refuses new roots");
    const KilnHandle oh = submit(pool, 2, named("other"), other);
    check(until([&] {
        State s = State::None;
        pool.status(2, oh, &s);
        return s == State::Completed;
    }), "another owner keeps running after the fault, on a replacement worker, and completes");
    check(until([&] {
        uint32_t out = 1;
        pool.drain_owner(1, 0, &out);
        return out == 0;
    }), "the faulted task's done was still delivered; the owner reaches quiescence");
}

// The faulting worker's OS thread is retired and replaced: later work runs on a different worker id.
KILN_TASKS_TEST(fault_retires_worker) {
    HostPlatform platform;
    Job first, faulter, after;  // before the pool: the faulter's done may still run when the test ends
    first.platform = faulter.platform = after.platform = &platform;
    Pool pool(config(1), &platform);  // one normal worker, so "a different id" means the thread was replaced
    pool.open_owner(1, "a");
    pool.open_owner(2, "bad");
    // a first task establishes the worker id
    first.release.open();
    const KilnHandle fh = submit(pool, 1, named("first"), first);
    check(until([&] { return first.done_calls.load() == 1; }), "the first task completed");
    const uint64_t worker_before = first.body_thread.load();
    check(worker_before != 0, "the first task ran on a worker");
    pool.release(1, fh);
    // the next body faults: its owner is quarantined and that worker thread retires
    platform.fault_next_call_of(reinterpret_cast<uintptr_t>(&job_body));
    KilnHandle bh = 0;
    Refusal why = Refusal::None;
    pool.submit(2, named("faulter"), &job_body, &job_done, &faulter, &bh, &why);
    check(until([&] {
        State s = State::None;
        pool.status(2, bh, &s);
        return s == State::Faulted;
    }), "the faulting task is Faulted");
    // a later task runs on the replacement worker, a different OS thread id
    after.release.open();
    const KilnHandle ah = submit(pool, 1, named("after"), after);
    check(until([&] { return after.done_calls.load() == 1; }), "a later task completed on the single-worker pool");
    check(after.body_thread.load() != 0 && after.body_thread.load() != worker_before,
          "it ran on a fresh worker thread (the faulted one was retired and replaced)");
    (void)ah;
}

// A done callback that faults: the one attempt is recorded, pins released, the worker retired.
KILN_TASKS_TEST(fault_in_done) {
    HostPlatform platform;
    Pool pool(config(2), &platform);
    pool.open_owner(1, "a");
    platform.fault_next_call_of(reinterpret_cast<uintptr_t>(&job_done));
    Job job;
    job.platform = &platform;
    job.release.open();
    const KilnHandle h = submit(pool, 1, named("w"), job);
    check(until([&] {
        uint32_t out = 1;
        pool.drain_owner(1, 0, &out);
        return out == 0;
    }), "a faulting done still reaches quiescence (one attempt)");
    // quiescence is counted before the worker releases the pins: wait for the release itself
    check(until([&] {
        return platform.module_refcount(reinterpret_cast<uintptr_t>(&job_body)) == 0 &&
               platform.module_refcount(reinterpret_cast<uintptr_t>(&job_done)) == 0;
    }), "pins released after the faulting done");
    (void)h;
}

// The watchdog reports a running task and a queued task past their ages, against the injected clock. Report only.
KILN_TASKS_TEST(watchdog) {
    HostPlatform platform;
    PoolConfig c = config(1);
    c.running_watchdog_ns = 1000;
    c.queued_watchdog_ns = 1000;
    Pool pool(c, &platform);
    pool.open_owner(1, "a");
    Job running, queued;
    running.platform = queued.platform = &platform;  // neither released: the worker stays on `running`
    const KilnHandle rh = submit(pool, 1, named("running"), running);
    check(until([&] { return running.body_runs.load() == 1; }), "the first task is running");
    const KilnHandle qh = submit(pool, 1, named("queued"), queued);
    platform.advance(5000);
    std::vector<Watch> reports;
    pool.run_watchdog(+[](const Watch& w, void* user) { static_cast<std::vector<Watch>*>(user)->push_back(w); }, &reports);
    bool saw_running = false, saw_queued = false;
    for (const Watch& w : reports) {
        saw_running |= w.kind == Watch::Kind::RunningTooLong && w.task == rh;
        saw_queued |= w.kind == Watch::Kind::QueuedTooLong && w.task == qh;
    }
    check(saw_running && saw_queued, "the watchdog reports the over-age running and queued tasks");
    running.release.open();
    queued.release.open();
    check(until([&] { return pool.counts().outstanding == 0; }), "drain");
}

// A managed-style suspend: the body arms a resumption, suspends (freeing the worker), the continuation runs on resume.
KILN_TASKS_TEST(suspend_frees_worker) {
    HostPlatform platform;
    Job lease, other;  // before the pool: the test ends on Completed, while their dones may still run
    lease.platform = other.platform = &platform;
    Pool pool(config(1), &platform);  // a single worker
    pool.open_owner(1, "a");
    lease.pool = &pool;
    lease.arm_on_run.store(true);
    lease.release.open();
    other.release.open();
    const KilnHandle lh = submit(pool, 1, named("lease"), lease);
    check(until([&] { return lease.body_runs.load() == 1 && lease.registration.load() != 0; }), "the lease ran and armed");
    const KilnHandle oh = submit(pool, 1, named("other"), other);
    check(until([&] {
        State s = State::None;
        pool.status(1, oh, &s);
        return s == State::Completed;
    }), "another task runs while the lease is suspended (the single worker was freed)");
    State sl = State::None;
    pool.status(1, lh, &sl);
    check(sl == State::Running, "the suspended lease is still Running");
    lease.arm_on_run.store(false);
    check(pool.resume(lh, lease.registration.load()) == KILN_OK, "resume");
    check(until([&] {
        State s = State::None;
        pool.status(1, lh, &s);
        return s == State::Completed;
    }), "the continuation ran and the lease completed");
}

// One LongRunning runner under one owner consumes a serial queue; jobs never overlap and keep order.
KILN_TASKS_TEST(single_long_running_runner) {
    HostPlatform platform;
    Pool pool(config(2, 4), &platform);  // several LongRunning workers available; only one runner is submitted
    pool.open_owner(100, "kiln.saves");
    struct Runner {
        std::mutex mutex;
        std::condition_variable cv;
        std::deque<int> queue;
        bool stop = false;
        std::vector<int> order;
        std::atomic<int> inside{0}, max_inside{0}, starts{0};
    } runner;
    struct User {
        Runner* runner;
    } user{&runner};
    auto runner_body = +[](const TaskRun* run, void* u) {
        auto* r = static_cast<User*>(u)->runner;
        r->starts.fetch_add(1);
        for (;;) {
            int job = -1;
            {
                std::unique_lock lock(r->mutex);
                r->cv.wait(lock, [&] { return r->stop || !r->queue.empty() || run->cancelled(run); });
                if (run->cancelled(run) && r->queue.empty()) return;
                if (r->queue.empty() && r->stop) return;
                if (r->queue.empty()) continue;
                job = r->queue.front();
                r->queue.pop_front();
            }
            const int now = r->inside.fetch_add(1) + 1;
            int prev = r->max_inside.load();
            while (now > prev && !r->max_inside.compare_exchange_weak(prev, now)) {
            }
            {
                std::lock_guard lock(r->mutex);  // order is read by the checker under the same lock
                r->order.push_back(job);
            }
            r->inside.fetch_sub(1);
        }
    };
    auto runner_done = +[](KilnHandle, uint32_t, void*) {};
    KilnHandle rh = 0;
    Refusal why = Refusal::None;
    check(pool.submit(100, named("SaveWriter", Priority::Normal, true), runner_body, runner_done, &user, &rh, &why) == KILN_OK,
          "the single SaveWriter runner is admitted as LongRunning");
    check(until([&] { return runner.starts.load() == 1; }), "exactly one runner started");
    for (int i = 0; i < 50; ++i) {
        std::lock_guard lock(runner.mutex);
        runner.queue.push_back(i);
        runner.cv.notify_all();
    }
    check(until([&] {
        std::lock_guard lock(runner.mutex);
        return runner.order.size() == 50;
    }), "every job ran");
    check(runner.starts.load() == 1, "still exactly one runner (no second start, no parallel runner)");
    check(runner.max_inside.load() == 1, "jobs never overlap: the single runner serialises them");
    std::vector<int> want(50);
    for (int i = 0; i < 50; ++i) want[i] = i;
    bool ordered;
    {
        std::lock_guard lock(runner.mutex);
        ordered = runner.order == want;
    }
    check(ordered, "jobs ran in strict enqueue order");
    State s = State::None;
    pool.cancel(100, rh, &s);  // Stop: signal cancellation; the runner drains and exits
    {
        std::lock_guard lock(runner.mutex);
        runner.stop = true;
        runner.cv.notify_all();
    }
    check(until([&] { return pool.counts().outstanding == 0; }), "the runner exits and the task is quiescent");
}

// Reentrancy: a body submits, queries, cancels and releases without deadlocking the pool.
KILN_TASKS_TEST(reentrant_from_body) {
    HostPlatform platform;
    Pool pool(config(2), &platform);
    pool.open_owner(1, "a");
    std::atomic<bool> ok{false};
    Job job;
    job.platform = &platform;
    job.release.open();
    job.extra = [&](const TaskRun* run) {
        KilnHandle inner = 0;
        Refusal why = Refusal::None;
        auto ib = +[](const TaskRun*, void*) {};
        auto id = +[](KilnHandle, uint32_t, void*) {};
        SubmitArgs a;
        a.name = "inner";
        const bool submitted = pool.submit(run->owner, a, ib, id, nullptr, &inner, &why) == KILN_OK;
        State s = State::None;
        pool.status(run->owner, inner, &s);
        pool.cancel(run->owner, run->task, &s);  // cancel self: a running task is unaffected but must not deadlock
        ok.store(submitted);
    };
    submit(pool, 1, named("outer"), job);
    check(until([&] { return ok.load(); }), "a body submitted/queried/cancelled reentrantly without deadlock");
    check(until([&] { return pool.counts().outstanding == 0; }), "everything quiesced");
}

// ---- publication races, quiescence, shutdown and stack-overflow conditions ----

// a worker that dequeues a task the instant it is published (before submit registered its metadata) must still
// find the metadata: no leaked module references, the right name, no phantom watchdog entry.
KILN_TASKS_TEST(publish_before_metadata) {
    HostPlatform platform;
    PoolConfig c = config(1);
    c.queued_watchdog_ns = 1000;
    c.running_watchdog_ns = 1000000000000ull;
    Pool pool(c, &platform);
    pool.open_owner(1, "mod.f1");
    // keep the single worker inside its drain loop (running a blocker), so it picks up the next task without waiting
    // for submit's wake: that is the window
    Job blocker;
    blocker.platform = &platform;
    submit(pool, 1, named("blocker"), blocker);
    check(until([&] { return blocker.body_runs.load() == 1; }), "the worker is busy in its drain loop");
    Job job;
    job.platform = &platform;
    job.release.open();
    // hold the submitter right after the model published the task; free the worker, which then dequeues the new task
    // straight from its drain loop and runs it as far as it can
    pool.set_test_hook([&](Pool::Point p) {
        if (p != Pool::Point::SubmitPublished) return;
        blocker.release.open();
        until([&] { return job.done_calls.load() == 1; }, 500);  // with the defect, body and done finish here
    });
    submit(pool, 1, named("racer"), job);
    pool.set_test_hook(nullptr);
    check(until([&] { return job.done_calls.load() == 1; }), "the task completed");
    check(until([&] { return pool.counts().outstanding == 0; }), "quiescent");
    // quiescence is counted before the worker releases the references: wait for the release itself
    check(until([&] {
        return platform.module_refcount(reinterpret_cast<uintptr_t>(&job_body)) == 0 &&
               platform.module_refcount(reinterpret_cast<uintptr_t>(&job_done)) == 0 &&
               platform.acquires() == platform.releases();
    }), "no module reference leaks when the worker wins the race");
    bool named_right = false;
    for (auto& [id, text] : platform.description_log()) named_right |= text == "mod.f1/racer";
    check(named_right, "the worker described the task with its real name");
    platform.advance(10000);
    std::vector<Watch> reports;
    pool.run_watchdog(+[](const Watch& w, void* user) { static_cast<std::vector<Watch>*>(user)->push_back(w); }, &reports);
    check(reports.empty(), "no phantom queued-too-long entry for a finished task");
}

// a task cancelled while queued (cancel, seal) never reports queued-too-long: not while its done is still pending
// (the watchdog checks the model's real state), and not after it is quiescent (its metadata is gone).
KILN_TASKS_TEST(cancelled_queued_no_watchdog) {
    HostPlatform platform;
    PoolConfig c = config(2);
    c.queued_watchdog_ns = 1000;
    c.running_watchdog_ns = 1000000000000ull;
    c.limits.owner_running = 1;  // one at a time per owner: tasks queue behind a blocker, and their dones wait too
    Pool pool(c, &platform);
    pool.open_owner(1, "a");
    pool.open_owner(2, "b");
    Job blocker, by_cancel, b_blocker, by_seal;
    blocker.platform = by_cancel.platform = b_blocker.platform = by_seal.platform = &platform;
    by_cancel.release.open();
    by_seal.release.open();
    submit(pool, 1, named("blocker"), blocker);
    submit(pool, 2, named("b-blocker"), b_blocker);
    check(until([&] { return blocker.body_runs.load() == 1 && b_blocker.body_runs.load() == 1; }), "both blockers run");
    const KilnHandle ch = submit(pool, 1, named("cancelled"), by_cancel);
    State s = State::None;
    check(pool.cancel(1, ch, &s) == KILN_OK && s == State::Cancelled, "cancelled while queued");
    submit(pool, 2, named("sealed"), by_seal);
    uint32_t out = 0;
    pool.drain_owner(2, 0, &out);  // seal: cancels the queued one
    // both dones are still pending (each owner's one slot is busy): the watchdog must not call them queued
    platform.advance(10000);
    std::vector<Watch> pending;
    pool.run_watchdog(+[](const Watch& w, void* user) { static_cast<std::vector<Watch>*>(user)->push_back(w); }, &pending);
    bool reported_cancelled = false;
    for (const Watch& w : pending) reported_cancelled |= w.kind == Watch::Kind::QueuedTooLong;
    check(!reported_cancelled, "a cancelled task with its done still pending is not reported queued");
    blocker.release.open();
    b_blocker.release.open();
    check(until([&] { return by_cancel.done_calls.load() == 1 && by_seal.done_calls.load() == 1; }),
          "the cancelled tasks' dones were delivered");
    check(until([&] { return pool.counts().outstanding == 0; }), "quiescent");
    // the worker drops a task's metadata just after the model counted its done returned: wait for that too
    check(until([&] { return pool.tracked_tasks() == 0; }), "the metadata of every quiescent task is released");
    platform.advance(10000);
    std::vector<Watch> reports;
    pool.run_watchdog(+[](const Watch& w, void* user) { static_cast<std::vector<Watch>*>(user)->push_back(w); }, &reports);
    check(reports.empty() && pool.tracked_tasks() == 0,
          "after quiescence no report and no metadata is left (" + std::to_string(pool.tracked_tasks()) + " tracked)");
}

// a worker stuck past shutdown is detached within the bound; when it later resumes it touches no freed state.
KILN_TASKS_TEST(stuck_worker_survives_pool) {
    HostPlatform platform;
    Job stuck;
    stuck.platform = &platform;
    const auto begin = std::chrono::steady_clock::now();
    {
        PoolConfig c = config(1);
        c.shutdown_join_ns = 50ull * 1000 * 1000;  // 50 ms bound
        Pool pool(c, &platform);
        pool.open_owner(1, "a");
        submit(pool, 1, named("stuck"), stuck);
        check(until([&] { return stuck.body_runs.load() == 1; }), "the body is stuck in its run");
    }  // ~Pool: the stuck worker is detached after the bound
    const auto took = std::chrono::steady_clock::now() - begin;
    check(took < std::chrono::seconds(5), "teardown returned within the bound (the stuck worker was detached)");
    const std::size_t platform_calls = platform.description_log().size();
    stuck.release.open();  // the detached worker resumes after the pool is gone
    check(until([&] { return Pool::live_cores() == 0; }), "the resumed worker finished and released the shared state");
    // ...and it called into the platform after ~Pool (it restored its idle description): the platform a pool was
    // given must outlive any worker it detached, which is why the production one is immortal
    check(platform.description_log().size() > platform_calls,
          "the detached worker used the platform after the pool was gone (the platform outlived it)");
}

// a body that faults while the pool shuts down (after maintenance stopped) leaves through the join set;
// no retired-worker entry keeps the shared state alive once the pool is gone.
KILN_TASKS_TEST(fault_during_shutdown_frees_state) {
    HostPlatform platform;
    const int before = Pool::live_cores();
    Job job;
    job.platform = &platform;
    {
        Pool pool(config(1), &platform);
        pool.open_owner(1, "a");
        platform.fault_after_call_of(reinterpret_cast<uintptr_t>(&job_body));
        submit(pool, 1, named("late-fault"), job);
        check(until([&] { return job.body_runs.load() == 1; }), "the body runs");
        pool.set_test_hook([&](Pool::Point p) {
            if (p == Pool::Point::MaintenanceStopped) job.release.open();  // it returns, then faults, mid-shutdown
        });
    }
    check(Pool::live_cores() == before, "the shared state was freed after a fault during shutdown (" +
                                            std::to_string(Pool::live_cores() - before) + " left)");
}

// a detached worker that resumes after the pool is gone and then faults still frees the shared state
// when it exits (its retirement record holds no reference back to it).
KILN_TASKS_TEST(detached_worker_fault_frees_state) {
    HostPlatform platform;
    const int before = Pool::live_cores();
    Job stuck;
    stuck.platform = &platform;
    {
        PoolConfig c = config(1);
        c.shutdown_join_ns = 50ull * 1000 * 1000;
        Pool pool(c, &platform);
        pool.open_owner(1, "a");
        platform.fault_after_call_of(reinterpret_cast<uintptr_t>(&job_body));
        submit(pool, 1, named("stuck-then-faults"), stuck);
        check(until([&] { return stuck.body_runs.load() == 1; }), "the body is stuck in its run");
    }  // detached after the bound
    stuck.release.open();  // it resumes, returns, and the platform reports a fault
    check(until([&] { return Pool::live_cores() == before; }), "the detached worker faulted and still freed the state (" +
                                                                   std::to_string(Pool::live_cores() - before) + " left)");
}

// drain_owner with timeout 0 is refused on a pool worker too.
KILN_TASKS_TEST(drain_zero_on_worker) {
    HostPlatform platform;
    Pool pool(config(2), &platform);
    pool.open_owner(1, "a");
    pool.open_owner(2, "b");
    std::atomic<KilnResult> r{KILN_OK};
    std::atomic<bool> ran{false};
    Job job;
    job.platform = &platform;
    job.release.open();
    job.extra = [&](const TaskRun*) {
        uint32_t out = 0;
        r.store(pool.drain_owner(2, 0, &out));
        ran.store(true);
    };
    submit(pool, 1, named("w"), job);
    check(until([&] { return ran.load(); }), "the body ran");
    check(r.load() == KILN_ERR_WRONG_THREAD, "drain_owner(0) is refused on a pool worker");
    Job other;
    other.platform = &platform;
    other.release.open();
    submit(pool, 2, named("still-open"), other);
    check(until([&] { return other.done_calls.load() == 1; }), "and it did not seal the other owner");
}

// Stack-overflow acceptance: a stuck body is named by the watchdog, holds only its own owner's slot, and other owners
// keep running.
KILN_TASKS_TEST(stuck_body_isolated_and_named) {
    HostPlatform platform;
    PoolConfig c = config(2);
    c.running_watchdog_ns = 1000;
    c.queued_watchdog_ns = 1000000000000ull;
    c.shutdown_join_ns = 50ull * 1000 * 1000;
    Job stuck;
    stuck.platform = &platform;
    {
        Pool pool(c, &platform);
        pool.open_owner(1, "mod.stuck");
        pool.open_owner(2, "mod.other");
        const KilnHandle sh = submit(pool, 1, named("hangs"), stuck);
        check(until([&] { return stuck.body_runs.load() == 1; }), "the stuck body runs");
        std::vector<std::unique_ptr<Job>> others;
        for (int i = 0; i < 6; ++i) {
            auto j = std::make_unique<Job>();
            j->platform = &platform;
            j->release.open();
            submit(pool, 2, named("fine"), *j);
            others.push_back(std::move(j));
        }
        check(until([&] {
            for (auto& j : others)
                if (j->done_calls.load() != 1) return false;
            return true;
        }), "another owner's tasks all complete while one owner's body is stuck");
        platform.advance(5000);
        std::vector<Watch> reports;
        pool.run_watchdog(+[](const Watch& w, void* user) { static_cast<std::vector<Watch>*>(user)->push_back(w); }, &reports);
        bool named_it = false;
        for (const Watch& w : reports)
            named_it |= w.kind == Watch::Kind::RunningTooLong && w.task == sh && w.owner == 1 && w.name == "hangs";
        check(named_it, "the watchdog names the stuck task (owner and name)");
    }
    stuck.release.open();
    check(until([&] { return Pool::live_cores() == 0; }), "the stuck worker cleaned up after release");
}


// ---------------------------------------------------------------------------------------------------------------

// a native body has no way to say "I stopped because of the cancel": a normal return after a cancel request
// is Completed (kiln_tasks.h: COMPLETED wins over a cancel requested too late), never Cancelled.
KILN_TASKS_TEST(native_return_after_cancel_is_completed) {
    HostPlatform platform;
    Pool pool(config(1), &platform);
    pool.open_owner(1, "a");
    Job job;
    job.platform = &platform;
    const KilnHandle h = submit(pool, 1, named("late cancel"), job);
    check(until([&] { return job.body_runs.load() == 1; }), "the body is running");
    State s = State::None;
    check(pool.cancel(1, h, &s) == KILN_OK && s == State::Running, "cancel requested while it runs");
    job.release.open();  // it finishes its work and returns normally
    check(until([&] { return job.done_calls.load() == 1; }), "done ran");
    check(job.done_state.load() == static_cast<uint32_t>(State::Completed),
          "done saw Completed for a normal return after a cancel request (got " +
              std::to_string(job.done_state.load()) + ")");
    check(pool.status(1, h, &s) == KILN_OK && s == State::Completed, "status is Completed");
}

// a worker that could not start does not leave the pool accepting work it can never run.
KILN_TASKS_TEST(start_failure_at_construction) {
    {
        HostPlatform platform;
        platform.fail_starts("kiln-task-N", 1);  // the only normal worker
        Pool pool(config(1), &platform);
        pool.open_owner(1, "a");
        Job job;
        job.platform = &platform;
        job.release.open();
        KilnHandle h = 7;
        Refusal why = Refusal::None;
        const KilnResult r = pool.submit(1, named("never runs"), &job_body, &job_done, &job, &h, &why);
        check(r == KILN_ERR_UNAVAILABLE && why == Refusal::Capability && h == 0,
              "no normal worker: submit refuses UNAVAILABLE + CAPABILITY (got " + std::to_string(r) + ")");
        check(platform.acquires() == platform.releases(), "the refused submit keeps no module reference");
        // the pool retries the start later; once it succeeds, work runs
        bool accepted = false;  // until() evaluates its predicate once more after success: submit only once
        check(until([&] {
            KilnHandle t = 0;
            Refusal w = Refusal::None;
            if (!accepted) accepted = pool.submit(1, named("after retry"), &job_body, &job_done, &job, &t, &w) == KILN_OK;
            return accepted;
        }), "the failed worker start is retried and the pool then accepts work");
        check(until([&] { return job.done_calls.load() == 1; }), "and runs it");
    }
    {
        HostPlatform platform;
        platform.fail_starts("kiln-task-L", 2);  // both LongRunning workers
        Pool pool(config(1, 2), &platform);
        pool.open_owner(1, "a");
        Job job;
        job.platform = &platform;
        job.release.open();
        KilnHandle h = 7;
        Refusal why = Refusal::None;
        check(pool.submit(1, named("long", Priority::Normal, true), &job_body, &job_done, &job, &h, &why) ==
                      KILN_ERR_UNAVAILABLE &&
                  why == Refusal::Capability && h == 0,
              "no LongRunning worker: a LongRunning submit refuses CAPABILITY");
        check(pool.submit(1, named("normal"), &job_body, &job_done, &job, &h, &why) == KILN_OK,
              "normal work is still accepted");
        check(until([&] { return job.done_calls.load() == 1; }), "and runs");
    }
    {
        HostPlatform platform;
        platform.fail_starts("kiln-task-maint", 1);
        Job job;  // before the pool: it must outlive any worker that could run it
        job.platform = &platform;
        job.release.open();
        Pool pool(config(1), &platform);
        pool.open_owner(1, "a");
        KilnHandle h = 7;
        Refusal why = Refusal::None;
        check(pool.submit(1, named("no maintenance"), &job_body, &job_done, &job, &h, &why) == KILN_ERR_UNAVAILABLE &&
                  why == Refusal::Capability && h == 0,
              "without its maintenance thread (no worker replacement) the pool refuses CAPABILITY");
    }
}

KILN_TASKS_TEST(start_failure_at_replacement) {
    {
        // the gap: the only normal worker faulted and its replacement could not start (retry far away)
        HostPlatform platform;
        Job faulter;  // before the pool: it outlives every worker
        faulter.platform = &platform;
        PoolConfig c = config(1);
        c.restart_retry_ns = 60ull * 1000 * 1000 * 1000;
        Pool pool(c, &platform);
        pool.open_owner(1, "a");
        pool.open_owner(2, "bad");
        platform.fail_starts("kiln-task-N", 1);
        platform.fault_next_call_of(reinterpret_cast<uintptr_t>(&job_body));
        KilnHandle fh = 0;
        Refusal why = Refusal::None;
        pool.submit(2, named("faulter"), &job_body, &job_done, &faulter, &fh, &why);
        check(until([&] { return platform.pending_start_failures() == 0 && !pool.can_run(Slot::Normal); }),
              "the faulted worker's replacement failed: no normal worker is live");
        Job late;
        late.platform = &platform;
        KilnHandle h = 7;
        check(pool.submit(1, named("in the gap"), &job_body, &job_done, &late, &h, &why) == KILN_ERR_UNAVAILABLE &&
                  why == Refusal::Capability && h == 0 && pool.start_failures() == 1,
              "while no normal worker is live, submit refuses CAPABILITY (one start failure counted)");
    }
    HostPlatform platform;
    Pool pool(config(1), &platform);
    pool.open_owner(1, "a");
    pool.open_owner(2, "bad");
    platform.fail_starts("kiln-task-N", 1);  // the replacement of the worker that faults next
    platform.fault_next_call_of(reinterpret_cast<uintptr_t>(&job_body));
    Job faulter;
    faulter.platform = &platform;
    KilnHandle fh = 0;
    Refusal why = Refusal::None;
    pool.submit(2, named("faulter"), &job_body, &job_done, &faulter, &fh, &why);
    check(until([&] { return platform.pending_start_failures() == 0; }), "the replacement start failed");
    Job after;
    after.platform = &platform;
    after.release.open();
    bool accepted = false;  // until() evaluates its predicate once more after success: submit only once
    check(until([&] {
        KilnHandle t = 0;
        Refusal w = Refusal::None;
        if (!accepted) accepted = pool.submit(1, named("after"), &job_body, &job_done, &after, &t, &w) == KILN_OK;
        return accepted;
    }), "a submit is accepted once a worker runs again");
    check(until([&] { return after.done_calls.load() == 1; }),
          "the failed replacement is retried: later work runs on the single-worker pool");
    check(until([&] { return faulter.done_calls.load() == 1; }), "and the faulted task's done was delivered");
}

// a replacement worker that got a reused thread id (a closed HANDLE value) and is stuck at shutdown is
// detached after the bound, not joined forever because the old incarnation's exit is remembered under the same id.
KILN_TASKS_TEST(reused_thread_id_shutdown_bounded) {
    HostPlatform platform;
    platform.reuse_ids(true);
    Job stuck;
    stuck.platform = &platform;
    std::atomic<bool> destroyed{false};
    PoolConfig c = config(1);
    c.shutdown_join_ns = 50ull * 1000 * 1000;  // 50 ms bound
    auto* pool = new Pool(c, &platform);
    pool->open_owner(1, "a");
    pool->open_owner(2, "bad");
    Job first;
    first.platform = &platform;
    first.release.open();
    submit(*pool, 1, named("first"), first);
    check(until([&] { return first.done_calls.load() == 1; }), "the first task ran");
    const uint64_t old_id = first.body_thread.load();
    platform.fault_next_call_of(reinterpret_cast<uintptr_t>(&job_body));
    Job faulter;
    faulter.platform = &platform;
    KilnHandle fh = 0;
    Refusal why = Refusal::None;
    pool->submit(2, named("faulter"), &job_body, &job_done, &faulter, &fh, &why);
    check(until([&] { return faulter.done_calls.load() == 1; }), "the faulting task ended; its worker retired");
    submit(*pool, 1, named("stuck"), stuck);
    check(until([&] { return stuck.body_runs.load() == 1; }), "the stuck body runs on the replacement");
    check(stuck.body_thread.load() == old_id, "the replacement got the retired worker's id back (the reuse case)");
    std::thread teardown([&] {
        delete pool;
        destroyed = true;
    });
    const bool bounded = until([&] { return destroyed.load(); }, 3000);
    check(bounded, "shutdown detached the stuck replacement within the bound, not an endless join");
    stuck.release.open();
    teardown.join();
    check(until([&] { return Pool::live_cores() == 0; }), "the detached worker finished and freed the state");
}

// a body still running after its owner was retired is still named by the watchdog.
KILN_TASKS_TEST(watchdog_after_retire) {
    HostPlatform platform;
    PoolConfig c = config(1);
    c.running_watchdog_ns = 1000;
    c.queued_watchdog_ns = 1000;
    Pool pool(c, &platform);
    pool.open_owner(1, "a");
    Job stuck;
    stuck.platform = &platform;
    const KilnHandle h = submit(pool, 1, named("outlives its owner"), stuck);
    check(until([&] { return stuck.body_runs.load() == 1; }), "running");
    uint32_t left = 0;
    pool.retire_owner(1, &left);
    check(left == 1, "retired with the body outstanding");
    platform.advance(5000);
    std::vector<Watch> reports;
    pool.run_watchdog(+[](const Watch& w, void* user) { static_cast<std::vector<Watch>*>(user)->push_back(w); }, &reports);
    bool named_it = false;
    for (const Watch& w : reports) named_it |= w.kind == Watch::Kind::RunningTooLong && w.task == h && w.owner == 1;
    check(named_it, "the watchdog still reports the body that outlives its retired owner");
    stuck.release.open();
    check(until([&] { return pool.counts().outstanding == 0; }), "quiescent");
}

// submit's capability check and its publication are one decision. A submit held
// between the check and the publication (inside acquire_module) while the only normal worker faults and its
// replacement cannot start is refused CAPABILITY, holds no reference and never runs.
KILN_TASKS_TEST(capability_rechecked_at_publication) {
    HostPlatform platform;
    Job held, faulter;  // before the pool: they outlive every worker
    held.platform = faulter.platform = &platform;
    held.release.open();
    PoolConfig c = config(1);
    c.restart_retry_ns = 60ull * 1000 * 1000 * 1000;
    Pool pool(c, &platform);
    pool.open_owner(1, "held");
    pool.open_owner(2, "faults");
    Gate in_window, proceed;
    std::atomic<bool> armed{true};
    std::atomic<std::thread::id> submitter{};
    platform.set_acquire_hook([&](uintptr_t) {
        if (std::this_thread::get_id() != submitter.load() || !armed.exchange(false)) return;
        in_window.open();  // the capability check passed; the publication has not happened
        proceed.wait();
    });
    KilnHandle h = 7;
    Refusal why = Refusal::None;
    KilnResult r = KILN_OK;
    std::thread a([&] {
        submitter.store(std::this_thread::get_id());
        r = pool.submit(1, named("held in the window"), &job_body, &job_done, &held, &h, &why);
    });
    in_window.wait();
    platform.fail_starts("kiln-task-N", 1);
    platform.fault_next_call_of(reinterpret_cast<uintptr_t>(&job_body));
    KilnHandle fh = 0;
    Refusal fwhy = Refusal::None;
    pool.submit(2, named("faulter"), &job_body, &job_done, &faulter, &fh, &fwhy);
    check(until([&] { return !pool.can_run(Slot::Normal) && platform.pending_start_failures() == 0; }),
          "the only normal worker faulted and its replacement could not start");
    proceed.open();
    a.join();
    platform.set_acquire_hook(nullptr);
    check(r == KILN_ERR_UNAVAILABLE && why == Refusal::Capability && h == 0,
          "the held submit is refused CAPABILITY at publication (got " + std::to_string(r) + ")");
    check(pool.tracked_tasks() == 1 && pool.counts().records == 1,
          "only the faulted task is tracked; the refused one holds no reference");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    check(held.body_runs.load() == 0, "and it never ran");
}

// an exception before publication (every allocation of a submit happens there) releases the
// module references and leaves nothing a worker could see; the next submit works.
KILN_TASKS_TEST(submit_exception_before_publication) {
    HostPlatform platform;
    Job j, next;
    j.platform = next.platform = &platform;
    j.release.open();
    next.release.open();
    Pool pool(config(1), &platform);
    pool.open_owner(1, "a");
    pool.set_test_hook([](Pool::Point p) {
        if (p == Pool::Point::MetaPrepared) throw std::bad_alloc();
    });
    KilnHandle h = 0;
    Refusal why = Refusal::None;
    bool threw = false;
    try {
        pool.submit(1, named("fails"), &job_body, &job_done, &j, &h, &why);
    } catch (const std::bad_alloc&) {
        threw = true;
    }
    pool.set_test_hook(nullptr);
    check(threw && pool.counts().records == 0 && pool.tracked_tasks() == 0 && platform.acquires() == platform.releases(),
          "nothing published, no reference kept");
    pool.set_model_test_hook([](Model::Point p) {
        if (p == Model::Point::SubmitPublishing) throw std::bad_alloc();
    });
    threw = false;
    try {
        pool.submit(1, named("fails in the model"), &job_body, &job_done, &j, &h, &why);
    } catch (const std::bad_alloc&) {
        threw = true;
    }
    pool.set_model_test_hook(nullptr);
    check(threw && pool.counts().records == 0 && pool.tracked_tasks() == 0 && platform.acquires() == platform.releases(),
          "a failure inside the model's publication: nothing published, no reference kept");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    check(j.body_runs.load() == 0, "no failed submission ran");
    submit(pool, 1, named("next"), next);
    check(until([&] { return next.done_calls.load() == 1; }), "the next submit runs");
}

// The maintenance thread's watchdog sweep: each over-age task reported once per kind (never every sweep), with its
// owner's name, without anyone calling run_watchdog (the binding logs these lines).
KILN_TASKS_TEST(watchdog_sweeps_once_per_state) {
    HostPlatform platform;
    Job running, queued;
    running.platform = queued.platform = &platform;
    struct Sink {
        std::mutex mutex;
        std::vector<Watch> seen;
        std::size_t count(KilnHandle task, Watch::Kind kind) {
            std::lock_guard lock(mutex);
            std::size_t n = 0;
            for (const Watch& w : seen) n += w.task == task && w.kind == kind;
            return n;
        }
    } sink;
    PoolConfig c = config(1);
    c.running_watchdog_ns = 1000;
    c.queued_watchdog_ns = 1000;
    c.watchdog_interval_ns = 1000 * 1000;  // 1 ms
    Pool pool(c, &platform);
    pool.open_owner(1, "mod.watched");
    pool.set_watch_sink(+[](const Watch& w, void* user) {
        auto* s = static_cast<Sink*>(user);
        std::lock_guard lock(s->mutex);
        s->seen.push_back(w);
    }, &sink);
    const KilnHandle rh = submit(pool, 1, named("runs long"), running);
    check(until([&] { return running.body_runs.load() == 1; }), "running");
    const KilnHandle qh = submit(pool, 1, named("waits"), queued);  // one worker: it waits behind
    platform.advance(5000);
    check(until([&] { return sink.count(rh, Watch::Kind::RunningTooLong) == 1 && sink.count(qh, Watch::Kind::QueuedTooLong) == 1; }),
          "the maintenance thread's sweep reported the running and the queued task");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));  // dozens of sweeps
    check(sink.count(rh, Watch::Kind::RunningTooLong) == 1 && sink.count(qh, Watch::Kind::QueuedTooLong) == 1,
          "each was reported once, not every sweep");
    {
        std::lock_guard lock(sink.mutex);
        check(!sink.seen.empty() && sink.seen.front().owner_name == "mod.watched" && sink.seen.front().owner == 1,
              "a report names the owner");
    }
    running.release.open();
    check(until([&] { return queued.body_runs.load() == 1; }), "the queued task runs");
    platform.advance(5000);
    check(until([&] { return sink.count(qh, Watch::Kind::RunningTooLong) == 1; }),
          "once running past its age it is reported again, once, for the new state");
    pool.set_watch_sink(nullptr, nullptr);
    queued.release.open();
    check(until([&] { return pool.counts().outstanding == 0; }), "quiescent");
}

// A worker kind that loses its last worker (the replacement's start failed) is said once, with the queued count, and
// once again when a retried start brings it back; admission is unchanged (the capability refuses meanwhile).
KILN_TASKS_TEST(slot_lost_and_recovered_noted_once) {
    HostPlatform platform;
    Job faulter, waiting;
    faulter.platform = waiting.platform = &platform;
    waiting.release.open();
    struct Notes {
        std::mutex mutex;
        std::vector<PoolNote> seen;
        std::size_t count(PoolNote::Kind k) {
            std::lock_guard lock(mutex);
            std::size_t n = 0;
            for (const PoolNote& p : seen) n += p.kind == k && p.slot == Slot::Normal;
            return n;
        }
    } notes;
    PoolConfig c = config(1);
    c.restart_retry_ns = 200ull * 1000 * 1000;  // 200 ms
    Pool pool(c, &platform);
    pool.set_note_sink(+[](const PoolNote& n, void* user) {
        auto* s = static_cast<Notes*>(user);
        std::lock_guard lock(s->mutex);
        s->seen.push_back(n);
    }, &notes);
    pool.open_owner(1, "a");
    pool.open_owner(2, "bad");
    platform.fail_starts("kiln-task-N", 2);  // the replacement and its first retry fail
    platform.fault_next_call_of(reinterpret_cast<uintptr_t>(&job_body));
    KilnHandle fh = 0;
    Refusal why = Refusal::None;
    pool.submit(2, named("faulter"), &job_body, &job_done, &faulter, &fh, &why);
    check(until([&] { return notes.count(PoolNote::Kind::SlotLost) == 1; }), "the lost slot was noted");
    {
        std::lock_guard lock(notes.mutex);
        check(!notes.seen.empty() && notes.seen.front().queued >= 1, "with the queued work (the faulted task's done)");
    }
    check(until([&] { return notes.count(PoolNote::Kind::SlotRecovered) == 1; }), "and its recovery, once a retry started");
    check(notes.count(PoolNote::Kind::SlotLost) == 1, "the failed retry in between did not say it again");
    submit(pool, 1, named("after"), waiting);
    check(until([&] { return waiting.done_calls.load() == 1 && faulter.done_calls.load() == 1; }), "work runs again");
    pool.set_note_sink(nullptr, nullptr);
}

// ---------------------------------------------------------------------------------------------------------------
// Process exit: close() then park(). After the park no worker starts another body or done; the caller waits (bounded)
// for the ones already running.

// park() on its own thread, so the test can watch it wait.
struct Parker {
    std::atomic<bool> returned{false};
    Pool::ParkResult result;
    std::chrono::steady_clock::duration took{};
    std::thread thread;
    void start(Pool& pool, uint64_t timeout_ns) {
        thread = std::thread([this, &pool, timeout_ns] {
            const auto t0 = std::chrono::steady_clock::now();
            result = pool.park(timeout_ns);
            took = std::chrono::steady_clock::now() - t0;
            returned = true;
        });
    }
    void join() {
        if (thread.joinable()) thread.join();
    }
};

// A body in flight is waited for; the cancelled queued task and every done stay undelivered once parked.
KILN_TASKS_TEST(exit_park_waits_for_the_running_body) {
    HostPlatform platform;
    Pool pool(config(1, 1), &platform);
    pool.open_owner(1, "mod.one");
    Job running, queued;
    running.platform = queued.platform = &platform;
    queued.release.open();
    submit(pool, 1, named("active"), running);
    check(until([&] { return running.body_runs.load() == 1; }), "the body is running");
    submit(pool, 1, named("waiting"), queued);  // the one normal worker is busy: queued
    pool.close();                               // the exit seals: the queued one is cancelled (its done queued)
    Parker parker;
    parker.start(pool, 5ull * 1000 * 1000 * 1000);
    check(until([&] { return pool.parked(); }), "the park began while the body runs");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    check(!parker.returned.load(), "park waits while a body runs");
    running.release.open();
    check(until([&] { return parker.returned.load(); }), "park returns once the body returned");
    parker.join();
    check(parker.result.running_at_start == 1 && parker.result.still_running == 0 && parker.result.names.empty(),
          "one callback was running at the park, none is left");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    check(queued.body_runs.load() == 0, "the cancelled queued task never ran");
    check(running.done_calls.load() == 0 && queued.done_calls.load() == 0, "no done runs once the pool parked");
    check(pool.parked(), "the pool says it is parked");
}

// A body that ignores its cancel past the bound is named and left running; nothing else runs after it returns.
KILN_TASKS_TEST(exit_park_bound) {
    HostPlatform platform;
    Job stuck, idle_owner;  // before the pool: the released body still reads its job on its way out
    stuck.platform = idle_owner.platform = &platform;
    Pool pool(config(2, 1), &platform);
    pool.open_owner(1, "mod.one");
    pool.open_owner(2, "mod.two");
    idle_owner.release.open();
    submit(pool, 1, named("stuck"), stuck);
    check(until([&] { return stuck.body_runs.load() == 1; }), "the body is running");
    pool.close();
    const auto t0 = std::chrono::steady_clock::now();
    const Pool::ParkResult r = pool.park(50ull * 1000 * 1000);
    const auto took = std::chrono::steady_clock::now() - t0;
    check(r.running_at_start == 1 && r.still_running == 1, "the body that ignores its cancel is still running");
    check(r.names == "mod.one/stuck", "and named <owner>/<task>: '" + r.names + "'");
    check(took >= std::chrono::milliseconds(50) && took < std::chrono::seconds(2), "park gave up at its bound");
    KilnHandle h = 0;
    Refusal why = Refusal::None;
    check(pool.submit(2, named("late"), &job_body, &job_done, &idle_owner, &h, &why) != KILN_OK, "nothing is accepted");
    stuck.release.open();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    check(stuck.done_calls.load() == 0, "the late body's done is not delivered after the park");
    check(pool.park(0).still_running == 0, "a second park finds every worker parked");
}

// After the park a timed drain_owner no longer waits (no done can run any more): it reports what is left at once.
KILN_TASKS_TEST(exit_park_drain_does_not_wait) {
    HostPlatform platform;
    Pool pool(config(1, 1), &platform);
    pool.open_owner(1, "mod.one");
    Job running, queued;
    running.platform = queued.platform = &platform;
    queued.release.open();
    submit(pool, 1, named("active"), running);
    check(until([&] { return running.body_runs.load() == 1; }), "the body is running");
    submit(pool, 1, named("waiting"), queued);
    pool.close();
    Parker parker;
    parker.start(pool, 5ull * 1000 * 1000 * 1000);
    check(until([&] { return pool.parked(); }), "the park began while the body runs");
    running.release.open();
    check(until([&] { return parker.returned.load(); }), "parked");
    parker.join();
    uint32_t left = 0;
    const auto t0 = std::chrono::steady_clock::now();
    check(pool.drain_owner(1, 5ull * 1000 * 1000 * 1000, &left) == KILN_OK && left > 0, "the dones are left undelivered");
    check(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(1), "drain_owner returned at once after the park");
}

// The exit can come from inside a body (a task that ends the process): park never waits for its own caller.
KILN_TASKS_TEST(exit_park_from_a_body) {
    HostPlatform platform;
    Job self;  // before the pool: the body still reads its job after it parked the pool
    self.platform = &platform;
    Pool pool(config(1, 1), &platform);
    pool.open_owner(1, "mod.one");
    self.release.open();
    Pool::ParkResult inner;
    std::chrono::steady_clock::duration took{};
    std::atomic<bool> returned{false};  // publishes inner and took to the test thread
    self.extra = [&](const TaskRun*) {
        pool.close();
        const auto t0 = std::chrono::steady_clock::now();
        inner = pool.park(2ull * 1000 * 1000 * 1000);
        took = std::chrono::steady_clock::now() - t0;
        returned = true;
    };
    submit(pool, 1, named("exits"), self);
    check(until([&] { return returned.load(); }), "the body parked the pool");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    check(inner.still_running == 0 && took < std::chrono::seconds(1), "park did not wait for the body that called it");
    check(self.done_calls.load() == 0, "its own done is not delivered after the park");
}

// The maintenance thread stops at the park: no watchdog sweep (no log line) runs after it.
KILN_TASKS_TEST(exit_park_stops_maintenance) {
    HostPlatform platform;
    PoolConfig c = config(1, 1);
    c.watchdog_interval_ns = 1000 * 1000;              // 1 ms (real time)
    c.running_watchdog_ns = 1000ull * 1000 * 1000;     // 1 s on the fake clock
    c.queued_watchdog_ns = 1000;
    Job stuck, queued;  // before the pool: the test ends by releasing the body, which still reads its job
    stuck.platform = queued.platform = &platform;
    Pool pool(c, &platform);
    pool.open_owner(1, "mod.one");
    std::atomic<int> reports{0};
    pool.set_watch_sink(+[](const Watch&, void* user) { static_cast<std::atomic<int>*>(user)->fetch_add(1); }, &reports);
    queued.release.open();
    submit(pool, 1, named("stuck"), stuck);
    check(until([&] { return stuck.body_runs.load() == 1; }), "the body is running");
    submit(pool, 1, named("waits"), queued);
    platform.advance(5000);
    check(until([&] { return reports.load() == 1; }), "the watchdog reports while the pool runs (the queued task)");
    pool.close();
    check(pool.park(1000 * 1000).still_running == 1, "the stuck body is left running");
    platform.advance(2ull * 1000 * 1000 * 1000);  // the running body is now overdue: a sweep would report it
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    check(reports.load() == 1, "no sweep ran after the park");
    stuck.release.open();
}

// Under load (TSan): once park() returned with nothing running, no body or done starts again.
KILN_TASKS_TEST(exit_park_under_load) {
    HostPlatform platform;
    Pool pool(config(4, 2), &platform);
    for (KilnOwner o = 1; o <= 4; ++o) pool.open_owner(o, "o");
    std::atomic<int> starts{0};
    struct Tiny {
        std::atomic<int>* starts;
    } tiny{&starts};
    const auto body = +[](const TaskRun*, void* user) { static_cast<Tiny*>(user)->starts->fetch_add(1); };
    const auto done = +[](KilnHandle, uint32_t, void* user) { static_cast<Tiny*>(user)->starts->fetch_add(1); };
    std::atomic<bool> stop{false};
    std::vector<std::thread> submitters;
    for (KilnOwner o = 1; o <= 4; ++o)
        submitters.emplace_back([&, o] {
            while (!stop.load()) {
                KilnHandle h = 0;
                Refusal why = Refusal::None;
                if (pool.submit(o, named("tiny", Priority::Normal, o == 4), body, done, &tiny, &h, &why) == KILN_OK)
                    pool.release(o, h);
                std::this_thread::yield();
            }
        });
    check(until([&] { return starts.load() > 200; }), "the pool is busy");
    pool.close();
    const Pool::ParkResult r = pool.park(5ull * 1000 * 1000 * 1000);
    const int at_park = starts.load();
    check(r.still_running == 0, "every running callback returned within the bound");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    check(starts.load() == at_park, "nothing started after the park returned");
    stop = true;
    for (auto& t : submitters) t.join();
}

// ---------------------------------------------------------------------------------------------------------------
// KILN_TASK_RESIDENT (SubmitArgs.resident): a LongRunning task meant to run until it is cancelled. The watchdog does
// not report it running until its cancellation is asked, then once the age after that; its segments stay out of the
// stats' time sums; everything else is a LongRunning task's.

constexpr uint64_t kSecond = 1000ull * 1000 * 1000;

SubmitArgs resident(const char* name) {
    SubmitArgs a = named(name, Priority::Normal, true);
    a.resident = true;
    return a;
}

// A service loop: runs until cancelled() says so, then returns (it opens its own release on the way out).
void loop_until_cancelled(Job& job) {
    job.extra = [&job](const TaskRun* run) {
        while (!run->cancelled(run)) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        job.release.open();
    };
}

std::vector<Watch> sweep(Pool& pool) {
    std::vector<Watch> reports;
    pool.run_watchdog(+[](const Watch& w, void* user) { static_cast<std::vector<Watch>*>(user)->push_back(w); }, &reports);
    return reports;
}

std::size_t reports_of(const std::vector<Watch>& reports, KilnHandle task, Watch::Kind kind) {
    std::size_t n = 0;
    for (const Watch& w : reports) n += w.task == task && w.kind == kind;
    return n;
}

// The default ages (30 s) on the fake clock. Running for minutes, sweep after sweep: the resident task is not reported
// while a plain LongRunning task beside it is (once), and a resident task waiting in the queue is reported as queued.
// Once its cancellation is asked (a body that ignores it) its running clock restarts: nothing at 29 s, one report
// "after its cancellation was asked" at 31 s, never again. Its crash-report row is any task's.
KILN_TASKS_TEST(watchdog_resident_until_cancel) {
    HostPlatform platform;
    Job loop, plain, waits;
    loop.platform = plain.platform = waits.platform = &platform;
    Pool pool(config(1, 2), &platform);
    pool.open_owner(1, "mod.service");
    pool.open_owner(2, "mod.other");
    {
        SubmitArgs not_long = resident("loop");
        not_long.long_running = false;
        KilnHandle h = 7;
        Refusal why = Refusal::None;
        check(pool.submit(1, not_long, &job_body, &job_done, &loop, &h, &why) == KILN_ERR_INVALID_ARGUMENT &&
                  why == Refusal::Argument && h == 0 && platform.acquires() == platform.releases() &&
                  pool.counts().records == 0,
              "resident without long_running: INVALID_ARGUMENT + Argument, nothing held");
    }
    const KilnHandle lh = submit(pool, 1, resident("loop"), loop);
    const KilnHandle ph = submit(pool, 1, named("plain", Priority::Normal, true), plain);
    check(until([&] { return loop.body_runs.load() == 1 && plain.body_runs.load() == 1; }), "both LongRunning bodies run");
    const KilnHandle wh = submit(pool, 2, resident("waits"), waits);  // the pool's two LongRunning threads are taken
    std::vector<Watch> reports;
    for (int i = 0; i < 4; ++i) {  // two minutes, a sweep every 30 s
        platform.advance(31 * kSecond);
        for (const Watch& w : sweep(pool)) reports.push_back(w);
    }
    check(reports_of(reports, lh, Watch::Kind::RunningTooLong) == 0 && reports_of(reports, lh, Watch::Kind::QueuedTooLong) == 0,
          "the running resident task is not reported before its cancel");
    check(reports_of(reports, ph, Watch::Kind::RunningTooLong) == 1, "the plain LongRunning task beside it is, once");
    check(reports_of(reports, wh, Watch::Kind::QueuedTooLong) == 1, "a resident task queued past its age is reported");
    bool plain_wording = true;
    for (const Watch& w : reports) plain_wording &= !w.after_cancel;
    check(plain_wording, "those reports are the usual ones (not after a cancellation)");
    const kiln::tasks_stats::CrashSnapshot crash = pool.stats_store().snapshot_crash(platform.now_ns());
    bool listed = false;
    for (std::size_t i = 0; i < crash.running_count; ++i)
        listed |= crash.running[i].handle == lh && crash.running[i].state == kiln::tasks_stats::EntryState::Running &&
                  std::string(crash.running[i].task_name) == "loop";
    check(listed, "a crash report lists the running resident task");
    State st = State::None;
    check(pool.cancel(1, lh, &st) == KILN_OK && st == State::Running, "cancel: asked, the body ignores it");
    check(sweep(pool).empty(), "the sweep that first sees the cancel reports nothing");
    platform.advance(29 * kSecond);
    check(sweep(pool).empty(), "29 s after the cancel: nothing yet");
    platform.advance(2 * kSecond);
    const std::vector<Watch> late = sweep(pool);
    check(late.size() == 1 && late[0].task == lh && late[0].kind == Watch::Kind::RunningTooLong && late[0].after_cancel &&
              late[0].age_ns >= 31 * kSecond && late[0].age_ns < 32 * kSecond && late[0].name == "loop" &&
              late[0].owner_name == "mod.service",
          "31 s after the cancel: one report, after its cancellation, aged from the cancel");
    platform.advance(60 * kSecond);
    check(sweep(pool).empty(), "and never again");
    plain.release.open();
    check(until([&] { return waits.body_runs.load() == 1; }), "the queued resident task starts in the freed slot");
    platform.advance(31 * kSecond);
    check(reports_of(sweep(pool), wh, Watch::Kind::RunningTooLong) == 0, "and is not reported running before its cancel");
    loop.release.open();
    waits.release.open();
    check(until([&] { return pool.counts().outstanding == 0; }), "quiescent");
}

// The tick statistics: a resident segment adds nothing to its owner's cpu and wall time and never becomes the window's
// longest, while its running and completion counts are kept. A plain LongRunning segment beside it is counted.
KILN_TASKS_TEST(resident_segment_stats) {
    HostPlatform platform;
    Job loop, plain;
    loop.platform = plain.platform = &platform;
    Pool pool(config(1, 2), &platform);
    pool.open_owner(1, "mod.resident");
    pool.open_owner(2, "mod.plain");
    pool.collect_tasks_window(0);  // a fresh window
    submit(pool, 1, resident("loop"), loop);
    submit(pool, 2, named("plain", Priority::Normal, true), plain);
    check(until([&] { return loop.body_runs.load() == 1 && plain.body_runs.load() == 1; }), "both run");
    platform.advance(90 * kSecond);
    platform.advance_cpu(40 * kSecond);
    const auto row = [](const kiln::tasks_stats::WindowSnapshot& w, KilnOwner owner) {
        for (std::size_t i = 0; i < w.owner_count; ++i)
            if (w.owners[i].owner == owner) return w.owners[i];
        return kiln::tasks_stats::OwnerSnapshot{};
    };
    {
        const kiln::tasks_stats::WindowSnapshot w = pool.collect_tasks_window(0);
        check(row(w, 1).present && row(w, 1).running_current == 1, "the running resident task counts as running");
    }
    loop.release.open();
    plain.release.open();
    check(until([&] { return pool.counts().outstanding == 0; }), "both ended");
    const kiln::tasks_stats::WindowSnapshot w = pool.collect_tasks_window(0);
    const kiln::tasks_stats::OwnerSnapshot r = row(w, 1), p = row(w, 2);
    check(r.present && r.cpu_ns == 0 && r.wall_ns == 0 && r.longest_ns == 0 && r.longest_name[0] == 0 &&
              r.completed == 1 && r.running_current == 0,
          "the resident segment: no cpu, no wall, never the longest; completed once (cpu " + std::to_string(r.cpu_ns) +
              ", wall " + std::to_string(r.wall_ns) + ")");
    check(p.present && p.wall_ns >= 90 * kSecond && p.cpu_ns >= 40 * kSecond && p.longest_ns >= 90 * kSecond &&
              std::string(p.longest_name) == "plain",
          "the plain one beside it is counted");
}

// drain_owner and a revoke (retire_owner) ask a resident body to stop as a cancel does: for one that ignores them the
// running clock starts at the first sweep after, and it is reported once 30 s later, with its owner's name.
KILN_TASKS_TEST(resident_clock_from_drain_and_revoke) {
    HostPlatform platform;
    Job drained, revoked;
    drained.platform = revoked.platform = &platform;
    Pool pool(config(1, 2), &platform);
    pool.open_owner(1, "mod.drained");
    pool.open_owner(2, "mod.revoked");
    const KilnHandle dh = submit(pool, 1, resident("loop"), drained);
    const KilnHandle rh = submit(pool, 2, resident("loop"), revoked);
    check(until([&] { return drained.body_runs.load() == 1 && revoked.body_runs.load() == 1; }), "both run");
    platform.advance(60 * kSecond);
    check(sweep(pool).empty(), "a minute: neither is reported");
    uint32_t left = 0;
    check(pool.drain_owner(1, 0, &left) == KILN_OK && left == 1, "drain(0): sealed, the resident body still running");
    check(pool.retire_owner(2, &left) == KILN_OK && left == 1, "revoke: retired, the resident body still running");
    check(sweep(pool).empty(), "the sweep that sees both cancels reports nothing");
    platform.advance(29 * kSecond);
    check(sweep(pool).empty(), "29 s later: nothing yet");
    platform.advance(2 * kSecond);
    const std::vector<Watch> late = sweep(pool);
    bool d = false, r = false;
    for (const Watch& w : late) {
        d |= w.task == dh && w.after_cancel && w.owner_name == "mod.drained";
        r |= w.task == rh && w.after_cancel && w.owner_name == "mod.revoked";
    }
    check(late.size() == 2 && d && r, "31 s later: each reported once, after its cancellation");
    drained.release.open();
    revoked.release.open();
    check(until([&] { return pool.counts().outstanding == 0; }), "quiescent");
}

// A fault quarantines its owner, and a quarantine does not ask the owner's running bodies to stop (cancelled() stays
// 0). Its resident body keeps running, unreported, until a cancel or drain asks it; then its clock starts.
KILN_TASKS_TEST(resident_quarantined_owner) {
    HostPlatform platform;
    Job loop, faulter;  // before the pool: the faulter's done may still run when the test ends
    loop.platform = faulter.platform = &platform;
    Pool pool(config(2, 2), &platform);
    pool.open_owner(1, "mod.faulty");
    const KilnHandle lh = submit(pool, 1, resident("loop"), loop);
    check(until([&] { return loop.body_runs.load() == 1; }), "the resident body runs");
    platform.fault_next_call_of(reinterpret_cast<uintptr_t>(&job_body));  // the next body called: the faulter's
    faulter.release.open();
    const KilnHandle fh = submit(pool, 1, named("faulter"), faulter);
    check(until([&] {
        State st = State::None;
        pool.status(1, fh, &st);
        return st == State::Faulted;
    }), "the other task faults: the owner is quarantined");
    KilnHandle h = 0;
    Refusal why = Refusal::None;
    check(pool.submit(1, named("after"), &job_body, &job_done, &faulter, &h, &why) == KILN_ERR_UNAVAILABLE &&
              why == Refusal::OwnerQuarantined,
          "the quarantined owner takes no new task");
    check(!pool.cancel_requested(lh), "the quarantine did not ask the resident body to stop");
    platform.advance(120 * kSecond);
    check(sweep(pool).empty(), "two minutes after the fault: the resident body is still not reported");
    uint32_t left = 0;
    check(pool.drain_owner(1, 0, &left) == KILN_OK && left >= 1 && pool.cancel_requested(lh), "a drain asks it");
    check(sweep(pool).empty(), "the sweep that sees the drain reports nothing");
    platform.advance(31 * kSecond);
    const std::vector<Watch> late = sweep(pool);
    check(late.size() == 1 && late[0].task == lh && late[0].after_cancel, "31 s after the drain: reported once");
    loop.release.open();
    check(until([&] {
        uint32_t out = 1;
        pool.drain_owner(1, 0, &out);
        return out == 0;
    }), "quiescent");
}

// A lease (the managed host's tasks: SubmitArgs.lease) admitted resident follows the same rule: not reported before
// its cancel, once 30 s after it; its segment adds no time to the owner's stats.
KILN_TASKS_TEST(resident_lease) {
    HostPlatform platform;
    Job lease;
    lease.platform = &platform;
    Pool pool(config(1, 2), &platform);
    pool.open_owner(1, "mod.host");
    pool.collect_tasks_window(0);  // a fresh window
    SubmitArgs a = resident("lease");
    a.lease = true;
    KilnHandle lh = 0;  // written before the release opens; the segment reads it after
    std::atomic<KilnResult> finished{KILN_ERR_INTERNAL};
    lease.after_release = [&] { finished = pool.finish(lh, Outcome::OwnTokenCancelled); };  // inside the segment
    lh = submit(pool, 1, a, lease);
    check(until([&] { return lease.body_runs.load() == 1; }), "its segment runs");
    platform.advance(120 * kSecond);
    check(sweep(pool).empty(), "two minutes: the running resident lease is not reported");
    State st = State::None;
    check(pool.cancel(1, lh, &st) == KILN_OK && st == State::Running, "cancel: asked");
    check(sweep(pool).empty(), "the sweep that sees the cancel reports nothing");
    platform.advance(31 * kSecond);
    const std::vector<Watch> late = sweep(pool);
    check(late.size() == 1 && late[0].task == lh && late[0].kind == Watch::Kind::RunningTooLong && late[0].after_cancel,
          "31 s after the cancel: reported once, after its cancellation");
    lease.release.open();
    check(until([&] {
        State s = State::None;
        pool.status(1, lh, &s);
        return s == State::Cancelled;
    }) && finished.load() == KILN_OK,
          "finish(own token) inside the segment: Cancelled");
    check(until([&] { return pool.counts().outstanding == 0; }), "quiescent");
    const kiln::tasks_stats::WindowSnapshot w = pool.collect_tasks_window(0);
    bool zero = false;
    for (std::size_t i = 0; i < w.owner_count; ++i)
        if (w.owners[i].owner == 1)
            zero = w.owners[i].wall_ns == 0 && w.owners[i].cpu_ns == 0 && w.owners[i].longest_ns == 0 &&
                   w.owners[i].cancelled == 1;
    check(zero, "its segment added no time to the owner's stats; it counts as cancelled");
}

// A suspended resident lease (its segment armed a resumption and returned) whose cancel is asked while no segment
// runs: the sweep sees the cancel, but the running clock starts with the next segment, not at that sweep.
KILN_TASKS_TEST(resident_lease_cancelled_while_suspended) {
    HostPlatform platform;
    Job lease;
    Gate hold;  // the second segment blocks here
    lease.platform = &platform;
    Pool pool(config(1, 2), &platform);
    pool.open_owner(1, "mod.host");
    lease.pool = &pool;
    lease.arm_on_run.store(true);
    lease.release.open();
    SubmitArgs a = resident("lease");
    a.lease = true;
    const KilnHandle lh = submit(pool, 1, a, lease);
    check(until([&] { return lease.registration.load() != 0 && pool.items_in_hand() == 0; }),
          "its first segment armed a resumption and returned: suspended");
    State st = State::None;
    check(pool.cancel(1, lh, &st) == KILN_OK && st == State::Running, "cancel while suspended: asked");
    check(sweep(pool).empty(), "the sweep that sees the cancel reports nothing");
    platform.advance(40 * kSecond);
    check(sweep(pool).empty(), "40 s later, still suspended: nothing (no segment runs)");
    // the next segment: no new arm, it blocks on `hold`, then finishes the lease with its own token
    std::atomic<KilnResult> finished{KILN_ERR_INTERNAL};
    lease.arm_on_run.store(false);
    lease.extra = [&hold](const TaskRun*) { hold.wait(); };
    lease.after_release = [&] { finished = pool.finish(lh, Outcome::OwnTokenCancelled); };
    check(pool.resume(lh, lease.registration.load()) == KILN_OK, "the host resumes it");
    check(until([&] { return lease.body_runs.load() == 2; }), "its next segment runs");
    check(sweep(pool).empty(), "that segment just started: not reported (the clock did not run while suspended)");
    platform.advance(29 * kSecond);
    check(sweep(pool).empty(), "29 s into that segment: nothing yet");
    platform.advance(2 * kSecond);
    const std::vector<Watch> late = sweep(pool);
    check(late.size() == 1 && late[0].task == lh && late[0].kind == Watch::Kind::RunningTooLong && late[0].after_cancel &&
              late[0].age_ns >= 31 * kSecond && late[0].age_ns < 32 * kSecond,
          "31 s into that segment: reported once, aged from the segment's start");
    hold.open();
    check(until([&] {
        State s = State::None;
        pool.status(1, lh, &s);
        return s == State::Cancelled;
    }) && finished.load() == KILN_OK,
          "finish(own token) in that segment: Cancelled");
    check(until([&] { return pool.counts().outstanding == 0; }), "quiescent");
}

// A resident service loop is cancelled, drained and parked at exit like any task: drain_owner signals it and waits
// for its body and done; at the exit close() signals it, park() waits for it, and one that ignores its cancel is named.
KILN_TASKS_TEST(resident_drain_and_exit_park) {
    HostPlatform platform;
    Job drained, parked, stuck;  // before the pool: a released body still reads its job on its way out
    drained.platform = parked.platform = stuck.platform = &platform;
    Pool pool(config(1, 2), &platform);
    pool.open_owner(1, "mod.one");
    pool.open_owner(2, "mod.two");
    pool.open_owner(3, "mod.three");
    loop_until_cancelled(drained);
    submit(pool, 1, resident("service"), drained);
    check(until([&] { return drained.body_runs.load() == 1; }), "the resident loop runs");
    uint32_t left = 9;
    check(pool.drain_owner(1, 5ull * 1000 * 1000 * 1000, &left) == KILN_OK && left == 0,
          "drain_owner signals the resident loop and waits until its body and done returned");
    check(drained.done_calls.load() == 1 && drained.done_state.load() == static_cast<uint32_t>(State::Completed),
          "its done ran once (a native body that returned: Completed)");
    // at the exit: this loop sees its cancel, then returns only once the park has begun (its release)
    std::atomic<bool> signalled{false};
    parked.extra = [&signalled](const TaskRun* run) {
        while (!run->cancelled(run)) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        signalled = true;
    };
    submit(pool, 2, resident("service"), parked);
    submit(pool, 3, resident("ignores"), stuck);
    check(until([&] { return parked.body_runs.load() == 1 && stuck.body_runs.load() == 1; }), "both run");
    pool.close();
    check(until([&] { return signalled.load(); }), "the exit signals the running resident loop");
    Parker parker;
    parker.start(pool, 500ull * 1000 * 1000);
    check(until([&] { return pool.parked(); }), "the park began while both run");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    check(!parker.returned.load(), "park waits while the resident bodies run");
    parked.release.open();
    check(until([&] { return parker.returned.load(); }), "park returns at its bound");
    parker.join();
    check(parker.result.running_at_start == 2 && parker.result.still_running == 1 &&
              parker.result.names == "mod.three/ignores",
          "the park waited for both and names the resident body that ignores its cancel: '" + parker.result.names + "'");
    stuck.release.open();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    check(parked.done_calls.load() == 0 && stuck.done_calls.load() == 0, "no done runs once the pool parked");
}

// A resident task's first body start is noted once (PoolNote::ResidentStarted, kiln.log's info line), on its worker
// before its body, with its owner, its name and the LongRunning threads resident bodies hold now, of the pool's. A
// plain LongRunning task is not noted, a resident lease's later segment is not noted again, and a resident body that
// returned gives its thread back to the count.
KILN_TASKS_TEST(resident_start_noted_once) {
    HostPlatform platform;
    Job loop, plain, worker, lease, later;  // before the pool: a released body still reads its job on its way out
    loop.platform = plain.platform = worker.platform = lease.platform = later.platform = &platform;
    struct Notes {
        std::mutex mutex;
        std::vector<PoolNote> seen;
        std::vector<PoolNote> resident() {
            std::lock_guard lock(mutex);
            std::vector<PoolNote> r;
            for (const PoolNote& n : seen)
                if (n.kind == PoolNote::Kind::ResidentStarted) r.push_back(n);
            return r;
        }
    } notes;
    Pool pool(config(1, 4), &platform);
    pool.set_note_sink(+[](const PoolNote& n, void* user) {
        auto* s = static_cast<Notes*>(user);
        std::lock_guard lock(s->mutex);
        s->seen.push_back(n);
    }, &notes);
    pool.open_owner(1, "mod.service");
    pool.open_owner(2, "mod.other");
    pool.open_owner(3, "mod.host");
    pool.open_owner(4, "mod.late");
    std::atomic<std::size_t> noted_before_body{99};
    loop.extra = [&](const TaskRun*) { noted_before_body = notes.resident().size(); };
    const KilnHandle lh = submit(pool, 1, resident("loop"), loop);
    check(until([&] { return loop.body_runs.load() == 1; }), "the resident body runs");
    check(noted_before_body.load() == 1, "its start was noted before its body ran");
    std::vector<PoolNote> r = notes.resident();
    check(r.size() == 1 && r[0].owner == 1 && r[0].task == lh && r[0].name == "loop" && r[0].owner_name == "mod.service" &&
              r[0].slot == Slot::LongRunning && r[0].held == 1 && r[0].long_running == 4,
          "one note: its owner, its task, 1 of the pool's 4 LongRunning threads held");
    if (!r.empty()) {
        char expect[256];
        std::snprintf(expect, sizeof expect,
                      "kiln.tasks: mod.service/loop (task 0x%llx) started as a resident task (the watchdog names it only "
                      "after its cancellation is asked); resident tasks now hold 1 of 4 LongRunning threads",
                      static_cast<unsigned long long>(lh));
        const std::string line = resident_started_line(r[0]);
        check(line == expect, "the kiln.log line: '" + line + "'");
    }
    submit(pool, 1, named("plain", Priority::Normal, true), plain);
    check(until([&] { return plain.body_runs.load() == 1; }), "a plain LongRunning task runs beside it");
    const KilnHandle wh = submit(pool, 2, resident("worker"), worker);
    check(until([&] { return worker.body_runs.load() == 1; }), "another owner's resident body runs");
    r = notes.resident();
    check(r.size() == 2 && r[1].task == wh && r[1].owner_name == "mod.other" && r[1].held == 2,
          "the plain one is not noted; the second resident start: 2 held");
    // a resident lease: its first segment arms a resumption and returns, the host resumes it, the next segment finishes
    lease.pool = &pool;
    lease.arm_on_run.store(true);
    lease.release.open();
    SubmitArgs a = resident("lease");
    a.lease = true;
    const KilnHandle sh = submit(pool, 3, a, lease);
    check(until([&] { return lease.registration.load() != 0 && pool.items_in_hand() == 3; }),
          "the lease's first segment armed a resumption and returned: suspended (3 bodies still in hand)");
    r = notes.resident();
    check(r.size() == 3 && r[2].task == sh && r[2].held == 3, "the lease's first segment: noted, 3 held");
    lease.arm_on_run.store(false);
    std::atomic<KilnResult> finished{KILN_ERR_INTERNAL};
    lease.after_release = [&] { finished = pool.finish(sh, Outcome::Completed); };
    check(pool.resume(sh, lease.registration.load()) == KILN_OK, "the host resumes it");
    check(until([&] { return lease.done_calls.load() == 1; }) && lease.body_runs.load() == 2 && finished.load() == KILN_OK,
          "its second segment ran and finished it");
    check(notes.resident().size() == 3, "the lease's second segment is not noted again");
    loop.release.open();
    check(until([&] { return loop.done_calls.load() == 1; }), "the first resident body returned");
    const KilnHandle th = submit(pool, 4, resident("later"), later);
    check(until([&] { return later.body_runs.load() == 1; }), "a later resident body runs");
    r = notes.resident();
    check(r.size() == 4 && r[3].task == th && r[3].held == 2,
          "its note counts only the resident bodies still running (the worker and itself: 2)");
    PoolNote unnamed;
    unnamed.kind = PoolNote::Kind::ResidentStarted;
    unnamed.owner = 7;
    unnamed.task = 0x2a;
    unnamed.name = "x";
    unnamed.held = 1;
    unnamed.long_running = 8;
    check(resident_started_line(unnamed).rfind("kiln.tasks: owner 7/x (task 0x2a) started as a resident task", 0) == 0,
          "an owner without a name is named by its id");
    plain.release.open();
    worker.release.open();
    later.release.open();
    check(until([&] { return pool.counts().outstanding == 0; }), "quiescent");
    pool.set_note_sink(nullptr, nullptr);
}

// At most one resident task per owner. A second one is refused INVALID_ARGUMENT + Argument, with nothing held, while
// the first is queued, running, cancelled with its body still running, or a managed lease between segments. A plain
// LongRunning task of the same owner and another owner's resident task are accepted, and the owner's next resident
// task is accepted from the first one's done.
struct Resubmit : Job {
    std::atomic<bool> tried{false};
    std::atomic<KilnResult> result{KILN_ERR_INTERNAL};
    std::atomic<KilnHandle> handle{0};
    Job* next = nullptr;
};

void KILN_CALL resubmit_done(KilnHandle, uint32_t, void* user) {
    auto* r = static_cast<Resubmit*>(static_cast<Job*>(user));
    KilnHandle h = 0;
    Refusal why = Refusal::None;
    r->result = r->pool->submit(1, resident("next"), &job_body, &job_done, r->next, &h, &why);
    r->handle = h;
    r->tried = true;
}

KILN_TASKS_TEST(resident_one_per_owner) {
    HostPlatform platform;
    // before the pool: a released body still reads its job on its way out
    Resubmit first;
    Job fill1, fill2, plain, other, lease, next, stray, again;
    for (Job* j : {static_cast<Job*>(&first), &fill1, &fill2, &plain, &other, &lease, &next, &stray, &again})
        j->platform = &platform;
    stray.release.open();
    Pool pool(config(1, 5), &platform);
    pool.open_owner(1, "mod.one");
    pool.open_owner(2, "mod.two");
    pool.open_owner(3, "mod.three");
    const auto refused = [&](KilnOwner owner, const std::string& what) {
        KilnHandle h = 7;
        Refusal why = Refusal::None;
        const auto records = pool.counts().records;
        const auto held = platform.acquires() - platform.releases();
        check(pool.submit(owner, resident("second"), &job_body, &job_done, &stray, &h, &why) ==
                      KILN_ERR_INVALID_ARGUMENT &&
                  why == Refusal::Argument && h == 0 && pool.counts().records == records &&
                  platform.acquires() - platform.releases() == held,
              what + ": INVALID_ARGUMENT + Argument, nothing held");
    };
    // queued: the owner's two LongRunning slots are taken by plain tasks
    submit(pool, 1, named("fill1", Priority::Normal, true), fill1);
    submit(pool, 1, named("fill2", Priority::Normal, true), fill2);
    check(until([&] { return fill1.body_runs.load() == 1 && fill2.body_runs.load() == 1; }), "two plain tasks run");
    first.pool = &pool;
    first.next = &next;
    KilnHandle fh = 0;
    check(pool.submit(1, resident("first"), &job_body, &resubmit_done, static_cast<Job*>(&first), &fh, nullptr) ==
                  KILN_OK &&
              fh != 0,
          "the owner's resident task is accepted");
    State st = State::None;
    check(pool.status(1, fh, &st) == KILN_OK && st == State::Queued, "and queued");
    refused(1, "a second one while the first is queued");
    submit(pool, 1, named("plain", Priority::Normal, true), plain);
    submit(pool, 2, resident("other"), other);
    check(until([&] { return other.body_runs.load() == 1; }), "a plain LongRunning task and another owner's resident "
                                                              "task are accepted; that one runs");
    fill1.release.open();
    fill2.release.open();
    check(until([&] { return first.body_runs.load() == 1 && plain.body_runs.load() == 1; }), "the first one runs");
    refused(1, "a second one while the first runs");
    check(pool.cancel(1, fh, &st) == KILN_OK && st == State::Running, "cancel: asked, the body has not returned");
    refused(1, "a second one while the cancelled first one's body still runs");
    // a resident lease between its segments (it armed a resumption and returned)
    lease.pool = &pool;
    lease.arm_on_run.store(true);
    lease.release.open();
    SubmitArgs a = resident("lease");
    a.lease = true;
    const KilnHandle sh = submit(pool, 3, a, lease);
    check(until([&] { return lease.registration.load() != 0 && pool.items_in_hand() == 3; }),
          "the lease's first segment armed a resumption and returned");
    refused(3, "a second one while the owner's resident lease is suspended");
    lease.arm_on_run.store(false);
    lease.after_release = [&] { pool.finish(sh, Outcome::Completed); };
    check(pool.resume(sh, lease.registration.load()) == KILN_OK, "the host resumes the lease");
    check(until([&] { return lease.done_calls.load() == 1; }), "the lease finished");
    // the first one's body returns; its done submits the owner's next resident task
    first.release.open();
    check(until([&] { return first.tried.load(); }) && first.result.load() == KILN_OK && first.handle.load() != 0,
          "the owner's next resident task is accepted from the first one's done");
    check(until([&] { return next.body_runs.load() == 1 && pool.items_in_hand() == 3; }),
          "it runs; the first one is gone");
    refused(1, "a second one beside that next one");
    next.release.open();
    plain.release.open();
    other.release.open();
    check(until([&] { return pool.counts().outstanding == 0; }), "quiescent");
    again.release.open();
    submit(pool, 1, resident("again"), again);
    check(until([&] { return again.done_calls.load() == 1; }), "once they ended, the owner's resident task is accepted");
    check(stray.body_runs.load() == 0, "no refused submission ran");
}

// The exit and the owner's state refuse before the one-per-owner cap, as they would any task: while the owner's first
// resident body still runs, a second resident task of a quarantined, sealed (drained) or revoked owner, or one after
// the exit began, gets that refusal, not the cap's Argument. A healthy owner gets the cap's.
KILN_TASKS_TEST(resident_cap_after_owner_state) {
    HostPlatform platform;
    Job quarantined, sealed, revoked, exiting, stray;  // before the pool: a released body still reads its job
    for (Job* j : {&quarantined, &sealed, &revoked, &exiting, &stray}) j->platform = &platform;
    stray.release.open();
    Pool pool(config(1, 4), &platform);
    pool.open_owner(1, "mod.quarantined");
    pool.open_owner(2, "mod.sealed");
    pool.open_owner(3, "mod.revoked");
    pool.open_owner(4, "mod.exiting");
    submit(pool, 1, resident("first"), quarantined);
    submit(pool, 2, resident("first"), sealed);
    submit(pool, 3, resident("first"), revoked);
    submit(pool, 4, resident("first"), exiting);
    check(until([&] {
        return quarantined.body_runs.load() == 1 && sealed.body_runs.load() == 1 && revoked.body_runs.load() == 1 &&
               exiting.body_runs.load() == 1;
    }), "each owner's first resident body runs");
    const auto second = [&](KilnOwner owner, KilnResult want, Refusal want_why, const std::string& what) {
        KilnHandle h = 7;
        Refusal why = Refusal::None;
        const auto records = pool.counts().records;
        const auto held = platform.acquires() - platform.releases();
        const KilnResult r = pool.submit(owner, resident("second"), &job_body, &job_done, &stray, &h, &why);
        check(r == want && why == want_why && h == 0 && pool.counts().records == records &&
                  platform.acquires() - platform.releases() == held,
              what + " (got " + std::to_string(r) + ", reason " + std::to_string(static_cast<uint32_t>(why)) + ")");
    };
    second(4, KILN_ERR_INVALID_ARGUMENT, Refusal::Argument, "a healthy owner: the cap's INVALID_ARGUMENT + Argument");
    check(pool.quarantine(1) == KILN_OK, "owner 1 quarantined");
    second(1, KILN_ERR_UNAVAILABLE, Refusal::OwnerQuarantined, "a quarantined owner: UNAVAILABLE + OwnerQuarantined");
    uint32_t left = 0;
    check(pool.drain_owner(2, 0, &left) == KILN_OK && left == 1, "owner 2 drained (sealed), its body still runs");
    second(2, KILN_ERR_UNAVAILABLE, Refusal::OwnerSealed, "a sealed owner: UNAVAILABLE + OwnerSealed");
    pool.retire_owner(3, nullptr);
    second(3, KILN_ERR_REVOKED, Refusal::OwnerSealed, "a revoked owner: REVOKED + OwnerSealed");
    pool.close();
    second(4, KILN_ERR_UNAVAILABLE, Refusal::Shutdown, "after the exit began: UNAVAILABLE + Shutdown");
    quarantined.release.open();
    sealed.release.open();
    revoked.release.open();
    exiting.release.open();
    check(until([&] { return pool.counts().outstanding == 0; }), "quiescent");
    check(stray.body_runs.load() == 0, "no refused submission ran");
}
}  // namespace

int main(int argc, char** argv) { return kiln::tasks::test::run(argc, argv, "tasks pool"); }
