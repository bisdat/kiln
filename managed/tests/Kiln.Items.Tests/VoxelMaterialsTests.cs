// SPDX-License-Identifier: MIT
// The game's voxel materials by cell id (0.2.0-dev.31.13, mod API 0.2.22): Kiln.Content's VoxelMaterialData readers on
// registry, world and item bytes laid out as client 26430625 / server 26430253 lay them out (VoxelMaterialLayout.Measured),
// and Content.TryGetVoxelMaterials / LoadVoxelMaterialsAsync with their labels. Invented GUIDs and ids; the real game's rows
// are checked by RealGameData below when KILN_XHL_RES points at bytes extracted with tools/kfc.
using System.Buffers.Binary;
using Xunit;

namespace Kiln;

internal sealed class ScriptedMaterials(Func<IReadOnlyList<VoxelMaterial>?> read, string? why = null) : IVoxelMaterialSource
{
    public int Reads;
    public string? Unavailable => why;

    public IReadOnlyList<VoxelMaterial>? Read(out string reason)
    {
        Interlocked.Increment(ref Reads);
        var m = read();
        reason = m is null ? "the registry is not readable yet (test)" : "";
        return m;
    }
}

[Collection("game names")]
public sealed class VoxelMaterialsTests : IDisposable
{
    private static readonly VoxelMaterialLayout L = VoxelMaterialLayout.Measured;

    private static readonly Guid Dirt = Guid.Parse("20292e7b-858b-4e5f-bdc8-e921190eb0f4"), Copper = Guid.Parse("00000026-7e57-4a11-9c00-000000000026"),
        Logs = Guid.Parse("00000194-7e57-4a11-9c00-000000000194");
    private static readonly Guid G1 = Guid.Parse("00000001-0000-4000-8000-0000000000aa"), G26 = Guid.Parse("00000026-0000-4000-8000-0000000000aa"),
        G37 = Guid.Parse("00000037-0000-4000-8000-0000000000aa"), G194 = Guid.Parse("00000194-0000-4000-8000-0000000000aa");
    private static readonly ItemId DirtBlock = new(0x1001), WoodBlock = new(0x2002), WoodBlock2 = new(0x2001), ModBlock = new(0x3003),
        WrongDomain = new(0x4004), Conflicting = new(0x5005), Nowhere = new(0x6006);

    public VoxelMaterialsTests()
    {
        VoxelMaterialData.ResetForTests();
        GameTextStore.ResetForTests();
        GameItemTexts.ResetForTests();
        GameNames.ResetForTests();
        DefinedNames.ResetForTests();
        GameLanguageSource.Override = () => "de-de";
    }

    public void Dispose()
    {
        VoxelMaterialData.ResetForTests();
        GameTextStore.ResetForTests();
        GameItemTexts.ResetForTests();
        GameNames.ResetForTests();
        DefinedNames.ResetForTests();
        GameLanguageSource.ResetForTests();
    }

    private static ItemId Id(Guid g) => new(GameTextData.Fnv16(g.ToByteArray()));

    /// <summary>A TerraformingEfficiencyRegistryResource: terrain rows (id, loot, hardness, rate, health) and building rows.</summary>
    private static byte[] Registry((byte Id, Guid Loot, byte Hardness, uint Rate, int Health)[] terrain, (byte Id, Guid Loot, byte Hardness, uint Rate, int Health)[] building)
    {
        var b = new TestBlob(16);
        var t = b.Array(L.TerrainConfigs, terrain.Length, L.TerrainStride);
        for (var i = 0; i < terrain.Length; i++) Row(b, t + i * L.TerrainStride, terrain[i]);
        var u = b.Array(L.BuildingConfigs, building.Length, L.BuildingStride);
        for (var i = 0; i < building.Length; i++) Row(b, u + i * L.BuildingStride, building[i]);
        return b.Done;

        static void Row(TestBlob b, int at, (byte Id, Guid Loot, byte Hardness, uint Rate, int Health) r)
        {
            b.U8(at, r.Id);
            if (r.Loot != Guid.Empty) b.Bytes(at + 4, r.Loot.ToByteArray());
            b.U8(at + 0x14, r.Hardness);
            b.U32(at + 0x18, r.Rate);
            b.I32(at + 0x1c, r.Health);
        }
    }

    private static readonly string[] Hardness = ["Soft", "SlightlyHard", "ModeratelyHard", "Hard", "VeryHard", "Unbreakable"];
    private static string? HardnessName(long v) => v >= 0 && v < Hardness.Length ? Hardness[v] : null;

