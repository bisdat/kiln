#pragma warning disable KILNX028
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json.Nodes;
namespace Kiln;

/// <summary>The byte material namespaces. This is separate from shader MaterialKind.</summary>
public enum WorldMaterialCategory : uint
{
    /// <summary>Building cell bytes 128..255: 128 slots, including the game's usage.</summary>
    Building=0,
    /// <summary>Nonair terrain cell bytes 1..127: 127 slots.</summary>
    Terrain=1
}

public static partial class WorldTables
{
    /// <summary>Reads an immutable process snapshot. Building has 128 slots; terrain has 127 nonair slots. Pending definitions consume no current room.</summary>
    [AnyThread,RequiresCapability("worldtables.budgets")]
    public static CapacityQueryResult GetMaterialBudget(WorldMaterialCategory category)
    {
        if(category is not WorldMaterialCategory.Building and not WorldMaterialCategory.Terrain)throw new ArgumentOutOfRangeException(nameof(category));
        var result=WorldTablesNative.MaterialBudget(category);WorldTablesEvents.Enable();return result;
    }
    /// <summary>Reads one 64-entry pool's process snapshot. Use a built-in debug name or mod:key. Unknown names return NotFound.</summary>
    [AnyThread,RequiresCapability("worldtables.budgets")]
    public static CapacityQueryResult GetPoolBudget(string pool)
    {
        if(!PoolName(pool))throw new ArgumentException("Expected a pool debug name or mod:key.",nameof(pool));
        var result=WorldTablesNative.PoolBudget(pool);WorldTablesEvents.Enable();return result;
    }
    /// <summary>Reads a receipt without waiting. Registered means the canonical boot table contains the pool; it does not prove a rendered menu.</summary>
    [AnyThread,RequiresCapability("worldtables.material_pools")]
    public static DefinitionRegistrationResult GetMaterialPoolRegistration(Guid registrationId)
    {
        if(registrationId==Guid.Empty)throw new ArgumentException("An empty registration ID is invalid.",nameof(registrationId));
        var result=WorldTablesNative.Registration(registrationId);WorldTablesEvents.Enable();return result;
    }
    internal static bool PoolName(string? text)=>text is {Length:>0} && text.Split(':') is var parts && parts.Length<=2 && parts.All(p=>MaterialPoolValidation.Valid(p));
}

internal static unsafe partial class WorldTablesNative
{
    internal static void ReportBudgetRows()
    {
        // Native reports resolved/missing executable inputs. Managed only supplies the missing/older fallback.
        // Never infer availability from an ABI pointer or overwrite a native layout refusal.
        var t=T;
        bool Missing(uint offset)=>t==null || !HasAppend(t->Size,offset) || *(nint*)((byte*)t+offset)==0;
        var budgets=Missing(144) || Missing(152);
        var pools=Missing(160) || Missing(168);
        if(!pools && t!=null && t->State!=null && t->Status!=null)WorldTablesEvents.WatchBoot();
        if(!budgets && !pools)return;
        ServiceCaps.Probe(Name);
        var reason=t==null?CapabilityReason.ServiceMissing:CapabilityReason.ServiceOlder;
        var text=t==null?"kiln.worldtables is not here: no native boot table service":"this kiln.worldtables has no appended budgets or material-pool registration members: update Kiln";
        foreach(var (name,missing) in new[]{("worldtables.budgets",budgets),("worldtables.material_pools",pools)})
            if(missing && Capabilities.Get(name).Reason==CapabilityReason.Unknown)ServiceCaps.Static(name,false,reason,text,Name);
    }
    internal static bool HasAppend(uint size,uint offset)=>size>=offset+8;
    internal static void RequirePoolWriter()
    {
        if(Baking || TestDefine is not null)return;
        _=RequireAppend(160,"worldtables.material_pools");
    }
    private static Table* RequireAppend(uint offset,string capability)
    {
        ReportBudgetRows();
        var t=T;
        if(t==null || !HasAppend(t->Size,offset) || *(nint*)((byte*)t+offset)==0)throw ServiceCaps.Unavailable(capability,"this worldtables table does not provide the appended member");
        return t;
    }
    // report: what native said about a bad argument (an author's duplicate or roof disagreement names its entries)
    internal static void ResultCode(int rc,string capability,string? report=null)
    {
        if(rc==3)throw ServiceCaps.Unavailable(capability);
        if(rc==1)throw new ArgumentException(string.IsNullOrEmpty(report)?"The worldtables request is malformed.":report);
        if(rc!=0)throw new InvalidOperationException($"worldtables returned {rc}");
    }
    internal static CapacityQueryResult MaterialBudget(WorldMaterialCategory category)
    {
        var t=RequireAppend(144,"worldtables.budgets");var reply=KilnCapacityInfo.New();
        ResultCode(t->MaterialBudget((uint)category,&reply),"worldtables.budgets");
        return CapacityMapping.ToResult(reply);
    }
    internal static CapacityQueryResult PoolBudget(string pool)
    {
        var t=RequireAppend(152,"worldtables.budgets");var reply=KilnCapacityInfo.New();var bytes=Encoding.UTF8.GetBytes(pool);
        fixed(byte* p=bytes)ResultCode(t->PoolBudget(new Str {Ptr=p,Len=(nuint)bytes.Length},&reply),"worldtables.budgets");
        return CapacityMapping.ToResult(reply);
    }
    internal static DefinitionRegistrationResult DefinePool(ulong owner,string name,string document,DefinitionTiming timing)=>PoolCall(owner,name,document,timing,Guid.Empty);
    internal static void AdoptRegistration(Guid id,ulong owner)=>_ = PoolCall(owner,"","",DefinitionTiming.RequireRegistered,id);
    internal static DefinitionRegistrationResult Registration(Guid id)=>PoolCall(0,"","",DefinitionTiming.RequireRegistered,id);
    private static DefinitionRegistrationResult PoolCall(ulong owner,string name,string document,DefinitionTiming timing,Guid id)
    {
        const string capability="worldtables.material_pools";
        var query=id!=Guid.Empty;var t=RequireAppend(query?168u:160u,capability);
        var n=Encoding.UTF8.GetBytes(name);var d=Encoding.UTF8.GetBytes(document);
        var registration=query?DefinitionRegistration.IdBytes(id):new byte[16];
        var buffer=new byte[1024];
        for(int attempt=0;attempt<4;++attempt) {
            var reply=KilnDefinitionReply.New();nuint length=0;int rc;
            fixed(byte* pn=n,pd=d,pi=registration,pr=buffer) {
                rc=query?t->PoolRegistration(owner,pi,&reply,pr,(nuint)buffer.Length,&length)
                    :t->DefinePoolResult(owner,new Str {Ptr=pn,Len=(nuint)n.Length},new Str {Ptr=pd,Len=(nuint)d.Length},(uint)timing,&reply,pr,(nuint)buffer.Length,&length);
            }
            if(rc==5 && length<(16u<<20)){buffer=new byte[checked((int)length+1)];continue;}
            ResultCode(rc,capability,rc==1 && length>0 && length<(nuint)buffer.Length?Encoding.UTF8.GetString(buffer,0,(int)length):null);
            if(length>=(nuint)buffer.Length)throw new InvalidOperationException("worldtables report length exceeded its buffer");
            return DefinitionRegistration.ToResult(reply,Encoding.UTF8.GetString(buffer,0,(int)length));
        }
        throw new InvalidOperationException("worldtables report did not settle after buffer growth");
    }
}

