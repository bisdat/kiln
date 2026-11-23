# Queries (Kiln.Queries)

## EntityQueryGeometry `0.2.3` — What of an entity an overlap question looks at.
enum: Origin

## EntityQueryResult `0.2.3` — The entities whose origin is inside the shape, nearest first (then by id).
- `EntityQueryResult(bool Ok, string? Reason, IReadOnlyList<EntityId> Entities, int Total, bool Truncated)`
- `Entities { get; init; } -> IReadOnlyList<EntityId>` — The matching entities, nearest first, at most the `max` asked for.
- `Ok { get; init; } -> bool` — True when the service answered.
- `Reason { get; init; } -> string?` — Why nothing is known; null when `EntityQueryResult.Ok`.
- `Total { get; init; } -> int` — How many entities matched in all.
- `Truncated { get; init; } -> bool` — True when more matched than `Entities` holds.

## PathQueryChangedEvent `0.2.3` — A path request of this mod moved on: queued, searching, or ended (`PathQueryChangedEvent.TerminalStatus`).
- `PathQueryChangedEvent(ulong request, uint generation, EntityId anchor, QueryPathState before, QueryPathState after, QueryPathStatus? terminalStatus, string? reason)`
- `After { get; } -> QueryPathState` — The state the request entered.
- `Anchor { get; } -> EntityId` — The entity the request was made for (`QueryPathRequest.Anchor`).
- `Before { get; } -> QueryPathState` — The state the request left.
- `Generation { get; } -> uint` — The server world the request belongs to (worlds since the process started).
- `Reason { get; } -> string?` — Why the request ended without a route; null while it is not over, and when it ended with a route.
- `Request { get; } -> ulong` — The request's id (`QueryPathTicket.Request`).
- `TerminalStatus { get; } -> QueryPathStatus?` — How the request ended; null while it is not over.

## Queries `0.2.3` — Questions about the server world, answered at once on the tick thread.
- `static CancelPath(ulong request) -> QueryCancelResult` — Cancels a request of the calling mod that has not ended.
- `static CanReach(QueryPathRequest request, CancellationToken cancellationToken = default(CancellationToken)) -> Task<QueryReachabilityResult>` — Whether `To` can be walked to from `From`: the same search as `Queries.FindPath`, its route thrown away.
- `static EntitiesInBox(WorldPosition center, WorldPosition halfExtents, EntityId exclude = default(EntityId), int max = 4096) -> EntityQueryResult` — The entities whose origin is inside the box around `center` that reaches `halfExtents` along each world axis...
- `static EntitiesInSphere(WorldPosition center, double radius, EntityId exclude = default(EntityId), int max = 4096) -> EntityQueryResult` — The entities whose origin is at most `radius` metres from `center`, nearest first.
- `static FindPath(QueryPathRequest request, CancellationToken cancellationToken = default(CancellationToken)) -> Task<QueryPathResult>` — `Queries.RequestPath` as a task.
- `static GroundBelow(WorldPosition from, double maxDown = 100, byte realm = 0) -> VoxelRayResult` — The first voxel surface below `from`, at most `maxDown` metres down: a roof or a floor above the terrain is that...
- `const MaxEntities = 4096 -> int` — The most entities an overlap returns.
- `const MaxPathDistance = 200 -> double` — The farthest apart `From` and `To` of a path request may be, in metres.
- `const MaxRayLength = 256 -> double` — The longest ray, in metres.
- `static RaycastVoxels(WorldPosition from, WorldPosition to, byte realm = 0) -> VoxelRayResult` — The first voxel surface on the segment from `from` to `to`.
- `static RequestPath(QueryPathRequest request) -> QueryPathTicket` — Asks for a walking route.
- `static VoxelLineOfSight(EntityId from, EntityId to, VoxelSightOptions? options = null) -> VoxelSightResult` — Whether voxels stand between two entities: a voxel ray from one's position to the other's, each with its offset...

