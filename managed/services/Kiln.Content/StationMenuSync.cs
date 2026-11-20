// SPDX-License-Identifier: MIT
// The channel kiln:station-menu v1 (chapter 16a; dev31 out/16a-station-tabs/DESIGN.md sections 3, 4, 6). Server role: a
// full snapshot of the effective station menus to every Kiln client that declared the channel, on join and after every
// change; an admission decider (Kiln.Network's controller) that holds joining Kiln clients until the mods' menus are
// declared and refuses a Kiln client without the channel while any menu exists (a client without Kiln is not concerned:
// it never gets a mod's own station). Client role: stages the parts, checks the hash, waits for the content state the
// snapshot names, swaps the server's collection in one native call, and acks Stored then Applied. Started by
// Kiln.Content's service start, next to ContentSync. docs/kiln/sync.md "Station menus".
using System.IO.Compression;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;

namespace Kiln;

internal static class StationMenuSync
{
    public const string ChannelName = "kiln:station-menu";
    public const string ChannelVersion = "1";
    internal const ushort Format = 1;
    internal const byte CodecDeflate = 1;
    internal const ushort MsgPart = 0, MsgAck = 1;
    internal const int PartBytes = 96 * 1024;
    internal const int MaxParts = 4096;
    internal const long MaxCompressed = 65L << 20, MaxRaw = 64L << 20;
    internal static readonly TimeSpan ReceiptDeadline = TimeSpan.FromSeconds(30), ContentDeadline = TimeSpan.FromSeconds(120);
    internal const byte StageStored = 0, StageApplied = 1, StageRefused = 2;
    /// <summary>What an older Kiln client is told (chapter 16a).</summary>
    internal const string RefusalText = "station-menu protocol1 required";
    internal const string Holder = "kiln.stationmenus";

    /// <summary>Message 0, server to client: part <c>Part</c> (0-based) of <c>Parts</c> of the deflated snapshot.</summary>
    internal sealed record MenuPartMsg(ushort Format, string StateId, string Session, ulong Generation, uint RawLength, ushort Part,
        ushort Parts, byte Codec, byte[] Data)
    {
        public void Encode(NetWriter w) =>
            w.UInt16(Format).String(StateId).String(Session).UInt64(Generation).UInt32(RawLength).UInt16(Part).UInt16(Parts).Byte(Codec).Bytes(Data);

        public static MenuPartMsg Decode(ref NetReader r) =>
            new(r.UInt16(), r.String(64), r.String(36), r.UInt64(), r.UInt32(), r.UInt16(), r.UInt16(), r.Byte(), r.Bytes(PartBytes));
    }

    /// <summary>Message 1, client to server: what the client did with a snapshot (Stage 0 Stored, 1 Applied, 2 Refused).</summary>
    internal sealed record MenuAckMsg(string StateId, string Session, ulong Generation, byte Stage, string Reason)
    {
        public void Encode(NetWriter w) => w.String(StateId).String(Session).UInt64(Generation).Byte(Stage).String(Reason);

        public static MenuAckMsg Decode(ref NetReader r) => new(r.String(64), r.String(36), r.UInt64(), r.Byte(), r.String(4096));
    }

    private static NetChannel? _channel;
    private static int _started;
    private static ulong _owner;

    private static void Log(LogLevel level, string text)
    {
        try { Backend.Current.Log(0, level, "station menus: " + text); } catch { }
    }

    // ------------------------------------------------------------------------------------------------ start ----

    /// <summary>Started by Kiln.Content's initialisation (service autostart). Idempotent.</summary>
    internal static void Start()
    {
        if (Interlocked.CompareExchange(ref _started, 1, 0) != 0) return;
        try
        {
            var prev = ModContext.CurrentMod;
            ModContext.CurrentMod = null;  // Kiln's own handlers
            try
            {
                Network.Disconnected += _ => _leavePending = true;
                Network.ServerReady += OnServerReady;
            }
            finally
            {
                ModContext.CurrentMod = prev;
            }
            try { _owner = Backend.Current.OwnerCreate("station-menu-sync"); }
            catch (Exception e) { Log(LogLevel.Warn, $"no owner for the server's menus: {e.Message}"); }
            StationMenuCaps.Rows.Ensure();
            ServiceTick.Add("kiln station menus", Tick);
        }
        catch (Exception e)
        {
            Log(LogLevel.Warn, $"unavailable: {e.Message}");
        }
    }

    /// <summary>The channel is open in this process: a server always (it serves and reads acks), a game client only while
    /// it can draw what it receives (stationmenus.render), so it never claims a protocol it cannot follow.</summary>
    internal static bool ChannelOpen => _channel != null || TestChannelOpen;

