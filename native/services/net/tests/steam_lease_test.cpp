// SPDX-License-Identifier: MIT
// kiln.net's Steam peer-to-peer messages (steam_messages.cpp, built with clang-cl) over the stand-in
// steam_api64.dll (tests/steam_standin), under Wine and on Windows:
//   steam-lease-test unit <port base>                      one process: real exports and the accessor fallback, the user
//                                                           handle first, faults (11), the callback through vtable slot 1
//                                                           and its offsets (12), the GameServer flag, release-once on a
//                                                           real message, exit
//   steam-lease-test fabric a|b <port base> <sync dir>     two processes: A (server, policy LISTED) refuses B's session
//                                                           until B is listed, then receives B's messages through the
//                                                           lease and replies; B receives the reply through its lease
// Exit code 0 = every check passed.
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../steam_messages.h"
#include "steam_standin/standin.h"

namespace st = knet::steam;

namespace {

int g_checks = 0, g_failures = 0;
const char* g_who = "lease";
#define CHECK(cond, ...)                                                   \
    do {                                                                   \
        ++g_checks;                                                        \
        if (!(cond)) {                                                     \
            ++g_failures;                                                  \
            std::printf("%s: FAIL line %d: %s: ", g_who, __LINE__, #cond); \
            std::printf(__VA_ARGS__);                                      \
            std::printf("\n");                                             \
            std::fflush(stdout);                                           \
        }                                                                  \
    } while (0)

HMODULE g_module{};
StandInSetIdentityFn set_identity;
StandInPumpFn pump;
StandInKnobFn knob_fn;
uint64_t knob(uint32_t k) { return knob_fn(k | STANDIN_KNOB_READ, 0); }
uint64_t take(uint32_t k) { return knob_fn(k, 0); }
uint64_t calls(StandInExport e) { return knob(uint32_t{STANDIN_KNOB_CALLS} + static_cast<uint32_t>(e)); }
uint64_t all_calls() {
    uint64_t n = 0;
    for (uint32_t e = 1; e < STANDIN_EXPORT_COUNT; ++e) n += knob(uint32_t{STANDIN_KNOB_CALLS} + e);
    return n;
}

void* lookup(const char* name, void* module) {
    return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(module), name));
}
void log(int, const char* text) { std::printf("%s: log: %s\n", g_who, text); }
constexpr KilnOwner kOwner = 42;
KilnOwner KILN_CALL owner() { return kOwner; }

