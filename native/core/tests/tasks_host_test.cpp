// SPDX-License-Identifier: MIT
// kiln.internal.tasks.host v1 through the binding (src/tasks.cpp Service::host_*): the managed host's leases on the
// one pool. A lease's segments run the core's trampoline, which calls the bound dispatcher; it suspends without a
// worker, resumes (also after a Post edge), ends with finish() inside a segment, counts once against capacity, keeps
// its continuations after a seal, and shows in the bounded snapshot. Built twice by test-tasks.sh (ASan/UBSan, TSan).
#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <mutex>
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

class Env : public ServiceEnv {
public:
    bool on_tick_or_world_thread() override { return false; }
    bool owner_alive(KilnOwner owner) override { return owner != 0 && owner != 99; }
    std::string owner_name(KilnOwner owner) override { return "mod." + std::to_string(owner); }
};

// The host's dispatcher: one per process in production; here the test's script for each segment.
struct Host {
    Service* service = nullptr;
    std::mutex mutex;
    std::function<void(KilnLease, uint32_t cancelled)> segment;  // what the next segment does
    std::atomic<int> segments{0};
    std::atomic<uint32_t> last_cancelled{9};
};
Host g_host;

void KILN_CALL dispatch(KilnLease lease, uint32_t what, uint32_t cancelled, void* host_state) {
    check(what == KILN_LEASE_SEGMENT && host_state == &g_host, "the dispatcher gets a segment with the host's state");
    g_host.last_cancelled = cancelled;
    std::function<void(KilnLease, uint32_t)> f;
    {
        std::lock_guard lock(g_host.mutex);
        f = g_host.segment;
    }
    g_host.segments.fetch_add(1);
    if (f) f(lease, cancelled);
}
void KILN_CALL other_dispatch(KilnLease, uint32_t, uint32_t, void*) {}

void on_segment(std::function<void(KilnLease, uint32_t)> f) {
    std::lock_guard lock(g_host.mutex);
    g_host.segment = std::move(f);
}

template <class F>
bool until(F pred, int ms = 5000) {
    for (int i = 0; i < ms * 10 && !pred(); ++i) std::this_thread::sleep_for(std::chrono::microseconds(100));
    return pred();
}

KilnTaskInfo info(const char* name, uint32_t flags = 0) {
    KilnTaskInfo i{};
    i.size = sizeof(KilnTaskInfo);
    i.flags = flags;
    i.name = {name, std::strlen(name)};
    return i;
}

PoolConfig config(uint32_t workers = 2, uint32_t capacity = 64) {
    PoolConfig c;
    c.normal_workers = workers;
    c.long_running_workers = 1;
    c.limits.owner_capacity = capacity;
    return c;
}

uint32_t state_of(Service& s, KilnOwner owner, KilnHandle h) {
    uint32_t st = 99;
    s.status(owner, h, &st);
    return st;
}

KILN_TASKS_TEST(bind_and_admit) {
    HostPlatform p;
    Env env;
    Pool pool(config(), &p);
    Service s(pool, env);
    const KilnTaskInfo i = info("lease");
    KilnLease l = 5;
    uint32_t why = 0;
    check(s.host_admit(1, &i, &g_host, &l, &why) == KILN_ERR_UNAVAILABLE && why == KILN_TASK_REFUSAL_CAPABILITY && l == 0,
          "no dispatcher bound: admit refuses UNAVAILABLE + CAPABILITY");
    check(s.host_bind(nullptr) == KILN_ERR_INVALID_ARGUMENT, "bind needs a dispatcher");
    check(s.host_bind(&dispatch) == KILN_OK && s.host_bind(&dispatch) == KILN_OK, "bind, and the same one again");
    check(s.host_bind(&other_dispatch) == KILN_ERR_INVALID_ARGUMENT, "a different dispatcher is refused");
    KilnTaskInfo bad = i;
    bad.flags = 1u << 5;
    check(s.host_admit(1, &bad, &g_host, &l, &why) == KILN_ERR_INVALID_ARGUMENT && why == KILN_TASK_REFUSAL_ARGUMENT,
          "admit validates like submit");
    check(s.host_admit(99, &i, &g_host, &l, &why) == KILN_ERR_REVOKED && why == KILN_TASK_REFUSAL_OWNER_SEALED,
          "admit refuses a dead owner like submit");
}

