// SPDX-License-Identifier: MIT
// The built-in receiver (dossier chapter 04 §4.4): Kiln's own kiln:markers channel and the internal kiln.markers.bridge
// table. Started by MarkersStartup (the autostart table), under no mod, on both sides: a client shows the markers a
// server sends without the sending mod. kiln-markers.dll owns the protocol (codecs, quotas, the server layers, the
// receiver, ACKs and retries); this side only maps Kiln.Network's connections to bridge peers, copies bytes in, sends the
// packets the native pump hands out, and pumps each role on its own thread (the server role on the service tick, the
// client role on the client world's step).
//
// Peers: every Network peer is synchronised before each pump, also vanilla, channel-less and handshake-pending ones (their
// refusals are counted in a server send's result), each Network connection object with a generation of its own. While
// this client hosts, the hosting player is one loopback endpoint (route 2) keyed by both marker world epochs and the
// local player: its own machine's Network row is left out (counted once) and the self-loop's marker frames are ignored.
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace Kiln;

/// <summary>A connection as the bridge sees it (Kiln.Network's NetPeer in a game; a fake in tests).</summary>
internal readonly record struct MarkerPeerView(object Key, uint Id, NetPeerState State, uint Player, bool HasChannel, bool IsLab, bool IsLocalMachine);

internal delegate void MarkerRawHandler(object peer, bool toServer, ushort message, ReadOnlySpan<byte> body);

/// <summary>What the bridge needs from the network and the client state.</summary>
internal interface IMarkersTransport
{
    /// <summary>The transport works (kiln.net).</summary>
    bool Available { get; }

    /// <summary>Opens kiln:markers version 1 (not required, at most 16384 bytes) for Kiln itself; returns its limit.</summary>
    int Open(MarkerRawHandler onRaw);

    /// <summary>Runs <paramref name="pump"/> at every client world step (main thread); nothing on a dedicated server.</summary>
    void HookClientTick(Action pump);

    /// <summary>Server role: every connection (Kiln, vanilla, pending).</summary>
    IReadOnlyList<MarkerPeerView> ServerPeers();

    /// <summary>Client role: the connection to the server, or null.</summary>
    MarkerPeerView? ServerLink();

    bool Send(object peer, ushort message, ReadOnlySpan<byte> body);

    bool IsClient { get; }
    bool IsHosting { get; }

    /// <summary>The local player's entity while it is known (0 otherwise).</summary>
    uint LocalPlayer { get; }
}

internal static unsafe class MarkersBridge
{
    internal const string ChannelName = "kiln:markers";
    internal const string ChannelVersion = "1";
    internal const int MaxBody = 16384;
    private const uint RoleServer = 0, RoleClient = 1, RouteNetwork = 1, RouteLoopback = 2;
    private const uint ServerPeer = 0xFFFFFFFF, LoopbackPeer = 0;
    private const uint HasPlayer = 1, NetworkReady = 2, HasChannel = 4, VanillaFlag = 8, RejectedFlag = 16;
    private const uint PacketsPerPump = 16;

    private sealed class Up
    {
        public object Key = null!;
        public uint Id;
        public ulong Gen;
        public uint Flags = uint.MaxValue;
        public uint Entity;
    }

    private static readonly object Lock = new();
    private static int _started;
    private static IMarkersTransport? _t;
    private static MarkersNative.BridgeTable* _b;
    private static ulong _nextGen;
    private static readonly Dictionary<object, Up> ServerUp = new(ReferenceEqualityComparer.Instance);
    private static readonly Dictionary<ulong, Up> ServerByGen = new();
    private static Up? _link;           // the client role's route-1 connection
    private static (ulong Server, ulong Client, uint Player) _loopKey;
    private static ulong _loopGen;      // the loopback's generation for _loopKey
    private static ulong _serverLoop, _clientLoop;  // what each role registered
    private static int _faults;

    /// <summary>What the receiver did at its start (logs, tests).</summary>
    internal static string State { get; private set; } = "not started";

    /// <summary>Why markers.send is off on this side when the receiver could not open its channel; null otherwise.</summary>
    internal static (CapabilityReason Reason, string Text)? Off { get; private set; }

    /// <summary>Starts the built-in receiver once (MarkersStartup; tests pass a fake transport). Never throws.</summary>
    internal static void Start(IMarkersTransport? transport = null)
    {
        if (Interlocked.CompareExchange(ref _started, 1, 0) != 0) return;
        var prev = ModContext.CurrentMod;
        ModContext.CurrentMod = null;  // Kiln's own channel and handlers, never a mod's
        try
        {
            StartCore(transport ?? new NetworkMarkersTransport());
        }
        catch (Exception e)
        {
            State = "failed: " + e.Message;
            Log(LogLevel.Warn, $"the built-in marker receiver did not start: {e.Message}");
        }
        finally
        {
            ModContext.CurrentMod = prev;
        }
    }

