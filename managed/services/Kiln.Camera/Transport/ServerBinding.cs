// SPDX-License-Identifier: MIT
// The server half of the camera protocol (chapter 03 §4.6) for ONE remote peer: the Hello/Welcome negotiation, the binding
// tuple and its token, the endpoint serials, the one request in flight with its ACK deadline and retries, End's priority,
// the heartbeat and the rate limit. No clock, network or randomness of its own: the caller gives a millisecond clock, a
// send delegate and a token source, so tests drive time and loss. Not thread-safe: S8 calls every member on SERVER Pre
// (Receive from the channel's handler, Submit from the bridge, Tick once per step); Close may come from a world-stop
// thread and then runs nothing else.
// Namespace Kiln.CameraTransport, not Kiln.Camera.Transport: a namespace Kiln.Camera would collide with S2's class Kiln.Camera.
namespace Kiln.CameraTransport;

/// <summary>KilnCameraTransport.submit's op values (camera_proposal.h).</summary>
internal enum TransportOp : uint
{
    Apply = 1,
    UpdatePreserve = 2,
    UpdateRestart = 3,
    End = 4,
}

/// <summary>A request's outcome, as KilnCameraResult carries it to native kiln.camera.</summary>
internal readonly record struct TransportResult(bool Ok, ushort Reason, byte Scope, uint Flags, ulong Frame, uint ElapsedMs,
    uint RemainingMs, string? Diagnostic, ulong Lease, ulong Revision)
{
    public bool MayHaveApplied => (Flags & CameraWireAbi.FlagMayHaveApplied) != 0;
    public bool AlreadyEnded => (Flags & CameraWireAbi.FlagAlreadyEnded) != 0;

    public static TransportResult Refused(ushort reason, ulong lease, ulong revision, uint flags = 0, string? text = null) =>
        new(false, reason, CameraWireAbi.CommitNone, flags, 0, 0, 0, text, lease, revision);
}

/// <summary>A lease the client ended by itself (Ended), or that the server's bookkeeping let go: for
/// kiln.camera's transport_ended.</summary>
internal readonly record struct RemoteEnded(ulong Lease, ulong Revision, byte EndReason, ushort Reason, ulong Frame);

internal enum ServerBindingState
{
    /// <summary>The peer is ready; no Hello yet (requests wait, at most 2 s).</summary>
    Waiting,
    /// <summary>Welcome sent: requests go out.</summary>
    Bound,
    /// <summary>No Hello within 2 s of readiness: requests are refused Negotiation until a Hello comes.</summary>
    Failed,
    /// <summary>Peer down or world stopped: everything refused.</summary>
    Closed,
}

/// <summary>What a <see cref="ServerBinding"/> is made with.</summary>
internal sealed class ServerBindingOptions
{
    /// <summary>Monotonic milliseconds (a fake clock in tests).</summary>
    public required Func<long> Clock { get; init; }
    /// <summary>Sends one message to this peer (NetChannel.SendRaw; false/lost is fine: retries and deadlines cover it).</summary>
    public required Action<CameraMessage, byte[]> Send { get; init; }
    /// <summary>A random nonzero 64-bit value per call (binding tokens).</summary>
    public required Func<ulong> Random { get; init; }
    public required ulong ServerNonce { get; init; }
    public required uint ServerEpoch { get; init; }
    /// <summary>The player this binding carries requests for, as the server names it.</summary>
    public required uint Player { get; init; }
    /// <summary>The player's incarnation (SpawnTime, connection identity): a change means a new binding.</summary>
    public required ulong Incarnation { get; init; }
    /// <summary>The channel's own maximum (Network.Config.MaxMessageBytes clamp); the effective body maximum is the
    /// smallest of this, the client's and 16384.</summary>
    public int ChannelMaxBody { get; init; } = CameraWireAbi.MaxBody;
    /// <summary>The part capabilities this server offers remotely (the client's mask is intersected with it).</summary>
    public ulong ServerParts { get; init; } = CameraWireAbi.RemoteParts;
    /// <summary>A lease the client ended, or one the bookkeeping let go (kiln.camera's transport_ended).</summary>
    public Action<RemoteEnded>? OnEnded { get; init; }
}

