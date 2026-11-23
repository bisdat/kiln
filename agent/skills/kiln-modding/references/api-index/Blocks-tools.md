# Blocks (Kiln.Blocks): tools

## BuildingToolBlock `0.2.14` — A building-mode terrain tool with its own root name, description, pool and brush family.
- `Brushes { get; init; } -> IReadOnlyList<PartRef<TerrainBrushBlock>>` — One to 64 named brushes.
- `Description { get; init; } -> string?` — The tool's own root description.
- `Icon { get; init; } -> string?` — A square PNG in the mod's folder.
- `Identity { get; init; } -> BlockIdentity?` — An existing tool identity to retain.
- `MaterialPool { get; init; } -> PartRef<ToolMaterialPoolBlock>?` — The terrain selector pool.
- `Name { get; init; } -> string` — The tool's own root name, not Overgrowth Tool's.
- `Primary { get; init; } -> TerrainToolAction` — The game's building-mode primary action.
- `Secondary { get; init; } -> TerrainToolAction` — The game's building-mode secondary action.
- `Was { get; init; } -> string?` — The old key, preserving the tool identity after a rename.

## TerrainBrushBlock `0.2.14` — A separately identified brush item, owned by one building-tool family.
- `Description { get; init; } -> string?` — The brush's own description.
- `Identity { get; init; } -> BlockIdentity?` — An identity to retain from an existing mod.
- `Name { get; init; } -> string` — The brush's own name.
- `Radius { get; init; } -> double` — Radius in metres, finite 0.25..8.
- `Shape { get; init; } -> TerrainBrushShape` — The shape; currently a sphere.
- `Was { get; init; } -> string?` — The old block key, preserving its derived identity after a rename.

## TerrainBrushShape `0.2.14` — The bounded shape a terrain tool uses.
enum: Sphere

## ToolMaterialPoolBlock `0.2.14` — A tool's independent selector pool, in UI order.
- `Entries { get; init; } -> IReadOnlyList<ToolMaterialPoolEntry>` — One to 64 distinct items and distinct terrain materials.
- `Name { get; init; } -> string` — The pool's readable name.
- `Was { get; init; } -> string?` — The old key, preserving the pool identity after a rename.

## ToolMaterialPoolEntry `0.2.14` — A placeable terrain item and its authoritative cost, not its mining drop.
- `ToolMaterialPoolEntry(ItemRef Item, byte TerrainMaterial, int VoxelsPerItem)`
- `Item { get; init; } -> ItemRef` — The item that places the material.
- `TerrainMaterial { get; init; } -> byte` — A terrain cell identity, 1..127, never a building cell.
- `VoxelsPerItem { get; init; } -> int` — Actual added voxel volume bought by one item; payment rounds up.
