/* SPDX-License-Identifier: MIT
 * test infrastructure only. Never packaged, never installed: it builds only into
 * <build>/cmake/bin/tests/steam-standin/ and only when asked for by name (EXCLUDE_FROM_ALL).
 *
 * The stand-in steam_api64.dll: the flat Steam exports Kiln's Steam-messages lease and the ShroudForge transport call,
 * served by a loopback-UDP fabric between test processes instead of Steam. Each process has one identity (a SteamID
 * and a role) and one UDP port on 127.0.0.1, port = fabric base + (SteamID % 1024).
 *
 * Steam behaviour it keeps:
 *  - a message from a peer whose session this process has not accepted raises one session request (callback 1251,
 *    dispatched only by StandIn_Pump, the stand-in for the game's SteamAPI_RunCallbacks) and is held until
 *    AcceptSessionWithUser; sending to a peer accepts its session, as in Steam. STANDIN_KNOB_EXPIRE_SESSION is Steam's
 *    timeout of an unaccepted session: the held messages are dropped and the peer's next message raises a new request;
 *  - messages over 512 KiB are refused with k_EResultInvalidParam (8), as Steam refuses a message that is too big;
 *  - received messages use SteamNetworkingMessage_t's layout (peer identity @16, channel @192, flags @196, release
 *    function @184), keep only the reliable bit (8) of the send flags, and must be released once; StandIn_Knob
 *    counts releases and double releases;
 *  - callbacks are CCallbackBase objects (vptr @0, flags @8, id @12) and the pump calls Run(void*) through vtable slot
 *    1, where MSVC (and clang-cl) put it (overloads in reverse order of declaration); the GameServer flag 0x02 picks
 *    which role's pump runs it, read at every dispatch (a flag set or cleared after RegisterCallback counts from the
 *    next dispatch). RegisterCallback sets flag 0x01 and the id, UnregisterCallback clears 0x01.
 *    The game's steam_api64.dll (sha256 e6d9bafb..., client 26430625 and server 26430253, read statically) does the
 *    same: RegisterCallback ORs 0x01 into @8 and writes the id @12; both pumps compare (flags >> 1) & 1 with their role
 *    for each callback and call [vtable + 8] with (this, parameter); RegisterCallback and UnregisterCallback change the
 *    callback map without a lock, so a registration from another thread races the game's pump in Steam itself.
 *  - the user handle and the interfaces exist only for the process's role (client: SteamAPI_*, server: GameServer)
 *    and only once Steam is "ready" (an identity is set and the init delay has passed);
 *  - the sockets accessors are the v013 ones, as in the game's steam_api64.dll, which has no v012 accessor (its only
 *    ISteamNetworkingSockets interface is SteamNetworkingSockets013, and its flat GetIdentity is built for that
 *    vtable). SteamInternal_FindOrCreate*Interface answers "SteamNetworkingSockets013", and "SteamNetworkingSockets012"
 *    too for ShroudForge's own fallback.
 *
 * Steam behaviour it does NOT model, so no test may rely on its absence: repeated session requests on a timer (only
 * the expire knob raises a new one); k_EResultLimitExceeded from a full send buffer (Steam's default is 512 KiB
 * queued per connection, so a 512 KiB message right after other sends to a peer not yet connected can be refused;
 * inject it with STANDIN_KNOB_SEND_ERESULT); unreliable messages lost, duplicated or reordered (the fabric delivers
 * every message once, in order, unless dropped by the knob); SteamNetworkingMessagesSessionFailed_t; the flat
 * accessors keeping a null first answer for the session (the accessor-null knob only switches them off); whether
 * Steam accepts a message to the local identity (the stand-in loops it back); whether Steam still serves the older
 * "SteamNetworkingSockets012" through SteamInternal_FindOrCreate*Interface (the stand-in does).
 *
 * Layouts follow ShroudForge's steam_network.cpp (MIT, Copyright (c) 2026 bonsaibauer) and Steamworks' public headers.
 */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* StandIn_SetIdentity flags */
#define STANDIN_ROLE_SERVER 1u

