// SPDX-License-Identifier: MIT
// The room for world programs (chapter 17.4, decision I3; since 0.2.0-dev.29): World.GetProgramBudget over the table
// kiln.world.budget (native/core/abi/services/kiln_world_budget.h). World.Status stays the legacy diagnostic it was.
using System.Runtime.InteropServices;

namespace Kiln;

/// <summary>The engine's limits on one program's access lists, as kiln.world checks them when a program is registered.
/// They do not grow with Kiln's program quota.</summary>
/// <param name="Templates">Template lists (<c>tpl</c> and <c>tplOptional</c> together).</param>
/// <param name="ReadWrite">Read and write lists, with their optional ones, together.</param>
/// <param name="SingletonReads">Singletons read.</param>
/// <param name="SingletonWrites">Singletons written.</param>
/// <param name="OtherTemplates">Other-entity template lists.</param>
/// <param name="OtherAccessors">Other-entity reads and writes together.</param>
/// <param name="EventsInOut">Event inputs and outputs together.</param>
/// <param name="IteratorBytes">The largest iterator.</param>
public sealed record ProgramAccessLimits(uint Templates, uint ReadWrite, uint SingletonReads, uint SingletonWrites,
    uint OtherTemplates, uint OtherAccessors, uint EventsInOut, uint IteratorBytes);

/// <summary>The answer of <see cref="World.GetProgramBudget"/>: two budgets for the same world, read together.</summary>
/// <param name="Ok">True when both budgets are ready for a live world.</param>
/// <param name="Reason">Null when <paramref name="Ok"/>; else the readiness name (<c>NoWorld</c>, <c>NotReady</c>,
/// <c>AccountingMismatch</c>).</param>
/// <param name="World">The world the answer is for while it lives (also when it is not ready yet); null when there is no
/// world of that kind. A receipt for this process only: never save or send it.</param>
/// <param name="KilnSlots">Kiln's own quota, for the process: 64 programs for Kiln's services, compat loaders and mods
/// together. <see cref="CapacityInfo.KilnUsed"/> the programs that run or wait for a world, <see cref="CapacityInfo.Reserved"/>
/// the slots of programs that cannot run (refused, faulted, a declaration no loaded mod bound): a slot is never freed in
/// a process. Its counts are 0 whenever <paramref name="Ok"/> is false.</param>
/// <param name="EngineSlots">The world's own program table (896 entries): <see cref="CapacityInfo.GameUsed"/> the game's
/// programs, <see cref="CapacityInfo.KilnUsed"/> those Kiln registered there, <see cref="CapacityInfo.Reserved"/> another
/// loader's direct registrations. Its counts are 0 whenever <paramref name="Ok"/> is false.</param>
/// <param name="PerFileMax">Declarations a mod's <c>kiln.programs.json</c> may hold (32).</param>
/// <param name="AccessLimits">Each program's access limits (always filled).</param>
public sealed record ProgramBudgetResult(bool Ok, string? Reason, CapacityWorldToken? World, CapacityInfo KilnSlots,
    CapacityInfo EngineSlots, uint PerFileMax, ProgramAccessLimits AccessLimits) : IKilnResult;

internal static unsafe class ProgramBudgetNative
{
    public const string Service = "kiln.world.budget";
    public const uint Version = 1;
    public const string Capability = "world.program_budget";