    private static Guid[] WorldTable()
    {
        var g = new Guid[256];
        g[1] = G1; g[26] = G26; g[37] = G37; g[194] = G194;
        return g;
    }

    private static List<VoxelPlacer> Placers() =>
    [
        new(DirtBlock, "Build_TerrainMaterial_T1_Dirt_Grassland", false, G1, 1, true),
        new(WrongDomain, "Block_T1_Wrong", true, G1, 1, true),                 // a building item cannot place terrain: left out
        new(Conflicting, "Build_TerrainMaterial_T2_Copper", false, Guid.NewGuid(), 26, true),  // another GUID than the world's: left out
        new(WoodBlock, "Block_T1_Wood_Rough_CRAFTING", true, G194, 194, true),
        new(WoodBlock2, "Block_T2_Wood_Rough", true, G194, 194, true),
        new(ModBlock, "Block_Mod_Thing", false, G37, 37, false),               // a mod's new item: listed, never the name
        new(Nowhere, "Build_TerrainMaterial_T1_Nowhere", false, Guid.NewGuid(), 99, true),     // no row for 99: left out
    ];

    private static List<VoxelMaterial> Built() => VoxelMaterialData.Build(Registry(
        [(1, Dirt, 0, 5, 3), (37, Dirt, 4, 40, 25), (26, Copper, 4, 8, 8), (5, Guid.Empty, 5, 0, 0), (1, Copper, 1, 99, 1), (200, Dirt, 0, 1, 1)],
        [(66, Logs, 1, 5, 4)]), L, WorldTable(), Placers(), HardnessName)!;

    [Fact]
    public void The_measured_layout_is_the_games_reflection()
    {
        // tools/kfc types (client 26430625): TerrainTerraformingConfig 184 B {terrainMaterial @0, terrainItem @4, hardness @0x14,
        // terrainPerLootItemExchangeRate @0x18, healthPoints @0x1c}, BuildingTerraformingConfig 180 B (the same first fields),
        // ItemInfo {itemId @0, equipment @0x104 (voxelData @0x33c: isBuildingVoxel @0, placeVoxelMaterial @4,
        // placeVoxelMaterialId @0x14), debugName @0x820}, VoxelWorldResource.materialGuids @0x1c
        Assert.Equal(new VoxelMaterialLayout(0, 184, 0, 4, 0x14, 0x18, 0x1c, 8, 180, 0, 4, 0x14, 0x18, 0x1c, 0, 0x820, 0x104 + 0x33c, 0, 4, 0x14, 0x1c), L);
        Assert.True(L.Valid);
        Assert.False((L with { TerrainRate = 184 }).Valid);
    }

    [Fact]
    public void A_material_name_is_the_worldtables_rule()
    {
        // the cases of native/services/worldtables/tests/test_merge.cpp and the catalog's
        Assert.Equal("WoodRough", VoxelMaterialData.MaterialName("Block_T1_Wood_Rough_CRAFTING"));
        Assert.Equal("RoofPlantFibre", VoxelMaterialData.MaterialName("Block_Roof_T1_PlantFibre"));
        Assert.Equal("TerrainStone", VoxelMaterialData.MaterialName("Build_TerrainMaterial_T1_Stone"));
        Assert.Equal("TerrainDirtGrassland", VoxelMaterialData.MaterialName("_Build_TerrainMaterial_T1_Dirt_Grassland"));
        Assert.Equal("StoneRough", VoxelMaterialData.MaterialName("Block_T2_Stone_Rough_REWARD"));
        Assert.Equal("", VoxelMaterialData.MaterialName("Material_T2_Ore_Copper"));
    }

