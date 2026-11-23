// SPDX-License-Identifier: MIT
// The camera transport bridge (chapter 03 §4.6, S8): the server's route for each remote visual request (a guest's client
// over the channel kiln.camera, the host's own player through a local queue, or a refusal), the per-peer bindings, and
// the client's receiver. Pure of the game: Kiln.Network/Kiln.Client stand behind ICameraNet and native kiln.camera behind
// ICameraLocalNative (CameraStartup wires the real ones; the tests fakes).
//
// Threads. The server role (Submit, Retire, ServerPump) runs on the server world's tick: native kiln.camera calls Submit
// from its server Pre, Kiln's service tick calls ServerPump. The client role (ClientPump) runs on the client world's step.
// Each role has its own lock; they share only the host's queues (HostSelfRoute) and the received-message queues. Caps is
// any thread (a snapshot). Nothing here calls back into native kiln.camera while a native lock is held (native calls
// Submit outside its locks, and the answers go to `done`, which only records).
using System.Collections.Concurrent;

namespace Kiln.CameraTransport;

internal enum CameraBridgePeer
{
    Unavailable,
    Vanilla,
    Negotiating,
    Ready,
}

internal enum CameraBridgeRoute
{
    Refused,
    HostSelf,
    AwaitNegotiation,
    Remote,
}

internal readonly record struct CameraBridgeDecision(CameraBridgeRoute Route, ushort Reason, ulong RequiredParts);

internal static partial class CameraBridge
{
    /// <summary>Chooses the physical route after native request validation. The persistent bridge must already exist;
    /// its absence is a local camera.remote capability error, not a game refusal. Player/host/peer identities come
    /// from framework snapshots, never from an author's claimed role or a packet's player field.</summary>
    /// <remarks>A HostSelf decision posts to CLIENT Pre and checks the actual local part rows there; it does not
    /// require camera.remote or a network handshake. Guest negotiation waits in ServerBinding for its 2 s deadline.
    /// A ready guest's missing part is ClientCapability, whereas a known vanilla guest is RequiresKilnClient even
    /// when its part mask is empty. No path silently substitutes a stock Mode for requested visuals.</remarks>
    internal static CameraBridgeDecision DecideRoute(uint parts, bool playerConnected, bool hostSelf, CameraBridgePeer peer, ulong recipientCaps)
    {
        const uint visual = CameraWireAbi.PartPath | CameraWireAbi.PartProfile | CameraWireAbi.PartLens | CameraWireAbi.PartShake;
        if (parts == 0 || (parts & ~visual) != 0 || (parts & (CameraWireAbi.PartPath | CameraWireAbi.PartProfile)) == (CameraWireAbi.PartPath | CameraWireAbi.PartProfile))
            throw new ArgumentException("the camera bridge takes a validated visual request, not Mode or unknown parts", nameof(parts));
        if (!Enum.IsDefined(peer)) throw new ArgumentOutOfRangeException(nameof(peer));
        ulong required = 0;
        if ((parts & CameraWireAbi.PartPath) != 0) required |= CameraWireAbi.CapLocalPose;
        if ((parts & CameraWireAbi.PartProfile) != 0) required |= CameraWireAbi.CapLocalProfile;
        if ((parts & CameraWireAbi.PartLens) != 0) required |= CameraWireAbi.CapLocalFov;
        if ((parts & CameraWireAbi.PartShake) != 0) required |= CameraWireAbi.CapLocalShake;

        CameraBridgeDecision Refused(ushort reason) => new(CameraBridgeRoute.Refused, reason, required);
        if (!playerConnected) return Refused(CameraReasons.PeerUnavailable);
        if (hostSelf) return new(CameraBridgeRoute.HostSelf, CameraReasons.None, required);
        return peer switch
        {
            CameraBridgePeer.Vanilla => Refused(CameraReasons.RequiresKilnClient),
            CameraBridgePeer.Negotiating => new(CameraBridgeRoute.AwaitNegotiation, CameraReasons.None, required),
            CameraBridgePeer.Ready when (recipientCaps & required) != required => Refused(CameraReasons.ClientCapability),
            CameraBridgePeer.Ready => new(CameraBridgeRoute.Remote, CameraReasons.None, required),
            _ => Refused(CameraReasons.PeerUnavailable),
        };
    }
}

