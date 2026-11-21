// SPDX-License-Identifier: MIT
// The server's side of the sidecars (the dossier's chapter 15 §4 M2 "Which entities, for whom", "When"; DESIGN slice A3).
// kiln.repl.net's track (track.cpp) says, per machine, which entities the peer was sent a create of (and their
// generation: the create's server tick) and which it lost (destroy lists A and B); SidecarServerCore keeps that per peer,
// runs each extension's providers once per step for the entities tracked for peers that have the extension, and queues an
// UPDATE when a value differs from the last one that peer got (a CLEAR when it went), sent within the peer's budget
// (network.json "repl": {"peerBudget": bytes per second}, 32 KiB/s by default; what does not fit waits, the newest value
// wins). A peer is re-read from its state map (peer_entities) when it is first seen, after a GAP and when an extension
// comes late; createTick 0 = created before the tracking began. DEFS go to a Kiln peer after its handshake and whenever an
// extension comes or goes (its values are then sent again under the new ids). Every 5 s a safety net compares the cached
// component tests with World.Find. A vanilla peer, a lab fake no kiln.net peer stands for, and a peer without kiln:repl
// never get a frame; a hosted world's own client (machine 0) gets none either: the providers' values are published for
// TryGet in this process and Changed is posted to the client world's thread. Tick thread throughout.
using System.Buffers.Binary;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text.Json.Nodes;

namespace Kiln;

/// <summary>A machine's peer as the sidecar server keeps it.</summary>
internal sealed class SidecarPeer(uint id)
{
    public readonly uint Id = id;
    public byte Machine, Flags;
    public PeerFacts Facts;
    public readonly Dictionary<uint, uint> Tracked = new();                       // entity -> createTick
    public readonly Dictionary<(uint Entity, ushort Ext), byte[]> Sent = new();    // what the peer has now
    public readonly Dictionary<(uint Entity, ushort Ext), Queued> Queue = new();   // what waits for the budget
    public readonly HashSet<ushort> Has = [];                                      // extensions it has (ids)
    public double Tokens = -1, Refilled;
    public int DefsVersion = -1;
    public bool Reseed = true;
    public double LastResync = double.NegativeInfinity;
    public long FramesSent;                                                        // kiln:repl messages to it

    /// <summary>It may be sent frames: a Kiln client with kiln:repl (not the local client, not an unbound fake).</summary>
    public bool Frames => !Facts.Local && Facts.Kiln && Facts.Peer!.HasChannel(SidecarWire.Channel);
}

/// <summary>A value (or a CLEAR: Value null) waiting for the peer's budget.</summary>
internal readonly record struct Queued(uint CreateTick, byte[]? Value, long Order);

/// <summary>What the core needs from the game and kiln.net (the tests fake it).</summary>
internal interface ISidecarHost
{
    /// <summary>The entity has the component (World.Has).</summary>
    bool Has(uint entity, string component);

    /// <summary>The peer's state map with the generations tracking knows (peer_entities); null when unreadable.</summary>
    (uint Entity, uint CreateTick)[]? PeerEntities(uint peer);

    /// <summary>Every entity with the component (World.Find), for the safety net; null when it cannot say.</summary>
    IReadOnlyCollection<uint>? Find(string component);

    /// <summary>Sends a kiln:repl message to the peer; false when kiln.net did not take it.</summary>
    bool Send(SidecarPeer peer, ushort message, byte[] body);

    /// <summary>The peer as a selector sees it now.</summary>
    PeerFacts Resolve(uint peer, byte flags);

    /// <summary>Changed for the hosted world's own client: run on the client world's thread.</summary>
    void PostToClient(Action work);

    int MaxMessageBytes { get; }

    void Log(LogLevel level, string line);
}

