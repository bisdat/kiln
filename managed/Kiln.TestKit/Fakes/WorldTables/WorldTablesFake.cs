// SPDX-License-Identifier: MIT
#pragma warning disable KILNX028
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Text.Encodings.Web;
using System.Text.Json;
using System.Text.Json.Nodes;
namespace Kiln.TestKit;

internal sealed record PoolFakeMember(uint Item,bool Roof,bool Live=true,bool Terrain=false);
internal sealed record PoolFakeRow(string Mod,string Key,Guid Guid,string Canonical,uint[] Items,bool Roof);
internal sealed partial class PoolFakeState
{
    public readonly object Gate=new();
    public readonly Dictionary<string,PoolFakeMember> Materials=new(StringComparer.Ordinal);
    public readonly Dictionary<string,PoolFakeMember> Game=new(StringComparer.Ordinal) { ["WoodRough"]=new(1001,false),["StoneRough"]=new(1002,false),["RoofPlantFibre"]=new(1003,true) };
    public readonly Dictionary<string,HashSet<string>> Dependencies=new(StringComparer.Ordinal);
    public readonly Dictionary<string,PoolFakeRow> Served=new(StringComparer.Ordinal);
    public readonly Dictionary<Guid,DefinitionRegistrationResult> Receipts=[];
    public readonly Dictionary<Guid,string> Owners=[];
    public readonly Dictionary<Guid,string> Pending=[];
    public readonly Dictionary<Guid,ulong> ActiveOwners=[];
    public readonly HashSet<uint> OriginalIds=[0x89bca1a0,0xa0be8a26,0x32c2122c,0x7a39475e];
    public bool StorageFails;
    public CapacityReadiness Readiness=CapacityReadiness.Ready;
    public ulong Revision=1;
    public string Hash=>Served.Count==0?WorldTablesNative.EmptyHash:Convert.ToHexStringLower(SHA256.HashData(Encoding.UTF8.GetBytes(string.Join("\n",Served.OrderBy(p=>p.Key,StringComparer.Ordinal).Select(p=>p.Value.Guid+":"+p.Value.Canonical)))))[..16];
    public static string Canonical(JsonNode n)=>Sort(n).ToJsonString(new JsonSerializerOptions {Encoder=JavaScriptEncoder.UnsafeRelaxedJsonEscaping});
    private static JsonNode Sort(JsonNode n)=>n switch {JsonObject o=>new JsonObject(o.OrderBy(p=>p.Key,StringComparer.Ordinal).Select(p=>KeyValuePair.Create(p.Key,p.Value is null?null:Sort(p.Value)))),JsonArray a=>new JsonArray(a.Select(v=>v is null?null:Sort(v)).ToArray()),_=>n.DeepClone()};
    public (string Canonical,uint[] Items,DefinitionRefusalCode Code,string? Reason) Validate(string mod,BuildingMaterialPoolBlock block)
    {
        var node=Compiler.MaterialPoolNode(mod,block);var items=new List<uint>();string? refusal=null;
        foreach(var entry in node["materials"]!.AsArray()) {
            PoolFakeMember? member=null;
            if(entry!["gameMaterial"] is {} game)Game.TryGetValue((string)game!,out member);
            else {
                var name=(string)entry["material"]!;
                Materials.TryGetValue(name,out member);
                var owner=name.Split(':')[0];
                var declared=Dependencies.TryGetValue(mod,out var deps) && deps.Contains(owner) || Mods.Get(mod)?.Dependencies.Any(d=>d.Id==owner)==true;
                if(owner!=mod && !declared){refusal="material requires an enabled declared dependency";continue;}
            }
            if(member is null || !member.Live || member.Terrain || member.Item==0){refusal="unknown building material or missing block item";continue;}
            if(member.Roof!=block.Roof)throw new ArgumentException("roof disagreement");
            if(items.Contains(member.Item))throw new ArgumentException("duplicate resolved item id");
            items.Add(member.Item);
        }
        return(Canonical(node),items.ToArray(),refusal is null?DefinitionRefusalCode.None:DefinitionRefusalCode.UnknownBase,refusal);
    }
    public DefinitionRegistrationResult Prepare(string mod,string key,BuildingMaterialPoolBlock block,DefinitionTiming timing,bool ownerActive)
    {
        var checkedPool=Validate(mod,block);var guid=BlockIds.Derive(mod,"materialpool",block.Was??key);
        var id=DefinitionRegistration.DeriveId(mod,DefinitionRegistrationKind.BuildingMaterialPool,guid,SHA256.HashData(Encoding.UTF8.GetBytes(checkedPool.Canonical)));
        var isServed=Served.TryGetValue(mod+":"+key,out var row) && row.Guid==guid && row.Canonical==checkedPool.Canonical;
        DefinitionRegistrationResult Refuse(DefinitionRefusalCode code,string reason)=>new(false,reason,code,id,guid,DefinitionRegistrationKind.BuildingMaterialPool,isServed?DefinitionRegistrationState.Retained:DefinitionRegistrationState.Refused,isServed?(uint)row!.Items.Length:0,Revision,[]);
        if(!ownerActive)return Refuse(DefinitionRefusalCode.OwnerInactive,"owner is no longer active");
        if(Readiness!=CapacityReadiness.Ready)return Refuse(DefinitionRefusalCode.NotReady,"the boot tables are not ready");
        if(checkedPool.Code!=DefinitionRefusalCode.None)return Refuse(checkedPool.Code,checkedPool.Reason!);
        var poolId=BlockIds.Fnv(guid);
        if(OriginalIds.Contains(poolId) || Served.Values.Any(p=>p.Guid!=guid && BlockIds.Fnv(p.Guid)==poolId))return Refuse(DefinitionRefusalCode.IdCollision,"pool id collision");
        if(row is not null && row.Guid!=guid)return Refuse(DefinitionRefusalCode.NameTaken,"pool name is taken");
        if(isServed)
            return new(true,null,DefinitionRefusalCode.None,id,guid,DefinitionRegistrationKind.BuildingMaterialPool,DefinitionRegistrationState.Registered,(uint)row.Items.Length,Revision,[]);
        if(timing==DefinitionTiming.AllowNextStart)return new(true,null,DefinitionRefusalCode.None,id,guid,DefinitionRegistrationKind.BuildingMaterialPool,DefinitionRegistrationState.AcceptedNextStart,0,Revision,[]);
        return Refuse(DefinitionRefusalCode.RegistryClosed,"the boot material-pool registry is closed");
    }
    public DefinitionRegistrationResult Commit(DefinitionRegistrationResult r,string owner,string canonical)
    {
        if(Receipts.TryGetValue(r.RegistrationId,out var old) && old.State==r.State && old.Code==r.Code && old.SlotsConsumed==r.SlotsConsumed && old.Reason==r.Reason)return old;
        if(old?.State==DefinitionRegistrationState.AcceptedNextStart && r.Code==DefinitionRefusalCode.RegistryClosed)return r;
        if(r.State==DefinitionRegistrationState.AcceptedNextStart) {
            if(StorageFails)r=r with {Ok=false,State=DefinitionRegistrationState.Refused,Code=DefinitionRefusalCode.StorageFailure,Reason="could not persist the pool definition and receipt"};
            else Pending[r.Definition]=canonical;
        }
        if(r.Ok)foreach(var prior in Receipts.Values.ToArray())
            if(prior.RegistrationId!=r.RegistrationId && prior.Definition==r.Definition && prior.State==DefinitionRegistrationState.AcceptedNextStart)
                Receipts[prior.RegistrationId]=prior with {Ok=false,State=DefinitionRegistrationState.Refused,SlotsConsumed=0,Code=DefinitionRefusalCode.Superseded,Reason="a newer definition superseded this pending pool",SnapshotRevision=++Revision};
        r=r with {SnapshotRevision=++Revision};Receipts[r.RegistrationId]=r;Owners[r.RegistrationId]=owner;return r;
    }
    public JsonObject Status()=>new() {
        ["state"]="merged",["hash"]=Hash,["revision"]=Revision,
        ["pools"]=new JsonArray(new[]{"BuildingDefaultMaterials","BuildingRoofMaterials","TerrainMaterials","OvergrowthMaterials"}.Select(n=>(JsonNode)new JsonObject {["name"]=n}).ToArray()),
        ["customPools"]=new JsonArray(Served.Values.Select(p=>(JsonNode)new JsonObject {["name"]=p.Mod+":"+p.Key,["guid"]=p.Guid.ToString(),["poolId"]=BlockIds.Fnv(p.Guid),["items"]=new JsonArray(p.Items.Select(i=>(JsonNode)JsonValue.Create(i)!).ToArray())}).ToArray()),
        ["shapeBindings"]=new JsonArray(),
        ["poolRegistrations"]=new JsonArray(Receipts.Values.Select(r=>(JsonNode)new JsonObject {["registration"]=Convert.ToHexStringLower(DefinitionRegistration.IdBytes(r.RegistrationId)),["definition"]=r.Definition.ToString(),["mod"]=Owners[r.RegistrationId],["owner"]=ActiveOwners.GetValueOrDefault(r.Definition),["state"]=(uint)r.State,["code"]=(uint)r.Code,["slots"]=r.SlotsConsumed,["revision"]=r.SnapshotRevision,["reason"]=r.Reason}).ToArray())
    };
}

