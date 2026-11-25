// SPDX-License-Identifier: MIT
// The kit's content lookups (0.2.0-dev.31.13, mod API 0.2.22): Kiln's own Content.TextIn / TryGetTexts / LoadTextsAsync,
// Items.Description / DisplayName(item, language) / TextTags and Content.TryGetVoxelMaterials / LoadVoxelMaterialsAsync on
// the texts and materials a test scripts, the capability rows they report, and parity with the readers of the game's bytes.
using Xunit;

namespace Kiln.TestKit.Tests;

public sealed class GameLookupsFakeTests
{
    private static readonly ItemId Pick = new(0x0c0ffee1), Ore = new(0x0c0ffee2), Logs = new(722268629);  // Logs: "Wood Logs" in items.tsv

    private static KilnHarness Client()
    {
        var kiln = new KilnHarness(new KilnHarnessOptions { Side = HarnessSide.HostingClient });
        kiln.Language = "de-de";
        kiln.GameTexts
            .Add("en-us", 10, "Copper Pickaxe").Add("en-us", 11, "Digs <b>ore</b>.").Add("en-us", 20, "Copper Ore")
            .Add("de-de", 10, "Kupferspitzhacke").Add("de-de", 11, "Gräbt <b>Erz</b>.").Add("de-de", 20, "Kupfererz")
            .AddItem(Pick, new ItemTextTags(10, 0, 11, 0)).AddItem(Ore, new ItemTextTags(20, 0, 0, 0));
        kiln.VoxelMaterials
            .Add(26, "Cell26", lootItem: Ore, exchangeRate: 8, hardness: "VeryHard", healthPoints: 8)
            .Add(194, "WoodRough", lootItem: Logs, exchangeRate: 5, placingItems: [new ItemId(0x2002)]);
        return kiln;
    }

    [Fact]
    public async Task A_client_harness_answers_texts_in_any_scripted_language()
    {
        using var kiln = Client();
        Assert.Equal(["en-us", "de-de"], Content.TextLanguages);
        var de = await Content.LoadTextsAsync(null);                         // the harness's language
        Assert.Equal(("de-de", GameTextSource.Game, 3), (de.Language, de.Source, de.Count));
        Assert.Equal("Gräbt <b>Erz</b>.", de.Get(11));
        Assert.Equal("Copper Ore", (await Content.LoadTextsAsync("en")).Get(20));
        Assert.Equal("Kupfererz", Content.TextIn(20, "de-de"));
        Assert.Equal(new ItemTextTags(10, 0, 11, 0), Items.TextTags(Pick));
        Assert.Equal("Gräbt Erz.", Items.Description(Pick));
        Assert.Equal("Digs ore.", Items.Description(Pick, "en-us"));
        Assert.Equal("Copper Pickaxe", Items.DisplayName(Pick, "en-us"));
        Assert.Equal(2, kiln.GameTexts.Reads);                               // de-de and en-us, each read once
        Assert.False((await Content.LoadTextsAsync("ja-jp")).Available);
        Assert.Equal("the game ships no texts in \"ja-jp\" (it has en-us, de-de)", (await Content.LoadTextsAsync("ja-jp")).Reason);
        Assert.Equal(CapabilityState.Available, Capabilities.Get(Content.GameTextsCapability).State);
        // a change applies to the next question
        kiln.GameTexts.Add("de-de", 20, "Kupfer");
        Assert.Equal("Kupfer", (await Content.LoadTextsAsync("de-de")).Get(20));
    }

    [Fact]
    public async Task A_client_harness_labels_voxel_materials_in_the_chosen_language()
    {
        using var kiln = Client();
        var de = await Content.LoadVoxelMaterialsAsync("de-de");
        Assert.True(de.Available);
        var ore = de.Get(26)!;
        Assert.Equal(("Cell26", Ore, (int?)8, "VeryHard", "Kupfererz", VoxelMaterialLabelSource.LootItem),
            (ore.Name, ore.LootItem, ore.ExchangeRate, ore.Hardness, ore.Label, ore.LabelSource));
        Assert.Equal("Copper Ore", (await Content.LoadVoxelMaterialsAsync("en-us")).Get(26)!.Label);
        Assert.Same(de.Get(194), de.PlacedBy(new ItemId(0x2002)));
        Assert.Equal([26], de.WithLootItem(Ore).Select(m => m.CellId));
        Assert.Equal(1, kiln.VoxelMaterials.Reads);                           // read once for both languages
        Assert.Equal(CapabilityState.Available, Capabilities.Get(Content.VoxelMaterialsCapability).State);
    }

