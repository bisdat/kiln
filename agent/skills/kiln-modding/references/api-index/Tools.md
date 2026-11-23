# Tools (Kiln.Tools)

## TerrainToolAction `0.2.14` — A declarative, per-tool terrain action.
- `Mode { get; init; } -> TerrainToolMode` — The operation to perform.
- `RemoveMaterials { get; init; } -> IReadOnlyList<byte>` — Terrain cell identities allowed for removal, per tool.
- `TerrainOnly { get; init; } -> bool` — Must be true.

## TerrainToolMode `0.2.14` — A building-tool action in the game's authoritative terrain pipeline.
enum: Remove, Add

## ToolMaterials `0.2.14` — Read-only observations of a building tool's selector.
- `static Available { get; } -> bool` — True when the qualified per-player, separate-source observation producer is available.
- `static TrySnapshot(EntityId actor, ItemId tool, out ToolMaterialSnapshot? snapshot) -> bool` — Reads an owned observation.

## ToolMaterialSnapshot `0.2.14` — An immutable, bounded snapshot.
- `Actor { get; } -> EntityId` — The observed player.
- `Entries { get; } -> IReadOnlyList<ToolMaterialStock>` — At most 64 immutable entries, in pool order.
- `Revision { get; } -> ulong` — The source observation's revision.
- `SelectedItem { get; } -> ItemId?` — Null when no owned, in-stock pool entry is selected.
- `SelectedMaterial { get; } -> byte?` — The selected terrain cell, or null.
- `Session { get; } -> ulong` — The world/session this observation belongs to.
- `Tool { get; } -> ItemId` — The family root, never the vanilla root's alias.

## ToolMaterialStock `0.2.14` — Owned material counts from the game's normal building supplies.
- `ToolMaterialStock(ItemId Item, byte TerrainMaterial, int VoxelsPerItem, ulong Backpack, ulong MagicChests)`
- `Backpack { get; init; } -> ulong` — How many of the item the player carries in the backpack.
- `Item { get; init; } -> ItemId` — The pool entry's placement item.
- `MagicChests { get; init; } -> ulong` — How many of the item the connected magic chests hold.
- `TerrainMaterial { get; init; } -> byte` — The terrain cell the item places (1 to 127).
- `VoxelsPerItem { get; init; } -> int` — How many voxels one item pays for (1 to 1,000,000); placement pays the ceiling of the added volume divided by it.
