// SPDX-License-Identifier: MIT
// test infrastructure only (see standin.h). Builds as steam_api64.dll into the test output folder only.
// Layouts follow ShroudForge's steam_network.cpp (MIT, Copyright (c) 2026 bonsaibauer) and Steamworks' public headers.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#include <windows.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <vector>

#include "standin.h"

#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

#define STANDIN_API extern "C" __declspec(dllexport)

namespace {

constexpr int32_t k_identity_type_steam_id = 16;
constexpr int32_t k_identity_size_steam_id = 8;
constexpr uint32_t k_max_message_bytes = 512u * 1024u;
constexpr int32_t k_session_request_callback = 1251;
constexpr uint8_t k_callback_flag_registered = 0x01;
constexpr uint8_t k_callback_flag_game_server = 0x02;
constexpr int32_t k_send_reliable = 8;  // k_nSteamNetworkingSend_Reliable: the only flag bit a received message keeps
// MSVC's vtable puts overloads in reverse order of declaration: CCallbackBase declares Run(void*) before
// Run(void*, bool, SteamAPICall_t), so Run(void*) is slot 1 (slot 0 is the three-argument Run, slot 2 the size).
constexpr size_t k_run_slot = 1;
constexpr int32_t k_user_handle = 1;
constexpr int32_t k_server_handle = 2;

enum EResult : int32_t { k_ok = 1, k_no_connection = 3, k_invalid_param = 8 };

struct SteamNetworkingIdentity {
    int32_t type;
    int32_t size;
    union {
        uint64_t steam_id;
        uint8_t reserved[128];
    } data;
};
static_assert(sizeof(SteamNetworkingIdentity) == 136);

struct SteamNetworkingMessage {
    void* data;
    int32_t size;
    uint32_t connection;
    SteamNetworkingIdentity peer;
    int64_t connection_user_data;
    int64_t received_usec;
    int64_t message_number;
    void (*free_data)(SteamNetworkingMessage*);
    void (*release)(SteamNetworkingMessage*);
    int32_t channel;
    int32_t flags;
    int64_t user_data;
    uint16_t lane;
    uint16_t pad[3];
};
static_assert(offsetof(SteamNetworkingMessage, peer) == 16);
static_assert(offsetof(SteamNetworkingMessage, release) == 184);
static_assert(offsetof(SteamNetworkingMessage, channel) == 192);
static_assert(offsetof(SteamNetworkingMessage, flags) == 196);

// CCallbackBase as Steam sees it: vptr @0, m_nCallbackFlags @8, m_iCallback @12.
constexpr size_t k_callback_flags_offset = 8;
constexpr size_t k_callback_id_offset = 12;
using RunFn = void (*)(void* self, void* parameter);

// One interface object per interface and role; the flat functions check the self pointer against them.
struct Interface { uint64_t tag; };
Interface g_user_messages{0x55534d47}, g_user_sockets{0x5553534b}, g_server_messages{0x47534d47},
    g_server_sockets{0x4753534b};

// The fabric datagram. A message larger than one datagram goes as fragments that share msg_id.
#pragma pack(push, 1)
struct WireHeader {
    uint32_t magic;
    uint16_t version;
    uint16_t flags_low;
    uint64_t from;
    uint64_t msg_id;
    int32_t channel;
    int32_t send_flags;
    uint32_t total;
    uint32_t offset;
    uint16_t fragment;
    uint16_t fragments;
};
#pragma pack(pop)
constexpr uint32_t k_wire_magic = 0x4953534b; // "KSSI"
constexpr uint16_t k_wire_version = 1;
constexpr uint32_t k_fragment_bytes = 60000;
constexpr uint32_t k_max_fragments = (k_max_message_bytes + k_fragment_bytes - 1) / k_fragment_bytes;

struct Inbound {
    uint64_t from;
    int32_t channel;
    int32_t flags;
    std::vector<uint8_t> payload;
};

struct Partial {
    int32_t channel{};
    int32_t flags{};
    uint32_t total{};
    uint16_t fragments{};
    uint16_t received{};
    std::vector<bool> got;
    std::vector<uint8_t> payload;
    ULONGLONG started_ms{};
};

struct Callback {
    void* object;
    int32_t id;
};

std::mutex g_mutex;
// state under g_mutex
uint64_t g_steam_id{};
bool g_server{};
uint32_t g_port_base{};
SOCKET g_socket{INVALID_SOCKET};
HANDLE g_receiver{};  // a raw thread: a joinable std::thread in a static would terminate the process at exit
std::set<uint64_t> g_accepted;   // sessions accepted (or opened by sending)
std::set<uint64_t> g_requested;  // a session request was raised, not accepted yet
std::map<uint64_t, std::deque<Inbound>> g_pending;  // held until accepted, per peer
std::map<int32_t, std::deque<Inbound>> g_channels;  // deliverable, per channel
std::deque<uint64_t> g_requests;  // session requests for the next pump
std::vector<Callback> g_callbacks;
std::set<SteamNetworkingMessage*> g_live;
std::deque<SteamNetworkingMessage*> g_quarantine;  // released headers, freed late so an address is not reused at once
std::map<std::pair<uint64_t, uint64_t>, Partial> g_partials;
uint64_t g_next_msg_id{1};
int64_t g_message_number{};
bool g_env_checked{};

std::atomic<bool> g_receiver_stop{};
constexpr uint32_t k_knob_count = uint32_t{STANDIN_KNOB_CALLS} + uint32_t{STANDIN_EXPORT_COUNT};
std::atomic<uint64_t> g_knobs[k_knob_count];

uint64_t knob(uint32_t id) { return g_knobs[id].load(std::memory_order_acquire); }
void bump(uint32_t id) { g_knobs[id].fetch_add(1, std::memory_order_acq_rel); }

uint64_t pending_count_locked() {
    uint64_t n = 0;
    for (const auto& [peer, queue] : g_pending) n += queue.size();
    return n;
}

void deliver_locked(Inbound&& message) {
    if (g_accepted.contains(message.from)) {
        g_channels[message.channel].push_back(std::move(message));
        return;
    }
    const auto from = message.from;
    g_pending[from].push_back(std::move(message));
    if (g_requested.insert(from).second) {
        g_requests.push_back(from);
        bump(STANDIN_KNOB_SESSION_REQUESTS);
    }
}

// Accepting (or sending to) a peer makes its held messages deliverable, in arrival order.
void accept_locked(uint64_t peer) {
    g_accepted.insert(peer);
    g_requested.erase(peer);
    auto it = g_pending.find(peer);
    if (it == g_pending.end()) return;
    for (auto& message : it->second) g_channels[message.channel].push_back(std::move(message));
    g_pending.erase(it);
}

void on_datagram(const uint8_t* data, size_t length) {
    WireHeader header;
    if (length < sizeof header) { bump(STANDIN_KNOB_WIRE_REJECTS); return; }
    std::memcpy(&header, data, sizeof header);
    const size_t body = length - sizeof header;
    if (header.magic != k_wire_magic || header.version != k_wire_version || !header.from ||
        header.total > k_max_message_bytes || !header.fragments || header.fragments > k_max_fragments ||
        header.fragment >= header.fragments || header.offset > header.total || body > header.total - header.offset ||
        header.channel < 0) {
        bump(STANDIN_KNOB_WIRE_REJECTS);
        return;
    }
    std::lock_guard lock(g_mutex);
    if (header.fragments == 1) {
        if (body != header.total || header.offset) { bump(STANDIN_KNOB_WIRE_REJECTS); return; }
        deliver_locked(Inbound{header.from, header.channel, header.send_flags,
                               std::vector<uint8_t>(data + sizeof header, data + length)});
        return;
    }
    const auto now = GetTickCount64();
    for (auto it = g_partials.begin(); it != g_partials.end();)
        it = now - it->second.started_ms > 2000 ? g_partials.erase(it) : std::next(it);
    if (g_partials.size() >= 64) { bump(STANDIN_KNOB_WIRE_REJECTS); return; }
    auto& partial = g_partials[{header.from, header.msg_id}];
    if (!partial.fragments) {
        partial.channel = header.channel;
        partial.flags = header.send_flags;
        partial.total = header.total;
        partial.fragments = header.fragments;
        partial.got.assign(header.fragments, false);
        partial.payload.resize(header.total);
        partial.started_ms = now;
    }
    if (partial.total != header.total || partial.fragments != header.fragments ||
        header.offset != static_cast<uint32_t>(header.fragment) * k_fragment_bytes ||
        (header.fragment + 1u < header.fragments && body != k_fragment_bytes) ||
        (header.fragment + 1u == header.fragments && header.offset + body != header.total)) {
        g_partials.erase({header.from, header.msg_id});
        bump(STANDIN_KNOB_WIRE_REJECTS);
        return;
    }
    if (partial.got[header.fragment]) return;
    partial.got[header.fragment] = true;
    std::memcpy(partial.payload.data() + header.offset, data + sizeof header, body);
    if (++partial.received < partial.fragments) return;
    Inbound message{header.from, partial.channel, partial.flags, std::move(partial.payload)};
    g_partials.erase({header.from, header.msg_id});
    deliver_locked(std::move(message));
}

DWORD WINAPI receiver_loop(void* parameter) {
    const SOCKET s = reinterpret_cast<SOCKET>(parameter);
    std::vector<uint8_t> buffer(65536);
    while (!g_receiver_stop.load(std::memory_order_acquire)) {
        const int got = recv(s, reinterpret_cast<char*>(buffer.data()), static_cast<int>(buffer.size()), 0);
        if (got > 0) on_datagram(buffer.data(), static_cast<size_t>(got));
        else if (got == SOCKET_ERROR) {
            const int error = WSAGetLastError();
            if (error != WSAETIMEDOUT && error != WSAECONNRESET && error != WSAEMSGSIZE) Sleep(10);
        }
    }
    return 0;
}

uint16_t port_for(uint64_t steam_id) { return static_cast<uint16_t>(g_port_base + steam_id % 1024); }

void stop_fabric_locked(std::unique_lock<std::mutex>& lock) {
    if (g_socket == INVALID_SOCKET) return;
    g_receiver_stop.store(true, std::memory_order_release);
    const SOCKET s = g_socket;
    g_socket = INVALID_SOCKET;
    const HANDLE receiver = g_receiver;
    g_receiver = nullptr;
    lock.unlock();  // the receiver takes the lock for each datagram
    if (receiver) {
        WaitForSingleObject(receiver, INFINITE);
        CloseHandle(receiver);
    }
    closesocket(s);
    lock.lock();
    g_receiver_stop.store(false, std::memory_order_release);
}

int32_t set_identity(uint64_t steam_id, uint32_t port_base, uint32_t flags) {
    if (!steam_id || port_base < 1024 || port_base > 64511) return 0;
    std::unique_lock lock(g_mutex);
    g_env_checked = true;
    stop_fabric_locked(lock);
    g_steam_id = 0;
    g_accepted.clear();
    g_requested.clear();
    g_pending.clear();
    g_channels.clear();
    g_requests.clear();
    g_partials.clear();
    static bool wsa = [] { WSADATA d; return WSAStartup(MAKEWORD(2, 2), &d) == 0; }();
    if (!wsa) return 0;
    const SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return 0;
    int buffer = 16 * 1024 * 1024;
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&buffer), sizeof buffer);
    setsockopt(s, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&buffer), sizeof buffer);
    DWORD timeout = 50;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof timeout);
    BOOL report = FALSE;  // a datagram to a closed port must not fail the next recv (WSAECONNRESET)
    DWORD unused = 0;
    WSAIoctl(s, SIO_UDP_CONNRESET, &report, sizeof report, nullptr, 0, &unused, nullptr, nullptr);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    g_port_base = port_base;
    address.sin_port = htons(port_for(steam_id));
    if (bind(s, reinterpret_cast<const sockaddr*>(&address), sizeof address) != 0) {
        closesocket(s);
        return 0;
    }
    g_socket = s;
    g_steam_id = steam_id;
    g_server = (flags & STANDIN_ROLE_SERVER) != 0;
    g_receiver = CreateThread(nullptr, 0, receiver_loop, reinterpret_cast<void*>(s), 0, nullptr);
    if (!g_receiver) {
        g_socket = INVALID_SOCKET;
        g_steam_id = 0;
        closesocket(s);
        return 0;
    }
    return 1;
}