    private static void StartCore(IMarkersTransport t)
    {
        var table = MarkersNative.Bridge;
        if (table == null)
        {
            State = "kiln.markers.bridge is not here";
            return;  // the native rows say why (kiln.markers missing), or markers.send stays pending
        }
        if (!t.Available)
        {
            State = "no transport";
            Off = (CapabilityReason.ServiceMissing, "needs kiln.net (Kiln.Network's transport) on this game build");
            ServiceCaps.Report(MarkersCaps.Send, CapabilityState.Unavailable, Off.Value.Reason, Off.Value.Text);
            return;
        }
        var limit = t.Open(OnRaw);
        if (limit < MaxBody)
        {
            // never raise the user's limit: without 16384 bytes the link cannot carry wire v1
            State = $"channel limit {limit}";
            Off = (CapabilityReason.Policy, $"the network limit allows {limit} bytes per message; markers need 16384 (kiln/config/network.json maxMessageBytes)");
            ServiceCaps.Report(MarkersCaps.Send, CapabilityState.Unavailable, Off.Value.Reason, Off.Value.Text);
            Log(LogLevel.Warn, $"markers are not sent or received: the network limit allows {limit} bytes per message, the marker channel needs 16384");
            return;
        }
        var owner = Backend.Current.OwnerCreate("kiln.markers.bridge");
        if (owner == 0)
        {
            State = "no owner";
            return;
        }
        var rc = table->Attach(owner);
        if (rc != MarkersNative.Ok)
        {
            State = $"attach answered {rc}";
            Log(LogLevel.Warn, $"kiln.markers.bridge refused the built-in receiver ({rc})");
            return;
        }
        lock (Lock)
        {
            _t = t;
            _b = table;
        }
        ServiceTick.Add("kiln markers bridge", static (in TickInfo _) => ServerPump());
        t.HookClientTick(ClientPump);
        State = "attached";
    }

    /// <summary>Tests: forget everything (the fake table and transport are gone).</summary>
    internal static void Reset()
    {
        lock (Lock)
        {
            _started = 0;
            _t = null;
            _b = null;
            _nextGen = 0;
            ServerUp.Clear();
            ServerByGen.Clear();
            _link = null;
            _loopKey = default;
            _loopGen = _serverLoop = _clientLoop = 0;
            _faults = 0;
            State = "not started";
            Off = null;
        }
    }

    private static void Log(LogLevel level, string text) => Backend.Current.Log(0, level, "markers: " + text);

    private static uint Flags(in MarkerPeerView p)
    {
        uint f = 0;
        // a lab peer (tools/nettest on a lab server's loopback transport, lab servers only) has no game player: it stands
        // for one while it is connected (a lab "vanilla" peer stays Connecting: Kiln.Network never times a lab peer out)
        if (p.Player != 0 || (p.IsLab && p.State is not (NetPeerState.Closed or NetPeerState.Rejected))) f |= HasPlayer;
        if (p.State == NetPeerState.Ready) f |= NetworkReady;
        if (p.HasChannel) f |= HasChannel;
        if (p.State == NetPeerState.Vanilla) f |= VanillaFlag;
        if (p.State == NetPeerState.Rejected) f |= RejectedFlag;
        return f;
    }

    private static void PeerUp(MarkersNative.BridgeTable* b, uint role, uint id, uint route, uint entity, uint flags, ulong gen)
    {
        var p = new MarkersNative.Peer
        {
            Size = (uint)sizeof(MarkersNative.Peer), Role = role, Id = id, Route = route, Entity = entity, MaxMessageBytes = MaxBody,
            Flags = flags, Generation = gen,
        };
        b->PeerUp(&p);
    }

    // The loopback's generation: one per (server epoch, client epoch, local player), the same for both roles.
    private static ulong LoopGeneration(IMarkersTransport t)
    {
        if (!t.IsClient || !t.IsHosting) return 0;
        var player = t.LocalPlayer;
        var m = MarkersNative.T;
        if (player == 0 || m == null || m->WorldEpoch == null) return 0;
        ulong server = 0, client = 0;
        if (m->WorldEpoch(0, &server) != MarkersNative.Ok || m->WorldEpoch(2, &client) != MarkersNative.Ok || server == 0 || client == 0) return 0;
        lock (Lock)
        {
            var key = (server, client, player);
            if (_loopKey != key || _loopGen == 0)
            {
                _loopKey = key;
                _loopGen = ++_nextGen;
            }
            return _loopGen;
        }
    }

