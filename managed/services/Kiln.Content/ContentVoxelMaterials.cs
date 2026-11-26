// SPDX-License-Identifier: MIT
// The game's voxel materials by cell id, independent of the items (since 0.2.0-dev.31.13, mod API 0.2.22):
// Content.TryGetVoxelMaterials / LoadVoxelMaterialsAsync, VoxelMaterialCatalog and VoxelMaterial. For XHL's Vein Mining
// ("which material is this, what does it give"), Blueprint (material item identities and exchange rates) and Easy HUD
// (the hovered material's name): a cell's material byte (ClientCursor.hoveredVoxelMaterialId, Queries.RaycastVoxels,
// Build.Read) keys it.
//
// Where it comes from (types from kiln.types, bytes through kiln.assets: what the game gets, so kiln.worldtables' merged
// rows are in; layout checked on client 26430625 and server 26430253, docs/re/building.md 1.6):
//  - keen::TerraformingEfficiencyRegistryResource (both sides): `terrainConfigs` (TerrainTerraformingConfig 184 B:
//    terrainMaterial u8 @0, terrainItem @4 (the loot), hardness @0x14, terrainPerLootItemExchangeRate u32 @0x18,
//    healthPoints @0x1c) and `buildingConfigs` (BuildingTerraformingConfig 180 B: buildingMaterial u8 @0 (cell 128 + it),
//    materialItem @4, hardness, blocksPerLootItemExchangeRate @0x18, healthPoints). 116 terrain and 85 building rows.
//  - keen::VoxelWorldResource.materialGuids[256] @0x1c: the GUID a save keeps per cell id (the first world that fills it).
//  - every keen::ItemInfo's equipment.voxelData (ChangeVoxelData {isBuildingVoxel @0, placeVoxelMaterial @4,
//    placeVoxelMaterialId u8 @0x14}): the items that place a material. A building item places 128..255, a terrain item 1..127;
//    a mapping that breaks that, or names another GUID than materialGuids, is left out (unknown, never guessed).
//  - names: Kiln's stable game-material name (native/services/worldtables material_name, the one WorldTables.GameMaterialId
//    and a world-blocks file accept: "Build_TerrainMaterial_T1_Stone" -> "TerrainStone"), else "Cell<id>". The game has no
//    text of its own for a voxel material (its SolidVoxelMaterial resources do not ship): the label is an item's name, and
//    says which item it is (VoxelMaterialLabelSource).
using System.Buffers.Binary;
using System.Collections;

namespace Kiln;

/// <summary>Which cell domain a <see cref="VoxelMaterial"/> is in. Since 0.2.0-dev.31.13, mod API 0.2.22.</summary>
public enum VoxelMaterialKind
{
    /// <summary>Terrain: cell ids 1..127 (dirt, stone, ores; the game uses 1..116).</summary>
    Terrain = 0,

    /// <summary>A building material: cell ids 128..255 (blocks a player builds with).</summary>
    Building = 1,
}

/// <summary>Which item a <see cref="VoxelMaterial.Label"/> is the name of. The game has no text of its own for a voxel
/// material, so a label is always an item's name. Since 0.2.0-dev.31.13, mod API 0.2.22.</summary>
public enum VoxelMaterialLabelSource
{
    /// <summary>No label (no item, or no text in the catalog's language).</summary>
    None = 0,

    /// <summary>The name of the item that places the material (<see cref="VoxelMaterial.PlacingItems"/>[0]).</summary>
    PlacingItem = 1,

    /// <summary>The name of what digging it gives (<see cref="VoxelMaterial.LootItem"/>): several materials can share it, so
    /// keep the cell id to tell them apart.</summary>
    LootItem = 2,
}