// A lease suspends without holding a worker, resumes, and ends with finish() inside its last segment.
KILN_TASKS_TEST(lease_lifecycle) {
    HostPlatform p;
    Env env;
    Pool pool(config(1), &p);  // one normal worker: a suspended lease must not hold it
    Service s(pool, env);
    s.host_bind(&dispatch);
    std::atomic<uint64_t> reg{0};
    on_segment([&](KilnLease l, uint32_t) {
        uint64_t r = 0;
        check(s.host_arm(l, &r) == KILN_OK && r != 0, "the first segment arms one resumption");
        reg = r;
    });
    const KilnTaskInfo i = info("managed");
    KilnLease l = 0;
    check(s.host_admit(1, &i, &g_host, &l, nullptr) == KILN_OK && l != 0, "a lease is admitted");
    check(until([&] { return reg.load() != 0 && g_host.segments.load() >= 1; }), "its first segment ran");
    check(state_of(s, 1, l) == KILN_TASK_RUNNING, "suspended, it is RUNNING (its public handle works)");
    // the one worker is free: a native task of another owner runs while the lease is suspended
    struct Native {
        std::atomic<int> done{0};
    } native;
    KilnHandle nh = 0;
    s.submit(2, &i, +[](const KilnTaskRun*, void*) {}, +[](KilnHandle, uint32_t, void* u) { static_cast<Native*>(u)->done++; },
             &native, &nh, nullptr);
    check(until([&] { return native.done.load() == 1; }), "a suspended lease occupies no worker");
    check(s.host_finish(l, 0) == KILN_ERR_INTERNAL, "finish outside a running segment: INTERNAL");
    on_segment([&](KilnLease lease, uint32_t cancelled) {
        check(cancelled == 0, "a normal resumption is not cancelled");
        check(s.host_finish(lease, 0) == KILN_OK, "finish(Completed) inside the last segment");
    });
    check(s.host_resume(l, reg.load()) == KILN_OK, "resume from another thread");
    check(s.host_resume(l, reg.load()) == KILN_ERR_INTERNAL, "a duplicate resume is INTERNAL");
    check(until([&] { return state_of(s, 1, l) == KILN_TASK_COMPLETED; }), "the lease is COMPLETED");
    uint32_t left = 9;
    check(until([&] {
        s.host_seal_owner(1, &left);
        return left == 0;
    }), "seal_owner: quiescent (the core's done trampoline ran)");
    check(p.acquires() == p.releases(), "the trampolines' module references were released");
}

// Continuations are never refused by capacity or a seal; a sealed lease resumes with its cancellation set.
KILN_TASKS_TEST(lease_after_seal_and_capacity) {
    HostPlatform p;
    Env env;
    Pool pool(config(2, 1), &p);  // capacity 1: the lease is the owner's only record
    Service s(pool, env);
    s.host_bind(&dispatch);
    std::atomic<uint64_t> reg{0};
    on_segment([&](KilnLease l, uint32_t) {
        uint64_t r = 0;
        s.host_arm(l, &r);
        reg = r;
    });
    const KilnTaskInfo i = info("sealed");
    KilnLease l = 0;
    s.host_admit(1, &i, &g_host, &l, nullptr);
    check(until([&] { return reg.load() != 0; }), "suspended");
    KilnLease l2 = 7;
    uint32_t why = 0;
    check(s.host_admit(1, &i, &g_host, &l2, &why) == KILN_ERR_UNAVAILABLE && why == KILN_TASK_REFUSAL_QUEUE_FULL,
          "a lease counts once against its owner's capacity");
    uint32_t left = 0;
    s.host_seal_owner(1, &left);
    check(left == 1 && s.host_is_cancelled(l) == 1, "sealed: the lease is outstanding and its cancellation is set");
    on_segment([&](KilnLease lease, uint32_t cancelled) {
        check(cancelled == 1, "the continuation is dispatched with cancelled = 1");
        s.host_finish(lease, 2);  // its own token's cancellation
    });
    check(s.host_resume(l, reg.load()) == KILN_OK, "a continuation is accepted after the seal (and at capacity)");
    check(until([&] {
        s.host_seal_owner(1, &left);
        return left == 0;
    }), "it finished; quiescent");
}

