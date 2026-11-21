<!-- SPDX-License-Identifier: MIT -->
# kiln.net Steam peer-to-peer messages

Seven kiln.net members that move Steam networking messages between game processes, separate from the game connection and
from game channel 12, in `native/core/abi/services/kiln_net.h`.

**In this release they are Kiln-internal: kiln mode only.** A lease opens only for the owner the core designates, Kiln's
interpreter for ShroudForge mods (`kiln/plugins/kiln-sfi.dll`) in a start that runs it (see
[launch.md](launch.md#the-shroudforge-dialect-owner-kilninternallaunch)); for any other owner `steam_open` answers
`KILN_ERR_UNAVAILABLE`. The SDK's `kiln_net.h` leaves the members out (they sit in its `kiln-internal` blocks), so a mod
built on the SDK sees the 128-byte kiln.net table. Interface resolution and the session-request callback are adapted
from ShroudForge by bonsaibauer, MIT.

## Members

`KilnNetApi` grows from 128 to 184 bytes; a caller checks `header.size >= 184` (the table version stays 1). A 128-byte
table (an older Kiln) has none of them. Code: `native/services/net/steam_messages.{h,cpp}`; the table and
the exit hook in `net.cpp`.

| Offset | Member | Answer |
|---:|---|---|
| 128 | `steam_status(KilnSteamMessagesStatus*)` | a snapshot; `KILN_OK` also when Steam is not ready |
| 136 | `steam_open(owner, const KilnSteamMessagesOpen*, KilnHandle* lease)` | a lease on an inclusive channel range |
| 144 | `steam_close(lease)` | idempotent |
| 152 | `steam_maintain(lease, const KilnSteamAcceptPolicy*, KilnSteamMessagesStatus* out)` | the session lease's accept policy |
| 160 | `steam_send(lease, peer, data, len, channel, flags, int32_t* steam_result)` | one message; Steam's EResult |
| 168 | `steam_receive(lease, channel, buffer, cap, KilnSteamMessageInfo*, uint32_t* got)` | at most one message, taken off Steam's queue |
| 176 | `steam_accept(lease, peer, uint32_t* accepted)` | a pending session accepted by hand |

Records: `KilnSteamMessagesStatus` 48 B, `KilnSteamMessagesOpen` 16 B, `KilnSteamAcceptPolicy` 24 B,
`KilnSteamMessageInfo` 24 B; `size` first, set by the caller. Every member may be called from any thread and none waits:
each makes at most one Steam call that moves data, or answers at once. Nothing is buffered: a message stays in Steam's
per-channel queue until someone receives it.

### Status flags

| Flag | Meaning |
|---|---|
| `AVAILABLE` | the role's user handle is nonzero and the messages interface resolves |
| `SERVER_IFACE` | the GameServer interfaces are used (a dedicated server), else the user's |
| `IDENTITY` | `local_steam_id` holds this process's Steam id (`GetIdentity` on the sockets interface) |
| `REGISTERED` | session requests are answered: the callback is registered and the policy's mode is not `NONE` |
| `LEASED` | a session lease is open |
| `CLOSED_FOR_EXIT` | the process is exiting: every member answers `KILN_ERR_UNAVAILABLE` |

`sessions_accepted` / `sessions_refused` count the callback's answers since boot; `accept_mode` and `listed` are the
policy in force.

### Order of refusals

- **open:** `KILN_ERR_UNAVAILABLE` (exit; owner 0 or not the owner `kiln.internal.launch` designates, logged once as
  `steam-messages-owner`), then `KILN_ERR_INVALID_ARGUMENT` (no record, a short record, unknown flags, first > last, a
  channel above 65535, an overlap with an open lease, a second `KILN_STEAM_OPEN_SESSIONS` lease, all 16 leases taken).
  open calls no Steam function.
- **maintain:** exit, then `KILN_ERR_NOT_FOUND` (no such open lease), `KILN_ERR_INVALID_ARGUMENT` (not the session lease;
  a short record, an unknown mode, more than 256 ids, an id 0, ids NULL: nothing changes). Steam not ready: `KILN_OK`,
  `*out` without `AVAILABLE`; a `NONE` policy is in force at once (it calls no Steam function and only narrows, as
  closing the session lease does), any other policy changes nothing until Steam is ready.
- **send:** exit, then `KILN_ERR_INVALID_ARGUMENT` before any Steam call (no result pointer, peer 0, more than 524,288
  bytes, bytes without data, flags other than 0, 8, 32 or 40), `KILN_ERR_NOT_FOUND` (no such lease),
  `KILN_ERR_INVALID_ARGUMENT` (a channel outside the lease), `KILN_ERR_UNAVAILABLE` (Steam not ready). Otherwise
  `KILN_OK` with `*steam_result` = Steam's EResult (1 = OK).
- **receive:** as send for the lease and channel. Steam answers 0 messages: `*got = 0`. One: copied, released,
  `*got = 1`, `flags` = `KILN_STEAM_SEND_RELIABLE` when Steam's reliable bit (8) is set. Bigger than `cap`, a negative
  size, bytes without data, or a sender that is not a nonzero SteamID: released, `KILN_STEAM_ERR_INVALID_DATA` (210).
  Any other count: released when there is a message, `KILN_ERR_INTERNAL`. The message is released exactly once on every
  path.
- **accept:** exit, `KILN_ERR_INVALID_ARGUMENT` (peer 0, no result pointer), `KILN_ERR_NOT_FOUND`, `KILN_ERR_UNAVAILABLE`.
- **Every member:** a Steam call that faults (SEH) answers `KILN_ERR_INTERNAL`, with one log line per kind.

## Steam

- **Exports** are looked up once, when kiln.net starts, in the `steam_api64.dll` both game executables import: by side,
  `SteamAPI_GetHSteamUser` / `SteamGameServer_GetHSteamUser`, the messages accessor v002, the sockets accessor v013, and
  `SteamInternal_FindOrCreateUserInterface` / `...GameServerInterface`; then `SendMessageToUser`,
  `ReceiveMessagesOnChannel`, `AcceptSessionWithUser`, `SteamNetworkingMessage_t_Release`, `GetIdentity` and
  `SteamAPI_RegisterCallback`. No member calls `GetModuleHandle` or `GetProcAddress`. kiln-net.dll does not import
  steam_api64.dll.
- **The game's DLL** (client 26430625 and server 26430253, sha256 e6d9bafb...) has every one of these. Its sockets
  interface is `SteamNetworkingSockets013` only (no v012 accessor), and its flat `GetIdentity` is built for that vtable,
  so the lease asks for 013.
- **Per call:** the user handle first; while it is 0 no other Steam function is called. Then the accessor, else
  FindOrCreate with the same version string. The interface is asked again on every call and a null answer is never kept.
- **Not used:** `SteamAPI_Init`, `Shutdown`, `RunCallbacks`, manual dispatch, `UnregisterCallback`. The game pumps
  callbacks.

## The session callback

- **Registration:** callback 1251 (`SteamNetworkingMessagesSessionRequest_t`) is registered exactly once per process, by
  the first maintain whose mode is not `NONE`. Its flag is set before the call, so no second maintain registers again.
  On a server the GameServer flag 0x02 is set on the object before `RegisterCallback`, because steam_api64 compares it
  with the pump's role at every dispatch.
- **Never unregistered.** `NONE`, closing the session lease and exit are answered inside the callback: it answers
  nothing, Steam times the request out, and the peer sees what it would see with no callback.
  - Why: steam_api64's `RegisterCallback` and `UnregisterCallback` change its callback map without a lock while the
    game's pump may walk it. One registration keeps that race to one call per process. Hosted ShroudForge registers from
    its mods thread the same way.
- **Run(void*)** runs on the game's pump thread:
  1. checks the identity: type 16, size 8, a nonzero id;
  2. reads the policy through a lock-free seqlock (at most 5 reads; a torn or busy read denies);
  3. calls `AcceptSessionWithUser`, SEH-guarded.

  It allocates nothing, logs nothing and shares no lock with any other thread.
- **The object** has CCallbackBase's shape: vptr, flags @8, id @12, `Run(void*)` in vtable slot 1 (MSVC's order). It is
  static and never freed. kiln-net must be built with clang-cl: `steam_messages.cpp` stops the build otherwise.
- **The policy writer** (maintain, close) stores the count before the ids and never clears stale ids. So the sequence check
  alone keeps a reader from mixing two policies; the host race test fails without it.

## Exit

kiln.net's `on_unload` (one of Kiln's own exit steps) sets `CLOSED_FOR_EXIT`, then waits at most 200 ms for member
calls and a callback already inside Steam. From then on every member answers `KILN_ERR_UNAVAILABLE` without a Steam call,
and the callback answers no request. There is no `UnregisterCallback` at exit, because the game's `SteamAPI_Shutdown` may
have run. A killed or crashed process runs no exit step.

