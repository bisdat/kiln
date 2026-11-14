// SPDX-License-Identifier: MIT
// kiln.markers (services/kiln_markers.h): session-only markers on the game's map, Kiln's compass strip and world labels,
// and the internal kiln.markers.bridge the managed built-in receiver uses. Dossier chapter 04; docs/kiln/services/markers.md.
//
// This file is the glue to the core: it publishes both tables and the capability rows, feeds the store (store.h) the world
// lifecycle, and delivers the queued notifications at each client step (and on the stopping thread when a world stops).
// The parts not built yet read unavailable (policy, "not built yet") and their members answer KILN_ERR_UNAVAILABLE naming
// the row.
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

#include "kiln_api.h"
#include "services/kiln_assets.h"
#include "services/kiln_caps.h"
#include "services/kiln_markers.h"

#include "arena.h"
#include "catalog.h"
#include "events.h"
#include "store.h"

namespace {

const KilnApi* g_api = nullptr;
KilnOwner g_owner = 0;
const KilnCapsApi* g_caps = nullptr;

#define KM_STR(s) KilnStr{s, sizeof(s) - 1}

void logf(int32_t level, const char* fmt, ...) {
    char text[768];
    va_list v;
    va_start(v, fmt);
    const int n = std::vsnprintf(text, sizeof text, fmt, v);
    va_end(v);
    if (n > 0) g_api->log(g_owner, level, KilnStr{text, static_cast<size_t>(n < static_cast<int>(sizeof text) ? n : static_cast<int>(sizeof text) - 1)});
}

void log_line(int level, const char* text) {
    if (g_api) g_api->log(g_owner, level, KilnStr{text, std::strlen(text)});
}

kmk::Arena g_arena;
kmk::Switches g_switches;
kmk::Store g_store(g_arena, g_switches);
kmk::Subscribers g_subs(&log_line);
kmk::CatalogHolder g_catalog;
const KilnAssetsApi* g_assets = nullptr;
std::string g_exe;  // this process's exe sha256 (lower-case hex)

uint32_t g_side = KILN_SIDE_SERVER;
bool g_lifecycle = false;    // world STARTED and STOPPING are seen
bool g_client_post = false;  // the client world's Post is ours

// ---- capability rows (chapter 04 §5) ------------------------------------------------------------------------------------

constexpr char kLocal[] = "markers.local";
constexpr char kCatalog[] = "markers.catalog";
constexpr char kMap[] = "markers.map";
constexpr char kCompass[] = "markers.compass";
constexpr char kWorld[] = "markers.world";
constexpr char kNameplates[] = "markers.nameplates";
constexpr char kAnchorsClient[] = "markers.anchors.client";
constexpr char kAnchorsServer[] = "markers.anchors.server";
constexpr char kSend[] = "markers.send";
constexpr char kMapText[] = "markers.map_text";

void report(const char* name, uint32_t state, uint32_t reason, const char* text) {
    if (!g_caps || !g_caps->report) return;
    g_caps->report(g_owner, KilnStr{name, std::strlen(name)}, state, reason, KilnStr{text, std::strlen(text)});
}

// markers.catalog: a client reads the game's resources through kiln.assets; a dedicated server has none and takes the
// catalog from a verified client over the marker link (not built yet).
std::atomic<bool> g_catalog_reader{false};
const char* g_catalog_off = "";
uint32_t g_catalog_off_reason = KILN_CAP_REASON_POLICY;

void report_rows() {
    static const char kNotBuilt[] = "not built yet in this kiln.markers (0.2.0-dev.28 in work)";
    for (const char* name : {kMap, kCompass, kWorld, kNameplates, kAnchorsClient, kAnchorsServer, kSend})
        report(name, KILN_CAP_UNAVAILABLE, KILN_CAP_REASON_POLICY, kNotBuilt);
    if (g_catalog_reader)
        report(kCatalog, KILN_CAP_AVAILABLE, KILN_CAP_REASON_NONE, "");
    else
        report(kCatalog, KILN_CAP_UNAVAILABLE, g_catalog_off_reason, g_catalog_off);
    report(kMapText, KILN_CAP_UNAVAILABLE, KILN_CAP_REASON_POLICY,
           "the native map tooltip is not part of v1: its game test (04-map-text) has not been answered");
    if (g_side != KILN_SIDE_CLIENT)
        report(kLocal, KILN_CAP_UNAVAILABLE, KILN_CAP_REASON_WRONG_SIDE, "local markers are a game client's; a server sends them (markers.send)");
    else if (!g_lifecycle)
        report(kLocal, KILN_CAP_UNAVAILABLE, KILN_CAP_REASON_SYMBOL_MISSING, "needs world.lifecycle (a client's markers end with its world)");
    else if (!g_client_post)
        report(kLocal, KILN_CAP_UNAVAILABLE, KILN_CAP_REASON_FAULTED, "the client world's step could not be subscribed");
    else
        report(kLocal, KILN_CAP_AVAILABLE, KILN_CAP_REASON_NONE, "");
}

// ---- results ----------------------------------------------------------------------------------------------------------

bool result_ok(const KilnMarkerResult* r) { return r && r->size >= sizeof(KilnMarkerResult); }

void clear_result(KilnMarkerResult* r) {
    const uint32_t size = r->size;
    std::memset(r, 0, sizeof(KilnMarkerResult));
    r->size = size;
}

// KILN_ERR_UNAVAILABLE with the row's name as the reason (header: "Results")
KilnResult off(KilnMarkerResult* r, const char* capability) {
    if (result_ok(r)) {
        clear_result(r);
        std::snprintf(r->reason, sizeof r->reason, "%s", capability);
    }
    return KILN_ERR_UNAVAILABLE;
}

// ---- delivery -----------------------------------------------------------------------------------------------------------

void dispatch(uint32_t role) {
    g_subs.dispatch([role] { return g_store.take(role); });
}

void KILN_CALL on_world(const KilnWorldEvent* e, void*) {
    if (!e || (e->kind != KILN_WORLD_SERVER && e->kind != KILN_WORLD_CLIENT)) return;
    const uint32_t role = e->kind == KILN_WORLD_CLIENT ? kmk::kRoleClient : kmk::kRoleServer;
    if (e->event == KILN_WORLD_STARTED) {
        g_store.world_started(e->kind);
    } else if (e->event == KILN_WORLD_STOPPING) {
        // records end with their world; their last notes go out before the stop returns, on this (stopping) thread
        g_store.world_stopping(e->kind);
        dispatch(role);
    }
}

// ---- the icon catalog (client) ------------------------------------------------------------------------------------------

bool read_asset(const uint8_t guid[16], uint32_t type, size_t limit, std::vector<uint8_t>* out, void*) {
    KilnAssetKey key{};
    std::memcpy(key.guid, guid, 16);
    key.type = type;
    key.part = 0;
    size_t len = 0;
    KilnResult rc = g_assets->resource_read(&key, nullptr, 0, &len);
    if (rc != KILN_ERR_BUFFER_TOO_SMALL && rc != KILN_OK) return false;
    if (len > limit) return false;
    out->resize(len);
    if (!len) return true;
    rc = g_assets->resource_read(&key, out->data(), out->size(), &len);
    return rc == KILN_OK && len == out->size();
}

void post_catalog(uint32_t ready, uint32_t count, uint64_t generation, const std::string& reason) {
    KilnMarkerCatalogChange c{};
    c.size = sizeof c;
    c.ready = ready;
    c.count = count;
    c.generation = generation;
    std::snprintf(c.reason, sizeof c.reason, "%s", reason.c_str());
    g_store.post_catalog(kmk::kRoleClient, c);
}

// when_resources_ready: on the game's thread, once its resource service is up (before the game read anything).
void KILN_CALL on_resources_ready(void*) {
    std::string why;
    auto c = kmk::read_catalog(&read_asset, nullptr, &why);
    if (!c) {
        logf(KILN_LOG_WARN, "markers: the icon catalog could not be read: %s; markers.catalog is off", why.c_str());
        g_catalog_reader = false;
        g_catalog_off_reason = KILN_CAP_REASON_FAULTED;
        report(kCatalog, KILN_CAP_UNAVAILABLE, KILN_CAP_REASON_FAULTED, ("the game's marker types could not be read: " + why).c_str());
        post_catalog(0, 0, 0, why);
        return;
    }
    const kmk::BuildPair* pair = kmk::pair_of_client(g_exe);
    const bool verified = pair && c->digest_hex() == pair->catalog_sha256;
    std::string reason;
    if (!verified)
        reason = pair ? "native map icons stay off: the game's marker types differ from the verified catalog of this build"
                      : "native map icons stay off: no verified catalog for this game build (a game update needs a new build pair)";
    const uint32_t count = static_cast<uint32_t>(c->rows.size()), map = c->map_rows();
    const std::string digest = c->digest_hex();
    g_catalog.publish(std::move(c), verified, reason);
    logf(KILN_LOG_INFO, "markers: icon catalog: %u marker types with an icon, %u for the map, sha256 %s (%s)", count, map, digest.c_str(),
         verified ? "the verified pair of this build" : reason.c_str());
    post_catalog(1, count, g_catalog.generation(), reason);
}

// Once a second the client's marker metadata is read again: when it differs from the published catalog (another mod
// replaced the registry, a hot reload), native map icons stay off until the game restarts (chapter 04 §4.3).
std::chrono::steady_clock::time_point g_next_check{};

void recheck_catalog() {
    const auto now = std::chrono::steady_clock::now();
    if (now < g_next_check) return;
    g_next_check = now + std::chrono::seconds(1);
    const auto c = g_catalog.get();
    if (!c || g_catalog.changed()) return;
    std::string why;
    const auto fresh = kmk::read_catalog(&read_asset, nullptr, &why);
    if (fresh && fresh->digest == c->digest) return;
    const std::string reason = fresh ? "native map icons are off until the game restarts: the game's marker types changed (sha256 " + fresh->digest_hex() + ")"
                                     : "native map icons are off until the game restarts: the game's marker types can no longer be read (" + why + ")";
    g_catalog.latch_changed(reason);
    logf(KILN_LOG_WARN, "markers: %s", reason.c_str());
}

void KILN_CALL on_client_post(const KilnTickInfo*, void*) {
    recheck_catalog();
    dispatch(kmk::kRoleClient);
}

// ---- kiln.markers v1 ----------------------------------------------------------------------------------------------------

KilnResult KILN_CALL t_bind_owner(KilnOwner owner, KilnStr ns) { return g_store.bind(owner, ns); }
KilnResult KILN_CALL t_release_owner(KilnOwner owner) {
    if (!owner) return KILN_ERR_INVALID_ARGUMENT;
    g_subs.remove_owner(owner);  // its callbacks first: what follows is for the others
    return g_store.release(owner);
}
KilnResult KILN_CALL t_catalog_count(uint32_t* count, uint64_t* generation) {
    if (!count || !generation) return KILN_ERR_INVALID_ARGUMENT;
    *count = 0;
    *generation = 0;
    if (!g_catalog_reader) return KILN_ERR_UNAVAILABLE;
    if (const auto c = g_catalog.get()) {
        *count = static_cast<uint32_t>(c->rows.size());
        *generation = g_catalog.generation();
    }
    return KILN_OK;
}
KilnResult KILN_CALL t_catalog_at(uint32_t index, KilnMarkerIcon* out) {
    if (!out || out->size < sizeof(KilnMarkerIcon)) return KILN_ERR_INVALID_ARGUMENT;
    if (!g_catalog_reader) return KILN_ERR_UNAVAILABLE;
    const auto c = g_catalog.get();
    if (!c || index >= c->rows.size()) return KILN_ERR_NOT_FOUND;
    kmk::fill_icon(c->rows[index], out);
    return KILN_OK;
}
KilnResult KILN_CALL t_catalog_resolve(KilnStr name, KilnMarkerIcon* out, KilnMarkerResult* r) {
    if (!result_ok(r) || !out || out->size < sizeof(KilnMarkerIcon)) return KILN_ERR_INVALID_ARGUMENT;
    clear_result(r);
    if (!g_catalog_reader) return off(r, kCatalog);
    if (!name.ptr && name.len) return KILN_ERR_INVALID_ARGUMENT;
    const std::string_view n = name.len ? std::string_view(name.ptr, name.len) : std::string_view();
    const auto c = g_catalog.get();
    const kmk::IconRow* row = nullptr;
    uint32_t matches = 0;
    if (!c) {
        // the syntax is checked first, also while warming up
        kmk::Catalog empty;
        if (kmk::resolve(empty, n, &row, &matches) == kmk::Resolved::BadSyntax) {
            std::snprintf(r->reason, sizeof r->reason, "an icon name is 1 to 128 printable ASCII characters: a canonical key (type: and eight hex digits) or an alias");
            return KILN_ERR_INVALID_ARGUMENT;
        }
        r->status = KILN_MARKER_CATALOG_NOT_READY;
        std::snprintf(r->reason, sizeof r->reason, "CatalogNotReady: the icon catalog is still warming up");
        return KILN_OK;
    }
    switch (kmk::resolve(*c, n, &row, &matches)) {
        case kmk::Resolved::Ok:
            kmk::fill_icon(*row, out);
            r->status = KILN_MARKER_ACCEPTED;
            return KILN_OK;
        case kmk::Resolved::NotFound:
            r->status = KILN_MARKER_NOT_FOUND;
            std::snprintf(r->reason, sizeof r->reason, "NotFound: no icon named '%.*s'", static_cast<int>(n.size()), n.data());
            return KILN_OK;
        case kmk::Resolved::Ambiguous:
            r->status = KILN_MARKER_AMBIGUOUS;
            std::snprintf(r->reason, sizeof r->reason, "Ambiguous: '%.*s' is the alias of %u icons; use a canonical key (type:...)",
                          static_cast<int>(n.size()), n.data(), matches);
            return KILN_OK;
        case kmk::Resolved::BadSyntax: break;
    }
    std::snprintf(r->reason, sizeof r->reason, "an icon name is 1 to 128 printable ASCII characters: a canonical key (type: and eight hex digits) or an alias");
    return KILN_ERR_INVALID_ARGUMENT;
}
KilnResult KILN_CALL t_world_epoch(uint32_t kind, uint64_t* epoch) { return g_store.world_epoch(kind, epoch); }
KilnResult KILN_CALL t_capture_anchor(KilnOwner, uint32_t, uint64_t, KilnEntity, KilnMarkerAnchorResult*) { return KILN_ERR_UNAVAILABLE; }
KilnResult KILN_CALL t_release_anchor(KilnOwner, KilnMarkerAnchor) { return KILN_ERR_UNAVAILABLE; }
KilnResult KILN_CALL t_set_local(KilnOwner owner, const KilnMarkerSpec* spec, KilnMarkerResult* r) { return g_store.set_local(owner, spec, r); }
KilnResult KILN_CALL t_remove_local(KilnOwner owner, KilnStr key, KilnMarkerResult* r) { return g_store.remove_local(owner, key, r); }
KilnResult KILN_CALL t_clear_local(KilnOwner owner, KilnMarkerResult* r) { return g_store.clear_local(owner, r); }
KilnResult KILN_CALL t_send(KilnOwner, KilnEntity, const KilnMarkerSpec*, KilnMarkerResult* r) { return off(r, kSend); }
KilnResult KILN_CALL t_unsend(KilnOwner, KilnEntity, KilnStr, KilnMarkerResult* r) { return off(r, kSend); }
KilnResult KILN_CALL t_clear_sent(KilnOwner, KilnEntity, KilnMarkerResult* r) { return off(r, kSend); }
KilnResult KILN_CALL t_suppress_sent(KilnOwner, KilnEntity, KilnStr, KilnMarkerResult* r) { return off(r, kSend); }
KilnResult KILN_CALL t_subscribe(KilnOwner owner, KilnMarkerChangedFn changed, KilnMarkerVisibilityFn visibility, KilnMarkerCatalogFn catalog,
                                 void* user, KilnHandle* out) {
    return g_subs.subscribe(owner, changed, visibility, catalog, user, out);
}
KilnResult KILN_CALL t_unsubscribe(KilnHandle handle) { return g_subs.unsubscribe(handle); }

const KilnMarkersApi kTable = {{sizeof(KilnMarkersApi), KILN_MARKERS_VERSION},
                               &t_bind_owner, &t_release_owner, &t_catalog_count, &t_catalog_at, &t_catalog_resolve,
                               &t_world_epoch, &t_capture_anchor, &t_release_anchor, &t_set_local, &t_remove_local,
                               &t_clear_local, &t_send, &t_unsend, &t_clear_sent, &t_suppress_sent, &t_subscribe,
                               &t_unsubscribe};

// ---- kiln.markers.bridge v1 (internal; the wire and the receiver land with the managed bridge) -------------------------

KilnResult KILN_CALL b_attach(KilnOwner) { return KILN_ERR_UNAVAILABLE; }
KilnResult KILN_CALL b_peer_up(const KilnMarkerPeer*) { return KILN_ERR_UNAVAILABLE; }
KilnResult KILN_CALL b_peer_down(uint32_t, uint32_t, uint64_t) { return KILN_ERR_UNAVAILABLE; }
KilnResult KILN_CALL b_receive(const KilnMarkerEnvelope*) { return KILN_ERR_UNAVAILABLE; }
KilnResult KILN_CALL b_pump(uint32_t, uint32_t, KilnMarkerSendFn, void*, uint32_t* processed) {
    if (processed) *processed = 0;
    return KILN_ERR_UNAVAILABLE;
}

const KilnMarkersBridgeApi kBridge = {{sizeof(KilnMarkersBridgeApi), KILN_MARKERS_BRIDGE_VERSION},
                                      &b_attach, &b_peer_up, &b_peer_down, &b_receive, &b_pump};

void KILN_CALL on_ready(void*) {
    g_caps = static_cast<const KilnCapsApi*>(g_api->service(KM_STR(KILN_CAPS_SERVICE), KILN_CAPS_VERSION));
    if (g_caps && g_caps->header.size < sizeof(KilnCapsApi)) g_caps = nullptr;
    g_store.set_icon_check(&kmk::CatalogHolder::check, &g_catalog);
    if (g_side != KILN_SIDE_CLIENT) {
        g_catalog_off = "a dedicated server has no marker resources: it takes the catalog from a verified client over the marker link (not built yet)";
        g_catalog_off_reason = KILN_CAP_REASON_POLICY;
    } else {
        // kiln.assets v1 with resource_read and the appended when_resources_ready (header.size covers it)
        g_assets = static_cast<const KilnAssetsApi*>(g_api->service(KM_STR(KILN_ASSETS_SERVICE), KILN_ASSETS_VERSION));
        constexpr size_t need = offsetof(KilnAssetsApi, when_resources_ready) + sizeof(g_assets->when_resources_ready);
        if (!g_assets || g_assets->header.size < need || !g_assets->resource_read || !g_assets->when_resources_ready) {
            g_assets = nullptr;
            g_catalog_off = "needs kiln.assets (resource_read and when_resources_ready)";
            g_catalog_off_reason = KILN_CAP_REASON_SERVICE_MISSING;
        } else {
            g_catalog_reader = true;
        }
    }
    report_rows();
    if (g_catalog_reader && g_assets->when_resources_ready(&on_resources_ready, nullptr) != KILN_OK) {
        g_catalog_reader = false;
        logf(KILN_LOG_WARN, "markers: kiln.assets' resources are off on this build: markers.catalog is off");
        report(kCatalog, KILN_CAP_UNAVAILABLE, KILN_CAP_REASON_SERVICE_MISSING, "kiln.assets' resources are off on this build");
    }
}

}  // namespace