/// <summary>A connection as the camera bridge sees it (Kiln.Network's NetPeer in a game; a fake in tests): the connection
/// object (its identity is the connection's), its id and state, the player it carries (0: none yet), whether it has the
/// channel kiln.camera, and whether it is the hosting client's own connection to its server (not a guest).</summary>
internal readonly record struct CameraPeerView(object Key, uint Id, NetPeerState State, uint Player, bool HasChannel, bool IsLocalMachine);

internal delegate void CameraRawHandler(object peer, bool toServer, ushort message, ReadOnlySpan<byte> body);

/// <summary>What the bridge needs of the network and the game's state.</summary>
internal interface ICameraNet
{
    /// <summary>The transport works (kiln.net).</summary>
    bool Available { get; }

    /// <summary>Opens the channel kiln.camera version 1 (not required, at most 16384 bytes) for Kiln itself, never for the mod
    /// whose code runs; returns its message limit.</summary>
    int Open(CameraRawHandler onRaw);

    /// <summary>Server role: every connection (Kiln, vanilla, pending).</summary>
    IReadOnlyList<CameraPeerView> ServerPeers();

    /// <summary>Client role: the connection to the server, or null.</summary>
    CameraPeerView? ServerLink();

    bool Send(object peer, ushort message, ReadOnlySpan<byte> body);

    bool IsClient { get; }

    bool IsHosting { get; }

    /// <summary>The local player's entity while known (0 otherwise).</summary>
    uint LocalPlayer { get; }

    /// <summary>The running server world's session number (0: none).</summary>
    uint ServerWorld { get; }

    /// <summary>The running client world's session number (0: none).</summary>
    uint ClientWorld { get; }
}

/// <summary>The bridge itself: one per process (CameraStartup), several in tests.</summary>
internal sealed class CameraBridgeHost
{
    private sealed class Peer
    {
        public required object Key;
        public required uint Player;
        public required ServerBinding Binding;
        public NetPeerState State;
        public bool HasChannel;
    }

    private readonly ICameraNet _net;
    private readonly ICameraLocalNative _local;
    private readonly Func<long> _clock;
    private readonly Func<ulong> _random;
    private readonly int _channelMax;
    private readonly Action<RemoteEnded> _ended;

    // ---- the server role (its lock) ----
    private readonly object _server = new();
    private readonly Dictionary<object, Peer> _peers = new(ReferenceEqualityComparer.Instance);
    private readonly Dictionary<uint, Peer> _byPlayer = [];
    private readonly Dictionary<ulong, object?> _routes = [];  // lease -> its peer key (null: the host's own route)
    private readonly Dictionary<ulong, ulong> _tokens = [];     // author owner -> wire owner token
    private readonly List<(Action<TransportResult> Done, TransportResult Result)> _results = [];
    private readonly ConcurrentQueue<(object Peer, CameraMessage Id, byte[] Body)> _serverInbox = new();
    private readonly HostSelfRoute _host;
    private ulong _nextToken, _incarnations;
    private uint _serverWorld;
    private ulong _serverNonce;
    private volatile Dictionary<uint, ulong> _caps = [];  // player -> the part bits its binding negotiated (Caps' snapshot)
    private volatile uint _hostPlayer;                     // the host's own player while this client hosts (0: none)

