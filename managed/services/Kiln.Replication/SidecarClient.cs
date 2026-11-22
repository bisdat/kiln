// SPDX-License-Identifier: MIT
// A Kiln client's side of the sidecars (the dossier's chapter 15 §4 M2 "Client"; DESIGN slice A4). The server's DEFS map
// its extension ids to this process's extensions (same name, version, size and layout hash; others are read past and
// dropped). Values are keyed (entity, createTick, extension): with kiln.repl.net's receive track (what this client stored
// on its client channel: creates and destroy lists) an UPDATE for the generation the client has is kept, one for an older
// generation is dropped, one for a generation the client has not created yet waits up to 5 s for its create and is then
// asked for again (RESYNC, at most one a second, 256 entities each); createTick 0 applies to the generation the client has.
// Without the receive track the newest generation the server names wins. A destroy, the entity leaving this client's view,
// a CLEAR, a DEFS change, the client world's stop and a disconnect drop values. Reads go through a view per extension,
// swapped in once per frame (TryGet from any thread); Changed is raised on the main thread. A hosted world's own client
// gets no frames: there the server's providers publish the view (SidecarServer). When this client's extensions change
// (a hot reload) it sends ANNOUNCE (message 4) with its repl.ext value. Main thread throughout.
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace Kiln;

/// <summary>The client's sidecar state machine (main thread; no game or kiln.net calls of its own).</summary>
internal sealed class SidecarClientCore
{
    public const double PendingSeconds = 5, ResyncSeconds = 1;

    private readonly record struct Def(int Size, ReplicatedExtension? Local);
    private readonly record struct Pending(uint CreateTick, byte[] Value, double Since);

    private Dictionary<ushort, Def> _defs = new();
    private readonly Dictionary<ushort, Dictionary<uint, (uint Ct, byte[] Value)>> _values = new();
    private readonly Dictionary<(uint Entity, ushort Ext), Pending> _pending = new();
    private readonly Dictionary<uint, uint> _live = new();  // entity -> the generation this client has (receive track)
    private readonly HashSet<ReplicatedExtension> _dirty = [];
    private readonly List<(ReplicatedExtension Ext, uint Entity, uint Ct, byte[]? Value)> _changes = [];
    private double _lastResync = double.NegativeInfinity;

    /// <summary>The receive track is on: generations are known.</summary>
    public bool Tracking { get; set; }

    /// <summary>DEFS came (values can be kept).</summary>
    public bool HasDefs { get; private set; }

    public int PendingCount => _pending.Count;

    /// <summary>The value this client keeps for an entity and an extension (server id), or null.</summary>
    public (uint Ct, byte[] Value)? Get(ushort ext, uint entity) =>
        _values.TryGetValue(ext, out var d) && d.TryGetValue(entity, out var v) ? v : null;

    public int SizeOf(ushort id) => _defs.TryGetValue(id, out var d) ? d.Size : -1;

    public void OnDefs(IReadOnlyList<SidecarWire.Def> defs)
    {
        var mine = Sidecars.Registered();
        var next = new Dictionary<ushort, Def>();
        foreach (var d in defs)
        {
            var local = Array.Find(mine, e => e.Name == d.Name && e.Version == d.Version && e.Size == d.Size && e.LayoutHash == d.LayoutHash);
            next[d.Id] = new Def(d.Size, local);
        }
        DropAll();  // the definitions changed: every value goes (the server sends them again under the new ids)
        _defs = next;
        HasDefs = true;
    }

