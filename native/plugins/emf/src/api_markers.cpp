// SPDX-License-Identifier: MIT
// EMF v33 markers (marker_set, marker_remove, marker_clear, marker_types, marker_send, marker_unsend) and v47's marker
// bar, translated onto Kiln's own marker service kiln.markers (native/core/abi/services/kiln_markers.h;
// docs/kiln/services/markers.md). Since 0.2.0-dev.31.13. No engine code or game memory here: kiln.markers draws, sends,
// follows and keeps its limits; this file keeps EMF's contract on top of it.
//
// What maps onto what (docs/kiln/emf-parity.md has the user-facing rows):
// - Each EMF mod is one kiln.markers publisher: its KilnOwner ("emf:<id>", mods.cpp) bound to the namespace
//   "emf:<id>". Two mods never share keys. EMF's Lua host is one EMF mod ("lua"); its scripts' ids are already
//   "<script>:<id>" and it removes a script's markers itself when the script goes, as on EMF.
// - marker_set / marker_remove / marker_clear: the local store (set_local, remove_local, clear_local) of the client
//   world. The dedicated server draws nothing (false, as EMF). marker_send / marker_unsend: the server layers (send,
//   unsend, suppress_sent) of the server world; player 0 = the broadcast layer.
// - Surfaces: EMF_MARKER_WORLD = KILN_MARKER_WORLD (a diamond and the text over the point), EMF_MARKER_EDGE_ARROW =
//   KILN_MARKER_COMPASS (Kiln's strip: the direction and the distance), EMF_MARKER_GAME_MAP = KILN_MARKER_MAP with an
//   icon of Kiln's catalog: `game_icon` when the catalog has it for the map, else one of the game's own pin types by
//   `shape` (diamond, circle, square = the first, second, third custom-pin type by id); EMF_MARKER_NONE gets no map icon
//   (Kiln's map has no text). EMF_MARKER_DISTANCE and EMF_MARKER_MINIMAP have no surface of their own.
// - The v47 bar (EMF_MARKER_BAR, `bar` 0..1) is ten cells after the text, "Rex [########..]": Kiln's labels have text
//   and no bar.
// - `entity`: a lease from capture_anchor on that world (the server's for sends), released once the record holds its
//   own reference. An entity the world does not know yet is drawn at x, y, z and tried again once a second (EMF: "where
//   it is drawn until the game knows the entity"); a set from another thread than the client world's is placed at
//   once and follows from that world's next step.
// - A capability that is off (kiln.markers refuses a spec whole) drops only its surface; the map surface waits for
//   the icon catalog (a dedicated server gets it from the first verified Kiln player) and is added once it is ready.
// - Ids: EMF 0.6.22's ids may hold ','; Kiln's keys may not. key_of() is the identity for every id without a comma
//   (and not ending in its suffix), else commas become '.' and "::" + the 48-bit comma mask in hex is appended.
// - Limits are kiln.markers' (128 markers per mod and source, 512 local, 128 sent per mod across players, ...); a call
//   over one answers EMF's failure (false, or that marker left out of marker_send's count), logged once per kind.
// - marker_send / marker_unsend off the server's tick thread are kept in order and carried out at the next server
//   step (they answer 0 then); a joined client's game has no server world: 0 at once.
// - Ownership: world stop clears that world's markers of every mod (kiln.markers clears its records itself); the
//   plugin's unload releases every mod's owner (release_owner is mandatory before a native publisher goes).
#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <deque>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "emf_internal.h"
#include "services/kiln_markers.h"

