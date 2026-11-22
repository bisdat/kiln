// SPDX-License-Identifier: MIT
using System.Text.Json.Nodes;
using Xunit;
namespace Kiln.TestKit.Tests;

public sealed class BuildingToolsFakeTests
{
    public class ToolMod:Mod
    {
        public override void OnLoad()
        {
            var pool=Blocks.Define("materials",new ToolMaterialPoolBlock {Name="Loose stone",Entries=[new("Build_TerrainMaterial_T1_Rubble",8,16)]});
            var brush=Blocks.Define("small",new TerrainBrushBlock {Name="Small sphere",Radius=0.5});
            Blocks.Define("tool",new BuildingToolBlock {Name="Terrain tool",MaterialPool=pool,Brushes=[brush],
                Primary=new() {Mode=TerrainToolMode.Remove,RemoveMaterials=[8]},Secondary=new() {Mode=TerrainToolMode.Add}});
        }
    }
    public sealed class ToolRecipeMod:ToolMod
    {
        public override void OnLoad()
        {
            base.OnLoad();
            var recipe=Blocks.Define("recipe",new RecipeBlock {Output="tool",TrackCrafted=true,Needs=new Dictionary<ItemRef,int> { ["Build_TerrainMaterial_T1_Rubble"]=1 }});
            Blocks.Define("found",new KnowledgeBlock {Trigger=KnowledgeTrigger.Pickup,Of="tool"});
            Blocks.Define("made",new KnowledgeBlock {Trigger=KnowledgeTrigger.Craft,Of=recipe.Name});
        }
    }
    private sealed class EmptyMod:Mod;

    [Fact]
    public void A_pending_tool_recipe_is_recorded_without_claiming_live_content_and_survives_reload()
    {
        using var h=new KilnHarness();var mod=h.Load<ToolRecipeMod>(new ModManifest("tool-recipe").Set("clients","mod"));h.Step(2);
        Assert.True(mod.IsLoaded,mod.Error);Assert.Empty(h.Run(()=>Blocks.Report()[mod.Id].Errors));
        var id=h.Run(()=>Blocks.Find("recipe",mod.Id)!.Guid);
        var lowered=JsonNode.Parse(h.Run(()=>Blocks.Lowered("recipe",mod.Id))!)!;
        Assert.Single(lowered["recipes"]!.AsArray());Assert.False(h.Run(()=>Content.Available));
        var runtimeRecipe=lowered["recipes"]![0]!;
        Assert.Equal(BlockIds.Derive(mod.Id,"recipe","recipes/recipe"),id);
        Assert.Equal(id,Guid.Parse((string)runtimeRecipe["guid"]!));Assert.True((bool)runtimeRecipe["trackCrafted"]!);
        var pickup=JsonNode.Parse(h.Run(()=>Blocks.Lowered("found",mod.Id))!)!["knowledge"]!;
        var crafted=JsonNode.Parse(h.Run(()=>Blocks.Lowered("made",mod.Id))!)!["knowledge"]!;
        var toolId=h.Run(()=>BlockIds.Fnv(Blocks.Find("tool",mod.Id)!.Guid));
        Assert.Equal($"0x{toolId:x8}",(string?)pickup["id"]);Assert.Equal("both",(string?)pickup["scope"]);
        Assert.Equal($"0x{BlockIds.Fnv(id):x8}",(string?)crafted["id"]);Assert.Equal("player",(string?)crafted["scope"]);
        var again=h.Reload(mod);Assert.True(again.IsLoaded,again.Error);
        Assert.Equal(id,h.Run(()=>Blocks.Find("recipe",again.Id)!.Guid));
        // This only records a recipe for a known pending tool. Ordinary content calls are still unavailable.
        Assert.ThrowsAny<Exception>(()=>h.As(again,()=>Blocks.Define("ordinary",new RecipeBlock {Output="Material_T1_Wood_Logs"})));
    }

    [Fact]
    public void Tool_blocks_load_as_pending_without_inventing_live_rows_or_actions()
    {
        using var h=new KilnHarness();
        var before=h.Run(WorldTables.StatusJson);
        var mod=h.Load<ToolMod>(new ModManifest("tool-kit").Set("clients","mod"));h.Step(2);
        Assert.True(mod.IsLoaded,mod.Error);
        var pool=JsonNode.Parse(h.Run(()=>Blocks.Lowered("materials","tool-kit"))!)!;
        var tool=JsonNode.Parse(h.Run(()=>Blocks.Lowered("tool","tool-kit"))!)!;
        Assert.Equal("pending",(string?)pool["status"]);
        Assert.Equal("pending",(string?)tool["status"]);
        Assert.Equal(BlockIds.Derive("tool-kit","toolmaterialpool","materials").ToString(),(string?)tool["toolFamily"]!["pool"]);
        Assert.Equal(BlockIds.Derive("tool-kit","item","small").ToString(),(string?)tool["toolFamily"]!["brushes"]![0]!["item"]);
        Assert.Null(tool["items"]);Assert.Null(tool["programs"]);
        Assert.Equal(2,h.WorldTables.PendingCount); // policy plus brush, never a second game-visible pool
        var definitions=JsonNode.Parse(h.WorldTables.PendingToolDefinitionsJson("tool-kit"))!;
        Assert.Equal(8,(int?)definitions["toolMaterialPools"]!["materials"]!["entries"]![0]!["terrainMaterial"]);
        Assert.Equal("sphere",(string?)definitions["toolBrushShapes"]!["small"]!["shape"]);
        var cells=h.WorldTables.TerrainBrushCells("tool-kit","small")!;Assert.Equal(8,cells.Length);Assert.All(cells,c=>Assert.Equal(255,c));
        cells[0]=0;Assert.Equal(255,h.WorldTables.TerrainBrushCells("tool-kit","small")![0]);
        var after=JsonNode.Parse(h.Run(WorldTables.StatusJson))!;
        Assert.Equal(JsonNode.Parse(before)!["pools"]!.ToJsonString(),after["pools"]!.ToJsonString());
        Assert.Empty(after["customPools"]!.AsArray());Assert.Empty(after["shapeBindings"]!.AsArray());
        Assert.Equal(56u,h.Run(()=>WorldTables.GetPoolBudget("TerrainMaterials")).Budget.GameUsed);
        Assert.Equal("0000000000000000",h.Run(()=>WorldTables.Hash));
    }

