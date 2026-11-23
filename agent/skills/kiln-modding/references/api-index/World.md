# World (Kiln.World)

## AttributeValueChangedEvent `0.2.4` — The typed face of a `ComponentValueChangedEvent` of Health, Stamina, Mana or Fuel whose current or maximum value changed: the values as the game's attribute code...
- `AttributeValueChangedEvent(ComponentValueChangedEvent change, ObservedAttribute attribute, int before, int after, int maximumBefore, int maximumAfter)`
- `After { get; } -> int` — The current value after.
- `Attribute { get; } -> ObservedAttribute` — Which attribute.
- `Before { get; } -> int` — The current value before (the game's signed integer; Fuel in units).
- `Change { get; } -> ComponentValueChangedEvent` — The raw change it is about.
- `Involves(EntityId entity) -> bool`
- `MaximumAfter { get; } -> int` — The maximum after.
- `MaximumBefore { get; } -> int` — The maximum before.

## ChangeOrigin `0.2.4` — Who changed a component (`ComponentValueChangedEvent.Origin`).
enum: Unknown, Game, Kiln, Mixed

## ClientComponentWrittenEvent `0.2.4` — A mod's client or prediction program (`EcsPrograms.TryBind`) changed a component of an entity of this player's client world (or prediction storage) through...
- `After { get; } -> ReadOnlyMemory<byte>` — The bytes written.
- `Before { get; } -> ReadOnlyMemory<byte>` — The component's bytes before the write.
- `Component { get; } -> string` — The component's short name, as the program wrote it.
- `Entity { get; } -> EntityId` — The entity (an id of that client world: the same number may name another entity in the server world).
- `Involves(EntityId entity) -> bool`
- `World { get; } -> WorldKind` — `WorldKind.Client`, or `WorldKind.Prediction` for a prediction program.
- `Writer { get; } -> string` — Who wrote: `"<modid>/program:<key>"` (the program's key in the mod's kiln.programs.json).

## ComponentChanges `0.2.4` — Named component watches.
- `const Capability = "events.component_net" -> string` — The capability row: `events.component_net`.
- `static Resume(ComponentWatch watch) -> ComponentWatchResult` — Resumes a suspended watch from scratch: its roots must still be the same lives, and it must fit the budgets again.
- `static Watch(ComponentWatchRequest request) -> ComponentWatchResult` — Starts a watch.

## ComponentValueChangedEvent `0.2.4` — A watched component changed (`ComponentChanges.Watch`): its bytes before and after the step's interval (from one compare to the next: the simulation and the Post and...
- `ComponentValueChangedEvent(EventEntityKey key, string component, byte[] before, byte[] after, ulong step, ChangeOrigin origin, string? sourceMod, IEnumerable<ulong> tokens, IEnumerable<EntityId> roots, IEnumerable<ulong> watches)` — Makes the event (Kiln posts it; a mod or a test may make one to test its handler).
- `After { get; } -> ReadOnlyMemory<byte>` — The bytes after (a copy); empty when the component is absent now or larger than 256 bytes.
- `Before { get; } -> ReadOnlyMemory<byte>` — The bytes before (a copy); empty when the component was absent or larger than 256 bytes.
- `Component { get; } -> string` — The component's short name: Health, Stamina, Mana, Fuel, Inventory or SlotSelection.
- `Entity { get; } -> EntityId` — The entity whose component changed.
- `Involves(EntityId entity) -> bool`
- `IsPresent { get; } -> bool` — True when the component is there now.
- `Key { get; } -> EventEntityKey` — Which life of the entity: a root's is kiln.decisions' key (its spawn time or player session); an entity reached...
- `OperationTokens { get; } -> IReadOnlyList<ulong>` — The operation tokens of the writes Kiln's writers reported in the interval (at most 32).
- `Origin { get; } -> ChangeOrigin` — Who changed it, as far as Kiln knows (`ChangeOrigin`).
- `RootEntities { get; } -> IReadOnlyList<EntityId>` — The watched roots this entity belongs to (itself, or the players whose inventory holds it).
- `SourceMod { get; } -> string?` — The reporting sources (a mod's id, or a Kiln service such as `kiln.combat`), several joined by `", "`; the native...
- `Step { get; } -> ulong` — The step whose compare saw it.
- `WasPresent { get; } -> bool` — True when the component was there before.
- `WatchIds { get; } -> IReadOnlyList<ulong>` — The watches that cover the pair.
- `World { get; } -> WorldKind` — The world: always `WorldKind.Server` in this version.

## ComponentWatch `0.2.4` — One watch (`ComponentChanges.Watch`).
- `Dispose() -> void` — Stops the watch: no change of it is reported from the next step on.
- `Id { get; } -> ulong` — The service's id of the watch (on `ComponentValueChangedEvent.WatchIds`).
- `LastCompleteStep { get; } -> ulong` — The last step whose changes it reported completely.
- `Pairs { get; } -> int` — The present (entity, component) pairs it covered at its last refresh.
- `Reason { get; } -> string?` — Why it is suspended or ended; null while ready.
- `Request { get; } -> ComponentWatchRequest` — What it was made for.
- `State { get; } -> ComponentWatchState` — Its state as of this step's Pre (a change raises `ComponentWatchStateChangedEvent`).
- `World { get; } -> WorldKind` — The world it watches: always `WorldKind.Server` in this version.

## ComponentWatchExpansion `0.2.4` — Whether a watch reaches past its roots.
enum: None, PlayerInventoryAndItems

## ComponentWatchRequest `0.2.4` — What to watch (`ComponentChanges.Watch`).
- `ComponentWatchRequest(ComponentWatchScope Scope, IReadOnlyList<EntityId> Entities, ObservedComponents Components, ComponentWatchExpansion Expansion = ComponentWatchExpansion.None)`
- `Components { get; init; } -> ObservedComponents` — A nonempty set of the six.
- `Entities { get; init; } -> IReadOnlyList<EntityId>` — The roots for `ComponentWatchScope.Entities`: 1 to 128 distinct entities; empty for `ComponentWatchScope.Players`.
- `Expansion { get; init; } -> ComponentWatchExpansion` — Whether linked inventories and their item entities are watched too.
- `Scope { get; init; } -> ComponentWatchScope` — The listed entities, or every player in the game.

## ComponentWatchResult `0.2.4` — The answer of `ComponentChanges.Watch` and `ComponentChanges.Resume`.
- `ComponentWatchResult(bool Ok, string? Reason, ComponentWatch? Watch, int Pairs)`
- `Ok { get; init; } -> bool` — True: the watch is ready.
- `Pairs { get; init; } -> int` — The present (entity, component) pairs it covers, or would have needed.
- `Reason { get; init; } -> string?` — Why not (a budget, a root that cannot be followed, no server world); null when `Ok`.
- `Watch { get; init; } -> ComponentWatch?` — The watch (for Watch: null when it was not made; for Resume: the same object).

## ComponentWatchScope `0.2.4` — What a watch follows.
enum: Entities, Players

## ComponentWatchState `0.2.4` — A watch's state (`ComponentWatch.State`, `ComponentWatchStateChangedEvent`).
enum: Ready, SuspendedBudget, SuspendedMetadata, Disposed

## ComponentWatchStateChangedEvent `0.2.4` — A watch was suspended or ended by Kiln (a budget, a root that cannot be followed, a queue that was not read, the world's end, its mod's unload).
- `ComponentWatchStateChangedEvent(ulong watchId, ComponentWatchState state, string? reason, ulong lastCompleteStep)`
- `LastCompleteStep { get; } -> ulong` — The last step whose changes it reported completely.
- `Reason { get; } -> string?` — Why.
- `State { get; } -> ComponentWatchState` — Its new state.
- `WatchId { get; } -> ulong` — The watch.

## ComponentWrittenEvent `0.2.2` — A mod changed a component: posted on the tick thread when a tracked write's answer arrives, one step after the call (two when the game writes the type too), for every...
- `Component { get; } -> string` — The component's short name.
- `Entity { get; } -> EntityId` — The entity whose component changed.
- `Involves(EntityId entity) -> bool`
- `ModId { get; } -> string?` — The id of the mod that wrote; null for Kiln's own tools.
- `Result { get; } -> WorldWriteResult` — The write's answer: the bytes before and after, who else writes the type, whether the save tracks it.
- `Writer { get; } -> string?` `0.2.4` — Who wrote: the mod's id for a tracked write through `World` (null for Kiln's own tools), or...
- `Writers { get; } -> IReadOnlyList<string>` `0.2.4` — Every writer of the component in that step, `ComponentWrittenEvent.Writer` first, each once (one entry for a tracked...

## EntityScaleChangedEvent `0.2.3` — An entity's transform scale changed through `World.SetTransformScale`: posted for a write that was committed and changed the vector (none for...
- `Before { get; } -> Scale3` — The scale before.
- `Entity { get; } -> EntityId` — The entity.
- `Involves(EntityId entity) -> bool`
- `ModId { get; } -> string?` — The id of the mod that set it; null for Kiln's own tools.
- `Now { get; } -> Scale3` — The scale now.
- `Result { get; } -> EntityScaleResult` — The whole answer.

## EntityScaleResult `0.2.3` — The answer to `World.SetTransformScale`: the tracked write's, with the scale before and after decoded.
- `EntityScaleResult(EntityId Entity, Scale3? Before, Scale3 Requested, Scale3? Now, bool? Kept, WorldWriteStatus WorldWriteStatus, string? Reason, bool? SaveTracked, bool? ReplicationTracked)`
- `Before { get; init; } -> Scale3?` — The scale the entity had right before the write, in the step the write ran; null when nothing was read (refused...
- `Changed { get; } -> bool` — True when the scale changed (`WorldWriteStatus.Written` and another value than before).
- `Entity { get; init; } -> EntityId` — The entity.
- `Kept { get; init; } -> bool?` — For a component the game writes too: whether the scale was still there one step later (as `WorldWriteResult.Kept`)...
- `Now { get; init; } -> Scale3?` — The scale right after the write; null when nothing was read.
- `Ok { get; } -> bool` — True when the entity's transform holds the scale now: written, or it held it already.
- `Persisted { get; init; } -> bool?` — Whether Kiln recorded this scale for replay after a save and restart.
- `Reason { get; init; } -> string?` — Why it did not happen; null when `EntityScaleResult.Ok`.
- `ReplicationTracked { get; init; } -> bool?` — Whether it records it for replication (`WorldWriteResult.ReplicationTracked`); null when this build does not tell.
- `Requested { get; init; } -> Scale3` — The scale asked for.
- `SaveTracked { get; init; } -> bool?` — Whether the game's write tracking records CurrentTransform for the save (the write's own answer...
- `WorldWriteStatus { get; init; } -> WorldWriteStatus` — What became of the tracked write.

## EntitySet `0.2.2` — The entities of the server world that have every component of `EntitySet.With` and none of `EntitySet.Without`, as of the last `EntitySet.Refresh`.
- `const Capability = "world.find" -> string` — The capability entity sets need: `world.find` (kiln.world's pass over the entity map).
- `Contains(EntityId entity) -> bool` — Whether `entity` was in the set at the last `EntitySet.Refresh`.
- `Count { get; } -> int`
- `Entities { get; } -> IReadOnlyList<EntityId>` — The entities, in no particular order.
- `GetEnumerator() -> IEnumerator<EntityId>`
- `Refresh() -> EntitySet` — Reads the set again from the world.
- `With { get; } -> IReadOnlyList<string>` — The components every entity of the set has (short names).
- `Without { get; } -> IReadOnlyList<string>` — The components no entity of the set has.

## EntityTransform — An entity's world transform: position in metres, rotation, scale.
- `EntityTransform()`
- `EntityTransform(WorldPosition Position, Quaternion Rotation, Vector3 Scale)`
- `Position { get; init; } -> WorldPosition`
- `Rotation { get; init; } -> Quaternion`
- `Scale { get; init; } -> Vector3`
- `YawDegrees { get; } -> double` — Heading about +Y (up) in degrees, 0..360.

## Nearby — Entities near a point or another entity.
- `static Entities(EntityId center, double radius, string component) -> IReadOnlyList<EntityId>` — Entities with the per-entity component `component` within `radius` metres of `center`, nearest first; `center`...
- `static Entities(WorldPosition at, double radius, string component) -> IReadOnlyList<EntityId>` — Entities with `component` within `radius` metres of a point, nearest first.
- `static Within(IEnumerable<EntityId> candidates, EntityId center, double radius) -> IReadOnlyList<EntityId>` — The entities of `candidates` within `radius` metres of `center`, nearest first (center left out).
- `static Within(IEnumerable<EntityId> candidates, WorldPosition at, double radius) -> IReadOnlyList<EntityId>` — The entities of `candidates` within `radius` metres of a point, nearest first.

## ObservedAttribute `0.2.4` — An attribute whose typed values `AttributeValueChangedEvent` carries.
enum: Health, Stamina, Mana, Fuel

## ObservedComponents `0.2.4` — The components a `ComponentWatchRequest` can name (a set).
enum: None, Health, Stamina, Mana, Fuel, Inventory, SlotSelection, All

## ProgramAccessLimits `0.2.4` — The engine's limits on one program's access lists, as kiln.world checks them when a program is registered.
- `ProgramAccessLimits(uint Templates, uint ReadWrite, uint SingletonReads, uint SingletonWrites, uint OtherTemplates, uint OtherAccessors, uint EventsInOut, uint IteratorBytes)`
- `EventsInOut { get; init; } -> uint` — Event inputs and outputs together.
- `IteratorBytes { get; init; } -> uint` — The largest iterator.
- `OtherAccessors { get; init; } -> uint` — Other-entity reads and writes together.
- `OtherTemplates { get; init; } -> uint` — Other-entity template lists.
- `ReadWrite { get; init; } -> uint` — Read and write lists, with their optional ones, together.
- `SingletonReads { get; init; } -> uint` — Singletons read.
- `SingletonWrites { get; init; } -> uint` — Singletons written.
- `Templates { get; init; } -> uint` — Template lists (`tpl` and `tplOptional` together).

## ProgramBudgetResult `0.2.4` — The answer of `World.GetProgramBudget`: two budgets for the same world, read together.
- `ProgramBudgetResult(bool Ok, string? Reason, CapacityWorldToken? World, CapacityInfo KilnSlots, CapacityInfo EngineSlots, uint PerFileMax, ProgramAccessLimits AccessLimits)`
- `AccessLimits { get; init; } -> ProgramAccessLimits` — Each program's access limits (always filled).
- `EngineSlots { get; init; } -> CapacityInfo` — The world's own program table (896 entries): `CapacityInfo.GameUsed` the game's programs, `CapacityInfo.KilnUsed`...
- `KilnSlots { get; init; } -> CapacityInfo` — Kiln's own quota, for the process: 64 programs for Kiln's services, compat loaders and mods together.
- `Ok { get; init; } -> bool` — True when both budgets are ready for a live world.
- `PerFileMax { get; init; } -> uint` — Declarations a mod's `kiln.programs.json` may hold (32).
- `Reason { get; init; } -> string?` — Null when `Ok`; else the readiness name (`NoWorld`, `NotReady`, `AccountingMismatch`).
- `World { get; init; } -> CapacityWorldToken?` — The world the answer is for while it lives (also when it is not ready yet); null when there is no world of that kind.

## Scale3 `0.2.3` — The three scale factors of a transform (`keen::float3`): X, Y, Z, 12 bytes, sequential.
- `Scale3()`
- `Scale3(float X, float Y, float Z)`
- `IsValid { get; } -> bool` — Every factor is finite and above zero (what `World.SetTransformScale` takes).
- `static One { get; } -> Scale3` — 1, 1, 1: the size the game gave.
- `static Uniform(float factor) -> Scale3` — The same factor on every axis.
- `X { get; init; } -> float` — The factor along the entity's X axis.
- `Y { get; init; } -> float` — Along Y (up).
- `Z { get; init; } -> float` — Along Z.

## Teleport — Moves entities the way the game's own fast travel does (the engine's Teleport component): players, villagers, animals and creatures.
- `static Available { get; } -> bool` — True when teleport works on this build and the server world is running.
- `static To(EntityId entity, WorldPosition target, TeleportOptions? options = null) -> Task<TeleportResult>` — Teleports `entity` to `target` (metres).
- `static ToEntity(EntityId entity, EntityId target, double offset = 1.5, TeleportOptions? options = null) -> Task<TeleportResult>` — Teleports `entity` next to `target` (e.g. a player to another player): `offset` metres beside it along +X, with the...
- `static ToSpawnPoint(EntityId entity, EntityId spawnPoint, TeleportOptions? options = null) -> Task<TeleportResult>` — Teleports `entity` onto a landing pad of a spawn point (an entity with PlayerSpawnPoint: flame altar, spire), as...

## TeleportFlags
enum: None, OffTarget, RawTarget, TimedOut

## TeleportOptions — Options for `Teleport`.
- `Checkpoint { get; init; } -> bool` — Players: also set a replication checkpoint, as the game's fast travel does (for long moves).
- `static readonly Default -> TeleportOptions`
- `FindGround { get; init; } -> bool` — The game's ground search: the nearest walkable spot within about 10 m (up or down) of the target.
- `YawDegrees { get; init; } -> double?` — Face this heading (degrees about +Y) on arrival; null keeps the current orientation.

## TeleportResult — Outcome of one teleport.
- `TeleportResult(TeleportStatus Status, EntityId Entity, WorldPosition Position, WorldPosition Target, double Distance, TeleportFlags Flags, int Steps, string Reason)`
- `Distance { get; init; } -> double`
- `Entity { get; init; } -> EntityId`
- `Flags { get; init; } -> TeleportFlags`
- `Moved { get; } -> bool`
- `Ok { get; } -> bool` `0.2.2` — The entity was moved: `TeleportResult.Moved` (the contract's name for it).
- `Position { get; init; } -> WorldPosition`
- `Reason { get; init; } -> string`
- `Status { get; init; } -> TeleportStatus`
- `Steps { get; init; } -> int`
- `Target { get; init; } -> WorldPosition`

## TeleportStatus
enum: Failed, Moved

## WaitingTransformScale `0.2.4` — A kept transform scale currently waiting for positional identity.
- `WaitingTransformScale(string Mod, Guid? Template, WorldPosition Position, Scale3 Scale, string Reason)`
- `Mod { get; init; } -> string` — The mod that last set the scale.
- `Position { get; init; } -> WorldPosition` — The recorded place used for matching.
- `Reason { get; init; } -> string` — Why this record is waiting.
- `Scale { get; init; } -> Scale3` — The scale kept for replay.
- `Template { get; init; } -> Guid?` — The recorded entity template, if known.

## World — The server world's entities (kiln.world).
- `static Available { get; } -> bool` — True when kiln.world is loaded on this server build.
- `static ComponentHash(string name) -> uint` — FNV-1a-32 of `"keen::ecs::" + name` (the name as given when it already contains "::"): the engine's component hash...
- `static ComponentSize(EntityId entity, string component) -> int` — Size in bytes of the entity's component, or 0 when it has none.
- `static EntityCount { get; } -> int` — Entities in the server world.
- `static Exists(EntityId entity) -> bool` — True if the entity exists (has a world transform).
- `static Find(string component) -> IReadOnlyList<EntityId>` — Every entity that has the per-entity component `component` (one pass over the entity map).
- `static GetProgramBudget(CapacityWorldKind kind = CapacityWorldKind.Server) -> ProgramBudgetResult` `0.2.4` — The room for world programs, for the live world of `kind`: Kiln's own 64 program slots (process wide: Kiln's...
- `static GetWaitingTransformScales() -> IReadOnlyList<WaitingTransformScale>` `0.2.4` — Kept scales waiting for the calling mod (all mods in service context), on the tick thread.
- `static Has(EntityId entity, string component) -> bool` — True if the entity has the per-entity component `component`.
- `static Position(EntityId entity) -> WorldPosition?` — The entity's position in metres, or null when it does not exist or has no transform.
- `static Query(IEnumerable<string> with, IEnumerable<string>? without) -> EntitySet` `0.2.2` — The entities that have every one of `with` and none of `without`, read now.
- `static Query(params string[] with) -> EntitySet` `0.2.2` — The entities that have every one of `with` (component short names), read now.
- `static Query<T1, T2>() -> EntitySet` `0.2.2` — The entities that have both `T1` and `T2`, read now.
- `static Query<T>() -> EntitySet` `0.2.2` — The entities that have component `T`, read now.
- `static ReadBytes(EntityId entity, string component) -> byte[]?` — A copy of the entity's component bytes, or null.
- `static SetTransformScale(EntityId entity, Scale3 scale) -> Task<EntityScaleResult>` `0.2.3` — Sets the three scale factors of `entity`'s `CurrentTransform` through the game's write tracking: one tracked write...
- `static Status { get; } -> WorldStatus` — What works now (programs, a live server world, teleport, find).
- `static SupportsTrackedWrites { get; } -> bool` `0.2.2` — True when tracked writes work in this process right now (`Capabilities.Why` of `World.TrackedWritesCapability` says...
- `const TrackedWritesCapability = "world.tracked_writes" -> string` `0.2.2` — The capability of tracked writes: `world.tracked_writes`.
- `static Transform(EntityId entity) -> EntityTransform?` — The entity's CurrentTransform, or null.
- `const TransformScaleCapability = "world.transform_scale" -> string` `0.2.3` — The capability of `World.SetTransformScale`: `world.transform_scale`.
- `static TryGetPosition(EntityId entity, out WorldPosition position) -> bool`
- `static TryRead<T>(EntityId entity, string component, out T value) -> bool` — Copies the first `sizeof(T)` bytes of the entity's component into `value`.
- `static TryWrite<T>(EntityId entity, string component, int offset, T value) -> bool` — Copies `value` into the entity's component at `offset`, as a raw store: the game is not told.
- `static Update<T>(EntityId entity, WorldChange<T> change) -> Task<WorldWriteResult>` `0.2.2` — Changes component `T` of `entity`: `change` gets the value the entity holds after the game's own programs of the...
- `static Write<T>(EntityId entity, T value) -> Task<WorldWriteResult>` `0.2.2` — Sets component `T` of `entity` to `value` through the game's write tracking, so the game's save and its replication...
- `static WriteAt<T>(EntityId entity, string component, int offset, T value) -> Task<WorldWriteResult>` `0.2.2` — Stores `value` at `offset` of a component through the game's write tracking (see `World.Write`): the tracked form of...
- `static WriteField(EntityId entity, string component, string path, object? value) -> Task<WorldWriteResult>` `0.2.2` — Sets one field of a component by its path in the game's own type (`"dataStorage[0]"`, `"scale"`), checked against...
- `static WriteInfo(string component) -> WorldWriteInfo` `0.2.2` — What kiln.world knows about writing component `component` from the calling mod: whether it is listed, which of the...
- `static WriteInfo<T>() -> WorldWriteInfo` `0.2.2` — `World.WriteInfo` for a generated component type.
- `static WriteRaw<T>(EntityId entity, string component, int offset, T value) -> bool` `0.2.2` — Copies `value` into the entity's component at `offset` at once, as a raw store: the game's write tracking is not...

## `WorldChange<T>` `0.2.2` — A change to a component, made on the value the entity holds at that moment (`World.Update`).
- `Invoke(ref T value) -> void`

## WorldStatus — What kiln.world can do on this server build right now.
- `WorldStatus()`
- `WorldStatus(bool Programs, bool WorldLive, bool Teleport, bool Find, int KilnPrograms, int EnginePrograms, int EngineMax, int TeleportsPending, ulong WorldSteps)`
- `EngineMax { get; init; } -> int`
- `EnginePrograms { get; init; } -> int`
- `Find { get; init; } -> bool`
- `KilnPrograms { get; init; } -> int`
- `Programs { get; init; } -> bool`
- `Teleport { get; init; } -> bool`
- `TeleportsPending { get; init; } -> int`
- `WorldLive { get; init; } -> bool`
- `WorldSteps { get; init; } -> ulong`

## WorldWriteInfo `0.2.2` — What kiln.world knows about writing one component type (`World.WriteInfo`).
- `WorldWriteInfo(string Component, WorldWriteState State, string? Reason, int GameWriterCount, IReadOnlyList<string> GameWriters, int InitWriterCount, bool? SaveTracked, bool? ReplicationTracked, bool Attribute, int ComponentSize)`
- `Attribute { get; init; } -> bool` — An attribute (Health, Stamina, Fuel): the game's formulas run after a write.
- `CanWrite { get; } -> bool` — `WorldWriteInfo.State` is `WorldWriteState.Ready`.
- `Component { get; init; } -> string` — The component's short name.
- `ComponentSize { get; init; } -> int` — The type's size in bytes on this build; 0 when not known.
- `FactsKnown { get; init; } -> bool` — False while the running server world has not published what it knows of the type (no world runs yet, or a new one is...
- `GameWriterCount { get; init; } -> int` — The game's own programs that write it on the server, every step they run.
- `GameWriters { get; init; } -> IReadOnlyList<string>` — The names of the first of them.
- `InitWriterCount { get; init; } -> int` — The game's programs that write it only when an entity is created.
- `Reason { get; init; } -> string?` — Why not, as a sentence that says what to do; null when `WorldWriteInfo.CanWrite`.
- `ReplicationTracked { get; init; } -> bool?` — Whether it records them for replication; null when this build does not tell (`WorldWriteResult.ReplicationTracked`).
- `SaveTracked { get; init; } -> bool?` — Whether the game's write tracking records changes of the type for the save; null when this build does not tell...
- `State { get; init; } -> WorldWriteState` — Whether the calling mod can write it right now.

## WorldWriteResult `0.2.2` — The answer to a tracked write (`World.Write`, `World.Update`, `World.WriteField`, `World.WriteAt`).
- `After { get; } -> ReadOnlyMemory<byte>` — The same bytes afterwards.
- `Attribute { get; } -> bool` — True for an attribute (Health, Stamina, Fuel): the game's formulas ran after the write, so `WorldWriteResult.After`...
- `Before { get; } -> ReadOnlyMemory<byte>` — The bytes the write named, as the component held them before; empty when nothing was read.
- `Changed { get; } -> bool` — True when bytes changed (`WorldWriteStatus.Written`).
- `Component { get; } -> string` — The component's short name.
- `ComponentSize { get; } -> int` — The size of the entity's component in bytes; 0 when it was not found.
- `Entity { get; } -> EntityId` — The entity.
- `GameWriterCount { get; } -> int` — How many of the game's own programs write this component type on the server.
- `GameWriters { get; } -> IReadOnlyList<string>` — The names of the first of those programs (`fuel_setup`, `actor_interaction`).
- `Kept { get; } -> bool?` — For a component the game writes too: whether the written bytes were still there one step later, after the game's...
- `Offset { get; } -> int` — The first byte written, within the component.
- `Ok { get; } -> bool` — True when the component holds the value now: written, or it held it already.
- `Reason { get; } -> string?` — Why it did not happen, as a sentence; null when `WorldWriteResult.Ok`.
- `ReplicationTracked { get; } -> bool?` — Whether the game's write tracking records a change of this component type for replication (the game's own per-type...
- `SaveTracked { get; } -> bool?` — Whether the game's write tracking records a change of this component type for the save (the game's own per-type...
- `Status { get; } -> WorldWriteStatus` — What became of the write.
- `Steps { get; } -> int` — Sim steps from the call to this answer.
- `WrittenOverBy { get; } -> string?` — For a write that was not `WorldWriteResult.Kept`: the policy of Kiln's that wrote over it in the step the write was...

## WorldWriteState `0.2.2` — Whether a component type can be written right now (`WorldWriteInfo.State`).
enum: Ready, Pending, NotDeclared, NotListed, UnknownComponent, OverLimit, Off, ListRefused

## WorldWriteStatus `0.2.2` — What became of a tracked write (`WorldWriteResult.Status`).
enum: Written, Unchanged, NoComponent, OutOfRange, NotDeclared, Refused, Faulted, Dropped, LayoutMismatch