/// <summary>The server's binding with one remote peer (§4.6).</summary>
internal sealed class ServerBinding
{
    // §4.6 timings
    internal const long NegotiationTimeoutMs = 2000, AckDeadlineMs = 3000, RetryIntervalMs = 1000, HeartbeatIntervalMs = 1000,
        CleanupGraceMs = 3000;
    // Hello's own budget (it is per peer, so per binding): a Hello with a new nonce replaces the binding at most once a
    // second, and a replayed Hello resends the same Welcome at most once a second; the rest are ignored (no Welcome, no
    // state change). The client says Hello again every second until a Welcome comes, so an ignored one is retried.
    internal const long HelloRebindIntervalMs = 1000, WelcomeResendIntervalMs = 1000;
    internal const int MaxRetries = 2, RateBurst = 16, RatePerSecond = 10;

    private sealed class Pending
    {
        public required TransportOp Op;
        public required ulong Owner, Lease, Revision;
        public required CameraWireRequest? Request;
        public required Action<TransportResult> Done;
        public ulong Serial, RequestId;
        public byte[]? Bytes;          // null while parked (no binding yet)
        public long FirstSentAt, LastSentAt;
        public int Retries;
    }

    private readonly ServerBindingOptions _o;
    private ulong _incarnation;
    private ServerBindingState _state = ServerBindingState.Waiting;
    private long _readySince;
    private bool _ready;
    private HelloMessage? _hello;
    private WireBinding _binding;
    private byte[]? _welcome;
    private long _welcomeSentAt;
    private long? _helloRebindAt;   // the last time a Hello replaced a binding (null: never)
    private ulong _clientCaps;
    private int _maxBody = CameraWireAbi.MaxBody, _maxKeys = CameraWireAbi.MaxKeys;
    private ulong _serial, _requestId, _heartbeats;
    private long _lastHeartbeat;
    private Pending? _pending;
    private readonly List<(Action<TransportResult> Done, TransportResult Result)> _results = [];
    // the lease the client holds as far as the server knows: what heartbeats authorize. DurationMs is its request's, and
    // _activeSince the server's clock at the ACK that started its timeline (Apply or a Restart update): the client's
    // reported times are clamped to the Duration, and the bookkeeping never outlives _activeSince + Duration (+ grace)
    private (ulong Lease, ulong Revision, ulong Owner, uint DurationMs)? _active;
    private long _activeHeard, _activeSince;
    private uint _activeRemaining;
    private readonly HashSet<ulong> _excluded = [];   // leases a timeout took out of the heartbeat
    // rate limit: a token bucket of RateBurst refilled at RatePerSecond (Apply and Update; End is never refused)
    private double _tokens = RateBurst;
    private long _tokensAt;

    public ServerBinding(ServerBindingOptions options)
    {
        _o = options ?? throw new ArgumentNullException(nameof(options));
        if (options.ServerNonce == 0 || options.ServerEpoch == 0 || options.Player == 0)
            throw new ArgumentException("a binding needs a nonzero server nonce, server epoch and player");
        _incarnation = options.Incarnation;
        _tokensAt = options.Clock();
    }

    public ServerBindingState State => _state;
    /// <summary>The part capabilities this peer negotiated (0 before Welcome): KilnCameraTransport.caps.</summary>
    public ulong ClientCaps => _state == ServerBindingState.Bound ? _clientCaps : 0;
    /// <summary>The binding tuple of now (default before Welcome).</summary>
    public WireBinding Binding => _binding;
    public int EffectiveMaxBody => _maxBody;
    /// <summary>The highest endpoint serial sent.</summary>
    public ulong HighestSerial => _serial;
    /// <summary>A request in flight (or parked before the binding).</summary>
    public bool Busy => _pending is not null;

    // ---- lifecycle ----

    /// <summary>Network Ready and the player's world are both there: the 2 s negotiation window starts.</summary>
    public void PeerReady()
    {
        if (_state == ServerBindingState.Closed || _ready) return;
        _ready = true;
        _readySince = _o.Clock();
    }

