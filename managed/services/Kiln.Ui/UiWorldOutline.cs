// SPDX-License-Identifier: MIT
// Ui.World.Outline (0.2.0-dev.30.3, GitHub issue #111): an entity's model outlined in this player's game with the game's
// own highlight. Native: kiln.render v2 (native/core/abi/services/kiln_render.h, "Outlines"): Kiln keeps the game's
// highlight groups Group1 and Group2 and colours them itself. Per-mod handles end with the mod's unload. Usage:
// docs/kiln/render.md "Outlines".
using System.Collections.Concurrent;

namespace Kiln;

/// <summary>How <see cref="Ui.World.Outline"/> outlines an entity. Since 0.2.0-dev.30.3 (mod API 0.2.13).</summary>
public sealed record OutlineOptions
{
    private readonly TimeSpan? _duration;

    /// <summary>The outline's colour. Its alpha must be above 0 (<see cref="Ui.World.Outline"/> throws
    /// <see cref="ArgumentException"/> otherwise). Default: <see cref="Ui.World.DefaultOutlineColor"/> (gold).
    /// Since 0.2.0-dev.30.3 (mod API 0.2.13).</summary>
    public Color Color { get; init; } = Ui.World.DefaultOutlineColor;

    /// <summary>The colour breathes (a 1.2 s period, 40 % to 100 % brightness). Since 0.2.0-dev.30.3 (mod API 0.2.13).</summary>
    public bool Pulse { get; init; }

    /// <summary>Among mods' outlines on the same entity the highest priority shows; on a tie the newest. The others wait
    /// (<see cref="OutlineReason.Outshone"/>) and take over when it ends. When more than two colours are in use, the
    /// colours of the highest-priority outlines keep the game's two highlight groups and the others share them
    /// (<see cref="WorldOutline.ColorShared"/>). Default 0. Since 0.2.0-dev.30.3 (mod API 0.2.13).</summary>
    public int Priority { get; init; }

    /// <summary>How long the outline lasts; null (the default) = until you dispose it, your mod unloads, the entity
    /// despawns or the client world ends. Otherwise above zero and at most <see cref="uint.MaxValue"/> milliseconds
    /// (about 49 days). Since 0.2.0-dev.30.3 (mod API 0.2.13).</summary>
    /// <exception cref="ArgumentOutOfRangeException">Zero, negative, or longer than <see cref="uint.MaxValue"/> ms.</exception>
    public TimeSpan? Duration
    {
        get => _duration;
        init
        {
            if (value is { } d && (d <= TimeSpan.Zero || d.TotalMilliseconds > uint.MaxValue))
                throw new ArgumentOutOfRangeException(nameof(Duration), d,
                    "an outline's Duration is above zero and at most uint.MaxValue milliseconds (null: until disposed)");
            _duration = value;
        }
    }

    /// <summary>KilnRenderOutline.duration_ms: 0 = until removed; a part of a millisecond counts as one.</summary>
    internal uint DurationMs => _duration is { } d ? (uint)Math.Min(uint.MaxValue, Math.Ceiling(d.TotalMilliseconds)) : 0;
}

/// <summary>Where an outline stands (<see cref="WorldOutline.State"/>; <see cref="WorldOutline.Reason"/> says why).
/// Since 0.2.0-dev.30.3 (mod API 0.2.13).</summary>
public enum OutlineState
{
    /// <summary>Accepted, not shown now: <see cref="OutlineReason.Pending"/>, <see cref="OutlineReason.Outshone"/>,
    /// <see cref="OutlineReason.NoModel"/> or <see cref="OutlineReason.SwitchedOff"/>. Since 0.2.0-dev.30.3 (mod API 0.2.13).</summary>
    Waiting = 0,

    /// <summary>The entity's model shows this outline's colour. Since 0.2.0-dev.30.3 (mod API 0.2.13).</summary>
    Shown = 1,

    /// <summary>The game's own highlight (the building cursor's) or another tool's shows on the entity instead, for now.
    /// Since 0.2.0-dev.30.3 (mod API 0.2.13).</summary>
    GameHighlight = 2,