/// <summary>The server's sidecar state machine (tick thread; no game calls of its own: <see cref="ISidecarHost"/>).</summary>
internal sealed class SidecarServerCore(ISidecarHost host)
{
    public const int DefaultBudget = 32 * 1024;
    public const double SafetyNetSeconds = 5, ResyncSeconds = 1;

    private readonly Dictionary<uint, SidecarPeer> _peers = new();
    private readonly Dictionary<(ushort Ext, uint Entity), int> _match = new();      // the provider an entity's archetype picks (-1: none)
    private readonly Dictionary<ushort, Dictionary<uint, byte[]>> _views = new();     // per extension: the values given last
    private readonly Dictionary<(ushort, uint), (bool Ok, byte[]? Value)> _step = new();  // this step's values (Ok false: the provider threw)
    private long _order;
    private double _safety = double.NegativeInfinity;
    private int _providersVersion = -1, _seenVersion = -1;

    /// <summary>Bytes per second per peer.</summary>
    public int Budget { get; set; } = DefaultBudget;

    public IReadOnlyDictionary<uint, SidecarPeer> PeerStates => _peers;

    /// <summary>The providers came or went: the component tests are made again.</summary>
    public void ProvidersChanged() => _providersVersion++;

    private SidecarPeer Peer(uint id, byte machine, byte flags)
    {
        if (!_peers.TryGetValue(id, out var p)) _peers[id] = p = new SidecarPeer(id);
        p.Machine = machine;
        p.Flags = flags;
        return p;
    }

    /// <summary>A machine that has a peer now (kiln.repl's machine table): known from here on, re-read from its map.</summary>
    public void Seen(uint id, byte machine, byte flags)
    {
        if (id != 0 && !_peers.ContainsKey(id)) Peer(id, machine, flags);
    }

    /// <summary>One Pre's events from kiln.repl.net's track, oldest first.</summary>
    public void OnEvents(ReadOnlySpan<ReplNetNative.TrackEvent> events)
    {
        foreach (ref readonly var e in events)
        {
            if (e.Kind == ReplNetNative.TrackPeerGone)
            {
                _peers.Remove(e.Peer);
                ExtensionAdvert.Forget(e.Peer);
                continue;
            }
            if (e.Peer == 0) continue;
            var p = Peer(e.Peer, e.Machine, e.PeerFlags);
            switch (e.Kind)
            {
                case ReplNetNative.TrackCreated:
                    if (p.Tracked.TryGetValue(e.Entity, out var old) && old != e.Tick) Forget(p, e.Entity, clear: false);
                    p.Tracked[e.Entity] = e.Tick;
                    ForgetMatch(e.Entity);
                    break;
                case ReplNetNative.TrackDestroyed:
                case ReplNetNative.TrackLeft:
                    if (p.Tracked.Remove(e.Entity, out var ct)) Forget(p, e.Entity, clear: true, ct);
                    break;
                case ReplNetNative.TrackGap:
                    p.Reseed = true;
                    break;
            }
        }
    }

    /// <summary>The entity's values on that peer go: CLEAR for each it was sent (a new generation needs none: the client
    /// drops the old one when it creates the new one).</summary>
    private void Forget(SidecarPeer p, uint entity, bool clear, uint createTick = 0)
    {
        foreach (var ext in Sidecars.Registered())
        {
            var key = (entity, ext.Id);
            p.Queue.Remove(key);
            if (!p.Sent.ContainsKey(key)) continue;
            if (clear) p.Queue[key] = new Queued(createTick, null, _order++);
            else p.Sent.Remove(key);
        }
    }

    private void ForgetMatch(uint entity)
    {
        foreach (var ext in Sidecars.Registered()) _match.Remove((ext.Id, entity));
    }

    /// <summary>A RESYNC from a Kiln client (at most one a second is honoured): its values of those generations go out again.</summary>
    public bool OnResync(uint peer, IReadOnlyList<SidecarWire.ResyncItem> items, double now)
    {
        if (!_peers.TryGetValue(peer, out var p) || now - p.LastResync < ResyncSeconds) return false;
        p.LastResync = now;
        foreach (var it in items)
        {
            if (!p.Tracked.ContainsKey(it.Entity)) continue;
            foreach (var ext in Sidecars.Registered()) p.Sent.Remove((it.Entity, ext.Id));
        }
        return true;
    }