// Test processes that load an unmodified Steam client (no StandIn_SetIdentity call) configure the stand-in through
// the environment, read once at the first export call.
void env_identity() {
    {
        std::lock_guard lock(g_mutex);
        if (g_env_checked) return;
        g_env_checked = true;
    }
    char id[32]{}, base[16]{}, role[16]{};
    if (!GetEnvironmentVariableA("STEAM_STANDIN_ID", id, sizeof id) ||
        !GetEnvironmentVariableA("STEAM_STANDIN_PORT_BASE", base, sizeof base))
        return;
    GetEnvironmentVariableA("STEAM_STANDIN_ROLE", role, sizeof role);
    set_identity(std::strtoull(id, nullptr, 10), static_cast<uint32_t>(std::strtoul(base, nullptr, 10)),
                 std::strcmp(role, "server") == 0 ? STANDIN_ROLE_SERVER : 0);
}

bool ready() {
    std::lock_guard lock(g_mutex);
    return g_steam_id && knob(STANDIN_KNOB_INIT_DELAY_PUMPS) == 0;
}

// Every export starts here: counts the call, raises the requested fault, and says whether Steam is ready.
bool enter(StandInExport id, bool counts_before_ready = true) {
    env_identity();
    bump(uint32_t{STANDIN_KNOB_CALLS} + static_cast<uint32_t>(id));
    if (knob(STANDIN_KNOB_FAULT_EXPORT) == static_cast<uint64_t>(id)) {
        volatile int* volatile target = nullptr;
        *target = 1;
    }
    const bool is_ready = ready();
    if (!is_ready && counts_before_ready) bump(STANDIN_KNOB_CALLS_BEFORE_READY);
    return is_ready;
}

