// SPDX-License-Identifier: MIT
// kiln.net's Steam peer-to-peer messages (steam_messages.h, kiln_net.h's Steam members,
// docs/kiln/services/net-steam-messages.md).
//
// Interface resolution, the identity and message layouts and the session-request callback class are adapted from
// ShroudForge's modules/transport/native/steam_network.cpp (MIT, Copyright (c) 2026 bonsaibauer).
//
// Steam:
//  - Exports are looked up once, at init, in the steam_api64.dll both game executables import (so it is loaded before
//    Kiln, or not at all). Members never call GetModuleHandle or GetProcAddress (both can wait for the loader lock).
//  - Per call: the role's user handle first (SteamAPI_GetHSteamUser, SteamGameServer_GetHSteamUser); while it is 0 no
//    other Steam function is called. Then the interface through the flat accessor (messages v002, sockets v013: the
//    game's DLL has no v012 sockets accessor and its flat GetIdentity is built for the 013 vtable), else through
//    SteamInternal_FindOrCreate*Interface with the same version. Interfaces are asked again on every call and a null
//    answer is never kept: the flat accessors themselves keep a null first answer for the session.
//  - Every Steam call is SEH-guarded; a fault answers KILN_ERR_INTERNAL.
// The session callback (SteamNetworkingMessagesSessionRequest_t, 1251):
//  - registered once per process, by the first maintain whose policy is not NONE, with the GameServer flag 0x02 set
//    before the call on a server (steam_api64 compares it with the pump's role at every dispatch), and never
//    unregistered: NONE, close and exit are answered inside the callback (no answer: Steam times the request out, as
//    if nothing were registered). RegisterCallback changes Steam's callback map without a lock while the game's pump
//    may walk it; registering once keeps that window to one call per process (hosted ShroudForge registers from its
//    mods thread the same way). A NONE policy is in force at once, also while Steam is not ready (it calls no Steam
//    function), as ShroudForge's unregistering is; a policy that answers waits for Steam.
//  - Run(void*) runs on the game's Steam pump thread: it checks the identity, reads the policy through a lock-free
//    seqlock (at most 5 reads, then the request is denied), then calls AcceptSessionWithUser. It allocates nothing,
//    logs nothing and takes no lock. The object is static and never freed.
// Exit: close_for_exit sets the flag every member and the callback check after counting themselves in, then waits for
// the ones already past the check; nothing calls Steam afterwards.
#include "steam_messages.h"

#ifdef _WIN32
#include <windows.h>
#endif

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

#if defined(_WIN32) && !defined(_MSC_VER)
#error "the session callback object needs the MSVC vtable order (Run(void*) in slot 1): build kiln-net with clang-cl"
#endif

#ifdef _WIN32
#define KNET_STEAM_CALL __cdecl
// No C++ object with a destructor lives in a guarded function, as __try requires.
#define KNET_GUARD_BEGIN __try {
#define KNET_GUARD_END                       \
    }                                        \
    __except (EXCEPTION_EXECUTE_HANDLER) {   \
        return false;                        \
    }                                        \
    return true;
#else  // host tests: a fake export table, which never faults
#define KNET_STEAM_CALL
#define KNET_GUARD_BEGIN {
#define KNET_GUARD_END \
    }                  \
    return true;
#endif

