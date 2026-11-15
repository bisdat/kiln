// SPDX-License-Identifier: MIT
using System.Reflection;
using System.Runtime.InteropServices;
using Xunit;
using static Kiln.Blocks.Tests.RawGateTests;

namespace Kiln.Blocks.Tests;

/// <summary>Attachments to a vanilla native (since 0.2.0-dev.29, <see cref="Abilities.Natives.Attach"/>): the managed
/// dispatcher as the core calls it (a static function and an id, on a thread where no mod is current) and the refusals
/// that need no game. The filter and the stage order are the core's (native/services/abilities/tests).</summary>
[Collection("raw gates")]
public sealed unsafe class NativesAttachTests
{
    static readonly delegate* unmanaged[Cdecl]<AbilitiesNative.NativeCall*, void*, void> AttachDispatch =
        (delegate* unmanaged[Cdecl]<AbilitiesNative.NativeCall*, void*, void>)typeof(AbilitiesDispatch)
            .GetMethod("AttachDispatch", BindingFlags.NonPublic | BindingFlags.Static)!.MethodHandle.GetFunctionPointer();

    static AbilitiesNative.NativeCall* NewCall()
    {
        var c = (AbilitiesNative.NativeCall*)NativeMemory.AllocZeroed((nuint)sizeof(AbilitiesNative.NativeCall));
        c->Size = (uint)sizeof(AbilitiesNative.NativeCall);
        c->OutError = 0xFF;
        return c;
    }

    static void Run(AbilitiesNative.NativeCall* c, long id)
    {
        var t = new Thread(() => AttachDispatch(c, (void*)id));
        t.Start();
        t.Join();
    }

    // An entry as Attach makes it, without a core (no native table in a test process)
    static AbilitiesDispatch.AttachEntry Entry(ModContext? mod, NativeWrapHandler handler) =>
        AbilitiesDispatch.InsertAttach(0x01d09759, NativeStage.Before, handler, mod);

    [Fact]
    public void AnAttachedHandlerRunsAsItsModAndCanAnswer()
    {
        ModContext? current = null;
        var e = Entry(Plain, (ref NativeCall c) =>
        {
            current = HostAccess.Current;
            c.Return(5);
        });
        var call = NewCall();
        try
        {
            Run(call, e.Id);
            Assert.Same(Plain, current);
            Assert.Equal(0, call->OutError);
            Assert.Equal(5, call->OutValue);
        }
        finally
        {
            AbilitiesDispatch.Detach(e);
            NativeMemory.Free(call);
        }
    }

    [Fact]
    public void ADetachedOrThrowingHandlerLeavesTheCallAsItWas()
    {
        var calls = 0;
        var e = Entry(Plain, (ref NativeCall c) => { calls++; c.Return(1); });
        AbilitiesDispatch.Detach(e);
        var throws = Entry(Plain, (ref NativeCall c) => throw new InvalidOperationException("boom"));
        var call = NewCall();
        try
        {
            Run(call, e.Id);
            Run(call, throws.Id);
            Run(call, long.MaxValue);
            Assert.Equal(0, calls);
            Assert.Equal(0xFF, call->OutError);  // undecided: the core runs the game's native (fail open)
            Assert.Equal(1, AbilitiesDispatch.AttachCount(Plain));  // the throwing one; the detached one is gone
        }
        finally
        {
            AbilitiesDispatch.Detach(throws);
            NativeMemory.Free(call);
        }
    }

    [Fact]
    public void AttachRefusesBadArgumentsBeforeAnythingElse()
    {
        As(Plain, () =>
        {
            Assert.Throws<ArgumentNullException>(() => Abilities.Natives.Attach(1, "getSelf", NativeStage.Before, null!));
            Assert.Throws<ArgumentException>(() => Abilities.Natives.Attach(1, "", NativeStage.Before, (ref NativeCall c) => { }));
            Assert.Throws<ArgumentException>(() => Abilities.Natives.Attach(1, "get self", NativeStage.Before, (ref NativeCall c) => { }));
            Assert.Throws<ArgumentException>(() => Abilities.Natives.Attach(1, "0xZZ", NativeStage.Before, (ref NativeCall c) => { }));
            Assert.Throws<ArgumentOutOfRangeException>(() => Abilities.Natives.Attach(1, "getSelf", (NativeStage)7, (ref NativeCall c) => { }));
            var off = Assert.Throws<KilnUnavailableException>(() => Abilities.Natives.Attach(1, "getSelf", NativeStage.Before, (ref NativeCall c) => { }));
            Assert.Equal("abilities.natives.attach", off.Capability);
        });
        Assert.Throws<InvalidOperationException>(() => As(null, () => Abilities.Natives.Attach(1, "getSelf", NativeStage.Before, (ref NativeCall c) => { })));
    }

    [Fact]
    public void NativeNamesAreNodeNamesOrHashes()
    {
        Assert.True(Abilities.Natives.NativeNameFormat("getTransform"));
        Assert.True(Abilities.Natives.NativeNameFormat("0xa18f5959"));
        Assert.True(Abilities.Natives.NativeNameFormat("0X1"));
        Assert.False(Abilities.Natives.NativeNameFormat("0x"));
        Assert.False(Abilities.Natives.NativeNameFormat("0x123456789"));
        Assert.False(Abilities.Natives.NativeNameFormat("1abc"));
        Assert.False(Abilities.Natives.NativeNameFormat("my-native"));
    }

    [Fact]
    public void AStageIsBeforeInsteadOrAfterAsTheCoreNumbersThem()
    {
        Assert.Equal(0, (int)NativeStage.Before);
        Assert.Equal(1, (int)NativeStage.Instead);
        Assert.Equal(2, (int)NativeStage.After);
        Assert.Equal(16, sizeof(AbilitiesNative.NativeFilter));
        Assert.Equal(112, sizeof(AbilitiesNative.NativeCounters));  // KilnNativeCounters with the review's retirement and attachment counters
        Assert.Equal(8, sizeof(AbilitiesNative.NativeArgRaw));
    }
}