    [Fact]
    public void Materials_are_keyed_by_cell_independent_of_their_items()
    {
        var m = Built();
        Assert.Equal([1, 5, 26, 37, 194], m.Select(x => x.CellId));
        var dirt = m[0];
        Assert.Equal((VoxelMaterialKind.Terrain, G1, "TerrainDirtGrassland", Id(Dirt), (int?)5, "Soft", 3),
            (dirt.Kind, dirt.Guid, dirt.Name, dirt.LootItem, dirt.ExchangeRate, dirt.Hardness, dirt.HealthPoints));
        Assert.Equal([DirtBlock], dirt.PlacingItems);                        // the building item with a terrain id is not its placer
        var fog = m[1];                                                        // unbreakable: no loot, no rate, no world GUID
        Assert.Equal(("Cell5", default(ItemId), (int?)null, Guid.Empty, "Unbreakable"), (fog.Name, fog.LootItem, fog.ExchangeRate, fog.Guid, fog.Hardness));
        var copper = m[2];
        Assert.Equal(("Cell26", Id(Copper), (int?)8, "VeryHard"), (copper.Name, copper.LootItem, copper.ExchangeRate, copper.Hardness));
        Assert.Empty(copper.PlacingItems);                                    // the conflicting mapping stays unknown
        var hard = m[3];                                                       // terrain 37 gives dirt too: still its own material
        Assert.Equal(("Cell37", Id(Dirt), (int?)40, 25), (hard.Name, hard.LootItem, hard.ExchangeRate, hard.HealthPoints));
        Assert.Equal([ModBlock], hard.PlacingItems);
        var wood = m[4];
        Assert.Equal((VoxelMaterialKind.Building, "WoodRough", Id(Logs), (int?)5), (wood.Kind, wood.Name, wood.LootItem, wood.ExchangeRate));
        Assert.Equal([WoodBlock2, WoodBlock], wood.PlacingItems);            // the same name from both: the lower id gives it
        Assert.All(m, x => Assert.Null(x.Label));
    }

    [Fact]
    public void A_name_two_cells_claim_is_nobodys()
    {
        var placers = Placers();
        placers.Add(new(new ItemId(0x7007), "Block_T3_Wood_Rough_REWARD", true, Guid.Empty, 198, true));
        var m = VoxelMaterialData.Build(Registry([(1, Dirt, 0, 5, 3)], [(66, Logs, 1, 5, 4), (70, Logs, 1, 5, 4)]), L, WorldTable(), placers, HardnessName)!;
        Assert.Equal(["TerrainDirtGrassland", "Cell194", "Cell198"], m.Select(x => x.Name));
        Assert.Null(VoxelMaterialData.Build(new byte[8], L, null, [], HardnessName));   // a registry too short for its arrays
    }

    [Fact]
    public void An_item_info_and_a_world_table_read_as_their_layouts_say()
    {
        var item = new TestBlob(0x828);
        item.U32(L.ItemId, 0x1001);
        item.String(L.ItemDebugName, "Build_TerrainMaterial_T1_Dirt_Grassland");
        item.Bytes(L.ItemVoxel + L.VoxelGuid, G1.ToByteArray());
        item.U8(L.ItemVoxel + L.VoxelId, 1);
        var p = VoxelMaterialData.ParsePlacer(item.Done, L, isGame: true)!.Value;
        Assert.Equal(new VoxelPlacer(DirtBlock, "Build_TerrainMaterial_T1_Dirt_Grassland", false, G1, 1, true), p);
        Assert.Null(VoxelMaterialData.ParsePlacer(new byte[0x828], L, true));   // no voxel data: places nothing
        Assert.Null(VoxelMaterialData.ParsePlacer(new byte[0x100], L, true));
        var world = new byte[L.WorldGuids + 4096];
        G26.ToByteArray().CopyTo(world, L.WorldGuids + 26 * 16);
        var guids = VoxelMaterialData.WorldGuids(world, L)!;
        Assert.Equal(G26, guids[26]);
        Assert.Equal(Guid.Empty, guids[1]);
        Assert.Null(VoxelMaterialData.WorldGuids(new byte[L.WorldGuids + 4096], L));    // most worlds have no table
    }

    private void UseGame(Func<IReadOnlyList<VoxelMaterial>?> read, string? why = null)
    {
        VoxelMaterialData.UseSource(new ScriptedMaterials(read, why));
        var texts = new ScriptedTexts();
        texts.Tables["de-de"] = new() { [100] = "Erde", [101] = "Kupfererz", [102] = "<b>Erdklumpen</b>" };
        texts.Tables["en-us"] = new() { [100] = "Dirt" };
        GameTextStore.UseSource(texts);
        GameNames.UseForTests(GameNameLayout.Measured, new() { [DirtBlock.Value] = Guid.NewGuid(), [Id(Copper).Value] = Guid.NewGuid(),
            [Id(Dirt).Value] = Guid.NewGuid(), [WoodBlock.Value] = Guid.NewGuid(), [WoodBlock2.Value] = Guid.NewGuid() }, new(), new(), "de-de", texts: null);
        GameItemTexts.Override = i => i == DirtBlock ? new(100, 0, 0, 0) : i == Id(Copper) ? new(101, 0, 0, 0) : i == Id(Dirt) ? new(102, 0, 0, 0) : null;
    }

