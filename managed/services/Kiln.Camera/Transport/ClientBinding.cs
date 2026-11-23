// SPDX-License-Identifier: MIT
// The client half of the camera protocol (chapter 03 §4.6): Hello until Welcome, the binding it accepts, and the receiver
// of the server's requests: serials (lower cannot mutate, equal replays its ACK), the 256 ACKs and 256 tombstones kept
// for 10 s, End's exact lease and owner check, the 1 s confirming-frame deadline, the 3 s authority heartbeat, Status, and
// the client's own expiry at Duration. The camera itself is the native LOCAL endpoint behind IRemoteCameraEndpoint (S8).
// No clock or network of its own; not thread-safe: S8 calls every member on CLIENT Pre (Close may come from a world-stop
// thread and then runs nothing else).
// Namespace Kiln.CameraTransport, not Kiln.Camera.Transport: a namespace Kiln.Camera would collide with S2's class Kiln.Camera.
namespace Kiln.CameraTransport;

/// <summary>A request forwarded to the native LOCAL endpoint.</summary>
/// <param name="Kind">Apply, Update or End.</param>
/// <param name="Serial">The endpoint serial: the key of <see cref="ClientBinding.Complete"/>.</param>
/// <param name="Lease">The server's lease id (the client keys its remote lease by it).</param>
/// <param name="Revision">The lease's revision this request carries.</param>
/// <param name="OwnerToken">The framework owner token of the request's author (never one a client chose).</param>
/// <param name="Timing">Update only: Preserve 0 / Restart 1.</param>
/// <param name="Request">Apply/Update: the complete request; End: null.</param>
internal readonly record struct RemoteRequest(CameraMessage Kind, ulong Serial, ulong Lease, ulong Revision, ulong OwnerToken,
    byte Timing, CameraWireRequest? Request);

/// <summary>The native endpoint's answer to a forwarded request (its KilnCameraResult).</summary>
internal readonly record struct NativeOutcome(bool Ok, ushort Reason, byte Scope, uint Flags, ulong Frame, string? Diagnostic = null);

/// <summary>The client's native LOCAL camera endpoint as the receiver drives it (S8 implements it over kiln.camera).</summary>
internal interface IRemoteCameraEndpoint
{
    /// <summary>Starts a forwarded request. <see cref="CameraReasons.None"/>: accepted, its outcome comes later through
    /// <see cref="ClientBinding.Complete"/>; any other value: refused now (nothing started).</summary>
    ushort Start(in RemoteRequest request);

    /// <summary>Stops the pending work of <paramref name="serial"/> (its frame deadline passed, or an End preempts it); a
    /// later outcome for it is ignored.</summary>
    void Cancel(ulong serial);

    /// <summary>Ends the remote lease locally at once, with no answer to the server (authority lost, a new binding, the
    /// world or the peer gone, the client's own expiry).</summary>
    void Release(ulong lease, byte endReason);
}

internal enum ClientBindingState
{
    /// <summary>Not started (before Network Ready and the local player's world).</summary>
    Idle,
    /// <summary>Hello sent, no Welcome yet.</summary>
    Hello,
    /// <summary>Welcome accepted: requests from this binding are taken.</summary>
    Bound,
    /// <summary>World stopped or server gone.</summary>
    Closed,
}

/// <summary>What a <see cref="ClientBinding"/> is made with.</summary>
internal sealed class ClientBindingOptions
{
    public required Func<long> Clock { get; init; }
    /// <summary>Sends one message to the server (NetChannel.SendRaw to Network.Server).</summary>
    public required Action<CameraMessage, byte[]> Send { get; init; }
    public required IRemoteCameraEndpoint Endpoint { get; init; }
    /// <summary>This client world's random nonce (new for every world start, also when an epoch number repeats).</summary>
    public required ulong ClientNonce { get; init; }
    public required uint ClientEpoch { get; init; }
    public required uint LocalEntity { get; init; }
    /// <summary>The local part capabilities this client offers (camera.local_* rows that are on).</summary>
    public required ulong Caps { get; init; }
    public int ChannelMaxBody { get; init; } = CameraWireAbi.MaxBody;
}

/// <summary>The client's receiver for one server binding (§4.6).</summary>
internal sealed class ClientBinding
{
    internal const long HelloRetryMs = 1000, FrameDeadlineMs = 1000, AuthorityTimeoutMs = 3000, CacheTtlMs = 10000;
    internal const int AckCacheSize = 256, TombstoneSize = 256;