    /// <summary>Tests: behave as if the channel were open.</summary>
    internal static bool TestChannelOpen;

    private static bool WantChannel()
    {
        if (!StationMenuStore.Available) return false;
        if (!Kiln.Client.IsClient) return true;
        return StationMenuStore.Backend.Info() is { Render: 1 };
    }

    private static void EnsureChannel()
    {
        var want = WantChannel();
        if (want == (_channel != null)) return;
        var prev = ModContext.CurrentMod;
        ModContext.CurrentMod = null;
        try
        {
            if (want)
            {
                _channel = Network.Channel(ChannelName, ChannelVersion, required: false, maxMessageBytes: PartBytes + 4096)
                    .Register<MenuPartMsg>(MsgPart, static (m, w) => m.Encode(w), static (ref NetReader r) => MenuPartMsg.Decode(ref r), OnPart, NetDirection.ToClient)
                    .Register<MenuAckMsg>(MsgAck, static (m, w) => m.Encode(w), static (ref NetReader r) => MenuAckMsg.Decode(ref r), OnAck, NetDirection.ToServer);
                Log(LogLevel.Info, $"channel {ChannelName} v{ChannelVersion} open");
            }
            else
            {
                // capability lost: the channel closes (Kiln.Network announces it again), the server's menus are taken off
                _channel!.Dispose();
                _channel = null;
                ClientLeave("this game can no longer draw station menus");
                Log(LogLevel.Warn, $"channel {ChannelName} closed: stationmenus.render is off");
            }
        }
        catch (Exception e) when (e is InvalidOperationException or ArgumentException or NotSupportedException)
        {
            Log(LogLevel.Warn, $"channel {ChannelName}: {e.Message}");
        }
        finally
        {
            ModContext.CurrentMod = prev;
        }
    }

    private static void Tick(in TickInfo tick)
    {
        if (tick.Step % 25 == 3) EnsureChannel();
        if (_leavePending) ClientLeave("left the server");
        if (tick.Step % 5 == 0)
        {
            ClientTick();
            DrainApplied();
            ServerTick(tick.Step);
        }
    }

    // ------------------------------------------------------------------------------------------------ server ---

    /// <summary>The server role: a dedicated server, or a game client while it hosts.</summary>
    internal static bool ServerRole => !Kiln.Client.IsClient || Kiln.Client.IsHosting;

    private sealed class Snapshot
    {
        public required string StateId;
        public required ulong Generation;
        public required uint ContentStateId;
        public required int RawLength;
        public required byte[][] Parts;
        public required int Menus;
    }

    private static Snapshot? _snapshot;
    private static volatile bool _dirty = true;
    private static Guid _session = Guid.NewGuid();
    private static uint _serverWorld;
    private static bool _serving;
    private static readonly Dictionary<uint, string> Sent = new();                      // peer -> state id it has whole
    private static readonly Dictionary<uint, (string StateId, int Next)> Sending = new();
    private const int PartsPerTick = 16;

    /// <summary>A fresh server session per authoritative world start.</summary>
    internal static Guid Session => _session;

    /// <summary>MenuDeclarationsReady: the mods' OnLoad pass and the blocks pass are done and inline menus were resolved
    /// once (or nothing of Kiln.Blocks runs).</summary>
    private static volatile bool _blocksDeclared;
    internal static Func<bool>? TestDeclared;

    internal static bool Declared
    {
        get
        {
            if (TestDeclared is { } t) return t();
            if (!Lifecycle.ModsLoaded) return false;
            if (_blocksDeclared) return true;
            // no Kiln.Blocks in this process and no blocks/ folder that would start it: nothing can define a menu at start
            return !AppDomain.CurrentDomain.GetAssemblies().Any(a => a.GetName().Name == "Kiln.Blocks") &&
                   !Mods.All.Any(m => m.IsLoaded && m.Directory.Length > 0 && Directory.Exists(Path.Combine(m.Directory, "blocks")));
        }
    }

    /// <summary>Kiln.Blocks' part of MenuDeclarationsReady: the blocks pass ran and inline menus were resolved once.</summary>
    internal static void BlocksDeclared()
    {
        if (_blocksDeclared) return;
        _blocksDeclared = true;
        Log(LogLevel.Info, "the mods' station menus are declared");
    }

    /// <summary>A Kiln peer declared the channel at exactly version 1 (its hello).</summary>
    internal static bool Declares(NetPeer p) => p.Declared.TryGetValue(ChannelName, out var v) && v == ChannelVersion;

