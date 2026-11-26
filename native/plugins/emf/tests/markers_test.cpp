// SPDX-License-Identifier: MIT
// EMF v33 markers and v47's bar on kiln.markers (api_markers.cpp, since 0.2.0-dev.31.13): the production adapter against
// a fake kiln.markers table, without a game. argv[1] "no-service": a Kiln without kiln.markers (its own process, the
// lookup happens once).
#include "emf_internal.h"
#include "services/kiln_markers.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

static int checks = 0, failures = 0;
#define CHECK(x)                                                    \
    do {                                                            \
        ++checks;                                                   \
        if (!(x)) {                                                 \
            ++failures;                                             \
            std::printf("FAIL %d: %s\n", __LINE__, #x);             \
        }                                                           \
    } while (0)

namespace kemf {
const KilnApi* g_api;
KilnOwner g_owner = 1;
uint32_t g_side = KILN_SIDE_CLIENT;
Services S;
thread_local Context t_ctx;
static Mod* g_caller = nullptr;
Mod* caller_mod(const void*) { return g_caller; }
static std::vector<std::string> unsupported_log, refusal_log;
void unsupported(const Mod* m, const char* fn, const char* why) {
    unsupported_log.push_back(std::string(m ? m->id : "?") + "|" + fn + "|" + (why ? why : ""));
}
void refuse(const Mod* m, const char* fn, const char* why, bool) {
    refusal_log.push_back(std::string(m ? m->id : "?") + "|" + fn + "|" + (why ? why : ""));
}
void note_call(const char*) {}
void mark_status_dirty() {}
static uint64_t clock_ms = 1000;
uint64_t now_ms() { return clock_ms; }
void logf(int, const char*, ...) {}
void copy_cstr(char* dst, size_t cap, std::string_view s) {
    if (!cap) return;
    const size_t n = std::min(cap - 1, s.size());
    std::memcpy(dst, s.data(), n);
    dst[n] = 0;
}
}  // namespace kemf
using namespace kemf;

// ---------------------------------------------------------------------------------------------- the fake service --
struct Rec {
    uint32_t surfaces = 0;
    KilnVec3 pos{};
    uint64_t follow = 0;
    float height = 0, max_distance = 0;
    uint32_t icon = 0, rgba = 0;
    std::string text;
    bool operator==(const Rec& o) const {
        return surfaces == o.surfaces && pos.x == o.pos.x && pos.y == o.pos.y && pos.z == o.pos.z && follow == o.follow &&
               height == o.height && max_distance == o.max_distance && icon == o.icon && rgba == o.rgba && text == o.text;
    }
};
using LKey = std::pair<KilnOwner, std::string>;
using SKey = std::tuple<KilnOwner, uint32_t, std::string>;  // recipient 0 = the broadcast layer
static std::map<KilnOwner, std::string> bound;
static std::map<LKey, Rec> local;
static std::map<SKey, Rec> sent;
static std::set<SKey> suppressed;
static std::vector<std::string> calls;
static uint64_t client_epoch = 1, server_epoch = 1;
static bool client_thread = true, tick_thread = true;
static std::set<uint32_t> players;
static std::set<std::string> off;
static std::vector<KilnMarkerIcon> catalog;
static std::set<uint32_t> client_entities, server_entities;
static uint64_t next_anchor = 100;
static std::map<uint64_t, uint32_t> anchors;  // live lease -> entity
static KilnMarkerChangedFn sub_changed = nullptr;
static void* sub_user = nullptr;
static int subscriptions = 0, unsubscriptions = 0;

static std::string str(KilnStr s) { return std::string(s.ptr ? s.ptr : "", s.ptr ? s.len : 0); }
static bool key_ok(const std::string& k) {
    if (k.empty() || k.size() > 64) return false;
    for (char c : k)
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == ':' || c == '-'))
            return false;
    return true;
}
static KilnResult bad(KilnMarkerResult* r, const char* why) {
    std::snprintf(r->reason, sizeof r->reason, "%s", why);
    return KILN_ERR_INVALID_ARGUMENT;
}
static KilnResult unavailable(KilnMarkerResult* r, const char* row) {
    std::snprintf(r->reason, sizeof r->reason, "%s", row);
    return KILN_ERR_UNAVAILABLE;
}
static KilnResult status(KilnMarkerResult* r, uint32_t s, const char* why = "") {
    r->status = s;
    r->committed = s == KILN_MARKER_ACCEPTED;
    std::snprintf(r->reason, sizeof r->reason, "%s", why);
    return KILN_OK;
}
static void reset(KilnMarkerResult* r) {
    const uint32_t size = r->size;
    *r = {};
    r->size = size;
}
// the spec's own rules, as kiln.markers' validate
static const char* invalid(const KilnMarkerSpec& s, Rec* out) {
    if (!s.surfaces || (s.surfaces & ~7u)) return "surfaces";
    if (!key_ok(str(s.key))) return "key";
    if (!s.follow && (s.position.x < -1000 || s.position.x > 12000 || s.position.z < -1000 || s.position.z > 12000 ||
                      s.position.y < -1000 || s.position.y > 5000))
        return "position is outside the world";
    if (!(s.height >= -128 && s.height <= 128)) return "height";
    if (!(s.max_distance >= 0 && s.max_distance <= 100000)) return "max_distance";
    const std::string text = str(s.text);
    if (text.size() > 128) return "text too long";
    for (unsigned char c : text)
        if (c < 0x20 || c == 0x7f) return "text control";
    if ((s.surfaces & KILN_MARKER_MAP) ? !s.icon_type : s.icon_type != 0) return "icon";
    Rec r;
    r.surfaces = s.surfaces;
    if (!s.follow) r.pos = s.position;
    r.follow = s.follow ? anchors.at(s.follow) : 0;  // the incarnation (here: the entity)
    r.height = s.height;
    r.max_distance = s.max_distance;
    r.icon = s.icon_type;
    r.rgba = s.rgba;
    r.text = text;
    *out = r;
    return nullptr;
}
static const char* surface_off(const KilnMarkerSpec& s, bool server) {
    if (s.follow && off.count(server ? "markers.anchors.server" : "markers.anchors.client"))
        return server ? "markers.anchors.server" : "markers.anchors.client";
    if ((s.surfaces & KILN_MARKER_MAP) && !server && off.count("markers.map")) return "markers.map";
    if ((s.surfaces & KILN_MARKER_COMPASS) && !server && off.count("markers.compass")) return "markers.compass";
    if ((s.surfaces & KILN_MARKER_WORLD) && !server && off.count("markers.world")) return "markers.world";
    return nullptr;
}
static bool icon_known(uint32_t id) {
    for (const auto& i : catalog)
        if (i.id == id && (i.surfaces & KILN_MARKER_MAP)) return true;
    return false;
}