    // ---- the server role (service tick: the server's tick thread) ----

    /// <summary>Synchronises every Network peer with the bridge, then pumps the server role.</summary>
    internal static void ServerPump()
    {
        var t = _t;
        var b = _b;
        if (t == null || b == null) return;
        try
        {
            if (t.IsClient && !t.IsHosting)
            {
                DropServerPeers(b);  // a joined client has no server role
                return;
            }
            SyncServer(t, b);
            uint done = 0;
            b->Pump(RoleServer, PacketsPerPump, &SendFromNative, (void*)RoleServer, &done);
        }
        catch (Exception e)
        {
            if (++_faults <= 5) Log(LogLevel.Error, $"the marker bridge's server step failed: {e}");
        }
    }

    private static void DropServerPeers(MarkersNative.BridgeTable* b)
    {
        lock (Lock)
        {
            foreach (var up in ServerUp.Values) b->PeerDown(RoleServer, up.Id, up.Gen);
            ServerUp.Clear();
            ServerByGen.Clear();
            if (_serverLoop != 0) b->PeerDown(RoleServer, LoopbackPeer, _serverLoop);
            _serverLoop = 0;
        }
    }

    private static void SyncServer(IMarkersTransport t, MarkersNative.BridgeTable* b)
    {
        var hosting = t.IsClient && t.IsHosting;
        var local = hosting ? t.LocalPlayer : 0;
        var peers = t.ServerPeers();
        var loop = LoopGeneration(t);
        lock (Lock)
        {
            var seen = new HashSet<object>(ReferenceEqualityComparer.Instance);
            foreach (var p in peers)
            {
                if (p.State == NetPeerState.Closed) continue;
                // the hosting player is the loopback endpoint: its own machine's Network row is not a second recipient
                if (hosting && (p.IsLocalMachine || (local != 0 && p.Player == local))) continue;
                seen.Add(p.Key);
                if (!ServerUp.TryGetValue(p.Key, out var up))
                {
                    up = new Up { Key = p.Key, Id = p.Id, Gen = ++_nextGen };  // a new connection object: a new generation
                    ServerUp[p.Key] = up;
                    ServerByGen[up.Gen] = up;
                }
                var flags = Flags(p);
                if (up.Flags == flags && up.Entity == p.Player) continue;
                up.Flags = flags;
                up.Entity = p.Player;
                PeerUp(b, RoleServer, up.Id, RouteNetwork, p.Player, flags, up.Gen);
            }
            if (ServerUp.Count != seen.Count)
                foreach (var gone in ServerUp.Values.Where(u => !seen.Contains(u.Key)).ToList())
                {
                    b->PeerDown(RoleServer, gone.Id, gone.Gen);
                    ServerUp.Remove(gone.Key);
                    ServerByGen.Remove(gone.Gen);
                }
            if (_serverLoop != loop)
            {
                if (_serverLoop != 0) b->PeerDown(RoleServer, LoopbackPeer, _serverLoop);
                if (loop != 0) PeerUp(b, RoleServer, LoopbackPeer, RouteLoopback, local, HasPlayer | NetworkReady | HasChannel, loop);
                _serverLoop = loop;
            }
        }
    }

    // ---- the client role (the client world's step: the main thread) ----

    /// <summary>Synchronises the server link (or the hosted loopback) with the bridge, then pumps the client role.</summary>
    internal static void ClientPump()
    {
        var t = _t;
        var b = _b;
        if (t == null || b == null || !t.IsClient) return;
        try
        {
            SyncClient(t, b);
            uint done = 0;
            b->Pump(RoleClient, PacketsPerPump, &SendFromNative, (void*)RoleClient, &done);
        }
        catch (Exception e)
        {
            if (++_faults <= 5) Log(LogLevel.Error, $"the marker bridge's client step failed: {e}");
        }
    }

    private static void SyncClient(IMarkersTransport t, MarkersNative.BridgeTable* b)
    {
        var loop = LoopGeneration(t);
        var link = t.IsHosting ? null : t.ServerLink();
        lock (Lock)
        {
            // hosting: the loopback is the only route; the Network self-loop to our own server is not a second one
            if (_link != null && (link is not { } l0 || l0.State == NetPeerState.Closed || !ReferenceEquals(l0.Key, _link.Key)))
            {
                b->PeerDown(RoleClient, ServerPeer, _link.Gen);
                _link = null;
            }
            if (link is { } l && l.State != NetPeerState.Closed)
            {
                _link ??= new Up { Key = l.Key, Id = ServerPeer, Gen = ++_nextGen };
                var flags = Flags(l) & ~HasPlayer;
                if (_link.Flags != flags)
                {
                    _link.Flags = flags;
                    PeerUp(b, RoleClient, ServerPeer, RouteNetwork, 0, flags, _link.Gen);
                }
            }
            if (_clientLoop != loop)
            {
                if (_clientLoop != 0) b->PeerDown(RoleClient, ServerPeer, _clientLoop);
                if (loop != 0) PeerUp(b, RoleClient, ServerPeer, RouteLoopback, 0, NetworkReady | HasChannel, loop);
                _clientLoop = loop;
            }
        }
    }