    internal static bool MenusExist => StationMenuStore.Count > 0;

    /// <summary>Server: whether a definition may be published now (null) or why not (a stable reason word first). Tick
    /// thread. A game client that serves nobody publishes locally and is never refused here.</summary>
    internal static string? CheckPublish(bool first)
    {
        if (TestPublishCheck is { } test) return test(first);
        if (!ServerRole) return null;
        if (!SyncSettings.Current.SendContent)
            return $"{StationMenuStore.ContentSyncDisabled}: this server does not send its content (sendContent is off in kiln/config/sync.json), so clients would not have the recipes a menu lists";
        if (NetNative.Available && !NetAdmission.CanHoldKilnPeers)
            return $"{StationMenuStore.NoHoldGate}: kiln.net cannot hold joining Kiln clients on this build (net.gate.hold_all), so a client without the menu protocol could not be stopped";
        // only those in the world: a joining one is still decided by the admission decider (refused, never kicked)
        var lacking = LackingPeers().Where(NetAdmission.InWorld).ToList();
        if (lacking.Count > 0)
            return $"{StationMenuStore.PeersLack}: connected Kiln clients without the station-menu protocol: {string.Join(", ", lacking.Take(8).Select(p => $"{p.Name} (0x{p.Id:x})"))}";
        if (!ChannelOpen)
            return $"{StationMenuStore.Refused}: this game cannot serve station menus (channel {ChannelName} is closed: stationmenus.render is off)";
        return null;
    }

    internal static Func<bool, string?>? TestPublishCheck;

    /// <summary>Connected Kiln clients (their hello passed) that do not declare the channel; never the host's own game.</summary>
    internal static List<NetPeer> LackingPeers()
    {
        if (!NetNative.Available && NetEngine.TestServerRole is null) return [];
        return NetEngine.ServerPeers().Where(p => !NetAdmission.IsLocalMachine(p) && p.State == NetPeerState.Ready && !Declares(p)).ToList();
    }

    /// <summary>The local collection changed: a new snapshot at the next server tick.</summary>
    internal static void Published() => _dirty = true;

    // ---- admission ----

    private static IDisposable? _decider;
    private static readonly Dictionary<uint, AdmissionHold> DeclHolds = new();

    /// <summary>The decider lives while it has something to decide: menus exist, or they are not declared yet. Any decider
    /// puts the gate in "hold all", which a server without menus has no need for.</summary>
    private static void EnsureDecider()
    {
        var need = ServerRole && NetNative.Available && (MenusExist || !Declared);
        if (need && _decider is null)
        {
            if (!NetAdmission.CanHoldKilnPeers) return;  // registration refuses with NoHoldGate instead
            try
            {
                _decider = NetAdmission.OnJoin(Holder, Decide);
            }
            catch (KilnUnavailableException e)
            {
                Log(LogLevel.Warn, $"no admission decider ({e.Message})");
            }
        }
        else if (!need && _decider is not null)
        {
            _decider.Dispose();
            _decider = null;
        }
    }

    /// <summary>A Kiln client's hello passed: held while the menus are not declared yet; refused when menus exist and it
    /// lacks the channel; else let in.</summary>
    internal static AdmissionHold? Decide(NetPeer p)
    {
        if (!Declared)
        {
            var hold = NetAdmission.HoldPeer(p, Holder, "the server is still reading its mods' station menus", NetAdmission.MaxDeadline);
            if (hold.Ended) return null;
            if (DeclHolds.Remove(p.Id, out var old)) old.End();
            DeclHolds[p.Id] = hold;
            return hold;
        }
        if (MenusExist && !Declares(p))
        {
            NetAdmission.RefusePeer(p, RefusalText, Holder);
            return null;
        }
        return null;
    }

    private static void AdmissionTick()
    {
        EnsureDecider();
        if (DeclHolds.Count > 0 && Declared)
        {
            foreach (var (id, hold) in DeclHolds.ToArray())
            {
                DeclHolds.Remove(id);
                if (hold.Ended) continue;
                if (MenusExist && hold.Peer is { } p && !Declares(p)) hold.Refuse(RefusalText);
                else hold.End();
            }
        }
        foreach (var (id, hold) in DeclHolds.ToArray())
            if (hold.Ended) DeclHolds.Remove(id);
        if (!MenusExist) return;
        // a client in the world that announced its channels again without this one (its render went off): refused, as at join
        foreach (var p in LackingPeers())
            if (p.KickAt is null && p.State != NetPeerState.Rejected)
                NetAdmission.RefusePeer(p, RefusalText, Holder);
    }

