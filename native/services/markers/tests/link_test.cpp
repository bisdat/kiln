// SPDX-License-Identifier: MIT
// kiln.markers' link with a fake bridge and a fake clock (dossier chapter 04 §6): the server layers and the server role
// (server.h) against built-in receivers (receiver.h) in other fake processes, joined by a fake network that can drop,
// duplicate and delay packets. M09 (target and broadcast layers), M10 (partial counts, retry, late join, reconnect), M11
// (loss, duplicates, late ACKs, stale tuples, gaps, resync, flood, bad messages), M13 server half (the catalog from a
// verified client, refusals), the hosted loopback (route 2, counted once), follow poses, the count limits and the error
// rule. No game: the catalog is synthetic (its own digest in a synthetic build pair).
#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

#include "bridge.h"
#include "catalog.h"
#include "check.h"
#include "receiver.h"
#include "server.h"
#include "store.h"
#include "wire.h"

using namespace kmk;

namespace {

uint64_t g_now = 100000;
uint64_t clock_fn() { return g_now; }
uint64_t g_rand = 0;
void rand_fn(uint8_t* out, size_t n) {
    ++g_rand;
    for (size_t i = 0; i < n; ++i) out[i] = static_cast<uint8_t>(g_rand * 31 + i * 7 + 1);
}
bool g_verbose = false;

// follow: serial -> (position, reason); resolve: a lease handle is its serial while g_follow_on
std::map<uint64_t, std::pair<KilnVec3, uint32_t>> g_follow;
bool g_follow_on = false;

// ---- a synthetic catalog and build pair -----------------------------------------------------------------------------------

std::shared_ptr<Catalog> synthetic_catalog(bool variant = false) {
    auto c = std::make_shared<Catalog>();
    const struct {
        uint32_t id, flags;
        const char* name;
    } rows[] = {{0x00000101u, 9, "mapmarker_camp"},       {0x00000202u, 25, "icon_cave"},  {0x00000303u, 41, "mapmarker_travel"},
                {0x00000404u, 9, "mapmarker_well_plant"}, {0x00000505u, 13, "icon_boss"}, {0x00000606u, 9, variant ? "mapmarker_other" : "mapmarker_tower"}};
    for (const auto& r : rows) {
        IconRow row;
        row.id = r.id;
        row.flags = r.flags;
        row.surfaces = (r.flags & KILN_MARKER_ICON_FAST_TRAVEL) ? 0u : static_cast<uint32_t>(KILN_MARKER_MAP);
        for (int i = 0; i < 16; ++i) row.image[i] = static_cast<uint8_t>(r.id + i);
        row.name = r.name;
        row.alias = alias_of(row.name);
        row.key = canonical_key(row.id);
        c->rows.push_back(row);
    }
    c->digest = digest_of(c->rows);
    return c;
}
const std::string kServerExe(64, '1'), kClientExe(64, '2');
std::string g_digest_hex;
BuildPair g_pair{kServerExe.c_str(), kClientExe.c_str(), nullptr};
constexpr uint32_t kCamp = 0x101, kTravel = 0x303, kUnknownIcon = 0x999;

// ---- a fake process -----------------------------------------------------------------------------------------------------

struct Proc {
    Arena arena;
    Switches sw;
    Store store{arena, sw};
    CatalogHolder catalog;
    Inbox inbox[2];
    std::unique_ptr<Server> server;
    std::unique_ptr<Receiver> receiver;
    std::unique_ptr<Bridge> bridge;
    std::vector<KilnMarkerChange> changes[2];
    std::vector<KilnMarkerCatalogChange> catalogs[2];
    int published = 0;
    bool on_thread = true;

    Proc(uint32_t side, const std::string& exe, bool client_surfaces) {
        store.set_clock(&clock_fn);
        LinkEnv env;
        env.now_ms = &clock_fn;
        env.random = &rand_fn;
        env.on_server_thread = [this] { return on_thread; };
        env.follow = [](uint64_t serial, KilnVec3* at, uint32_t* reason) {
            auto it = g_follow.find(serial);
            if (it == g_follow.end()) return false;
            *at = it->second.first;
            *reason = it->second.second;
            return true;
        };
        env.resolve_follow = [](KilnOwner, KilnMarkerAnchor lease, uint64_t* serial) -> KilnResult {
            if (!g_follow_on) return KILN_ERR_UNAVAILABLE;
            if (!g_follow.count(lease)) return KILN_ERR_NOT_FOUND;
            *serial = lease;
            return KILN_OK;
        };
        env.log = [](int, const char* t) {
            if (g_verbose) std::printf("    log: %s\n", t);
        };
        env.exe = exe;
        env.side = side;
        env.pair = &g_pair;
        server = std::make_unique<Server>(store, catalog, inbox[0], env);
        receiver = std::make_unique<Receiver>(store, catalog, sw, inbox[1], env);
        bridge = std::make_unique<Bridge>(server.get(), receiver.get(), &inbox[0], &inbox[1]);
        bridge->attach(77);
        server->set_available(true);
        server->on_catalog_published = [this] {
            ++published;
            KilnMarkerCatalogChange ch{};
            ch.size = sizeof ch;
            ch.ready = 1;
            ch.count = static_cast<uint32_t>(catalog.get()->rows.size());
            ch.generation = catalog.generation();
            store.post_catalog(kRoleServer, ch);
        };
        if (client_surfaces) {
            sw.compass = true;
            sw.world = true;
            sw.map = true;
        }
    }
    void take() {
        for (uint32_t role = 0; role < 2; ++role)
            for (const Note& n : store.take(role)) {
                if (n.kind == Note::Change) changes[role].push_back(n.change);
                if (n.kind == Note::Catalog) catalogs[role].push_back(n.catalog);
            }
    }
    size_t count(uint32_t role, uint32_t kind, uint32_t audience = 99) const {
        size_t c = 0;
        for (const auto& ch : changes[role]) c += ch.kind == kind && (audience == 99 || ch.audience == audience);
        return c;
    }
    std::vector<ClientRow> received() const {
        std::vector<ClientRow> rows, out;
        store.snapshot_client(&rows);
        for (auto& r : rows)
            if (r.source == KILN_MARKER_RECEIVED) out.push_back(r);
        return out;
    }
    const ClientRow* row(const std::string& key) const {
        static std::vector<ClientRow> keep;
        keep = received();
        for (const ClientRow& r : keep)
            if (r.key == key) return &r;
        return nullptr;
    }
};

KilnMarkerPeer peer(uint32_t role, uint32_t id, uint32_t route, uint32_t entity, uint32_t flags, uint64_t generation) {
    KilnMarkerPeer p{};
    p.size = sizeof p;
    p.role = role;
    p.peer = id;
    p.route = route;
    p.entity = entity;
    p.max_message_bytes = 16384;
    p.flags = flags;
    p.generation = generation;
    return p;
}

KilnResult up(Bridge& b, KilnMarkerPeer p) { return b.peer_up(&p); }

constexpr uint32_t kKiln = kHasPlayer | kNetworkReady | kHasMarkersChannel;
constexpr uint32_t kVanillaPeer = kHasPlayer | kVanilla;

// ---- a fake network -----------------------------------------------------------------------------------------------------

struct Conn {
    Proc* client = nullptr;  // null: a vanilla game client (no Kiln, no receiver)
    uint32_t peer = 0, entity = 0;
    uint64_t sgen = 0, cgen = 0;
    bool up = true;
    size_t frames = 0;  // server -> this peer
};

struct Net {
    Proc* srv = nullptr;
    std::deque<Conn> conns;
    // 0: drop, 1: deliver, 2: deliver twice, 3: deliver after hold_ms
    std::function<int(bool to_client, Conn& c, uint32_t message, const std::vector<uint8_t>& bytes)> filter;
    uint64_t hold_ms = 0;
    struct Held {
        uint64_t at;
        bool to_client;
        Conn* c;
        uint32_t message;
        std::vector<uint8_t> bytes;
    };
    std::vector<Held> held;
    std::map<std::pair<uint32_t, uint64_t>, int> sent_seq;  // (message, sequence) -> transmissions server -> client

