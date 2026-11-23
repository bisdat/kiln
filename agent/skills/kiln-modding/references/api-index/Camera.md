# Camera (Kiln.Camera)

## Camera `0.2.17` — Camera overrides.
- `static Changed -> Action<CameraChange>?` — Every change of every camera lease: applied, updated, ended (with the reason).
- `static End(CameraLease lease) -> Task<CameraResult>` — Ends a lease of the calling mod.
- `static GetCapabilities(CameraEndpoint endpoint, EntityId player = default(EntityId)) -> CameraCapability` — What the camera service can do for `endpoint`.
- `static GetVisual() -> CameraVisualResult` — The camera the newest frame was drawn with.
- `static Set(EntityId player, CameraOverride request) -> Task<CameraResult>` — A camera request of the server for `player`: a mode lock (see `Camera.SetMode`), or visual parts for a player whose...
- `static SetLocal(CameraOverride request) -> Task<CameraResult>` — A render-only camera for this client's own player: a path, a follow profile, a lens, a shake.
- `static SetMode(EntityId player, CameraMode mode, TimeSpan duration, CameraTransitionOverride transition = CameraTransitionOverride.None, int priority = 0) -> Task<CameraResult>` — Locks `player`'s camera to one of the game's own modes for `duration`.
- `static Update(CameraLease lease, CameraOverride replacement, CameraUpdateTiming timing = CameraUpdateTiming.Preserve) -> Task<CameraResult>` — Replaces the whole request of a lease (not a merge).

## CameraBlend `0.2.17` — How a visual request fades in and out, in whole milliseconds; both together at most the duration.
- `CameraBlend(TimeSpan In, TimeSpan Out)`
- `In { get; init; } -> TimeSpan`
- `Out { get; init; } -> TimeSpan`

## CameraCapability `0.2.17` — What the camera service can do here; one bit per capability row `camera.*`.
enum: None, ServerMode, LocalPose, LocalFov, LocalProfile, LocalShake, RenderedSnapshot, Remote, ResourceProfile

## CameraChange `0.2.17` — A lease was applied, updated or ended (`Camera.Changed`).
- `CameraChange(CameraLease Lease, ulong Revision, CameraChangeKind Kind, CameraEndReason EndReason, CameraReason ReasonCode, string? Reason, ulong Frame)`
- `EndReason { get; init; } -> CameraEndReason`
- `Frame { get; init; } -> ulong` — The client frame that showed it, when one confirmed it; else 0.
- `Kind { get; init; } -> CameraChangeKind`
- `Lease { get; init; } -> CameraLease`
- `Reason { get; init; } -> string?`
- `ReasonCode { get; init; } -> CameraReason`
- `Revision { get; init; } -> ulong`

## CameraChangeKind `0.2.17` — What happened to a lease (`CameraChange`).
enum: Applied, Updated, Ended

## CameraCollisionPolicy `0.2.17` — Camera collision.
enum: None

## CameraCommitScope `0.2.17` — What an `Ok` result proves.
enum: None, ServerState, ClientFrame

## CameraEndpoint `0.2.17` — Whose camera a request is for, and who carries it out.
enum: ServerMode, Local, Remote

## CameraEndReason `0.2.17` — Why a lease ended.
enum: None, Released, Expired, TargetLost, PlayerLeft, WorldStopped, Superseded, CapabilityLost, OwnerRetired, PeerDisconnected, AuthorityTimeout, SafetyRejected, TransportTimeout, CancelledBeforeApply

## CameraInputPolicy `0.2.17` — What a request may touch besides the picture.
enum: VisualOnly, StockMode, AimToo

## CameraKey `0.2.17` — One key of a `CameraPath`.
- `CameraKey(TimeSpan At, CameraPose Pose, float? FieldOfViewDegrees = null)`
- `At { get; init; } -> TimeSpan` — When, from the lease's start, in whole milliseconds.
- `FieldOfViewDegrees { get; init; } -> float?` — The field of view then.
- `Pose { get; init; } -> CameraPose` — Where the camera is then.

