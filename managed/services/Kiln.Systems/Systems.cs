// SPDX-License-Identifier: MIT
// Kiln.Systems: the game's own ECS programs, and your code before, after or instead of one of them (kiln.world's
// program wraps, chapter 06a). Guide: docs/kiln/services/systems.md.
using System.Collections.Concurrent;

namespace Kiln;

/// <summary>One of the game's ECS programs in one world's program table.</summary>
/// <param name="Name">Its name: <c>"fuel_setup"</c>. Names last; indices move between game builds.</param>
/// <param name="Index">Its position in that world's table now.</param>
/// <param name="World">The world whose table it is in.</param>
/// <param name="Flags">The descriptor's flags (0x1 a client program, 0x4 SYSTEM, 0x8 INIT, 0x200 CHUNKED, ...).</param>
/// <param name="UpdateRva">Its update in the game's image (0: none in this world).</param>
/// <param name="Runs">True when it has an update in this world (a client program on a server has none).</param>
/// <param name="Writes">The components its descriptor writes (write and optional write lists).</param>
/// <param name="Wraps">The managed wraps on it now.</param>
public sealed record SystemInfo(string Name, int Index, WorldKind World, uint Flags, ulong UpdateRva, bool Runs,
    IReadOnlyList<string> Writes, IReadOnlyList<WrapInfo> Wraps);

/// <summary>One managed stage on a program.</summary>
public sealed record WrapInfo(string Mod, string Program, WorldKind World, WrapStage Stage, int Priority, WrapState State,
    bool PerEntity);

/// <summary>Where a stage runs relative to the game's update.</summary>
public enum WrapStage
{
    /// <summary>Before the update (high priority first).</summary>
    Before = 0,
    /// <summary>Instead of the update; at most one per program.</summary>
    Replace = 1,
    /// <summary>After the update (in the exact reverse order of the Befores).</summary>
    After = 2,
}

/// <summary>A wrap's lifetime: made at once, applied in the target world's next step, removed in the step after a
/// Dispose.</summary>
public enum WrapState
{
    /// <summary>Waits for the target world's next step (or for the next world of that kind).</summary>
    Pending,
    /// <summary>In the program's chain.</summary>
    Active,
    /// <summary>Gone for good (disposed, its mod unloaded, native SET loaded).</summary>
    Removed,
    /// <summary>Never applied (another tool swapped the program, the program is not in that world).</summary>
    Refused,
}

/// <summary>Which world a wrap targets.</summary>
public sealed record WrapOptions
{
    /// <summary>The world (default the server world). <see cref="WorldKind.Client"/> needs capability
    /// <c>systems.wraps.client</c>. <see cref="WorldKind.Prediction"/> (since 0.2.0-dev.29.18, mod API 0.2.10) needs
    /// capability <c>systems.wraps.prediction</c>: the stages run on the prediction storage's slices of a program of the
    /// client world's table (the prediction storage has no table of its own), Before and After only, on a program the
    /// prediction storage steps (descriptor flags without 0x1, 0x10 and 0x40). Where that capability is off a prediction
    /// wrap throws <see cref="ArgumentException"/>, as before 0.2.0-dev.29.18.</summary>
    public WorldKind World { get; init; } = WorldKind.Server;

    /// <summary>The priority of the stages whose call gives none.</summary>
    public int Priority { get; init; }
}

/// <summary>The outcome of making a wrap or a Replace stage.</summary>
/// <param name="Ok">True when the wrap was made.</param>
/// <param name="Reason">Why not: another mod replaces the program (named), the program is not in that world, native SET
/// is loaded. Null when <paramref name="Ok"/>.</param>
/// <param name="Wrap">The wrap; null when not <paramref name="Ok"/>.</param>
public sealed record WrapResult<TWrap>(bool Ok, string? Reason, TWrap? Wrap) : IKilnResult where TWrap : class;

/// <summary>What a stage did so far, summed over its stages.</summary>
public sealed record WrapStats(long Slices, long Entities, double Microseconds, long Faults, long Overruns, int Threads)
{
    /// <summary>Slices a <see cref="ProgramWrap.WhenArchetype"/> filter left to the game because the entity map changed
    /// under its archetype read three times in a row (kiln.log says so once per stage). Since 0.2.0-dev.29.</summary>
    public long ArchetypeSkips { get; init; }
}