/* Export ids, for STANDIN_KNOB_FAULT_EXPORT and STANDIN_KNOB_CALLS + id. */
enum StandInExport {
    STANDIN_EXPORT_USER_MESSAGES_V002 = 1,     /* SteamAPI_SteamNetworkingMessages_SteamAPI_v002 */
    STANDIN_EXPORT_SERVER_MESSAGES_V002 = 2,   /* SteamAPI_SteamGameServerNetworkingMessages_SteamAPI_v002 */
    STANDIN_EXPORT_USER_SOCKETS_V013 = 3,      /* SteamAPI_SteamNetworkingSockets_SteamAPI_v013 */
    STANDIN_EXPORT_SERVER_SOCKETS_V013 = 4,    /* SteamAPI_SteamGameServerNetworkingSockets_SteamAPI_v013 */
    STANDIN_EXPORT_SEND = 5,                   /* SteamAPI_ISteamNetworkingMessages_SendMessageToUser */
    STANDIN_EXPORT_RECEIVE = 6,                /* SteamAPI_ISteamNetworkingMessages_ReceiveMessagesOnChannel */
    STANDIN_EXPORT_ACCEPT = 7,                 /* SteamAPI_ISteamNetworkingMessages_AcceptSessionWithUser */
    STANDIN_EXPORT_RELEASE = 8,                /* SteamAPI_SteamNetworkingMessage_t_Release */
    STANDIN_EXPORT_GET_IDENTITY = 9,           /* SteamAPI_ISteamNetworkingSockets_GetIdentity */
    STANDIN_EXPORT_FIND_USER = 10,             /* SteamInternal_FindOrCreateUserInterface */
    STANDIN_EXPORT_FIND_SERVER = 11,           /* SteamInternal_FindOrCreateGameServerInterface */
    STANDIN_EXPORT_USER_HANDLE = 12,           /* SteamAPI_GetHSteamUser */
    STANDIN_EXPORT_SERVER_HANDLE = 13,         /* SteamGameServer_GetHSteamUser */
    STANDIN_EXPORT_REGISTER = 14,              /* SteamAPI_RegisterCallback */
    STANDIN_EXPORT_UNREGISTER = 15,            /* SteamAPI_UnregisterCallback */
    STANDIN_EXPORT_COUNT = 16
};

/* StandIn_Knob(knob, value) sets the knob and returns its previous value. With STANDIN_KNOB_READ or'ed into knob it
 * only reads. Counters (16 and up) are read and reset by writing 0; read-only ones ignore writes. */
#define STANDIN_KNOB_READ 0x80000000u
enum StandInKnob {
    STANDIN_KNOB_INIT_DELAY_PUMPS = 1,    /* Steam stays "not ready" for this many more StandIn_Pump calls */
    STANDIN_KNOB_ACCESSOR_NULL = 2,       /* 1: the four flat accessors (v002/v012) return null */
    STANDIN_KNOB_SEND_ERESULT = 3,        /* nonzero: SendMessageToUser returns it and sends nothing */
    STANDIN_KNOB_FAULT_EXPORT = 4,        /* a StandInExport id: that export raises an access violation */
    STANDIN_KNOB_DROP_SENDS = 5,          /* the next N sends answer k_EResultOK and are lost */
    STANDIN_KNOB_INJECT_REQUEST = 6,      /* write a SteamID: a session request from it, as if it had sent a message */
    STANDIN_KNOB_EXPIRE_SESSION = 7,      /* write a SteamID: its unaccepted session times out (returns 1 if it had one) */
    STANDIN_KNOB_RELEASES = 16,           /* messages released (first release of each) */
    STANDIN_KNOB_DOUBLE_RELEASES = 17,    /* releases of a pointer that is not a live message */
    STANDIN_KNOB_LIVE_MESSAGES = 18,      /* read-only: messages handed out and not yet released */
    STANDIN_KNOB_CALLS_BEFORE_READY = 19, /* calls of any export but the two user-handle ones while not ready */
    STANDIN_KNOB_SESSION_REQUESTS = 20,   /* session requests raised */
    STANDIN_KNOB_PENDING_MESSAGES = 21,   /* read-only: messages held for peers whose session is not accepted */
    STANDIN_KNOB_CALLBACKS_RUN = 22,      /* Run(void*) calls the pump made */
    STANDIN_KNOB_BAD_SELF = 23,           /* calls with an interface pointer that is not this role's (or none) */
    STANDIN_KNOB_WIRE_REJECTS = 24,       /* datagrams dropped as malformed */
    STANDIN_KNOB_REGISTERED = 25,         /* read-only: callbacks registered now */
    STANDIN_KNOB_SEND_FLAGS_SEEN = 26,    /* bit (flags & 63) of every SendMessageToUser call past its parameter checks */
    STANDIN_KNOB_CALLS = 256              /* + StandInExport id: calls of that export */
};

/* Sets this process's identity and binds its fabric port (127.0.0.1, port_base + steam_id % 1024). Returns 1, or 0
 * for a zero id, a base outside 1024..64511 or a port that cannot be bound. Setting it again rebinds and clears the
 * sessions and queues. Without a call, the first export call reads STEAM_STANDIN_ID, STEAM_STANDIN_PORT_BASE and
 * STEAM_STANDIN_ROLE=server from the environment, for test processes that load an unmodified Steam client. */
typedef int32_t (*StandInSetIdentityFn)(uint64_t steam_id, uint32_t port_base, uint32_t flags);
/* The game's callback pump: runs Run(void*) of every matching registered callback once per pending session request,
 * counts one pump for the init delay, and returns the number of Run calls. Never call it from a callback. */
typedef int32_t (*StandInPumpFn)(void);
typedef uint64_t (*StandInKnobFn)(uint32_t knob, uint64_t value);

#ifdef __cplusplus
}
#endif
