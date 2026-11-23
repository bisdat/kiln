// SPDX-License-Identifier: MIT
// S8's copied-table lifetime boundary, no game/Wine: ready gating, caller identity, self-retirement, bounded drain,
// and rebind against readers. A Call protects bridge delegates, not author callbacks (Core owns that barrier).
#include "../bridge.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

using namespace kcam;
using namespace std::chrono_literals;
static std::atomic<int> checks{0};
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x); std::_Exit(1); } } while (0)

template<class F> void until(F condition) {
    const auto end = std::chrono::steady_clock::now() + 5s;
    while (!condition()) { if (std::chrono::steady_clock::now() >= end) CHECK(false); std::this_thread::yield(); }
}

static std::atomic<int> caps_calls{0};
static KilnResult KILN_CALL submit(KilnOwner, uint32_t, KilnCameraLease, uint64_t, const KilnCameraRequest*, KilnCameraDone, void*) { return KILN_OK; }
static KilnResult KILN_CALL caps(KilnEntity, uint64_t* mask) { ++caps_calls; *mask = KILN_CAMERA_CAP_LOCAL_FOV; return KILN_OK; }
static void KILN_CALL retire(KilnOwner) {}
static KilnCameraTransport table() { return {{sizeof(KilnCameraTransport), 1}, &submit, &caps, &retire}; }

static void shape_and_ready() {
    TransportBinding binding;
    CHECK(!binding.ready());
    CHECK(!binding.acquire());
    auto t = table();
    CHECK(binding.bind(0, &t) == KILN_ERR_INVALID_ARGUMENT);
    CHECK(binding.bind(7, nullptr) == KILN_ERR_INVALID_ARGUMENT);
    t.header.size--;
    CHECK(binding.bind(7, &t) == KILN_ERR_INVALID_ARGUMENT);
    t = table(); t.header.version = 2;
    CHECK(binding.bind(7, &t) == KILN_ERR_INVALID_ARGUMENT);
    t = table(); t.submit = nullptr;
    CHECK(binding.bind(7, &t) == KILN_ERR_INVALID_ARGUMENT);
    t = table(); t.caps = nullptr;
    CHECK(binding.bind(7, &t) == KILN_ERR_INVALID_ARGUMENT);
    t = table(); t.retire = nullptr;
    CHECK(binding.bind(7, &t) == KILN_ERR_INVALID_ARGUMENT);
    t = table(); t.header.size += 64; // forward prefix accepted, but no un-copied tail may be advertised
    CHECK(binding.bind(7, &t) == KILN_OK);
    CHECK(!binding.ready());
    CHECK(!binding.acquire()); // binding is not startup readiness
    CHECK(binding.bind(7, &t) == KILN_ERR_INVALID_ARGUMENT);
    CHECK(binding.set_ready(8, true) == KILN_ERR_REVOKED);
    CHECK(binding.unbind(8) == KILN_ERR_REVOKED);
    CHECK(binding.set_ready(7, true) == KILN_OK);
    t = {}; // only the copy remains
    uint64_t generation = 0;
    {
        auto call = binding.acquire();
        CHECK(call);
        CHECK(call.owner() == 7);
        CHECK(call.generation() != 0);
        generation = call.generation();
        CHECK(call.table()->header.size == sizeof(KilnCameraTransport));
        CHECK(call.table()->header.version == 1);
        uint64_t mask = 0;
        CHECK(call.table()->caps(1, &mask) == KILN_OK);
        CHECK(mask == KILN_CAMERA_CAP_LOCAL_FOV);
        CHECK(binding.in_flight() == 1);
        auto moved = std::move(call);
        CHECK(!call);
        CHECK(moved);
        CHECK(binding.in_flight() == 1);
    }
    CHECK(binding.in_flight() == 0);
    CHECK(binding.set_ready(7, false) == KILN_OK);
    CHECK(!binding.acquire());
    CHECK(binding.set_ready(7, true) == KILN_OK);
    CHECK(binding.unbind(7) == KILN_OK);
    CHECK(!binding.ready());
    CHECK(binding.unbind(7) == KILN_ERR_NOT_FOUND);
    t = table();
    CHECK(binding.bind(9, &t) == KILN_OK);
    CHECK(binding.set_ready(9, true) == KILN_OK);
    {
        auto call = binding.acquire();
        CHECK(call.generation() > generation);
    }
    CHECK(binding.unbind(9) == KILN_OK);
}