internal static partial class Compiler
{
    private static void RecordPoolHandle(ModBlocks mod,string name,BuildingMaterialPoolBlock block,JsonObject node,TableBlockStatus status)
    {
        mod.Handles[name]=new TableBlockHandle<BuildingMaterialPoolBlock>(mod.Id,name,BlockIds.Derive(mod.Id,"materialpool",block.Was??name),mod.Id+":"+name,block,status,-1);
        mod.Lowered[name]=new JsonObject {["materialPool"]=node.DeepClone(),["status"]=status==TableBlockStatus.Registered?"registered":"pending"};
        mod.ReportDirty=true;
    }
    internal static DefinitionRegistrationResult DefinePoolResult(ModBlocks mod,string name,BuildingMaterialPoolBlock block,DefinitionRegistrationOptions options)
    {
        ArgumentNullException.ThrowIfNull(options);
        if(options.Timing is not DefinitionTiming.RequireRegistered and not DefinitionTiming.AllowNextStart)throw new ArgumentOutOfRangeException(nameof(options));
        ArgumentException.ThrowIfNullOrEmpty(name);
        if(!MaterialPoolValidation.Valid(name))throw new ArgumentException("Invalid pool key.",nameof(name));
        var node=MaterialPoolNode(mod.Id,block);
        mod.ManifestClients??=BlocksBridge.ManifestClients(mod.Id);
        if(mod.ManifestClients!="mod")throw new ArgumentException("Material pools require clients: mod.",nameof(block));
        var doc=new JsonObject {["format"]=1,["mod"]=mod.Id,["clients"]="mod",["buildingMaterialPools"]=new JsonObject {[name]=node}};
        if(WorldTablesNative.Baking) {
            WorldTablesNative.DefineJson(doc.ToJsonString());
            RecordPoolHandle(mod,name,block,node,TableBlockStatus.Pending);
            // Bake records an artifact, never a live registration or a promise of durable profile storage.
            return new(false,"offline bake recorded an artifact; no game registry is present",DefinitionRefusalCode.NotReady,Guid.Empty,BlockIds.Derive(mod.Id,"materialpool",block.Was??name),DefinitionRegistrationKind.BuildingMaterialPool,DefinitionRegistrationState.Refused,0,0,[]);
        }
        var result=WorldTablesNative.DefinePool(mod.Owner,name,doc.ToJsonString(),options.Timing);
        if(result.Ok)RecordPoolHandle(mod,name,block,node,result.State==DefinitionRegistrationState.Registered?TableBlockStatus.Registered:TableBlockStatus.Pending);
        WorldTablesEvents.Enable();
        return result;
    }
}
