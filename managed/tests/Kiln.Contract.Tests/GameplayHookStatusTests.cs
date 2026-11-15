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
    [InlineData(false)]
    [InlineData(true)]
    public void Protect_waiting_for_world_stop_throws_the_named_unavailable_exception(bool update)
    {
        _state = 2; _legacy = false; _allowZoneAdd=update;
        var t = (GameplayNative.Table*)NativeMemory.AllocZeroed((nuint)sizeof(GameplayNative.Table));
        t->Header = new ServiceHeader { Size = (uint)sizeof(GameplayNative.Table), Version = 1 };
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
        Assert.Contains("build protection begins with the next world: this mod asked for it while a world was running", e.Message);
    }

    private static uint _state;
    private static bool _legacy;

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Status(GameplayNative.Status* s)
    {
        s->BuildHooks = _state == 1 ? (byte)1 : (byte)0;
        s->EntityHooks = 1;
        s->DiscardOn = 1;
        s->BuildHookState = _state;
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
}
