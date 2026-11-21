// SPDX-License-Identifier: MIT
// kiln.repl.net's sidecar tracking (kiln_repl.h "tracking (sidecars)"; repl.h "track.cpp"; DESIGN "Native ABI": track,
// peer_entities). Which entities each peer was sent and lost on its client channel, for Kiln's own channel 12.
//
// Send side (a server, or a hosted world): stats.cpp's SendCallback hook (one link for stats, watches and tracking:
// stats_apply_hooks' want-set asks track_active()) hands each client-channel batch to track_callback after the game's
// callback, where batch->sent says which messages the session accepted (their bytes are final only there). Its job writes
// one record per batch into its machine's ring (rings.h, track.h): CREATED per accepted create (tick = the create's header
// tick = the entity's generation for that peer), DESTROYED per id of a destroy list's list A, LEFT per id of its list B;
// nothing for refused messages and nothing on the prediction channel; no allocation, no lock, guard.h's shape (the hook is
// stats.cpp's). In Pre (tick thread) track_pre reads the rings under the peers the last frame had: GAP before a record
// that follows dropped ones, then its events; PEER_GONE for a machine whose peer changed (and for every machine when the
// world stops); every track's fn gets the batch in one call. track_pre also keeps a generation table per machine (entity ->
// create tick) for peer_entities, which reads the machine's client context's state map (the engine map at ctx +
// kCtxStateMap; PerPlayerState byte +0 = acked) on the tick thread, where no replication job runs.
//
// Receive side (a game client): client.cpp's ClientStore hook (pred 0) hands each stored client-channel message to
// track_client_store, which writes the same records into one ring; client.cpp's client Pre hands them to the receive
// tracks (peer 0, machine 0). The store hook is in while a receive watch or a receive track exists (client_apply).
//
// Tracks: at most 16. remove() and an owner's revoke only mark a track (any thread); the thread of its direction frees it
// between deliveries, so a track's fn is never called after the Pre that follows. A fn that faults ends its track.
#include <windows.h>

#include <atomic>
#include <cstring>
#include <iterator>
#include <unordered_map>
#include <vector>

#include "repl.h"
#include "rings.h"
#include "track.h"
#include "wire.h"