KILN_TASKS_TEST(lease_resume_after_step) {
    HostPlatform p;
    Env env;
    Pool pool(config(), &p);
    Service s(pool, env);
    s.host_bind(&dispatch);
    std::atomic<uint64_t> reg{0};
    on_segment([&](KilnLease l, uint32_t) {
        uint64_t r = 0;
        s.host_arm(l, &r);
        reg = r;
    });
    const KilnTaskInfo i = info("next window");
    KilnLease l = 0;
    s.host_admit(1, &i, &g_host, &l, nullptr);
    check(until([&] { return reg.load() != 0; }), "suspended");
    uint32_t why = 0;
    check(s.host_resume_after_step(l, reg.load(), &why) == KILN_ERR_UNAVAILABLE && why == KILN_TASK_REFUSAL_NO_SERVER_WORLD,
          "no live server world: UNAVAILABLE + NO_SERVER_WORLD");
    s.session_started(4);
    s.post_edge(4, 40);
    const int before = g_host.segments.load();
    on_segment([&](KilnLease lease, uint32_t cancelled) {
        check(cancelled == 0, "woken by its session's edge: not cancelled");
        s.host_finish(lease, 0);
    });
    check(s.host_resume_after_step(l, reg.load(), &why) == KILN_OK, "resume after the next step");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    check(g_host.segments.load() == before, "nothing runs before the next edge");
    s.post_edge(4, 41);
    check(until([&] { return state_of(s, 1, l) == KILN_TASK_COMPLETED; }), "the next edge resumed it; it finished");
}

KILN_TASKS_TEST(lease_without_an_end_faults) {
    HostPlatform p;
    Env env;
    Pool pool(config(), &p);
    Service s(pool, env);
    s.host_bind(&dispatch);
    on_segment(nullptr);  // the host's segment returns without arming or finishing (a host bug)
    const KilnTaskInfo i = info("forgets");
    KilnLease l = 0;
    s.host_admit(1, &i, &g_host, &l, nullptr);
    check(until([&] { return state_of(s, 1, l) == KILN_TASK_FAULTED; }), "a segment that neither arms nor finishes: FAULTED");
}

KILN_TASKS_TEST(lease_snapshot) {
    HostPlatform p;
    Env env;
    Pool pool(config(), &p);
    Service s(pool, env);
    s.host_bind(&dispatch);
    std::atomic<uint64_t> reg{0};
    on_segment([&](KilnLease l, uint32_t) {
        uint64_t r = 0;
        s.host_arm(l, &r);
        reg = r;
    });
    const KilnTaskInfo i = info("listed");
    KilnLease l = 0;
    s.host_admit(1, &i, &g_host, &l, nullptr);
    check(until([&] { return reg.load() != 0; }), "suspended");
    uint32_t count = 9;
    check(s.host_snapshot(nullptr, 0, &count) == KILN_ERR_BUFFER_TOO_SMALL && count == 1, "BUFFER_TOO_SMALL says 1 row");
    KilnLeaseSnapshot rows[2]{};
    check(s.host_snapshot(rows, 2, &count) == KILN_OK && count == 1 && rows[0].size == sizeof(KilnLeaseSnapshot) &&
              rows[0].lease == l && rows[0].owner == 1 && rows[0].state == KILN_TASK_RUNNING &&
              std::string(rows[0].name) == "listed" && rows[0].running_ns == 0,
          "one row: the suspended lease, its owner, RUNNING, its name, no segment time while suspended");
    on_segment([&](KilnLease lease, uint32_t) { s.host_finish(lease, 0); });
    s.host_resume(l, reg.load());
    check(until([&] { return state_of(s, 1, l) == KILN_TASK_COMPLETED; }), "finished");
    check(s.host_snapshot(rows, 2, &count) == KILN_OK && count == 0, "a finished lease is not listed");
}