    private sealed class PendingRemote
    {
        public required RemoteRequest Request;
        public required WireHeader Header;
        public required long ReceivedAt;
    }

    private sealed class Active
    {
        public required ulong Lease, Revision, Owner, EstablishedBySerial;
        public required uint DurationMs;
        public required long T0;
    }

    private readonly ClientBindingOptions _o;
    private ClientBindingState _state = ClientBindingState.Idle;
    private long _lastHello;
    private WireBinding _binding;
    private int _maxBody = CameraWireAbi.MaxBody, _maxKeys = CameraWireAbi.MaxKeys;
    private ulong _allowedCaps;
    private ulong _highestSerial;
    private long _lastAuthority;
    private PendingRemote? _pending;
    private Active? _active;
    // serial -> (ACK bytes, when), oldest first; lease -> when ended
    private readonly Dictionary<ulong, (byte[] Ack, long At)> _acks = [];
    private readonly Queue<ulong> _ackOrder = new();
    private readonly Dictionary<ulong, long> _tombstones = [];
    private readonly Queue<ulong> _tombOrder = new();

    public ClientBinding(ClientBindingOptions options)
    {
        _o = options ?? throw new ArgumentNullException(nameof(options));
        if (options.ClientNonce == 0 || options.ClientEpoch == 0 || options.LocalEntity == 0)
            throw new ArgumentException("a client binding needs a nonzero nonce, epoch and local entity");
    }

    public ClientBindingState State => _state;
    public WireBinding Binding => _binding;
    /// <summary>The highest endpoint serial seen under this binding (kept past cache eviction).</summary>
    public ulong HighestSerial => _highestSerial;
    /// <summary>The remote lease the client holds (0 when none).</summary>
    public ulong ActiveLease => _active?.Lease ?? 0;
    public ulong ActiveRevision => _active?.Revision ?? 0;
    public int AckCount => _acks.Count;
    public int TombstoneCount => _tombstones.Count;
    public ulong AllowedCaps => _allowedCaps;

    /// <summary>Network Ready and the local player and world are ready: Hello now, then every second until Welcome.</summary>
    public void Start()
    {
        if (_state != ClientBindingState.Idle) return;
        _state = ClientBindingState.Hello;
        SendHello(_o.Clock());
    }

    private void SendHello(long now)
    {
        _lastHello = now;
        _o.Send(CameraMessage.Hello, CameraWire.Hello(new HelloMessage(CameraWireAbi.Protocol, _o.ClientNonce, _o.ClientEpoch, _o.LocalEntity,
            _o.Caps & CameraWireAbi.RemoteParts, CameraWireAbi.MaxKeys, (uint)Math.Min(CameraWireAbi.MaxBody, Math.Max(1, _o.ChannelMaxBody)))));
    }

    /// <summary>World stopped or the server went away: the remote lease ends locally, nothing is sent.</summary>
    public void Close(byte endReason = CameraEndReasons.PeerDisconnected)
    {
        if (_state == ClientBindingState.Closed) return;
        ClearRemote(endReason);
        _state = ClientBindingState.Closed;
    }

    // ---- receiving (S8: only messages from the negotiated SERVER peer reach this) ----

    public void Receive(CameraMessage id, ReadOnlySpan<byte> body)
    {
        if (_state is ClientBindingState.Idle or ClientBindingState.Closed) return;
        switch (id)
        {
            case CameraMessage.Welcome when CameraWire.TryWelcome(body, out var w):
                OnWelcome(w);
                break;
            case CameraMessage.Apply or CameraMessage.Update or CameraMessage.End when _state == ClientBindingState.Bound:
                OnMutation(id, body);
                break;
            case CameraMessage.Heartbeat when _state == ClientBindingState.Bound && CameraWire.TryHeartbeat(body, out var hb) && hb.Binding == _binding:
                OnHeartbeat(hb);
                break;
        }
    }