    [Fact]
    public async Task The_catalog_labels_each_material_with_an_items_name_and_says_which()
    {
        UseGame(Built);
        Assert.Null(Content.TryGetVoxelMaterials("de-de"));                  // not read yet: the reads start
        var c = await Content.LoadVoxelMaterialsAsync("de-de");
        Assert.True(c.Available);
        Assert.Equal("de-de", c.Language);
        Assert.Equal(5, c.Count);
        Assert.Equal(("Erde", VoxelMaterialLabelSource.PlacingItem), (c.Get(1)!.Label, c.Get(1)!.LabelSource));
        Assert.Equal(("Kupfererz", VoxelMaterialLabelSource.LootItem), (c.Get(26)!.Label, c.Get(26)!.LabelSource));
        // 37's placer is a mod item without a name here: the loot item's name, without markup
        Assert.Equal(("Erdklumpen", VoxelMaterialLabelSource.LootItem), (c.Get(37)!.Label, c.Get(37)!.LabelSource));
        Assert.Equal((null, VoxelMaterialLabelSource.None), (c.Get(5)!.Label, c.Get(5)!.LabelSource));
        Assert.Equal((null, VoxelMaterialLabelSource.None), (c.Get(194)!.Label, c.Get(194)!.LabelSource));   // no German name for it
        Assert.Null(c.Get(0));
        Assert.Null(c.Get(2));
        Assert.Null(c.Get(300));
        Assert.Same(c.Get(1), c.PlacedBy(DirtBlock));
        Assert.Same(c.Get(194), c.PlacedBy(WoodBlock));
        Assert.Null(c.PlacedBy(Id(Copper)));
        Assert.Equal([1, 37], c.WithLootItem(Id(Dirt)).Select(x => x.CellId));
        Assert.Empty(c.WithLootItem(new ItemId(1)));
        Assert.Same(c, Content.TryGetVoxelMaterials("de"));                   // read once; the same catalog while nothing changed
        var en = await Content.LoadVoxelMaterialsAsync("en-us");
        Assert.Equal("Dirt", en.Get(1)!.Label);
        Assert.Null(en.Get(26)!.Label);                                       // no English text for its tag, no shipped name for the id
    }

    [Fact]
    public async Task A_server_has_the_materials_with_kilns_shipped_english_labels()
    {
        var logs = new ItemId(722268629);                                     // "Wood Logs" in Kiln's shipped items.tsv
        UseGame(() => [new VoxelMaterial(194, VoxelMaterialKind.Building, G194, "WoodRough", logs, 5, [], "SlightlyHard", 4, null, VoxelMaterialLabelSource.None)]);
        GameTextStore.UseSource(new ScriptedTexts { Why = GameTextStore.ServerReason });
        var en = await Content.LoadVoxelMaterialsAsync("en-us");
        Assert.Equal(("Wood Logs", VoxelMaterialLabelSource.LootItem), (en.Get(194)!.Label, en.Get(194)!.LabelSource));
        var de = Content.TryGetVoxelMaterials("de-de")!;                      // no waiting for texts a server does not have
        Assert.True(de.Available);
        Assert.Null(de.Get(194)!.Label);
    }

    [Fact]
    public async Task An_unreadable_registry_says_why_and_is_tried_again_later()
    {
        var reads = 0;
        UseGame(() => ++reads > 1 ? Built() : null);
        var first = await Content.LoadVoxelMaterialsAsync();
        Assert.False(first.Available);
        Assert.Empty(first);
        Assert.Equal("the registry is not readable yet (test)", first.Reason);
        Assert.False(Content.TryGetVoxelMaterials()!.Available);             // within the back-off: the same answer, no read
        Assert.Equal(1, reads);
        Assert.True((await Content.LoadVoxelMaterialsAsync()).Available);     // an explicit load asks again
        // no resources at all: at once, with the reason
        VoxelMaterialData.UseSource(new ScriptedMaterials(() => null, "kiln.assets' resource reads are off here (row assets.resources)"));
        var off = Content.TryGetVoxelMaterials()!;
        Assert.False(off.Available);
        Assert.Contains("assets.resources", off.Reason);
    }