namespace kemf {

namespace {

constexpr size_t kIdMax = 48;           // EMF's id: 1-48 of A-Z a-z 0-9 _ . - : ,
constexpr size_t kTextMax = 64;         // EMF cuts the text at 64 bytes
constexpr uint32_t kPerSend = 256;      // EMF: at most 256 per marker_send
constexpr double kCoordMax = 100000.0;  // EMF: finite, within 100 km of the origin
constexpr uint64_t kRetryMs = 1000;     // a followed entity the world does not know yet: tried again once a second
constexpr size_t kQueueMax = 1024;      // calls waiting for the server's tick thread
constexpr int kBarCells = 10;
constexpr const char* kNoService = "kiln.markers is not available";

// An EMF marker after EMF's checks, with its Kiln key and final text (cut, cleaned, bar).
struct Want {
    std::string id, key, text;
    uint32_t entity = 0;
    double x = 0, y = 0, z = 0;
    float height = 0, max_distance = 0;
    uint32_t rgba = 0, flags = 0, game_icon = 0;
    uint8_t shape = 0;
};
struct Retry {
    bool follow = false, map = false;
    uint64_t at = 0;
    bool any() const { return follow || map; }
};
struct Local {
    Want w;
    Retry retry;
};
struct Sent {
    bool suppress = false;  // marker_unsend of a broadcast marker for one player: suppress_sent
    Want w;
    uint32_t reach = 0;     // players the last change reached (an identical send answers it again)
    Retry retry;
};
struct ModMarkers {
    Mod* mod = nullptr;
    std::map<std::string, Local> local;                       // EMF id -> marker
    std::map<std::pair<uint32_t, std::string>, Sent> sent;   // (player, EMF id); player 0 = every player
};
struct Op {  // a marker_send / marker_unsend that waits for the server's tick thread
    Mod* mod = nullptr;
    bool send = false;
    uint32_t player = 0;
    Want w;          // send
    std::string id;  // unsend
};
struct Icon {
    uint32_t id = 0, flags = 0, surfaces = 0;
    std::string alias;
};

std::mutex g_mu;  // everything below. Held across kiln.markers calls: none of them calls back on the calling thread.
bool g_looked = false;
const KilnMarkersApi* g_km = nullptr;
std::map<Mod*, ModMarkers> g_state;  // a mod is here once its owner is bound
std::deque<Op> g_ops;
KilnHandle g_sub = 0;
uint64_t g_icons_gen = ~0ull;
std::vector<Icon> g_icons;           // the catalog's rows, by id
std::atomic<bool> g_work{false};     // retries or queued calls exist: markers_after_step has something to do

// ---------------------------------------------------------------------------------------------- EMF's rules ----
bool id_ok(const char* id) {
    if (!id) return false;
    const size_t n = std::strlen(id);
    if (n == 0 || n > kIdMax) return false;
    for (size_t i = 0; i < n; ++i) {
        const char c = id[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-' ||
              c == ':' || c == ','))
            return false;
    }
    return true;
}

bool finite_within(double v, double limit) { return std::isfinite(v) && std::fabs(v) <= limit; }

// The id as a Kiln key: [A-Za-z0-9_.:-], 1-64 bytes, injective over EMF's ids (see the file comment).
bool looks_encoded(std::string_view s) {
    if (s.size() < 14) return false;
    const std::string_view t = s.substr(s.size() - 14);
    if (t[0] != ':' || t[1] != ':') return false;
    for (char c : t.substr(2))
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}
std::string key_of(std::string_view id) {
    std::string k(id);
    uint64_t mask = 0;
    for (size_t i = 0; i < k.size(); ++i)
        if (k[i] == ',') {
            k[i] = '.';
            mask |= uint64_t{1} << i;
        }
    if (mask || looks_encoded(id)) {
        char suffix[16];
        std::snprintf(suffix, sizeof suffix, "::%012llx", static_cast<unsigned long long>(mask));
        k += suffix;
    }
    return k;
}

// EMF's cut (at most n bytes, never inside a UTF-8 character), then Kiln's text rules: strict UTF-8 (broken bytes are
// left out), one line (U+0000-001F, U+007F-009F become spaces).
std::string clean_text(const char* text) {
    std::string s = text ? std::string(text) : std::string();
    if (s.size() > kTextMax) {
        size_t end = kTextMax;
        while (end > 0 && (static_cast<unsigned char>(s[end]) & 0xC0) == 0x80) --end;
        s.resize(end);
    }
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        const auto c = static_cast<unsigned char>(s[i]);
        size_t n = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 0;
        uint32_t cp = n == 1 ? c : n == 2 ? (c & 0x1Fu) : n == 3 ? (c & 0x0Fu) : (c & 0x07u);
        bool ok = n != 0 && i + n <= s.size();
        for (size_t j = 1; ok && j < n; ++j) {
            const auto d = static_cast<unsigned char>(s[i + j]);
            ok = (d & 0xC0) == 0x80;
            cp = (cp << 6) | (d & 0x3Fu);
        }
        if (ok) {  // overlong forms, surrogates and values past U+10FFFF are not UTF-8
            const uint32_t least[5] = {0, 0, 0x80, 0x800, 0x10000};
            ok = cp >= least[n] && cp <= 0x10FFFF && !(cp >= 0xD800 && cp <= 0xDFFF);
        }
        if (!ok) {
            ++i;
            continue;
        }
        if (cp < 0x20 || (cp >= 0x7F && cp <= 0x9F)) out += ' ';
        else out.append(s, i, n);
        i += n;
    }
    return out;
}