    // ---- snapshot ----

    private static void ServerTick(ulong step)
    {
        var role = ServerRole;
        var world = Kiln.Client.IsClient ? Kiln.Client.ServerWorldSession : 0u;
        if (!role || world != _serverWorld)
        {
            if (_serving || world != _serverWorld) ResetServer();
            _serverWorld = world;
            _serving = role;
            if (!role)
            {
                _decider?.Dispose();
                _decider = null;
                return;
            }
        }
        _serving = true;
        if (!NetNative.Available && NetEngine.TestServerRole is null) return;
        AdmissionTick();
        if (_channel is null) return;
        var peers = NetEngine.ServerPeers();
        lock (Sent)
        {
            foreach (var gone in Sent.Keys.Where(k => !peers.Any(p => p.Id == k)).ToList()) Sent.Remove(gone);
            foreach (var gone in Sending.Keys.Where(k => !peers.Any(p => p.Id == k)).ToList()) Sending.Remove(gone);
        }
        if (!peers.Any(p => p.State == NetPeerState.Ready && p.HasChannel(ChannelName) && !NetAdmission.IsLocalMachine(p))) return;
        if (_dirty || _snapshot is null) Build();
        if (_snapshot is not { } s) return;
        lock (Sent)
            foreach (var p in peers)
            {
                if (p.State != NetPeerState.Ready || !p.HasChannel(ChannelName) || NetAdmission.IsLocalMachine(p)) continue;
                if (Sent.TryGetValue(p.Id, out var have) && have == s.StateId) continue;
                if (Send(p, s)) Sent[p.Id] = s.StateId;
            }
    }

    private static void ResetServer()
    {
        lock (Sent)
        {
            Sent.Clear();
            Sending.Clear();
        }
        _snapshot = null;
        _dirty = true;
        _session = Guid.NewGuid();
        foreach (var h in DeclHolds.Values) h.End();
        DeclHolds.Clear();
    }

    /// <summary>The snapshot from one stable content generation: the menu export and the content state read with the
    /// generations checked before and after; on a change it is tried again at the next tick.</summary>
    private static void Build()
    {
        var st0 = Content.Status();
        var gen0 = StationMenuStore.Generation;
        var serving = ContentSync.Serving;
        var export = StationMenuStore.Backend.Export();
        var st1 = Content.Status();
        if (export is null || st0?.Generation != st1?.Generation || gen0 != StationMenuStore.Generation) return;
        if (!TryMenus(export, out var generation, out var menusRaw, out var count, out var why))
        {
            Log(LogLevel.Warn, $"the local menus could not be exported: {why}");
            return;
        }
        var contentState = count == 0 ? 0u : serving?.StateId ?? 0u;
        var raw = Encoding.UTF8.GetBytes(Envelope(_session, generation, contentState, menusRaw));
        if (raw.Length > MaxRaw)
        {
            Log(LogLevel.Warn, $"the menu snapshot is {raw.Length >> 20} MiB, over {MaxRaw >> 20} MiB: not sent");
            return;
        }
        var parts = Cut(Compress(raw), PartSize());
        if (parts.Length > MaxParts)
        {
            Log(LogLevel.Warn, "the menu snapshot needs more than 4096 parts: not sent");
            return;
        }
        var id = StateIdOf(raw);
        _dirty = false;
        if (_snapshot?.StateId == id) return;
        _snapshot = new Snapshot { StateId = id, Generation = generation, ContentStateId = contentState, RawLength = raw.Length, Parts = parts, Menus = count };
        Log(LogLevel.Info, $"snapshot {id[..12]}: {count} menu(s), generation {generation}, content state {contentState:x8}, session {_session}, " +
                           $"{raw.Length} bytes -> {parts.Sum(p => p.Length)} deflated in {parts.Length} part(s)");
    }

    private static int PartSize() => Math.Clamp((int)Math.Max(1024, Network.Status.MaxFrame) - 256, 512, PartBytes);

    private static bool Send(NetPeer p, Snapshot s)
    {
        var from = Sending.TryGetValue(p.Id, out var prog) && prog.StateId == s.StateId ? prog.Next : 0;
        var to = Math.Min(s.Parts.Length, from + PartsPerTick);
        for (var i = from; i < to; i++)
            if (!_channel!.SendTo(p, new MenuPartMsg(Format, s.StateId, _session.ToString("D"), s.Generation, (uint)s.RawLength, (ushort)i, (ushort)s.Parts.Length, CodecDeflate, s.Parts[i])))
            {
                Sending[p.Id] = (s.StateId, i);
                return false;
            }
        if (to < s.Parts.Length)
        {
            Sending[p.Id] = (s.StateId, to);
            return false;
        }
        Sending.Remove(p.Id);
        Log(LogLevel.Info, $"snapshot {s.StateId[..12]} ({s.Menus} menu(s), generation {s.Generation}) sent to {p.Name} (0x{p.Id:x})");
        return true;
    }