static KilnResult KILN_CALL f_bind(KilnOwner owner, KilnStr ns) {
    const std::string n = str(ns);
    if (!owner || !key_ok(n)) return KILN_ERR_INVALID_ARGUMENT;
    for (const auto& [o, b] : bound)
        if (b == n && o != owner) return KILN_ERR_INVALID_ARGUMENT;
    if (auto it = bound.find(owner); it != bound.end()) return it->second == n ? KILN_OK : KILN_ERR_INVALID_ARGUMENT;
    bound[owner] = n;
    calls.push_back("bind " + n);
    return KILN_OK;
}
static KilnResult KILN_CALL f_release_owner(KilnOwner owner) {
    calls.push_back("release_owner " + (bound.count(owner) ? bound[owner] : std::string("?")));
    std::erase_if(local, [&](const auto& kv) { return kv.first.first == owner; });
    std::erase_if(sent, [&](const auto& kv) { return std::get<0>(kv.first) == owner; });
    std::erase_if(suppressed, [&](const auto& k) { return std::get<0>(k) == owner; });
    bound.erase(owner);
    return KILN_OK;
}
static KilnResult KILN_CALL f_catalog_count(uint32_t* count, uint64_t* generation) {
    *count = static_cast<uint32_t>(catalog.size());
    *generation = catalog.empty() ? 0 : 1;
    return KILN_OK;
}
static KilnResult KILN_CALL f_catalog_at(uint32_t index, KilnMarkerIcon* out) {
    if (!out || out->size < sizeof(KilnMarkerIcon)) return KILN_ERR_INVALID_ARGUMENT;
    if (index >= catalog.size()) return KILN_ERR_NOT_FOUND;
    *out = catalog[index];
    return KILN_OK;
}
static KilnResult KILN_CALL f_resolve(KilnStr, KilnMarkerIcon*, KilnMarkerResult*) { return KILN_ERR_INVALID_ARGUMENT; }
static KilnResult KILN_CALL f_world_epoch(uint32_t kind, uint64_t* epoch) {
    *epoch = kind == KILN_WORLD_CLIENT ? client_epoch : kind == KILN_WORLD_SERVER ? server_epoch : 0;
    return KILN_OK;
}
static KilnResult KILN_CALL f_capture(KilnOwner owner, uint32_t kind, uint64_t epoch, KilnEntity entity, KilnMarkerAnchorResult* out) {
    if (!out || out->size < sizeof *out || !owner || !entity) return KILN_ERR_INVALID_ARGUMENT;
    const uint32_t size = out->size;
    *out = {};
    out->size = size;
    calls.push_back((kind == KILN_WORLD_CLIENT ? "capture client " : "capture server ") + std::to_string(entity));
    if (off.count(kind == KILN_WORLD_CLIENT ? "markers.anchors.client" : "markers.anchors.server")) return KILN_ERR_UNAVAILABLE;
    if (!(kind == KILN_WORLD_CLIENT ? client_thread : tick_thread)) return KILN_ERR_WRONG_THREAD;
    const uint64_t current = kind == KILN_WORLD_CLIENT ? client_epoch : server_epoch;
    if (!current || epoch != current) {
        out->status = KILN_MARKER_NOT_READY;
        return KILN_OK;
    }
    if (!(kind == KILN_WORLD_CLIENT ? client_entities : server_entities).count(entity)) {
        out->status = KILN_MARKER_NOT_FOUND;
        return KILN_OK;
    }
    out->status = KILN_MARKER_ACCEPTED;
    out->anchor = next_anchor++;
    anchors[out->anchor] = entity;
    return KILN_OK;
}
static std::set<uint64_t> released;
static KilnResult KILN_CALL f_release_anchor(KilnOwner, KilnMarkerAnchor a) {
    released.insert(a);
    return KILN_OK;
}
static KilnResult KILN_CALL f_set_local(KilnOwner owner, const KilnMarkerSpec* spec, KilnMarkerResult* r) {
    if (!r || r->size < sizeof *r) return KILN_ERR_INVALID_ARGUMENT;
    reset(r);
    if (!spec || spec->size < sizeof *spec) return bad(r, "spec size");
    if (const char* row = surface_off(*spec, false)) return unavailable(r, row);
    Rec rec;
    if (const char* why = invalid(*spec, &rec)) return bad(r, why);
    if (!bound.count(owner)) return bad(r, "not bound");
    if (!client_thread && spec->follow) return bad(r, "never: the lease was captured on the thread");
    if (!client_epoch) return status(r, KILN_MARKER_NOT_READY, "NotReady: there is no client world");
    if ((spec->surfaces & KILN_MARKER_MAP) && catalog.empty()) return status(r, KILN_MARKER_CATALOG_NOT_READY, "CatalogNotReady");
    if ((spec->surfaces & KILN_MARKER_MAP) && !icon_known(spec->icon_type)) return bad(r, "icon_type is not in the catalog");
    const LKey k{owner, str(spec->key)};
    calls.push_back("set " + k.second);
    auto it = local.find(k);
    if (it != local.end() && it->second == rec) return status(r, KILN_MARKER_ACCEPTED);
    if (it == local.end()) {
        size_t mine = 0;
        for (const auto& kv : local) mine += kv.first.first == owner;
        if (mine >= 128) return status(r, KILN_MARKER_FULL, "Full: namespace has 128 local markers (the most)");
    }
    local[k] = rec;
    status(r, KILN_MARKER_ACCEPTED);
    r->affected = 1;
    return KILN_OK;
}
static KilnResult KILN_CALL f_remove_local(KilnOwner owner, KilnStr key, KilnMarkerResult* r) {
    reset(r);
    if (!client_epoch) return status(r, KILN_MARKER_NOT_READY);
    calls.push_back("remove " + str(key));
    if (!local.erase({owner, str(key)})) return status(r, KILN_MARKER_NOT_FOUND, "NotFound");
    status(r, KILN_MARKER_ACCEPTED);
    r->affected = 1;
    return KILN_OK;
}
static KilnResult KILN_CALL f_clear_local(KilnOwner owner, KilnMarkerResult* r) {
    reset(r);
    if (!client_epoch) return status(r, KILN_MARKER_NOT_READY);
    calls.push_back("clear_local");
    r->affected = static_cast<uint32_t>(std::erase_if(local, [&](const auto& kv) { return kv.first.first == owner; }));
    status(r, KILN_MARKER_ACCEPTED);
    return KILN_OK;
}
static uint32_t sent_by(KilnOwner owner) {
    uint32_t n = 0;
    for (const auto& kv : sent) n += std::get<0>(kv.first) == owner;
    for (const auto& k : suppressed) n += std::get<0>(k) == owner;
    return n;
}
static bool server_call_ok(KilnMarkerResult* r, KilnResult* rc, KilnEntity to) {
    if (!tick_thread) {
        *rc = KILN_ERR_WRONG_THREAD;
        return false;
    }
    if (!server_epoch) {
        *rc = status(r, KILN_MARKER_NOT_READY, "NotReady: there is no server world");
        return false;
    }
    if (to && !players.count(to)) {
        *rc = status(r, KILN_MARKER_NOT_FOUND, "NotFound: player is not connected");
        return false;
    }
    return true;
}
static KilnResult KILN_CALL f_send(KilnOwner owner, KilnEntity to, const KilnMarkerSpec* spec, KilnMarkerResult* r) {
    reset(r);
    if (!spec || spec->size < sizeof *spec) return bad(r, "spec size");
    if (const char* row = surface_off(*spec, true)) return unavailable(r, row);
    Rec rec;
    if (const char* why = invalid(*spec, &rec)) return bad(r, why);
    KilnResult rc;
    if (!server_call_ok(r, &rc, to)) return rc;
    if ((spec->surfaces & KILN_MARKER_MAP) && catalog.empty()) return status(r, KILN_MARKER_CATALOG_NOT_READY, "CatalogNotReady");
    if ((spec->surfaces & KILN_MARKER_MAP) && !icon_known(spec->icon_type)) return bad(r, "icon_type is not in the catalog");
    const SKey k{owner, to, str(spec->key)};
    calls.push_back("send " + std::to_string(to) + " " + std::get<2>(k));
    auto it = sent.find(k);
    if (it != sent.end() && it->second == rec) {
        status(r, KILN_MARKER_ACCEPTED);
        return KILN_OK;
    }
    if (it == sent.end() && !suppressed.count(k) && sent_by(owner) >= 128)
        return status(r, KILN_MARKER_FULL, "Full: 'emf:x' has 128 sent markers (broadcast and per-player together; the most)");
    sent[k] = rec;
    suppressed.erase(k);
    status(r, KILN_MARKER_ACCEPTED);
    r->affected = 1;
    if (to) {
        r->queued = 1;
    } else {
        for (uint32_t p : players)
            if (!sent.count({owner, p, std::get<2>(k)}) && !suppressed.count({owner, p, std::get<2>(k)})) ++r->queued;
    }
    return KILN_OK;
}
static KilnResult KILN_CALL f_unsend(KilnOwner owner, KilnEntity to, KilnStr key, KilnMarkerResult* r) {
    reset(r);
    KilnResult rc;
    if (!server_call_ok(r, &rc, to)) return rc;
    const SKey k{owner, to, str(key)};
    calls.push_back("unsend " + std::to_string(to) + " " + std::get<2>(k));
    if (!sent.erase(k) && !suppressed.erase(k)) return status(r, KILN_MARKER_NOT_FOUND, "NotFound");
    status(r, KILN_MARKER_ACCEPTED);
    r->affected = 1;
    r->queued = to ? 1 : static_cast<uint32_t>(players.size());
    return KILN_OK;
}
static KilnResult KILN_CALL f_clear_sent(KilnOwner, KilnEntity, KilnMarkerResult* r) {
    reset(r);
    calls.push_back("clear_sent");
    return status(r, KILN_MARKER_ACCEPTED);
}
static KilnResult KILN_CALL f_suppress(KilnOwner owner, KilnEntity to, KilnStr key, KilnMarkerResult* r) {
    reset(r);
    if (!to) return bad(r, "recipient 0");
    KilnResult rc;
    if (!server_call_ok(r, &rc, to)) return rc;
    const std::string k = str(key);
    calls.push_back("suppress " + std::to_string(to) + " " + k);
    if (!sent.count({owner, 0, k}) && !sent.count({owner, to, k})) return status(r, KILN_MARKER_NOT_FOUND, "NotFound");
    sent.erase({owner, to, k});
    suppressed.insert({owner, to, k});
    status(r, KILN_MARKER_ACCEPTED);
    r->affected = 1;
    r->queued = 1;
    return KILN_OK;
}
static KilnResult KILN_CALL f_subscribe(KilnOwner, KilnMarkerChangedFn changed, KilnMarkerVisibilityFn, KilnMarkerCatalogFn, void* user,
                                        KilnHandle* out) {
    ++subscriptions;
    sub_changed = changed;
    sub_user = user;
    *out = 77;
    return KILN_OK;
}
static KilnResult KILN_CALL f_unsubscribe(KilnHandle h) {
    if (h == 77) ++unsubscriptions, sub_changed = nullptr;
    return KILN_OK;
}

