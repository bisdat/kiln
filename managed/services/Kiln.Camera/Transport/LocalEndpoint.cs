// SPDX-License-Identifier: MIT
// The receiving end of a remote camera request (chapter 03 §4.6): the native LOCAL_VISUAL endpoint as the bridge drives
// it, for a joined guest (behind ClientBinding) and for the host's own player (behind HostSelfRoute, a local queue with no
// packets). Each remote author (the server's owner token) gets a native owner of its own on this side, so the native
// lease model decides between two server authors exactly as the server did (priority, supersession), and a local
// client mod's lease competes with them by the same rules. Client world thread only; the native answers arrive through
// queues the callbacks fill (OnDone, OnChanged), drained by the bridge's client pump.
namespace Kiln.CameraTransport;

/// <summary>native kiln.camera's LOCAL_VISUAL entries as the bridge uses them (a fake in tests).</summary>
internal interface ICameraLocalNative
{
    /// <summary>The local part bits that are on now (capabilities(LOCAL_VISUAL), local parts only). Any thread.</summary>
    ulong LocalCaps();

    /// <summary>A native owner of the bridge for one remote author; 0 when none can be made. Client thread.</summary>
    ulong OwnerFor(ulong ownerToken);

    /// <summary>apply/update/end on LOCAL_VISUAL as <paramref name="owner"/>. The native rc; KILN_OK: the outcome comes
    /// later through <see cref="LocalEndpoint.OnDone"/> with <paramref name="key"/>.</summary>
    int Apply(ulong owner, CameraWireRequest request, nint key, out ulong lease);

    int Update(ulong owner, ulong lease, byte timing, CameraWireRequest request, nint key);

    int End(ulong owner, ulong lease, nint key);
}

/// <summary>The native LOCAL endpoint behind a receiver (<see cref="IRemoteCameraEndpoint"/>).</summary>
internal sealed class LocalEndpoint : IRemoteCameraEndpoint
{
    internal const int MaxOwners = 32;
    // native rcs (kiln_api.h)
    private const int Ok = 0, InvalidArgument = 1, NotFound = 2, Unavailable = 3, WrongThread = 4, BufferTooSmall = 5, Revoked = 6;

    private sealed class Op
    {
        public required ulong Serial;
        public required CameraMessage Kind;
        public required ulong Remote, Local, Owner;
        public bool Silent;
    }

    private readonly ICameraLocalNative _n;
    private readonly Dictionary<ulong, (ulong Local, ulong Owner)> _leases = [];  // remote lease -> its native local lease
    private readonly Dictionary<nint, Op> _ops = [];
    private readonly HashSet<ulong> _ending = [];   // local leases this endpoint ended: their ENDED is not the client's own
    private readonly Dictionary<ulong, ulong> _owners = [];  // owner token -> native owner (at most MaxOwners)
    private readonly List<(ulong Serial, NativeOutcome Outcome)> _completed = [];
    private readonly List<(ulong Lease, byte EndReason, ushort Reason, ulong Frame)> _ended = [];
    private long _nextKey;

    public LocalEndpoint(ICameraLocalNative native) => _n = native ?? throw new ArgumentNullException(nameof(native));

    /// <summary>Remote leases this endpoint carries now.</summary>
    public int Leases => _leases.Count;

    /// <summary>Requests whose native answer has not come.</summary>
    public int Waiting => _ops.Count;

    public ushort Start(in RemoteRequest r)
    {
        ulong local;
        var key = (nint)(++_nextKey);
        int rc;
        switch (r.Kind)
        {
            case CameraMessage.Apply:
            {
                if (_leases.ContainsKey(r.Lease)) return CameraReasons.Cancelled;  // a lease is never revived
                var owner = Owner(r.OwnerToken);
                if (owner == 0) return CameraReasons.ResourceBusy;
                rc = _n.Apply(owner, r.Request!, key, out local);
                if (rc != Ok) return Refusal(rc);
                _ops[key] = new Op { Serial = r.Serial, Kind = r.Kind, Remote = r.Lease, Local = local, Owner = owner };
                _leases[r.Lease] = (local, owner);
                return CameraReasons.None;
            }
            case CameraMessage.Update:
            {
                if (!_leases.TryGetValue(r.Lease, out var held)) return CameraReasons.UnknownLease;
                rc = _n.Update(held.Owner, held.Local, r.Timing, r.Request!, key);
                if (rc != Ok) return Refusal(rc);
                _ops[key] = new Op { Serial = r.Serial, Kind = r.Kind, Remote = r.Lease, Local = held.Local, Owner = held.Owner };
                return CameraReasons.None;
            }
            case CameraMessage.End:
            {
                if (!_leases.TryGetValue(r.Lease, out var held)) return CameraReasons.UnknownLease;
                _leases.Remove(r.Lease);
                _ending.Add(held.Local);
                rc = _n.End(held.Owner, held.Local, key);
                if (rc != Ok) return Refusal(rc);
                _ops[key] = new Op { Serial = r.Serial, Kind = r.Kind, Remote = r.Lease, Local = held.Local, Owner = held.Owner };
                return CameraReasons.None;
            }
            default:
                return CameraReasons.MalformedRemote;
        }
    }