    /// <summary>One step: peers re-read where needed, the extensions each peer has, the providers' values, the frames.</summary>
    public void Step(double now, uint serverTick)
    {
        var exts = Sidecars.Registered();
        var version = Sidecars.Version;
        if (_providersVersion != _seenVersion || version != _lastDefs)
        {
            _match.Clear();
            _seenVersion = _providersVersion;
        }
        if (version != _lastDefs)
        {
            foreach (var id in _views.Keys.ToList())
                if (Array.Find(exts, e => e.Id == id) is null) _views.Remove(id);
            _lastDefs = version;
        }
        _step.Clear();
        foreach (var p in _peers.Values)
        {
            p.Facts = host.Resolve(p.Id, p.Flags);
            if (p.Reseed) Reseed(p);
            UpdateHas(p, exts);
            if (p.Frames && p.Has.Count > 0 && p.DefsVersion != version) SendDefs(p, version);  // none to a peer without any
            foreach (var ext in exts)
            {
                if (!p.Has.Contains(ext.Id) || ext.Providers.Count == 0) continue;
                foreach (var (entity, ct) in p.Tracked)
                {
                    var key = (entity, ext.Id);
                    if (!Value(ext, entity, out var v)) continue;  // the provider faulted: keep what the peer has
                    var had = p.Sent.TryGetValue(key, out var sent);
                    if (v is not null)
                    {
                        if (had && sent!.AsSpan().SequenceEqual(v)) p.Queue.Remove(key);
                        else p.Queue[key] = new Queued(ct, v, p.Queue.TryGetValue(key, out var q) ? q.Order : _order++);
                    }
                    else if (had) p.Queue[key] = new Queued(ct, null, p.Queue.TryGetValue(key, out var q) ? q.Order : _order++);
                    else p.Queue.Remove(key);
                }
            }
            if (p.Facts.Local) DeliverLocal(p);
            else if (p.Frames && p.DefsVersion == version) Flush(p, now, serverTick);
            else p.Queue.Clear();  // nobody to send to: a vanilla peer or an unbound fake never gets a frame
        }
        Publish(exts);
        if (now - _safety >= SafetyNetSeconds)
        {
            _safety = now;
            SafetyNet(exts);
        }
    }

    private int _lastDefs = -1;

    private void Reseed(SidecarPeer p)
    {
        var map = host.PeerEntities(p.Id);
        if (map is null) return;
        p.Reseed = false;
        var now = new Dictionary<uint, uint>(map.Length);
        foreach (var (entity, ct) in map)
            now[entity] = ct != 0 ? ct : p.Tracked.GetValueOrDefault(entity);
        foreach (var (entity, ct) in p.Tracked)
            if (!now.ContainsKey(entity)) Forget(p, entity, clear: true, ct);
        p.Tracked.Clear();
        foreach (var (entity, ct) in now) p.Tracked[entity] = ct;
    }

    private void UpdateHas(SidecarPeer p, ReplicatedExtension[] exts)
    {
        foreach (var ext in exts)
        {
            var has = ext.IsRegistered && (p.Facts.Local || (p.Facts.Kiln && ext.AvailableFor(p.Facts.Peer!)));
            if (has == p.Has.Contains(ext.Id)) continue;
            if (has)
            {
                p.Has.Add(ext.Id);
                if (!p.Facts.Local) host.Log(LogLevel.Info, $"repl: peer 0x{p.Id:x} has {ext}");
            }
            else
            {
                p.Has.Remove(ext.Id);
                foreach (var key in p.Sent.Keys.Where(k => k.Ext == ext.Id).ToList()) p.Sent.Remove(key);
                foreach (var key in p.Queue.Keys.Where(k => k.Ext == ext.Id).ToList()) p.Queue.Remove(key);
                if (!p.Facts.Local) host.Log(LogLevel.Info, $"repl: peer 0x{p.Id:x} lost {ext}");
            }
        }
        // an id that is no extension any more (disposed, hot reload)
        p.Has.RemoveWhere(id => Array.Find(exts, e => e.Id == id) is null);
    }

