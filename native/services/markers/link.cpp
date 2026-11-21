// SPDX-License-Identifier: MIT
// kiln.markers S7, S8, the link (dossier chapter 04 §4.4, §4.5): wire v1 (wire.h), the server layers and the server role
// of each bridge peer (server.h), the built-in receiver's client role (receiver.h), and the internal kiln.markers.bridge
// table the managed receiver (Kiln.Markers) drives (bridge.h). This file is the link's side of the glue (glue.h): it builds
// the parts from the core and forwards the glue's fixed calls to them.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <random>
#include <thread>

#include "bridge.h"
#include "glue.h"
#include "linkenv.h"
#include "receiver.h"
#include "server.h"
#include "sha256.h"

namespace kmk {

namespace {

uint64_t mono_ms() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

// Session tokens (server epochs, client nonces): unique, not secret. SHA-256 over a counter, both clocks, the thread and
// whatever std::random_device gives.
void token_bytes(uint8_t* out, size_t n) {
    static std::atomic<uint64_t> counter{0};
    Sha256 h;
    const uint64_t c = ++counter;
    const auto steady = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto wall = std::chrono::system_clock::now().time_since_epoch().count();
    const auto thread = std::hash<std::thread::id>{}(std::this_thread::get_id());
    h.update(&c, sizeof c);
    h.update(&steady, sizeof steady);
    h.update(&wall, sizeof wall);
    h.update(&thread, sizeof thread);
    h.update(&out, sizeof out);
    try {
        std::random_device rd;
        const uint32_t r[2] = {rd(), rd()};
        h.update(r, sizeof r);
    } catch (...) {
        // no entropy source: the clocks and the counter keep tokens unique in this process
    }
    const auto d = h.finish();
    for (size_t i = 0; i < n; ++i) out[i] = d[i % d.size()];
}

Inbox g_inbox[2];
std::unique_ptr<Server> g_server;
std::unique_ptr<Receiver> g_receiver;
std::unique_ptr<Bridge> g_bridge;
std::once_flag g_once;

void ensure() {
    std::call_once(g_once, [] {
        LinkEnv env;
        env.now_ms = &mono_ms;
        env.random = &token_bytes;
        env.on_server_thread = [] { return !g.api || !g.api->on_world_thread || g.api->on_world_thread(KILN_WORLD_SERVER); };
        env.follow = [](uint64_t serial, KilnVec3* at, uint32_t* reason) { return anchors_follow(serial, at, reason); };
        // a server-world lease of the sending owner -> its incarnation serial, with one reference the server keeps while a
        // stored definition follows it (anchors.cpp); without server anchors a followed send is UNAVAILABLE
        env.resolve_follow = [](KilnOwner owner, KilnMarkerAnchor lease, uint64_t* serial) -> KilnResult {
            char why[128];
            const uint32_t st = anchors_retain(owner, KILN_WORLD_SERVER, lease, serial, why, sizeof why);
            return st == KILN_MARKER_ACCEPTED ? KILN_OK : KILN_ERR_NOT_FOUND;
        };
        env.release_follow = [](uint64_t serial) { anchors_drop(serial); };
        env.log = [](int level, const char* text) { logf(level, "%s", text); };
        env.exe = g.exe;
        env.side = g.side;
        g_server = std::make_unique<Server>(*g.store, *g.catalog, g_inbox[kRoleServer], env);
        g_receiver = std::make_unique<Receiver>(*g.store, *g.catalog, *g.sw, g_inbox[kRoleClient], env);
        g_bridge = std::make_unique<Bridge>(g_server.get(), g_receiver.get(), &g_inbox[kRoleServer], &g_inbox[kRoleClient]);
        g_server->on_catalog_published = [] { catalog_from_link(); };
    });
}

constexpr char kSend[] = "markers.send";

// markers.send: the server layers and the receiver's link. Needs the world lifecycle (every store is a session's), the
// server world's step (the server role) and the built-in receiver's bridge (Kiln.Markers attaches it at the first
// service tick; it does not when the marker channel cannot carry 16384 bytes).
void report_send() {
    if (!g.lifecycle) {
        g_server->set_available(false);
        report(kSend, KILN_CAP_UNAVAILABLE, KILN_CAP_REASON_SYMBOL_MISSING, "needs world.lifecycle (sent markers end with their world)");
    } else if (!g.server_post) {
        g_server->set_available(false);
        report(kSend, KILN_CAP_UNAVAILABLE, KILN_CAP_REASON_FAULTED, "the server world's step could not be subscribed");
    } else if (!g_bridge->attached()) {
        g_server->set_available(false);
        report(kSend, KILN_CAP_PENDING, KILN_CAP_REASON_SESSION, "waits for the built-in receiver (Kiln.Markers) to attach the marker channel");
    } else {
        g_server->set_available(true);
        report(kSend, KILN_CAP_AVAILABLE, KILN_CAP_REASON_NONE, "");
    }
}

KilnResult KILN_CALL b_attach(KilnOwner owner) {
    ensure();
    const KilnResult rc = g_bridge->attach(owner);
    if (rc == KILN_OK) {
        report_send();
        logf(KILN_LOG_INFO, "markers: the built-in receiver attached the marker channel (kiln:markers v1)");
    }
    return rc;
}
KilnResult KILN_CALL b_peer_up(const KilnMarkerPeer* peer) {
    ensure();
    return g_bridge->peer_up(peer);
}
KilnResult KILN_CALL b_peer_down(uint32_t role, uint32_t peer, uint64_t generation) {
    ensure();
    return g_bridge->peer_down(role, peer, generation);
}
KilnResult KILN_CALL b_receive(const KilnMarkerEnvelope* envelope) {
    ensure();
    return g_bridge->receive(envelope);
}
KilnResult KILN_CALL b_pump(uint32_t role, uint32_t max_packets, KilnMarkerSendFn send, void* user, uint32_t* processed) {
    ensure();
    return g_bridge->pump(role, max_packets, send, user, processed, mono_ms());
}

}  // namespace

void link_ready() {
    ensure();
    report_send();
}

void link_world(const KilnWorldEvent& e) {
    ensure();
    if (e.kind == KILN_WORLD_SERVER) {
        if (e.event == KILN_WORLD_STARTED) g_server->world_started();
        else if (e.event == KILN_WORLD_STOPPING) g_server->world_stopping();
    } else if (e.kind == KILN_WORLD_CLIENT) {
        if (e.event == KILN_WORLD_STARTED) g_receiver->world_started();
        else if (e.event == KILN_WORLD_STOPPING) g_receiver->world_stopping();
    }
}

void link_server_post() {
    ensure();
    g_server->post();
}

void link_client_post() {
    ensure();
    g_receiver->post();
}

KilnResult link_send(KilnOwner owner, KilnEntity recipient, const KilnMarkerSpec* spec, KilnMarkerResult* r) {
    ensure();
    return g_server->send(owner, recipient, spec, r);
}
KilnResult link_unsend(KilnOwner owner, KilnEntity recipient, KilnStr key, KilnMarkerResult* r) {
    ensure();
    return g_server->unsend(owner, recipient, key, r);
}
KilnResult link_clear_sent(KilnOwner owner, KilnEntity recipient, KilnMarkerResult* r) {
    ensure();
    return g_server->clear_sent(owner, recipient, r);
}
KilnResult link_suppress_sent(KilnOwner owner, KilnEntity recipient, KilnStr key, KilnMarkerResult* r) {
    ensure();
    return g_server->suppress_sent(owner, recipient, key, r);
}
void link_release_owner(KilnOwner owner) {
    ensure();
    g_server->release_owner(owner);
}

const KilnMarkersBridgeApi kBridgeTable = {{sizeof(KilnMarkersBridgeApi), KILN_MARKERS_BRIDGE_VERSION},
                                           &b_attach, &b_peer_up, &b_peer_down, &b_receive, &b_pump};

}  // namespace kmk