    private static void OnAck(MenuAckMsg a, NetContext ctx)
    {
        if (a.Session != _session.ToString("D"))
        {
            Log(LogLevel.Info, $"{ctx.Sender.Name}: an ack of another session ({a.Session}) ignored");
            return;
        }
        var stage = a.Stage switch { StageStored => "stored", StageApplied => "applied", StageRefused => "REFUSED", _ => $"stage {a.Stage}" };
        Log(a.Stage == StageRefused ? LogLevel.Warn : LogLevel.Info,
            $"{ctx.Sender.Name} (0x{ctx.Sender.Id:x}): snapshot {(a.StateId.Length >= 12 ? a.StateId[..12] : a.StateId)} generation {a.Generation} {stage}" +
            (a.Reason.Length > 0 ? $": {a.Reason}" : ""));
        if (a.Stage == StageRefused) lock (Sent) Sent.Remove(ctx.Sender.Id);  // sent again with the next snapshot
    }

    // ------------------------------------------------------------------------------------------------ client ---

    private sealed record Ready(string StateId, string Session, ulong Generation, uint ContentStateId, string Container, int Menus, DateTime Since);

    private static readonly object Lock = new();
    private static StagingState? _staging;
    private static Ready? _ready;              // validated, waiting for its content state
    private static string? _appliedState;      // state id of the remote collection in place
    private static Guid _appliedSession;
    private static bool _contentRefused;
    private static volatile bool _leavePending;

    private static void OnServerReady(NetPeer server)
    {
        ClientLeave(null);
        // a Kiln server with the channel: a mod's own workshops list nothing until its first snapshot is in
        if (Kiln.Client.IsClient && !Kiln.Client.IsHosting && _channel is not null && server.HasChannel(ChannelName))
        {
            StationMenuStore.Backend.SetPending(true);
            Log(LogLevel.Info, "joined a server with station menus: own workshops wait for its snapshot");
        }
    }

    private static void OnPart(MenuPartMsg m, NetContext ctx)
    {
        if (!ctx.Sender.IsServer || (Kiln.Client.IsClient && Kiln.Client.IsHosting)) return;
        Staged? done;
        string? refuse;
        lock (Lock)
        {
            (done, refuse, _staging) = Accept(_staging, m, DateTime.UtcNow);
        }
        if (refuse is not null)
        {
            Log(LogLevel.Warn, $"snapshot {Short(m.StateId)} refused: {refuse}");
            SendAck(m.StateId, m.Session, m.Generation, StageRefused, refuse);
            return;
        }
        if (done is null) return;
        var parsed = ParseEnvelope(done.Raw, m.Session, m.Generation);
        if (parsed.Error is { } bad)
        {
            Log(LogLevel.Warn, $"snapshot {Short(m.StateId)} refused: {bad}");
            SendAck(m.StateId, m.Session, m.Generation, StageRefused, bad);
            return;
        }
        if (_appliedState == m.StateId && _appliedSession.ToString("D") == m.Session)
        {
            SendAck(m.StateId, m.Session, m.Generation, StageApplied, "");  // the same snapshot again: nothing to do
            return;
        }
        lock (Lock) _ready = new Ready(m.StateId, m.Session, m.Generation, parsed.ContentStateId, parsed.Container!, parsed.Menus, DateTime.UtcNow);
        _contentRefused = false;
        SendAck(m.StateId, m.Session, m.Generation, StageStored, "");
        Log(LogLevel.Info, $"snapshot {Short(m.StateId)}: {parsed.Menus} menu(s), generation {m.Generation}, waits for content state {parsed.ContentStateId:x8}");
        ClientTick();
    }