    // ---- the client role (its lock) ----
    private readonly object _client = new();
    private readonly ConcurrentQueue<(object Peer, CameraMessage Id, byte[] Body)> _clientInbox = new();
    private readonly ConcurrentQueue<(nint Key, NativeOutcome Outcome)> _localDone = new();
    private readonly ConcurrentQueue<(ulong Lease, uint Kind, byte EndReason, ushort Reason, ulong Frame)> _localChanged = new();
    private readonly LocalEndpoint _endpoint;
    private ClientBinding? _binding;
    private (object Link, uint World, uint Player) _bindingKey;
    private bool _hostClient;  // the client half of the host's route is live

    public CameraBridgeHost(ICameraNet net, ICameraLocalNative local, Func<long> clock, Func<ulong> random, int channelMax, Action<RemoteEnded> ended)
    {
        _net = net ?? throw new ArgumentNullException(nameof(net));
        _local = local ?? throw new ArgumentNullException(nameof(local));
        _clock = clock ?? throw new ArgumentNullException(nameof(clock));
        _random = random ?? throw new ArgumentNullException(nameof(random));
        _ended = ended ?? throw new ArgumentNullException(nameof(ended));
        _channelMax = Math.Clamp(channelMax, 1, CameraWireAbi.MaxBody);
        _endpoint = new LocalEndpoint(local);
        _host = new HostSelfRoute(_clock) { OnEnded = Ended };
    }

    /// <summary>The client's binding with its server (null: none). Tests.</summary>
    internal ClientBinding? ClientBinding => _binding;

    /// <summary>Leases the server role routes now (tests).</summary>
    internal int RouteCount
    {
        get { lock (_server) return _routes.Count; }
    }

    /// <summary>The server's binding for <paramref name="player"/> (null: none). Tests.</summary>
    internal ServerBinding? BindingOf(uint player)
    {
        lock (_server) return _byPlayer.TryGetValue(player, out var p) ? p.Binding : null;
    }

    // ================================================================ native kiln.camera's transport table

    /// <summary>KilnCameraTransport.submit (server Pre, outside kiln.camera's locks). The outcome always comes through
    /// <paramref name="done"/> at a later <see cref="ServerPump"/>, never inside this call.</summary>
    public void Submit(ulong author, TransportOp op, ulong lease, ulong revision, uint player, CameraWireRequest? request, Action<TransportResult> done)
    {
        ArgumentNullException.ThrowIfNull(done);
        lock (_server)
        {
            var token = Token(author);
            if (op == TransportOp.Apply)
            {
                var hostSelf = player != 0 && player == _hostPlayer;
                Peer? peer = null;
                if (!hostSelf) _byPlayer.TryGetValue(player, out peer);
                var connected = hostSelf || peer is not null;
                var decision = CameraBridge.DecideRoute(request!.Parts, connected, hostSelf, connected && peer is not null ? PeerOf(peer) : CameraBridgePeer.Unavailable,
                    peer?.Binding.ClientCaps ?? 0);
                switch (decision.Route)
                {
                    case CameraBridgeRoute.HostSelf:
                        _routes[lease] = null;
                        _host.Submit(op, token, lease, revision, request, ForgetIfRefused(lease, done));
                        return;
                    case CameraBridgeRoute.Remote or CameraBridgeRoute.AwaitNegotiation:
                        _routes[lease] = peer!.Key;
                        peer.Binding.Submit(op, token, lease, revision, request, ForgetIfRefused(lease, done));
                        return;
                    default:
                        _results.Add((done, TransportResult.Refused(decision.Reason, lease, revision, 0, RefusalText(decision.Reason, peer))));
                        return;
                }
            }
            if (!_routes.TryGetValue(lease, out var key))
            {
                // the route went (its peer left, the world restarted): an End has nothing left to end
                _results.Add((done, op == TransportOp.End
                    ? new TransportResult(true, CameraReasons.None, CameraWireAbi.CommitNone, CameraWireAbi.FlagAlreadyEnded, 0, 0, 0, null, lease, revision)
                    : TransportResult.Refused(CameraReasons.PeerUnavailable, lease, revision, 0, "the player's connection is gone")));
                return;
            }
            if (op == TransportOp.End) _routes.Remove(lease);
            if (key is null) _host.Submit(op, token, lease, revision, request, done);
            else if (_peers.TryGetValue(key, out var p)) p.Binding.Submit(op, token, lease, revision, request, done);
            else _results.Add((done, TransportResult.Refused(CameraReasons.PeerUnavailable, lease, revision, 0, "the player's connection is gone")));
        }
    }