static KilnMarkersApi fake{};
static bool service_present = true;
static int lookups = 0;
static uintptr_t worlds[3] = {0x1000, 0, 0x3000};
static bool KILN_CALL on_tick_thread() { return tick_thread; }
static uintptr_t KILN_CALL world_of(uint32_t kind) { return kind < 3 ? worlds[kind] : 0; }
static const void* KILN_CALL service(KilnStr name, uint32_t version) {
    ++lookups;
    return service_present && str(name) == KILN_MARKERS_SERVICE && version == KILN_MARKERS_VERSION ? &fake : nullptr;
}

static KilnMarkerIcon icon(uint32_t id, uint32_t flags, uint32_t surfaces, const char* alias) {
    KilnMarkerIcon i{};
    i.size = sizeof i;
    i.id = id;
    i.flags = KILN_MARKER_ICON_IMAGE | flags;
    i.surfaces = surfaces;
    std::snprintf(i.key, sizeof i.key, "type:%08x", id);
    std::snprintf(i.alias, sizeof i.alias, "%s", alias);
    std::snprintf(i.texture_name, sizeof i.texture_name, "mapmarker_%s", alias);
    return i;
}
static void fill_catalog() {
    catalog = {icon(0x10000001u, KILN_MARKER_ICON_CUSTOM, KILN_MARKER_MAP, "custom_a"),
               icon(0x10000002u, KILN_MARKER_ICON_CUSTOM, KILN_MARKER_MAP, "custom_b"),
               icon(0x10000003u, KILN_MARKER_ICON_CUSTOM, KILN_MARKER_MAP, "custom_c"),
               icon(0x20000001u, KILN_MARKER_ICON_FAST_TRAVEL, 0, "shrine"),
               icon(0xffe5e444u, KILN_MARKER_ICON_ABOVE_FOG, KILN_MARKER_MAP, "well_plant")};
}