    public void OnUpdate(IReadOnlyList<SidecarWire.Value> items, double now)
    {
        foreach (var it in items)
        {
            if (!_defs.TryGetValue(it.ExtId, out var def) || def.Local is null || it.Bytes.Length != def.Size) continue;
            var bytes = it.Bytes.ToArray();
            if (Tracking)
            {
                if (_live.TryGetValue(it.Entity, out var gen))
                {
                    if (it.CreateTick == 0 || it.CreateTick == gen) Store(it.ExtId, def.Local, it.Entity, gen, bytes);
                    else if (it.CreateTick > gen) Wait(it, bytes, now);
                    // else an older generation: stale, dropped
                }
                else
                {
                    Wait(it, bytes, now);
                }
                continue;
            }
            var had = Get(it.ExtId, it.Entity);
            if (had is { } h && it.CreateTick != 0 && it.CreateTick < h.Ct) continue;  // stale
            Store(it.ExtId, def.Local, it.Entity, it.CreateTick != 0 ? it.CreateTick : had?.Ct ?? 0, bytes);
        }
    }

    private void Wait(SidecarWire.Value it, byte[] bytes, double now)
    {
        var key = (it.Entity, it.ExtId);
        var since = _pending.TryGetValue(key, out var old) && old.CreateTick == it.CreateTick ? old.Since : now;
        _pending[key] = new Pending(it.CreateTick, bytes, since);
    }

    public void OnClear(IReadOnlyList<SidecarWire.Key> keys)
    {
        foreach (var k in keys)
        {
            if (_pending.TryGetValue((k.Entity, k.ExtId), out var p) && (k.CreateTick == 0 || p.CreateTick == k.CreateTick))
                _pending.Remove((k.Entity, k.ExtId));
            if (Get(k.ExtId, k.Entity) is { } v && (k.CreateTick == 0 || v.Ct == k.CreateTick)) Drop(k.ExtId, k.Entity);
        }
    }

    /// <summary>The receive track's events (kind as KILN_REPL_TRACK_*).</summary>
    public void OnTrack(byte kind, uint entity, uint tick)
    {
        switch (kind)
        {
            case ReplNetNative.TrackCreated:
                _live[entity] = tick;
                foreach (var ext in _values.Keys.ToList())
                    if (Get(ext, entity) is { } v && v.Ct != tick) Drop(ext, entity);  // an older generation of the id
                foreach (var (key, p) in _pending.Where(kv => kv.Key.Entity == entity).ToList())
                {
                    if (p.CreateTick == tick || p.CreateTick == 0)
                    {
                        _pending.Remove(key);
                        if (_defs.TryGetValue(key.Ext, out var def) && def.Local is not null) Store(key.Ext, def.Local, entity, tick, p.Value);
                    }
                    else if (p.CreateTick < tick)
                    {
                        _pending.Remove(key);
                    }
                }
                break;
            case ReplNetNative.TrackDestroyed:
            case ReplNetNative.TrackLeft:
                _live.Remove(entity);
                foreach (var ext in _values.Keys.ToList()) Drop(ext, entity);
                foreach (var key in _pending.Keys.Where(k => k.Entity == entity).ToList()) _pending.Remove(key);
                break;
            case ReplNetNative.TrackPeerGone:
                _live.Clear();
                DropAll();
                break;
        }
    }

    /// <summary>Disconnect, the client world's stop: nothing is known any more.</summary>
    public void Reset()
    {
        DropAll();
        _live.Clear();
        _defs = new();
        HasDefs = false;
    }

    /// <summary>An extension of this process went: its values go.</summary>
    public void Removed(ReplicatedExtension ext)
    {
        foreach (var (id, def) in _defs.ToList())
            if (ReferenceEquals(def.Local, ext))
            {
                _values.Remove(id);
                _defs[id] = def with { Local = null };
                foreach (var key in _pending.Keys.Where(k => k.Ext == id).ToList()) _pending.Remove(key);
            }
    }

    /// <summary>Values that waited 5 s for their create: what to ask for again (at most 256, at most once a second); they
    /// stop waiting.</summary>
    public List<SidecarWire.ResyncItem> Due(double now)
    {
        var due = new List<SidecarWire.ResyncItem>();
        if (now - _lastResync < ResyncSeconds) return due;
        foreach (var (key, p) in _pending.ToList())
        {
            if (now - p.Since < PendingSeconds) continue;
            _pending.Remove(key);
            if (due.Count < SidecarWire.MaxResync && !due.Contains(new SidecarWire.ResyncItem(key.Entity, p.CreateTick)))
                due.Add(new SidecarWire.ResyncItem(key.Entity, p.CreateTick));
        }
        if (due.Count > 0) _lastResync = now;
        return due;
    }

