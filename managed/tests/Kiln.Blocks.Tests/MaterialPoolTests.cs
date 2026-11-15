using System.Text.Json;
using Xunit;
namespace Kiln.Blocks.Tests;
[Collection("item icons")]
public sealed class MaterialPoolTests
{
    [Fact]
    public void Chapter_pool_and_shape_read_typed_refs()
    {
        var pool=BlockJson.Parse<BuildingMaterialPoolBlock>("""{"name":"Cold Walls","materials":[{"material":"ice"},{"gameMaterial":"WoodRough"}]}""");
        Assert.Equal("ice",pool.Materials[0].Material!.Name);
        Assert.Equal("WoodRough",pool.Materials[1].GameMaterial);
        var shape=BlockJson.Parse<BlueprintBlock>("""{"name":"Cold Wall","cells":[["##"]],"materialPool":"cold_walls"}""");
        Assert.Equal("cold_walls",shape.MaterialPool!.Name);
        Assert.Equal(ClientsLevel.Mod,typeof(BuildingMaterialPoolBlock).GetCustomAttributes(typeof(BlockClientsAttribute),false).Cast<BlockClientsAttribute>().Single().Level);
    }
    [Fact]
    public void Entry_shape_and_boundaries_validate_before_native()
    {
        Assert.Throws<ArgumentException>(()=>MaterialPoolValidation.Check(new BuildingMaterialPoolBlock {Name="x",Materials=[]}));
        Assert.Throws<ArgumentException>(()=>MaterialPoolValidation.Check(new BuildingMaterialPoolBlock {Name="x",Materials=[new()]}));
        Assert.Throws<ArgumentException>(()=>MaterialPoolValidation.Check(new BuildingMaterialPoolBlock {Name="x",Materials=[new(Material:"ice",GameMaterial:"WoodRough")]}));
        Assert.Throws<ArgumentException>(()=>MaterialPoolValidation.Check(new BuildingMaterialPoolBlock {Name="x",Materials=Enumerable.Repeat(new BuildingMaterialPoolEntry(GameMaterial:"WoodRough"),65).ToArray()}));
    }

    [Fact]
    public void Pool_lowering_uses_the_writer_and_blueprint_guid_reference()
    {
        var mod=new ModBlocks("pool-test",1) {ManifestClients="mod"};
        string? poolDocument=null;
        WorldTablesNative.TestDefine=document=>{
            var doc=System.Text.Json.Nodes.JsonNode.Parse(document)!.AsObject();
            var section=doc.ContainsKey("buildingMaterialPools")?"buildingMaterialPools":"blueprints";
            if(section=="buildingMaterialPools") poolDocument=document;
            var name=doc[section]!.AsObject().First().Key;
            return new System.Text.Json.Nodes.JsonObject {["blocks"]=new System.Text.Json.Nodes.JsonArray(new System.Text.Json.Nodes.JsonObject {["name"]=name,["kind"]=section=="blueprints"?"blueprint":"buildingMaterialPool",["status"]="registered"})}.ToJsonString();
        };
        try {
            var h=(TableBlockHandle<BuildingMaterialPoolBlock>)Compiler.Define(mod,"cold_walls",new BuildingMaterialPoolBlock {Name="Cold Walls",Materials=[new(Material:"ice"),new(GameMaterial:"WoodRough")]});
            Assert.Equal(BlockIds.Derive("pool-test","materialpool","cold_walls"),h.Guid);
            Assert.Contains("pool-test:ice",poolDocument!);
            var lowered=Compiler.Lower(mod,"cold_wall",new BlueprintBlock {Name="Cold Wall",Cells=[["##"]],MaterialPool="cold_walls"});
            Assert.Equal(h.Guid.ToString(),(string?)lowered.Low.Json["items"]![0]!["refs"]!["equipment.voxelBlueprintMaterialPool"]);
            Assert.Throws<ArgumentException>(()=>Compiler.Lower(mod,"wrong_roof",new BlueprintBlock {Name="Wrong",Roof=true,Cells=[["#"]],MaterialPool="cold_walls"}));
        } finally {WorldTablesNative.TestDefine=null;}
    }
    [Fact]
    public void Offline_bake_captures_only_artifact_data_and_never_reports_registered()
    {
        WorldTablesNative.ForgetBaked("offline-pools"); WorldTablesNative.Baking=true;
        try {
            var mod=new ModBlocks("offline-pools",1) {ManifestClients="mod"};
            var h=(TableBlockHandle<BuildingMaterialPoolBlock>)Compiler.Define(mod,"walls",new BuildingMaterialPoolBlock {Name="Walls",Materials=[new(GameMaterial:"WoodRough")]});
            Assert.Equal(TableBlockStatus.Pending,h.Status);
            Assert.NotNull(WorldTablesNative.BakedDocument("offline-pools")!["buildingMaterialPools"]!["walls"]);
        } finally {WorldTablesNative.Baking=false; WorldTablesNative.ForgetBaked("offline-pools");}
    }
}