bool load() {
    wchar_t path[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (!n || n >= MAX_PATH) return false;
    std::wstring dll(path, n);
    dll.resize(dll.find_last_of(L"\\/") + 1);
    dll += L"steam_api64.dll";  // only the stand-in next to this test
    g_module = LoadLibraryW(dll.c_str());
    if (!g_module) return false;
    set_identity = reinterpret_cast<StandInSetIdentityFn>(GetProcAddress(g_module, "StandIn_SetIdentity"));
    pump = reinterpret_cast<StandInPumpFn>(GetProcAddress(g_module, "StandIn_Pump"));
    knob_fn = reinterpret_cast<StandInKnobFn>(GetProcAddress(g_module, "StandIn_Knob"));
    return set_identity && pump && knob_fn && GetModuleHandleW(L"steam_api64.dll") == g_module;
}

void start(bool server) {
    st::reset_for_tests();
    st::Config c;
    c.server = server;
    // as kiln.net does: the loaded steam_api64.dll by name, looked up once
    HMODULE m = GetModuleHandleW(L"steam_api64.dll");
    c.lookup = m ? &lookup : nullptr;
    c.lookup_user = m;
    c.dialect_owner = &owner;
    c.owners_watched = true;  // as kiln.net does once the core's owner-retiring notification is subscribed
    c.log = &log;
    const char* missing = nullptr;
    CHECK(st::init(c, &missing) == st::Presence::Complete, "init: every export found (%s)", missing ? missing : "-");
}

KilnHandle open(uint32_t first, uint32_t last, bool sessions) {
    KilnSteamMessagesOpen r{sizeof r, sessions ? static_cast<uint32_t>(KILN_STEAM_OPEN_SESSIONS) : 0u, first, last};
    KilnHandle h = 0;
    return st::st_open(kOwner, &r, &h) == KILN_OK ? h : 0;
}
KilnResult maintain(KilnHandle lease, uint32_t mode, std::vector<uint64_t> ids = {}) {
    KilnSteamAcceptPolicy p{sizeof p, mode, ids.empty() ? nullptr : ids.data(), static_cast<uint32_t>(ids.size()), 0};
    KilnSteamMessagesStatus out{};
    out.size = sizeof out;
    return st::st_maintain(kOwner, lease, &p, &out);
}
KilnSteamMessagesStatus status(KilnResult* rc = nullptr) {
    KilnSteamMessagesStatus s{};
    s.size = sizeof s;
    const KilnResult r = st::st_status(&s);
    if (rc) *rc = r;
    return s;
}
uint8_t flags_at_8() { return static_cast<const uint8_t*>(st::callback_object_for_tests())[8]; }
int32_t id_at_12() {
    int32_t id;
    std::memcpy(&id, static_cast<const uint8_t*>(st::callback_object_for_tests()) + 12, sizeof id);
    return id;
}

std::vector<uint8_t> pattern(size_t n, uint8_t seed) {
    std::vector<uint8_t> v(n);
    for (size_t i = 0; i < n; ++i) v[i] = static_cast<uint8_t>(seed + i * 13 + (i >> 8));
    return v;
}

// Polls the lease for one message on a channel (the fabric is asynchronous).
KilnResult receive_wait(KilnHandle lease, uint32_t channel, std::vector<uint8_t>& buf, KilnSteamMessageInfo& info,
                        uint32_t& got, DWORD timeout_ms) {
    const ULONGLONG until = GetTickCount64() + timeout_ms;
    KilnResult rc;
    do {
        info.size = sizeof info;
        rc = st::st_receive(kOwner, lease, channel, buf.data(), static_cast<uint32_t>(buf.size()), &info, &got);
        if (rc != KILN_OK || got) return rc;
        Sleep(5);
    } while (GetTickCount64() < until);
    return rc;
}

constexpr uint64_t kClient = 76561198000000301ull, kServer = 90000000000000302ull;
constexpr uint64_t kA = 76561198000000401ull, kB = 76561198000000502ull;

int unit(uint32_t base) {
    g_who = "lease unit";
    start(false);
    const KilnHandle lease = open(0, 100, true);
    CHECK(lease != 0, "a session lease");
    // 2: before Steam is ready only the user handle is asked
    knob_fn(STANDIN_KNOB_INIT_DELAY_PUMPS, 1);
    CHECK(set_identity(kClient, base, 0) == 1, "client identity");
    take(STANDIN_KNOB_CALLS_BEFORE_READY);
    KilnSteamMessagesStatus s = status();
    int32_t er = -1;
    CHECK(!(s.flags & KILN_STEAM_MSG_AVAILABLE) && st::st_send(kOwner, lease, kClient, "x", 1, 1, 8, &er) == KILN_ERR_UNAVAILABLE &&
              maintain(lease, KILN_STEAM_ACCEPT_ANY) == KILN_OK,
          "not ready: unavailable, maintain changes nothing");
    CHECK(knob(STANDIN_KNOB_CALLS_BEFORE_READY) == 0 && calls(STANDIN_EXPORT_REGISTER) == 0,
          "no export but the user handle before ready (%llu)", (unsigned long long)knob(STANDIN_KNOB_CALLS_BEFORE_READY));
    pump();
    s = status();
    CHECK((s.flags & KILN_STEAM_MSG_AVAILABLE) && (s.flags & KILN_STEAM_MSG_IDENTITY) && s.local_steam_id == kClient &&
              !(s.flags & KILN_STEAM_MSG_SERVER_IFACE),
          "ready: available, local id through the v013 sockets interface (%x)", s.flags);
    CHECK(calls(STANDIN_EXPORT_USER_SOCKETS_V013) >= 1 && calls(STANDIN_EXPORT_FIND_USER) == 0, "the v013 accessor answered");
    knob_fn(STANDIN_KNOB_ACCESSOR_NULL, 1);
    s = status();
    CHECK((s.flags & KILN_STEAM_MSG_AVAILABLE) && s.local_steam_id == kClient && calls(STANDIN_EXPORT_FIND_USER) == 2,
          "accessors null: FindOrCreate for messages 002 and sockets 013");
    knob_fn(STANDIN_KNOB_ACCESSOR_NULL, 0);
    CHECK(knob(STANDIN_KNOB_BAD_SELF) == 0, "every interface pointer was the role's");

    // 12: registration, then Run(void*) reached through vtable slot 1 by the stand-in's pump
    CHECK(flags_at_8() == 0 && id_at_12() == 1251, "before registration: flags 0, id 1251");
    CHECK(maintain(lease, KILN_STEAM_ACCEPT_ANY) == KILN_OK && knob(STANDIN_KNOB_REGISTERED) == 1 &&
              calls(STANDIN_EXPORT_REGISTER) == 1,
          "the first ANY registers the callback");
    CHECK(flags_at_8() == 0x01 && id_at_12() == 1251, "client: registered flag @8 (no 0x02), id 1251 @12 (%x)", flags_at_8());
    constexpr uint64_t kPeer = 76561198000009001ull;
    knob_fn(STANDIN_KNOB_INJECT_REQUEST, kPeer);
    take(STANDIN_KNOB_CALLBACKS_RUN);
    CHECK(pump() == 1 && knob(STANDIN_KNOB_CALLBACKS_RUN) == 1, "the pump ran our callback once");
    s = status();
    CHECK(s.sessions_accepted == 1 && calls(STANDIN_EXPORT_ACCEPT) == 1, "slot 1 is Run(void*): it accepted");
    CHECK(knob_fn(STANDIN_KNOB_EXPIRE_SESSION, kPeer) == 0, "the session is accepted (nothing to expire)");
    CHECK(maintain(lease, KILN_STEAM_ACCEPT_NONE) == KILN_OK, "NONE");
    knob_fn(STANDIN_KNOB_INJECT_REQUEST, kPeer + 1);
    pump();
    CHECK(calls(STANDIN_EXPORT_ACCEPT) == 1 && knob_fn(STANDIN_KNOB_EXPIRE_SESSION, kPeer + 1) == 1,
          "NONE: the request stays unanswered (it times out)");
    CHECK(maintain(lease, KILN_STEAM_ACCEPT_LISTED, {kPeer + 2}) == KILN_OK && calls(STANDIN_EXPORT_REGISTER) == 1 &&
              calls(STANDIN_EXPORT_UNREGISTER) == 0,
          "still one registration, never an unregister");

    // 7 on a real message: self loop, release once; too big for the buffer: dropped, released once
    take(STANDIN_KNOB_RELEASES);
    const auto big = pattern(524288, 3);
    CHECK(st::st_send(kOwner, lease, kClient, big.data(), 524288, 7, 40, &er) == KILN_OK && er == 1, "512 KiB to self");
    CHECK(st::st_send(kOwner, lease, kClient, "small", 5, 7, 32, &er) == KILN_OK && er == 1, "an unreliable one");
    std::vector<uint8_t> buf(524288);
    KilnSteamMessageInfo info{};
    uint32_t got = 0;
    CHECK(receive_wait(lease, 7, buf, info, got, 5000) == KILN_OK && got == 1 && info.length == 524288 &&
              !std::memcmp(buf.data(), big.data(), big.size()) && info.flags == KILN_STEAM_SEND_RELIABLE &&
              info.peer_steam_id == kClient,
          "512 KiB back, reliable");
    std::vector<uint8_t> tiny(4);
    CHECK(receive_wait(lease, 7, tiny, info, got, 5000) == KILN_STEAM_ERR_INVALID_DATA && got == 0,
          "5 bytes into a 4-byte buffer: invalid data");
    CHECK(knob(STANDIN_KNOB_RELEASES) == 2 && knob(STANDIN_KNOB_DOUBLE_RELEASES) == 0 && knob(STANDIN_KNOB_LIVE_MESSAGES) == 0,
          "both released exactly once");
    CHECK(st::st_send(kOwner, lease, kClient, big.data(), 524288, 101, 8, &er) == KILN_ERR_INVALID_ARGUMENT, "channel outside");

    // 11: a faulting Steam export answers internal, and works again after
    struct Fault {
        StandInExport e;
        const char* what;
    };
    for (const Fault f : {Fault{STANDIN_EXPORT_SEND, "send"}, Fault{STANDIN_EXPORT_USER_HANDLE, "user handle"},
                          Fault{STANDIN_EXPORT_USER_MESSAGES_V002, "messages accessor"}}) {
        knob_fn(STANDIN_KNOB_FAULT_EXPORT, f.e);
        CHECK(st::st_send(kOwner, lease, kClient, "x", 1, 8, 8, &er) == KILN_ERR_INTERNAL, "fault in %s: send internal", f.what);
        knob_fn(STANDIN_KNOB_FAULT_EXPORT, 0);
    }
    KilnResult rc;
    knob_fn(STANDIN_KNOB_FAULT_EXPORT, STANDIN_EXPORT_GET_IDENTITY);
    status(&rc);
    CHECK(rc == KILN_ERR_INTERNAL, "fault in GetIdentity: status internal");
    knob_fn(STANDIN_KNOB_FAULT_EXPORT, STANDIN_EXPORT_ACCEPT);
    uint32_t acc = 9;
    CHECK(st::st_accept(kOwner, lease, kPeer, &acc) == KILN_ERR_INTERNAL, "fault in accept: internal");
    knob_fn(STANDIN_KNOB_FAULT_EXPORT, STANDIN_EXPORT_RECEIVE);
    CHECK(st::st_receive(kOwner, lease, 7, buf.data(), 16, &info, &got) == KILN_ERR_INTERNAL && got == 0, "fault in receive: internal");
    knob_fn(STANDIN_KNOB_FAULT_EXPORT, STANDIN_EXPORT_RELEASE);
    CHECK(st::st_send(kOwner, lease, kClient, "r", 1, 9, 8, &er) == KILN_OK, "a message to fault on release");
    Sleep(300);
    info.size = sizeof info;
    CHECK(st::st_receive(kOwner, lease, 9, buf.data(), 16, &info, &got) == KILN_ERR_INTERNAL && got == 0, "fault in release: internal");
    knob_fn(STANDIN_KNOB_FAULT_EXPORT, 0);
    CHECK(st::st_send(kOwner, lease, kClient, "ok", 2, 8, 8, &er) == KILN_OK && er == 1, "after the faults it works");
    std::vector<uint8_t> small(16);
    CHECK(receive_wait(lease, 8, small, info, got, 5000) == KILN_OK && got == 1 && info.length == 2, "and receives");
    // 11 inside the session callback, on the pumping thread (the stand-in's pump catches nothing): a fault in a Steam
    // call there is caught, the request is counted refused and stays unanswered, and the next one is answered again
    CHECK(maintain(lease, KILN_STEAM_ACCEPT_ANY) == KILN_OK, "ANY for the callback's faults");
    uint64_t requester = kPeer + 20;
    for (const Fault f : {Fault{STANDIN_EXPORT_ACCEPT, "AcceptSessionWithUser"}, Fault{STANDIN_EXPORT_USER_HANDLE, "user handle"},
                          Fault{STANDIN_EXPORT_USER_MESSAGES_V002, "messages accessor"}}) {
        const KilnSteamMessagesStatus was = status();
        knob_fn(STANDIN_KNOB_INJECT_REQUEST, requester);
        knob_fn(STANDIN_KNOB_FAULT_EXPORT, f.e);
        const int32_t runs = pump();
        knob_fn(STANDIN_KNOB_FAULT_EXPORT, 0);
        s = status();
        CHECK(runs == 1 && s.sessions_refused == was.sessions_refused + 1 && s.sessions_accepted == was.sessions_accepted &&
                  knob_fn(STANDIN_KNOB_EXPIRE_SESSION, requester) == 1,
              "fault in %s inside the callback: caught, refused, unanswered", f.what);
        ++requester;
    }
    knob_fn(STANDIN_KNOB_INJECT_REQUEST, requester);
    pump();
    CHECK(knob_fn(STANDIN_KNOB_EXPIRE_SESSION, requester) == 0, "after the callback's faults it accepts again");

    // the server role: GameServer interfaces, flag 0x02 set before RegisterCallback, so the server's pump runs it
    start(true);
    CHECK(set_identity(kServer, base, STANDIN_ROLE_SERVER) == 1, "server identity");
    const KilnHandle sl = open(0, 10, true);
    s = status();
    CHECK((s.flags & KILN_STEAM_MSG_SERVER_IFACE) && (s.flags & KILN_STEAM_MSG_AVAILABLE) && s.local_steam_id == kServer,
          "server: GameServer interfaces, its id (%x)", s.flags);
    const uint64_t registered_before = calls(STANDIN_EXPORT_REGISTER);  // a fresh module, the stand-in still has the old one
    CHECK(maintain(sl, KILN_STEAM_ACCEPT_LISTED, {kPeer + 10}) == KILN_OK && calls(STANDIN_EXPORT_REGISTER) == registered_before + 1,
          "server registration");
    CHECK(flags_at_8() == 0x03, "server: 0x02 beside the registered flag (%x)", flags_at_8());
    const uint64_t accepts = calls(STANDIN_EXPORT_ACCEPT);
    knob_fn(STANDIN_KNOB_INJECT_REQUEST, kPeer + 10);
    knob_fn(STANDIN_KNOB_INJECT_REQUEST, kPeer + 11);
    pump();
    CHECK(calls(STANDIN_EXPORT_ACCEPT) == accepts + 1 && knob_fn(STANDIN_KNOB_EXPIRE_SESSION, kPeer + 10) == 0 &&
              knob_fn(STANDIN_KNOB_EXPIRE_SESSION, kPeer + 11) == 1,
          "the server's pump ran it: the listed peer accepted, the other left pending");

    // 10: exit
    CHECK(st::close_for_exit(1000), "closed for exit");
    const uint64_t before = all_calls();
    status(&rc);
    CHECK(rc == KILN_ERR_UNAVAILABLE && st::st_send(kOwner, sl, kPeer, "x", 1, 1, 8, &er) == KILN_ERR_UNAVAILABLE &&
              maintain(sl, KILN_STEAM_ACCEPT_ANY) == KILN_ERR_UNAVAILABLE,
          "after exit: unavailable");
    knob_fn(STANDIN_KNOB_INJECT_REQUEST, kPeer + 12);
    pump();  // the stand-in's pump still runs the registered callback: it answers nothing
    CHECK(all_calls() == before && calls(STANDIN_EXPORT_UNREGISTER) == 0,
          "no Steam export called after exit, no unregister (%llu then %llu)", (unsigned long long)before,
          (unsigned long long)all_calls());
    CHECK(knob_fn(STANDIN_KNOB_EXPIRE_SESSION, kPeer + 12) == 1, "the request after exit stays unanswered");
    std::printf("%s: %s %d checks\n", g_who, g_failures ? "FAIL" : "PASS", g_checks);
    return g_failures ? 1 : 0;
}

bool touch(const std::wstring& p) {
    const HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);
    return true;
}
bool wait_file(const std::wstring& p, DWORD ms) {
    const ULONGLONG until = GetTickCount64() + ms;
    while (GetFileAttributesW(p.c_str()) == INVALID_FILE_ATTRIBUTES) {
        if (GetTickCount64() >= until) return false;
        Sleep(20);
    }
    return true;
}

