// SPDX-License-Identifier: MIT
// kiln.client.crosshair (client_crosshair.h says what and how).
#include "client_crosshair.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>

namespace kiln::crosshair {
namespace {

// The game's result (client_crosshair.h): the offsets the sigdb probes on the trace body pin.
constexpr size_t kStatus = 0x00, kPosition = 0x08, kNormal = 0x14, kMaterial = 0x60, kMaterialSet = 0x61, kEntity = 0x64,
                 kDistanceSq = 0x68;
static_assert(kDistanceSq + 4 <= kResultBytes, "the squared distance is inside the result");
constexpr float kMaxCoordinate = 1.0e7f;  // metres: far outside any world, inside float's integer range

// One published sample, copied through atomic words (the seqlock below). Value fields only.
struct Sample {
    uint64_t sequence;  // 0: nothing
    uint64_t client_step;
    int64_t captured_ns;
    uint32_t flags, status, entity, material, client_session, pad;
    double position[3];
    float normal[3];
    float distance;
};
static_assert(sizeof(Sample) % 8 == 0, "copied as whole words");

int64_t steady_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

const char* const kUnbound =
    "no verified crosshair trace on this game build: sigdb has no client.crosshair_trace (client_cursor's call of the "
    "gameplay scene trace and its result layout) for it";
const char* const kInstallFailed = "hooking the crosshair trace failed (kiln.log says why)";
const char* const kInstallThrew = "hooking the crosshair trace failed at the first read: it threw";

std::atomic<int64_t (*)()> g_clock{&steady_ns};

// The store: one writer at a time (g_writing), any reader; a reader that meets a write answers BUSY, never a torn value.
constexpr size_t kWords = sizeof(Sample) / 8;
std::atomic<uint64_t> g_seq{0};  // odd while a write is in progress
std::atomic<uint64_t> g_words[kWords]{};
std::atomic<bool> g_writing{false};
std::atomic<uint64_t> g_next{0};  // the last sequence handed out

// The session the producer and get() serve (client Pre), and the newest one that ended (STOPPING).
std::atomic<uint32_t> g_live{0};
std::atomic<uint64_t> g_step{0};
std::atomic<uint32_t> g_ended{0};
std::atomic<uint32_t> g_faulted{0};  // the session whose copy faulted (0: none)

std::atomic<uint32_t> g_offset{0};  // client/crosshair.hit once bound; 0: unbound
std::atomic<uint64_t> g_samples{0}, g_skipped{0}, g_faults{0}, g_installs{0};

std::mutex g_install_mu;  // one install at a time
std::atomic<Installer> g_installer{nullptr};
std::atomic<bool> g_install_failed{false};
std::atomic<const char*> g_why{kUnbound};
std::atomic<uint64_t> g_generation{1};

void changed() { g_generation.fetch_add(1, std::memory_order_acq_rel); }

void store(const Sample& s) {
    uint64_t w[kWords];
    std::memcpy(w, &s, sizeof w);
    g_seq.fetch_add(1, std::memory_order_acq_rel);  // odd: a write is in progress
    for (size_t i = 0; i < kWords; ++i) g_words[i].store(w[i], std::memory_order_relaxed);
    g_seq.fetch_add(1, std::memory_order_release);
}

// false: a write was in progress or ran over the read
bool load(Sample* out) {
    const uint64_t before = g_seq.load(std::memory_order_acquire);
    if (before & 1) return false;
    uint64_t w[kWords];
    for (size_t i = 0; i < kWords; ++i) w[i] = g_words[i].load(std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_acquire);
    if (g_seq.load(std::memory_order_relaxed) != before) return false;
    std::memcpy(out, w, sizeof w);
    return true;
}

// The one writer: false when another visit holds it.
bool begin_write() {
    bool expected = false;
    return g_writing.compare_exchange_strong(expected, true, std::memory_order_acquire, std::memory_order_relaxed);
}
void end_write() { g_writing.store(false, std::memory_order_release); }

void clear_samples() {
    if (!begin_write()) return;  // the visit that holds it publishes for a session get() checks anyway
    store(Sample{});
    end_write();
}

bool finite(float v) { return std::isfinite(v); }

// The first get's install. True when the hook is in.
bool install_once() {
    const uint64_t seen = g_installs.load(std::memory_order_acquire);
    std::lock_guard install(g_install_mu);
    if (g_offset.load(std::memory_order_acquire)) return true;
    const Installer fn = g_installer.load(std::memory_order_acquire);
    if (!fn) return false;
    if (g_installs.load(std::memory_order_relaxed) != seen) return false;  // refused while this one waited
    const uint64_t attempt = seen + 1;
    uint32_t offset = 0;
    const char* why = nullptr;
    Install result = Install::Failed;
    try {  // a mod's thread: nothing the install throws leaves get
        result = fn(&offset, &why);
    } catch (...) {
        why = kInstallThrew;
    }
    g_installs.store(attempt, std::memory_order_release);
    if (result == Install::Bound && offset && offset < kMaxFrameOffset) {
        bind(offset);
        return true;
    }
    if (result == Install::Bound) why = "the crosshair trace's result offset is outside client_cursor's frame";
    else if (result == Install::Again && attempt < kInstallAttempts) return false;
    g_install_failed.store(true, std::memory_order_release);
    g_why.store(why ? why : kInstallFailed, std::memory_order_release);
    g_installer.store(nullptr, std::memory_order_release);
    changed();
    return false;
}

}  // namespace

void decode(const uint8_t raw[kResultBytes], KilnCrosshairHit* out) {
    out->flags = 0;
    out->status = raw[kStatus];
    out->entity = 0;
    out->material = 0;
    out->distance = 0;
    for (int i = 0; i < 3; ++i) {
        out->position[i] = 0;
        out->normal[i] = 0;
    }
    if (out->status != KILN_CROSSHAIR_STATUS_HIT) return;
    out->flags = KILN_CROSSHAIR_HIT;
    float p[3], n[3], d2 = 0;
    uint32_t entity = 0;
    std::memcpy(p, raw + kPosition, sizeof p);
    std::memcpy(n, raw + kNormal, sizeof n);
    std::memcpy(&d2, raw + kDistanceSq, sizeof d2);
    std::memcpy(&entity, raw + kEntity, sizeof entity);
    if (finite(p[0]) && finite(p[1]) && finite(p[2]) && std::fabs(p[0]) < kMaxCoordinate &&
        std::fabs(p[1]) < kMaxCoordinate && std::fabs(p[2]) < kMaxCoordinate) {
        out->flags |= KILN_CROSSHAIR_POSITION;
        for (int i = 0; i < 3; ++i) out->position[i] = p[i];
    }
    if (finite(n[0]) && finite(n[1]) && finite(n[2])) {
        const double len = std::sqrt(double{n[0]} * n[0] + double{n[1]} * n[1] + double{n[2]} * n[2]);
        if (std::fabs(len - 1.0) <= 1e-2) {
            out->flags |= KILN_CROSSHAIR_NORMAL;
            for (int i = 0; i < 3; ++i) out->normal[i] = n[i];
        }
    }
    if (finite(d2) && d2 >= 0 && d2 <= kMaxDistance * kMaxDistance) {
        out->flags |= KILN_CROSSHAIR_DISTANCE;
        out->distance = std::sqrt(d2);
    }
    if (entity) {
        out->flags |= KILN_CROSSHAIR_ENTITY;
        out->entity = entity;
    }
    if (raw[kMaterialSet]) {
        out->flags |= KILN_CROSSHAIR_MATERIAL;
        out->material = raw[kMaterial];
    }
}

void bind(uint32_t hit_offset) {
    const uint32_t before = g_offset.exchange(hit_offset < kMaxFrameOffset ? hit_offset : 0, std::memory_order_acq_rel);
    if (before != g_offset.load(std::memory_order_acquire)) changed();
}

void observe(uintptr_t frame, const Memory& m) {
    const uint32_t offset = g_offset.load(std::memory_order_acquire);
    if (!offset) return;
    const uint32_t session = g_live.load(std::memory_order_acquire);
    if (!session || session <= g_ended.load(std::memory_order_acquire) ||
        g_faulted.load(std::memory_order_acquire) == session || !frame) {
        g_skipped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    uint8_t raw[kResultBytes];
    if (!m.read(m.ctx, raw, frame + offset, sizeof raw)) {
        g_faulted.store(session, std::memory_order_release);
        g_faults.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    KilnCrosshairHit h{};
    decode(raw, &h);
    if (!begin_write()) {
        g_skipped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    Sample s{};
    s.sequence = g_next.fetch_add(1, std::memory_order_relaxed) + 1;
    s.client_step = g_step.load(std::memory_order_relaxed);
    s.captured_ns = g_clock.load(std::memory_order_relaxed)();
    s.flags = h.flags;
    s.status = h.status;
    s.entity = h.entity;
    s.material = h.material;
    s.client_session = session;
    for (int i = 0; i < 3; ++i) {
        s.position[i] = h.position[i];
        s.normal[i] = h.normal[i];
    }
    s.distance = h.distance;
    store(s);
    end_write();
    g_samples.fetch_add(1, std::memory_order_relaxed);
}

void session_live(uint32_t session, uint64_t step) {
    g_step.store(step, std::memory_order_relaxed);
    const uint32_t before = g_live.exchange(session, std::memory_order_acq_rel);
    if (before != session) {
        clear_samples();
        if (g_faulted.load(std::memory_order_acquire) != session) g_faulted.store(0, std::memory_order_release);
    }
}

void session_ended(uint32_t session) {
    uint32_t ended = g_ended.load(std::memory_order_acquire);
    while (session > ended && !g_ended.compare_exchange_weak(ended, session, std::memory_order_acq_rel)) {
    }
    uint32_t live = session;
    g_live.compare_exchange_strong(live, 0, std::memory_order_acq_rel);
    clear_samples();
}

KilnResult get(const Env& env, KilnCrosshairHit* out, uint32_t* out_reason) {
    if (!out || out->size < sizeof(KilnCrosshairHit)) return KILN_ERR_INVALID_ARGUMENT;
    if (!env.client_side || !env.hooks) return KILN_ERR_UNAVAILABLE;
    if (!g_offset.load(std::memory_order_acquire) && !install_once()) {
        // an install refused for threads that could not be checked: a later get tries again; else final, or none
        return KILN_ERR_UNAVAILABLE;
    }
    KilnCrosshairHit h{};
    h.size = sizeof h;
    uint32_t reason = KILN_CROSSHAIR_REASON_NO_SAMPLE;
    const uint32_t live = g_live.load(std::memory_order_acquire);
    Sample s{};
    if (live && live > g_ended.load(std::memory_order_acquire) && g_faulted.load(std::memory_order_acquire) == live) {
        reason = KILN_CROSSHAIR_REASON_FAULT;
    } else if (!load(&s)) {
        reason = KILN_CROSSHAIR_REASON_BUSY;
    } else if (live && s.sequence && s.client_session == live && live > g_ended.load(std::memory_order_acquire)) {
        const int64_t now = g_clock.load(std::memory_order_relaxed)();
        h.flags = s.flags;
        h.status = s.status;
        h.entity = s.entity;
        h.material = s.material;
        h.client_session = s.client_session;
        h.sequence = s.sequence;
        h.client_step = s.client_step;
        h.age_ns = now > s.captured_ns ? static_cast<uint64_t>(now - s.captured_ns) : 0;
        for (int i = 0; i < 3; ++i) {
            h.position[i] = s.position[i];
            h.normal[i] = s.normal[i];
        }
        h.distance = s.distance;
        reason = h.age_ns > KILN_CROSSHAIR_FRESH_NS ? KILN_CROSSHAIR_REASON_STALE : KILN_CROSSHAIR_REASON_NONE;
    }
    *out = h;
    if (out_reason) *out_reason = reason;
    return KILN_OK;
}

void set_installer(Installer install) {
    g_installer.store(install, std::memory_order_release);
    g_install_failed.store(false, std::memory_order_release);
    changed();
}

void unbound_reason(const char* why) {
    g_why.store(why ? why : kUnbound, std::memory_order_release);
    g_install_failed.store(false, std::memory_order_release);
    changed();
}

bool usable(const char** why, bool* installed) {
    const bool bound = g_offset.load(std::memory_order_acquire) != 0;
    if (installed) *installed = bound;
    if (bound || g_installer.load(std::memory_order_acquire)) return true;
    if (why) *why = g_why.load(std::memory_order_acquire);
    return false;
}

bool install_failed() { return g_install_failed.load(std::memory_order_acquire); }
uint64_t generation() { return g_generation.load(std::memory_order_acquire); }

Stats stats() {
    return {g_samples.load(std::memory_order_relaxed), g_skipped.load(std::memory_order_relaxed),
            g_faults.load(std::memory_order_relaxed), g_installs.load(std::memory_order_relaxed)};
}

void set_clock_for_tests(int64_t (*ns)()) { g_clock.store(ns ? ns : &steady_ns, std::memory_order_relaxed); }

void reset_for_tests() {
    std::lock_guard install(g_install_mu);
    g_writing.store(false);
    store(Sample{});
    g_next = 0;
    g_live = 0;
    g_step = 0;
    g_ended = 0;
    g_faulted = 0;
    g_offset = 0;
    g_samples = g_skipped = g_faults = g_installs = 0;
    g_installer = nullptr;
    g_install_failed = false;
    g_why = kUnbound;
    changed();
}

void hold_publish_for_tests(bool held) {
    if (held) {
        g_writing.store(true, std::memory_order_release);
        g_seq.fetch_add(1, std::memory_order_acq_rel);
    } else {
        g_seq.fetch_add(1, std::memory_order_acq_rel);
        g_writing.store(false, std::memory_order_release);
    }
}

}  // namespace kiln::crosshair
