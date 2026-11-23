// SPDX-License-Identifier: MIT
using System.Diagnostics.CodeAnalysis;
using System.Security.Cryptography;
using System.Text;

namespace Kiln;

/// <summary>When registry edits run. A context has exactly one point; the declaration attribute accepts the four automatic flags.</summary>
[Flags, Experimental("KILNX031")]
public enum RegistryEntryPoint
{
    /// <summary>At resources-ready, before game data is consumed. The default for an OnRegistry override.</summary>
    GameStart = 1,
    /// <summary>After the runtime instance loads, once the registry's base exists. Global inputs only.</summary>
    ModLoad = 2,
    /// <summary>Each new world, including an id-less one. Settings stay unavailable until adoption reauthors the point with a bound id.</summary>
    WorldLoad = 4,
    /// <summary>After the old runtime instance retires and its replacement loads. Replacing a declared ModLoad, it keeps
    /// ModLoad's process lifetime unless it reads per-world settings (then it is replayed at the next bound world).</summary>
    HotReload = 8,
    /// <summary>An explicit live transaction, not an automatically invoked class declaration.</summary>
    Manual = 16,
}

/// <summary>The registry's all-mod passes. Ordinary assignments target Alter; Final is explicit.</summary>
[Experimental("KILNX031")]
public enum RegistryPhase
{
    /// <summary>All mods' add pass.</summary>
    Add = 1,
    /// <summary>All mods' ordinary field edits.</summary>
    Alter = 2,
    /// <summary>All mods' final pass, reading add + alter, not another mod's final.</summary>
    Final = 3,
}

/// <summary>Completed registry snapshot readiness, separate from an author's Add/Alter/Final passes.</summary>
[Experimental("KILNX031")]
public enum RegistryReadyPhase
{
    /// <summary>The complete game-start registry is ready.</summary>
    Start = 4,
    /// <summary>The live application is ready (also used for later live refreshes).</summary>
    WorldLoad = 5,
}

/// <summary>Declares the automatic points of a Mod subclass without executing its code. Manual transactions are explicit.</summary>
[AttributeUsage(AttributeTargets.Class, Inherited = true), Experimental("KILNX031")]
public sealed class RegistryEntryPointsAttribute : Attribute
{
    /// <summary>Declares one or more automatic points. Zero, Manual and unknown bits are invalid.</summary>
    public RegistryEntryPointsAttribute(RegistryEntryPoint points)
    {
        if (!Valid(points)) throw new ArgumentOutOfRangeException(nameof(points), "declare GameStart, ModLoad, WorldLoad and/or HotReload");
        Points = points;
    }
    /// <summary>The constant flags in the declaration.</summary>
    public RegistryEntryPoint Points { get; }
    internal static bool Valid(RegistryEntryPoint points) => points != 0 && ((uint)points & ~15u) == 0;
}

/// <summary>A Kiln-owned world token and its save identity, when known. Token is never a game address.</summary>
[Experimental("KILNX031")]
public readonly record struct RegistryWorld(ulong Token, WorldKind Kind, string? Id);

/// <summary>Why a registry context cannot be used.</summary>
[Experimental("KILNX031")]
public enum RegistryContextError
{
    /// <summary>The callback returned.</summary>
    Closed,
    /// <summary>The owning mod retired.</summary>
    Unloaded,
    /// <summary>The caller is not on this callback's thread.</summary>
    WrongThread,
    /// <summary>This point has no world settings, or they are not yet bound.</summary>
    WorldSettingsUnavailable,
    /// <summary>The context's world or settings identity no longer matches.</summary>
    WorldChanged,
    /// <summary>An input snapshot is read-only.</summary>
    ReadOnly,
    /// <summary>GameStart authoring cannot use another mod's runtime export; choose ModLoad or later.</summary>
    RuntimeExportUnavailable,
}

/// <summary>A closed, retired, wrong-thread or otherwise invalid registry context. No native pointer is accessed first.</summary>
[Experimental("KILNX031")]
public sealed class RegistryContextException : InvalidOperationException
{
    /// <summary>Creates a typed context error.</summary>
    public RegistryContextException(RegistryContextError code, string message) : base(message) => Code = code;
    /// <summary>The machine-readable reason.</summary>
    public RegistryContextError Code { get; }
}

/// <summary>The host-owned frame of Mod.OnRegistry. Use Kiln.Registry.Registry.For(context) for typed access.
/// Operations expire when the callback returns or the mod retires; immutable identity metadata remains a snapshot.</summary>
[Experimental("KILNX031")]
public sealed class RegistryContext
{
    private readonly WeakReference<ModContext> _owner;
    private readonly int _thread = Environment.CurrentManagedThreadId;
    private readonly string _code;
    private readonly IncrementalHash _settings = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
    private readonly HashSet<string> _observedSettings = new(StringComparer.Ordinal);
    private volatile bool _open = true;
    private string? _closedIdentity;
    [ThreadStatic] internal static RegistryContext? Current;
    [ThreadStatic] private static bool _observing;