    public void Cancel(ulong serial)
    {
        foreach (var (key, op) in _ops)
        {
            if (op.Serial != serial || op.Silent) continue;
            op.Silent = true;
            // an apply given up (no frame within 1 s, an End before it, a newer request): stop it locally, whatever it did
            if (op.Kind == CameraMessage.Apply && _leases.TryGetValue(op.Remote, out var held) && held.Local == op.Local) EndSilently(op.Remote);
            return;
        }
    }

    public void Release(ulong lease, byte endReason) => EndSilently(lease);

    /// <summary>Ends every remote lease this endpoint carries (a binding that ends, a world that stops, hosting that
    /// stops); nothing is answered.</summary>
    public void ReleaseAll()
    {
        foreach (var remote in _leases.Keys.ToList()) EndSilently(remote);
        foreach (var op in _ops.Values) op.Silent = true;
    }

    private void EndSilently(ulong remote)
    {
        if (!_leases.Remove(remote, out var held)) return;
        _ending.Add(held.Local);
        var key = (nint)(++_nextKey);
        if (_n.End(held.Owner, held.Local, key) == Ok)
            _ops[key] = new Op { Serial = 0, Kind = CameraMessage.End, Remote = remote, Local = held.Local, Owner = held.Owner, Silent = true };
    }

    // One native owner per remote author, reused once its leases are gone; at most MaxOwners.
    private ulong Owner(ulong token)
    {
        if (_owners.TryGetValue(token, out var owner)) return owner;
        if (_owners.Count >= MaxOwners)
        {
            var busy = _leases.Values.Select(l => l.Owner).ToHashSet();
            var free = _owners.FirstOrDefault(o => !busy.Contains(o.Value));
            if (free.Value == 0) return 0;
            _owners.Remove(free.Key);
            _owners[token] = free.Value;
            return free.Value;
        }
        owner = _n.OwnerFor(token);
        if (owner != 0) _owners[token] = owner;
        return owner;
    }

    private static ushort Refusal(int rc) => rc switch
    {
        InvalidArgument => CameraReasons.MalformedRemote,
        Unavailable => CameraReasons.ClientCapability,
        NotFound or WrongThread => CameraReasons.NoCameraFrame,
        BufferTooSmall => CameraReasons.RateLimit,
        Revoked => CameraReasons.PeerUnavailable,
        _ => CameraReasons.MalformedRemote,
    };

    /// <summary>A native `done` of one of this endpoint's requests (client world thread, from the queue).</summary>
    public void OnDone(nint key, in NativeOutcome outcome)
    {
        if (!_ops.Remove(key, out var op) || op.Silent) return;
        if (!outcome.Ok && op.Kind == CameraMessage.Apply && _leases.TryGetValue(op.Remote, out var held) && held.Local == op.Local) _leases.Remove(op.Remote);
        _completed.Add((op.Serial, outcome));
    }

    /// <summary>A native change of a LOCAL lease (client world thread, from the queue): an ENDED this endpoint did not
    /// ask for is the client's own end of that remote lease (expired, target lost, superseded by a local mod ...).</summary>
    public void OnChanged(ulong local, uint kind, byte endReason, ushort reason, ulong frame)
    {
        if (kind != 3) return;  // KILN_CAMERA_ENDED
        if (_ending.Remove(local)) return;
        foreach (var (remote, held) in _leases)
        {
            if (held.Local != local) continue;
            _leases.Remove(remote);
            _ended.Add((remote, endReason, reason, frame));
            return;
        }
    }