static Mod mod_a, mod_b, lua;
static EmfMarker marker(const char* id, uint32_t flags = EMF_MARKER_WORLD) {
    EmfMarker m{};
    m.size = sizeof m;
    m.id = id;
    m.x = 100;
    m.y = 50;
    m.z = 200;
    m.flags = flags;
    return m;
}
static const Rec* local_of(const Mod& m, const std::string& key) {
    auto it = local.find({m.owner, key});
    return it == local.end() ? nullptr : &it->second;
}
static const Rec* sent_of(const Mod& m, uint32_t to, const std::string& key) {
    auto it = sent.find({m.owner, to, key});
    return it == sent.end() ? nullptr : &it->second;
}
static size_t count_calls(const std::string& prefix) {
    size_t n = 0;
    for (const auto& c : calls) n += c.rfind(prefix, 0) == 0;
    return n;
}
static void fresh() {
    markers_shutdown();
    local.clear();
    sent.clear();
    suppressed.clear();
    calls.clear();
    bound.clear();
    anchors.clear();
    released.clear();
    refusal_log.clear();
    unsupported_log.clear();
    off.clear();
    players.clear();
    client_entities.clear();
    server_entities.clear();
    client_epoch = server_epoch = 1;
    client_thread = tick_thread = true;
    g_side = KILN_SIDE_CLIENT;
    fill_catalog();
    g_caller = &mod_a;
}

// ---------------------------------------------------------------------------------------------- local markers ----
static void test_set(const EmfApi& t) {
    fresh();
    // a plain world marker: bound to the mod's own namespace, key = the id, EMF's numbers carried over
    EmfMarker m = marker("camp");
    m.text = "Camp";
    m.height = 2.5f;
    m.max_distance = 300;
    m.rgba = 0x11223344u;
    CHECK(t.marker_set(&m));
    CHECK(bound[mod_a.owner] == "emf:mod-a");
    const Rec* r = local_of(mod_a, "camp");
    CHECK(r && r->surfaces == KILN_MARKER_WORLD && r->pos.x == 100 && r->pos.y == 50 && r->pos.z == 200 && r->follow == 0);
    CHECK(r && r->height == 2.5f && r->max_distance == 300 && r->rgba == 0x11223344u && r->text == "Camp" && r->icon == 0);
    // set again = update; an identical value changes nothing and is still true
    m.text = "Camp 2";
    CHECK(t.marker_set(&m) && local_of(mod_a, "camp")->text == "Camp 2");
    CHECK(t.marker_set(&m));
    // rgba 0 = the HUD's accent colour on EMF: Kiln's default (white) here; no text is fine
    EmfMarker plain = marker("plain");
    CHECK(t.marker_set(&plain) && local_of(mod_a, "plain")->rgba == 0xffffffffu && local_of(mod_a, "plain")->text.empty());
    // the edge arrow is Kiln's compass strip (direction and distance); the map needs an icon of the catalog
    EmfMarker e = marker("edge", EMF_MARKER_WORLD | EMF_MARKER_EDGE_ARROW | EMF_MARKER_DISTANCE);
    CHECK(t.marker_set(&e) && local_of(mod_a, "edge")->surfaces == (KILN_MARKER_WORLD | KILN_MARKER_COMPASS));
    EmfMarker g = marker("map", EMF_MARKER_GAME_MAP);
    g.game_icon = 0xffe5e444u;
    CHECK(t.marker_set(&g) && local_of(mod_a, "map")->surfaces == KILN_MARKER_MAP && local_of(mod_a, "map")->icon == 0xffe5e444u);
    // no game icon: one of the game's own pin types by shape (diamond, circle, square); text only gets no map icon
    const uint32_t pins[3] = {0x10000001u, 0x10000002u, 0x10000003u};
    for (uint8_t shape = 0; shape < 3; ++shape) {
        EmfMarker s = marker("pin", EMF_MARKER_WORLD | EMF_MARKER_GAME_MAP);
        s.shape = shape;
        CHECK(t.marker_set(&s) && local_of(mod_a, "pin")->surfaces == (KILN_MARKER_MAP | KILN_MARKER_WORLD) &&
              local_of(mod_a, "pin")->icon == pins[shape]);
    }
    EmfMarker none = marker("textonly", EMF_MARKER_WORLD | EMF_MARKER_GAME_MAP);
    none.shape = EMF_MARKER_NONE;
    CHECK(t.marker_set(&none) && local_of(mod_a, "textonly")->surfaces == KILN_MARKER_WORLD);
    // a fast-travel type (refused on Kiln's map) or an unknown one: the shape's pin instead, logged
    EmfMarker ft = marker("ft", EMF_MARKER_GAME_MAP);
    ft.game_icon = 0x20000001u;
    CHECK(t.marker_set(&ft) && local_of(mod_a, "ft")->icon == 0x10000001u);
    ft.game_icon = 0x0badf00du;
    CHECK(t.marker_set(&ft) && local_of(mod_a, "ft")->icon == 0x10000001u);
    // flags that ask for no Kiln surface (only DISTANCE or MINIMAP): kept as the mod's marker, nothing drawn
    EmfMarker mini = marker("mini", EMF_MARKER_MINIMAP);
    CHECK(t.marker_set(&mini) && !local_of(mod_a, "mini"));
    CHECK(t.marker_remove("mini"));
    // a marker that was drawn and is set again with no surface: Kiln's record goes
    EmfMarker gone = marker("camp", 0);
    CHECK(t.marker_set(&gone) && !local_of(mod_a, "camp"));
    // height beyond Kiln's [-128, 128]: clamped (EMF allows 1000 m)
    EmfMarker tall = marker("tall");
    tall.height = 500;
    CHECK(t.marker_set(&tall) && local_of(mod_a, "tall")->height == 128);
    tall.height = -900;
    CHECK(t.marker_set(&tall) && local_of(mod_a, "tall")->height == -128);
}

