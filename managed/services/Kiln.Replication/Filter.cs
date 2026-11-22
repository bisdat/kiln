// SPDX-License-Identifier: MIT
// Kiln.Replication's per-peer filters and touch over kiln.repl.net (native/core/abi/services/kiln_repl.h; the dossier's
// chapter 15 §4 M3, §5.2; DESIGN slices B3). A filter edits one component as one peer is sent it; Kiln runs it on the
// game's replication job threads through an unmanaged trampoline, so a filter body must not allocate or call Kiln APIs.
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace Kiln;

public static partial class Replication
{
    /// <summary>
    /// Edits component <typeparamref name="TComponent"/> as the server sends it to the peers <paramref name="peers"/>
    /// selects: <paramref name="filter"/> gets that peer's copy of the component in its snapshot and may change it; the
    /// client then has exactly the edited value (the game codes its delta from it and keeps it as that peer's baseline).
    /// Other peers get the game's value. The selector is asked again every step, so a peer moves in or out by itself.
    /// </summary>
    /// <remarks>
    /// <para>The body runs on the game's replication job threads, in parallel per peer and channel: read the value and
    /// <see cref="FilterContext"/> only; no allocation, no Kiln API, no locks. An exception keeps the game's value for that
    /// send and removes the filter (logged once). Keep values inside the game's own ranges: a vanilla client cannot
    /// follow an invented enum value.</para>
    /// <para>The client channel re-sends an entity only when the game changes it. Kiln re-sends the affected entities
    /// when the filter is added, removed or its peers change; a value that depends on other components of the same
    /// entity names them in <see cref="FilterOptions.Sources"/>; anything else calls <see cref="Touch"/>.</para>
    /// </remarks>
    /// <returns>Dispose it to remove the filter (the next send has the game's value again); it also goes when your mod
    /// unloads.</returns>
    /// <exception cref="ArgumentException">The component does not travel on the selected channels, the prediction channel
    /// without <see cref="FilterOptions.AllowPrediction"/>, more than 8 sources, an unknown source.</exception>
    /// <exception cref="KilnUnavailableException"><c>repl.filter</c> is off (or <c>repl.touch</c>, with sources on the
    /// client channel).</exception>
    [ServerOnly]
    [TickThreadOnly]
    [RequiresCapability(ReplicationCaps.Filter)]
    public static IDisposable Filter<TComponent>(PeerSelector peers, ComponentFilter<TComponent> filter, FilterOptions? options = null)
        where TComponent : unmanaged, IGameComponent<TComponent>
    {
        Services.RequireTickThread("Replication.Filter");
        ArgumentNullException.ThrowIfNull(peers);
        ArgumentNullException.ThrowIfNull(filter);
        options ??= new FilterOptions();
        if (options.Channels == 0 || (options.Channels & ~(ReplChannels.Client | ReplChannels.Prediction)) != 0)
            throw new ArgumentException("no channel", nameof(options));
        if ((options.Channels & ReplChannels.Prediction) != 0 && !options.AllowPrediction)
            throw new ArgumentException("a filter on the prediction channel needs AllowPrediction", nameof(options));
        if (options.Sources.Count > 8) throw new ArgumentException("at most 8 sources", nameof(options));
        return FilterHost.Add(OwnerOfCaller(), ModContext.CurrentMod, ComponentHashOf<TComponent>.Hash, TComponent.QualifiedName,
            Unsafe.SizeOf<TComponent>(), peers, filter, FilterHost.InvokerOf<TComponent>(), options);
    }

