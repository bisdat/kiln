// SPDX-License-Identifier: MIT
// kiln.net's Steam peer-to-peer messages (kiln_net.h's Steam members;
// docs/kiln/services/net-steam-messages.md). Built into kiln-net.dll; the host tests build it alone with a fake export
// table (tests/steam_messages_test.cpp), the Wine and Windows tests with the stand-in steam_api64.dll.
#pragma once
#include <cstdint>

#include "services/kiln_net.h"

namespace knet::steam {

// Finds one export of the steam_api64.dll that is already loaded (kiln.net: GetProcAddress, at init only).
using LookupFn = void* (*)(const char* name, void* user);
using DialectOwnerFn = KilnOwner(KILN_CALL*)(void);
using LogFn = void (*)(int level, const char* text);

struct Config {
    bool server = false;                    // the GameServer interfaces (a dedicated server), else the user's
    LookupFn lookup = nullptr;              // nullptr: steam_api64.dll is not loaded in this process
    void* lookup_user = nullptr;
    DialectOwnerFn dialect_owner = nullptr; // kiln.internal.launch's dialect_owner; nullptr: no lease can be opened
    LogFn log = nullptr;                    // members only (the session callback never logs)
};

enum class Presence { NoModule, MissingExport, Complete };

// Once, on the boot thread, before the table is published: resolves every export the members use (never again, so no
// member calls GetProcAddress or GetModuleHandle). Calls no Steam function. *missing: the first export not found.
Presence init(const Config& config, const char** missing);

// The kiln.net members (kiln_net.h, steam_*).
KilnResult KILN_CALL st_status(KilnSteamMessagesStatus* out);
KilnResult KILN_CALL st_open(KilnOwner owner, const KilnSteamMessagesOpen* request, KilnHandle* lease);
KilnResult KILN_CALL st_close(KilnHandle lease);
KilnResult KILN_CALL st_maintain(KilnHandle lease, const KilnSteamAcceptPolicy* policy, KilnSteamMessagesStatus* out);
KilnResult KILN_CALL st_send(KilnHandle lease, uint64_t peer, const void* data, uint32_t len, uint32_t channel,
                             uint32_t flags, int32_t* steam_result);
KilnResult KILN_CALL st_receive(KilnHandle lease, uint32_t channel, void* buffer, uint32_t cap,
                                KilnSteamMessageInfo* out, uint32_t* got);
KilnResult KILN_CALL st_accept(KilnHandle lease, uint64_t peer, uint32_t* accepted);

// kiln.net's exit handler: from now on every member answers KILN_ERR_UNAVAILABLE and calls nothing, and the session
// callback answers no request. Waits at most wait_ms for member calls and a callback already past that check. Calls no
// Steam function (no UnregisterCallback: the game's SteamAPI_Shutdown may have run). False when the wait ran out.
bool close_for_exit(uint32_t wait_ms);

// True once a member found Steam ready here (the capability row's "available").
bool ready_seen();

// Tests only.
void reset_for_tests();
void* callback_object_for_tests();  // the static CCallbackBase-shaped object handed to SteamAPI_RegisterCallback

}  // namespace knet::steam