    /// <summary>The player's incarnation changed (SpawnTime or connection identity): the old binding ends, a new token
    /// is sent with a new Welcome when the peer's Hello is known, and old tokens, nonces and leases never act again.</summary>
    public void Rebind(ulong incarnation)
    {
        if (_state == ServerBindingState.Closed || incarnation == _incarnation) return;
        _incarnation = incarnation;
        DropBinding(CameraEndReasons.PeerDisconnected);
        if (_hello is { } h) Bind(h);
    }

    /// <summary>Peer down or the world stopped: every request ends now, on the calling thread (terminal delivery may run on
    /// a world-stop thread; nothing is sent).</summary>
    public void Close(byte endReason = CameraEndReasons.PeerDisconnected)
    {
        if (_state == ServerBindingState.Closed) return;
        DropBinding(endReason);
        _state = ServerBindingState.Closed;
        Deliver();
    }

    // Ends what the old binding carried: the request in flight, the active lease.
    private void DropBinding(byte endReason)
    {
        if (_pending is { } p)
        {
            _pending = null;
            Queue(p, TransportResult.Refused(CameraReasons.PeerUnavailable, p.Lease, p.Revision,
                p.Bytes is not null ? CameraWireAbi.FlagMayHaveApplied : 0, "the peer's binding ended"));
        }
        if (_active is { } a)
        {
            _active = null;
            _o.OnEnded?.Invoke(new RemoteEnded(a.Lease, a.Revision, endReason, CameraReasons.PeerUnavailable, 0));
        }
        _excluded.Clear();
        _welcome = null;
        _binding = default;
        if (_state != ServerBindingState.Closed) _state = ServerBindingState.Waiting;
    }

    // ---- requests ----

    /// <summary>KilnCameraTransport.submit for this peer. The outcome always arrives through <paramref name="done"/> at a
    /// later <see cref="Tick"/> (never inline): Busy, RateLimit, BodyTooLarge, Negotiation, PeerUnavailable, the
    /// client's answer, or TransportTimeout with MayHaveApplied.</summary>
    /// <exception cref="ArgumentException">A request for End, none for Apply/Update, an unknown op or a zero lease.</exception>
    public void Submit(TransportOp op, ulong owner, ulong lease, ulong revision, CameraWireRequest? request, Action<TransportResult> done)
    {
        ArgumentNullException.ThrowIfNull(done);
        if (op is < TransportOp.Apply or > TransportOp.End) throw new ArgumentOutOfRangeException(nameof(op));
        if ((op == TransportOp.End) != (request is null)) throw new ArgumentException("End carries no request; Apply and Update carry one");
        if (lease == 0 || revision == 0 || owner == 0) throw new ArgumentException("a lease, its revision and its owner token are nonzero");
        var p = new Pending { Op = op, Owner = owner, Lease = lease, Revision = revision, Request = request, Done = done };
        if (_state == ServerBindingState.Closed)
        {
            Queue(p, TransportResult.Refused(CameraReasons.PeerUnavailable, lease, revision, 0, "the peer is gone"));
            return;
        }
        if (_state == ServerBindingState.Failed)
        {
            Queue(p, TransportResult.Refused(CameraReasons.Negotiation, lease, revision, 0, "the peer did not negotiate kiln.camera"));
            return;
        }
        if (op == TransportOp.End)
        {
            // End has priority: whatever is in flight is cancelled (it may have reached the client), and the End goes next
            if (_pending is { } old)
            {
                _pending = null;
                Queue(old, TransportResult.Refused(CameraReasons.Cancelled, old.Lease, old.Revision,
                    old.Bytes is not null ? CameraWireAbi.FlagMayHaveApplied : 0, "ended before the client answered"));
            }
        }
        else
        {
            if (_pending is not null)
            {
                Queue(p, TransportResult.Refused(CameraReasons.Busy, lease, revision, 0, "another camera request to this player is in flight"));
                return;
            }
            var now = _o.Clock();
            Refill(now);
            if (_tokens < 1)
            {
                Queue(p, TransportResult.Refused(CameraReasons.RateLimit, lease, revision, 0, "more than 10 camera requests a second to this player"));
                return;
            }
            var bad = CameraWire.Validate(request!, _maxKeys);
            if (bad != CameraReasons.None)
            {
                Queue(p, TransportResult.Refused(bad, lease, revision, 0, "the request does not fit the wire's limits"));
                return;
            }
            var size = CameraWire.BlobSize(request!);
            if (size < 0 || CameraWireAbi.CommonHeaderBytes + 1 + size > Math.Min(_maxBody, Math.Min(_o.ChannelMaxBody, CameraWireAbi.MaxBody)))
            {
                Queue(p, TransportResult.Refused(CameraReasons.BodyTooLarge, lease, revision, 0,
                    $"the request is {CameraWireAbi.CommonHeaderBytes + 1 + size} bytes; the binding takes {Math.Min(_maxBody, _o.ChannelMaxBody)}"));
                return;
            }
            _tokens -= 1;
        }
        _pending = p;
        if (_state == ServerBindingState.Bound) SendPending(p, _o.Clock());
        // Waiting: parked until the Hello (Tick refuses it Negotiation after 2 s)
    }