    private void OnWelcome(WelcomeMessage w)
    {
        if (w.ClientNonce != _o.ClientNonce || w.ClientEpoch != _o.ClientEpoch) return;  // for another client world
        if (_state == ClientBindingState.Bound && w.Token == _binding.Token) return;    // a repeated Welcome
        // a new token: the old binding's remote owners go before the new one is accepted, and its caches reset
        ClearRemote(CameraEndReasons.PeerDisconnected);
        _acks.Clear();
        _ackOrder.Clear();
        _tombstones.Clear();
        _tombOrder.Clear();
        _highestSerial = 0;
        _binding = new WireBinding(w.Token, w.ServerNonce, w.ServerEpoch, w.ClientNonce, w.ClientEpoch, w.Player, w.Incarnation);
        _maxBody = (int)Math.Min(w.MaxBody, (uint)Math.Min(CameraWireAbi.MaxBody, Math.Max(1, _o.ChannelMaxBody)));
        _maxKeys = Math.Min(CameraWireAbi.MaxKeys, (int)w.MaxKeys);
        _allowedCaps = w.AllowedCaps & _o.Caps;
        _state = ClientBindingState.Bound;
        _lastAuthority = _o.Clock();
    }

    private void OnMutation(CameraMessage kind, ReadOnlySpan<byte> body)
    {
        // the header alone first: a body past the limit or from another binding is dropped without effect
        if (!CameraWire.TryHeaderOnly(body, _maxBody, out var h) || h.Binding != _binding) return;
        var now = _o.Clock();
        if (_acks.TryGetValue(h.Serial, out var cached))
        {
            _o.Send(CameraMessage.Ack, cached.Ack);  // an equal serial replays its ACK, never applies again
            return;
        }
        if (_pending is { } waiting && waiting.Request.Serial == h.Serial) return;  // the same request, still pending: its ACK comes
        if (h.Serial <= _highestSerial) return;  // lower: cannot mutate (also after its ACK left the cache)
        _highestSerial = h.Serial;
        if (!CameraWire.TryMutation(kind, body, _maxBody, _maxKeys, out var m))
        {
            Answer(h, false, CameraReasons.MalformedRemote, 0, 0, "malformed camera request");  // a bounded refusal, no change
            return;
        }
        var msg = m!;
        switch (kind)
        {
            case CameraMessage.Apply:
                if (_tombstones.ContainsKey(h.Lease) || (_active is { } act && act.Lease == h.Lease))
                {
                    Answer(h, false, CameraReasons.Cancelled, 0, 0, "that lease has ended; it is never revived");
                    return;
                }
                if (msg.Request!.Has(CameraWireAbi.PartMode))
                {
                    Answer(h, false, CameraReasons.WrongEndpoint, 0, 0, "Mode is the server's, not a visual request");
                    return;
                }
                if (!CapsAllow(msg.Request!))
                {
                    Answer(h, false, CameraReasons.ClientCapability, 0, 0, "this client does not offer a part the request needs");
                    return;
                }
                break;
            case CameraMessage.Update:
                if (_active is not { } cur || cur.Lease != h.Lease || cur.Owner != h.OwnerToken)
                {
                    Answer(h, false, CameraReasons.UnknownLease, 0, 0, "no such remote lease to update");  // the active revision stays
                    return;
                }
                if (!CapsAllow(msg.Request!))
                {
                    Answer(h, false, CameraReasons.ClientCapability, 0, 0, "this client does not offer a part the request needs");
                    return;
                }
                break;
            case CameraMessage.End:
                if (_tombstones.ContainsKey(h.Lease))
                {
                    Answer(h, true, CameraReasons.None, 0, CameraWireAbi.FlagAlreadyEnded, null);  // owned terminal tombstone
                    return;
                }
                if (_pending is { } pend && pend.Request.Lease == h.Lease && pend.Request.OwnerToken == h.OwnerToken &&
                    pend.Request.Kind == CameraMessage.Apply)
                {
                    // End preempts an Apply still waiting for its frame: cancelled before it applied
                    _o.Endpoint.Cancel(pend.Request.Serial);
                    _pending = null;
                    Tombstone(h.Lease, now);
                    Answer(h, true, CameraReasons.None, 0, 0, null);
                    return;
                }
                if (_active is not { } a || a.Lease != h.Lease || a.Owner != h.OwnerToken)
                {
                    Answer(h, false, CameraReasons.UnknownLease, 0, 0, "an End for a lease this client does not hold");  // an old End
                    return;
                }
                if (_pending is { } upd)
                {
                    _o.Endpoint.Cancel(upd.Request.Serial);  // End has priority over a pending Update
                    _pending = null;
                }
                break;
        }
        if (_pending is { } stale)
        {
            // a newer request while one waits: the server gave that one up (its deadline passed)
            _o.Endpoint.Cancel(stale.Request.Serial);
            _pending = null;
        }
        var forwarded = new RemoteRequest(kind, h.Serial, h.Lease, h.Revision, h.OwnerToken, msg.Timing, msg.Request);
        var refused = _o.Endpoint.Start(forwarded);
        if (refused != CameraReasons.None)
        {
            Answer(h, false, refused, 0, 0, null);
            return;
        }
        _pending = new PendingRemote { Request = forwarded, Header = h, ReceivedAt = now };
    }