    // A refused apply never became a lease (one that timed out was already let go on the client by its binding): its route
    // goes with the answer, so a mod retrying against a refusing client does not grow the table.
    private Action<TransportResult> ForgetIfRefused(ulong lease, Action<TransportResult> done) => r =>
    {
        if (!r.Ok)
            lock (_server) _routes.Remove(lease);
        done(r);
    };

    // A lease that ended on the bridge's side (the client's own end, a timeout, a peer or world gone) keeps no route.
    private void Ended(RemoteEnded e)
    {
        lock (_server) _routes.Remove(e.Lease);
        _ended(e);
    }

    /// <summary>KilnCameraTransport.caps (any thread): what the player's client offered; the host's own player: this
    /// client's local part rows. False: no such player here.</summary>
    public bool Caps(uint player, out ulong mask)
    {
        mask = 0;
        if (player == 0) return false;
        if (player == _hostPlayer)
        {
            mask = _local.LocalCaps() & CameraWireAbi.RemoteParts;
            return true;
        }
        return _caps.TryGetValue(player, out mask);
    }

    /// <summary>KilnCameraTransport.retire (server tick): the author is gone; its token is never used again.</summary>
    public void Retire(ulong author)
    {
        lock (_server) _tokens.Remove(author);
    }

    private ulong Token(ulong author)
    {
        if (_tokens.TryGetValue(author, out var token)) return token;
        _tokens[author] = token = ++_nextToken;  // per author, never taken from a client
        return token;
    }

    private CameraBridgePeer PeerOf(Peer p) => p.State switch
    {
        NetPeerState.Vanilla => CameraBridgePeer.Vanilla,
        NetPeerState.Ready when !p.HasChannel => CameraBridgePeer.Vanilla,  // a Kiln without kiln.camera: not a camera client
        NetPeerState.Ready when p.Binding.State == ServerBindingState.Bound => CameraBridgePeer.Ready,
        NetPeerState.Ready or NetPeerState.Connecting => CameraBridgePeer.Negotiating,
        _ => CameraBridgePeer.Unavailable,
    };

    private static string RefusalText(ushort reason, Peer? peer) => reason switch
    {
        CameraReasons.RequiresKilnClient when peer is { State: NetPeerState.Ready } =>
            "the player's client runs a Kiln without kiln.camera (an older Kiln): it cannot draw a server's camera",
        CameraReasons.RequiresKilnClient => "the player's client does not run Kiln: a visual camera needs it (a mode lock reaches vanilla clients)",
        CameraReasons.ClientCapability => "the player's client does not offer a camera part this request needs",
        CameraReasons.PeerUnavailable => "the player is not connected to this server",
        _ => "refused",
    };

    // ================================================================ the server role

    /// <summary>Kiln's service tick (the server world's tick): the peers against the network, the received messages, each
    /// binding's step, the host's route; then the outcomes of the step in order.</summary>
    public void ServerPump()
    {
        List<(Action<TransportResult>, TransportResult)> batch;
        lock (_server)
        {
            var world = _net.IsClient && !_net.IsHosting ? 0u : _net.ServerWorld;
            if (world != _serverWorld)
            {
                // a new server world (or none): nothing of the old one goes on
                CloseAll(CameraEndReasons.WorldStopped);
                _serverWorld = world;
                do _serverNonce = _random(); while (_serverNonce == 0);
            }
            if (world != 0)
            {
                SyncPeers();
                while (_serverInbox.TryDequeue(out var m))
                    if (_peers.TryGetValue(m.Peer, out var p)) p.Binding.Receive(m.Id, m.Body);
                foreach (var p in _peers.Values.ToList()) p.Binding.Tick();
                _host.ServerPump();
                var caps = new Dictionary<uint, ulong>(_byPlayer.Count);
                foreach (var (player, p) in _byPlayer)
                    if (p.Binding.State == ServerBindingState.Bound) caps[player] = p.Binding.ClientCaps;
                _caps = caps;
            }
            else
            {
                while (_serverInbox.TryDequeue(out _)) { }
            }
            batch = [.. _results];
            _results.Clear();
        }
        foreach (var (done, result) in batch) done(result);
    }