static void test_refusals(const EmfApi& t) {
    fresh();
    const size_t before = calls.size();
    CHECK(!t.marker_set(nullptr));
    EmfMarker m = marker("");
    CHECK(!t.marker_set(&m));
    m.id = nullptr;
    CHECK(!t.marker_set(&m));
    m.id = "has space";
    CHECK(!t.marker_set(&m));
    const std::string long_id(49, 'a');
    m.id = long_id.c_str();
    CHECK(!t.marker_set(&m));
    const std::string max_id(48, 'a');
    m.id = max_id.c_str();
    CHECK(t.marker_set(&m) && local_of(mod_a, max_id));
    m = marker("small");
    m.size = 76;  // smaller than v33's EmfMarker
    CHECK(!t.marker_set(&m));
    m = marker("nan");
    m.x = std::nan("");
    CHECK(!t.marker_set(&m));
    m = marker("far");
    m.z = 100001;
    CHECK(!t.marker_set(&m));
    m = marker("h");
    m.height = 1001;
    CHECK(!t.marker_set(&m));
    m = marker("md");
    m.max_distance = -1;
    CHECK(!t.marker_set(&m));
    m = marker("shape");
    m.shape = 4;
    CHECK(!t.marker_set(&m));
    m = marker("bar", EMF_MARKER_WORLD | EMF_MARKER_BAR);
    m.bar = std::nanf("");
    CHECK(!t.marker_set(&m));
    // only the 48-a marker reached the service (bind, set)
    CHECK(count_calls("set ") == 1 && calls.size() == before + 2);
    // within EMF's 100 km but outside Kiln's world box: refused by kiln.markers, false
    m = marker("outside");
    m.x = 50000;
    CHECK(!t.marker_set(&m) && !local_of(mod_a, "outside"));
    CHECK(!refusal_log.empty());
    // the dedicated server draws nothing: false, nothing set (marker_send is the server's way)
    g_side = KILN_SIDE_SERVER;
    m = marker("srv");
    CHECK(!t.marker_set(&m) && !local_of(mod_a, "srv") && !t.marker_remove("srv") && t.marker_clear() == 0);
    g_side = KILN_SIDE_CLIENT;
    // no client world yet: false (EMF keeps it for the next world; Kiln's markers belong to one world)
    client_epoch = 0;
    CHECK(!t.marker_set(&m));
    client_epoch = 1;
    CHECK(t.marker_set(&m));
    // no calling mod: false
    g_caller = nullptr;
    CHECK(!t.marker_set(&m) && !t.marker_remove("srv") && t.marker_clear() == 0);
    g_caller = &mod_a;
}

static void test_ids_and_text(const EmfApi& t) {
    fresh();
    // EMF 0.6.22's ids may hold ',', Kiln's keys may not: an injective key, readable for every other id
    EmfMarker a = marker("a,b"), b = marker("a.b");
    CHECK(t.marker_set(&a) && t.marker_set(&b));
    CHECK(local_of(mod_a, "a.b") && local_of(mod_a, "a.b::000000000002"));
    EmfMarker c = marker("x::000000000000");  // an id that looks like an encoded key is encoded too
    CHECK(t.marker_set(&c) && local_of(mod_a, "x::000000000000::000000000000"));
    const std::string commas(48, ',');
    EmfMarker d = marker(commas.c_str());
    CHECK(t.marker_set(&d) && local_of(mod_a, std::string(48, '.') + "::ffffffffffff"));
    CHECK(t.marker_remove("a,b") && !local_of(mod_a, "a.b::000000000002") && local_of(mod_a, "a.b"));
    // text: cut at 64 bytes, never inside a UTF-8 character; control characters become spaces, broken bytes go
    const std::string sixty3(63, 'x');
    const std::string cut = sixty3 + "\xc3\xa9";  // 65 bytes: the e-acute would be cut in half
    EmfMarker e = marker("text");
    e.text = cut.c_str();
    CHECK(t.marker_set(&e) && local_of(mod_a, "text")->text == sixty3);
    e.text = "a\tb\nc\x7f" "d\xff" "e\xc2\x85" "f";
    CHECK(t.marker_set(&e) && local_of(mod_a, "text")->text == "a b c de f");
    // v47 bar: EMF_MARKER_BAR with `bar` 0..1 is drawn as ten cells after the text (Kiln's labels have no bar)
    EmfMarker bar = marker("bar", EMF_MARKER_WORLD | EMF_MARKER_BAR);
    bar.text = "Rex";
    bar.bar = 0.75f;
    CHECK(t.marker_set(&bar) && local_of(mod_a, "bar")->text == "Rex [########..]");
    bar.bar = 0;
    CHECK(t.marker_set(&bar) && local_of(mod_a, "bar")->text == "Rex [..........]");
    bar.bar = 7;  // past 1: full
    CHECK(t.marker_set(&bar) && local_of(mod_a, "bar")->text == "Rex [##########]");
    bar.text = nullptr;
    bar.bar = 0.5f;
    CHECK(t.marker_set(&bar) && local_of(mod_a, "bar")->text == "[#####.....]");
    bar.flags = EMF_MARKER_WORLD;  // no flag: no bar, whatever `bar` holds
    CHECK(t.marker_set(&bar) && local_of(mod_a, "bar")->text.empty());
    const std::string sixty4(64, 'y');
    bar.text = sixty4.c_str();
    bar.flags |= EMF_MARKER_BAR;
    CHECK(t.marker_set(&bar) && local_of(mod_a, "bar")->text == sixty4 + " [#####.....]");  // 77 bytes, under Kiln's 128
}

