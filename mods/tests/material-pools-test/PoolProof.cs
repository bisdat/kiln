// SPDX-License-Identifier: MIT
#pragma warning disable KILNX028
using System.Text;
using System.Text.Json;
using Kiln;
namespace MaterialPoolsTest;

// Shared by the lab and the guided check. Reads served resources and item fields, never just declaration handles.
internal static class PoolProof
{
    internal sealed record Row(uint Slot,uint Id,string Name,uint[] Items);
    internal const string Fixture="mp-fixture";
    internal static readonly string[] Keys=["four","shared","full","roof"];
    internal static uint Item(string key)=>BlockIds.Fnv(BlockIds.Derive(Fixture,"item",key));
    private static int At(byte[] bytes,int field,int stride,out int count)
    {
        if(field<0 || (long)field+8>bytes.Length)throw new InvalidDataException("missing array header");
        var start=(long)field+BitConverter.ToInt32(bytes,field);count=BitConverter.ToInt32(bytes,field+4);
        if(count<0 || start<0 || start+(long)count*stride>bytes.Length)throw new InvalidDataException("array outside served bytes");
        return checked((int)start);
    }
    internal static Row[] ReadPools(byte[] bytes)
    {
        var first=At(bytes,0,24,out var count);if(count>4096)throw new InvalidDataException("unreasonable pool rows");
        return Enumerable.Range(0,count).Select(i=>{
            var row=first+i*24;var members=At(bytes,row+8,4,out var n);var text=At(bytes,row+16,1,out var len);
            if(n>64 || len>192)throw new InvalidDataException("invalid served pool bounds");
            return new Row(BitConverter.ToUInt32(bytes,row),BitConverter.ToUInt32(bytes,row+4),Encoding.UTF8.GetString(bytes,text,len),Enumerable.Range(0,n).Select(j=>BitConverter.ToUInt32(bytes,members+j*4)).ToArray());
        }).ToArray();
    }
    internal static byte[] Resource(string type)
    {
        var keys=Assets.ListResources(new AssetKey(Guid.Empty,type).Type);
        if(keys.Length!=1)throw new InvalidDataException($"expected one {type}, got {keys.Length}");
        return Assets.ReadResource(keys[0])??throw new InvalidDataException("served resource unavailable: "+type);
    }
    // The running Kiln build, as Kiln.Api carries it (0.2.0-dev+<day>.<commit>): the lab proof records it, the guided check compares it.
    internal static string KilnBuild=>typeof(Kiln.Mod).Assembly.GetCustomAttributes(typeof(System.Reflection.AssemblyMetadataAttribute),false)
        .Cast<System.Reflection.AssemblyMetadataAttribute>().FirstOrDefault(a=>a.Key=="Kiln.Build")?.Value??Game.KilnVersion;
    internal static string Ids(IEnumerable<uint> ids)=>"["+string.Join(",",ids)+"]";
    internal static string Budget(CapacityQueryResult b)=>$"ok {b.Ok} {b.Budget.Readiness}: capacity {b.Budget.Capacity}, game {b.Budget.GameUsed}, kiln {b.Budget.KilnUsed}, reserved {b.Budget.Reserved}, available {b.Budget.Available?.ToString()??"-"}";
    // Every row says what it read; a FAIL row also says what it expected. Detail goes to the log, never into the verdict.
    internal static IReadOnlyList<(string Name,bool Ok,string Detail)> Inspect()
    {
        var checks=new List<(string,bool,string)>();
        void Check(string name,bool ok,string detail="")=>checks.Add((name,ok,detail));
        Check("capabilities",WorldTables.Available && Capabilities.Is("worldtables.material_pools") && Capabilities.Is("worldtables.budgets"),WorldTables.State);
        var rows=ReadPools(Resource("keen::VoxelBlueprintMaterialPoolRegistryResource"));
        var baseline=JsonSerializer.Deserialize<Baseline[]>(BaselineJson,new JsonSerializerOptions{PropertyNameCaseInsensitive=true})!;
        var changed=new List<string>();
        foreach(var b in baseline) {
            var named=rows.Where(r=>r.Name==b.Name).ToArray();
            if(named.Length!=1)changed.Add($"{b.Name}: {named.Length} rows");
            else if(named[0] is var r && (r.Slot!=b.EquipmentSlot || r.Id!=b.PoolId || !r.Items.SequenceEqual(b.Items)))
                changed.Add($"{b.Name}: expected pool {b.PoolId} slot {b.EquipmentSlot} members {Ids(b.Items)}; read pool {r.Id} slot {r.Slot} members {Ids(r.Items)}");
        }
        Check("vanilla-rows-unchanged",changed.Count==0,changed.Count==0?$"{baseline.Length} rows, {rows.Length} served":string.Join(" | ",changed));
        var wall=baseline.Single(b=>b.Name=="BuildingDefaultMaterials");
        var roof=baseline.Single(b=>b.Name=="BuildingRoofMaterials");
        var missing=new List<string>();
        uint Game(string debugName){var id=Content.Items.Get(debugName)?.Id??0;if(id==0)missing.Add(debugName+" not in Content.Items");return id;}
        var expected=new Dictionary<string,uint[]> {
            ["four"]=[Item("ice"),Item("frost"),Item("slate"),Game("Block_T1_Wood_Rough_CRAFTING")],
            ["shared"]=[Item("ice"),Game("Block_T1_Stone_Rough_CRAFTING")],
            ["full"]=[..wall.Items,Item("ice"),Item("frost"),Item("slate")],
            ["roof"]=[Game("Block_Roof_T1_PlantFibre")]
        };
        using var status=JsonDocument.Parse(WorldTables.StatusJson());
        var bindings=status.RootElement.GetProperty("shapeBindings").EnumerateArray().ToArray();
        var shapes=Resource("keen::VoxelBlueprintItemRegistryResource");var shapesAt=At(shapes,0,28,out var shapesCount);
        foreach(var key in Keys) {
            var name=Fixture+":"+key;var pool=rows.SingleOrDefault(r=>r.Name==name);var guid=BlockIds.Derive(Fixture,"materialpool",key);
            var slot=key=="roof"?roof.EquipmentSlot:wall.EquipmentSlot;var want=expected[key];
            var member=pool is not null && pool.Id==BlockIds.Fnv(guid) && pool.Slot==slot && want.All(i=>i!=0) && pool.Items.SequenceEqual(want);
            var read=pool is null?$"no served row named {name} ({rows.Length} rows: {string.Join(", ",rows.Select(r=>r.Name))})":$"read pool {pool.Id} slot {pool.Slot} members {Ids(pool.Items)}";
            Check("membership-"+key,member,member?$"pool {pool!.Id} slot {pool.Slot}, {pool.Items.Length} member(s) in order":
                $"expected pool {BlockIds.Fnv(guid)} slot {slot} members {Ids(want)}; {read}{(missing.Count>0?"; "+string.Join(", ",missing.Distinct()):"")}");
            var item=Content.Items.Get(Fixture+"."+key+"_shape");
            var reference=item?.GetRef("equipment.voxelBlueprintMaterialPool");
            var bound=item is not null && bindings.Any(b=>b.GetProperty("item").GetString()==item.Guid.ToString() && b.GetProperty("pool").GetString()==guid.ToString());
            var shapeRow=item is not null && Enumerable.Range(0,shapesCount).Any(i=>BitConverter.ToUInt32(shapes,shapesAt+28*i)==item.Id);
            Check("binding-"+key,item is not null && reference==guid && bound && shapeRow,
                item is null?$"no item {Fixture}.{key}_shape":$"item {item.Guid} ({item.Id}) pool reference {reference?.ToString()??"none"} (expected {guid}), status binding {bound}, shape row {shapeRow}");
            var model=Assets.ReadResource(new AssetKey(BlockIds.Derive(Fixture,"item",key+"_shape"),"keen::VoxelModelResource"));
            var cells=model is {Length:>=28}?model.Skip(24).Count(v=>v==(key=="roof"?8:0x12)):-1;
            Check("preview-data-"+key,cells==3,model is null?"no preview model":$"{model.Length} bytes, {cells} filled cell(s) (expected 3)");
            var budget=WorldTables.GetPoolBudget(name);
            Check("budget-"+key,budget.Ok && budget.Budget.Capacity==64 && budget.Budget.GameUsed==0 && budget.Budget.KilnUsed==want.Length && budget.Budget.Reserved==0,
                Budget(budget)+$" (expected capacity 64, game 0, kiln {want.Length}, reserved 0)");
        }
        var shared=rows.SingleOrDefault(r=>r.Name==Fixture+":shared");var four=rows.SingleOrDefault(r=>r.Name==Fixture+":four");
        var materials=WorldTables.Materials.Where(m=>m.Mod==Fixture && m.Live).ToArray();
        Check("shared-material-one-id",materials.Length==3 && materials.Select(m=>m.CellId).Distinct().Count()==3 && shared is not null && four is not null && shared.Items[0]==four.Items[0] && materials.Count(m=>m.Guid==BlockIds.Derive(Fixture,"material","ice"))==1,
            $"live fixture materials {string.Join(", ",materials.Select(m=>m.Block+"="+m.CellId))}; shared first {shared?.Items.FirstOrDefault().ToString()??"no row"}, four first {four?.Items.FirstOrDefault().ToString()??"no row"}");
        Check("no-65-served",rows.All(r=>r.Items.Length<=64) && !rows.Any(r=>r.Name.EndsWith(":too-many",StringComparison.Ordinal)),string.Join(", ",rows.Select(r=>r.Name+" "+r.Items.Length)));
        var vanilla=Content.Items.Get(BlockIds.Fnv(Guid.Parse("74cf7092-c244-49a4-87bf-7e95641037ba")));
        var vanillaRef=vanilla?.GetRef("equipment.voxelBlueprintMaterialPool");
        Check("vanilla-shape-binding",vanilla is not null && vanillaRef is {} v && BlockIds.Fnv(v)==wall.PoolId,
            vanilla is null?"no vanilla shape item 74cf7092-c244-49a4-87bf-7e95641037ba":$"{vanilla.Name}: pool reference {vanillaRef?.ToString()??"none"} (id {(vanillaRef is {} g?BlockIds.Fnv(g):0)}, expected {wall.PoolId})");
        var building=WorldTables.GetMaterialBudget(WorldMaterialCategory.Building);var terrain=WorldTables.GetMaterialBudget(WorldMaterialCategory.Terrain);
        Check("material-budgets",building.Ok && building.Budget.Capacity==128 && building.Budget.GameUsed==79 && building.Budget.KilnUsed==3 && terrain.Ok && terrain.Budget.Capacity==127 && terrain.Budget.GameUsed==116,
            $"building {Budget(building)} (expected 128/79/3); terrain {Budget(terrain)} (expected 127/116)");
        var unknown=WorldTables.GetPoolBudget("mp-fixture:absent");
        Check("unknown-pool",unknown.Budget.Readiness==CapacityReadiness.NotFound,Budget(unknown));
        return checks;
    }
    private sealed record Baseline(string Name,uint EquipmentSlot,uint PoolId,uint[] Items);
    private const string BaselineJson="""
[{"name":"BuildingDefaultMaterials","equipmentSlot":18,"poolId":2310840736,"items":[2543947810,3721356294,3877300758,2041116756,3288587434,688801496,2848160320,3775747292,2581939916,2825372463,1856761753,1406367026,2216303598,72900743,1555483091,582832101,2875012578,310129061,1506458989,3379333313,2175686957,2012474258,3337807364,2871749713,641139736,1956408861,2365603799,3627519922,3312174805,1358536105,296562836,168200942,54733896,3178080467,485052852,53541119,1619485296,276744828,1405390032,192912459,1176719610,3402080269,2310650651,627477408,1010285014,2615018308,3950526602,835259216,3380780330,2762553617,3575022075,755945588,531118786,1253279979,2587516407,3952140415,454345705,2130648477,2060473756,57557573,2067027342]},{"name":"BuildingRoofMaterials","equipmentSlot":19,"poolId":2696841766,"items":[4077117519,196827131,3502973158,551181842,3389648330,1699388154,3899129081,3147026944,1398837105,0,2670548968,228876949,4234622287,1749208422]},{"name":"TerrainMaterials","equipmentSlot":17,"poolId":851579436,"items":[1558115984,890360552,1068232992,631520303,1181280513,760251531,1823674426,3604850591,2493843175,3489782185,3937186197,2385754716,260133819,3152907053,3445636557,1401630464,447697249,323499580,3938514298,4199799645,3865976041,3585782303,2541805223,3376745431,2992740264,1853942795,885155884,3234886486,1930450533,3321738283,1187543406,1257792166,2732808327,2080424991,4095370049,2421105715,2517406505,459345780,4219078478,3787344002,2925726773,1422307195,4044361518,1458330217,4233423640,1619282664,3620677285,3232269711,4056696698,2252426664,2780052662,2572858337,2181045178,3899759098,303090384,2851443310]},{"name":"OvergrowthMaterials","equipmentSlot":20,"poolId":2050574174,"items":[3148270344,779558916,1353762644,594839802,1521452298]}]
""";
}