    private void SendPending(Pending p, long now)
    {
        p.Serial = ++_serial;
        p.RequestId = ++_requestId;
        var header = new WireHeader(_binding, p.Owner, p.Lease, p.RequestId, p.Serial, p.Revision);
        var kind = p.Op switch { TransportOp.Apply => CameraMessage.Apply, TransportOp.End => CameraMessage.End, _ => CameraMessage.Update };
        var timing = p.Op == TransportOp.UpdateRestart ? CameraWireAbi.TimingRestart : CameraWireAbi.TimingPreserve;
        p.Bytes = CameraWire.Mutation(kind, header, timing, p.Request, Math.Min(_maxBody, _o.ChannelMaxBody));
        if (p.Bytes is null)
        {
            // the negotiated limit is smaller than the one it was checked against while parked
            _pending = null;
            Queue(p, TransportResult.Refused(CameraReasons.BodyTooLarge, p.Lease, p.Revision, 0, "the request is larger than the binding takes"));
            return;
        }
        p.FirstSentAt = p.LastSentAt = now;
        _o.Send(kind, p.Bytes);
    }

    // ---- receiving ----

    /// <summary>One message from this peer (S8: the channel's OnRaw handler, after checking the sender is this peer).
    /// Anything malformed, from another binding, or out of turn is dropped without effect.</summary>
    public void Receive(CameraMessage id, ReadOnlySpan<byte> body)
    {
        if (_state == ServerBindingState.Closed) return;
        switch (id)
        {
            case CameraMessage.Hello when CameraWire.TryHello(body, out var hello):
                OnHello(hello);
                break;
            case CameraMessage.Ack when _state == ServerBindingState.Bound && CameraWire.TryAck(body, out var ack) && ack.Header.Binding == _binding:
                OnAck(ack);
                break;
            case CameraMessage.Ended when _state == ServerBindingState.Bound && CameraWire.TryEnded(body, out var ended) && ended.Header.Binding == _binding:
                OnEnded(ended);
                break;
            case CameraMessage.Status when _state == ServerBindingState.Bound && CameraWire.TryStatus(body, out var status) && status.Binding == _binding:
                if (_active is { } a && status.Lease == a.Lease && status.Revision == a.Revision)
                {
                    // a client's number, so never more than the lease's own Duration (Tick also caps it at _activeSince + Duration)
                    _activeHeard = _o.Clock();
                    _activeRemaining = Math.Min(status.RemainingMs, a.DurationMs);
                }
                break;
        }
    }

    private void OnHello(HelloMessage hello)
    {
        var now = _o.Clock();
        // the same peer, world and incarnation: the same Welcome (idempotent), never a new binding; at most once a second
        if (_state == ServerBindingState.Bound && _hello is { } known && known.ClientNonce == hello.ClientNonce &&
            known.ClientEpoch == hello.ClientEpoch && known.LocalEntity == hello.LocalEntity)
        {
            if (now - _welcomeSentAt < WelcomeResendIntervalMs) return;
            _welcomeSentAt = now;
            _o.Send(CameraMessage.Welcome, _welcome!);
            return;
        }
        if (_state == ServerBindingState.Bound)
        {
            // a new client world replaces the binding, at most once a second: a client churning nonces is ignored
            if (_helloRebindAt is { } last && now - last < HelloRebindIntervalMs) return;
            _helloRebindAt = now;
            DropBinding(CameraEndReasons.PeerDisconnected);
        }
        Bind(hello);
    }