// ---- tick hops ----
struct Hops {
    std::mutex mutex;
    std::vector<std::pair<KilnLease, uint32_t>> seen;  // (lease, cancelled) in dispatch order
    HostPlatform* platform = nullptr;
    uint64_t cost_ns = 0;  // each hop "takes" this long (the fake clock moves)
};
Hops g_hops;
void KILN_CALL hop_dispatch(KilnLease lease, uint32_t what, uint32_t cancelled, void* host_state) {
    if (what == KILN_LEASE_SEGMENT) return dispatch(lease, what, cancelled, host_state);
    std::lock_guard lock(g_hops.mutex);
    g_hops.seen.emplace_back(lease, cancelled);
    if (g_hops.platform) g_hops.platform->advance(g_hops.cost_ns);
}

// A suspended lease of `owner` (its first segment arms and returns).
KilnLease suspended_lease(Service& s, KilnOwner owner, const char* name) {
    std::atomic<uint64_t> reg{0};
    on_segment([&](KilnLease l, uint32_t) {
        uint64_t r = 0;
        s.host_arm(l, &r);
        reg = r;
    });
    const KilnTaskInfo i = info(name);
    KilnLease l = 0;
    s.host_admit(owner, &i, &g_host, &l, nullptr);
    check(until([&] { return reg.load() != 0; }), std::string("lease ") + name + " suspended");
    on_segment(nullptr);
    return l;
}

// Ends a suspended lease (its first registration, 1) so nothing outlives the test.
void end_lease(Service& s, KilnOwner owner, KilnLease l) {
    on_segment([&](KilnLease lease, uint32_t) { s.host_finish(lease, 2); });
    s.host_resume(l, 1);
    check(until([&] {
        uint32_t st = 0;
        s.status(owner, l, &st);
        return st >= KILN_TASK_COMPLETED || st == KILN_TASK_NONE;
    }), "the test's lease ended");
    on_segment(nullptr);
}

std::size_t hops_of(KilnLease l) {
    std::lock_guard lock(g_hops.mutex);
    std::size_t n = 0;
    for (auto& [lease, c] : g_hops.seen) n += lease == l;
    return n;
}

KILN_TASKS_TEST(tick_hops_need_a_server_world) {
    HostPlatform p;
    Env env;
    Pool pool(config(), &p);
    Service s(pool, env);
    s.host_bind(&hop_dispatch);
    const KilnLease l = suspended_lease(s, 1, "hops");
    check(s.host_tick_hop(l) == KILN_ERR_UNAVAILABLE, "no live server world: a hop refuses at once (never waits for one)");
    s.session_started(1);
    check(s.host_tick_hop(l) == KILN_OK && s.pending_tick_hops() == 1, "with one: queued");
    check(s.host_tick_hop(0x12345) == KILN_ERR_INTERNAL, "an unknown lease: INTERNAL");
    s.run_tick_hops();
    end_lease(s, 1, l);
}

KILN_TASKS_TEST(tick_hops_budgets) {
    HostPlatform p;
    Env env;
    Pool pool(config(), &p);
    Service s(pool, env);
    s.host_bind(&hop_dispatch);
    {
        std::lock_guard lock(g_hops.mutex);
        g_hops.seen.clear();
        g_hops.platform = &p;
        g_hops.cost_ns = 0;
    }
    s.session_started(1);
    const KilnLease a = suspended_lease(s, 1, "a"), b = suspended_lease(s, 2, "b");
    for (int i = 0; i < 70; ++i) s.host_tick_hop(a);
    for (int i = 0; i < 3; ++i) s.host_tick_hop(b);
    check(s.run_tick_hops() == 67 && hops_of(a) == 64 && hops_of(b) == 3,
          "per owner at most 64 hops a step; the other owner is served in the same step");
    check(s.run_tick_hops() == 6 && hops_of(a) == 70 && s.pending_tick_hops() == 0, "the excess waits for the next step");
    // time: 1.5 ms a hop: an owner stops after its 2 ms (two hops), all owners after 4 ms; the turn moves on
    {
        std::lock_guard lock(g_hops.mutex);
        g_hops.seen.clear();
        g_hops.cost_ns = 1500 * 1000;
    }
    const KilnLease c = suspended_lease(s, 3, "c");
    for (const KilnLease l : {a, b, c})
        for (int i = 0; i < 5; ++i) s.host_tick_hop(l);
    const std::size_t first = s.run_tick_hops();
    check(first == 3, "4 ms a step in all, checked between callbacks: 3 hops of 1.5 ms (got " + std::to_string(first) + ")");
    check(s.pending_tick_hops() == 12, "the rest wait, none dropped");
    std::size_t steps = 1;
    while (s.pending_tick_hops() && steps < 20) {
        s.run_tick_hops();
        ++steps;
    }
    check(hops_of(a) == 5 && hops_of(b) == 5 && hops_of(c) == 5, "every hop ran, over later steps");
    {
        std::lock_guard lock(g_hops.mutex);
        bool fifo = true;
        // round-robin: no owner gets more than 2 hops (its 2 ms) before another owner with pending hops is served
        int run = 0;
        KilnLease last = 0;
        for (auto& [l, cancelled] : g_hops.seen) {
            run = l == last ? run + 1 : 1;
            last = l;
            fifo = fifo && run <= 2;
        }
        check(fifo, "an owner's turn ends at its 2 ms; owners take turns");
        g_hops.cost_ns = 0;
    }
    end_lease(s, 1, a);
    end_lease(s, 2, b);
    end_lease(s, 3, c);
}

