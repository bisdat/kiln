<!-- SPDX-License-Identifier: MIT -->
# kiln.net Steam peer-to-peer messages

Since 0.2.0-dev.30.3. Seven kiln.net members that move Steam networking messages between game processes, separate from
the game connection and from game channel 12, in `native/core/abi/services/kiln_net.h` (the SDK ships them). C# mods use
them through Kiln.Network's `SteamMessages` ([../network.md](../network.md#steam-peer-messages-steammessages)).
Interface resolution and the session-request callback are adapted from ShroudForge by bonsaibauer, MIT.

## Who opens a lease

- **Kiln's interpreter for ShroudForge mods** (`kiln/plugins/kiln-sfi.dll`), through the owner the core designates for it
  in a start that runs it ([launch.md](launch.md#the-shroudforge-dialect-owner-kilninternallaunch)).
- **A C# mod whose kiln.mod.json says `"steamMessages": true`** ([../modding/manifest.md](../modding/manifest.md)). Kiln.Host
  admits the native owner it makes for such a mod when it loads it; the grant goes with the owner when the mod unloads,
  and a reloaded mod's new owner is admitted again. kiln.net asks the core which owners are admitted (a Kiln-internal
  table, [README.md](README.md#kiln-internal-tables)).
- **Every other owner** gets `KILN_ERR_UNAVAILABLE` from `steam_open`, logged once as `steam-messages-owner`.

## Owners and handles

- **Every member that takes a lease takes its owner first** (`steam_close`, `steam_maintain`, `steam_send`,
  `steam_receive`, `steam_accept`). A lease that is not an open lease of that owner answers `KILN_ERR_NOT_FOUND`,
  exactly as a closed or unknown one, so a caller learns nothing about other owners' leases; `steam_close` answers
  `KILN_OK` for it and closes nothing.
- **Owner 0** answers `KILN_ERR_INVALID_ARGUMENT`, a **revoked owner** `KILN_ERR_REVOKED` (also from `steam_open`).
- **A lease never outlives its owner.** kiln.net closes every lease of an owner when the core revokes it (a mod unloading,
  a plugin whose start failed), through the core's owner-retiring notification; closing the session lease sets the
  policy to `NONE` as `steam_close` does. A lease opened while its owner was being revoked is closed again before
  `steam_open` returns. Where kiln.net cannot subscribe to that notification, `steam_open` opens no lease at all
  (`KILN_ERR_INTERNAL`, logged once as `steam-messages-unwatched`).
- **A call in flight finishes on its own channels.** A send, receive or accept already past its lease check when the
  lease closes (`steam_close`, its owner revoked) makes its one Steam call. The lease reads as closed at once, but its
  channels and its session role stay taken until that call returns, so the call never reaches another lease's channels.
- **Handles are not guessable.** A lease handle is 64 random bits from the system's cryptographic generator
  (`BCryptGenRandom`), never 0 and never the handle of another open lease. When no random value can be drawn,
  `steam_open` answers `KILN_ERR_INTERNAL` and opens nothing.
- Owner ids themselves are small numbers: the owner check keeps well-behaved code on its own leases, and the random
  handle keeps a caller that guesses an owner from reaching a lease it never saw. Neither is a sandbox against native
  code in the process, which can call Steam itself.

## Members

`KilnNetApi` grows from 128 to 184 bytes; a caller checks `header.size >= 184` (the table version stays 1). A 128-byte
table (an older Kiln) has none of them. Code: `native/services/net/steam_messages.{h,cpp}`; the table and
the exit hook in `net.cpp`.

| Offset | Member | Answer |
|---:|---|---|
| 128 | `steam_status(KilnSteamMessagesStatus*)` | a snapshot; `KILN_OK` also when Steam is not ready |
| 136 | `steam_open(owner, const KilnSteamMessagesOpen*, KilnHandle* lease)` | a lease on an inclusive channel range |
| 144 | `steam_close(owner, lease)` | idempotent |
| 152 | `steam_maintain(owner, lease, const KilnSteamAcceptPolicy*, KilnSteamMessagesStatus* out)` | the session lease's accept policy |
| 160 | `steam_send(owner, lease, peer, data, len, channel, flags, int32_t* steam_result)` | one message; Steam's EResult |
| 168 | `steam_receive(owner, lease, channel, buffer, cap, KilnSteamMessageInfo*, uint32_t* got)` | at most one message, taken off Steam's queue |
| 176 | `steam_accept(owner, lease, peer, uint32_t* accepted)` | a pending session accepted by hand |

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

Every member but status checks the exit first (`KILN_ERR_UNAVAILABLE`), then the owner (`KILN_ERR_INVALID_ARGUMENT` for 0,
`KILN_ERR_REVOKED` for a revoked owner).

- **open:** `KILN_ERR_UNAVAILABLE` (an owner that is neither the designated one nor admitted, logged once as
  `steam-messages-owner`), then `KILN_ERR_INVALID_ARGUMENT` (no record, a short record, unknown flags, first > last, a
  channel above 65535, an overlap with an open lease of any owner, a second `KILN_STEAM_OPEN_SESSIONS` lease, all 16
  leases taken), `KILN_ERR_INTERNAL` (no owner-retiring notification here, see above; no random handle). open calls no
  Steam function.
- **close:** `KILN_OK` whether or not the handle is an open lease of the owner.
- **maintain:** `KILN_ERR_NOT_FOUND` (no such open lease of the owner), `KILN_ERR_INVALID_ARGUMENT` (not the session lease;
  a short record, an unknown mode, more than 256 ids, an id 0, ids NULL: nothing changes). Steam not ready: `KILN_OK`,
  `*out` without `AVAILABLE`; a `NONE` policy is in force at once (it calls no Steam function and only narrows, as
  closing the session lease does), any other policy changes nothing until Steam is ready.
- **send:** `KILN_ERR_INVALID_ARGUMENT` before any Steam call (no result pointer, peer 0, more than 524,288
  bytes, bytes without data, flags other than 0, 8, 32 or 40), `KILN_ERR_NOT_FOUND` (no such lease of the owner),
  `KILN_ERR_INVALID_ARGUMENT` (a channel outside the lease), `KILN_ERR_UNAVAILABLE` (Steam not ready). Otherwise
  `KILN_OK` with `*steam_result` = Steam's EResult (1 = OK).
- **receive:** as send for the lease and channel. Steam answers 0 messages: `*got = 0`. One: copied, released,
  `*got = 1`, `flags` = `KILN_STEAM_SEND_RELIABLE` when Steam's reliable bit (8) is set. Bigger than `cap`, a negative
  size, bytes without data, or a sender that is not a nonzero SteamID: released, `KILN_STEAM_ERR_INVALID_DATA` (210).
  Any other count: released when there is a message, `KILN_ERR_INTERNAL`. The message is released exactly once on every
  path.
- **accept:** `KILN_ERR_INVALID_ARGUMENT` (peer 0, no result pointer), `KILN_ERR_NOT_FOUND`, `KILN_ERR_UNAVAILABLE`.
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
| resolution order, open gate, maintain, callback filtering, the policy race, send, receive, accept, close, exit; owners and handles: random handles, forged and guessed handles, a wrong owner, owner 0, revoked owners (the core's notification and the core's answer), the open/revoke race, a call in flight when its lease closes, no owner-retiring notification | `native/services/net/tests/test-steam-messages.sh asan` and `tsan` (`steam_messages_test.cpp`, a fake export table and a fake grants table) |
| the grants (kept with the owner, gone with it) and who resolves `kiln.internal.owners` | `native/core/tests/test-owner-retiring.sh`, `native/core/tests/test-service-access.sh` |
| real exports, faults, the vtable slot and offsets, the GameServer flag, two processes | `native/services/net/tests/steam_lease_run.sh` (Wine) and `steam_lease_run.ps1` (Windows), over the stand-in steam_api64.dll |
| the row | `native/core/tests/test-caps.sh` |
| the layout: the four records and the table (184 bytes), the same header in the core and the SDK, as C11 and C++20 | `native/core/tests/test-abi-headers.sh` |
| the C# layer: the binding's layout, the owner on every lease member, the opt-in, every answer | `managed/tests/Kiln.Network.Tests` (`SteamMessagesTests`) |

## Not verified here

- Real Steam: resolution on both executables, callback delivery by the game's pump on both roles, and whether the
  one-time registration races the pump. These need a real game.
- Whether Steam still answers `SteamNetworkingSockets012`, which ShroudForge's own fallback asks for: it only affects the
  local id ShroudForge reports.