// nullptr when the marker passes EMF's checks (MarkerStore::set and the v47 bar); `w` then holds it.
const char* check(const EmfMarker* m, Want* w) {
    if (!m) return "no marker";
    if (m->size < sizeof(EmfMarker)) return "bad numbers (a size smaller than v33's EmfMarker)";
    if (!id_ok(m->id)) return "a bad id (1-48 of A-Z a-z 0-9 _ . - : ,)";
    if (!finite_within(m->x, kCoordMax) || !finite_within(m->y, kCoordMax) || !finite_within(m->z, kCoordMax) ||
        !finite_within(m->height, 1000.0) || !finite_within(m->max_distance, kCoordMax) || m->max_distance < 0 ||
        m->shape > EMF_MARKER_NONE)
        return "bad numbers (coordinates not finite or beyond 100 km, a bad height, max_distance or shape)";
    if ((m->flags & EMF_MARKER_BAR) && !std::isfinite(m->bar)) return "bad numbers (the bar is not a number)";
    w->id = m->id;
    w->key = key_of(w->id);
    w->entity = m->entity;
    w->x = m->x;
    w->y = m->y;
    w->z = m->z;
    w->height = m->height;
    w->max_distance = m->max_distance;
    w->rgba = m->rgba;
    w->flags = m->flags;
    w->game_icon = m->game_icon;
    w->shape = m->shape;
    w->text = clean_text(m->text);
    if (m->flags & EMF_MARKER_BAR) {
        const int filled = static_cast<int>(std::lround(std::clamp(m->bar, 0.0f, 1.0f) * kBarCells));
        std::string bar = "[" + std::string(filled, '#') + std::string(kBarCells - filled, '.') + "]";
        w->text = w->text.empty() ? bar : w->text + " " + bar;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------------------------- kiln.markers ---
const KilnMarkersApi* km() {  // g_mu held. Looked up once: services are published before any mod loads.
    if (!g_looked) {
        g_looked = true;
        const auto* t = g_api && g_api->service
                            ? static_cast<const KilnMarkersApi*>(g_api->service(kstr(KILN_MARKERS_SERVICE), KILN_MARKERS_VERSION))
                            : nullptr;
        g_km = t && t->header.size >= sizeof(KilnMarkersApi) ? t : nullptr;
    }
    return g_km;
}

bool on_tick() { return g_api && g_api->on_tick_thread && g_api->on_tick_thread(); }
bool server_world_here() { return g_api && g_api->world && g_api->world(KILN_WORLD_SERVER) != 0; }
KilnMarkerResult result() {
    KilnMarkerResult r{};
    r.size = sizeof r;
    return r;
}
std::string ns_of(const Mod* m) { return "emf:" + m->id; }

void KILN_CALL on_changed(const KilnMarkerChange* c, void*);

// The mod's state, binding its owner on first use; nullptr (logged) when it cannot be bound.
ModMarkers* state(Mod* m, const KilnMarkersApi* k, const char* fn) {
    if (auto it = g_state.find(m); it != g_state.end()) return &it->second;
    if (!m->owner) {
        refuse(m, fn, "the mod has no Kiln owner", true);
        return nullptr;
    }
    const std::string ns = ns_of(m);
    const KilnResult rc = k->bind_owner(m->owner, kstr(ns));
    if (rc != KILN_OK) {
        refuse(m, fn,
               rc == KILN_ERR_BUFFER_TOO_SMALL ? "64 mods already have markers in this game (Kiln's limit)"
                                               : "its marker namespace could not be bound",
               true);
        return nullptr;
    }
    if (!g_sub && k->subscribe(g_owner, &on_changed, nullptr, nullptr, nullptr, &g_sub) != KILN_OK) g_sub = 0;
    ModMarkers& s = g_state[m];
    s.mod = m;
    return &s;
}
ModMarkers* existing(Mod* m) {
    auto it = g_state.find(m);
    return it == g_state.end() ? nullptr : &it->second;
}

// The catalog's rows (g_mu held); refreshed when its generation changes. False while it warms up.
bool icons_ready(const KilnMarkersApi* k) {
    uint32_t n = 0;
    uint64_t gen = 0;
    if (k->catalog_count(&n, &gen) != KILN_OK || !n) {
        g_icons.clear();
        g_icons_gen = ~0ull;
        return false;
    }
    if (gen != g_icons_gen) {
        g_icons.clear();
        for (uint32_t i = 0; i < n; ++i) {
            KilnMarkerIcon icon{};
            icon.size = sizeof icon;
            if (k->catalog_at(i, &icon) != KILN_OK) continue;
            g_icons.push_back({icon.id, icon.flags, icon.surfaces, icon.alias});
        }
        std::sort(g_icons.begin(), g_icons.end(), [](const Icon& a, const Icon& b) { return a.id < b.id; });
        g_icons_gen = gen;
    }
    return !g_icons.empty();
}

// The map icon of an EMF_MARKER_GAME_MAP marker: 0 = none (text only, or no map type), *pending while the catalog warms up.
uint32_t map_icon(const KilnMarkersApi* k, Mod* m, const char* fn, const Want& w, bool* pending) {
    if (!icons_ready(k)) {
        *pending = true;
        return 0;
    }
    if (w.game_icon) {
        for (const Icon& i : g_icons)
            if (i.id == w.game_icon && (i.surfaces & KILN_MARKER_MAP)) return i.id;
        refuse(m, fn, "game_icon is no map icon on Kiln (a fast-travel or unknown type): the shape's pin instead", true);
    }
    if (w.shape == EMF_MARKER_NONE) return 0;  // text only: Kiln's map draws no text
    std::vector<uint32_t> pins, any;
    for (const Icon& i : g_icons) {
        if (!(i.surfaces & KILN_MARKER_MAP)) continue;
        any.push_back(i.id);
        if (i.flags & KILN_MARKER_ICON_CUSTOM) pins.push_back(i.id);
    }
    const std::vector<uint32_t>& from = pins.empty() ? any : pins;
    return from.empty() ? 0 : from[w.shape % from.size()];
}

enum class Outcome { Ok, Refused };
struct Put {
    Outcome o = Outcome::Refused;
    uint32_t queued = 0, affected = 0;
    bool stored = false;  // a Kiln record holds it (false: no surface to draw it on)
    Retry retry;
};

// Sets the marker in the client world's store (server false) or sends it to `player` of the server world.
Put put(const KilnMarkersApi* k, ModMarkers& st, const Want& w, bool server, uint32_t player, const char* fn) {
    Mod* m = st.mod;
    Put out;
    const uint64_t now = now_ms();
    uint32_t base = 0;
    if (w.flags & EMF_MARKER_WORLD) base |= KILN_MARKER_WORLD;
    if (w.flags & EMF_MARKER_EDGE_ARROW) base |= KILN_MARKER_COMPASS;
    uint32_t icon = 0;
    if (w.flags & EMF_MARKER_GAME_MAP) {
        icon = map_icon(k, m, fn, w, &out.retry.map);
        if (icon) base |= KILN_MARKER_MAP;
    }
    uint32_t dropped = 0;  // surfaces whose capability is off
    bool no_follow = false, lease_ended = false;
    const uint32_t kind = server ? KILN_WORLD_SERVER : KILN_WORLD_CLIENT;
    auto remove_record = [&] {
        KilnMarkerResult r = result();
        if (server) k->unsend(m->owner, player, kstr(w.key), &r);
        else k->remove_local(m->owner, kstr(w.key), &r);
    };
    for (int attempt = 0; attempt < 6; ++attempt) {
        const uint32_t surfaces = base & ~dropped;
        if (!surfaces) {
            if (base) {
                refuse(m, fn, "every surface it asks for is off on this game build", true);
                return out;
            }
            remove_record();  // nothing to draw it on (yet): kept as the mod's marker, Kiln's record goes
            out.o = Outcome::Ok;
            return out;
        }
        KilnMarkerAnchor anchor = 0;
        out.retry.follow = false;
        if (w.entity && !no_follow) {
            uint64_t epoch = 0;
            k->world_epoch(kind, &epoch);
            KilnMarkerAnchorResult a{};
            a.size = sizeof a;
            const KilnResult rc = k->capture_anchor(m->owner, kind, epoch, w.entity, &a);
            if (rc == KILN_OK && a.status == KILN_MARKER_ACCEPTED && a.anchor) {
                anchor = a.anchor;
            } else if (rc == KILN_ERR_UNAVAILABLE) {
                no_follow = true;
                refuse(m, fn, "following an entity is off on this game build: the marker stays at x, y, z", true);
            } else {
                // not known (yet), or another thread than that world's: at x, y, z now, followed later
                out.retry.follow = true;
                out.retry.at = rc == KILN_ERR_WRONG_THREAD ? now : now + kRetryMs;
            }
        }
        KilnMarkerSpec spec{};
        spec.size = sizeof spec;
        spec.surfaces = surfaces;
        spec.key = kstr(w.key);
        spec.position = {w.x, w.y, w.z};
        spec.follow = anchor;
        spec.height = std::clamp(w.height, -128.0f, 128.0f);
        spec.max_distance = w.max_distance;
        spec.icon_type = (surfaces & KILN_MARKER_MAP) ? icon : 0;
        spec.rgba = w.rgba ? w.rgba : 0xffffffffu;
        spec.text = kstr(w.text);
        KilnMarkerResult r = result();
        const KilnResult rc = server ? k->send(m->owner, player, &spec, &r) : k->set_local(m->owner, &spec, &r);
        if (anchor) k->release_anchor(m->owner, anchor);  // the record holds its own reference
        if (rc == KILN_ERR_UNAVAILABLE) {
            const std::string row = r.reason;
            if (row == "markers.map") dropped |= KILN_MARKER_MAP;
            else if (row == "markers.compass") dropped |= KILN_MARKER_COMPASS;
            else if (row == "markers.world") dropped |= KILN_MARKER_WORLD;
            else if (row == "markers.anchors.client" || row == "markers.anchors.server") no_follow = true;
            else {
                unsupported(m, fn, row.empty() ? kNoService : row.c_str());
                return out;
            }
            if (row.rfind("markers.anchors", 0) != 0) refuse(m, fn, ("a surface is off on this game build: " + row).c_str(), true);
            continue;
        }
        if (rc != KILN_OK) {
            refuse(m, fn, r.reason[0] ? r.reason : "refused by kiln.markers", true);
            return out;
        }
        if (r.status == KILN_MARKER_CATALOG_NOT_READY) {
            dropped |= KILN_MARKER_MAP;
            out.retry.map = true;
            continue;
        }
        if (r.status == KILN_MARKER_INVALID_ANCHOR) {  // the lease ended between capture and set: at x, y, z, again later
            no_follow = lease_ended = true;
            continue;
        }
        if (!r.committed) {
            if (!(server && r.status == KILN_MARKER_NOT_FOUND)) {  // a player without Kiln (or gone): nothing, quietly
                const std::string why = std::string(r.status == KILN_MARKER_FULL ? "Kiln's limit: " : "") + r.reason;
                refuse(m, fn, why.c_str(), true);
            }
            return out;
        }
        if (lease_ended) {
            out.retry.follow = true;
            out.retry.at = now + kRetryMs;
        }
        out.o = Outcome::Ok;
        out.stored = true;
        out.queued = r.queued;
        out.affected = r.affected;
        return out;
    }
    refuse(m, fn, "kiln.markers refused every way of drawing it", true);
    return out;
}

void note_work() {  // g_mu held
    bool any = !g_ops.empty();
    for (const auto& [mod, st] : g_state) {
        for (const auto& [id, l] : st.local) any |= l.retry.any();
        for (const auto& [key, s] : st.sent) any |= !s.suppress && s.retry.any();
        if (any) break;
    }
    g_work.store(any);
}

// ---------------------------------------------------------------------------------------------- server layers --
uint32_t send_one(const KilnMarkersApi* k, ModMarkers& st, uint32_t player, const Want& w) {
    Mod* m = st.mod;
    if (!player) {
        // every player: one player's own value or hiding of that id goes, the newer value shows (as on EMF's games)
        for (auto it = st.sent.begin(); it != st.sent.end();) {
            if (it->first.first && it->first.second == w.id) {
                KilnMarkerResult r = result();
                k->unsend(m->owner, it->first.first, kstr(w.key), &r);
                it = st.sent.erase(it);
            } else {
                ++it;
            }
        }
    }
    const Put p = put(k, st, w, true, player, "marker_send");
    if (p.o != Outcome::Ok) return 0;
    Sent& e = st.sent[{player, w.id}];
    const uint32_t reach = p.stored ? (p.affected ? p.queued : std::max(p.queued, e.suppress ? 0u : e.reach)) : 0;
    e.suppress = false;
    e.w = w;
    e.reach = reach;
    e.retry = p.retry;
    return reach;
}

uint32_t unsend_one(const KilnMarkersApi* k, ModMarkers& st, uint32_t player, const std::string& id) {
    Mod* m = st.mod;
    const std::string key = key_of(id);
    uint32_t reach = 0;
    if (!player) {  // from every game: the broadcast and every player's own value or hiding
        for (auto it = st.sent.begin(); it != st.sent.end();) {
            if (it->first.second == id) {
                KilnMarkerResult r = result();
                if (k->unsend(m->owner, it->first.first, kstr(key), &r) == KILN_OK && r.committed) reach = std::max(reach, r.queued);
                it = st.sent.erase(it);
            } else {
                ++it;
            }
        }
        return reach;
    }
    auto own = st.sent.find({player, id});
    if (own != st.sent.end() && own->second.suppress) return 0;  // hidden from that game already
    KilnMarkerResult r = result();
    if (st.sent.count({0, id})) {
        // a marker every player has: hidden from this player's game (their own value, if any, goes with it)
        if (k->suppress_sent(m->owner, player, kstr(key), &r) == KILN_OK && r.committed) {
            Sent& e = st.sent[{player, id}];
            e = Sent{};
            e.suppress = true;
            return r.queued;
        }
        if (own != st.sent.end()) st.sent.erase(own);
        return 0;
    }
    if (own == st.sent.end()) return 0;
    if (k->unsend(m->owner, player, kstr(key), &r) == KILN_OK && r.committed) reach = r.queued;
    st.sent.erase(own);
    return reach;
}

void drain_ops(const KilnMarkersApi* k) {  // g_mu held, on the server's tick thread
    while (!g_ops.empty()) {
        Op op = std::move(g_ops.front());
        g_ops.pop_front();
        ModMarkers* st = existing(op.mod);
        if (!st) continue;
        if (op.send) send_one(k, *st, op.player, op.w);
        else unsend_one(k, *st, op.player, op.id);
    }
}

void KILN_CALL on_changed(const KilnMarkerChange* c, void*) {
    // a player left: kiln.markers dropped what was sent to them only; their entries go here too
    if (!c || c->size < sizeof(KilnMarkerChange) || c->cause != KILN_MARKER_PEER_LEFT || c->audience != KILN_MARKER_SERVER_TARGET ||
        !c->recipient)
        return;
    std::lock_guard lock(g_mu);
    for (auto& [mod, st] : g_state) {
        if (ns_of(mod) != c->owner) continue;
        std::erase_if(st.sent, [&](const auto& kv) { return kv.first.first == c->recipient; });
    }
}

// ---------------------------------------------------------------------------------------------- the API --------
// In the API functions themselves (never in a helper): the return address names the calling mod.
bool api_marker_set(const EmfMarker* mk) {
    Mod* m = caller_mod(_ReturnAddress());
    note_call("marker_set");
    if (!m) return false;
    if (g_side == KILN_SIDE_SERVER) {
        refuse(m, "marker_set", "the dedicated server draws nothing; marker_send gives players' games markers", true);
        return false;
    }
    Want w;
    if (const char* why = check(mk, &w)) {
        refuse(m, "marker_set", why, true);
        return false;
    }
    std::lock_guard lock(g_mu);
    const KilnMarkersApi* k = km();
    if (!k) {
        unsupported(m, "marker_set", kNoService);
        return false;
    }
    ModMarkers* st = state(m, k, "marker_set");
    if (!st) return false;
    const Put p = put(k, *st, w, false, 0, "marker_set");
    if (p.o != Outcome::Ok) return false;
    Local& l = st->local[w.id];
    l.w = std::move(w);
    l.retry = p.retry;
    note_work();
    return true;
}

bool api_marker_remove(const char* id) {
    Mod* m = caller_mod(_ReturnAddress());
    note_call("marker_remove");
    if (!m || !id || g_side == KILN_SIDE_SERVER) return false;
    std::lock_guard lock(g_mu);
    const KilnMarkersApi* k = km();
    if (!k) {
        unsupported(m, "marker_remove", kNoService);
        return false;
    }
    ModMarkers* st = existing(m);
    if (!st) return false;
    auto it = st->local.find(id);
    if (it == st->local.end()) return false;  // the mod has no such marker
    KilnMarkerResult r = result();
    k->remove_local(m->owner, kstr(it->second.w.key), &r);
    st->local.erase(it);
    return true;
}

uint32_t api_marker_clear() {
    Mod* m = caller_mod(_ReturnAddress());
    note_call("marker_clear");
    if (!m || g_side == KILN_SIDE_SERVER) return 0;
    std::lock_guard lock(g_mu);
    const KilnMarkersApi* k = km();
    if (!k) {
        unsupported(m, "marker_clear", kNoService);
        return 0;
    }
    ModMarkers* st = existing(m);
    if (!st || st->local.empty()) return 0;
    const auto n = static_cast<uint32_t>(st->local.size());
    KilnMarkerResult r = result();
    k->clear_local(m->owner, &r);
    st->local.clear();
    return n;
}

// The catalog's types usable on Kiln's map (id, EMF_MAP_TYPE_CUSTOM for the game's pin types, the alias as the name):
// how many there are, `out` filled up to `cap`. 0 while the catalog warms up (a dedicated server: until the first
// verified Kiln player). Fast-travel types are not listed: Kiln refuses them on the map.
uint32_t api_marker_types(EmfMapMarkerType* out, uint32_t cap) {
    Mod* m = caller_mod(_ReturnAddress());
    note_call("marker_types");
    std::lock_guard lock(g_mu);
    const KilnMarkersApi* k = km();
    if (!k) {
        unsupported(m, "marker_types", kNoService);
        return 0;
    }
    if (!icons_ready(k)) return 0;
    uint32_t n = 0;
    for (const Icon& i : g_icons) {
        if (!(i.surfaces & KILN_MARKER_MAP)) continue;
        if (out && n < cap) {
            EmfMapMarkerType t{};
            t.size = sizeof t;
            t.id = i.id;
            t.flags = (i.flags & KILN_MARKER_ICON_CUSTOM) ? EMF_MAP_TYPE_CUSTOM : 0u;
            copy_cstr(t.name, sizeof t.name, i.alias);
            out[n] = t;
        }
        ++n;
    }
    return n;
}

uint32_t api_marker_send(uint32_t player, const EmfMarker* markers, uint32_t count) {
    Mod* m = caller_mod(_ReturnAddress());
    note_call("marker_send");
    if (!m || !markers || !count) return 0;
    if (count > kPerSend) {
        refuse(m, "marker_send", "more than 256 markers in one call: the first 256 are sent", true);
        count = kPerSend;
    }
    std::vector<Want> wants;
    wants.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        Want w;
        if (const char* why = check(&markers[i], &w)) {
            refuse(m, "marker_send", (std::string("a marker was left out: ") + why).c_str(), true);
            continue;
        }
        wants.push_back(std::move(w));
    }
    if (wants.empty()) return 0;
    if (!server_world_here()) {
        refuse(m, "marker_send", "no server world here (the host's game or the dedicated server sends markers)", true);
        return 0;
    }
    std::lock_guard lock(g_mu);
    const KilnMarkersApi* k = km();
    if (!k) {
        unsupported(m, "marker_send", kNoService);
        return 0;
    }
    ModMarkers* st = state(m, k, "marker_send");
    if (!st) return 0;
    if (!on_tick()) {  // carried out at the next server step, in order with the calls before it
        if (g_ops.size() + wants.size() > kQueueMax) {
            refuse(m, "marker_send", "1024 marker calls already wait for the server's step: left out", true);
            return 0;
        }
        for (Want& w : wants) g_ops.push_back(Op{m, true, player, std::move(w), {}});
        note_work();
        return 0;
    }
    drain_ops(k);
    uint32_t reach = 0;
    for (const Want& w : wants) reach = std::max(reach, send_one(k, *st, player, w));
    note_work();
    return reach;
}

uint32_t api_marker_unsend(uint32_t player, const char* id) {
    Mod* m = caller_mod(_ReturnAddress());
    note_call("marker_unsend");
    if (!m || !id_ok(id) || !server_world_here()) return 0;
    std::lock_guard lock(g_mu);
    const KilnMarkersApi* k = km();
    if (!k) {
        unsupported(m, "marker_unsend", kNoService);
        return 0;
    }
    ModMarkers* st = existing(m);
    if (!st) return 0;
    if (!on_tick()) {
        if (g_ops.size() >= kQueueMax) {
            refuse(m, "marker_unsend", "1024 marker calls already wait for the server's step: left out", true);
            return 0;
        }
        g_ops.push_back(Op{m, false, player, {}, id});
        note_work();
        return 0;
    }
    drain_ops(k);
    const uint32_t reach = unsend_one(k, *st, player, id);
    note_work();
    return reach;
}

}  // namespace

