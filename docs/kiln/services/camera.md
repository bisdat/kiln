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

## The rendered camera: GetVisual, Project, LookAt

On a game client, `Camera.GetVisual()` (`camera.rendered_snapshot`, any thread) returns the camera the newest frame was
really drawn with: after the game's own shake and extra rotation and after any kiln.camera override, with the
frame's field of view, aspect ratio and near/far planes. It is the logical, unjittered main view (no anti-aliasing
jitter, lens distortion or motion blur). Kiln reads it in the renderer's frame preparation, once per camera selection
and only for the live client world's renderer (other views and previews are skipped), and keeps values only. It
never waits: `Ok == false` with `SnapshotBusy` (a frame is being written that instant), `SnapshotStale` (older than
250 ms: the game is paused or not drawing) or `NoCameraFrame` (no frame of the live client world yet, the world
stopped, or the game's view is one v1 does not describe: a scaled camera, a lens outside 0.01 rad to pi - 0.01 rad, a
rotation that is not a unit quaternion). `TargetSampleStep` names the client step whose positions the frame's camera
was built from. `Render.Camera` stays what it was: the player's input camera, which is not the same thing.

`CameraVisualSnapshot.Project(point, viewportOrigin, viewportSize)` puts a world point on that frame's screen with the
snapshot's own lens: give the rectangle the game's view fills on your surface (letterbox included). A point behind the
near plane or past the far plane has no pixel; a point within depth keeps its pixel when it is off the rectangle
(`OnScreen` false), unless the number does not fit a float. `CameraPose.LookAt(position, target, up)` builds the
orientation whose +Z looks at `target` with `up` corrected to be square to it; a target closer than 0.0001 m or an up
along the view direction is an `ArgumentException` (no roll is guessed). Both are pure helpers: no capability, no
thread. Since mod API 0.2.17.

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
