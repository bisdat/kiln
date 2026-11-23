# Systems (Kiln.Systems)

## `Apply<T, TSnap>` `0.2.4` — What `ProgramWrap.AroundEach` does with an entity after the game's update, with what its capture kept before it.
- `Invoke(ref T entity, in TSnap before) -> void`

## Attribute32 `0.2.4` — An attribute component (Health, Stamina, Mana, Fuel, Level ...) through a program wrap's write slot: the game's block...
- `Changed() -> void` — Runs the game's attribute formulas on the component after you changed a slot (clamps to the maximum, derived...
- `Component { get; } -> string` — The component's name.
- `Count { get; } -> int` — How many slots (the block's count, within the component's size).
- `HasValues { get; } -> bool` — True when the block holds values (flag bit 0): the game skips an attribute without them.
- `RootId { get; } -> uint` — The attribute group's root id (the block's first word).
- `this[int slot] { get; } -> int` — Slot `slot`.
- `Values { get; } -> Span<int>` — The slots.

## `Capture<T, TSnap>` `0.2.4` — What `ProgramWrap.AroundEach` keeps of an entity before the game's update.
- `Invoke(in T entity) -> TSnap`

## EntityHandler `0.2.4` — A per-entity stage: runs once per entity of each slice, on the thread that runs the slice.
- `Invoke(EntityId entity) -> void`

## `EntityHandler<T>` `0.2.4` — A typed per-entity stage: runs once per entity of each slice, on the thread that runs the slice.
- `Invoke(ref T entity) -> void`

## EntityView `0.2.4` — One entity of a slice inside a per-entity stage, and its components through the program's own slots: the components the program's descriptor lists...
- `Attribute(string component) -> Attribute32` — An attribute component the program writes (`"Fuel"`, `"Health"`, `"Stamina"`): its slots, written like...
- `Entity { get; } -> EntityId` — The entity.
- `Has(string component) -> bool` — True when the program lists `component` (any read or write list, or a singleton) and the entity has it now: an...
- `Program { get; } -> string` — The program whose slice this entity is of.
- `Read<T>(string component) -> T` — The entity's `component` to read (`"CurrentTransform"`, or a singleton such as `"impact::AttributeRegistry"`): a...
- `Row { get; } -> int` — Its row in the slice (from 0).
- `Step { get; } -> ulong` — The world's step.
- `Write<T>(string component) -> T` — The entity's `component` to write (a write list of the program): a reference into the game's storage.

## EntityViewHandler `0.2.4` — A per-entity stage with the entity's components (`EntityView`): runs once per entity of each slice, on the thread that runs the slice.
- `Invoke(ref EntityView entity) -> void`