## QueryCancelResult `0.2.3` — The answer of `Queries.CancelPath`.
- `QueryCancelResult(bool Ok, string? Reason, ulong Request)`
- `Ok { get; init; } -> bool` — True when a request of the calling mod that had not ended was ended as `QueryPathStatus.Cancelled`.
- `Reason { get; init; } -> string?` — Why not; `UnknownRequest` when the request is unknown, another mod's or already ended.
- `Request { get; init; } -> ulong` — The request id that was asked about.

## QueryPathMove `0.2.3` — How a point of a route is reached from the one before it (the game's PathMoveType).
enum: Walk, Fly, JumpUp, JumpDown, JumpAcross, OpenDoor, Swim, Dive, WalkUnderWater, FlyThroughWater

## QueryPathPoint `0.2.3` — One point of a route.
- `QueryPathPoint(WorldPosition Position, byte RawMove)`
- `Move { get; } -> QueryPathMove?` — How this point is reached from the one before it; null when `QueryPathPoint.RawMove` is not one of the known...
- `Position { get; init; } -> WorldPosition` — Where the point is in the world.
- `RawMove { get; init; } -> byte` — The game's move byte as it came; `QueryPathPoint.Move` when Kiln knows it.

## QueryPathRequest `0.2.3` — A question to the game's path search: a walking route from `From` to `To`, at most 200 m apart.
- `QueryPathRequest(EntityId Anchor, WorldPosition From, WorldPosition To)`
- `Anchor { get; init; } -> EntityId` — An entity the game's query is made for.
- `ClosestFallback { get; init; } -> bool` — When the target has no node, allow the closest one.
- `From { get; init; } -> WorldPosition` — Where the route starts.
- `HorizontalSpace { get; init; } -> double` — Free space the route needs sideways, in metres, as the game's query takes it (0 to 16; rounded up to half metres).
- `MaxIterations { get; init; } -> uint` — The search's iteration limit, 1 to 65535.
- `To { get; init; } -> WorldPosition` — Where the route should end.
- `VerticalSpace { get; init; } -> double` — Free space the route needs upwards, in metres.