/// <summary>
/// One of the game's voxel materials, keyed by its cell id (the low byte of a voxel cell): what it is, what digging it
/// gives and at what rate, which items place it, and a label in the catalog's language. Since 0.2.0-dev.31.13, mod API 0.2.22.
/// </summary>
/// <param name="CellId">The cell id: 1..127 terrain, 128..255 building.</param>
/// <param name="Kind">Terrain or building.</param>
/// <param name="Guid">The material's GUID as a save keeps it (<c>VoxelWorldResource.materialGuids</c>); <see cref="Guid.Empty"/>
/// when the world table has none for the id.</param>
/// <param name="Name">Kiln's stable name of the game material (<c>"TerrainStone"</c>, <c>"WoodRough"</c>), the one
/// <c>WorldTables.GameMaterialId</c> and world-block files accept, made from the debug name of the item that places it; for
/// a material no item places (ores, rubble), <c>"Cell&lt;id&gt;"</c>. Not a game text: show <see cref="Label"/> to players.</param>
/// <param name="LootItem">What digging it gives (<c>terrainItem</c> / <c>materialItem</c>); <c>default</c> for none (an
/// unbreakable material).</param>
/// <param name="ExchangeRate">How many cells of it one <see cref="LootItem"/> is (<c>terrainPerLootItemExchangeRate</c> /
/// <c>blocksPerLootItemExchangeRate</c>: copper ore 8); null when the game data gives none.</param>
/// <param name="PlacingItems">The items that place it (their <c>equipment.voxelData</c>), the one <see cref="Name"/> comes
/// from first, the rest by id; empty for none.</param>
/// <param name="Hardness">The game's hardness category (<c>"Soft"</c> ... <c>"VeryHard"</c>, <c>"Unbreakable"</c>), or null.</param>
/// <param name="HealthPoints">How many hits of strength 1 a cell takes (<c>healthPoints</c>).</param>
/// <param name="Label">A player-facing name in <see cref="VoxelMaterialCatalog.Language"/>: an item's name (see
/// <paramref name="LabelSource"/>), without markup; null when there is none in that language.</param>
/// <param name="LabelSource">Which item <see cref="Label"/> names.</param>
public sealed record VoxelMaterial(int CellId, VoxelMaterialKind Kind, Guid Guid, string Name, ItemId LootItem, int? ExchangeRate,
    IReadOnlyList<ItemId> PlacingItems, string? Hardness, int HealthPoints, string? Label, VoxelMaterialLabelSource LabelSource)
{
    /// <summary>The cell id, the name and the label.</summary>
    public override string ToString() => $"{CellId} {Name}" + (Label is null ? "" : $" ({Label})");
}

/// <summary>
/// The game's voxel materials with labels in one language (<see cref="Content.TryGetVoxelMaterials"/>): every material the
/// terraforming registry configures, by cell id. Immutable, any thread. An unavailable catalog (<see cref="Available"/>
/// false) is empty and says why. Since 0.2.0-dev.31.13, mod API 0.2.22.
/// </summary>
public sealed class VoxelMaterialCatalog : IReadOnlyList<VoxelMaterial>
{
    private readonly IReadOnlyList<VoxelMaterial> _all;
    private readonly VoxelMaterial?[] _byCell = new VoxelMaterial?[256];
    private readonly Dictionary<uint, VoxelMaterial> _placedBy = new();
    private readonly Dictionary<uint, List<VoxelMaterial>> _byLoot = new();

    internal VoxelMaterialCatalog(string language, IReadOnlyList<VoxelMaterial>? materials, string reason)
    {
        Language = language;
        _all = materials ?? [];
        Available = materials is not null;
        Reason = Available ? "" : (reason.Length > 0 ? reason : "no voxel materials");
        foreach (var m in _all)
        {
            if (m.CellId is > 0 and < 256) _byCell[m.CellId] ??= m;
            foreach (var item in m.PlacingItems) _placedBy.TryAdd(item.Value, m);
            if (m.LootItem.Value != 0)
            {
                if (!_byLoot.TryGetValue(m.LootItem.Value, out var list)) _byLoot[m.LootItem.Value] = list = [];
                list.Add(m);
            }
        }
    }

    /// <summary>The language of the labels (<c>"de-de"</c>).</summary>
    public string Language { get; }

    /// <summary>The game's registry could be read: the catalog has its materials.</summary>
    public bool Available { get; }

    /// <summary>Why there are no materials ("" when <see cref="Available"/>).</summary>
    public string Reason { get; }

