// SPDX-License-Identifier: MIT
// kiln.repl.net's message hooks and inject (kiln_repl.h message_hook / message_inject; the dossier's chapter 15 §4 M4
// "Receive", "Send", "Inject"; DESIGN slice C2). The plain logic is msgcore.h (host-tested); this file is the glue:
//   receive  mid hooks (kiln.hooks mid2) at the receive loops' dispatch sites, where rax holds the game channel: the
//            server's machine and player loops (a dedicated server; the same sites in a hosting client's own server
//            loop) and the client's two loops. The chain may PASS, DROP (rip := the loop's free site, the game's own
//            default case) or REWRITE in place (new size <= the old one, written back to the read result).
//   send     raw detours on the session's SessionAlloc (records the buffer and its socket: kind +0, composed channel +4,
//            peer index +8) and SessionCommit (the final bytes): PASS, REWRITE in place (size <= the old one) or DROP
//            (SessionAbort instead of the commit). The game's sends of channels 4/5 (job threads) and Kiln's channel 12
//            are never hooked.
//   inject   SessionAlloc + copy + SessionCommit on the machine socket of a server peer for the schema channels that
//            socket carries (1, 15, 19, 20). Channel 21 goes on the player socket, whose handle is not mapped yet: refused.
// Handlers run where the game runs the loop: the server's tick thread (a hosted world: its server thread) and the
// client's main thread. A handler fault is a pass: the message goes on as it was, the hook is flagged, removed at the
// next Pre and logged once. The hooks are in only while a hook for that direction exists.
#include <windows.h>

#include <cstring>
#include <mutex>

#include "kiln_hook.hpp"
#include "msgcore.h"
#include "repl.h"
#include "services/kiln_hooks.h"

