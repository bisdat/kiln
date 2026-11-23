# Camera control

Since mod API **0.2.17**, `Kiln.Camera` wraps the new, size-tagged `kiln.camera` v1 table.
Mods use Kiln's player ids and typed camera requests, never game functions, addresses, or component offsets.

## Maintained stock Mode

On the server world's tick thread, call `Camera.SetMode(player, mode, duration, transition, priority)` or
`Camera.Set(player, new CameraOverride(Mode: ..., InputPolicy: CameraInputPolicy.StockMode))`.
`camera.server_mode` requires world lifecycle, owner cleanup, the connected-player snapshot, and tracked writes.
Kiln declares its own write access; a calling mod needs no low-level `writes` manifest entry.
If the declaration is refused or the tracked-write program is not running, `camera.server_mode` reports unavailable
with a `faulted` reason instead of accepting a request that cannot complete.

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
a claim that the client drew it. Ordinary Mode completions and `Camera.Changed` run on SERVER Pre (local visual
completions on CLIENT Pre), completion before the operation's change event. Terminal `WorldStopped` may arrive on the destroying thread and must not access the game.
A retiring author receives no new callbacks; live observers can still receive its terminal change event.
Camera writes must come from mod code: native owner 0 and managed calls outside a mod are refused.
The owner-retiring observer closes that owner's callback gate before cleanup; off-thread retirement waits outside
locks for in-flight callbacks (bounded to 2 seconds, logged and retained rather than freed on timeout). Self-retirement
does not wait on its own callback. This camera-specific barrier deliberately differs from the generic observer's
non-waiting guidance. No author callback runs in a render hook.

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

## The local visual camera: SetLocal