    [Fact]
    public void Brush_samples_half_metre_centres_with_zero_fill_outside_the_sphere()
    {
        using var h=new KilnHarness();var mod=h.Load<EmptyMod>(new ModManifest("tool-sphere").Set("clients","mod"));h.Step(2);
        h.As(mod,()=>{
            var brush=Blocks.Define("medium",new TerrainBrushBlock {Name="Medium sphere",Radius=0.75});
            Blocks.Define("remove",new BuildingToolBlock {Name="Remove only",Brushes=[brush],Primary=new() {Mode=TerrainToolMode.Remove,RemoveMaterials=[8]},Secondary=new() {Mode=TerrainToolMode.Remove,RemoveMaterials=[8]}});
        });
        var cells=h.WorldTables.TerrainBrushCells("tool-sphere","medium")!;
        Assert.Equal(27,cells.Length);Assert.Equal(19,cells.Count(c=>c==255));Assert.Equal(8,cells.Count(c=>c==0));
        Assert.Equal(0,cells[0]);Assert.Equal(255,cells[13]);
    }

    [Fact]
    public void Pending_pool_storage_failure_is_named_and_not_persisted()
    {
        using var h=new KilnHarness();var mod=h.Load<EmptyMod>(new ModManifest("tool-storage").Set("clients","mod"));h.Step(2);
        h.WorldTables.StorageFails=true;
        var error=Assert.Throws<BlockError>(()=>h.As(mod,()=>Blocks.Define("materials",new ToolMaterialPoolBlock {Name="Loose stone",Entries=[new("Build_TerrainMaterial_T1_Rubble",8,16)]})));
        Assert.Contains("could not persist the pool definition and receipt",error.Message);Assert.Equal(0,h.WorldTables.PendingCount);
        Assert.Null(JsonNode.Parse(h.WorldTables.PendingToolDefinitionsJson("tool-storage"))!["toolMaterialPools"]);
    }

    [Fact]
    public void All_56_decoded_vanilla_members_validate_by_name_id_and_guid()
    {
        using var stream=typeof(KilnHarness).Assembly.GetManifestResourceStream("Kiln.TestKit.Fakes.WorldTables.TerrainPool.json");
        Assert.NotNull(stream);var fixture=JsonNode.Parse(stream)!;Assert.Equal(56,fixture["items"]!.AsArray().Count);
        using var h=new KilnHarness();var mod=h.Load<EmptyMod>(new ModManifest("tool-fixture").Set("clients","mod"));h.Step(2);
        foreach(var row in fixture["items"]!.AsArray())
            foreach(var field in new[]{"name","item","guid"}) {
                var item=field=="item"?row![field]!.ToString():(string)row![field]!;
                var key=field+row!["item"];
                h.As(mod,()=>Blocks.Define(key,new ToolMaterialPoolBlock {Name=key,Entries=[new(item,(byte)row["cell"]!,16)]}));
            }
        Assert.Equal(168,h.WorldTables.PendingCount);
    }

    [Theory]
    [InlineData("Build_TerrainMaterial_T1_Rubble",66,"item does not place the declared terrain material")]
    [InlineData("missing-terrain-item",8,"no unambiguous placeable terrain item missing-terrain-item")]
    public void Invalid_item_mapping_is_named_and_not_persisted(string item,byte cell,string reason)
    {
        using var h=new KilnHarness();var mod=h.Load<EmptyMod>(new ModManifest("tool-invalid").Set("clients","mod"));h.Step(2);
        var error=Assert.Throws<BlockError>(()=>h.As(mod,()=>Blocks.Define("materials",new ToolMaterialPoolBlock {Name="Invalid",Entries=[new(item,cell,16)]})));
        Assert.Contains(reason,error.Message);Assert.Equal(0,h.WorldTables.PendingCount);
    }

    [Fact]
    public void Placeable_non_member_is_refused_with_the_production_policy_subset_error()
    {
        using var h=new KilnHarness();var mod=h.Load<EmptyMod>(new ModManifest("tool-nonmember").Set("clients","mod"));h.Step(2);
        h.WorldTables.TerrainItem("outside-pool",42,8);
        var error=Assert.Throws<BlockError>(()=>h.As(mod,()=>Blocks.Define("materials",new ToolMaterialPoolBlock {Name="Invalid",Entries=[new("outside-pool",8,16)]})));
        Assert.Contains("entries[0].item: outside-pool is not a member of the qualified vanilla TerrainMaterials pool; tool pools are policy subsets, not new game rows",error.Message);
        Assert.Equal(0,h.WorldTables.PendingCount);
    }
}