    private static void ClientTick()
    {
        if (!Game.IsTickThread) return;
        Ready? r;
        lock (Lock)
        {
            if (_staging is { } s && DateTime.UtcNow - s.Started > ReceiptDeadline)
            {
                Log(LogLevel.Warn, $"snapshot {Short(s.StateId)}: {s.Have}/{s.Parts.Length} part(s) in {ReceiptDeadline.TotalSeconds:0} s; dropped");
                _staging = null;
            }
            r = _ready;
        }
        if (r is null) return;
        if (!ContentReadyFor(r.ContentStateId))
        {
            if (!_contentRefused && DateTime.UtcNow - r.Since > ContentDeadline)
            {
                _contentRefused = true;  // kept: tried again when the matching content state arrives
                SendAck(r.StateId, r.Session, r.Generation, StageRefused, "ContentNotReady: the server's content state did not arrive in 120 s");
            }
            return;
        }
        var (rc, error) = StationMenuStore.Backend.RemoteReplace(_owner, r.Container);
        lock (Lock) if (ReferenceEquals(_ready, r)) _ready = null;
        if (rc != StationMenuNative.Ok)
        {
            var why = error.Length > 0 ? error : StationMenuNative.Message(rc);
            Log(LogLevel.Warn, $"snapshot {Short(r.StateId)} not applied: {why}");
            SendAck(r.StateId, r.Session, r.Generation, StageRefused, why);
            return;
        }
        _appliedState = r.StateId;
        _appliedSession = Guid.TryParse(r.Session, out var g) ? g : Guid.Empty;
        StationMenuStore.Backend.SetPending(false);
        SendAck(r.StateId, r.Session, r.Generation, StageApplied, "");
        Log(LogLevel.Info, $"snapshot {Short(r.StateId)} applied: {r.Menus} menu(s) from the server, generation {r.Generation}");
    }

    /// <summary>Hook for tests: whether the content state the snapshot names is in.</summary>
    internal static Func<uint, bool>? TestContentReady;

    private static bool ContentReadyFor(uint contentStateId)
    {
        if (TestContentReady is { } t) return t(contentStateId);
        if (contentStateId == 0) return true;  // no menu lists a recipe: nothing to wait for
        var st = ContentSync.Status;
        return st.StateId == contentStateId && st.State == ContentSyncState.Applied;
    }

    private static void SendAck(string stateId, string session, ulong generation, byte stage, string reason)
    {
        if (reason.Length > 1000) reason = reason[..1000];
        _channel?.SendToServer(new MenuAckMsg(stateId, session, generation, stage, reason));
    }

    private static void ClientLeave(string? why)
    {
        _leavePending = false;
        lock (Lock)
        {
            _staging = null;
            _ready = null;
        }
        var had = _appliedState != null;
        _appliedState = null;
        _appliedSession = Guid.Empty;
        if (!StationMenuStore.Available || !Kiln.Client.IsClient) return;
        StationMenuStore.Backend.SetPending(false);
        StationMenuStore.Backend.RemoteClear(_owner);
        if (had && why is not null) Log(LogLevel.Info, $"{why}: the server's menus are taken off");
    }

    private static void DrainApplied()
    {
        if (!Kiln.Client.IsClient || !StationMenuStore.Available) return;
        var json = StationMenuStore.Backend.DrainApplied();
        if (string.IsNullOrEmpty(json) || json == "[]") return;
        foreach (var e in ReadApplied(json))
        {
            Log(e.Ok ? LogLevel.Info : LogLevel.Warn, $"workshop 0x{e.Workshop:x8}: menu {(e.FromServer ? "from the server" : "of this game")} generation {e.Generation} " +
                                                       (e.Ok ? "built" : $"refused: {e.Reason}"));
            if (Events.HasSubscribers<StationMenuAppliedEvent>())
                Events.Post(new StationMenuAppliedEvent(e.Definition, e.Workshop, e.FromServer ? _appliedSession : Guid.Empty, e.Generation, e.FromServer, e.Ok, e.Reason));
        }
    }

    internal readonly record struct Applied(Guid Definition, uint Workshop, ulong Generation, bool FromServer, bool Ok, string Reason);

    /// <summary>The drain_applied JSON (pure: tests).</summary>
    internal static List<Applied> ReadApplied(string json)
    {
        var list = new List<Applied>();
        try
        {
            using var doc = JsonDocument.Parse(json);
            if (doc.RootElement.ValueKind != JsonValueKind.Array) return list;
            foreach (var e in doc.RootElement.EnumerateArray())
            {
                if (e.ValueKind != JsonValueKind.Object) continue;
                Guid.TryParse(e.TryGetProperty("definition", out var d) ? d.GetString() : null, out var def);
                list.Add(new Applied(def, e.TryGetProperty("workshop", out var w) && w.TryGetUInt32(out var wv) ? wv : 0,
                    e.TryGetProperty("generation", out var g) && g.TryGetUInt64(out var gv) ? gv : 0,
                    e.TryGetProperty("fromServer", out var f) && f.ValueKind == JsonValueKind.True,
                    e.TryGetProperty("ok", out var o) && o.ValueKind == JsonValueKind.True,
                    e.TryGetProperty("reason", out var r) && r.ValueKind == JsonValueKind.String ? r.GetString() ?? "" : ""));
            }
        }
        catch (JsonException)
        {
        }
        return list;
    }

