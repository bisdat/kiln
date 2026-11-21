# kiln.client building input

Three kiln.client members that queue one building action (select, place, remove, undo, dismantle) as the local player's
own input: `building_input_submit`, `building_input_status` and `building_input_cancel` in
`native/core/abi/services/kiln_client.h`. Kiln's interpreter for ShroudForge mods uses them for a ShroudForge mod's
`runtime.world.building.submit / status / cancel`.

**In this release they are Kiln-internal: no supported game build has the input producer yet**, so every call answers
`KILN_ERR_UNAVAILABLE` ([why](#why-it-is-unavailable-on-every-supported-build)). The SDK's `kiln_client.h` leaves them
out (they sit in its `kiln-internal` blocks), so a mod built on the SDK sees the 56-byte kiln.client table; this page
describes them for the day a game build gets the producer and they become public. Adapted from ShroudForge by
bonsaibauer, MIT (1.7.9 `modules/game-runtime/native/player-input/apply_building_input.cpp`).

## Members

`KilnClientApi` grows from 56 to 80 bytes; a caller checks `header.size >= 80` (the table version stays 1).

| Offset | Member | Answer |
|---:|---|---|
| 56 | `building_input_submit(owner, expected_session, const KilnBuildingInput*, uint32_t* request_id)` | queues one request for the live client session |
| 64 | `building_input_status(owner, request_id, uint32_t* state)` | `KILN_BUILDING_INPUT_*`, a copy |
| 72 | `building_input_cancel(owner, request_id, uint32_t* cancelled)` | 1 for the caller's latest id |

All three may be called from any thread, and none of them waits for the game. `expected_session` is 0 (the live client
session) or `KilnClientState.client_session`.

`KilnBuildingInput` (112 bytes): `size`, `action` (`SELECT 0, PLACE 1, REMOVE 2, UNDO 3, DISMANTLE 4`), `player`,
`item_id`, `material_item_id`, `slot`, `target_entity_id`, `reserved` (0), `position[3]` (metres), `rotation[4]`
(quaternion x y z w), `scale[3]`.

### Order of refusals

**submit:**
1. `KILN_ERR_INVALID_ARGUMENT`: no record or out, a short record, owner 0.
2. `KILN_ERR_REVOKED`: the owner was revoked. Also when the revoke lands while submit runs: the request it had
   queued is cancelled as for a revoked owner (asked again once the request is in the slot, so the owner-retiring
   callback cannot miss it).
3. `KILN_ERR_UNAVAILABLE`: a dedicated server, safe idle, or no producer bound to this build. This comes before every
   field rule, so on a build without a producer every request answers it.
4. `KILN_ERR_INVALID_ARGUMENT`: a field outside ShroudForge's rules:
   - player 0, action above 4, slot above 255, `reserved` not 0;
   - SELECT without an item;
   - DISMANTLE without a target, or a target on any other action;
   - for PLACE, REMOVE and DISMANTLE: a non-finite quaternion or one with a squared norm below 1e-12, a position whose
     Q32.32 value is not strictly inside the int64 range, or a scale outside (0, 1024].

   SELECT and UNDO ignore the transform, as ShroudForge does.
5. `KILN_CLIENT_ERR_BUILDING_STALE` (201): no client world is live, its session already stopped, or `expected_session`
   names another session.
6. `KILN_CLIENT_ERR_BUILDING_BUSY` (200): a request of any owner is queued or pressed.

**status and cancel:** `KILN_ERR_INVALID_ARGUMENT` (no out, owner 0), `KILN_ERR_REVOKED`, then `KILN_ERR_UNAVAILABLE` (a
dedicated server or safe idle). Otherwise `KILN_OK`; any id but the caller's latest answers `UNKNOWN` and is not cancelled.

## Request life

There is one request slot for the whole client. Ids start at 1, grow by one and skip 0.

| State | Meaning |
|---|---|
| `QUEUED` 1 | waiting for the input path |
| `PRESSED` 2 | the action is held in the local `ClientPlayerInput` |
| `DISPATCHED` 3 | emitted as ordinary input; not the world's acknowledgement (the server still checks permission, range, recipe and placement) |
| `TIMEOUT` 4 | 5 s after submit while still queued; a pressed request answers it and is still released |
| `CANCELLED` 5 | cancelled, its owner was revoked, its client world stopped, the producer stopped, or a guarded copy faulted |

**Rules:**
- A selection is dispatched at once: slot, item and material, each block's version stamp written last.
- PLACE, REMOVE and UNDO are pressed, then released at a later input version. PLACE adds `MainhandAction_Release`.
- DISMANTLE is held for 1.2 s, with its bits and transform written again at every visit.
- The producer waits while the player's own building input is down.
- Only the `ClientPlayerInput` that took a press is released.
- **Cancel:** a queued request ends at once. A pressed one answers `CANCELLED` and is released at its next input; the
  slot stays busy until then. A dispatched one stays dispatched.
- **A revoked owner:** as cancel.
- **Timeout over cancel:** the 5 s rule also rewrites a pressed request's pending answer, as ShroudForge's status does.
  A pressed request that was cancelled (or whose owner was revoked) and is not released within 5 s of its submit
  answers `CANCELLED`, then `TIMEOUT`. It is the one answer that moves back; it is still released.
- **The client world's STOPPING:** an open request of that session ends cancelled, with nothing left to release, and no
  later submit binds to that session.

## Threads

- **Callers.** submit, status and cancel hold the slot's mutex for a copy only.
- **The producer** runs on the client's main thread inside the local input observation (`client_input.h`):
  - it only try-locks the slot, and skips the visit when a caller holds it (counted);
  - it allocates nothing, logs nothing, runs no mod code and no Lua, and touches game memory only through SEH-guarded
    copies;
  - a faulting copy ends the request cancelled.

## Why it is unavailable on every supported build

The producer needs four things the current builds lack in Kiln's sigdb:
1. **An observation point in the client cursor system.** It must sit where the frame's `ClientPlayerInput` is complete
   and the building systems have not read it yet.
2. **The game's building input version call** for that visit's execution view.
3. **The `ClientPlayerInput` layout:** `cursorInput`, `digitalInput`, `createBuildingItemAction` and
   `buildingStockCycleAction`.
4. **The `PlayerInputType` bit numbers.**

ShroudForge 1.7.9 has the same producer only in its client profile 1076226 (a KFC3 build). It cannot select that profile
for any KFC4 build (every profile from 25966634 on), so on those builds its building input is always unavailable too. The
1076226 hook pattern and its seven original bytes do not occur in client 26430625. The input layout and the bit numbers
moved: `cursorInput` 624 to 760, `digitalInput` 792 to 936, and the undo bit 37 to 40 (bit 37 is `Glider` there).

Until a Kiln build verifies all four, these hold:
- `building_input::bound()` stays false;
- `client.building_input` reports `unavailable`, `symbol-missing`, with that reason;
- submit answers `KILN_ERR_UNAVAILABLE`.

The change that binds it:
- adds one sigdb feature for the point, the version call and the layout constants;
- installs the detour in `client_input.cpp` (the only detour on that function, shared with the local input sample);
- binds the producer;
- holds the capability in the client gate until its guided check passes in a real game.

## Capability row

`client.building_input` (core, static). The causes, in the order they are checked:

| Cause | Row |
|---|---|
| safe idle | `policy`, "not started in safe idle: ..." |
| a dedicated server | `wrong-side` |
| kiln.client did not start | `service-missing` |
| no producer bound | `symbol-missing`, with the producer's reason |
| otherwise | `available` |

## Tests

| What | Where |
|---|---|
| members, producer and threads | `native/core/tests/test-client-building-input.sh asan` and `tsan` (`client_building_input_test.cpp`) |
| Windows guarded copies and the selection | `native/core/tests/test-client-input-wine.sh` (`client_input_windows_host.cpp`) |
| the row | `native/core/tests/test-caps.sh` |
| the layout: the record (112 bytes) and the core's table (80 bytes); the SDK's header without its `kiln-internal` blocks (56 bytes, no building input name left), as C11 and C++20 | `native/core/tests/test-abi-headers.sh` |