    /// <summary>How many materials (terrain and building).</summary>
    public int Count => _all.Count;

    /// <summary>The materials in cell id order.</summary>
    public VoxelMaterial this[int index] => _all[index];

    /// <summary>The material of a cell id (a cell's low byte), or null: 0, an id the game does not configure, out of range.</summary>
    public VoxelMaterial? Get(int cellId) => cellId is > 0 and < 256 ? _byCell[cellId] : null;

    /// <summary>The material an item places (a building block, a terrain block of the build menu), or null.</summary>
    public VoxelMaterial? PlacedBy(ItemId item) => _placedBy.GetValueOrDefault(item.Value);

    /// <summary>The materials whose digging gives <paramref name="item"/>, in cell id order (several share one: five rubble
    /// ids give the same rubble); empty for none.</summary>
    public IReadOnlyList<VoxelMaterial> WithLootItem(ItemId item) => _byLoot.TryGetValue(item.Value, out var l) ? l : [];

    /// <summary>Every material, in cell id order.</summary>
    public IEnumerator<VoxelMaterial> GetEnumerator() => _all.GetEnumerator();

    IEnumerator IEnumerable.GetEnumerator() => GetEnumerator();

    /// <summary>How many materials, in which language; or why there are none.</summary>
    public override string ToString() => Available ? $"{Count} voxel materials ({Language})" : $"no voxel materials: {Reason}";
}

/// <summary>The reflected offsets the voxel material reader uses (<see cref="Measured"/>: client 26430625, server 26430253).</summary>
internal sealed record VoxelMaterialLayout(
    int TerrainConfigs, int TerrainStride, int TerrainId, int TerrainItem, int TerrainHardness, int TerrainRate, int TerrainHealth,
    int BuildingConfigs, int BuildingStride, int BuildingId, int BuildingItem, int BuildingHardness, int BuildingRate, int BuildingHealth,
    int ItemId, int ItemDebugName, int ItemVoxel, int VoxelIsBuilding, int VoxelGuid, int VoxelId, int WorldGuids)
{
    public static readonly VoxelMaterialLayout Measured = new(0, 184, 0, 4, 0x14, 0x18, 0x1c, 8, 180, 0, 4, 0x14, 0x18, 0x1c,
        0, 0x820, 0x440, 0, 4, 0x14, 0x1c);

    public static VoxelMaterialLayout? Reflected()
    {
        if (!GameTypes.Available) return null;
        static int F(string type, string field) => GameTypes.Find(type)?.Field(field)?.Offset ?? -1;
        static int S(string type) => GameTypes.Find(type)?.Size ?? -1;
        const string R = "keen::TerraformingEfficiencyRegistryResource", T = "keen::TerrainTerraformingConfig",
            B = "keen::BuildingTerraformingConfig", I = "keen::ItemInfo", V = "keen::ChangeVoxelData";
        var equipment = F(I, "equipment");
        var voxelData = F("keen::EquipmentSetup", "voxelData");
        var voxel = equipment < 0 || voxelData < 0 ? -1 : equipment + voxelData;
        var l = new VoxelMaterialLayout(
            F(R, "terrainConfigs"), S(T), F(T, "terrainMaterial"), F(T, "terrainItem"), F(T, "hardness"), F(T, "terrainPerLootItemExchangeRate"), F(T, "healthPoints"),
            F(R, "buildingConfigs"), S(B), F(B, "buildingMaterial"), F(B, "materialItem"), F(B, "hardness"), F(B, "blocksPerLootItemExchangeRate"), F(B, "healthPoints"),
            F(I, "itemId"), F(I, "debugName"), voxel, F(V, "isBuildingVoxel"), F(V, "placeVoxelMaterial"), F(V, "placeVoxelMaterialId"),
            F("keen::VoxelWorldResource", "materialGuids"));
        return l.Valid ? l : null;
    }

    // Each row's fields must fit inside its stride; the optional ones (hardness, health) may be -1 (a build without them).
    public bool Valid =>
        TerrainConfigs >= 0 && TerrainStride > 0 && TerrainId >= 0 && TerrainId < TerrainStride && TerrainItem >= 0 && TerrainItem + 16 <= TerrainStride &&
        BuildingConfigs >= 0 && BuildingStride > 0 && BuildingId >= 0 && BuildingId < BuildingStride && BuildingItem >= 0 && BuildingItem + 16 <= BuildingStride &&
        Math.Max(TerrainRate, Math.Max(TerrainHardness, TerrainHealth)) + 4 <= TerrainStride &&
        Math.Max(BuildingRate, Math.Max(BuildingHardness, BuildingHealth)) + 4 <= BuildingStride &&
        ItemId >= 0 && ItemDebugName >= 0 && ItemVoxel >= 0 && VoxelIsBuilding >= 0 && VoxelGuid >= 0 && VoxelId >= 0 && WorldGuids >= 0;
}