    /// <summary>The frame's end: the views of the extensions that changed, then Changed for each change.</summary>
    public void Publish()
    {
        foreach (var ext in _dirty)
        {
            var view = new Dictionary<uint, byte[]>();
            foreach (var (id, def) in _defs)
                if (ReferenceEquals(def.Local, ext) && _values.TryGetValue(id, out var d))
                    foreach (var (entity, v) in d) view[entity] = v.Value;
            if (ext.IsRegistered) ext.Publish(view);
        }
        _dirty.Clear();
        if (_changes.Count == 0) return;
        var changes = _changes.ToArray();
        _changes.Clear();
        foreach (var c in changes) c.Ext.RaiseChanged(new EntityId(c.Entity), c.Ct, c.Value ?? [], null);
    }

    private void Store(ushort id, ReplicatedExtension ext, uint entity, uint ct, byte[] value)
    {
        if (!_values.TryGetValue(id, out var d)) _values[id] = d = new();
        if (d.TryGetValue(entity, out var old) && old.Ct == ct && old.Value.AsSpan().SequenceEqual(value)) return;
        d[entity] = (ct, value);
        _dirty.Add(ext);
        _changes.Add((ext, entity, ct, value));
    }

    private void Drop(ushort id, uint entity)
    {
        if (!_values.TryGetValue(id, out var d) || !d.Remove(entity, out var old)) return;
        if (_defs.TryGetValue(id, out var def) && def.Local is not null)
        {
            _dirty.Add(def.Local);
            _changes.Add((def.Local, entity, old.Ct, null));
        }
    }

    private void DropAll()
    {
        foreach (var (id, d) in _values)
        {
            if (d.Count == 0) continue;
            if (_defs.TryGetValue(id, out var def) && def.Local is not null)
            {
                _dirty.Add(def.Local);
                foreach (var (entity, v) in d) _changes.Add((def.Local, entity, v.Ct, null));
            }
            d.Clear();
        }
        _pending.Clear();
    }
}

/// <summary>The sidecar client in the game: kiln.repl.net's receive track, the client's main-thread tick, kiln.net.</summary>
internal static unsafe class SidecarClient
{
    private static readonly SidecarClientCore Core = new();
    private static ulong _track;
    private static bool _trackRefused, _started, _connected;
    private static int _announced = -1;

    internal static SidecarClientCore State => Core;

    internal static void Start()
    {
        if (_started || Mods.Side != ModSide.Client) return;
        _started = true;
        var outer = ModContext.CurrentMod;
        try
        {
            ModContext.CurrentMod = null;  // Kiln's own tick, not the first mod's
            Client.Tick += Frame;
        }
        catch (Exception e)
        {
            Sidecars.Log(LogLevel.Warn, $"repl: the sidecar client could not start ({e.GetType().Name}: {e.Message})");
        }
        finally
        {
            ModContext.CurrentMod = outer;
        }
    }

    private static void Frame(in TickInfo t)
    {
        try
        {
            var connected = !Client.IsHosting && Network.ServerHasKiln;
            if (connected != _connected)
            {
                _connected = connected;
                if (!connected)
                {
                    Core.Reset();
                    _announced = -1;
                }
            }
            if (!connected)
            {
                Core.Publish();
                return;
            }
            if (_track == 0 && !_trackRefused && Sidecars.Registered().Length > 0) BeginTrack();
            Core.Tracking = _track != 0;
            var now = Environment.TickCount64 / 1000.0;
            var due = Core.Due(now);
            if (due.Count > 0 && Sidecars.Channel is { } ch && Network.Server is { } server)
            {
                var w = new NetWriter();
                SidecarWire.WriteResync(w, due);
                ch.SendRaw(server, SidecarWire.Resync, w.ToArray());
            }
            Announce();
            Core.Publish();
        }
        catch (Exception e)
        {
            Sidecars.Report(null, "the sidecar client's frame threw", e);
        }
    }

