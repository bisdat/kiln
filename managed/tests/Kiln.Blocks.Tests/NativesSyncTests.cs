// SPDX-License-Identifier: MIT
using System.Reflection;
using System.Runtime.InteropServices;
using Xunit;
using static Kiln.Blocks.Tests.RawGateTests;

namespace Kiln.Blocks.Tests;

/// <summary>Synchronous Kiln natives (since 0.2.0-dev.29, <see cref="Abilities.Natives.RegisterSync"/>): the managed
/// dispatcher as the core calls it (a static function and an id, on a thread where no mod is current), the capability
/// row's decision, and the refusals that need no game.</summary>
[Collection("raw gates")]
public sealed unsafe class NativesSyncTests
{
    static readonly delegate* unmanaged[Cdecl]<AbilitiesNative.NativeCall*, void*, void> SyncDispatch =
        (delegate* unmanaged[Cdecl]<AbilitiesNative.NativeCall*, void*, void>)typeof(AbilitiesDispatch)
            .GetMethod("SyncDispatch", BindingFlags.NonPublic | BindingFlags.Static)!.MethodHandle.GetFunctionPointer();

    sealed class Call : IDisposable
    {
        public readonly AbilitiesNative.NativeCall* C;
        readonly uint* _args;
        readonly byte* _layout, _data;

        // one sint32 variable (index 0) as args[0]
        public Call(int value)
        {
            _args = (uint*)NativeMemory.AllocZeroed(sizeof(uint));
            _args[0] = 0xFFFF0000;
            _layout = (byte*)NativeMemory.AllocZeroed(24);
            *(uint*)(_layout + 8) = 0x9859f31e;
            *(ushort*)(_layout + 12) = 4;
            _data = (byte*)NativeMemory.AllocZeroed(4);
            *(int*)_data = value;
            C = (AbilitiesNative.NativeCall*)NativeMemory.AllocZeroed((nuint)sizeof(AbilitiesNative.NativeCall));
            C->Size = (uint)sizeof(AbilitiesNative.NativeCall);
            C->Argc = 1;
            C->Args = _args;
            C->Layout = _layout;
            C->LayoutCount = 1;
            C->Data = _data;
            C->DataSize = 4;
            C->OutError = 0xFF;
        }

        public int Value => *(int*)_data;

        public void Dispose()
        {
            NativeMemory.Free(C);
            NativeMemory.Free(_data);
            NativeMemory.Free(_layout);
            NativeMemory.Free(_args);
        }
    }

    // As the core's thunk calls it: on another thread, no mod current, `user` = the entry's id.
    static void Run(Call call, long id)
    {
        var t = new Thread(() => SyncDispatch(call.C, (void*)id));
        t.Start();
        t.Join();
    }

    [Fact]
    public void TheHandlersValueIsTheAnswerAndItRunsAsItsMod()
    {
        ModContext? current = null;
        var e = AbilitiesDispatch.AddSync("plain-mod.Double", (ref NativeCall c) =>
        {
            current = HostAccess.Current;
            c.SetU32(0, c.GetU32(0) * 2);
            return 7;
        }, Plain);
        try
        {
            using var call = new Call(21);
            Run(call, e.Id);
            Assert.Equal(0, call.C->OutError);
            Assert.Equal(7, call.C->OutValue);
            Assert.Equal(42, call.Value);  // the argument was written inside the call
            Assert.Same(Plain, current);
        }
        finally { AbilitiesDispatch.RemoveSync(e); }
    }

    [Fact]
    public void AFaultAnswersInternalAndCallOriginalThrows()
    {
        Exception? original = null;
        var e = AbilitiesDispatch.AddSync("plain-mod.Bad", (ref NativeCall c) =>
        {
            try { c.CallOriginal(); } catch (Exception x) { original = x; }
            throw new InvalidOperationException("boom");
        }, Plain);
        try
        {
            using var call = new Call(1);
            Run(call, e.Id);
            Assert.Equal(0x11, call.C->OutError);  // the instance stops with Error; the game goes on
            Assert.Equal("a synchronous Kiln native has no original to call", Assert.IsType<InvalidOperationException>(original).Message);
        }
        finally { AbilitiesDispatch.RemoveSync(e); }
    }

    [Fact]
    public void ARemovedOrUnknownIdAnswersUnknownNative()
    {
        var calls = 0;
        var e = AbilitiesDispatch.AddSync("plain-mod.Gone", (ref NativeCall c) => { calls++; return 1; }, Plain);
        AbilitiesDispatch.RemoveSync(e);
        using var call = new Call(1);
        Run(call, e.Id);
        Assert.Equal(0x1F, call.C->OutError);
        using var other = new Call(1);
        Run(other, long.MaxValue);
        Assert.Equal(0x1F, other.C->OutError);
        Assert.Equal(0, calls);
    }

