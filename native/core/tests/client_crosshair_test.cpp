// SPDX-License-Identifier: MIT
// Host tests of kiln.client.crosshair (src/services/client_crosshair.cpp): the decoder of the game's 112-byte trace
// result as client 26430625 writes it, get()'s refusals, the lazy install at the first get (once, retried only for
// Again, final otherwise), the copy from client_cursor's frame, freshness (sequence, session, step, age, STALE), the
// session rules, a faulted copy, a publish in progress (BUSY), and the producer's thread rules (it allocates nothing and
// readers on other threads never see a torn sample). Run by test-client-crosshair.sh (asan, tsan).
#include "services/client_crosshair.h"

#include <sanitizer/allocator_interface.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <thread>
#include <vector>

namespace ch = kiln::crosshair;
namespace {
int passed = 0, failed = 0;
void check(bool ok, const char* text, const std::string& detail = "") {
    std::printf("%s %s%s%s\n", ok ? "PASS" : "FAIL", text, detail.empty() ? "" : ": ", detail.c_str());
    ok ? ++passed : ++failed;
}

std::atomic<int64_t> g_now{1'000'000'000};
int64_t fake_clock() { return g_now.load(std::memory_order_relaxed); }

thread_local bool t_counting = false;
thread_local unsigned t_allocs = 0;
__attribute__((no_sanitize("address", "thread", "undefined"))) void on_malloc(const volatile void*, size_t) {
    if (t_counting) ++t_allocs;
}
__attribute__((no_sanitize("address", "thread", "undefined"))) void on_free(const volatile void*) {}

constexpr uint32_t kHit = 0xa90;  // client/crosshair.hit on client 26430625

// A result as the game's trace writes it (client 26430625, docs/re/client.md).
struct Result {
    uint8_t b[ch::kResultBytes]{};
    Result& status(uint8_t s) { b[0] = s; return *this; }
    Result& f32(size_t at, float v) { std::memcpy(b + at, &v, 4); return *this; }
    Result& u32(size_t at, uint32_t v) { std::memcpy(b + at, &v, 4); return *this; }
    Result& position(float x, float y, float z) { return f32(0x08, x).f32(0x0c, y).f32(0x10, z); }
    Result& normal(float x, float y, float z) { return f32(0x14, x).f32(0x18, y).f32(0x1c, z); }
    Result& material(uint8_t m) { b[0x60] = m; b[0x61] = 1; return *this; }
    Result& entity(uint32_t e) { return u32(0x64, e); }
    Result& distance_sq(float d2) { return f32(0x68, d2); }
};
Result terrain_hit() {
    return Result{}.status(0).position(1024.5f, 37.25f, -2048.75f).normal(0, 1, 0).material(37).distance_sq(36.0f);
}

// client_cursor's frame: a buffer the producer copies from at frame + hit.
struct Frame {
    std::vector<uint8_t> bytes = std::vector<uint8_t>(kHit + ch::kResultBytes + 64);
    uintptr_t rbp() const { return reinterpret_cast<uintptr_t>(bytes.data()); }
    void put(const Result& r) { std::memcpy(bytes.data() + kHit, r.b, sizeof r.b); }
};
std::atomic<uintptr_t> g_last_src{0};
std::atomic<bool> g_fault{false};
bool mem_read(void*, void* dst, uintptr_t src, size_t n) {
    g_last_src.store(src, std::memory_order_relaxed);
    if (g_fault.load(std::memory_order_relaxed)) return false;
    std::memcpy(dst, reinterpret_cast<const void*>(src), n);
    return true;
}
const ch::Memory kMem{&mem_read, nullptr};

constexpr ch::Env kClient{true, true};

KilnCrosshairHit fresh_out() {
    KilnCrosshairHit h;
    std::memset(&h, 0xcd, sizeof h);
    h.size = sizeof h;
    return h;
}

std::atomic<int> g_installs{0};
ch::Install install_bound(uint32_t* offset, const char**) {
    g_installs.fetch_add(1);
    *offset = kHit;
    return ch::Install::Bound;
}
ch::Install install_again(uint32_t*, const char** why) {
    g_installs.fetch_add(1);
    *why = "threads could not be checked";
    return ch::Install::Again;
}
std::atomic<bool> g_hold_install{false}, g_in_install{false};
ch::Install install_again_held(uint32_t* offset, const char** why) {
    g_in_install = true;
    while (g_hold_install.load()) std::this_thread::yield();
    return install_again(offset, why);
}
ch::Install install_failed(uint32_t*, const char** why) {
    g_installs.fetch_add(1);
    *why = "the detour could not be written";
    return ch::Install::Failed;
}

void reset() {
    ch::reset_for_tests();
    ch::set_clock_for_tests(&fake_clock);
    g_now = 1'000'000'000;
    g_fault = false;
    g_installs = 0;
    g_in_install = false;
}

// A ready build with a live session 3 at step 100, installed by a first get.
void ready() {
    reset();
    ch::set_installer(&install_bound);
    ch::session_live(3, 100);
    KilnCrosshairHit h = fresh_out();
    uint32_t reason = 99;
    ch::get(kClient, &h, &reason);
}

void decoder() {
    KilnCrosshairHit h{};
    ch::decode(terrain_hit().b, &h);
    check(h.flags == (KILN_CROSSHAIR_HIT | KILN_CROSSHAIR_POSITION | KILN_CROSSHAIR_NORMAL | KILN_CROSSHAIR_DISTANCE |
                      KILN_CROSSHAIR_MATERIAL),
          "decode: a terrain hit has position, normal, distance and material, no entity", std::to_string(h.flags));
    check(h.status == 0 && h.entity == 0 && h.material == 37, "decode: the status, no entity, material 37");
    check(h.position[0] == 1024.5 && h.position[1] == 37.25 && h.position[2] == -2048.75,
          "decode: the position is the game's float, widened");
    check(h.normal[0] == 0 && h.normal[1] == 1 && h.normal[2] == 0, "decode: the normal");
    check(h.distance == 6.0f, "decode: the distance is the square root of the game's squared distance",
          std::to_string(h.distance));

    h = {};
    ch::decode(Result{}.status(0).position(1, 2, 3).normal(0.6f, 0.8f, 0).entity(4711).distance_sq(2.25f).b, &h);
    check(h.flags == (KILN_CROSSHAIR_HIT | KILN_CROSSHAIR_POSITION | KILN_CROSSHAIR_NORMAL | KILN_CROSSHAIR_DISTANCE |
                      KILN_CROSSHAIR_ENTITY) &&
              h.entity == 4711 && h.material == 0 && h.distance == 1.5f,
          "decode: an entity hit without a material flag has the entity and no material");

    h = {};
    // the game's miss: every field zeroed, status 31
    ch::decode(Result{}.status(KILN_CROSSHAIR_STATUS_NOTHING).b, &h);
    check(h.flags == 0 && h.status == 31 && h.entity == 0 && h.material == 0 && h.distance == 0 && h.position[0] == 0,
          "decode: a miss (31) sets nothing but the status");
    h = {};
    // a miss never reports the bytes it happens to carry
    ch::decode(terrain_hit().status(KILN_CROSSHAIR_STATUS_NO_CAMERA).entity(9).b, &h);
    check(h.flags == 0 && h.status == 21 && h.entity == 0 && h.material == 0, "decode: no camera (21) is a miss too");

    const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
    h = {};
    ch::decode(terrain_hit().position(nan, 0, 0).b, &h);
    check((h.flags & KILN_CROSSHAIR_POSITION) == 0 && (h.flags & KILN_CROSSHAIR_HIT), "decode: a NaN position is unset");
    h = {};
    ch::decode(terrain_hit().position(1e9f, 0, 0).b, &h);
    check((h.flags & KILN_CROSSHAIR_POSITION) == 0, "decode: a position outside any world is unset");
    h = {};
    ch::decode(terrain_hit().normal(0, 2, 0).b, &h);
    check((h.flags & KILN_CROSSHAIR_NORMAL) == 0 && h.normal[1] == 0, "decode: a normal that is not unit length is unset");
    h = {};
    ch::decode(terrain_hit().normal(0, inf, 0).b, &h);
    check((h.flags & KILN_CROSSHAIR_NORMAL) == 0, "decode: an infinite normal is unset");
    for (float d2 : {-1.0f, nan, inf, 2.0e6f}) {
        h = {};
        ch::decode(terrain_hit().distance_sq(d2).b, &h);
        check((h.flags & KILN_CROSSHAIR_DISTANCE) == 0 && h.distance == 0, "decode: a squared distance out of range is unset",
              std::to_string(d2));
    }
    h = {};
    ch::decode(terrain_hit().distance_sq(0).b, &h);
    check((h.flags & KILN_CROSSHAIR_DISTANCE) && h.distance == 0, "decode: a hit at the trace's start is distance 0");
}

void refusals() {
    reset();
    ch::set_installer(&install_bound);
    ch::session_live(3, 100);
    KilnCrosshairHit h = fresh_out();
    uint32_t reason = 0;
    check(ch::get({false, true}, &h, &reason) == KILN_ERR_UNAVAILABLE, "get: a dedicated server is unavailable");
    check(ch::get({true, false}, &h, &reason) == KILN_ERR_UNAVAILABLE, "get: safe idle is unavailable");
    check(g_installs == 0, "get: a refused get installs nothing");
    check(ch::get(kClient, nullptr, &reason) == KILN_ERR_INVALID_ARGUMENT, "get: no out is invalid");
    h.size = sizeof h - 1;
    check(ch::get(kClient, &h, &reason) == KILN_ERR_INVALID_ARGUMENT, "get: a short out is invalid");
    check(g_installs == 0, "get: an invalid get installs nothing");

    reset();
    const char* why = nullptr;
    bool installed = true;
    check(!ch::usable(&why, &installed) && why && std::string(why).find("client.crosshair_trace") != std::string::npos,
          "usable: unbound by default, the reason names the sigdb feature", why ? why : "");
    h = fresh_out();
    check(ch::get(kClient, &h, &reason) == KILN_ERR_UNAVAILABLE, "get: no installer and no binding is unavailable");
    ch::unbound_reason("held for a test");
    check(!ch::usable(&why, &installed) && std::string(why) == "held for a test", "usable: unbound_reason is the answer");
}

void lazy_install() {
    reset();
    const uint64_t g0 = ch::generation();
    ch::set_installer(&install_bound);
    const char* why = nullptr;
    bool installed = true;
    check(ch::usable(&why, &installed) && !installed && ch::generation() != g0,
          "usable: an installer waits for the first get (not installed yet)");
    KilnCrosshairHit h = fresh_out();
    uint32_t reason = 99;
    // the menu: no live session yet. The get still installs (a mod may read early); nothing to give.
    check(ch::get(kClient, &h, &reason) == KILN_OK && reason == KILN_CROSSHAIR_REASON_NO_SAMPLE && h.sequence == 0 &&
              h.flags == 0 && h.size == sizeof h,
          "get: the first get answers NO_SAMPLE with a zeroed sample");
    check(g_installs == 1 && ch::usable(&why, &installed) && installed, "get: the first get installed the hook, once");
    h = fresh_out();
    ch::get(kClient, &h, &reason);
    check(g_installs == 1 && ch::stats().installs == 1, "get: a later get installs nothing more");

    reset();
    ch::set_installer(&install_again);
    // a refusal for unchecked threads is not final: the row stays on, so get answers NO_SAMPLE (as the docs promise
    // for the first read), never UNAVAILABLE while usable() says yes
    for (uint32_t i = 0; i + 1 < ch::kInstallAttempts; ++i) {
        h = fresh_out();
        reason = 99;
        const KilnResult rc = ch::get(kClient, &h, &reason);
        check(rc == KILN_OK && reason == KILN_CROSSHAIR_REASON_NO_SAMPLE && h.sequence == 0 && h.flags == 0 &&
                  h.size == sizeof h,
              "get: an install refused for unchecked threads answers NO_SAMPLE with a zeroed sample", std::to_string(i));
        check(ch::usable(&why, &installed) && !installed && !ch::install_failed(),
              "usable: still on after a retryable refusal", std::to_string(i));
        // a mod polling every frame does not burn the attempts: none before kInstallRetryNs on the service's clock
        const int before = g_installs.load();
        g_now += ch::kInstallRetryNs - 1;
        h = fresh_out();
        reason = 99;
        check(ch::get(kClient, &h, &reason) == KILN_OK && reason == KILN_CROSSHAIR_REASON_NO_SAMPLE &&
                  g_installs == before,
              "get: no new attempt sooner than kInstallRetryNs after a refusal", std::to_string(g_installs.load()));
        g_now += 1;
    }
    h = fresh_out();
    check(ch::get(kClient, &h, &reason) == KILN_ERR_UNAVAILABLE, "get: the last refused attempt is final, unavailable");
    check(g_installs == static_cast<int>(ch::kInstallAttempts) && ch::install_failed(),
          "get: Again is tried kInstallAttempts times, then final");
    check(!ch::usable(&why, &installed) && std::string(why) == "threads could not be checked",
          "usable: the last refusal's reason stays");
    h = fresh_out();
    check(ch::get(kClient, &h, &reason) == KILN_ERR_UNAVAILABLE && g_installs == static_cast<int>(ch::kInstallAttempts),
          "get: no install after the final failure");

    // a get that waited on the install of another thread, which was refused for unchecked threads: NO_SAMPLE too
    reset();
    g_hold_install = true;
    ch::set_installer(&install_again_held);
    KilnResult first_rc = KILN_ERR_INTERNAL, waiter_rc = KILN_ERR_INTERNAL;
    uint32_t first_reason = 99, waiter_reason = 99;
    std::thread first([&] {
        KilnCrosshairHit x = fresh_out();
        first_rc = ch::get(kClient, &x, &first_reason);
    });
    while (!g_in_install.load()) std::this_thread::yield();
    std::thread waiter([&] {
        KilnCrosshairHit x = fresh_out();
        waiter_rc = ch::get(kClient, &x, &waiter_reason);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    g_hold_install = false;
    first.join();
    waiter.join();
    check(first_rc == KILN_OK && first_reason == KILN_CROSSHAIR_REASON_NO_SAMPLE && waiter_rc == KILN_OK &&
              waiter_reason == KILN_CROSSHAIR_REASON_NO_SAMPLE && g_installs == 1,
          "get: the refused install's thread and a thread that waited on it both answer NO_SAMPLE, one attempt",
          std::to_string(first_rc) + " " + std::to_string(waiter_rc) + " " + std::to_string(g_installs.load()));

    reset();
    ch::set_installer(&install_failed);
    const uint64_t g1 = ch::generation();
    h = fresh_out();
    check(ch::get(kClient, &h, &reason) == KILN_ERR_UNAVAILABLE && ch::install_failed() && ch::generation() != g1,
          "get: a failed install is final at once and moves the generation");
    h = fresh_out();
    ch::get(kClient, &h, &reason);
    check(g_installs == 1, "get: a failed install is never tried again");
    ch::set_installer(&install_bound);
    check(!ch::install_failed(), "set_installer clears install_failed");
}

void samples() {
    ready();
    Frame f;
    f.put(terrain_hit());
    KilnCrosshairHit h = fresh_out();
    uint32_t reason = 99;
    check(ch::get(kClient, &h, &reason) == KILN_OK && reason == KILN_CROSSHAIR_REASON_NO_SAMPLE,
          "get: nothing observed yet is NO_SAMPLE");
    ch::observe(f.rbp(), kMem);
    check(g_last_src == f.rbp() + kHit, "observe: copies from client_cursor's frame at client/crosshair.hit");
    g_now += 5'000'000;
    h = fresh_out();
    check(ch::get(kClient, &h, &reason) == KILN_OK && reason == KILN_CROSSHAIR_REASON_NONE, "get: a fresh sample");
    check(h.size == sizeof h && h.sequence == 1 && h.client_session == 3 && h.client_step == 100 && h.age_ns == 5'000'000,
          "get: sequence 1, the live session and step, its age",
          std::to_string(h.sequence) + " " + std::to_string(h.client_session) + " " + std::to_string(h.client_step) + " " +
              std::to_string(h.age_ns));
    check((h.flags & KILN_CROSSHAIR_HIT) && h.material == 37 && h.distance == 6.0f && h.position[1] == 37.25,
          "get: the decoded hit");

    ch::session_live(3, 101);
    f.put(Result{}.status(KILN_CROSSHAIR_STATUS_NOTHING));
    ch::observe(f.rbp(), kMem);
    h = fresh_out();
    ch::get(kClient, &h, &reason);
    check(reason == KILN_CROSSHAIR_REASON_NONE && h.sequence == 2 && h.client_step == 101 && h.flags == 0 &&
              h.status == 31 && h.age_ns == 0,
          "get: the next trace (a miss) is sequence 2 at the next step");

    g_now += static_cast<int64_t>(KILN_CROSSHAIR_FRESH_NS);
    h = fresh_out();
    ch::get(kClient, &h, &reason);
    check(reason == KILN_CROSSHAIR_REASON_NONE, "get: exactly KILN_CROSSHAIR_FRESH_NS old is still fresh");
    g_now += 1;
    h = fresh_out();
    ch::get(kClient, &h, &reason);
    check(reason == KILN_CROSSHAIR_REASON_STALE && h.sequence == 2 && h.status == 31 &&
              h.age_ns == KILN_CROSSHAIR_FRESH_NS + 1,
          "get: an older sample is STALE and still given");
    h = fresh_out();
    check(ch::get(kClient, &h, nullptr) == KILN_OK && h.sequence == 2, "get: out_reason may be NULL");

    // a clock that went back (another thread's QPC read) is age 0, never a huge unsigned age
    g_now = 0;
    h = fresh_out();
    ch::get(kClient, &h, &reason);
    check(h.age_ns == 0 && reason == KILN_CROSSHAIR_REASON_NONE, "get: a sample from the future is age 0");
}

void sessions() {
    ready();
    Frame f;
    f.put(terrain_hit());
    ch::observe(f.rbp(), kMem);
    KilnCrosshairHit h = fresh_out();
    uint32_t reason = 0;
    ch::session_live(4, 500);
    ch::get(kClient, &h, &reason);
    check(reason == KILN_CROSSHAIR_REASON_NO_SAMPLE && h.sequence == 0 && h.flags == 0,
          "session: a sample of an older session is never given");
    ch::observe(f.rbp(), kMem);
    h = fresh_out();
    ch::get(kClient, &h, &reason);
    check(reason == KILN_CROSSHAIR_REASON_NONE && h.client_session == 4 && h.sequence == 2,
          "session: the new session's sample (the sequence keeps growing)");
    ch::session_ended(4);
    h = fresh_out();
    ch::get(kClient, &h, &reason);
    check(reason == KILN_CROSSHAIR_REASON_NO_SAMPLE && h.sequence == 0, "session: after STOPPING nothing is given");
    const uint64_t skipped = ch::stats().skipped;
    ch::observe(f.rbp(), kMem);
    h = fresh_out();
    ch::get(kClient, &h, &reason);
    check(reason == KILN_CROSSHAIR_REASON_NO_SAMPLE && ch::stats().skipped == skipped + 1,
          "session: a visit with no live session is skipped");
    ch::session_live(4, 501);  // a late Pre of the stopped session does not bring it back
    ch::observe(f.rbp(), kMem);
    h = fresh_out();
    ch::get(kClient, &h, &reason);
    check(reason == KILN_CROSSHAIR_REASON_NO_SAMPLE, "session: an ended session stays ended");
    ch::session_live(5, 10);
    ch::observe(f.rbp(), kMem);
    h = fresh_out();
    ch::get(kClient, &h, &reason);
    check(reason == KILN_CROSSHAIR_REASON_NONE && h.client_session == 5, "session: the next session works");
}

void faults() {
    ready();
    Frame f;
    f.put(terrain_hit());
    ch::observe(f.rbp(), kMem);
    g_fault = true;
    ch::observe(f.rbp(), kMem);
    KilnCrosshairHit h = fresh_out();
    uint32_t reason = 0;
    check(ch::get(kClient, &h, &reason) == KILN_OK && reason == KILN_CROSSHAIR_REASON_FAULT && h.sequence == 0 &&
              ch::stats().faults == 1,
          "fault: a faulted copy is FAULT, no sample");
    g_fault = false;
    ch::observe(f.rbp(), kMem);
    h = fresh_out();
    ch::get(kClient, &h, &reason);
    check(reason == KILN_CROSSHAIR_REASON_FAULT && ch::stats().faults == 1,
          "fault: no copy is tried again in the faulted session");
    ch::session_live(4, 1);
    ch::observe(f.rbp(), kMem);
    h = fresh_out();
    ch::get(kClient, &h, &reason);
    check(reason == KILN_CROSSHAIR_REASON_NONE && h.client_session == 4, "fault: the next session copies again");

    // unbound: nothing is read
    reset();
    ch::session_live(1, 1);
    g_last_src = 0;
    ch::observe(f.rbp(), kMem);
    check(g_last_src == 0, "observe: unbound reads nothing");
}

void busy() {
    ready();
    Frame f;
    f.put(terrain_hit());
    ch::observe(f.rbp(), kMem);
    ch::hold_publish_for_tests(true);
    KilnCrosshairHit h = fresh_out();
    uint32_t reason = 0;
    check(ch::get(kClient, &h, &reason) == KILN_OK && reason == KILN_CROSSHAIR_REASON_BUSY && h.sequence == 0 &&
              h.flags == 0,
          "busy: a publish in progress is BUSY with a zeroed sample");
    const uint64_t skipped = ch::stats().skipped;
    ch::observe(f.rbp(), kMem);
    check(ch::stats().skipped == skipped + 1, "busy: another visit skips instead of writing over it");
    ch::hold_publish_for_tests(false);
    h = fresh_out();
    ch::get(kClient, &h, &reason);
    check(reason == KILN_CROSSHAIR_REASON_NONE && h.sequence == 1, "busy: the held sample is readable again");
}

void producer_allocates_nothing() {
    ready();
    Frame f;
    f.put(terrain_hit());
    ch::observe(f.rbp(), kMem);  // warm
    t_allocs = 0;
    t_counting = true;
    for (int i = 0; i < 100; ++i) ch::observe(f.rbp(), kMem);
    KilnCrosshairHit h = fresh_out();
    uint32_t reason = 0;
    ch::get(kClient, &h, &reason);
    t_counting = false;
    check(t_allocs == 0, "threads: observe and get allocate nothing", std::to_string(t_allocs));
}

// One producer thread publishes traces whose fields all derive from one counter; three readers check every sample
// they get is one whole trace (no torn mix of two).
void no_torn_samples() {
    ready();
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> reads{0}, torn{0}, busy_reads{0};
    std::thread producer([&] {
        Frame f;
        for (uint32_t i = 1; !stop.load(std::memory_order_relaxed); ++i) {
            const float v = static_cast<float>(i % 100000);
            f.put(Result{}.status(0).position(v, v + 1, v + 2).normal(0, 0, 1).entity(i).distance_sq(4.0f));
            ch::observe(f.rbp(), kMem);
        }
    });
    std::vector<std::thread> readers;
    for (int r = 0; r < 3; ++r)
        readers.emplace_back([&] {
            for (int i = 0; i < 20000; ++i) {
                KilnCrosshairHit h = fresh_out();
                uint32_t reason = 0;
                if (ch::get(kClient, &h, &reason) != KILN_OK) {
                    torn.fetch_add(1);
                    continue;
                }
                if (reason == KILN_CROSSHAIR_REASON_BUSY) {
                    busy_reads.fetch_add(1);
                    continue;
                }
                if (h.sequence == 0) continue;
                reads.fetch_add(1);
                const double v = static_cast<double>(h.entity % 100000);
                if (h.position[0] != v || h.position[1] != v + 1 || h.position[2] != v + 2 || h.distance != 2.0f)
                    torn.fetch_add(1);
            }
        });
    for (auto& t : readers) t.join();
    stop = true;
    producer.join();
    check(torn == 0 && reads > 0, "threads: readers never see a torn sample",
          std::to_string(reads.load()) + " reads, " + std::to_string(busy_reads.load()) + " busy, " +
              std::to_string(torn.load()) + " torn");
}

}  // namespace

int main() {
    __sanitizer_install_malloc_and_free_hooks(&on_malloc, &on_free);
    decoder();
    refusals();
    lazy_install();
    samples();
    sessions();
    faults();
    busy();
    producer_allocates_nothing();
    no_torn_samples();
    std::printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
