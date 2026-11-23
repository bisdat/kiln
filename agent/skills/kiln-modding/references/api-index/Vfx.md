# Vfx (Kiln.Vfx)

## Offset — Where an attached effect sits relative to its slot (keen::ecs::Offset).
- `Offset()`
- `Offset(Vector3 Local = default(Vector3), Vector3 World = default(Vector3), Quaternion? Rotation = null)`
- `Deconstruct(out Vector3 Local, out Vector3 World, out Quaternion? Rotation) -> void`
- `Equals(Offset other) -> bool`
- `Local { get; init; } -> Vector3` — Metres along the slot's own axes.
- `static operator !=(Offset left, Offset right) -> bool`
- `static operator ==(Offset left, Offset right) -> bool`
- `Rotation { get; init; } -> Quaternion?` — The offset's own rotation; null for none.
- `World { get; init; } -> Vector3` — Metres along the world's axes.

## ScreenColor — A colour and how much of it: `"#ff4030"` is all of it.
- `ScreenColor(string Color, float Amount = 1)`
- `Amount { get; init; } -> float` — How much of it, 0 to 1.
- `Color { get; init; } -> string` — The colour as `#rrggbb`.
- `static implicit operator ScreenColor(string color) -> ScreenColor` — A colour in `#rrggbb` form, all of it (`ScreenColor.Amount` 1).
- `static TryParse(string text, out (float R, float G, float B) rgb) -> bool` — `#rrggbb` as sRGB values from 0 to 1.

## ScreenEffect — A full-screen effect.
- `ScreenEffect(ScreenEffect original)`
- `Contrast { get; init; } -> float?` — Around mid grey, 0.05 to 4.
- `Exposure { get; init; } -> float?` — Stops, -8 to 8: 1 is twice as bright.
- `FadeIn { get; init; } -> float?` — Seconds to reach full, 0 to 60.
- `FadeOut { get; init; } -> float?` — Seconds to fade once it ends or is stopped, 0 to 60.
- `Fill { get; init; } -> ScreenColor?` — The whole picture goes to this colour, by its amount: a fade to black, or a flash with a fade out.
- `Gamma { get; init; } -> float?` — Above 1 lifts the mid tones, 0.1 to 5.
- `Length { get; } -> double?` — Seconds from start to gone, or null: until it is stopped.
- `Order { get; init; } -> int?` — Lower runs first.
- `Problems() -> IReadOnlyList<(string Field, string Problem)>` — What is wrong, by field as written in JSON: `("tint.color", "\"red\" is not #rrggbb")`.
- `Saturation { get; init; } -> float?` — 0 is grey, 0 to 4.
- `Seconds { get; init; } -> float?` — Seconds at full, 0 to 3600.
- `Strength { get; init; } -> float?` — 0 to 1: how much of the whole effect shows.
- `Temperature { get; init; } -> float?` — -1 cold to 1 warm, keeping brightness.
- `Tint { get; init; } -> ScreenColor?` — A colour the picture is multiplied by: `"#7fd4ff"`, or with how much: `new ScreenColor("#7fd4ff", 0.5f)`.
- `Vignette { get; init; } -> ScreenVignette?` — Darker (or coloured) edges: `0.6f` is how much, or a `ScreenVignette`.

## ScreenEffects — Full-screen effects: colour grading, a tint, a vignette, fades and flashes (`ScreenEffect`).
- `static Available { get; } -> bool` — A play here would show (client; capability `kiln.screen`), or the relay is open (server; capability `screen.relay`).
- `static IsPlaying(ScreenHandle h) -> bool` — Client: it still shows, fading out included.
- `static Play(ScreenEffect effect) -> ScreenHandle` — Client: plays here.
- `static PlayFor(EntityId player, ScreenEffect effect) -> ScreenHandle` — Server: on one player's Kiln client (the host's own screen when that player is the host).
- `static PlayNear(WorldPosition at, float radius, ScreenEffect effect) -> ScreenHandle` — Server: on the Kiln clients whose player is within `radius` metres of `at`, when it starts.
- `static SetStrength(ScreenHandle h, float strength) -> void` — Changes how much of it shows, 0 to 1, while it plays: for what follows a value, such as health.
- `static Status() -> ScreenStatus` — What kiln.screen and the relay are doing.
- `static Stop(ScreenHandle h, bool immediate = false) -> void` — Stops it, by its fade out unless `immediate`.

