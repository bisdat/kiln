// SPDX-License-Identifier: MIT
// Kiln.ModValues on the wire: channel kiln:mod-values on both sides, one ServerSession per live association whose
// client has the channel (a lab peer's too), the hosted game's own player through the same reducers in process, and
// the client's ClientStream with its carry offer from the CharacterSync sidecar. A vanilla client, or one without the
// channel, gets no frame.
namespace Kiln;

internal static class ModValuesStream
{
    private static NetChannel? _channel;
    private static Ledger? _ledger;
    private static int _defsVersion = -1;
    private static readonly Dictionary<(uint Peer, CharacterKey Character), Session> Sessions = [];
    private static readonly Queue<object> ToLocalClient = new(), ToLocalServer = new();
    private const uint LocalPeer = uint.MaxValue;

    private sealed class Session(ServerSession s, NetPeer? peer)
    {
        public readonly ServerSession S = s;
        public readonly NetPeer? Peer = peer;
        public long Frames;
    }

    /// <summary>Frames sent per association (labs and tests).</summary>
    internal static IReadOnlyDictionary<string, long> FramesSent =>
        Sessions.ToDictionary(kv => $"{(kv.Key.Peer == LocalPeer ? "local" : $"0x{kv.Key.Peer:x}")}/{kv.Key.Character}", kv => kv.Value.Frames);

    internal static void Start(Ledger ledger)
    {
        _ledger = ledger;
        if (!Network.Available) return;
        // six types, this exact order, on both sides (chapter 05b section 4.4)
        _channel = Network.Channel(ModValuesWire.Channel, ModValuesWire.Version, required: false, maxMessageBytes: ModValuesWire.MaxMessageBytes)
            .RegisterJson(ModValuesWire.Info<ModValuesWire.Hello>(), (m, c) => ModValuesClient.OnHello(m, c.Sender), NetDirection.ToClient)
            .RegisterJson(ModValuesWire.Info<ModValuesWire.StatePart>(), (m, _) => ModValuesClient.OnPart(m), NetDirection.ToClient)
            .RegisterJson(ModValuesWire.Info<ModValuesWire.Ack>(), (m, c) => OnFromClient(m, c.Sender), NetDirection.ToServer)
            .RegisterJson(ModValuesWire.Info<ModValuesWire.Resync>(), (m, c) => OnFromClient(m, c.Sender), NetDirection.ToServer)
            .RegisterJson(ModValuesWire.Info<ModValuesWire.CarryOfferPart>(), (m, c) => OnFromClient(m, c.Sender), NetDirection.ToServer)
            .RegisterJson(ModValuesWire.Info<ModValuesWire.CarryResult>(), (m, _) => ModValuesClient.OnResult(m), NetDirection.ToClient);
    }

    internal static bool Send(NetPeer? peer, object m)
    {
        if (peer is null)
        {
            // the hosted game's own player: the same reducers, in process (delivered after this step's pumps)
            (peer is null && m is ModValuesWire.Ack or ModValuesWire.Resync or ModValuesWire.CarryOfferPart ? ToLocalServer : ToLocalClient).Enqueue(m);
            return true;
        }
        if (_channel is null) return false;
        return m switch
        {
            ModValuesWire.Hello h => _channel.SendTo(peer, h),
            ModValuesWire.StatePart p => _channel.SendTo(peer, p),
            ModValuesWire.Ack a => _channel.SendTo(peer, a),
            ModValuesWire.Resync r => _channel.SendTo(peer, r),
            ModValuesWire.CarryOfferPart o => _channel.SendTo(peer, o),
            ModValuesWire.CarryResult x => _channel.SendTo(peer, x),
            _ => false,
        };
    }

    internal static void MarkDirty(CharacterKey c, ModValueKey key)
    {
        foreach (var s in Sessions.Values)
            if (s.S.Character == c) s.S.MarkDirty(key.ToString());
    }

    internal static void DefinitionsChanged()
    {
        // the server tick sees the registry's version and rotates every stream (a definition or policy change)
    }

