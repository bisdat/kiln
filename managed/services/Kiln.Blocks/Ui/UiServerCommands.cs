// SPDX-License-Identifier: MIT
// The server's side of joined-server UI commands (dossier 09 §4.5; DESIGN.md §5). Tick thread only (the network handlers of a
// server run there, and so does the end-of-dispatch drain NetEngine raises), except the completion of an async handler, which
// only queues itself for the next drain.
//  - Identity: a connection's player is the one the game gave the connection (NetPeer.Player), never a message field. Each
//    association (connection, player) gets a fresh nonce in a Hello; a request under another nonce never runs.
//  - Replay: per nonce a high-water id. A new id above it advances it before any admission check; an id at or below it is a
//    duplicate (same SHA-256 of the line: the record's current state again), a conflict (another line: ProtocolConflict,
//    never run), or forgotten (ReplayExpired: never run). The last 64 final replies are kept for 120 s.
//  - Admission: 4 requests/s per connection (burst 8), status 2/s (burst 4); at most 64 active per connection and 512 in
//    all; FIFO per connection, round-robin between them, at most 16 starts per drain; 2 running per connection and 16 in
//    the process; a request that waited 5 s expires unrun. The association is checked again right before a start.
//  - Run: Started is set before Commands.Execute(CommandSource.ForPlayer(player), line). A synchronous command is final at
//    once. An async one keeps its slot until its task ends; at 45 s it reports RunningOverBudget (RunningTimeout) and still
//    keeps the slot, also when its connection goes or its player changes: a stuck handler costs one of 16 slots, never an
//    unbounded number of orphaned tasks. No thread is made per command.
//  - Never the console, never a remote-admin source, never a source a client names.
using System.Collections.Concurrent;

namespace Kiln;

/// <summary>What running a line gave (Commands.Execute's result in the receiver's terms). An async command's
/// <see cref="Completion"/> ends with its final run.</summary>
internal sealed record UiCommandRun(bool Ok, UiCommandCode Code, string? Reason, IReadOnlyList<string> Output, Task<UiCommandRun>? Completion = null);

internal sealed class UiServerCommands
{
    // ---- limits (fields so that tests can shrink them; the game uses these) ----
    internal int RequestsPerSecond = 4, RequestBurst = 8, StatusPerSecond = 2, StatusBurst = 4;
    internal int MaxActivePerPeer = 64, MaxActive = 512, MaxStartsPerDrain = 16, MaxRunningPerPeer = 2, MaxRunning = 16;
    internal int CachedReplies = 64;
    internal long CacheMs = 120_000, QueueMs = 5_000, BudgetMs = 45_000;

    // ---- seams ----
    /// <summary>Milliseconds.</summary>
    internal Func<long> Clock = static () => Environment.TickCount64;
    /// <summary>Runs a line as a player (tick thread).</summary>
    internal Func<EntityId, string, UiCommandRun> Execute = static (_, _) => throw new InvalidOperationException("no command registry");
    /// <summary>Sends a Hello or a Reply to a connection; false when it did not go out.</summary>
    internal Func<object, object, bool> Send = static (_, _) => false;
    /// <summary>The player a connection has now (EntityId.None: none, or the connection is gone).</summary>
    internal Func<object, EntityId> CurrentPlayer = static _ => EntityId.None;
    internal Action<string> Log = static _ => { };

    private sealed class Rec
    {
        public required ulong Id;
        public required byte[] Digest;
        public required string Line;
        public required Peer Owner;
        public required long ReceivedMs;
        public long StartedMs, FinalMs;
        public UiCommandState State = UiCommandState.Queued;
        public UiCommandCode Code = UiCommandCode.None;
        public bool Ok;
        public string? Reason;
        public string[] Output = [];
        public bool Truncated;
        public bool HoldsSlot;
    }

    private sealed class Bucket(int perSecond, int burst, long now)
    {
        private double _tokens = burst;
        private long _at = now;

        public bool Take(long now)
        {
            if (now > _at)
            {
                _tokens = Math.Min(burst, _tokens + (now - _at) * perSecond / 1000.0);
                _at = now;
            }
            if (_tokens < 1) return false;
            _tokens -= 1;
            return true;
        }
    }

    private sealed class Peer(object connection, EntityId player, string nonce, Bucket requests, Bucket status)
    {
        public readonly object Connection = connection;
        public readonly EntityId Player = player;
        public readonly string Nonce = nonce;
        public readonly Bucket Requests = requests, Status = status;
        public readonly Dictionary<ulong, Rec> Records = new();
        public readonly LinkedList<Rec> Queue = new();
        public readonly LinkedList<Rec> Finals = new();  // the cached final replies, oldest first
        public ulong HighWater;
        public int Running;
        public bool HelloSent, Retired;
        public int Active => Queue.Count + Running;
    }

