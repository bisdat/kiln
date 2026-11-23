# Build (Kiln.Build)

## BlockBreakFlags `0.2.15` — Controlled breaking options.
enum: None, Instant, Drops, WorldBuildings, PlayerTerrain

## BlockBreakOptions `0.2.15` — One exact, loaded cell box, optionally selected by a cell mask.
- `Build { get; init; } -> BuildOptions` — The existing build permissions, actor, journal and label.
- `Damage { get; init; } -> int[]` — Five nonnegative damage values, by hardness (up to 100000).
- `Flags { get; init; } -> BlockBreakFlags`
- `Materials { get; init; } -> byte[]` — Raw voxel material ids to include.
- `ProtectedSphere { get; init; } -> BlockBreakSphere?` — Optional sphere.
- `Selection { get; init; } -> bool[]?` — Optional mask in x,y,z order, with exactly the box's cell count.
- `TerrainHealAfter { get; init; } -> TimeSpan` — Forget terrain damage after this inactivity.
- `TerrainHealthScale { get; init; } -> double` — Terrain damage threshold as a multiple of material health.

## BlockBreakResult `0.2.15` — Counts and refusal of a controlled break.
- `BlockBreakResult(BlockRefusal Refusal, ulong Considered, ulong Damaged, ulong Removed, ulong Dropped, ulong ProtectedCells, ulong UnknownCells, ulong UnbreakableCells, ulong ExcludedCells, bool MayHaveChanged, bool Verified, uint BlocksWritten, bool Kept)`
- `BlocksWritten { get; init; } -> uint`
- `Considered { get; init; } -> ulong`
- `Damaged { get; init; } -> ulong`
- `Dropped { get; init; } -> ulong`
- `ExcludedCells { get; init; } -> ulong`
- `Kept { get; init; } -> bool`
- `MayHaveChanged { get; init; } -> bool`
- `Ok { get; } -> bool`
- `ProtectedCells { get; init; } -> ulong`
- `Reason { get; } -> string?`
- `Refusal { get; init; } -> BlockRefusal`
- `Removed { get; init; } -> ulong`
- `UnbreakableCells { get; init; } -> ulong`
- `UnknownCells { get; init; } -> ulong`
- `Verified { get; init; } -> bool`

## BlockBreakSphere `0.2.15` — Sphere selection with a caller-controlled protection margin and minimum radius.
- `BlockBreakSphere(WorldPosition Center, double Radius, double Margin = 0, double MinimumRadius = 0)`
- `Center { get; init; } -> WorldPosition`
- `Margin { get; init; } -> double`
- `MinimumRadius { get; init; } -> double`
- `Radius { get; init; } -> double`

## BlockHardness `0.2.15` — The material's mining hardness class.
enum: Soft, SlightlyHard, ModeratelyHard, Hard, VeryHard, Unbreakable

## BlockInfo `0.2.15` — Loaded cell facts.
- `BlockInfo(BuildBlockKind Kind, byte Material, byte Fill, BlockProvenance Provenance, BlockHardness? Hardness, int? Health, int? Damage, WorldPosition Center)`
- `Center { get; init; } -> WorldPosition`
- `Damage { get; init; } -> int?`
- `Fill { get; init; } -> byte`
- `Hardness { get; init; } -> BlockHardness?`
- `Health { get; init; } -> int?`
- `Kind { get; init; } -> BuildBlockKind`
- `Material { get; init; } -> byte`
- `Provenance { get; init; } -> BlockProvenance`

## BlockProvenance `0.2.15` — Comparison with the shipped world, not ownership or execution side.
enum: Unknown, PlayerPlaced, Natural

## BlockQueryResult `0.2.15` — A successful query may have no hit.
- `BlockQueryResult(BlockInfo? Info, BlockRefusal Refusal)`
- `Found { get; } -> bool`
- `Info { get; init; } -> BlockInfo?`
- `Ok { get; } -> bool`
- `Reason { get; } -> string?`
- `Refusal { get; init; } -> BlockRefusal`

## BlockRefusal `0.2.15` — A block operation's explicit refusal.
enum: None, SymbolMissing, ServiceMissing, ServiceOlder, WrongSide, Offline, NoSession, InvalidArgument, WrongThread, NotLoaded, Zone, Base, Faulted, Unverified, Canceled, DamageMapFull