/// <summary>An item that places a voxel material (its <c>equipment.voxelData</c>).</summary>
internal readonly record struct VoxelPlacer(ItemId Item, string DebugName, bool IsBuildingVoxel, Guid Material, int CellId, bool IsGame);

/// <summary>What the voxel material catalog reads from: the game's resources, or a test kit's script.</summary>
internal interface IVoxelMaterialSource
{
    /// <summary>Why there are no materials here at all (no kiln.assets, no kiln.types), or null.</summary>
    string? Unavailable { get; }

    /// <summary>The materials without labels (<see cref="VoxelMaterial.Label"/> null), in cell id order, or null with the
    /// reason. Runs on a worker.</summary>
    IReadOnlyList<VoxelMaterial>? Read(out string reason);
}

/// <summary>Readers (pure, host-tested) and the process's one catalog of the game's voxel materials.</summary>
internal static class VoxelMaterialData
{
    private const uint ItemInfoType = 0xb5ce8765;

    // ---- pure readers -------------------------------------------------------------------------------------------------

    /// <summary>The placer of an ItemInfo's bytes, or null: no voxel data (no material GUID), or the bytes are too short.</summary>
    public static VoxelPlacer? ParsePlacer(ReadOnlySpan<byte> item, VoxelMaterialLayout l, bool isGame)
    {
        var v = l.ItemVoxel;
        if (v + Math.Max(Math.Max(l.VoxelGuid + 16, l.VoxelId + 1), l.VoxelIsBuilding + 1) > item.Length || l.ItemId + 4 > item.Length) return null;
        var guid = item.Slice(v + l.VoxelGuid, 16);
        if (!guid.ContainsAnyExcept((byte)0)) return null;
        var debug = GameTextData.BlobString(item, l.ItemDebugName) ?? "";
        return new VoxelPlacer(new ItemId(BinaryPrimitives.ReadUInt32LittleEndian(item[l.ItemId..])), debug, item[v + l.VoxelIsBuilding] != 0,
            new Guid(guid), item[v + l.VoxelId], isGame);
    }

    /// <summary>The 256 GUIDs of a VoxelWorldResource's <c>materialGuids</c>, or null when it has none (most worlds).</summary>
    public static Guid[]? WorldGuids(ReadOnlySpan<byte> world, VoxelMaterialLayout l)
    {
        if (l.WorldGuids + 256 * 16 > world.Length) return null;
        var table = world.Slice(l.WorldGuids, 256 * 16);
        if (!table.ContainsAnyExcept((byte)0)) return null;
        var guids = new Guid[256];
        for (var i = 0; i < 256; i++) guids[i] = new Guid(table.Slice(i * 16, 16));
        return guids;
    }