On a game client, `Camera.SetLocal(request)` (the client world's thread) sets a render-only camera for this client's
own player: a keyed `Path` (`camera.local_pose`) or a follow `Profile` (`camera.local_profile`) or neither, plus a
`Lens` (`camera.local_fov`) and/or a `Shake` (`camera.local_shake`); a lens alone needs only `camera.local_fov`. Kiln writes it in one place, the renderer's camera selection: the selected pose's position and
rotation (its scale stays the game's) and/or the field of view the game is about to store. The player's input camera,
aim, the game's camera object and every component stay as they are, so movement, aim and `Render.Camera` do not
change. A request is `Ok` (`ClientFrame`) only after a frame showed it; the native completion names that frame,
and the managed result identifies its lease and confirmed revision. When something
after Kiln changes the frame (another camera writer), the request ends with `FrameFault` and `MayHaveApplied`; with
no confirming frame within 1 s it is `NoCameraFrame` (`MayHaveApplied` when one may have been drawn). There is no
collision: an authored camera can clip terrain and buildings.

Times run on the process's monotonic clock (pausing the simulation does not pause them). T0 is the first frame that
showed the lease; it ends at T0 + `Duration` and the next frame is the game's own. Path keys: positions in straight
lines, rotations the short way round, keyed lenses linear in degrees; before the first key and after the last the
camera holds that key; no splines, no looping. `Update` replaces the whole request: `Preserve` evaluates the new one at
the time already elapsed (refused `ExpiredUpdate` when no time is left, `InsufficientLifetime` when less is left than
the new blends, and the old revision stays), `Restart` starts a new T0. Blends use smoothstep: `Blend.In` from what
was on screen (the game's view on a fresh request, the previous visual on an update or a takeover), `Blend.Out` over
the duration's last part toward the game's fresh view; `Camera.End` blends the last shown view out to the game's view
and completes after that. Parts a replacement drops go back to the game's view at once.

A follow `Profile` follows `Follow` (`EntityId.None`: the local player) at `Distance` metres from a pivot `OffsetLocal`
metres away in the target's axes and `OffsetWorldY` metres up. The metres are absolute: not added to the game's own
distance and not scaled with the target. `FreeOrbit` keeps the game's orientation (the player keeps turning the view),
`FollowHeading` takes the target's upright heading (yaw only: a banking or climbing target does not tilt the view)
turned by `Rotation`, `FixedWorld` uses `Rotation` as it is. Kiln copies the target's position in each client step
before the step runs, so the camera follows one completed step behind, without extrapolation. A target the client
world does not have is refused `TargetUnavailable` (so is a heading camera whose target points straight up or down on
its first frame); one that disappears later ends the lease `TargetLost`, one not copied for 250 ms `SafetyRejected`.
A `Shake` moves the camera without turning it, in the camera's own axes, never further than `AmplitudeMetres`; equal
seeds give equal shakes whatever the frame rate, `Falloff` fades it over the duration's last part, and `End` stops it.

Limits: every key and every derived camera position must stay within 250 m of the local player, whose position Kiln
copies each client step; a path, profile or shake request whose player sample is missing or older than 250 ms writes nothing (and ends
`SafetyRejected` once it was shown), a lens alone does not need it. A value out of bounds is refused `Bounds` before the
first frame and ends the lease `SafetyRejected` after it. Kiln leaves the game's view alone when it is one v1 does not
take (a scaled camera, a lens outside 0.01 to pi - 0.01 rad). A write that faults turns visual writes off for the rest
of the client world (the rows say so). Since mod API 0.2.17.

## A server's visual for a player's client (`Camera.Set`)

`Camera.Set(player, request)` with visual parts (no `Mode`) asks the render-only camera of one player's client, from the
server world. It needs `camera.remote`: Kiln.Camera binds its transport bridge at Kiln's first service tick (outside any
mod, so no mod owns or unloads it), opens the channel `kiln.camera` version 1 (not required: vanilla and older clients
still join; at most 16384 bytes a message) and turns the row on. Per player:

- **A guest whose client runs Kiln** gets the request over `kiln.camera`: its client says Hello with the local part rows
  it has, the server answers Welcome with a binding token, and each Apply, Update and End carries that binding, the
  lease and its revision. `Ok` comes only when a frame of that client was drawn with it (`CameraCommitScope.ClientFrame`,
  `Camera.Changed`'s `Frame` is the client's). No answer within 3 s (two resends of the same request) is
  `TransportTimeout` with `MayHaveApplied`: end the lease. The client lets go by itself at the lease's duration, when the
  server's heartbeat is missing for 3 s, and when the connection or a world ends; it reports its own end, which arrives as
  `Changed` `Ended` with the client's reason (`Expired`, `TargetLost`, `SafetyRejected` ...). A player who leaves ends
  the lease `PeerDisconnected`.
- **The host's own player** (a client that hosts) goes through a local queue from the server world to the client world:
  no packet, no handshake; the host's own `camera.local_*` rows decide.
- **Refusals** (`Ok = false`, never a silent stock fallback): a vanilla client, or a Kiln without `kiln.camera`,
  `RequiresKilnClient`; a client without a part the request needs (its `camera.local_*` row is off there)
  `ClientCapability`; a client that does not finish the camera handshake within 2 s `Negotiation`; no such player
  connected `PeerUnavailable`; more than 10 requests a second (burst 16) to one player `RateLimit`; a request larger than
  the negotiated body `BodyTooLarge`.

`Camera.GetCapabilities(CameraEndpoint.Remote, player)` answers `Remote` plus the parts that player's client offered (the
host's own player: the host's local rows). A mount or cutscene that requires a part checks it there first.
No arbitrary pose, lens, follow profile, or shake is ever sent to a vanilla peer; a vanilla player only gets stock mode
locks (`SetMode`).
The input camera and aim are not a visual-control API. The reserved resource-profile bit is unavailable in v1.
`camera.service` means the table exists; `camera.server` and `camera.local` are managed aggregates, not substitutes
for checking every requested part. See [capability rows](caps.md).

## Entry points and capability rows

All members below are since mod API 0.2.17. SERVER and CLIENT refer to worlds, not the executable: a hosting client
has both. `Update` and `End` follow the lease's stored world, even if both worlds happen to use one OS thread.

| Entry | World / thread | Required row |
|---|---|---|
| `SetMode`, `Set` with Mode | SERVER Pre / tick thread | `camera.server_mode` |
| `Set` with visuals | SERVER Pre / tick thread | `camera.remote`; the player's client must offer the parts (else `ClientCapability`) |
| `SetLocal` | CLIENT Pre / client-world thread | Each requested `camera.local_*` part; Mode refuses `WrongEndpoint` |
| `Update` | Stored owning world / tick thread | `camera.service` and the replacement's part rows |
| `End` | Stored owning world / tick thread | `camera.service`; cleanup is allowed after a part turns off |
| `GetCapabilities` | Any thread | `camera.service`; bits mean feature availability, not a live frame/player |
| `GetVisual` | Client / any thread, try-lock once | `camera.rendered_snapshot` |
| `Changed` | Owning Pre; terminal `WorldStopped` may use the destroying thread | Event carries values only; no game access at terminal world stop |

### Numeric limits and authored data

All authored numbers are finite. Positions are within ±1,000,000 m on each axis and visual positions within 250 m of
the sampled local player. Authored quaternion norm squared must be within 0.001 of 1 (then normalized). Profiles use
absolute distance 0.25–200 m, local-offset vector length at most 100 m and world-Y offset ±100 m, with combined pivot
and output checked again. Lens degrees are 5–160; shake amplitude is at most 0.5 m and frequency 0.1–30 Hz.
A path has 1–256 keys, first at 0, strictly increasing whole-millisecond times no later than its duration. Every key
has a lens or none does; keyed lenses cannot combine with a standalone lens. Blends are nonnegative whole
milliseconds with In + Out ≤ Duration. Collision is explicitly `None` in v1; rotational shake, splines, looping and
`AimToo` are not implemented (`AimToo` refuses `InputPolicyUnsupported`).

`request.ToJson()` / `CameraOverride.FromJson(...)` use camelCase properties, exact enum names, integer milliseconds, `[x,y,z]`
positions/vectors and `[x,y,z,w]` rotations. The same authored shape is described by
`schema/camera-override.schema.json`; no engine names, functions or offsets enter the request.

## Sample and offline fake

[CameraSample](../../../mods/samples/CameraSample/) is an SDK mod (since 0.2.17). Its admin command
`camera-mode` requests five seconds of stock first-person for the caller; `camera-mode-off` explicitly ends that
mod's lease. On a hosting game client, its local console commands `camera-lens`, `camera-path` and
`camera-local-off` post to `Client.Post` before touching the local camera. These local examples are not a remote
camera request from a dedicated server. The path starts from a copied final visual pose and moves two metres in
world X over two seconds. It has no collision. Every asynchronous result is checked and logged, including a lease
returned with `MayHaveApplied`, which the sample retains so it can be ended.

A lens-only request is `new CameraOverride(Lens: new CameraLens(40))`; it asks for no pose capability. The sample's
two-key path uses explicit poses/rotations and `CameraBlend` for the whole request, not per-part blending. End the
sample's current local lease before starting another; owner retirement also cancels all its work on unload.
The mount-follow sample is separate work, not included here.

`KilnHarness.Camera` records accepted calls and models lease ownership, priorities, updates, End and change ordering.
`Expire`, `End`, `StopWorld` and `RefuseNext` script expiry, target/peer/capability losses, world stop and game
refusals; unloading a mod ends its leases with `OwnerRetired`. It does not draw frames, advance camera clocks,
blend, capture a snapshot or model remote transport. The sample tests use its real command for Mode and invoke the
loaded mod's local handlers on the harness world thread (the kit does not model `Client.Post`).

## Verification

`native/services/camera/tests/run.sh` exercises the real service entry point, ABI, lease model, and Mode backend on
a host, including absent dependencies, connected-player checks, cancellation, clocks, and lifecycle invalidation.
`mods/tests/camera-test` links the same Mode backend into a dedicated-server fixture with two real AutomatedPlayer
entities and a private fixture resolver. Its eight acceptance results are counted by Kiln.Testing. Those entities
are not authenticated clients. The fixture's clock and model epoch changes are deterministic; it does not claim to
reload the engine's world or prove what a connected player's screen displays.
