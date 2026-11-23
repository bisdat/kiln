// SPDX-License-Identifier: MIT
using System.Text.Json.Nodes;
using Kiln;
using Kiln.Registry;
using DataRegistry = Kiln.Registry.Registry;
using Xunit;

namespace Kiln.RegistryTests;

public sealed class RegistryContextTests
{
    private static ModContext Mod() => HostAccess.CreateContext(new ModInfo("test", "Test", "1.0.0", "."), 77, ".");
    private static RegistryContext Frame(ModContext mod, RegistryEntryPoint point = RegistryEntryPoint.GameStart,
        RegistryPhase phase = RegistryPhase.Alter, RegistryWorld? world = null) =>
        new(mod, point, phase, world, "test-code", (_, _, _) => "10", (_, _) => new[] { "game:wood" });

    [Fact]
    public void TypedCellsBufferStableIrAndExpireWithTheirContext()
    {
        var context = Frame(Mod());
        using var scope = context.Enter();
        var registry = DataRegistry.For(context);
        var item = registry.Items["game:wood"];
        Assert.Equal(10, item.MaxStack);
        item.MaxStack = 500;
        Assert.Equal(500, item.MaxStack);
        var ir = JsonNode.Parse(context.CaptureIr()!)!;
        Assert.Equal("test", ir["mod"]!.GetValue<string>());
        Assert.Equal("alter", ir["stage"]!.GetValue<string>());
        Assert.Equal("items", ir["edits"]![0]!["registry"]!.GetValue<string>());
        Assert.Equal("maxStack", ir["edits"]![0]!["field"]!.GetValue<string>());
        Assert.Equal(500, ir["edits"]![0]!["op"]!.GetValue<int>());
        context.Close();
        Assert.Equal(RegistryContextError.Closed, Assert.Throws<RegistryContextException>(() => DataRegistry.For(context)).Code);
        Assert.Equal(RegistryContextError.Closed, Assert.Throws<RegistryContextException>(() => item.MaxStack).Code);
        Assert.Equal(RegistryContextError.Closed, Assert.Throws<RegistryContextException>(() => item.MaxStack = 20).Code);
    }

    [Fact]
    public void UnloadedOwnerFailsBeforeAnyBackendAccess()
    {
        var mod = Mod(); var context = Frame(mod);
        using var scope = context.Enter();
        var item = DataRegistry.For(context).Items["game:wood"];
        mod.Retire();
        Assert.Equal(RegistryContextError.Unloaded, Assert.Throws<RegistryContextException>(() => item.MaxStack).Code);
        context.Close();
    }

    [Fact]
    public void CallbackContextCannotBeUsedFromAnotherThread()
    {
        var context = Frame(Mod());
        using var scope = context.Enter();
        Exception? failure = null;
        var thread = new Thread(() => failure = Record.Exception(() => DataRegistry.For(context)));
        thread.Start(); thread.Join();
        Assert.Equal(RegistryContextError.WrongThread, Assert.IsType<RegistryContextException>(failure).Code);
        context.Close();
    }

    [Theory]
    [InlineData(RegistryEntryPoint.GameStart)]
    [InlineData(RegistryEntryPoint.ModLoad)]
    public void EarlyPointsCannotReadWorldSettingsEvenIfAWorldExists(RegistryEntryPoint point)
    {
        var context = Frame(Mod(), point, world: new RegistryWorld(123, WorldKind.Server, "world-a"));
        using var scope = context.Enter();
        Assert.Equal(RegistryContextError.WorldSettingsUnavailable,
            Assert.Throws<RegistryContextException>(() => Config.RegisterWorld<Settings>()).Code);
        context.Close();
    }

    [Fact]
    public void PointWorldAndObservedSettingsContributeToIdentity()
    {
        var mod = Mod();
        var a = Frame(mod, RegistryEntryPoint.WorldLoad, world: new RegistryWorld(1, WorldKind.Server, "a"));
        var b = Frame(mod, RegistryEntryPoint.WorldLoad, world: new RegistryWorld(2, WorldKind.Server, "b"));
        var c = Frame(mod, RegistryEntryPoint.HotReload, world: new RegistryWorld(1, WorldKind.Server, "a"));
        Assert.NotEqual(a.Identity, b.Identity); Assert.NotEqual(a.Identity, c.Identity);
        var before = a.Identity;
        a.ObserveSettings("test", "{\"value\":1}");
        Assert.NotEqual(before, a.Identity);
        a.Close(); b.Close(); c.Close();
    }

    [Fact]
    public void GlobalSettingsAreReadOnlyInputsAndAutomaticallyAffectIdentity()
    {
        var context = Frame(Mod());
        using var scope = context.Enter();
        var before = context.Identity;
        var settings = Config.Register<Settings>(name: "registry-input");
        Assert.Equal(3, settings.Value.Value);
        Assert.NotEqual(before, context.Identity);
        Assert.DoesNotContain(Config.All, file => ReferenceEquals(file, settings));
        Assert.Equal(RegistryContextError.ReadOnly, Assert.Throws<RegistryContextException>(() => settings.Save()).Code);
        context.Close();
    }

    [Fact]
    public void ResolvedViewsCannotWriteEvenInTheAlterPass()
    {
        var owner = Mod();
        var context = new RegistryContext(owner, RegistryEntryPoint.Manual, RegistryPhase.Alter, null, "read",
            (_, _, _) => "10", (_, _) => new[] { "game:wood" }) { ReadOnly = true };
        using var scope = context.Enter();
        var item = DataRegistry.For(context).Items["game:wood"];
        Assert.Equal(RegistryContextError.ReadOnly, Assert.Throws<RegistryContextException>(() => item.MaxStack = 20).Code);
        context.Close();
    }

