// SPDX-License-Identifier: MIT
// test infrastructure only. Tests the stand-in steam_api64.dll (standin.h) under Wine and on Windows:
//   steam-standin-test unit <port base>                      one process: readiness, init delay, the accessor-null
//                                                             fallback, cap, EResult/drop/fault knobs, release-once,
//                                                             roles, and the callback pump's vtable slot
//   steam-standin-test fabric a|b <port base> <sync dir>     two processes (start both): A (server role) gets B's
//                                                             session request, holds B's messages, lets the session
//                                                             time out (dropping them) so B's resent messages raise
//                                                             a second request, accepts and then receives them; A
//                                                             replies and B receives without a request (its own send
//                                                             opened the session)
// Exit code 0 = every check passed. The SessionRequestProbe class has the shape of ShroudForge's session callback
// (steam_network.cpp, MIT, Copyright (c) 2026 bonsaibauer).
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "standin.h"

namespace {

int g_checks = 0;
int g_failures = 0;
const char* g_who = "standin";

#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        ++g_checks;                                                            \
        if (!(cond)) {                                                         \
            ++g_failures;                                                      \
            std::printf("%s: FAIL line %d: %s: ", g_who, __LINE__, #cond);     \
            std::printf(__VA_ARGS__);                                          \
            std::printf("\n");                                                 \
            std::fflush(stdout);                                               \
        }                                                                      \
    } while (0)

struct Identity {
    int32_t type;
    int32_t size;
    union {
        uint64_t steam_id;
        uint8_t reserved[128];
    } data;
};
static_assert(sizeof(Identity) == 136);

Identity steam(uint64_t id) {
    Identity result{};
    result.type = 16;
    result.size = 8;
    result.data.steam_id = id;
    return result;
}

// The received message, read through raw offsets as Kiln and ShroudForge read it.
struct Message {
    const uint8_t* bytes;
    const void* data() const { return *reinterpret_cast<void* const*>(bytes); }
    int32_t size() const { return *reinterpret_cast<const int32_t*>(bytes + 8); }
    int32_t peer_type() const { return *reinterpret_cast<const int32_t*>(bytes + 16); }
    int32_t peer_size() const { return *reinterpret_cast<const int32_t*>(bytes + 20); }
    uint64_t peer() const { return *reinterpret_cast<const uint64_t*>(bytes + 24); }
    void (*release() const)(void*) { return *reinterpret_cast<void (*const*)(void*)>(bytes + 184); }
    int32_t channel() const { return *reinterpret_cast<const int32_t*>(bytes + 192); }
    int32_t flags() const { return *reinterpret_cast<const int32_t*>(bytes + 196); }
};

struct Api {
    HMODULE module{};
    void* (*user_messages)();
    void* (*server_messages)();
    void* (*user_sockets)();
    void* (*server_sockets)();
    int32_t (*send)(void*, const Identity*, const void*, uint32_t, int32_t, int32_t);
    int32_t (*receive)(void*, int32_t, void**, int32_t);
    bool (*accept)(void*, const Identity*);
    void (*release)(void*);
    bool (*get_identity)(void*, Identity*);
    void* (*find_user)(int32_t, const char*);
    void* (*find_server)(int32_t, const char*);
    int32_t (*user_handle)();
    int32_t (*server_handle)();
    void (*register_callback)(void*, int32_t);
    void (*unregister_callback)(void*);
    StandInSetIdentityFn set_identity;
    StandInPumpFn pump;
    StandInKnobFn knob;
};
Api api;

template <typename T>
bool bind_export(T& slot, const char* name) {
    slot = reinterpret_cast<T>(GetProcAddress(api.module, name));
    if (!slot) std::printf("%s: missing export %s\n", g_who, name);
    return slot != nullptr;
}

bool load() {
    wchar_t path[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (!n || n >= MAX_PATH) return false;
    std::wstring dll(path, n);
    dll.resize(dll.find_last_of(L"\\/") + 1);
    dll += L"steam_api64.dll";  // only the stand-in next to this test, never another copy
    api.module = LoadLibraryW(dll.c_str());
    if (!api.module) {
        std::printf("%s: cannot load the stand-in steam_api64.dll next to the test\n", g_who);
        return false;
    }
    bool ok = true;
    ok &= bind_export(api.user_messages, "SteamAPI_SteamNetworkingMessages_SteamAPI_v002");
    ok &= bind_export(api.server_messages, "SteamAPI_SteamGameServerNetworkingMessages_SteamAPI_v002");
    ok &= bind_export(api.user_sockets, "SteamAPI_SteamNetworkingSockets_SteamAPI_v013");
    ok &= bind_export(api.server_sockets, "SteamAPI_SteamGameServerNetworkingSockets_SteamAPI_v013");
    ok &= bind_export(api.send, "SteamAPI_ISteamNetworkingMessages_SendMessageToUser");
    ok &= bind_export(api.receive, "SteamAPI_ISteamNetworkingMessages_ReceiveMessagesOnChannel");
    ok &= bind_export(api.accept, "SteamAPI_ISteamNetworkingMessages_AcceptSessionWithUser");
    ok &= bind_export(api.release, "SteamAPI_SteamNetworkingMessage_t_Release");
    ok &= bind_export(api.get_identity, "SteamAPI_ISteamNetworkingSockets_GetIdentity");
    ok &= bind_export(api.find_user, "SteamInternal_FindOrCreateUserInterface");
    ok &= bind_export(api.find_server, "SteamInternal_FindOrCreateGameServerInterface");
    ok &= bind_export(api.user_handle, "SteamAPI_GetHSteamUser");
    ok &= bind_export(api.server_handle, "SteamGameServer_GetHSteamUser");
    ok &= bind_export(api.register_callback, "SteamAPI_RegisterCallback");
    ok &= bind_export(api.unregister_callback, "SteamAPI_UnregisterCallback");
    ok &= bind_export(api.set_identity, "StandIn_SetIdentity");
    ok &= bind_export(api.pump, "StandIn_Pump");
    ok &= bind_export(api.knob, "StandIn_Knob");
    return ok;
}

uint64_t read_knob(uint32_t knob) { return api.knob(knob | STANDIN_KNOB_READ, 0); }
uint64_t take_knob(uint32_t knob) { return api.knob(knob, 0); }

struct ProbeLog {
    int run1 = 0;
    int run3 = 0;
    uint64_t last_peer = 0;
    int32_t last_type = 0;
    int32_t last_size = 0;
};

// The session-request callback with ShroudForge's shape: Run(void*) declared first, so MSVC puts it in slot 1; the
// flags at 8 and the id at 12, as in CCallbackBase.
ProbeLog g_sf_log;
class SfShapedCallback {
public:
    virtual void Run(void* parameter) {
        const auto* request = static_cast<const Identity*>(parameter);
        ++g_sf_log.run1;
        g_sf_log.last_type = request->type;
        g_sf_log.last_size = request->size;
        g_sf_log.last_peer = request->data.steam_id;
    }
    virtual void Run(void*, bool, uint64_t) { ++g_sf_log.run3; }
    virtual int32_t GetCallbackSizeBytes() { return static_cast<int32_t>(sizeof(Identity)); }
    uint8_t callback_flags{};
    int32_t callback_id{1251};
};

// A session-request callback that accepts, as ShroudForge's does (AcceptSessionWithUser from inside Run(void*)).
void* g_accepting_interface = nullptr;
int g_accepting_result = -1;
class AcceptingCallback {
public:
    virtual void Run(void* parameter) {
        g_accepting_result = api.accept(g_accepting_interface, static_cast<const Identity*>(parameter)) ? 1 : 0;
    }
    virtual void Run(void*, bool, uint64_t) {}
    virtual int32_t GetCallbackSizeBytes() { return static_cast<int32_t>(sizeof(Identity)); }
    uint8_t callback_flags{};
    int32_t callback_id{1251};
};

// A hand-built object with the Itanium order (Run(void*) in slot 0): what a non-MSVC build of the class would give.
ProbeLog g_itanium_log;
void itanium_run1(void*, void* parameter) {
    ++g_itanium_log.run1;
    g_itanium_log.last_peer = static_cast<const Identity*>(parameter)->data.steam_id;
}
void itanium_run3(void*, void*, bool, uint64_t) { ++g_itanium_log.run3; }
int32_t itanium_size(void*) { return 136; }
void* const g_itanium_vtable[3] = {reinterpret_cast<void*>(&itanium_run1), reinterpret_cast<void*>(&itanium_run3),
                                   reinterpret_cast<void*>(&itanium_size)};
struct alignas(8) RawCallback {
    void* const* vptr;
    uint8_t flags;
    uint8_t pad[3];
    int32_t id;
};
static_assert(sizeof(RawCallback) == 16);

uint8_t flags_at_8(const void* object) { return static_cast<const uint8_t*>(object)[8]; }
int32_t id_at_12(const void* object) {
    int32_t id;
    std::memcpy(&id, static_cast<const uint8_t*>(object) + 12, sizeof id);
    return id;
}

// No C++ objects with destructors in here, so __try works whatever the unwinding mode.
DWORD call_send_seh(void* self, const Identity* peer, int32_t* result) {
    __try {
        *result = api.send(self, peer, "x", 1, 8, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return GetExceptionCode();
    }
    return 0;
}

std::vector<uint8_t> pattern(size_t size, uint8_t seed) {
    std::vector<uint8_t> bytes(size);
    for (size_t i = 0; i < size; ++i) bytes[i] = static_cast<uint8_t>(seed + i * 31 + (i >> 9));
    return bytes;
}

// Waits up to timeout_ms for a channel to hold at least want messages; returns what it received.
std::vector<void*> receive_wait(void* iface, int32_t channel, int32_t want, DWORD timeout_ms, int32_t max = 16) {
    std::vector<void*> got;
    const ULONGLONG until = GetTickCount64() + timeout_ms;
    while (static_cast<int32_t>(got.size()) < want && GetTickCount64() < until) {
        void* batch[16];
        const int32_t n = api.receive(iface, channel, batch, (std::min)(max - static_cast<int32_t>(got.size()), 16));
        for (int32_t i = 0; i < n; ++i) got.push_back(batch[i]);
        if (n <= 0) Sleep(5);
    }
    return got;
}

constexpr uint64_t k_client_id = 76561198000000001ull;
constexpr uint64_t k_server_id = 90000000000000002ull;
constexpr uint64_t k_fabric_a = 76561198000000101ull;  // server role
constexpr uint64_t k_fabric_b = 76561198000000202ull;  // client role

int unit(uint32_t port_base) {
    g_who = "standin unit";
    // Before an identity: not ready, no handle, no interface; the calls are counted as before-ready.
    CHECK(api.user_handle() == 0, "user handle before identity");
    CHECK(api.user_messages() == nullptr, "accessor before identity");
    CHECK(read_knob(STANDIN_KNOB_CALLS_BEFORE_READY) == 1, "calls before ready %llu",
          (unsigned long long)read_knob(STANDIN_KNOB_CALLS_BEFORE_READY));
    CHECK(api.set_identity(0, port_base, 0) == 0, "zero id refused");
    CHECK(api.set_identity(k_client_id, 80, 0) == 0, "port base below 1024 refused");

    // Init delay: the handle stays 0 for exactly that many pumps.
    api.knob(STANDIN_KNOB_INIT_DELAY_PUMPS, 2);
    CHECK(api.set_identity(k_client_id, port_base, 0) == 1, "client identity binds");
    CHECK(api.user_handle() == 0, "delay: handle 0 before the first pump");
    api.pump();
    CHECK(api.user_handle() == 0, "delay: handle 0 after one pump");
    CHECK(api.find_user(1, "SteamNetworkingMessages002") == nullptr, "delay: no interface while not ready");
    api.pump();
    CHECK(api.user_handle() == 1, "delay: handle after two pumps");
    take_knob(STANDIN_KNOB_CALLS_BEFORE_READY);

    // Roles: a client has the user interfaces only.
    CHECK(api.server_handle() == 0, "client: no GameServer handle");
    CHECK(api.server_messages() == nullptr && api.server_sockets() == nullptr, "client: no GameServer accessors");
    void* messages = api.user_messages();
    void* sockets = api.user_sockets();
    CHECK(messages && sockets && messages != sockets, "client accessors");
    Identity me{};
    CHECK(api.get_identity(sockets, &me) && me.type == 16 && me.size == 8 && me.data.steam_id == k_client_id,
          "GetIdentity gives the client id");

    // Accessor null: the fallback through SteamInternal_FindOrCreateUserInterface finds the same objects.
    api.knob(STANDIN_KNOB_ACCESSOR_NULL, 1);
    CHECK(api.user_messages() == nullptr && api.user_sockets() == nullptr, "accessor-null knob");
    const int32_t handle = api.user_handle();
    CHECK(api.find_user(handle, "SteamNetworkingMessages002") == messages, "fallback finds the messages interface");
    CHECK(api.find_user(handle, "SteamNetworkingSockets013") == sockets, "fallback finds the sockets interface (013)");
    CHECK(api.find_user(handle, "SteamNetworkingSockets012") == sockets, "fallback: ShroudForge's 012 answered too");
    CHECK(!GetProcAddress(api.module, "SteamAPI_SteamNetworkingSockets_SteamAPI_v012") &&
              !GetProcAddress(api.module, "SteamAPI_SteamGameServerNetworkingSockets_SteamAPI_v012"),
          "no v012 sockets accessor, as in the game's steam_api64.dll");
    CHECK(api.find_user(handle, "SteamNetworkingMessages001") == nullptr, "fallback: wrong version");
    CHECK(api.find_user(0, "SteamNetworkingMessages002") == nullptr, "fallback: handle 0");
    CHECK(api.find_server(handle, "SteamNetworkingMessages002") == nullptr, "fallback: GameServer on a client");
    api.knob(STANDIN_KNOB_ACCESSOR_NULL, 0);
    CHECK(api.user_messages() == messages, "accessor back");

    // Send to self: sizes 0, 5 and the 512 KiB cap; one over the cap is refused and never arrives.
    const Identity self = steam(k_client_id);
    const auto big = pattern(524288, 7);
    take_knob(STANDIN_KNOB_SEND_FLAGS_SEEN);
    CHECK(api.send(messages, &self, nullptr, 0, 40, 4) == 1, "empty message");
    CHECK(api.send(messages, &self, "hello", 5, 32, 4) == 1, "unreliable message");
    CHECK(api.send(messages, &self, big.data(), 524288, 40, 4) == 1, "512 KiB message");
    const auto over = pattern(524289, 1);
    CHECK(api.send(messages, &self, over.data(), 524289, 40, 4) == 8, "524289 bytes: k_EResultInvalidParam");
    CHECK(api.send(messages, &self, "x", 1, 8, -1) == 8, "negative channel");
    const Identity zero = steam(0);
    CHECK(api.send(messages, &zero, "x", 1, 8, 4) == 8, "peer 0");
    CHECK(api.send(sockets, &self, "x", 1, 8, 4) == 8 && read_knob(STANDIN_KNOB_BAD_SELF) == 1,
          "a sockets pointer is not a messages interface");
    // The flags of each send that passed its parameter checks (40 and 32 here; the refused ones above do not count).
    CHECK(read_knob(STANDIN_KNOB_SEND_FLAGS_SEEN) == ((uint64_t{1} << 40) | (uint64_t{1} << 32)),
          "send flags seen %llx", (unsigned long long)read_knob(STANDIN_KNOB_SEND_FLAGS_SEEN));
    CHECK(take_knob(STANDIN_KNOB_SEND_FLAGS_SEEN) != 0 && read_knob(STANDIN_KNOB_SEND_FLAGS_SEEN) == 0,
          "writing 0 resets the send flags seen");
    auto got = receive_wait(messages, 4, 3, 5000);
    CHECK(got.size() == 3, "three messages arrive, got %zu", got.size());
    if (got.size() == 3) {
        const Message m0{static_cast<const uint8_t*>(got[0])}, m1{static_cast<const uint8_t*>(got[1])},
            m2{static_cast<const uint8_t*>(got[2])};
        // a received message keeps only the reliable bit (8) of the send flags, as Steam's m_nFlags does
        CHECK(m0.size() == 0 && m0.flags() == 8 && m0.channel() == 4, "empty message fields, flags %d", m0.flags());
        CHECK(m0.peer_type() == 16 && m0.peer_size() == 8 && m0.peer() == k_client_id, "peer identity @16");
        CHECK(m1.size() == 5 && !std::memcmp(m1.data(), "hello", 5) && m1.flags() == 0, "unreliable fields");
        CHECK(m2.size() == 524288 && !std::memcmp(m2.data(), big.data(), big.size()) && m2.flags() == 8,
              "512 KiB message intact");
        CHECK(read_knob(STANDIN_KNOB_LIVE_MESSAGES) == 3, "three live messages");
        // released once each: two through the export, one through the message's own release pointer (@184)
        api.release(got[0]);
        api.release(got[1]);
        m2.release()(got[2]);
        CHECK(read_knob(STANDIN_KNOB_RELEASES) == 3, "three releases, %llu",
              (unsigned long long)read_knob(STANDIN_KNOB_RELEASES));
        CHECK(read_knob(STANDIN_KNOB_LIVE_MESSAGES) == 0, "none live");
        CHECK(read_knob(STANDIN_KNOB_DOUBLE_RELEASES) == 0, "no double release yet");
        api.release(got[1]);
        CHECK(read_knob(STANDIN_KNOB_DOUBLE_RELEASES) == 1, "a second release is counted, not freed again");
        CHECK(read_knob(STANDIN_KNOB_RELEASES) == 3, "the second release is not a release");
        take_knob(STANDIN_KNOB_RELEASES);
        take_knob(STANDIN_KNOB_DOUBLE_RELEASES);
    }
    CHECK(api.receive(messages, 4, nullptr, 1) == 0, "receive with no array");

    // EResult injection and drop: neither message arrives; the sentinel after them does, alone.
    api.knob(STANDIN_KNOB_SEND_ERESULT, 3);
    CHECK(api.send(messages, &self, "injected", 8, 8, 5) == 3, "injected EResult");
    api.knob(STANDIN_KNOB_SEND_ERESULT, 0);
    api.knob(STANDIN_KNOB_DROP_SENDS, 1);
    CHECK(api.send(messages, &self, "dropped", 7, 8, 5) == 1, "a dropped send answers OK");
    CHECK(read_knob(STANDIN_KNOB_DROP_SENDS) == 0, "the drop is used up");
    CHECK(api.send(messages, &self, "sentinel", 8, 8, 5) == 1, "sentinel");
    got = receive_wait(messages, 5, 1, 5000);
    Sleep(100);
    void* extra[4];
    const int32_t more = api.receive(messages, 5, extra, 4);
    CHECK(got.size() == 1 && more == 0, "only the sentinel arrives (%zu + %d)", got.size(), more);
    if (got.size() == 1) {
        const Message m{static_cast<const uint8_t*>(got[0])};
        CHECK(m.size() == 8 && !std::memcmp(m.data(), "sentinel", 8), "sentinel payload");
        api.release(got[0]);
    }
    for (int32_t i = 0; i < more; ++i) api.release(extra[i]);

    // A faulting export raises an access violation the caller can catch; the knob off, it works again.
    api.knob(STANDIN_KNOB_FAULT_EXPORT, STANDIN_EXPORT_SEND);
    int32_t result = -1;
    CHECK(call_send_seh(messages, &self, &result) == EXCEPTION_ACCESS_VIOLATION, "faulting send");
    api.knob(STANDIN_KNOB_FAULT_EXPORT, 0);
    CHECK(call_send_seh(messages, &self, &result) == 0 && result == 1, "send after the fault knob");
    got = receive_wait(messages, 0, 1, 5000);
    for (void* m : got) api.release(m);

    // The pump: Run(void*) through slot 1 of a clang-cl object, flags @8 and id @12 as CCallbackBase.
    SfShapedCallback sf;
    CHECK(flags_at_8(&sf) == 0 && id_at_12(&sf) == 1251, "SF-shaped class: flags @8, id @12");
    api.register_callback(&sf, 1251);
    CHECK(flags_at_8(&sf) == 0x01 && id_at_12(&sf) == 1251, "RegisterCallback sets the registered flag");
    RawCallback itanium{g_itanium_vtable, 0, {}, 1251};
    api.register_callback(&itanium, 1251);
    SfShapedCallback* server_flagged = new SfShapedCallback;  // flag 0x02: a GameServer callback, not run on a client
    server_flagged->callback_flags = 0x02;
    ProbeLog before = g_sf_log;
    api.register_callback(server_flagged, 1251);
    CHECK(read_knob(STANDIN_KNOB_REGISTERED) == 3, "three callbacks registered");
    constexpr uint64_t k_peer = 76561198000009999ull;
    api.knob(STANDIN_KNOB_INJECT_REQUEST, k_peer);
    api.knob(STANDIN_KNOB_INJECT_REQUEST, k_peer);  // one request per peer until accepted
    CHECK(read_knob(STANDIN_KNOB_SESSION_REQUESTS) == 1, "one session request");
    const int32_t runs = api.pump();
    CHECK(runs == 2, "two Run calls (the clang-cl object and the Itanium one), %d", runs);
    CHECK(g_sf_log.run1 == before.run1 + 1 && g_sf_log.run3 == 0, "slot 1 reaches Run(void*) of the clang-cl object");
    CHECK(g_sf_log.last_peer == k_peer && g_sf_log.last_type == 16 && g_sf_log.last_size == 8, "request identity");
    CHECK(g_itanium_log.run3 == 1 && g_itanium_log.run1 == 0,
          "slot 1 of an Itanium-ordered object is the three-argument Run: the stand-in is MSVC-only");
    CHECK(api.pump() == 0, "a request runs once");
    CHECK(api.accept(messages, &self) == true, "accept: an active session (self) is true");
    const Identity requested = steam(k_peer), unknown = steam(k_peer + 1);
    CHECK(api.accept(messages, &unknown) == false, "accept: no session");
    CHECK(api.accept(messages, &requested) == true, "accept: the pending request");
    api.knob(STANDIN_KNOB_INJECT_REQUEST, k_peer);
    CHECK(read_knob(STANDIN_KNOB_SESSION_REQUESTS) == 1, "no request from an accepted peer");
    api.unregister_callback(&sf);
    CHECK(flags_at_8(&sf) == 0 && read_knob(STANDIN_KNOB_REGISTERED) == 2, "UnregisterCallback clears the flag");
    api.knob(STANDIN_KNOB_INJECT_REQUEST, k_peer + 2);
    api.pump();
    CHECK(g_sf_log.run1 == before.run1 + 1, "an unregistered callback is not run");
    api.unregister_callback(&itanium);

    // A request that is never accepted times out in Steam (STANDIN_KNOB_EXPIRE_SESSION stands in for the timeout);
    // the peer's next message then raises a new request.
    take_knob(STANDIN_KNOB_SESSION_REQUESTS);
    api.knob(STANDIN_KNOB_INJECT_REQUEST, k_peer + 4);
    api.knob(STANDIN_KNOB_INJECT_REQUEST, k_peer + 4);
    CHECK(read_knob(STANDIN_KNOB_SESSION_REQUESTS) == 1, "one request while it is pending");
    CHECK(api.knob(STANDIN_KNOB_EXPIRE_SESSION, k_peer + 4) == 1, "the pending session expires");
    CHECK(api.knob(STANDIN_KNOB_EXPIRE_SESSION, k_peer + 4) == 0, "nothing left to expire");
    CHECK(api.knob(STANDIN_KNOB_EXPIRE_SESSION, k_peer) == 0, "an accepted session does not expire");
    api.knob(STANDIN_KNOB_INJECT_REQUEST, k_peer + 4);
    CHECK(read_knob(STANDIN_KNOB_SESSION_REQUESTS) == 2, "after the timeout the next message raises a new request");
    api.knob(STANDIN_KNOB_EXPIRE_SESSION, k_peer + 4);
    // ShroudForge's Run(void*) calls AcceptSessionWithUser on the pump thread: the pump holds no lock while it runs.
    AcceptingCallback accepting;
    g_accepting_interface = messages;
    api.register_callback(&accepting, 1251);
    CHECK(api.pump() == 0, "an expired request is not dispatched");
    api.knob(STANDIN_KNOB_INJECT_REQUEST, k_peer + 5);
    CHECK(api.pump() == 1 && g_accepting_result == 1, "Run(void*) accepts from inside the pump");
    CHECK(api.knob(STANDIN_KNOB_EXPIRE_SESSION, k_peer + 5) == 0, "the session accepted in Run is active");
    api.unregister_callback(&accepting);

    // Server role: GameServer handle and interfaces only; GameServer-flagged callbacks only.
    CHECK(api.set_identity(k_server_id, port_base, STANDIN_ROLE_SERVER) == 1, "server identity binds");
    CHECK(api.user_handle() == 0 && api.server_handle() == 2, "server handles");
    CHECK(api.user_messages() == nullptr && api.user_sockets() == nullptr, "server: no user accessors");
    void* server_messages = api.server_messages();
    void* server_sockets = api.server_sockets();
    CHECK(server_messages && server_sockets && server_messages != messages, "server accessors");
    CHECK(api.find_server(2, "SteamNetworkingMessages002") == server_messages, "server fallback");
    CHECK(api.find_user(1, "SteamNetworkingMessages002") == nullptr, "no user fallback on a server");
    CHECK(api.get_identity(server_sockets, &me) && me.data.steam_id == k_server_id, "server GetIdentity");
    take_knob(STANDIN_KNOB_BAD_SELF);
    CHECK(api.receive(messages, 0, extra, 1) == 0 && read_knob(STANDIN_KNOB_BAD_SELF) == 1,
          "a user interface pointer is refused on a server");
    SfShapedCallback client_flagged;
    api.register_callback(&client_flagged, 1251);
    // steam_api64 reads the GameServer flag at every dispatch: 0x02 set after RegisterCallback counts from the next pump
    SfShapedCallback flagged_late;
    api.register_callback(&flagged_late, 1251);
    flagged_late.callback_flags |= 0x02;
    const int sf_before = g_sf_log.run1;
    api.knob(STANDIN_KNOB_INJECT_REQUEST, k_peer + 3);
    const int32_t server_runs = api.pump();
    CHECK(server_runs == 2, "two Runs on a server: flagged before RegisterCallback and flagged after it, %d", server_runs);
    CHECK(g_sf_log.run1 == sf_before + 2 && g_sf_log.last_peer == k_peer + 3, "both GameServer callbacks ran");
    CHECK(flags_at_8(server_flagged) == 0x03, "GameServer flag kept beside the registered flag");
    // ...and a flag cleared after RegisterCallback stops it on a server from the next pump
    server_flagged->callback_flags &= static_cast<uint8_t>(~0x02);
    api.knob(STANDIN_KNOB_INJECT_REQUEST, k_peer + 6);
    const int32_t cleared_runs = api.pump();
    CHECK(cleared_runs == 1 && g_sf_log.run1 == sf_before + 3, "only the still-flagged callback runs after the clear, %d",
          cleared_runs);
    api.unregister_callback(&client_flagged);
    api.unregister_callback(&flagged_late);
    api.unregister_callback(server_flagged);
    delete server_flagged;
    CHECK(read_knob(STANDIN_KNOB_LIVE_MESSAGES) == 0, "no message left live");

    std::printf("%s: %s %d checks\n", g_who, g_failures ? "FAIL" : "PASS", g_checks);
    return g_failures ? 1 : 0;
}

bool touch(const std::wstring& path) {
    const HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);
    return true;
}

bool wait_file(const std::wstring& path, DWORD timeout_ms) {
    const ULONGLONG until = GetTickCount64() + timeout_ms;
    while (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        if (GetTickCount64() >= until) return false;
        Sleep(20);
    }
    return true;
}

int fabric_a(uint32_t port_base, const std::wstring& sync) {
    g_who = "standin fabric A";
    CHECK(api.set_identity(k_fabric_a, port_base, STANDIN_ROLE_SERVER) == 1, "A binds");
    SfShapedCallback callback;
    callback.callback_flags = 0x02;  // a GameServer callback, as ShroudForge sets it on a dedicated server
    api.register_callback(&callback, 1251);
    CHECK(touch(sync + L"\\a.ready"), "a.ready");
    CHECK(wait_file(sync + L"\\b.ready", 20000), "B never got ready");
    void* messages = api.server_messages();
    CHECK(messages != nullptr, "A's GameServer messages interface");

    // B's first message raises one session request on A's pump.
    const ULONGLONG until = GetTickCount64() + 15000;
    while (g_sf_log.run1 == 0 && GetTickCount64() < until) {
        api.pump();
        Sleep(10);
    }
    CHECK(g_sf_log.run1 == 1 && g_sf_log.last_peer == k_fabric_b && g_sf_log.last_type == 16,
          "A's callback got B's request (runs %d, peer %llu)", g_sf_log.run1, (unsigned long long)g_sf_log.last_peer);
    // B sends three: all held, none readable while A has not accepted.
    const ULONGLONG held_until = GetTickCount64() + 15000;
    while (read_knob(STANDIN_KNOB_PENDING_MESSAGES) < 3 && GetTickCount64() < held_until) Sleep(10);
    Sleep(200);
    CHECK(read_knob(STANDIN_KNOB_PENDING_MESSAGES) == 3, "three held for B, %llu",
          (unsigned long long)read_knob(STANDIN_KNOB_PENDING_MESSAGES));
    void* out[8];
    CHECK(api.receive(messages, 5, out, 8) == 0 && api.receive(messages, 6, out, 8) == 0,
          "nothing readable before accept");
    api.pump();
    CHECK(g_sf_log.run1 == 1 && read_knob(STANDIN_KNOB_SESSION_REQUESTS) == 1, "one request for many messages");
    // Unaccepted, the session times out (the knob): its held messages are gone, and B's next message raises a new
    // request, as Steam posts the request again while a peer keeps sending.
    CHECK(api.knob(STANDIN_KNOB_EXPIRE_SESSION, k_fabric_b) == 1, "B's unaccepted session expires");
    CHECK(read_knob(STANDIN_KNOB_PENDING_MESSAGES) == 0, "its held messages are dropped");
    CHECK(touch(sync + L"\\a.expired"), "a.expired");
    const ULONGLONG again_until = GetTickCount64() + 15000;
    while (g_sf_log.run1 < 2 && GetTickCount64() < again_until) {
        api.pump();
        Sleep(10);
    }
    CHECK(g_sf_log.run1 == 2 && g_sf_log.last_peer == k_fabric_b && read_knob(STANDIN_KNOB_SESSION_REQUESTS) == 2,
          "B's next message raises a new request (runs %d)", g_sf_log.run1);
    const ULONGLONG held_again = GetTickCount64() + 15000;
    while (read_knob(STANDIN_KNOB_PENDING_MESSAGES) < 3 && GetTickCount64() < held_again) Sleep(10);
    Sleep(200);
    CHECK(read_knob(STANDIN_KNOB_PENDING_MESSAGES) == 3, "three held for B again (the dropped one never came), %llu",
          (unsigned long long)read_knob(STANDIN_KNOB_PENDING_MESSAGES));
    const Identity b = steam(k_fabric_b);
    CHECK(api.accept(messages, &b), "A accepts B");
    CHECK(read_knob(STANDIN_KNOB_PENDING_MESSAGES) == 0, "nothing held after accept");
    auto ch5 = receive_wait(messages, 5, 2, 2000, 8);
    Sleep(100);
    const int32_t extra5 = api.receive(messages, 5, out, 8);
    CHECK(ch5.size() == 2 && extra5 == 0, "channel 5: exactly two (the dropped one never came), %zu + %d",
          ch5.size(), extra5);
    if (ch5.size() == 2) {
        const Message m0{static_cast<const uint8_t*>(ch5[0])}, m1{static_cast<const uint8_t*>(ch5[1])};
        CHECK(m0.size() == 5 && !std::memcmp(m0.data(), "b2a-1", 5) && m0.flags() == 8 && m0.peer() == k_fabric_b,
              "first: reliable b2a-1 from B");
        CHECK(m1.size() == 5 && !std::memcmp(m1.data(), "b2a-2", 5) && m1.flags() == 0 && m1.channel() == 5,
              "second: unreliable b2a-2");
    }
    auto ch6 = receive_wait(messages, 6, 1, 2000, 8);
    CHECK(ch6.size() == 1, "channel 6: the 512 KiB message");
    if (ch6.size() == 1) {
        const Message m{static_cast<const uint8_t*>(ch6[0])};
        const auto big = pattern(524288, 42);
        CHECK(m.size() == 524288 && !std::memcmp(m.data(), big.data(), big.size()) && m.flags() == 8,
              "512 KiB message intact across the fabric");
    }
    for (void* m : ch5) api.release(m);
    for (void* m : ch6) api.release(m);
    for (int32_t i = 0; i < extra5; ++i) api.release(out[i]);
    CHECK(read_knob(STANDIN_KNOB_RELEASES) == ch5.size() + ch6.size() + static_cast<size_t>(extra5) &&
              read_knob(STANDIN_KNOB_LIVE_MESSAGES) == 0 && read_knob(STANDIN_KNOB_DOUBLE_RELEASES) == 0,
          "each released once");

    // The other direction.
    CHECK(api.send(messages, &b, "a2b-1", 5, 40, 9) == 1, "A replies to B");
    CHECK(wait_file(sync + L"\\b.done", 20000), "B never finished");
    api.unregister_callback(&callback);
    std::printf("%s: %s %d checks\n", g_who, g_failures ? "FAIL" : "PASS", g_checks);
    return g_failures ? 1 : 0;
}

int fabric_b(uint32_t port_base, const std::wstring& sync) {
    g_who = "standin fabric B";
    CHECK(api.set_identity(k_fabric_b, port_base, 0) == 1, "B binds");
    SfShapedCallback probe;  // a user callback (no GameServer flag): must not run, B's send opened the session
    api.register_callback(&probe, 1251);
    CHECK(wait_file(sync + L"\\a.ready", 20000), "A never got ready");
    CHECK(touch(sync + L"\\b.ready"), "b.ready");
    void* messages = api.user_messages();
    CHECK(messages != nullptr, "B's messages interface");
    const Identity a = steam(k_fabric_a);
    CHECK(api.send(messages, &a, "b2a-1", 5, 40, 5) == 1, "b2a-1");
    CHECK(api.send(messages, &a, "b2a-2", 5, 32, 5) == 1, "b2a-2");
    const auto big = pattern(524288, 42);
    CHECK(api.send(messages, &a, big.data(), 524288, 40, 6) == 1, "512 KiB");
    const auto over = pattern(524289, 42);
    CHECK(api.send(messages, &a, over.data(), 524289, 40, 6) == 8, "over the cap: k_EResultInvalidParam");
    // A lets the session time out unaccepted; B sends the same again
    CHECK(wait_file(sync + L"\\a.expired", 30000), "A never expired the session");
    api.knob(STANDIN_KNOB_DROP_SENDS, 1);
    CHECK(api.send(messages, &a, "dropped", 7, 40, 5) == 1, "dropped send");
    CHECK(api.send(messages, &a, "b2a-1", 5, 40, 5) == 1, "b2a-1 again");
    CHECK(api.send(messages, &a, "b2a-2", 5, 32, 5) == 1, "b2a-2 again");
    CHECK(api.send(messages, &a, big.data(), 524288, 40, 6) == 1, "512 KiB again");

    std::vector<void*> got;
    const ULONGLONG until = GetTickCount64() + 20000;
    while (got.empty() && GetTickCount64() < until) {
        api.pump();
        got = receive_wait(messages, 9, 1, 20, 4);
    }
    CHECK(got.size() == 1, "B receives A's reply");
    if (got.size() == 1) {
        const Message m{static_cast<const uint8_t*>(got[0])};
        CHECK(m.size() == 5 && !std::memcmp(m.data(), "a2b-1", 5) && m.peer() == k_fabric_a && m.flags() == 8,
              "a2b-1 from A, reliable");
        api.release(got[0]);
    }
    api.pump();
    CHECK(g_sf_log.run1 == 0 && g_sf_log.run3 == 0 && read_knob(STANDIN_KNOB_SESSION_REQUESTS) == 0,
          "no session request on B: its own send opened the session");
    CHECK(read_knob(STANDIN_KNOB_RELEASES) == 1 && read_knob(STANDIN_KNOB_LIVE_MESSAGES) == 0, "released once");
    api.unregister_callback(&probe);
    touch(sync + L"\\b.done");
    std::printf("%s: %s %d checks\n", g_who, g_failures ? "FAIL" : "PASS", g_checks);
    return g_failures ? 1 : 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (!load()) return 2;
    if (argc == 3 && !std::wcscmp(argv[1], L"unit")) return unit(static_cast<uint32_t>(_wtoi(argv[2])));
    if (argc == 5 && !std::wcscmp(argv[1], L"fabric")) {
        const auto base = static_cast<uint32_t>(_wtoi(argv[3]));
        if (!std::wcscmp(argv[2], L"a")) return fabric_a(base, argv[4]);
        if (!std::wcscmp(argv[2], L"b")) return fabric_b(base, argv[4]);
    }
    std::printf("usage: steam-standin-test unit <port base> | fabric a|b <port base> <sync dir>\n");
    return 2;
}
