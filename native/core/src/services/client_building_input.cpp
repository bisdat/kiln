// SPDX-License-Identifier: MIT
// kiln.client's building input (client_building_input.h says what and how).
// Adapted from ShroudForge (MIT, Copyright (c) 2026 bonsaibauer): modules/game-runtime/native/player-input/
// apply_building_input.cpp of ShroudForge 1.7.9.
#include "client_building_input.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>

namespace kiln::building_input {
namespace {

constexpr uint64_t kQueueMs = 5000;          // a request not emitted by then times out (a pressed one still releases)
constexpr uint64_t kDismantleHoldMs = 1200;  // a dismantle is held this long, written again at every visit

// keen::WorldTransform: position in Q32.32 metres, quaternion x y z w, scale.
struct Transform {
    int64_t position[3];
    float rotation[4];
    float scale[3];
    uint32_t padding;
};
static_assert(sizeof(Transform) == 56, "keen::WorldTransform is 56 bytes");

struct Command {
    uint32_t id = 0, status = KILN_BUILDING_INPUT_UNKNOWN, terminal = 0;  // terminal: what a pressed one ends as
    uint32_t kind = 0, player = 0, item = 0, material = 0, slot = 0, target = 0;
    uint32_t session = 0, pressed_version = 0;
    KilnOwner owner = 0;
    Transform transform{};
    uint64_t queued_at = 0, pressed_at = 0;
    uintptr_t input = 0;  // the ClientPlayerInput that took the press: the only one released
};

uint64_t steady_ms() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
}

const char* const kUnbound =
    "no verified client input observation point on this game build: the client cursor system's input point, its building "
    "version call and the ClientPlayerInput layout are not in sigdb";

std::atomic<uint64_t (*)()> g_clock{&steady_ms};
std::mutex g_mu;  // the slot below; never held across a call out of this file
Command g_cmd;
uint32_t g_next_id = 0;
uint32_t g_ended = 0;  // the newest client session whose world stopped
Layout g_layout{};
std::atomic<bool> g_bound{false};
std::atomic<uint64_t> g_skipped{0}, g_faults{0};

uint64_t now_ms() { return g_clock.load(std::memory_order_relaxed)(); }
bool open(const Command& c) { return c.status == KILN_BUILDING_INPUT_QUEUED || c.status == KILN_BUILDING_INPUT_PRESSED; }
uint32_t shown(const Command& c) { return c.terminal ? c.terminal : c.status; }
uint64_t bit(uint8_t n) { return n < 64 ? uint64_t{1} << n : 0; }

// The request's fields, by the source's rules (KfcRuntimeWorldBuildingInput). The source does this math in long double,
// which is double on its Windows compilers; double here keeps the same results on every compiler.
bool convert(const KilnBuildingInput& in, Command& out) {
    const uint32_t kind = in.action;
    if (!in.player || kind > KILN_BUILDING_ACTION_DISMANTLE || in.slot > 255 || in.reserved ||
        (kind == KILN_BUILDING_ACTION_SELECT && !in.item_id) ||
        (kind == KILN_BUILDING_ACTION_DISMANTLE) != (in.target_entity_id != 0))
        return false;
    out.kind = kind;
    out.player = in.player;
    out.item = in.item_id;
    out.material = in.material_item_id;
    out.slot = in.slot;
    out.target = in.target_entity_id;
    out.transform = {};
    if (kind == KILN_BUILDING_ACTION_PLACE || kind == KILN_BUILDING_ACTION_REMOVE || kind == KILN_BUILDING_ACTION_DISMANTLE) {
        double norm = 0;
        for (double r : in.rotation) {
            if (!std::isfinite(r)) return false;
            norm += r * r;
        }
        if (!std::isfinite(norm) || norm < 1e-12) return false;
        const double length = std::sqrt(norm);
        for (int i = 0; i < 4; ++i) out.transform.rotation[i] = static_cast<float>(in.rotation[i] / length);
        for (int i = 0; i < 3; ++i) {
            const double fixed = std::ldexp(in.position[i], 32);
            if (!std::isfinite(fixed) || fixed <= static_cast<double>(INT64_MIN) || fixed >= static_cast<double>(INT64_MAX) ||
                !std::isfinite(in.scale[i]) || in.scale[i] <= 0 || in.scale[i] > 1024)
                return false;
            out.transform.position[i] = static_cast<int64_t>(std::llround(fixed));
            out.transform.scale[i] = static_cast<float>(in.scale[i]);
        }
    }
    return true;
}

// The source's 5 s rule, at every status and every visit.
void expire(Command& c, uint64_t now) {
    if (now - c.queued_at <= kQueueMs) return;
    if (c.status == KILN_BUILDING_INPUT_QUEUED) c.status = KILN_BUILDING_INPUT_TIMEOUT;
    else if (c.status == KILN_BUILDING_INPUT_PRESSED) c.terminal = KILN_BUILDING_INPUT_TIMEOUT;
}

void fault(Command& c) {
    c.status = KILN_BUILDING_INPUT_CANCELLED;
    g_faults.fetch_add(1, std::memory_order_relaxed);
}

// One visit to the request's own player's input (the source's apply()). g_mu is held.
void apply(const Observation& o, const Memory& m, const Layout& l, Command& c, uint64_t now) {
    uint64_t bits = 0;
    if (!m.read(m.ctx, &bits, o.record + l.digital, sizeof bits)) return fault(c);
    const auto put = [&](uint32_t offset, const void* bytes, size_t n) { return m.write(m.ctx, o.record + offset, bytes, n); };
    const Transform cursor[2] = {c.transform, c.transform};  // primary and secondary
    const uint64_t building = bit(l.mainhand) | bit(l.mainhand_tap) | bit(l.mainhand_hold) | bit(l.mainhand_release) |
                              bit(l.secondary) | bit(l.contextual) | bit(l.contextual_hold) | bit(l.undo);
    const uint64_t dismantle = bit(l.contextual) | bit(l.contextual_hold);
    if (c.status == KILN_BUILDING_INPUT_PRESSED) {
        // Released only on the input object that took the press: never redirected to another one.
        if (o.record != c.input) return;
        if (c.kind == KILN_BUILDING_ACTION_DISMANTLE && now - c.pressed_at < kDismantleHoldMs) {
            bits |= dismantle;
            if (!put(l.cursor, cursor, sizeof cursor) || !put(l.digital, &bits, sizeof bits)) fault(c);
            return;
        }
        // A system may visit the same input more than once per input version: the press lives until a later one.
        if (o.version == c.pressed_version) return;
        if (c.kind != KILN_BUILDING_ACTION_UNDO && !put(l.cursor, cursor, sizeof cursor)) return fault(c);
        bits &= ~building;
        if (c.kind == KILN_BUILDING_ACTION_PLACE) bits |= bit(l.mainhand_release);
        if (!put(l.digital, &bits, sizeof bits)) return fault(c);
        c.status = c.terminal ? c.terminal : KILN_BUILDING_INPUT_DISPATCHED;  // emitted, not acknowledged by the world
        return;
    }
    if (bits & building) return;  // the player's own building action finishes first
    if (c.kind == KILN_BUILDING_ACTION_SELECT) {
        // Data first, the version stamp last (the game picks the action up by its version).
        const auto slot = static_cast<uint8_t>(c.slot);
        if (!put(l.create + 4, &slot, 1) || !put(l.create + 8, &c.item, 4) || !put(l.create, &o.version, 4)) return fault(c);
        if (c.material) {
            // The terrain and default building material; roof and overgrowth stay the game's.
            const uint32_t materials[2] = {c.material, c.material};
            if (!put(l.stock + 4, materials, sizeof materials) || !put(l.stock, &o.version, 4)) return fault(c);
        }
        c.status = KILN_BUILDING_INPUT_DISPATCHED;
    } else {
        if (c.kind == KILN_BUILDING_ACTION_DISMANTLE && !c.target) return;  // submit refuses it; never pressed without one
        // The cursor transform only; the game's validity flags stay, and the server still checks permission, range,
        // recipe and placement as for the player's own input.
        if (c.kind != KILN_BUILDING_ACTION_UNDO && !put(l.cursor, cursor, sizeof cursor)) return fault(c);
        bits |= c.kind == KILN_BUILDING_ACTION_PLACE    ? bit(l.mainhand) | bit(l.mainhand_tap)
                : c.kind == KILN_BUILDING_ACTION_REMOVE ? bit(l.secondary)
                : c.kind == KILN_BUILDING_ACTION_UNDO   ? bit(l.undo)
                                                        : dismantle;
        if (!put(l.digital, &bits, sizeof bits)) return fault(c);
        c.status = KILN_BUILDING_INPUT_PRESSED;
        c.pressed_version = o.version;
        c.pressed_at = now;
    }
    c.input = o.record;
}

// The checks status and cancel share.
KilnResult caller_ok(const Env& env, KilnOwner owner) {
    if (owner == 0) return KILN_ERR_INVALID_ARGUMENT;
    if (!env.owner_alive) return KILN_ERR_REVOKED;
    if (!env.client_side || !env.hooks) return KILN_ERR_UNAVAILABLE;
    return KILN_OK;
}

}  // namespace