    private void SendDefs(SidecarPeer p, int version)
    {
        var w = new NetWriter();
        SidecarWire.WriteDefs(w, Sidecars.Defs());
        if (!host.Send(p, SidecarWire.Defs, w.ToArray())) return;
        p.FramesSent++;
        p.DefsVersion = version;
        // the client drops every value when the definitions change: all of them go out again under the new ids
        p.Sent.Clear();
        p.Queue.Clear();
    }

    /// <summary>The entity's value of the extension this step (once per step for every peer); false when the provider
    /// threw (nothing changes on any peer).</summary>
    private bool Value(ReplicatedExtension ext, uint entity, out byte[]? value)
    {
        if (_step.TryGetValue((ext.Id, entity), out var known))
        {
            value = known.Value;
            return known.Ok;
        }
        var providers = ext.Providers;
        if (!_match.TryGetValue((ext.Id, entity), out var pick))
        {
            pick = -1;
            for (var i = 0; i < providers.Count && pick < 0; i++)
                if (host.Has(entity, providers[i].ComponentName)) pick = i;
            _match[(ext.Id, entity)] = pick;
        }
        value = null;
        if (pick >= 0 && pick < providers.Count)
        {
            var buf = new byte[ext.Size];
            var provider = providers[pick];
            var outer = ModContext.CurrentMod;
            try
            {
                ModContext.CurrentMod = provider.Mod;
                if (provider.Provide(new EntityId(entity), buf)) value = buf;
            }
            catch (Exception e)
            {
                if (Interlocked.Increment(ref provider.Faults) == 1)
                    Sidecars.Report(provider.Mod, $"a provider of extension {ext} on {provider.ComponentName} threw (entity {entity}); its values stay as they were", e);
                _step[(ext.Id, entity)] = (false, null);
                return false;
            }
            finally
            {
                ModContext.CurrentMod = outer;
            }
        }
        _step[(ext.Id, entity)] = (true, value);
        return true;
    }

    /// <summary>The hosted world's own client: no frames; what changed is its new state, Changed on its thread.</summary>
    private void DeliverLocal(SidecarPeer p)
    {
        if (p.Queue.Count == 0) return;
        var changes = new List<(ReplicatedExtension Ext, uint Entity, uint Ct, byte[]? Value)>();
        foreach (var (key, q) in p.Queue)
        {
            if (q.Value is null) p.Sent.Remove(key);
            else p.Sent[key] = q.Value;
            if (Sidecars.ById(key.Ext) is { } ext) changes.Add((ext, key.Entity, q.CreateTick, q.Value));
        }
        p.Queue.Clear();
        host.PostToClient(() =>
        {
            foreach (var c in changes) c.Ext.RaiseChanged(new EntityId(c.Entity), c.Ct, c.Value ?? [], null);
        });
    }

