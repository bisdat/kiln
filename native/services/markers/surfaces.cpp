// SPDX-License-Identifier: MIT
// kiln.markers' surfaces (chapter 04 §4.5, §4.6; plan S5, S6): at each client Post the client's rows (local and received)
// are admitted per surface (select.h), their visibility events are made (visibility.h) and the admitted map rows are
// appended to the game's map list (map_list.h). Rows: markers.map (here), markers.compass, markers.world and
// markers.nameplates (the drawn surfaces).
//
// Threads: everything here runs on the client world's thread (its Post, its lifecycle), except the frame callback of the
// drawn surfaces, which only reads an immutable snapshot.
#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "services/kiln_render.h"
#include "services/kiln_ui.h"

#include "glue.h"
#include "map_list.h"
#include "select.h"
#include "visibility.h"

namespace kmk {

namespace {

constexpr char kMap[] = "markers.map";
constexpr char kCompass[] = "markers.compass";
constexpr char kWorld[] = "markers.world";
constexpr char kNameplates[] = "markers.nameplates";

KilnStr str(const char* s) { return KilnStr{s, std::strlen(s)}; }

// ---- the native map ---------------------------------------------------------------------------------------------------

MapLayout g_layout;
bool g_map_guards = false;        // markers.client_map resolved and its layout is the measured one
std::string g_map_static_off;     // why not (the row's text)
uint32_t g_map_static_reason = KILN_CAP_REASON_SYMBOL_MISSING;
bool g_map_latched = false;       // a guard failed at run time: off until the game restarts
std::string g_map_latch_reason;
int g_map_reported = -1;          // the row's last reported state (KILN_CAP_*), -1 none
std::string g_map_reported_text;
uint64_t g_ui_epoch = 0;          // the client world epoch g_ui belongs to
uintptr_t g_ui = 0;               // that world's G38UiPlayData
Appended g_appended;

bool constant(const char* name, int64_t* out) { return g.api->constant && g.api->constant(str(name), out) == KILN_OK; }

void map_setup() {
    if (!g.api->feature || !g.api->feature(str("markers.client_map"))) {
        g_map_static_off = "the native map list's guards did not resolve on this game build (markers.client_map)";
        return;
    }
    if (!g.api->feature(str("singletons")) || !g.api->singleton) {
        g_map_static_off = "the client world's UI data cannot be found on this build (feature singletons)";
        return;
    }
    int64_t count = 0, cap_off = 0, cap = 0, size = 0, pos = 0, z = 0, type = 0, vis = 0;
    if (!constant("markers/main.count", &count) || !constant("markers/main.capacity_offset", &cap_off) || !constant("markers/list.capacity", &cap) ||
        !constant("markers/entry.size", &size) || !constant("markers/entry.position", &pos) || !constant("markers/entry.z", &z) ||
        !constant("markers/entry.type", &type) || !constant("markers/entry.visibility", &vis)) {
        g_map_static_off = "a constant of the native map list is missing (markers.client_map)";
        return;
    }
    g_layout.header = count - 8;
    g_layout.capacity = static_cast<uint64_t>(cap);
    g_layout.entry_size = static_cast<uint32_t>(size);
    g_layout.position = static_cast<uint32_t>(pos);
    g_layout.z = static_cast<uint32_t>(z);
    g_layout.type = static_cast<uint32_t>(type);
    g_layout.visibility = static_cast<uint32_t>(vis);
    // the measured header: data, count, capacity in a row; the entry as chapter 04 §2.3 traced it
    if (cap_off != count + 8 || !g_layout.valid() || z != pos + 16) {
        g_map_static_off = "the native map list's layout differs from the measured one (markers.client_map)";
        g_map_static_reason = KILN_CAP_REASON_FAULTED;
        return;
    }
    g_map_guards = true;
}

// markers.map now: the guards, the run-time latch and the icon catalog (warming: available, Set answers
// CatalogNotReady; published and verified for this build: available; otherwise off with the catalog's reason).
void update_map_row() {
    int state = KILN_CAP_AVAILABLE;
    uint32_t reason = KILN_CAP_REASON_NONE;
    std::string text;
    if (!g_map_guards) {
        state = KILN_CAP_UNAVAILABLE, reason = g_map_static_reason, text = g_map_static_off;
    } else if (g_map_latched) {
        state = KILN_CAP_UNAVAILABLE, reason = KILN_CAP_REASON_FAULTED, text = g_map_latch_reason;
    } else if (g.catalog->get() && (!g.catalog->map_verified() || g.catalog->changed())) {
        state = KILN_CAP_UNAVAILABLE, reason = KILN_CAP_REASON_POLICY, text = g.catalog->map_reason();
    }
    g.sw->map.store(state == KILN_CAP_AVAILABLE);
    if (state == g_map_reported && text == g_map_reported_text) return;
    g_map_reported = state;
    g_map_reported_text = text;
    report(kMap, static_cast<uint32_t>(state), reason, text.c_str());
}

void latch_map(const char* why) {
    if (g_map_latched) return;
    g_map_latched = true;
    g_map_latch_reason = why;
    logf(KILN_LOG_WARN, "markers: %s: native map icons are off until the game restarts", why);
    update_map_row();
}

struct AppendCall {
    uint8_t* ui;
    const MapRow* rows;
    uint32_t n;
    uint32_t written;
    ListState state;
};

// The game's memory under a fault guard (no C++ object with a destructor in this frame).
bool guarded_read(uint8_t* ui, uint64_t* count, uint64_t* room, ListState* state) {
    __try {
        *state = read_list(ui, g_layout, count, room);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool guarded_append(AppendCall* c) {
    __try {
        c->written = append_rows(c->ui, g_layout, c->rows, c->n, &g_appended, &c->state);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

uint8_t* ui_data(uint64_t epoch) {
    if (g_ui_epoch != epoch || !g_ui) {
        g_ui_epoch = epoch;
        g_appended = Appended{};
        const uintptr_t w = g.api->world ? g.api->world(KILN_WORLD_CLIENT) : 0;
        g_ui = w ? g.api->singleton(w, str("G38UiPlayData")) : 0;
    }
    return reinterpret_cast<uint8_t*>(g_ui);
}

// ---- the camera and the viewport (kiln.render, kiln.ui) ---------------------------------------------------------------

const KilnRenderApi* g_render = nullptr;
const KilnUiApi* g_uiapi = nullptr;
bool g_ui_status = false;  // kiln.ui's status() is in its table

void read_view(View* v) {
    if (g_render) {
        KilnRenderCamera c{};
        c.size = sizeof c;
        double fwd[3];
        if (g_render->camera(&c) == KILN_OK && c.valid && forward_of(c.rotation, fwd) && std::isfinite(c.position[0]) &&
            std::isfinite(c.position[1]) && std::isfinite(c.position[2])) {
            v->camera = true;
            for (int i = 0; i < 3; ++i) v->cam[i] = c.position[i], v->fwd[i] = fwd[i];
            v->near_plane = c.near_plane > 0 && std::isfinite(c.near_plane) ? c.near_plane : 0.1;
        }
    }
    if (g_uiapi && g_ui_status) {
        KilnUiStatus s{};
        s.size = sizeof s;
        if (g_uiapi->status(&s) == KILN_OK) v->viewport = s.width > 0 && s.height > 0;
    }
}

// ---- admission and visibility -----------------------------------------------------------------------------------------

Visibility g_vis;
std::vector<ClientRow> g_rows;
std::vector<Candidate> g_cand;
std::vector<Admitted> g_adm;
std::vector<MapRow> g_maprows;

void candidates(const std::vector<ClientRow>& rows, std::vector<Candidate>* out) {
    out->resize(rows.size());
    for (size_t i = 0; i < rows.size(); ++i) {
        const ClientRow& r = rows[i];
        Candidate& c = (*out)[i];
        c = Candidate{};
        c.requested = r.rec.surfaces;
        c.source_rank = r.source == KILN_MARKER_LOCAL ? 0 : 1;
        c.priority = r.rec.priority;
        c.ns = r.ns;
        c.key = r.key;
        c.publisher_generation = r.publisher_generation;
        c.height = r.rec.height;
        c.max_distance = r.rec.max_distance;
        c.x = r.rec.x, c.y = r.rec.y, c.z = r.rec.z;
        c.source_state = r.source_reason;
        if (r.source == KILN_MARKER_LOCAL && r.rec.follow) {
            KilnVec3 p{};
            uint32_t why = KILN_MARKER_ANCHOR_ENDED;
            if (anchors_follow(r.rec.follow, &p, &why)) {
                c.source_state = why;
                if (why == KILN_MARKER_READY) c.x = p.x, c.y = p.y, c.z = p.z;
            } else {
                c.source_state = KILN_MARKER_ANCHOR_ENDED;
            }
        }
    }
}

void post_map(uint64_t epoch, const std::vector<uint32_t>& order) {
    if (!g.sw->map.load() || (order.empty() && !g_appended.n)) return;
    uint8_t* ui = ui_data(epoch);
    if (!ui) return;
    g_maprows.clear();
    for (uint32_t i : order) g_maprows.push_back(MapRow{g_cand[i].x, g_cand[i].y, g_cand[i].z, g_rows[i].rec.icon});
    AppendCall c{ui, g_maprows.data(), static_cast<uint32_t>(g_maprows.size()), 0, ListState::Ok};
    if (!guarded_append(&c)) return latch_map("writing the native map list faulted");
    if (c.state != ListState::Ok) return latch_map("the native map list's header failed its guards (data pointer, capacity or count)");
}

}  // namespace

void surfaces_ready() {
    if (g.side != KILN_SIDE_CLIENT) {
        static const char kSide[] = "drawn and native map surfaces are a game client's; a server sends markers (markers.send)";
        for (const char* name : {kMap, kCompass, kWorld, kNameplates}) report(name, KILN_CAP_UNAVAILABLE, KILN_CAP_REASON_WRONG_SIDE, kSide);
        return;
    }
    g_render = static_cast<const KilnRenderApi*>(g.api->service(str(KILN_RENDER_SERVICE), KILN_RENDER_VERSION));
    if (g_render && g_render->header.size < sizeof(KilnRenderApi)) g_render = nullptr;
    g_uiapi = static_cast<const KilnUiApi*>(g.api->service(str(KILN_UI_SERVICE), KILN_UI_VERSION));
    g_ui_status = g_uiapi && g_uiapi->header.size >= offsetof(KilnUiApi, status) + sizeof(g_uiapi->status) && g_uiapi->status;
    map_setup();
    update_map_row();
    static const char kNotBuilt[] = "not built yet in this kiln.markers";
    for (const char* name : {kCompass, kWorld, kNameplates}) report(name, KILN_CAP_UNAVAILABLE, KILN_CAP_REASON_POLICY, kNotBuilt);
}

void surfaces_world(const KilnWorldEvent& e, std::vector<Note>* notes) {
    if (g.side != KILN_SIDE_CLIENT || e.kind != KILN_WORLD_CLIENT) return;
    if (e.event == KILN_WORLD_STOPPING) {
        uint64_t epoch = 0;
        g.store->world_epoch(KILN_WORLD_CLIENT, &epoch);
        g_vis.clear(epoch, notes);
        g_appended = Appended{};
        g_ui = 0;
        g_ui_epoch = 0;
    }
}

void surfaces_client_post(std::vector<Note>* notes) {
    if (g.side != KILN_SIDE_CLIENT) return;
    uint64_t epoch = 0;
    g.store->world_epoch(KILN_WORLD_CLIENT, &epoch);
    update_map_row();
    g.store->snapshot_client(&g_rows);
    if (g_rows.empty() && !g_vis.size() && !g_appended.n) return;
    View v;
    v.world_active = epoch != 0;
    v.cap[kSurfMap] = g.sw->map.load();
    v.cap[kSurfCompass] = g.sw->compass.load();
    v.cap[kSurfWorld] = g.sw->world.load();
    read_view(&v);
    if (v.cap[kSurfMap] && epoch) {
        if (uint8_t* ui = ui_data(epoch)) {
            uint64_t count = 0, room = 0;
            ListState st = ListState::Ok;
            if (!guarded_read(ui, &count, &room, &st)) {
                latch_map("reading the native map list faulted");
            } else if (st != ListState::Ok) {
                latch_map("the native map list's header failed its guards (data pointer, capacity or count)");
            } else {
                // our own rows of the last step, when the game did not clear them, are room again
                if (g_appended.n && count == g_appended.base + g_appended.n) room += g_appended.n;
                v.map_room = static_cast<uint32_t>(room);
            }
            v.cap[kSurfMap] = g.sw->map.load();
        }
    }
    candidates(g_rows, &g_cand);
    std::vector<uint32_t> order[3];
    admit(v, g_cand, &g_adm, order);
    g_vis.step(epoch, g_rows, g_adm, notes);
    post_map(epoch, order[kSurfMap]);
}

}  // namespace kmk
