// SPDX-License-Identifier: MIT
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
namespace Kiln.TestKit;

internal sealed record CraftSourceEntry(ulong Owner, ulong Handle, uint Player, uint Container, uint Order, bool Deny, uint[] Items, uint PlayerSession)
{
    internal bool Allows(uint item) => Deny ? !Items.Contains(item) : Items.Contains(item);
}
internal sealed class CraftingState
{
    internal readonly object Gate=new();
    internal ulong Next;
    internal readonly List<CraftSourceEntry> Sources=[];
}

// Only the appended linked-source members are modelled. Older station operations explicitly refuse.
internal sealed unsafe class CraftingFake : IKitFake
{
    public string Service=>StationsNative.Name;
    private static StationsNative.Table* _table;
    private static KilnHarness? Harness=>FakeTables.Active(StationsNative.Name);
    private static CraftingState? State=>Harness is {} h ? KitFakes.State<CraftingState>(h) : null;
    public void Register()
    {
        if(_table==null)
        {
            _table=(StationsNative.Table*)NativeMemory.AllocZeroed((nuint)sizeof(StationsNative.Table));
            _table->Header=new ServiceHeader {Size=(uint)sizeof(StationsNative.Table),Version=1};
            var slots=(nint*)((byte*)_table+8);
            for(var i=0;i<30;i++) slots[i]=(nint)(delegate* unmanaged[Cdecl]<int>)&Unavailable;
            _table->CraftItems=&CraftItems;
            _table->AddSource=&Add;_table->RemoveSource=&Remove;_table->CancelSources=&Cancel;_table->SourceStock=&Stock;
        }
        KitBackend.Instance.RegisterTable(Service,(nint)_table);
        FakeTables.Features([Crafting.SourcesCapability]);
        FakeTables.Offline(Crafting.SourceUiCapability,"the test kit has no game client crafting screens");
    }
    public void Start(KilnHarness harness) { }
    public void BeforeStep(KilnHarness harness,ulong step) { }
    public void End(KilnHarness harness) {var s=KitFakes.State<CraftingState>(harness);lock(s.Gate)s.Sources.Clear();}
    public void AddCoverage(List<CoverageRow> rows)=>rows.Add(new CoverageRow("Player crafting sources",CoverageKind.RecordingFake,
        [StationsNative.Name,Crafting.SourcesCapability],"Player/container registrations, copied allow/deny filters, order metadata, per-player counts, inventory permission, disposal and mod unload. No game RecipePaid hook, base stock, station outputs or client screens are simulated."));
    [UnmanagedCallersOnly(CallConvs=[typeof(CallConvCdecl)])]
    private static int Unavailable()=>FakeTables.Unavailable;
    [UnmanagedCallersOnly(CallConvs=[typeof(CallConvCdecl)])]
    private static int CraftItems(uint* items,uint n,uint* output,uint capacity,uint* count)
    {
        if(count==null || (n!=0 && items==null) || (capacity!=0 && output==null))return FakeTables.InvalidArgument;
        *count=n;if(capacity<n)return FakeTables.BufferTooSmall;
        for(uint i=0;i<n;i++)output[i]=items[i];
        return FakeTables.Ok; // no game recipe catalog: only the supplied lent items are modelled
    }

    [UnmanagedCallersOnly(CallConvs=[typeof(CallConvCdecl)])]
    private static int Add(ulong owner,StationsNative.SourceRequest* r,ulong* handle)
    {
        try
        {
            if(handle==null)return FakeTables.InvalidArgument;*handle=0;
            if(r==null || r->Size<sizeof(StationsNative.SourceRequest) || r->Player is 0 or >1023 || r->Container<=1023 || r->Order>1 || r->Filter>1 || r->ItemCount>65536 || (r->ItemCount!=0 && r->Items==null))return FakeTables.InvalidArgument;
            if(!FakeTables.OnTickThread)return FakeTables.WrongThread;
            if(FakeTables.Off(Crafting.SourcesCapability) || State is not {} s)return FakeTables.Unavailable;
            if(Items.Read(new EntityId(r->Container)) is not {Initialized:true,IsPlayer:false,IsNpc:false})return FakeTables.InvalidArgument;
            if(Players.Find(new EntityId(r->Player)) is not {InGame:true} player)return FakeTables.InvalidArgument;
            var items=new ReadOnlySpan<uint>(r->Items,(int)r->ItemCount).ToArray().Distinct().Order().ToArray();
            if(items.Contains(0u))return FakeTables.InvalidArgument;
            lock(s.Gate)
            {
                if(s.Sources.Count>=4096)return FakeTables.BufferTooSmall;
                *handle=++s.Next;s.Sources.Add(new(owner,*handle,r->Player,r->Container,r->Order,r->Filter==0,items,player.Handle));
            }
            return FakeTables.Ok;
        }
        catch{return FakeTables.Internal;}
    }
    [UnmanagedCallersOnly(CallConvs=[typeof(CallConvCdecl)])]
    private static int Remove(ulong owner,ulong handle)
    {
        try {if(State is not {} s)return FakeTables.NotFound;lock(s.Gate)return s.Sources.RemoveAll(x=>x.Owner==owner && x.Handle==handle)>0?FakeTables.Ok:FakeTables.NotFound;}
        catch{return FakeTables.Internal;}
    }
    [UnmanagedCallersOnly(CallConvs=[typeof(CallConvCdecl)])]
    private static void Cancel(ulong owner)
    {
        try {if(State is {} s)lock(s.Gate)s.Sources.RemoveAll(x=>x.Owner==owner);}catch{ }
    }
    [UnmanagedCallersOnly(CallConvs=[typeof(CallConvCdecl)])]
    private static int Stock(uint player,uint flags,StationsNative.Stack* rows,uint capacity,uint* count)
    {
        try
        {
            if(count==null || (capacity!=0 && rows==null) || player is 0 or >1023 || (flags & ~1u)!=0)return FakeTables.InvalidArgument;*count=0;
            if(!FakeTables.OnTickThread)return FakeTables.WrongThread;
            if(FakeTables.Off(Crafting.SourcesCapability) || State is not {} s)return FakeTables.Unavailable;
            var stock=Counts(s,player);*count=(uint)stock.Count;
            if(capacity<*count)return FakeTables.BufferTooSmall;
            var i=0;foreach(var (item,n) in stock.OrderBy(x=>x.Key))rows[i++]=new(){Item=item,Count=(uint)Math.Min(n,uint.MaxValue)};
            return FakeTables.Ok;
        }
        catch{return FakeTables.Internal;}
    }
    internal static Dictionary<uint,long> Counts(CraftingState state,uint player)
    {
        var result=new Dictionary<uint,long>();
        if(Players.Find(new EntityId(player)) is not {InGame:true,PermissionsKnown:true} who || !who.Can(PlayerPermissions.AccessInventories))return result;
        CraftSourceEntry[] sources;lock(state.Gate)sources=[..state.Sources.Where(s=>s.Player==player && s.PlayerSession==who.Handle)];
        var seen=new HashSet<(uint,uint)>();
        foreach(var source in sources)
            if(Items.Read(new EntityId(source.Container)) is {Initialized:true,IsPlayer:false,IsNpc:false} inv)
                foreach(var group in inv.Slots.Where(x=>!x.IsEmpty).GroupBy(x=>x.Item.Value))
                    if(source.Allows(group.Key) && seen.Add((source.Container,group.Key)))result[group.Key]=result.GetValueOrDefault(group.Key)+group.Sum(x=>(long)x.Count);
        return result;
    }
}