    /// <summary>The queue within the peer's budget: UPDATEs and CLEARs of at most 4,096 items and the channel's message size.</summary>
    private void Flush(SidecarPeer p, double now, uint serverTick)
    {
        if (p.Tokens < 0)
        {
            p.Tokens = Budget;
            p.Refilled = now;
        }
        else
        {
            p.Tokens = Math.Min(Budget, p.Tokens + Budget * Math.Max(0, now - p.Refilled));
            p.Refilled = now;
        }
        if (p.Queue.Count == 0) return;
        var maxBody = Math.Clamp(host.MaxMessageBytes, 1024, 60 * 1024) - 64;
        var updates = new List<SidecarWire.Value>();
        var clears = new List<SidecarWire.Key>();
        var sentItems = new List<((uint Entity, ushort Ext) Key, Queued Q)>();
        int updateBytes = SidecarWire.Header, clearBytes = SidecarWire.Header;
        foreach (var (key, q) in p.Queue.OrderBy(kv => kv.Value.Order).ToList())
        {
            var cost = q.Value is null ? SidecarWire.ClearItemBytes : SidecarWire.UpdateItemBytes(q.Value.Length);
            if (cost > p.Tokens) break;  // the rest waits for the next step (the newest value wins)
            p.Tokens -= cost;
            if (q.Value is null)
            {
                if (clears.Count >= SidecarWire.MaxItems || clearBytes + cost > maxBody) { SendClears(p, serverTick, clears); clearBytes = SidecarWire.Header; }
                clears.Add(new SidecarWire.Key(key.Entity, q.CreateTick, key.Ext));
                clearBytes += cost;
            }
            else
            {
                if (updates.Count >= SidecarWire.MaxItems || updateBytes + cost > maxBody) { SendUpdates(p, serverTick, updates); updateBytes = SidecarWire.Header; }
                updates.Add(new SidecarWire.Value(key.Entity, q.CreateTick, key.Ext, q.Value));
                updateBytes += cost;
            }
            sentItems.Add((key, q));
        }
        SendUpdates(p, serverTick, updates);
        SendClears(p, serverTick, clears);
        foreach (var (key, q) in sentItems)
        {
            p.Queue.Remove(key);
            if (q.Value is null) p.Sent.Remove(key);
            else p.Sent[key] = q.Value;
            if (Sidecars.ById(key.Ext) is { } ext) ext.RaiseChanged(new EntityId(key.Entity), q.CreateTick, q.Value ?? [], p.Facts.Peer);
        }
    }

    private void SendUpdates(SidecarPeer p, uint tick, List<SidecarWire.Value> items)
    {
        if (items.Count == 0) return;
        var w = new NetWriter();
        SidecarWire.WriteUpdate(w, tick, items);
        if (host.Send(p, SidecarWire.Update, w.ToArray())) p.FramesSent++;
        items.Clear();
    }

    private void SendClears(SidecarPeer p, uint tick, List<SidecarWire.Key> keys)
    {
        if (keys.Count == 0) return;
        var w = new NetWriter();
        SidecarWire.WriteClear(w, tick, keys);
        if (host.Send(p, SidecarWire.Clear, w.ToArray())) p.FramesSent++;
        keys.Clear();
    }

    /// <summary>The values given this step become each extension's view (TryGet on the server and the host's client).</summary>
    private void Publish(ReplicatedExtension[] exts)
    {
        foreach (var ext in exts)
        {
            if (ext.Providers.Count == 0) continue;
            var view = new Dictionary<uint, byte[]>();
            foreach (var ((id, entity), v) in _step)
                if (id == ext.Id && v.Value is not null) view[entity] = v.Value;
            if (_views.TryGetValue(ext.Id, out var old) && Same(old, view)) continue;
            _views[ext.Id] = view;
            ext.Publish(view);
        }
    }

    private static bool Same(Dictionary<uint, byte[]> a, Dictionary<uint, byte[]> b)
    {
        if (a.Count != b.Count) return false;
        foreach (var (k, v) in a)
            if (!b.TryGetValue(k, out var w) || !v.AsSpan().SequenceEqual(w)) return false;
        return true;
    }

