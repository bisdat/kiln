// SPDX-License-Identifier: MIT
#pragma warning disable KILNX028
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text.Json.Nodes;
namespace Kiln;

/// <summary>A committed material-table hash change, or a deferred pool definition. Delivered after a world's jobs at Post.
/// A deferred definition leaves OldHash and NewHash equal and consumes no live pool entries.</summary>
public sealed class MaterialTableChangedEvent : KilnEvent
{
    static MaterialTableChangedEvent()=>WorldTablesEvents.Enable();
    internal MaterialTableChangedEvent(string oldHash,string newHash,IEnumerable<Guid> groups,IEnumerable<Guid> materials,bool deferred)
    {OldHash=oldHash;NewHash=newHash;Groups=Array.AsReadOnly(groups.ToArray());Materials=Array.AsReadOnly(materials.ToArray());Deferred=deferred;}
    /// <summary>The previously observed table hash.</summary>
    public string OldHash {get;}
    /// <summary>The committed table hash; unchanged for a deferred definition.</summary>
    public string NewHash {get;}
    /// <summary>Affected pool GUIDs, copied at observation.</summary>
    public IReadOnlyList<Guid> Groups {get;}
    /// <summary>Affected material GUIDs, copied at observation.</summary>
    public IReadOnlyList<Guid> Materials {get;}
    /// <summary>A next-start definition changed; no live table capacity was consumed.</summary>
    public bool Deferred {get;}
}