    void deliver(bool to_client, Conn& c, uint32_t message, const std::vector<uint8_t>& bytes) {
        if (to_client) {
            if (c.client) c.client->inbox[1].push(kServerPeer, c.cgen, message, bytes.data(), bytes.size());
        } else {
            srv->inbox[0].push(c.peer, c.sgen, message, bytes.data(), bytes.size());
        }
    }
    void route(bool to_client, Conn& c, uint32_t message, std::vector<uint8_t> bytes) {
        const int f = filter ? filter(to_client, c, message, bytes) : 1;
        if (f == 0) return;
        if (f == 3) {
            held.push_back({g_now + hold_ms, to_client, &c, message, std::move(bytes)});
            return;
        }
        deliver(to_client, c, message, bytes);
        if (f == 2) deliver(to_client, c, message, bytes);
    }
    static uint8_t KILN_CALL server_send(const KilnMarkerEnvelope* e, void* user) {
        Net* n = static_cast<Net*>(user);
        for (Conn& c : n->conns)
            if (c.up && c.peer == e->peer && c.sgen == e->peer_generation) {
                ++c.frames;
                wire::Header h;
                const char* why = nullptr;
                if (wire::decode_header(e->bytes, e->length, &h, &why)) ++n->sent_seq[{e->message, h.sequence}];
                n->route(true, c, e->message, std::vector<uint8_t>(e->bytes, e->bytes + e->length));
                return 1;
            }
        return 0;
    }
    struct ClientCtx {
        Net* net;
        Conn* conn;
    };
    static uint8_t KILN_CALL client_send(const KilnMarkerEnvelope* e, void* user) {
        auto* x = static_cast<ClientCtx*>(user);
        if (!x->conn->up) return 0;
        x->net->route(false, *x->conn, e->message, std::vector<uint8_t>(e->bytes, e->bytes + e->length));
        return 1;
    }
    Conn& join(Proc* client, uint32_t peer_id, uint32_t entity, uint32_t flags = kKiln) {
        static uint64_t gens = 1000;
        Conn c;
        c.client = client;
        c.peer = peer_id;
        c.entity = entity;
        c.sgen = ++gens;
        c.cgen = ++gens;
        conns.push_back(c);
        Conn& r = conns.back();
        CHECK_EQ(up(*srv->bridge, peer(0, peer_id, kRouteNetwork, entity, flags, r.sgen)), KILN_OK);
        if (client) CHECK_EQ(up(*client->bridge, peer(1, kServerPeer, kRouteNetwork, 0, kNetworkReady | kHasMarkersChannel, r.cgen)), KILN_OK);
        return r;
    }
    void leave(Conn& c) {
        c.up = false;
        CHECK_EQ(srv->bridge->peer_down(0, c.peer, c.sgen), KILN_OK);
        if (c.client) CHECK_EQ(c.client->bridge->peer_down(1, kServerPeer, c.cgen), KILN_OK);
    }
    void step(int n = 1, uint64_t ms = 50) {
        for (int i = 0; i < n; ++i) {
            g_now += ms;
            for (auto it = held.begin(); it != held.end();) {
                if (it->at <= g_now) {
                    deliver(it->to_client, *it->c, it->message, it->bytes);
                    it = held.erase(it);
                } else {
                    ++it;
                }
            }
            for (Conn& c : conns) {
                if (!c.up || !c.client) continue;
                c.client->receiver->post();
                c.client->take();
                ClientCtx ctx{this, &c};
                uint32_t done = 0;
                c.client->bridge->pump(1, 16, &client_send, &ctx, &done, g_now);
            }
            srv->server->post();
            srv->take();
            uint32_t done = 0;
            srv->bridge->pump(0, 16, &server_send, this, &done, g_now);
        }
    }
    void settle(int steps = 40) { step(steps); }
};

struct Spec {
    KilnMarkerSpec s{};
    std::string key, text;
    explicit Spec(std::string k, std::string t = "Raid entrance", uint32_t surfaces = KILN_MARKER_COMPASS | KILN_MARKER_WORLD, uint32_t icon = 0)
        : key(std::move(k)), text(std::move(t)) {
        s.size = sizeof s;
        s.surfaces = surfaces;
        s.position = {3670, 820, 1500};
        s.rgba = 0xffffffffu;
        s.icon_type = icon;
    }
    const KilnMarkerSpec* operator&() {
        s.key = KilnStr{key.data(), key.size()};
        s.text = KilnStr{text.data(), text.size()};
        return &s;
    }
};

KilnMarkerResult res() {
    KilnMarkerResult r{};
    r.size = sizeof r;
    return r;
}

KilnStr ks(const std::string& s) { return KilnStr{s.data(), s.size()}; }

// A server process with its world and a bound author (owner 5, "author"), and its fake network.
struct World {
    Proc srv{KILN_SIDE_SERVER, kServerExe, false};
    std::vector<std::unique_ptr<Proc>> clients;
    Net net;
    explicit World(bool server_catalog = true) {
        net.srv = &srv;
        if (server_catalog) srv.catalog.publish(synthetic_catalog(), true, "");
        srv.store.world_started(KILN_WORLD_SERVER);
        srv.server->world_started();
        CHECK_EQ(srv.store.bind(5, KilnStr{"author", 6}), KILN_OK);
    }
    Proc& client(bool catalog = true, bool surfaces = true) {
        clients.push_back(std::make_unique<Proc>(KILN_SIDE_CLIENT, kClientExe, surfaces));
        Proc& c = *clients.back();
        if (catalog) c.catalog.publish(synthetic_catalog(), true, "");
        c.store.world_started(KILN_WORLD_CLIENT);
        c.receiver->world_started();
        return c;
    }
    KilnMarkerResult send(KilnEntity to, Spec& sp, KilnResult want = KILN_OK) {
        KilnMarkerResult r = res();
        CHECK_EQ(srv.server->send(5, to, &sp, &r), want);
        return r;
    }
    KilnMarkerResult unsend(KilnEntity to, const std::string& k) {
        KilnMarkerResult r = res();
        CHECK_EQ(srv.server->unsend(5, to, ks(k), &r), KILN_OK);
        return r;
    }
    KilnMarkerResult suppress(KilnEntity to, const std::string& k) {
        KilnMarkerResult r = res();
        CHECK_EQ(srv.server->suppress_sent(5, to, ks(k), &r), KILN_OK);
        return r;
    }
    KilnMarkerResult clear(KilnEntity to) {
        KilnMarkerResult r = res();
        CHECK_EQ(srv.server->clear_sent(5, to, &r), KILN_OK);
        return r;
    }
};

std::vector<uint32_t> kinds(const Proc& p, uint32_t role, uint32_t audience = 99) {
    std::vector<uint32_t> k;
    for (const auto& c : p.changes[role])
        if (audience == 99 || c.audience == audience) k.push_back(c.kind);
    return k;
}

bool reason_starts(const KilnMarkerResult& r, const char* prefix) { return std::strncmp(r.reason, prefix, std::strlen(prefix)) == 0; }

// ---- cases ----------------------------------------------------------------------------------------------------------------

void handshake_and_catalog() {  // M13 server half, M08 protocol: HELLO, CATALOG, WELCOME, an empty snapshot
    kmk_test::begin();
    World w(false);  // a dedicated server: no catalog of its own
    Proc& p = w.client();
    Spec map("gate", "Raid entrance", KILN_MARKER_MAP | KILN_MARKER_COMPASS, kCamp);
    KilnMarkerResult r = w.send(0, map);
    CHECK_EQ(r.status, KILN_MARKER_CATALOG_NOT_READY);  // warming up: nothing stored
    CHECK_EQ(r.committed, 0u);
    Spec plate("plate", "Smith", KILN_MARKER_WORLD);  // a world-only spec needs no catalog
    r = w.send(0, plate);
    CHECK(r.status == KILN_MARKER_ACCEPTED && r.committed == 1);
    w.net.join(&p, 0x81, 101);
    w.net.settle();
    Server::LinkInfo li;
    CHECK(w.srv.server->link_info(0x81, &li));
    CHECK(li.welcomed && li.map_ok && !li.need_catalog);
    CHECK_EQ(li.accepted, wire::kCapMap | wire::kCapCompass | wire::kCapWorld | wire::kCapFollowPoses);
    CHECK_EQ(w.srv.published, 1);
    CHECK(w.srv.catalog.get() && w.srv.catalog.get()->rows.size() == 6 && w.srv.catalog.map_verified());
    CHECK_EQ(w.srv.catalogs[0].size(), size_t{1});
    CHECK(!w.srv.catalogs[0].empty() && w.srv.catalogs[0][0].ready == 1 && w.srv.catalogs[0][0].count == 6 && w.srv.catalogs[0][0].role == 0);
    const auto info = p.receiver->info();
    CHECK(info.phase == Receiver::Phase::Linked && info.applied > 0 && info.view == 1);
    CHECK_EQ(p.count(1, KILN_MARKER_ADDED, KILN_MARKER_RECEIVED), size_t{1});
    // now the catalog is here: a map marker goes, and reaches the client
    r = w.send(0, map);
    CHECK(r.status == KILN_MARKER_ACCEPTED && r.committed == 1 && r.queued == 1 && r.refused == 0);
    w.net.settle();
    CHECK(p.row("gate") && p.row("gate")->rec.icon == kCamp);
    CHECK_EQ(p.count(1, KILN_MARKER_ADDED, KILN_MARKER_RECEIVED), size_t{2});
    // icons the catalog does not offer for the map
    Spec bad_icon("x", "t", KILN_MARKER_MAP, kUnknownIcon), travel("y", "t", KILN_MARKER_MAP, kTravel);
    r = w.send(0, bad_icon, KILN_ERR_INVALID_ARGUMENT);
    r = w.send(0, travel, KILN_ERR_INVALID_ARGUMENT);
    CHECK(std::strstr(r.reason, "fast-travel") != nullptr);
    kmk_test::end("link.m13_catalog_from_a_verified_client");
}

void wrong_catalog() {  // M13: a client whose catalog is not the build pair's never turns the map on
    kmk_test::begin();
    World w(false);
    Proc& p = w.client(false);
    p.catalog.publish(synthetic_catalog(true), true, "");  // another digest
    w.net.join(&p, 0x81, 101);
    w.net.settle();
    Server::LinkInfo li;
    CHECK(w.srv.server->link_info(0x81, &li));
    CHECK(li.welcomed && !li.map_ok && !li.need_catalog);
    CHECK_EQ(li.accepted & wire::kCapMap, 0u);
    CHECK_EQ(w.srv.published, 0);
    CHECK(!w.srv.catalog.get());
    // a client that sends a CATALOG whose digest field lies: refused, the map stays off for it
    World w2(false);
    Proc& q = w2.client();
    w2.net.filter = [](bool to_client, Conn&, uint32_t m, const std::vector<uint8_t>& b) {
        if (!to_client && m == wire::kCatalog) {
            auto& bytes = const_cast<std::vector<uint8_t>&>(b);
            bytes[wire::kHeaderBytes + 5] ^= 1;  // a row's id byte: the rows no longer give the digest
        }
        return 1;
    };
    w2.net.join(&q, 0x82, 102);
    w2.net.settle();
    CHECK(w2.srv.server->link_info(0x82, &li));
    CHECK(li.welcomed && !li.map_ok);
    CHECK(!w2.srv.catalog.get());
    Spec map("gate", "t", KILN_MARKER_MAP, kCamp);
    KilnMarkerResult r = w2.send(0, map);
    CHECK_EQ(r.status, KILN_MARKER_CATALOG_NOT_READY);
    kmk_test::end("link.m13_wrong_catalog_never_reaches_the_map");
}

void m09_layers() {
    kmk_test::begin();
    World w;
    Proc& p = w.client();
    Proc& q = w.client();
    w.net.join(&p, 0x81, 101);
    w.net.join(&q, 0x82, 102);
    w.net.settle();
    Spec A("k", "A"), B("k", "B");
    auto text = [](Proc& c) -> std::string {
        const ClientRow* r = c.row("k");
        return r ? std::string(r->rec.text) : std::string("-");
    };
    KilnMarkerResult r = w.send(0, A);
    CHECK(r.status == KILN_MARKER_ACCEPTED && r.committed == 1 && r.queued == 2 && r.refused == 0 && r.affected == 1);
    w.net.settle();
    CHECK(text(p) == "A" && text(q) == "A");
    r = w.send(101, B);
    CHECK(r.status == KILN_MARKER_ACCEPTED && r.queued == 1);
    w.net.settle();
    CHECK(text(p) == "B" && text(q) == "A");
    r = w.unsend(101, "k");
    CHECK(r.status == KILN_MARKER_ACCEPTED && r.queued == 1);
    w.net.settle();
    CHECK(text(p) == "A" && text(q) == "A");
    r = w.suppress(101, "k");
    CHECK(r.status == KILN_MARKER_ACCEPTED && r.committed == 1);
    w.net.settle();
    CHECK(text(p) == "-" && text(q) == "A");
    r = w.suppress(101, "k");  // again: nothing changes
    CHECK(r.status == KILN_MARKER_ACCEPTED && r.affected == 0);
    r = w.clear(101);
    CHECK(r.status == KILN_MARKER_ACCEPTED && r.affected == 1);
    w.net.settle();
    CHECK(text(p) == "A" && text(q) == "A");
    r = w.clear(0);
    CHECK(r.status == KILN_MARKER_ACCEPTED && r.affected == 1);
    w.net.settle();
    CHECK(text(p) == "-" && text(q) == "-");
    // the server's logical events: one per committed mutation
    const auto sk = kinds(w.srv, 0);
    const std::vector<uint32_t> want_s = {KILN_MARKER_ADDED, KILN_MARKER_ADDED, KILN_MARKER_REMOVED, KILN_MARKER_ADDED, KILN_MARKER_CLEARED, KILN_MARKER_CLEARED};
    CHECK(sk == want_s);
    if (w.srv.changes[0].size() == 6) {
        CHECK_EQ(w.srv.changes[0][0].audience, uint32_t{KILN_MARKER_SERVER_BROADCAST});
        CHECK(w.srv.changes[0][1].audience == KILN_MARKER_SERVER_TARGET && w.srv.changes[0][1].recipient == 101);
        CHECK_EQ(w.srv.changes[0][4].affected, 1u);
        CHECK(w.srv.changes[0][5].audience == KILN_MARKER_SERVER_BROADCAST && w.srv.changes[0][5].affected == 1);
        CHECK(std::string(w.srv.changes[0][0].owner) == "author" && std::string(w.srv.changes[0][0].key) == "k");
    }
    // the receivers' events: per changed effective key
    const std::vector<uint32_t> want_p = {KILN_MARKER_ADDED, KILN_MARKER_UPDATED, KILN_MARKER_UPDATED, KILN_MARKER_REMOVED, KILN_MARKER_ADDED,
                                          KILN_MARKER_REMOVED};
    CHECK(kinds(p, 1) == want_p);
    const std::vector<uint32_t> want_q = {KILN_MARKER_ADDED, KILN_MARKER_REMOVED};
    CHECK(kinds(q, 1) == want_q);
    for (const auto& c : p.changes[1]) CHECK(c.audience == KILN_MARKER_RECEIVED && std::string(c.owner) == "author");
    kmk_test::end("link.m09_target_and_broadcast_layers");
}

void updates_in_place() {  // a new value for a stored key (broadcast and per-player): one Updated per receiver
    kmk_test::begin();
    World w;
    Proc& p = w.client();
    Proc& q = w.client();
    w.net.join(&p, 0x81, 101);
    w.net.join(&q, 0x82, 102);
    w.net.settle();
    Spec a1("k", "A1"), a2("k", "A2"), b1("k", "B1"), b2("k", "B2");
    w.send(0, a1);
    w.net.settle();
    KilnMarkerResult r = w.send(0, a2);
    CHECK(r.status == KILN_MARKER_ACCEPTED && r.queued == 2 && r.affected == 1);
    w.net.settle();
    CHECK(p.row("k") && std::string(p.row("k")->rec.text) == "A2" && q.row("k") && std::string(q.row("k")->rec.text) == "A2");
    w.send(101, b1);
    w.net.settle();
    r = w.send(101, b2);
    CHECK(r.status == KILN_MARKER_ACCEPTED && r.queued == 1);
    w.net.settle();
    CHECK(p.row("k") && std::string(p.row("k")->rec.text) == "B2" && std::string(q.row("k")->rec.text) == "A2");
    const std::vector<uint32_t> want_p = {KILN_MARKER_ADDED, KILN_MARKER_UPDATED, KILN_MARKER_UPDATED, KILN_MARKER_UPDATED};
    const std::vector<uint32_t> want_q = {KILN_MARKER_ADDED, KILN_MARKER_UPDATED};
    CHECK(kinds(p, 1) == want_p);
    CHECK(kinds(q, 1) == want_q);
    const std::vector<uint32_t> want_s = {KILN_MARKER_ADDED, KILN_MARKER_UPDATED, KILN_MARKER_ADDED, KILN_MARKER_UPDATED};
    CHECK(kinds(w.srv, 0) == want_s);
    kmk_test::end("link.updates_of_a_stored_key");
}

void m10_partial_and_retry() {
    kmk_test::begin();
    World w;
    Proc& p = w.client();
    Conn& cp = w.net.join(&p, 0x81, 101);
    Conn& cq = w.net.join(nullptr, 0x82, 102, kVanillaPeer);  // a vanilla player
    w.net.settle();
    Spec A("k", "A");
    KilnMarkerResult r = w.send(0, A);
    CHECK_EQ(r.committed, 1u);
    CHECK_EQ(r.status, KILN_MARKER_PARTIAL);
    CHECK(r.queued == 1 && r.refused == 1 && reason_starts(r, "Partial"));
    w.net.step();  // the server's notes go out at its next step
    CHECK_EQ(w.srv.count(0, KILN_MARKER_ADDED), size_t{1});
    w.net.settle();
    CHECK_EQ(p.count(1, KILN_MARKER_ADDED), size_t{1});
    // the identical retry: no new event, nothing new for p (it has it), q still refused
    r = w.send(0, A);
    CHECK(r.committed == 1 && r.affected == 0 && r.queued == 0 && r.refused == 1 && r.status == KILN_MARKER_REQUIRES_CLIENT);
    w.net.settle();
    CHECK_EQ(w.srv.count(0, KILN_MARKER_ADDED), size_t{1});
    CHECK_EQ(p.count(1, KILN_MARKER_ADDED), size_t{1});
    CHECK_EQ(p.changes[1].size(), size_t{1});
    // a late Kiln player gets the broadcast in its snapshot
    Proc& late = w.client();
    w.net.join(&late, 0x83, 103);
    w.net.settle();
    CHECK_EQ(late.count(1, KILN_MARKER_ADDED), size_t{1});
    CHECK(late.row("k") && std::string(late.row("k")->rec.text) == "A");
    // a per-player override does not outlive its connection; the broadcast does
    Spec B("only_p", "B");
    r = w.send(101, B);
    CHECK(r.status == KILN_MARKER_ACCEPTED && r.queued == 1);
    w.net.settle();
    CHECK(p.row("only_p") != nullptr);
    const size_t target_clears = w.srv.count(0, KILN_MARKER_CLEARED, KILN_MARKER_SERVER_TARGET);
    w.net.leave(cp);
    CHECK_EQ(w.srv.count(0, KILN_MARKER_CLEARED, KILN_MARKER_SERVER_TARGET), target_clears);  // queued, delivered at the next step
    w.net.step();
    CHECK_EQ(w.srv.count(0, KILN_MARKER_CLEARED, KILN_MARKER_SERVER_TARGET), target_clears + 1);
    CHECK(p.received().empty());  // the client's received records went with the link
    w.net.join(&p, 0x81, 101);  // the same player again, a new connection
    w.net.settle();
    CHECK(p.row("k") != nullptr && p.row("only_p") == nullptr);
    // a targeted send to a player who is not connected is refused before the commit
    Spec C("c", "C");
    r = w.send(999, C);
    CHECK(r.status == KILN_MARKER_NOT_FOUND && r.committed == 0);
    // no frame ever went to the vanilla player
    CHECK_EQ(cq.frames, size_t{0});
    kmk_test::end("link.m10_partial_counts_retry_late_join_reconnect");
}

void m11_loss_and_resync() {
    kmk_test::begin();
    World w;
    Proc& p = w.client();
    Conn& c = w.net.join(&p, 0x81, 101);
    w.net.settle();
    // (a) a snapshot of 40 records (two chunks) whose first chunk is lost once: the same transaction again, never half
    std::vector<std::unique_ptr<Spec>> specs;
    for (int i = 0; i < 40; ++i) {
        specs.push_back(std::make_unique<Spec>("k" + std::to_string(100 + i), "text " + std::to_string(i)));
        w.send(0, *specs.back());
    }
    w.net.leave(c);
    Conn& c2 = w.net.join(&p, 0x81, 101);
    int dropped = 0;
    w.net.filter = [&](bool to_client, Conn&, uint32_t m, const std::vector<uint8_t>& b) {
        if (to_client && m == wire::kSnapChunk && !dropped) {
            wire::Header h;
            wire::SnapChunk sc;
            const char* why = nullptr;
            if (wire::decode_snap_chunk(b.data(), b.size(), &h, &sc, &why) && sc.index == 0 && !sc.records.empty()) {
                ++dropped;
                return 0;
            }
        }
        return 1;
    };
    for (int i = 0; i < 120 && p.received().size() < 40; ++i) {
        w.net.step();
        const size_t n = p.received().size();
        CHECK(n == 0 || n == 40);  // nothing in between: a snapshot is published whole or not at all
    }
    CHECK_EQ(dropped, 1);
    CHECK_EQ(p.received().size(), size_t{40});
    w.net.settle();
    CHECK_EQ(p.count(1, KILN_MARKER_ADDED), size_t{40});
    // (b) a duplicated SNAP_END and duplicated chunks: no duplicate events
    const size_t added = p.count(1, KILN_MARKER_ADDED);
    w.net.filter = [](bool to_client, Conn&, uint32_t m, const std::vector<uint8_t>&) {
        return to_client && (m == wire::kSnapEnd || m == wire::kSnapChunk || m == wire::kDelta) ? 2 : 1;
    };
    Spec extra("extra", "x");
    w.send(0, extra);
    w.net.settle();
    CHECK_EQ(p.count(1, KILN_MARKER_ADDED), added + 1);
    // (c) ACKs held past 2 s: the transaction is sent again (same sequence), applied once
    w.net.filter = [](bool to_client, Conn&, uint32_t m, const std::vector<uint8_t>&) { return !to_client && m == wire::kAck ? 3 : 1; };
    w.net.hold_ms = 2600;
    Spec late("late", "y");
    w.send(0, late);
    w.net.sent_seq.clear();
    w.net.step(100);
    w.net.filter = nullptr;
    w.net.settle();
    CHECK_EQ(p.count(1, KILN_MARKER_ADDED), added + 2);
    int resent = 0;
    for (const auto& [k, n] : w.net.sent_seq)
        if (k.first == wire::kDelta && n > 1) ++resent;
    CHECK(resent >= 1);
    Server::LinkInfo li;
    CHECK(w.srv.server->link_info(0x81, &li) && !li.stalled && !li.in_flight);
    // (d) everything to the client lost for a while: 3 resends, then stalled; heartbeats bring a resync and a full
    // snapshot of the CURRENT state (a key removed meanwhile is not replayed)
    // (the heartbeats pass, so the lease holds; the client's RESYNCs are lost too)
    w.net.filter = [](bool to_client, Conn&, uint32_t m, const std::vector<uint8_t>&) {
        if (to_client) return m == wire::kHeartbeat || m == wire::kWelcome ? 1 : 0;
        return m == wire::kResync ? 0 : 1;
    };
    Spec gone("gone", "z");
    w.send(0, gone);
    w.net.sent_seq.clear();
    w.net.step(200);  // 10 s
    CHECK(w.srv.server->link_info(0x81, &li) && li.stalled);
    int tries = 0;
    for (const auto& [k, n] : w.net.sent_seq)
        if (k.first == wire::kDelta) tries = std::max(tries, n);
    CHECK_EQ(tries, 4);  // the first send and 3 resends
    w.unsend(0, "gone");
    Spec fresh("fresh", "f");
    w.send(0, fresh);
    // the link's lease ran out on the client meanwhile: it cleared and says HELLO again
    w.net.filter = nullptr;
    w.net.settle(80);
    CHECK(w.srv.server->link_info(0x81, &li) && !li.stalled);
    CHECK(p.row("gone") == nullptr && p.row("fresh") != nullptr && p.row("late") != nullptr);
    CHECK_EQ(p.received().size(), size_t{43});
    // (e) a packet of an old tuple (another nonce) is ignored: no event, not a bad message
    const size_t events = p.changes[1].size();
    wire::Header h;
    h.server_epoch = w.srv.server->epoch();
    h.client_nonce.fill(0x55);
    h.sequence = 999;
    auto bytes = wire::encode_heartbeat(h);
    p.inbox[1].push(kServerPeer, c2.cgen, wire::kHeartbeat, bytes.data(), bytes.size());
    w.net.step(3);
    CHECK_EQ(p.changes[1].size(), events);
    CHECK(!p.receiver->info().suspended);
    // (f) a gap (a DELTA two ahead) asks for RESYNC; the server answers with a full snapshot (no duplicate events)
    const auto ri = p.receiver->info();
    h.client_nonce = ri.nonce;
    h.server_epoch = ri.server_epoch;
    h.sequence = ri.applied + 2;
    wire::DeltaOp op;
    op.op = wire::kRemove;
    op.remove_id = 1;
    bytes = wire::encode_delta(h, {op});
    size_t resyncs = 0;
    w.net.filter = [&](bool to_client, Conn&, uint32_t m, const std::vector<uint8_t>&) {
        if (!to_client && m == wire::kResync) ++resyncs;
        return 1;
    };
    p.inbox[1].push(kServerPeer, c2.cgen, wire::kDelta, bytes.data(), bytes.size());
    w.net.settle();
    CHECK(resyncs >= 1);
    CHECK_EQ(p.changes[1].size(), events);
    CHECK_EQ(p.received().size(), size_t{43});
    w.net.filter = nullptr;
    kmk_test::end("link.m11_loss_duplicates_late_acks_stall_resync");
}

void m11_bad_messages() {
    kmk_test::begin();
    World w;
    Proc& p = w.client();
    Proc& q = w.client();
    Conn& cp = w.net.join(&p, 0x81, 101);
    w.net.join(&q, 0x82, 102);
    w.net.settle();
    // three messages the server may not get, from p, in 10 s: p's marker link is suspended; q goes on
    wire::Header h;
    auto bytes = wire::encode_heartbeat(h);
    for (int i = 0; i < 3; ++i) w.srv.inbox[0].push(0x81, cp.sgen, wire::kHeartbeat, bytes.data(), bytes.size());
    w.net.step();
    Server::LinkInfo li;
    CHECK(w.srv.server->link_info(0x81, &li) && li.suspended);
    Spec A("k", "A");
    KilnMarkerResult r = w.send(0, A);
    CHECK(r.queued == 1 && r.refused == 1 && r.status == KILN_MARKER_PARTIAL);
    w.net.step(5);
    CHECK(q.row("k") != nullptr && p.row("k") == nullptr);
    // p's receiver says HELLO again once its lease runs out; the server takes it back
    w.net.settle(260);
    CHECK(w.srv.server->link_info(0x81, &li) && !li.suspended && li.welcomed);
    CHECK(p.row("k") != nullptr);
    // the receiver: three client-to-server messages from the server suspend its link only
    for (int i = 0; i < 3; ++i) p.inbox[1].push(kServerPeer, cp.cgen, wire::kAck, bytes.data(), bytes.size());
    w.net.step();
    CHECK(p.receiver->info().suspended);
    // a flood: the inbound queue holds 128 envelopes, the rest are dropped (and asked for again), never applied in part
    size_t taken = 0;
    for (int i = 0; i < 200; ++i) taken += q.inbox[1].push(kServerPeer, 1, wire::kHeartbeat, bytes.data(), bytes.size());
    CHECK_EQ(taken, Inbox::kEnvelopes);
    w.net.settle();
    CHECK(q.row("k") != nullptr && !q.receiver->info().suspended);
    kmk_test::end("link.m11_bad_messages_suspend_the_marker_link_only");
}

void receiver_limits() {  // the receiver's own limits against a server that does not keep them (crafted frames)
    kmk_test::begin();
    World w;
    Proc& p = w.client();
    Conn& c = w.net.join(&p, 0x81, 101);
    w.net.settle();
    const auto ri = p.receiver->info();
    wire::Header h;
    h.server_epoch = ri.server_epoch;
    h.client_nonce = ri.nonce;
    std::vector<wire::Ack> acks;
    w.net.filter = [&](bool to_client, Conn&, uint32_t m, const std::vector<uint8_t>& b) {
        if (!to_client && m == wire::kAck) {
            wire::Header ah;
            wire::Ack a;
            const char* why = nullptr;
            if (wire::decode_ack(b.data(), b.size(), &ah, &a, &why)) acks.push_back(a);
        }
        return to_client ? 0 : 1;  // the real server stays quiet
    };
    // 129 records of one namespace in one snapshot: refused whole (Full), nothing applied
    std::vector<wire::Record> recs;
    for (int i = 0; i < 129; ++i) {
        wire::Record r;
        r.wire_id = 5000 + i;
        r.publisher_generation = 1;
        r.ns = "flood";
        char k[16];
        std::snprintf(k, sizeof k, "k%03d", i);
        r.key = k;
        r.revision = 1;
        r.surfaces = KILN_MARKER_WORLD;
        r.x = 1;
        r.y = 2;
        r.z = 3;
        recs.push_back(r);
    }
    h.sequence = ri.applied + 1;
    wire::SnapBegin b{77, 129, 5};
    auto push = [&](uint32_t m, const std::vector<uint8_t>& bytes) { p.inbox[1].push(kServerPeer, c.cgen, m, bytes.data(), bytes.size()); };
    push(wire::kSnapBegin, wire::encode_snap_begin(h, b));
    for (uint16_t i = 0; i < 5; ++i) {
        std::vector<wire::Record> part(recs.begin() + i * 32, recs.begin() + std::min<size_t>(129, (i + 1) * 32));
        push(wire::kSnapChunk, wire::encode_snap_chunk(h, 77, i, part));
    }
    push(wire::kSnapEnd, wire::encode_snap_end(h, wire::SnapEnd{77, wire::snapshot_digest(recs)}));
    w.net.step(4);
    CHECK(!acks.empty() && acks.back().status == wire::kAckFull);
    CHECK_EQ(p.received().size(), size_t{0});  // nothing applied
    // a snapshot whose digest does not match: never applied, RESYNC asked
    acks.clear();
    recs.resize(3);
    b = wire::SnapBegin{78, 3, 1};
    h.sequence = ri.applied + 2;
    push(wire::kSnapBegin, wire::encode_snap_begin(h, b));
    push(wire::kSnapChunk, wire::encode_snap_chunk(h, 78, 0, recs));
    wire::Digest wrong{};
    push(wire::kSnapEnd, wire::encode_snap_end(h, wire::SnapEnd{78, wrong}));
    w.net.step(4);
    CHECK(acks.empty());
    CHECK_EQ(p.received().size(), size_t{0});
    // an unsupported surface (the tooltip was not accepted): ACK unsupported, prior view kept
    recs.resize(1);
    recs[0].surfaces = KILN_MARKER_MAP;
    recs[0].icon = kCamp;
    recs[0].label_mode = KILN_MARKER_LABEL_NATIVE_TOOLTIP;
    h.sequence = ri.applied + 3;
    b = wire::SnapBegin{79, 1, 1};
    push(wire::kSnapBegin, wire::encode_snap_begin(h, b));
    push(wire::kSnapChunk, wire::encode_snap_chunk(h, 79, 0, recs));
    push(wire::kSnapEnd, wire::encode_snap_end(h, wire::SnapEnd{79, wire::snapshot_digest(recs)}));
    w.net.step(4);
    CHECK(!acks.empty() && acks.back().status == wire::kAckUnsupported);
    CHECK_EQ(p.received().size(), size_t{0});
    w.net.filter = nullptr;
    kmk_test::end("link.m11_receiver_limits_and_refusals");
}

void hosted_loopback() {  // route 2: the hosting player's own receiver, counted once with its Network row
    kmk_test::begin();
    World w;
    Proc& host = w.srv;
    host.sw.compass = host.sw.world = host.sw.map = true;
    host.store.world_started(KILN_WORLD_CLIENT);
    host.receiver->world_started();
    // the hosting player (entity 100): its route-2 endpoint and the Network row of its own machine (slot 0)
    CHECK_EQ(up(*host.bridge, peer(0, kLoopbackServerPeer, kRouteLoopback, 100, kKiln, 9)), KILN_OK);
    CHECK_EQ(up(*host.bridge, peer(1, kServerPeer, kRouteLoopback, 0, kNetworkReady | kHasMarkersChannel, 9)), KILN_OK);
    Conn& own = w.net.join(nullptr, 0x80, 100);  // must get nothing
    Proc& p = w.client();
    w.net.join(&p, 0x81, 101);
    for (int i = 0; i < 40; ++i) {
        w.net.step();
        host.receiver->post();
        host.take();
        uint32_t done = 0;
        host.bridge->pump(1, 16, nullptr, nullptr, &done, g_now);  // route 2 never reaches a send callback
    }
    Spec A("k", "A");
    KilnMarkerResult r = w.send(0, A);
    CHECK(r.status == KILN_MARKER_ACCEPTED && r.queued == 2 && r.refused == 0);  // the host once, p once
    for (int i = 0; i < 40; ++i) {
        w.net.step();
        host.receiver->post();
        host.take();
        uint32_t done = 0;
        host.bridge->pump(1, 16, nullptr, nullptr, &done, g_now);
    }
    CHECK(host.row("k") != nullptr && p.row("k") != nullptr);
    CHECK_EQ(host.count(1, KILN_MARKER_ADDED, KILN_MARKER_RECEIVED), size_t{1});
    CHECK_EQ(own.frames, size_t{0});
    // the host is targeted through its loopback
    Spec B("mine", "B");
    r = w.send(100, B);
    CHECK(r.status == KILN_MARKER_ACCEPTED && r.queued == 1);
    kmk_test::end("link.hosted_loopback_counted_once");
}

void follow_poses() {
    kmk_test::begin();
    World w;
    Proc& p = w.client();
    w.net.join(&p, 0x81, 101);
    w.net.settle();
    Spec f("boss", "Boss", KILN_MARKER_WORLD);
    f.s.follow = 77;
    KilnMarkerResult r = w.send(0, f, KILN_ERR_UNAVAILABLE);  // no anchors on the server yet
    CHECK(std::string(r.reason) == "markers.anchors.server");
    g_follow_on = true;
    g_follow[77] = {{100, 50, 100}, KILN_MARKER_READY};
    r = w.send(0, f);
    CHECK(r.status == KILN_MARKER_ACCEPTED && r.queued == 1);
    w.net.settle(10);
    const ClientRow* row = p.row("boss");
    CHECK(row && row->followed && row->rec.x == 100);
    const size_t events = p.changes[1].size();
    // it moves 0.5 m every 50 ms: poses, no logical change
    for (int i = 0; i < 40; ++i) {
        g_follow[77].first.x += 0.5;
        w.net.step();
    }
    w.net.step(4);
    row = p.row("boss");
    CHECK(row && std::fabs(row->rec.x - g_follow[77].first.x) < 2.6);  // within one or two pose periods
    CHECK(row && row->pose_prev_ms < row->pose_last_ms && row->pose_last.x == row->rec.x);
    CHECK_EQ(p.changes[1].size(), events);
    // standing still: keepalives every 500 ms keep it fresh; without poses for 750 ms it reads SOURCE_STALE
    w.net.step(30);
    row = p.row("boss");
    CHECK(row && row->source_reason == KILN_MARKER_READY);
    g_now += 800;
    row = p.row("boss");
    CHECK(row && row->source_reason == KILN_MARKER_SOURCE_STALE);
    w.net.step(15);
    row = p.row("boss");
    CHECK(row && row->source_reason == KILN_MARKER_READY);
    // the entity is destroyed: ANCHOR_ENDED, kept (no Removed); the same serial never comes back
    g_follow[77].second = KILN_MARKER_ANCHOR_ENDED;
    w.net.step(10);
    row = p.row("boss");
    CHECK(row && row->source_reason == KILN_MARKER_ANCHOR_ENDED);
    g_follow[77].second = KILN_MARKER_READY;
    w.net.step(20);
    row = p.row("boss");
    CHECK(row && row->source_reason == KILN_MARKER_ANCHOR_ENDED);
    CHECK_EQ(p.changes[1].size(), events);
    // a new capture (serial 78) sent again: one Updated, READY again
    g_follow[78] = {{300, 60, 300}, KILN_MARKER_READY};
    f.s.follow = 78;
    r = w.send(0, f);
    CHECK(r.status == KILN_MARKER_ACCEPTED && r.affected == 1);
    w.net.settle(10);
    row = p.row("boss");
    CHECK(row && row->source_reason == KILN_MARKER_READY && row->rec.x == 300);
    CHECK_EQ(p.changes[1].size(), events + 1);
    CHECK_EQ(p.count(1, KILN_MARKER_UPDATED), size_t{1});
    g_follow_on = false;
    g_follow.clear();
    kmk_test::end("link.follow_poses_stale_and_ended");
}

void limits() {
    kmk_test::begin();
    World w;
    Proc& p = w.client();
    w.net.join(&p, 0x81, 101);
    w.net.settle();
    // 128 per publisher
    for (int i = 0; i < 128; ++i) {
        Spec s("a" + std::to_string(i), "t");
        KilnMarkerResult r = w.send(0, s);
        CHECK_EQ(r.status, KILN_MARKER_ACCEPTED);
        if (i % 30 == 0) w.net.step();  // the step takes the queued notes (64 per publisher may wait)
    }
    w.net.step();
    Spec over("a128", "t");
    KilnMarkerResult r = w.send(0, over);
    CHECK(r.status == KILN_MARKER_FULL && r.committed == 0);
    // 512 in the broadcast: three more publishers fill it, a fifth is refused
    for (KilnOwner o = 6; o <= 9; ++o) {
        const std::string ns = "pub" + std::to_string(o);
        CHECK_EQ(w.srv.store.bind(o, KilnStr{ns.data(), ns.size()}), KILN_OK);
    }
    for (KilnOwner o = 6; o <= 8; ++o)
        for (int i = 0; i < 128; ++i) {
            Spec s("b" + std::to_string(i), "t");
            KilnMarkerResult rr = res();
            CHECK_EQ(w.srv.server->send(o, 0, &s, &rr), KILN_OK);
            CHECK_EQ(rr.status, KILN_MARKER_ACCEPTED);
            if (i % 30 == 0) w.net.step();
        }
    w.net.step();
    Spec fifth("c", "t");
    KilnMarkerResult rr = res();
    CHECK_EQ(w.srv.server->send(9, 0, &fifth, &rr), KILN_OK);
    CHECK(rr.status == KILN_MARKER_FULL && rr.committed == 0 && reason_starts(rr, "Full: 512 broadcast"));
    // a recipient sees at most 512: a per-player marker on top is refused for that player
    rr = res();
    CHECK_EQ(w.srv.server->send(9, 101, &fifth, &rr), KILN_OK);
    CHECK(rr.status == KILN_MARKER_FULL && rr.committed == 0);
    const size_t before = w.srv.changes[0].size();
    w.net.step();
    CHECK_EQ(w.srv.changes[0].size(), before);  // refusals made no event
    w.net.settle(200);
    CHECK_EQ(p.received().size(), size_t{512});
    // release_owner: one CLEARED for its scope, its records leave every receiver
    w.srv.server->release_owner(6);
    w.net.step();
    CHECK_EQ(w.srv.count(0, KILN_MARKER_CLEARED), size_t{1});
    CHECK(!w.srv.changes[0].empty() && w.srv.changes[0].back().cause == KILN_MARKER_OWNER_RETIRED && w.srv.changes[0].back().affected == 128);
    w.net.settle(80);
    CHECK_EQ(p.received().size(), size_t{384});
    kmk_test::end("link.limits_and_release_owner");
}

void error_rule() {
    kmk_test::begin();
    World w;
    Spec ok("k", "t");
    KilnMarkerResult r = res();
    w.srv.server->set_available(false);
    CHECK_EQ(w.srv.server->send(5, 0, &ok, &r), KILN_ERR_UNAVAILABLE);
    CHECK(std::string(r.reason) == "markers.send");
    w.srv.server->set_available(true);
    w.srv.on_thread = false;
    r = res();
    CHECK_EQ(w.srv.server->send(5, 0, &ok, &r), KILN_ERR_WRONG_THREAD);
    r = res();
    CHECK_EQ(w.srv.server->unsend(5, 0, ks("k"), &r), KILN_ERR_WRONG_THREAD);
    w.srv.on_thread = true;
    Spec tooltip("k", "t", KILN_MARKER_MAP, kCamp);
    tooltip.s.map_label_mode = KILN_MARKER_LABEL_NATIVE_TOOLTIP;
    r = res();
    CHECK_EQ(w.srv.server->send(5, 0, &tooltip, &r), KILN_ERR_UNAVAILABLE);
    CHECK(std::string(r.reason) == "markers.map_text");
    Spec bad("bad key", "t");
    r = res();
    CHECK_EQ(w.srv.server->send(5, 0, &bad, &r), KILN_ERR_INVALID_ARGUMENT);
    r = res();
    CHECK_EQ(w.srv.server->send(42, 0, &ok, &r), KILN_ERR_INVALID_ARGUMENT);  // not bound
    r = res();
    CHECK_EQ(w.srv.server->suppress_sent(5, 0, ks("k"), &r), KILN_ERR_INVALID_ARGUMENT);
    r = w.unsend(0, "k");
    CHECK(r.status == KILN_MARKER_NOT_FOUND && r.committed == 0);
    r = w.clear(0);  // an empty scope: Ok, nothing changed
    CHECK(r.status == KILN_MARKER_ACCEPTED && r.affected == 0);
    // broadcast without recipients: Ok, for later joins
    r = w.send(0, ok);
    CHECK(r.status == KILN_MARKER_ACCEPTED && r.committed == 1 && r.queued == 0 && r.refused == 0);
    // no server world: NotReady
    w.srv.server->world_stopping();
    w.srv.store.world_stopping(KILN_WORLD_SERVER);
    w.srv.take();
    CHECK(!w.srv.changes[0].empty() && w.srv.changes[0].back().kind == KILN_MARKER_CLEARED && w.srv.changes[0].back().cause == KILN_MARKER_WORLD_STOPPED);
    r = res();
    CHECK_EQ(w.srv.server->send(5, 0, &ok, &r), KILN_OK);
    CHECK_EQ(r.status, KILN_MARKER_NOT_READY);
    kmk_test::end("link.error_rule_and_world_stop");
}

void client_world_stop() {  // M12: a client leaving its world: one CLEARED per namespace; a new world starts empty
    kmk_test::begin();
    World w;
    CHECK_EQ(w.srv.store.bind(6, KilnStr{"other", 5}), KILN_OK);
    Proc& p = w.client();
    Conn& c = w.net.join(&p, 0x81, 101);
    w.net.settle();
    Spec a("a", "A"), b("b", "B");
    w.send(0, a);
    KilnMarkerResult r = res();
    CHECK_EQ(w.srv.server->send(6, 0, &b, &r), KILN_OK);
    w.net.settle();
    CHECK_EQ(p.received().size(), size_t{2});
    p.receiver->world_stopping();
    p.store.world_stopping(KILN_WORLD_CLIENT);
    p.take();
    CHECK_EQ(p.count(1, KILN_MARKER_CLEARED), size_t{2});
    for (const auto& ch : p.changes[1])
        if (ch.kind == KILN_MARKER_CLEARED) CHECK(ch.cause == KILN_MARKER_WORLD_STOPPED && ch.affected == 1);
    CHECK(p.received().empty());
    // rejoin: a new client world gets the server's current broadcast again
    p.store.world_started(KILN_WORLD_CLIENT);
    p.receiver->world_started();
    w.net.settle();
    CHECK_EQ(p.received().size(), size_t{2});
    (void)c;
    kmk_test::end("link.m12_client_world_stop_and_rejoin");
}

void bridge_rules() {
    kmk_test::begin();
    Proc host(KILN_SIDE_SERVER, kServerExe, false);
    // receive copies the bytes
    std::vector<uint8_t> b = {1, 2, 3};
    KilnMarkerEnvelope e{};
    e.size = sizeof e;
    e.role = 0;
    e.peer = 5;
    e.message = 8;
    e.peer_generation = 1;
    e.bytes = b.data();
    e.length = 3;
    CHECK_EQ(host.bridge->receive(&e), KILN_OK);
    b[0] = 9;
    auto q = host.inbox[0].take();
    CHECK(q.size() == 1 && q[0].bytes[0] == 1 && q[0].peer == 5 && q[0].message == 8);
    e.size = 8;
    CHECK_EQ(host.bridge->receive(&e), KILN_ERR_INVALID_ARGUMENT);
    // peer_up is idempotent per generation; peer_down needs the exact generation
    CHECK_EQ(up(*host.bridge, peer(0, 7, kRouteNetwork, 0, kNetworkReady, 3)), KILN_OK);
    CHECK_EQ(up(*host.bridge, peer(0, 7, kRouteNetwork, 11, kKiln, 3)), KILN_OK);
    CHECK_EQ(host.bridge->peer_down(0, 7, 4), KILN_ERR_NOT_FOUND);
    CHECK_EQ(host.bridge->peer_down(0, 7, 3), KILN_OK);
    KilnMarkerPeer bad = peer(0, 7, 3, 0, 0, 3);
    CHECK_EQ(host.bridge->peer_up(&bad), KILN_ERR_INVALID_ARGUMENT);
    // 32 registered peers at most
    for (uint32_t i = 0; i < 32; ++i) CHECK_EQ(up(*host.bridge, peer(0, 100 + i, kRouteNetwork, 0, 0, 1)), KILN_OK);
    CHECK_EQ(up(*host.bridge, peer(0, 200, kRouteNetwork, 0, 0, 1)), KILN_ERR_BUFFER_TOO_SMALL);
    // an unattached bridge answers unavailable
    Inbox i0, i1;
    Bridge none(nullptr, nullptr, &i0, &i1);
    e.size = sizeof e;
    CHECK_EQ(none.receive(&e), KILN_ERR_UNAVAILABLE);
    kmk_test::end("link.bridge_table_rules");
}

}  // namespace

int main(int argc, char** argv) {
    g_verbose = argc > 1 && std::strcmp(argv[1], "-v") == 0;
    g_digest_hex = synthetic_catalog()->digest_hex();
    g_pair.catalog_sha256 = g_digest_hex.c_str();
    handshake_and_catalog();
    wrong_catalog();
    m09_layers();
    updates_in_place();
    m10_partial_and_retry();
    m11_loss_and_resync();
    m11_bad_messages();
    receiver_limits();
    hosted_loopback();
    follow_poses();
    limits();
    error_rule();
    client_world_stop();
    bridge_rules();
    return kmk_test::finish("link_test");
}