    private readonly Dictionary<object, Peer> _peers = new(ReferenceEqualityComparer.Instance);
    private readonly List<Peer> _order = new();  // round-robin order
    private int _next;
    private int _active;   // queued + running, orphans included
    private int _running;  // slots held, orphans included
    private readonly List<Rec> _holding = new();  // records holding a slot, orphans included
    private readonly ConcurrentQueue<(Rec Rec, UiCommandRun Run)> _completions = new();
    private int _logged;

    /// <summary>Tests and the status page: slots held now (of <see cref="MaxRunning"/>).</summary>
    internal int RunningSlots => _running;

    /// <summary>Tests: a connection's nonce now, or null.</summary>
    internal string? NonceOf(object connection) => _peers.TryGetValue(connection, out var p) ? p.Nonce : null;

    /// <summary>Tests: where a request stands on the server, or null when it has no record of it.</summary>
    internal UiCommandState? StateOf(object connection, ulong id) =>
        _peers.TryGetValue(connection, out var p) && p.Records.TryGetValue(id, out var r) ? r.State : null;

    private void Note(string text)
    {
        if (_logged < 50 && ++_logged > 0) Log(text);
    }

    // ---- associations -----------------------------------------------------------------------------------------------

    /// <summary>The connections that have the channel now, with the player each has (EntityId.None: none yet). A
    /// connection that is new, or whose player changed, gets a fresh nonce in a Hello; one that is gone or has no player
    /// loses its nonce (its queued requests end unrun, its running ones keep their slots).</summary>
    public void Sync(IReadOnlyList<(object Connection, EntityId Player)> live)
    {
        HashSet<object>? seen = null;
        foreach (var (conn, player) in live)
        {
            if (player.IsNone) continue;
            (seen ??= new HashSet<object>(ReferenceEqualityComparer.Instance)).Add(conn);
            Associate(conn, player);
        }
        if (_peers.Count == (seen?.Count ?? 0)) return;
        // a connection still there hears that its queued requests ended unrun; a send to one that is gone does nothing
        foreach (var conn in _peers.Keys.Where(c => seen is null || !seen.Contains(c)).ToList()) Retire(conn, stillConnected: true);
    }

    /// <summary>A connection with a player: its nonce, made (and said in a Hello) when the association is new.</summary>
    public string Associate(object connection, EntityId player)
    {
        if (player.IsNone) throw new ArgumentException("no player", nameof(player));
        if (_peers.TryGetValue(connection, out var p))
        {
            if (p.Player == player)
            {
                if (!p.HelloSent) p.HelloSent = Send(connection, new UiCommandWire.Hello(p.Nonce));
                return p.Nonce;
            }
            Retire(connection, stillConnected: true);
        }
        var now = Clock();
        p = new Peer(connection, player, UiCommandWire.NewNonce(), new Bucket(RequestsPerSecond, RequestBurst, now), new Bucket(StatusPerSecond, StatusBurst, now));
        _peers[connection] = p;
        _order.Add(p);
        p.HelloSent = Send(connection, new UiCommandWire.Hello(p.Nonce));
        return p.Nonce;
    }

    /// <summary>The connection is gone, or its player is (another character, a world change).</summary>
    public void Gone(object connection) => Retire(connection, stillConnected: false);

    private void Retire(object connection, bool stillConnected)
    {
        if (!_peers.Remove(connection, out var p)) return;
        p.Retired = true;
        var i = _order.IndexOf(p);
        _order.RemoveAt(i);
        if (_next > i) _next--;
        foreach (var r in p.Queue.ToList())
        {
            // never run: a connection that is still there hears so under its old nonce
            p.Queue.Remove(r);
            _active--;
            Finish(r, UiCommandCode.AssociationChanged, "the connection's player changed before it started: nothing was run", send: stillConnected);
        }
        // running ones are orphans now: they keep their slots (and count as active) until their handlers end
        p.Records.Clear();
        p.Finals.Clear();
    }

    // ---- messages (tick thread; nothing runs here) ------------------------------------------------------------------