    /// <summary>Over for good; <see cref="WorldOutline.Reason"/> says why. Since 0.2.0-dev.30.3 (mod API 0.2.13).</summary>
    Ended = 3,
}

/// <summary>Why an outline waits or ended (<see cref="WorldOutline.Reason"/>). Since 0.2.0-dev.30.3 (mod API 0.2.13).</summary>
public enum OutlineReason
{
    /// <summary>Nothing to say: shown, or the game's highlight shows. Since 0.2.0-dev.30.3 (mod API 0.2.13).</summary>
    None = 0,

    /// <summary>Waiting: not applied yet (it is at the next client step), or there is no client world now. Since 0.2.0-dev.30.3 (mod API 0.2.13).</summary>
    Pending = 1,

    /// <summary>Waiting: another outline on the entity wins (higher priority, or the same priority and newer). Since 0.2.0-dev.30.3 (mod API 0.2.13).</summary>
    Outshone = 2,

    /// <summary>Waiting: the entity has no model instance now (hidden, not streamed in, a voxel model or the terrain).
    /// Since 0.2.0-dev.30.3 (mod API 0.2.13).</summary>
    NoModel = 3,

    /// <summary>Waiting: the player turned mod outlines off (kiln.json <c>"ui": {"outlines": "off"}</c>). Since 0.2.0-dev.30.3 (mod API 0.2.13).</summary>
    SwitchedOff = 4,

    /// <summary>Ended: disposed, or <see cref="Ui.World.ClearOutlines"/>. Since 0.2.0-dev.30.3 (mod API 0.2.13).</summary>
    Removed = 16,

    /// <summary>Ended: the entity is gone from the client world. Since 0.2.0-dev.30.3 (mod API 0.2.13).</summary>
    Despawned = 17,

    /// <summary>Ended: the client world ended (the player left the game or joined another). Since 0.2.0-dev.30.3 (mod API 0.2.13).</summary>
    WorldEnded = 18,

    /// <summary>Ended: <see cref="OutlineOptions.Duration"/> ran out. Since 0.2.0-dev.30.3 (mod API 0.2.13).</summary>
    Expired = 19,

    /// <summary>Ended: the mod that asked for it unloaded. Since 0.2.0-dev.30.3 (mod API 0.2.13).</summary>
    ModUnloaded = 20,

    /// <summary>Ended: Kiln's outline mechanism stopped (a hook or its stage failed; kiln.log says why). Since 0.2.0-dev.30.3 (mod API 0.2.13).</summary>
    Failed = 21,
}

/// <summary>An outline on an entity's model in this player's game (<see cref="Ui.World.Outline"/>). Dispose it to take
/// the outline away. Since 0.2.0-dev.30.3 (mod API 0.2.13).</summary>
/// <remarks>Every member works on any thread. <see cref="State"/>, <see cref="Reason"/>, <see cref="Active"/> and
/// <see cref="ColorShared"/> ask kiln.render at each read.</remarks>
[ClientOnly]
public sealed class WorldOutline : IDisposable
{
    private readonly object _gate = new();
    private ulong _handle;
    private bool _ended;
    private OutlineReason _endReason;

    internal WorldOutline(ulong handle, EntityId entity, OutlineOptions options, ModContext? mod)
    {
        _handle = handle;
        Entity = entity;
        Options = options;
        Mod = mod;
    }

    /// <summary>The mod that asked (null: Kiln itself, a test).</summary>
    internal ModContext? Mod { get; }

    /// <summary>kiln.render's handle (0 once it ended here): the test kit's fake finds its record by it.</summary>
    internal ulong Handle
    {
        get { lock (_gate) return _handle; }
    }

    /// <summary>The outlined entity (in the client's world: the server's id of a replicated entity). Since 0.2.0-dev.30.3 (mod API 0.2.13).</summary>
    [AnyThread]
    public EntityId Entity { get; }

    /// <summary>The options it was asked with. Since 0.2.0-dev.30.3 (mod API 0.2.13).</summary>
    [AnyThread]
    public OutlineOptions Options { get; }

    /// <summary>Where it stands now (asked of kiln.render at each read); <see cref="OutlineState.Ended"/> after
    /// <see cref="Dispose"/>. Since 0.2.0-dev.30.3 (mod API 0.2.13).</summary>
    [AnyThread]
    public OutlineState State => Read().State;