## Build — Changes the world the way the game saves and replicates it, so vanilla clients see the result: voxel cells (terrain and building blocks), shape edits (dig, add...
- `static Add(BuildShape shape, byte material, BuildOptions? options = null) -> Task<BuildResult>` — Fills the shape with `material` (a voxel material id, e.g. from `Build.MaterialOf`).
- `static Available { get; } -> bool` — True when kiln.build runs on this game build (capability `kiln.build`).
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
- `static PlaceProp(Guid template, WorldPosition position, Quaternion? rotation = null, uint itemId = 0, BuildOptions? options = null) -> EntityId` — Places a prop the way the game's building helper does (CurrentTransform, DisableRandomLootOnDestroy, UsedItem).
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
- `Actor { get; } -> EntityId`
- `Box { get; } -> CellBox`
- `Changed { get; } -> bool`
- `JournalId { get; } -> long` — The journal entry (0 = not journaled).
- `Kind { get; } -> BuildEditKind`
- `Label { get; } -> string?`
- `Origin { get; } -> string`
- `PropsPlaced { get; } -> IReadOnlyList<EntityId>`
- `PropsRemoved { get; } -> IReadOnlyList<EntityId>`
- `Result { get; } -> BuildResult?` — The voxel part (null for prop-only edits).

## BuildEditingEvent — Posted before a Kiln edit (managed API, schematics, commands) changes the world.
- `Actor { get; } -> EntityId` — The player the edit is for, or none (Kiln itself).
- `Box { get; } -> CellBox` — The cells the edit may change (props: the cell of the prop's position).
- `Kind { get; } -> BuildEditKind`
- `Label { get; } -> string?`
- `Origin { get; } -> string` — The mod that asked for it.

## BuildEditKind — What a Kiln edit is.
enum: Write, Dig, Add, Flatten, Smooth, Paint, Replace, Paste, PlaceProp, RemoveProp, Undo

## BuildException — An edit Kiln could not run at all (service unavailable, bad box, engine fault).
- `BuildException(string message, int code = 0)`
- `Code { get; } -> int`

## BuildFlags — How a Kiln edit may act.
enum: None, RespectZones, InBases, AllowAltars, Unloaded, DryRun, Transient

## BuildOptions — Optional settings of an edit.
- `Actor { get; init; } -> EntityId` — The player the edit is for (zones with `BuildFlags.RespectZones`, journal owner, events); none = Kiln itself.
- `static Default { get; } -> BuildOptions`
- `Flags { get; init; } -> BuildFlags`
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
- `Changed { get; } -> bool`
- `Kept { get; init; } -> bool` — The touched chunks are in Kiln's save regions (the game saves them).
- `Milliseconds { get; init; } -> double`
- `Ok { get; } -> bool` `0.2.2` — The edit was not refused (`BuildResult.Refused` is false): it ran, or was a dry run.
- `Reason { get; } -> string?` `0.2.2` — Why the edit was refused, in words (`BuildResult.Refusal` is the code); null when `BuildResult.Ok`.
- `Refusal { get; init; } -> BuildRefusal`
- `Refused { get; } -> bool`
- `Verified { get; init; } -> bool` — The cells read back equal to what was written.
- `Zone { get; init; } -> uint` — The denying zone (`BuildRefusal.Zone`) or base altar (`BuildRefusal.Base`).

## BuildShape — A region for shape edits, in world metres.
- `BuildShape()`
- `BuildShape(ShapeKind Kind, WorldPosition Center, Vector3 Half)`
- `static Box(WorldPosition center, double halfX, double halfY, double halfZ) -> BuildShape`
- `Center { get; init; } -> WorldPosition`
- `static Cylinder(WorldPosition center, double radius, double halfHeight) -> BuildShape`
- `Half { get; init; } -> Vector3` — Box: half extents.
- `Kind { get; init; } -> ShapeKind`
- `static Sphere(WorldPosition center, double radius) -> BuildShape`

## BuildStatus — kiln.build's state (any thread).
- `BuildStatus(bool Read, bool Write, bool Props, bool World, bool Persist, ulong Reads, ulong Writes, ulong CellsRead, ulong CellsWritten, ulong BlocksWritten, ulong PropsPlaced, ulong PropsRemoved, ulong Refused, ulong KeptChunks, ulong SavesSeen, string Detail)`
- `BlocksWritten { get; init; } -> ulong`
- `CellsRead { get; init; } -> ulong`
- `CellsWritten { get; init; } -> ulong`
- `Detail { get; init; } -> string`
- `KeptChunks { get; init; } -> ulong`
- `Persist { get; init; } -> bool`
- `Props { get; init; } -> bool`
- `PropsPlaced { get; init; } -> ulong`
- `PropsRemoved { get; init; } -> ulong`
- `Read { get; init; } -> bool`
- `Reads { get; init; } -> ulong`
- `Refused { get; init; } -> ulong`
- `SavesSeen { get; init; } -> ulong`
- `World { get; init; } -> bool`
- `Write { get; init; } -> bool`
- `Writes { get; init; } -> ulong`

## CaptureOptions — Capture settings.
- `Anchor { get; init; } -> CellPos?` — The paste point, in cells from the low corner.
- `Author { get; init; } -> string?`
- `Description { get; init; } -> string?`
- `LoadTimeout { get; init; } -> TimeSpan`
- `Name { get; init; } -> string?`
- `Props { get; init; } -> PropFilter` — Which props to keep.

## CellBox — A box of cells: low corner `(X, Y, Z)` and size.
- `CellBox()`
- `CellBox(int X, int Y, int Z, int SizeX, int SizeY, int SizeZ)`
- `Contains(CellPos p) -> bool`
- `Count { get; } -> long`
- `End { get; } -> CellPos` — The cell just outside the high corner.
- `static FromCorners(CellPos a, CellPos b) -> CellBox` — The box spanning two corner cells (both included, any order).
- `Index(int x, int y, int z) -> int`
- `IsEmpty { get; } -> bool`
- `Min { get; } -> CellPos`
- `Offset(CellPos d) -> CellBox`
- `SizeX { get; init; } -> int`
- `SizeY { get; init; } -> int`
- `SizeZ { get; init; } -> int`
- `X { get; init; } -> int`
- `Y { get; init; } -> int`
- `Z { get; init; } -> int`

## CellPos — Cell coordinates (integer index into the voxel grid; see `Build.CellOf`).
- `CellPos()`
- `CellPos(int X, int Y, int Z)`
- `static operator +(CellPos a, CellPos b) -> CellPos`
- `static operator -(CellPos a, CellPos b) -> CellPos`
- `X { get; init; } -> int`
- `Y { get; init; } -> int`
- `Z { get; init; } -> int`

## FindWallFlags `0.2.15` — Additional terrain hits for a wall search.
enum: None, PlayerTerrain, Terrain

## JournalInfo — One recorded edit.
- `JournalInfo(long Id, string Owner, string Label, BuildEditKind Kind, DateTimeOffset At, long Cells, int PropsPlaced, int PropsRemoved)`
- `At { get; init; } -> DateTimeOffset`
- `Cells { get; init; } -> long`
- `Id { get; init; } -> long`
- `Kind { get; init; } -> BuildEditKind`
- `Label { get; init; } -> string`
- `Owner { get; init; } -> string`
- `PropsPlaced { get; init; } -> int`
- `PropsRemoved { get; init; } -> int`

## PasteMode — How a paste writes cells.
enum: Replace, Merge

## PasteOptions — Paste settings.
- `Actor { get; init; } -> EntityId`
- `ClearProps { get; init; } -> bool` — Remove building props (UsedItem) already inside the target box first.
- `Flags { get; init; } -> BuildFlags`
- `Journal { get; init; } -> bool`
- `Label { get; init; } -> string?`
- `LoadTimeout { get; init; } -> TimeSpan`
- `Mode { get; init; } -> PasteMode`
- `Offset { get; init; } -> CellPos` — Extra offset in cells after rotation.
- `Props { get; init; } -> bool` — Place the schematic's props.
- `Rotation { get; init; } -> int` — Quarter turns about +Y (yaw +90 degrees each): 0..3.

## PasteResult — What a paste did.
- `PasteResult(CellBox Box, BuildResult Cells, int PropsPlaced, int PropsFailed, int PropsCleared, long JournalId, IReadOnlyList<EntityId> Props)`
- `Box { get; init; } -> CellBox`
- `Cells { get; init; } -> BuildResult`
- `JournalId { get; init; } -> long`
- `Ok { get; } -> bool`
- `Props { get; init; } -> IReadOnlyList<EntityId>`
- `PropsCleared { get; init; } -> int`
- `PropsFailed { get; init; } -> int`
- `PropsPlaced { get; init; } -> int`
- `Reason { get; } -> string?` `0.2.2` — Why the paste is not ok: the cells' refusal (`BuildResult.Reason`), else how many props could not be placed; null...

## PlacedProp — A prop in the world.
- `PlacedProp(EntityId Entity, Guid Template, string TemplateName, WorldPosition Position, Quaternion Rotation, Vector3 Scale, uint ItemId, TemplateKind Kind, PropFlags Flags)`
- `Entity { get; init; } -> EntityId`
- `Flags { get; init; } -> PropFlags`
- `ItemId { get; init; } -> uint`
- `Kind { get; init; } -> TemplateKind`
- `Position { get; init; } -> WorldPosition`
- `Rotation { get; init; } -> Quaternion`
- `Scale { get; init; } -> Vector3`
- `Template { get; init; } -> Guid`
- `TemplateName { get; init; } -> string`

## PlaceOnceRecord — A PlaceOnce record (one per key, per mod, per world).
- `At { get; set; } -> double[]`
- `Box { get; set; } -> int[]`
- `Cells { get; set; } -> long`
- `Hash { get; set; } -> string`
- `Key { get; set; } -> string`
- `KilnVersion { get; set; } -> string`
- `PlacedAt { get; set; } -> DateTimeOffset`
- `Props { get; set; } -> int`
- `Rotation { get; set; } -> int`
- `Schematic { get; set; } -> string`

## PlaceOnceResult — What `Build.PlaceOnce` did.
- `PlaceOnceResult(bool Placed, bool AlreadyPlaced, PlaceOnceRecord? Record, PasteResult? Paste, string? Error = null)`
- `AlreadyPlaced { get; init; } -> bool`
- `Error { get; init; } -> string?`
- `Ok { get; } -> bool` `0.2.2` — The structure is in the world: placed now, or by an earlier call.
- `Paste { get; init; } -> PasteResult?`
- `Placed { get; init; } -> bool` — True when it placed now; false when the world already had it (`PlaceOnceResult.Record`) or the edit failed...
- `Reason { get; } -> string?` `0.2.2` — Why it is not (`PlaceOnceResult.Error`, else the paste's reason); null when `PlaceOnceResult.Ok`.
- `Record { get; init; } -> PlaceOnceRecord?`

## PlayerBase — A player base: the game's own build zone around a flame altar (BuildZoneSystem).
- `PlayerBase(EntityId Altar, WorldPosition Center, Vector3 Half, bool Abandoned)`
- `Abandoned { get; init; } -> bool`
- `Altar { get; init; } -> EntityId`
- `Center { get; init; } -> WorldPosition`
- `Contains(WorldPosition p) -> bool`
- `Half { get; init; } -> Vector3`

## PropFilter — Which entities `Build.Props` returns.
enum: Static, Placed, Saved, All

## PropFlags
enum: None, Placed, Altar, Saved

## Schematic — A captured region: cells (x fastest, then y, then z) and props.
- `Schematic(int sizeX, int sizeY, int sizeZ, Voxel[] cells, IReadOnlyList<SchematicProp>? props = null, CellPos? anchor = null, double cellSize = 0.5)`
- `Anchor { get; } -> CellPos` — The paste point in cells from the low corner (a paste puts this cell at the target position).
- `Author { get; init; } -> string`
- `static Capture(CellBox box, CaptureOptions? options = null) -> Schematic` — Captures the cells of `box` and the props in it (tiles must be loaded; see `Schematic.CaptureAsync`).
- `static CaptureAsync(CellBox box, CaptureOptions? options = null) -> Task<Schematic>` — Loads the tiles under the box, then `Schematic.Capture`s.
- `Cells { get; } -> ReadOnlySpan<Voxel>`
- `CellsEqual(Schematic other) -> bool` — True when both hold the same cells (and sizes).
- `CellSize { get; } -> double`
- `Count { get; } -> long`
- `Description { get; init; } -> string`
- `const FormatVersion = 1 -> int`
- `static FromBytes(byte[] data) -> Schematic`
- `Hash { get; } -> string` — SHA-256 of the cells and props (hex): what `Build.PlaceOnce` records.
- `static Load(string path) -> Schematic` — Reads a .kschem file.
- `Name { get; init; } -> string`
- `Paste(WorldPosition at, PasteOptions? options = null) -> Task<PasteResult>` — Pastes at `at` (the anchor cell lands on its cell), turned and offset as asked.
- `PasteAt(CellPos corner, PasteOptions? options = null) -> Task<PasteResult>` — Pastes with the low corner at a cell (no rotation is applied here: rotate first with `Schematic.Rotated`).
- `Props { get; } -> IReadOnlyList<SchematicProp>`
- `static Read(Stream input) -> Schematic`
- `Rotated(int quarterTurns) -> Schematic` — The schematic turned `quarterTurns` times 90 degrees about +Y (yaw +90 each): cell (x, z) of a box sizeX x sizeZ...
- `Save(string path) -> void` — Writes the schematic as .kschem (atomically: temp file, then rename).
- `SizeX { get; } -> int`
- `SizeY { get; } -> int`
- `SizeZ { get; } -> int`
- `SolidCount { get; } -> long` — Cells that hold matter (fill > 0).
- `Source { get; init; } -> Json.Nodes.JsonObject?` — Where it was captured (null for built or loaded ones without it).
- `TargetBox(WorldPosition at, int rotation = 0, CellPos offset = default(CellPos)) -> CellBox` — The cell box a paste at `at` covers (the anchor cell lands on the cell of `at`).
- `this[int x, int y, int z] { get; } -> Voxel`
- `ToBytes() -> byte[]`
- `Write(Stream output) -> void`

## SchematicProp — A prop in a schematic: template, position relative to the schematic's low corner (metres), rotation and the item dismantling gives back (0 = the item that places the...
- `SchematicProp(Guid Template, string Name, Vector3 Offset, Quaternion Rotation, uint ItemId)`
- `ItemId { get; init; } -> uint`
- `Name { get; init; } -> string`
- `Offset { get; init; } -> Vector3`
- `Rotation { get; init; } -> Quaternion`
- `Template { get; init; } -> Guid`

## ShapeKind
enum: Box, Sphere, Cylinder

## ShapeOp — The shape operations of `Build`.
enum: Dig, Add, Flatten, Smooth, Paint, Replace

## UndoResult — What an undo did.
- `UndoResult(long Id, string Label, long CellsRestored, long CellsKept, int PropsRemoved, int PropsRestored, bool Complete)`
- `CellsKept { get; init; } -> long` — Cells that changed again after the edit (left alone).
- `CellsRestored { get; init; } -> long` — Cells set back to their old value.
- `Complete { get; init; } -> bool`
- `Id { get; init; } -> long`
- `Label { get; init; } -> string`
- `PropsRemoved { get; init; } -> int`
- `PropsRestored { get; init; } -> int`

## Voxel — One voxel cell as the game stores it (a 0.5 m cube on svn 1285601): `Voxel.Material` and `Voxel.Fill` 0..255 (255 = solid; values between place the surface inside the...
- `Voxel()`
- `Voxel(ushort Raw)`
- `static Air { get; } -> Voxel`
- `Fill { get; } -> byte`
- `IsAir { get; } -> bool`
- `IsBuilding { get; } -> bool` — A building-block material (128..255).
- `IsSolid { get; } -> bool`
- `Material { get; } -> byte`
- `static Of(byte material, byte fill = 255) -> Voxel` — A cell of `material` filled to `fill` (255 solid).
- `Raw { get; init; } -> ushort`

## VoxelGrid — The voxel grid's geometry: world metres of cell (0,0,0)'s low corner and the cell size.
- `VoxelGrid(double CellSize, WorldPosition Origin, int CellsX, int CellsZ, int TileCells, int Level)`
- `CellSize { get; init; } -> double`
- `CellsX { get; init; } -> int`
- `CellsZ { get; init; } -> int`
- `Level { get; init; } -> int`
- `Origin { get; init; } -> WorldPosition`
- `SizeX { get; } -> double`
- `SizeZ { get; } -> double`
- `TileCells { get; init; } -> int`