bool is_server() {
    std::lock_guard lock(g_mutex);
    return g_server;
}

bool valid_messages(void* self) {
    const bool ok = self == (is_server() ? &g_server_messages : &g_user_messages);
    if (!ok) bump(STANDIN_KNOB_BAD_SELF);
    return ok;
}

bool steam_identity(const SteamNetworkingIdentity* identity) {
    return identity && identity->type == k_identity_type_steam_id && identity->size == k_identity_size_steam_id &&
           identity->data.steam_id;
}

void free_message(SteamNetworkingMessage* message) {
    std::free(message->data);
    message->data = nullptr;
}

}  // namespace

STANDIN_API void __cdecl SteamAPI_SteamNetworkingMessage_t_Release(SteamNetworkingMessage* message);

STANDIN_API void* __cdecl SteamAPI_SteamNetworkingMessages_SteamAPI_v002() {
    const bool r = enter(STANDIN_EXPORT_USER_MESSAGES_V002);
    return r && !is_server() && !knob(STANDIN_KNOB_ACCESSOR_NULL) ? &g_user_messages : nullptr;
}

STANDIN_API void* __cdecl SteamAPI_SteamGameServerNetworkingMessages_SteamAPI_v002() {
    const bool r = enter(STANDIN_EXPORT_SERVER_MESSAGES_V002);
    return r && is_server() && !knob(STANDIN_KNOB_ACCESSOR_NULL) ? &g_server_messages : nullptr;
}