    /// <summary>Server step: sessions follow the live associations; each one pumps.</summary>
    internal static void ServerTick()
    {
        if (_ledger is null) return;
        var world = _ledger.Store.World;
        var now = DateTime.UtcNow;
        var want = new Dictionary<(uint, CharacterKey), (NetPeer? Peer, bool Local)>();
        if (world != Guid.Empty)
            foreach (var l in CharacterSync.Live())
            {
                if (l.Key.IsAccountOnly || l.Key.IsNone) continue;
                if (l.Local) want[(LocalPeer, l.Key)] = (null, true);
                // a player's peer must still be that player's (an entity id can be reused by the next player); a lab
                // peer has no player and is bound to its session
                else if (l.Peer is { State: NetPeerState.Ready } p && p.HasChannel(ModValuesWire.Channel) && (l.Lab || p.Player == l.Entity))
                    want[(p.Id, l.Key)] = (p, false);
            }
        foreach (var k in Sessions.Keys.ToArray())
            if (!want.ContainsKey(k) || Sessions[k].S.World != world)
            {
                if (k.Peer == LocalPeer) ToLocalClient.Enqueue(ClearLocal.Instance);
                Sessions.Remove(k);
            }
        var defs = ModValuesService.Registry.Version;
        var rotate = defs != _defsVersion;
        _defsVersion = defs;
        foreach (var (k, (peer, local)) in want)
        {
            if (Sessions.TryGetValue(k, out var have))
            {
                if (rotate) Out(have, have.S.Rotate());
                continue;
            }
            var s = Sessions[k] = new Session(new ServerSession(k.Item2, world, now), peer);
            ModValuesService.Log(LogLevel.Info, $"mod-values: stream for character {k.Item2} ({(local ? "this machine's player" : peer!.ToString())}), nonce {s.S.Nonce}");
            Out(s, s.S.Hello());
        }
        foreach (var s in Sessions.Values)
        {
            var c = s.S.Character;
            s.S.Pump(now, () => _ledger.Visible(c), m => Out(s, m));
        }
        while (ToLocalServer.TryDequeue(out var m)) OnFromClient(m, null);
        while (ToLocalClient.TryDequeue(out var m))
        {
            if (m is ClearLocal) ModValuesClient.Clear();
            else ModValuesClient.OnLocal(m);
        }
    }

    private sealed class ClearLocal
    {
        public static readonly ClearLocal Instance = new();
    }

    private static void Out(Session s, object m)
    {
        if (Send(s.Peer, m)) s.Frames++;
    }

    private static Session? Find(NetPeer? from, string nonce)
    {
        var id = from?.Id ?? LocalPeer;
        foreach (var (k, s) in Sessions)
            if (k.Peer == id && s.S.Nonce == nonce) return s;
        return null;
    }

    internal static void OnFromClient(object m, NetPeer? from)
    {
        switch (m)
        {
            case ModValuesWire.Ack a when Find(from, a.Nonce) is { } s: s.S.OnAck(a); break;
            case ModValuesWire.Resync r when Find(from, r.Nonce) is { } s: s.S.OnResync(r); break;
            case ModValuesWire.CarryOfferPart o when Find(from, o.Nonce) is { } s:
                var doc = s.S.OnOfferPart(o, DateTime.UtcNow, x => Out(s, x));
                if (doc is not null) Out(s, s.S.OfferDone(o.Transfer, o.Sha256, Import(s.S.Character, doc)));
                break;
        }
    }

    /// <summary>A client's carried offer: the server's import setting, CharacterImportingEvent (the reserved keys only)
    /// and ModValueImportingEvent first, then the ledger's maximum merge.</summary>
    internal static ImportSummary Import(CharacterKey c, byte[] doc)
    {
        var entries = ServerSession.ParseOffer(doc);
        var sidecarRevision = ServerSession.ParseOfferRevision(doc);
        var entity = CharacterSync.Live().FirstOrDefault(l => l.Key == c).Entity;
        static ImportSummary Refused(string why, int n) => new(false, why, 0, 0, n, new Dictionary<string, int>());
        if (entries is null) return Refused("malformed", 0);
        if (_ledger is null || !ModValuesService.IsLive(c)) return Refused(ModValueReasons.CharacterUnavailable, entries.Count);
        if (!CharacterSettings.Load().ImportProgress) return Refused(ModValueReasons.ImportRefused, entries.Count);
        var keys = entries.Select(e => ModValueKey.TryParse(e.Key, out var k) ? k : default).Where(k => k.Mod is not null).ToList();
        if (Events.HasSubscribers<CharacterImportingEvent>())
        {
            var mods = keys.Select(k => k.Mod).Distinct(StringComparer.Ordinal)
                .ToDictionary(m => m, _ => (IReadOnlyList<string>)[SavesValueStore.ProjectionKey], StringComparer.Ordinal);
            var ev = Events.Post(new CharacterImportingEvent(c, entity, sidecarRevision, CharacterStore.RevisionOf(c), mods));
            if (ev.Canceled) return Refused(ModValueReasons.ImportRefused, entries.Count);
        }
        if (new BusValueEvents().Importing(new ModValueImportingEvent(c, keys)) is not null) return Refused(ModValueReasons.ImportRefused, entries.Count);
        var s = _ledger.Import(c, entries);
        ModValuesService.Log(LogLevel.Info, $"mod-values: carry offer of character {c}: applied {s.Applied}, unchanged {s.Unchanged}, rejected {s.Rejected}" +
                                            (s.ReasonCounts.Count > 0 ? $" ({string.Join(", ", s.ReasonCounts.Select(kv => $"{kv.Key} {kv.Value}"))})" : ""));
        return s;
    }
}

/// <summary>The client's copy (both a remote server's stream and a hosted game's own).</summary>
internal static class ModValuesClient
{
    private static readonly ClientStream Stream = new(() => DateTime.UtcNow, Reply, new BusSyncedEvents());
    private static NetPeer? _server;
    private static bool _local;