void fill_markers(EmfApi* t) {
    t->marker_set = &api_marker_set;
    t->marker_remove = &api_marker_remove;
    t->marker_clear = &api_marker_clear;
    t->marker_types = &api_marker_types;
    t->marker_send = &api_marker_send;
    t->marker_unsend = &api_marker_unsend;
}

// After each step of a world, on its thread: the server's queued calls and the retries of that world's markers
// (a followed entity that was not known yet, the map once the icon catalog is ready).
void markers_after_step(EmfWorld*, uint8_t kind) {
    if (!g_work.load(std::memory_order_relaxed)) return;
    std::lock_guard lock(g_mu);
    const KilnMarkersApi* k = g_km;
    if (!k) return;
    const uint64_t now = now_ms();
    bool catalog_checked = false, catalog = false;
    auto due = [&](const Retry& r) {
        if (r.follow && now >= r.at) return true;
        if (!r.map) return false;
        if (!catalog_checked) {  // the map: once the catalog is there
            catalog = icons_ready(k);
            catalog_checked = true;
        }
        return catalog;
    };
    if (kind == EMF_WORLD_SERVER) {
        if (on_tick()) drain_ops(k);
        for (auto& [mod, st] : g_state)
            for (auto it = st.sent.begin(); it != st.sent.end();) {
                Sent& s = it->second;
                if (s.suppress || !due(s.retry)) {
                    ++it;
                    continue;
                }
                const Put p = put(k, st, s.w, true, it->first.first, "marker_send");
                // refused (the player went, a limit): the record sent before stays where it was, and so does its entry
                // (a later marker_unsend still takes it off); it is not tried again
                s.retry = p.o == Outcome::Ok ? p.retry : Retry{};
                ++it;
            }
    } else if (kind == EMF_WORLD_CLIENT) {
        for (auto& [mod, st] : g_state)
            for (auto& [id, l] : st.local) {
                if (!due(l.retry)) continue;
                const Put p = put(k, st, l.w, false, 0, "marker_set");
                l.retry = p.o == Outcome::Ok ? p.retry : Retry{};
            }
    }
    note_work();
}

void markers_world_stopping(uint8_t kind) {
    std::lock_guard lock(g_mu);
    for (auto& [mod, st] : g_state) {
        if (kind == EMF_WORLD_CLIENT) st.local.clear();  // kiln.markers clears the client world's records itself
        if (kind == EMF_WORLD_SERVER) st.sent.clear();   // ... and the sent ones when the server world stops
    }
    if (kind == EMF_WORLD_SERVER) g_ops.clear();
    note_work();
}

void markers_shutdown() {
    std::vector<KilnOwner> owners;
    KilnHandle sub = 0;
    const KilnMarkersApi* k = nullptr;
    {
        std::lock_guard lock(g_mu);
        k = g_km;
        for (const auto& [mod, st] : g_state) owners.push_back(mod->owner);
        g_state.clear();
        g_ops.clear();
        sub = g_sub;
        g_sub = 0;
        g_work.store(false);
    }
    if (!k) return;
    // outside g_mu: unsubscribe and release_owner wait for a callback that may be running on another thread
    if (sub) k->unsubscribe(sub);
    for (KilnOwner o : owners) k->release_owner(o);
}

}  // namespace kemf