// v013 only, as the game's steam_api64.dll (it has no v012 accessor)
STANDIN_API void* __cdecl SteamAPI_SteamNetworkingSockets_SteamAPI_v013() {
    const bool r = enter(STANDIN_EXPORT_USER_SOCKETS_V013);
    return r && !is_server() && !knob(STANDIN_KNOB_ACCESSOR_NULL) ? &g_user_sockets : nullptr;
}

STANDIN_API void* __cdecl SteamAPI_SteamGameServerNetworkingSockets_SteamAPI_v013() {
    const bool r = enter(STANDIN_EXPORT_SERVER_SOCKETS_V013);
    return r && is_server() && !knob(STANDIN_KNOB_ACCESSOR_NULL) ? &g_server_sockets : nullptr;
}

STANDIN_API int32_t __cdecl SteamAPI_GetHSteamUser() {
    return enter(STANDIN_EXPORT_USER_HANDLE, false) && !is_server() ? k_user_handle : 0;
}

STANDIN_API int32_t __cdecl SteamGameServer_GetHSteamUser() {
    return enter(STANDIN_EXPORT_SERVER_HANDLE, false) && is_server() ? k_server_handle : 0;
}

STANDIN_API void* __cdecl SteamInternal_FindOrCreateUserInterface(int32_t user, const char* version) {
    if (!enter(STANDIN_EXPORT_FIND_USER) || is_server() || user != k_user_handle || !version) return nullptr;
    if (!std::strcmp(version, "SteamNetworkingMessages002")) return &g_user_messages;
    // 013 is the game's version; 012 is what ShroudForge's fallback asks for (served here, unknown in Steam)
    if (!std::strcmp(version, "SteamNetworkingSockets013") || !std::strcmp(version, "SteamNetworkingSockets012"))
        return &g_user_sockets;
    return nullptr;
}