int fabric_a(uint32_t base, const std::wstring& sync) {
    g_who = "lease fabric A";
    start(true);
    CHECK(set_identity(kA, base, STANDIN_ROLE_SERVER) == 1, "A binds (server)");
    const KilnHandle lease = open(0, 100, true);
    CHECK(maintain(lease, KILN_STEAM_ACCEPT_LISTED, {kA + 7}) == KILN_OK, "A lists someone else");
    CHECK(touch(sync + L"\\a.ready") && wait_file(sync + L"\\b.ready", 20000), "both ready");
    // B's first message: its request is refused by the policy, its messages stay held and unreadable
    ULONGLONG until = GetTickCount64() + 15000;
    while (knob(STANDIN_KNOB_PENDING_MESSAGES) < 2 && GetTickCount64() < until) {
        pump();
        Sleep(10);
    }
    pump();
    KilnSteamMessagesStatus s = status();
    CHECK(s.sessions_refused >= 1 && s.sessions_accepted == 0, "B refused (refused %llu)", (unsigned long long)s.sessions_refused);
    std::vector<uint8_t> buf(524288);
    KilnSteamMessageInfo info{};
    uint32_t got = 9;
    info.size = sizeof info;
    CHECK(st::st_receive(kOwner, lease, 5, buf.data(), 16, &info, &got) == KILN_OK && got == 0, "nothing readable from B");
    CHECK(knob_fn(STANDIN_KNOB_EXPIRE_SESSION, kB) == 1, "B's unanswered session times out");
    // now B is listed: its next message's request is accepted by the callback
    CHECK(maintain(lease, KILN_STEAM_ACCEPT_LISTED, {kA + 7, kB}) == KILN_OK, "A lists B");
    CHECK(touch(sync + L"\\a.listed"), "a.listed");
    until = GetTickCount64() + 15000;
    while (status().sessions_accepted == 0 && GetTickCount64() < until) {
        pump();
        Sleep(10);
    }
    CHECK(status().sessions_accepted == 1, "the callback accepted B");
    CHECK(receive_wait(lease, 5, buf, info, got, 5000) == KILN_OK && got == 1 && info.length == 5 &&
              !std::memcmp(buf.data(), "b2a-1", 5) && info.peer_steam_id == kB && info.flags == KILN_STEAM_SEND_RELIABLE,
          "b2a-1, reliable, from B");
    CHECK(receive_wait(lease, 5, buf, info, got, 5000) == KILN_OK && got == 1 && !std::memcmp(buf.data(), "b2a-2", 5) &&
              info.flags == 0,
          "b2a-2, unreliable");
    const auto big = pattern(524288, 9);
    CHECK(receive_wait(lease, 6, buf, info, got, 5000) == KILN_OK && got == 1 && info.length == 524288 &&
              !std::memcmp(buf.data(), big.data(), big.size()),
          "512 KiB intact");
    CHECK(knob(STANDIN_KNOB_LIVE_MESSAGES) == 0 && knob(STANDIN_KNOB_DOUBLE_RELEASES) == 0, "each released once");
    int32_t er = -1;
    CHECK(st::st_send(kOwner, lease, kB, "a2b-1", 5, 9, 40, &er) == KILN_OK && er == 1, "A replies");
    CHECK(wait_file(sync + L"\\b.done", 20000), "B finished");
    std::printf("%s: %s %d checks\n", g_who, g_failures ? "FAIL" : "PASS", g_checks);
    return g_failures ? 1 : 0;
}

