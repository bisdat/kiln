// SPDX-License-Identifier: MIT
using System.Reflection;
using System.Runtime.InteropServices;
using Xunit;

namespace Kiln.Blocks.Tests;

/// <summary>Review P1 (dev.29): a sync native's handler inside while its mod retires. Admission closes first, the
/// retirement waits a bounded time and is deferred while the call is inside, the handler finishes with what it uses
/// intact, and no call enters after the close.</summary>
[Collection("raw gates")]
public sealed unsafe class NativesRetireTests
{
    static readonly delegate* unmanaged[Cdecl]<AbilitiesNative.NativeCall*, void*, void> SyncDispatch =
        (delegate* unmanaged[Cdecl]<AbilitiesNative.NativeCall*, void*, void>)typeof(AbilitiesDispatch)
            .GetMethod("SyncDispatch", BindingFlags.NonPublic | BindingFlags.Static)!.MethodHandle.GetFunctionPointer();

    static AbilitiesNative.NativeCall* NewCall()
    {
        var c = (AbilitiesNative.NativeCall*)NativeMemory.AllocZeroed((nuint)sizeof(AbilitiesNative.NativeCall));
        c->Size = (uint)sizeof(AbilitiesNative.NativeCall);
        c->OutError = 0xFF;
        return c;
    }

    static ModContext NewMod(string id) => HostAccess.CreateContext(new ModInfo(id, id, "1.0.0", "no-such-folder"), 0, "no-such-folder");

    [Fact]
    public void ARetirementWaitsBoundedThenDefersAndTheHandlerFinishesWithItsStateIntact()
    {
        var mod = NewMod("gate-mod-1");
        using var gateOpen = new ManualResetEventSlim(false);
        using var inside = new ManualResetEventSlim(false);
        var state = new List<int> { 41 };  // what the handler uses after it was let go
        int calls = 0, after = -1;
        var e = AbilitiesDispatch.AddSync("gate-mod-1.Hold", (ref NativeCall c) =>
        {
            Interlocked.Increment(ref calls);
            inside.Set();
            gateOpen.Wait(TimeSpan.FromSeconds(20));
            state.Add(state[0] + 1);  // intact: nothing was taken from under it
            after = state[^1];
            return 1;
        }, mod);
        var c1 = NewCall();
        var c2 = NewCall();
        try
        {
            var t = new Thread(() => SyncDispatch(c1, (void*)e.Id));
            t.Start();
            Assert.True(inside.Wait(TimeSpan.FromSeconds(10)));
            var gate = AbilitiesDispatch.GateOf(mod);
            Interlocked.Exchange(ref gate.Closed, 1);  // what Retire does first
            var sw = System.Diagnostics.Stopwatch.StartNew();
            AbilitiesDispatch.RetireGate(mod, gate);
            Assert.True(sw.ElapsedMilliseconds < 1000);  // bounded: about RetireWaitMs, never for ever
            Assert.Equal(1, AbilitiesDispatch.DeferredCount);
            // a new call after the close does not enter
            var t2 = new Thread(() => SyncDispatch(c2, (void*)e.Id));
            t2.Start();
            t2.Join();
            Assert.Equal(0x1F, c2->OutError);
            Assert.Equal(1, Volatile.Read(ref calls));
            AbilitiesDispatch.FinishDeferred();
            Assert.Equal(1, AbilitiesDispatch.DeferredCount);  // still inside: kept
            gateOpen.Set();
            t.Join();
            Assert.Equal(42, after);
            Assert.Equal(1, c1->OutValue);
            AbilitiesDispatch.FinishDeferred();
            Assert.Equal(0, AbilitiesDispatch.DeferredCount);  // the last call left: retired
            Assert.Equal(0, Volatile.Read(ref gate.Inflight));
        }
        finally
        {
            gateOpen.Set();
            AbilitiesDispatch.RemoveSync(e);
            NativeMemory.Free(c1);
            NativeMemory.Free(c2);
        }
    }

    [Fact]
    public void ARetirementFromInsideAHandlerDoesNotWaitForItself()
    {
        var mod = NewMod("gate-mod-2");
        bool? quiet = null;
        long ms = -1;
        var e = AbilitiesDispatch.AddSync("gate-mod-2.Self", (ref NativeCall c) =>
        {
            var gate = AbilitiesDispatch.GateOf(mod);
            var sw = System.Diagnostics.Stopwatch.StartNew();
            quiet = AbilitiesDispatch.WaitQuiet(gate);
            ms = sw.ElapsedMilliseconds;
            return 1;
        }, mod);
        var c = NewCall();
        try
        {
            var t = new Thread(() => SyncDispatch(c, (void*)e.Id));
            t.Start();
            t.Join();
            Assert.False(quiet);
            Assert.True(ms < 50);
        }
        finally
        {
            AbilitiesDispatch.RemoveSync(e);
            NativeMemory.Free(c);
        }
    }

    [Fact]
    public void AnAttachmentsStateIsGoneOnceDetached()
    {
        var a = new NativeAttachment(AbilitiesDispatch.InsertAttach(1, NativeStage.Before, (ref NativeCall c) => { }, null));
        Assert.Equal(NativeAttachmentState.Active, a.State);  // no core table here: what an older core gives
        a.Dispose();
        Assert.Equal(NativeAttachmentState.Gone, a.State);
        Assert.Equal(NativeState.None, Abilities.Natives.StateOf(0x1234));
    }
}