## CameraLease `0.2.17` — A camera request that was accepted: what `Camera.Update` and `Camera.End` take.
- `CameraLease()`
- `CameraLease(ulong Value, CameraEndpoint Endpoint, EntityId Player, uint SessionGeneration)`
- `Endpoint { get; init; } -> CameraEndpoint` — Whose camera, and who carries it out.
- `Player { get; init; } -> EntityId` — The player whose camera it is.
- `SessionGeneration { get; init; } -> uint` — The world the lease belongs to: it ends when that world stops.
- `Value { get; init; } -> ulong` — The lease's number; never reused while the game runs.

## CameraLens `0.2.17` — A field of view, alone or next to a path, profile or shake.
- `CameraLens(float FieldOfViewDegrees)`
- `FieldOfViewDegrees { get; init; } -> float` — The vertical field of view, 5 to 160 degrees.

## CameraMode `0.2.17` — A stock camera mode of the game, for `Camera.SetMode`.
enum: EntityCamera, PlayerCameraSpawning, PlayerCameraSpawningDolly, PlayerCameraRespawning, PlayerCameraDefault, PlayerCameraDefaultTarget, PlayerCameraSprintingTarget, PlayerCameraSprinting, PlayerCameraRangedBowDefault, PlayerCameraRangedBowSneaking, PlayerCameraRangedStaffDefault, PlayerCameraRangedStaffSneaking, PlayerCameraHipfire, PlayerCameraHipfireStaffChanneling, PlayerCameraHipfireStaffChannelingTarget, PlayerCameraExecute, PlayerCameraIdle, PlayerCameraSneaking, PlayerCameraSitting, PlayerCameraCooking, PlayerCameraClimbing, PlayerCameraHookShot, PlayerCameraGlider, PlayerCameraSwimming, PlayerCameraDiving, PlayerCameraHeadDivingStart, PlayerCameraHeadDivingDefault, PlayerCameraDialogue, PlayerCameraAltar, PlayerCameraMotherFlame, PlayerCameraObelisk, PlayerCameraShroudForge, PlayerCameraBuildingAltar, PlayerCameraBuildingAreaDepositProp, PlayerCameraBuildingFirstPerson, PlayerCameraBuildingFirstPersonSneak, PlayerCameraBuildingThirdPerson, PlayerCameraBuildingFlySparkMode, PlayerCameraBuildingFlySparkModeSprinting, PlayerCameraFishing, PlayerCameraFishingHardFocusFloat, PlayerCameraFishingSoftFocusFloat, PlayerCameraFishingVictory, PlayerCameraFreeAimWandDefault, PlayerCameraFreeAimWandSprinting, PlayerCameraFreeAimWandSneaking

## CameraModeRequest `0.2.17` — A stock camera mode lock.
- `CameraModeRequest(CameraMode Mode, CameraTransitionOverride Transition = CameraTransitionOverride.None)`
- `Mode { get; init; } -> CameraMode`
- `Transition { get; init; } -> CameraTransitionOverride`

## CameraOverride `0.2.17` — One camera request.
- `CameraOverride(CameraModeRequest? Mode = null, CameraProfile? Profile = null, CameraPath? Path = null, CameraLens? Lens = null, CameraShake? Shake = null, TimeSpan Duration = default(TimeSpan), int Priority = 0, CameraBlend? Blend = null, CameraInputPolicy InputPolicy = CameraInputPolicy.VisualOnly)`
- `Blend { get; init; } -> CameraBlend?` — Fade in and out; null: none.
- `Duration { get; init; } -> TimeSpan` — How long the lease lasts, 1 ms to 120 s in whole milliseconds; zero means 10 s.
- `static FromJson(Json.JsonElement json) -> CameraOverride` — Reads one request object in a larger authoring document, with the rules of `CameraOverride.FromJson`.
- `static FromJson(string json) -> CameraOverride` — Reads an authored request: camelCase fields, exact enum names, position/vector/quaternion arrays, and integer...
- `InputPolicy { get; init; } -> CameraInputPolicy` — What the request may touch besides the picture.
- `Lens { get; init; } -> CameraLens?` — A field of view.
- `Mode { get; init; } -> CameraModeRequest?` — A stock camera mode lock.
- `Path { get; init; } -> CameraPath?` — A keyed camera.
- `Priority { get; init; } -> int` — -1000 to 1000.
- `Profile { get; init; } -> CameraProfile?` — A follow camera.
- `Shake { get; init; } -> CameraShake?` — A shake on top of whatever the view is.
- `ToJson() -> string` — Writes this request in the format `CameraOverride.FromJson` reads, after validating it.

