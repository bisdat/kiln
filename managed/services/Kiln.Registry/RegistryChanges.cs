// SPDX-License-Identifier: MIT
using System.Diagnostics.CodeAnalysis;

namespace Kiln.Registry;

/// <summary>The outcome of an explicit registry change. Pending is not a claim that game bytes changed.</summary>
[Experimental("KILNX031")]
public enum RegistryChangeOutcome
{
    /// <summary>Applied through the mapped backend.</summary>
    Applied = 0,
    /// <summary>Retained until a world loads.</summary>
    PendingWorld = 1,
    /// <summary>Refused, with a reason.</summary>
    Refused = 2,
    /// <summary>A later point cannot write a restart-only field.</summary>
    RestartRequired = 3,
    /// <summary>No suitable world exists.</summary>
    WorldRequired = 4,
    /// <summary>The change was removed and remaining layers recomposed.</summary>
    Reverted = 5,
    /// <summary>Another writer's bytes were preserved.</summary>
    Skipped = 6,
    /// <summary>The backend could not prove rollback.</summary>
    Unverified = 7,
    /// <summary>A lazily loaded resource is still pending.</summary>
    PendingResource = 8,
}

/// <summary>A copied outcome. Partial application has refused edits and is not Ok; inspect all counts.</summary>
[Experimental("KILNX031")]
public sealed record RegistryChangeResult(RegistryChangeOutcome Outcome, uint Accepted, uint Refused, uint Skipped,
    ulong World, ulong Generation, string? Reason) : IKilnResult
{
    /// <summary>Applied or reverted without refusals or skipped foreign bytes. Pending states are not Ok yet.</summary>
    public bool Ok => (Outcome is RegistryChangeOutcome.Applied or RegistryChangeOutcome.Reverted) && Refused == 0 && Skipped == 0;
    internal static RegistryChangeResult From(RegistryCompletion value) => new((RegistryChangeOutcome)value.Outcome,
        value.Accepted, value.Refused, value.Skipped, value.World, value.Generation,
        value.Reason.Length != 0 ? value.Reason : value.Refused != 0 ? $"{value.Refused} edit(s) refused; see the registry report" :
        value.Skipped != 0 ? $"{value.Skipped} field(s) skipped to preserve another writer's bytes" : value.Outcome switch
        {
            0 or 5 => null,
            1 => "retained until a world loads",
            3 => "a restart-only field cannot be changed at this entry point",
            4 => "a suitable active world is required",
            6 => "another writer's bytes were preserved",
            7 => "the backend could not verify rollback",
            8 => "a resource is not loaded yet; the change remains pending",
            _ => "the change was refused; see the registry report",
        });
}

/// <summary>An owner-bound live change. Only explicit Revert or owner retirement removes it; leaving a scope does not.
/// Revert removes this transaction, never an old memory snapshot.</summary>
[Experimental("KILNX031")]
public sealed class RegistryChange
{
    private readonly WeakReference<ModContext> _owner;
    private ulong _handle;
    internal RegistryChange(ModContext owner, RegistryCompletion value)
        => (_owner, _handle, Result) = (new(owner), value.Handle, RegistryChangeResult.From(value));
    /// <summary>The last copied outcome (safe to retain after a callback or unload).</summary>
    public RegistryChangeResult Result { get; private set; }
    /// <summary>Reverts on the active world's thread. Foreign bytes are skipped, and an expired handle is refused.</summary>
    [TickThreadOnly, RequiresCapability("kiln.registry.live")]
    public RegistryChangeResult Revert()
    {
        if (!_owner.TryGetTarget(out var owner) || !owner.Alive)
            throw new RegistryContextException(RegistryContextError.Unloaded, "the registry change's mod retired");
        if (ModContext.CurrentMod is { } caller && !ReferenceEquals(caller, owner)) throw new ArgumentException("the change belongs to another mod");
        Game.AssertTickThread();
        if (_handle == 0) return Result;
        var revert = RegistryRuntime.Revert ?? throw new KilnUnavailableException("kiln.registry.live");
        Result = RegistryChangeResult.From(revert(owner, _handle)); _handle = 0; return Result;
    }
}

