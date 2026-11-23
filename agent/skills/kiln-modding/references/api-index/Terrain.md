# Terrain (Kiln.Terrain)

## Terrain `0.2.3` — Bounded terrain overlays: a mod declares a patch of ground (an ore vein, a mound, a tunnel) and Kiln applies it once per world over several ticks and remembers which...
- `static Applied { get; } -> IReadOnlyList<TerrainPlanInfo>` — The calling mod's kept plans in this world.
- `static Apply(TerrainPatch patch, TerrainApplyOptions? options = null) -> Task<TerrainResult>` — Applies `patch`: waits for the world to run, then writes one batch per tick.
- `static Changed -> Action<TerrainChangedEvent>?` — One batch of an overlay reached the world.
- `static Forget(string id) -> bool` — Drops the calling mod's record of plan `id` in this world: the plan no longer owns its blocks, and the next...
- `static Forget(string id, bool restoreTerrain) -> TerrainResult` `0.2.4` — Forget ownership, or explicitly refuse terrain restoration (original cells are not stored).
- `static Inspect(TerrainPatch patch, TerrainApplyOptions? options = null) -> Task<TerrainPlanReport>` — A dry run of `patch`: its size, what an `Terrain.Apply` would do now and the blocks it would leave alone.

## TerrainApplyOptions `0.2.3` — How `Terrain.Apply` runs.
- `Cancel { get; init; } -> CancellationToken` — Stops the patch between two batches; the batches already written stay.
- `CellsPerTick { get; init; } -> int` — Per-call ceiling: 512 to 262,144, default 32,768.
- `InBases { get; init; } -> bool` — Write inside player bases too.
- `InstallChoice { get; init; } -> TerrainInstallChoice` `0.2.4` — Unknown-history decision.
- `LoadTimeout { get; init; } -> TimeSpan` — How long a batch waits for its voxel tiles to stream in.
- `Progress { get; init; } -> IProgress<TerrainProgress>?` — Told after every batch, on the tick thread.

## TerrainChangedEvent `0.2.3` — One batch of an overlay reached the world.
- `Batch { get; } -> int` — This batch's number, from 1.
- `BatchCount { get; } -> int` — How many batches the patch has.
- `Box { get; } -> CellBox` — The cells of this batch.
- `CellsChanged { get; } -> long` — Cells whose value changed in this batch.
- `Last { get; } -> bool` — The patch's last batch.
- `Mod { get; } -> string` — The mod that owns the plan.
- `Plan { get; } -> string` — The plan's name inside the mod (`TerrainPatch.Id`).
- `Version { get; } -> int` — The plan's version (`TerrainPatch.Version`).

## TerrainConflict `0.2.3` — One block an overlay left alone.
- `TerrainConflict(TerrainConflictKind Kind, CellBox Block, string? Owner = null)`
- `Block { get; init; } -> CellBox` — The block's cells (8 per axis, on the world's 8-cell grid).
- `Kind { get; init; } -> TerrainConflictKind` — Why the block was not written.
- `Owner { get; init; } -> string?` — For `TerrainConflictKind.OtherPlan`: "mod/plan".

## TerrainConflictKind `0.2.3` — Why a block was not written.
enum: PlayerEdit, OtherPlan, BaseZone

## TerrainHeightfield `0.2.3` — One height per column of the region (x fastest, then z), in cells above the region's floor.
- `TerrainHeightfield(float[] Heights, byte Fill, TerrainHeightMode Mode = TerrainHeightMode.Replace, byte Surface = 0, int SurfaceDepth = 0)`
- `Fill { get; init; } -> byte` — The palette number of the column's matter (not 0, not air).
- `Heights { get; init; } -> float[]` — `SizeX * SizeZ` heights, each 0 to the region's `SizeY`.
- `Mode { get; init; } -> TerrainHeightMode` — How the column meets the ground that is there.
- `Surface { get; init; } -> byte` — The palette number of the column's top cells; 0 = the same as `Fill`.
- `SurfaceDepth { get; init; } -> int` — How many top cells use `Surface`.

## TerrainHeightMode `0.2.3` — How a `TerrainHeightfield` meets the ground that is there.
enum: Replace, AddOnTop

## TerrainInstallChoice `0.2.4` — The explicit decision for a kept plan whose ownership history is unknown.
enum: Automatic, Adopt, InstallAnyway

## TerrainPaletteEntry `0.2.3` — What a palette number of a patch stands for.
- `TerrainPaletteEntry(byte Material, byte Fill = 255)`
- `Fill { get; init; } -> byte` — How full the cell is, 1 to 255 (255 solid).
- `Material { get; init; } -> byte` — 0 = air (the cell is dug out); 1 to 116 a terrain material of the game; 128 to 212 a building-block material.

## TerrainPatch `0.2.3` — A bounded patch of ground: an ore vein, a mound, a tunnel.
- `TerrainPatch(string Id, int Version, CellBox Region, TerrainSource Source, IReadOnlyList<TerrainPaletteEntry> Palette, TerrainPersistence Persistence = TerrainPersistence.Kept)`
- `static FromJson(string json) -> TerrainPatch` — Reads a patch from its JSON shape (docs/kiln/services/terrain.md).
- `Id { get; init; } -> string` — The plan's name inside the mod: 1 to 64 characters of a-z, 0-9, '.', '_', '-'.
- `Palette { get; init; } -> IReadOnlyList<TerrainPaletteEntry>` — What the source's numbers mean (number `n` is entry `n - 1`).
- `Persistence { get; init; } -> TerrainPersistence` — Saved and recorded, or not.
- `Region { get; init; } -> CellBox` — The cells the patch may touch: at most 256 per axis and 1,048,576 in all.
- `Source { get; init; } -> TerrainSource` — The cells.
- `ToJson() -> string` — The patch as JSON; `TerrainPatch.FromJson` reads it back equal.
- `Version { get; init; } -> int` — 1 or higher.