STANDIN_API void* __cdecl SteamInternal_FindOrCreateGameServerInterface(int32_t user, const char* version) {
    if (!enter(STANDIN_EXPORT_FIND_SERVER) || !is_server() || user != k_server_handle || !version) return nullptr;
    if (!std::strcmp(version, "SteamNetworkingMessages002")) return &g_server_messages;
    if (!std::strcmp(version, "SteamNetworkingSockets013") || !std::strcmp(version, "SteamNetworkingSockets012"))
        return &g_server_sockets;
    return nullptr;
}

STANDIN_API bool __cdecl SteamAPI_ISteamNetworkingSockets_GetIdentity(void* self, SteamNetworkingIdentity* out) {
    if (!enter(STANDIN_EXPORT_GET_IDENTITY) || !out) return false;
    if (self != (is_server() ? &g_server_sockets : &g_user_sockets)) {
        bump(STANDIN_KNOB_BAD_SELF);
        return false;
    }
    std::memset(out, 0, sizeof *out);
    out->type = k_identity_type_steam_id;
    out->size = k_identity_size_steam_id;
    std::lock_guard lock(g_mutex);
    out->data.steam_id = g_steam_id;
    return true;
}

STANDIN_API int32_t __cdecl SteamAPI_ISteamNetworkingMessages_SendMessageToUser(
    void* self, const SteamNetworkingIdentity* peer, const void* data, uint32_t size, int32_t flags, int32_t channel) {
    if (!enter(STANDIN_EXPORT_SEND)) return k_no_connection;
    if (!valid_messages(self) || !steam_identity(peer) || (size && !data) || channel < 0) return k_invalid_param;
    // too big is k_EResultInvalidParam in Steam (k_EResultLimitExceeded means a full send buffer, not modelled)
    if (size > k_max_message_bytes) return k_invalid_param;
    g_knobs[STANDIN_KNOB_SEND_FLAGS_SEEN].fetch_or(uint64_t{1} << (static_cast<uint32_t>(flags) & 63u),
                                                   std::memory_order_acq_rel);
    if (const auto injected = knob(STANDIN_KNOB_SEND_ERESULT)) return static_cast<int32_t>(injected);
    const uint64_t to = peer->data.steam_id;
    std::unique_lock lock(g_mutex);
    accept_locked(to);  // in Steam, sending to a peer accepts its session
    for (uint64_t drops = knob(STANDIN_KNOB_DROP_SENDS); drops;) {
        if (g_knobs[STANDIN_KNOB_DROP_SENDS].compare_exchange_weak(drops, drops - 1)) return k_ok;
    }
    if (g_socket == INVALID_SOCKET) return k_no_connection;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port_for(to));
    const uint16_t fragments = static_cast<uint16_t>(size ? (size + k_fragment_bytes - 1) / k_fragment_bytes : 1);
    WireHeader header{k_wire_magic, k_wire_version, 0, g_steam_id, g_next_msg_id++, channel, flags, size, 0, 0,
                      fragments};
    std::vector<uint8_t> datagram;
    for (uint16_t i = 0; i < fragments; ++i) {
        header.fragment = i;
        header.offset = static_cast<uint32_t>(i) * k_fragment_bytes;
        const uint32_t body = size ? (std::min)(k_fragment_bytes, size - header.offset) : 0;
        datagram.resize(sizeof header + body);
        std::memcpy(datagram.data(), &header, sizeof header);
        if (body) std::memcpy(datagram.data() + sizeof header, static_cast<const uint8_t*>(data) + header.offset, body);
        if (sendto(g_socket, reinterpret_cast<const char*>(datagram.data()), static_cast<int>(datagram.size()), 0,
                   reinterpret_cast<const sockaddr*>(&address), sizeof address) == SOCKET_ERROR)
            return k_no_connection;
    }
    return k_ok;
}