    /// <summary>Kiln's stable game-material name from the debug name of the item that places it ("Block_T1_Wood_Rough_CRAFTING"
    /// -> "WoodRough", "Build_TerrainMaterial_T1_Stone" -> "TerrainStone"), or "" for another kind of item: the rule of
    /// native/services/worldtables/merge.cpp material_name and crates/kiln-manage's catalog.</summary>
    public static string MaterialName(string debugName)
    {
        var s = debugName.TrimStart('_');
        string prefix;
        if (s.StartsWith("Build_TerrainMaterial_", StringComparison.Ordinal))
        {
            prefix = "Terrain";
            s = s["Build_TerrainMaterial_".Length..];
        }
        else if (s.StartsWith("Block_", StringComparison.Ordinal))
        {
            prefix = "";
            s = s["Block_".Length..];
        }
        else return "";
        var sb = new System.Text.StringBuilder(prefix);
        foreach (var tok in s.Split('_'))
        {
            if (tok.Length == 0) continue;
            var tier = tok.Length >= 2 && tok[0] == 'T' && tok.AsSpan(1).IndexOfAnyExceptInRange('0', '9') < 0;
            var up = tok.ToUpperInvariant();
            if (tier || up == "CRAFTING" || up == "REWARD") continue;
            sb.Append(char.ToUpperInvariant(tok[0])).Append(tok, 1, tok.Length - 1);
        }
        return sb.ToString();
    }

    /// <summary>The materials of a TerraformingEfficiencyRegistryResource's bytes, joined with the world's GUID table and the
    /// items that place them; labels null. Null when the registry does not read as its layout says.</summary>
    public static List<VoxelMaterial>? Build(ReadOnlySpan<byte> registry, VoxelMaterialLayout l, Guid[]? guids, IEnumerable<VoxelPlacer> placers,
        Func<long, string?> hardnessName)
    {
        if (!GameTextData.BlobArray(registry, l.TerrainConfigs, l.TerrainStride, out var ts, out var tn) ||
            !GameTextData.BlobArray(registry, l.BuildingConfigs, l.BuildingStride, out var bs, out var bn)) return null;
        var rows = new SortedDictionary<int, (ItemId Loot, int? Rate, string? Hardness, int Health)>();
        void Row(ReadOnlySpan<byte> r, int cell, int item, int hardness, int rate, int health)
        {
            if (rows.ContainsKey(cell)) return;  // the first row of an id is the one the game finds
            var raw = BinaryPrimitives.ReadUInt32LittleEndian(r[rate..]);
            rows[cell] = (new ItemId(GameTextData.Fnv16(r.Slice(item, 16))), raw is > 0 and <= int.MaxValue ? (int)raw : null,
                hardness >= 0 ? hardnessName(r[hardness]) : null, health >= 0 ? BinaryPrimitives.ReadInt32LittleEndian(r[health..]) : 0);
        }
        for (var i = 0; i < tn; i++)
        {
            var r = registry.Slice(ts + i * l.TerrainStride, l.TerrainStride);
            var id = r[l.TerrainId];
            if (id is > 0 and < 128) Row(r, id, l.TerrainItem, l.TerrainHardness, l.TerrainRate, l.TerrainHealth);
        }
        for (var i = 0; i < bn; i++)
        {
            var r = registry.Slice(bs + i * l.BuildingStride, l.BuildingStride);
            var id = r[l.BuildingId];
            if (id < 128) Row(r, 128 + id, l.BuildingItem, l.BuildingHardness, l.BuildingRate, l.BuildingHealth);
        }
        // the items that place each configured cell, in its own domain and with the world's GUID for it
        var byCell = new Dictionary<int, List<VoxelPlacer>>();
        var nameCells = new Dictionary<string, int>(StringComparer.Ordinal);  // -1: two cells, no name
        foreach (var p in placers)
        {
            if (!rows.ContainsKey(p.CellId) || p.IsBuildingVoxel != p.CellId >= 128) continue;
            if (guids is not null && guids[p.CellId] != Guid.Empty && guids[p.CellId] != p.Material) continue;
            if (!byCell.TryGetValue(p.CellId, out var list)) byCell[p.CellId] = list = [];
            if (list.Any(x => x.Item == p.Item)) continue;
            list.Add(p);
            if (p.IsGame && MaterialName(p.DebugName) is { Length: > 0 } n)
                nameCells[n] = nameCells.TryGetValue(n, out var c) && c != p.CellId ? -1 : p.CellId;
        }
        var result = new List<VoxelMaterial>(rows.Count);
        foreach (var (cell, row) in rows)
        {
            var list = byCell.GetValueOrDefault(cell) ?? [];
            string? name = null;
            VoxelPlacer? from = null;
            foreach (var p in list.Where(p => p.IsGame).OrderBy(p => MaterialName(p.DebugName), StringComparer.Ordinal).ThenBy(p => p.Item.Value))
                if (MaterialName(p.DebugName) is { Length: > 0 } n && nameCells.GetValueOrDefault(n) == cell)
                {
                    name = n;
                    from = p;
                    break;
                }
            from ??= list.Where(p => p.IsGame).OrderBy(p => p.Item.Value).Cast<VoxelPlacer?>().FirstOrDefault()
                     ?? list.OrderBy(p => p.Item.Value).Cast<VoxelPlacer?>().FirstOrDefault();
            var items = list.Select(p => p.Item).OrderBy(i => from is { } f && i == f.Item ? 0 : 1).ThenBy(i => i.Value).ToArray();
            result.Add(new VoxelMaterial(cell, cell >= 128 ? VoxelMaterialKind.Building : VoxelMaterialKind.Terrain,
                guids?[cell] ?? Guid.Empty, name ?? $"Cell{cell}", row.Loot, row.Rate, items, row.Hardness, row.Health, null,
                VoxelMaterialLabelSource.None));
        }
        return result;
    }