    /// <summary>Every 5 s: the cached component tests of tracked entities against World.Find; mismatches are logged and
    /// made again.</summary>
    private void SafetyNet(ReplicatedExtension[] exts)
    {
        foreach (var ext in exts)
        {
            var providers = ext.Providers;
            if (providers.Count == 0) continue;
            var sets = new IReadOnlyCollection<uint>?[providers.Count];
            for (var i = 0; i < providers.Count; i++) sets[i] = host.Find(providers[i].ComponentName);
            if (Array.Exists(sets, s => s is null)) continue;
            var wrong = 0;
            foreach (var ((id, entity), pick) in _match.ToList())
            {
                if (id != ext.Id) continue;
                var actual = -1;
                for (var i = 0; i < providers.Count && actual < 0; i++)
                    if (sets[i]!.Contains(entity)) actual = i;
                if (actual == pick) continue;
                wrong++;
                _match.Remove((id, entity));
            }
            if (wrong > 0) host.Log(LogLevel.Warn, $"repl: sidecar safety net: {wrong} tracked entities had a stale component test for {ext}; tested again");
        }
    }
}

/// <summary>The sidecar server in the game: kiln.repl.net's track, the service tick, kiln.net (tick thread).</summary>
internal static unsafe class SidecarServer
{
    private static SidecarServerCore? _core;
    private static ulong _track;
    private static bool _refused;
    private static IDisposable? _tick;
    private static DateTime _configAt;
    private static DateTime _configStamp;
    private static double _machinesAt = double.NegativeInfinity;

    private const int Machines = 17;  // KILN_REPL_MACHINES

    internal static SidecarServerCore? Core => _core;

    /// <summary>The first extension: the service tick drives the server's part from here on.</summary>
    internal static void Start()
    {
        if (_tick is not null) return;
        _tick = ServiceTick.Add("kiln.replication.sidecars", static (in TickInfo t) => Step(in t));
    }

    private static void Step(in TickInfo t)
    {
        var exts = Sidecars.Registered();
        if (exts.Length == 0)
        {
            Stop();
            return;
        }
        if (_core is null && !Begin()) return;
        var now = Environment.TickCount64 / 1000.0;
        ReadBudget();
        if (now - _machinesAt >= 1)
        {
            _machinesAt = now;
            Span<uint> peers = stackalloc uint[Machines];
            Span<byte> flags = stackalloc byte[Machines];
            if (ReplNetNative.MachinePeers(peers, flags) == ReplNative.Ok)
                for (var m = 0; m < Machines; m++) _core!.Seen(peers[m], (byte)m, flags[m]);
        }
        _core!.Step(now, (uint)t.Step);
    }

    /// <summary>The track: on a server or a hosted world whose kiln.repl.net can track (said once when it cannot).</summary>
    private static bool Begin()
    {
        if (_refused || (ReplNetNative.Capabilities() & ReplNetNative.CapTrack) == 0 || ReplNetNative.T == null) return false;
        ulong h = 0;
        var rc = ReplNetNative.T->Track(Replication.ServiceOwner, ReplNative.Send, &OnTrack, null, &h);
        if (rc != ReplNative.Ok)
        {
            if (rc != ReplNative.WrongThread)
            {
                _refused = true;
                Sidecars.Log(LogLevel.Warn, $"repl: sidecar tracking refused ({rc}); extension values do not travel from this server");
            }
            return false;
        }
        _track = h;
        _core = new SidecarServerCore(new Host());
        ReadBudget(force: true);
        Sidecars.Log(LogLevel.Info, $"repl: sidecar server on ({Sidecars.Registered().Length} extension(s), budget {_core.Budget} B/s per peer)");
        return true;
    }

