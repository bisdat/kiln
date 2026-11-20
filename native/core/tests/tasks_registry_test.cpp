// SPDX-License-Identifier: MIT
// kiln.tasks against the REAL owner registry (src/registry.cpp, its owner-retiring observers): the binding is the
// registry's observer exactly as services/tasks.cpp registers it (Service::owner_retiring_observer, observer owner 0).
// A revoke forced into submit's alive/queue window, a revoke while a body runs (nonblocking, the body signalled, its
// done still delivered, its module references held until done returned), and an owner revoked between two tasks.
// Built twice by test-tasks.sh (ASan/UBSan and TSan). The registry's fault guard and log are stubs.
#include <windows.h>  // tests/stubs/windows.h

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstring>
#include <thread>

#include "../src/registry.cpp"
#include "tasks.h"
#include "tasks_platform_host.h"
#include "tasks_test_support.h"

namespace kiln {
namespace log {
void writef(int, const char*, const char*, ...) {}
}  // namespace log
namespace guard {
bool task(KilnTaskFn fn, void* user, unsigned long*) {
    fn(user);
    return true;
}
}  // namespace guard
namespace hooks::seh {
bool call_owner_retiring(void(KILN_CALL* fn)(KilnOwner, void*), KilnOwner retiring, void* user, unsigned long*) {
    fn(retiring, user);
    return true;
}
}  // namespace hooks::seh
namespace tasks {
Platform& default_platform() {
    static auto* host = new test::HostPlatform;  // never used: every pool here gets an explicit platform
    return *host;
}
}  // namespace tasks
}  // namespace kiln

using namespace kiln::tasks;
using kiln::tasks::test::check;
using kiln::tasks::test::Gate;
using kiln::tasks::test::HostPlatform;
namespace reg = kiln::reg;