    private void CloseAll(byte reason)
    {
        foreach (var p in _peers.Values) p.Binding.Close(reason);
        _peers.Clear();
        _byPlayer.Clear();
        _routes.Clear();
        _host.Close(reason);
        _hostPlayer = 0;
        _caps = [];
    }

    private void SyncPeers()
    {
        var hosting = _net.IsClient && _net.IsHosting;
        var local = hosting ? _net.LocalPlayer : 0;
        if (_hostPlayer != local)
        {
            if (_hostPlayer != 0)
            {
                _host.Close(CameraEndReasons.PeerDisconnected);
                foreach (var lease in _routes.Where(r => r.Value is null).Select(r => r.Key).ToList()) _routes.Remove(lease);
            }
            _hostPlayer = local;
        }
        var seen = new HashSet<object>(ReferenceEqualityComparer.Instance);
        foreach (var v in _net.ServerPeers())
        {
            if (v.State is NetPeerState.Closed or NetPeerState.Rejected || v.Player == 0) continue;
            // the hosting client's own connection to its server is the host's route, not a guest
            if (hosting && (v.IsLocalMachine || v.Player == local)) continue;
            if (_peers.TryGetValue(v.Key, out var p) && p.Player != v.Player)
            {
                Drop(p, CameraEndReasons.PeerDisconnected);  // another player on this connection: a new binding
                p = null;
            }
            if (p is null)
            {
                if (_byPlayer.TryGetValue(v.Player, out var stale)) Drop(stale, CameraEndReasons.PeerDisconnected);
                p = new Peer { Key = v.Key, Player = v.Player, Binding = NewBinding(v.Key, v.Player) };
                _peers[v.Key] = p;
                _byPlayer[v.Player] = p;
            }
            p.State = v.State;
            p.HasChannel = v.HasChannel;
            if (v.State == NetPeerState.Ready && v.HasChannel) p.Binding.PeerReady();
            seen.Add(v.Key);
        }
        foreach (var gone in _peers.Values.Where(p => !seen.Contains(p.Key)).ToList()) Drop(gone, CameraEndReasons.PeerDisconnected);
    }

    private void Drop(Peer p, byte reason)
    {
        p.Binding.Close(reason);
        _peers.Remove(p.Key);
        if (_byPlayer.TryGetValue(p.Player, out var q) && ReferenceEquals(q, p)) _byPlayer.Remove(p.Player);
        foreach (var lease in _routes.Where(r => ReferenceEquals(r.Value, p.Key)).Select(r => r.Key).ToList()) _routes.Remove(lease);
    }

    private ServerBinding NewBinding(object key, uint player) => new(new ServerBindingOptions
    {
        Clock = _clock,
        Send = (id, body) => _net.Send(key, (ushort)id, body),
        Random = _random,
        ServerNonce = _serverNonce,
        ServerEpoch = _serverWorld,
        Player = player,
        Incarnation = ++_incarnations,  // the connection's identity: a new binding per connection object and player
        ChannelMaxBody = _channelMax,
        OnEnded = Ended,
    });

    // ================================================================ the client role

    /// <summary>The channel's raw handler (any thread): copies the message into the role's queue.</summary>
    public void OnRaw(object peer, bool toServer, ushort message, ReadOnlySpan<byte> body)
    {
        if (message > (ushort)CameraMessage.Status || body.Length > CameraWireAbi.MaxBody) return;
        var queue = toServer ? _serverInbox : _clientInbox;
        if (queue.Count >= 1024) return;  // a flood is dropped (retries and deadlines cover a lost message)
        queue.Enqueue((peer, (CameraMessage)message, body.ToArray()));
    }