STANDIN_API int32_t __cdecl SteamAPI_ISteamNetworkingMessages_ReceiveMessagesOnChannel(
    void* self, int32_t channel, SteamNetworkingMessage** out, int32_t max) {
    if (!enter(STANDIN_EXPORT_RECEIVE) || !valid_messages(self) || !out || max <= 0) return 0;
    std::lock_guard lock(g_mutex);
    auto it = g_channels.find(channel);
    int32_t count = 0;
    while (it != g_channels.end() && !it->second.empty() && count < max) {
        Inbound inbound = std::move(it->second.front());
        it->second.pop_front();
        auto* message = static_cast<SteamNetworkingMessage*>(std::calloc(1, sizeof(SteamNetworkingMessage)));
        message->data = std::malloc(inbound.payload.empty() ? 1 : inbound.payload.size());
        if (!inbound.payload.empty()) std::memcpy(message->data, inbound.payload.data(), inbound.payload.size());
        message->size = static_cast<int32_t>(inbound.payload.size());
        message->peer.type = k_identity_type_steam_id;
        message->peer.size = k_identity_size_steam_id;
        message->peer.data.steam_id = inbound.from;
        LARGE_INTEGER now, frequency;
        QueryPerformanceCounter(&now);
        QueryPerformanceFrequency(&frequency);
        message->received_usec = static_cast<int64_t>(now.QuadPart / (frequency.QuadPart / 1000000));
        message->message_number = ++g_message_number;
        message->release = &SteamAPI_SteamNetworkingMessage_t_Release;
        message->channel = channel;
        message->flags = inbound.flags & k_send_reliable;  // Steam: only the reliable bit is valid on receipt
        g_live.insert(message);
        out[count++] = message;
    }
    return count;
}

STANDIN_API bool __cdecl SteamAPI_ISteamNetworkingMessages_AcceptSessionWithUser(
    void* self, const SteamNetworkingIdentity* peer) {
    if (!enter(STANDIN_EXPORT_ACCEPT) || !valid_messages(self) || !steam_identity(peer)) return false;
    std::lock_guard lock(g_mutex);
    const uint64_t id = peer->data.steam_id;
    // Steam: true for a pending request and for a session that is already active, false for no session at all
    if (g_accepted.contains(id)) return true;
    if (!g_requested.contains(id)) return false;
    accept_locked(id);
    return true;
}

STANDIN_API void __cdecl SteamAPI_SteamNetworkingMessage_t_Release(SteamNetworkingMessage* message) {
    enter(STANDIN_EXPORT_RELEASE);
    if (!message) return;
    std::lock_guard lock(g_mutex);
    if (!g_live.erase(message)) {  // never dereferenced: it may be freed
        bump(STANDIN_KNOB_DOUBLE_RELEASES);
        return;
    }
    free_message(message);
    bump(STANDIN_KNOB_RELEASES);
    g_quarantine.push_back(message);
    if (g_quarantine.size() > 4096) {
        std::free(g_quarantine.front());
        g_quarantine.pop_front();
    }
}

STANDIN_API void __cdecl SteamAPI_RegisterCallback(void* callback, int32_t id) {
    enter(STANDIN_EXPORT_REGISTER);
    if (!callback) return;
    auto* bytes = static_cast<uint8_t*>(callback);
    bytes[k_callback_flags_offset] |= k_callback_flag_registered;
    std::memcpy(bytes + k_callback_id_offset, &id, sizeof id);
    std::lock_guard lock(g_mutex);
    for (const auto& entry : g_callbacks)
        if (entry.object == callback) return;
    g_callbacks.push_back({callback, id});
}