    /// <summary>The answers and own ends gathered since the last call, in order (the receiver reports them outside its own
    /// methods, so no receiver call re-enters another).</summary>
    public (List<(ulong Serial, NativeOutcome Outcome)> Completed, List<(ulong Lease, byte EndReason, ushort Reason, ulong Frame)> Ended) Take()
    {
        var c = _completed.ToList();
        var e = _ended.ToList();
        _completed.Clear();
        _ended.Clear();
        return (c, e);
    }
}

/// <summary>
/// The host's own player (§4.4, §4.6): a server request for the player of this hosting client goes from the SERVER Pre to
/// the CLIENT Pre through a queue, into the native LOCAL endpoint, and its answer comes back the same way. No packet, no
/// handshake, no heartbeat; the host's local part rows decide. The two halves run on two threads (the server world's
/// and the client world's): they share only the two queues.
/// </summary>
internal sealed class HostSelfRoute
{
    internal const long AnswerDeadlineMs = 3000;

    private sealed class Waiting
    {
        public required TransportOp Op;
        public required ulong Lease, Revision;
        public required Action<TransportResult> Done;
        public required long SentAt;
        public uint DurationMs;
    }

    private readonly Func<long> _clock;
    private readonly System.Collections.Concurrent.ConcurrentQueue<RemoteRequest> _toClient = new();
    private readonly System.Collections.Concurrent.ConcurrentQueue<(ulong Serial, NativeOutcome Outcome, ulong Lease, byte EndReason, ushort Reason)> _toServer = new();
    // server half
    private readonly Dictionary<ulong, Waiting> _pending = [];
    private readonly Dictionary<ulong, (ulong Revision, uint DurationMs)> _active = [];
    private readonly List<(Action<TransportResult> Done, TransportResult Result)> _results = [];
    private readonly List<RemoteEnded> _ends = [];  // ends found while the step's results wait: told after them
    private ulong _serial;
    // client half
    private readonly HashSet<ulong> _started = [];  // serials the client half handed to the endpoint

    public HostSelfRoute(Func<long> clock) => _clock = clock ?? throw new ArgumentNullException(nameof(clock));

    /// <summary>A lease the host's client ended by itself (transport_ended).</summary>
    public Action<RemoteEnded>? OnEnded { get; init; }

    public bool Carries(ulong lease) => _active.ContainsKey(lease) || _pending.Values.Any(p => p.Lease == lease);

    public int Pending => _pending.Count;

    // ---- the server half (server world's thread) ----

    public void Submit(TransportOp op, ulong ownerToken, ulong lease, ulong revision, CameraWireRequest? request, Action<TransportResult> done)
    {
        var serial = ++_serial;
        _pending[serial] = new Waiting { Op = op, Lease = lease, Revision = revision, Done = done, SentAt = _clock(), DurationMs = request?.DurationMs ?? 0 };
        var kind = op switch { TransportOp.Apply => CameraMessage.Apply, TransportOp.End => CameraMessage.End, _ => CameraMessage.Update };
        var timing = op == TransportOp.UpdateRestart ? CameraWireAbi.TimingRestart : CameraWireAbi.TimingPreserve;
        _toClient.Enqueue(new RemoteRequest(kind, serial, lease, revision, ownerToken, timing, request));
    }