internal sealed class WorldTablesFake:IKitFake
{
    public string Service=>"kiln.worldtables";
    public void Register(){KitBackend.Instance.RegisterTable(Service,WorldTablesFakeTable.Create());FakeTables.Features(["worldtables.budgets","worldtables.material_pools"]);}
    public void Start(KilnHarness h){WorldTablesEvents.Reset();_ = KitFakes.State<PoolFakeState>(h);}
    public void BeforeStep(KilnHarness h,ulong step)
    {
        var s=KitFakes.State<PoolFakeState>(h);
        if(!s.RecipeSinkInstalled){Lowered._sinkOverride=new PendingToolRecipeSink(Lowered.Sink);s.RecipeSinkInstalled=true;}
        lock(s.Gate)
            foreach(var (definition,owner) in s.ActiveOwners)if(!h.PoolOwnerActive(owner))
                foreach(var r in s.Receipts.Values.ToArray())if(r.Definition==definition && r.State==DefinitionRegistrationState.Registered)
                    s.Receipts[r.RegistrationId]=r with {Ok=false,State=DefinitionRegistrationState.Retained,Code=DefinitionRefusalCode.OwnerInactive,Reason="owner is no longer active; the boot row remains in the game",SnapshotRevision=++s.Revision};
    }
    public void End(KilnHarness h)=>WorldTablesEvents.Reset();
    public void AddCoverage(List<CoverageRow> rows)=>rows.Add(new("WorldTables",CoverageKind.RecordingFake,[Service,"worldtables.budgets","worldtables.material_pools"],"Scripted immutable boot pools, budgets, refusal and receipts; pending tool-policy pools validated against the decoded 56-item vanilla terrain pool and item-to-cell mapping; computed spherical brush descriptors and fill bytes; recorded recipes for pending tools (kiln.content stays unavailable). No extra game-visible tool pool rows or live tool actions. New calls cannot register into the closed boot registry. No game resource ingestion, selector, renderer, save remap, or actual network handshake is modelled."));
}
internal static unsafe class WorldTablesFakeTable
{
    private static WorldTablesNative.Table* _table;
    private static readonly Dictionary<string,nint> Strings=new(StringComparer.Ordinal);
    private static WorldTablesNative.Str Text(string s){lock(Strings){if(!Strings.TryGetValue(s,out var p)){var b=Encoding.UTF8.GetBytes(s+"\0");p=(nint)NativeMemory.Alloc((nuint)b.Length);Marshal.Copy(b,0,p,b.Length);Strings[s]=p;}return new(){Ptr=(byte*)p,Len=(nuint)Encoding.UTF8.GetByteCount(s)};}}
    private static KilnHarness? Harness=>FakeTables.Active("kiln.worldtables");
    private static PoolFakeState? State=>Harness is {} h?KitFakes.State<PoolFakeState>(h):null;
    public static nint Create()
    {
        if(_table!=null)return(nint)_table;
        var t=(WorldTablesNative.Table*)NativeMemory.AllocZeroed((nuint)sizeof(WorldTablesNative.Table));t->Size=(uint)sizeof(WorldTablesNative.Table);t->Version=1;
        t->MaterialAt=&MaterialAt_;t->TweakAt=&TweakAt_;t->CraftingPropAt=&CraftingAt_;
        t->State=&State_;t->TableHash=&Hash_;t->MaterialCount=&Count_;t->MaterialId=&Id_;t->FirstNewId=&First_;t->DefineJson=&DefineJson;t->Status=&Status_;t->PrunePending=&Prune_;
        t->TweakCount=&Count_;t->GameMaterialId=&GameId_;t->CraftingPropCount=&Count_;t->CraftingPropIndex=&Id_;t->Forget=&Unavailable_;
        t->MaterialBudget=&MaterialBudget;t->PoolBudget=&PoolBudget;t->DefinePoolResult=&DefinePool;t->PoolRegistration=&Registration;
        return(nint)(_table=t);
    }
    [UnmanagedCallersOnly(CallConvs=[typeof(CallConvCdecl)])]private static int MaterialAt_(uint i,WorldTablesNative.Material* o)=>4;
    [UnmanagedCallersOnly(CallConvs=[typeof(CallConvCdecl)])]private static int TweakAt_(uint i,WorldTablesNative.Tweak* o)=>4;
    [UnmanagedCallersOnly(CallConvs=[typeof(CallConvCdecl)])]private static int CraftingAt_(uint i,WorldTablesNative.CraftingProp* o)=>4;
    [UnmanagedCallersOnly(CallConvs=[typeof(CallConvCdecl)])]private static int State_()=>State is null?0:3;
    [UnmanagedCallersOnly(CallConvs=[typeof(CallConvCdecl)])]private static uint Count_()=>0;
    [UnmanagedCallersOnly(CallConvs=[typeof(CallConvCdecl)])]private static int Id_(byte* id)=>-1;
    [UnmanagedCallersOnly(CallConvs=[typeof(CallConvCdecl)])]private static int First_()=>-1;
    [UnmanagedCallersOnly(CallConvs=[typeof(CallConvCdecl)])]private static int GameId_(WorldTablesNative.Str name)=>-1;
    [UnmanagedCallersOnly(CallConvs=[typeof(CallConvCdecl)])]private static int Unavailable_(byte* id)=>3;
    [UnmanagedCallersOnly(CallConvs=[typeof(CallConvCdecl)])]private static WorldTablesNative.Str Hash_()=>Text(State?.Hash??WorldTablesNative.EmptyHash);
    private static int Copy(string text,byte* buffer,nuint capacity,nuint* length){var b=Encoding.UTF8.GetBytes(text);if(length==null)return 1;*length=(nuint)b.Length;if(buffer==null || capacity<=(nuint)b.Length)return 5;b.CopyTo(new Span<byte>(buffer,b.Length));buffer[b.Length]=0;return 0;}
    [UnmanagedCallersOnly(CallConvs=[typeof(CallConvCdecl)])]private static int Status_(byte* b,nuint c,nuint* n){try{var s=State;if(s is null)return 3;lock(s.Gate)return Copy(s.Status().ToJsonString(),b,c,n);}catch{return 7;}}
    private static void Capacity(KilnCapacityInfo* output,uint capacity,uint game,uint kiln,uint reserved,CapacityReadiness readiness,ulong revision)
    {
        if(readiness==CapacityReadiness.Ready && (ulong)game+kiln+reserved>capacity)readiness=CapacityReadiness.AccountingMismatch;
        uint size=output->Size;*output=KilnCapacityInfo.New();output->Size=size;output->WorldKind=uint.MaxValue;output->Revision=revision;output->Flags=2;output->Readiness=(uint)readiness;
        if(readiness==CapacityReadiness.Ready){output->Flags|=1;output->Capacity=capacity;output->GameUsed=game;output->KilnUsed=kiln;output->Reserved=reserved;}
        if(readiness==CapacityReadiness.UnknownExternalUsage)output->Flags|=16;
    }
    [UnmanagedCallersOnly(CallConvs=[typeof(CallConvCdecl)])]private static int MaterialBudget(uint category,KilnCapacityInfo* output)
    {
        if(category>1 || output==null || output->Size<48)return 1;var s=State;if(s is null || FakeTables.Off("worldtables.budgets"))return 3;
        lock(s.Gate){uint kiln=(uint)s.Materials.Values.Count(m=>m.Terrain==(category==1)&&m.Live),reserved=(uint)s.Materials.Values.Count(m=>m.Terrain==(category==1)&&!m.Live);Capacity(output,category==0?128u:127u,category==0?79u:116u,kiln,reserved,s.Readiness,s.Revision);return 0;}
    }
    [UnmanagedCallersOnly(CallConvs=[typeof(CallConvCdecl)])]private static int PoolBudget(WorldTablesNative.Str pool,KilnCapacityInfo* output)
    {
        if(output==null || output->Size<48 || !WorldTables.PoolName(pool.Text()))return 1;var s=State;if(s is null || FakeTables.Off("worldtables.budgets"))return 3;
        lock(s.Gate){var name=pool.Text();uint game=name switch {"BuildingDefaultMaterials"=>61,"BuildingRoofMaterials"=>14,"TerrainMaterials"=>56,"OvergrowthMaterials"=>5,_=>0};s.Served.TryGetValue(name,out var custom);var readiness=s.Readiness;if(game==0 && custom==null && readiness==CapacityReadiness.Ready)readiness=CapacityReadiness.NotFound;var retained=custom is not null && s.Receipts.Values.Any(r=>r.Definition==custom.Guid && r.State==DefinitionRegistrationState.Retained);Capacity(output,64,game,custom is null || retained?0:(uint)custom.Items.Length,retained?(uint)custom!.Items.Length:0,readiness,s.Revision);return 0;}
    }
    private static void Fill(KilnDefinitionReply* output,DefinitionRegistrationResult r)
    {
        uint size=output->Size;*output=KilnDefinitionReply.New();output->Size=size;output->Ok=r.Ok?1u:0;output->Kind=2;output->Disposition=(uint)r.State;output->SlotsConsumed=r.SlotsConsumed;output->ReasonCode=(uint)r.Code;output->SnapshotRevision=r.SnapshotRevision;
        DefinitionRegistration.IdBytes(r.RegistrationId).CopyTo(new Span<byte>(output->RegistrationId,16));r.Definition.TryWriteBytes(new Span<byte>(output->DefinitionGuid,16));
    }
    private static string Report(DefinitionRegistrationResult r)=>JsonSerializer.Serialize(new {reason=r.Reason,warnings=r.Warnings});
    [UnmanagedCallersOnly(CallConvs=[typeof(CallConvCdecl)])]private static int DefinePool(ulong owner,WorldTablesNative.Str name,WorldTablesNative.Str json,uint timing,KilnDefinitionReply* output,byte* b,nuint c,nuint* n)
    {
        try {
            if(owner==0 || output==null || output->Size<64 || timing>1 || n==null || !MaterialPoolValidation.Valid(name.Text()))return 1;
            var h=Harness;var s=State;if(h is null || s is null || FakeTables.Off("worldtables.material_pools"))return 3;
            var doc=JsonNode.Parse(json.Text())!.AsObject();var mod=(string)doc["mod"]!;var key=name.Text();
            var block=BlockJson.Parse<BuildingMaterialPoolBlock>(doc["buildingMaterialPools"]![key]!.ToJsonString());
            lock(s.Gate){
                var validated=s.Validate(mod,block);var r=s.Prepare(mod,key,block,(DefinitionTiming)timing,h.PoolOwnerActive(owner));var report=Report(r);
                var failure=Report(r with {Reason="could not persist the pool definition and receipt"});var required=Math.Max(Encoding.UTF8.GetByteCount(Report(r with {Reason="owner is no longer active"})),Math.Max(Encoding.UTF8.GetByteCount(report),r.State==DefinitionRegistrationState.AcceptedNextStart?Encoding.UTF8.GetByteCount(failure):0));
                *n=(nuint)required;if(b==null || c<=(nuint)required)return 5;
                r=s.Commit(r,mod,validated.Canonical);if(r.State==DefinitionRegistrationState.Registered)s.ActiveOwners[r.Definition]=owner;Fill(output,r);return Copy(Report(r),b,c,n);
            }
        }catch(ArgumentException){return 1;}catch(JsonException){return 1;}catch{return 7;}
    }
    [UnmanagedCallersOnly(CallConvs=[typeof(CallConvCdecl)])]private static int Registration(ulong owner,byte* id,KilnDefinitionReply* output,byte* b,nuint c,nuint* n)
    {
        try{if(id==null || output==null || output->Size<64 || n==null)return 1;var guid=new Guid(new ReadOnlySpan<byte>(id,16),bigEndian:true);if(guid==Guid.Empty)return 1;var s=State;if(s is null || FakeTables.Off("worldtables.material_pools"))return 3;lock(s.Gate){var r=s.Receipts.TryGetValue(guid,out var known)?known:new(false,"no material-pool registration of that id",DefinitionRefusalCode.NotFound,guid,Guid.Empty,DefinitionRegistrationKind.BuildingMaterialPool,DefinitionRegistrationState.Refused,0,s.Revision,[]);var report=Report(r);*n=(nuint)Encoding.UTF8.GetByteCount(report);if(b==null || c<=*n)return 5;if(owner!=0 && r.State==DefinitionRegistrationState.Registered && !s.ActiveOwners.ContainsKey(r.Definition)){if(!Harness!.PoolOwnerActive(owner))return 6;s.ActiveOwners[r.Definition]=owner;}Fill(output,r);return Copy(report,b,c,n);}}catch{return 7;}
    }
    [UnmanagedCallersOnly(CallConvs=[typeof(CallConvCdecl)])]private static int DefineJson(WorldTablesNative.Str json,byte* b,nuint c,nuint* n)
    {
        try {
            var s=State;if(s is null)return 3;
            var doc=JsonNode.Parse(json.Text())!.AsObject();var mod=(string)doc["mod"]!;
            var blocks=new JsonArray();var errors=new JsonArray();
            lock(s.Gate) {
                var modelled=false;
                if(doc["buildingMaterialPools"] is JsonObject pools) {
                    modelled=true;
                    foreach(var (key,value) in pools) {
                        var block=BlockJson.Parse<BuildingMaterialPoolBlock>(value!.ToJsonString());var check=s.Validate(mod,block);
                        if(check.Code!=DefinitionRefusalCode.None){errors.Add(check.Reason);continue;}
                        var r=s.Prepare(mod,key,block,DefinitionTiming.AllowNextStart,true);r=s.Commit(r,mod,check.Canonical);
                        blocks.Add(new JsonObject {["name"]=key,["kind"]="buildingMaterialPool",["status"]=r.State==DefinitionRegistrationState.Registered?"registered":"pending"});
                    }
                }
                if(doc["toolMaterialPools"] is JsonObject tools) {modelled=true;if((string?)doc["clients"]!="mod")errors.Add("toolMaterialPools: requires clients: mod");else s.DefineToolPools(mod,tools,blocks,errors);}
                if(doc["toolBrushShapes"] is JsonObject brushes) {modelled=true;if((string?)doc["clients"]!="mod")errors.Add("toolBrushShapes: requires clients: mod");else s.DefineToolBrushes(mod,brushes,blocks,errors);}
                if(!modelled)errors.Add("the test kit does not model this legacy worldtables definition");
                return Copy(new JsonObject {["errors"]=errors,["blocks"]=blocks}.ToJsonString(),b,c,n);
            }
        }catch(ArgumentException){return 1;}catch{return 7;}
    }
    [UnmanagedCallersOnly(CallConvs=[typeof(CallConvCdecl)])]private static int Prune_(WorldTablesNative.Str mod,WorldTablesNative.Str names)=>3;
}