    public void OnRequest(object connection, UiCommandWire.Request m)
    {
        if (!_peers.TryGetValue(connection, out var p))
        {
            Note("blocks: ui-command: a request from a connection without a player (or before its hello): dropped");
            return;
        }
        if (!UiCommandWire.TryParseId(m.Id, out var id))
        {
            Note($"blocks: ui-command: a request with a bad id from {p.Player}: dropped");
            return;
        }
        if (m.Nonce != p.Nonce)
        {
            if (p.Requests.Take(Clock())) Reply(p, m.Nonce, id, UiCommandState.Final, false, UiCommandCode.AssociationChanged, "not this connection's current hello: nothing was run", [], false);
            return;
        }
        var line = m.Line ?? "";
        if (id <= p.HighWater)
        {
            if (!p.Records.TryGetValue(id, out var known))
            {
                // forgotten, or an id skipped: it never runs now, whatever the line
                Reply(p, p.Nonce, id, UiCommandState.Final, false, UiCommandCode.ReplayExpired, "the server no longer remembers this request: whether it ran is unknown", [], false);
                return;
            }
            if (!known.Digest.AsSpan().SequenceEqual(UiCommandWire.LineProblem(line) is null ? UiCommandWire.Digest(line) : []))
            {
                Reply(p, p.Nonce, id, UiCommandState.Final, false, UiCommandCode.ProtocolConflict, "this request id came before with another line: this one was not run", [], false);
                return;
            }
            Answer(p, known);
            return;
        }
        // a new id: the watermark moves first, so a refused id can never run later
        p.HighWater = id;
        var now = Clock();
        var problem = UiCommandWire.LineProblem(line);
        var rec = new Rec { Id = id, Digest = problem is null ? UiCommandWire.Digest(line) : [], Line = line, Owner = p, ReceivedMs = now };
        p.Records[id] = rec;
        if (problem is not null) { Finish(rec, UiCommandCode.ParseFailure, problem, send: true); return; }
        if (!p.Requests.Take(now)) { Finish(rec, UiCommandCode.RateLimited, "too many commands: try again", send: true); return; }
        if (p.Active >= MaxActivePerPeer || _active >= MaxActive) { Finish(rec, UiCommandCode.QueueFull, "too many commands are waiting: try again", send: true); return; }
        p.Queue.AddLast(rec);
        _active++;
        Answer(p, rec);  // queued
    }

    public void OnCancel(object connection, UiCommandWire.Cancel m)
    {
        if (!_peers.TryGetValue(connection, out var p) || !UiCommandWire.TryParseId(m.Id, out var id)) return;
        if (m.Nonce != p.Nonce || !p.Status.Take(Clock())) return;
        if (!p.Records.TryGetValue(id, out var r))
        {
            Reply(p, p.Nonce, id, UiCommandState.Final, false, UiCommandCode.UnknownRequest, "the server has no such request to cancel", [], false);
            return;
        }
        switch (r.State)
        {
            case UiCommandState.Queued:
                p.Queue.Remove(r);
                _active--;
                Finish(r, UiCommandCode.CanceledBeforeStart, "canceled before it started: nothing was run", send: true);
                break;
            case UiCommandState.Started or UiCommandState.RunningOverBudget:
                Reply(p, p.Nonce, id, r.State, false, UiCommandCode.TooLateToCancel, "it had started: it runs on", r.Output, r.Truncated);
                break;
            default:
                Answer(p, r);
                break;
        }
    }

    public void OnStatus(object connection, UiCommandWire.StatusRequest m)
    {
        if (!_peers.TryGetValue(connection, out var p) || !UiCommandWire.TryParseId(m.Id, out var id)) return;
        if (m.Nonce != p.Nonce || !p.Status.Take(Clock())) return;
        if (p.Records.TryGetValue(id, out var r)) Answer(p, r);
        else if (id <= p.HighWater) Reply(p, p.Nonce, id, UiCommandState.Final, false, UiCommandCode.ReplayExpired, "the server no longer remembers this request: whether it ran is unknown", [], false);
        else Reply(p, p.Nonce, id, UiCommandState.Final, false, UiCommandCode.UnknownRequest, "the server has no such request", [], false);
    }

    // ---- the drain (tick thread, after a step's messages were all handled) -------------------------------------------

    public void Drain()
    {
        var now = Clock();
        while (_completions.TryDequeue(out var c)) Completed(c.Rec, c.Run);
        foreach (var r in _holding)
            if (r.State == UiCommandState.Started && now - r.StartedMs >= BudgetMs)
            {
                r.State = UiCommandState.RunningOverBudget;
                r.Code = UiCommandCode.RunningTimeout;
                r.Reason = "it has run for more than 45 s and still runs";
                if (!r.Owner.Retired) Answer(r.Owner, r);
            }
        foreach (var p in _order)
        {
            if (p.Finals.Count > 0) Evict(p, now);
            while (p.Queue.First is { } head && now - head.Value.ReceivedMs >= QueueMs)
            {
                p.Queue.RemoveFirst();
                _active--;
                Finish(head.Value, UiCommandCode.ExpiredBeforeStart, "the command was not run: it waited too long", send: true);
            }
        }
        var starts = 0;
        var idle = 0;
        while (starts < MaxStartsPerDrain && _running < MaxRunning && _order.Count > 0 && idle < _order.Count)
        {
            if (_next >= _order.Count) _next = 0;
            var p = _order[_next++];
            if (p.Queue.First is not { } head || p.Running >= MaxRunningPerPeer)
            {
                idle++;
                continue;
            }
            idle = 0;
            p.Queue.RemoveFirst();
            Start(head.Value, now);
            starts++;
        }
    }