    /// <summary>The extensions changed since the handshake (a hot reload): the server hears the new list (ANNOUNCE).</summary>
    private static void Announce()
    {
        var version = Sidecars.Version;
        if (_announced < 0)
        {
            _announced = version;  // the handshake carried this list
            return;
        }
        if (version == _announced || Sidecars.Channel is not { } ch || Network.Server is not { } server || !server.HasChannel(SidecarWire.Channel)) return;
        var w = new NetWriter();
        w.String(Sidecars.AdvertisedValue());
        if (ch.SendRaw(server, SidecarWire.Announce, w.ToArray())) _announced = version;
    }

    private static void BeginTrack()
    {
        if ((ReplNetNative.Capabilities() & ReplNetNative.CapClientTrack) == 0 || ReplNetNative.T == null)
        {
            _trackRefused = true;
            Sidecars.Log(LogLevel.Info, "repl: no receive track on this client (repl.client is off): extension values follow the server's generations only");
            return;
        }
        ulong h = 0;
        var rc = ReplNetNative.T->Track(Replication.ServiceOwner, ReplNative.Receive, &OnTrack, null, &h);
        if (rc == ReplNative.Ok)
        {
            _track = h;
            return;
        }
        if (rc != ReplNative.WrongThread)
        {
            _trackRefused = true;
            Sidecars.Log(LogLevel.Warn, $"repl: the receive track was refused ({rc}): extension values follow the server's generations only");
        }
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static void OnTrack(ReplNetNative.TrackEvent* events, uint count, void* user)
    {
        try
        {
            for (var i = 0; i < count; i++) Core.OnTrack(events[i].Kind, events[i].Entity, events[i].Tick);
        }
        catch (Exception e)
        {
            Sidecars.Report(null, "the sidecar client's track handler threw", e);
        }
    }

    /// <summary>A kiln:repl message from the server (main thread).</summary>
    internal static void Received(ushort id, ReadOnlySpan<byte> body)
    {
        var r = new NetReader(body);
        switch (id)
        {
            case SidecarWire.Defs:
                Core.OnDefs(SidecarWire.ReadDefs(ref r));
                break;
            case SidecarWire.Update:
                if (!Core.HasDefs) return;
                Core.OnUpdate(SidecarWire.ReadUpdate(ref r, Core.SizeOf).Items, Environment.TickCount64 / 1000.0);
                break;
            case SidecarWire.Clear:
                Core.OnClear(SidecarWire.ReadClear(ref r).Keys);
                break;
        }
    }

    internal static void Removed(ReplicatedExtension ext) => Core.Removed(ext);
}

internal static partial class Sidecars
{
    static partial void StartParts()
    {
        SidecarServer.Start();
        SidecarClient.Start();
    }

    static partial void Received(NetContext context, ushort id, ReadOnlySpan<byte> body)
    {
        try
        {
            if (context.Sender.IsServer)
            {
                SidecarClient.Received(id, body);
                return;
            }
            switch (id)
            {
                case SidecarWire.Resync:
                    SidecarServer.OnResync(context, body);
                    break;
                case SidecarWire.Announce:
                    var r = new NetReader(body);
                    var value = r.String(64 * 1024);
                    ExtensionAdvert.Announce(context.Sender.Id, value);
                    Log(LogLevel.Info, $"repl: peer 0x{context.Sender.Id:x} announced its extensions: {(value.Length == 0 ? "none" : value)}");
                    break;
            }
        }
        catch (NetFormatException e)
        {
            Log(LogLevel.Warn, $"repl: a kiln:repl message {id} from {(context.Sender.IsServer ? "the server" : $"peer 0x{context.Sender.Id:x}")} could not be read ({e.Message}); dropped");
        }
    }

    static partial void Removed(ReplicatedExtension ext) => SidecarClient.Removed(ext);

    static partial void ProvidersMoved(ReplicatedExtension ext) => SidecarServer.ProvidersChanged();
}