    private static void Stop()
    {
        if (_track != 0) ReplNetNative.Remove(_track);
        _track = 0;
        _core = null;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static void OnTrack(ReplNetNative.TrackEvent* events, uint count, void* user)
    {
        try
        {
            _core?.OnEvents(new ReadOnlySpan<ReplNetNative.TrackEvent>(events, (int)count));
        }
        catch (Exception e)
        {
            Sidecars.Report(null, "the sidecar server's track handler threw", e);
        }
    }

    internal static void ProvidersChanged() => _core?.ProvidersChanged();

    internal static void OnResync(NetContext ctx, ReadOnlySpan<byte> body)
    {
        if (_core is null) return;
        var r = new NetReader(body);
        _core.OnResync(ctx.Sender.Id, SidecarWire.ReadResync(ref r), Environment.TickCount64 / 1000.0);
    }

    /// <summary>network.json "repl": {"peerBudget": bytes per second} (read at the start and when the file changes).</summary>
    private static void ReadBudget(bool force = false)
    {
        if (_core is null) return;
        var nowUtc = DateTime.UtcNow;
        if (!force && nowUtc - _configAt < TimeSpan.FromSeconds(5)) return;
        _configAt = nowUtc;
        try
        {
            var path = NetworkConfig.FilePath;
            if (path is null || !File.Exists(path))
            {
                _core.Budget = SidecarServerCore.DefaultBudget;
                return;
            }
            var stamp = File.GetLastWriteTimeUtc(path);
            if (!force && stamp == _configStamp) return;
            _configStamp = stamp;
            var node = JsonNode.Parse(File.ReadAllText(path), documentOptions: new() { CommentHandling = System.Text.Json.JsonCommentHandling.Skip, AllowTrailingCommas = true });
            var b = node?["repl"]?["peerBudget"];
            _core.Budget = b is JsonValue v && v.TryGetValue<int>(out var n) && n >= 1024 ? n : SidecarServerCore.DefaultBudget;
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or System.Text.Json.JsonException)
        {
            _core.Budget = SidecarServerCore.DefaultBudget;
        }
    }

    private sealed class Host : ISidecarHost
    {
        public bool Has(uint entity, string component) => World.Has(new EntityId(entity), component);

        public (uint Entity, uint CreateTick)[]? PeerEntities(uint peer)
        {
            var t = ReplNetNative.T;
            if (t == null) return null;
            var buf = new ReplNetNative.PeerEntity[256];
            for (var attempt = 0; attempt < 4; attempt++)
            {
                uint n = 0;
                int rc;
                fixed (ReplNetNative.PeerEntity* p = buf) rc = t->PeerEntities(peer, p, (uint)buf.Length, &n);
                if (rc == ReplNative.BufferTooSmall)
                {
                    buf = new ReplNetNative.PeerEntity[Math.Max(n + 64, (uint)buf.Length * 2)];
                    continue;
                }
                if (rc != ReplNative.Ok) return null;
                var list = new (uint, uint)[n];
                for (var i = 0; i < n; i++) list[i] = (buf[i].Entity, buf[i].CreateTick);
                return list;
            }
            return null;
        }

        public IReadOnlyCollection<uint>? Find(string component)
        {
            try
            {
                return World.Find(component).Select(e => e.Value).ToHashSet();
            }
            catch (Exception e) when (e is InvalidOperationException or NotSupportedException or ArgumentException)
            {
                return null;
            }
        }

        public bool Send(SidecarPeer peer, ushort message, byte[] body) =>
            Sidecars.Channel is { } ch && peer.Facts.Peer is { } p && ch.SendRaw(p, message, body);

        private Dictionary<uint, NetPeer>? _table;
        private ulong _tableStep;

        public PeerFacts Resolve(uint peer, byte flags)
        {
            if (_table is null || _tableStep != ServiceTick.Step)
            {
                _table = Replication.PeerTable();
                _tableStep = ServiceTick.Step;
            }
            return PeerFacts.Of(peer, flags, _table);
        }

        public void PostToClient(Action work)
        {
            try
            {
                Client.Post(work);
            }
            catch (Exception e) when (e is InvalidOperationException or NotSupportedException)
            {
                // no client world: nobody to tell
            }
        }

        public int MaxMessageBytes => Sidecars.Channel?.MaxMessageBytes ?? 60 * 1024;

        public void Log(LogLevel level, string line) => Sidecars.Log(level, line);
    }
}