    /// <summary>native kiln.camera's `done` of a bridge LOCAL request (client Pre, or a world-stop thread): queued.</summary>
    public void LocalDone(nint key, in NativeOutcome outcome) => _localDone.Enqueue((key, outcome));

    /// <summary>native kiln.camera's change of a LOCAL lease (any thread): queued.</summary>
    public void LocalChanged(ulong lease, uint kind, byte endReason, ushort reason, ulong frame) => _localChanged.Enqueue((lease, kind, endReason, reason, frame));

    /// <summary>The client world's step: the host's route (hosting) or the guest's binding with its server (joined).</summary>
    public void ClientPump()
    {
        lock (_client)
        {
            while (_localDone.TryDequeue(out var d)) _endpoint.OnDone(d.Key, d.Outcome);
            while (_localChanged.TryDequeue(out var c)) _endpoint.OnChanged(c.Lease, c.Kind, c.EndReason, c.Reason, c.Frame);
            if (!_net.IsClient || _net.ClientWorld == 0)
            {
                StopClient();
                return;
            }
            if (_net.IsHosting)
            {
                CloseBinding();
                _hostClient = true;
                _host.ClientPump(_endpoint);
                return;
            }
            if (_hostClient)
            {
                _host.ClientClose(_endpoint);
                _hostClient = false;
            }
            var link = _net.ServerLink();
            var player = _net.LocalPlayer;
            if (link is not { State: NetPeerState.Ready, HasChannel: true } l || player == 0)
            {
                CloseBinding();
                while (_clientInbox.TryDequeue(out _)) { }
                return;
            }
            var world = _net.ClientWorld;
            if (_binding is null || !ReferenceEquals(_bindingKey.Link, l.Key) || _bindingKey.World != world || _bindingKey.Player != player)
            {
                CloseBinding();
                ulong nonce;
                do nonce = _random(); while (nonce == 0);  // a new one for every client world, also when its number repeats
                var linkKey = l.Key;
                _binding = new ClientBinding(new ClientBindingOptions
                {
                    Clock = _clock,
                    Send = (id, body) => _net.Send(linkKey, (ushort)id, body),
                    Endpoint = _endpoint,
                    ClientNonce = nonce,
                    ClientEpoch = world,
                    LocalEntity = player,
                    Caps = _local.LocalCaps() & CameraWireAbi.RemoteParts,
                    ChannelMaxBody = _channelMax,
                });
                _bindingKey = (linkKey, world, player);
                _binding.Start();
            }
            var b = _binding;
            while (_clientInbox.TryDequeue(out var m))
                if (ReferenceEquals(m.Peer, _bindingKey.Link)) b.Receive(m.Id, m.Body);  // only the negotiated server's
            ReportLocal(b);
            b.Tick();
            ReportLocal(b);
        }
    }

    // the endpoint's answers and own ends, outside any ClientBinding call
    private void ReportLocal(ClientBinding b)
    {
        var (completed, ended) = _endpoint.Take();
        foreach (var (serial, outcome) in completed) b.Complete(serial, outcome);
        foreach (var (lease, endReason, reason, frame) in ended) b.LocalEnded(lease, endReason, reason, frame);
    }

    private void CloseBinding()
    {
        if (_binding is null) return;
        _binding.Close(CameraEndReasons.PeerDisconnected);
        _binding = null;
        _bindingKey = default;
        _endpoint.Take();
    }

    private void StopClient()
    {
        CloseBinding();
        if (_hostClient)
        {
            _host.ClientClose(_endpoint);
            _hostClient = false;
        }
        _endpoint.ReleaseAll();
        _endpoint.Take();
        while (_clientInbox.TryDequeue(out _)) { }
    }
}
