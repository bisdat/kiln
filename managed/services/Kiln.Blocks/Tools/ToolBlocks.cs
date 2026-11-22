// SPDX-License-Identifier: MIT
using System.Text.Json.Nodes;
namespace Kiln;

/// <summary>The bounded shape a terrain tool uses. Since 0.2.0-dev.30.5.</summary>
public enum TerrainBrushShape
{
    /// <summary>A sphere centred at the game's building cursor.</summary>
    Sphere = 0,
}

/// <summary>A separately identified brush item, owned by one building-tool family. It never joins the vanilla
/// Overgrowth family. Declaring the part alone does not publish an orphan item. Since 0.2.0-dev.30.5.</summary>
[BlockClients(ClientsLevel.Mod)]
public sealed record TerrainBrushBlock : IBlock
{
    /// <summary>The brush's own name.</summary>
    public required string Name { get; init; }
    /// <summary>The brush's own description.</summary>
    public string? Description { get; init; }
    /// <summary>An identity to retain from an existing mod. Null derives one from this mod and key.</summary>
    public BlockIdentity? Identity { get; init; }
    /// <summary>The shape; currently a sphere.</summary>
    public TerrainBrushShape Shape { get; init; } = TerrainBrushShape.Sphere;
    /// <summary>Radius in metres, finite 0.25..8.</summary>
    public required double Radius { get; init; }
    /// <summary>The old block key, preserving its derived identity after a rename.</summary>
    public string? Was { get; init; }
}

/// <summary>A placeable terrain item and its authoritative cost, not its mining drop. Since 0.2.0-dev.30.5.</summary>
/// <param name="Item">The item that places the material.</param>
/// <param name="TerrainMaterial">A terrain cell identity, 1..127, never a building cell.</param>
/// <param name="VoxelsPerItem">Actual added voxel volume bought by one item; payment rounds up.</param>
public sealed record ToolMaterialPoolEntry(ItemRef Item, byte TerrainMaterial, int VoxelsPerItem);

/// <summary>A tool's independent selector pool, in UI order. Counts use the game's normal building supplies:
/// backpack and connected magic chests, never ordinary chests. Pools have at most 64 entries. Since 0.2.0-dev.30.5.</summary>
[BlockClients(ClientsLevel.Mod)]
public sealed record ToolMaterialPoolBlock : IBlock
{
    /// <summary>The pool's readable name.</summary>
    public required string Name { get; init; }
    /// <summary>One to 64 distinct items and distinct terrain materials. The compiler copies the list into Kiln-owned storage.</summary>
    public required IReadOnlyList<ToolMaterialPoolEntry> Entries { get; init; }
    /// <summary>The old key, preserving the pool identity after a rename.</summary>
    public string? Was { get; init; }
}

/// <summary>A building-mode terrain tool with its own root name, description, pool and brush family.
/// Engine policies are immutable native values; no managed callback runs in an impact detour. Missing qualified
/// hooks refuse the action instead of performing unrestricted terraforming. Since 0.2.0-dev.30.5.</summary>
[BlockClients(ClientsLevel.Mod)]
public sealed record BuildingToolBlock : IBlock
{
    /// <summary>The tool's own root name, not Overgrowth Tool's.</summary>
    public required string Name { get; init; }
    /// <summary>The tool's own root description.</summary>
    public string? Description { get; init; }
    /// <summary>An existing tool identity to retain. Null derives one from this mod and key.</summary>
    public BlockIdentity? Identity { get; init; }
    /// <summary>The terrain selector pool. Required for an Add action; optional for Remove-only tools.</summary>
    public PartRef<ToolMaterialPoolBlock>? MaterialPool { get; init; }
    /// <summary>One to 64 named brushes. A brush belongs to exactly one tool. The compiler copies the list into Kiln-owned storage.</summary>
    public required IReadOnlyList<PartRef<TerrainBrushBlock>> Brushes { get; init; }
    /// <summary>The game's building-mode primary action.</summary>
    public required TerrainToolAction Primary { get; init; }
    /// <summary>The game's building-mode secondary action.</summary>
    public required TerrainToolAction Secondary { get; init; }
    /// <summary>A square PNG in the mod's folder. Null keeps the tool starter's icon.</summary>
    public string? Icon { get; init; }
    /// <summary>The old key, preserving the tool identity after a rename.</summary>
    public string? Was { get; init; }
}

internal static class ToolBlockValidation
{
    internal static void Text(string name, string? description)
    {
        if (string.IsNullOrWhiteSpace(name) || name.Length > 1024 || name.Contains('\0') || description?.Length > 16384 || description?.Contains('\0') == true)
            throw new ArgumentException("A tool or brush needs its own bounded name and description.");
    }
    internal static void Brush(TerrainBrushBlock block)
    {
        ArgumentNullException.ThrowIfNull(block); Text(block.Name,block.Description);
        if (block.Shape != TerrainBrushShape.Sphere || !double.IsFinite(block.Radius) || block.Radius is < 0.25 or > 8)
            throw new ArgumentException("A terrain brush is a sphere of radius 0.25..8 metres.");
        if (block.Was is {} was && !MaterialPoolValidation.Valid(was)) throw new ArgumentException("Invalid brush Was.");
    }
    internal static JsonObject PoolNode(ToolMaterialPoolBlock block)
    {
        ArgumentNullException.ThrowIfNull(block); Text(block.Name,null);
        if (block.Entries is not { Count: >=1 and <=64 }) throw new ArgumentException("A tool pool lists 1..64 entries.");
        var items=new HashSet<string>(StringComparer.OrdinalIgnoreCase); var cells=new HashSet<byte>(); var entries=new JsonArray();
        foreach (var e in block.Entries)
        {
            if (e is null || string.IsNullOrWhiteSpace(e.Item.Value) || !items.Add(e.Item.Value) || e.TerrainMaterial is <1 or >127 || !cells.Add(e.TerrainMaterial) || e.VoxelsPerItem is <1 or >1000000)
                throw new ArgumentException("Tool pool entries need distinct items/cells, terrainMaterial 1..127 and voxelsPerItem 1..1000000.");
            entries.Add(new JsonObject {["item"]=e.Item.Value,["terrainMaterial"]=e.TerrainMaterial,["voxelsPerItem"]=e.VoxelsPerItem});
        }
        var node=new JsonObject {["name"]=block.Name,["entries"]=entries};
        if (block.Was is {} was) { if (!MaterialPoolValidation.Valid(was)) throw new ArgumentException("Invalid tool pool Was."); node["was"]=was; }
        return node;
    }
}
