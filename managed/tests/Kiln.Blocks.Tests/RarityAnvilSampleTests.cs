// SPDX-License-Identifier: MIT
// The upgrade station sample (mods/samples/RarityAnvil): its data twin, blocks/twins.json, reads as the block records and lowers
// to the same interaction as the C# blocks (the rule named in the mod, an anvil for the held item and a forge with the window).
using System.Runtime.CompilerServices;
using System.Text.Json.Nodes;
using Xunit;

namespace Kiln.BlockTests;

public sealed class RarityAnvilSampleTests
{
    private static ModBlocks Mod(string id) => new(id, 1) { ManifestClients = "kiln" };

    private static string Repo([CallerFilePath] string self = "")
    {
        for (var d = new DirectoryInfo(Path.GetDirectoryName(self) ?? ""); d != null; d = d.Parent)
            if (File.Exists(Path.Combine(d.FullName, "schema/kiln-blocks.schema.json"))) return d.FullName;
        throw new DirectoryNotFoundException("the repository");
    }

    [Fact]
    public void The_samples_data_twin_reads_and_lowers()
    {
        var doc = JsonNode.Parse(File.ReadAllText(Path.Combine(Repo(), "mods/samples/RarityAnvil/blocks/twins.json")))!;
        var mod = Mod("rarity-anvil");
        foreach (var (name, node) in doc["rarityUpgrades"]!.AsObject())
            Compiler.Lower(mod, name, BlockJson.Parse<RarityUpgradeBlock>(node!.ToJsonString()));
        foreach (var (name, node) in doc["rarityRolls"]!.AsObject())
            Assert.Single(BlockJson.Parse<RarityRollBlock>(node!.ToJsonString()).Items);
        foreach (var (name, node) in doc["weapons"]!.AsObject())
            Assert.Equal("Anvil Blade", BlockJson.Parse<WeaponBlock>(node!.ToJsonString()).Name);
        var props = doc["props"]!.AsObject();
        Assert.Equal(2, props.Count);
        var targets = new List<string?>();
        foreach (var (name, node) in props)
        {
            var j = Compiler.Lower(mod, name, BlockJson.Parse<PropBlock>(node!.ToJsonString())).Low.Json;
            Assert.Equal("rarity-anvil:standard-upgrades-json", (string?)j["interact"]!["upgrade"]!["rule"]);
            targets.Add((string?)j["interact"]!["upgrade"]!["target"]);
        }
        Assert.Equal(["held", "choose"], targets);
    }
}