    [Fact]
    public void AModsUnloadTakesOnlyItsOwnSyncNatives()
    {
        var mine = AbilitiesDispatch.AddSync("plain-mod.A", (ref NativeCall c) => 1, Plain);
        var theirs = AbilitiesDispatch.AddSync("coremod.A", (ref NativeCall c) => 2, Coremod);
        try
        {
            AbilitiesDispatch.RemoveSyncs(Plain);  // no native table here: only the managed side
            Assert.Equal(0, AbilitiesDispatch.SyncCount(Plain));
            Assert.Equal(1, AbilitiesDispatch.SyncCount(Coremod));
            using var call = new Call(1);
            Run(call, theirs.Id);
            Assert.Equal(2, call.C->OutValue);
            using var gone = new Call(1);
            Run(gone, mine.Id);
            Assert.Equal(0x1F, gone.C->OutError);
        }
        finally { AbilitiesDispatch.RemoveSync(theirs); }
    }

    [Fact]
    public void TheRowSaysWhyItIsOff()
    {
        var off = new Capability("abilities.natives", CapabilityState.Unavailable, CapabilityReason.SymbolMissing,
            "symbol abilities/RegisterNative: no match", "sigdb");
        var on = new Capability("abilities.natives", CapabilityState.Available, CapabilityReason.None, "", "sigdb");

        var r = AbilitiesCaps.DecideNatives(false, null, false, on, "synchronous natives");
        Assert.Equal((CapabilityState.Unavailable, CapabilityReason.ServiceMissing), (r.State, r.Reason));
        // a game client: kiln.abilities is there, its sigdb feature is not
        r = AbilitiesCaps.DecideNatives(true, false, true, off, "synchronous natives");
        Assert.Equal((CapabilityState.Unavailable, CapabilityReason.SymbolMissing), (r.State, r.Reason));
        Assert.StartsWith("needs abilities.natives: symbol abilities/RegisterNative", r.Text);
        r = AbilitiesCaps.DecideNatives(true, null, false, on, "synchronous natives");
        Assert.Equal((CapabilityState.Unavailable, CapabilityReason.ServiceOlder), (r.State, r.Reason));
        r = AbilitiesCaps.DecideNatives(true, false, true, on, "synchronous natives");
        Assert.Equal((CapabilityState.Unavailable, CapabilityReason.Faulted), (r.State, r.Reason));
        r = AbilitiesCaps.DecideNatives(true, true, true, on, "synchronous natives");
        Assert.Equal((CapabilityState.Available, CapabilityReason.None), (r.State, r.Reason));
        // the sigdb row not reported (a test kit): decided by the service alone
        r = AbilitiesCaps.DecideNatives(true, true, true, null, "synchronous natives");
        Assert.Equal(CapabilityState.Available, r.State);
    }

    [Fact]
    public void RegisterSyncRefusesBadArgumentsBeforeAnythingElse()
    {
        As(Plain, () =>
        {
            Assert.Throws<ArgumentNullException>(() => Abilities.Natives.RegisterSync("X", 1, null!));
            Assert.Throws<ArgumentOutOfRangeException>(() => Abilities.Natives.RegisterSync("X", 32, (ref NativeCall c) => 1));
            Assert.Throws<ArgumentException>(() => Abilities.Natives.RegisterSync("", 1, (ref NativeCall c) => 1));
            var other = Assert.Throws<ArgumentException>(() => Abilities.Natives.RegisterSync("coremod.X", 1, (ref NativeCall c) => 1));
            Assert.Contains("under its own id (plain-mod.<Name>)", other.Message);
            Assert.Throws<ArgumentException>(() => Abilities.Natives.RegisterSync("On-Hit", 1, (ref NativeCall c) => 1));
            // no kiln.abilities in a test process: the capability is off, named
            var off = Assert.Throws<KilnUnavailableException>(() => Abilities.Natives.RegisterSync("OnHit", 1, (ref NativeCall c) => 1));
            Assert.Equal("abilities.natives.sync", off.Capability);
        });
        Assert.Throws<InvalidOperationException>(() => As(null, () => Abilities.Natives.RegisterSync("OnHit", 1, (ref NativeCall c) => 1)));
    }

    [Fact]
    public void NamesFollowTheNativeRule()
    {
        Assert.True(Abilities.Natives.ValidName("mymod.OnHit"));
        Assert.True(Abilities.Natives.ValidName("my.mod.OnHit"));
        Assert.True(Abilities.Natives.ValidName("my-mod.On_Hit2"));
        Assert.False(Abilities.Natives.ValidName("mymod.On-Hit"));
        Assert.False(Abilities.Natives.ValidName("mymod."));
        Assert.False(Abilities.Natives.ValidName(".OnHit"));
        Assert.False(Abilities.Natives.ValidName("m." + new string('a', 89)));
        Assert.True(Abilities.Natives.ValidName("m." + new string('a', 88)));
    }
}