    private void Start(Rec r, long now)
    {
        var p = r.Owner;
        // the exact association, again: a reused slot or a new character never inherits a queued command
        if (CurrentPlayer(p.Connection) != p.Player)
        {
            _active--;
            Finish(r, UiCommandCode.AssociationChanged, "the connection's player changed before it started: nothing was run", send: true);
            return;
        }
        r.State = UiCommandState.Started;  // before the handler: even an async one can act before it returns
        r.StartedMs = now;
        r.HoldsSlot = true;
        p.Running++;
        _running++;
        _holding.Add(r);
        UiCommandRun run;
        try
        {
            run = Execute(p.Player, r.Line);
        }
        catch (Exception e) when (e is not OutOfMemoryException)
        {
            run = new UiCommandRun(false, UiCommandCode.HandlerFailure, $"the command failed: {e.Message}", []);
        }
        if (run.Completion is { } task)
        {
            r.Output = UiCommandWire.ClipOutput(run.Output, out r.Truncated);
            Answer(p, r);  // started
            task.ContinueWith(static (t, state) =>
            {
                var (self, rec) = ((UiServerCommands, Rec))state!;
                var final = t.IsCompletedSuccessfully ? t.Result
                    : new UiCommandRun(false, UiCommandCode.HandlerFailure, t.IsCanceled ? "the command was canceled" : $"the command failed: {t.Exception?.InnerException?.Message}", []);
                self._completions.Enqueue((rec, final));
            }, (this, r), CancellationToken.None, TaskContinuationOptions.ExecuteSynchronously, TaskScheduler.Default);
            return;
        }
        Completed(r, run);
    }

    private void Completed(Rec r, UiCommandRun run)
    {
        if (r.HoldsSlot)
        {
            r.HoldsSlot = false;
            _running--;
            _holding.Remove(r);
            if (!r.Owner.Retired) r.Owner.Running--;
        }
        _active--;
        r.Output = UiCommandWire.ClipOutput(run.Output, out r.Truncated);
        Finish(r, run.Ok ? UiCommandCode.None : run.Code == UiCommandCode.None ? UiCommandCode.HandlerFailure : run.Code, run.Ok ? null : run.Reason, send: !r.Owner.Retired, ok: run.Ok);
    }

    /// <summary>Makes a record final (cached for replays while its connection keeps its nonce) and, when asked, says so.</summary>
    private void Finish(Rec r, UiCommandCode code, string? reason, bool send, bool ok = false)
    {
        r.State = UiCommandState.Final;
        r.Code = code;
        r.Ok = ok && code == UiCommandCode.None;
        r.Reason = r.Ok ? null : UiCommandWire.ClipReason(reason ?? code.ToString());
        r.FinalMs = Clock();
        var p = r.Owner;
        if (!p.Retired)
        {
            p.Finals.AddLast(r);
            Evict(p, r.FinalMs);
        }
        if (send) Answer(p, r);
    }

    private void Evict(Peer p, long now)
    {
        while (p.Finals.First is { } oldest && (p.Finals.Count > CachedReplies || now - oldest.Value.FinalMs >= CacheMs))
        {
            p.Finals.RemoveFirst();
            p.Records.Remove(oldest.Value.Id);
        }
    }

    private void Answer(Peer p, Rec r) =>
        Reply(p, p.Nonce, r.Id, r.State, r.Ok, r.Code, r.State == UiCommandState.Final || r.State == UiCommandState.RunningOverBudget ? r.Reason : null, r.Output, r.Truncated);

    private void Reply(Peer p, string? nonce, ulong id, UiCommandState state, bool ok, UiCommandCode code, string? reason, string[] output, bool truncated) =>
        Send(p.Connection, new UiCommandWire.Reply(nonce ?? "", UiCommandWire.IdText(id), UiCommandWire.StateName(state), ok, UiCommandWire.CodeName(code),
            UiCommandWire.ClipReason(reason), output, truncated));
}