/// <summary>A stage was turned off: it faulted, overran its budget 10 slices in a row, threw, or native SET came.
/// Raised on the tick thread.</summary>
public sealed record WrapDisabledEvent(string Program, string Mod, WrapStage Stage, string Reason);

/// <summary>What a slice filter sees (<see cref="ProgramWrap.When"/>). <paramref name="Layout"/> is the slice's
/// archetype key (0 when not known).</summary>
public readonly record struct SliceInfo(string Program, WorldKind World, int Count, ulong Step, bool OnWorldThread, nint Layout);

/// <summary>A slice stage: runs once per slice of the program, on the thread that runs the slice.</summary>
public delegate void SliceHandler(ref Slice slice);

/// <summary>A per-entity stage: runs once per entity of each slice, on the thread that runs the slice.</summary>
public delegate void EntityHandler(EntityId entity);

/// <summary>The game's ECS programs, and wraps on them: your code before, after or instead of a program's update, per
/// slice (one archetype group's range of entities) or per entity, in the program's own slot and thread. Handlers run on
/// the game's worker threads (always in the client world, in the server world once a player is in it): no tick-thread
/// API inside them (they throw), per-slice state only, shared state through atomics, <see cref="Slice.Post"/> for work
/// on the tick thread. One Replace per program. Refused while native SET is loaded.</summary>
public static unsafe partial class Systems
{
    /// <summary>The program <paramref name="program"/> of that world's table, or null (not in that world, no world yet).</summary>
    /// <exception cref="ArgumentException"><paramref name="program"/> is null or empty.</exception>
    [AnyThread]
    public static SystemInfo? Find(string program, WorldKind world = WorldKind.Server)
    {
        ArgumentException.ThrowIfNullOrEmpty(program);
        var t = SystemsNative.T;
        if (t == null) return null;
        var index = FindIndex(t, program, world);
        return index < 0 ? null : Info(t, world, index);
    }

    /// <summary>Every program of that world's table, in table order (empty before the world's first step).</summary>
    [AnyThread]
    public static IReadOnlyList<SystemInfo> All(WorldKind world = WorldKind.Server)
    {
        var t = SystemsNative.T;
        var list = new List<SystemInfo>();
        if (t == null) return list;
        for (var i = 0; ; i++)
        {
            var info = Info(t, world, i);
            if (info is null) break;
            list.Add(info);
        }
        return list;
    }

    /// <summary>A wrap on <paramref name="program"/>: add its stages with <see cref="ProgramWrap.Before"/>,
    /// <see cref="ProgramWrap.After"/>, <see cref="ProgramWrap.Replace"/>, <see cref="ProgramWrap.BeforeEach(EntityHandler, int?)"/>
    /// and <see cref="ProgramWrap.AfterEach(EntityHandler, int?)"/> (per entity, also with an <see cref="EntityViewHandler"/>). Stages apply in the target world's next step and again, by program name, in
    /// later worlds of that kind; they go with <see cref="ProgramWrap.Dispose"/> or when the mod unloads.</summary>
    /// <returns><c>Ok = false</c>: the program is not in that world (a client program on a server), native SET is
    /// loaded.</returns>
    /// <exception cref="ArgumentException"><paramref name="program"/> is null or empty, or the world is
    /// <see cref="WorldKind.Prediction"/> while <see cref="PredictionCapability"/> is off.</exception>
    /// <exception cref="KilnUnavailableException"><c>systems.wraps</c> (or <c>systems.wraps.client</c>) is off.</exception>
    [TickThreadOnly, RequiresCapability("systems.wraps")]
    public static WrapResult<ProgramWrap> Wrap(string program, WrapOptions? options = null)
    {
        ArgumentException.ThrowIfNullOrEmpty(program);
        options ??= new WrapOptions();
        var refused = Check(program, options, "Systems.Wrap", out _, out _);
        return refused is not null ? new WrapResult<ProgramWrap>(false, refused, null)
            : new WrapResult<ProgramWrap>(true, null, new ProgramWrap(program, options, ModContext.CurrentMod));
    }