## Without kiln.net

When kiln.net does not start (the game's session functions are unresolved on a build, or neither role starts), no
`kiln.net` table is published and these members are absent with it. Channel 12 behaves as before.

## Capability row

`net.steam_messages`. kiln-net.dll reports it; the core reports only the two cases where kiln-net.dll cannot.

| Cause | Row | Reported by |
|---|---|---|
| safe idle | `policy`, "not started in safe idle: ..." | core |
| kiln-net.dll did not start | `service-missing`, "kiln.net did not start: ..." | core |
| steam_api64.dll not loaded | `service-missing` | kiln-net.dll |
| an export missing | `symbol-missing`, the first missing name | kiln-net.dll |
| exports found, no member has found Steam ready yet | `pending` (`session`) | kiln-net.dll |
| a member found Steam ready | `available` (reported on kiln.net's next server or client-world tick) | kiln-net.dll |

## Tests

| What | Where |
|---|---|
| resolution order, open gate, maintain, callback filtering, the policy race, send, receive, accept, close, exit | `native/services/net/tests/test-steam-messages.sh asan` and `tsan` (`steam_messages_test.cpp`, a fake export table) |
| real exports, faults, the vtable slot and offsets, the GameServer flag, two processes | `native/services/net/tests/steam_lease_run.sh` (Wine) and `steam_lease_run.ps1` (Windows), over the stand-in steam_api64.dll |
| the row | `native/core/tests/test-caps.sh` |
| the layout: the four records and the core's table (184 bytes); the SDK's header without its `kiln-internal` blocks (128 bytes, no Steam messages name left), as C11 and C++20 | `native/core/tests/test-abi-headers.sh` |

## Not verified here

- Real Steam: resolution on both executables, callback delivery by the game's pump on both roles, and whether the
  one-time registration races the pump. These need a real game.
- Whether Steam still answers `SteamNetworkingSockets012`, which ShroudForge's own fallback asks for: it only affects the
  local id ShroudForge reports.