KilnResult submit(const Env& env, KilnOwner owner, uint64_t expected_session, const KilnBuildingInput* input,
                  uint32_t* request_id) {
    if (request_id) *request_id = 0;
    if (!input || !request_id || input->size < sizeof(KilnBuildingInput) || owner == 0) return KILN_ERR_INVALID_ARGUMENT;
    if (!env.owner_alive) return KILN_ERR_REVOKED;
    if (!env.client_side || !env.hooks || !g_bound.load(std::memory_order_acquire)) return KILN_ERR_UNAVAILABLE;
    Command next;
    if (!convert(*input, next)) return KILN_ERR_INVALID_ARGUMENT;
    const uint64_t now = now_ms();
    {
        std::lock_guard lock(g_mu);
        if (!g_bound.load(std::memory_order_relaxed)) return KILN_ERR_UNAVAILABLE;  // stopped meanwhile
        if (!env.live_session || env.live_session <= g_ended || (expected_session && expected_session != env.live_session))
            return KILN_CLIENT_ERR_BUILDING_STALE;
        if (open(g_cmd)) return KILN_CLIENT_ERR_BUILDING_BUSY;
        next.id = ++g_next_id;
        if (!next.id) next.id = ++g_next_id;
        next.status = KILN_BUILDING_INPUT_QUEUED;
        next.owner = owner;
        next.session = env.live_session;
        next.queued_at = now;
        g_cmd = next;
    }
    // The owner may have been revoked after env was read: its owner_gone() then ran before the request was in the slot
    // and found nothing. Asked again now (outside the slot's lock), a revoke after this answer finds the request there.
    if (env.owner_alive_now && !env.owner_alive_now(owner)) {
        owner_gone(owner);
        return KILN_ERR_REVOKED;
    }
    *request_id = next.id;
    return KILN_OK;
}