    internal RegistryContext(ModContext owner, RegistryEntryPoint point, RegistryPhase phase, RegistryWorld? world,
        string codeIdentity, Func<string, string, string, string> read, Func<string, string, IReadOnlyList<string>> select,
        Dictionary<string, ConfigFile>? inputs = null)
    {
        _owner = new(owner); ModId = owner.Info.Id; EntryPoint = point; Phase = phase; World = world; _code = codeIdentity;
        ReadValue = read; SelectIds = select; InputConfigs = inputs ?? new(StringComparer.Ordinal);
    }
    /// <summary>The author's mod id.</summary>
    public string ModId { get; }
    /// <summary>The chosen application point.</summary>
    public RegistryEntryPoint EntryPoint { get; }
    /// <summary>The current all-mod pass.</summary>
    public RegistryPhase Phase { get; }
    /// <summary>The world's identity, or null before one exists.</summary>
    public RegistryWorld? World { get; }
    /// <summary>Hash of code, point, pass, world and observed settings. The live API has no executable-plan cache.</summary>
    public string Identity
    {
        get
        {
            if (!_open) return _closedIdentity!;
            if (_thread != Environment.CurrentManagedThreadId) throw Error(RegistryContextError.WrongThread, "use the callback's own thread");
            var inputs = FormattableString.Invariant($"{_code}\n{EntryPoint}\n{Phase}\n{World?.Token}\n{World?.Kind}\n{World?.Id}\n{Convert.ToHexStringLower(_settings.GetCurrentHash())}");
            return Convert.ToHexStringLower(SHA256.HashData(Encoding.UTF8.GetBytes(inputs)));
        }
    }

    internal Func<string, string, string, string> ReadValue { get; }
    internal Func<string, string, IReadOnlyList<string>> SelectIds { get; }
    internal object? Adapter;
    internal bool ReadOnly { get; init; }
    internal Func<ulong>? CurrentWorldToken { get; init; }
    internal Func<string?>? Capture;
    internal Dictionary<string, ConfigFile> InputConfigs { get; private set; }
    /// <summary>A per-world setting was read in this frame (it survives Close: the host decides the change's lifetime after).</summary>
    internal bool ReadWorldSettings { get; private set; }
    internal ModContext Owner => _owner.TryGetTarget(out var mod) ? mod : throw Error(RegistryContextError.Unloaded, "the mod retired");
    internal void Ensure()
    {
        if (!_owner.TryGetTarget(out var owner) || !owner.Alive) throw Error(RegistryContextError.Unloaded, "the mod retired");
        if (!_open) throw Error(RegistryContextError.Closed, "the registry callback returned");
        if (_thread != Environment.CurrentManagedThreadId) throw Error(RegistryContextError.WrongThread, "use the callback's own thread");
        if (World is { } world && EntryPoint is RegistryEntryPoint.WorldLoad or RegistryEntryPoint.HotReload or RegistryEntryPoint.Manual &&
            CurrentWorldToken is { } current && current() != world.Token)
            throw Error(RegistryContextError.WorldChanged, "the context's world ended or changed");
    }
    internal void EnsureWorldSettings()
    {
        Ensure();
        if (EntryPoint is RegistryEntryPoint.GameStart or RegistryEntryPoint.ModLoad || World is not { Token: not 0 })
            throw Error(RegistryContextError.WorldSettingsUnavailable, "world settings are available only at world load or later, with an active world");
    }
    internal void ObserveSettings(string id, string values)
    {
        if (_observedSettings.Add(id)) _settings.AppendData(Encoding.UTF8.GetBytes(id + "\0" + values + "\n"));
    }
    internal static void WithoutObservation(Action load)
    {
        var previous = _observing; _observing = true;
        try { load(); } finally { _observing = previous; }
    }
    internal static void Observe(ConfigFile file)
    {
        if (Current is not { } frame || _observing) return;
        frame.Ensure();
        if (file.PerWorld)
        {
            frame.EnsureWorldSettings();
            if (frame.World?.Id is not { } id || file.WorldId != id)
                throw Error(RegistryContextError.WorldSettingsUnavailable, "this world's settings have not been bound");
            frame.ReadWorldSettings = true;
        }
        if (frame._observedSettings.Contains(file.Id)) return;
        _observing = true;
        try { frame.ObserveSettings(file.Id, file.ValuesJson()); }
        finally { _observing = false; }
    }
    internal string? CaptureIr() { Ensure(); return Capture?.Invoke(); }
    internal void Close()
    {
        if (!_open) return;
        _closedIdentity = Identity; _open = false; Capture = null; Adapter = null; InputConfigs = new(); _settings.Dispose(); _observedSettings.Clear();
    }
    internal IDisposable Enter() { Ensure(); return new Scope(this); }
    internal static RegistryContextException Error(RegistryContextError code, string message) => new(code, message);
    private sealed class Scope : IDisposable
    {
        private readonly RegistryContext? _previous = Current;
        private readonly ModContext? _mod = ModContext.CurrentMod;
        internal Scope(RegistryContext frame) { Current = frame; ModContext.CurrentMod = frame.Owner; }
        public void Dispose() { Current = _previous; ModContext.CurrentMod = _mod; }
    }
}

internal sealed record RegistryCompletion(uint Outcome, uint Accepted, uint Refused, uint Skipped,
    ulong World, ulong Generation, ulong Handle, string Reason);
internal sealed record RegistryReadyState(RegistryReadyPhase Phase, ulong Generation, RegistryWorld? World);

// Host installs these; Kiln.Registry never references Kiln.Host or moves its published generated types into Kiln.Api.
internal static class RegistryRuntime
{
    internal static object? Host;
    internal static Func<bool>? Available;
    internal static Func<ModContext, Action<RegistryContext>, RegistryCompletion>? Manual;
    internal static Func<ModContext, ulong, RegistryCompletion>? Revert;
    internal static Action<ModContext, Action<RegistryContext>>? Read;
    internal static Action<string?>? WorldChanged;
    internal static Action<RegistryReadyPhase, ulong, RegistryWorld?>? Ready;
    internal static volatile RegistryReadyState? LastReady;
    internal static void PublishReady(RegistryReadyPhase phase, ulong generation, RegistryWorld? world)
    {
        LastReady = new(phase, generation, world);
        Ready?.Invoke(phase, generation, world);
    }
}