    private void Bind(HelloMessage hello)
    {
        _hello = hello;
        ulong token;
        do token = _o.Random(); while (token == 0 || token == _binding.Token);
        _binding = new WireBinding(token, _o.ServerNonce, _o.ServerEpoch, hello.ClientNonce, hello.ClientEpoch, _o.Player, _incarnation);
        _maxBody = (int)Math.Min(Math.Min((uint)CameraWireAbi.MaxBody, hello.MaxBody), (uint)Math.Max(1, _o.ChannelMaxBody));
        _maxKeys = Math.Min(CameraWireAbi.MaxKeys, (int)hello.MaxKeys);
        _clientCaps = hello.Caps & _o.ServerParts & CameraWireAbi.RemoteParts;
        _welcome = CameraWire.Welcome(new WelcomeMessage(hello.ClientNonce, hello.ClientEpoch, _o.ServerNonce, _o.ServerEpoch, _o.Player,
            _incarnation, token, _clientCaps, (uint)_maxBody, (ushort)_maxKeys));
        _state = ServerBindingState.Bound;
        var now = _o.Clock();
        _welcomeSentAt = now;
        _o.Send(CameraMessage.Welcome, _welcome);
        _lastHeartbeat = now;  // the first heartbeat a second after Welcome
        if (_pending is { Bytes: null } parked) SendPending(parked, now);
    }

    private void OnAck(AckMessage ack)
    {
        if (_pending is not { Bytes: not null } p || ack.Header.Serial != p.Serial || ack.Header.RequestId != p.RequestId ||
            ack.Header.Lease != p.Lease || ack.Header.Revision != p.Revision)
            return;  // late, duplicate (a replayed ACK), or for something already given up
        _pending = null;
        var now = _o.Clock();
        // The client's numbers are not trusted: its times are clamped to the Duration it was sent (an ok Apply/Update), else
        // to the protocol's longest lease; the flags to the defined result flags (the decoder refuses others too)
        var visual = ack.Ok && p.Op != TransportOp.End;
        var bound = visual ? p.Request!.DurationMs : CameraWireAbi.MaxDurationMs;
        var elapsed = Math.Min(ack.ElapsedMs, bound);
        var remaining = Math.Min(ack.RemainingMs, bound);
        var scope = ack.Scope;
        var flags = ack.Flags & (CameraWireAbi.FlagMayHaveApplied | CameraWireAbi.FlagAlreadyEnded);
        if (ack.Ok)
        {
            if (p.Op == TransportOp.End)
            {
                if (_active is { } a && a.Lease == p.Lease) _active = null;
            }
            else
            {
                // a Preserve update keeps the lease's timeline (and so its start); Apply and Restart begin a new one
                if (p.Op != TransportOp.UpdatePreserve || _active is not { } cur || cur.Lease != p.Lease) _activeSince = now;
                _active = (p.Lease, p.Revision, p.Owner, bound);
                _activeHeard = now;
                _activeRemaining = remaining;
                scope = CameraWireAbi.CommitClientFrame;  // a successful remote visual result is ClientFrame committed (§4.6)
            }
        }
        Queue(p, new TransportResult(ack.Ok, ack.Reason, scope, flags, ack.Frame, elapsed, remaining,
            ack.Diagnostic.Length == 0 ? null : ack.Diagnostic, p.Lease, p.Revision));
    }

    private void OnEnded(EndedMessage ended)
    {
        // checked against the lease, revision and owner the server has: a stale notification changes nothing
        if (_active is not { } a || ended.Header.Lease != a.Lease || ended.Header.Revision != a.Revision || ended.Header.OwnerToken != a.Owner) return;
        _active = null;
        _o.OnEnded?.Invoke(new RemoteEnded(a.Lease, a.Revision, ended.EndReason, ended.Reason, ended.Frame));
    }