KilnResult status(const Env& env, KilnOwner owner, uint32_t request_id, uint32_t* state) {
    if (!state) return KILN_ERR_INVALID_ARGUMENT;
    *state = KILN_BUILDING_INPUT_UNKNOWN;
    if (const KilnResult rc = caller_ok(env, owner); rc != KILN_OK) return rc;
    const uint64_t now = now_ms();
    std::lock_guard lock(g_mu);
    if (!request_id || request_id != g_cmd.id || owner != g_cmd.owner) return KILN_OK;
    expire(g_cmd, now);
    *state = shown(g_cmd);
    return KILN_OK;
}

KilnResult cancel(const Env& env, KilnOwner owner, uint32_t request_id, uint32_t* cancelled) {
    if (!cancelled) return KILN_ERR_INVALID_ARGUMENT;
    *cancelled = 0;
    if (const KilnResult rc = caller_ok(env, owner); rc != KILN_OK) return rc;
    std::lock_guard lock(g_mu);
    if (!request_id || request_id != g_cmd.id || owner != g_cmd.owner) return KILN_OK;
    // A queued request never runs; a pressed one is released at its next input; a finished one stays as it ended.
    if (g_cmd.status == KILN_BUILDING_INPUT_QUEUED) g_cmd.status = KILN_BUILDING_INPUT_CANCELLED;
    else if (g_cmd.status == KILN_BUILDING_INPUT_PRESSED) g_cmd.terminal = KILN_BUILDING_INPUT_CANCELLED;
    *cancelled = 1;
    return KILN_OK;
}

