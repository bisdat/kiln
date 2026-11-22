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