    // ---- bytes in and out ----

    /// <summary>kiln:markers' raw handler: copies the message into the bridge (any thread, enqueue only).</summary>
    internal static void OnRaw(object peer, bool toServer, ushort message, ReadOnlySpan<byte> body)
    {
        var b = _b;
        var t = _t;
        if (b == null || t == null) return;
        uint role, id;
        ulong gen;
        lock (Lock)
        {
            if (toServer)
            {
                if (!ServerUp.TryGetValue(peer, out var up)) return;  // the hosting player's self-loop, or not synchronised yet
                role = RoleServer;
                id = up.Id;
                gen = up.Gen;
            }
            else
            {
                if (_link == null || !ReferenceEquals(_link.Key, peer)) return;  // hosting (the loopback carries it), or an old link
                role = RoleClient;
                id = ServerPeer;
                gen = _link.Gen;
            }
        }
        fixed (byte* p = body)
        {
            var e = new MarkersNative.Envelope
            {
                Size = (uint)sizeof(MarkersNative.Envelope), Role = role, Peer = id, Message = message, PeerGeneration = gen, Bytes = p,
                Length = (uint)body.Length,
            };
            b->Receive(&e);  // copied; a full queue drops it (the receiver asks for a resync)
        }
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static byte SendFromNative(MarkersNative.Envelope* e, void* user)
    {
        try
        {
            var t = _t;
            if (t == null || e == null || e->Size < (uint)sizeof(MarkersNative.Envelope) || e->Message > ushort.MaxValue) return 0;
            object? peer;
            lock (Lock)
            {
                if (e->Role == RoleServer) peer = ServerByGen.TryGetValue(e->PeerGeneration, out var up) && up.Id == e->Peer ? up.Key : null;
                else peer = _link != null && _link.Gen == e->PeerGeneration ? _link.Key : null;
            }
            if (peer == null) return 0;
            return t.Send(peer, (ushort)e->Message, new ReadOnlySpan<byte>(e->Bytes, (int)e->Length)) ? (byte)1 : (byte)0;
        }
        catch (Exception)
        {
            return 0;  // tried again at the next pump
        }
    }
}

/// <summary>The bridge's transport in a game: Kiln.Network and Kiln.Client.</summary>
internal sealed class NetworkMarkersTransport : IMarkersTransport
{
    private NetChannel? _channel;

    public bool Available => Network.Available;

    public int Open(MarkerRawHandler onRaw)
    {
        _channel = Network.Channel(MarkersBridge.ChannelName, MarkersBridge.ChannelVersion, required: false, maxMessageBytes: MarkersBridge.MaxBody);
        _channel.OnRaw((ctx, message, body) => onRaw(ctx.Sender, ctx.Direction == NetDirection.ToServer, message, body));
        return _channel.MaxMessageBytes;
    }

    public void HookClientTick(Action pump)
    {
        if (!Client.Available || !Client.IsClient) return;
        Client.Tick += (in TickInfo _) => pump();
    }

    private static MarkerPeerView View(NetPeer p) =>
        new(p, p.Id, p.State, p.Player.Value, p.HasChannel(MarkersBridge.ChannelName), p.IsLab, !p.IsServer && !p.IsLab && (p.Id & 0x7F) == 0);

    public IReadOnlyList<MarkerPeerView> ServerPeers()
    {
        var peers = Network.Peers;
        var list = new List<MarkerPeerView>(peers.Count);
        foreach (var p in peers) list.Add(View(p));
        return list;
    }

    public MarkerPeerView? ServerLink() => Network.Server is { } s ? View(s) : null;

    public bool Send(object peer, ushort message, ReadOnlySpan<byte> body) =>
        _channel is { } ch && peer is NetPeer p && ch.SendRaw(p, message, body);

    public bool IsClient => Client.Available && Client.IsClient;
    public bool IsHosting => Client.Available && Client.IsHosting;
    public uint LocalPlayer => Client.Available && Client.HasLocalPlayer ? Client.LocalPlayer.Value : 0;
}
