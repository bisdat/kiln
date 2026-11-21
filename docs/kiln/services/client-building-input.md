# kiln.client building input

Three kiln.client members that queue one building action (select, place, remove, undo, dismantle) as the local player's
own input: `building_input_submit`, `building_input_status` and `building_input_cancel` in
`native/core/abi/services/kiln_client.h`. Kiln's interpreter for ShroudForge mods uses them for a ShroudForge mod's
`runtime.world.building.submit / status / cancel`, and Kiln.Client's `BuildingInput` is their C# layer.

**Since 0.2.0-dev.30.3 the input producer is bound on game client 26430625**, and
**held by the client gate until its guided checks pass in a real game**
([why and how to lift it](#held-until-its-guided-checks-pass)). While it is held nothing is hooked and every call
answers `KILN_ERR_UNAVAILABLE`, as on a build without the producer. The SDK's `kiln_client.h` still leaves the members
out (they sit in its `kiln-internal` blocks), so a mod built on the SDK sees the 56-byte kiln.client table. Adapted from
ShroudForge by bonsaibauer, MIT (1.7.9 `modules/game-runtime/native/player-input/apply_building_input.cpp`).

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
| `TIMEOUT` 4 | 5 s after submit while still queued; a pressed request answers it and is still released (unless its input never came back: below) |
| `CANCELLED` 5 | cancelled, its owner was revoked, its client world stopped, the producer stopped, or a guarded copy faulted |

**Rules:**
- A selection is dispatched at once: slot, item and material, each block's version stamp written last.
- PLACE, REMOVE and UNDO are pressed, then released at a later input version. PLACE adds `MainhandAction_Release`.
- DISMANTLE is held for 1.2 s, with its bits and transform written again at every visit.
- The producer waits while the player's own building input is down.
- Only the `ClientPlayerInput` that took a press is released. A press that is not released 10 s after it was made
  (the local player's entity or its `ClientPlayerInput` changed before the release) ends `TIMEOUT` without a release
  and frees the slot; the game's own input writer rewrites every digital bit at its next step anyway.
- **Cancel:** a queued request ends at once. A pressed one answers `CANCELLED` and is released at its next input; the
  slot stays busy until then (at most 10 s after the press). A dispatched one stays dispatched.
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

## Where the producer runs

The observation point is sigdb's `client.input_observation` (`sigdb/symbols/client.d/client-input.json`; the evidence
is in [docs/re/client.md](../../re/client.md#the-local-input-path-building-input)). Each client frame in which the client
world's step counter moved, the game client does, in this order, on its main thread:

1. advances the client world's step counter (the version the game stamps its own input actions with);
2. runs its **local input writer**: it fills the local player's `ClientPlayerInput` for that step, `digitalInput` from the
   player's devices first, then the camera;
3. sends the step's **input message**, which copies the whole `ClientPlayerInputData` (952 bytes) to the server;
4. runs the client world's programs (the cursor program writes `cursorInput` here, for the next message);
5. writes the input actions the player chose in its menus (a building item picked in the build menu, for one).

The producer runs between 2 and 3: a kiln.hooks mid link on the local input writer, right after its camera stores. So
what it writes (`cursorInput`'s two transforms, `digitalInput`'s building bits, `createBuildingItemAction`,
`buildingStockCycleAction`) goes out in that step's message, and nothing of the game writes those fields between the
producer and the message. The version of a visit is the client world's step counter, read where the game reads it;
there is one visit per step, so a press goes out in one message and its release in the next.

- **One detour, two links.** kiln.rules' read-only camera check sits on the same instruction. Both are links of one
  kiln.hooks detour: the producer reads the registers and writes only the four fields above; the camera check sees the
  same registers and the same camera fields as without it.
- **The layout** is the build's reflected one: `cursorInput` 760, `digitalInput` 936, `createBuildingItemAction` 500,
  `buildingStockCycleAction` 160; `PlayerInputType` MainhandAction 0 to 3, SecondaryBuildingAction 4, ContextualAction 5,
  ContextualAction_Hold 7, BuildingUndo 40 (bit 37 is Glider on this build). Every value is fixed in sigdb, so a game
  update that moves one turns the feature off instead of writing the wrong field.
- **Only the local player's input.** The visit carries the entity the writer filled; the producer acts only when it is
  the local player's entity of the live client session (`client_input.h`, the selection).
- **The server decides.** This is input, not a server-side build: the server checks permissions, resources, range and
  placement exactly as for the player's own input. `DISPATCHED` means "sent as input", not "built".

## Held until its guided checks pass

`client.building_input` is in the client gate's list (`native/core/gates/client-input.json`, client side only) with the
checks `building-input.select`, `building-input.place-remove`, `building-input.undo-dismantle`, `building-input.own-input`
and `building-input.joined` (`mods/tests/client-pass/BuildingInput.cs`). While it is held:
- the local input observation is not installed and the producer stays unbound;
- `client.building_input` reads `unavailable`, `policy`, "game test pending: ..." with those check ids;
- C#'s `BuildingInput.Available` is false and kiln mode's `runtime.world.building.*` answer unavailable.

`"preview": ["client.building_input"]` (or `true`) in `kiln/kiln.json` (in the game folder) lifts it for one game, for the guided pass.
The row is removed from the list in the commit that records the pass.

## Capability row

`client.building_input` (core, static). The causes, in the order they are checked:

| Cause | Row |
|---|---|
| held by the client gate (above) | `policy`, "game test pending: ..." |
| safe idle | `policy`, "not started in safe idle: ..." |
| a dedicated server | `wrong-side` |
| kiln.client did not start | `service-missing` |
| no producer bound | `symbol-missing`, with the reason: sigdb has no `client.input_observation` for this build, the local player is not known on it (`client.local_player`), or hooking the observation point failed |
| otherwise | `available` |

## Tests

| What | Where |
|---|---|
| members, producer, the unbound reasons and threads | `native/core/tests/test-client-building-input.sh asan` and `tsan` (`client_building_input_test.cpp`) |
| Windows guarded copies, the selection and the observation point's visit (the version read through the input object and the client world; faults skip the visit) | `client_input_windows_host.cpp`: `native/core/tests/test-client-input-wine.sh` under Wine, or built the same way and run on Windows |
| the row, the gate's hold and preview | `native/core/tests/test-caps.sh` |
| the observation point, the version getter and the layout on the client exe | `tools/sigport/sigdb_check.py sigdb/symbols/client.json <enshrouded.exe>` (`client.input_observation` on), `tools/sigport/build_record.py check` |
| the layout: the record (112 bytes) and the core's table (80 bytes); the SDK's header without its `kiln-internal` blocks (56 bytes, no building input name left), as C11 and C++20 | `native/core/tests/test-abi-headers.sh` |
| the guided checks are the gate row's, each in the pack and in rows.json | `tools/client-checks/test_report.py` |
| in a real game: select, place and remove, undo and dismantle, nothing while your own building action is held, a joined client | the client pass, area "Building input" ([client-pass.md](../client-pass.md)) |
