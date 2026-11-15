// SPDX-License-Identifier: MIT
#pragma warning disable KILNX028
using System.Text.Json;
using Kiln;
namespace MaterialPoolsTest;

public sealed class MaterialPoolsMod:Mod
{
    private readonly List<string> _passed=[];
    private int _failed;
    private string Mode=>File.Exists(Path.Combine(Context.DataDirectory,"mode"))?File.ReadAllText(Path.Combine(Context.DataDirectory,"mode")).Trim():Mods.Side==ModSide.Client?"observe":"served";
    private void Check(string name,bool ok,string detail="")
    {
        if(ok)_passed.Add(name);else ++_failed;
        Context.Log.Info($"mp: {(ok?"PASS":"FAIL")} {name}{(detail.Length>0?": "+detail:"")}");
    }
    public override void OnLoad(){Directory.CreateDirectory(Context.DataDirectory);File.Delete(Path.Combine(Context.DataDirectory,"result.json"));_ = Run();}
    private async Task Run()
    {
        try {
            for(var i=0;i<7200 && !(WorldTables.Available && Content.Ready);++i)await Context.Scheduler.NextTick();
            if(!WorldTables.Available || !Content.Ready)throw new InvalidOperationException("worldtables/content did not become ready");
            for(var i=0;i<100;++i)await Context.Scheduler.NextTick();
            switch(Mode) {
                case "observe":foreach(var (name,ok,detail) in PoolProof.Inspect())Check(name,ok,detail);ServedAndErrors();break;
                case "served":
                    foreach(var (name,ok,detail) in PoolProof.Inspect())Check(name,ok,detail);
                    ServedAndErrors();await Cells(write:true,absent:false);break;
                case "restart":
                    foreach(var (name,ok,detail) in PoolProof.Inspect())Check(name,ok,detail);
                    await Cells(write:false,absent:false);break;
                case "absent":await Cells(write:false,absent:true);break;
                case "late-material":
                    var pending=Blocks.Define("ungrouped",new BuildingMaterialBlock{Name="Late Ungrouped Block"});
                    var vanillaRoom=WorldTables.GetPoolBudget("BuildingDefaultMaterials");
                    Check("late-material-uses-vanilla-room",pending.Status==TableBlockStatus.Pending && vanillaRoom.Ok && vanillaRoom.Budget.KilnUsed==0 && vanillaRoom.Budget.Available==3);
                    break;
                case "late":Late();break;
                case "replay":await Replay();break;
                case "inactive":
                    var inactiveId=Guid.Parse(File.ReadAllText(Path.Combine(Context.DataDirectory,"receipt.txt")));
                    Check("inactive-request-unapplied",WorldTables.GetMaterialPoolRegistration(inactiveId).Code==DefinitionRefusalCode.NotFound && WorldTables.GetPoolBudget("material-pools-test:late").Budget.Readiness==CapacityReadiness.NotFound);
                    using(var inactive=JsonDocument.Parse(WorldTables.StatusJson()))Check("inactive-file-named",inactive.RootElement.GetProperty("errors").EnumerateArray().Any(e=>e.GetString()!.Contains("material-pools-test",StringComparison.Ordinal) && e.GetString()!.Contains("owning mod is not loaded",StringComparison.Ordinal)));
                    break;
                case "torn":
                    var oldId=Guid.Parse(File.ReadAllText(Path.Combine(Context.DataDirectory,"receipt.txt")));
                    using(var torn=JsonDocument.Parse(WorldTables.StatusJson()))Check("torn-envelope-refused",WorldTables.GetMaterialPoolRegistration(oldId).Code==DefinitionRefusalCode.NotFound && WorldTables.GetPoolBudget("material-pools-test:late").Budget.Readiness==CapacityReadiness.NotFound && torn.RootElement.GetProperty("errors").EnumerateArray().Any(e=>e.GetString()!.Contains("torn/invalid",StringComparison.Ordinal)));
                    break;
                case "building-seed":Boundary(WorldMaterialCategory.Building,49,0,0);await Cells(write:true,absent:false,mod:"mp-cap-building");break;
                case "building-retained":Boundary(WorldMaterialCategory.Building,0,49,1);await Cells(write:false,absent:true);break;
                case "terrain-boundary":Boundary(WorldMaterialCategory.Terrain,11,0,1);break;
                case "status":Check("status-snapshot",true);break;
                default:throw new InvalidOperationException("unknown test mode "+Mode);
            }
        } catch(Exception e){Check("completed-without-exception",false,e.Message);}
        var status=WorldTables.StatusJson();File.WriteAllText(Path.Combine(Context.DataDirectory,"served.json"),status);
        var result=new {mode=Mode,hash=WorldTables.Hash,passed=_passed,failed=_failed};
        var path=Path.Combine(Context.DataDirectory,"result.json");File.WriteAllText(path+".tmp",JsonSerializer.Serialize(result));File.Move(path+".tmp",path,true);
        Context.Log.Info($"mp: DONE {Mode} {_passed.Count} passed {_failed} failed hash {WorldTables.Hash}");
    }
    private static BuildingMaterialPoolBlock CodePool()=>new(){Name="Served Code Pool",Materials=[new(GameMaterial:"WoodRough"),new(GameMaterial:"StoneRough")]};
    private void ServedAndErrors()
    {
        var registered=Blocks.Define("served",CodePool(),new DefinitionRegistrationOptions());
        Check("served-registration",registered.Ok && registered.State==DefinitionRegistrationState.Registered && registered.SlotsConsumed==2,registered.ToString());
        var repeat=Blocks.Define("served",CodePool(),new DefinitionRegistrationOptions());
        Check("served-idempotent",repeat==registered || repeat.RegistrationId==registered.RegistrationId && repeat.SnapshotRevision==registered.SnapshotRevision);
        bool Throws(Action action){try{action();return false;}catch(ArgumentException){return true;}}
        Check("65-refused-before-serving",Throws(()=>Blocks.Define("too-many",CodePool() with {Materials=Enumerable.Repeat(new BuildingMaterialPoolEntry(GameMaterial:"WoodRough"),65).ToArray()},new DefinitionRegistrationOptions(DefinitionTiming.AllowNextStart))) && WorldTables.GetPoolBudget("material-pools-test:too-many").Budget.Readiness==CapacityReadiness.NotFound);
        Check("duplicate-refused",Throws(()=>Blocks.Define("duplicate",CodePool() with {Materials=[new(GameMaterial:"WoodRough"),new(GameMaterial:"WoodRough")]},new DefinitionRegistrationOptions())));
        Check("roof-refused",Throws(()=>Blocks.Define("wrong-roof",CodePool() with {Roof=true},new DefinitionRegistrationOptions())));
        var absent=Blocks.Define("missing",CodePool() with {Materials=[new(GameMaterial:"ThisMaterialDoesNotExist")]},new DefinitionRegistrationOptions(DefinitionTiming.AllowNextStart));
        Check("unknown-base-refused",!absent.Ok && absent.Code==DefinitionRefusalCode.UnknownBase && absent.SlotsConsumed==0);
    }
    private void Late()
    {
        var pool=CodePool() with {Name="Late Code Pool"};
        var closed=Blocks.Define("late",pool,new DefinitionRegistrationOptions());
        Check("late-registry-closed",!closed.Ok && closed.Code==DefinitionRefusalCode.RegistryClosed && closed.SlotsConsumed==0);
        var accepted=Blocks.Define("late",pool,new DefinitionRegistrationOptions(DefinitionTiming.AllowNextStart));
        Check("late-accepted-zero-slots",accepted.Ok && accepted.State==DefinitionRegistrationState.AcceptedNextStart && accepted.SlotsConsumed==0);
        var again=Blocks.Define("late",pool,new DefinitionRegistrationOptions(DefinitionTiming.AllowNextStart));
        Check("late-idempotent",again.RegistrationId==accepted.RegistrationId && again.SnapshotRevision==accepted.SnapshotRevision);
        Check("late-not-live",WorldTables.GetPoolBudget("material-pools-test:late").Budget.Readiness==CapacityReadiness.NotFound);
        if(!accepted.Ok)throw new InvalidOperationException("cannot checkpoint a refused deferred request");
        File.WriteAllText(Path.Combine(Context.DataDirectory,"receipt.txt"),accepted.RegistrationId.ToString());
    }
    private async Task Replay()
    {
        var id=Guid.Parse(File.ReadAllText(Path.Combine(Context.DataDirectory,"receipt.txt")));
        var receipt=WorldTables.GetMaterialPoolRegistration(id);
        Check("restart-receipt-registered",receipt.Ok && receipt.State==DefinitionRegistrationState.Registered && receipt.SlotsConsumed==2,receipt.ToString());
        for(int i=0;i<3;++i)await Context.Scheduler.NextTick();
        using(var ownership=JsonDocument.Parse(WorldTables.StatusJson())) {
            var row=ownership.RootElement.GetProperty("poolRegistrations").EnumerateArray().Single(r=>r.GetProperty("definition").GetString()==receipt.Definition.ToString());
            Check("restart-owner-adopted-before-redefine",row.GetProperty("owner").GetUInt64()!=0 && row.GetProperty("state").GetUInt32()==(uint)DefinitionRegistrationState.Registered);
        }
        var same=Blocks.Define("late",CodePool() with {Name="Late Code Pool"},new DefinitionRegistrationOptions());
        Check("restart-same-receipt",same.Ok && same.RegistrationId==id && same.SlotsConsumed==2);
        var actual=PoolProof.ReadPools(PoolProof.Resource("keen::VoxelBlueprintMaterialPoolRegistryResource"));
        Check("restart-one-row",actual.Count(r=>r.Name=="material-pools-test:late")==1 && actual.Single(r=>r.Name=="material-pools-test:late").Items.Length==2);
    }
    private sealed record CellWitness(Guid Guid,int[] Box);
    private async Task Cells(bool write,bool absent,string mod=PoolProof.Fixture)
    {
        for(var i=0;i<7200 && Build.Grid is null;++i)await Context.Scheduler.NextTick();
        var path=Path.Combine(Context.DataDirectory,"cells.json");CellWitness[] witnesses;
        if(write) {
            var positionFile=Path.Combine(Context.DataDirectory,"position.json");
            var position=File.Exists(positionFile)?JsonSerializer.Deserialize<double[]>(File.ReadAllText(positionFile)):null;
            var boundsFile=Path.Combine(Context.DataDirectory,"base-box.json");
            var bounds=File.Exists(boundsFile)?JsonSerializer.Deserialize<double[]>(File.ReadAllText(boundsFile))!:PoolGround.DefaultBase;
            var live=WorldTables.Materials.Where(m=>m.Mod==mod && m.Live && !m.Terrain).ToArray();
            if(mod=="mp-cap-building")live=live.Take(1).ToArray(); // one saved witness; the world's table retains all 49 definitions
            if(live.Length==0)throw new InvalidOperationException("no live test material to write");
            // Selection must finish for ALL materials before the first write. Failure leaves the world untouched.
            var boxes=await PoolGround.Find(live.Length,bounds,position,p=>Build.BoxAround(p,1,1,1),b=>Build.ReadAsync(b));
            foreach(var box in boxes)Context.Log.Info($"mp: selected cells {box.X} {box.Y} {box.Z} {box.SizeX} {box.SizeY} {box.SizeZ}");
            Check("all-ground-read-before-write",boxes.Count==live.Length);
            var list=new List<CellWitness>();
            foreach(var material in live) {
                var box=boxes[list.Count];
                var cells=Enumerable.Repeat(Voxel.Of((byte)material.CellId,255),checked((int)box.Count)).ToArray();
                var result=await Build.WriteAsync(box,cells,new BuildOptions{Flags=BuildFlags.InBases});
                if(!result.Ok || !result.Verified || !result.Kept)throw new InvalidOperationException("test cells were not a verified kept write: "+result);
                var read=await Build.ReadAsync(box);if(!read.All(v=>v.Material==material.CellId && v.Fill==255))throw new InvalidOperationException("written cells did not read back");
                list.Add(new(material.Guid,[box.X,box.Y,box.Z,box.SizeX,box.SizeY,box.SizeZ]));
            }
            var save=await Saves.Request();if(!save.Success)throw new InvalidOperationException("game save failed: "+save);
            witnesses=list.ToArray();File.WriteAllText(path,JsonSerializer.Serialize(witnesses));
            Check("cells-written-and-game-saved",witnesses.Length==live.Length);
        } else {
            witnesses=JsonSerializer.Deserialize<CellWitness[]>(File.ReadAllText(path))!;
            if(witnesses.Length==0)throw new InvalidDataException("empty restart witness");
        }
        foreach(var w in witnesses) {
            var m=WorldTables.Materials.SingleOrDefault(m=>m.Guid==w.Guid);var a=w.Box;var box=new CellBox(a[0],a[1],a[2],a[3],a[4],a[5]);
            var cells=await Build.ReadAsync(box);
            if(m is null || m.Live==absent || !cells.All(v=>v.Material==m.CellId && v.Fill==255))throw new InvalidOperationException("retention/state/cells mismatch for "+w.Guid);
        }
        Check(absent?"absent-materials-and-cells-retained":"restart-materials-and-cells-live",true,witnesses.Length+" material GUID(s)");
    }
    private void Boundary(WorldMaterialCategory category,uint live,uint reserved,int expectedRefusals)
    {
        var budget=WorldTables.GetMaterialBudget(category);var total=category==WorldMaterialCategory.Building?128u:127u;var game=category==WorldMaterialCategory.Building?79u:116u;
        Check(category==WorldMaterialCategory.Building?"building-49-plus-1":"terrain-11-plus-1",budget.Ok && budget.Budget.Capacity==total && budget.Budget.GameUsed==game && budget.Budget.KilnUsed==live && budget.Budget.Reserved==reserved && budget.Budget.Available==0);
        using var status=JsonDocument.Parse(WorldTables.StatusJson());
        var refused=status.RootElement.GetProperty("refused").EnumerateArray().Where(v=>v.GetString()!.Contains(category==WorldMaterialCategory.Building?"mp-overflow":"mp-cap-terrain",StringComparison.Ordinal)).ToArray();
        Check("boundary-refusal-count",refused.Length==expectedRefusals,string.Join("; ",refused.Select(v=>v.GetString())));
        if(category==WorldMaterialCategory.Terrain) {
            var pool=WorldTables.GetPoolBudget("TerrainMaterials");Check("terrain-no-selector-work",pool.Ok && pool.Budget.GameUsed==56 && pool.Budget.KilnUsed==0);
        } else if(reserved>0) {
            var rows=PoolProof.ReadPools(PoolProof.Resource("keen::VoxelBlueprintMaterialPoolRegistryResource"));
            var ids=Enumerable.Range(0,49).Select(i=>BlockIds.Fnv(BlockIds.Derive("mp-cap-building","item","m"+i))).ToHashSet();
            Check("retained-building-no-selector-items",rows.All(r=>!r.Items.Any(ids.Contains)));
        }
    }
}
