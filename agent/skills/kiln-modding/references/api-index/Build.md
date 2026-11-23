# Build (Kiln.Build)

## AltarSize `0.2.17` — The build zone of one altar level: the full size of the box an altar of that level lets players build in.
- `AltarSize(int AltarLevel, Vector3 Size)`
- `AltarLevel { get; init; } -> int` — The altar level, 1 to 5 (an altar's upgrades; the game's table has a 0 for "no altar").
- `Size { get; init; } -> Vector3` — The full size in metres on each axis (x, y, z).

## BaseLimits `0.2.17` — How many Flame Altars the world may have and how large their build zones are in this start: the game's own table and the one the game runs with (after every mod's...
- `BaseLimits(int FlameLevel, int CurrentAltars, int OriginalCount, int EffectiveCount, IReadOnlyList<AltarSize> OriginalSizes, IReadOnlyList<AltarSize> EffectiveSizes)`
- `AppliedRules { get; init; } -> IReadOnlyList<string>` — The base rules in this start's table, as `mod:name`, in load order (empty without any, and when Kiln could not write...
- `CurrentAltars { get; init; } -> int` — Flame Altars the world has now (the game's progress count).
- `EffectiveCount { get; init; } -> int` — The count at `FlameLevel` in the table the game runs with.
- `EffectiveCounts { get; init; } -> IReadOnlyList<int>` — The count the game runs with at every Flame level (index = Flame level, from 0).
- `EffectiveSizes { get; init; } -> IReadOnlyList<AltarSize>` — The build-zone sizes the game runs with, altar levels 1 to 5.
- `FlameLevel { get; init; } -> int` — The world's Flame level now.
- `OriginalCount { get; init; } -> int` — The game's own count at `FlameLevel`.
- `OriginalCounts { get; init; } -> IReadOnlyList<int>` — The game's own count at every Flame level (index = Flame level, from 0).
- `OriginalSizes { get; init; } -> IReadOnlyList<AltarSize>` — The game's own build-zone sizes, altar levels 1 to 5.
- `PatchedBy { get; init; } -> IReadOnlyList<string>` — Mods whose own resource patches of the game's balancing table are in this start's table (a mod that changes the...
- `RegistryChanged { get; init; } -> IReadOnlyList<string>` — Values a registry edit changed after the base rules (the registry stage composes on the rules' result and wins), one...

## BlockBreakFlags `0.2.15` — Controlled breaking options.
enum: None, Instant, Drops, WorldBuildings, PlayerTerrain

## BlockBreakOptions `0.2.15` — One exact, loaded cell box, optionally selected by a cell mask.
- `Build { get; init; } -> BuildOptions` — The existing build permissions, actor, journal and label.
- `Damage { get; init; } -> int[]` — Five nonnegative damage values, by hardness (up to 100000).
- `Flags { get; init; } -> BlockBreakFlags` — The break mode and protection choices.
- `Materials { get; init; } -> byte[]` — Raw voxel material ids to include.
- `ProtectedSphere { get; init; } -> BlockBreakSphere?` — Optional sphere.
- `Selection { get; init; } -> bool[]?` — Optional mask in x,y,z order, with exactly the box's cell count.
- `TerrainHealAfter { get; init; } -> TimeSpan` — Forget terrain damage after this inactivity.
- `TerrainHealthScale { get; init; } -> double` — Terrain damage threshold as a multiple of material health.

## BlockBreakResult `0.2.15` — Counts and refusal of a controlled break.
- `BlockBreakResult(BlockRefusal Refusal, ulong Considered, ulong Damaged, ulong Removed, ulong Dropped, ulong ProtectedCells, ulong UnknownCells, ulong UnbreakableCells, ulong ExcludedCells, bool MayHaveChanged, bool Verified, uint BlocksWritten, bool Kept)`
- `BlocksWritten { get; init; } -> uint` — 8x8x8 blocks the writer committed; can be partial after a fault.
- `Considered { get; init; } -> ulong` — Selected cells that were not air.
- `Damaged { get; init; } -> ulong` — Cells whose damage rose.
- `Dropped { get; init; } -> ulong` — How many drops were granted.
- `ExcludedCells { get; init; } -> ulong` — Considered cells left alone by the selection rules: outside the `BlockBreakOptions.ProtectedSphere`, terrain without...
- `Kept { get; init; } -> bool` — The writer reported the touched chunks in Kiln's save regions.
- `MayHaveChanged { get; init; } -> bool` — The writer or damage mutator was entered, so changes may have happened (also after a fault).
- `Ok { get; } -> bool` — True when the break was not refused (`BlockBreakResult.Refusal` is `BlockRefusal.None`).
- `ProtectedCells { get; init; } -> ulong` — Considered cells spared because their provenance is Natural: natural buildings (unless...
- `Reason { get; } -> string?` — Why the break was refused or failed, in words; null when `BlockBreakResult.Ok`.
- `Refusal { get; init; } -> BlockRefusal` — `BlockRefusal.None` when the call went ahead; otherwise why it was refused or failed.
- `Removed { get; init; } -> ulong` — Selected cells read back as air.
- `UnbreakableCells { get; init; } -> ulong` — Considered cells spared because their material is `BlockHardness.Unbreakable`.
- `UnknownCells { get; init; } -> ulong` — Considered cells spared because their material facts are unknown or their provenance is Unknown (buildings only when...
- `Verified { get; init; } -> bool` — The readback behind `Removed` completed (also set when nothing was selected).

## BlockBreakSphere `0.2.15` — Sphere selection with a caller-controlled protection margin and minimum radius.
- `BlockBreakSphere(WorldPosition Center, double Radius, double Margin = 0, double MinimumRadius = 0)`
- `Center { get; init; } -> WorldPosition` — The sphere's centre in world metres.
- `Margin { get; init; } -> double` — The protection margin in metres, zero or more; default 0.
- `MinimumRadius { get; init; } -> double` — Metres, zero or more; default 0.
- `Radius { get; init; } -> double` — The sphere's radius in metres; finite and greater than zero, or the call is refused as `BlockRefusal.InvalidArgument`.

## BlockHardness `0.2.15` — The material's mining hardness class.
enum: Soft, SlightlyHard, ModeratelyHard, Hard, VeryHard, Unbreakable

## BlockInfo `0.2.15` — Loaded cell facts.
- `BlockInfo(BuildBlockKind Kind, byte Material, byte Fill, BlockProvenance Provenance, BlockHardness? Hardness, int? Health, int? Damage, WorldPosition Center)`
- `Center { get; init; } -> WorldPosition` — The centre of the cell in world metres.
- `Damage { get; init; } -> int?` — The cell's current damage, 0-255.
- `Fill { get; init; } -> byte` — The raw fill byte.
- `Hardness { get; init; } -> BlockHardness?` — The material's mining hardness class; null when unknown (never guessed) or for an air cell.
- `Health { get; init; } -> int?` — The material's raw health points from its config, not scaled by damage; null when unknown or for an air cell.
- `Kind { get; init; } -> BuildBlockKind` — What occupies the cell: air, terrain or a building block.
- `Material { get; init; } -> byte` — The raw voxel material id.
- `Provenance { get; init; } -> BlockProvenance` — Comparison with the shipped world: `BlockProvenance.Natural`, `BlockProvenance.PlayerPlaced` or...

## BlockProvenance `0.2.15` — Comparison with the shipped world, not ownership or execution side.
enum: Unknown, PlayerPlaced, Natural

## BlockQueryResult `0.2.15` — A successful query may have no hit.
- `BlockQueryResult(BlockInfo? Info, BlockRefusal Refusal)`
- `Found { get; } -> bool` — True when the query hit a cell: `BlockQueryResult.Info` is not null.
- `Info { get; init; } -> BlockInfo?` — The facts of the cell that was read or hit; null when a query that ran found nothing (a clear line), or when it was...
- `Ok { get; } -> bool` — True when the query ran (`BlockQueryResult.Refusal` is `BlockRefusal.None`), whether or not it found a cell.
- `Reason { get; } -> string?` — Why the query was refused, in words; null when `BlockQueryResult.Ok`.
- `Refusal { get; init; } -> BlockRefusal` — `BlockRefusal.None` when the query ran, even with no hit; otherwise why it did not.

## BlockRefusal `0.2.15` — A block operation's explicit refusal.
enum: None, SymbolMissing, ServiceMissing, ServiceOlder, WrongSide, Offline, NoSession, InvalidArgument, WrongThread, NotLoaded, Zone, Base, Faulted, Unverified, Canceled, DamageMapFull

## Build — Changes the world the way the game saves and replicates it, so vanilla clients see the result: voxel cells (terrain and building blocks), shape edits (dig, add...
- `static Add(BuildShape shape, byte material, BuildOptions? options = null) -> Task<BuildResult>` — Fills the shape with `material` (a voxel material id, e.g. from `Build.MaterialOf`).
- `static Available { get; } -> bool` — True when kiln.build runs on this game build (capability `kiln.build`).
- `static BaseLimits { get; } -> BaseLimits?` `0.2.17` — How many Flame Altars the world may have now and how large their build zones are: the game's own numbers and the...
- `static Bases { get; } -> IReadOnlyList<PlayerBase>` — Player bases (the game's base build zones, rebuilt every step).
- `static BlockAt(WorldPosition position) -> BlockQueryResult` `0.2.15` — Reads a loaded cell on the server tick.
- `static BoxAround(WorldPosition center, double halfX, double halfY, double halfZ) -> CellBox` — The cells covering `center` +/- half extents (metres).
- `static BoxBetween(WorldPosition a, WorldPosition b) -> CellBox` — The cells covering the box between two world positions.
- `static BreakBox(CellBox box, BlockBreakOptions? options = null) -> BlockBreakResult` `0.2.15` — Breaks selected cells in an exact box of at most 4096 loaded cells.
- `static CellOf(WorldPosition p) -> CellPos` — The cell that contains a world position.
- `static CenterOf(CellPos c) -> WorldPosition` — World position of a cell's centre.
- `static CornerOf(CellPos c) -> WorldPosition` — World position of a cell's low corner.
- `static Dig(BuildShape shape, BuildOptions? options = null) -> Task<BuildResult>` — Removes terrain and blocks inside the shape (a smooth one-cell edge).
- `static FindWall(WorldPosition from, WorldPosition to, FindWallFlags flags = FindWallFlags.None) -> BlockQueryResult` `0.2.15` — Finds the first crossed building cell within 64 metres, optionally terrain.
- `static Flatten(BuildShape shape, double levelY, byte material = 0, BuildOptions? options = null) -> Task<BuildResult>` — Levels the shape's footprint to `levelY` metres: solid below (each column's own material, or `material`), air above...
- `static ForgetPlaced(string key) -> bool` — Forgets a PlaceOnce record (the next PlaceOnce with that key places again).
- `static Grid { get; } -> VoxelGrid?` — The voxel grid's geometry (tick thread; null before the world runs).
- `static History(string? owner = null) -> IReadOnlyList<JournalInfo>` — The session journal, newest first (all owners, or one: a mod id, or "player:\<entity>").
- `static IsLoaded(CellBox box) -> bool` — Whether every voxel tile under the box is loaded.
- `static JournalOwner(BuildOptions? options = null) -> string` — The journal owner of edits made with these options (the calling mod, or the actor player).
- `static Keep(CellBox box) -> void` — Adds a box to Kiln's save regions: the game then saves every terrain change there, players' digging included (as...
- `static Load(CellBox box, TimeSpan? timeout = null) -> Task<bool>` — Waits (on the tick thread) until the tiles under the box are loaded.
- `static MaterialOf(uint itemId) -> byte` — The voxel material a building-material item places ("Stone Block" ...), 0 when none.
- `static Paint(BuildShape shape, byte material, BuildOptions? options = null) -> Task<BuildResult>` — Gives every non-air cell inside `material`, keeping its fill.
- `static PlacedOnce { get; } -> IReadOnlyList<PlaceOnceRecord>` — The calling mod's PlaceOnce records in this world.
- `static PlaceOnce(string key, Schematic schematic, WorldPosition at, PasteOptions? options = null) -> Task<PlaceOnceResult>` — Pastes `schematic` at `at` unless the calling mod already placed `key` in this world.
- `static PlaceOnce(string key, Func<Task<bool>> edit) -> Task<PlaceOnceResult>` — Runs `edit` (any Kiln.Build edits; return true when it worked) unless the calling mod already placed `key` in this...
- `static PlaceProp(Guid template, WorldPosition position, Quaternion? rotation = null, uint itemId = 0, BuildOptions? options = null) -> EntityId` — Places a prop by the GUID of its template; see `Build.PlaceProp`.
- `static PlaceProp(string template, WorldPosition position, Quaternion? rotation = null, uint itemId = 0, BuildOptions? options = null) -> EntityId` — Places a prop the way the game's building helper does (CurrentTransform, DisableRandomLootOnDestroy, UsedItem).
- `static Prop(EntityId entity) -> PlacedProp?` — The prop's template, transform and UsedItem; null when it does not exist.
- `static Props(CellBox box, PropFilter filter = PropFilter.Placed) -> IReadOnlyList<PlacedProp>` — Props inside a cell box (positions within its world-space bounds).
- `static Props(WorldPosition min, WorldPosition max, PropFilter filter = PropFilter.Placed) -> IReadOnlyList<PlacedProp>` — Props whose position lies in the box between `min` and `max`, nearest to its centre first.
- `static Read(CellBox box) -> Voxel[]` — Reads the cells of a box (x fastest).
- `static ReadAsync(CellBox box, TimeSpan? timeout = null) -> Task<Voxel[]>` — Loads the tiles, then reads.
- `static ReadCell(CellPos c) -> Voxel` — One cell.
- `static Release(CellBox box) -> long` — Forgets the save regions inside a box: their terrain reverts at the next restart unless a base covers it.
- `static RemoveProp(EntityId entity, BuildOptions? options = null) -> bool` — Removes a prop (any static entity; never creatures, villagers or players).
- `static Replace(BuildShape shape, byte from, byte to, BuildOptions? options = null) -> Task<BuildResult>` — Cells of material `from` inside become `to`.
- `static ShapeBox(ShapeOp op, BuildShape shape, double levelY = NaN) -> CellBox` — The cells a shape edit touches.
- `static Smooth(BuildShape shape, float strength = 0.5, int passes = 2, BuildOptions? options = null) -> Task<BuildResult>` — Blurs the surface inside the shape (`strength` 0..1 per pass, 1..8 passes).
- `static Status { get; } -> BuildStatus?` — kiln.build's state (any thread).
- `static TemplateOf(uint itemId) -> Guid?` — The template a prop item places, or null.
- `static TryRead(CellBox box, out Voxel[] cells) -> bool` — Reads the cells of a box; false (cells still filled from what is loaded) when a tile is missing.
- `static Undo(string? owner = null, TimeSpan? timeout = null) -> Task<UndoResult?>` — Reverts the newest journal entry of `owner` (default: the calling mod's own edits; pass `Build.JournalOwner` of a...
- `static UndoEntry(long id, TimeSpan? timeout = null) -> Task<UndoResult?>` — Reverts one journal entry by id.
- `static WasPlaced(string key) -> bool` — Whether the calling mod placed `key` in this world.
- `static Write(CellBox box, ReadOnlySpan<Voxel> cells, BuildOptions? options = null, ReadOnlySpan<bool> mask = default(ReadOnlySpan<bool>)) -> BuildResult` — Writes exact cells (x fastest) through the game's block writer.
- `static WriteAsync(CellBox box, Voxel[] cells, BuildOptions? options = null, bool[]? mask = null) -> Task<BuildResult>` — Loads the tiles, then `Build.Write`s.
- `static WriteIfUnchanged(CellBox box, ReadOnlySpan<Voxel> cells, ReadOnlySpan<Voxel> expected, BuildOptions? options = null, ReadOnlySpan<bool> mask = default(ReadOnlySpan<bool>)) -> BuildResult` `0.2.3` — A compare-and-write: `Build.Write`s `cells` only if the world still holds `expected` when the edit commits, which is...

## BuildBlockKind `0.2.15` — What occupies a voxel cell.
enum: Air, Terrain, Building

## BuildCommands — The `/build` command tree (admins only; permission node `kiln.command.build`).
- `static Register() -> CommandRegistration` — Asks for `/build` on behalf of the calling mod.
- `static SchematicDirectory { get; } -> string` — Where `/build capture` and `/build paste` keep schematics.
- `static SchematicPath(string name) -> string` — The path of a schematic by name (letters, digits, `- _ .`).

## BuildEditEvent — Posted after a Kiln edit changed the world (or was refused by the world: zone, base, not loaded; or `Build.WriteIfUnchanged` found the cells changed).
- `Actor { get; } -> EntityId` — The player the edit was for, or none (Kiln itself).
- `Box { get; } -> CellBox` — The cells the edit covered (props: the cell of the prop's position).
- `Changed { get; } -> bool` — True when cells changed (`BuildResult.Changed`) or the edit placed or removed props.
- `JournalId { get; } -> long` — The journal entry (0 = not journaled).
- `Kind { get; } -> BuildEditKind` — What kind of edit it was.
- `Label { get; } -> string?` — The label of the edit (`BuildOptions.Label`); null when it has none.
- `Origin { get; } -> string` — The mod that asked for it.
- `PropsPlaced { get; } -> IReadOnlyList<EntityId>` — The props the edit placed.
- `PropsRemoved { get; } -> IReadOnlyList<EntityId>` — The props the edit removed.
- `Result { get; } -> BuildResult?` — The voxel part (null for prop-only edits).

## BuildEditingEvent — Posted before a Kiln edit (managed API, schematics, commands) changes the world.
- `Actor { get; } -> EntityId` — The player the edit is for, or none (Kiln itself).
- `Box { get; } -> CellBox` — The cells the edit may change (props: the cell of the prop's position).
- `Kind { get; } -> BuildEditKind` — What kind of edit is about to run.
- `Label { get; } -> string?` — The label of the edit (`BuildOptions.Label`); null when it has none.
- `Origin { get; } -> string` — The mod that asked for it.

## BuildEditKind — What a Kiln edit is.
enum: Write, Dig, Add, Flatten, Smooth, Paint, Replace, Paste, PlaceProp, RemoveProp, Undo

## BuildException — An edit Kiln could not run at all (service unavailable, bad box, engine fault).
- `BuildException(string message, int code = 0)`
- `Code { get; } -> int` — The kiln.build result code behind the failure; 0 when the exception did not come from one.

## BuildFlags — How a Kiln edit may act.
enum: None, RespectZones, InBases, AllowAltars, Unloaded, DryRun, Transient

## BuildOptions — Optional settings of an edit.
- `Actor { get; init; } -> EntityId` — The player the edit is for (zones with `BuildFlags.RespectZones`, journal owner, events); none = Kiln itself.
- `static Default { get; } -> BuildOptions` — The options a call uses when it is given none: no flags, no actor, journaled, the default load timeout and no label.
- `Flags { get; init; } -> BuildFlags` — How the edit may act (`BuildFlags`).
- `Journal { get; init; } -> bool` — Record the edit in the session journal so `Build.Undo` can revert it.
- `Label { get; init; } -> string?` — Shown in the journal and events ("paste outpost", "/build dig").
- `LoadTimeout { get; init; } -> TimeSpan` — How long an async edit waits for the voxel tiles to stream in.

## BuildRefusal — Why an edit changed nothing.
enum: None, Zone, Base, NotLoaded, DryRun, Canceled, Changed `0.2.3`

## BuildResult — What an edit did.
- `BuildResult(long CellsChanged, int BlocksWritten, bool Verified, bool Kept, CellBox Box, double Milliseconds, BuildRefusal Refusal = BuildRefusal.None, uint Zone = 0)`
- `BlocksWritten { get; init; } -> int` — 8x8x8 blocks the engine committed.
- `Box { get; init; } -> CellBox` — The cells the edit covered.
- `CellsChanged { get; init; } -> long` — Cells whose value changed.
- `Changed { get; } -> bool` — True when cells changed and the edit was not refused or a dry run (`BuildResult.CellsChanged` is above zero and...
- `Kept { get; init; } -> bool` — The touched chunks are in Kiln's save regions (the game saves them).
- `Milliseconds { get; init; } -> double` — The time the engine's writer took, in milliseconds.
- `Ok { get; } -> bool` `0.2.2` — The edit was not refused (`BuildResult.Refused` is false): it ran, or was a dry run.
- `Reason { get; } -> string?` `0.2.2` — Why the edit was refused, in words (`BuildResult.Refusal` is the code); null when `BuildResult.Ok`.
- `Refusal { get; init; } -> BuildRefusal` — Why the edit changed nothing; `BuildRefusal.None` when it was not refused.
- `Refused { get; } -> bool` — True when the edit was refused: `BuildResult.Refusal` is anything but `BuildRefusal.None` or `BuildRefusal.DryRun`.
- `Verified { get; init; } -> bool` — The cells read back equal to what was written.
- `Zone { get; init; } -> uint` — The denying zone (`BuildRefusal.Zone`) or base altar (`BuildRefusal.Base`).

## BuildShape — A region for shape edits, in world metres.
- `BuildShape()`
- `BuildShape(ShapeKind Kind, WorldPosition Center, Vector3 Half)`
- `static Box(WorldPosition center, double halfX, double halfY, double halfZ) -> BuildShape` — A box around `center` with these half extents (metres).
- `Center { get; init; } -> WorldPosition` — The centre of the shape in world metres.
- `static Cylinder(WorldPosition center, double radius, double halfHeight) -> BuildShape` — A vertical cylinder around `center` with this radius and half height (metres).
- `Half { get; init; } -> Vector3` — Box: half extents.
- `Kind { get; init; } -> ShapeKind` — The geometry of the shape.
- `static Sphere(WorldPosition center, double radius) -> BuildShape` — A sphere around `center` with this radius (metres).

## BuildStatus — kiln.build's state (any thread).
- `BuildStatus(bool Read, bool Write, bool Props, bool World, bool Persist, ulong Reads, ulong Writes, ulong CellsRead, ulong CellsWritten, ulong BlocksWritten, ulong PropsPlaced, ulong PropsRemoved, ulong Refused, ulong KeptChunks, ulong SavesSeen, string Detail)`
- `BlocksWritten { get; init; } -> ulong` — 8x8x8 blocks committed since kiln.build started.
- `CellsRead { get; init; } -> ulong` — Cells read since kiln.build started.
- `CellsWritten { get; init; } -> ulong` — Cells whose value a write changed since kiln.build started.
- `Detail { get; init; } -> string` — A note on why something is off, in words (for example that the voxel world is not initialized yet).
- `KeptChunks { get; init; } -> ulong` — The 16 m chunks in Kiln's save regions.
- `Persist { get; init; } -> bool` — Kiln's save regions are hooked into the game's save, so Kiln's edits are saved.
- `Props { get; init; } -> bool` — Prop placement works on this build.
- `PropsPlaced { get; init; } -> ulong` — Props placed since kiln.build started.
- `PropsRemoved { get; init; } -> ulong` — Props removed since kiln.build started.
- `Read { get; init; } -> bool` — Voxel reads work on this build.
- `Reads { get; init; } -> ulong` — Voxel reads served since kiln.build started.
- `Refused { get; init; } -> ulong` — Edits and prop placements that were refused or failed since kiln.build started.
- `SavesSeen { get; init; } -> ulong` — World saves whose region list Kiln extended.
- `World { get; init; } -> bool` — The server world runs and the voxel world was found.
- `Write { get; init; } -> bool` — Voxel writes work on this build.
- `Writes { get; init; } -> ulong` — Voxel writes done since kiln.build started.

## CaptureOptions — Capture settings.
- `Anchor { get; init; } -> CellPos?` — The paste point, in cells from the low corner.
- `Author { get; init; } -> string?` — The schematic's author.
- `Description { get; init; } -> string?` — The schematic's description.
- `LoadTimeout { get; init; } -> TimeSpan` — How long `Schematic.CaptureAsync` waits for the voxel tiles to stream in.
- `Name { get; init; } -> string?` — The schematic's name.
- `Props { get; init; } -> PropFilter` — Which props to keep.

## CellBox — A box of cells: low corner `(X, Y, Z)` and size.
- `CellBox()`
- `CellBox(int X, int Y, int Z, int SizeX, int SizeY, int SizeZ)`
- `Contains(CellPos p) -> bool` — Whether the cell `p` (cell coordinates) lies inside the box.
- `Count { get; } -> long` — The number of cells in the box: `SizeX * SizeY * SizeZ`.
- `End { get; } -> CellPos` — The cell just outside the high corner.
- `static FromCorners(CellPos a, CellPos b) -> CellBox` — The box spanning two corner cells (both included, any order).
- `Index(int x, int y, int z) -> int` — Where the cell at `x`, `y`, `z` sits in an array of the box's cells (x fastest, then y, then z).
- `IsEmpty { get; } -> bool` — True when a size is zero or negative, so the box holds no cells.
- `Min { get; } -> CellPos` — The low corner cell, `(X, Y, Z)`.
- `Offset(CellPos d) -> CellBox` — The same box moved by `d` cells; the size stays.
- `SizeX { get; init; } -> int` — The number of cells along x.
- `SizeY { get; init; } -> int` — The number of cells along y.
- `SizeZ { get; init; } -> int` — The number of cells along z.
- `X { get; init; } -> int` — The x index of the low corner cell.
- `Y { get; init; } -> int` — The y index of the low corner cell.
- `Z { get; init; } -> int` — The z index of the low corner cell.

## CellPos — Cell coordinates (integer index into the voxel grid; see `Build.CellOf`).
- `CellPos()`
- `CellPos(int X, int Y, int Z)`
- `static operator +(CellPos a, CellPos b) -> CellPos` — Adds two cell positions component by component.
- `static operator -(CellPos a, CellPos b) -> CellPos` — Subtracts `b` from `a` component by component.
- `X { get; init; } -> int` — The cell index along x.
- `Y { get; init; } -> int` — The cell index along y, the vertical axis.
- `Z { get; init; } -> int` — The cell index along z.

## FindWallFlags `0.2.15` — Additional terrain hits for a wall search.
enum: None, PlayerTerrain, Terrain

## JournalInfo — One recorded edit.
- `JournalInfo(long Id, string Owner, string Label, BuildEditKind Kind, DateTimeOffset At, long Cells, int PropsPlaced, int PropsRemoved)`
- `At { get; init; } -> DateTimeOffset` — When the entry was started.
- `Cells { get; init; } -> long` — How many cells it recorded.
- `Id { get; init; } -> long` — The entry's id (`Build.UndoEntry`).
- `Kind { get; init; } -> BuildEditKind` — What kind of edit it was.
- `Label { get; init; } -> string` — The label of the edit.
- `Owner { get; init; } -> string` — The owner: the mod id, or `player:<entity>` for an edit made for a player.
- `PropsPlaced { get; init; } -> int` — How many props it placed.
- `PropsRemoved { get; init; } -> int` — How many props it removed.

## PasteMode — How a paste writes cells.
enum: Replace, Merge

## PasteOptions — Paste settings.
- `Actor { get; init; } -> EntityId` — The player the paste is for (zones with `BuildFlags.RespectZones`, journal owner, events); none = Kiln itself.
- `ClearProps { get; init; } -> bool` — Remove building props (UsedItem) already inside the target box first.
- `Flags { get; init; } -> BuildFlags` — How the paste may act (`BuildFlags`), as for `BuildOptions.Flags`.
- `Journal { get; init; } -> bool` — Record the paste as one journal entry so `Build.Undo` can revert it.
- `Label { get; init; } -> string?` — Shown in the journal and events.
- `LoadTimeout { get; init; } -> TimeSpan` — How long the paste waits for the voxel tiles to stream in.
- `Mode { get; init; } -> PasteMode` — Whether the schematic's air overwrites the world: `PasteMode.Replace` (the default) or `PasteMode.Merge`.
- `Offset { get; init; } -> CellPos` — Extra offset in cells after rotation.
- `Props { get; init; } -> bool` — Place the schematic's props.
- `Rotation { get; init; } -> int` — Quarter turns about +Y (yaw +90 degrees each): 0..3.

## PasteResult — What a paste did.
- `PasteResult(CellBox Box, BuildResult Cells, int PropsPlaced, int PropsFailed, int PropsCleared, long JournalId, IReadOnlyList<EntityId> Props)`
- `Box { get; init; } -> CellBox` — The cells the paste covered.
- `Cells { get; init; } -> BuildResult` — The result of the cell write, with its refusal if there was one.
- `JournalId { get; init; } -> long` — The id of the journal entry (`Build.UndoEntry`); 0 when the paste was not journaled or recorded nothing.
- `Ok { get; } -> bool` — The paste was not refused and every prop was placed: `PasteResult.Cells` is not refused and...
- `Props { get; init; } -> IReadOnlyList<EntityId>` — The entities of the props that were placed.
- `PropsCleared { get; init; } -> int` — How many building props `PasteOptions.ClearProps` removed first.
- `PropsFailed { get; init; } -> int` — How many props could not be placed.
- `PropsPlaced { get; init; } -> int` — How many props were placed.
- `Reason { get; } -> string?` `0.2.2` — Why the paste is not ok: the cells' refusal (`BuildResult.Reason`), else how many props could not be placed; null...

## PlacedProp — A prop in the world.
- `PlacedProp(EntityId Entity, Guid Template, string TemplateName, WorldPosition Position, Quaternion Rotation, Vector3 Scale, uint ItemId, TemplateKind Kind, PropFlags Flags)`
- `Entity { get; init; } -> EntityId` — The prop's entity.
- `Flags { get; init; } -> PropFlags` — What the prop is: placed, a flame altar, saved by the game.
- `ItemId { get; init; } -> uint` — The item its UsedItem names, which dismantling gives back; 0 when it has none.
- `Kind { get; init; } -> TemplateKind` — The template's kind (kiln.spawn's).
- `Position { get; init; } -> WorldPosition` — The prop's position in world metres.
- `Rotation { get; init; } -> Quaternion` — The prop's orientation.
- `Scale { get; init; } -> Vector3` — The prop's scale along x, y and z.
- `Template { get; init; } -> Guid` — The GUID of the prop's template.
- `TemplateName { get; init; } -> string` — The name of the prop's template.

## PlaceOnceRecord — A PlaceOnce record (one per key, per mod, per world).
- `At { get; set; } -> double[]` — The position it was pasted at: `[x, y, z]` in world metres.
- `Box { get; set; } -> int[]` — The cells the paste covered: `[x, y, z, sizeX, sizeY, sizeZ]` in cells.
- `Cells { get; set; } -> long` — How many cells the paste changed.
- `Hash { get; set; } -> string` — The hash of the schematic (`Schematic.Hash`).
- `Key { get; set; } -> string` — The key the mod gave to `PlaceOnce`.
- `KilnVersion { get; set; } -> string` — The Kiln version that placed it.
- `PlacedAt { get; set; } -> DateTimeOffset` — When it was placed (UTC).
- `Props { get; set; } -> int` — How many props the paste placed.
- `Rotation { get; set; } -> int` — The quarter turns the paste used (`PasteOptions.Rotation`).
- `Schematic { get; set; } -> string` — The name of the schematic (`Schematic.Name`).

## PlaceOnceResult — What `Build.PlaceOnce` did.
- `PlaceOnceResult(bool Placed, bool AlreadyPlaced, PlaceOnceRecord? Record, PasteResult? Paste, string? Error = null)`
- `AlreadyPlaced { get; init; } -> bool` — True when the calling mod had already placed this key in this world, so nothing was placed now.
- `Error { get; init; } -> string?` — Why it failed, in words; null when it did not fail.
- `Ok { get; } -> bool` `0.2.2` — The structure is in the world: placed now, or by an earlier call.
- `Paste { get; init; } -> PasteResult?` — The paste's result; null when it did not paste (already placed, the world never started, or the edit overload was used).
- `Placed { get; init; } -> bool` — True when it placed now; false when the world already had it (`PlaceOnceResult.Record`) or the edit failed...
- `Reason { get; } -> string?` `0.2.2` — Why it is not (`PlaceOnceResult.Error`, else the paste's reason); null when `PlaceOnceResult.Ok`.
- `Record { get; init; } -> PlaceOnceRecord?` — The record: the new one when `Placed`, the earlier one when `AlreadyPlaced`, otherwise null.

## PlayerBase — A player base: the game's own build zone around a flame altar (BuildZoneSystem).
- `PlayerBase(EntityId Altar, WorldPosition Center, Vector3 Half, bool Abandoned)`
- `Abandoned { get; init; } -> bool` — True when the game marks the base as abandoned.
- `Altar { get; init; } -> EntityId` — The flame altar entity that owns the zone.
- `Center { get; init; } -> WorldPosition` — The centre of the build zone in world metres.
- `Contains(WorldPosition p) -> bool` — Whether a world position lies inside the zone: within `PlayerBase.Half` of `PlayerBase.Center` on all three axes...
- `Half { get; init; } -> Vector3` — Half the zone's size along x, y and z, in metres.

## PropFilter — Which entities `Build.Props` returns.
enum: Static, Placed, Saved, All

## PropFlags — What a prop is (`PlacedProp.Flags`).
enum: None, Placed, Altar, Saved

## Schematic — A captured region: cells (x fastest, then y, then z) and props.
- `Schematic(int sizeX, int sizeY, int sizeZ, Voxel[] cells, IReadOnlyList<SchematicProp>? props = null, CellPos? anchor = null, double cellSize = 0.5)` — Builds a schematic from cells (x fastest, then y, then z).
- `Anchor { get; } -> CellPos` — The paste point in cells from the low corner (a paste puts this cell at the target position).
- `Author { get; init; } -> string` — The schematic's author, saved in the file.
- `static Capture(CellBox box, CaptureOptions? options = null) -> Schematic` — Captures the cells of `box` and the props in it (tiles must be loaded; see `Schematic.CaptureAsync`).
- `static CaptureAsync(CellBox box, CaptureOptions? options = null) -> Task<Schematic>` — Loads the tiles under the box, then `Schematic.Capture`s.
- `Cells { get; } -> ReadOnlySpan<Voxel>` — The cells, x fastest, then y, then z: `index = (z * SizeY + y) * SizeX + x`.
- `CellsEqual(Schematic other) -> bool` — True when both hold the same cells (and sizes).
- `CellSize { get; } -> double` — The cell edge in metres (the grid it was captured on; 0.5 by default).
- `Count { get; } -> long` — The number of cells: `SizeX * SizeY * SizeZ`.
- `Description { get; init; } -> string` — A description, saved in the file.
- `const FormatVersion = 1 -> int` — The version of the .kschem format that Kiln writes and reads.
- `static FromBytes(byte[] data) -> Schematic` — Reads a .kschem from bytes.
- `Hash { get; } -> string` — SHA-256 of the cells and props (hex): what `Build.PlaceOnce` records.
- `static Load(string path) -> Schematic` — Reads a .kschem file.
- `Name { get; init; } -> string` — The schematic's name, saved in the file.
- `Paste(WorldPosition at, PasteOptions? options = null) -> Task<PasteResult>` — Pastes at `at` (the anchor cell lands on its cell), turned and offset as asked.
- `PasteAt(CellPos corner, PasteOptions? options = null) -> Task<PasteResult>` — Pastes with the low corner at a cell (no rotation is applied here: rotate first with `Schematic.Rotated`).
- `Props { get; } -> IReadOnlyList<SchematicProp>` — The props, each with its position relative to the low corner.
- `static Read(Stream input) -> Schematic` — Reads a .kschem from a stream, from its current position.
- `Rotated(int quarterTurns) -> Schematic` — The schematic turned `quarterTurns` times 90 degrees about +Y (yaw +90 each): cell (x, z) of a box sizeX x sizeZ...
- `Save(string path) -> void` — Writes the schematic as .kschem (atomically: temp file, then rename).
- `SizeX { get; } -> int` — The number of cells along x.
- `SizeY { get; } -> int` — The number of cells along y.
- `SizeZ { get; } -> int` — The number of cells along z.
- `SolidCount { get; } -> long` — Cells that hold matter (fill > 0).
- `Source { get; init; } -> Json.Nodes.JsonObject?` — Where it was captured (null for built or loaded ones without it).
- `TargetBox(WorldPosition at, int rotation = 0, CellPos offset = default(CellPos)) -> CellBox` — The cell box a paste at `at` covers (the anchor cell lands on the cell of `at`).
- `this[int x, int y, int z] { get; } -> Voxel` — The cell at `x`, `y`, `z`, counted from the low corner.
- `ToBytes() -> byte[]` — The schematic as .kschem bytes (what `Schematic.Save` writes to a file).
- `Write(Stream output) -> void` — Writes the schematic as .kschem to a stream.

## SchematicProp — A prop in a schematic: template, position relative to the schematic's low corner (metres), rotation and the item dismantling gives back (0 = the item that places the...
- `SchematicProp(Guid Template, string Name, Vector3 Offset, Quaternion Rotation, uint ItemId)`
- `ItemId { get; init; } -> uint` — The item dismantling gives back: 0 is the item that places the template, `UInt32.MaxValue` is none.
- `Name { get; init; } -> string` — The name of the template when the prop was captured (a label: a paste uses `Template`).
- `Offset { get; init; } -> Vector3` — The prop's position in metres from the schematic's low corner.
- `Rotation { get; init; } -> Quaternion` — The prop's orientation as a quaternion.
- `Template { get; init; } -> Guid` — The GUID of the prop's template; a paste places the prop by it.

## ShapeKind — The geometry of a `BuildShape`.
enum: Box, Sphere, Cylinder

## ShapeOp — The shape operations of `Build`.
enum: Dig, Add, Flatten, Smooth, Paint, Replace

## UndoResult — What an undo did.
- `UndoResult(long Id, string Label, long CellsRestored, long CellsKept, int PropsRemoved, int PropsRestored, bool Complete)`
- `CellsKept { get; init; } -> long` — Cells that changed again after the edit (left alone).
- `CellsRestored { get; init; } -> long` — Cells set back to their old value.
- `Complete { get; init; } -> bool` — True when every restored write read back as written, every removed prop was put back and no cell was left alone...
- `Id { get; init; } -> long` — The id of the journal entry that was reverted.
- `Label { get; init; } -> string` — The label of that entry.
- `PropsRemoved { get; init; } -> int` — Props the edit had placed that the undo removed.
- `PropsRestored { get; init; } -> int` — Props the edit had removed that the undo put back.

## Voxel — One voxel cell as the game stores it (a 0.5 m cube on svn 1285601): `Voxel.Material` and `Voxel.Fill` 0..255 (255 = solid; values between place the surface inside the...
- `Voxel()`
- `Voxel(ushort Raw)`
- `static Air { get; } -> Voxel` — The empty cell: material 0 and fill 0 (`Voxel.Raw` 0).
- `Fill { get; } -> byte` — How much matter the cell holds (the high byte of `Voxel.Raw`): 255 is solid, 0 is none, and values between place the...
- `IsAir { get; } -> bool` — True when `Voxel.Fill` is 0, whatever the material (the game leaves a surface's material on the air cells next to it).
- `IsBuilding { get; } -> bool` — A building-block material (128..255).
- `IsSolid { get; } -> bool` — True when `Voxel.Fill` is 255, a fully solid cell.
- `Material { get; } -> byte` — The material id (the low byte of `Voxel.Raw`): 1..127 terrain, 128..255 a building-block material; 0 is no material.
- `static Of(byte material, byte fill = 255) -> Voxel` — A cell of `material` filled to `fill` (255 solid).
- `Raw { get; init; } -> ushort` — The engine's own 16-bit cell value: the material in the low byte and the fill in the high byte (`material | fill << 8`).

## VoxelGrid — The voxel grid's geometry: world metres of cell (0,0,0)'s low corner and the cell size.
- `VoxelGrid(double CellSize, WorldPosition Origin, int CellsX, int CellsZ, int TileCells, int Level)`
- `CellSize { get; init; } -> double` — The edge length of a cell in metres (0.5 on the current build).
- `CellsX { get; init; } -> int` — The grid's extent in cells along x: valid cells are below it (20480 on the current builds).
- `CellsZ { get; init; } -> int` — The grid's extent in cells along z: valid cells are below it (20480 on the current builds).
- `Level { get; init; } -> int` — The engine's edit level of the voxel pyramid that the cells belong to (6 on the current build, which makes them 0.5 m).
- `Origin { get; init; } -> WorldPosition` — The world position of cell (0, 0, 0)'s low corner, in metres.
- `SizeX { get; } -> double` — The grid's extent along x in metres: `VoxelGrid.CellsX` times `VoxelGrid.CellSize`.
- `SizeZ { get; } -> double` — The grid's extent along z in metres: `VoxelGrid.CellsZ` times `VoxelGrid.CellSize`.
- `TileCells { get; init; } -> int` — The size of a streaming tile in cells along x (the game streams 128 m tiles around players and bases).