    internal static SyncedState State => Stream.State;

    private static void Reply(object m)
    {
        if (_local) ModValuesStream.Send(null, m);
        else if (_server is not null) ModValuesStream.Send(_server, m);
    }

    internal static void OnHello(ModValuesWire.Hello h, NetPeer? server)
    {
        _server = server;
        _local = server is null;
        Stream.OnHello(h);
        if (h.Status != ModValuesWire.Ready || !CharacterKey.TryParse(h.Character, out var c) || c.Id == Guid.Empty) return;
        var sidecar = CharacterSidecar.Load(c.Id);
        var projections = sidecar.Mods.Where(kv => kv.Value.ContainsKey(SavesValueStore.ProjectionKey))
            .ToDictionary(kv => kv.Key, kv => kv.Value[SavesValueStore.ProjectionKey], StringComparer.Ordinal);
        foreach (var part in Stream.Offer(sidecar.Revision, projections)) Reply(part);
    }

    internal static void OnPart(ModValuesWire.StatePart p) => Stream.OnPart(p);

    internal static void OnResult(ModValuesWire.CarryResult r) =>
        ModValuesService.Log(LogLevel.Info, $"mod-values: the server {(r.Ok ? "took" : "did not take")} this character's carried values: applied {r.Applied}, unchanged {r.Unchanged}, rejected {r.Rejected}");

    internal static void OnLocal(object m)
    {
        switch (m)
        {
            case ModValuesWire.Hello h: OnHello(h, null); break;
            case ModValuesWire.StatePart p: OnPart(p); break;
            case ModValuesWire.CarryResult r: OnResult(r); break;
        }
    }

    internal static void Clear() => Stream.Clear();

    /// <summary>The client's step: leaving the server's world clears the copy before anything can show it again.</summary>
    internal static void ClientTick()
    {
        if (Stream.State.Nonce is null) return;
        if (_local)
        {
            // the host's own copy: this process runs no world any more (back in the menu), so nothing of it stays readable
            Stream.Clear();
            _local = false;
            return;
        }
        if (_server is null || Network.Server is null || !ReferenceEquals(Network.Server, _server) && Network.Server.Id != _server.Id) Stream.Clear();
    }

    internal static ModValueReadResult Read(string? reader, ModValueKey key) => Read(Stream.State, reader, key);

    internal static ModValueReadResult Read(SyncedState s, string? reader, ModValueKey key)
    {
        var k = key.ToString();
        if (s.Nonce is null || !s.Available || !s.Definitions.TryGetValue(k, out var d)) return new(false, ModValueReasons.NotSynced, null);
        var allowed = reader == key.Mod || d.Sync == (uint)ModValueSyncPolicy.Readers &&
            (d.ReadPolicy == (uint)ModValueReadPolicy.Public || d.ReadPolicy == (uint)ModValueReadPolicy.AllowList && reader is not null && d.ReadMods.Contains(reader));
        if (!allowed) return new(false, ModValueReasons.AccessDenied, null);
        return s.Snapshot(k) is { } v ? new(true, null, v) : new(false, ModValueReasons.NotSynced, null);
    }

    internal static ModValueDefinition? Definition(ModValueKey key)
    {
        if (!Stream.State.Definitions.TryGetValue(key.ToString(), out var d)) return null;
        // the stream sends what a display needs: no bounds, initial or thresholds (those stay on the server)
        return new ModValueDefinition(key, (ModValueKind)d.Kind, new UiText(d.Label.ToDictionary()), new UiText(d.NumberFormat.ToDictionary()), 0, 0, 0,
            (int)d.Scale, [], (ModValuePersistence)d.Persistence, d.Version, d.CarryEpoch, (ModValueReadPolicy)d.ReadPolicy, d.ReadMods.ToArray(),
            (ModValueSyncPolicy)d.Sync, true);
    }
}

internal sealed class BusSyncedEvents : ISyncedEvents
{
    public void Synced(ModValueSyncedEvent e)
    {
        if (Events.HasSubscribers<ModValueSyncedEvent>()) Events.Post(e);
    }

    public void Published(ModValuesSnapshotPublishedEvent e)
    {
        if (Events.HasSubscribers<ModValuesSnapshotPublishedEvent>()) Events.Post(e);
    }
}