    /// <summary>Why it waits or ended (<see cref="OutlineReason.None"/> while shown). Since 0.2.0-dev.30.3 (mod API 0.2.13).</summary>
    [AnyThread]
    public OutlineReason Reason => Read().Reason;

    /// <summary><see cref="State"/> is not <see cref="OutlineState.Ended"/>: it shows or may show later. Since 0.2.0-dev.30.3 (mod API 0.2.13).</summary>
    [AnyThread]
    public bool Active => Read().State != OutlineState.Ended;

    /// <summary>More than two colours are in use at once, and this one shows with the nearest other colour (the game
    /// has two highlight groups for mods). Since 0.2.0-dev.30.3 (mod API 0.2.13).</summary>
    [AnyThread]
    public bool ColorShared => Read().Merged;

    /// <summary>Takes the outline away (kiln.render gives the entity's highlight back at the next client step).
    /// Idempotent; never throws; a no-op after the outline ended or your mod unloaded. Since 0.2.0-dev.30.3 (mod API 0.2.13).</summary>
    [AnyThread]
    public void Dispose() => WorldOutlines.Remove(this, OutlineReason.Removed);

    internal (OutlineState State, OutlineReason Reason, bool Merged) Read()
    {
        ulong h;
        lock (_gate)
        {
            if (_ended) return (OutlineState.Ended, _endReason, false);
            h = _handle;
        }
        var info = WorldOutlines.Info(h);
        if (info is not { } i)
        {
            // kiln.render does not know the handle (any more): forgotten after its owner's clear, or the table is gone
            WorldOutlines.Forget(this, Mod is { Alive: false } ? OutlineReason.ModUnloaded : OutlineReason.Removed);
            return (OutlineState.Ended, EndReason, false);
        }
        var state = (OutlineState)i.State;
        var reason = (OutlineReason)i.Reason;
        if (state == OutlineState.Ended) WorldOutlines.Forget(this, reason);  // ended there: nothing to remove later
        return (state, reason, i.Merged != 0);
    }

    private OutlineReason EndReason
    {
        get { lock (_gate) return _endReason; }
    }

    /// <summary>Ends it here: the handle is forgotten. Returns the handle when this call ended it, else 0.</summary>
    internal ulong End(OutlineReason reason)
    {
        lock (_gate)
        {
            if (_ended) return 0;
            _ended = true;
            _endReason = reason;
            var h = _handle;
            _handle = 0;
            return h;
        }
    }
}

public static partial class Ui
{
    /// <summary>The game's world as this player's client draws it: outlines on entities with the game's own highlight
    /// (<see cref="Outline"/>). Client only. Since 0.2.0-dev.30.3 (mod API 0.2.13).</summary>
    /// <remarks>Not <c>Kiln.World</c> (the server's and the client's entity worlds): inside <see cref="Ui"/>'s own code
    /// the simple name <c>World</c> means this class.</remarks>
    public static partial class World
    {
        /// <summary>The capability row of <see cref="Outline"/>: a sigdb feature of the game client (kiln.render's palette
        /// hooks); kiln.render lowers it when its hooks or its stage fail, Kiln.Ui when kiln.render has no outlines (an
        /// older kiln-core). Since 0.2.0-dev.30.3 (mod API 0.2.13).</summary>
        public const string OutlineCapability = "ui.world_outline";

        /// <summary>The colour of an outline whose options name none: gold, (255, 196, 64). Since 0.2.0-dev.30.3 (mod API 0.2.13).</summary>
        public static Color DefaultOutlineColor { get; } = new(255, 196, 64);

        /// <summary>Outlines work in this process: the row <see cref="OutlineCapability"/> is on and kiln.render has its
        /// outline functions (v2). False on a dedicated server and offline. Since 0.2.0-dev.30.3 (mod API 0.2.13).</summary>
        [AnyThread]
        public static bool OutlinesAvailable => Capabilities.Is(OutlineCapability) && RenderNative.HasOutlines;