## ScreenHandle — A playing screen effect: from `ScreenEffects`, for `ScreenEffects.Stop`.
- `ScreenHandle()`
- `ScreenHandle(uint Value)`
- `IsNone { get; } -> bool` — True for `ScreenHandle.None`.
- `static readonly None -> ScreenHandle` — No effect: the default value, what a play that did not start returns.
- `Value { get; init; } -> uint` — The handle's number; 0 is `ScreenHandle.None`.

## ScreenStatus — What kiln.screen and the relay are doing.
- `ScreenStatus(bool Available, bool Native, bool Enabled, bool Ready, int Playing, string Transfer, int Width, int Height, uint Format, long Plays, long Refused, long Frames, long Skipped, double PipelineMs, string Reason, bool Relay, long Sent, long Received)`
- `Available { get; init; } -> bool` — A play would show: on a client, kiln.screen is on; on a server, the relay is open.
- `Enabled { get; init; } -> bool` — It is not switched off, by kiln/config/screen.json or by a failure.
- `Format { get; init; } -> uint` — The Vulkan format (`VkFormat`) of the last swapchain drawn to.
- `Frames { get; init; } -> long` — Presented frames the pass drew into.
- `Height { get; init; } -> int` — Height in pixels of the last swapchain drawn to.
- `Native { get; init; } -> bool` — kiln.screen is in this process (a client).
- `PipelineMs { get; init; } -> double` — How long the pipeline took to build, in milliseconds.
- `Playing { get; init; } -> int` — How many effects are playing.
- `Plays { get; init; } -> long` — Plays since start.
- `Ready { get; init; } -> bool` — The pass is built for the current swapchain: the next frame with an effect draws it.
- `Reason { get; init; } -> string` — Why it is off, "" when it isn't.
- `Received { get; init; } -> long` — Relay messages this process has received.
- `Refused { get; init; } -> long` — Plays refused: kiln.screen was off, or the most it can play at once were already playing.
- `Relay { get; init; } -> bool` — The relay channel `kiln:screen` is open.
- `Sent { get; init; } -> long` — Relay messages this process has sent.
- `Skipped { get; init; } -> long` — Frames left alone because the GPU was still using that image.
- `Transfer { get; init; } -> string` — How the swapchain's values are read: "linear", "sRGB" or "PQ".
- `Width { get; init; } -> int` — Width in pixels of the last swapchain drawn to.

## ScreenVignette — Darker or coloured edges.
- `Amount { get; init; } -> float?` — 0 to 1 at the corners.
- `Color { get; init; } -> string?` — `#rrggbb`.
- `static implicit operator ScreenVignette(float amount) -> ScreenVignette` — The short form: a vignette of this `ScreenVignette.Amount`, in black.
- `Radius { get; init; } -> float?` — Where it starts: 0 the centre, 1 the corners.
- `Softness { get; init; } -> float?` — How far past `ScreenVignette.Radius` it takes to reach full, 0.001 to 2.

## Slot — The game's attachment slots (keen::AttachmentSlot).
enum: Root, Head, Chest, Pelvis, FootL, FootR, FootRL, FootRR, HandR, HandL, KneeR, KneeL, ArmL, Block, HandPalmR, HandPalmL, Generic00, Generic01, Generic02, Generic03, Generic04, Generic05, Generic06, Generic07, Generic08, Generic09, Generic10, Generic11, Generic12, Generic13, Generic14, Generic15, Generic16, Generic17, Generic18, Generic19

