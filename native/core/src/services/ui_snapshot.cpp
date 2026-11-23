// SPDX-License-Identifier: MIT
// kiln.ui.snapshot v1 (abi/services/kiln_ui_snapshot.h): live game values for UI blocks (dossier chapter 09 §4.4, §5.1).
//
// Demands (handles with a mask) say what the collector reads; their union is one atomic word, so the world threads' hot
// paths return at once when nobody asked. The collector:
//   - client world's Post (main thread), at most every 100 ms: one GetEntityTransform of the local player when the
//     position is demanded; the sample waits (up to 250 ms) for 05a's stage of the same session and step when vitals
//     or inventory are demanded, and publishes those fields as unavailable (STALE_SOURCE) when none comes: never a mix;
//   - 05a's pre-provider stage (the player-UI update's thread): copied only while a collection is due, and it publishes
//     a waiting sample of its own step (the update may run before or after the client world's Post in a frame);
//   - the hosted server world's Post: one kiln.time get, at most every 100 ms, with the server world's own stamps.
// A publication is immutable (shared_ptr<const Pub>); a token holds one, so copy always answers from the same one.
// One mutex guards demands, tokens, samples and the latest publication; no game call and no env call that takes
// another service's lock runs under it (the transform read, kiln.time and the stage subscription run outside).
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

#include "services/kiln_caps.h"
#include "ui_snapshot_internal.h"