    /// <summary>A typed wrap on the program of view <typeparamref name="T"/> (Kiln.Game's <c>Ecs.Programs.*</c>, made from
    /// the program catalog of the game build Kiln was built for): the stages of <see cref="Wrap(string, WrapOptions?)"/>
    /// plus per-entity stages and <see cref="ProgramWrap{T}.AroundEach{TSnap}"/> that see the typed view. The live
    /// descriptor's contract must be the view's: on another game build whose program changed, the wrap is refused.</summary>
    /// <returns><c>Ok = false</c>: the program is not in that world, native SET is loaded, or its live descriptor differs
    /// from the view's (the reason names each list that changed: <c>"fuel_setup: write list changed: +X -Y"</c>).</returns>
    /// <exception cref="ArgumentException">The view has no name or no contract, or the world is
    /// <see cref="WorldKind.Prediction"/> while <see cref="PredictionCapability"/> is off.</exception>
    /// <exception cref="KilnUnavailableException"><c>systems.wraps.typed</c> (or <c>systems.wraps.client</c>) is off.</exception>
    [TickThreadOnly, RequiresCapability("systems.wraps.typed")]
    public static WrapResult<ProgramWrap<T>> Wrap<T>(WrapOptions? options = null) where T : IGameProgram<T>, allows ref struct
    {
        var program = T.Name;
        if (string.IsNullOrEmpty(program)) throw new ArgumentException($"{typeof(T).Name} names no program", nameof(T));
        if (T.Contract == 0) throw new ArgumentException($"{typeof(T).Name} carries no contract", nameof(T));
        options ??= new WrapOptions();
        var refused = Check(program, options, "Systems.Wrap", out var t, out var index);
        if (refused is not null) return new WrapResult<ProgramWrap<T>>(false, refused, null);
        if (!SystemsNative.HasContract(t))
            return new WrapResult<ProgramWrap<T>>(false, $"{program}: kiln.world has no program contract member; typed wraps need a newer native service", null);
        var typed = Capabilities.Get("systems.wraps.typed");
        if (typed.State != CapabilityState.Available) throw new KilnUnavailableException("systems.wraps.typed", typed.ReasonText);
        var buffer = new byte[8192];
        uint live = 0, length = 0;
        int rc;
        fixed (byte* text = buffer)
            rc = t->GameProgramContract((uint)TableOf(options.World), index, &live, text, (uint)buffer.Length, &length);
        if (rc != SystemsNative.Ok)
            return new WrapResult<ProgramWrap<T>>(false, $"{program}: kiln.world cannot read its descriptor (result {rc})", null);
        if (live != T.Contract)
            return new WrapResult<ProgramWrap<T>>(false, Contracts.Differences(program, T.Lists,
                System.Text.Encoding.UTF8.GetString(buffer, 0, (int)Math.Min(length, (uint)buffer.Length))), null);
        return new WrapResult<ProgramWrap<T>>(true, null,
            new ProgramWrap<T>(new ProgramWrap(program, options, ModContext.CurrentMod)));
    }

    // What both Wraps check: the world, the thread, the mod, the capability rows, the program in that world. Null: go.
    private static string? Check(string program, WrapOptions options, string member, out SystemsNative.Table* t, out int index)
    {
        t = null;
        index = -1;
        if (options.World == WorldKind.Prediction && !Capabilities.Is(PredictionCapability))
            throw new ArgumentException("the prediction world has no program table of its own (it runs the client world's); " +
                $"prediction wraps need capability {PredictionCapability}: {Capabilities.Why(PredictionCapability)}", nameof(options));
        Services.RequireTickThread(member);
        if (ModContext.CurrentMod is { Alive: false }) throw new InvalidOperationException("the mod is unloading");
        var why = WhyOff(options.World);
        if (why.Refuse is { } refuse) return refuse;
        t = SystemsNative.T;
        if (t == null) throw new KilnUnavailableException("systems.wraps", "kiln.world v4 is not here");
        var table = TableOf(options.World);
        index = FindIndex(t, program, table);
        if (index < 0)
            return $"{program}: no such program in the {Lower(table)} world" +
                (options.World == WorldKind.Server ? " (it may be a client program)" : "");
        var info = Info(t, table, index)!;
        if (!info.Runs) return $"{program} has no update in the {Lower(table)} world";
        if (options.World == WorldKind.Prediction && (info.Flags & NotPredicted) != 0)
            return $"{program} does not run in the prediction storage (descriptor flags 0x{info.Flags:x}: " +
                ((info.Flags & 0x1) != 0 ? "a client-world program" : (info.Flags & 0x10) != 0 ? "hostOnly" : "the engine's bit 0x40") + ")";
        return null;
    }

