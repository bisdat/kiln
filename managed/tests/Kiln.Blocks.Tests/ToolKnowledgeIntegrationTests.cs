// SPDX-License-Identifier: MIT
using Xunit;

namespace Kiln.BlockTests;

[Collection("item icons")]
public sealed class ToolKnowledgeIntegrationTests
{
    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public void Pickup_knowledge_tracks_the_item_of_a_tool_or_brush(bool root)
    {
        var mod = new ModBlocks(root ? "tool-pickup-root" : "tool-pickup-brush", 1) { ManifestClients = "mod" };
        var previous = WorldTablesNative.Baking;
        WorldTablesNative.ForgetBaked(mod.Id);
        WorldTablesNative.Baking = true;
        try
        {
            var brush = Compiler.Define(mod, "brush", new TerrainBrushBlock { Name = "Small brush", Radius = 0.5 });
            BlockHandle item = brush;
            if (root)
                item = Compiler.Define(mod, "tool", new BuildingToolBlock
                {
                    Name = "Terrain tool",
                    Brushes = [brush.Name],
                    Primary = new TerrainToolAction { Mode = TerrainToolMode.Remove, RemoveMaterials = [8] },
                    Secondary = new TerrainToolAction { Mode = TerrainToolMode.Remove, RemoveMaterials = [8] },
                });

            var lowered = Compiler.Lower(mod, "picked-up", new KnowledgeBlock
            {
                Trigger = KnowledgeTrigger.Pickup,
                Of = item.Name,
            }).Low.Json["knowledge"]!;

            Assert.Equal($"0x{BlockIds.Fnv(item.Guid):x8}", lowered["id"]!.GetValue<string>());
            Assert.Equal("pickup", lowered["track"]!.GetValue<string>());
            Assert.Equal("both", lowered["scope"]!.GetValue<string>());
        }
        finally
        {
            WorldTablesNative.Baking = previous;
            WorldTablesNative.ForgetBaked(mod.Id);
        }
    }
}
