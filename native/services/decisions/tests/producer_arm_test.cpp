// SPDX-License-Identifier: MIT
// Host test of producer_arm.h: a producer's hook goes in at its first arm (a mod listening), once, from any thread;
// before that nothing is installed; a failed install refuses the kind with its reason and is never retried.
#include <cstdio>
#include <thread>
#include <vector>

#include "../producer_arm.h"

using kiln::decisions::ProducerArm;

static int g_pass = 0, g_fail = 0;
#define CHECK(x) ((x) ? ++g_pass : (++g_fail, std::printf("FAIL line %d: %s\n", __LINE__, #x)))

int main() {
    // unresolved: nothing can go in
    {
        ProducerArm a;
        int installs = 0;
        CHECK(!a.usable() && !a.armed());
        CHECK(!a.arm([&](std::string&) { return ++installs, true; }) && installs == 0);
    }
    // the service's start: resolved, nothing installed yet
    {
        ProducerArm a;
        int installs = 0;
        a.ready();
        CHECK(a.usable() && !a.armed() && installs == 0 && a.state() == ProducerArm::State::Ready);
        // the first listener: installed once; later arms install nothing
        CHECK(a.arm([&](std::string&) { return ++installs, true; }) && installs == 1 && a.armed());
        CHECK(a.arm([&](std::string&) { return ++installs, true; }) && installs == 1);
        a.refuse("late", KILN_CAP_REASON_FAULTED);  // an armed producer stays armed
        CHECK(a.armed() && a.usable());
    }
    // a failed install: refused for the process with its reason, never retried
    {
        ProducerArm a;
        int installs = 0;
        a.ready();
        CHECK(!a.arm([&](std::string& e) { ++installs, e = "kiln.hooks could not hook the target (see kiln.log)"; return false; }));
        CHECK(installs == 1 && !a.armed() && !a.usable() && a.reason() == KILN_CAP_REASON_FAULTED);
        CHECK(a.why() == "kiln.hooks could not hook the target (see kiln.log)");
        CHECK(!a.arm([&](std::string&) { return ++installs, true; }) && installs == 1);
    }
    // refused at the start (a symbol missing): no install, the reason kept
    {
        ProducerArm a;
        int installs = 0;
        a.refuse("feature events.death did not resolve on this game build", KILN_CAP_REASON_SYMBOL_MISSING);
        CHECK(!a.arm([&](std::string&) { return ++installs, true; }) && installs == 0 && a.reason() == KILN_CAP_REASON_SYMBOL_MISSING);
    }
    // eight threads arm at once: one install
    {
        ProducerArm a;
        a.ready();
        std::atomic<int> installs{0}, ok{0};
        std::vector<std::thread> t;
        for (int i = 0; i < 8; ++i)
            t.emplace_back([&] {
                if (a.arm([&](std::string&) { return ++installs, true; })) ++ok;
            });
        for (auto& x : t) x.join();
        CHECK(installs.load() == 1 && ok.load() == 8 && a.armed());
    }
    std::printf("producer_arm_test: %d PASS, %d FAIL\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