    // a request needs the client's negotiated part rows: Path -> pose, Profile -> profile, Lens -> fov, Shake -> shake
    private bool CapsAllow(CameraWireRequest r) =>
        (!r.Has(CameraWireAbi.PartPath) || (_allowedCaps & CameraWireAbi.CapLocalPose) != 0) &&
        (!r.Has(CameraWireAbi.PartProfile) || (_allowedCaps & CameraWireAbi.CapLocalProfile) != 0) &&
        (!r.Has(CameraWireAbi.PartLens) || (_allowedCaps & CameraWireAbi.CapLocalFov) != 0) &&
        (!r.Has(CameraWireAbi.PartShake) || (_allowedCaps & CameraWireAbi.CapLocalShake) != 0);

    /// <summary>The native endpoint's outcome for a forwarded request (S8, on CLIENT Pre when kiln.camera's done runs).
    /// An outcome for a serial that is no longer pending (cancelled, timed out, replaced) is ignored.</summary>
    public void Complete(ulong serial, in NativeOutcome outcome)
    {
        if (_state != ClientBindingState.Bound || _pending is not { } p || p.Request.Serial != serial) return;
        _pending = null;
        var now = _o.Clock();
        var r = p.Request;
        if (outcome.Ok)
        {
            switch (r.Kind)
            {
                case CameraMessage.Apply:
                    _active = new Active
                    {
                        Lease = r.Lease, Revision = r.Revision, Owner = r.OwnerToken, EstablishedBySerial = r.Serial,
                        DurationMs = r.Request!.DurationMs, T0 = now,
                    };
                    break;
                case CameraMessage.Update when _active is { } a && a.Lease == r.Lease:
                    a.Revision = r.Revision;
                    a.DurationMs = r.Request!.DurationMs;
                    if (r.Timing == CameraWireAbi.TimingRestart) a.T0 = now;  // Preserve keeps T0
                    break;
                case CameraMessage.End:
                    _active = null;
                    Tombstone(r.Lease, now);
                    break;
            }
        }
        var (elapsed, remaining) = Times(now);
        Answer(p.Header, outcome.Ok, outcome.Ok ? CameraReasons.None : outcome.Reason, outcome.Scope, outcome.Flags, outcome.Diagnostic, outcome.Frame,
            elapsed, remaining);
    }

    /// <summary>The native endpoint ended a remote lease by itself (expired, target lost, a safety refusal): the server is
    /// told with Ended, and the lease is tombstoned.</summary>
    public void LocalEnded(ulong lease, byte endReason, ushort reason, ulong frame)
    {
        if (_state != ClientBindingState.Bound || _active is not { } a || a.Lease != lease) return;
        _active = null;
        Tombstone(lease, _o.Clock());
        _o.Send(CameraMessage.Ended, CameraWire.Ended(new EndedMessage(
            new WireHeader(_binding, a.Owner, a.Lease, 0, _highestSerial, a.Revision), endReason, reason, frame)));
    }

    private void OnHeartbeat(HeartbeatMessage hb)
    {
        var now = _o.Clock();
        _lastAuthority = now;
        // the server's authority covers one lease (or none). Acted on only once the server has sent everything this client
        // holds (HighestSerial), so an answer still on its way never loses its lease.
        if (_active is { } a && hb.HighestSerial >= a.EstablishedBySerial && hb.Lease != a.Lease)
        {
            _active = null;
            _o.Endpoint.Release(a.Lease, CameraEndReasons.AuthorityTimeout);
            Tombstone(a.Lease, now);
        }
        if (_pending is { } p && hb.HighestSerial >= p.Request.Serial && hb.Lease != p.Request.Lease && p.Request.Kind == CameraMessage.Apply)
        {
            _o.Endpoint.Cancel(p.Request.Serial);
            _pending = null;
            Tombstone(p.Request.Lease, now);
        }
        var (elapsed, remaining) = Times(now);
        _o.Send(CameraMessage.Status, CameraWire.Status(new StatusMessage(_binding, hb.Sequence, _active?.Lease ?? 0, _active?.Revision ?? 0, elapsed, remaining)));
    }