## Vfx — Play any VFX from code, at a position or attached to an entity's slot.
- `static Available { get; } -> bool` — Client: kiln.vfx can reach the game on this build (capabilities `kiln.vfx`, `vfx.play`).
- `static Info(VfxHandle h) -> VfxInfo?` — Details of a handle (client), null when unknown.
- `static IsKnown(VfxId vfx) -> bool` — Client: the game has this VFX registered.
- `static IsPlaying(VfxHandle h) -> bool` — True while the VFX is queued or held by the FxController (false once it ended, after `Vfx.Stop`, for unknown handles...
- `static PlayAt(VfxId vfx, WorldPosition at, Quaternion? rotation = null, float scale = 1, VfxParams? parameters = null, TimeSpan? lifetime = null, float radius = 0) -> VfxHandle` — Plays `vfx` at `at`.
- `static PlayFor(EntityId player, VfxId vfx, WorldPosition at, VfxParams? parameters = null, TimeSpan? lifetime = null) -> VfxHandle` — Server: plays `vfx` at `at` for one player (their Kiln client).
- `static PlayOn(VfxId vfx, EntityId entity, Slot slot = Slot.Root, Offset offset = default(Offset), VfxParams? parameters = null, TimeSpan? lifetime = null, float radius = 0) -> VfxHandle` — Attaches `vfx` to `entity` at `slot`.
- `static Preload(VfxId vfx) -> void` — Client: holds a reference on the VFX for the session, so its first real play does not start late (pipelines load in...
- `static ResourceBudget { get; } -> CapacityQueryResult` `0.2.4` — Client: the game's VFX resources for this process, all mods together (chapter 17's budget).
- `static Set(VfxHandle h, string parameter, Color value) -> void` — Sets a named parameter of a running VFX.
- `static Set(VfxHandle h, string parameter, Vector3 value) -> void` — Sets a named parameter of a running VFX.
- `static Set(VfxHandle h, string parameter, float value) -> void` — Sets a named parameter of a running VFX.
- `static Set(VfxHandle h, string parameter, int value) -> void` — Sets a named parameter of a running VFX.
- `static State(VfxHandle h) -> VfxState` — The handle's state (client).
- `static Status() -> VfxStatus` — kiln.vfx's and the relay's state.
- `static StatusJson() -> string` — Diagnostics as JSON (status and recent handles, client).
- `static Stop(VfxHandle h, bool immediate = false) -> void` — Stops a VFX (any thread on a client; tick thread for server handles).
- `static StopAll(bool immediate = false) -> int` — Stops every VFX the calling mod started (client).
- `static readonly UntilStopped -> TimeSpan` — As a lifetime: runs until `Vfx.Stop`.

## VfxEndReason — Why a VFX is `VfxState.Stopped` or `VfxState.Failed`.
enum: None, Finished, Stopped, EntityGone, UnknownVfx, StartFailed, TimedOut, WorldEnded, Fault, Refused, NoInstance

## VfxHandle — A running (or finished) VFX.
- `VfxHandle()`
- `VfxHandle(uint Value)`
- `IsNone { get; } -> bool` — True for `VfxHandle.None`.
- `IsRemote { get; } -> bool` — A server-side handle (the VFX was relayed to Kiln clients): it can be stopped and set, not observed.
- `static readonly None -> VfxHandle` — No VFX: the default value, what a play that did not start returns.
- `Value { get; init; } -> uint` — The handle's number; 0 is `VfxHandle.None`.

## VfxId — Which VFX: one of the game's by name, one of a mod's, or a GUID.
- `VfxId()` — A VFX by GUID (the same as `VfxId.Of`).
- `VfxId(Guid guid)` — A VFX by GUID (the same as `VfxId.Of`).
- `Equals(VfxId other) -> bool`
- `static Game(string name) -> VfxId` — One of the game's VFX by name.
- `GameName { get; } -> string?` — The name given to `VfxId.Game`, or null.
- `Guid { get; } -> Guid` — The GUID.
- `IsNone { get; } -> bool` — True for `VfxId.None`: no game name and an empty GUID.
- `static Named(ModContext mod, string name) -> VfxId` — The calling mod's VFX `name`: `UUIDv5(<mod>:vfx:<name>)`, the rule of every block id.
- `static Named(string modId, string name) -> VfxId` — A VFX of the mod `modId`, whether or not that mod is here.
- `static readonly None -> VfxId` — No VFX: the default value.
- `static Of(Guid guid) -> VfxId` — Any VFX by GUID.
- `static Of(string guid) -> VfxId` — Any VFX by GUID text.
- `static operator !=(VfxId left, VfxId right) -> bool`
- `static operator ==(VfxId left, VfxId right) -> bool`

## VfxInfo — Details of a VFX (client side).
- `VfxInfo(VfxHandle Handle, VfxState State, VfxEndReason Reason, uint FxHandle, uint Instance, int Thread, EntityId Entity, bool Attached, bool Unmatched, TimeSpan? StartDelay, TimeSpan? Lifetime, WorldPosition Position)`
- `Attached { get; init; } -> bool` — A `Vfx.PlayOn` took effect: the effect is attached to the entity.
- `Entity { get; init; } -> EntityId` — The entity it was played on (`Vfx.PlayOn`); `EntityId.None` for a play at a position.
- `FxHandle { get; init; } -> uint` — The game's FxController handle for it; 0 for none.
- `Handle { get; init; } -> VfxHandle` — The handle asked about.
- `Instance { get; init; } -> uint` — The game's VFX instance behind it; 0 for none yet (for example an attachment that is not resolved yet).
- `Lifetime { get; init; } -> TimeSpan?` — The length the play settled on; `Vfx.UntilStopped` for a loop; null when it is not known (before it started).
- `Position { get; init; } -> WorldPosition` — The position it plays at.
- `Reason { get; init; } -> VfxEndReason` — Why it ended or never started; `VfxEndReason.None` while it has not ended.
- `StartDelay { get; init; } -> TimeSpan?` — Time from the request to the game's start of it; null until it started.
- `State { get; init; } -> VfxState` — Where it stands.
- `Thread { get; init; } -> int` — The id of the thread that made the game call.
- `Unmatched { get; init; } -> bool` — The entity could not be verified near the position (a relayed `Vfx.PlayOn`): the effect played at `Position` without...

## VfxParams — Named parameters of an effect, set when it starts.
- `Set(string name, Color value) -> VfxParams` — Adds a colour parameter.
- `Set(string name, Vector3 value) -> VfxParams` — Adds a vector parameter.
- `Set(string name, float value) -> VfxParams` — Adds a float parameter.
- `Set(string name, int value) -> VfxParams` — Adds an integer parameter.

## VfxState — Where a VFX stands.
enum: None, Pending, Playing, Stopped, Failed

## VfxStatus — kiln.vfx's state.
- `VfxStatus(bool Available, bool Native, bool Guard, bool Controller, bool SystemReady, bool Simulated, uint Registered, uint MapCapacity, int Thread, uint Live, ulong Played, ulong Failed, ulong Refused, ulong Updates, bool Relay, long RelaySent, long RelayReceived)`
- `Available { get; init; } -> bool` — The same as `Vfx.Available`.
- `Controller { get; init; } -> bool` — An FxController update ran and no world has ended since.
- `Failed { get; init; } -> ulong` — Kiln's plays that never started.
- `Guard { get; init; } -> bool` — The guard is hooked and its map layout checked out.
- `Live { get; init; } -> uint` — Kiln's handles that are pending or playing.
- `MapCapacity { get; init; } -> uint` — The capacity of the game's VFX map.
- `Native { get; init; } -> bool` — kiln.vfx is in this process (a client).
- `Played { get; init; } -> ulong` — Kiln's plays that reached the FxController.
- `Refused { get; init; } -> ulong` — Requests the guard refused: ones that would have aborted the game.
- `Registered { get; init; } -> uint` — How many VFX are in the game's map.
- `Relay { get; init; } -> bool` — The relay channel `kiln:vfx` is open.
- `RelayReceived { get; init; } -> long` — Relay messages this process has received.
- `RelaySent { get; init; } -> long` — Relay messages this process has sent.
- `Simulated { get; init; } -> bool` — A simulated game (labs and tests only).
- `SystemReady { get; init; } -> bool` — The game's VFX system has entries in its map.
- `Thread { get; init; } -> int` — The thread of the FxController update; 0 when not seen yet.
- `Updates { get; init; } -> ulong` — FxController updates seen.