    // ---- what Wrap says when wraps are off ----

    internal readonly record struct Off(string? Refuse);

    internal static Off WhyOff(WorldKind world)
    {
        var row = Capabilities.Get("systems.wraps");
        if (row.State != CapabilityState.Available)
        {
            // native SET (kiln.world reports the row off with reason policy): a refusal, not a missing capability
            if (row.Reason == CapabilityReason.Policy && row.ReasonText.Contains("SET", StringComparison.Ordinal))
                return new Off(row.ReasonText);
            throw new KilnUnavailableException("systems.wraps", row.ReasonText);
        }
        if (world == WorldKind.Client && (Capabilities.IsHeld("systems.wraps.client") || !Capabilities.Is("systems.wraps.client")))
            throw new KilnUnavailableException("systems.wraps.client", Capabilities.Why("systems.wraps.client"));
        if (world == WorldKind.Prediction && (Capabilities.IsHeld(PredictionCapability) || !Capabilities.Is(PredictionCapability)))
            throw new KilnUnavailableException(PredictionCapability, Capabilities.Why(PredictionCapability));
        return new Off(null);
    }

    /// <summary>The capability of prediction wraps (<see cref="WrapOptions.World"/> = <see cref="WorldKind.Prediction"/>):
    /// on where <c>systems.wraps.client</c> is. Since 0.2.0-dev.29.18 (mod API 0.2.10).</summary>
    public const string PredictionCapability = "systems.wraps.prediction";

    // the descriptor flags that keep a program out of the prediction storage's step (client StepPrograms: kind 1 skips the
    // engine's 0x40 and runs only programs without the client flag 0x1 and HOST_ONLY 0x10)
    internal const uint NotPredicted = 0x1 | 0x10 | 0x40;

    // The world whose table holds a wrap's program: the prediction storage runs the client world's.
    internal static WorldKind TableOf(WorldKind w) => w == WorldKind.Prediction ? WorldKind.Client : w;

    internal static string Lower(WorldKind w) => w switch { WorldKind.Client => "client", WorldKind.Prediction => "prediction", _ => "server" };

    internal static int FindIndex(SystemsNative.Table* t, string program, WorldKind world)
    {
        var bytes = System.Text.Encoding.UTF8.GetBytes(program);
        fixed (byte* p = bytes)
            return t->GameProgramFind((uint)world, new NativeStr { Ptr = p, Len = (nuint)bytes.Length });
    }

    internal static SystemInfo? Info(SystemsNative.Table* t, WorldKind world, int index)
    {
        var info = new SystemsNative.ProgramInfo { Size = (uint)sizeof(SystemsNative.ProgramInfo) };
        if (t->GameProgramInfo((uint)world, index, &info) != SystemsNative.Ok) return null;
        var name = info.Name.ToString();
        var writes = new List<string>();
        var names = stackalloc NativeStr[64];
        foreach (var list in (ReadOnlySpan<int>)[SystemsNative.ListWrite, SystemsNative.ListWriteOpt])
        {
            var n = t->GameProgramList((uint)world, index, list, names, 64);
            for (var i = 0; i < n && i < 64; i++) writes.Add(names[i].ToString());
        }
        return new SystemInfo(name, info.Index, world, info.DescFlags, info.UpdateRva,
            (info.Flags & SystemsNative.ProgramNoUpdate) == 0, writes, ProgramWrap.InfosOf(name, world));
    }

    // ---- the managed wraps of the process ----
    internal static readonly ConcurrentDictionary<ManagedStage, byte> Live = new();
}