static void test_ownership(const EmfApi& t) {
    fresh();
    EmfMarker m = marker("same");
    m.text = "A";
    CHECK(t.marker_set(&m));
    g_caller = &mod_b;
    m.text = "B";
    CHECK(t.marker_set(&m));
    CHECK(bound[mod_b.owner] == "emf:mod-b" && local_of(mod_a, "same")->text == "A" && local_of(mod_b, "same")->text == "B");
    // remove: false for another mod's marker or none
    CHECK(!t.marker_remove("other") && !t.marker_remove(nullptr));
    EmfMarker two = marker("two");
    CHECK(t.marker_set(&two));
    CHECK(t.marker_clear() == 2 && !local_of(mod_b, "same") && local_of(mod_a, "same"));
    CHECK(t.marker_clear() == 0);
    g_caller = &mod_a;
    CHECK(t.marker_remove("same") && !t.marker_remove("same"));
    // limits: Kiln holds 128 markers per mod (EMF 256): the 129th is false, logged
    char id[16];
    for (int i = 0; i < 128; ++i) {
        std::snprintf(id, sizeof id, "m%d", i);
        EmfMarker k = marker(id);
        CHECK(t.marker_set(&k));
    }
    EmfMarker over = marker("m128");
    refusal_log.clear();
    CHECK(!t.marker_set(&over) && !refusal_log.empty() && refusal_log.back().find("128") != std::string::npos);
    // an update of one of the 128 still works; remove makes room
    EmfMarker upd = marker("m5");
    upd.text = "five";
    CHECK(t.marker_set(&upd) && t.marker_remove("m0") && t.marker_set(&over));
    CHECK(t.marker_clear() == 128);
    // the client world stops: Kiln's records are gone, and so are the mod's markers here
    EmfMarker w = marker("w");
    CHECK(t.marker_set(&w));
    markers_world_stopping(EMF_WORLD_CLIENT);
    local.clear();  // what kiln.markers does at the stop
    CHECK(!t.marker_remove("w") && t.marker_clear() == 0);
    // unload: every bound mod's owner is released (kiln.markers then clears its records and frees the namespace)
    CHECK(t.marker_set(&w));
    calls.clear();
    markers_shutdown();
    CHECK(count_calls("release_owner emf:mod-a") == 1 && count_calls("release_owner emf:mod-b") == 1 && bound.empty());
    CHECK(subscriptions >= 1 && unsubscriptions >= 1);
}

static void test_follow(const EmfApi& t) {
    fresh();
    client_entities = {42};
    // inside a client world callback: the entity's lease is taken, the record follows, the lease goes back
    EmfMarker m = marker("pet", EMF_MARKER_WORLD);
    m.entity = 42;
    m.height = 1.7f;
    CHECK(t.marker_set(&m));
    const Rec* r = local_of(mod_a, "pet");
    CHECK(r && r->follow == 42 && count_calls("capture client 42") == 1 && released.size() == 1);
    // an entity the game does not know (yet): drawn at x, y, z (EMF: "until the game knows the entity"); followed once
    // it is there (tried again at the client's step, at most once a second)
    EmfMarker later = marker("later");
    later.entity = 43;
    CHECK(t.marker_set(&later) && local_of(mod_a, "later")->follow == 0 && local_of(mod_a, "later")->pos.x == 100);
    client_entities.insert(43);
    markers_after_step(reinterpret_cast<EmfWorld*>(worlds[2]), EMF_WORLD_CLIENT);
    CHECK(local_of(mod_a, "later")->follow == 0);  // under a second
    clock_ms += 1000;
    markers_after_step(reinterpret_cast<EmfWorld*>(worlds[2]), EMF_WORLD_CLIENT);
    CHECK(local_of(mod_a, "later")->follow == 43);
    // another thread: placed at once, followed at the client world's next step
    client_thread = false;
    client_entities.insert(44);
    EmfMarker off_thread = marker("thread");
    off_thread.entity = 44;
    CHECK(t.marker_set(&off_thread) && local_of(mod_a, "thread")->follow == 0);
    client_thread = true;
    markers_after_step(reinterpret_cast<EmfWorld*>(worlds[2]), EMF_WORLD_CLIENT);
    CHECK(local_of(mod_a, "thread")->follow == 44);
    // following is off on this game build: fixed at x, y, z, logged
    off.insert("markers.anchors.client");
    EmfMarker fixed = marker("fixed");
    fixed.entity = 42;
    CHECK(t.marker_set(&fixed) && local_of(mod_a, "fixed")->follow == 0);
    off.clear();
    // the marker is set again without an entity: no retry follows it later
    later.entity = 0;
    CHECK(t.marker_set(&later) && local_of(mod_a, "later")->follow == 0);
    clock_ms += 2000;
    markers_after_step(reinterpret_cast<EmfWorld*>(worlds[2]), EMF_WORLD_CLIENT);
    CHECK(local_of(mod_a, "later")->follow == 0);
    // every lease taken was given back
    CHECK(released.size() == anchors.size());
}

static void test_degrade(const EmfApi& t) {
    fresh();
    // the icon catalog is still warming up: the marker shows on the other surfaces, the map is added once it is ready
    catalog.clear();
    EmfMarker m = marker("warm", EMF_MARKER_WORLD | EMF_MARKER_GAME_MAP);
    CHECK(t.marker_set(&m) && local_of(mod_a, "warm")->surfaces == KILN_MARKER_WORLD);
    EmfMarker only = marker("maponly", EMF_MARKER_GAME_MAP);
    CHECK(t.marker_set(&only) && !local_of(mod_a, "maponly"));
    fill_catalog();
    clock_ms += 1000;
    markers_after_step(reinterpret_cast<EmfWorld*>(worlds[2]), EMF_WORLD_CLIENT);
    CHECK(local_of(mod_a, "warm")->surfaces == (KILN_MARKER_MAP | KILN_MARKER_WORLD) && local_of(mod_a, "warm")->icon == 0x10000001u);
    CHECK(local_of(mod_a, "maponly") && local_of(mod_a, "maponly")->surfaces == KILN_MARKER_MAP);
    // a surface whose capability is off is left out (Kiln refuses a spec whole), the rest is drawn
    off.insert("markers.compass");
    EmfMarker c = marker("compass", EMF_MARKER_WORLD | EMF_MARKER_EDGE_ARROW);
    CHECK(t.marker_set(&c) && local_of(mod_a, "compass")->surfaces == KILN_MARKER_WORLD);
    off.insert("markers.world");
    EmfMarker none = marker("nothing", EMF_MARKER_WORLD | EMF_MARKER_EDGE_ARROW);
    CHECK(!t.marker_set(&none) && !local_of(mod_a, "nothing"));
}