    // ------------------------------------------------------------------------------------------------ codec ----

    internal sealed record Staged(byte[] Raw);

    /// <summary>One received part (pure: tests). Returns the whole snapshot when it is complete and its hash checks, a
    /// refusal reason, or neither (more parts to come); and the staging to keep.</summary>
    internal static (Staged? Done, string? Refuse, StagingState? Next) Accept(StagingState? staging, MenuPartMsg m, DateTime now)
    {
        if (m.Format != Format) return (null, $"unknown snapshot format {m.Format}", staging);
        if (m.Codec != CodecDeflate) return (null, $"unknown codec {m.Codec}", staging);
        if (!IsStateId(m.StateId)) return (null, "bad state id", staging);
        if (!Guid.TryParseExact(m.Session, "D", out _) || m.Session.Length != 36) return (null, "bad session", staging);
        if (m.Parts is 0 or > MaxParts || m.Part >= m.Parts) return (null, $"bad part {m.Part}/{m.Parts}", staging);
        if (m.Data.Length > PartBytes) return (null, "a part over 96 KiB", staging);
        if (m.RawLength > MaxRaw) return (null, $"a snapshot of {m.RawLength >> 20} MiB, over 64 MiB", staging);
        // a new snapshot (another state, session, generation or size) starts over; old and new never share parts
        if (staging is null || staging.StateId != m.StateId || staging.Session != m.Session || staging.Generation != m.Generation ||
            staging.RawLength != m.RawLength || staging.Parts.Length != m.Parts || now - staging.Started > ReceiptDeadline)
            staging = new StagingState(m.StateId, m.Session, m.Generation, m.RawLength, m.Parts, now);
        if (staging.Parts[m.Part] is null)
        {
            staging.Parts[m.Part] = m.Data;
            staging.Have++;
            staging.Bytes += m.Data.Length;
            if (staging.Bytes > MaxCompressed) return (null, "over 65 MiB compressed", null);
        }
        if (staging.Have < staging.Parts.Length) return (null, null, staging);
        var packed = new byte[staging.Bytes];
        var off = 0;
        foreach (var p in staging.Parts)
        {
            p!.CopyTo(packed, off);
            off += p.Length;
        }
        byte[] raw;
        try
        {
            raw = Decompress(packed, (int)m.RawLength);
        }
        catch (Exception e) when (e is InvalidDataException or IOException)
        {
            return (null, "bad compressed data: " + e.Message, null);
        }
        if (raw.Length != m.RawLength) return (null, $"the snapshot is {raw.Length} bytes, not {m.RawLength}", null);
        if (StateIdOf(raw) != m.StateId) return (null, "the snapshot does not match its hash", null);
        return (new Staged(raw), null, null);
    }

    internal sealed class StagingState(string stateId, string session, ulong generation, uint rawLength, ushort parts, DateTime started)
    {
        public readonly string StateId = stateId, Session = session;
        public readonly ulong Generation = generation;
        public readonly uint RawLength = rawLength;
        public readonly byte[]?[] Parts = new byte[]?[parts];
        public readonly DateTime Started = started;
        public int Have;
        public long Bytes;
    }


    internal readonly record struct Parsed(string? Error, uint ContentStateId, string? Container, int Menus);