extern "C" KILN_EXPORT KilnResult KILN_CALL kiln_plugin_main(const KilnApi* api, KilnOwner owner, KilnPluginInfo* info) {
    if (api->abi_version != KILN_ABI_VERSION || api->size < sizeof(KilnApi)) return KILN_ERR_UNAVAILABLE;
    g_api = api;
    g_owner = owner;
    info->id = KM_STR(KILN_MARKERS_SERVICE);
    info->version = KilnStr{"1", 1};
    info->on_ready = &on_ready;
    g_side = api->side();
    KilnBuildInfo build{};
    build.size = sizeof build;
    if (api->build_info(&build) == KILN_OK) g_exe.assign(build.exe_sha256, strnlen(build.exe_sha256, sizeof build.exe_sha256));
    // Session only: every store is bound to a world, so without the lifecycle there is no store (chapter 04 §4.2).
    KilnHandle h = 0;
    g_lifecycle = api->feature && api->feature(KM_STR("world.lifecycle")) && api->subscribe_world(owner, &on_world, nullptr, &h) == KILN_OK;
    if (!g_lifecycle) logf(KILN_LOG_WARN, "markers: the world lifecycle is unavailable on this build: markers.local and markers.send are off");
    if (g_side == KILN_SIDE_CLIENT) {
        KilnHandle p = 0;
        g_client_post = api->subscribe_world_tick(owner, KILN_WORLD_CLIENT, KILN_TICK_POST, &on_client_post, nullptr, &p) == KILN_OK;
        if (!g_client_post) logf(KILN_LOG_WARN, "markers: the client world's step could not be subscribed: markers.local is off");
    }
    g_switches.local = g_side == KILN_SIDE_CLIENT && g_lifecycle && g_client_post;
    KilnResult rc = api->register_service(owner, KM_STR(KILN_MARKERS_SERVICE), &kTable);
    if (rc != KILN_OK) return rc;
    rc = api->register_service(owner, KM_STR(KILN_MARKERS_BRIDGE_SERVICE), &kBridge);
    if (rc != KILN_OK) logf(KILN_LOG_WARN, "markers: kiln.markers.bridge was not registered (%d): the built-in receiver cannot attach", rc);
    return KILN_OK;
}
