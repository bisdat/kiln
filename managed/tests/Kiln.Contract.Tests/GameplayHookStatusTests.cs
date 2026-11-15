// SPDX-License-Identifier: MIT
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using Xunit;

namespace Kiln.Contract.Tests;

public sealed unsafe class GameplayHookStatusTests : FakeTableTest
{
    private static int _requests, _requestResult;
    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Request(ulong owner) { ++_requests; return _requestResult; }

    [Theory]
    [InlineData(0)]
    [InlineData(3)]
    public void Direct_build_subscription_requests_hooks_without_waiting_for_a_tick_and_refuses_by_capability(int result)
    {
        _requests = 0; _requestResult = result;
        var size = sizeof(GameplayNative.Table) + 5 * sizeof(nint);
        var t = (GameplayNative.Table*)NativeMemory.AllocZeroed((nuint)size);
        t->Header = new ServiceHeader { Size = (uint)size, Version = 1 };
        t->Status = &Status;
        ((nint*)((byte*)t + sizeof(GameplayNative.Table)))[4] = (nint)(delegate* unmanaged[Cdecl]<ulong, int>)&Request;
        Swap(typeof(Gameplay), "_listening", 1);
        Serve("kiln.gameplay", t, typeof(GameplayNative), "_table");
        ServiceCaps.Static("gameplay.build_hooks", true, CapabilityReason.Faulted, "test", "kiln.gameplay");
        if (result == 0) { using var sub = Events.On<BuildEvent>(_ => { }); }
        else Assert.Throws<KilnUnavailableException>(() => Events.On<BuildEvent>(_ => { }));
        Assert.Equal(1, _requests);
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int RefuseZone(ulong owner, GameplayNative.Zone* zone, uint* id) {
        if (!_allowZoneAdd) return 3;
        *id=42; return 0;
    }
    private static bool _allowZoneAdd;
    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int RefuseUpdate(uint id, GameplayNative.Zone* zone) => 3;
    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int RemoveZone(uint id) => 0;

    [Theory]
    [InlineData(false,136,true)] [InlineData(true,136,true)]
    [InlineData(false,144,true)] [InlineData(true,144,true)]
    [InlineData(false,144,false)] [InlineData(true,144,false)]
    public void Protect_waiting_names_the_actual_install_policy_including_older_tables(bool update,int size,bool waitForStop)
    {
        _state = 2; _legacy = false; _allowZoneAdd=update; _stopWait=waitForStop;
        var t = (GameplayNative.Table*)NativeMemory.AllocZeroed((nuint)size);
        t->Header = new ServiceHeader { Size = (uint)size, Version = 1 };
        if(size==144) *(nint*)((byte*)t+136)=(nint)(delegate* unmanaged[Cdecl]<ulong,uint,int>)&RequestGroups;
        t->Status = &Status; t->ZoneAdd = &RefuseZone; t->ZoneUpdate = &RefuseUpdate; t->ZoneRemove = &RemoveZone;
        Swap(typeof(Gameplay), "_listening", 1);
        Serve("kiln.gameplay", t, typeof(GameplayNative), "_table");
        using var handle=update ? Gameplay.Protect(new ProtectionZone { Deny=GameplayActions.Terraform }) : null;
        var e = Assert.Throws<KilnUnavailableException>(() => {
            if (handle != null) handle.Update(new ProtectionZone { Deny=GameplayActions.Build });
            else Gameplay.Protect(new ProtectionZone { Deny=GameplayActions.Build });
        });
        if (handle != null) Assert.Equal(GameplayActions.Terraform,handle.Zone.Deny);
        Assert.Contains("gameplay.build_hooks", e.Message);
        Assert.Contains(waitForStop ? "build protection begins with the next world: this mod asked for it while a world was running" :
            "build protection is waiting for checked installation at a safe Pre", e.Message);
    }

    // Refused three times for unchecked threads during play: the group waits for the next no-world moment, and
    // protection says so instead of promising the next safe Pre.
    [Fact]
    public void Protect_waiting_for_the_next_no_world_moment_says_so()
    {
        _state = 2; _legacy = false; _allowZoneAdd=false; _stopWait=false; _noWorldWait=true;
        try
        {
            var t = (GameplayNative.Table*)NativeMemory.AllocZeroed((nuint)144);
            t->Header = new ServiceHeader { Size = 144, Version = 1 };
            *(nint*)((byte*)t+136)=(nint)(delegate* unmanaged[Cdecl]<ulong,uint,int>)&RequestGroups;
            t->Status = &Status; t->ZoneAdd = &RefuseZone; t->ZoneUpdate = &RefuseUpdate; t->ZoneRemove = &RemoveZone;
            Swap(typeof(Gameplay), "_listening", 1);
            Serve("kiln.gameplay", t, typeof(GameplayNative), "_table");
            var e = Assert.Throws<KilnUnavailableException>(() => Gameplay.Protect(new ProtectionZone { Deny=GameplayActions.Break }));
            Assert.Contains("gameplay.build_hooks", e.Message);
            Assert.Contains("build protection waits for the next no-world moment: waiting for the next no-world moment (the next server start)", e.Message);
            Assert.DoesNotContain("at a safe Pre", e.Message);
        }
        finally { _noWorldWait=false; }
    }

    private static uint _state;
    private static bool _legacy, _stopWait, _noWorldWait;

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Status(GameplayNative.Status* s)
    {
        s->BuildHooks = _state == 1 ? (byte)1 : (byte)0;
        s->EntityHooks = 1;
        s->DiscardOn = 1;
        s->BuildHookState = _state;
        if(_stopWait) { var why="build hooks wait for the world to stop: asked while a world ran"u8;
            for(int i=0;i<why.Length;++i) s->BuildHookReason[i]=why[i]; }
        if(_noWorldWait) { var why="waiting for the next no-world moment (the next server start): gameplay/DismantleBlock thread check refused on three attempts during play; installed pass-through targets 2"u8;
            for(int i=0;i<why.Length;++i) s->BuildHookReason[i]=why[i]; }
        if (_legacy) s->Size = (uint)Marshal.OffsetOf<GameplayNative.Status>(nameof(GameplayNative.Status.BuildHookState));
        return 0;
    }

    [Theory]
    [InlineData(0u, false)]
    [InlineData(1u, false)]
    [InlineData(2u, false)]
    [InlineData(3u, false)]
    [InlineData(0u, true)]
    public void Hook_boolean_means_installed_and_older_status_keeps_its_entity_and_discard_fields(uint state, bool legacy)
    {
        _state = state; _legacy = legacy;
        var t = (GameplayNative.Table*)NativeMemory.AllocZeroed((nuint)sizeof(GameplayNative.Table));
        t->Header = new ServiceHeader { Size = (uint)sizeof(GameplayNative.Table), Version = 1 };
        t->Status = &Status;
        Swap(typeof(Gameplay), "_listening", 1);
        Serve("kiln.gameplay", t, typeof(GameplayNative), "_table");
        var s = Gameplay.Status!;
        Assert.Equal(state == 1, s.BuildHooks);
        Assert.True(s.EntityHooks);
        Assert.True(s.Discard);
    }
    private static uint _groups;
    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int RequestGroups(ulong owner,uint groups) { _groups=groups; return _requestResult; }
    [Theory]
    [InlineData(2u)] [InlineData(4u)] [InlineData(8u)] [InlineData(16u)]
    public void Every_group_uses_the_appended_request_and_returns_refusal(uint groups)
    {
        var t=(GameplayNative.Table*)NativeMemory.AllocZeroed(144); t->Header=new ServiceHeader { Size=144,Version=1 };
        ((nint*)((byte*)t+136))[0]=(nint)(delegate* unmanaged[Cdecl]<ulong,uint,int>)&RequestGroups;
        Serve("kiln.gameplay",t,typeof(GameplayNative),"_table"); Swap(typeof(Gameplay),"_listening",1);
        _groups=0; _requestResult=0; Assert.Null(Gameplay.PrepareHookEvents(groups)); Assert.Equal(groups,_groups);
        _requestResult=3; Assert.NotNull(Gameplay.PrepareHookEvents(groups));
        t->Header.Size=136; _groups=0; Assert.Null(Gameplay.PrepareHookEvents(groups)); Assert.Equal(0u,_groups);
    }
    [Fact]
    public void Gameplay_group_ABI_is_append_only()
    {
        Assert.Equal(96,sizeof(GameplayNative.Table));
        Assert.Equal(128,sizeof(GameplayNative.Table)+4*sizeof(nint)); // request_build
        Assert.Equal(136,sizeof(GameplayNative.Table)+5*sizeof(nint)); // request_hooks
        Assert.Equal(648,Marshal.OffsetOf<GameplayNative.Status>(nameof(GameplayNative.Status.HookGroups)).ToInt32());
        Assert.Equal(2728,sizeof(GameplayNative.Status));
    }

    [Fact]
    public void Entity_watcher_itself_requests_the_entity_group()
    {
        var t=(GameplayNative.Table*)NativeMemory.AllocZeroed(144); t->Header=new ServiceHeader { Size=144,Version=1 };
        ((nint*)((byte*)t+136))[0]=(nint)(delegate* unmanaged[Cdecl]<ulong,uint,int>)&RequestGroups;
        Serve("kiln.gameplay",t,typeof(GameplayNative),"_table"); Swap(typeof(Gameplay),"_listening",1);
        _groups=0; _requestResult=0; using var watch=Gameplay.WatchEntities(EntityCategories.Default); Assert.Equal(16u,_groups);
    }
    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Subscribe(ulong owner,uint mask,delegate* unmanaged[Cdecl]<GameplayNative.Event*,void*,uint> fn,void* user,ulong* handle) { *handle=1; return 0; }
    [Theory]
    [InlineData("ItemCraftedEvent",2u,0,0)] [InlineData("NaturalSpawnEvent",8u,2,0)]
    [InlineData("EntityJoinLevelEvent",16u,3,0)] [InlineData("EntityLeaveLevelEvent",16u,3,0)]
    [InlineData("ItemCraftedEvent",2u,0,3)] [InlineData("NaturalSpawnEvent",8u,2,3)]
    [InlineData("EntityJoinLevelEvent",16u,3,3)] [InlineData("EntityLeaveLevelEvent",16u,3,3)]
    public void A_new_direct_event_subscription_requests_before_the_next_tick(string type,uint group,int row,int result)
    {
        _groups=0; _requestResult=0;
        var t=(GameplayNative.Table*)NativeMemory.AllocZeroed(144); t->Header=new ServiceHeader { Size=144,Version=1 };
        t->Subscribe=&Subscribe; t->Status=&Status;
        ((nint*)((byte*)t+136))[0]=(nint)(delegate* unmanaged[Cdecl]<ulong,uint,int>)&RequestGroups;
        var kiln=new IsolatedKiln(["Kiln.Gameplay"]); kiln.Serve("kiln.gameplay",(nint)t); kiln.Step();
        var capState=Enum.Parse(kiln.Type("Kiln.CapabilityState"),"Pending");
        var capReason=Enum.Parse(kiln.Type("Kiln.CapabilityReason"),"Session");
        kiln.Type("Kiln.ServiceCaps").GetMethod("Report",System.Reflection.BindingFlags.Public|System.Reflection.BindingFlags.Static)!
            .Invoke(null,[Gameplay.HookRows[row],capState,capReason,"waiting"]);
        kiln.Subscribe(kiln.Services["Kiln.Gameplay"].GetType("Kiln.CraftEvent")!); // listener already active
        _groups=0; _requestResult=result;
        if(result==0) kiln.Subscribe(kiln.Services["Kiln.Gameplay"].GetType("Kiln."+type)!);
        else {
            var exception=Assert.Throws<System.Reflection.TargetInvocationException>(()=>kiln.Subscribe(kiln.Services["Kiln.Gameplay"].GetType("Kiln."+type)!));
            Assert.Equal("KilnUnavailableException",exception.InnerException!.GetType().Name);
            Assert.Contains(Gameplay.HookRows[row],exception.InnerException.Message);
        }
        Assert.Equal(group,_groups);
    }

    [Fact]
    public void Build_events_prepare_terraform_as_well_as_building_pieces()
    {
        var t=(GameplayNative.Table*)NativeMemory.AllocZeroed(144); t->Header=new ServiceHeader { Size=144,Version=1 };
        *(nint*)((byte*)t+128)=(nint)(delegate* unmanaged[Cdecl]<ulong,int>)&Request;
        *(nint*)((byte*)t+136)=(nint)(delegate* unmanaged[Cdecl]<ulong,uint,int>)&RequestGroups;
        Serve("kiln.gameplay",t,typeof(GameplayNative),"_table"); Swap(typeof(Gameplay),"_listening",1);
        _groups=0; _requests=0; _requestResult=0;
        Assert.Null(Gameplay.PrepareBuildEvents()); Assert.Equal(4u,_groups); Assert.Equal(1,_requests);
    }

    [Fact]
    public void Entity_availability_probe_requests_the_group_before_deciding_to_subscribe()
    {
        var t=(GameplayNative.Table*)NativeMemory.AllocZeroed(144); t->Header=new ServiceHeader { Size=144,Version=1 };
        t->Status=&IdleEntityStatus;
        *(nint*)((byte*)t+96)=(nint)(delegate* unmanaged[Cdecl]<uint,int>)&Filter;
        *(nint*)((byte*)t+136)=(nint)(delegate* unmanaged[Cdecl]<ulong,uint,int>)&RequestGroups;
        Serve("kiln.gameplay",t,typeof(GameplayNative),"_table"); Swap(typeof(Gameplay),"_listening",1);
        _groups=0; _requestResult=0;
        Assert.True(Gameplay.EntityEventsAvailable); Assert.Equal(16u,_groups);
        _requestResult=3; Assert.False(Gameplay.EntityEventsAvailable);
    }
    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Filter(uint categories) => 0;

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int IdleEntityStatus(GameplayNative.Status* status) { status->EntityHooks=0; return 0; }

}