    /// <summary>Checks a complete snapshot's envelope (pure: tests): format 1, the session and generation of its parts, a
    /// content state, menus without a duplicate definition or workshop. Container: what remote_replace takes, the
    /// records as they came.</summary>
    internal static Parsed ParseEnvelope(byte[] raw, string session, ulong generation)
    {
        try
        {
            using var doc = JsonDocument.Parse(raw);
            var root = doc.RootElement;
            if (root.ValueKind != JsonValueKind.Object) return new("not a JSON object", 0, null, 0);
            foreach (var p in root.EnumerateObject())
                if (p.Name is not ("contentStateId" or "format" or "generation" or "menus" or "session"))
                    return new($"unknown member {p.Name}", 0, null, 0);
            if (!root.TryGetProperty("format", out var f) || !f.TryGetInt32(out var fv) || fv != 1) return new("unknown format", 0, null, 0);
            if (!root.TryGetProperty("session", out var s) || s.ValueKind != JsonValueKind.String || s.GetString() != session)
                return new("the session does not match its parts", 0, null, 0);
            if (!root.TryGetProperty("generation", out var g) || !g.TryGetUInt64(out var gv) || gv != generation)
                return new("the generation does not match its parts", 0, null, 0);
            if (!root.TryGetProperty("contentStateId", out var c) || !c.TryGetUInt32(out var cv)) return new("no content state", 0, null, 0);
            if (!root.TryGetProperty("menus", out var menus) || menus.ValueKind != JsonValueKind.Array) return new("no menus", 0, null, 0);
            var defs = new HashSet<string>(StringComparer.Ordinal);
            var workshops = new HashSet<uint>();
            var n = 0;
            foreach (var m in menus.EnumerateArray())
            {
                n++;
                if (m.ValueKind != JsonValueKind.Object || !m.TryGetProperty("definition", out var d) || d.ValueKind != JsonValueKind.Object ||
                    !d.TryGetProperty("id", out var id) || id.ValueKind != JsonValueKind.String ||
                    !m.TryGetProperty("origin", out var o) || o.ValueKind != JsonValueKind.Object ||
                    !o.TryGetProperty("workshopId", out var w) || !w.TryGetUInt32(out var wv))
                    return new($"menu {n}: not a station-menu record", 0, null, 0);
                if (!defs.Add(id.GetString()!)) return new($"menu {n}: definition {id.GetString()} twice", 0, null, 0);
                if (!workshops.Add(wv)) return new($"menu {n}: workshop 0x{wv:x8} twice", 0, null, 0);
            }
            return new(null, cv, Container(generation, menus.GetRawText()), n);
        }
        catch (JsonException e)
        {
            return new("not JSON: " + e.Message, 0, null, 0);
        }
    }

    /// <summary>The native container {"format":1,"generation":N,"menus":[...]} around records as they came.</summary>
    internal static string Container(ulong generation, string menusRaw) => $"{{\"format\":1,\"generation\":{generation},\"menus\":{menusRaw}}}";

    /// <summary>The snapshot as sent: canonical key order, the records exactly as the native export wrote them.</summary>
    internal static string Envelope(Guid session, ulong generation, uint contentStateId, string menusRaw) =>
        $"{{\"contentStateId\":{contentStateId},\"format\":1,\"generation\":{generation},\"menus\":{menusRaw},\"session\":\"{session:D}\"}}";

    /// <summary>The menus of a native export container, as raw JSON text.</summary>
    internal static bool TryMenus(string export, out ulong generation, out string menusRaw, out int count, out string why)
    {
        generation = 0;
        menusRaw = "[]";
        count = 0;
        why = "";
        try
        {
            using var doc = JsonDocument.Parse(export);
            var root = doc.RootElement;
            if (!root.TryGetProperty("generation", out var g) || !g.TryGetUInt64(out generation) ||
                !root.TryGetProperty("menus", out var m) || m.ValueKind != JsonValueKind.Array)
            {
                why = "not an export container";
                return false;
            }
            menusRaw = m.GetRawText();
            count = m.GetArrayLength();
            return true;
        }
        catch (JsonException e)
        {
            why = e.Message;
            return false;
        }
    }

    internal static string StateIdOf(ReadOnlySpan<byte> raw) => Convert.ToHexStringLower(SHA256.HashData(raw));

    private static bool IsStateId(string s) => s.Length == 64 && s.All(ch => ch is >= '0' and <= '9' or >= 'a' and <= 'f');

    private static string Short(string stateId) => stateId.Length >= 12 ? stateId[..12] : stateId;

    internal static byte[][] Cut(byte[] packed, int partBytes)
    {
        var n = Math.Max(1, (packed.Length + partBytes - 1) / partBytes);
        var parts = new byte[n][];
        for (var i = 0; i < n; i++)
        {
            var off = i * partBytes;
            parts[i] = packed.AsSpan(off, Math.Min(partBytes, packed.Length - off)).ToArray();
        }
        return parts;
    }

    internal static byte[] Compress(byte[] raw) => ContentSync.Compress(raw);

    internal static byte[] Decompress(byte[] packed, int expected) => ContentSync.Decompress(packed, expected);

    /// <summary>Tests: forget every state.</summary>
    internal static void ResetForTests()
    {
        lock (Sent)
        {
            Sent.Clear();
            Sending.Clear();
        }
        lock (Lock)
        {
            _staging = null;
            _ready = null;
        }
        _snapshot = null;
        _dirty = true;
        _appliedState = null;
        _decider?.Dispose();
        _decider = null;
        DeclHolds.Clear();
        _blocksDeclared = false;
        TestDeclared = null;
        TestPublishCheck = null;
        TestContentReady = null;
        TestChannelOpen = false;
    }
}
