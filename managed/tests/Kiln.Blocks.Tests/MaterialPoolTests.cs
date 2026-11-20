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
            var roof=Assert.Throws<ArgumentException>(()=>Compiler.Lower(mod,"wrong_roof",new BlueprintBlock {Name="Wrong",Roof=true,Cells=[["#"]],MaterialPool="cold_walls"}));
            Assert.StartsWith("Blueprint wrong_roof is a roof shape and material pool pool-test:cold_walls a wall pool: Roof must agree.",roof.Message);
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
    // Item 10: what the SDK bake records of world-table blocks declared from C#. A C# material's strength (a StrengthBlock
    // handle) is lowered into the material itself; a crafting prop, a strength and a surface section of a define_json
    // document are kept as they are, and each block is reported with its own kind.
    [Fact]
    public void The_bake_keeps_every_world_table_section_with_its_own_kind()
    {
        WorldTablesNative.ForgetBaked("bake-kinds"); WorldTablesNative.Baking=true;
        try {
            var mod=new ModBlocks("bake-kinds",1) {ManifestClients="mod"};
            var tough=Compiler.Define(mod,"tough",new StrengthBlock {Hardness=Hardness.Hard,Health=300});
            Compiler.Define(mod,"frost",new BuildingMaterialBlock {Name="Frost",Strength="tough"});
            var report=System.Text.Json.Nodes.JsonNode.Parse(WorldTablesNative.DefineJson("""{"format":1,"mod":"bake-kinds","craftingProps":{"anvil":{"name":"Anvil"}},"strengths":{"soft":{"hardness":"soft"}},"surfaces":{"glow":{"side":{"tint":"#ffffff"}}},"terrainMaterials":{"mud":{"name":"Mud"}}}""")!)!;
            var baked=WorldTablesNative.BakedDocument("bake-kinds")!;
            Assert.Equal("hard",(string?)baked["buildingMaterials"]!["frost"]!["strength"]!["hardness"]);
            Assert.Equal(300,(int?)baked["buildingMaterials"]!["frost"]!["strength"]!["health"]);
            Assert.NotNull(baked["craftingProps"]?["anvil"]);
            Assert.NotNull(baked["strengths"]?["soft"]);
            Assert.NotNull(baked["surfaces"]?["glow"]);
            var kinds=report["blocks"]!.AsArray().ToDictionary(b=>(string)b!["name"]!,b=>(string)b!["kind"]!);
            Assert.Equal("craftingProp",kinds["anvil"]);
            Assert.Equal("terrainMaterial",kinds["mud"]);
            Assert.False(kinds.ContainsKey("soft"));
            _=tough;
        } finally {WorldTablesNative.Baking=false; WorldTablesNative.ForgetBaked("bake-kinds");}
    }
}
