// SPDX-License-Identifier: MIT
// kiln.tasks v1 through the binding (src/tasks.cpp Service, the table services/tasks.cpp publishes): argument and
// owner validation with the refusal out on every non-OK return, record lifetime and release, queue full, drain's thread
// rules and seal, the capability and shutdown refusals, and submit forced against an owner revoke. Over the host test
// platform; built twice by test-tasks.sh (ASan/UBSan and TSan); `tasks_api_test <name>` runs one test.
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <set>
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

namespace kiln::tasks {
Platform& default_platform() {
    static auto* host = new HostPlatform;  // never used: every pool here gets an explicit platform
    return *host;
}
}  // namespace kiln::tasks

namespace {

thread_local bool t_tick = false;  // this test thread plays the tick / a world thread

class FakeEnv : public ServiceEnv {
public:
    bool on_tick_or_world_thread() override { return t_tick; }
    bool owner_alive(KilnOwner owner) override {
        std::lock_guard lock(mutex_);
        return alive_.count(owner) != 0;
    }
    std::string owner_name(KilnOwner owner) override { return "mod." + std::to_string(owner); }
    void add(KilnOwner owner) {
        std::lock_guard lock(mutex_);
        alive_.insert(owner);
    }
    void kill(KilnOwner owner) {
        std::lock_guard lock(mutex_);
        alive_.erase(owner);
    }

private:
    std::mutex mutex_;
    std::set<KilnOwner> alive_;
};

struct Job {
    Gate release;
    std::atomic<int> body_runs{0}, done_calls{0};
    std::atomic<uint32_t> done_state{0}, saw_cancel{0}, run_size{0};
    std::atomic<KilnResult> inner{KILN_OK};
    Service* service = nullptr;  // optional: the body drains its own owner (a pool worker)
};

void KILN_CALL body(const KilnTaskRun* run, void* user) {
    auto* j = static_cast<Job*>(user);
    j->run_size.store(run->size);
    j->body_runs.fetch_add(1);
    if (j->service) {
        uint32_t left = 0;
        j->inner.store(j->service->drain_owner(run->owner, 0, &left));
    }
    j->release.wait();
    j->saw_cancel.store(run->cancelled(run));
}

void KILN_CALL done(KilnHandle, uint32_t state, void* user) {
    auto* j = static_cast<Job*>(user);
    j->done_state.store(state);
    j->done_calls.fetch_add(1);
}

// A managed host's dispatcher that is never called (only refused admissions use it).
void KILN_CALL no_dispatch(KilnLease, uint32_t, uint32_t, void*) {}

template <class F>
bool until(F pred, int ms = 5000) {
    for (int i = 0; i < ms * 10 && !pred(); ++i) std::this_thread::sleep_for(std::chrono::microseconds(100));
    return pred();
}

KilnTaskInfo info(const char* name, uint32_t flags = 0, uint32_t priority = 0) {
    KilnTaskInfo i{};
    i.size = sizeof(KilnTaskInfo);
    i.flags = flags;
    i.name = {name, std::strlen(name)};
    i.priority = priority;
    return i;
}

PoolConfig config(uint32_t capacity = 64) {
    PoolConfig c;
    c.normal_workers = 2;
    c.long_running_workers = 2;
    c.limits.owner_capacity = capacity;
    return c;
}

uint32_t state_of(Service& s, KilnOwner owner, KilnHandle h) {
    uint32_t st = 99;
    s.status(owner, h, &st);
    return st;
}

// A refused submit: the result, the refusal, *task = 0, and nothing accepted or referenced.
void refused(Service& s, HostPlatform& p, Pool& pool, KilnOwner owner, const KilnTaskInfo* i, KilnTaskBodyFn b,
             KilnTaskDoneFn d, KilnResult want, uint32_t why_want, const std::string& what) {
    Job j;
    KilnHandle h = 77;
    uint32_t why = 99;
    const auto records = pool.counts().records;
    const auto held = p.acquires() - p.releases();  // other tasks in flight may hold references
    const KilnResult r = s.submit(owner, i, b, d, &j, &h, &why);
    check(r == want && why == why_want && h == 0 && pool.counts().records == records &&
              p.acquires() - p.releases() == held,
          what + " (got " + std::to_string(r) + "/" + std::to_string(why) + ")");
}

// ---------------------------------------------------------------------------------------------------------------
KILN_TASKS_TEST(owner_zero_refused_everywhere) {
    HostPlatform p;
    FakeEnv env;
    Pool pool(config(), &p);
    Service s(pool, env);
    const KilnTaskInfo i = info("t");
    refused(s, p, pool, 0, &i, &body, &done, KILN_ERR_INVALID_ARGUMENT, KILN_TASK_REFUSAL_ARGUMENT, "submit owner 0");
    uint32_t st = 9, left = 9;
    check(s.cancel(0, 1, &st) == KILN_ERR_INVALID_ARGUMENT && s.status(0, 1, &st) == KILN_ERR_INVALID_ARGUMENT &&
              s.release(0, 1) == KILN_ERR_INVALID_ARGUMENT && s.drain_owner(0, 0, &left) == KILN_ERR_INVALID_ARGUMENT,
          "owner 0: cancel/status/release/drain_owner refuse INVALID_ARGUMENT");
}

KILN_TASKS_TEST(malformed_requests_change_nothing) {
    HostPlatform p;
    FakeEnv env;
    env.add(1);
    Pool pool(config(), &p);
    Service s(pool, env);
    const KilnTaskInfo ok = info("ok");
    const auto bad = [&](KilnTaskInfo i, const std::string& what) {
        refused(s, p, pool, 1, &i, &body, &done, KILN_ERR_INVALID_ARGUMENT, KILN_TASK_REFUSAL_ARGUMENT, what);
    };
    refused(s, p, pool, 1, nullptr, &body, &done, KILN_ERR_INVALID_ARGUMENT, KILN_TASK_REFUSAL_ARGUMENT, "null info");
    refused(s, p, pool, 1, &ok, nullptr, &done, KILN_ERR_INVALID_ARGUMENT, KILN_TASK_REFUSAL_ARGUMENT, "null body");
    refused(s, p, pool, 1, &ok, &body, nullptr, KILN_ERR_INVALID_ARGUMENT, KILN_TASK_REFUSAL_ARGUMENT, "null done");
    {
        Job j;
        uint32_t why = 99;
        check(s.submit(1, &ok, &body, &done, &j, nullptr, &why) == KILN_ERR_INVALID_ARGUMENT &&
                  why == KILN_TASK_REFUSAL_ARGUMENT && pool.counts().records == 0,
              "null task out: ARGUMENT, nothing accepted");
    }
    KilnTaskInfo i = ok;
    i.size = sizeof(KilnTaskInfo) - 1;
    bad(i, "a short info");
    i = ok;
    i.flags = 1u << 3;
    bad(i, "an unknown flag");
    i.flags = KILN_TASK_LONG_RUNNING | (1u << 31);
    bad(i, "an unknown flag beside a known one");
    i = ok;
    i.priority = 3;
    bad(i, "priority 3");
    i = ok;
    i.reserved = 1;
    bad(i, "reserved != 0");
    i = ok;
    i.name = {"", 0};
    bad(i, "an empty name");
    const std::string long_name(129, 'a');
    i.name = {long_name.data(), long_name.size()};
    bad(i, "a 129-byte name");
    i.name = {"a\0b", 3};
    bad(i, "an embedded NUL");
    i.name = {"a\tb", 3};
    bad(i, "a control character");
    i.name = {"a\xC3", 2};
    bad(i, "invalid UTF-8");
    i.name = {nullptr, 4};
    bad(i, "a NULL name pointer with a length");
    // a longer (newer) record is accepted; its extra bytes are ignored
    struct Longer {
        KilnTaskInfo v1;
        uint64_t later;
    } longer{ok, 0xFFFF};
    longer.v1.size = sizeof(Longer);
    Job j;
    j.release.open();
    KilnHandle h = 0;
    uint32_t why = 1234;
    check(s.submit(1, &longer.v1, &body, &done, &j, &h, &why) == KILN_OK && h != 0 && why == 1234,
          "a longer record is accepted; the refusal out is untouched on OK");
    check(until([&] { return j.done_calls.load() == 1; }) && j.run_size.load() == sizeof(KilnTaskRun),
          "it runs; KilnTaskRun.size says what Kiln filled (40)");
    const std::string max_name(128, 'b');
    i = ok;
    i.name = {max_name.data(), max_name.size()};
    Job j2;
    j2.release.open();
    check(s.submit(1, &i, &body, &done, &j2, &h, nullptr) == KILN_OK, "a 128-byte name, NULL refusal pointer: accepted");
    check(until([&] { return j2.done_calls.load() == 1; }), "and runs");
}

KILN_TASKS_TEST(dead_and_unknown_owners) {
    HostPlatform p;
    FakeEnv env;
    env.add(1);
    env.add(2);
    Pool pool(config(), &p);
    Service s(pool, env);
    const KilnTaskInfo i = info("t");
    refused(s, p, pool, 9, &i, &body, &done, KILN_ERR_REVOKED, KILN_TASK_REFUSAL_OWNER_SEALED, "a dead owner: REVOKED");
    uint32_t st = 9;
    check(s.status(2, 12345, &st) == KILN_ERR_NOT_FOUND && st == KILN_TASK_NONE,
          "a live owner that never submitted: NOT_FOUND, NONE");
    Job j;
    j.release.open();
    KilnHandle h = 0;
    check(s.submit(1, &i, &body, &done, &j, &h, nullptr) == KILN_OK, "owner 1 submits");
    check(until([&] { return j.done_calls.load() == 1; }), "it ran");
    check(s.status(2, h, &st) == KILN_ERR_NOT_FOUND && st == KILN_TASK_NONE && s.release(2, h) == KILN_ERR_NOT_FOUND,
          "another owner's handle is NOT_FOUND");
    env.kill(1);
    s.owner_retiring(1);
    check(s.status(1, h, &st) == KILN_ERR_REVOKED && s.cancel(1, h, &st) == KILN_ERR_REVOKED,
          "after revoke: REVOKED");
}

KILN_TASKS_TEST(lifecycle_and_release) {
    HostPlatform p;
    FakeEnv env;
    env.add(1);
    Pool pool(config(), &p);
    Service s(pool, env);
    const KilnTaskInfo i = info("work");
    Job j;
    KilnHandle h = 0;
    check(s.submit(1, &i, &body, &done, &j, &h, nullptr) == KILN_OK && h, "submit");
    check(until([&] { return j.body_runs.load() == 1; }), "running");
    check(state_of(s, 1, h) == KILN_TASK_RUNNING, "status RUNNING");
    check(s.release(1, h) == KILN_ERR_UNAVAILABLE && state_of(s, 1, h) == KILN_TASK_RUNNING,
          "release of a running task: UNAVAILABLE, nothing changes, not cancelled");
    uint32_t st = 0;
    check(s.cancel(1, h, &st) == KILN_OK && st == KILN_TASK_RUNNING, "cancel a running task: requested, still RUNNING");
    j.release.open();
    check(until([&] { return j.done_calls.load() == 1; }), "done ran");
    check(j.saw_cancel.load() == 1, "the body saw cancelled() == 1");
    check(j.done_state.load() == KILN_TASK_COMPLETED && state_of(s, 1, h) == KILN_TASK_COMPLETED,
          "a native body that returns after a cancel request is COMPLETED");
    check(s.release(1, h) == KILN_OK && s.release(1, h) == KILN_OK, "release, and again (idempotent)");
    check(s.status(1, h, &st) == KILN_ERR_NOT_FOUND && st == KILN_TASK_NONE, "a released handle: NOT_FOUND, NONE");
    check(s.status(1, h, nullptr) == KILN_ERR_INVALID_ARGUMENT, "status needs its out pointer");
}

KILN_TASKS_TEST(queue_full) {
    HostPlatform p;
    FakeEnv env;
    env.add(1);
    Pool pool(config(2), &p);
    Service s(pool, env);
    const KilnTaskInfo i = info("t");
    Job a, b;
    a.release.open();
    b.release.open();
    KilnHandle ha = 0, hb = 0;
    s.submit(1, &i, &body, &done, &a, &ha, nullptr);
    s.submit(1, &i, &body, &done, &b, &hb, nullptr);
    check(until([&] { return a.done_calls.load() == 1 && b.done_calls.load() == 1; }), "two tasks ran");
    refused(s, p, pool, 1, &i, &body, &done, KILN_ERR_UNAVAILABLE, KILN_TASK_REFUSAL_QUEUE_FULL,
            "at the cap (terminal records count): UNAVAILABLE + QUEUE_FULL");
    check(s.release(1, ha) == KILN_OK, "release one terminal task");
    Job c;
    c.release.open();
    KilnHandle hc = 0;
    check(s.submit(1, &i, &body, &done, &c, &hc, nullptr) == KILN_OK, "its slot is back");
    check(until([&] { return c.done_calls.load() == 1; }), "and it runs");
}

KILN_TASKS_TEST(drain_thread_rules_and_seal) {
    HostPlatform p;
    FakeEnv env;
    env.add(1);
    env.add(2);
    Pool pool(config(), &p);
    Service s(pool, env);
    const KilnTaskInfo i = info("t");
    Job j;
    KilnHandle h = 0;
    s.submit(1, &i, &body, &done, &j, &h, nullptr);
    check(until([&] { return j.body_runs.load() == 1; }), "running");
    uint32_t left = 99;
    std::thread tick([&] {
        t_tick = true;
        check(s.drain_owner(1, 10, &left) == KILN_ERR_WRONG_THREAD, "a timed drain on the tick: WRONG_THREAD");
        check(s.drain_owner(2, 0, &left) == KILN_OK && left == 0, "drain 0 on the tick: seal + poll");
    });
    tick.join();
    check(s.drain_owner(1, 20, &left) == KILN_OK && left == 1, "a timed-out drain: OK with running > 0, nothing forced");
    refused(s, p, pool, 1, &i, &body, &done, KILN_ERR_UNAVAILABLE, KILN_TASK_REFUSAL_OWNER_SEALED,
            "a drained owner is sealed for good");
    j.release.open();
    check(s.drain_owner(1, 5000, &left) == KILN_OK && left == 0, "an off-tick timed drain waits for quiescence");
    // from inside a body (a pool worker): any drain is WRONG_THREAD
    env.add(3);
    Job self;
    self.service = &s;
    self.release.open();
    KilnHandle hs = 0;
    s.submit(3, &i, &body, &done, &self, &hs, nullptr);
    check(until([&] { return self.done_calls.load() == 1; }) && self.inner.load() == KILN_ERR_WRONG_THREAD,
          "drain_owner(0) from a pool worker: WRONG_THREAD (no self-drain)");
    Job other;
    KilnHandle ho = 0;
    check(s.submit(3, &i, &body, &done, &other, &ho, nullptr) == KILN_OK, "that refused drain did not seal the owner");
    other.release.open();
    check(until([&] { return other.done_calls.load() == 1; }), "and its task runs");
}

KILN_TASKS_TEST(capability_and_shutdown) {
    {
        HostPlatform p;
        p.fail_starts("kiln-task-N", 2);
        FakeEnv env;
        env.add(1);
        PoolConfig c = config();
        c.restart_retry_ns = 60ull * 1000 * 1000 * 1000;
        Pool pool(c, &p);
        Service s(pool, env);
        const KilnTaskInfo i = info("t");
        refused(s, p, pool, 1, &i, &body, &done, KILN_ERR_UNAVAILABLE, KILN_TASK_REFUSAL_CAPABILITY,
                "no worker could start: UNAVAILABLE + CAPABILITY");
    }
    HostPlatform p;
    FakeEnv env;
    env.add(1);
    Pool pool(config(), &p);
    Service s(pool, env);
    const KilnTaskInfo i = info("t");
    Job running, queued, q2;
    q2.release.open();  // were it (wrongly) started, it would return: the test fails instead of hanging
    KilnHandle hr = 0, hq = 0, hq2 = 0;
    s.submit(1, &i, &body, &done, &running, &hr, nullptr);
    s.submit(1, &i, &body, &done, &queued, &hq, nullptr);
    check(until([&] { return running.body_runs.load() == 1 && queued.body_runs.load() == 1; }), "two running (cap 2)");
    s.submit(1, &i, &body, &done, &q2, &hq2, nullptr);
    s.exiting();
    refused(s, p, pool, 1, &i, &body, &done, KILN_ERR_UNAVAILABLE, KILN_TASK_REFUSAL_SHUTDOWN,
            "after the exit began: UNAVAILABLE + SHUTDOWN");
    check(state_of(s, 1, hq2) == KILN_TASK_CANCELLED || state_of(s, 1, hq2) == KILN_TASK_NONE,
          "a queued task is cancelled at exit");
    running.release.open();
    queued.release.open();
    check(until([&] { return running.done_calls.load() == 1; }) && running.saw_cancel.load() == 1,
          "a running body is signalled at exit");
    check(until([&] { return pool.counts().outstanding == 0; }), "quiescent (every job outlives its callbacks)");
}

// KILN_TASK_RESIDENT only with KILN_TASK_LONG_RUNNING: alone it is refused INVALID_ARGUMENT + ARGUMENT like an unknown
// bit, before any side effect (no owner opened, nothing referenced), on submit and submit_in alike. With it the task is
// accepted, also with AFTER_STEP, through submit_in and at any priority; it runs, cancel reaches its body, and the
// watchdog does not report it running before that cancel.
KILN_TASKS_TEST(resident_flag) {
    HostPlatform p;
    FakeEnv env;
    env.add(1);
    PoolConfig c = config();
    c.running_watchdog_ns = 1000;
    c.queued_watchdog_ns = 1000ull * 1000 * 1000 * 1000;
    Pool pool(c, &p);
    Service s(pool, env);
    const auto argument = [&](uint32_t flags, const std::string& what) {
        const KilnTaskInfo i = info("loop", flags);
        refused(s, p, pool, 1, &i, &body, &done, KILN_ERR_INVALID_ARGUMENT, KILN_TASK_REFUSAL_ARGUMENT, what);
    };
    argument(KILN_TASK_RESIDENT, "RESIDENT alone: INVALID_ARGUMENT + ARGUMENT");
    argument(KILN_TASK_RESIDENT | KILN_TASK_AFTER_STEP, "RESIDENT | AFTER_STEP: ARGUMENT (before the world check)");
    {
        Job j;
        KilnHandle h = 77;
        uint32_t why = 99;
        const KilnTaskInfo i = info("loop", KILN_TASK_RESIDENT);
        check(s.submit_in(1, &i, KILN_WORLD_SERVER, 0, &body, &done, &j, &h, &why) == KILN_ERR_INVALID_ARGUMENT &&
                  why == KILN_TASK_REFUSAL_ARGUMENT && h == 0,
              "submit_in: RESIDENT alone is ARGUMENT (before NO_WORLD)");
    }
    {
        // the managed host's admit shares the check: refused the same way (its lease context is freed)
        check(s.host_bind(&no_dispatch) == KILN_OK, "a host dispatcher is bound");
        KilnLease l = 77;
        uint32_t why = 99;
        const KilnTaskInfo i = info("loop", KILN_TASK_RESIDENT);
        check(s.host_admit(1, &i, nullptr, &l, &why) == KILN_ERR_INVALID_ARGUMENT && why == KILN_TASK_REFUSAL_ARGUMENT &&
                  l == 0,
              "host_admit: RESIDENT alone is ARGUMENT");
    }
    check(pool.counts().owners == 0 && pool.counts().records == 0 && p.acquires() == p.releases(),
          "no refusal opened the owner or referenced a module");
    // accepted: plain, with AFTER_STEP, and bound with submit_in, at High priority
    s.session_started(KILN_WORLD_SERVER, 1);
    Job plain, after, bound;
    KilnHandle hp = 0, ha = 0, hb = 0;
    uint32_t why = 1234;
    KilnTaskInfo i = info("loop", KILN_TASK_RESIDENT | KILN_TASK_LONG_RUNNING, KILN_TASK_PRIORITY_HIGH);
    check(s.submit(1, &i, &body, &done, &plain, &hp, &why) == KILN_OK && hp != 0 && why == 1234,
          "RESIDENT | LONG_RUNNING is accepted (High priority)");
    i = info("after", KILN_TASK_RESIDENT | KILN_TASK_LONG_RUNNING | KILN_TASK_AFTER_STEP);
    check(s.submit(1, &i, &body, &done, &after, &ha, &why) == KILN_OK && state_of(s, 1, ha) == KILN_TASK_QUEUED,
          "with AFTER_STEP: accepted, waiting for the Post edge");
    i = info("bound", KILN_TASK_RESIDENT | KILN_TASK_LONG_RUNNING);
    check(s.submit_in(1, &i, KILN_WORLD_SERVER, 1, &body, &done, &bound, &hb, &why) == KILN_OK && hb != 0,
          "submit_in: accepted");
    check(until([&] { return plain.body_runs.load() == 1; }), "the plain one runs");
    check(s.post_edge(KILN_WORLD_SERVER, 1, 1), "a Post edge");
    // the owner's two LongRunning slots: `plain` holds one, the edge made `after` runnable beside `bound`
    check(until([&] { return after.body_runs.load() + bound.body_runs.load() == 1; }), "one more runs in the second slot");
    p.advance(5000);
    std::vector<Watch> reports;
    pool.run_watchdog(+[](const Watch& w, void* user) { static_cast<std::vector<Watch>*>(user)->push_back(w); }, &reports);
    check(reports.empty(), "the watchdog reports none of them running (" + std::to_string(reports.size()) + ")");
    uint32_t st = 0;
    check(s.cancel(1, hp, &st) == KILN_OK && st == KILN_TASK_RUNNING, "cancel: requested, still RUNNING");
    plain.release.open();
    check(until([&] { return plain.done_calls.load() == 1; }) && plain.saw_cancel.load() == 1 &&
              plain.done_state.load() == static_cast<uint32_t>(KILN_TASK_COMPLETED),
          "its body saw cancelled() and returned; its done ran (Completed)");
    check(until([&] { return after.body_runs.load() == 1 && bound.body_runs.load() == 1; }), "the third runs in the freed slot");
    after.release.open();
    bound.release.open();
    check(until([&] { return pool.counts().outstanding == 0; }), "quiescent");
}

// At the exit a resident task is signalled like any other, and the park waits for its body.
KILN_TASKS_TEST(resident_exit_park) {
    HostPlatform p;
    FakeEnv env;
    env.add(1);
    Pool pool(config(), &p);
    Service s(pool, env);
    const KilnTaskInfo i = info("loop", KILN_TASK_RESIDENT | KILN_TASK_LONG_RUNNING);
    Job running;
    KilnHandle h = 0;
    check(s.submit(1, &i, &body, &done, &running, &h, nullptr) == KILN_OK, "accepted");
    check(until([&] { return running.body_runs.load() == 1; }), "it runs");
    s.exiting();
    std::atomic<bool> returned{false};
    Pool::ParkResult parked;
    std::thread parker([&] {
        parked = s.park(5ull * 1000 * 1000 * 1000);
        returned = true;
    });
    check(until([&] { return pool.parked(); }), "the park began");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    check(!returned.load(), "the park waits for the resident body");
    running.release.open();
    parker.join();
    check(parked.running_at_start == 1 && parked.still_running == 0 && running.saw_cancel.load() == 1,
          "it saw its cancel, returned, and the park ended without it");
}

// After the exit began, drain_owner still reports the owner's real running count (seal, cancel queued, poll); with
// timeout 0 it never waits. An owner the pool never saw is still refused UNAVAILABLE. The same after the park, where a
// timed drain returns at once (no done runs any more).
KILN_TASKS_TEST(drain_after_exit_reports_running) {
    HostPlatform p;
    FakeEnv env;
    env.add(1);
    env.add(2);
    Pool pool(config(), &p);
    Service s(pool, env);
    const KilnTaskInfo i = info("t");
    Job running;
    KilnHandle h = 0;
    s.submit(1, &i, &body, &done, &running, &h, nullptr);
    check(until([&] { return running.body_runs.load() == 1; }), "a body runs");
    s.exiting();
    uint32_t left = 0;
    KilnResult r = s.drain_owner(1, 0, &left);
    check(r == KILN_OK && left == 1,
          "after exiting(): drain(0) reports the running body (got " + std::to_string(r) + ", " + std::to_string(left) + ")");
    left = 7;
    r = s.drain_owner(2, 0, &left);
    check(r == KILN_ERR_UNAVAILABLE && left == 0, "an owner the pool never saw: UNAVAILABLE, 0 (got " + std::to_string(r) + ")");
    check(pool.counts().owners == 1, "and no owner instance was created for it");
    const Pool::ParkResult parked = s.park(10ull * 1000 * 1000);
    check(parked.still_running == 1, "the park leaves the held body running");
    left = 0;
    r = s.drain_owner(1, 0, &left);
    check(r == KILN_OK && left == 1, "after the park: drain(0) still reports it (got " + std::to_string(r) + ", " +
                                         std::to_string(left) + ")");
    const auto t0 = std::chrono::steady_clock::now();
    r = s.drain_owner(1, 2000, &left);
    check(r == KILN_OK && left == 1 && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(1),
          "a timed drain after the park returns at once with the real count");
    running.release.open();
    check(until([&] { return running.saw_cancel.load() == 1; }), "the body saw its cancel and returns");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    left = 0;
    s.drain_owner(1, 0, &left);
    check(left == 1 && running.done_calls.load() == 0, "its done stays undelivered after the park: still outstanding");
}

// submit forced against an owner revoke between its owner_alive check and its queueing: refused, or accepted and
// then cancelled with its done; the body never runs.
KILN_TASKS_TEST(submit_vs_revoke) {
    for (const auto point : {Service::Point::AfterAliveCheck, Service::Point::AfterCapture}) {
        HostPlatform p;
        FakeEnv env;
        env.add(1);
        Pool pool(config(), &p);
        Service s(pool, env);
        s.session_started(1);
        s.post_edge(1, 1);
        s.set_test_hook([&](Service::Point at) {
            if (at != point) return;
            env.kill(1);
            s.owner_retiring(1);  // the registry's observer, on the revoking thread
        });
        const KilnTaskInfo i = info("raced", point == Service::Point::AfterCapture ? KILN_TASK_AFTER_STEP : 0);
        Job j;
        j.release.open();
        KilnHandle h = 5;
        uint32_t why = 99;
        const KilnResult r = s.submit(1, &i, &body, &done, &j, &h, &why);
        s.set_test_hook(nullptr);
        s.post_edge(1, 2);
        const bool refused_ok = r == KILN_ERR_REVOKED && why == KILN_TASK_REFUSAL_OWNER_SEALED && h == 0;
        const bool cancelled = r == KILN_OK && until([&] { return j.done_calls.load() == 1; }) &&
                               j.done_state.load() == KILN_TASK_CANCELLED;
        check(refused_ok || cancelled, "a revoke inside submit: refused REVOKED, or cancelled with its done (got " +
                                           std::to_string(r) + ")");
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        check(j.body_runs.load() == 0, "the revoked owner's body never ran");
        check(until([&] { return pool.counts().outstanding == 0; }), "quiescent");
    }
}


// a submit that passed its owner_alive check before the owner was revoked must not reopen it.
// Its retired instance (with a callback still running) stays the owner's record, so drain_owner keeps counting it.
KILN_TASKS_TEST(stale_submit_never_reopens) {
    Job running, late;
    late.release.open();
    HostPlatform p;
    FakeEnv env;
    env.add(1);
    Pool pool(config(), &p);
    Service s(pool, env);
    const KilnTaskInfo i = info("t");
    KilnHandle h = 0;
    s.submit(1, &i, &body, &done, &running, &h, nullptr);
    check(until([&] { return running.body_runs.load() == 1; }), "a callback of owner 1 runs");
    s.set_test_hook([&](Service::Point at) {
        if (at != Service::Point::AfterAliveCheck) return;
        env.kill(1);
        s.owner_retiring(1);  // the revoke completes while this submit is between its check and its queueing
    });
    KilnHandle lh = 5;
    uint32_t why = 0;
    const KilnResult r = s.submit(1, &i, &body, &done, &late, &lh, &why);
    s.set_test_hook(nullptr);
    check(r == KILN_ERR_REVOKED && why == KILN_TASK_REFUSAL_OWNER_SEALED && lh == 0, "the stale submit is refused");
    uint32_t left = 0;
    s.drain_owner(1, 0, &left);
    check(left == 1, "drain still counts the retired instance's running callback (got " +
                         std::to_string(left) + ")");
    running.release.open();
    check(until([&] {
        uint32_t l = 9;
        s.drain_owner(1, 0, &l);
        return l == 0;
    }), "quiescent once it returned");
}

// the exit closes root admission for good, also for an owner whose first submit was between
// its checks when the exit began: it never creates an unsealed owner nor queues a body.
KILN_TASKS_TEST(exit_before_first_owner) {
    Job j;
    j.release.open();
    HostPlatform p;
    FakeEnv env;
    env.add(5);
    Pool pool(config(), &p);
    Service s(pool, env);
    s.set_test_hook([&](Service::Point at) {
        if (at == Service::Point::AfterAliveCheck) s.exiting();
    });
    const KilnTaskInfo i = info("first");
    KilnHandle h = 5;
    uint32_t why = 0;
    const KilnResult r = s.submit(5, &i, &body, &done, &j, &h, &why);
    s.set_test_hook(nullptr);
    check(r == KILN_ERR_UNAVAILABLE && why == KILN_TASK_REFUSAL_SHUTDOWN && h == 0,
          "the first submit across the exit is refused SHUTDOWN (got " + std::to_string(r) + "/" +
              std::to_string(why) + ")");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    check(j.body_runs.load() == 0 && pool.counts().records == 0 && pool.counts().owners == 0,
          "nothing was queued, no owner instance was created");
}

// on any exit (also a crash's non-zero one) admission closes lock-free: refuse_new() only stores
// a flag that submit checks before any lock, so it works even with the model's lock stranded by a crashed thread.
KILN_TASKS_TEST(exit_refusal_takes_no_lock) {
    Job stranded;
    stranded.release.open();
    HostPlatform p;
    FakeEnv env;
    env.add(1);
    env.add(2);
    Pool pool(config(), &p);
    Service s(pool, env);
    Gate in_lock, let_go;
    pool.set_model_test_hook([&](Model::Point at) {
        if (at != Model::Point::SubmitPublishing) return;
        in_lock.open();  // this thread holds the model's lock now, as a thread that crashed in it would
        let_go.wait();
    });
    const KilnTaskInfo i = info("t");
    std::thread holder([&] {
        KilnHandle h = 0;
        s.submit(1, &i, &body, &done, &stranded, &h, nullptr);
    });
    in_lock.wait();
    std::atomic<bool> returned{false};
    KilnResult r = KILN_OK;
    uint32_t why = 0;
    KilnHandle h = 5;
    Job late;
    late.release.open();  // were it (wrongly) accepted, it would return: the test fails instead of hanging
    std::thread exiting([&] {
        s.refuse_new();  // what unload_run does on every exit code
        r = s.submit(2, &i, &body, &done, &late, &h, &why);
        returned = true;
    });
    check(until([&] { return returned.load(); }, 2000),
          "with the model's lock held elsewhere, refuse_new and a later submit return at once");
    check(r == KILN_ERR_UNAVAILABLE && why == KILN_TASK_REFUSAL_SHUTDOWN && h == 0, "the submit is refused SHUTDOWN");
    let_go.open();
    holder.join();
    exiting.join();
    pool.set_model_test_hook(nullptr);
    check(until([&] { return stranded.done_calls.load() == 1; }), "the task already in admission completes");
}
}  // namespace

int main(int argc, char** argv) { return kiln::tasks::test::run(argc, argv, "tasks api"); }