    [Fact]
    public async Task A_dedicated_server_harness_has_no_texts_as_a_server_but_its_materials()
    {
        using var kiln = new KilnHarness();                                   // the default: a dedicated server
        kiln.GameTexts.Add("de-de", 20, "Kupfererz").AddItem(Ore, new ItemTextTags(20, 0, 0, 0));
        kiln.VoxelMaterials.Add(194, "WoodRough", lootItem: Logs, exchangeRate: 5);
        var de = await Content.LoadTextsAsync("de-de");
        Assert.False(de.Available);
        Assert.Equal(GameTextStore.ServerReason, de.Reason);                 // the reason a real server gives
        Assert.Null(Items.Description(Ore, "de-de"));
        Assert.DoesNotContain("de-de", Content.TextLanguages);               // English at most, from a Kiln catalog on this machine
        Assert.Equal(0, kiln.GameTexts.Reads);
        Content.TryGetTexts("de-de");
        var row = Capabilities.Get(Content.GameTextsCapability);
        Assert.Equal((CapabilityState.Unavailable, CapabilityReason.WrongSide), (row.State, row.Reason));
        var en = await Content.LoadVoxelMaterialsAsync("en-us");
        Assert.Equal(("Wood Logs", VoxelMaterialLabelSource.LootItem), (en.Get(194)!.Label, en.Get(194)!.LabelSource));   // Kiln's English
        Assert.Null((await Content.LoadVoxelMaterialsAsync("de-de")).Get(194)!.Label);
    }

    [Fact]
    public async Task A_test_can_switch_the_rows_off()
    {
        using var kiln = Client();
        kiln.Capabilities.Override(Content.GameTextsCapability, false);
        kiln.Capabilities.Override(Content.VoxelMaterialsCapability, false);
        var texts = await Content.LoadTextsAsync("de-de");
        Assert.Equal((false, "switched off by the test"), (texts.Available, texts.Reason));
        var materials = await Content.LoadVoxelMaterialsAsync("de-de");
        Assert.Equal((false, "switched off by the test"), (materials.Available, materials.Reason));
        Assert.Null(Items.Description(Pick));
    }

    [Fact]
    public async Task The_kit_answers_as_the_games_bytes_do()
    {
        // parity: the same material read from registry bytes (the game's path) and scripted in the kit gives the same record
        var layout = VoxelMaterialLayout.Measured;
        var registry = new byte[16 + 184];
        BitConverter.GetBytes(16).CopyTo(registry, 0);                         // terrainConfigs: one row right after the head
        BitConverter.GetBytes(1).CopyTo(registry, 4);
        BitConverter.GetBytes(8).CopyTo(registry, 8);                          // buildingConfigs: none (offset 8 from its field)
        registry[16] = 26;
        var loot = Guid.Parse("00000026-7e57-4a11-9c00-000000000026");
        loot.ToByteArray().CopyTo(registry, 16 + 4);
        registry[16 + 0x14] = 4;
        BitConverter.GetBytes(8u).CopyTo(registry, 16 + 0x18);
        BitConverter.GetBytes(8).CopyTo(registry, 16 + 0x1c);
        var fromBytes = VoxelMaterialData.Build(registry, layout, null, [], v => v == 4 ? "VeryHard" : null)!.Single();

        using var kiln = new KilnHarness(new KilnHarnessOptions { Side = HarnessSide.HostingClient });
        kiln.VoxelMaterials.Add(26, "Cell26", lootItem: fromBytes.LootItem, exchangeRate: 8, hardness: "VeryHard", healthPoints: 8);
        var fromKit = (await Content.LoadVoxelMaterialsAsync("en-us")).Get(26)!;
        Assert.Equal(Describe(fromBytes), Describe(fromKit with { Label = null, LabelSource = VoxelMaterialLabelSource.None }));
        Assert.Equal(new ItemId(GameTextData.Fnv16(loot.ToByteArray())), fromKit.LootItem);
    }

    private static string Describe(VoxelMaterial m) =>
        $"{m.CellId} {m.Kind} {m.Guid} {m.Name} {m.LootItem.Value} {m.ExchangeRate} [{string.Join(",", m.PlacingItems.Select(i => i.Value))}] {m.Hardness} {m.HealthPoints} {m.Label} {m.LabelSource}";

    [Fact]
    public void The_coverage_table_names_the_lookups()
    {
        var row = Assert.Single(Coverage.All, r => r.Area.StartsWith("Content lookups", StringComparison.Ordinal));
        Assert.Equal(CoverageKind.RecordingFake, row.Kind);
        Assert.Equal([Content.GameTextsCapability, Content.VoxelMaterialsCapability], row.Capabilities);
    }
}