void bind(const Layout* layout) {
    std::lock_guard lock(g_mu);
    // Installed or removed, the input path changed under an open request (the source's Reset).
    if (open(g_cmd)) g_cmd.status = KILN_BUILDING_INPUT_CANCELLED;
    if (layout) g_layout = *layout;
    g_bound.store(layout != nullptr, std::memory_order_release);
}

bool bound(const char** why) {
    if (g_bound.load(std::memory_order_acquire)) return true;
    if (why) *why = kUnbound;
    return false;
}

void observe(const Observation& o, const Memory& m) {
    std::unique_lock lock(g_mu, std::try_to_lock);
    if (!lock.owns_lock()) {
        g_skipped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    Command& c = g_cmd;
    if (!g_bound.load(std::memory_order_relaxed) || !open(c) || !o.record) return;
    const uint64_t now = now_ms();
    if (now - c.queued_at > kQueueMs) {
        if (c.status == KILN_BUILDING_INPUT_QUEUED) {
            c.status = KILN_BUILDING_INPUT_TIMEOUT;
            return;
        }
        c.terminal = KILN_BUILDING_INPUT_TIMEOUT;  // still release an already pressed input
    }
    if (o.entity != c.player || o.session != c.session) return;
    apply(o, m, g_layout, c, now);
}

void session_ended(uint32_t session) {
    std::lock_guard lock(g_mu);
    if (session > g_ended) g_ended = session;
    // Its input object is gone with the world: nothing is left to release.
    if (open(g_cmd) && g_cmd.session <= session) g_cmd.status = KILN_BUILDING_INPUT_CANCELLED;
}

void owner_gone(KilnOwner owner) {
    if (owner == 0) return;
    std::lock_guard lock(g_mu);
    if (g_cmd.owner != owner) return;
    if (g_cmd.status == KILN_BUILDING_INPUT_QUEUED) g_cmd.status = KILN_BUILDING_INPUT_CANCELLED;
    else if (g_cmd.status == KILN_BUILDING_INPUT_PRESSED) g_cmd.terminal = KILN_BUILDING_INPUT_CANCELLED;
}

Stats stats() { return {g_skipped.load(std::memory_order_relaxed), g_faults.load(std::memory_order_relaxed)}; }

void set_clock_for_tests(uint64_t (*ms)()) { g_clock.store(ms ? ms : &steady_ms, std::memory_order_relaxed); }

void reset_for_tests() {
    std::lock_guard lock(g_mu);
    g_cmd = {};
    g_next_id = 0;
    g_ended = 0;
    g_layout = {};
    g_bound.store(false, std::memory_order_release);
    g_skipped.store(0, std::memory_order_relaxed);
    g_faults.store(0, std::memory_order_relaxed);
}

void set_next_id_for_tests(uint32_t last) {
    std::lock_guard lock(g_mu);
    g_next_id = last;
}

std::mutex& mutex_for_tests() { return g_mu; }

}  // namespace kiln::building_input