    [StructLayout(LayoutKind.Sequential)]
    public struct AccessLimits
    {
        public uint Templates, ReadWrite, SingletonReads, SingletonWrites, OtherTemplates, OtherAccessors, EventsInOut, IteratorBytes;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct Reply  // 152 bytes
    {
        public uint Size, Readiness, WorldKind, Generation, PerFileMax, ReservedZero;
        public AccessLimits Access;
        public KilnCapacityInfo KilnSlots, EngineSlots;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct Table
    {
        public ServiceHeader Header;
        public delegate* unmanaged[Cdecl]<uint, Reply*, int> ProgramBudget;
    }

    private static Table* _table;
    private static bool _looked;

    public static Table* T
    {
        get
        {
            if (_table != null || _looked) return _table;
            _table = Services.Get<Table>(Service, Version);
            _looked = true;
            return _table;
        }
    }

    /// <summary>Tests: ask for the table again.</summary>
    internal static void Reset()
    {
        _table = null;
        _looked = false;
    }

    /// <summary>The managed result of a structurally valid reply. Ok needs both budgets ready and the world named; a
    /// reply that breaks that is a mismatch, never room.</summary>
    public static ProgramBudgetResult ToResult(in Reply r)
    {
        var kiln = CapacityMapping.ToResult(r.KilnSlots).Budget;
        var engine = CapacityMapping.ToResult(r.EngineSlots).Budget;
        var readiness = r.Readiness <= (uint)CapacityReadiness.NotFound ? (CapacityReadiness)r.Readiness : CapacityReadiness.AccountingMismatch;
        CapacityWorldToken? token = r.Generation != 0 && r.WorldKind is (uint)CapacityWorldKind.Server or (uint)CapacityWorldKind.Client
            ? new CapacityWorldToken((CapacityWorldKind)r.WorldKind, r.Generation)
            : null;
        var ok = readiness == CapacityReadiness.Ready && kiln.Ready && engine.Ready && token is not null &&
                 kiln.Scope == CapacityScope.Process && engine.Scope == CapacityScope.World && engine.World == token;
        if (!ok && readiness == CapacityReadiness.Ready) readiness = CapacityReadiness.AccountingMismatch;
        if (!ok)
        {
            // the pair makes no partial promise
            kiln = kiln with { Capacity = 0, GameUsed = 0, KilnUsed = 0, Reserved = 0, Readiness = kiln.Ready ? readiness : kiln.Readiness };
            engine = engine with { Capacity = 0, GameUsed = 0, KilnUsed = 0, Reserved = 0, Readiness = engine.Ready ? readiness : engine.Readiness };
        }
        var a = r.Access;
        return new ProgramBudgetResult(ok, ok ? null : readiness.ToString(), token, kiln, engine, r.PerFileMax,
            new ProgramAccessLimits(a.Templates, a.ReadWrite, a.SingletonReads, a.SingletonWrites, a.OtherTemplates, a.OtherAccessors,
                a.EventsInOut, a.IteratorBytes));
    }
}

public static unsafe partial class World
{
    /// <summary>
    /// The room for world programs, for the live world of <paramref name="kind"/>: Kiln's own 64 program slots (process
    /// wide: Kiln's services, compat loaders and mods together) and that world's program table of 896 entries (the
    /// game's, Kiln's and another loader's), as one answer with one revision, plus the limits of a declaration file and of
    /// one program's access lists. A cached answer: kiln.world counts what it already keeps (its slot ledger and its
    /// directory of the world's table, read in the world's own step), never the game's memory, and nothing runs for it
    /// while nobody asks. <c>NoWorld</c> when there is no world of that kind (a client world on a dedicated server),
    /// <c>NotReady</c> while a world's table is not read yet; then both budgets are 0 and promise nothing. Works on both
    /// sides (the class is otherwise server-side). Any thread. Registration itself still happens at a world's start only,
    /// and is checked against the live table then: a budget read earlier is not a promise.
    /// </summary>
    /// <param name="kind">The server world (default) or the client world.</param>
    /// <exception cref="ArgumentException"><paramref name="kind"/> is not Server or Client.</exception>
    /// <exception cref="KilnUnavailableException"><c>world.program_budget</c>: kiln.world.budget is not here (kiln.world
    /// is missing or older than 0.2.0-dev.29). Programs off on this build return <c>NotReady</c>.</exception>
    [AnyThread]
    [RequiresCapability(ProgramBudgetNative.Capability)]
    public static ProgramBudgetResult GetProgramBudget(CapacityWorldKind kind = CapacityWorldKind.Server)
    {
        if (kind is not (CapacityWorldKind.Server or CapacityWorldKind.Client))
            throw new ArgumentException($"world kind {(uint)kind} is not Server or Client", nameof(kind));
        var t = ProgramBudgetNative.T;
        if (t == null || t->Header.Size < (uint)sizeof(ProgramBudgetNative.Table))
            throw ServiceCaps.Unavailable(ProgramBudgetNative.Capability, "kiln.world.budget is not here (update Kiln)");
        var r = new ProgramBudgetNative.Reply { Size = (uint)sizeof(ProgramBudgetNative.Reply) };
        var rc = t->ProgramBudget((uint)kind, &r);
        if (rc == WorldNative.InvalidArgument) throw new ArgumentException($"world kind {kind} refused by kiln.world.budget", nameof(kind));
        if (rc != WorldNative.Ok) throw new InvalidOperationException($"World.GetProgramBudget: {WorldNative.Describe(rc)}");
        return ProgramBudgetWatch.Seen(kind, ProgramBudgetNative.ToResult(r));
    }
}

/// <summary>The program budget's <see cref="CapacityChangedEvent"/> (<c>Quota</c> <c>world.program_budget</c>, the
/// each world's engine slots and Kiln's slots): measured separately every 25 steps while something listens.</summary>
internal static class ProgramBudgetWatch
{
    private static readonly object Gate = new();
    private static readonly Dictionary<CapacityWorldKind, ProgramBudgetResult> Last = new();

    public static ProgramBudgetResult Seen(CapacityWorldKind kind, ProgramBudgetResult r)
    {
        ProgramBudgetResult? prev;
        lock (Gate)
        {
            Last.TryGetValue(kind, out prev);
            Last[kind] = r;
        }
        if (prev is null || !Events.HasSubscribers<CapacityChangedEvent>()) return r;
        if (CapacityMapping.Changed(ProgramBudgetNative.Capability, prev.KilnSlots, r.KilnSlots) is { } a) Events.Enqueue(a);
        if (CapacityMapping.Changed(ProgramBudgetNative.Capability, prev.EngineSlots, r.EngineSlots) is { } b) Events.Enqueue(b);
        return r;
    }

    public static void Tick(ulong step)
    {
        if (step % 25 != 9 || !Events.HasSubscribers<CapacityChangedEvent>()) return;
        foreach (var kind in new[] { CapacityWorldKind.Server, CapacityWorldKind.Client })
        {
            try { World.GetProgramBudget(kind); }
            catch (Exception e) when (e is KilnUnavailableException or InvalidOperationException) { }
        }
    }
}