        /// <summary>Outlines <paramref name="entity"/>'s model with the game's own highlight, in this player's game, until
        /// you dispose the handle, your mod unloads, <see cref="OutlineOptions.Duration"/> runs out, the entity despawns
        /// or the client world ends. Since 0.2.0-dev.30.3 (mod API 0.2.13).</summary>
        /// <remarks>
        /// <para>The entity is one of the client world's (a replicated entity has the server's id): props, chests,
        /// stations, NPCs, creatures, dropped items, anything drawn with a model. Voxel models and the terrain cannot be
        /// outlined (they wait with <see cref="OutlineReason.NoModel"/>). The highlight is drawn with the model, so what
        /// stands in front of it hides it: there is no through-walls outline.</para>
        /// <para>Two colours (colour and pulse) show at once; a third shares the nearest one's (<see cref="WorldOutline.ColorShared"/>).
        /// On one entity the highest <see cref="OutlineOptions.Priority"/> wins, then the newest. The game's own highlight
        /// (the building cursor's) wins over mods unless the player's kiln.json says <c>"ui": {"outlines": "mods-first"}</c>;
        /// <c>"off"</c> keeps every mod outline waiting. Another tool's highlight (Greeneye's Quick Deposit and Find) always
        /// shows instead. It shows from the next client step.</para>
        /// </remarks>
        /// <param name="entity">The entity to outline (not <see cref="EntityId.None"/>).</param>
        /// <param name="options">Colour, pulse, priority and duration; null = gold, steady, priority 0, until disposed.</param>
        /// <returns>The outline's handle: its <see cref="WorldOutline.State"/>, and <see cref="WorldOutline.Dispose"/>.</returns>
        /// <exception cref="ArgumentException">Entity 0, or a colour with alpha 0.</exception>
        /// <exception cref="KilnUnavailableException">The row <see cref="OutlineCapability"/> is off (a dedicated server,
        /// a game build where the highlight was not found, an older kiln-core, offline).</exception>
        /// <exception cref="InvalidOperationException">Your mod holds 1024 outlines already, or it is unloading.</exception>
        [ClientOnly, AnyThread, RequiresCapability(OutlineCapability)]
        public static WorldOutline Outline(EntityId entity, OutlineOptions? options = null) => WorldOutlines.Add(entity, options);

        /// <summary>Takes every outline of your mod away (their handles end with <see cref="OutlineReason.Removed"/>).
        /// Never throws. Since 0.2.0-dev.30.3 (mod API 0.2.13).</summary>
        [ClientOnly, AnyThread]
        public static void ClearOutlines() => WorldOutlines.ClearOf(UiModCalls.Caller, OutlineReason.Removed);
    }
}

/// <summary>Ui.World's outlines on kiln.render v2: the live handles per mod, and their end with the mod.</summary>
internal static unsafe class WorldOutlines
{
    private static readonly ConcurrentDictionary<WorldOutline, byte> Live = new(ReferenceEqualityComparer.Instance);
    private static readonly HashSet<ModContext> Watched = new(ReferenceEqualityComparer.Instance);

    internal static WorldOutline Add(EntityId entity, OutlineOptions? options)
    {
        options ??= new OutlineOptions();
        if (entity.Value == 0) throw new ArgumentException("an entity is needed (not 0)", nameof(entity));
        if (options.Color.A == 0) throw new ArgumentException("the outline's colour has alpha 0 (it would never show)", nameof(options));
        UiValueBridge.RequireCapability(Ui.World.OutlineCapability);
        var t = RenderNative.T2;
        if (t == null)
            throw new KilnUnavailableException(Ui.World.OutlineCapability, RenderNative.Available
                ? "kiln.render has no outlines (v1: an older kiln-core)"
                : Capabilities.Why(RenderNative.Service));
        var mod = UiModCalls.Caller;
        UiModCalls.RequireAlive(mod);
        RenderNative.Outline o = new()
        {
            Size = (uint)sizeof(RenderNative.Outline), Rgba = options.Color.Rgba, Priority = options.Priority,
            Flags = options.Pulse ? RenderNative.OutlinePulse : 0, DurationMs = options.DurationMs,
        };
        ulong h = 0;
        var rc = t->AddOutline(UiValueBridge.OwnerOf(mod), entity.Value, &o, &h);
        if (rc != UiValuesNative.Ok || h == 0)
            throw rc switch
            {
                UiValuesNative.Unavailable => new KilnUnavailableException(Ui.World.OutlineCapability, Detail() is { Length: > 0 } d
                    ? $"kiln.render cannot outline now: {d}"
                    : "kiln.render cannot outline now (see kiln.log)"),
                UiValuesNative.InvalidArgument => new ArgumentException($"kiln.render refused the outline of entity {entity.Value}"),
                UiValuesNative.BufferTooSmall => new InvalidOperationException(
                    "this mod holds 1024 outlines already: dispose the ones it no longer needs (or Ui.World.ClearOutlines)"),
                UiValuesNative.Revoked => new InvalidOperationException("the mod is unloading"),
                _ => new InvalidOperationException($"kiln.render refused the outline ({rc})"),
            };
        var outline = new WorldOutline(h, entity, options, mod);
        Live[outline] = 0;
        Watch(mod);
        if (mod is { Alive: false }) Remove(outline, OutlineReason.ModUnloaded);  // it unloaded in between
        return outline;
    }