## TerrainPersistence `0.2.3` — Whether an overlay is saved with the world.
enum: Kept, Transient

## TerrainPlanInfo `0.2.3` — A kept plan of the calling mod in this world (`Terrain.Applied`).
- `TerrainPlanInfo(string Plan, int Version, CellBox Region, bool Complete, int Blocks, int GivenUp, DateTimeOffset AppliedAt)`
- `Adopted { get; init; } -> bool` `0.2.4` — The plan adopted current terrain; its original terrain cannot be restored.
- `AppliedAt { get; init; } -> DateTimeOffset` — When the plan was last applied (UTC): set when an apply finishes or adopts the current cells.
- `Blocks { get; init; } -> int` — Blocks the plan owns.
- `Complete { get; init; } -> bool` — False while an apply is under way or was interrupted.
- `GivenUp { get; init; } -> int` — Blocks the plan gave up after a conflict.
- `InstalledWithoutProof { get; init; } -> bool` `0.2.4` — The initial automatic installation lacked history/proof and was admitted below the presence bound.
- `Plan { get; init; } -> string` — The plan's name inside the mod (`TerrainPatch.Id`).
- `PresenceShare { get; init; } -> double?` `0.2.4` — The initial installation's measured exact-match fraction, retained in the ledger; otherwise null.
- `Region { get; init; } -> CellBox` — The cells the plan may touch.
- `Version { get; init; } -> int` — The version that is applied.

## TerrainPlanReport `0.2.3` — What `Terrain.Inspect` found.
- `TerrainPlanReport(bool Ok, string? Reason, CellBox Box, long Cells, int Blocks, int Batches, long EstimatedBytes, TerrainState Would, IReadOnlyList<TerrainConflict> Conflicts)`
- `Batches { get; init; } -> int` — Ticks an apply takes at least.
- `Blocks { get; init; } -> int` — 8-cell blocks the patch addresses.
- `Box { get; init; } -> CellBox` — The patch's region.
- `Cells { get; init; } -> long` — Cells the patch addresses.
- `Conflicts { get; init; } -> IReadOnlyList<TerrainConflict>` — The blocks an apply would leave alone.
- `EstimatedBytes { get; init; } -> long` — What the save grows by at most (1,024 bytes a block, before the game packs them).
- `Ok { get; init; } -> bool` — False when the inspection could not be made: the world is not running, or the tiles did not load.
- `PresenceShare { get; init; } -> double?` `0.2.4` — Exact-match fraction measured for the automatic first-install decision; otherwise null.
- `Reason { get; init; } -> string?` — Why the apply would not complete, or why the inspection failed; null when there is nothing to say.
- `Would { get; init; } -> TerrainState` — How an apply would end now.

## TerrainProgress `0.2.3` — How far an `Terrain.Apply` is.
- `TerrainProgress()`
- `TerrainProgress(int Batches, int BatchCount, long ChangedCells)`
- `BatchCount { get; init; } -> int` — How many batches the patch has.
- `Batches { get; init; } -> int` — The number of the batch that was just written, counting from 1.
- `ChangedCells { get; init; } -> long` — Cells whose value changed so far, over the batches written by this call.

## TerrainResult `0.2.3` — What `Terrain.Apply` did.
- `TerrainResult(bool Ok, string? Reason, TerrainState State, string Plan, int Version, long ChangedCells, int Batches, IReadOnlyList<TerrainConflict> Conflicts)`
- `Batches { get; init; } -> int` — Batches written to the world by this call.
- `ChangedCells { get; init; } -> long` — Cells whose value changed, in the batches that were written.
- `Conflicts { get; init; } -> IReadOnlyList<TerrainConflict>` — The blocks that were left alone; empty when there are none.
- `InstalledWithoutProof { get; init; } -> bool` `0.2.4` — The initial automatic installation used the below-bound presence heuristic without history/proof.
- `Ok { get; init; } -> bool` — True when the call completed, found the plan already applied, or adopted the current cells; see `State`.
- `Plan { get; init; } -> string` — The plan's name inside the mod (`TerrainPatch.Id`).
- `PresenceShare { get; init; } -> double?` `0.2.4` — Exact-match fraction of addressed cells when the initial decision used terrain sampling; otherwise null.
- `Reason { get; init; } -> string?` — Why it ended as it did when it did not complete; null when there is nothing to say.
- `State { get; init; } -> TerrainState` — How the call ended.
- `Version { get; init; } -> int` — The plan's version (`TerrainPatch.Version`).

## TerrainSource `0.2.3` — Where a patch's cells come from: `TerrainVoxelVolume` or `TerrainHeightfield`.
- `TerrainSource(TerrainSource original)`

## TerrainState `0.2.3` — How an overlay call ended.
enum: Completed, AlreadyApplied, Conflict, Partial, Refused, NeedsAdoption `0.2.4`, Adopted `0.2.4`

## TerrainVoxelVolume `0.2.3` — One palette number per cell of the region, x fastest, then y, then z (`CellBox.Index`).
- `TerrainVoxelVolume(byte[] Cells)`
- `Cells { get; init; } -> byte[]` — The palette numbers, one byte per cell of the region (x fastest, then y, then z).