## CameraPath `0.2.17` — A camera that moves through keys (a cutscene): positions go in straight lines, orientations the short way round; before the first key and after the last the camera...
- `CameraPath(IReadOnlyList<CameraKey> Keys)`
- `Keys { get; init; } -> IReadOnlyList<CameraKey>` — 1 to 256 keys with rising times; each within 250 metres of the player.

## CameraPose `0.2.17` — Where a camera is and where it looks: +Z of `Rotation` is forward, +Y up.
- `CameraPose(WorldPosition Position, Quaternion Rotation)`
- `static LookAt(WorldPosition position, WorldPosition target, Vector3 up) -> CameraPose` — A camera at `position` looking at `target`, with `up` as the direction that is up on the screen (corrected to be...
- `Position { get; init; } -> WorldPosition` — World metres.
- `Rotation { get; init; } -> Quaternion` — A unit quaternion.

## CameraProfile `0.2.17` — A camera that follows an entity at a fixed distance.
- `CameraProfile(EntityId Follow, float Distance, Vector3 OffsetLocal, float OffsetWorldY = 0, CameraRotationMode RotationMode = CameraRotationMode.FreeOrbit, Quaternion? Rotation = null, CameraCollisionPolicy Collision = CameraCollisionPolicy.None)`
- `Collision { get; init; } -> CameraCollisionPolicy` — Only `CameraCollisionPolicy.None`.
- `Distance { get; init; } -> float` — Metres from the pivot to the camera, 0.25 to 200.
- `Follow { get; init; } -> EntityId` — The entity to follow; `EntityId.None`: the local player.
- `LockRotation { get; } -> bool` — Whether the profile fixes the orientation (any mode but `CameraRotationMode.FreeOrbit`).
- `OffsetLocal { get; init; } -> Vector3` — The pivot's offset from the target in the target's own axes, at most 100 metres long.
- `OffsetWorldY { get; init; } -> float` — The pivot's offset straight up, -100 to 100 metres.
- `Rotation { get; init; } -> Quaternion?` — See `CameraRotationMode`; null is identity.
- `RotationMode { get; init; } -> CameraRotationMode` — Where the orientation comes from.

## CameraProjection `0.2.17` — A world point on the screen.
- `CameraProjection(bool WithinDepth, bool OnScreen, Vector2? Pixel, float DepthMetres, ulong Frame)`
- `DepthMetres { get; init; } -> float` — Metres in front of the camera (negative: behind it).
- `Frame { get; init; } -> ulong` — The snapshot's frame.
- `OnScreen { get; init; } -> bool` — Inside the viewport.
- `Pixel { get; init; } -> Vector2?` — Where; null when it is not within depth or the number does not fit a pixel.
- `WithinDepth { get; init; } -> bool` — Between the near and the far plane.

## CameraReason `0.2.17` — Why a request was refused.
enum: None, Busy, AlreadyOwned, UnknownLease, ForeignOwner, WrongEndpoint, InputPolicyUnsupported, RequiresKilnClient, PeerUnavailable, Negotiation, TargetUnavailable, NoCameraFrame, StaleWorld, ExpiredUpdate, Collision, TransportTimeout, MalformedRemote, ResourceBusy, FrameFault, SnapshotBusy, SnapshotStale, Bounds, RateLimit, LeaseKind, Cancelled, ClientCapability, InsufficientLifetime, BodyTooLarge

## CameraResult `0.2.17` — The answer to a camera request.
- `CameraResult(bool Ok, string? Reason, CameraReason ReasonCode, CameraLease Lease, ulong Revision, CameraCommitScope CommitScope, bool MayHaveApplied = false, bool AlreadyEnded = false)`
- `AlreadyEnded { get; init; } -> bool` — `Camera.End` of a lease that had ended already; no second change event follows.
- `CommitScope { get; init; } -> CameraCommitScope` — What an Ok result proves.
- `Lease { get; init; } -> CameraLease` — The lease.
- `MayHaveApplied { get; init; } -> bool` — Not confirmed, but a frame may have shown it: end the lease.
- `Ok { get; init; } -> bool` — True when it happened: see `CommitScope` for what that proves.
- `Reason { get; init; } -> string?` — Why not, in words for a log; null when `Ok`.
- `ReasonCode { get; init; } -> CameraReason` — Why not, to decide by.
- `Revision { get; init; } -> ulong` — The lease's revision in force: 1 after the first apply, higher after each update.

## CameraRotationMode `0.2.17` — Where a follow profile takes its orientation from.
enum: FreeOrbit, FollowHeading, FixedWorld

## CameraShake `0.2.17` — A shake that moves the camera without turning it.
- `CameraShake(float AmplitudeMetres, float FrequencyHz, TimeSpan Falloff, uint Seed = 0)`
- `AmplitudeMetres { get; init; } -> float` — The largest displacement, 0 to 0.5 metres.
- `Falloff { get; init; } -> TimeSpan` — The shake fades over this last part of the duration; zero: full strength until the end.
- `FrequencyHz { get; init; } -> float` — 0.1 to 30.
- `Seed { get; init; } -> uint` — Picks the shake's phases.

## CameraTransitionOverride `0.2.17` — How the game moves into a locked stock mode.
enum: None, BlinkLerpSpeed, CameraTransitionLerpSpeed

## CameraUpdateTiming `0.2.17` — What `Camera.Update` does with the lease's clock.
enum: Preserve, Restart

## CameraVisualResult `0.2.17` — The answer of `Camera.GetVisual`.
- `CameraVisualResult(bool Ok, string? Reason, CameraReason ReasonCode, CameraVisualSnapshot? Snapshot)`
- `Ok { get; init; } -> bool`
- `Reason { get; init; } -> string?`
- `ReasonCode { get; init; } -> CameraReason`
- `Snapshot { get; init; } -> CameraVisualSnapshot?`

## CameraVisualSnapshot `0.2.17` — The camera a frame was drawn with: after the game's own shake and after any camera override.
- `CameraVisualSnapshot(CameraPose Pose, float FieldOfViewDegrees, float AspectRatio, float NearPlane, float FarPlane, ulong Frame, ulong TargetSampleStep, uint SessionGeneration, TimeSpan Age)`
- `Age { get; init; } -> TimeSpan` — How old the snapshot was when it was read; never above 250 ms.
- `AspectRatio { get; init; } -> float`
- `FarPlane { get; init; } -> float`
- `FieldOfViewDegrees { get; init; } -> float`
- `Frame { get; init; } -> ulong` — Counts the frames this was taken from.
- `NearPlane { get; init; } -> float`
- `Pose { get; init; } -> CameraPose`
- `Project(WorldPosition point, Vector2 viewportOrigin, Vector2 viewportSize) -> CameraProjection` — Where `point` is on the screen of this frame's camera: the logical view the frame was drawn with (no anti-aliasing...
- `SessionGeneration { get; init; } -> uint` — The client world it belongs to.
- `TargetSampleStep { get; init; } -> ulong` — The client step whose positions the frame's camera was built from.
