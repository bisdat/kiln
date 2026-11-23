// SPDX-License-Identifier: MIT
#pragma warning disable KILNX031
using System.Text.Json.Nodes;
using Kiln.Host;
using Xunit;

namespace Kiln.Host.Tests;

public sealed class RegistryDispatchTests : ModsFixture
{
    private ModContext Owner(FakeRegistry native, string id)
        => HostAccess.CreateContext(new ModInfo(id, id, "1.0.0", Root), native.Host.OwnerCreate(id), DataDir);
    private static void Edit(RegistryContext context, int value, string registry = "items")
    {
        context.Capture = () => new JsonObject { ["ir"] = 1, ["mod"] = context.ModId,
            ["stage"] = context.Phase.ToString().ToLowerInvariant(), ["edits"] = new JsonArray(new JsonObject {
                ["registry"] = registry, ["select"] = registry == "items" ? "game:wood" : "game:balance",
                ["field"] = registry == "items" ? "maxStack" : "levelCap", ["op"] = value }) }.ToJsonString();
    }
    [RegistryEntryPoints(RegistryEntryPoint.ModLoad | RegistryEntryPoint.HotReload)]
    private sealed class Author(Action<RegistryContext> callback) : Mod
    {
        public override void OnRegistry(RegistryContext context) => callback(context);
    }
    [Fact]
    public void ForeignReadyWaitsForTheNextHostPreBeforeRunningRuntimeAuthors()
    {
        using var native = new FakeRegistry { Phase = 0 };
        using var manager = NewManager(native.Host);
        manager.PrepareRegistry(100);
        var dispatch = Assert.IsType<RegistryDispatch>(RegistryRuntime.Host);
        var tick = Environment.CurrentManagedThreadId;
        var threads = new List<int>();
        var owner = Owner(native, "pending");
        dispatch.Loaded(new Author(_ => threads.Add(Environment.CurrentManagedThreadId)), owner, Root, false);
        dispatch.FlushLoads(); Assert.Empty(threads);
        var loader = new Thread(() => { native.Phase = 4; native.FireReady(); });
        loader.Start(); loader.Join();
        Assert.Empty(threads);
        manager.Pre(Tick(1), []);
        Assert.Equal(new[] { tick, tick, tick }, threads);
        owner.Retire();
    }
    [Fact]
    public void RuntimeDrainsAndManualChangesRefuseAForeignThread()
    {
        using var native = new FakeRegistry(); using var manager = NewManager(native.Host);
        using var dispatch = new RegistryDispatch(manager); dispatch.Prepare(100);
        var owner = Owner(native, "threaded");
        var tick = Environment.CurrentManagedThreadId;
        native.Host.TickThread = () => Environment.CurrentManagedThreadId == tick;
        Exception? drain = null, manual = null;
        var worker = new Thread(() => {
            drain = Record.Exception(dispatch.FlushLoads);
            manual = Record.Exception(() => RegistryRuntime.Manual!(owner, c => Edit(c, 90)));
        });
        worker.Start(); worker.Join();
        Assert.IsType<InvalidOperationException>(drain);
        Assert.IsType<InvalidOperationException>(manual);
        Assert.Equal(0, native.Submits);
        owner.Retire();
    }
    [Fact]
    public void AllAlterPassesPrecedeFinalAndAllFinalsReadTheAlterView()
    {
        using var native = new FakeRegistry(); using var manager = NewManager(native.Host);
        using var dispatch = new RegistryDispatch(manager); dispatch.Prepare(100);
        var seen = new List<string>();
        void Invoke(RegistryContext context, int value)
        {
            seen.Add($"{context.ModId}:{context.Phase}:{context.ReadValue("items", "game:wood", "maxStack")}");
            if (context.Phase == RegistryPhase.Alter) Edit(context, value);
            if (context.Phase == RegistryPhase.Final) Edit(context, value + 100);
        }
        var a = Owner(native, "a"); var b = Owner(native, "b");
        dispatch.Loaded(new Author(c => Invoke(c, 20)), a, Root, false);
        dispatch.Loaded(new Author(c => Invoke(c, 30)), b, Root, false);
        dispatch.FlushLoads();
        Assert.Equal(new[] { "a:Add:10", "b:Add:10", "a:Alter:10", "b:Alter:20", "a:Final:30", "b:Final:30" }, seen);
        Assert.Equal(130, native.Value);
        a.Retire(); b.Retire();
        Assert.Equal(10, native.Value);
    }
    [Fact]
    public void ReplacingAModThatDropsAnEditRestoresBaseBeforeHotReload()
    {
        using var native = new FakeRegistry(); using var manager = NewManager(native.Host);
        using var dispatch = new RegistryDispatch(manager); dispatch.Prepare(100);
        var old = Owner(native, "test");
        dispatch.Loaded(new Author(c => { if (c.Phase == RegistryPhase.Alter) Edit(c, 50); }), old, Root, false);
        dispatch.FlushLoads(); Assert.Equal(50, native.Value);
        old.Retire(); Assert.Equal(10, native.Value);
        var replacement = Owner(native, "test"); var points = new List<RegistryEntryPoint>();
        dispatch.Loaded(new Author(c => { points.Add(c.EntryPoint); Assert.Equal("10", c.ReadValue("items", "game:wood", "maxStack")); }), replacement, Root, true);
        dispatch.FlushLoads();
        Assert.Equal(new[] { RegistryEntryPoint.HotReload, RegistryEntryPoint.HotReload, RegistryEntryPoint.HotReload }, points);
        Assert.Equal(10, native.Value); replacement.Retire();
    }
    [Fact]
    public void ManualBuffersAtomicallyExpiresItsContextAndRevertsOnlyItsLayer()
    {
        using var native = new FakeRegistry(); using var manager = NewManager(native.Host);
        using var dispatch = new RegistryDispatch(manager); dispatch.Prepare(100);
        var owner = Owner(native, "manual"); RegistryContext? retained = null;
        var result = RegistryRuntime.Manual!(owner, c => { retained = c; Edit(c, 40); });
        Assert.Equal(0u, result.Outcome); Assert.Equal(40, native.Value);
        Assert.Equal(RegistryContextError.Closed, Assert.Throws<RegistryContextException>(() => retained!.Ensure()).Code);
        var submits = native.Submits;
        Assert.Throws<InvalidOperationException>(() => RegistryRuntime.Manual!(owner, c => { Edit(c, 90); throw new InvalidOperationException("fail"); }));
        Assert.Equal(submits, native.Submits); Assert.Equal(40, native.Value);
        Assert.Equal(5u, RegistryRuntime.Revert!(owner, result.Handle).Outcome); Assert.Equal(10, native.Value);
        var refused = RegistryRuntime.Manual!(owner, c => Edit(c, 50, "balance"));
        Assert.Equal(3u, refused.Outcome); Assert.Equal(1u, refused.Refused); Assert.Equal(10, native.Value);
        native.World = 0;
        Assert.Equal(4u, RegistryRuntime.Manual!(owner, _ => throw new Exception("must not run")).Outcome);
    }
    [Fact]
    public void AThrowingAutomaticAuthorIsNotCalledInLaterPasses()
    {
        using var native = new FakeRegistry(); using var manager = NewManager(native.Host);
        using var dispatch = new RegistryDispatch(manager); dispatch.Prepare(100);
        var owner = Owner(native, "fault"); var calls = 0;
        dispatch.Loaded(new Author(c => { ++calls; Edit(c, 99); throw new InvalidOperationException("test fault"); }), owner, Root, false);
        dispatch.FlushLoads();
        Assert.Equal(1, calls); Assert.Equal(0, native.Submits); Assert.Equal(10, native.Value); owner.Retire();
    }
    [Fact]
    public void DisposingTheManagerReleasesTheReadySubscriptionAndRuntimeBridge()
    {
        using var native = new FakeRegistry(); var manager = NewManager(native.Host);
        manager.PrepareRegistry(100); Assert.Equal(1, native.Subscriptions);
        manager.Dispose(); Assert.Equal(0, native.Subscriptions); Assert.Null(RegistryRuntime.Manual);
    }
    [Fact]
    public unsafe void MissingStageFunctionsMakeTheTransportUnavailable()
    {
        using var native = new FakeRegistry(); var transport = new RegistryNative(native.Host);
        Assert.True(transport.Available); native.Stage->Subscribe = null;
        Assert.False(transport.Available);
    }
    public override void Dispose()
    {
        RegistryRuntime.Available = null; RegistryRuntime.Manual = null; RegistryRuntime.Revert = null;
        RegistryRuntime.Read = null; RegistryRuntime.WorldChanged = null; RegistryRuntime.LastReady = null;
        base.Dispose();
    }
}