    private static string Detail() => RenderNative.ReadOutlineStatus() is { } s ? RenderNative.DetailOf(s) : "";

    internal static RenderNative.OutlineInfo? Info(ulong handle)
    {
        var t = RenderNative.T2;
        if (t == null || handle == 0) return null;
        RenderNative.OutlineInfo i = default;
        i.Size = (uint)sizeof(RenderNative.OutlineInfo);
        return t->GetOutlineInfo(handle, &i) == UiValuesNative.Ok ? i : null;
    }

    /// <summary>Ends <paramref name="outline"/> here and asks kiln.render to remove it. Never throws.</summary>
    internal static void Remove(WorldOutline outline, OutlineReason reason)
    {
        try
        {
            var h = outline.Handle;
            // it may have ended there already (despawned, expired): keep that reason
            if (h != 0 && Info(h) is { State: RenderNative.OutlineEnded } i) reason = (OutlineReason)i.Reason;
            h = outline.End(reason);
            Live.TryRemove(outline, out _);
            var t = RenderNative.T2;
            if (h != 0 && t != null) t->RemoveOutline(h);
        }
        catch (Exception e)
        {
            try { UiEngine.Log(LogLevel.Warn, $"ui.world: outline remove: {e.Message}"); }
            catch { /* nothing left to say */ }
        }
    }

    /// <summary>An outline that ended in kiln.render (or that it forgot): ended here, nothing to remove.</summary>
    internal static void Forget(WorldOutline outline, OutlineReason reason)
    {
        outline.End(reason);
        Live.TryRemove(outline, out _);
    }

    /// <summary>Ends every outline of <paramref name="mod"/> here and clears its owner in kiln.render. Never throws.</summary>
    internal static void ClearOf(ModContext? mod, OutlineReason reason)
    {
        try
        {
            var t = RenderNative.T2;
            foreach (var o in Live.Keys)
            {
                if (!ReferenceEquals(o.Mod, mod)) continue;
                var h = o.End(reason);
                Live.TryRemove(o, out _);
                if (h != 0 && t != null) t->RemoveOutline(h);  // also where the owner is shared (a mod without its own)
            }
            // the owner's clear also ends what this process does not hold (and forgets its ended requests)
            if (t != null && (mod is null || mod.Owner != 0)) t->ClearOutlines(UiValueBridge.OwnerOf(mod));
        }
        catch (Exception e)
        {
            try { UiEngine.Log(LogLevel.Warn, $"ui.world: clear outlines: {e.Message}"); }
            catch { /* nothing left to say */ }
        }
    }

    private static void Watch(ModContext? mod)
    {
        if (mod is null) return;
        lock (Watched)
            if (!Watched.Add(mod)) return;
        mod.Retiring += () =>
        {
            ClearOf(mod, OutlineReason.ModUnloaded);
            lock (Watched) Watched.Remove(mod);
        };
    }

    /// <summary>Outlines held now (tests).</summary>
    internal static int LiveCount => Live.Count;

    internal static void ResetForTests()
    {
        Live.Clear();
        lock (Watched) Watched.Clear();
    }
}