static void self_unbind_closes_but_does_not_free() {
    TransportBinding binding; auto t = table();
    CHECK(binding.bind(7, &t) == KILN_OK);
    CHECK(binding.set_ready(7, true) == KILN_OK);
    {
        auto call = binding.acquire();
        CHECK(call);
        CHECK(binding.unbind(7) == KILN_ERR_WRONG_THREAD);
        CHECK(!binding.ready());
        CHECK(!binding.acquire());
        CHECK(binding.set_ready(7, true) == KILN_ERR_REVOKED);
        CHECK(binding.bind(8, &t) == KILN_ERR_INVALID_ARGUMENT);
        uint64_t mask = 0;
        CHECK(call.table()->caps(1, &mask) == KILN_OK); // reserved call/delegates stay alive through return
    }
    CHECK(binding.unbind(7) == KILN_OK);
}

static void off_thread_drain(bool timeout) {
    TransportBinding binding; auto t = table();
    CHECK(binding.bind(7, &t) == KILN_OK);
    CHECK(binding.set_ready(7, true) == KILN_OK);
    binding.set_drain_wait_ms(timeout ? 1 : 2000);
    std::atomic<bool> entered{false}, release{false}, unbound{false};
    std::thread reader([&] {
        auto call = binding.acquire();
        CHECK(call);
        entered = true;
        until([&] { return release.load(); });
        uint64_t mask = 0;
        CHECK(call.table()->caps(1, &mask) == KILN_OK);
    });
    until([&] { return entered.load(); });
    if (timeout) {
        CHECK(binding.unbind(7) == KILN_ERR_INTERNAL);
        CHECK(binding.in_flight() == 1);
        CHECK(!binding.acquire());
        CHECK(binding.bind(8, &t) == KILN_ERR_INVALID_ARGUMENT); // failed drain must retain the old roots
        release = true;
        reader.join();
        CHECK(binding.unbind(7) == KILN_OK);
    } else {
        std::thread closer([&] { CHECK(binding.unbind(7) == KILN_OK); unbound = true; });
        until([&] { return !binding.ready(); });
        CHECK(!unbound);
        CHECK(!binding.acquire());
        release = true;
        reader.join(); closer.join();
        CHECK(unbound);
    }
    CHECK(binding.in_flight() == 0);
    CHECK(!binding.ready());
}

static void rebind_race() {
    TransportBinding binding; auto t = table();
    std::atomic<bool> stop{false};
    std::atomic<int> taken{0};
    std::thread reader([&] {
        while (!stop.load()) {
            auto call = binding.acquire();
            if (!call) { std::this_thread::yield(); continue; }
            uint64_t mask = 0;
            CHECK(call.table()->caps(1, &mask) == KILN_OK);
            CHECK(call.owner() != 0);
            CHECK(call.generation() != 0);
            ++taken;
        }
    });
    for (uint64_t i = 1; i <= 100; ++i) {
        CHECK(binding.bind(i, &t) == KILN_OK);
        const int before = taken.load();
        CHECK(binding.set_ready(i, true) == KILN_OK);
        until([&] { return taken.load() > before; });
        CHECK(binding.unbind(i) == KILN_OK);
        CHECK(binding.in_flight() == 0);
        CHECK(!binding.ready());
    }
    stop = true; reader.join();
    CHECK(taken >= 100);
}

int main() {
    shape_and_ready();
    self_unbind_closes_but_does_not_free();
    off_thread_drain(false);
    off_thread_drain(true);
    rebind_race();
    std::printf("camera bridge binding: PASS (%d checks, copied table, ready gate, drain, timeout, self-unbind, rebind race)\n", checks.load());
}