    // ---- the process's catalog ----------------------------------------------------------------------------------------

    private static readonly object Gate = new();
    private static IVoxelMaterialSource? _override;
    private static IReadOnlyList<VoxelMaterial>? _materials;
    private static Task? _loading;
    private static string _reason = "";
    private static long _failedAt;
    private static int _failures;
    private static long _generation;

    /// <summary>Test seam (the test kit, host tests): read from <paramref name="source"/>; null: the game's resources again.</summary>
    internal static void UseSource(IVoxelMaterialSource? source)
    {
        lock (Gate)
        {
            _override = source;
            _materials = null;
            _loading = null;
            _reason = "";
            _failedAt = 0;
            _failures = 0;
            _generation++;
        }
    }

    /// <summary>Test seam: the source's data changed (a test kit's script): the next question reads it again.</summary>
    internal static void Invalidate()
    {
        lock (Gate)
        {
            _materials = null;
            _loading = null;
            _failedAt = 0;
            _failures = 0;
            _generation++;
        }
    }

    private static IVoxelMaterialSource Source { get { lock (Gate) return _override ?? ResourceVoxelMaterialSource.Instance; } }

    public static string? Unavailable => Source.Unavailable;

    /// <summary>The materials (labels null) when read, (null, reason) when the read failed and waits to be tried again (5 s,
    /// doubling to 5 minutes), or null while read (this starts the read).</summary>
    public static (IReadOnlyList<VoxelMaterial>? Materials, string Reason)? TryGet()
    {
        var source = Source;
        if (source.Unavailable is { } why) return (null, why);
        lock (Gate)
        {
            if (_materials is not null) return (_materials, "");
            if (_loading is not null) return null;
            if (_failedAt != 0 && Environment.TickCount64 - _failedAt < RetryDelay.Of(_failures, 5_000, 300_000)) return (null, _reason);
            Start(source);
            return null;
        }
    }

    public static async Task<(IReadOnlyList<VoxelMaterial>? Materials, string Reason)> LoadAsync(CancellationToken ct)
    {
        var source = Source;
        if (source.Unavailable is { } why) return (null, why);
        Task loading;
        lock (Gate)
        {
            if (_materials is not null) return (_materials, "");
            loading = _loading ?? Start(source);
        }
        await loading.WaitAsync(ct).ConfigureAwait(false);
        lock (Gate) return _materials is not null ? (_materials, "") : (null, _reason.Length > 0 ? _reason : "the voxel materials were not read");
    }

    // Under Gate.
    private static Task Start(IVoxelMaterialSource source)
    {
        var generation = _generation;
        return _loading = Task.Run(() =>
        {
            IReadOnlyList<VoxelMaterial>? m = null;
            string reason;
            try
            {
                m = source.Read(out reason);
            }
            catch (Exception x) when (x is not OutOfMemoryException)
            {
                reason = $"the game's voxel materials could not be read: {x.Message}";
            }
            lock (Gate)
            {
                if (generation != _generation) return;
                _loading = null;
                if (m is not null)
                {
                    _materials = m;
                    _failedAt = 0;
                    _failures = 0;
                }
                else
                {
                    _reason = reason.Length > 0 ? reason : "the game's voxel materials could not be read";
                    _failedAt = Environment.TickCount64;
                    _failures++;
                }
            }
        });
    }