## QueryPathResult `0.2.3` — A route as it was at the step the game answered: a copy, nothing live.
- `QueryPathResult(bool Ok, string? Reason, ulong Request, uint Generation, QueryPathStatus Status, byte? GameResult, bool ReachesTarget, IReadOnlyList<QueryPathPoint> Points, double Length, uint Steps)`
- `GameResult { get; init; } -> byte?` — The game's own result code, when it gave one.
- `Generation { get; init; } -> uint` — The server world the request belongs to (worlds since the process started); 0 when it was refused at once.
- `Length { get; init; } -> double` — Metres along `Points`, for a found or partial route.
- `Ok { get; init; } -> bool` — True when the game gave a walking route: `QueryPathStatus.Found` or `QueryPathStatus.Partial`.
- `Points { get; init; } -> IReadOnlyList<QueryPathPoint>` — The route's points from start to end, a copy.
- `ReachesTarget { get; init; } -> bool` — True when the route's last point is at the target (`QueryPathStatus.Found`); false for a `QueryPathStatus.Partial`...
- `Reason { get; init; } -> string?` — Why there is no route (the game's word, or the refusal); null when `QueryPathResult.Ok`.
- `Request { get; init; } -> ulong` — The request's id (`QueryPathTicket.Request`); 0 when it was refused at once.
- `Status { get; init; } -> QueryPathStatus` — How the request ended.
- `Steps { get; init; } -> uint` — Server steps the request was worked on.

## QueryPathState `0.2.3` — Where a path request is.
enum: Created, Queued, Searching, Completed, Cancelled, TimedOut

## QueryPathStatus `0.2.3` — How a path request ended (or `QueryPathStatus.Pending`).
enum: Pending, Found, Partial, NoPath, NoStartNode, NoTargetNode, IterationLimit, OutOfMemory, NotLoadedOrUnknown, Cancelled, OwnerGone, TimedOut, Truncated, Refused, UnsupportedTraversal, MalformedResult, QueueFull, OwnerIdentityUnavailable, OutOfGrid, WorldClosed, OwnerRetired

## QueryPathTicket `0.2.3` — An admitted request (`QueryPathTicket.Ok`: `QueryPathTicket.Completion` ends some steps later) or a refusal (`QueryPathTicket.Completion` is complete already, with...
- `QueryPathTicket(bool Ok, string? Reason, ulong Request, Task<QueryPathResult> Completion)`
- `Completion { get; init; } -> Task<QueryPathResult>` — The task that ends with the result: some server steps later when admitted, already complete when refused.
- `Ok { get; init; } -> bool` — True when the request was admitted.
- `Reason { get; init; } -> string?` — Why the request was refused; null when `QueryPathTicket.Ok`.
- `Request { get; init; } -> ulong` — The request's id, for `Queries.CancelPath` and to match `PathQueryChangedEvent.Request`; 0 when refused.

## QueryReachabilityResult `0.2.3` — Whether the target can be walked to.
- `QueryReachabilityResult(bool Ok, string? Reason, bool Reachable, QueryPathStatus Status, byte? GameResult)`
- `GameResult { get; init; } -> byte?` — The game's own result code, when it gave one.
- `Ok { get; init; } -> bool` — True when the search gave an answer.
- `Reachable { get; init; } -> bool` — True when a walking route arrives at the target.
- `Reason { get; init; } -> string?` — Why nothing is known; null when `QueryReachabilityResult.Ok`.
- `Status { get; init; } -> QueryPathStatus` — How the search that answered ended.

## VoxelRayHit `0.2.3` — Where a voxel ray met the first voxel surface: terrain or building voxels, nothing else.
- `VoxelRayHit(WorldPosition Position, double Distance, uint Material, ushort Voxel)`
- `Distance { get; init; } -> double` — Metres from the ray's start to `Position`.
- `Material { get; init; } -> uint` — The game's material id of the voxel.
- `Position { get; init; } -> WorldPosition` — Where the segment meets the voxel.
- `Voxel { get; init; } -> ushort` — The raw packed voxel.

## VoxelRayResult `0.2.3` — A voxel ray's answer.
- `VoxelRayResult(bool Ok, string? Reason, VoxelRayHit? Hit)`
- `Hit { get; init; } -> VoxelRayHit?` — The first voxel surface on the segment; null when it is clear, and when not `VoxelRayResult.Ok`.
- `Ok { get; init; } -> bool` — True when the segment was read: `Hit` is the answer.
- `Reason { get; init; } -> string?` — Why nothing is known; null when `VoxelRayResult.Ok`.

## VoxelSightOptions `0.2.3` — From where to where `Queries.VoxelLineOfSight` looks: offsets in metres along the world's axes (Y up) added to each entity's position.
- `FromOffset { get; init; } -> WorldPosition` — Metres added to the first entity's position for the ray's start.
- `Realm { get; init; } -> byte` — The realm both entities are in.
- `ToOffset { get; init; } -> WorldPosition` — Metres added to the second entity's position for the ray's end.

## VoxelSightResult `0.2.3` — Whether voxels stand between two entities.
- `VoxelSightResult(bool Ok, string? Reason, bool Visible, VoxelRayHit? Obstruction)`
- `Obstruction { get; init; } -> VoxelRayHit?` — The first voxel surface in the way; null when `Visible`, and when not `VoxelSightResult.Ok`.
- `Ok { get; init; } -> bool` — True when the line was read: `Visible` is the answer.
- `Reason { get; init; } -> string?` — Why nothing is known; null when `VoxelSightResult.Ok`.
- `Visible { get; init; } -> bool` — True when no voxel stands between the two entities.