    [Fact]
    public void AWorldBoundContextFailsBeforeReadingAfterTheWorldChanges()
    {
        var owner = Mod(); ulong token = 1; var reads = 0;
        var context = new RegistryContext(owner, RegistryEntryPoint.WorldLoad, RegistryPhase.Alter,
            new RegistryWorld(1, WorldKind.Server, "a"), "world", (_, _, _) => { ++reads; return "10"; }, (_, _) => new[] { "game:wood" })
            { CurrentWorldToken = () => token };
        using var scope = context.Enter();
        var item = DataRegistry.For(context).Items["game:wood"];
        token = 2;
        Assert.Equal(RegistryContextError.WorldChanged, Assert.Throws<RegistryContextException>(() => item.MaxStack).Code);
        Assert.Equal(0, reads);
        context.Close();
    }

    [Fact]
    public void CatchingAnInvalidIndexedWriteDoesNotLeaveABufferedEdit()
    {
        var owner = Mod();
        var context = new RegistryContext(owner, RegistryEntryPoint.GameStart, RegistryPhase.Alter, null, "list",
            (_, _, _) => "[1,2]", (_, _) => []);
        using var scope = context.Enter();
        var registry = DataRegistry.For(context);
        var field = RegistrySchema.Find("balance.xpPerLevel")!;
        Assert.Throws<ArgumentOutOfRangeException>(() => registry.Cells("balance", "game:balance").Set(field.Id, 9, 3L));
        Assert.Null(context.CaptureIr()); context.Close();
    }

    [Fact]
    public void ReadyFromANativeThreadIsDeliveredThroughTheOwnersScheduler()
    {
        var owner = Mod(); var calls = 0; var callbackThread = 0;
        var context = Frame(owner, RegistryEntryPoint.ModLoad);
        using (context.Enter()) DataRegistry.Ready += _ => { ++calls; callbackThread = Environment.CurrentManagedThreadId; };
        context.Close(); owner.Pending.Clear();
        var native = new Thread(() => RegistryRuntime.PublishReady(RegistryPhase.Start, 42, null));
        native.Start(); native.Join();
        Assert.Equal(0, calls);
        Assert.Single(owner.Pending);
        owner.Pending.Dequeue()();
        Assert.Equal(1, calls); Assert.Equal(Environment.CurrentManagedThreadId, callbackThread);
        owner.Retire();
    }

    [Fact]
    public void CatchingAFailedListReadDoesNotLeaveABufferedEdit()
    {
        var owner = Mod();
        var context = new RegistryContext(owner, RegistryEntryPoint.GameStart, RegistryPhase.Alter, null, "list",
            (_, _, _) => throw new KeyNotFoundException("unavailable list"), (_, _) => []);
        using var scope = context.Enter();
        var registry = DataRegistry.For(context);
        Assert.Throws<KeyNotFoundException>(() => registry.Items["game:wood"].Perks.Clear());
        Assert.Null(context.CaptureIr()); context.Close();
    }

    [Fact]
    public void DefaultEntryIdsAreRejectedBeforeTransportAccess()
    {
        var context = Frame(Mod()); using var scope = context.Enter();
        Assert.Throws<ArgumentException>(() => DataRegistry.For(context).Items[default(RegistryId)]);
        context.Close();
    }

    [Fact]
    public void ChangesHaveExplicitRevertLifetimeRatherThanDisposableScopeLifetime()
    {
        Assert.False(typeof(IDisposable).IsAssignableFrom(typeof(RegistryChange)));
        Assert.Null(typeof(RegistryChange).GetMethod("Dispose"));
        Assert.NotNull(typeof(RegistryChange).GetMethod(nameof(RegistryChange.Revert))!.GetCustomAttributes(typeof(TickThreadOnlyAttribute), false).SingleOrDefault());
    }
    private sealed class ThreadBackend(int tick) : IKilnBackend
    {
        public string Version => "test";
        public bool IsTickThread => Environment.CurrentManagedThreadId == tick;
        public bool HasFeature(string name) => false;
        public void Log(ulong owner, LogLevel level, string text) { }
        public nint Service(string name, uint minVersion) => 0;
    }
    [Fact]
    public void ExplicitRevertRefusesOtherThreadsAndRunsOnlyOnceOnTheTick()
    {
        var owner = Mod(); var calls = 0;
        var backend = Backend.Current; var previous = RegistryRuntime.Revert;
        Backend.Current = new ThreadBackend(Environment.CurrentManagedThreadId);
        RegistryRuntime.Revert = (_, _) => { ++calls; return new(5, 0, 0, 0, 1, 2, 0, ""); };
        try
        {
            var change = new RegistryChange(owner, new(0, 1, 0, 0, 1, 1, 9, ""));
            Exception? failure = null;
            var worker = new Thread(() => failure = Record.Exception(() => { change.Revert(); }));
            worker.Start(); worker.Join();
            Assert.IsType<InvalidOperationException>(failure); Assert.Equal(0, calls);
            Assert.True(change.Revert().Ok); Assert.True(change.Revert().Ok); Assert.Equal(1, calls);
        }
        finally { Backend.Current = backend; RegistryRuntime.Revert = previous; owner.Retire(); }
    }
    [Fact]
    public void EveryNonOkCompletionHasAReason()
    {
        for (uint outcome = 0; outcome <= 8; ++outcome)
        {
            var result = RegistryChangeResult.From(new(outcome, 0, 0, 0, 0, 0, 0, ""));
            if (!result.Ok) Assert.False(string.IsNullOrWhiteSpace(result.Reason), $"outcome {outcome} has no reason");
        }
    }

    public sealed class Settings { public int Value { get; set; } = 3; }
}