    /// <summary>
    /// Re-sends <paramref name="entity"/> on the client channel without changing a byte: the next sim step marks it, and
    /// that frame's send snapshots it again for every peer (each through its filters). Use it when a filter's output
    /// changes for a reason Kiln cannot see (another entity, a setting).
    /// </summary>
    /// <returns>Ok when queued; otherwise why not ("entity gone", "no carrier component").</returns>
    /// <exception cref="ArgumentException"><paramref name="entity"/> is <see cref="EntityId.None"/>.</exception>
    /// <exception cref="KilnUnavailableException"><c>repl.touch</c> is off.</exception>
    [ServerOnly]
    [TickThreadOnly]
    [RequiresCapability(ReplicationCaps.Touch)]
    public static unsafe TouchResult Touch(EntityId entity)
    {
        Services.RequireTickThread("Replication.Touch");
        if (entity.IsNone) throw new ArgumentException("no entity", nameof(entity));
        var t = ReplNetNative.T;
        if (t == null || (t->Capabilities() & ReplNetNative.CapTouch) == 0)
            throw ReplicationCaps.Off(ReplicationCaps.Touch, "kiln.repl.net cannot touch on this server");
        uint status = 0;
        var rc = t->Touch(entity.Value, &status);
        if (rc == ReplNative.Unavailable) throw ReplicationCaps.Off(ReplicationCaps.Touch, "kiln.repl.net refused the touch");
        var result = rc != ReplNative.Ok ? new TouchResult(false, $"kiln.repl.net refused it ({rc})") { Entity = entity }
            : status switch
            {
                ReplNetNative.TouchQueued => new TouchResult(true, null) { Entity = entity },
                ReplNetNative.TouchNoCarrier => new TouchResult(false, "no carrier component") { Entity = entity },
                _ => new TouchResult(false, "entity gone") { Entity = entity },
            };
        Touched?.Invoke(result);
        return result;
    }

    /// <summary>Raised on the tick thread for every <see cref="Touch"/>, with its result.</summary>
    public static event Action<TouchResult>? Touched;

    /// <summary>
    /// Hides Kiln-only items from the peers <paramref name="peers"/> selects, besides the ones Kiln always hides them
    /// from (vanilla clients and Kiln clients without the server's content): each Kiln-only item id the server would
    /// send them becomes the item's look-alike (<c>VanillaLook</c>, else the game item it is based on) or an empty slot.
    /// </summary>
    /// <returns>Dispose it to stop (Kiln's own rule stays).</returns>
    /// <exception cref="KilnUnavailableException"><c>repl.itemremap</c> is off.</exception>
    [ServerOnly]
    [TickThreadOnly]
    [RequiresCapability(ReplicationCaps.ItemRemap)]
    public static IDisposable RemapItems(PeerSelector peers)
    {
        Services.RequireTickThread("Replication.RemapItems");
        ArgumentNullException.ThrowIfNull(peers);
        if ((ReplNetNative.Capabilities() & ReplNetNative.CapRemap) == 0)
            throw ReplicationCaps.Off(ReplicationCaps.ItemRemap, "kiln.repl.net has no item remap here");
        return ItemRemap.AddSelector(peers);
    }

    /// <summary>What the item remap did since the server started.</summary>
    [ServerOnly]
    [AnyThread]
    public static ReplRemapStats RemapStats() => ItemRemap.Stats();
}

/// <summary>The managed side of the filters: the registrations, their peers per step, the job-thread trampoline.</summary>
internal static unsafe class FilterHost
{
    internal delegate void Invoker(Box box, byte* scratch, ReplNetNative.FilterCtx* ctx);

    internal sealed class Box : IDisposable
    {
        public required Delegate Body;
        public required Invoker Invoke;
        public required uint Size;
        public required string Component;
        public required PeerSelector Peers;
        public required ulong Owner;
        public ulong Handle;
        public GCHandle Self;
        public uint[] Last = [];
        public volatile Exception? Fault;
        public bool Disposed;

        public void Dispose()
        {
            lock (Gate)
            {
                if (Disposed) return;
                Disposed = true;
                if (Handle != 0) ReplNetNative.Remove(Handle);
                Live.Remove(this);
                // the native set may still call it until its next Pre: the handle goes two steps later
                Retiring.Add((this, Step + 2));
            }
        }
    }

