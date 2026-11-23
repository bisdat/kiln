// SPDX-License-Identifier: MIT
namespace Kiln;

/// <summary>A building-tool action in the game's authoritative terrain pipeline. Since 0.2.0-dev.30.5.</summary>
public enum TerrainToolMode
{
    /// <summary>Remove only the tool's allowed terrain cells, with normal mining yields and building undo.</summary>
    Remove = 1,
    /// <summary>Place the selected pool material, paying from the game's normal building sources.</summary>
    Add = 2,
}

/// <summary>A declarative, per-tool terrain action. Since 0.2.0-dev.30.5.</summary>
/// <remarks>The game binds primary/secondary building input and held repeat at its own cadence. Native immutable
/// snapshots apply on simulation workers; no mod delegate runs in an engine hook. Missing mappings refuse the whole
/// action. Normal game permissions, payment and undo remain authoritative.</remarks>
public sealed record TerrainToolAction
{
    /// <summary>The operation to perform.</summary>
    public required TerrainToolMode Mode { get; init; }
    /// <summary>Terrain cell identities allowed for removal, per tool. Cell zero and building cells are refused.
    /// Empty means nothing may be removed. The compiler copies this list; changing it does not change a live policy.</summary>
    public IReadOnlyList<byte> RemoveMaterials { get; init; } = Array.Empty<byte>();
    /// <summary>Must be true. Tool actions never remove building voxels or entities.</summary>
    public bool TerrainOnly { get; init; } = true;
}