    // ---- the game's own data (KILN_XHL_RES: bytes extracted with tools/kfc; never committed) ---------------------------

    [Fact]
    public void RealGameData()
    {
        var dir = Environment.GetEnvironmentVariable("KILN_XHL_RES");
        Assert.SkipUnless(dir is { Length: > 0 } && Directory.Exists(dir), "set KILN_XHL_RES to a folder of game bytes extracted with tools/kfc");
        var registry = File.ReadAllBytes(Path.Combine(dir!, "registry.bin"));
        var guids = Directory.GetFiles(Path.Combine(dir!, "worlds")).Select(f => VoxelMaterialData.WorldGuids(File.ReadAllBytes(f), L)).FirstOrDefault(g => g is not null);
        var placers = Directory.GetFiles(Path.Combine(dir!, "items"))
            .Select(f => VoxelMaterialData.ParsePlacer(File.ReadAllBytes(f), L, isGame: true)).OfType<VoxelPlacer>().ToList();
        var names = Directory.GetFiles(Path.Combine(dir!, "items")).Select(f => File.ReadAllBytes(f))
            .ToDictionary(b => BinaryPrimitives.ReadUInt32LittleEndian(b), b => GameTextData.BlobString(b, L.ItemDebugName) ?? "");
        var m = VoxelMaterialData.Build(registry, L, guids, placers, HardnessName)!;
        Assert.Equal(116, m.Count(x => x.Kind == VoxelMaterialKind.Terrain));
        Assert.Equal(85, m.Count(x => x.Kind == VoxelMaterialKind.Building));
        Assert.NotNull(guids);
        // docs/re/building.md 1.6: copper ore (26) is VeryHard, 8 health, gives Material_T2_Ore_Copper per 8 cells
        var copper = m.Single(x => x.CellId == 26);
        Assert.Equal(("VeryHard", 8, (int?)8, "Material_T2_Ore_Copper"), (copper.Hardness, copper.HealthPoints, copper.ExchangeRate, names[copper.LootItem.Value]));
        // three ids without loot: 5, 16, 34 (unbreakable world materials)
        Assert.Equal([5, 16, 34], m.Where(x => x.Kind == VoxelMaterialKind.Terrain && x.LootItem.Value == 0).Select(x => x.CellId));
        // 37 gives the grassland dirt item, as 1, 15 and 73 do (XHL's case): one label, four materials
        Assert.Equal([1, 15, 37, 73], m.Where(x => x.LootItem == m.Single(y => y.CellId == 37).LootItem).Select(x => x.CellId));
        Assert.Equal("Build_TerrainMaterial_T1_Dirt_Grassland", names[m.Single(x => x.CellId == 37).LootItem.Value]);
        Assert.Contains(m, x => x.Name == "TerrainStone" && x.Kind == VoxelMaterialKind.Terrain);
        Assert.Contains(m, x => x.Name == "WoodRough" && x.Kind == VoxelMaterialKind.Building);
        // at least the pool TerrainMaterials' 56 terrain ids are placed by an item, each in its own domain and GUID
        Assert.True(m.Count(x => x.Kind == VoxelMaterialKind.Terrain && x.PlacingItems.Count > 0) >= 56);
        File.WriteAllLines(Path.Combine(dir!, "materials.txt"), m.Select(x =>
            $"{x.CellId}\t{x.Kind}\t{x.Name}\t{(x.LootItem.Value == 0 ? "-" : names.GetValueOrDefault(x.LootItem.Value, "?"))}\t{x.ExchangeRate}\t{x.Hardness}\t{x.HealthPoints}\t" +
            string.Join(",", x.PlacingItems.Select(i => names.GetValueOrDefault(i.Value, "?")))));
        // the loca collection and one language's blob, when extracted
        var loca = Path.Combine(dir!, "loca.bin");
        if (File.Exists(loca))
        {
            var langs = GameTextData.Languages(File.ReadAllBytes(loca), GameTextLayout.Measured)!;
            Assert.Equal(15, langs.Count);
            Assert.Equal([1L, 3], langs.Take(2).Select(x => x.Value));       // En_Us, De_De first
            foreach (var blob in Directory.GetFiles(dir!, "loca-*.bin"))
            {
                var table = GameTextData.Table(File.ReadAllBytes(blob), GameTextLayout.Measured);
                Assert.NotNull(table);
                Assert.True(table!.Count > 10_000, $"{blob}: {table.Count} texts");
            }
        }
    }
}