    private static readonly object Gate = new();
    private static readonly List<Box> Live = [];
    private static readonly List<(Box Box, ulong FreeAt)> Retiring = [];
    private static ulong Step;
    private static IDisposable? _tick;
    internal const int Machines = 17;  // KILN_REPL_MACHINES
    private static NetPeer?[] _byMachine = new NetPeer?[Machines];

    public static Invoker InvokerOf<T>() where T : unmanaged => Invoke<T>;

    private static void Invoke<T>(Box box, byte* scratch, ReplNetNative.FilterCtx* ctx) where T : unmanaged =>
        ((ComponentFilter<T>)box.Body)(ref Unsafe.AsRef<T>(scratch), new FilterContext(ctx));

    /// <summary>The peer of a machine as the last step saw it (job threads read it; the tick thread replaces the array).</summary>
    public static NetPeer? PeerOfMachine(byte machine)
    {
        var a = Volatile.Read(ref _byMachine);
        return machine < a.Length ? a[machine] : null;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static void Trampoline(byte* scratch, uint size, ReplNetNative.FilterCtx* ctx, void* user)
    {
        Box? box = null;
        try
        {
            box = (Box)GCHandle.FromIntPtr((nint)user).Target!;
            if (size != box.Size || box.Disposed)
            {
                if (ctx->Failed != null) *ctx->Failed = 1;
                return;
            }
            box.Invoke(box, scratch, ctx);
        }
        catch (Exception e)
        {
            if (box != null) box.Fault ??= e;
            if (ctx->Failed != null) *ctx->Failed = 1;
        }
    }

    public static IDisposable Add(ulong owner, ModContext? mod, uint hash, string component, int size, PeerSelector peers, Delegate body,
        Invoker invoke, FilterOptions options)
    {
        var t = ReplNetNative.T;
        if (t == null || (t->Capabilities() & ReplNetNative.CapFilter) == 0)
            throw ReplicationCaps.Off(ReplicationCaps.Filter, "kiln.repl.net has no filters here");
        var channels = ((options.Channels & ReplChannels.Client) != 0 ? ReplNetNative.ClientChannel : 0u) |
                       ((options.Channels & ReplChannels.Prediction) != 0 ? ReplNetNative.PredictionChannel : 0u);
        var sources = new uint[options.Sources.Count];
        for (var i = 0; i < sources.Length; i++)
        {
            var s = options.Sources[i];
            if (string.IsNullOrWhiteSpace(s)) throw new ArgumentException("an empty source", nameof(options));
            if (Replication.Component(s) is null) throw new ArgumentException($"no component {s} in this world", nameof(options));
            sources[i] = ReplNative.ComponentHash(s);
        }
        if (Replication.Component(component) is { } info)
        {
            if (info.Size != size) throw new ArgumentException($"{component} is {info.Size} bytes in this game build, the struct {size}", nameof(body));
            if ((options.Channels & ReplChannels.Client) != 0 && !info.MayBeOnClientChannel)
                throw new ArgumentException($"{component} does not travel on the client channel", nameof(options));
            if ((options.Channels & ReplChannels.Prediction) != 0 && !info.MayBeOnPredictionChannel)
                throw new ArgumentException($"{component} does not travel on the prediction channel", nameof(options));
        }
        var box = new Box { Body = body, Invoke = invoke, Size = (uint)size, Component = component, Peers = peers, Owner = owner };
        box.Self = GCHandle.Alloc(box);
        var initial = PeersOf(peers);
        ulong h = 0;
        int rc;
        fixed (uint* p = initial)
        fixed (uint* src = sources)
            rc = t->FilterAdd(owner, hash, initial.Length == 0 ? null : p, (uint)initial.Length, channels, options.Priority,
                sources.Length == 0 ? null : src, (uint)sources.Length, &Trampoline, (void*)GCHandle.ToIntPtr(box.Self), &h);
        if (rc != ReplNative.Ok)
        {
            box.Self.Free();
            if (rc == ReplNative.Unavailable)
                throw ReplicationCaps.Off(sources.Length > 0 ? ReplicationCaps.Touch : ReplicationCaps.Filter, "kiln.repl.net refused the filter");
            throw new ArgumentException($"kiln.repl.net refused a filter on {component} ({rc}): the component does not travel on " +
                "the selected channels, or 256 filters (16 on one component) are in place");
        }
        box.Handle = h;
        box.Last = initial;
        lock (Gate)
        {
            Live.Add(box);
            _tick ??= ServiceTick.Add("kiln replication filters", static (in TickInfo _) => OnTick());
        }
        if (mod is not null) mod.Retiring += box.Dispose;
        return box;
    }

    /// <summary>The service tick that resolves the filters' peers and runs the item remap (once).</summary>
    internal static void EnsureTick()
    {
        lock (Gate) _tick ??= ServiceTick.Add("kiln replication filters", static (in TickInfo _) => OnTick());
    }

    /// <summary>The peer ids a selector picks now; [0] (no peer has id 0) when it picks none, [] when it picks every one.</summary>
    internal static uint[] PeersOf(PeerSelector selector)
    {
        if (ReferenceEquals(selector, Peers.All)) return [];
        Span<uint> ids = stackalloc uint[Machines];
        Span<byte> flags = stackalloc byte[Machines];
        if (ReplNetNative.MachinePeers(ids, flags) != ReplNative.Ok) return [0];
        var net = NetPeersById();
        var list = new List<uint>();
        for (var m = 0; m < ids.Length; m++)
        {
            if (ids[m] == 0) continue;
            if (m == 0 && (flags[m] & ReplNative.PeerLocal) == 0) continue;  // a dedicated server's machine 0: itself, sent nothing
            if (selector.Matches(PeerFacts.Of(ids[m], flags[m], net))) list.Add(ids[m]);
        }
        return list.Count == 0 ? [0] : list.ToArray();
    }

    internal static Dictionary<uint, NetPeer> NetPeersById()
    {
        var d = new Dictionary<uint, NetPeer>();
        try
        {
            foreach (var p in Network.Peers) d[p.Id] = p;
        }
        catch (Exception)
        {
            // kiln.net not here: only fakes and the local client
        }
        return d;
    }

    private static void OnTick()
    {
        bool any;
        lock (Gate) any = Live.Count > 0 || Retiring.Count > 0;
        if (!any)
        {
            ItemRemap.Tick();
            return;
        }
        // the machines' peers for FilterContext.Peer
        Span<uint> ids = stackalloc uint[Machines];
        Span<byte> flags = stackalloc byte[Machines];
        var byMachine = new NetPeer?[Machines];
        if (ReplNetNative.MachinePeers(ids, flags) == ReplNative.Ok)
        {
            var net = NetPeersById();
            for (var m = 0; m < ids.Length; m++) byMachine[m] = ids[m] == 0 ? null : PeerFacts.Of(ids[m], flags[m], net).Peer;
        }
        Volatile.Write(ref _byMachine, byMachine);
        List<Box> live;
        lock (Gate)
        {
            Step++;
            for (var i = Retiring.Count - 1; i >= 0; i--)
                if (Retiring[i].FreeAt <= Step)
                {
                    if (Retiring[i].Box.Self.IsAllocated) Retiring[i].Box.Self.Free();
                    Retiring.RemoveAt(i);
                }
            live = [.. Live];
        }
        foreach (var box in live)
        {
            if (box.Fault is { } e)
            {
                ReplNetLog.Warn($"repl: a filter on {box.Component} threw {e.GetType().Name}: {e.Message}; it was removed");
                box.Dispose();
                continue;
            }
            var now = PeersOf(box.Peers);
            if (now.AsSpan().SequenceEqual(box.Last)) continue;
            box.Last = now;
            var t = ReplNetNative.T;
            if (t == null) continue;
            fixed (uint* p = now) t->FilterSetPeers(box.Handle, now.Length == 0 ? null : p, (uint)now.Length);
        }
        ItemRemap.Tick();
    }
}