static void test_types(const EmfApi& t) {
    fresh();
    // the catalog's types usable on the map (fast-travel types are refused on Kiln's map: not listed)
    CHECK(t.marker_types(nullptr, 0) == 4);
    EmfMapMarkerType out[8];
    std::memset(out, 0x5a, sizeof out);
    CHECK(t.marker_types(out, 2) == 4);
    CHECK(out[0].size == sizeof(EmfMapMarkerType) && out[0].id == 0x10000001u && out[0].flags == EMF_MAP_TYPE_CUSTOM &&
          std::string(out[0].name) == "custom_a" && out[1].id == 0x10000002u);
    CHECK(out[2].size == 0x5a5a5a5au);  // past cap: untouched
    CHECK(t.marker_types(out, 8) == 4 && out[3].id == 0xffe5e444u && out[3].flags == 0 && std::string(out[3].name) == "well_plant");
    catalog.clear();
    CHECK(t.marker_types(out, 8) == 0);
}

// ---------------------------------------------------------------------------------------------- server markers ---
static uint32_t one(const EmfApi& t, uint32_t player, const EmfMarker& m) { return t.marker_send(player, &m, 1); }

static void test_send(const EmfApi& t) {
    fresh();
    g_side = KILN_SIDE_SERVER;
    players = {5, 6};
    server_entities = {900};
    g_caller = &lua;
    // Beastmaster 1.22's beast marker, as the Lua host builds it (an id "<script>:<id>")
    EmfMarker beast = marker("beastmaster:beast.900",
                             EMF_MARKER_WORLD | EMF_MARKER_DISTANCE | EMF_MARKER_GAME_MAP | EMF_MARKER_EDGE_ARROW | EMF_MARKER_BAR);
    beast.entity = 900;
    beast.height = 1.7f;
    beast.text = "Rex";
    beast.bar = 0.75f;
    CHECK(one(t, 5, beast) == 1);
    const Rec* r = sent_of(lua, 5, "beastmaster:beast.900");
    CHECK(r && r->surfaces == (KILN_MARKER_MAP | KILN_MARKER_COMPASS | KILN_MARKER_WORLD) && r->follow == 900 &&
          r->icon == 0x10000001u && r->text == "Rex [########..]" && r->height == 1.7f);
    CHECK(count_calls("capture server 900") == 1 && released.size() == 1 && bound[lua.owner] == "emf:lua");
    // the same again: nothing changes, still "1 player"
    CHECK(one(t, 5, beast) == 1);
    // a stable: fixed, square, within 200 m
    EmfMarker stable = marker("beastmaster:stable.100_200", EMF_MARKER_WORLD | EMF_MARKER_DISTANCE | EMF_MARKER_GAME_MAP);
    stable.shape = EMF_MARKER_SQUARE;
    stable.text = "Stable";
    stable.height = 1.5f;
    stable.max_distance = 200;
    CHECK(one(t, 6, stable) == 1);
    r = sent_of(lua, 6, "beastmaster:stable.100_200");
    CHECK(r && r->surfaces == (KILN_MARKER_MAP | KILN_MARKER_WORLD) && r->icon == 0x10000003u && r->max_distance == 200 && r->follow == 0);
    // every player (0): the broadcast layer, "how many players" = the Kiln players it reached
    EmfMarker all = marker("all");
    CHECK(one(t, 0, all) == 2 && sent_of(lua, 0, "all"));
    // a player who is not connected or has no Kiln: nothing, 0 (EMF: players without EMF get nothing)
    CHECK(one(t, 9, all) == 0 && !sent_of(lua, 9, "all"));
    // bad markers are left out of a call; at most 256 a call
    EmfMarker batch[3] = {marker("ok1"), marker("bad id"), marker("ok2")};
    CHECK(t.marker_send(5, batch, 3) == 1 && sent_of(lua, 5, "ok1") && sent_of(lua, 5, "ok2"));
    CHECK(t.marker_send(5, nullptr, 1) == 0 && t.marker_send(5, batch, 0) == 0);
    std::vector<EmfMarker> many(300, marker("same"));
    calls.clear();
    CHECK(t.marker_send(5, many.data(), 300) == 1 && count_calls("send 5 same") == 256);
    // not on the server's tick thread: carried out at the next server step, in order; answers 0 then
    tick_thread = false;
    EmfMarker later = marker("later");
    CHECK(one(t, 5, later) == 0 && !sent_of(lua, 5, "later"));
    CHECK(t.marker_unsend(5, "ok1") == 0 && sent_of(lua, 5, "ok1"));
    tick_thread = true;
    markers_after_step(reinterpret_cast<EmfWorld*>(worlds[0]), EMF_WORLD_SERVER);
    CHECK(sent_of(lua, 5, "later") && !sent_of(lua, 5, "ok1"));
    // a joined client's game (no server world here): 0 at once, nothing kept
    worlds[0] = 0;
    tick_thread = false;
    CHECK(one(t, 5, later) == 0);
    worlds[0] = 0x1000;
    tick_thread = true;
    markers_after_step(reinterpret_cast<EmfWorld*>(worlds[0]), EMF_WORLD_SERVER);
    // Kiln's limit: 128 sent markers per mod across players (EMF has none but 256 a call): the rest are left out
    char id[16];
    uint32_t reached = 0;
    for (int i = 0; i < 140; ++i) {
        std::snprintf(id, sizeof id, "n%d", i);
        EmfMarker n = marker(id);
        reached += one(t, 6, n);
    }
    CHECK(reached < 140 && sent_by(lua.owner) == 128);
    CHECK(!refusal_log.empty() && refusal_log.back().find("128") != std::string::npos);
}