    /// <summary>The answers of the client half, the answer deadline; then the outcomes are handed over in order.</summary>
    public void ServerPump()
    {
        var now = _clock();
        while (_toServer.TryDequeue(out var a))
        {
            if (a.Serial == 0)
            {
                // the host's client ended a lease by itself
                if (_active.Remove(a.Lease, out var held)) OnEnded?.Invoke(new RemoteEnded(a.Lease, held.Revision, a.EndReason, a.Reason, 0));
                continue;
            }
            if (!_pending.Remove(a.Serial, out var p)) continue;
            var o = a.Outcome;
            if (o.Ok && p.Op == TransportOp.End) _active.Remove(p.Lease);
            else if (o.Ok) _active[p.Lease] = (p.Revision, p.DurationMs);
            _results.Add((p.Done, new TransportResult(o.Ok, o.Ok ? CameraReasons.None : o.Reason, o.Ok && p.Op != TransportOp.End ? CameraWireAbi.CommitClientFrame : CameraWireAbi.CommitNone,
                o.Flags, o.Frame, 0, 0, o.Diagnostic, p.Lease, p.Revision)));
        }
        foreach (var (serial, p) in _pending.ToList())
        {
            if (now - p.SentAt < AnswerDeadlineMs) continue;
            // the client world did not answer (it does not step): honest uncertainty, and the lease is let go there
            _pending.Remove(serial);
            var held = _active.Remove(p.Lease, out var was);
            _results.Add((p.Done, TransportResult.Refused(p.Op == TransportOp.End ? CameraReasons.None : CameraReasons.TransportTimeout, p.Lease, p.Revision,
                p.Op == TransportOp.End ? 0 : CameraWireAbi.FlagMayHaveApplied, "the host's client world did not answer within 3 s")));
            if (p.Op != TransportOp.End)
            {
                _toClient.Enqueue(new RemoteRequest(CameraMessage.End, 0, p.Lease, p.Revision, 0, 0, null));
                // an update of the applied lease: it ends here as well (TransportTimeout), as on a guest's route (review S8 M1)
                if (held) _ends.Add(new RemoteEnded(p.Lease, was.Revision, CameraEndReasons.TransportTimeout, CameraReasons.TransportTimeout, 0));
            }
        }
        Deliver();
    }

    /// <summary>Hosting stopped, the host's player changed or the server world stopped: everything waiting is refused, the
    /// host's leases end, and the client half lets go of what it showed. The route is used again afterwards.</summary>
    public void Close(byte endReason)
    {
        foreach (var p in _pending.Values)
            _results.Add((p.Done, TransportResult.Refused(CameraReasons.PeerUnavailable, p.Lease, p.Revision, CameraWireAbi.FlagMayHaveApplied, "the host's own route closed")));
        _pending.Clear();
        foreach (var (lease, held) in _active) OnEnded?.Invoke(new RemoteEnded(lease, held.Revision, endReason, CameraReasons.PeerUnavailable, 0));
        _active.Clear();
        while (_toServer.TryDequeue(out _)) { }
        _toClient.Enqueue(new RemoteRequest(CameraMessage.End, 0, 0, 0, 0, 0, null));  // the client half: release everything
        Deliver();
    }

    private void Deliver()
    {
        if (_results.Count > 0)
        {
            var batch = _results.ToArray();
            _results.Clear();
            foreach (var (done, result) in batch) done(result);
        }
        if (_ends.Count > 0)
        {
            var ends = _ends.ToArray();
            _ends.Clear();
            foreach (var e in ends) OnEnded?.Invoke(e);
        }
    }

    // ---- the client half (client world's thread) ----

    /// <summary>Hands what the server half sent to the endpoint, then sends the endpoint's answers back.</summary>
    public void ClientPump(LocalEndpoint endpoint)
    {
        while (_toClient.TryDequeue(out var r))
        {
            if (r.Serial == 0)
            {
                if (r.Lease != 0)
                {
                    endpoint.Release(r.Lease, CameraEndReasons.TransportTimeout);  // the server gave this one up
                    continue;
                }
                _started.Clear();  // the server half closed: nothing it sent before stays
                endpoint.ReleaseAll();
                endpoint.Take();
                continue;
            }
            var refused = endpoint.Start(r);
            if (refused != CameraReasons.None) _toServer.Enqueue((r.Serial, new NativeOutcome(false, refused, CameraWireAbi.CommitNone, 0, 0), 0, 0, 0));
            else _started.Add(r.Serial);
        }
        Answer(endpoint);
    }

    /// <summary>The endpoint's answers and own ends (after its native results were drained into it).</summary>
    public void Answer(LocalEndpoint endpoint)
    {
        var (completed, ended) = endpoint.Take();
        foreach (var (serial, outcome) in completed)
            if (_started.Remove(serial)) _toServer.Enqueue((serial, outcome, 0, 0, 0));
        foreach (var (lease, endReason, reason, _) in ended) _toServer.Enqueue((0, default, lease, endReason, reason));
    }

    /// <summary>The client half stops (hosting ended, the client world stopped): the endpoint lets go of everything.</summary>
    public void ClientClose(LocalEndpoint endpoint)
    {
        while (_toClient.TryDequeue(out _)) { }
        _started.Clear();
        endpoint.ReleaseAll();
        endpoint.Take();
    }
}
