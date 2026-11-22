// SPDX-License-Identifier: MIT
using Xunit;
namespace Kiln.Blocks.Tests;
[Collection("item icons")]
public sealed class ToolBlocksTests
{
    [Fact]
    public void Tool_pool_json_and_typed_definition_lower_identically()
    {
        var json = BlockJson.Parse<ToolMaterialPoolBlock>("""{"name":"Loose stone","entries":[{"item":"Build_TerrainMaterial_T1_Rubble","terrainMaterial":8,"voxelsPerItem":16}]}""");
        var typed = new ToolMaterialPoolBlock { Name="Loose stone", Entries=[new("Build_TerrainMaterial_T1_Rubble",8,16)] };
        Assert.Equal(ToolBlockValidation.PoolNode(typed).ToJsonString(),ToolBlockValidation.PoolNode(json).ToJsonString());
        Assert.Equal("toolMaterialPools",BlockJson.SectionOf(typeof(ToolMaterialPoolBlock)));
        Assert.Equal("terrainBrushes",BlockJson.SectionOf(typeof(TerrainBrushBlock)));
        Assert.Equal("buildingTools",BlockJson.SectionOf(typeof(BuildingToolBlock)));
    }
    [Fact]
    public void Pool_bounds_duplicates_building_cells_and_invalid_costs_refuse()
    {
        Assert.Throws<ArgumentException>(()=>ToolBlockValidation.PoolNode(new() {Name="x",Entries=[]}));
        Assert.Throws<ArgumentException>(()=>ToolBlockValidation.PoolNode(new() {Name="x",Entries=Enumerable.Repeat(new ToolMaterialPoolEntry("a",8,16),65).ToArray()}));
        Assert.Throws<ArgumentException>(()=>ToolBlockValidation.PoolNode(new() {Name="x",Entries=[new("a",8,16),new("a",66,5)]}));
        Assert.Throws<ArgumentException>(()=>ToolBlockValidation.PoolNode(new() {Name="x",Entries=[new("a",8,16),new("b",8,5)]}));
        Assert.Throws<ArgumentException>(()=>ToolBlockValidation.PoolNode(new() {Name="x",Entries=[new("a",128,16)]}));
        Assert.Throws<ArgumentException>(()=>ToolBlockValidation.PoolNode(new() {Name="x",Entries=[new("a",8,0)]}));
    }
    [Fact]
    public void Brush_identity_names_and_radius_do_not_alias_vanilla()
    {
        var brush=BlockJson.Parse<TerrainBrushBlock>("""{"name":"Small stone brush","description":"Own description","identity":{"legacy":"c52ceb9f-b1b0-4b4b-a801-52bf8340c002"},"shape":"sphere","radius":0.5}""");
        Assert.Equal(3155237504u,brush.Identity!.Id);
        Assert.Equal("Small stone brush",brush.Name);
        Assert.Equal("Own description",brush.Description);
        Assert.Equal(0.5,brush.Radius);
        var mod=new ModBlocks("tool-test",1) {ManifestClients="mod"};
        var h=Compiler.Define(mod,"small",brush);
        Assert.Equal(brush.Identity.Guid,h.Guid);
        Assert.NotEqual(0x1a2bf7b0u,BlockIds.Fnv(h.Guid));
        Assert.Throws<ArgumentException>(()=>ToolBlockValidation.Brush(new() {Name="x",Radius=double.NaN}));
        Assert.Throws<ArgumentException>(()=>ToolBlockValidation.Brush(new() {Name="x",Radius=0}));
        Assert.Throws<ArgumentException>(()=>ToolBlockValidation.Brush(new() {Name="x",Radius=9}));
    }
    [Fact]
    public void Real_root_has_no_action_slots_and_only_brush_clones_receive_actions()
    {
        var rootPath=Environment.GetEnvironmentVariable("KILN_TOOL_ROOT_JSON");
        var brushPath=Environment.GetEnvironmentVariable("KILN_TOOL_BRUSH_JSON");
        Assert.SkipUnless(File.Exists(rootPath)&&File.Exists(brushPath),"Requires independently decoded game root/brush JSON fixtures.");
        using var root=System.Text.Json.JsonDocument.Parse(File.ReadAllText(rootPath!));
        using var brush=System.Text.Json.JsonDocument.Parse(File.ReadAllText(brushPath!));
        Assert.Equal("76650084-337b-44ad-966c-29d8617ac746",root.RootElement.GetProperty("objectId").GetString());
        Assert.Empty(root.RootElement.GetProperty("sequences").EnumerateArray());
        Assert.False(root.RootElement.GetProperty("equipment").GetProperty("repeatActionWhenPressed").GetBoolean());
        var slots=brush.RootElement.GetProperty("sequences");Assert.Equal(2,slots.GetArrayLength());
        Assert.Equal("Secondary",slots[0].GetProperty("triggeredBy").GetString());Assert.Equal("Primary",slots[1].GetProperty("triggeredBy").GetString());
        var mod=new ModBlocks("tool-root-shape-test",1) {ManifestClients="mod"};
        Compiler.Define(mod,"small",new TerrainBrushBlock {Name="Own small brush",Radius=0.5});
        WorldTablesNative.TestDefine=document=>{
            var parsed=System.Text.Json.Nodes.JsonNode.Parse(document)!;
            var name=parsed["toolBrushShapes"]!.AsObject().First().Key;
            return new System.Text.Json.Nodes.JsonObject {["blocks"]=new System.Text.Json.Nodes.JsonArray(new System.Text.Json.Nodes.JsonObject {["name"]=name,["kind"]="terrainBrush",["status"]="registered"})}.ToJsonString();
        };
        try {
            var lowered=Compiler.Lower(mod,"root",new BuildingToolBlock {Name="Own root",Description="Own description",Brushes=["small"],
                Primary=new() {Mode=TerrainToolMode.Remove,RemoveMaterials=[8]},Secondary=new() {Mode=TerrainToolMode.Remove,RemoveMaterials=[8]}});
            var items=lowered.Low.Json["items"]!.AsArray();Assert.Equal(2,items.Count);
            var rootItem=items.Single(i=>(string?)i!["base"]=="Tool_Building_Overgrowth")!;
            Assert.DoesNotContain(rootItem["fields"]!.AsObject(),p=>p.Key.StartsWith("sequences[",StringComparison.Ordinal));
            Assert.DoesNotContain(rootItem["refs"]!.AsObject(),p=>p.Key.StartsWith("sequences[",StringComparison.Ordinal));
            var brushItem=items.Single(i=>(string?)i!["base"]=="Building_Overgrowth_Shape_Small")!;
            Assert.True(brushItem["fields"]!.AsObject().ContainsKey("equipment.repeatActionWhenPressed"));
            // Root and brush use the existing terrain row; the tool's separate pool identity is policy-only.
            Assert.Equal("256c43ba-dd20-4369-b3bf-f49c671a59ec",(string?)rootItem["refs"]!["equipment.voxelBlueprintMaterialPool"]);
            Assert.Equal("256c43ba-dd20-4369-b3bf-f49c671a59ec",(string?)brushItem["refs"]!["equipment.voxelBlueprintMaterialPool"]);
        } finally {WorldTablesNative.TestDefine=null;}
    }
    [Fact]
    public void Bake_keeps_tool_pool_and_brush_descriptors_pending_not_live()
    {
        WorldTablesNative.ForgetBaked("tool-bake"); WorldTablesNative.Baking=true;
        try {
            var mod=new ModBlocks("tool-bake",1) {ManifestClients="mod"};
            var handle=Compiler.Define(mod,"rubble",new ToolMaterialPoolBlock {Name="Rubble",Entries=[new("Build_TerrainMaterial_T1_Rubble",8,16)]});
            Assert.Equal(BlockIds.Derive("tool-bake","toolmaterialpool","rubble"),handle.Guid);
            var doc=WorldTablesNative.BakedDocument("tool-bake")!;
            Assert.Equal(8,(int?)doc["toolMaterialPools"]!["rubble"]!["entries"]![0]!["terrainMaterial"]);
            Assert.Equal("pending",(string?)mod.Lowered["rubble"]["status"]);
        } finally {WorldTablesNative.Baking=false;WorldTablesNative.ForgetBaked("tool-bake");}
    }
}