KILN_TASKS_TEST(tick_hops_cancelled_and_quiescence) {
    HostPlatform p;
    Env env;
    Pool pool(config(), &p);
    Service s(pool, env);
    s.host_bind(&hop_dispatch);
    {
        std::lock_guard lock(g_hops.mutex);
        g_hops.seen.clear();
        g_hops.platform = nullptr;
    }
    s.session_started(1);
    const KilnLease l = suspended_lease(s, 1, "retiring");
    s.host_tick_hop(l);
    uint32_t left = 0;
    s.host_seal_owner(1, &left);
    s.host_tick_hop(l);  // allowed after the seal
    check(s.run_tick_hops() == 2, "both hops ran");
    end_lease(s, 1, l);  // sealed: its continuation is still accepted
    {
        std::lock_guard lock(g_hops.mutex);
        check(g_hops.seen.size() == 2 && g_hops.seen[0].second == 1 && g_hops.seen[1].second == 1,
              "a retiring lease's hops are dispatched with cancelled = 1");
    }
    // a finished lease is not quiescent while a hop is pending; the STOPPING dispatches it cancelled
    const KilnLease m = suspended_lease(s, 2, "pending");
    s.host_tick_hop(m);
    on_segment([&](KilnLease lease, uint32_t) { s.host_finish(lease, 0); });
    // resume the lease's armed registration (its first, 1)
    check(s.host_resume(m, 1) == KILN_OK, "resumed");
    check(until([&] { return state_of(s, 2, m) == KILN_TASK_COMPLETED; }), "finished");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    s.host_seal_owner(2, &left);
    check(left == 1, "finished but not quiescent: a hop is pending");
    s.session_stopping(1);
    check(hops_of(m) == 1 && s.pending_tick_hops() == 0, "the session's STOPPING dispatched the pending hop");
    {
        std::lock_guard lock(g_hops.mutex);
        check(g_hops.seen.back().second == 1, "...with cancelled = 1 (never carried into the next world)");
    }
    check(until([&] {
        s.host_seal_owner(2, &left);
        return left == 0;
    }), "then quiescent");
}

// a fault in the host's dispatcher on a tick hop is contained: that hop still ends, its lease
// is cancelled and its owner quarantined, the fault is reported, the other hops still run; also at STOPPING.
struct FaultEnv : Env {
    std::atomic<int> faults{0};
    std::atomic<KilnLease> lease{0};
    void hop_faulted(KilnOwner, KilnLease l, uint32_t) override {
        lease = l;
        faults++;
    }
};