    // ---- time ----

    /// <summary>CLIENT Pre, once a step: Hello retries, the 1 s frame deadline, the 3 s authority, the client's own expiry
    /// at Duration, and the 10 s cache age.</summary>
    public void Tick()
    {
        var now = _o.Clock();
        if (_state == ClientBindingState.Hello && now - _lastHello >= HelloRetryMs) SendHello(now);
        if (_state != ClientBindingState.Bound) return;
        if (_pending is { } p && p.Request.Kind != CameraMessage.End && now - p.ReceivedAt >= FrameDeadlineMs)
        {
            // no usable confirming frame within 1 s: refused NoCameraFrame; the attempt is stopped, and it may have drawn
            _o.Endpoint.Cancel(p.Request.Serial);
            _pending = null;
            if (p.Request.Kind == CameraMessage.Apply) Tombstone(p.Request.Lease, now);
            Answer(p.Header, false, CameraReasons.NoCameraFrame, 0, CameraWireAbi.FlagMayHaveApplied, "no camera frame within 1 s");
        }
        if (now - _lastAuthority >= AuthorityTimeoutMs) ClearRemote(CameraEndReasons.AuthorityTimeout);
        if (_active is { } a && now - a.T0 >= a.DurationMs)
        {
            // the client expires at Duration by itself, also when the server's End was lost
            _o.Endpoint.Release(a.Lease, CameraEndReasons.Expired);
            LocalEnded(a.Lease, CameraEndReasons.Expired, CameraReasons.None, 0);
        }
        Age(now);
    }

    private (uint Elapsed, uint Remaining) Times(long now)
    {
        if (_active is not { } a) return (0, 0);
        var elapsed = (ulong)Math.Max(0, now - a.T0);
        return ((uint)Math.Min(elapsed, uint.MaxValue), (uint)Math.Max(0, (long)a.DurationMs - (long)elapsed));
    }

    // The remote leases of this binding end locally (authority lost, a new binding, world stop, peer gone).
    private void ClearRemote(byte endReason)
    {
        var now = _o.Clock();
        if (_pending is { } p)
        {
            _o.Endpoint.Cancel(p.Request.Serial);
            _pending = null;
            if (p.Request.Kind == CameraMessage.Apply) Tombstone(p.Request.Lease, now);
        }
        if (_active is { } a)
        {
            _active = null;
            _o.Endpoint.Release(a.Lease, endReason);
            Tombstone(a.Lease, now);
        }
    }

    private void Answer(in WireHeader h, bool ok, ushort reason, byte scope, uint flags, string? text, ulong frame = 0,
        uint elapsed = 0, uint remaining = 0)
    {
        var ack = CameraWire.Ack(new AckMessage(h, ok, reason, scope, flags, frame, elapsed, remaining, text ?? ""));
        var now = _o.Clock();
        _acks[h.Serial] = (ack, now);
        _ackOrder.Enqueue(h.Serial);
        while (_acks.Count > AckCacheSize && _ackOrder.TryDequeue(out var oldest)) _acks.Remove(oldest);
        _o.Send(CameraMessage.Ack, ack);
    }

    private void Tombstone(ulong lease, long now)
    {
        if (lease == 0 || !_tombstones.TryAdd(lease, now)) return;
        _tombOrder.Enqueue(lease);
        while (_tombstones.Count > TombstoneSize && _tombOrder.TryDequeue(out var oldest)) _tombstones.Remove(oldest);
    }

    private void Age(long now)
    {
        while (_ackOrder.TryPeek(out var s))
        {
            if (_acks.TryGetValue(s, out var e) && now - e.At < CacheTtlMs) break;
            _ackOrder.Dequeue();
            if (_acks.TryGetValue(s, out e) && now - e.At >= CacheTtlMs) _acks.Remove(s);
        }
        while (_tombOrder.TryPeek(out var l))
        {
            if (_tombstones.TryGetValue(l, out var at) && now - at < CacheTtlMs) break;
            _tombOrder.Dequeue();
            if (_tombstones.TryGetValue(l, out at) && now - at >= CacheTtlMs) _tombstones.Remove(l);
        }
    }
}
