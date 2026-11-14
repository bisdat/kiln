// SPDX-License-Identifier: MIT
// kiln.markers (services/kiln_markers.h): session-only markers on the game's map, Kiln's compass strip and world labels,
// and the internal kiln.markers.bridge the managed built-in receiver uses. Dossier chapter 04; docs/kiln/services/markers.md.
//
// This file is the glue to the core: it publishes both tables and the capability rows. The parts behind them land slice by
// slice; until a part is built its row reads unavailable (policy, "not built yet") and its members answer
// KILN_ERR_UNAVAILABLE naming the row.
#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <initializer_list>

#include "kiln_api.h"
#include "services/kiln_caps.h"
#include "services/kiln_markers.h"

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

void report_rows() {
    static const char kNotBuilt[] = "not built yet in this kiln.markers (0.2.0-dev.28 in work)";
    for (const char* name : {kLocal, kCatalog, kMap, kCompass, kWorld, kNameplates, kAnchorsClient, kAnchorsServer, kSend})
        report(name, KILN_CAP_UNAVAILABLE, KILN_CAP_REASON_POLICY, kNotBuilt);
    report(kMapText, KILN_CAP_UNAVAILABLE, KILN_CAP_REASON_POLICY,
           "the native map tooltip is not part of v1: its game test (04-map-text) has not been answered");
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

// ---- kiln.markers v1 ----------------------------------------------------------------------------------------------------

KilnResult KILN_CALL t_bind_owner(KilnOwner, KilnStr) { return KILN_ERR_UNAVAILABLE; }
KilnResult KILN_CALL t_release_owner(KilnOwner) { return KILN_OK; }
KilnResult KILN_CALL t_catalog_count(uint32_t* count, uint64_t* generation) {
    if (!count || !generation) return KILN_ERR_INVALID_ARGUMENT;
    *count = 0;
    *generation = 0;
    return KILN_ERR_UNAVAILABLE;
}
KilnResult KILN_CALL t_catalog_at(uint32_t, KilnMarkerIcon*) { return KILN_ERR_UNAVAILABLE; }
KilnResult KILN_CALL t_catalog_resolve(KilnStr, KilnMarkerIcon*, KilnMarkerResult* r) { return off(r, kCatalog); }
KilnResult KILN_CALL t_world_epoch(uint32_t, uint64_t* epoch) {
    if (epoch) *epoch = 0;
    return KILN_ERR_UNAVAILABLE;
}
KilnResult KILN_CALL t_capture_anchor(KilnOwner, uint32_t, uint64_t, KilnEntity, KilnMarkerAnchorResult*) { return KILN_ERR_UNAVAILABLE; }
KilnResult KILN_CALL t_release_anchor(KilnOwner, KilnMarkerAnchor) { return KILN_ERR_UNAVAILABLE; }
KilnResult KILN_CALL t_set_local(KilnOwner, const KilnMarkerSpec*, KilnMarkerResult* r) { return off(r, kLocal); }
KilnResult KILN_CALL t_remove_local(KilnOwner, KilnStr, KilnMarkerResult* r) { return off(r, kLocal); }
KilnResult KILN_CALL t_clear_local(KilnOwner, KilnMarkerResult* r) { return off(r, kLocal); }
KilnResult KILN_CALL t_send(KilnOwner, KilnEntity, const KilnMarkerSpec*, KilnMarkerResult* r) { return off(r, kSend); }
KilnResult KILN_CALL t_unsend(KilnOwner, KilnEntity, KilnStr, KilnMarkerResult* r) { return off(r, kSend); }
KilnResult KILN_CALL t_clear_sent(KilnOwner, KilnEntity, KilnMarkerResult* r) { return off(r, kSend); }
KilnResult KILN_CALL t_suppress_sent(KilnOwner, KilnEntity, KilnStr, KilnMarkerResult* r) { return off(r, kSend); }
KilnResult KILN_CALL t_subscribe(KilnOwner, KilnMarkerChangedFn, KilnMarkerVisibilityFn, KilnMarkerCatalogFn, void*, KilnHandle* out) {
    if (out) *out = 0;
    return KILN_ERR_UNAVAILABLE;
}
KilnResult KILN_CALL t_unsubscribe(KilnHandle) { return KILN_ERR_NOT_FOUND; }

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
    report_rows();
}

}  // namespace

extern "C" KILN_EXPORT KilnResult KILN_CALL kiln_plugin_main(const KilnApi* api, KilnOwner owner, KilnPluginInfo* info) {
    if (api->abi_version != KILN_ABI_VERSION || api->size < sizeof(KilnApi)) return KILN_ERR_UNAVAILABLE;
    g_api = api;
    g_owner = owner;
    info->id = KM_STR(KILN_MARKERS_SERVICE);
    info->version = KilnStr{"1", 1};
    info->on_ready = &on_ready;
    KilnResult rc = api->register_service(owner, KM_STR(KILN_MARKERS_SERVICE), &kTable);
    if (rc != KILN_OK) return rc;
    rc = api->register_service(owner, KM_STR(KILN_MARKERS_BRIDGE_SERVICE), &kBridge);
    if (rc != KILN_OK) logf(KILN_LOG_WARN, "markers: kiln.markers.bridge was not registered (%d): the built-in receiver cannot attach", rc);
    return KILN_OK;
}