/// <summary>A ready notification. The world token is Kiln-owned, never a native address.</summary>
[Experimental("KILNX031")]
public sealed record RegistryReady(RegistryReadyPhase Phase, ulong Generation, RegistryWorld? World);

public static partial class Registry
{
    private static readonly object ReadyGate = new();
    private static readonly List<(ModContext Owner, Action<RegistryReady> Handler)> Handlers = [];
    private static RegistryReady? _lastReady;
    static Registry()
    {
        RegistryRuntime.Ready += Publish;
        if (RegistryRuntime.LastReady is { } ready) _lastReady = new(ready.Phase, ready.Generation, ready.World);
    }
    /// <summary>The Host has the stage and live tables. Per-field availability is still reported by registry field capabilities.</summary>
    [AnyThread]
    public static bool Available => RegistryRuntime.Available?.Invoke() == true;
    /// <summary>Raised after a complete stage/live application. A late subscriber gets a copied notification on its next tick.</summary>
    [TickThreadOnly]
    public static event Action<RegistryReady> Ready
    {
        add
        {
            ArgumentNullException.ThrowIfNull(value);
            var mod = CallingMod();
            if (RegistryContext.Current?.EntryPoint == RegistryEntryPoint.GameStart)
                throw new RegistryContextException(RegistryContextError.ReadOnly, "register runtime ready handlers in OnLoad, not a game-start author");
            RegistryReady? last;
            lock (ReadyGate) { Handlers.Add((mod, value)); last = _lastReady; }
            mod.Retiring += () => { lock (ReadyGate) Handlers.RemoveAll(x => ReferenceEquals(x.Owner, mod)); };
            if (last is not null) mod.Scheduler.Post(() => ModInvoke.Run(mod, () => value(last), "registry ready"));
        }
        remove { if (value is not null) lock (ReadyGate) Handlers.RemoveAll(x => x.Handler == value && ReferenceEquals(x.Owner, ModContext.CurrentMod)); }
    }
    /// <summary>An explicit, buffered live transaction. A throwing handler submits nothing. Restart fields return RestartRequired.</summary>
    [TickThreadOnly, RequiresCapability("kiln.registry.live")]
    public static RegistryChange Change(Action<RegistryStage> edits)
    {
        ArgumentNullException.ThrowIfNull(edits); var owner = CallingMod();
        var run = RegistryRuntime.Manual ?? throw new KilnUnavailableException("kiln.registry.live");
        return new(owner, run(owner, context => edits(For(context))));
    }
    /// <summary>Reads the resolved mapped view in an expiring, read-only context. Keep copied values, not entries.</summary>
    [TickThreadOnly, RequiresCapability("kiln.registry")]
    public static void Read(Action<RegistryStage> read)
    {
        ArgumentNullException.ThrowIfNull(read);
        (RegistryRuntime.Read ?? throw new KilnUnavailableException("kiln.registry"))(CallingMod(), context => read(For(context)));
    }
    private static ModContext CallingMod()
    {
        var mod = ModContext.CurrentMod ?? throw new InvalidOperationException("registry runtime access belongs inside a loaded mod callback");
        if (!mod.Alive) throw new RegistryContextException(RegistryContextError.Unloaded, "the mod retired");
        return mod;
    }
    private static void Publish(RegistryReadyPhase phase, ulong generation, RegistryWorld? world)
    {
        var ready = new RegistryReady(phase, generation, world);
        (ModContext Owner, Action<RegistryReady> Handler)[] handlers;
        lock (ReadyGate) { _lastReady = ready; handlers = Handlers.ToArray(); }
        // Resources-ready can arrive on a native loader thread. Never run runtime mod code on that thread or reenter a commit.
        foreach (var (owner, handler) in handlers)
            if (owner.Alive) owner.Scheduler.Post(() => ModInvoke.Run(owner, () => handler(ready), "registry ready"));
    }
}