namespace knet::steam {
namespace {

constexpr int32_t kIdentitySteamId = 16;           // k_ESteamNetworkingIdentityType_SteamID
constexpr int32_t kIdentitySteamIdSize = 8;
constexpr int32_t kSessionRequestCallback = 1251;  // SteamNetworkingMessagesSessionRequest_t
constexpr uint8_t kCallbackFlagGameServer = 0x02;  // k_ECallbackFlagsGameServer
constexpr uint32_t kMaxIds = 256;
constexpr int kPolicyReads = 5;                    // the first read and at most 4 retries; then the request is denied
constexpr size_t kMaxLeases = 16;
constexpr uint32_t kMaxChannel = 65535;
constexpr KilnHandle kHandleTag = 0x5354ull << 48;
constexpr char kMessagesVersion[] = "SteamNetworkingMessages002";
constexpr char kSocketsVersion[] = "SteamNetworkingSockets013";

struct Identity {  // SteamNetworkingIdentity
    int32_t type;
    int32_t size;
    union {
        uint64_t steam_id;
        uint32_t reserved[32];
    } data;
};
static_assert(sizeof(Identity) == 136 && offsetof(Identity, data) == 8);

struct MessagePrefix {  // the start of SteamNetworkingMessage_t
    void* data;
    int32_t size;
    uint32_t connection;
    Identity peer;
    int64_t connection_user_data;
    int64_t received_at;
    int64_t message_number;
    void* free_data;
    void* release;
    int32_t channel;
    int32_t flags;
};
static_assert(offsetof(MessagePrefix, peer) == 16 && offsetof(MessagePrefix, release) == 184 &&
              offsetof(MessagePrefix, channel) == 192 && offsetof(MessagePrefix, flags) == 196);

using HandleFn = int32_t(KNET_STEAM_CALL*)();
using AccessorFn = void*(KNET_STEAM_CALL*)();
using FindFn = void*(KNET_STEAM_CALL*)(int32_t, const char*);
using SendFn = int32_t(KNET_STEAM_CALL*)(void*, const Identity*, const void*, uint32_t, int32_t, int32_t);
using ReceiveFn = int32_t(KNET_STEAM_CALL*)(void*, int32_t, MessagePrefix**, int32_t);
using AcceptFn = bool(KNET_STEAM_CALL*)(void*, const Identity*);
using ReleaseFn = void(KNET_STEAM_CALL*)(MessagePrefix*);
using IdentityFn = bool(KNET_STEAM_CALL*)(void*, Identity*);
using RegisterFn = void(KNET_STEAM_CALL*)(void*, int32_t);

// Written by init before the table is published, read-only afterwards.
struct Exports {
    HandleFn handle = nullptr;
    AccessorFn messages = nullptr;
    AccessorFn sockets = nullptr;
    FindFn find = nullptr;
    SendFn send = nullptr;
    ReceiveFn receive = nullptr;
    AcceptFn accept = nullptr;
    ReleaseFn release = nullptr;
    IdentityFn identity = nullptr;
    RegisterFn reg = nullptr;
};
Exports g_exports;
Config g_config;
bool g_complete = false;  // every export the lease needs is there

struct RoleNames {
    const char* handle;
    const char* messages;
    const char* sockets;
    const char* find;
};
constexpr RoleNames kUserNames{"SteamAPI_GetHSteamUser", "SteamAPI_SteamNetworkingMessages_SteamAPI_v002",
                               "SteamAPI_SteamNetworkingSockets_SteamAPI_v013", "SteamInternal_FindOrCreateUserInterface"};
constexpr RoleNames kServerNames{"SteamGameServer_GetHSteamUser",
                                 "SteamAPI_SteamGameServerNetworkingMessages_SteamAPI_v002",
                                 "SteamAPI_SteamGameServerNetworkingSockets_SteamAPI_v013",
                                 "SteamInternal_FindOrCreateGameServerInterface"};

// ---- the exit gate ----
std::atomic<bool> g_closed{false};
std::atomic<uint32_t> g_inflight{0};
struct Entered {  // counts a member call (or the callback) in, then checks the exit flag (seq_cst on both sides)
    bool open;
    Entered() {
        g_inflight.fetch_add(1, std::memory_order_seq_cst);
        open = !g_closed.load(std::memory_order_seq_cst);
    }
    ~Entered() { g_inflight.fetch_sub(1, std::memory_order_seq_cst); }
    Entered(const Entered&) = delete;
    Entered& operator=(const Entered&) = delete;
};

// ---- counters (the callback updates them: atomics only) ----
std::atomic<uint64_t> g_sessions_accepted{0}, g_sessions_refused{0}, g_callback_faults{0};
std::atomic<bool> g_ready_seen{false};
std::atomic<uint32_t> g_fault_logged{0};  // one log line per kind of fault

// ---- the accept policy: one writer at a time (under g_mutex), any number of lock-free readers ----
// The writer stores the count before the ids and never clears ids past the count, so only the sequence check keeps a
// reader from mixing two policies (an odd sequence: a write is in progress).
struct Policy {
    std::atomic<uint32_t> seq{0};
    std::atomic<uint32_t> mode{KILN_STEAM_ACCEPT_NONE};
    std::atomic<uint32_t> count{0};
    std::atomic<uint64_t> ids[kMaxIds]{};
};
Policy g_policy;

// ---- leases, policy writers, registration bookkeeping. Never taken by the session callback. ----
struct Lease {
    KilnHandle handle = 0;
    KilnOwner owner = 0;
    uint32_t first = 0, last = 0;
    bool sessions = false;
};
std::mutex g_mutex;
Lease g_leases[kMaxLeases];
uint64_t g_generation = 0;
std::atomic<bool> g_registered{false};        // set before RegisterCallback is called, never cleared
std::atomic<bool> g_register_faulted{false};
std::atomic<bool> g_owner_refusal_logged{false};

void log_text(int level, const char* text) {
    if (g_config.log) g_config.log(level, text);
}

// ---- guarded Steam calls ----
bool call_handle(HandleFn f, int32_t* out) {
    KNET_GUARD_BEGIN
    *out = f();
    KNET_GUARD_END
}
bool call_accessor(AccessorFn f, void** out) {
    KNET_GUARD_BEGIN
    *out = f();
    KNET_GUARD_END
}
bool call_find(FindFn f, int32_t handle, const char* version, void** out) {
    KNET_GUARD_BEGIN
    *out = f(handle, version);
    KNET_GUARD_END
}
bool call_send(SendFn f, void* self, const Identity* peer, const void* data, uint32_t len, int32_t flags,
               int32_t channel, int32_t* out) {
    KNET_GUARD_BEGIN
    *out = f(self, peer, data, len, flags, channel);
    KNET_GUARD_END
}
bool call_receive(ReceiveFn f, void* self, int32_t channel, MessagePrefix** message, int32_t* out) {
    KNET_GUARD_BEGIN
    *out = f(self, channel, message, 1);
    KNET_GUARD_END
}
bool call_accept(AcceptFn f, void* self, const Identity* peer, bool* out) {
    KNET_GUARD_BEGIN
    *out = f(self, peer);
    KNET_GUARD_END
}
bool call_release(ReleaseFn f, MessagePrefix* message) {
    KNET_GUARD_BEGIN
    f(message);
    KNET_GUARD_END
}
bool call_identity(IdentityFn f, void* self, Identity* out, bool* ok) {
    KNET_GUARD_BEGIN
    *ok = f(self, out);
    KNET_GUARD_END
}
bool call_register(RegisterFn f, void* object, int32_t id) {
    KNET_GUARD_BEGIN
    f(object, id);
    KNET_GUARD_END
}
// Reads a received message (Steam's memory) and copies its payload: *size -1 = unusable (too big, no data, no Steam id).
bool read_message(const MessagePrefix* m, void* buffer, uint32_t cap, int32_t* size, uint64_t* peer, int32_t* flags) {
    KNET_GUARD_BEGIN
    const int32_t n = m->size;
    if (n < 0 || static_cast<uint32_t>(n) > cap || (n && !m->data) || m->peer.type != kIdentitySteamId ||
        m->peer.size != kIdentitySteamIdSize || !m->peer.data.steam_id) {
        *size = -1;
    } else {
        if (n) std::memcpy(buffer, m->data, static_cast<size_t>(n));
        *size = n;
        *peer = m->peer.data.steam_id;
        *flags = m->flags;
    }
    KNET_GUARD_END
}

enum FaultKind : uint32_t {
    kFaultHandle = 1,
    kFaultAccessor = 2,
    kFaultFind = 4,
    kFaultSend = 8,
    kFaultReceive = 16,
    kFaultAccept = 32,
    kFaultRelease = 64,
    kFaultIdentity = 128,
    kFaultRegister = 256
};
// A member's fault: one log line per kind. The callback passes quiet (it never logs).
void note_fault(FaultKind kind, const char* what, bool quiet) {
    if (quiet) {
        g_callback_faults.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (g_fault_logged.fetch_or(kind, std::memory_order_relaxed) & kind) return;
    char line[200];
    std::snprintf(line, sizeof line, "net: steam messages: Steam's %s faulted; the call answers internal (logged once)",
                  what);
    log_text(KILN_LOG_WARN, line);
}

Identity steam_identity(uint64_t id) {
    Identity r{};
    r.type = kIdentitySteamId;
    r.size = kIdentitySteamIdSize;
    r.data.steam_id = id;
    return r;
}

enum class Steam { Ready, NotReady, Fault };
struct Resolved {
    Steam state = Steam::NotReady;
    void* messages = nullptr;
    int32_t handle = 0;
};

// The role's user handle, then the messages interface (accessor, else FindOrCreate). Never cached.
Resolved resolve_messages(bool quiet) {
    Resolved r;
    if (!g_complete) return r;
    if (!call_handle(g_exports.handle, &r.handle)) {
        note_fault(kFaultHandle, "user handle", quiet);
        r.state = Steam::Fault;
        return r;
    }
    if (!r.handle) return r;  // Steam is not initialised (yet): no other export is called
    void* iface = nullptr;
    if (g_exports.messages && !call_accessor(g_exports.messages, &iface)) {
        note_fault(kFaultAccessor, "messages accessor", quiet);
        r.state = Steam::Fault;
        return r;
    }
    if (!iface && g_exports.find && !call_find(g_exports.find, r.handle, kMessagesVersion, &iface)) {
        note_fault(kFaultFind, "FindOrCreate interface lookup", quiet);
        r.state = Steam::Fault;
        return r;
    }
    if (!iface) return r;
    r.messages = iface;
    r.state = Steam::Ready;
    g_ready_seen.store(true, std::memory_order_relaxed);
    return r;
}

// This process's Steam id through the sockets interface (v013 accessor, else FindOrCreate "...013"). False: none.
bool local_identity(int32_t handle, uint64_t* id, bool* fault) {
    if (!g_exports.identity) return false;
    void* sockets = nullptr;
    if (g_exports.sockets && !call_accessor(g_exports.sockets, &sockets)) {
        note_fault(kFaultAccessor, "sockets accessor", false);
        *fault = true;
        return false;
    }
    if (!sockets && g_exports.find && !call_find(g_exports.find, handle, kSocketsVersion, &sockets)) {
        note_fault(kFaultFind, "FindOrCreate interface lookup", false);
        *fault = true;
        return false;
    }
    if (!sockets) return false;
    Identity me{};
    bool ok = false;
    if (!call_identity(g_exports.identity, sockets, &me, &ok)) {
        note_fault(kFaultIdentity, "GetIdentity", false);
        *fault = true;
        return false;
    }
    if (!ok || me.type != kIdentitySteamId || me.size != kIdentitySteamIdSize || !me.data.steam_id) return false;
    *id = me.data.steam_id;
    return true;
}

// ---- policy ----
void write_policy_locked(uint32_t mode, const uint64_t* ids, uint32_t count) {
    const uint32_t s = g_policy.seq.load(std::memory_order_relaxed);
    g_policy.seq.store(s + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    g_policy.mode.store(mode, std::memory_order_relaxed);
    g_policy.count.store(count, std::memory_order_relaxed);
    for (uint32_t i = 0; i < count; ++i) g_policy.ids[i].store(ids[i], std::memory_order_relaxed);
    g_policy.seq.store(s + 2, std::memory_order_release);
}

// The session callback's read: a consistent snapshot decides; after kPolicyReads torn or busy reads, deny.
bool policy_allows(uint64_t peer) {
    for (int attempt = 0; attempt < kPolicyReads; ++attempt) {
        const uint32_t before = g_policy.seq.load(std::memory_order_acquire);
        if (before & 1u) continue;
        const uint32_t mode = g_policy.mode.load(std::memory_order_relaxed);
        uint32_t count = g_policy.count.load(std::memory_order_relaxed);
        if (count > kMaxIds) count = kMaxIds;
        bool hit = mode == KILN_STEAM_ACCEPT_ANY;
        if (mode == KILN_STEAM_ACCEPT_LISTED)
            for (uint32_t i = 0; i < count && !hit; ++i) hit = g_policy.ids[i].load(std::memory_order_relaxed) == peer;
        std::atomic_thread_fence(std::memory_order_acquire);
        if (g_policy.seq.load(std::memory_order_relaxed) == before) return hit;
    }
    return false;
}

// ---- the session callback (CCallbackBase's shape: vptr @0, flags @8, id @12; Run(void*) in MSVC's slot 1) ----
void on_session_request(const void* parameter) {
    Entered entered;
    bool accepted = false;
    if (entered.open && parameter) {
        const auto* request = static_cast<const Identity*>(parameter);
        if (request->type == kIdentitySteamId && request->size == kIdentitySteamIdSize && request->data.steam_id &&
            policy_allows(request->data.steam_id)) {
            const Resolved r = resolve_messages(true);
            bool ok = false;
            if (r.state == Steam::Ready) {
                if (call_accept(g_exports.accept, r.messages, request, &ok))
                    accepted = ok;
                else
                    g_callback_faults.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
    (accepted ? g_sessions_accepted : g_sessions_refused).fetch_add(1, std::memory_order_relaxed);
}

class SessionRequestCallback {
public:
    virtual void Run(void* parameter) { on_session_request(parameter); }
    virtual void Run(void*, bool, uint64_t) {}
    virtual int32_t GetCallbackSizeBytes() { return static_cast<int32_t>(sizeof(Identity)); }
    uint8_t callback_flags = 0;                  // Steam's: 0x01 registered (Steam sets it), 0x02 GameServer (ours)
    int32_t callback_id = kSessionRequestCallback;
};
static_assert(sizeof(SessionRequestCallback) == 16, "CCallbackBase: vptr, flags @8, id @12");
SessionRequestCallback g_callback;  // static, never freed, never unregistered

// ---- leases ----
bool find_lease_locked(KilnHandle handle, Lease** out) {
    if ((handle & (0xFFFFull << 48)) != kHandleTag) return false;
    const size_t index = static_cast<size_t>(handle & 0xFF);
    if (index >= kMaxLeases || g_leases[index].handle != handle) return false;
    *out = &g_leases[index];
    return true;
}

// Whether the lease is open and its channels cover the channel: KILN_OK, KILN_ERR_NOT_FOUND (no such open lease) or
// KILN_ERR_INVALID_ARGUMENT (outside its range).
KilnResult check_lease(KilnHandle handle, uint32_t channel, bool any_channel) {
    std::lock_guard lock(g_mutex);
    Lease* lease = nullptr;
    if (!find_lease_locked(handle, &lease)) return KILN_ERR_NOT_FOUND;
    if (!any_channel && (channel < lease->first || channel > lease->last)) return KILN_ERR_INVALID_ARGUMENT;
    return KILN_OK;
}

void fill_status(KilnSteamMessagesStatus* out, uint32_t flags, uint64_t local_id) {
    uint32_t mode = KILN_STEAM_ACCEPT_NONE, listed = 0;
    bool leased = false;
    {
        std::lock_guard lock(g_mutex);  // the writers' lock: a stable view without the seqlock
        mode = g_policy.mode.load(std::memory_order_relaxed);
        listed = mode == KILN_STEAM_ACCEPT_LISTED ? g_policy.count.load(std::memory_order_relaxed) : 0;
        for (const Lease& l : g_leases) leased = leased || (l.handle && l.sessions);
    }
    if (g_config.server) flags |= KILN_STEAM_MSG_SERVER_IFACE;
    if (leased) flags |= KILN_STEAM_MSG_LEASED;
    if (g_registered.load(std::memory_order_acquire) && !g_register_faulted.load(std::memory_order_acquire) &&
        mode != KILN_STEAM_ACCEPT_NONE && !(flags & KILN_STEAM_MSG_CLOSED_FOR_EXIT))
        flags |= KILN_STEAM_MSG_REGISTERED;
    out->flags = flags;
    out->local_steam_id = local_id;
    out->sessions_accepted = g_sessions_accepted.load(std::memory_order_relaxed);
    out->sessions_refused = g_sessions_refused.load(std::memory_order_relaxed);
    out->accept_mode = mode;
    out->listed = listed;
    out->reserved = 0;
}

KilnResult snapshot(KilnSteamMessagesStatus* out) {
    const Resolved r = resolve_messages(false);
    if (r.state == Steam::Fault) {
        fill_status(out, 0, 0);
        return KILN_ERR_INTERNAL;
    }
    uint32_t flags = 0;
    uint64_t id = 0;
    if (r.state == Steam::Ready) {
        flags |= KILN_STEAM_MSG_AVAILABLE;
        bool fault = false;
        if (local_identity(r.handle, &id, &fault)) flags |= KILN_STEAM_MSG_IDENTITY;
        if (fault) {
            fill_status(out, flags, 0);
            return KILN_ERR_INTERNAL;
        }
    }
    fill_status(out, flags, id);
    return KILN_OK;
}

}  // namespace

Presence init(const Config& config, const char** missing) {
    std::lock_guard lock(g_mutex);
    g_config = config;
    g_exports = Exports{};
    g_complete = false;
    *missing = nullptr;
    if (!config.lookup) {
        *missing = "steam_api64.dll";
        return Presence::NoModule;
    }
    const RoleNames& n = config.server ? kServerNames : kUserNames;
    auto get = [&](const char* name) { return config.lookup(name, config.lookup_user); };
    g_exports.handle = reinterpret_cast<HandleFn>(get(n.handle));
    g_exports.messages = reinterpret_cast<AccessorFn>(get(n.messages));
    g_exports.sockets = reinterpret_cast<AccessorFn>(get(n.sockets));
    g_exports.find = reinterpret_cast<FindFn>(get(n.find));
    g_exports.send = reinterpret_cast<SendFn>(get("SteamAPI_ISteamNetworkingMessages_SendMessageToUser"));
    g_exports.receive = reinterpret_cast<ReceiveFn>(get("SteamAPI_ISteamNetworkingMessages_ReceiveMessagesOnChannel"));
    g_exports.accept = reinterpret_cast<AcceptFn>(get("SteamAPI_ISteamNetworkingMessages_AcceptSessionWithUser"));
    g_exports.release = reinterpret_cast<ReleaseFn>(get("SteamAPI_SteamNetworkingMessage_t_Release"));
    g_exports.identity = reinterpret_cast<IdentityFn>(get("SteamAPI_ISteamNetworkingSockets_GetIdentity"));
    g_exports.reg = reinterpret_cast<RegisterFn>(get("SteamAPI_RegisterCallback"));
    const char* first_missing = !g_exports.handle                       ? n.handle
                                : !g_exports.messages && !g_exports.find ? n.messages
                                : !g_exports.send    ? "SteamAPI_ISteamNetworkingMessages_SendMessageToUser"
                                : !g_exports.receive ? "SteamAPI_ISteamNetworkingMessages_ReceiveMessagesOnChannel"
                                : !g_exports.accept  ? "SteamAPI_ISteamNetworkingMessages_AcceptSessionWithUser"
                                : !g_exports.release ? "SteamAPI_SteamNetworkingMessage_t_Release"
                                : !g_exports.reg     ? "SteamAPI_RegisterCallback"
                                                     : nullptr;
    if (first_missing) {
        *missing = first_missing;
        return Presence::MissingExport;
    }
    g_complete = true;
    return Presence::Complete;
}

KilnResult KILN_CALL st_status(KilnSteamMessagesStatus* out) {
    if (!out || out->size < sizeof(KilnSteamMessagesStatus)) return KILN_ERR_INVALID_ARGUMENT;
    Entered entered;
    if (!entered.open) {
        fill_status(out, KILN_STEAM_MSG_CLOSED_FOR_EXIT, 0);
        return KILN_ERR_UNAVAILABLE;
    }
    return snapshot(out);
}

KilnResult KILN_CALL st_open(KilnOwner owner, const KilnSteamMessagesOpen* request, KilnHandle* lease) {
    Entered entered;
    if (!entered.open) return KILN_ERR_UNAVAILABLE;
    const KilnOwner designated = g_config.dialect_owner ? g_config.dialect_owner() : 0;
    if (owner == 0 || owner != designated) {
        if (!g_owner_refusal_logged.exchange(true, std::memory_order_relaxed)) {
            char line[160];
            std::snprintf(line, sizeof line,
                          "net: steam messages: owner %llu is not the designated owner; no lease (steam-messages-owner, "
                          "logged once)",
                          static_cast<unsigned long long>(owner));
            log_text(KILN_LOG_WARN, line);
        }
        return KILN_ERR_UNAVAILABLE;
    }
    if (!lease || !request || request->size < sizeof(KilnSteamMessagesOpen) ||
        (request->flags & ~static_cast<uint32_t>(KILN_STEAM_OPEN_SESSIONS)) ||
        request->first_channel > request->last_channel || request->last_channel > kMaxChannel)
        return KILN_ERR_INVALID_ARGUMENT;
    const bool sessions = (request->flags & KILN_STEAM_OPEN_SESSIONS) != 0;
    std::lock_guard lock(g_mutex);
    size_t free_slot = kMaxLeases;
    for (size_t i = 0; i < kMaxLeases; ++i) {
        const Lease& l = g_leases[i];
        if (!l.handle) {
            if (free_slot == kMaxLeases) free_slot = i;
            continue;
        }
        if (request->first_channel <= l.last && l.first <= request->last_channel) return KILN_ERR_INVALID_ARGUMENT;
        if (sessions && l.sessions) return KILN_ERR_INVALID_ARGUMENT;
    }
    if (free_slot == kMaxLeases) return KILN_ERR_INVALID_ARGUMENT;
    g_generation = (g_generation + 1) & 0xFFFFFFFFFFull;  // 40 bits between the tag and the slot
    if (!g_generation) g_generation = 1;
    Lease& l = g_leases[free_slot];
    l.handle = kHandleTag | (g_generation << 8) | free_slot;
    l.owner = owner;
    l.first = request->first_channel;
    l.last = request->last_channel;
    l.sessions = sessions;
    *lease = l.handle;
    return KILN_OK;
}

KilnResult KILN_CALL st_close(KilnHandle handle) {
    Entered entered;
    if (!entered.open) return KILN_ERR_UNAVAILABLE;
    std::lock_guard lock(g_mutex);
    Lease* lease = nullptr;
    if (!find_lease_locked(handle, &lease)) return KILN_OK;  // closed already (or never a lease): nothing to do
    if (lease->sessions) write_policy_locked(KILN_STEAM_ACCEPT_NONE, nullptr, 0);  // the callback answers nothing now
    *lease = Lease{};
    return KILN_OK;
}

KilnResult KILN_CALL st_maintain(KilnHandle handle, const KilnSteamAcceptPolicy* policy, KilnSteamMessagesStatus* out) {
    if (out && out->size < sizeof(KilnSteamMessagesStatus)) return KILN_ERR_INVALID_ARGUMENT;
    Entered entered;
    if (!entered.open) {
        if (out) fill_status(out, KILN_STEAM_MSG_CLOSED_FOR_EXIT, 0);
        return KILN_ERR_UNAVAILABLE;
    }
    {
        std::lock_guard lock(g_mutex);
        Lease* lease = nullptr;
        if (!find_lease_locked(handle, &lease)) return KILN_ERR_NOT_FOUND;
        if (!lease->sessions) return KILN_ERR_INVALID_ARGUMENT;
    }
    if (!policy || policy->size < sizeof(KilnSteamAcceptPolicy) || policy->mode > KILN_STEAM_ACCEPT_LISTED ||
        policy->count > kMaxIds || (policy->mode == KILN_STEAM_ACCEPT_LISTED && policy->count && !policy->ids))
        return KILN_ERR_INVALID_ARGUMENT;
    const uint32_t mode = policy->mode;
    const uint32_t count = mode == KILN_STEAM_ACCEPT_LISTED ? policy->count : 0;
    uint64_t ids[kMaxIds];
    for (uint32_t i = 0; i < count; ++i) {
        ids[i] = policy->ids[i];
        if (!ids[i]) return KILN_ERR_INVALID_ARGUMENT;
    }
    // NONE calls no Steam function and only narrows: it is in force at once, whatever Steam's state (as closing the
    // session lease is), so the callback never answers on a policy a NONE replaced, also once Steam is back.
    if (mode == KILN_STEAM_ACCEPT_NONE) {
        std::lock_guard lock(g_mutex);
        Lease* lease = nullptr;
        if (!find_lease_locked(handle, &lease) || !lease->sessions) return KILN_ERR_NOT_FOUND;  // closed meanwhile
        write_policy_locked(KILN_STEAM_ACCEPT_NONE, nullptr, 0);
    }
    const Resolved r = resolve_messages(false);
    if (r.state == Steam::Fault) {
        if (out) fill_status(out, 0, 0);
        return KILN_ERR_INTERNAL;
    }
    if (r.state != Steam::Ready) {  // Steam not ready: a policy that answers waits for it
        if (out) fill_status(out, 0, 0);
        return KILN_OK;
    }
    if (mode != KILN_STEAM_ACCEPT_NONE) {
        std::lock_guard lock(g_mutex);
        Lease* lease = nullptr;
        if (!find_lease_locked(handle, &lease) || !lease->sessions) return KILN_ERR_NOT_FOUND;  // closed meanwhile
        write_policy_locked(mode, ids, count);
    }
    KilnResult rc = KILN_OK;
    // Once per process; the flag is set before the call, so a second maintain never registers again.
    if (mode != KILN_STEAM_ACCEPT_NONE && !g_registered.exchange(true, std::memory_order_acq_rel)) {
        if (g_config.server) g_callback.callback_flags |= kCallbackFlagGameServer;  // before the call: see above
        if (call_register(g_exports.reg, &g_callback, kSessionRequestCallback)) {
            log_text(KILN_LOG_INFO, g_config.server
                                        ? "net: steam messages: session callback registered (GameServer, once per process)"
                                        : "net: steam messages: session callback registered (user, once per process)");
        } else {
            g_register_faulted.store(true, std::memory_order_release);
            note_fault(kFaultRegister, "RegisterCallback", false);
            rc = KILN_ERR_INTERNAL;
        }
    }
    if (out) {
        const KilnResult s = snapshot(out);
        if (rc == KILN_OK) rc = s;
    }
    return rc;
}

KilnResult KILN_CALL st_send(KilnHandle handle, uint64_t peer, const void* data, uint32_t len, uint32_t channel,
                             uint32_t flags, int32_t* steam_result) {
    Entered entered;
    if (!entered.open) return KILN_ERR_UNAVAILABLE;
    if (!steam_result || !peer || len > KILN_STEAM_MESSAGE_MAX || (len && !data) ||
        (flags != 0 && flags != 8 && flags != 32 && flags != 40))
        return KILN_ERR_INVALID_ARGUMENT;
    *steam_result = 0;
    if (const KilnResult rc = check_lease(handle, channel, false); rc != KILN_OK) return rc;
    const Resolved r = resolve_messages(false);
    if (r.state == Steam::Fault) return KILN_ERR_INTERNAL;
    if (r.state != Steam::Ready) return KILN_ERR_UNAVAILABLE;
    const Identity target = steam_identity(peer);
    int32_t result = 0;
    if (!call_send(g_exports.send, r.messages, &target, data, len, static_cast<int32_t>(flags),
                   static_cast<int32_t>(channel), &result)) {
        note_fault(kFaultSend, "SendMessageToUser", false);
        return KILN_ERR_INTERNAL;
    }
    *steam_result = result;
    return KILN_OK;
}

KilnResult KILN_CALL st_receive(KilnHandle handle, uint32_t channel, void* buffer, uint32_t cap,
                                KilnSteamMessageInfo* out, uint32_t* got) {
    Entered entered;
    if (!entered.open) return KILN_ERR_UNAVAILABLE;
    if (!got || !out || out->size < sizeof(KilnSteamMessageInfo) || (cap && !buffer)) return KILN_ERR_INVALID_ARGUMENT;
    *got = 0;
    if (const KilnResult rc = check_lease(handle, channel, false); rc != KILN_OK) return rc;
    const Resolved r = resolve_messages(false);
    if (r.state == Steam::Fault) return KILN_ERR_INTERNAL;
    if (r.state != Steam::Ready) return KILN_ERR_UNAVAILABLE;
    MessagePrefix* message = nullptr;
    int32_t count = 0;
    if (!call_receive(g_exports.receive, r.messages, static_cast<int32_t>(channel), &message, &count)) {
        note_fault(kFaultReceive, "ReceiveMessagesOnChannel", false);
        return KILN_ERR_INTERNAL;  // what Steam handed out (if anything) is unknown: nothing to release
    }
    if (count == 0) return KILN_OK;
    // From here the message is released exactly once on every path.
    const auto release = [&] {
        if (call_release(g_exports.release, message)) return true;
        note_fault(kFaultRelease, "message release", false);
        return false;
    };
    if (count != 1 || !message) {
        if (message) release();
        return KILN_ERR_INTERNAL;
    }
    int32_t size = -1, flags = 0;
    uint64_t peer = 0;
    const bool read = read_message(message, buffer, cap, &size, &peer, &flags);
    const bool released = release();
    if (!read || !released) return KILN_ERR_INTERNAL;
    if (size < 0) return KILN_STEAM_ERR_INVALID_DATA;
    out->length = static_cast<uint32_t>(size);
    out->peer_steam_id = peer;
    out->channel = channel;
    out->flags = (flags & KILN_STEAM_SEND_RELIABLE) ? KILN_STEAM_SEND_RELIABLE : 0u;
    *got = 1;
    return KILN_OK;
}

KilnResult KILN_CALL st_accept(KilnHandle handle, uint64_t peer, uint32_t* accepted) {
    Entered entered;
    if (!entered.open) return KILN_ERR_UNAVAILABLE;
    if (!accepted || !peer) return KILN_ERR_INVALID_ARGUMENT;
    *accepted = 0;
    if (const KilnResult rc = check_lease(handle, 0, true); rc != KILN_OK) return rc;
    const Resolved r = resolve_messages(false);
    if (r.state == Steam::Fault) return KILN_ERR_INTERNAL;
    if (r.state != Steam::Ready) return KILN_ERR_UNAVAILABLE;
    const Identity target = steam_identity(peer);
    bool ok = false;
    if (!call_accept(g_exports.accept, r.messages, &target, &ok)) {
        note_fault(kFaultAccept, "AcceptSessionWithUser", false);
        return KILN_ERR_INTERNAL;
    }
    *accepted = ok ? 1u : 0u;
    return KILN_OK;
}

bool close_for_exit(uint32_t wait_ms) {
    g_closed.store(true, std::memory_order_seq_cst);
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(wait_ms);
    while (g_inflight.load(std::memory_order_seq_cst) != 0) {
        if (std::chrono::steady_clock::now() >= until) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

bool ready_seen() { return g_ready_seen.load(std::memory_order_relaxed); }

void reset_for_tests() {
    std::lock_guard lock(g_mutex);
    g_exports = Exports{};
    g_config = Config{};
    g_complete = false;
    g_closed.store(false);
    g_inflight.store(0);
    g_sessions_accepted.store(0);
    g_sessions_refused.store(0);
    g_callback_faults.store(0);
    g_ready_seen.store(false);
    g_fault_logged.store(0);
    g_registered.store(false);
    g_register_faulted.store(false);
    g_owner_refusal_logged.store(false);
    for (Lease& l : g_leases) l = Lease{};
    write_policy_locked(KILN_STEAM_ACCEPT_NONE, nullptr, 0);
    for (auto& id : g_policy.ids) id.store(0, std::memory_order_relaxed);
    g_callback.callback_flags = 0;
    g_callback.callback_id = kSessionRequestCallback;
}

void* callback_object_for_tests() { return &g_callback; }

}  // namespace knet::steam