    internal static void ResetForTests() => UseSource(null);

    /// <summary>The game's resources as the source.</summary>
    private sealed class ResourceVoxelMaterialSource : IVoxelMaterialSource
    {
        public static readonly ResourceVoxelMaterialSource Instance = new();

        public string? Unavailable =>
            !ResourceStore.Available ? "kiln.assets' resource reads are off here (row assets.resources)"
            : !GameTypes.Available ? "kiln.types has no game types here"
            : null;

        public IReadOnlyList<VoxelMaterial>? Read(out string reason)
        {
            if (VoxelMaterialLayout.Reflected() is not { } l)
            {
                reason = "this game build's terraforming, item or world types differ from what Kiln reads (keen::TerraformingEfficiencyRegistryResource, keen::ItemInfo.equipment.voxelData, keen::VoxelWorldResource.materialGuids)";
                return null;
            }
            byte[]? registry = null;
            if (GameTypes.Find("keen::TerraformingEfficiencyRegistryResource") is { } rt)
                foreach (var id in ResourceStore.List(rt.Hash))
                    if (id.Part == 0 && (registry = ResourceStore.Read(id)) is not null) break;
            if (registry is null)
            {
                reason = "the game's terraforming registry (keen::TerraformingEfficiencyRegistryResource) is not readable yet";
                return null;
            }
            Guid[]? guids = null;
            if (GameTypes.Find("keen::VoxelWorldResource") is { } wt)
                foreach (var id in ResourceStore.List(wt.Hash))
                    if (id.Part == 0 && ResourceStore.Read(id) is { } w && (guids = WorldGuids(w, l)) is not null) break;
            var placers = new List<VoxelPlacer>();
            var buffer = new byte[4096];
            foreach (var id in ResourceStore.List(ItemInfoType))
            {
                if (id.Part != 0) continue;
                try
                {
                    if (ResourceStore.TryRead(id, ref buffer, out var n) && ParsePlacer(buffer.AsSpan(0, n), l, !ResourceStore.IsNew(id)) is { } p)
                        placers.Add(p);
                }
                catch (InvalidOperationException)
                {
                    // one item that cannot be read (it changed while read) places nothing; the others still count
                }
            }
            var hardness = (GameTypes.Find("keen::HardnessCategory")?.EnumValues ?? []).GroupBy(v => v.Value).ToDictionary(g => g.Key, g => g.First().Name);
            var built = Build(registry, l, guids, placers, v => hardness.GetValueOrDefault(v));
            reason = built is null ? "the game's terraforming registry does not read as its reflected layout says" : "";
            return built;
        }
    }
}

public static partial class Content
{
    /// <summary>The capability row of <see cref="TryGetVoxelMaterials"/> and <see cref="LoadVoxelMaterialsAsync"/>: available
    /// on both sides where kiln.assets reads resources (<c>assets.resources</c>) and kiln.types is here. The labels also need
    /// the game's texts (<see cref="GameTextsCapability"/>), except English ones, which Kiln ships. Since 0.2.0-dev.31.13,
    /// mod API 0.2.22.</summary>
    public const string VoxelMaterialsCapability = "content.voxel_materials";