namespace repl {
namespace {

constexpr uint32_t kMaxTracks = 16;
track::Table<kMaxTracks> g_tracks;

// ---- send side ----
Ring g_ring[KILN_REPL_MACHINES];          // producer: the machine's client-channel SendCallback job; consumer: Pre
std::atomic<bool> g_send_on{false};       // a live send track and its rings: the callback writes
bool g_send_rings = false;                // tick thread
std::atomic<bool> g_send_dirty{false};    // a send track was removed off the tick thread
std::unordered_map<uint32_t, uint32_t> g_gen[KILN_REPL_MACHINES];  // entity -> create tick, per machine (tick thread)
std::vector<KilnReplTrackEvent> g_events; // one Pre's batch (tick thread; grows, never inside a hook)
struct Gone {
    uint32_t peer;
    uint8_t flags, machine;
};
Gone g_gone[KILN_REPL_MACHINES * 2];      // PEER_GONE waiting for the next Pre (tick thread)
uint32_t g_gone_n = 0;

// ---- receive side (the game's main thread) ----
Ring g_client_ring;
std::atomic<bool> g_client_on{false};
std::atomic<bool> g_client_dirty{false};
std::vector<KilnReplTrackEvent> g_client_events;

bool call_guarded(KilnReplTrackFn fn, const KilnReplTrackEvent* e, uint32_t n, void* user) {
    __try {
        fn(e, n, user);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Every live track of the direction gets the batch (one call); a track whose fn faults ends.
void deliver(uint32_t direction, const std::vector<KilnReplTrackEvent>& events) {
    if (events.empty()) return;
    g_tracks.each(direction, [&](track::Slot& s) {
        if (call_guarded(s.fn, events.data(), static_cast<uint32_t>(events.size()), s.user)) return;
        logf(KILN_LOG_WARN, "repl: a sidecar track callback faulted; the track ends");
        if (g_tracks.remove(s.handle.load(std::memory_order_relaxed)) == KILN_REPL_SEND) g_send_dirty.store(true, std::memory_order_relaxed);
        else g_client_dirty.store(true, std::memory_order_relaxed);
    });
}

KilnReplTrackEvent event(uint32_t peer, uint8_t flags, uint8_t machine, uint8_t kind, uint32_t entity, uint32_t tick) {
    KilnReplTrackEvent e{};
    e.peer = peer;
    e.entity = entity;
    e.tick = tick;
    e.kind = kind;
    e.peer_flags = flags;
    e.machine = machine;
    return e;
}

void free_send_rings() {
    for (Ring& r : g_ring) r.release();
    for (auto& g : g_gen) g.clear();
    g_send_rings = false;
}

// Tick thread: the callback follows the live send tracks; the hooks follow the want-set.
void apply_send() {
    g_send_dirty.store(false, std::memory_order_relaxed);
    const uint32_t live = g_tracks.apply(KILN_REPL_SEND);
    const bool was = g_send_on.load(std::memory_order_relaxed);
    if (live && !g_send_rings) {
        bool ok = true;
        for (Ring& r : g_ring) ok = ok && r.init();
        if (!ok) {
            free_send_rings();
            logf(KILN_LOG_WARN, "repl: sidecar tracking has no rings (out of memory): no events");
        } else {
            g_send_rings = true;
        }
    }
    g_send_on.store(live && g_send_rings, std::memory_order_release);
    if (!live && g_send_rings) free_send_rings();  // the callback no longer writes: send step quiet in Pre / a tick API
    if (was != g_send_on.load(std::memory_order_relaxed)) stats_apply_hooks();
}

uint32_t machine_of_peer(uint32_t peer, uint8_t* flags) {
    for (uint32_t m = 0; m < KILN_REPL_MACHINES; ++m) {
        uint8_t f = 0;
        if (peer && peer_of(m, &f) == peer) {
            if (flags) *flags = f;
            return m;
        }
    }
    return KILN_REPL_MACHINES;
}

// The machine's client context (tick thread; 0 when the world or its objects are not there).
uintptr_t client_ctx(uint32_t machine) {
    const uintptr_t world = g_world.load(std::memory_order_relaxed);
    if (!world || L.world_objects < 0 || L.obj_stride <= 0 || L.obj_client_ctx < 0) return 0;
    const uintptr_t objs = read<uintptr_t>(world + static_cast<uintptr_t>(L.world_objects));
    if (!objs) return 0;
    return objs + machine * static_cast<uintptr_t>(L.obj_stride) + static_cast<uintptr_t>(L.obj_client_ctx);
}

// The state map of a client context: every relevant entity and its acked byte. Guarded (a bad map is no entities).
struct MapRead {
    uintptr_t ctx;
    KilnReplPeerEntity* out;
    uint32_t cap, n;
};
bool read_state_map(MapRead* r) {
    __try {
        const uintptr_t map = r->ctx + kCtxStateMap;
        const uint64_t cap = *reinterpret_cast<const uint64_t*>(map + kMapCap);
        r->n = 0;
        if (!cap) return true;
        if (cap & (cap - 1)) return false;
        const auto* bits = *reinterpret_cast<const uint64_t* const*>(map + kMapBits);
        const auto* keys = *reinterpret_cast<const uint32_t* const*>(map + kMapKeys);
        const auto* values = *reinterpret_cast<const uintptr_t* const*>(map + kMapValues);
        if (!bits || !keys || !values) return false;
        for (uint64_t i = 0; i < cap; ++i) {
            if (!((bits[i >> 6] >> (i & 63)) & 1)) continue;
            if (r->n < r->cap && r->out) {
                KilnReplPeerEntity& e = r->out[r->n];
                e.entity = keys[i];
                const uintptr_t state = values[i];
                e.acked = state ? *reinterpret_cast<const uint8_t*>(state) : 0;
                std::memset(e.pad, 0, sizeof e.pad);
                e.create_tick = 0;  // filled outside the guard
            }
            ++r->n;
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

}  // namespace

void track_start() {}

uint32_t track_caps() {
    return (observe_on() ? KILN_REPL_NET_CAP_TRACK : 0u) | (client_on() ? KILN_REPL_NET_CAP_CLIENT_TRACK : 0u);
}

bool track_active() { return g_send_on.load(std::memory_order_acquire); }

// stats.cpp's SendCallback hook, inside its guard (job thread): the batch's events into its machine's ring.
void track_callback(const Batch* b) {
    if (!g_send_on.load(std::memory_order_acquire) || b->machine >= KILN_REPL_MACHINES) return;
    track::push_batch(g_ring[b->machine], b);
}

void track_pre() {
    if (g_send_dirty.load(std::memory_order_relaxed)) apply_send();
    if (!g_send_rings && !g_gone_n) return;
    g_events.clear();
    for (uint32_t i = 0; i < g_gone_n; ++i)
        g_events.push_back(event(g_gone[i].peer, g_gone[i].flags, g_gone[i].machine, KILN_REPL_TRACK_PEER_GONE, 0, 0));
    g_gone_n = 0;
    if (g_send_rings)
        for (uint32_t m = 0; m < KILN_REPL_MACHINES; ++m) {
            uint8_t flags = 0;
            const uint32_t peer = peer_of(m, &flags);
            auto& gen = g_gen[m];
            track::drain(g_ring[m], [&](uint8_t kind, uint32_t entity, uint32_t tick) {
                if (kind == KILN_REPL_TRACK_CREATED) gen[entity] = tick;
                else if (kind == KILN_REPL_TRACK_DESTROYED || kind == KILN_REPL_TRACK_LEFT) gen.erase(entity);
                g_events.push_back(event(peer, flags, static_cast<uint8_t>(m), kind, entity, tick));
            });
        }
    deliver(KILN_REPL_SEND, g_events);
    if (g_send_dirty.load(std::memory_order_relaxed)) apply_send();
}

void track_peer_changed(uint32_t machine, uint32_t old_peer) {
    if (machine >= KILN_REPL_MACHINES) return;
    if (g_send_rings) g_ring[machine].discard();  // read in this Pre already: what is left belongs to no one
    g_gen[machine].clear();
    if (!old_peer || !g_send_on.load(std::memory_order_relaxed) || g_gone_n >= std::size(g_gone)) return;
    // the old peer's flags are gone with the snapshot; a fake's handle still says what it was
    g_gone[g_gone_n++] = Gone{old_peer, static_cast<uint8_t>((old_peer & KILN_REPL_FAKE_PEER) ? KILN_REPL_PEER_FAKE : 0),
                              static_cast<uint8_t>(machine)};
}

void track_world_stopping() {
    if (!g_send_on.load(std::memory_order_relaxed)) {
        g_gone_n = 0;
        return;
    }
    // the last batch of this world: what is still unread, then every machine's peer is gone
    track_pre();
    g_events.clear();
    for (uint32_t m = 0; m < KILN_REPL_MACHINES; ++m) {
        uint8_t flags = 0;
        if (const uint32_t peer = peer_of(m, &flags))
            g_events.push_back(event(peer, flags, static_cast<uint8_t>(m), KILN_REPL_TRACK_PEER_GONE, 0, 0));
        g_gen[m].clear();
        if (g_send_rings) g_ring[m].discard();
    }
    g_gone_n = 0;
    deliver(KILN_REPL_SEND, g_events);
}

void track_owner_retiring(KilnOwner owner) {
    const uint32_t dirs = g_tracks.retire(owner);
    if (dirs & KILN_REPL_SEND) g_send_dirty.store(true, std::memory_order_relaxed);
    if (dirs & KILN_REPL_RECEIVE) g_client_dirty.store(true, std::memory_order_relaxed);
}

KilnResult track_remove(KilnHandle h) {
    const uint32_t dir = g_tracks.remove(h);
    if (!dir) return KILN_ERR_NOT_FOUND;
    if (dir == KILN_REPL_SEND) {
        if (g_api->on_tick_thread()) apply_send();
        else g_send_dirty.store(true, std::memory_order_relaxed);
    } else {
        g_client_dirty.store(true, std::memory_order_relaxed);
    }
    return KILN_OK;
}

KilnResult KILN_CALL t_track(KilnOwner owner, uint32_t direction, KilnReplTrackFn fn, void* user, KilnHandle* out) {
    if (out) *out = 0;
    if (!out || !fn || !owner || (direction != KILN_REPL_SEND && direction != KILN_REPL_RECEIVE)) return KILN_ERR_INVALID_ARGUMENT;
    if (direction == KILN_REPL_SEND) {
        if (!(track_caps() & KILN_REPL_NET_CAP_TRACK)) return KILN_ERR_UNAVAILABLE;
        if (!g_api->on_tick_thread()) return KILN_ERR_WRONG_THREAD;
        const uint64_t h = g_tracks.add(owner, KILN_REPL_SEND, fn, user, kTrackTag);
        if (!h) return KILN_ERR_INVALID_ARGUMENT;  // 16 tracks
        apply_send();
        if (!g_send_on.load(std::memory_order_relaxed)) {
            g_tracks.remove(h);
            apply_send();
            return KILN_ERR_UNAVAILABLE;
        }
        *out = h;
        return KILN_OK;
    }
    if (!(track_caps() & KILN_REPL_NET_CAP_CLIENT_TRACK)) return KILN_ERR_UNAVAILABLE;
    if (!g_api->on_world_thread(KILN_WORLD_CLIENT)) return KILN_ERR_WRONG_THREAD;
    if (!g_client_ring.ready() && !g_client_ring.init()) return KILN_ERR_UNAVAILABLE;
    const uint64_t h = g_tracks.add(owner, KILN_REPL_RECEIVE, fn, user, kTrackTag);
    if (!h) return KILN_ERR_INVALID_ARGUMENT;
    g_client_on.store(true, std::memory_order_release);
    if (!client_apply()) {  // the store hook could not go in
        g_tracks.remove(h);
        g_client_on.store(g_tracks.apply(KILN_REPL_RECEIVE) != 0, std::memory_order_release);
        client_apply();
        return KILN_ERR_UNAVAILABLE;
    }
    *out = h;
    return KILN_OK;
}

KilnResult KILN_CALL t_peer_entities(uint32_t peer, KilnReplPeerEntity* out, uint32_t cap, uint32_t* count) {
    if (count) *count = 0;
    if (!(track_caps() & KILN_REPL_NET_CAP_TRACK)) return KILN_ERR_UNAVAILABLE;
    if (!g_api->on_tick_thread()) return KILN_ERR_WRONG_THREAD;
    if (!count || (cap && !out)) return KILN_ERR_INVALID_ARGUMENT;
    const uint32_t m = machine_of_peer(peer, nullptr);
    if (m >= KILN_REPL_MACHINES) return KILN_ERR_INVALID_ARGUMENT;
    const uintptr_t ctx = client_ctx(m);
    if (!ctx) return KILN_ERR_INVALID_ARGUMENT;
    MapRead r{ctx, out, cap, 0};
    if (!read_state_map(&r)) return KILN_ERR_UNAVAILABLE;
    const uint32_t filled = r.n < cap ? r.n : cap;
    for (uint32_t i = 0; i < filled; ++i) {
        const auto it = g_gen[m].find(out[i].entity);
        out[i].create_tick = it == g_gen[m].end() ? 0 : it->second;
    }
    *count = r.n;
    return r.n > cap ? KILN_ERR_BUFFER_TOO_SMALL : KILN_OK;
}

// ---- the client side (client.cpp) ----
bool track_client_active() { return g_client_on.load(std::memory_order_acquire); }

void track_client_store(const uint8_t* msg, uint64_t size) {
    if (g_client_on.load(std::memory_order_acquire)) track::push_message(g_client_ring, msg, size);
}

void track_client_pre() {
    if (g_client_dirty.exchange(false, std::memory_order_relaxed)) {
        const bool live = g_tracks.apply(KILN_REPL_RECEIVE) != 0;
        if (live != g_client_on.load(std::memory_order_relaxed)) {
            g_client_on.store(live, std::memory_order_release);
            client_apply();
        }
        if (!live) g_client_ring.discard();
    }
    if (!g_client_on.load(std::memory_order_relaxed) || !g_client_ring.ready()) return;
    g_client_events.clear();
    track::drain(g_client_ring, [](uint8_t kind, uint32_t entity, uint32_t tick) {
        g_client_events.push_back(event(0, 0, 0, kind, entity, tick));
    });
    deliver(KILN_REPL_RECEIVE, g_client_events);
}

void track_client_world_stopping() {
    if (!g_client_on.load(std::memory_order_relaxed)) return;
    track_client_pre();
    g_client_ring.discard();
    g_client_events.clear();
    g_client_events.push_back(event(0, 0, 0, KILN_REPL_TRACK_PEER_GONE, 0, 0));
    deliver(KILN_REPL_RECEIVE, g_client_events);
}

}  // namespace repl
