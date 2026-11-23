# Camera control

Since mod API **0.2.17**, `Kiln.Camera` wraps the new, size-tagged `kiln.camera` v1 table.
Mods use Kiln's player ids and typed camera requests, never game functions, addresses, or component offsets.

## Maintained stock Mode

On the server world's tick thread, call `Camera.SetMode(player, mode, duration, transition, priority)` or
`Camera.Set(player, new CameraOverride(Mode: ..., InputPolicy: CameraInputPolicy.StockMode))`.
`camera.server_mode` requires world lifecycle, owner cleanup, the connected-player snapshot, and tracked writes.
Kiln declares its own write access; a calling mod needs no low-level `writes` manifest entry.

One tracked three-byte camera-control write runs per held player per server step, after the game's conflicting
writer. The player's connection handle and join step distinguish a new incarnation. The first successful write,
including an already-matching `Unchanged` result, starts the lease's monotonic clock. Pausing simulation does not
pause that clock; an expired lease cannot write on resume. This reaches vanilla clients without a Kiln handshake.
Only the stock first-person mode (`PlayerCameraBuildingFirstPerson`, mode 34) has been observed in the real game;
other stock modes may be subject to the game's normal state restrictions.

`Camera.Update` is a complete replacement. `Preserve` retains the original timeline; `Restart` starts a new one at
the first successful replacement write. A same-owner Apply refuses `AlreadyOwned`; another owner's equal or lower
priority refuses `Busy`. A higher-priority request keeps the previous lease until it can commit, then emits the
old `Ended(Superseded)` before the new `Applied`.

`Camera.End` invalidates the lease before cancelling its pending write. The next stock writer determines the
hand-back; Kiln never restores a saved boot-time camera. Ending a known terminal lease succeeds with `AlreadyEnded`
and no duplicate change event. Player departure, incarnation change, expiry, owner retirement, and world loss stop
maintenance. Queued callbacks retain their own lifetime token and reject invalidated work before touching bytes.

## Results, events, and limits

The task completes after acceptance, not inline with the call. Mode success is **ServerState**, not a visual ACK or
a claim that the client drew it. Ordinary completions and `Camera.Changed` run on SERVER Pre, completion before the
operation's change event. Terminal `WorldStopped` may arrive on the destroying thread and must not access the game.
A retiring author receives no new callbacks; live observers can still receive its terminal change event.

Bad arguments throw `ArgumentException`; the wrong world's thread throws `InvalidOperationException`; missing
capabilities throw `KilnUnavailableException` naming the required row. Valid current-state refusals return
`CameraResult.Ok == false` with a stable `CameraReason`. Request durations are whole milliseconds, 1–120,000
(default 10 seconds), priority is -1000–1000, Mode blend times must be zero, and the transition enum chooses stock
blending. The native model admits at most 64 pending operations and 16 Mode slots.

## Other endpoints

The typed records, native ABI, TestKit fake, and managed wire/state machines also describe local/remote visuals.
Their physical renderer and transport bridge are not implemented in this slice: their service rows are unavailable,
not hidden behind a client-test switch. No arbitrary pose, lens, follow profile, or shake is sent to a vanilla peer.
The input camera and aim are not a visual-control API. The reserved resource-profile bit is unavailable in v1.
`camera.service` means the table exists; `camera.server` and `camera.local` are managed aggregates, not substitutes
for checking every requested part. See [capability rows](caps.md).

## Verification

`native/services/camera/tests/run.sh` exercises the real service entry point, ABI, lease model, and Mode backend on
a host, including absent dependencies, connected-player checks, cancellation, clocks, and lifecycle invalidation.
`mods/tests/camera-test` links the same Mode backend into a dedicated-server fixture with two real AutomatedPlayer
entities and a private fixture resolver. Its eight acceptance results are counted by Kiln.Testing. Those entities
are not authenticated clients. The fixture's clock and model epoch changes are deterministic; it does not claim to
reload the engine's world or prove what a connected player's screen displays.