    /// <summary>
    /// The game's voxel materials with labels in <paramref name="language"/> (a key the game ships, <c>"de-de"</c>; null the
    /// player's game language, <c>"en-us"</c> on a server), when they are read already, without waiting. Null while they (or
    /// that language's texts) are read: the first call starts that in the background; call again later, or <c>await</c>
    /// <see cref="LoadVoxelMaterialsAsync"/>. Unavailable at once, with a <see cref="VoxelMaterialCatalog.Reason"/>, where the
    /// game data cannot be read (see <see cref="VoxelMaterialsCapability"/>). The materials are read once per process; the
    /// labels follow the language asked. Since 0.2.0-dev.31.13, mod API 0.2.22.
    /// </summary>
    [AnyThread]
    public static VoxelMaterialCatalog? TryGetVoxelMaterials(string? language = null)
    {
        LookupCaps.Rows.Ensure();
        var lang = GameTextStore.Resolve(language);
        if (VoxelMaterialData.TryGet() is not { } read) return null;
        if (read.Materials is null) return new VoxelMaterialCatalog(lang, null, read.Reason);
        // wait for the language's texts where there are any, so the labels do not come and go
        object? texts = null;
        if (GameTextStore.Unavailable is null)
        {
            if (GameTextStore.TryGet(lang) is not { } t) return null;
            texts = t.Texts;
        }
        if (!GameItemTexts.Ready()) return null;
        return Labelled(lang, read.Materials, texts);
    }

    /// <summary>
    /// Reads the game's voxel materials and <paramref name="language"/>'s texts (see <see cref="TryGetVoxelMaterials"/>) and
    /// completes with the catalog, on a worker; never faults for missing data (an unavailable catalog says why).
    /// Since 0.2.0-dev.31.13, mod API 0.2.22.
    /// </summary>
    /// <exception cref="OperationCanceledException"><paramref name="cancellationToken"/> was cancelled.</exception>
    [AnyThread]
    public static async Task<VoxelMaterialCatalog> LoadVoxelMaterialsAsync(string? language = null, CancellationToken cancellationToken = default)
    {
        LookupCaps.Rows.Ensure();
        var lang = GameTextStore.Resolve(language);
        var (materials, reason) = await VoxelMaterialData.LoadAsync(cancellationToken).ConfigureAwait(false);
        if (materials is null) return new VoxelMaterialCatalog(lang, null, reason);
        object? texts = null;
        if (GameTextStore.Unavailable is null) texts = (await GameTextStore.LoadAsync(lang, cancellationToken).ConfigureAwait(false)).Texts;
        if (!GameItemTexts.Ready()) await Task.Run(() => GameItemTexts.Ready(wait: true), cancellationToken).ConfigureAwait(false);
        return Labelled(lang, materials, texts);
    }

    // The last catalogs made, per language (a HUD asks every frame): the game's tables do not change once read, so one is
    // made again only for other materials or another text table (a test kit's new script) or once a minute (a new item's
    // definition text).
    private static readonly object LabelGate = new();
    private static readonly Dictionary<string, (IReadOnlyList<VoxelMaterial> From, object? Texts, long At, VoxelMaterialCatalog Catalog)> LabelCache =
        new(StringComparer.Ordinal);

    private static VoxelMaterialCatalog Labelled(string language, IReadOnlyList<VoxelMaterial> materials, object? texts)
    {
        lock (LabelGate)
            if (LabelCache.TryGetValue(language, out var c) && ReferenceEquals(c.From, materials) && ReferenceEquals(c.Texts, texts) &&
                Environment.TickCount64 - c.At < 60_000)
                return c.Catalog;
        var made = MakeLabelled(language, materials);
        lock (LabelGate)
        {
            if (LabelCache.Count >= 8) LabelCache.Clear();
            LabelCache[language] = (materials, texts, Environment.TickCount64, made);
        }
        return made;
    }

    private static VoxelMaterialCatalog MakeLabelled(string language, IReadOnlyList<VoxelMaterial> materials)
    {
        var list = new List<VoxelMaterial>(materials.Count);
        foreach (var m in materials)
        {
            string? label = null;
            var source = VoxelMaterialLabelSource.None;
            if (m.PlacingItems.Count > 0 && global::Kiln.Items.DisplayNameIn(m.PlacingItems[0], language) is { Length: > 0 } placed)
                (label, source) = (placed, VoxelMaterialLabelSource.PlacingItem);
            else if (m.LootItem.Value != 0 && global::Kiln.Items.DisplayNameIn(m.LootItem, language) is { Length: > 0 } loot)
                (label, source) = (loot, VoxelMaterialLabelSource.LootItem);
            list.Add(m with { Label = label, LabelSource = source });
        }
        return new VoxelMaterialCatalog(language, list, "");
    }
}