namespace kiln::uisnapshot {
namespace {

constexpr uint32_t kVitals = KUW_LOCAL_VITALS, kPosition = KUW_LOCAL_POSITION, kInventory = KUW_LOCAL_INVENTORY,
                   kHost = KUW_HOST_TIME;
constexpr KilnHandle kTagMask = 0xffffull << 48;
constexpr KilnHandle kDemandTag = 0x5544ull << 48;  // "UD"
constexpr KilnHandle kTokenTag = 0x5554ull << 48;   // "UT"

Env g_env;

struct Pub {
    KilnUiWorldValues v{};
    std::vector<KilnUiInventoryRow> rows;
};
using PubPtr = std::shared_ptr<const Pub>;

struct Token {
    KilnHandle handle = 0;
    KilnOwner owner = 0;
    PubPtr pub;
    uint64_t created_ms = 0, gen = 0;
    uint32_t session = 0;
    bool revoked = false;
};

struct Demand {
    KilnHandle handle = 0;
    KilnOwner owner = 0;
    uint32_t mask = 0;
};

// The last stage copied (no game pointer: rows are values).
struct StageCopy {
    bool valid = false;
    uint32_t session = 0, local = 0, available = 0, rows_total = 0;
    uint64_t step = 0, capture_ms = 0;
    uint32_t health = 0, health_max = 0, stamina = 0, stamina_max = 0, mana = 0, mana_max = 0;
    std::vector<KilnUiInventoryRow> rows;
};

// One client-world Post's sample, waiting to be published.
struct PostSample {
    bool valid = false;
    uint32_t requested = 0, session = 0, state = 0, server_session = 0, local = 0;
    uint64_t step = 0, capture_ms = 0, due_ms = 0;
    bool pos_ok = false;
    uint32_t pos_reason = KUW_NO_SNAPSHOT;
    int64_t xyz[3] = {};
};

// The hosted server world's last hour.
struct HostSample {
    bool valid = false;
    uint32_t server_session = 0, reason = KUW_NO_SNAPSHOT;
    uint64_t step = 0, capture_ms = 0;
    double hours = 0;
};

struct State {
    std::mutex mu;
    std::vector<Demand> demands;
    std::vector<Token> tokens;  // live ones, and revoked ones until released or 2 s old
    PubPtr latest;
    uint64_t gen = 1;      // +1 at every clear: a token of an older one is revoked
    uint32_t session = 0;  // the client-world session the samples belong to (0 = none)
    StageCopy stage;
    PostSample post;
    HostSample host;
    uint64_t last_pub_ms = 0;  // 0: nothing published in this session yet
    uint64_t host_last_ms = 0;
    uint64_t next_publication = 0;  // monotonic for the process (a clear does not reset it)
    uint64_t next_handle = 0;
    int stage_sub = 0;  // 0 not subscribed, 1 subscribed, -1 refused (vitals and inventory: LAYOUT_OFF)
    KilnHandle stage_handle = 0;
    bool position_off = false;  // the transform reader faulted
    bool host_missing = false;  // kiln.time is not here
};
State& S = *new State;  // never destroyed: a game thread may still collect while the process ends
std::atomic<uint32_t> g_union{0};
std::atomic<uint64_t> g_transform_reads{0}, g_time_reads{0}, g_stage_copies{0}, g_publications{0}, g_subscribes{0},
    g_unsubscribes{0};

uint64_t now_ms() { return g_env.tick_ms ? g_env.tick_ms() : 0; }
uint64_t age(uint64_t now, uint64_t at) { return now > at ? now - at : 0; }
bool alive(KilnOwner o) { return o == 0 || !g_env.owner_alive || g_env.owner_alive(o); }
bool client_side() { return g_env.client_side && g_env.client_side(); }

void logf(int level, const char* fmt, ...) {
    if (!g_env.log) return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    g_env.log(level, buf);
}

void row_off(const char* name, const char* text) {
    if (g_env.row_off) g_env.row_off(name, KILN_CAP_REASON_FAULTED, text);
}

void recompute_union_locked() {
    uint32_t u = 0;
    for (const Demand& d : S.demands) u |= d.mask;
    g_union.store(u, std::memory_order_release);
}

// A world or session change: the publication and every sample go, and every token is revoked (even within its TTL).
void clear_locked() {
    ++S.gen;
    S.latest.reset();
    for (Token& t : S.tokens) t.revoked = true;
    S.stage = StageCopy{};
    S.post = PostSample{};
    S.host = HostSample{};
    S.last_pub_ms = 0;
    S.host_last_ms = 0;
}

void follow_session_locked(uint32_t session) {
    if (session == S.session) return;
    clear_locked();
    S.session = session;
}

void sweep_locked(uint64_t now) {
    std::erase_if(S.tokens, [&](const Token& t) { return age(now, t.created_ms) > kTokenTtlMs; });
}

std::vector<Token>::iterator find_token_locked(KilnHandle h) {
    return std::find_if(S.tokens.begin(), S.tokens.end(), [&](const Token& t) { return t.handle == h; });
}

// The 56-byte CurrentTransform: orientation (4 floats at +24) and scale (3 floats at +40) must be finite numbers, or
// the pointer was not a transform.
bool transform_ok(const uint8_t* raw) {
    for (size_t at = 24; at < 52; at += 4) {
        float f;
        std::memcpy(&f, raw + at, sizeof f);
        if (!std::isfinite(f)) return false;
    }
    return true;
}

uint32_t stage_reason_locked(uint32_t session) {
    return S.stage.valid && S.stage.session == session ? KUW_STALE_SOURCE : KUW_NO_SNAPSHOT;
}

// Builds and publishes one immutable composite from the waiting sample. matched: the stage is of the sample's session
// (and step, when the position was demanded) and local player.
void publish_locked(uint64_t now, bool matched) {
    const PostSample& p = S.post;
    const uint32_t want = p.requested;
    auto pub = std::make_shared<Pub>();
    KilnUiWorldValues& v = pub->v;
    v.size = sizeof v;
    v.requested_mask = want;
    v.publication = ++S.next_publication;
    v.local_session = p.session;
    const bool stage_stamp = matched && !(want & kPosition);  // no position: the stage's own step stamps the local part
    v.local_step = stage_stamp ? S.stage.step : p.step;
    v.local_capture_ms = stage_stamp ? S.stage.capture_ms : p.capture_ms;
    v.local_player = p.local;
    uint32_t avail = 0;

    if (!(want & kVitals)) v.unavailable_reason[0] = KUW_NO_SNAPSHOT;
    else if (S.stage_sub < 0) v.unavailable_reason[0] = KUW_LAYOUT_OFF;
    else if (!matched) v.unavailable_reason[0] = stage_reason_locked(p.session);
    else if (!(S.stage.available & uivalues::kStageVitals)) v.unavailable_reason[0] = KUW_MISSING_COMPONENT;
    else {
        avail |= kVitals;
        v.health = S.stage.health;
        v.health_max = S.stage.health_max;
        v.stamina = S.stage.stamina;
        v.stamina_max = S.stage.stamina_max;
        v.mana = S.stage.mana;
        v.mana_max = S.stage.mana_max;
    }

    if (!(want & kPosition)) v.unavailable_reason[1] = KUW_NO_SNAPSHOT;
    else if (!p.pos_ok) v.unavailable_reason[1] = p.pos_reason;
    else {
        avail |= kPosition;
        v.x_fixed = p.xyz[0];
        v.y_fixed = p.xyz[1];
        v.z_fixed = p.xyz[2];
    }

    if (!(want & kInventory)) v.unavailable_reason[2] = KUW_NO_SNAPSHOT;
    else if (S.stage_sub < 0) v.unavailable_reason[2] = KUW_LAYOUT_OFF;
    else if (!matched) v.unavailable_reason[2] = stage_reason_locked(p.session);
    else if (!(S.stage.available & uivalues::kStageInventory)) v.unavailable_reason[2] = KUW_MISSING_COMPONENT;
    else if (S.stage.rows_total > kRowsMax) v.unavailable_reason[2] = KUW_BUDGET;  // never a partial list
    else {
        avail |= kInventory;
        pub->rows = S.stage.rows;
        v.inventory_count = static_cast<uint32_t>(pub->rows.size());
    }

    if (!(want & kHost)) v.unavailable_reason[3] = KUW_NO_SNAPSHOT;
    else if (p.state == KILN_SESSION_REMOTE) v.unavailable_reason[3] = KUW_DEFERRED_SOURCE;  // never a subscription
    else if (p.state != KILN_SESSION_HOST) v.unavailable_reason[3] = KUW_NO_WORLD;
    else if (S.host.valid && S.host.server_session == p.server_session) {
        avail |= kHost;
        v.server_session = S.host.server_session;
        v.server_step = S.host.step;
        v.server_capture_ms = S.host.capture_ms;
        v.server_hours = S.host.hours;
    } else if (S.host_missing) v.unavailable_reason[3] = KUW_LAYOUT_OFF;
    else v.unavailable_reason[3] = S.host.server_session == p.server_session ? S.host.reason : KUW_NO_SNAPSHOT;

    v.available_mask = avail;
    S.latest = std::move(pub);
    S.last_pub_ms = now ? now : 1;
    S.post.valid = false;
    g_publications.fetch_add(1, std::memory_order_relaxed);
}

// Publishes the waiting sample when its stage is here or its grace ran out.
void try_publish_locked(uint64_t now) {
    if (!S.post.valid) return;
    const PostSample& p = S.post;
    const bool want_stage = (p.requested & (kVitals | kInventory)) != 0 && S.stage_sub > 0;
    bool matched = false;
    if (want_stage) {
        matched = S.stage.valid && S.stage.session == p.session && S.stage.local == p.local &&
                  (!(p.requested & kPosition) || S.stage.step == p.step);
        if (!matched && age(now, p.due_ms) < kMatchGraceMs) return;  // the stage of this step may still come
    }
    publish_locked(now, matched);
}

// The stage subscription follows the demand (client world's Post, main thread): no vitals or inventory demanded, no
// observer, so 05a's update walks no inventory model for us.
void reconcile_stage(uint32_t want) {
    const bool need = (want & (kVitals | kInventory)) != 0;
    int state;
    KilnHandle handle;
    {
        std::lock_guard lk(S.mu);
        state = S.stage_sub;
        handle = S.stage_handle;
    }
    if (need && state == 0) {
        KilnHandle h = 0;
        const KilnResult rc =
            g_env.stage_subscribe ? g_env.stage_subscribe(&stage_observer, nullptr, &h) : KILN_ERR_UNAVAILABLE;
        g_subscribes.fetch_add(1, std::memory_order_relaxed);
        {
            std::lock_guard lk(S.mu);
            S.stage_sub = rc == KILN_OK ? 1 : -1;
            S.stage_handle = rc == KILN_OK ? h : 0;
        }
        if (rc != KILN_OK) {
            logf(KILN_LOG_WARN,
                 "ui.snapshot: kiln.ui.values' pre-provider stage refused (%d): local vitals and inventory stay "
                 "unavailable (ui.values.hud is off, or its hooks could not be installed)",
                 rc);
            const char* why = "kiln.ui.values' pre-provider stage is not available here (ui.values.hud is off, or its "
                              "update hook could not be installed; kiln.log says why)";
            row_off("world.vitals", why);
            row_off("world.inventory", why);
        }
    } else if (!need && state == 1) {
        if (g_env.stage_unsubscribe) g_env.stage_unsubscribe(handle);
        g_unsubscribes.fetch_add(1, std::memory_order_relaxed);
        std::lock_guard lk(S.mu);
        S.stage_sub = 0;
        S.stage_handle = 0;
        S.stage = StageCopy{};
    }
}

// ---- the table ----
KilnResult KILN_CALL t_demand_add(KilnOwner owner, uint32_t mask, KilnUiDemandResult* out) {
    if (!out || out->size < sizeof(KilnUiDemandResult) || mask == 0 || (mask & ~kKnownMask))
        return KILN_ERR_INVALID_ARGUMENT;
    if (!client_side()) return KILN_ERR_UNAVAILABLE;
    if (!alive(owner)) return KILN_ERR_REVOKED;
    WorldNow w{};
    const bool joined = (mask & kHost) && g_env.now && g_env.now(&w) && w.state == KILN_SESSION_REMOTE;
    std::lock_guard lk(S.mu);
    if (!alive(owner)) return KILN_ERR_REVOKED;  // under the lock: owner_retired (after the owner died) runs after us
    out->ok = 0;
    out->reason = KUW_NONE;
    out->reserved = 0;
    out->handle = 0;
    const size_t mine = static_cast<size_t>(
        std::count_if(S.demands.begin(), S.demands.end(), [&](const Demand& d) { return d.owner == owner; }));
    if (mine >= kDemandsPerOwner || S.demands.size() >= kDemandsGlobal) {
        out->reason = KUW_BUDGET;
        return KILN_OK;
    }
    const KilnHandle h = kDemandTag | (++S.next_handle & ~kTagMask);
    S.demands.push_back(Demand{h, owner, mask});
    recompute_union_locked();
    out->ok = 1;
    out->handle = h;
    out->reason = joined ? KUW_DEFERRED_SOURCE : KUW_NONE;  // accepted; host time is deferred on a joined world
    return KILN_OK;
}

KilnResult KILN_CALL t_demand_remove(KilnHandle h) {
    if (!h) return KILN_ERR_INVALID_ARGUMENT;
    std::lock_guard lk(S.mu);
    const auto it = std::find_if(S.demands.begin(), S.demands.end(), [&](const Demand& d) { return d.handle == h; });
    if (it == S.demands.end()) return KILN_ERR_NOT_FOUND;
    S.demands.erase(it);
    recompute_union_locked();
    return KILN_OK;
}

KilnResult KILN_CALL t_acquire(KilnOwner owner, KilnUiSnapshotAcquire* out) {
    if (!out || out->size < sizeof(KilnUiSnapshotAcquire)) return KILN_ERR_INVALID_ARGUMENT;
    if (!client_side()) return KILN_ERR_UNAVAILABLE;
    if (!alive(owner)) return KILN_ERR_REVOKED;
    WorldNow w{};
    const bool have = g_env.now && g_env.now(&w);
    const uint64_t now = now_ms();
    std::lock_guard lk(S.mu);
    if (!alive(owner)) return KILN_ERR_REVOKED;
    out->ok = 0;
    out->reason = KUW_NONE;
    out->reserved = 0;
    out->token = 0;
    out->publication = 0;
    out->capture_ms = 0;
    sweep_locked(now);
    if (!have || w.state == KILN_SESSION_NONE || w.client_session == 0) {
        if (S.latest) clear_locked();  // the world is gone: nothing of it is shown again
        out->reason = KUW_NO_WORLD;
        return KILN_OK;
    }
    follow_session_locked(w.client_session);  // a new session clears the old one's publication and tokens
    if (!S.latest) {
        out->reason = KUW_NO_SNAPSHOT;
        return KILN_OK;
    }
    if (age(now, S.latest->v.local_capture_ms) > kMaxAgeMs) {
        out->reason = KUW_EXPIRED;
        return KILN_OK;
    }
    uint32_t mine = 0, all = 0;
    for (const Token& t : S.tokens)
        if (!t.revoked) {
            ++all;
            mine += t.owner == owner;
        }
    if (mine >= kTokensPerOwner || all >= kTokensGlobal) {
        out->reason = KUW_BUDGET;
        return KILN_OK;
    }
    PubPtr view = S.latest;
    if ((view->v.available_mask & kHost) && age(now, view->v.server_capture_ms) > kMaxAgeMs) {
        // the hosted world's hour is too old: this token's view goes without it (the rest is the same publication)
        auto pinned = std::make_shared<Pub>(*view);
        pinned->v.available_mask &= ~kHost;
        pinned->v.unavailable_reason[3] = KUW_EXPIRED;
        pinned->v.server_session = pinned->v.server_step = pinned->v.server_capture_ms = 0;
        pinned->v.server_hours = 0;
        view = std::move(pinned);
    }
    Token t;
    t.handle = kTokenTag | (++S.next_handle & ~kTagMask);
    t.owner = owner;
    t.pub = view;
    t.created_ms = now;
    t.gen = S.gen;
    t.session = S.session;
    S.tokens.push_back(t);
    out->ok = 1;
    out->token = t.handle;
    out->publication = view->v.publication;
    out->capture_ms = view->v.local_capture_ms;
    return KILN_OK;
}

KilnResult KILN_CALL t_copy(KilnHandle token, KilnUiWorldValues* values, KilnUiInventoryRow* rows, uint32_t capacity,
                            uint32_t* required) {
    if (!token || !values || values->size < sizeof(KilnUiWorldValues) || (capacity && !rows))
        return KILN_ERR_INVALID_ARGUMENT;
    WorldNow w{};
    const bool have = g_env.now && g_env.now(&w);
    const uint64_t now = now_ms();
    PubPtr pub;
    {
        std::lock_guard lk(S.mu);
        auto it = find_token_locked(token);
        if (it == S.tokens.end()) return KILN_ERR_NOT_FOUND;
        if (it->revoked || it->gen != S.gen) {
            it->revoked = true;
            return KILN_ERR_REVOKED;
        }
        if (!have || w.state == KILN_SESSION_NONE || w.client_session != it->session) {
            // the world changed and no event said so yet: the old publication goes now, with every token
            if (!have || w.state == KILN_SESSION_NONE) clear_locked();
            else follow_session_locked(w.client_session);
            return KILN_ERR_REVOKED;
        }
        if (age(now, it->created_ms) > kTokenTtlMs) {
            S.tokens.erase(it);
            return KILN_ERR_NOT_FOUND;  // the 2 s pin ran out: an Expired refusal
        }
        pub = it->pub;  // immutable: copied outside the lock
    }
    const uint32_t n = static_cast<uint32_t>(pub->rows.size());
    if (capacity < n) {
        if (required) *required = n;  // and nothing else
        return KILN_ERR_BUFFER_TOO_SMALL;
    }
    const uint32_t size = values->size;
    std::memcpy(values, &pub->v, sizeof(KilnUiWorldValues));
    values->size = size;
    if (n) std::memcpy(rows, pub->rows.data(), n * sizeof(KilnUiInventoryRow));
    if (required) *required = n;
    return KILN_OK;
}

KilnResult KILN_CALL t_release(KilnHandle token) {
    if (!token) return KILN_ERR_INVALID_ARGUMENT;
    std::lock_guard lk(S.mu);
    const auto it = find_token_locked(token);
    if (it == S.tokens.end()) return KILN_ERR_NOT_FOUND;
    S.tokens.erase(it);
    return KILN_OK;
}

const KilnUiSnapshotApi kTable = {
    {sizeof(KilnUiSnapshotApi), KILN_UI_SNAPSHOT_VERSION}, &t_demand_add, &t_demand_remove, &t_acquire, &t_copy,
    &t_release,
};

}  // namespace

Env& env() { return g_env; }
const KilnUiSnapshotApi* table() { return &kTable; }

void on_client_post(uint64_t step) {
    const uint32_t want = g_union.load(std::memory_order_acquire);
    reconcile_stage(want);
    if (!want) return;  // nobody asked: no read
    WorldNow w{};
    if (!g_env.now || !g_env.now(&w)) return;
    const uint64_t now = now_ms();
    bool read = false;
    uint64_t due = now;
    {
        std::lock_guard lk(S.mu);
        follow_session_locked(w.client_session);
        const bool waiting = S.post.valid;  // a sample of an earlier step still waits for its stage: take this step's
        if (!waiting && S.last_pub_ms && age(now, S.last_pub_ms) < kCadenceMs) return;
        if (waiting) due = S.post.due_ms;
        read = (want & kPosition) && !S.position_off && g_env.read_transform && w.local_player && w.client_world;
    }
    uint8_t raw[kTransformBytes] = {};
    int rc = kReadAbsent;
    if (read) {  // one read per Post, on the client world's own thread
        rc = g_env.read_transform(w.client_world, w.local_player, raw);
        g_transform_reads.fetch_add(1, std::memory_order_relaxed);
    }
    bool faulted = false;
    {
        std::lock_guard lk(S.mu);
        if (S.session != w.client_session) return;  // a world event cleared in between
        PostSample p;
        p.valid = true;
        p.requested = want;
        p.session = w.client_session;
        p.state = w.state;
        p.server_session = w.server_session;
        p.local = w.local_player;
        p.step = step;
        p.capture_ms = now;
        p.due_ms = due;
        if (!(want & kPosition)) p.pos_reason = KUW_NO_SNAPSHOT;
        else if (S.position_off || !g_env.read_transform) p.pos_reason = KUW_LAYOUT_OFF;
        else if (!w.local_player || !w.client_world) p.pos_reason = KUW_NO_WORLD;
        else if (rc == kReadFault) {
            S.position_off = faulted = true;
            p.pos_reason = KUW_LAYOUT_OFF;
        } else if (rc != kReadOk || !transform_ok(raw)) p.pos_reason = KUW_MISSING_COMPONENT;
        else {
            p.pos_ok = true;
            p.pos_reason = KUW_NONE;
            std::memcpy(p.xyz, raw, sizeof p.xyz);  // keen::WorldPosition: i64 x, y, z at +0, +8, +16
        }
        S.post = p;
        try_publish_locked(now);
    }
    if (faulted) {
        logf(KILN_LOG_ERROR, "ui.snapshot: GetEntityTransform faulted on the client world; the local position is off");
        row_off("world.position", "GetEntityTransform faulted on the client world (kiln.log has the crash report)");
    }
}

void on_server_post(uint64_t step) {
    if (!(g_union.load(std::memory_order_acquire) & kHost)) return;
    WorldNow w{};
    if (!g_env.now || !g_env.now(&w) || w.state != KILN_SESSION_HOST) return;  // only the world this game hosts
    const uint64_t now = now_ms();
    {
        std::lock_guard lk(S.mu);
        if (S.host_last_ms && age(now, S.host_last_ms) < kCadenceMs) return;
        S.host_last_ms = now ? now : 1;
    }
    double hours = 0;
    int rc = kReadFault;
    if (g_env.host_hours) {
        rc = g_env.host_hours(&hours);
        g_time_reads.fetch_add(1, std::memory_order_relaxed);
    }
    std::lock_guard lk(S.mu);
    HostSample h;
    h.server_session = w.server_session;
    h.step = step;
    h.capture_ms = now;
    S.host_missing = rc == kReadFault;
    if (rc == kReadOk && std::isfinite(hours)) {
        h.valid = true;
        h.reason = KUW_NONE;
        h.hours = hours;
    } else {
        h.reason = rc == kReadFault ? KUW_LAYOUT_OFF : rc == kReadOk ? KUW_MISSING_COMPONENT : KUW_NO_SNAPSHOT;
    }
    S.host = h;
}

void on_world_event(uint32_t event, uint32_t kind, uint32_t session) {
    if (kind != KILN_WORLD_CLIENT && kind != KILN_WORLD_SERVER) return;
    std::lock_guard lk(S.mu);
    clear_locked();
    if (kind == KILN_WORLD_CLIENT) S.session = event == KILN_WORLD_STARTED ? session : 0;
}

void owner_retired(KilnOwner owner) {
    std::lock_guard lk(S.mu);
    if (std::erase_if(S.demands, [&](const Demand& d) { return d.owner == owner; })) recompute_union_locked();
    for (Token& t : S.tokens)
        if (t.owner == owner) t.revoked = true;
}

void stage_observer(const uivalues::Stage* s, void*) {
    if (!s || s->size < sizeof(uivalues::Stage)) return;
    if (!(g_union.load(std::memory_order_acquire) & (kVitals | kInventory))) return;
    const uint64_t now = now_ms();
    std::lock_guard lk(S.mu);
    if (S.stage_sub <= 0 || s->session != S.session) return;  // the client world's Post takes a new session in first
    const bool due = S.post.valid || !S.last_pub_ms || age(now, S.last_pub_ms) >= kCadenceMs;
    if (!due) return;  // copied only while a collection is due
    StageCopy& c = S.stage;
    c.valid = true;
    c.session = static_cast<uint32_t>(s->session);
    c.step = s->step;
    c.capture_ms = s->capture_ms;
    c.local = s->local_player;
    c.available = s->available;
    c.rows_total = s->rows_total;
    const bool vitals = (s->available & uivalues::kStageVitals) != 0;
    c.health = vitals ? s->health : 0;
    c.health_max = vitals ? s->health_max : 0;
    c.stamina = vitals ? s->stamina : 0;
    c.stamina_max = vitals ? s->stamina_max : 0;
    c.mana = vitals ? s->mana : 0;
    c.mana_max = vitals ? s->mana_max : 0;
    c.rows.clear();
    if ((s->available & uivalues::kStageInventory) && s->rows_total <= kRowsMax && s->rows) {
        const uint32_t n = std::min(s->row_count, kRowsMax);
        c.rows.resize(n);
        for (uint32_t i = 0; i < n; ++i) {
            const uivalues::StageRow& r = s->rows[i];
            c.rows[i] = KilnUiInventoryRow{r.holder, r.inventory_page, r.slot_index, r.item, r.count, r.pide};
        }
    }
    g_stage_copies.fetch_add(1, std::memory_order_relaxed);
    try_publish_locked(now);
}

Stats stats() {
    Stats s;
    s.transform_reads = g_transform_reads.load();
    s.time_reads = g_time_reads.load();
    s.stage_copies = g_stage_copies.load();
    s.publications = g_publications.load();
    s.stage_subscribes = g_subscribes.load();
    s.stage_unsubscribes = g_unsubscribes.load();
    return s;
}

uint32_t demand_union() { return g_union.load(std::memory_order_acquire); }

void reset_for_tests() {
    std::lock_guard lk(S.mu);
    S.demands.clear();
    S.tokens.clear();
    S.latest.reset();
    S.gen = 1;
    S.session = 0;
    S.stage = StageCopy{};
    S.post = PostSample{};
    S.host = HostSample{};
    S.last_pub_ms = S.host_last_ms = 0;
    S.next_publication = S.next_handle = 0;
    S.stage_sub = 0;
    S.stage_handle = 0;
    S.position_off = S.host_missing = false;
    g_union.store(0);
    for (auto* c : {&g_transform_reads, &g_time_reads, &g_stage_copies, &g_publications, &g_subscribes, &g_unsubscribes})
        c->store(0);
}

}  // namespace kiln::uisnapshot
