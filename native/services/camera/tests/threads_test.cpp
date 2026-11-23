// SPDX-License-Identifier: MIT
// kiln.camera across threads, on a host (run.sh builds it twice: ThreadSanitizer, and AddressSanitizer).
//
// 1. Owner retirement on another thread waits for callbacks into the owner that already started (bounded), and no
//    callback into it starts once it is called: the approved deviation from chapter 03 4.6 (DECISIONS.md, "Camera
//    owner-retiring barrier"). A self-retirement (from inside the owner's own callback) does not wait; a wait that
//    times out says so loudly and returns.
// 2. World stops and owner retirements on another thread against the tick thread's Mode step, writes, results and Pre.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../mode.h"

using namespace kcam;
using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

static std::atomic<int> g_checks{0};
#define CHECK(x)                                                                  \
    do {                                                                          \
        g_checks.fetch_add(1, std::memory_order_relaxed);                         \
        if (!(x)) {                                                               \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x);     \
            std::fflush(stderr);                                                  \
            std::_Exit(1);                                                        \
        }                                                                         \
    } while (0)

namespace {

constexpr KilnEntity A = 1, B = 2;

// Waits up to `limit` for `cond`; false on timeout (a regression fails instead of hanging the suite).
template <class F>
bool wait_for(F cond, milliseconds limit = milliseconds(5000)) {
    const auto end = Clock::now() + limit;
    while (!cond()) {
        if (Clock::now() > end) return false;
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    return true;
}

struct TestEnv : Env {
    std::mutex m;
    std::vector<std::string> errors;
    bool call_done(KilnCameraDone fn, const KilnCameraResult& r, void* user) override { return fn(&r, user), true; }
    bool call_changed(KilnCameraChanged fn, const KilnCameraChange& c, void* user) override { return fn(&c, user), true; }
    void log(int32_t level, const std::string& text) override {
        if (level != KILN_LOG_ERROR) return;
        std::lock_guard<std::mutex> lock(m);
        errors.push_back(text);
    }
    size_t count(const char* needle) {
        std::lock_guard<std::mutex> lock(m);
        size_t n = 0;
        for (const auto& e : errors)
            if (e.find(needle) != std::string::npos) ++n;
        return n;
    }
};

// Takes every request; the test commits by hand.
struct TakeAll : Backend {
    uint64_t caps() const override { return KILN_CAMERA_CAP_SERVER_MODE; }
    KilnResult admit(Stored&, uint32_t*, const char**) override { return KILN_OK; }
    bool stage(const Staged&, uint32_t*, const char**) override { return true; }
    void cancel_update(KilnCameraLease, uint64_t) override {}
    bool release(KilnCameraLease, uint64_t) override { return true; }
    void drop(KilnCameraLease) override {}
    void world_stopped(uint32_t) override {}
};

KilnCameraRequest mode_request(KilnEntity player) {
    KilnCameraRequest r{};
    r.size = sizeof r;
    r.parts = KILN_CAMERA_MODE;
    r.endpoint = KILN_CAMERA_SERVER_MODE;
    r.player_entity = player;
    r.input_policy = KILN_CAMERA_STOCK_MODE;
    r.duration_ms = 120000;
    r.camera_id = 34;
    return r;
}

// One owner's callbacks, as the test sees them.
struct Probe {
    Core* core = nullptr;
    KilnOwner owner = 0;
    std::atomic<bool> block{false};     // the next callback waits for `go`
    std::atomic<bool> go{false};
    std::atomic<bool> self_retire{false};  // the callback retires its own owner
    std::atomic<bool> retired{false};   // owner_retiring returned
    std::atomic<int> entered{0}, finished{0}, late{0};
    std::atomic<int64_t> self_ms{-1};
    void enter() {
        if (retired.load()) late.fetch_add(1);  // a callback that starts after owner_retiring returned
        entered.fetch_add(1);
        if (self_retire.exchange(false)) {
            const auto t0 = Clock::now();
            core->owner_retiring(owner);
            self_ms.store(std::chrono::duration_cast<milliseconds>(Clock::now() - t0).count());
        }
        if (block.exchange(false)) CHECK(wait_for([&] { return go.load(); }, milliseconds(10000)));
        finished.fetch_add(1);
    }
};
void KILN_CALL probe_done(const KilnCameraResult*, void* user) { static_cast<Probe*>(user)->enter(); }
void KILN_CALL probe_changed(const KilnCameraChange*, void* user) { static_cast<Probe*>(user)->enter(); }

struct Rig {
    TestEnv env;
    Core core{&env};
    TakeAll mode;
    Rig() {
        core.set_backend(KILN_CAMERA_SERVER_MODE, &mode);
        core.set_writes(true);
        core.world_started(kServer, 1);
    }
    // An apply of `owner` answered in the next Pre (to `probe`).
    KilnCameraLease committed(KilnOwner owner, KilnEntity player, Probe* probe) {
        KilnCameraLease id = 0;
        const KilnCameraRequest q = mode_request(player);
        CHECK(core.apply(owner, &q, &id, &probe_done, probe, kOnServer) == KILN_OK && id);
        CHECK(core.committed(id, 1, 1));
        return id;
    }
};

// ---- 1. the barrier ---------------------------------------------------------------------------------------------------

// A `done` of owner 11 runs on the tick thread; owner 11 retires on another thread. The retirement returns only after
// that callback returned, and the owner's second answer, queued behind it, never starts.
void test_retirement_waits_for_a_running_done() {
    Rig r;
    Probe p;
    p.core = &r.core;
    p.owner = 11;
    r.committed(11, A, &p);
    r.committed(11, B, &p);
    p.block = true;
    std::thread tick([&] { r.core.pre(kServer); });
    CHECK(wait_for([&] { return p.entered.load() == 1; }));
    CHECK(r.core.calls_in(11) == 1);
    std::atomic<bool> returned{false};
    std::thread revoker([&] {
        r.core.owner_retiring(11);
        p.retired = true;
        returned = true;
    });
    std::this_thread::sleep_for(milliseconds(150));
    CHECK(!returned.load());  // it waits for the running callback
    p.go = true;
    revoker.join();
    tick.join();
    CHECK(returned.load() && p.finished.load() == 1);
    CHECK(p.entered.load() == 1);  // the gate closed: the second answer was never called
    CHECK(p.late.load() == 0);
    CHECK(r.core.calls_in(11) == 0);
    CHECK(r.env.count("still run") == 0);
}

// The same for a change subscriber of owner 33.
void test_retirement_waits_for_a_running_change() {
    Rig r;
    Probe p, owner;
    p.core = owner.core = &r.core;
    p.owner = 33;
    owner.owner = 11;
    KilnHandle h = 0;
    CHECK(r.core.subscribe(33, &probe_changed, &p, &h) == KILN_OK);
    r.committed(11, A, &owner);
    r.committed(11, B, &owner);
    p.block = true;
    std::thread tick([&] { r.core.pre(kServer); });
    CHECK(wait_for([&] { return p.entered.load() == 1; }));
    std::atomic<bool> returned{false};
    std::thread revoker([&] {
        r.core.owner_retiring(33);
        p.retired = true;
        returned = true;
    });
    std::this_thread::sleep_for(milliseconds(150));
    CHECK(!returned.load());
    p.go = true;
    revoker.join();
    tick.join();
    CHECK(p.entered.load() == 1 && p.late.load() == 0);  // the second APPLIED never reached it
    CHECK(owner.entered.load() == 2);                    // owner 11 got both answers
}

// A retirement from inside the owner's own callback does not wait for itself, and nothing of it is called afterwards.
void test_self_retirement_does_not_wait() {
    Rig r;
    Probe p;
    p.core = &r.core;
    p.owner = 11;
    r.committed(11, A, &p);
    r.committed(11, B, &p);
    p.self_retire = true;
    r.core.pre(kServer);
    CHECK(p.self_ms.load() >= 0 && p.self_ms.load() < 1000);
    CHECK(p.entered.load() == 1);
    CHECK(r.core.calls_in(11) == 0);
    CHECK(r.env.count("still run") == 0);
}

// A callback that does not return in time: the retirement returns after its bounded wait with a loud error, the owner
// stays retired (no callback starts), and the count drains when the callback finally returns.
void test_retirement_wait_is_bounded() {
    Rig r;
    r.core.set_retire_wait_ms(60);
    Probe p;
    p.core = &r.core;
    p.owner = 11;
    r.committed(11, A, &p);
    r.committed(11, B, &p);
    p.block = true;
    std::thread tick([&] { r.core.pre(kServer); });
    CHECK(wait_for([&] { return p.entered.load() == 1; }));
    const auto t0 = Clock::now();
    r.core.owner_retiring(11);
    const auto waited = std::chrono::duration_cast<milliseconds>(Clock::now() - t0).count();
    p.retired = true;
    CHECK(waited >= 50 && waited < 3000);
    CHECK(r.env.count("still run") == 1);
    CHECK(r.core.calls_in(11) == 1);  // still running: kiln.camera frees nothing it holds
    p.go = true;
    tick.join();
    CHECK(p.entered.load() == 1 && p.late.load() == 0);
    CHECK(r.core.calls_in(11) == 0);
}

// ---- 2. world stops and retirements against the tick thread -----------------------------------------------------------

// kiln.world's tracked writes for the Mode backend, used by the tick thread only.
struct TickWorld : ModeWorld {
    struct Req {
        uint64_t id;
        KilnWorldWriteRequest q;
        bool ran = false, cancelled = false, dropped = false;
        uint32_t status = 0;
    };
    std::vector<Req> reqs;
    uint64_t next = 1, ns = 1000000000;
    KilnResult submit(const KilnWorldWriteRequest& q, uint64_t* out) override {
        reqs.push_back({next, q});
        *out = next++;
        return KILN_OK;
    }
    // As write_queue: one that has not run never runs; one that ran loses its result. NOT_FOUND: already delivered.
    KilnResult cancel(uint64_t ticket) override {
        for (Req& r : reqs)
            if (r.id == ticket) {
                (r.ran ? r.dropped : r.cancelled) = true;
                return KILN_OK;
            }
        return KILN_ERR_NOT_FOUND;
    }
    bool player(KilnEntity e, Incarnation* out) override {
        if (e != A && e != B) return false;
        *out = {e, 1};
        return true;
    }
    uint64_t now_ms() override { return ns / 1000000; }
    uint64_t now_ns() override { return ns; }
    uint64_t step() override { return ns / 1000000; }
    void log(int32_t, const std::string&) override {}
    void sim() {
        ns += 16000000;
        for (Req& r : reqs) {
            if (r.ran || r.cancelled) continue;
            r.ran = true;
            uint8_t ncc[3] = {0, 7, 0};
            r.status = r.q.change(r.q.entity, ncc, 3, r.q.user) ? KILN_WORLD_WRITE_DONE : KILN_WORLD_WRITE_REFUSED;
        }
    }
    void deliver() {
        std::vector<Req> due;
        for (auto it = reqs.begin(); it != reqs.end();) {
            if (it->ran || it->cancelled) {
                if (it->ran && !it->dropped) due.push_back(*it);
                it = reqs.erase(it);
            } else {
                ++it;
            }
        }
        for (Req& r : due) {
            KilnWorldWriteResult res{};
            res.size = sizeof res;
            res.status = r.status;
            res.ticket = r.id;
            r.q.callback(&res, r.q.user);
        }
    }
};

constexpr int kOwners = 512;
constexpr KilnOwner kBase = 1000;
struct OwnerState {
    std::atomic<int> running{0}, late{0};
    std::atomic<bool> retired{false};
};
OwnerState g_owner[kOwners];

void stress_callback(KilnOwner owner) {
    if (owner < kBase || owner >= kBase + kOwners) return;
    OwnerState& o = g_owner[owner - kBase];
    if (o.retired.load()) o.late.fetch_add(1);
    o.running.fetch_add(1);
    const auto until = Clock::now() + std::chrono::microseconds(30);
    while (Clock::now() < until) {
    }
    o.running.fetch_sub(1);
}
void KILN_CALL stress_done(const KilnCameraResult*, void* user) { stress_callback(static_cast<KilnOwner>(reinterpret_cast<uintptr_t>(user))); }
void KILN_CALL stress_changed(const KilnCameraChange*, void* user) { stress_callback(static_cast<KilnOwner>(reinterpret_cast<uintptr_t>(user))); }

// The tick thread steps the Mode backend and the core and applies, updates and ends leases of a moving owner; another
// thread stops the world and retires owners at random moments. When owner_retiring returns, no callback into that
// owner runs and none starts afterwards; nothing crashes or races (the two sanitizer builds).
void test_stops_and_retirements_against_the_tick_thread(int rounds) {
    TestEnv env;
    Core core(&env);
    TickWorld world;
    ModeBackend mode(&core, &world, 0xd2c927b5u, "kiln.camera");
    core.set_backend(KILN_CAMERA_SERVER_MODE, &mode);
    core.set_writes(true);
    mode.set_ready(true);
    std::atomic<uint32_t> epoch{1};
    core.world_started(kServer, 1);
    std::atomic<KilnOwner> current{kBase};
    std::atomic<bool> done{false};
    std::atomic<int> stops{0}, retirements{0}, applies{0};

    std::thread other([&] {
        uint32_t seed = 12345;
        while (!done.load()) {
            seed = seed * 1103515245u + 12345u;
            std::this_thread::sleep_for(std::chrono::microseconds(50 + (seed >> 16) % 400));
            if ((seed >> 8) % 3 == 0) {
                core.world_stopping(kServer, 0);
                stops.fetch_add(1);
            } else {
                const KilnOwner o = current.load();
                core.owner_retiring(o);
                OwnerState& s = g_owner[o - kBase];
                CHECK(s.running.load() == 0);  // nothing of it runs when the retirement returns
                s.retired.store(true);
                retirements.fetch_add(1);
            }
        }
    });

    std::vector<KilnCameraLease> mine;
    for (int i = 0; i < rounds; ++i) {
        if (core.epoch(kServer) == 0) core.world_started(kServer, epoch.fetch_add(1) + 1);
        KilnOwner o = current.load();
        if (g_owner[o - kBase].retired.load() && o + 1 < kBase + kOwners) {
            current.store(++o);
            mine.clear();
            KilnHandle h = 0;
            core.subscribe(o, &stress_changed, reinterpret_cast<void*>(static_cast<uintptr_t>(o)), &h);
        }
        void* user = reinterpret_cast<void*>(static_cast<uintptr_t>(o));
        if (i % 3 == 0) {
            KilnCameraLease id = 0;
            const KilnCameraRequest q = mode_request(i % 2 ? A : B);
            if (core.apply(o, &q, &id, &stress_done, user, kOnServer) == KILN_OK) {
                mine.push_back(id);
                applies.fetch_add(1);
            }
        }
        if (i % 5 == 0 && !mine.empty()) {
            const KilnCameraRequest q = mode_request(i % 2 ? A : B);
            core.update(o, mine.back(), KILN_CAMERA_RESTART_TIMELINE, &q, &stress_done, user, kOnServer);
        }
        if (i % 7 == 0 && !mine.empty()) {
            core.end(o, mine.front(), &stress_done, user, kOnServer);
            mine.erase(mine.begin());
        }
        world.deliver();
        mode.step();
        core.pre(kServer);
        world.sim();
    }
    done = true;
    other.join();
    core.world_stopping(kServer, 0);
    mode.step();
    world.deliver();
    for (const OwnerState& s : g_owner) CHECK(s.late.load() == 0 && s.running.load() == 0);
    CHECK(env.count("still run") == 0);
    std::printf("camera threads: stress %d rounds, %d applies, %d world stops, %d retirements\n", rounds, applies.load(), stops.load(),
                retirements.load());
}

}  // namespace

int main(int argc, char** argv) {
    const int rounds = argc > 1 ? std::atoi(argv[1]) : 4000;
    test_retirement_waits_for_a_running_done();
    test_retirement_waits_for_a_running_change();
    test_self_retirement_does_not_wait();
    test_retirement_wait_is_bounded();
    test_stops_and_retirements_against_the_tick_thread(rounds);
    std::printf("camera threads: PASS (%d checks)\n", g_checks.load());
}