STANDIN_API void __cdecl SteamAPI_UnregisterCallback(void* callback) {
    enter(STANDIN_EXPORT_UNREGISTER);
    if (!callback) return;
    std::lock_guard lock(g_mutex);
    for (auto it = g_callbacks.begin(); it != g_callbacks.end(); ++it) {
        if (it->object != callback) continue;
        static_cast<uint8_t*>(callback)[k_callback_flags_offset] &= static_cast<uint8_t>(~k_callback_flag_registered);
        g_callbacks.erase(it);
        return;
    }
}

STANDIN_API int32_t __cdecl StandIn_SetIdentity(uint64_t steam_id, uint32_t port_base, uint32_t flags) {
    return set_identity(steam_id, port_base, flags);
}

STANDIN_API int32_t __cdecl StandIn_Pump() {
    env_identity();
    for (uint64_t delay = knob(STANDIN_KNOB_INIT_DELAY_PUMPS); delay;) {
        if (g_knobs[STANDIN_KNOB_INIT_DELAY_PUMPS].compare_exchange_weak(delay, delay - 1)) break;
    }
    std::deque<uint64_t> requests;
    std::vector<Callback> callbacks;
    bool server;
    {
        std::lock_guard lock(g_mutex);
        requests.swap(g_requests);
        callbacks = g_callbacks;
        server = g_server;
    }
    int32_t runs = 0;
    for (const uint64_t peer : requests) {
        SteamNetworkingIdentity request{};  // SteamNetworkingMessagesSessionRequest_t: the remote identity
        request.type = k_identity_type_steam_id;
        request.size = k_identity_size_steam_id;
        request.data.steam_id = peer;
        for (const auto& callback : callbacks) {
            if (callback.id != k_session_request_callback) continue;
            // steam_api64 compares the GameServer bit with the pump's role at every dispatch, not at registration
            const bool game_server =
                (static_cast<const uint8_t*>(callback.object)[k_callback_flags_offset] & k_callback_flag_game_server) != 0;
            if (game_server != server) continue;
            const auto run = (*static_cast<RunFn* const*>(callback.object))[k_run_slot];
            run(callback.object, &request);
            bump(STANDIN_KNOB_CALLBACKS_RUN);
            ++runs;
        }
    }
    return runs;
}

STANDIN_API uint64_t __cdecl StandIn_Knob(uint32_t id, uint64_t value) {
    const bool read_only = (id & STANDIN_KNOB_READ) != 0;
    id &= ~STANDIN_KNOB_READ;
    if (id >= k_knob_count) return 0;
    if (id == STANDIN_KNOB_LIVE_MESSAGES || id == STANDIN_KNOB_PENDING_MESSAGES || id == STANDIN_KNOB_REGISTERED) {
        std::lock_guard lock(g_mutex);
        return id == STANDIN_KNOB_LIVE_MESSAGES ? g_live.size()
               : id == STANDIN_KNOB_PENDING_MESSAGES ? pending_count_locked()
                                                     : g_callbacks.size();
    }
    if (id == STANDIN_KNOB_EXPIRE_SESSION) {
        // Steam's timeout of a session request nobody accepted: the held messages are gone and the peer's next
        // message raises a new request (Steam keeps posting requests while a peer keeps sending).
        if (read_only || !value) return 0;
        std::lock_guard lock(g_mutex);
        if (!g_requested.erase(value)) return 0;
        g_pending.erase(value);
        std::erase(g_requests, value);
        return 1;
    }
    if (id == STANDIN_KNOB_INJECT_REQUEST) {
        if (read_only || !value) return 0;
        std::lock_guard lock(g_mutex);
        if (!g_accepted.contains(value) && g_requested.insert(value).second) {
            g_requests.push_back(value);
            bump(STANDIN_KNOB_SESSION_REQUESTS);
        }
        return 0;
    }
    return read_only ? knob(id) : g_knobs[id].exchange(value, std::memory_order_acq_rel);
}
