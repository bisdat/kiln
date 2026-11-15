// SPDX-License-Identifier: MIT
using System.Reflection;
using System.Runtime.InteropServices;
using Kiln.Host;
using Xunit;

namespace Kiln.Host.Tests;

/// <summary>A mod's native admission closes before the host runs its OnUnload (06c, after the review's P1): a sync
/// native's handler of the Hello sample that is inside when the unload begins leaves before Hello's OnUnload runs
/// ("goodbye"), and a call that comes while OnUnload runs is refused (0x1F) instead of reaching the mod's code.</summary>
public sealed unsafe class NativeAdmissionOrderTests : ModsFixture
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

    [Fact]
    public void A_handler_inside_when_the_unload_begins_leaves_before_OnUnload_runs()
    {
        InstallHello();
        var host = new FakeHost();
        using var mods = NewManager(host);
        mods.LoadAll();
        var ctx = mods.Find("hello")!.ModContext;
        using var inside = new ManualResetEventSlim(false);
        bool? sawGoodbye = null;
        var e = AbilitiesDispatch.AddSync("hello.Hold", (ref NativeCall c) =>
        {
            inside.Set();
            var gate = AbilitiesDispatch.GateOf(ctx);
            var sw = System.Diagnostics.Stopwatch.StartNew();
            while (Volatile.Read(ref gate.Closed) == 0 && sw.Elapsed.TotalSeconds < 10) Thread.Yield();
            sawGoodbye = host.Has("goodbye");  // the mod's state as this handler sees it on its way out
            return 1;
        }, ctx);
        AbilitiesDispatch.Watch(ctx);
        var call = NewCall();
        var wait = AbilitiesDispatch.RetireWaitMs;
        AbilitiesDispatch.RetireWaitMs = 5000;  // the bound, raised: only the order decides this test
        try
        {
            var t = new Thread(() => SyncDispatch(call, (void*)e.Id));
            t.Start();
            Assert.True(inside.Wait(TimeSpan.FromSeconds(10)));
            Assert.True(mods.Unload("hello", out _));
            Assert.True(t.Join(TimeSpan.FromSeconds(20)));
            Assert.Equal((byte)0, call->OutError);
            Assert.Equal(false, sawGoodbye);  // it left before Hello's OnUnload said goodbye
            Assert.True(host.Has("goodbye"));
        }
        finally
        {
            AbilitiesDispatch.RetireWaitMs = wait;
            AbilitiesDispatch.RemoveSync(e);
            NativeMemory.Free(call);
        }
    }

    [Fact]
    public void The_unload_starting_step_runs_before_OnUnload_once_and_a_throwing_step_is_logged()
    {
        InstallHello();
        var host = new FakeHost();
        using var mods = NewManager(host);
        mods.LoadAll();
        var ctx = mods.Find("hello")!.ModContext;
        var order = new List<string>();
        ctx.UnloadStarting += () => throw new InvalidOperationException("a broken service");
        ctx.UnloadStarting += () => order.Add(host.Has("goodbye") ? "start after OnUnload" : "start");
        ctx.Retiring += () => order.Add(host.Has("goodbye") ? "retiring" : "retiring before OnUnload");
        Assert.True(mods.Unload("hello", out _));
        Assert.Equal(["start", "retiring"], order);  // once (Retire's own call finds it done), before OnUnload
        Assert.True(host.Has("a service's step at the start of this mod's unload threw: InvalidOperationException: a broken service"), host.Dump());
        Assert.True(host.Has("mod hello unloaded"));
    }

    [Fact]
    public void A_call_while_OnUnload_runs_is_refused_and_never_reaches_the_mod()
    {
        InstallHello();
        var host = new FakeHost();
        using var mods = NewManager(host);
        mods.LoadAll();
        var ctx = mods.Find("hello")!.ModContext;
        var handlerCalls = 0;
        var e = AbilitiesDispatch.AddSync("hello.Late", (ref NativeCall c) =>
        {
            Interlocked.Increment(ref handlerCalls);
            return 1;
        }, ctx);
        AbilitiesDispatch.Watch(ctx);
        var call = NewCall();
        byte? during = null;
        // Hello's OnUnload logs "goodbye": a call of its native from right there (as a worker could make it then)
        host.OnLine = line =>
        {
            if (!line.Contains("goodbye", StringComparison.Ordinal) || during is not null) return;
            call->OutError = 0xFF;
            SyncDispatch(call, (void*)e.Id);
            during = call->OutError;
        };
        try
        {
            Assert.True(mods.Unload("hello", out _));
            Assert.Equal((byte)0x1F, during);  // refused: as a removed native
            Assert.Equal(0, handlerCalls);
        }
        finally
        {
            host.OnLine = null;
            AbilitiesDispatch.RemoveSync(e);
            NativeMemory.Free(call);
        }
    }
}