int fabric_b(uint32_t base, const std::wstring& sync) {
    g_who = "lease fabric B";
    start(false);
    CHECK(set_identity(kB, base, 0) == 1, "B binds (client)");
    const KilnHandle lease = open(0, 100, true);
    CHECK(maintain(lease, KILN_STEAM_ACCEPT_ANY) == KILN_OK, "B accepts anyone");
    CHECK(wait_file(sync + L"\\a.ready", 20000) && touch(sync + L"\\b.ready"), "both ready");
    int32_t er = -1;
    CHECK(st::st_send(kOwner, lease, kA, "first", 5, 5, 40, &er) == KILN_OK && er == 1, "B's first (refused by A)");
    CHECK(st::st_send(kOwner, lease, kA, "first", 5, 5, 40, &er) == KILN_OK && er == 1, "and another");
    CHECK(wait_file(sync + L"\\a.listed", 30000), "A listed B");
    const auto big = pattern(524288, 9);
    CHECK(st::st_send(kOwner, lease, kA, "b2a-1", 5, 5, 40, &er) == KILN_OK && er == 1, "b2a-1");
    CHECK(st::st_send(kOwner, lease, kA, "b2a-2", 5, 5, 32, &er) == KILN_OK && er == 1, "b2a-2");
    CHECK(st::st_send(kOwner, lease, kA, big.data(), 524288, 6, 40, &er) == KILN_OK && er == 1, "512 KiB");
    std::vector<uint8_t> buf(64);
    KilnSteamMessageInfo info{};
    uint32_t got = 0;
    const ULONGLONG until = GetTickCount64() + 20000;
    KilnResult rc = KILN_OK;
    while (!got && GetTickCount64() < until) {
        pump();
        rc = receive_wait(lease, 9, buf, info, got, 20);
    }
    CHECK(rc == KILN_OK && got == 1 && info.length == 5 && !std::memcmp(buf.data(), "a2b-1", 5) && info.peer_steam_id == kA,
          "B receives A's reply through its lease");
    CHECK(status().sessions_accepted == 0, "no session request on B: its own send opened the session");
    touch(sync + L"\\b.done");
    std::printf("%s: %s %d checks\n", g_who, g_failures ? "FAIL" : "PASS", g_checks);
    return g_failures ? 1 : 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (!load()) {
        std::printf("lease: cannot load the stand-in steam_api64.dll next to the test\n");
        return 2;
    }
    if (argc == 3 && !std::wcscmp(argv[1], L"unit")) return unit(static_cast<uint32_t>(_wtoi(argv[2])));
    if (argc == 5 && !std::wcscmp(argv[1], L"fabric")) {
        const auto base = static_cast<uint32_t>(_wtoi(argv[3]));
        if (!std::wcscmp(argv[2], L"a")) return fabric_a(base, argv[4]);
        if (!std::wcscmp(argv[2], L"b")) return fabric_b(base, argv[4]);
    }
    std::printf("usage: steam-lease-test unit <port base> | fabric a|b <port base> <sync dir>\n");
    return 2;
}