// Snapshots only: no game-memory walk. A server mod carries Post; a client uses kiln.client's own Post only (never both).
// Custom boot rows need owner observation even without an API call; other observers start on pool/budget/event use.
// Per tick: one read of the service's state and revision (no allocation). The status is read and parsed only when
// those change, or once when the mods have loaded (boot rows are adopted by their loaded mods then).
internal static unsafe class WorldTablesEvents
{
    [StructLayout(LayoutKind.Sequential)] private struct ClientPrefix
    {
        public uint Size,Version;public nint State;
        public delegate* unmanaged[Cdecl]<ulong,int,delegate* unmanaged[Cdecl]<void*,void*,void>,void*,ulong*,int> Subscribe;
        public delegate* unmanaged[Cdecl]<ulong,int> Unsubscribe;
    }
    private static readonly object Gate=new();
    private static IDisposable? _tick,_bootProbe;
    private static bool _bootChecked;
    private static ModContext? _rider;
    private static readonly HashSet<Guid> DeferredSeen=[];
    private static readonly HashSet<(Guid,ulong)> Adopted=[];
    private static readonly Dictionary<Guid,DefinitionRegistrationResult> Receipts=[];
    private static readonly Dictionary<string,CapacityInfo> Budgets=[];
    private static string _hash=WorldTablesNative.EmptyHash;
    private static ClientPrefix* _client;
    private static ulong _clientHandle,_owner;
    private static (int State,ulong Revision,bool ModsLoaded) _seen=(int.MinValue,0,false);
    private static (int State,ulong Revision,bool ModsLoaded)? _faultedAt;
    // Start ownership observation for boot rows even when their mod never calls a pool API.
    // One bounded metadata snapshot decides this; no custom pool leaves no continuing pool observer.
    internal static void WatchBoot()
    {
        lock(Gate) {
            if(_bootChecked)return;
            ProbeBoot();
            if(!_bootChecked)_bootProbe??=ServiceTick.Add("worldtables boot pool owners",static(in TickInfo _)=>ProbeBoot());
        }
    }
    private static void ProbeBoot()
    {
        lock(Gate) {
            if(_bootChecked)return;
            var state=WorldTablesNative.State;
            if(state==WorldTablesNative.Waiting)return;
            bool pools=false;
            try {
                if(state is WorldTablesNative.Merged or WorldTablesNative.Untouched)
                    pools=(JsonNode.Parse(WorldTables.StatusJson()) as JsonObject)?["customPools"] is JsonArray {Count:>0};
            } catch(Exception e) {BlocksBridge.Log(LogLevel.Error,"worldtables boot owner observation failed: "+e.Message);}
            _bootChecked=true;_bootProbe?.Dispose();_bootProbe=null;
            if(pools)Enable();
        }
    }
    internal static void Enable()
    {
        lock(Gate) {
            if(_clientHandle==0 && Mods.Side==ModSide.Client) {
                _client=(ClientPrefix*)BlocksBridge.ServiceTable("kiln.client",1);
                if(_client!=null && _client->Size>=32 && _client->Subscribe!=null) {
                    if(_owner==0)_owner=Backend.Current.OwnerCreate("kiln.worldtables.events");
                    if(_owner!=0){ulong handle=0;if(_client->Subscribe(_owner,1,&ClientPost,null,&handle)==0)_clientHandle=handle;}
                }
            }
            // A client polls from kiln.client's Post alone: a mod's Post as well would poll twice a step. Elsewhere a loaded
            // mod's Post carries it; until one is there, Kiln's service tick looks for one (and stops once it rides).
            if(_clientHandle!=0 || _rider is {Alive:true})return;
            _tick??=ServiceTick.Add("worldtables changes",static(in TickInfo _)=>Attach());
        }
        if(Game.IsTickThread)Attach();
    }
    private static void Attach()
    {
        lock(Gate) { if(_clientHandle!=0 || _rider is {Alive:true})return; }
        var current=ModContext.CurrentMod;
        var candidates=current is {Alive:true}?new[]{current}:Mods.All.Where(m=>m.IsLoaded && m.Info!=null).Select(m=>ModContext.FindLive(m.Info!)).Where(m=>m is {Alive:true}).ToArray();
        lock(Gate) {
            if(_clientHandle!=0 || _rider is {Alive:true})return;
            if(_rider!=null)_rider.Tick.Post-=ServerPost;
            _rider=candidates.FirstOrDefault();
            if(_rider is {} rider) {
                rider.Tick.Post+=ServerPost;
                _tick?.Dispose();_tick=null;
                rider.Retiring+=()=>{lock(Gate){if(ReferenceEquals(_rider,rider)){rider.Tick.Post-=ServerPost;_rider=null;_tick??=ServiceTick.Add("worldtables changes",static(in TickInfo _)=>Attach());}}};
            }
        }
    }
    private static void ServerPost(in TickInfo _)=>Poll();
    [UnmanagedCallersOnly(CallConvs=[typeof(CallConvCdecl)])]
    private static void ClientPost(void* tick,void* user)=>Poll();
    internal static void Poll()
    {
        var key=(WorldTablesNative.State,WorldTablesNative.Revision(),Lifecycle.ModsLoaded);
        lock(Gate) {
            if(key==_seen || key==_faultedAt)return;
            try {
                var doc=JsonNode.Parse(WorldTablesNative.StatusJson()) as JsonObject;
                _seen=key;_faultedAt=null;
                if(doc is null || (string?)doc["state"] is not ("merged" or "untouched"))return;
                var hash=(string?)doc["hash"]??WorldTablesNative.EmptyHash;
                var groups=(doc["customPools"] as JsonArray??[]).Select(n=>Guid.Parse((string)n!["guid"]!)).ToArray();
                if(hash!=_hash) {
                    var previous=_hash;_hash=hash;
                    if(Events.HasSubscribers<MaterialTableChangedEvent>())Events.Post(new MaterialTableChangedEvent(previous,hash,groups,WorldTables.Materials.Where(m=>m.Live).Select(m=>m.Guid),false));
                }
                foreach(var n in doc["poolRegistrations"] as JsonArray??[]) {
                    var id=new Guid(Convert.FromHexString((string)n!["registration"]!),bigEndian:true);
                    var state=(DefinitionRegistrationState)(uint)n["state"]!;var code=(DefinitionRefusalCode)(uint)n["code"]!;
                    if(state==DefinitionRegistrationState.Registered) {
                        var mod=Mods.All.FirstOrDefault(m=>m.Id==(string)n["mod"]! && m.IsLoaded);
                        var context=mod?.Info is {} info?ModContext.FindLive(info):null;
                        if(context is {Alive:true} && Adopted.Add((id,context.Owner)))WorldTablesNative.AdoptRegistration(id,context.Owner);
                    }
                    var now=new DefinitionRegistrationResult(code==DefinitionRefusalCode.None,(string?)n["reason"],code,id,Guid.Parse((string)n["definition"]!),DefinitionRegistrationKind.BuildingMaterialPool,state,(uint)n["slots"]!,(ulong)n["revision"]!,[]);
                    Receipts.TryGetValue(id,out var before);
                    if(before is not null && before.State==now.State && before.Code==now.Code && before.SlotsConsumed==now.SlotsConsumed && before.Reason==now.Reason)continue;
                    Receipts[id]=now;
                    if(Events.HasSubscribers<DefinitionRegistrationChangedEvent>())Events.Post(new DefinitionRegistrationChangedEvent((string)n["mod"]!,now,before?.State,before?.SlotsConsumed??0,null));
                    if(state==DefinitionRegistrationState.AcceptedNextStart && DeferredSeen.Add(id) && Events.HasSubscribers<MaterialTableChangedEvent>())Events.Post(new MaterialTableChangedEvent(hash,hash,[now.Definition],[],true));
                }
                // the snapshots themselves, without Enable(): this is the observer
                if(WorldTablesNative.BudgetsReadable) try {
                    Observe("building",WorldTablesNative.MaterialBudget(WorldMaterialCategory.Building).Budget);
                    Observe("terrain",WorldTablesNative.MaterialBudget(WorldMaterialCategory.Terrain).Budget);
                    foreach(var n in doc["pools"] as JsonArray??[]) {var name=(string)n!["name"]!;Observe("pool/"+name,WorldTablesNative.PoolBudget(name).Budget);}
                    foreach(var n in doc["customPools"] as JsonArray??[]) {var name=(string)n!["name"]!;Observe("pool/"+name,WorldTablesNative.PoolBudget(name).Budget);}
                } catch(KilnUnavailableException) {}  // budgets off on this build: the capability rows say why
            } catch(Exception e) {
                // the next change reads again: a status caught mid-change, or a bad answer, is not the end of notifications
                _faultedAt=key;BlocksBridge.Log(LogLevel.Error,"worldtables change notifications missed a change (they read again at the next one): "+e.Message);
            }
        }
    }
    private static void Observe(string name,CapacityInfo current)
    {
        if(Budgets.TryGetValue(name,out var previous) && previous with {Revision=0}==current with {Revision=0})return;
        previous??=new(0,0,0,0,CapacityReadiness.NotReady,false,CapacityScope.Process,false,null,0);
        Budgets[name]=current;if(Events.HasSubscribers<CapacityChangedEvent>())Events.Post(new CapacityChangedEvent("worldtables.budgets/"+name,previous,current));
    }
    internal static void Reset()
    {
        lock(Gate) {
            if(_rider!=null)_rider.Tick.Post-=ServerPost;_rider=null;
            _tick?.Dispose();_tick=null;_bootProbe?.Dispose();_bootProbe=null;_bootChecked=false;
            if(_clientHandle!=0 && _client!=null && _client->Unsubscribe!=null)_client->Unsubscribe(_clientHandle);
            _clientHandle=0;_client=null;_owner=0;_seen=(int.MinValue,0,false);_faultedAt=null;
            Receipts.Clear();Budgets.Clear();DeferredSeen.Clear();Adopted.Clear();_hash=WorldTablesNative.EmptyHash;
        }
    }
}
