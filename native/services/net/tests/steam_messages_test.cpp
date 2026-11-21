// SPDX-License-Identifier: MIT
// host tests of kiln.net's Steam peer-to-peer messages (steam_messages.cpp) over a fake steam_api64 export
// table, under ASan/UBSan and TSan (test-steam-messages.sh). Numbered: 1 layout, 2 resolution
// order, 3 the open gate, 4 maintain and registration, 5 the session callback and its policy race, 6 send, 7 receive,
// 8 accept, 9 close, 10 exit. Faults (11) and the vtable slot (12) need Windows: steam_lease_test.cpp.
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "../steam_messages.h"

namespace st = knet::steam;

namespace {

int g_checks = 0, g_failures = 0;
#define CHECK(cond, ...)                                                   \
    do {                                                                   \
        ++g_checks;                                                        \
        if (!(cond)) {                                                     \
            ++g_failures;                                                  \
            std::printf("FAIL line %d: %s: ", __LINE__, #cond);            \
            std::printf(__VA_ARGS__);                                      \
            std::printf("\n");                                             \
        }                                                                  \
    } while (0)

// ---- the fake steam_api64 ----
struct Identity {
    int32_t type, size;
    union {
        uint64_t steam_id;
        uint32_t reserved[32];
    } data;
};
struct Message {  // SteamNetworkingMessage_t's start, as steam_messages.cpp reads it
    void* data;
    int32_t size;
    uint32_t connection;
    Identity peer;
    int64_t connection_user_data, received_at, message_number;
    void* free_data;
    void* release;
    int32_t channel, flags;
};
static_assert(offsetof(Message, release) == 184 && offsetof(Message, flags) == 196);

enum Export { kUserHandle, kServerHandle, kUserMsgs, kServerMsgs, kUserSockets, kServerSockets, kFindUser, kFindServer,
              kSend, kReceive, kAccept, kRelease, kIdentity, kRegister, kUnregister, kExports };
const char* const kNames[kExports] = {
    "SteamAPI_GetHSteamUser", "SteamGameServer_GetHSteamUser", "SteamAPI_SteamNetworkingMessages_SteamAPI_v002",
    "SteamAPI_SteamGameServerNetworkingMessages_SteamAPI_v002", "SteamAPI_SteamNetworkingSockets_SteamAPI_v013",
    "SteamAPI_SteamGameServerNetworkingSockets_SteamAPI_v013", "SteamInternal_FindOrCreateUserInterface",
    "SteamInternal_FindOrCreateGameServerInterface", "SteamAPI_ISteamNetworkingMessages_SendMessageToUser",
    "SteamAPI_ISteamNetworkingMessages_ReceiveMessagesOnChannel",
    "SteamAPI_ISteamNetworkingMessages_AcceptSessionWithUser", "SteamAPI_SteamNetworkingMessage_t_Release",
    "SteamAPI_ISteamNetworkingSockets_GetIdentity", "SteamAPI_RegisterCallback", "SteamAPI_UnregisterCallback"};

int g_messages_iface = 1, g_sockets_iface = 2;  // the interface objects' addresses are what matter

struct Received {  // what the fake's receive hands out next on a channel
    std::vector<uint8_t> payload;
    int32_t size;            // the size field (may lie: -1, or bigger than the payload)
    bool null_data = false;
    Identity peer;
    int32_t flags;
};

struct Fake {
    std::atomic<int32_t> handle{0};
    std::atomic<int> calls[kExports]{};
    std::atomic<bool> missing[kExports]{};
    std::atomic<bool> accessor_null{false}, find_null{false}, sockets_null{false};
    std::atomic<int32_t> send_result{1};
    std::atomic<bool> accept_result{true};
    std::atomic<uint64_t> identity_id{76561198000000042ull};
    std::mutex mu;  // the rest
    std::vector<std::string> find_versions;
    std::map<int32_t, std::deque<Received>> queues;
    int32_t receive_count_override = 0;  // nonzero: the count receive answers (with the queue's front, or none)
    bool receive_null_message = false;
    std::map<Message*, int> released;    // releases per message handed out
    std::vector<Message*> handed;
    Identity last_send_peer{};
    uint32_t last_send_len = 0;
    int32_t last_send_flags = -1, last_send_channel = -1;
    Identity last_accept_peer{};
    std::atomic<void*> registered{nullptr};
    std::atomic<int> register_flags_at_call{-1};
    std::atomic<int32_t> register_id{0};
    std::atomic<bool> block_send{false};
    std::atomic<bool> in_send{false};
    std::function<void()> on_register;  // runs inside RegisterCallback (the re-entrant maintain test)
};
Fake* g_fake = nullptr;

void count(Export e) { g_fake->calls[e].fetch_add(1, std::memory_order_relaxed); }

int32_t f_user_handle() { count(kUserHandle); return g_fake->handle.load(); }
int32_t f_server_handle() { count(kServerHandle); return g_fake->handle.load(); }
void* f_user_msgs() { count(kUserMsgs); return g_fake->accessor_null ? nullptr : &g_messages_iface; }
void* f_server_msgs() { count(kServerMsgs); return g_fake->accessor_null ? nullptr : &g_messages_iface; }
void* f_user_sockets() { count(kUserSockets); return g_fake->accessor_null || g_fake->sockets_null ? nullptr : &g_sockets_iface; }
void* f_server_sockets() { count(kServerSockets); return g_fake->accessor_null || g_fake->sockets_null ? nullptr : &g_sockets_iface; }
void* find(int32_t h, const char* version) {
    {
        std::lock_guard l(g_fake->mu);
        g_fake->find_versions.emplace_back(version ? version : "");
    }
    if (g_fake->find_null || !h || !version) return nullptr;
    if (!std::strcmp(version, "SteamNetworkingMessages002")) return &g_messages_iface;
    if (!std::strcmp(version, "SteamNetworkingSockets013") && !g_fake->sockets_null) return &g_sockets_iface;
    return nullptr;
}
void* f_find_user(int32_t h, const char* v) { count(kFindUser); return find(h, v); }
void* f_find_server(int32_t h, const char* v) { count(kFindServer); return find(h, v); }
int32_t f_send(void* self, const Identity* peer, const void*, uint32_t len, int32_t flags, int32_t channel) {
    count(kSend);
    g_fake->in_send = true;
    while (g_fake->block_send.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    g_fake->in_send = false;
    std::lock_guard l(g_fake->mu);
    if (self != &g_messages_iface) return 8;
    g_fake->last_send_peer = *peer;
    g_fake->last_send_len = len;
    g_fake->last_send_flags = flags;
    g_fake->last_send_channel = channel;
    return g_fake->send_result.load();
}
int32_t f_receive(void* self, int32_t channel, Message** out, int32_t max) {
    count(kReceive);
    if (self != &g_messages_iface || max != 1) return 0;
    std::lock_guard l(g_fake->mu);
    auto& q = g_fake->queues[channel];
    if (g_fake->receive_null_message) {
        *out = nullptr;
        return g_fake->receive_count_override ? g_fake->receive_count_override : 1;
    }
    if (q.empty()) return g_fake->receive_count_override;  // 0, or a bogus count with no message
    Received r = std::move(q.front());
    q.pop_front();
    auto* m = new Message{};
    m->data = r.null_data ? nullptr : (r.payload.empty() ? nullptr : new uint8_t[r.payload.size()]);
    if (m->data) std::memcpy(m->data, r.payload.data(), r.payload.size());
    m->size = r.size;
    m->peer = r.peer;
    m->channel = channel;
    m->flags = r.flags;
    g_fake->released[m] = 0;
    g_fake->handed.push_back(m);
    *out = m;
    return g_fake->receive_count_override ? g_fake->receive_count_override : 1;
}
bool f_accept(void* self, const Identity* peer) {
    count(kAccept);
    std::lock_guard l(g_fake->mu);
    if (self != &g_messages_iface) return false;
    g_fake->last_accept_peer = *peer;
    return g_fake->accept_result.load();
}
void f_release(Message* m) {
    count(kRelease);
    std::lock_guard l(g_fake->mu);
    auto it = g_fake->released.find(m);
    if (it != g_fake->released.end()) ++it->second;
}
bool f_identity(void* self, Identity* out) {
    count(kIdentity);
    if (self != &g_sockets_iface) return false;
    *out = Identity{};
    out->type = 16;
    out->size = 8;
    out->data.steam_id = g_fake->identity_id.load();
    return true;
}
void f_register(void* object, int32_t id) {
    count(kRegister);
    g_fake->register_flags_at_call = static_cast<const uint8_t*>(object)[8];
    static_cast<uint8_t*>(object)[8] |= 0x01;  // as steam_api64 does
    std::memcpy(static_cast<uint8_t*>(object) + 12, &id, sizeof id);
    g_fake->register_id = id;
    g_fake->registered.store(object);
    if (g_fake->on_register) g_fake->on_register();
}
void f_unregister(void*) { count(kUnregister); }

void* const kFns[kExports] = {
    reinterpret_cast<void*>(&f_user_handle), reinterpret_cast<void*>(&f_server_handle),
    reinterpret_cast<void*>(&f_user_msgs), reinterpret_cast<void*>(&f_server_msgs),
    reinterpret_cast<void*>(&f_user_sockets), reinterpret_cast<void*>(&f_server_sockets),
    reinterpret_cast<void*>(&f_find_user), reinterpret_cast<void*>(&f_find_server), reinterpret_cast<void*>(&f_send),
    reinterpret_cast<void*>(&f_receive), reinterpret_cast<void*>(&f_accept), reinterpret_cast<void*>(&f_release),
    reinterpret_cast<void*>(&f_identity), reinterpret_cast<void*>(&f_register), reinterpret_cast<void*>(&f_unregister)};

std::atomic<int> g_lookups{0};
void* lookup(const char* name, void*) {
    g_lookups.fetch_add(1);
    for (int i = 0; i < kExports; ++i)
        if (!std::strcmp(name, kNames[i])) return g_fake->missing[i] ? nullptr : kFns[i];
    return nullptr;
}

std::mutex g_log_mu;
std::vector<std::string> g_log;
void log(int, const char* text) {
    std::lock_guard l(g_log_mu);
    g_log.emplace_back(text);
}
int log_count(const char* needle) {
    std::lock_guard l(g_log_mu);
    int n = 0;
    for (const auto& s : g_log) n += s.find(needle) != std::string::npos;
    return n;
}

constexpr KilnOwner kDialect = 77;
std::atomic<KilnOwner> g_dialect{kDialect};
KilnOwner KILN_CALL dialect_owner() { return g_dialect.load(); }

int total_calls(bool except_handles = false) {
    int n = 0;
    for (int i = 0; i < kExports; ++i)
        if (!except_handles || (i != kUserHandle && i != kServerHandle)) n += g_fake->calls[i].load();
    return n;
}
void zero_calls() {
    for (auto& c : g_fake->calls) c.store(0);
}

// A fresh fake and a fresh lease module. ready: the user handle is nonzero.
void fresh(bool server = false, bool ready = true) {
    delete g_fake;
    g_fake = new Fake;
    g_fake->handle = ready ? (server ? 2 : 1) : 0;
    st::reset_for_tests();
    {
        std::lock_guard l(g_log_mu);
        g_log.clear();
    }
    g_dialect = kDialect;
    st::Config c;
    c.server = server;
    c.lookup = &lookup;
    c.dialect_owner = &dialect_owner;
    c.log = &log;
    const char* missing = nullptr;
    const st::Presence p = st::init(c, &missing);
    CHECK(p == st::Presence::Complete && !missing, "fresh: complete");
}

KilnSteamMessagesStatus status_now() {
    KilnSteamMessagesStatus s{};
    s.size = sizeof s;
    st::st_status(&s);
    return s;
}

KilnHandle open_lease(uint32_t first, uint32_t last, bool sessions, KilnResult* rc = nullptr, KilnOwner owner = kDialect) {
    KilnSteamMessagesOpen req{sizeof req, sessions ? static_cast<uint32_t>(KILN_STEAM_OPEN_SESSIONS) : 0u, first, last};
    KilnHandle h = 0;
    const KilnResult r = st::st_open(owner, &req, &h);
    if (rc) *rc = r;
    return r == KILN_OK ? h : 0;
}

KilnResult maintain(KilnHandle lease, uint32_t mode, std::vector<uint64_t> ids = {}, KilnSteamMessagesStatus* out = nullptr) {
    KilnSteamAcceptPolicy p{sizeof p, mode, ids.empty() ? nullptr : ids.data(), static_cast<uint32_t>(ids.size()), 0};
    return st::st_maintain(lease, &p, out);
}

using RunFn = void (*)(void*);
// Steam's dispatch, on the host: the object's Run(void*) through C++ (the slot order is Windows' business: test 12).
struct CallbackShape {
    virtual void Run(void*) = 0;
    virtual void Run(void*, bool, uint64_t) = 0;
    virtual int32_t GetCallbackSizeBytes() = 0;
};
void dispatch(const Identity& request) {
    void* object = g_fake->registered.load();
    if (object) static_cast<CallbackShape*>(object)->Run(const_cast<Identity*>(&request));
}
Identity steam(uint64_t id, int32_t type = 16, int32_t size = 8) {
    Identity r{};
    r.type = type;
    r.size = size;
    r.data.steam_id = id;
    return r;
}

// ---- 1. layout ----
void t1_layout() {
    CHECK(sizeof(KilnNetApi) == 184 && offsetof(KilnNetApi, steam_status) == 128 && offsetof(KilnNetApi, steam_accept) == 176,
          "the table is 184 bytes, the Steam members at 128..176");
    CHECK(sizeof(KilnSteamMessagesStatus) == 48 && sizeof(KilnSteamMessagesOpen) == 16 &&
              sizeof(KilnSteamAcceptPolicy) == 24 && sizeof(KilnSteamMessageInfo) == 24,
          "record sizes 48/16/24/24");
    // what a reader of a 128-byte kiln.net (main's) sees: the members are absent
    KilnServiceHeader old_table{128, KILN_NET_VERSION};
    CHECK(old_table.size < offsetof(KilnNetApi, steam_status) + sizeof(void*), "a 128-byte table has no steam_status");
    auto* cb = static_cast<const uint8_t*>(st::callback_object_for_tests());
    int32_t id = 0;
    std::memcpy(&id, cb + 12, sizeof id);
    CHECK(cb[8] == 0 && id == 1251, "the callback object: flags @8 = 0, id @12 = 1251 before registration");
}

// ---- 2. resolution ----
void t2_resolution() {
    // no steam_api64.dll
    delete g_fake;
    g_fake = new Fake;
    st::reset_for_tests();
    st::Config none;
    none.dialect_owner = &dialect_owner;
    const char* missing = nullptr;
    CHECK(st::init(none, &missing) == st::Presence::NoModule && missing && !std::strcmp(missing, "steam_api64.dll"),
          "no module");
    KilnSteamMessagesStatus s = status_now();
    CHECK(!(s.flags & KILN_STEAM_MSG_AVAILABLE) && total_calls() == 0, "no module: not available, nothing called");
    KilnHandle lease = open_lease(0, 10, true);
    int32_t er = -1;
    CHECK(lease && st::st_send(lease, 5, "x", 1, 1, 8, &er) == KILN_ERR_UNAVAILABLE, "no module: send unavailable");
    // a missing export
    delete g_fake;
    g_fake = new Fake;
    g_fake->missing[kRegister] = true;
    g_fake->handle = 1;
    st::reset_for_tests();
    st::Config c;
    c.lookup = &lookup;
    c.dialect_owner = &dialect_owner;
    CHECK(st::init(c, &missing) == st::Presence::MissingExport && missing && !std::strcmp(missing, "SteamAPI_RegisterCallback"),
          "missing RegisterCallback named (%s)", missing ? missing : "-");
    s = status_now();
    CHECK(!(s.flags & KILN_STEAM_MSG_AVAILABLE) && total_calls() == 0, "missing export: not available, nothing called");
    // the accessor alone or FindOrCreate alone is enough for the messages interface
    delete g_fake;
    g_fake = new Fake;
    g_fake->missing[kUserMsgs] = true;
    g_fake->missing[kFindUser] = true;
    st::reset_for_tests();
    CHECK(st::init(c, &missing) == st::Presence::MissingExport && !std::strcmp(missing, "SteamAPI_SteamNetworkingMessages_SteamAPI_v002"),
          "neither accessor nor FindOrCreate: missing");

    // user handle 0: no other export is called by any member
    fresh(false, false);
    lease = open_lease(0, 100, true);
    zero_calls();
    s = status_now();
    CHECK(s.flags == KILN_STEAM_MSG_LEASED, "handle 0: status without AVAILABLE, only LEASED (%x)", s.flags);
    CHECK(maintain(lease, KILN_STEAM_ACCEPT_ANY) == KILN_OK, "handle 0: maintain OK");
    CHECK(st::st_send(lease, 5, "x", 1, 1, 8, &er) == KILN_ERR_UNAVAILABLE, "handle 0: send unavailable");
    KilnSteamMessageInfo info{sizeof info, 0, 0, 0, 0};
    uint32_t got = 9, acc = 9;
    uint8_t buf[8];
    CHECK(st::st_receive(lease, 1, buf, sizeof buf, &info, &got) == KILN_ERR_UNAVAILABLE && got == 0, "handle 0: receive");
    CHECK(st::st_accept(lease, 5, &acc) == KILN_ERR_UNAVAILABLE && acc == 0, "handle 0: accept");
    CHECK(total_calls(true) == 0 && g_fake->calls[kUserHandle] == 5, "handle 0: only the user handle was asked (%d other, %d handle)",
          total_calls(true), g_fake->calls[kUserHandle].load());
    CHECK(g_fake->calls[kRegister] == 0, "handle 0: maintain registered nothing");
    // Steam comes up: the next call goes on to the interface
    g_fake->handle = 1;
    CHECK(st::st_send(lease, 5, "x", 1, 1, 8, &er) == KILN_OK && er == 1, "handle 1: send OK");
    CHECK(g_fake->calls[kUserMsgs] == 1 && g_fake->calls[kFindUser] == 0, "the accessor answered: no fallback");
    CHECK(st::ready_seen(), "ready seen");

    // the accessor answers null: FindOrCreate with the same version; the sockets with 013
    g_fake->accessor_null = true;
    zero_calls();
    s = status_now();
    CHECK((s.flags & KILN_STEAM_MSG_AVAILABLE) && (s.flags & KILN_STEAM_MSG_IDENTITY) && s.local_steam_id == 76561198000000042ull,
          "fallback: available with the local id (%x)", s.flags);
    {
        std::lock_guard l(g_fake->mu);
        const auto& v = g_fake->find_versions;
        CHECK(v.size() == 2 && v[0] == "SteamNetworkingMessages002" && v[1] == "SteamNetworkingSockets013",
              "fallback versions: messages 002, sockets 013 (%zu)", v.size());
        g_fake->find_versions.clear();
    }
    CHECK(g_fake->calls[kUserSockets] == 1 && g_fake->calls[kFindUser] == 2, "sockets: v013 accessor, then FindOrCreate");
    // never caching a null: both null, then both back
    g_fake->find_null = true;
    CHECK(st::st_send(lease, 5, "x", 1, 1, 8, &er) == KILN_ERR_UNAVAILABLE, "no interface: unavailable");
    g_fake->accessor_null = false;
    g_fake->find_null = false;
    CHECK(st::st_send(lease, 5, "x", 1, 1, 8, &er) == KILN_OK && er == 1, "the interface is asked again: OK");
    // the sockets interface missing: available, no identity
    g_fake->sockets_null = true;
    s = status_now();
    CHECK((s.flags & KILN_STEAM_MSG_AVAILABLE) && !(s.flags & KILN_STEAM_MSG_IDENTITY) && s.local_steam_id == 0,
          "no sockets interface: no identity");

    // a server uses the GameServer exports only
    fresh(true, true);
    lease = open_lease(0, 10, true);
    zero_calls();
    s = status_now();
    CHECK((s.flags & KILN_STEAM_MSG_SERVER_IFACE) && (s.flags & KILN_STEAM_MSG_AVAILABLE), "server: GameServer interfaces");
    CHECK(g_fake->calls[kServerHandle] == 1 && g_fake->calls[kServerMsgs] == 1 && g_fake->calls[kServerSockets] == 1 &&
              g_fake->calls[kUserHandle] == 0 && g_fake->calls[kUserMsgs] == 0 && g_fake->calls[kUserSockets] == 0,
          "server: no user export called");
}

// ---- 3. the open gate ----
void t3_open() {
    fresh();
    zero_calls();
    KilnResult rc;
    CHECK(!open_lease(0, 10, true, &rc, 5) && rc == KILN_ERR_UNAVAILABLE, "wrong owner: unavailable");
    CHECK(!open_lease(0, 10, true, &rc, 0) && rc == KILN_ERR_UNAVAILABLE, "owner 0: unavailable");
    CHECK(log_count("steam-messages-owner") == 1, "the refusal is logged once");
    g_dialect = 0;
    CHECK(!open_lease(0, 10, true, &rc, kDialect) && rc == KILN_ERR_UNAVAILABLE, "no designated owner: unavailable");
    g_dialect = kDialect;
    const KilnHandle a = open_lease(0, 10, true, &rc);
    CHECK(a && rc == KILN_OK, "the designated owner opens a session lease");
    CHECK(!open_lease(10, 20, false, &rc) && rc == KILN_ERR_INVALID_ARGUMENT, "overlap at one channel refused");
    CHECK(!open_lease(5, 5, false, &rc) && rc == KILN_ERR_INVALID_ARGUMENT, "overlap inside refused");
    const KilnHandle b = open_lease(11, 20, false, &rc);
    CHECK(b && rc == KILN_OK && b != a, "an adjacent range is fine");
    CHECK(!open_lease(100, 200, true, &rc) && rc == KILN_ERR_INVALID_ARGUMENT, "a second session lease refused");
    CHECK(!open_lease(30, 29, false, &rc) && rc == KILN_ERR_INVALID_ARGUMENT, "first > last refused");
    CHECK(!open_lease(65530, 65536, false, &rc) && rc == KILN_ERR_INVALID_ARGUMENT, "a channel above 65535 refused");
    const KilnHandle top = open_lease(65535, 65535, false, &rc);
    CHECK(top && rc == KILN_OK, "65535 alone is fine");
    KilnSteamMessagesOpen short_req{8, 0, 40, 41};
    KilnHandle h = 0;
    CHECK(st::st_open(kDialect, &short_req, &h) == KILN_ERR_INVALID_ARGUMENT, "a short record refused");
    KilnSteamMessagesOpen bad_flags{sizeof bad_flags, 2, 40, 41};
    CHECK(st::st_open(kDialect, &bad_flags, &h) == KILN_ERR_INVALID_ARGUMENT, "unknown flags refused");
    CHECK(st::st_open(kDialect, nullptr, &h) == KILN_ERR_INVALID_ARGUMENT, "no record refused");
    int opened = 3;
    for (uint32_t c = 1000; opened < 16; c += 10, ++opened) CHECK(open_lease(c, c + 1, false), "lease %d", opened);
    CHECK(!open_lease(5000, 5001, false, &rc) && rc == KILN_ERR_INVALID_ARGUMENT, "the 17th lease refused");
    CHECK(total_calls() == 0, "open calls no Steam export (%d)", total_calls());
    CHECK(st::st_close(b) == KILN_OK && open_lease(5000, 5001, false), "a closed slot is reused");
}

// ---- 4. maintain ----
void t4_maintain(bool server) {
    fresh(server, false);
    const KilnHandle lease = open_lease(0, 10, true);
    const KilnHandle plain = open_lease(20, 30, false);
    KilnSteamMessagesStatus out{};
    out.size = sizeof out;
    // Steam not ready: OK, nothing changes
    CHECK(maintain(lease, KILN_STEAM_ACCEPT_LISTED, {5, 6}, &out) == KILN_OK && !(out.flags & KILN_STEAM_MSG_AVAILABLE) &&
              out.accept_mode == KILN_STEAM_ACCEPT_NONE && out.listed == 0,
          "not ready: nothing changes");
    CHECK(g_fake->calls[kRegister] == 0, "not ready: nothing registered");
    g_fake->handle = server ? 2 : 1;
    CHECK(maintain(plain, KILN_STEAM_ACCEPT_ANY) == KILN_ERR_INVALID_ARGUMENT, "a lease without sessions refused");
    CHECK(maintain(0x1234, KILN_STEAM_ACCEPT_ANY) == KILN_ERR_NOT_FOUND, "an unknown lease: not found");
    // the first policy that is not NONE registers, once
    CHECK(maintain(lease, KILN_STEAM_ACCEPT_LISTED, {5, 6}, &out) == KILN_OK, "listed");
    CHECK((out.flags & KILN_STEAM_MSG_AVAILABLE) && (out.flags & KILN_STEAM_MSG_REGISTERED) && (out.flags & KILN_STEAM_MSG_LEASED) &&
              out.accept_mode == KILN_STEAM_ACCEPT_LISTED && out.listed == 2,
          "registered, listed 2 (%x)", out.flags);
    CHECK(g_fake->calls[kRegister] == 1 && g_fake->register_id == 1251 && g_fake->registered.load() == st::callback_object_for_tests(),
          "RegisterCallback(static object, 1251) once");
    if (server)
        CHECK(g_fake->register_flags_at_call == 0x02, "server: the GameServer flag 0x02 set before the call (%d)",
              g_fake->register_flags_at_call.load());
    else
        CHECK(g_fake->register_flags_at_call == 0x00, "client: no GameServer flag (%d)", g_fake->register_flags_at_call.load());
    CHECK(log_count("session callback registered") == 1, "logged once");
    CHECK(maintain(lease, KILN_STEAM_ACCEPT_ANY, {}, &out) == KILN_OK && out.accept_mode == KILN_STEAM_ACCEPT_ANY && out.listed == 0,
          "any");
    CHECK(maintain(lease, KILN_STEAM_ACCEPT_NONE, {}, &out) == KILN_OK && out.accept_mode == KILN_STEAM_ACCEPT_NONE &&
              !(out.flags & KILN_STEAM_MSG_REGISTERED) && (out.flags & KILN_STEAM_MSG_LEASED),
          "NONE: no longer answering (%x)", out.flags);
    CHECK(maintain(lease, KILN_STEAM_ACCEPT_LISTED, {9}, &out) == KILN_OK && (out.flags & KILN_STEAM_MSG_REGISTERED),
          "listed again: answering again");
    CHECK(g_fake->calls[kRegister] == 1 && g_fake->calls[kUnregister] == 0, "still one RegisterCallback, never Unregister");
    // refused policies leave the policy as it was
    std::vector<uint64_t> many(257);
    for (size_t i = 0; i < many.size(); ++i) many[i] = 1000 + i;
    CHECK(maintain(lease, KILN_STEAM_ACCEPT_LISTED, many) == KILN_ERR_INVALID_ARGUMENT, "257 ids refused");
    many.resize(256);
    CHECK(maintain(lease, KILN_STEAM_ACCEPT_LISTED, {7, 0, 8}) == KILN_ERR_INVALID_ARGUMENT, "an id 0 refused");
    KilnSteamAcceptPolicy no_ids{sizeof no_ids, KILN_STEAM_ACCEPT_LISTED, nullptr, 3, 0};
    CHECK(st::st_maintain(lease, &no_ids, nullptr) == KILN_ERR_INVALID_ARGUMENT, "ids NULL refused");
    KilnSteamAcceptPolicy bad_mode{sizeof bad_mode, 3, nullptr, 0, 0};
    CHECK(st::st_maintain(lease, &bad_mode, nullptr) == KILN_ERR_INVALID_ARGUMENT, "mode 3 refused");
    KilnSteamAcceptPolicy short_p{16, KILN_STEAM_ACCEPT_ANY, nullptr, 0, 0};
    CHECK(st::st_maintain(lease, &short_p, nullptr) == KILN_ERR_INVALID_ARGUMENT, "a short record refused");
    const KilnSteamMessagesStatus s = status_now();
    CHECK(s.accept_mode == KILN_STEAM_ACCEPT_LISTED && s.listed == 1, "refusals changed nothing (mode %u, %u ids)",
          s.accept_mode, s.listed);
    CHECK(maintain(lease, KILN_STEAM_ACCEPT_LISTED, many) == KILN_OK && status_now().listed == 256, "256 ids are fine");
    // a maintain that runs while RegisterCallback is still inside Steam (another thread's) never registers again
    fresh(server, true);
    const KilnHandle l2 = open_lease(0, 10, true);
    int inner = -1;
    g_fake->on_register = [&] {  // once: a second registration must not recurse for ever
        if (inner != -1) return;
        inner = -2;
        inner = maintain(l2, KILN_STEAM_ACCEPT_ANY);
    };
    CHECK(maintain(l2, KILN_STEAM_ACCEPT_ANY) == KILN_OK && inner == KILN_OK, "re-entered maintain");
    CHECK(g_fake->calls[kRegister] == 1, "the flag is set before the call: one RegisterCallback (%d)",
          g_fake->calls[kRegister].load());
    g_fake->on_register = nullptr;
}

// ---- 5. the session callback ----
void t5_callback() {
    fresh();
    const KilnHandle lease = open_lease(0, 10, true);
    CHECK(maintain(lease, KILN_STEAM_ACCEPT_LISTED, {100, 200}) == KILN_OK, "listed 100, 200");
    zero_calls();
    dispatch(steam(100));
    CHECK(g_fake->calls[kAccept] == 1 && g_fake->last_accept_peer.data.steam_id == 100, "a listed peer is accepted");
    dispatch(steam(300));
    dispatch(steam(100, 1, 8));
    dispatch(steam(100, 16, 4));
    dispatch(steam(0));
    CHECK(g_fake->calls[kAccept] == 1, "not listed, wrong type, wrong size, id 0: no AcceptSessionWithUser");
    g_fake->accept_result = false;
    dispatch(steam(200));
    g_fake->accept_result = true;
    KilnSteamMessagesStatus s = status_now();
    CHECK(s.sessions_accepted == 1 && s.sessions_refused == 5, "counters: 1 accepted, 5 refused (%llu, %llu)",
          (unsigned long long)s.sessions_accepted, (unsigned long long)s.sessions_refused);
    CHECK(maintain(lease, KILN_STEAM_ACCEPT_ANY) == KILN_OK, "any");
    dispatch(steam(300));
    CHECK(g_fake->calls[kAccept] == 3 && g_fake->last_accept_peer.data.steam_id == 300, "ANY accepts anyone");
    CHECK(maintain(lease, KILN_STEAM_ACCEPT_NONE) == KILN_OK, "none");
    dispatch(steam(300));
    CHECK(g_fake->calls[kAccept] == 3, "NONE answers nothing");
    CHECK(maintain(lease, KILN_STEAM_ACCEPT_ANY) == KILN_OK && st::st_close(lease) == KILN_OK, "any, then close");
    dispatch(steam(300));
    CHECK(g_fake->calls[kAccept] == 3, "a closed session lease answers nothing");
    g_fake->handle = 0;
    const KilnHandle again = open_lease(0, 10, true);
    g_fake->handle = 1;
    CHECK(maintain(again, KILN_STEAM_ACCEPT_ANY) == KILN_OK, "a new session lease");
    g_fake->handle = 0;
    zero_calls();
    dispatch(steam(300));
    CHECK(total_calls(true) == 0 && g_fake->calls[kUserHandle] == 1, "Steam gone (handle 0): only the handle is asked");
    // NONE calls no Steam function and only narrows: it is in force at once, also while Steam is not ready, so the
    // callback never answers on the policy it replaced once Steam is back; a policy that answers still waits for Steam
    zero_calls();
    KilnSteamMessagesStatus out{};
    out.size = sizeof out;
    CHECK(maintain(again, KILN_STEAM_ACCEPT_NONE, {}, &out) == KILN_OK && !(out.flags & KILN_STEAM_MSG_AVAILABLE) &&
              out.accept_mode == KILN_STEAM_ACCEPT_NONE && !(out.flags & KILN_STEAM_MSG_REGISTERED),
          "NONE while Steam is not ready: in force at once (mode %u, flags %x)", out.accept_mode, out.flags);
    CHECK(total_calls(true) == 0, "NONE while Steam is not ready: no Steam call but the handle (%d)", total_calls(true));
    CHECK(maintain(again, KILN_STEAM_ACCEPT_ANY, {}, &out) == KILN_OK && out.accept_mode == KILN_STEAM_ACCEPT_NONE,
          "ANY while Steam is not ready: nothing changes (mode %u)", out.accept_mode);
    g_fake->handle = 1;
    dispatch(steam(300));
    CHECK(g_fake->calls[kAccept] == 0, "Steam back: the request is not accepted on the replaced policy");
    CHECK(maintain(again, KILN_STEAM_ACCEPT_ANY) == KILN_OK && status_now().accept_mode == KILN_STEAM_ACCEPT_ANY,
          "ANY once Steam is ready");
    dispatch(steam(300));
    CHECK(g_fake->calls[kAccept] == 1, "and the next request is accepted");
}

// The policy race (TSan): a writer cycles A (256 ids), B (1 id), C (256 ids) while the pump thread asks about ids of
// all three. A consistent read accepts only ids of a policy that was in force during the read; a torn read can see C's
// count with A's stale ids past B's one id. Generation g (published after maintain returns) means policy g-1 is set.
void t5_race(int iterations) {
    fresh();
    const KilnHandle lease = open_lease(0, 10, true);
    std::vector<uint64_t> sets[3];
    for (uint64_t i = 0; i < 256; ++i) sets[0].push_back(0x1000000 + i);
    sets[1].push_back(0x2000000);
    for (uint64_t i = 0; i < 256; ++i) sets[2].push_back(0x3000000 + i);
    CHECK(maintain(lease, KILN_STEAM_ACCEPT_LISTED, sets[2]) == KILN_OK, "start with C");  // so A's slots get stale
    std::atomic<uint64_t> gen{0};
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> violations{0}, accepted{0}, probes{0};
    g_fake->calls[kAccept] = 0;
    std::thread writer([&] {
        for (int k = 0; k < iterations && !stop; ++k) {
            maintain(lease, KILN_STEAM_ACCEPT_LISTED, sets[k % 3]);
            gen.store(static_cast<uint64_t>(k) + 1, std::memory_order_seq_cst);
        }
        stop = true;
    });
    std::thread pump([&] {
        std::mt19937_64 rng(12345);
        while (!stop.load()) {
            const int j = static_cast<int>(rng() % 3);
            // mostly A's ids in the upper slots: what a torn read during a write of C would still find
            const uint64_t id = j == 1 ? sets[1][0] : sets[j][128 + rng() % 128];
            const uint64_t g1 = gen.load(std::memory_order_seq_cst);
            const int before = g_fake->calls[kAccept].load();
            dispatch(steam(id));
            const bool ok = g_fake->calls[kAccept].load() != before;
            const uint64_t g2 = gen.load(std::memory_order_seq_cst);
            probes.fetch_add(1);
            if (!ok) continue;
            accepted.fetch_add(1);
            if (g2 + 1 - g1 >= 2) continue;  // three or more policies in the window: any id may be right
            // policies in force during the window: generations g1 .. g2+1 (policy g-1; before gen 1, C)
            bool allowed = false;
            for (uint64_t g = g1; g <= g2 + 1; ++g) allowed = allowed || (g == 0 ? 2 : static_cast<int>((g - 1) % 3)) == j;
            if (!allowed) violations.fetch_add(1);
        }
    });
    writer.join();
    pump.join();
    CHECK(violations == 0, "the reader never accepted an id outside the policies in force (%llu of %llu accepted, %llu probes)",
          (unsigned long long)violations.load(), (unsigned long long)accepted.load(), (unsigned long long)probes.load());
    CHECK(accepted > 0, "some requests were accepted");
}

// ---- 6. send ----
void t6_send() {
    fresh();
    const KilnHandle lease = open_lease(10, 20, false);
    std::vector<uint8_t> big(524289, 0x5a);
    int32_t er = -1;
    zero_calls();
    CHECK(st::st_send(lease, 7, big.data(), 524288, 10, 40, &er) == KILN_OK && er == 1 && g_fake->last_send_len == 524288,
          "524,288 bytes go");
    CHECK(st::st_send(lease, 7, big.data(), 524289, 10, 40, &er) == KILN_ERR_INVALID_ARGUMENT, "524,289 refused");
    CHECK(st::st_send(lease, 7, nullptr, 0, 20, 0, &er) == KILN_OK && g_fake->last_send_len == 0 && g_fake->last_send_channel == 20,
          "0 bytes go");
    CHECK(st::st_send(lease, 7, nullptr, 3, 10, 8, &er) == KILN_ERR_INVALID_ARGUMENT, "bytes without data refused");
    CHECK(st::st_send(lease, 7, "x", 1, 9, 8, &er) == KILN_ERR_INVALID_ARGUMENT, "channel 9 is outside 10..20");
    CHECK(st::st_send(lease, 7, "x", 1, 21, 8, &er) == KILN_ERR_INVALID_ARGUMENT, "channel 21 is outside");
    for (uint32_t f : {1u, 16u, 48u, 0xFFFFFFFFu})
        CHECK(st::st_send(lease, 7, "x", 1, 10, f, &er) == KILN_ERR_INVALID_ARGUMENT, "flags %u refused", f);
    CHECK(st::st_send(lease, 0, "x", 1, 10, 8, &er) == KILN_ERR_INVALID_ARGUMENT, "peer 0 refused");
    CHECK(st::st_send(lease, 7, "x", 1, 10, 8, nullptr) == KILN_ERR_INVALID_ARGUMENT, "no result pointer refused");
    CHECK(g_fake->calls[kSend] == 2, "the refusals called nothing (%d sends)", g_fake->calls[kSend].load());
    for (uint32_t f : {0u, 8u, 32u, 40u})
        CHECK(st::st_send(lease, 7, "x", 1, 15, f, &er) == KILN_OK && g_fake->last_send_flags == static_cast<int32_t>(f),
              "flags %u passed through", f);
    CHECK(g_fake->last_send_peer.type == 16 && g_fake->last_send_peer.size == 8 && g_fake->last_send_peer.data.steam_id == 7,
          "the peer as a SteamID identity");
    g_fake->send_result = 3;
    CHECK(st::st_send(lease, 7, "x", 1, 10, 8, &er) == KILN_OK && er == 3, "EResult passed through with OK");
    g_fake->handle = 0;
    CHECK(st::st_send(lease, 7, "x", 1, 10, 8, &er) == KILN_ERR_UNAVAILABLE, "Steam not ready: unavailable");
    g_fake->handle = 1;
    CHECK(st::st_send(0x5354000000000100ull, 7, "x", 1, 10, 8, &er) == KILN_ERR_NOT_FOUND, "an unknown lease: not found");
}

// ---- 7. receive ----
void push(int32_t channel, std::vector<uint8_t> payload, int32_t size, uint64_t peer, int32_t flags, int32_t type = 16,
          bool null_data = false) {
    std::lock_guard l(g_fake->mu);
    g_fake->queues[channel].push_back(Received{std::move(payload), size, null_data, steam(peer, type), flags});
}
bool each_released_once(size_t* handed) {
    std::lock_guard l(g_fake->mu);
    *handed = g_fake->handed.size();
    for (auto& [m, n] : g_fake->released)
        if (n != 1) return false;
    return true;
}
void free_handed() {
    std::lock_guard l(g_fake->mu);
    for (Message* m : g_fake->handed) {
        delete[] static_cast<uint8_t*>(m->data);
        delete m;
    }
    g_fake->handed.clear();
    g_fake->released.clear();
}
void t7_receive() {
    fresh();
    const KilnHandle lease = open_lease(0, 50, false);
    uint8_t buf[16];
    KilnSteamMessageInfo info{};
    info.size = sizeof info;
    uint32_t got = 9;
    zero_calls();
    CHECK(st::st_receive(lease, 3, buf, sizeof buf, &info, &got) == KILN_OK && got == 0 && g_fake->calls[kRelease] == 0,
          "nothing queued: got 0, nothing released");
    push(3, {'h', 'i'}, 2, 900, 8);
    CHECK(st::st_receive(lease, 3, buf, sizeof buf, &info, &got) == KILN_OK && got == 1 && info.length == 2 &&
              !std::memcmp(buf, "hi", 2) && info.peer_steam_id == 900 && info.channel == 3 &&
              info.flags == KILN_STEAM_SEND_RELIABLE,
          "a reliable message");
    push(3, {'u'}, 1, 901, 0);
    CHECK(st::st_receive(lease, 3, buf, sizeof buf, &info, &got) == KILN_OK && got == 1 && info.flags == 0, "unreliable");
    push(3, {}, 0, 902, 8);
    CHECK(st::st_receive(lease, 3, nullptr, 0, &info, &got) == KILN_OK && got == 1 && info.length == 0, "empty into cap 0");
    push(3, std::vector<uint8_t>(17, 1), 17, 903, 8);
    CHECK(st::st_receive(lease, 3, buf, sizeof buf, &info, &got) == KILN_STEAM_ERR_INVALID_DATA && got == 0,
          "bigger than the buffer: invalid data");
    push(3, {'x'}, 1, 904, 8, 1);
    CHECK(st::st_receive(lease, 3, buf, sizeof buf, &info, &got) == KILN_STEAM_ERR_INVALID_DATA, "not a SteamID: invalid data");
    push(3, {'x'}, 1, 0, 8);
    CHECK(st::st_receive(lease, 3, buf, sizeof buf, &info, &got) == KILN_STEAM_ERR_INVALID_DATA, "SteamID 0: invalid data");
    push(3, {}, 4, 905, 8, 16, true);
    CHECK(st::st_receive(lease, 3, buf, sizeof buf, &info, &got) == KILN_STEAM_ERR_INVALID_DATA, "bytes without data");
    push(3, {}, -1, 906, 8);
    CHECK(st::st_receive(lease, 3, buf, sizeof buf, &info, &got) == KILN_STEAM_ERR_INVALID_DATA, "a negative size");
    {
        std::lock_guard l(g_fake->mu);
        g_fake->receive_count_override = 2;
    }
    push(3, {'x'}, 1, 907, 8);
    CHECK(st::st_receive(lease, 3, buf, sizeof buf, &info, &got) == KILN_ERR_INTERNAL && got == 0,
          "a count of 2: internal, released");
    {
        std::lock_guard l(g_fake->mu);
        g_fake->receive_count_override = -1;
    }
    CHECK(st::st_receive(lease, 3, buf, sizeof buf, &info, &got) == KILN_ERR_INTERNAL, "a count of -1 with nothing: internal");
    {
        std::lock_guard l(g_fake->mu);
        g_fake->receive_count_override = 0;
        g_fake->receive_null_message = true;
    }
    CHECK(st::st_receive(lease, 3, buf, sizeof buf, &info, &got) == KILN_ERR_INTERNAL, "a count of 1 with no message");
    {
        std::lock_guard l(g_fake->mu);
        g_fake->receive_null_message = false;
    }
    size_t handed = 0;
    CHECK(each_released_once(&handed) && handed == 9, "each of the %zu messages released exactly once", handed);
    CHECK(g_fake->calls[kRelease] == 9, "nine releases (%d)", g_fake->calls[kRelease].load());
    free_handed();
    CHECK(st::st_receive(lease, 51, buf, sizeof buf, &info, &got) == KILN_ERR_INVALID_ARGUMENT, "channel outside the lease");
    KilnSteamMessageInfo short_info{8, 0, 0, 0, 0};
    CHECK(st::st_receive(lease, 3, buf, sizeof buf, &short_info, &got) == KILN_ERR_INVALID_ARGUMENT, "a short info record");
    CHECK(st::st_receive(lease, 3, nullptr, 4, &info, &got) == KILN_ERR_INVALID_ARGUMENT, "cap without a buffer");
}

// ---- 8. accept ----
void t8_accept() {
    fresh();
    const KilnHandle lease = open_lease(0, 5, false);
    uint32_t acc = 9;
    CHECK(st::st_accept(lease, 1234, &acc) == KILN_OK && acc == 1 && g_fake->last_accept_peer.data.steam_id == 1234 &&
              g_fake->last_accept_peer.type == 16,
          "accepted");
    g_fake->accept_result = false;
    CHECK(st::st_accept(lease, 1234, &acc) == KILN_OK && acc == 0, "no pending session: 0");
    CHECK(st::st_accept(lease, 0, &acc) == KILN_ERR_INVALID_ARGUMENT, "peer 0");
    CHECK(st::st_accept(lease, 1, nullptr) == KILN_ERR_INVALID_ARGUMENT, "no result pointer");
    g_fake->handle = 0;
    CHECK(st::st_accept(lease, 1234, &acc) == KILN_ERR_UNAVAILABLE, "not ready");
}

// ---- 9. close ----
void t9_close() {
    fresh();
    const KilnHandle lease = open_lease(0, 5, true);
    CHECK(maintain(lease, KILN_STEAM_ACCEPT_ANY) == KILN_OK && (status_now().flags & KILN_STEAM_MSG_REGISTERED), "answering");
    CHECK(st::st_close(lease) == KILN_OK && st::st_close(lease) == KILN_OK && st::st_close(0) == KILN_OK, "close is idempotent");
    const KilnSteamMessagesStatus s = status_now();
    CHECK(!(s.flags & KILN_STEAM_MSG_REGISTERED) && !(s.flags & KILN_STEAM_MSG_LEASED) && s.accept_mode == KILN_STEAM_ACCEPT_NONE,
          "closed: not answering, no lease, NONE (%x)", s.flags);
    CHECK(g_fake->calls[kUnregister] == 0, "close never unregisters");
    int32_t er = 0;
    CHECK(st::st_send(lease, 5, "x", 1, 1, 8, &er) == KILN_ERR_NOT_FOUND, "a closed lease: not found");
    const KilnHandle again = open_lease(0, 5, true);
    CHECK(again && again != lease, "the range and the session role are free again, with a new handle");
}

// ---- 10. exit ----
void t10_exit() {
    fresh();
    const KilnHandle lease = open_lease(0, 5, true);
    CHECK(maintain(lease, KILN_STEAM_ACCEPT_ANY) == KILN_OK, "answering");
    // a member inside Steam holds the exit back, within its bound
    g_fake->block_send = true;
    std::thread sender([&] {
        int32_t er = 0;
        st::st_send(lease, 5, "x", 1, 1, 8, &er);
    });
    while (!g_fake->in_send.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(!st::close_for_exit(30), "the exit waits for a member inside Steam and gives up after its bound");
    g_fake->block_send = false;
    sender.join();
    CHECK(st::close_for_exit(1000), "then it is through");
    zero_calls();
    KilnSteamMessagesStatus s{};
    s.size = sizeof s;
    CHECK(st::st_status(&s) == KILN_ERR_UNAVAILABLE && (s.flags & KILN_STEAM_MSG_CLOSED_FOR_EXIT) &&
              !(s.flags & KILN_STEAM_MSG_REGISTERED),
          "status: closed for exit");
    int32_t er = 0;
    uint32_t got = 0, acc = 0;
    uint8_t buf[4];
    KilnSteamMessageInfo info{};
    info.size = sizeof info;
    KilnHandle h = 0;
    KilnSteamMessagesOpen req{sizeof req, 0, 100, 101};
    CHECK(st::st_send(lease, 5, "x", 1, 1, 8, &er) == KILN_ERR_UNAVAILABLE &&
              st::st_receive(lease, 1, buf, sizeof buf, &info, &got) == KILN_ERR_UNAVAILABLE &&
              st::st_accept(lease, 5, &acc) == KILN_ERR_UNAVAILABLE && maintain(lease, KILN_STEAM_ACCEPT_ANY) == KILN_ERR_UNAVAILABLE &&
              st::st_open(kDialect, &req, &h) == KILN_ERR_UNAVAILABLE && st::st_close(lease) == KILN_ERR_UNAVAILABLE,
          "every member: unavailable");
    dispatch(steam(300));
    CHECK(total_calls() == 0, "no Steam call after the exit flag (%d), no UnregisterCallback", total_calls());
    // no call starts once close_for_exit returned, with a thread still hammering
    fresh();
    const KilnHandle l2 = open_lease(0, 5, true);
    std::atomic<bool> go{true};
    std::thread hammer([&] {
        int32_t r = 0;
        while (go) st::st_send(l2, 5, "x", 1, 1, 8, &r);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    CHECK(st::close_for_exit(1000), "closed while sends run");
    const int at_close = g_fake->calls[kSend].load();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    go = false;
    hammer.join();
    CHECK(g_fake->calls[kSend].load() == at_close, "no send started after close_for_exit returned (%d then %d)", at_close,
          g_fake->calls[kSend].load());
}

}  // namespace

int main(int argc, char** argv) {
    const int race = argc > 1 ? std::atoi(argv[1]) : 100000;
    t1_layout();
    t2_resolution();
    t3_open();
    t4_maintain(false);
    t4_maintain(true);
    t5_callback();
    t5_race(race);
    t6_send();
    t7_receive();
    t8_accept();
    t9_close();
    t10_exit();
    delete g_fake;
    std::printf("steam messages host: %s %d checks (race %d iterations)\n", g_failures ? "FAIL" : "PASS", g_checks, race);
    return g_failures ? 1 : 0;
}