namespace {

class RegistryEnv : public ServiceEnv {
public:
    bool on_tick_or_world_thread() override { return false; }
    bool owner_alive(KilnOwner owner) override { return reg::owner_alive(owner); }
    std::string owner_name(KilnOwner owner) override { return reg::owner_name(owner); }
};

struct Job {
    Gate release;
    std::atomic<int> body_runs{0}, done_calls{0};
    std::atomic<uint32_t> done_state{0}, saw_cancel{0};
};
void KILN_CALL body(const KilnTaskRun* run, void* user) {
    auto* j = static_cast<Job*>(user);
    j->body_runs.fetch_add(1);
    j->release.wait();
    j->saw_cancel.store(run->cancelled(run));
}
void KILN_CALL done(KilnHandle, uint32_t state, void* user) {
    auto* j = static_cast<Job*>(user);
    j->done_state.store(state);
    j->done_calls.fetch_add(1);
}
template <class F>
bool until(F pred, int ms = 5000) {
    for (int i = 0; i < ms * 10 && !pred(); ++i) std::this_thread::sleep_for(std::chrono::microseconds(100));
    return pred();
}
KilnTaskInfo info(const char* name) {
    KilnTaskInfo i{};
    i.size = sizeof(KilnTaskInfo);
    i.name = {name, std::strlen(name)};
    return i;
}
PoolConfig config() {
    PoolConfig c;
    c.normal_workers = 2;
    c.long_running_workers = 1;
    return c;
}

struct Fixture {
    HostPlatform platform;
    RegistryEnv env;
    Pool pool{config(), &platform};
    Service service{pool, env};
    KilnHandle observer = 0;
    Fixture() {
        check(reg::add_owner_retiring(0, &Service::owner_retiring_observer, &service, &observer) == KILN_OK,
              "the binding observes owner retirement (observer owner 0)");
    }
    ~Fixture() { reg::remove_owner_retiring(observer); }
};

KILN_TASKS_TEST(revoke_inside_submit) {
    for (const auto point : {Service::Point::AfterAliveCheck}) {
        Job j;
        j.release.open();
        Fixture f;
        const KilnOwner owner = reg::owner_create("mod.raced");
        f.service.set_test_hook([&](Service::Point at) {
            if (at == point) reg::owner_revoke(owner);  // the real revoke, on this thread, observers included
        });
        const KilnTaskInfo i = info("raced");
        KilnHandle h = 9;
        uint32_t why = 0;
        const KilnResult r = f.service.submit(owner, &i, &body, &done, &j, &h, &why);
        f.service.set_test_hook(nullptr);
        const bool refused = r == KILN_ERR_REVOKED && why == KILN_TASK_REFUSAL_OWNER_SEALED && h == 0;
        const bool cancelled = r == KILN_OK && until([&] { return j.done_calls.load() == 1; }) &&
                               j.done_state.load() == KILN_TASK_CANCELLED;
        check(refused || cancelled, "a real revoke inside submit: refused or cancelled with its done");
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        check(j.body_runs.load() == 0 && f.pool.counts().outstanding == 0, "its body never ran; nothing outstanding");
    }
}

KILN_TASKS_TEST(revoke_while_running) {
    Job running, queued, third;
    Fixture f;
    const KilnOwner owner = reg::owner_create("mod.running");
    const KilnTaskInfo i = info("work");
    KilnHandle h1 = 0, h2 = 0, h3 = 0;
    f.service.submit(owner, &i, &body, &done, &running, &h1, nullptr);
    f.service.submit(owner, &i, &body, &done, &queued, &h2, nullptr);
    check(until([&] { return running.body_runs.load() == 1 && queued.body_runs.load() == 1; }), "two bodies run");
    f.service.submit(owner, &i, &body, &done, &third, &h3, nullptr);  // queued: the owner's two slots are busy
    const auto t0 = std::chrono::steady_clock::now();
    reg::owner_revoke(owner);
    check(std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(500),
          "owner_revoke returned at once while bodies still run (the observer never waits)");
    check(f.platform.module_refcount(reinterpret_cast<uintptr_t>(&body)) >= 2,
          "the running bodies' modules stay referenced after the revoke");
    uint32_t st = 0;
    check(f.service.status(owner, h1, &st) == KILN_ERR_REVOKED, "the revoked owner's handles: REVOKED");
    KilnHandle late = 1;
    uint32_t why = 0;
    Job lj;
    check(f.service.submit(owner, &i, &body, &done, &lj, &late, &why) == KILN_ERR_REVOKED &&
              why == KILN_TASK_REFUSAL_OWNER_SEALED,
          "a later submit of the revoked owner: REVOKED + OWNER_SEALED");
    running.release.open();
    queued.release.open();
    third.release.open();
    check(until([&] { return running.done_calls.load() == 1 && queued.done_calls.load() == 1 &&
                             third.done_calls.load() == 1; }),
          "every accepted task still gets its done after the revoke");
    check(running.saw_cancel.load() == 1 && queued.saw_cancel.load() == 1, "the running bodies were signalled");
    check(third.done_state.load() == KILN_TASK_CANCELLED && third.body_runs.load() == 0,
          "the queued one was cancelled, its body never ran");
    check(until([&] { return f.pool.counts().outstanding == 0 && f.platform.module_refcount(reinterpret_cast<uintptr_t>(&body)) == 0; }),
          "quiescent; the module references were released only after the dones returned");
}

KILN_TASKS_TEST(other_owners_unaffected) {
    Job a, b;
    a.release.open();
    Fixture f;
    const KilnOwner gone = reg::owner_create("mod.gone");
    const KilnOwner stays = reg::owner_create("mod.stays");
    const KilnTaskInfo i = info("t");
    KilnHandle ha = 0, hb = 0;
    f.service.submit(gone, &i, &body, &done, &a, &ha, nullptr);
    check(until([&] { return a.done_calls.load() == 1; }), "the first owner's task ran");
    reg::owner_revoke(gone);
    check(f.service.submit(stays, &i, &body, &done, &b, &hb, nullptr) == KILN_OK, "another owner still submits");
    b.release.open();
    check(until([&] { return b.done_calls.load() == 1; }) && b.done_state.load() == KILN_TASK_COMPLETED,
          "and its task completes");
    reg::owner_revoke(stays);
}

}  // namespace

int main(int argc, char** argv) { return kiln::tasks::test::run(argc, argv, "tasks registry"); }