namespace repl {
namespace {

constexpr uint32_t kMaxHooks = 64;
msg::HookEntry g_hooks[kMaxHooks];
uint64_t g_serial = 0;
std::mutex g_mu;  // registration (two threads in a hosted world)

const KilnHooksApi* g_hooks_api = nullptr;
bool g_recv_server = false, g_recv_client = false, g_send = false, g_inject = false;  // resolved
msg::RxSite g_sites[4];   // server machine, server player, client machine, client player
uintptr_t g_site_at[4] = {};
KilnHandle g_mid[4] = {};
bool g_mid_in[4] = {};

// ---- send: SessionAlloc / SessionCommit / SessionAbort ----
struct AllocRes {
    uint8_t status;
    uint8_t pad[7];
    uint64_t handle;
    uint64_t h1, h2;
    uint32_t size;
    uint32_t pad2;
    uint8_t* data;
    uint64_t capacity;
};
static_assert(sizeof(AllocRes) == 0x38 && offsetof(AllocRes, handle) == 8 && offsetof(AllocRes, size) == 0x20);
struct AllocSpec {
    uint64_t min_size, max_size;
    uint8_t explicit_size;
    uint8_t pad[7];
};
using AllocFn = AllocRes* (*)(AllocRes* out, uintptr_t session, uintptr_t socket, const AllocSpec* spec);
using CommitFn = void (*)(uintptr_t session, uint64_t* handle);
uintptr_t f_alloc = 0, f_commit = 0, f_abort = 0;
kiln::RawHook h_alloc, h_commit;
bool g_send_in = false;
int64_t c_channel = -1, c_gs_sockets = -1, c_sockets_machines = -1, c_machine_stride = -1, c_machine_sockets = -1;

// The socket of each open allocation of this thread (sends of one thread are sequential: alloc, copy, commit).
struct Open {
    AllocRes* res;
    uintptr_t socket;
};
thread_local Open t_open[8];
thread_local uint32_t t_open_n = 0;
alignas(16) thread_local uint8_t t_scratch[msg::kMaxMessage];

uint8_t side_here() {
    uint8_t s = 0;
    if (g_api->on_tick_thread()) s |= msg::kSideServer;
    if (side() == KILN_SIDE_CLIENT && g_api->on_world_thread(KILN_WORLD_CLIENT)) s |= msg::kSideClient;
    return s;
}

bool any_hook(uint32_t dir, uint8_t side_mask) {
    for (auto& h : g_hooks)
        if (h.handle.load(std::memory_order_acquire) && !h.removed.load(std::memory_order_relaxed) && (h.directions & dir) &&
            (h.side & side_mask))
            return true;
    return false;
}

AllocRes* d_alloc(AllocRes* out, uintptr_t session, uintptr_t socket, const AllocSpec* spec) {
    AllocRes* r = h_alloc.original<AllocFn>()(out, session, socket, spec);
    if (r && t_open_n < 8) t_open[t_open_n++] = Open{r, socket};
    return r;
}

// The socket's game channel and peer (msgcore's layout notes: kind +0, composed channel +4, peer index +8).
bool socket_info(uintptr_t socket, uint32_t* channel, uint8_t* player, uint32_t* peer_index) {
    __try {
        const uint8_t kind = *reinterpret_cast<const uint8_t*>(socket);
        const uint32_t composed = *reinterpret_cast<const uint32_t*>(socket + 4);
        if (c_channel < 12 || composed < static_cast<uint32_t>(c_channel - 12)) return false;
        *channel = composed - static_cast<uint32_t>(c_channel - 12);
        *player = kind == msg::kPlayerSocket ? 1 : 0;
        *peer_index = *reinterpret_cast<const uint32_t*>(socket + 8);
        return *channel < 25;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void d_commit(uintptr_t session, uint64_t* handle) {
    AllocRes* res = handle ? reinterpret_cast<AllocRes*>(reinterpret_cast<uint8_t*>(handle) - offsetof(AllocRes, handle)) : nullptr;
    uintptr_t socket = 0;
    for (uint32_t i = t_open_n; i-- > 0;)
        if (t_open[i].res == res) {
            socket = t_open[i].socket;
            t_open[i] = t_open[--t_open_n];
            break;
        }
    uint32_t channel = 0, peer_index = 0;
    uint8_t player = 0;
    const uint8_t sides = socket ? side_here() : 0;
    if (sides && socket_info(socket, &channel, &player, &peer_index) && msg::hookable(channel) && res->data &&
        res->size && res->size <= msg::kMaxMessage) {
        // the peer: a machine socket names its machine as index + 16, a player socket its player slot; on a client: the server (0)
        uint32_t peer = 0;
        if (sides & msg::kSideServer) peer = player ? peer_index : peer_of(peer_index >= 16 ? peer_index - 16 : peer_index, nullptr);
        const msg::ChainResult cr = msg::run_chain(g_hooks, kMaxHooks, sides, KILN_REPL_SEND, static_cast<uint8_t>(channel), peer,
                                                   player, res->data, res->size, t_scratch, sizeof t_scratch);
        if (cr.action == KILN_REPL_DROP) {
            reinterpret_cast<CommitFn>(f_abort)(session, handle);  // the message is not sent
            return;
        }
        if (cr.action == KILN_REPL_REWRITE) res->size = cr.length;
    }
    h_commit.original<CommitFn>()(session, handle);
}

// ---- receive: the dispatch sites ----
void KILN_CALL mid_rx(KilnRegisters* r, void* user) {
    const auto* site = static_cast<const msg::RxSite*>(user);
    const uint32_t channel = static_cast<uint32_t>(r->rax);
    if (!msg::hookable(channel)) return;
    const uint8_t side_mask = site->peer_kind == msg::kPeerServer ? msg::kSideClient : msg::kSideServer;
    const uint8_t* data = nullptr;
    uint64_t size = 0;
    uint32_t peer = 0;
    __try {
        if (!msg::rx_read(r, *site, &data, &size)) return;
        peer = msg::rx_peer(r, *site);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    const msg::ChainResult cr = msg::run_chain(g_hooks, kMaxHooks, side_mask, KILN_REPL_RECEIVE, static_cast<uint8_t>(channel), peer,
                                               site->player, const_cast<uint8_t*>(data), static_cast<uint32_t>(size), t_scratch,
                                               sizeof t_scratch);
    if (cr.action == KILN_REPL_DROP) msg::rx_drop(r, *site);
    else if (cr.action == KILN_REPL_REWRITE) msg::rx_set_size(r, *site, cr.length);
}

void apply_hooks() {
    // receive: the sites of a side are in while a receive hook of that side exists
    const bool want_server = any_hook(KILN_REPL_RECEIVE, msg::kSideServer), want_client = any_hook(KILN_REPL_RECEIVE, msg::kSideClient);
    for (int i = 0; i < 4; ++i) {
        const bool want = g_site_at[i] && (i < 2 ? want_server && g_recv_server : want_client && g_recv_client);
        if (want && !g_mid_in[i] && g_hooks_api) {
            static const char* labels[4] = {"repl/messages server machine", "repl/messages server player", "repl/messages client machine",
                                            "repl/messages client player"};
            KilnHookOrder order{};
            order.size = sizeof order;
            order.priority = -100;  // after other observers of the same site
            order.label = KilnStr{labels[i], std::strlen(labels[i])};
            g_mid_in[i] = g_hooks_api->mid2(g_owner, g_site_at[i], &order, &mid_rx, &g_sites[i], &g_mid[i]) == KILN_OK;
            logf(g_mid_in[i] ? KILN_LOG_INFO : KILN_LOG_WARN, "repl: %s hook %s", labels[i], g_mid_in[i] ? "installed" : "refused by kiln.hooks");
        } else if (!want && g_mid_in[i]) {
            g_hooks_api->revoke(g_mid[i]);
            g_mid_in[i] = false;
            g_mid[i] = 0;
        }
    }
    // send
    const bool want_send = g_send && any_hook(KILN_REPL_SEND, msg::kSideServer | msg::kSideClient);
    if (want_send && !g_send_in) {
        g_send_in = h_alloc.install(g_api, g_owner, f_alloc, reinterpret_cast<void*>(&d_alloc), "repl/messages SessionAlloc") &&
                    h_commit.install(g_api, g_owner, f_commit, reinterpret_cast<void*>(&d_commit), "repl/messages SessionCommit");
        if (!g_send_in) {
            h_alloc.revoke();
            h_commit.revoke();
        }
        logf(g_send_in ? KILN_LOG_INFO : KILN_LOG_WARN, "repl: message send hooks %s", g_send_in ? "installed (session alloc, commit)" : "could not be installed");
    } else if (!want_send && g_send_in) {
        h_alloc.revoke();
        h_commit.revoke();
        g_send_in = false;
        logf(KILN_LOG_INFO, "repl: message send hooks removed (no send hook)");
    }
}

uintptr_t machine_socket(uintptr_t gs, uint32_t peer, uint32_t channel, uintptr_t* session) {
    if (!gs || c_gs_sockets < 0 || c_sockets_machines < 0 || c_machine_stride <= 0 || c_machine_sockets < 0) return 0;
    __try {
        const uintptr_t set = *reinterpret_cast<const uintptr_t*>(gs + c_gs_sockets);
        if (!set) return 0;
        const uintptr_t table = *reinterpret_cast<const uintptr_t*>(set + c_sockets_machines);
        if (!table) return 0;
        const uintptr_t entry = table + static_cast<uintptr_t>(peer & 0x7f) * static_cast<uintptr_t>(c_machine_stride);
        if (*reinterpret_cast<const uint32_t*>(entry) != peer) return 0;
        const uintptr_t sockets = *reinterpret_cast<const uintptr_t*>(entry + c_machine_sockets);
        const uint64_t count = *reinterpret_cast<const uint64_t*>(entry + c_machine_sockets + 8);
        const int idx = msg::socket_index(channel, c_channel, count);
        if (!sockets || idx < 0) return 0;
        const uintptr_t socket = reinterpret_cast<const uintptr_t*>(sockets)[idx];
        if (!socket) return 0;
        *session = *reinterpret_cast<const uintptr_t*>(set);
        return socket;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

bool seh_send(uintptr_t session, uintptr_t socket, const void* data, uint32_t length, uint32_t* status) {
    __try {
        AllocRes res{};
        const AllocSpec spec{length, length, 1, {}};
        AllocRes* r = reinterpret_cast<AllocFn>(f_alloc)(&res, session, socket, &spec);
        if (!r || r->status != 0 || !r->data || r->capacity < length) {
            *status = r ? r->status : 0xff;
            if (r && r->handle) reinterpret_cast<CommitFn>(f_abort)(session, &r->handle);
            return false;
        }
        std::memcpy(r->data, data, length);
        r->size = length;
        reinterpret_cast<CommitFn>(f_commit)(session, &r->handle);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *status = 0xfe;
        return false;
    }
}

}  // namespace

void messages_start() {
    auto sym = [](const char* name, uintptr_t* out) { return g_api->symbol(KilnStr{name, std::strlen(name)}, out) == KILN_OK; };
    auto cst = [](const char* name, int64_t* out) { return g_api->constant(KilnStr{name, std::strlen(name)}, out) == KILN_OK; };
    auto feat = [](const char* name) { return g_api->feature(KilnStr{name, std::strlen(name)}); };
    g_hooks_api = static_cast<const KilnHooksApi*>(g_api->service(KilnStr{KILN_HOOKS_SERVICE, sizeof(KILN_HOOKS_SERVICE) - 1}, 2));
    const bool mid_ok = g_hooks_api && g_hooks_api->header.size >= offsetof(KilnHooksApi, mid2) + sizeof(g_hooks_api->mid2) && g_hooks_api->mid2;
    // server loops (a dedicated server, or a hosting client's own server)
    uintptr_t free_m = 0, free_p = 0;
    g_recv_server = mid_ok && feat("repl.messages.server") && sym("repl/ServerMachineDispatch", &g_site_at[0]) &&
                    sym("repl/ServerPlayerDispatch", &g_site_at[1]) && sym("repl/ServerMachineFree", &free_m) &&
                    sym("repl/ServerPlayerFree", &free_p);
    if (g_recv_server) {
        g_sites[0] = msg::RxSite{-0x68, -0x60, free_m, 0, msg::kPeerAtRdi};
        g_sites[1] = msg::RxSite{-0x68, -0x60, free_p, 1, msg::kPeerInEdi};
    } else {
        g_site_at[0] = g_site_at[1] = 0;
    }
    if (side() == KILN_SIDE_CLIENT) {
        g_recv_client = mid_ok && feat("repl.messages.client") && sym("repl/ClientMachineDispatch", &g_site_at[2]) &&
                        sym("repl/ClientPlayerDispatch", &g_site_at[3]) && sym("repl/ClientMachineFree", &free_m) &&
                        sym("repl/ClientPlayerFree", &free_p);
        if (g_recv_client) {
            g_sites[2] = msg::RxSite{-0x18, -0x10, free_m, 0, msg::kPeerServer};
            g_sites[3] = msg::RxSite{-0x18, -0x10, free_p, 1, msg::kPeerServer};
        } else {
            g_site_at[2] = g_site_at[3] = 0;
        }
    }
    g_send = sym("net/SessionAlloc", &f_alloc) && sym("net/SessionCommit", &f_commit) && sym("net/SessionAbort", &f_abort) &&
             cst("net/channel", &c_channel);
    g_inject = g_send && cst("net/gs.sockets", &c_gs_sockets) && cst("net/sockets.machines", &c_sockets_machines) &&
               cst("net/machine.stride", &c_machine_stride) && cst("net/machine.sockets", &c_machine_sockets);
    logf(KILN_LOG_INFO, "repl: message hooks: receive %s/%s, send %s, inject %s (hooks go in while a hook exists)",
         g_recv_server ? "server on" : "server off", g_recv_client ? "client on" : "client off", g_send ? "on" : "off",
         g_inject ? "on" : "off");
}

uint32_t messages_caps() {
    return (g_send ? KILN_REPL_NET_CAP_MESSAGES_SEND : 0u) | ((g_recv_server || g_recv_client) ? KILN_REPL_NET_CAP_MESSAGES_RECEIVE : 0u);
}

void messages_pre() {
    bool changed = false;
    for (auto& h : g_hooks) {
        if (!h.handle.load(std::memory_order_acquire)) continue;
        if (h.faulted.load(std::memory_order_acquire) && !h.logged.exchange(true)) {
            char where[160];
            describe(h.fault_where.load(std::memory_order_relaxed), where, sizeof where);
            logf(KILN_LOG_WARN, "repl: message hook of owner 0x%llx on channel %u faulted at %s; removed",
                 static_cast<unsigned long long>(h.owner), h.channel, where);
            h.removed.store(true, std::memory_order_release);
        }
        if (h.removed.load(std::memory_order_acquire)) {
            h.handle.store(0, std::memory_order_release);
            h.removed.store(false, std::memory_order_relaxed);
            h.faulted.store(false, std::memory_order_relaxed);
            h.logged.store(false, std::memory_order_relaxed);
            changed = true;
        }
    }
    if (changed) apply_hooks();
}

void messages_owner_retiring(KilnOwner owner) {
    for (auto& h : g_hooks)
        if (h.handle.load(std::memory_order_acquire) && h.owner == owner) h.removed.store(true, std::memory_order_release);
}

KilnResult messages_remove(KilnHandle h) {
    for (auto& e : g_hooks)
        if (h && e.handle.load(std::memory_order_acquire) == h) {
            e.removed.store(true, std::memory_order_release);  // never called again: the chain skips it at once
            return KILN_OK;
        }
    return KILN_ERR_NOT_FOUND;
}

KilnResult KILN_CALL t_message_hook(KilnOwner owner, uint8_t channel, uint32_t directions, KilnReplMessageFn fn, void* user,
                                    KilnHandle* out) {
    if (out) *out = 0;
    if (!fn || !out || !msg::hookable(channel) || !directions || (directions & ~(KILN_REPL_SEND | KILN_REPL_RECEIVE)))
        return KILN_ERR_INVALID_ARGUMENT;
    const uint8_t sides = side_here();
    if (!sides) return KILN_ERR_WRONG_THREAD;
    if ((directions & KILN_REPL_SEND) && !g_send) return KILN_ERR_UNAVAILABLE;
    if ((directions & KILN_REPL_RECEIVE) && !(((sides & msg::kSideServer) && g_recv_server) || ((sides & msg::kSideClient) && g_recv_client)))
        return KILN_ERR_UNAVAILABLE;
    std::lock_guard lock(g_mu);
    for (auto& e : g_hooks) {
        if (e.handle.load(std::memory_order_relaxed)) continue;
        e.owner = owner;
        e.fn = fn;
        e.user = user;
        e.channel = channel;
        e.directions = static_cast<uint8_t>(directions);
        e.side = sides;
        e.removed.store(false, std::memory_order_relaxed);
        e.faulted.store(false, std::memory_order_relaxed);
        e.logged.store(false, std::memory_order_relaxed);
        e.fault_where.store(0, std::memory_order_relaxed);
        e.bad.store(0, std::memory_order_relaxed);
        const KilnHandle h = kMessageTag | (++g_serial & ~kTagMask);
        e.handle.store(h, std::memory_order_release);
        apply_hooks();
        *out = h;
        return KILN_OK;
    }
    return KILN_ERR_INVALID_ARGUMENT;  // 64 hooks
}

KilnResult KILN_CALL t_message_inject(uint32_t peer, uint8_t channel, const void* data, uint32_t length) {
    if (!g_inject) return KILN_ERR_UNAVAILABLE;
    if (!g_api->on_tick_thread()) return KILN_ERR_WRONG_THREAD;
    if (!peer || !data || !length || length > msg::kMaxMessage || !msg::injectable(channel)) return KILN_ERR_INVALID_ARGUMENT;
    if (msg::inject_socket(channel) != msg::kMachineSocket) return KILN_ERR_INVALID_ARGUMENT;  // channel 21: the player socket, not mapped
    // a connected game peer only: the machine whose slot holds it now (kiln.repl's per-Pre snapshot), never a lab fake,
    // never a dedicated server's machine 0 (itself)
    bool connected = false;
    for (uint32_t m = 0; m < KILN_REPL_MACHINES && !connected; ++m) {
        uint8_t flags = 0;
        connected = peer_of(m, &flags) == peer && !(flags & KILN_REPL_PEER_FAKE) && (m != 0 || (flags & KILN_REPL_PEER_LOCAL));
    }
    if (!connected) return KILN_ERR_INVALID_ARGUMENT;
    uintptr_t session = 0;
    const uintptr_t socket = machine_socket(g_gs.load(std::memory_order_relaxed), peer, channel, &session);
    if (!socket || !session) return KILN_ERR_INVALID_ARGUMENT;  // not a connected peer (a lab fake has no session)
    uint32_t status = 0;
    if (!seh_send(session, socket, data, length, &status)) {
        logf(KILN_LOG_WARN, "repl: inject on channel %u to peer 0x%x: the session refused it (status %u)", channel, peer, status);
        return KILN_ERR_UNAVAILABLE;
    }
    return KILN_OK;
}

}  // namespace repl