    // ---- time ----

    /// <summary>SERVER Pre, once a step: the negotiation window, retries and the ACK deadline, the heartbeat, the
    /// bookkeeping expiry; then the outcomes of the step are handed over in order.</summary>
    public void Tick()
    {
        var now = _o.Clock();
        if (_state is ServerBindingState.Waiting && _ready && now - _readySince >= NegotiationTimeoutMs)
        {
            _state = ServerBindingState.Failed;
            if (_pending is { } parked)
            {
                _pending = null;
                Queue(parked, TransportResult.Refused(CameraReasons.Negotiation, parked.Lease, parked.Revision, 0,
                    "the peer did not negotiate kiln.camera within 2 s"));
            }
        }
        if (_pending is { Bytes: not null } p)
        {
            if (now - p.FirstSentAt >= AckDeadlineMs)
                TimeOut(p, now);
            else if (p.Retries < MaxRetries && now - p.LastSentAt >= RetryIntervalMs)
            {
                // the SAME serial and request again, never a new Apply
                p.Retries++;
                p.LastSentAt = now;
                _o.Send(p.Op switch { TransportOp.Apply => CameraMessage.Apply, TransportOp.End => CameraMessage.End, _ => CameraMessage.Update }, p.Bytes);
            }
        }
        if (_state == ServerBindingState.Bound && now - _lastHeartbeat >= HeartbeatIntervalMs)
        {
            _lastHeartbeat = now;
            var (lease, revision) = Authorized();
            _o.Send(CameraMessage.Heartbeat, CameraWire.Heartbeat(new HeartbeatMessage(_binding, ++_heartbeats, _serial, lease, revision)));
        }
        if (_active is { } a && now - Math.Min(_activeHeard + _activeRemaining, _activeSince + a.DurationMs) >= CleanupGraceMs)
        {
            // bookkeeping only: the client expires by itself; no word from it for its remaining time plus the grace, and
            // never later than the lease's Duration after its timeline started plus the grace, whatever Status says
            _active = null;
            _o.OnEnded?.Invoke(new RemoteEnded(a.Lease, a.Revision, CameraEndReasons.Expired, CameraReasons.None, 0));
        }
        Deliver();
    }

    // What the heartbeat authorizes: the request in flight (so an ACK still on its way does not lose its lease), else the
    // active lease; never a lease a timeout excluded; 0/0 clears.
    private (ulong Lease, ulong Revision) Authorized()
    {
        if (_pending is { Bytes: not null, Op: not TransportOp.End } p && !_excluded.Contains(p.Lease)) return (p.Lease, p.Revision);
        if (_pending is { Bytes: not null, Op: TransportOp.End }) return (0, 0);
        if (_active is { } a && !_excluded.Contains(a.Lease)) return (a.Lease, a.Revision);
        return (0, 0);
    }

    private void TimeOut(Pending p, long now)
    {
        _pending = null;
        // honest uncertainty: the client may have shown it; a higher-serial End follows and the heartbeat stops covering it
        _excluded.Add(p.Lease);
        if (_active is { } a && a.Lease == p.Lease) _active = null;
        Queue(p, TransportResult.Refused(CameraReasons.TransportTimeout, p.Lease, p.Revision, CameraWireAbi.FlagMayHaveApplied,
            "the client did not answer within 3 s"));
        if (p.Op != TransportOp.End)
        {
            var end = new WireHeader(_binding, p.Owner, p.Lease, ++_requestId, ++_serial, p.Revision);
            _o.Send(CameraMessage.End, CameraWire.Mutation(CameraMessage.End, end, 0, null, _maxBody)!);
        }
    }

    private void Refill(long now)
    {
        _tokens = Math.Min(RateBurst, _tokens + (now - _tokensAt) * RatePerSecond / 1000.0);
        _tokensAt = now;
    }

    private void Queue(Pending p, TransportResult r) => _results.Add((p.Done, r));

    private void Deliver()
    {
        if (_results.Count == 0) return;
        var batch = _results.ToArray();
        _results.Clear();
        foreach (var (done, result) in batch) done(result);
    }
}