## `IGameProgram<TSelf>` `0.2.4` — A typed view of one of the game's ECS programs: Kiln.Game generates one per program of the program catalog (`Kiln.Ecs.Programs.FuelSetup` for `fuel_setup`), with a...
- `Bind(EntityView entity) -> TSelf` — The view of one entity of a per-entity stage.
- `Contract { get; } -> uint` — FNV-1a-32 of the program's contract text (its 21 access lists, three masks, flags and interval, chapter 06a 9.7), as...
- `Lists { get; } -> string` — The contract text the view was made from, without its empty lists (one `list=names` line each, then the masks, flags...
- `Name { get; } -> string` — The program's name (`"fuel_setup"`).

## `IQuery<TSelf>` `0.2.4` — A query struct of `Systems.Define`: a `ref struct` with `QueryAttribute` that makes itself from the program's context for one entity.
- `Bind(EcsProgramContext context) -> TSelf` — The query of the entity `context` is at; valid during its call only.

## ProgramOptions `0.2.4` — How the game schedules a defined program: the flags of its `QueryAttribute`, said again where it is bound (the build reserved it with the attribute's; a difference is...
- `Barrier { get; init; } -> bool` — See `QueryAttribute.Barrier`.
- `Chunked { get; init; } -> bool` — See `QueryAttribute.Chunked`.
- `Exclusive { get; init; } -> bool` — See `QueryAttribute.Exclusive`.
- `HostOnly { get; init; } -> bool` — See `QueryAttribute.HostOnly`.
- `Init { get; init; } -> bool` — See `QueryAttribute.Init`.
- `Pinned { get; init; } -> bool` — See `QueryAttribute.Pinned`.

## ProgramRegistration `0.2.4` — The outcome of `Systems.Define`: a bound program, or why not.
- `Dispose() -> void` — Unbinds the handler: kiln.world waits until no call of it runs.
- `Key { get; } -> string` — The reservation's key.
- `Ok { get; } -> bool` — True when the handler is bound to the program reserved at boot.
- `Reason { get; } -> string?` — Why not: `"not reserved at boot: rebuild the mod and restart the server"`, or kiln.world's refusal.

## ProgramWrap `0.2.4` — A wrap on one program of one world (`Systems.Wrap`): its stages, their state and counters.
- `After(SliceHandler handler, int? priority = null) -> ProgramWrap` — A stage after the game's update (the exact reverse of the Before order).
- `AfterEach(EntityHandler handler, int? priority = null) -> ProgramWrap` — Per entity after the game's update.
- `AfterEach(EntityViewHandler handler, int? priority = null) -> ProgramWrap` — Per entity after the game's update, with the entity's components (see the other `ProgramWrap.BeforeEach`).
- `Before(SliceHandler handler, int? priority = null) -> ProgramWrap` — A stage before the game's update (priorities high to low; ties in the order made).
- `BeforeEach(EntityHandler handler, int? priority = null) -> ProgramWrap` — Per entity before the game's update: `handler` runs for every entity of each slice.
- `BeforeEach(EntityViewHandler handler, int? priority = null) -> ProgramWrap` — Per entity before the game's update, with the entity's components (`EntityView`): read what the program reads, write...
- `Disabled -> Action<WrapDisabledEvent>?` — Raised on the tick thread when one of the wrap's stages is turned off: a fault, its budget (1 ms per slice, 10...
- `Dispose() -> void` — Removes every stage: no stage call starts after this returns.
- `Program { get; } -> string` — The program's name.
- `Replace(SliceHandler handler) -> WrapResult<ProgramWrap>` — Instead of the game's update; call `Slice.RunOriginal` to run it.
- `State { get; } -> WrapState` — The wrap's state: `WrapState.Active` while a stage is in the chain, Pending until then, Removed after Dispose...
- `Stats { get; } -> WrapStats` — What the wrap's stages did so far.
- `When(Func<SliceInfo, bool> filter) -> ProgramWrap` — Runs the stages only on slices `filter` accepts (called per slice, on its thread).
- `WhenArchetype(IEnumerable<string> has, IEnumerable<string>? lacks = null) -> ProgramWrap` — Runs the stages only on slices whose archetype has every component of `has` and none of `lacks`; decided once per...
- `World { get; } -> WorldKind` — The wrap's world.

## `ProgramWrap<T>` `0.2.4` — A typed wrap (`Systems.Wrap`): the stages of `ProgramWrap` plus per-entity stages that see the program's typed view `T`, and `ProgramWrap.AroundEach`.
- `After(SliceHandler handler, int? priority = null) -> ProgramWrap<T>` — A slice stage after the game's update (see `ProgramWrap.After`).
- `AfterEach(EntityHandler<T> handler, int? priority = null) -> ProgramWrap<T>` — Per entity after the game's update, with the entity's typed view.
- `AroundEach<TSnap>(Capture<T, TSnap> capture, Apply<T, TSnap> apply, int? priority = null) -> ProgramWrap<T>` — Around the game's update, per entity: `capture` runs for every entity of the slice before the update (one `TSnap`...
- `Before(SliceHandler handler, int? priority = null) -> ProgramWrap<T>` — A slice stage before the game's update (see `ProgramWrap.Before`).
- `BeforeEach(EntityHandler<T> handler, int? priority = null) -> ProgramWrap<T>` — Per entity before the game's update, with the entity's typed view: read what the program reads, write what it writes...
- `Disabled -> Action<WrapDisabledEvent>?` — See `ProgramWrap.Disabled`.
- `Dispose() -> void` — Removes every stage: no stage call starts after this returns.
- `Program { get; } -> string` — The program's name.
- `Replace(SliceHandler handler) -> WrapResult<ProgramWrap<T>>` — Instead of the game's update (see `ProgramWrap.Replace`).
- `State { get; } -> WrapState` — See `ProgramWrap.State`.
- `Stats { get; } -> WrapStats` — See `ProgramWrap.Stats`.
- `Untyped { get; } -> ProgramWrap` — The untyped wrap under this one (the same stages).
- `When(Func<SliceInfo, bool> filter) -> ProgramWrap<T>` — See `ProgramWrap.When`.
- `WhenArchetype(IEnumerable<string> has, IEnumerable<string>? lacks = null) -> ProgramWrap<T>` — See `ProgramWrap.WhenArchetype`.
- `World { get; } -> WorldKind` — The wrap's world.

## QueryAttribute `0.2.4` — Declares a program of the mod's own over the struct it is on: which components each entity it runs for has (the access lists) and how the game schedules it.
- `AnyOf { get; set; } -> string[]` — Components of which an entity must have at least one (anyOf).
- `Barrier { get; set; } -> bool` — A scheduling barrier (flag barrier).
- `Chunked { get; set; } -> bool` — Runs per chunk (flag chunked).
- `EventsIn { get; set; } -> string[]` — Event streams it reads (eventsIn).
- `EventsOut { get; set; } -> string[]` — Event streams it sends (eventsOut).
- `Exclusive { get; set; } -> bool` — Runs alone (flag exclusive).
- `HostOnly { get; set; } -> bool` — Runs only where the world is hosted (flag hostOnly).
- `Init { get; set; } -> bool` — An INIT program: runs for archetype groups that gained entities (flag init).
- `Key { get; set; } -> string?` — The reservation's key in kiln.programs.json.
- `OtherRead { get; set; } -> string[]` — Other entities' components it reads (otherRead).
- `OtherTemplate { get; set; } -> string[]` — Other entities' template data it reads (otherTpl).
- `OtherWrite { get; set; } -> string[]` — Other entities' components it writes (otherWrite).
- `Pinned { get; set; } -> bool` — Runs on the world's own thread (flag pinned).
- `Read { get; set; } -> string[]` — Components the program reads; every entity has them (read).
- `ReadOptional { get; set; } -> string[]` — Components the program reads when the entity has them (readOptional).
- `SingletonsRead { get; set; } -> string[]` — Singletons it reads (singletonsRead).
- `SingletonsWrite { get; set; } -> string[]` — Singletons it writes (singletonsWrite).
- `Template { get; set; } -> string[]` — Template data every entity's archetype has (tpl).
- `TemplateOptional { get; set; } -> string[]` — Template data an archetype may have (tplOptional).
- `With { get; set; } -> string[]` — Components an entity must have, not read (with).
- `Without { get; set; } -> string[]` — Components an entity must not have (without).
- `Write { get; set; } -> string[]` — Components the program writes; every entity has them (write).
- `WriteOptional { get; set; } -> string[]` — Components the program writes when the entity has them (writeOptional).

## Slice `0.2.4` — What a slice stage gets: the slice (one archetype group's range of the program's entities), valid during the call only.
- `Count { get; } -> int` — Entities in the slice (0 for a SYSTEM program).
- `EntityAt(int row) -> EntityId` — The id of the slice's entity at `row` (from 0).
- `IsWorldThread { get; } -> bool` — True on the world's own thread, false on a game task worker.
- `Post(Action action) -> void` — Runs `action` on the tick thread before the next step, in your mod (the only way to use a tick-thread API from a stage).
- `Program { get; } -> string` — The program's name.
- `RunOriginal() -> void` — Inside a Replace: runs the game's update for this slice, once.
- `Step { get; } -> ulong` — The world's step.
- `World { get; } -> WorldKind` — The wrap's world.

## SliceHandler `0.2.4` — A slice stage: runs once per slice of the program, on the thread that runs the slice.
- `Invoke(ref Slice slice) -> void`

## SliceInfo `0.2.4` — What a slice filter sees (`ProgramWrap.When`).
- `SliceInfo()`
- `SliceInfo(string Program, WorldKind World, int Count, ulong Step, bool OnWorldThread, nint Layout)`
- `Count { get; init; } -> int` — Entities in the slice (0 for a SYSTEM program).
- `Layout { get; init; } -> nint` — The slice's archetype key; 0 when not known.
- `OnWorldThread { get; init; } -> bool` — True on the world's own thread, false on a game task worker.
- `Program { get; init; } -> string` — The program's name.
- `Step { get; init; } -> ulong` — The world's step.
- `World { get; init; } -> WorldKind` — The world the slice is in.

## SystemInfo `0.2.4` — One of the game's ECS programs in one world's program table.
- `SystemInfo(string Name, int Index, WorldKind World, uint Flags, ulong UpdateRva, bool Runs, IReadOnlyList<string> Writes, IReadOnlyList<WrapInfo> Wraps)`
- `Flags { get; init; } -> uint` — The descriptor's flags (0x1 a client program, 0x4 SYSTEM, 0x8 INIT, 0x200 CHUNKED, ...).
- `Index { get; init; } -> int` — Its position in that world's table now.
- `Name { get; init; } -> string` — Its name: `"fuel_setup"`.
- `Runs { get; init; } -> bool` — True when it has an update in this world (a client program on a server has none).
- `UpdateRva { get; init; } -> ulong` — Its update in the game's image (0: none in this world).
- `World { get; init; } -> WorldKind` — The world whose table it is in.
- `Wraps { get; init; } -> IReadOnlyList<WrapInfo>` — The managed wraps on it now.
- `Writes { get; init; } -> IReadOnlyList<string>` — The components its descriptor writes (write and optional write lists).

## Systems `0.2.4` — The game's ECS programs, and wraps on them: your code before, after or instead of a program's update, per slice (one archetype group's range of entities) or per...
- `static All(WorldKind world = WorldKind.Server) -> IReadOnlyList<SystemInfo>` — Every program of that world's table, in table order (empty before the world's first step).
- `static Define<T>(string key, ProgramOptions options, EntityHandler<T> each) -> ProgramRegistration` — Binds `each` to the program the build reserved for query `T` (its `QueryAttribute`, in the mod's...
- `static Find(string program, WorldKind world = WorldKind.Server) -> SystemInfo?` — The program `program` of that world's table, or null (not in that world, no world yet).
- `const PredictionCapability = "systems.wraps.prediction" -> string` `0.2.10` — The capability of prediction wraps (`WrapOptions.World` = `WorldKind.Prediction`): on where `systems.wraps.client` is.
- `static Wrap(string program, WrapOptions? options = null) -> WrapResult<ProgramWrap>` — A wrap on `program`: add its stages with `ProgramWrap.Before`, `ProgramWrap.After`, `ProgramWrap.Replace`...
- `static Wrap<T>(WrapOptions? options = null) -> WrapResult<ProgramWrap<T>>` — A typed wrap on the program of view `T` (Kiln.Game's `Ecs.Programs.*`, made from the program catalog of the game...

## WrapDisabledEvent `0.2.4` — A stage was turned off: it faulted, overran its budget 10 slices in a row, threw, or native SET came.
- `WrapDisabledEvent(string Program, string Mod, WrapStage Stage, string Reason)`
- `Mod { get; init; } -> string` — The id of the mod that made the wrap ("kiln" outside mod code).
- `Program { get; init; } -> string` — The name of the program the stage is on.
- `Reason { get; init; } -> string` — Why, in words: a fault (with its exception code), "budget", the exception the stage threw, "native SET loaded", or...
- `Stage { get; init; } -> WrapStage` — The stage that was turned off.

## WrapInfo `0.2.4` — One managed stage on a program.
- `WrapInfo(string Mod, string Program, WorldKind World, WrapStage Stage, int Priority, WrapState State, bool PerEntity)`
- `Mod { get; init; } -> string` — The id of the mod that made the wrap ("kiln" outside mod code).
- `PerEntity { get; init; } -> bool` — True for a per-entity stage, false for a slice stage.
- `Priority { get; init; } -> int` — The stage's priority; higher runs first among the Befores.
- `Program { get; init; } -> string` — The name of the program the stage is on.
- `Stage { get; init; } -> WrapStage` — Where the stage runs relative to the game's update.
- `State { get; init; } -> WrapState` — Where the stage is in its lifetime.
- `World { get; init; } -> WorldKind` — The world the wrap targets.

## WrapOptions `0.2.4` — Which world a wrap targets.
- `Priority { get; init; } -> int` — The priority of the stages whose call gives none.
- `World { get; init; } -> WorldKind` — The world (default the server world).

## `WrapResult<TWrap>` `0.2.4` — The outcome of making a wrap or a Replace stage.
- `WrapResult(bool Ok, string? Reason, TWrap? Wrap)`
- `Ok { get; init; } -> bool` — True when the wrap was made.
- `Reason { get; init; } -> string?` — Why not: another mod replaces the program (named), the program is not in that world, native SET is loaded.
- `Wrap { get; init; } -> TWrap?` — The wrap; null when not `Ok`.

## WrapStage `0.2.4` — Where a stage runs relative to the game's update.
enum: Before, Replace, After

## WrapState `0.2.4` — A wrap's lifetime: made at once, applied in the target world's next step, removed in the step after a Dispose.
enum: Pending, Active, Removed, Refused

## WrapStats `0.2.4` — What a stage did so far, summed over its stages.
- `WrapStats(long Slices, long Entities, double Microseconds, long Faults, long Overruns, int Threads)`
- `ArchetypeSkips { get; init; } -> long` — Slices a `ProgramWrap.WhenArchetype` filter left to the game because the entity map changed under its archetype read...
- `Entities { get; init; } -> long` — How many entities were in those slices.
- `Faults { get; init; } -> long` — How many times a stage faulted or threw.
- `Microseconds { get; init; } -> double` — The time spent in the stages, in microseconds.
- `Overruns { get; init; } -> long` — How many slices ran over the stage's budget (1 ms per slice).
- `Slices { get; init; } -> long` — How many slices the stages ran in.
- `Threads { get; init; } -> int` — How many distinct threads the stages ran on (counted up to 16; the highest of any one stage).