KILN_TASKS_TEST(tick_hop_fault_contained) {
    for (const bool at_stop : {false, true}) {
        HostPlatform p;
        FaultEnv env;
        Pool pool(config(), &p);
        Service s(pool, env);
        s.host_bind(&hop_dispatch);
        {
            std::lock_guard lock(g_hops.mutex);
            g_hops.seen.clear();
            g_hops.platform = nullptr;
        }
        s.session_started(1);
        const KilnLease a = suspended_lease(s, 1, "faults"), b = suspended_lease(s, 2, "fine");
        s.host_tick_hop(a);
        s.host_tick_hop(b);
        p.fault_next_call_of(reinterpret_cast<uintptr_t>(&hop_dispatch));
        const std::string t = at_stop ? "at STOPPING: " : "on the tick: ";
        bool threw = false;
        try {
            if (at_stop) s.session_stopping(1);
            else s.run_tick_hops();
        } catch (...) {
            threw = true;
        }
        check(!threw, t + "nothing leaves the hop dispatch");
        check(env.faults.load() == 1 && env.lease.load() == a, t + "the fault is reported once, with its lease");
        check(hops_of(b) == 1 && s.pending_tick_hops() == 0, t + "the other owner's hop still ran");
        check(s.host_is_cancelled(a) == 1, t + "the faulting lease is cancelled");
        KilnHandle h = 0;
        uint32_t why = 0;
        const KilnTaskInfo i = info("after");
        check(s.submit(1, &i, +[](const KilnTaskRun*, void*) {}, +[](KilnHandle, uint32_t, void*) {}, nullptr, &h, &why) ==
                      KILN_ERR_UNAVAILABLE &&
                  why == KILN_TASK_REFUSAL_OWNER_QUARANTINED,
              t + "its owner is quarantined");
        end_lease(s, 1, a);
        end_lease(s, 2, b);
        uint32_t left = 9;
        check(until([&] {
            s.host_seal_owner(1, &left);
            return left == 0;
        }), t + "the faulting lease's hop ended: its done arrived, the owner is quiescent");
    }
}

// a hop queued across its session's STOPPING is refused, never carried into the next world.
KILN_TASKS_TEST(tick_hop_across_stopping) {
    HostPlatform p;
    Env env;
    Pool pool(config(), &p);
    Service s(pool, env);
    s.host_bind(&hop_dispatch);
    {
        std::lock_guard lock(g_hops.mutex);
        g_hops.seen.clear();
        g_hops.platform = nullptr;
    }
    s.session_started(1);
    const KilnLease l = suspended_lease(s, 1, "late hop");
    s.set_test_hook([&](Service::Point at) {
        if (at == Service::Point::HopBeforePush) s.session_stopping(1);  // the stop lands between the check and the push
    });
    const KilnResult r = s.host_tick_hop(l);
    s.set_test_hook(nullptr);
    check(r == KILN_ERR_UNAVAILABLE && s.pending_tick_hops() == 0, "a hop that met its session's STOPPING is refused");
    s.session_started(2);
    check(s.run_tick_hops() == 0 && hops_of(l) == 0, "nothing of it runs in the next world");
    end_lease(s, 1, l);
    uint32_t left = 9;
    check(until([&] {
        s.host_seal_owner(1, &left);
        return left == 0;
    }), "no hop holds the lease: quiescent");
}

// a server world that stopped without a destroy event (the next one starts a new session) never
// carries its hops over: they are dispatched cancelled when the new session starts, and their leases stay drainable.
KILN_TASKS_TEST(tick_hops_not_carried_without_a_stop) {
    HostPlatform p;
    Env env;
    Pool pool(config(), &p);
    Service s(pool, env);
    s.host_bind(&hop_dispatch);
    {
        std::lock_guard lock(g_hops.mutex);
        g_hops.seen.clear();
        g_hops.platform = nullptr;
    }
    s.session_started(1);
    const KilnLease l = suspended_lease(s, 1, "old world");
    check(s.host_tick_hop(l) == KILN_OK && s.pending_tick_hops() == 1, "a hop is queued in session 1");
    s.session_started(2);  // no STOPPING for session 1
    check(s.pending_tick_hops() == 0 && hops_of(l) == 1, "the new session dispatched the old session's hop");
    {
        std::lock_guard lock(g_hops.mutex);
        check(!g_hops.seen.empty() && g_hops.seen.back().second == 1, "...with cancelled = 1");
    }
    check(s.run_tick_hops() == 0 && hops_of(l) == 1, "nothing of it runs again in the new world");
    end_lease(s, 1, l);
    uint32_t left = 9;
    check(until([&] {
        s.host_seal_owner(1, &left);
        return left == 0;
    }), "its hop ended: the lease drains");
}
}  // namespace

int main(int argc, char** argv) { return kiln::tasks::test::run(argc, argv, "tasks host"); }