static void test_unsend(const EmfApi& t) {
    fresh();
    g_side = KILN_SIDE_SERVER;
    players = {5, 6};
    EmfMarker m = marker("k");
    // a marker sent to one player: unsend takes it off that game
    CHECK(one(t, 5, m) == 1);
    CHECK(t.marker_unsend(5, "k") == 1 && !sent_of(mod_a, 5, "k"));
    CHECK(t.marker_unsend(5, "k") == 0);  // nothing there
    // a marker sent to every player, taken off one game: hidden for that player only
    CHECK(one(t, 0, m) == 2);
    calls.clear();
    CHECK(t.marker_unsend(6, "k") == 1 && suppressed.count({mod_a.owner, 6, "k"}) && sent_of(mod_a, 0, "k"));
    CHECK(count_calls("suppress 6 k") == 1);
    // sent to every player again: that player sees it again (EMF sends it to every game)
    CHECK(one(t, 0, m) >= 1 && !suppressed.count({mod_a.owner, 6, "k"}));
    // one player's own value, then every player's: the newer one shows for that player too (as EMF's games do)
    EmfMarker own = marker("k");
    own.text = "own";
    CHECK(one(t, 5, own) == 1 && sent_of(mod_a, 5, "k"));
    CHECK(one(t, 0, m) == 2 && !sent_of(mod_a, 5, "k"));
    // unsend for every player (0): the broadcast and every player's own value and hiding go
    CHECK(one(t, 6, own) == 1);
    CHECK(t.marker_unsend(5, "k") == 1);  // hides the broadcast for 5
    calls.clear();
    CHECK(t.marker_unsend(0, "k") == 2);
    CHECK(sent.empty() && suppressed.empty());
    CHECK(t.marker_unsend(0, "k") == 0);
    CHECK(t.marker_unsend(0, nullptr) == 0 && t.marker_unsend(0, "bad id") == 0);
    // a player left (kiln.markers drops their layer: PEER_LEFT): their entries go here too
    CHECK(one(t, 6, own) == 1);
    CHECK(sub_changed != nullptr);
    if (sub_changed) {
        KilnMarkerChange c{};
        c.size = sizeof c;
        c.kind = KILN_MARKER_CLEARED;
        c.audience = KILN_MARKER_SERVER_TARGET;
        c.recipient = 6;
        c.cause = KILN_MARKER_PEER_LEFT;
        std::snprintf(c.owner, sizeof c.owner, "emf:mod-a");
        sent.erase({mod_a.owner, 6, "k"});
        players.erase(6);
        sub_changed(&c, sub_user);
    }
    calls.clear();
    CHECK(one(t, 0, m) == 1 && count_calls("unsend 6") == 0);
    // the server world stops: kiln.markers clears what was sent; the mod's entries go here too
    markers_world_stopping(EMF_WORLD_SERVER);
    sent.clear();
    calls.clear();
    CHECK(t.marker_unsend(0, "k") == 0 && count_calls("unsend") == 0);
}

static void test_server_catalog(const EmfApi& t) {
    fresh();
    g_side = KILN_SIDE_SERVER;
    players = {5};
    // a dedicated server takes the catalog from the first verified Kiln player: until then no map icon, the rest goes
    catalog.clear();
    EmfMarker m = marker("s", EMF_MARKER_WORLD | EMF_MARKER_GAME_MAP);
    CHECK(one(t, 5, m) == 1 && sent_of(mod_a, 5, "s")->surfaces == KILN_MARKER_WORLD);
    fill_catalog();
    markers_after_step(reinterpret_cast<EmfWorld*>(worlds[0]), EMF_WORLD_SERVER);
    CHECK(sent_of(mod_a, 5, "s")->surfaces == (KILN_MARKER_MAP | KILN_MARKER_WORLD));
    // a retry that kiln.markers refuses (the player went) keeps the entry: marker_unsend still reaches the record sent
    catalog.clear();
    EmfMarker gone = marker("gone", EMF_MARKER_WORLD | EMF_MARKER_GAME_MAP);
    CHECK(one(t, 5, gone) == 1);
    players.clear();
    fill_catalog();
    markers_after_step(reinterpret_cast<EmfWorld*>(worlds[0]), EMF_WORLD_SERVER);
    CHECK(sent_of(mod_a, 5, "gone") && sent_of(mod_a, 5, "gone")->surfaces == KILN_MARKER_WORLD);
    players = {5};
    calls.clear();
    CHECK(t.marker_unsend(5, "gone") == 1 && !sent_of(mod_a, 5, "gone") && count_calls("send ") == 0);
    // an unsent marker is not sent again later
    catalog.clear();
    EmfMarker n = marker("u", EMF_MARKER_WORLD | EMF_MARKER_GAME_MAP);
    CHECK(one(t, 5, n) == 1 && t.marker_unsend(5, "u") == 1);
    fill_catalog();
    markers_after_step(reinterpret_cast<EmfWorld*>(worlds[0]), EMF_WORLD_SERVER);
    CHECK(!sent_of(mod_a, 5, "u"));
}

static int test_no_service() {
    service_present = false;
    g_side = KILN_SIDE_CLIENT;
    g_caller = &mod_a;
    EmfApi t{};
    fill_markers(&t);
    EmfMarker m = marker("m");
    EmfMapMarkerType mt{};
    CHECK(!t.marker_set(&m) && !t.marker_remove("m") && t.marker_clear() == 0 && t.marker_types(&mt, 1) == 0);
    CHECK(t.marker_send(0, &m, 1) == 0 && t.marker_unsend(0, "m") == 0);
    CHECK(!unsupported_log.empty() && lookups == 1);
    std::printf("markers_test (no kiln.markers): %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}

int main(int argc, char** argv) {
    KilnApi api{};
    api.on_tick_thread = &on_tick_thread;
    api.world = &world_of;
    api.service = &service;
    g_api = &api;
    fake.header.size = sizeof fake;
    fake.header.version = KILN_MARKERS_VERSION;
    fake.bind_owner = &f_bind;
    fake.release_owner = &f_release_owner;
    fake.catalog_count = &f_catalog_count;
    fake.catalog_at = &f_catalog_at;
    fake.catalog_resolve = &f_resolve;
    fake.world_epoch = &f_world_epoch;
    fake.capture_anchor = &f_capture;
    fake.release_anchor = &f_release_anchor;
    fake.set_local = &f_set_local;
    fake.remove_local = &f_remove_local;
    fake.clear_local = &f_clear_local;
    fake.send = &f_send;
    fake.unsend = &f_unsend;
    fake.clear_sent = &f_clear_sent;
    fake.suppress_sent = &f_suppress;
    fake.subscribe = &f_subscribe;
    fake.unsubscribe = &f_unsubscribe;
    mod_a.id = "mod-a";
    mod_a.owner = 11;
    mod_b.id = "mod-b";
    mod_b.owner = 12;
    lua.id = "lua";
    lua.owner = 13;
    if (argc > 1 && std::string(argv[1]) == "no-service") return test_no_service();
    EmfApi t{};
    fill_markers(&t);
    CHECK(t.marker_set && t.marker_remove && t.marker_clear && t.marker_types && t.marker_send && t.marker_unsend);
    if (!t.marker_set || !t.marker_send) return 1;
    test_set(t);
    test_refusals(t);
    test_ids_and_text(t);
    test_ownership(t);
    test_follow(t);
    test_degrade(t);
    test_types(t);
    test_send(t);
    test_unsend(t);
    test_server_catalog(t);
    markers_shutdown();
    std::printf("markers_test: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
