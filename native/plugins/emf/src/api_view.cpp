// SPDX-License-Identifier: MIT
// The EmfApi table, v35 (EMF 0.6.5) "a minimap's parts": player_view since 0.2.0-dev.31.13; fog_at, map_markers and
// the canvases are not carried yet.
//
// player_view (EMF src/core/api_minimap.cpp): the local player's position, heading and camera yaw, and the map's size;
// false on the dedicated server, at the main menu and before the player is known. Any thread.
// On Kiln:
//  - the player: kiln.client's local player and its CurrentTransform (keen::WorldTransform: 32.32 fixed position,
//    orientation), read on the thread that may read it: the client world on the game's main thread, the server world
//    on a host's tick thread (a host's ids are the same in both worlds). Any other thread gets the copy of the latest
//    client step, kept while a mod asked within the last 10 s (the first such call answers false, as EMF's own first
//    moments do); a copy of another world or player does not count.
//  - heading: the orientation's forward (0, 0, 1) as degrees, 0 north (+z), 90 east (+x) (EMF's heading_from_quat).
//  - camera_yaw: kiln.render's camera (the view the game renders with, any thread), the same way; EMF_VIEW_CAMERA only
//    while that camera is valid (a frame in the last 500 ms).
//  - world_size: 10240 m (Embervale's map), what EMF answers until its fog-of-war grid is read; Kiln reads no fog.
//
// Not carried yet, each answering EMF's documented failure (docs/kiln/emf-parity.md, v35):
//  - fog_at: EMF copies the client world's FogOfWar singleton grid twice a second; Kiln has no service that reads the
//    fog of war and its layout is not qualified on this build.
//  - map_markers: EMF captures the game's own map list with its hooks; no Kiln service exposes that list.
//  - canvas_set / canvas_draw: a drawing panel over kiln.render (corners, clipping, circles, images) is a piece of its
//    own, and three of its commands (FOG, MARKERS, MAP) need the fog, the v33 markers and the game's map picture.
#include <intrin.h>

#include <atomic>

#include "buffs_model.h"
#include "emf_internal.h"
#include "services/kiln_render.h"

namespace kemf {

void view_client_step(EmfWorld* client);

namespace {

constexpr const char* kFogWhy = "not available on Kiln yet: no Kiln service reads the fog of war";
constexpr const char* kMarkersWhy = "not available on Kiln yet: no Kiln service lists the game's own map markers";
constexpr const char* kCanvasWhy = "not available on Kiln yet: canvases are not drawn on Kiln";
constexpr uint64_t kKeepMs = 10000;

const KilnRenderApi* render() {
    static const KilnRenderApi* const r = []() -> const KilnRenderApi* {
        auto* s = static_cast<const KilnRenderApi*>(g_api->service(kstr(KILN_RENDER_SERVICE), 1));
        return s && s->header.size >= offsetof(KilnRenderApi, camera) + sizeof(void*) && s->camera ? s : nullptr;
    }();
    return r;
}

struct Snap {
    bool ok = false;
    double pos[3] = {};
    float q[4] = {};
    uint32_t generation = 0;
    KilnEntity player = 0;
};
std::mutex g_mutex;
Snap g_snap;
std::atomic<uint64_t> g_asked_ms{0};

bool client_state(KilnClientState* st) {
    *st = KilnClientState{};
    st->size = sizeof *st;
    return S.client && S.client->state && S.client->state(st) == KILN_OK;
}

// The entity's CurrentTransform in `w` (on that world's thread).
bool read_transform(EmfWorld* w, KilnEntity e, Snap* s) {
    if (!w || !e) return false;
    uint64_t size = 0;
    const void* p = component_of(w, fnv1a("keen::ecs::CurrentTransform"), e, &size);
    uint8_t t[40];
    if (!p || size < sizeof t || !seh_copy(t, p, sizeof t)) return false;
    int64_t fixed[3];
    std::memcpy(fixed, t, sizeof fixed);
    for (int i = 0; i < 3; ++i) s->pos[i] = view::metres(fixed[i]);
    std::memcpy(s->q, t + 24, sizeof s->q);
    s->player = e;
    s->ok = true;
    return true;
}

bool read_now(const KilnClientState& st, Snap* s) {
    if (S.client && S.client->on_client_thread && S.client->on_client_thread())
        return read_transform(reinterpret_cast<EmfWorld*>(g_api->world(KILN_WORLD_CLIENT)), st.local_player, s);
    if (st.session == KILN_SESSION_HOST && g_api->on_tick_thread())
        return read_transform(reinterpret_cast<EmfWorld*>(g_api->world(KILN_WORLD_SERVER)), st.local_player, s);
    return false;
}

bool api_player_view(EmfPlayerView* out) {
    note_call("player_view");
    if (!out || g_side == KILN_SIDE_SERVER) return false;  // the dedicated server has no player of its own
    g_asked_ms.store(now_ms(), std::memory_order_relaxed);
    KilnClientState st;
    if (!client_state(&st) || !st.local_player) return false;  // the main menu, or before the player is known
    Snap s;
    if (!read_now(st, &s)) {
        std::lock_guard lock(g_mutex);
        s = g_snap;
        if (!s.ok || s.generation != st.generation || s.player != st.local_player) return false;
    }
    EmfPlayerView v{};
    v.size = sizeof v;
    v.x = s.pos[0];
    v.y = s.pos[1];
    v.z = s.pos[2];
    v.heading_deg = view::heading_from_quat(s.q[0], s.q[1], s.q[2], s.q[3]);
    if (const KilnRenderApi* r = render()) {
        KilnRenderCamera cam{};
        cam.size = sizeof cam;
        if (r->camera(&cam) == KILN_OK && cam.valid) {
            v.camera_yaw_deg = view::heading_from_quat(cam.rotation[0], cam.rotation[1], cam.rotation[2], cam.rotation[3]);
            v.flags |= EMF_VIEW_CAMERA;
        }
    }
    v.world_size = view::kDefaultWorldSize;
    view::copy_view(v, out);
    return true;
}

bool api_fog_at(double, double, float*) {
    unsupported(caller_mod(_ReturnAddress()), "fog_at", kFogWhy);
    return false;
}

uint32_t api_map_markers(EmfGameMarker*, uint32_t) {
    unsupported(caller_mod(_ReturnAddress()), "map_markers", kMarkersWhy);
    return 0;
}

bool api_canvas_set(const char*, const EmfCanvas*) {
    unsupported(caller_mod(_ReturnAddress()), "canvas_set", kCanvasWhy);
    return false;
}

bool api_canvas_draw(const char*, const EmfCanvasCmd*, uint32_t) {
    unsupported(caller_mod(_ReturnAddress()), "canvas_draw", kCanvasWhy);
    return false;  // an unknown canvas
}

}  // namespace

// After each client step, on the main thread: the copy for other threads, while a mod asks.
void view_client_step(EmfWorld* client) {
    if (g_side == KILN_SIDE_SERVER) return;
    const uint64_t asked = g_asked_ms.load(std::memory_order_relaxed);
    if (!asked || now_ms() - asked > kKeepMs) return;
    KilnClientState st;
    Snap s;
    const bool ok = client_state(&st) && st.local_player && read_transform(client, st.local_player, &s);
    s.generation = st.generation;
    std::lock_guard lock(g_mutex);
    g_snap = ok ? s : Snap{};
}

void fill_view(EmfApi* t) {
    t->player_view = &api_player_view;
    t->fog_at = &api_fog_at;
    t->map_markers = &api_map_markers;
    t->canvas_set = &api_canvas_set;
    t->canvas_draw = &api_canvas_draw;
}

}  // namespace kemf
