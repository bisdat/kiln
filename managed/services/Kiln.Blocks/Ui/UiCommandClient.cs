// SPDX-License-Identifier: MIT
// The client's side of joined-server UI commands (dossier 09 §4.5; DESIGN.md §5). A request goes out once and waits 15 s for
// a final answer; nothing is ever sent again on its own (no retry with a new id, none with a new nonce).
//  - Cancel before the send: nothing is sent (CanceledBeforeSend). After: one Cancel goes out; the server's answer decides:
//    removed while queued (CanceledBeforeStart, not run) or too late (CanceledAfterStart, outcome unknown). A cancel after a
//    Started reply ends the wait at once as CanceledAfterStart: a started handler has no cancellation contract.
//  - No final answer within 15 s: TimedOutUnknown (outcome unknown, not "failed without effects").
//  - Exactly one UiCommandCompletedEvent per wait. The receipt stays (the last 64): a later answer updates it and posts
//    UiCommandStatusChangedEvent; BlockUi.CommandStatus reads it or asks the server (StatusRequest never runs anything).
//  - Ids go out in the order they are taken (the send is under the lock): the server never sees a higher id first.
// Events are posted from Pump and from reply handling, both on the client's main thread.
using System.Collections.Concurrent;

namespace Kiln;

internal sealed class UiCommandClient
{
    /// <summary>The game's client (UiCommandChannel sets its seams); tests make their own.</summary>
    internal static UiCommandClient Shared = new();

    internal long WaitMs = 15_000;
    internal int KeptReceipts = 64;

    // ---- seams ----
    internal Func<long> Clock = static () => Environment.TickCount64;
    /// <summary>Sends a Request, Cancel or StatusRequest to the server; false when it did not go out.</summary>
    internal Func<object, bool> Send = static _ => false;
    internal Action<KilnEvent> Post = static e => Events.Post(e);
    /// <summary>The capability row follows the hello.</summary>
    internal Action<bool> AvailabilityChanged = static _ => { };

    private sealed class Receipt(string nonce, ulong id)
    {
        public readonly string Nonce = nonce;
        public readonly ulong Id = id;
        public UiCommandState State = UiCommandState.Queued;
        public UiCommandCode Code = UiCommandCode.None;
        public bool Ok, Truncated, Final;
        public string? Reason;
        public string[] Output = [];
        // the wait
        public TaskCompletionSource<UiCommandResult>? Wait;
        public long Deadline;
        public bool CancelSent;
        public CancellationTokenRegistration Registration;
    }

    private sealed record StatusWait(string Nonce, ulong Id, TaskCompletionSource<UiCommandResult> Done, long Deadline);

    private readonly object _gate = new();
    private string? _nonce;
    private ulong _nextId = 1;
    private readonly Dictionary<(string, ulong), Receipt> _receipts = new();
    private readonly LinkedList<Receipt> _ended = new();  // receipts whose wait ended, oldest first
    private readonly List<StatusWait> _statusWaits = new();
    private readonly ConcurrentQueue<KilnEvent> _outbox = new();

    /// <summary>The server said hello on this connection: commands can be sent.</summary>
    public bool Available => Volatile.Read(ref _nonce) is not null;

    /// <summary>The current hello's nonce, or null.</summary>
    public string? Nonce => Volatile.Read(ref _nonce);

    // ---- from the server (main thread) -----------------------------------------------------------------------------

    /// <summary>A Hello: a new challenge for this connection and player. Ids start at 1 again; earlier receipts keep their nonce.</summary>
    public void OnHello(string? nonce)
    {
        if (!UiCommandWire.NonceOk(nonce)) return;
        bool was;
        lock (_gate)
        {
            was = _nonce is not null;
            // ids start at 1 for a new nonce only: the same hello again must not reuse ids the server has records of
            if (_nonce != nonce) _nextId = 1;
            _nonce = nonce;
        }
        if (!was) AvailabilityChanged(true);
    }

    /// <summary>The connection to the server is gone: no more sends; waits end now as TimedOutUnknown (no answer can come).</summary>
    public void OnLinkLost()
    {
        bool was;
        lock (_gate)
        {
            was = _nonce is not null;
            _nonce = null;
            foreach (var r in _receipts.Values.Where(r => r.Wait is not null).ToList())
                EndWait(r, Unknown(r, UiCommandCode.TimedOutUnknown, "the connection to the server closed: the command may or may not have run"));
            foreach (var s in _statusWaits) s.Done.TrySetResult(Snapshot(s.Nonce, s.Id) ?? Unknown(s.Nonce, s.Id, UiCommandState.Queued, UiCommandCode.TimedOutUnknown, "the connection to the server closed"));
            _statusWaits.Clear();
        }
        if (was) AvailabilityChanged(false);
        Flush();
    }

    public void OnReply(UiCommandWire.Reply m)
    {
        if (!UiCommandWire.TryParseId(m.Id, out var id) || !UiCommandWire.TryState(m.State, out var state) || !UiCommandWire.TryCode(m.Code, out var code)) return;
        var nonce = m.Nonce ?? "";
        lock (_gate)
        {
            if (_receipts.TryGetValue((nonce, id), out var r))
            {
                if (!r.Final)
                {
                    var changed = false;
                    if (state == UiCommandState.Final)
                    {
                        r.Final = true;
                        r.State = UiCommandState.Final;
                        r.Code = code;
                        r.Ok = m.Ok && code == UiCommandCode.None;
                        r.Reason = r.Ok ? null : m.Reason ?? code.ToString();
                        r.Output = m.Output ?? [];
                        r.Truncated = m.Truncated;
                        changed = true;
                    }
                    else if (state > r.State)
                    {
                        r.State = state;
                        if (code != UiCommandCode.TooLateToCancel) r.Code = code;
                        if (m.Reason is not null) r.Reason = m.Reason;
                        if (m.Output is { Length: > 0 }) { r.Output = m.Output; r.Truncated = m.Truncated; }
                        changed = true;
                    }
                    if (changed) _outbox.Enqueue(new UiCommandStatusChangedEvent(r.Nonce, r.Id, r.State, r.Final ? Result(r) : null));
                    if (r.Wait is not null)
                    {
                        if (r.Final) EndWait(r, Result(r));
                        else if (code == UiCommandCode.TooLateToCancel && r.CancelSent)
                            EndWait(r, Unknown(r, UiCommandCode.CanceledAfterStart, "canceled after it started on the server: it may have run, or may still run"));
                    }
                }
                Answer(nonce, id, Result(r));
            }
            else Answer(nonce, id, new UiCommandResult(m.Ok && state == UiCommandState.Final && code == UiCommandCode.None, m.Reason, m.Output ?? [], id, nonce, state,
                state == UiCommandState.Final ? UiCommandWire.OutcomeOf(code) : UiCommandOutcome.Unknown, code, m.Truncated));
        }
        Flush();
    }

    // ---- requests (any thread) -------------------------------------------------------------------------------------

    public Task<UiCommandResult> Execute(string line, CancellationToken cancellation)
    {
        ArgumentNullException.ThrowIfNull(line);
        if (UiCommandWire.LineProblem(line) is { } problem) throw new ArgumentException(problem, nameof(line));
        Receipt r;
        lock (_gate)
        {
            if (_nonce is not { } nonce)
                throw new KilnUnavailableException(BlockUi.ServerCommandCapability, "the server this game joined has not said hello on kiln:ui-command (no Kiln there, an older Kiln, or not joined): nothing was sent");
            if (cancellation.IsCancellationRequested)
            {
                var none = new UiCommandResult(false, "canceled before it was sent: nothing was sent", [], 0, nonce, UiCommandState.Final, UiCommandOutcome.NotSent, UiCommandCode.CanceledBeforeSend, false);
                _outbox.Enqueue(new UiCommandCompletedEvent(none));
                return Task.FromResult(none);
            }
            var id = _nextId++;
            r = new Receipt(nonce, id)
            {
                Wait = new TaskCompletionSource<UiCommandResult>(TaskCreationOptions.RunContinuationsAsynchronously),
                Deadline = Clock() + WaitMs,
            };
            // under the lock: ids reach the server in the order they were taken
            if (!Send(new UiCommandWire.Request(nonce, UiCommandWire.IdText(id), line)))
                throw new KilnUnavailableException(BlockUi.ServerCommandCapability, "the request could not be sent (the connection to the server is not ready): nothing was sent");
            _receipts[(nonce, id)] = r;
        }
        if (cancellation.CanBeCanceled) r.Registration = cancellation.Register(static s => { var (self, rec) = ((UiCommandClient, Receipt))s!; self.Cancel(rec); }, (this, r));
        return r.Wait!.Task;
    }

    private void Cancel(Receipt r)
    {
        lock (_gate)
        {
            if (r.Wait is null || r.CancelSent) return;
            r.CancelSent = true;
            if (r.State is UiCommandState.Started or UiCommandState.RunningOverBudget)
            {
                // too late for certain: its handler runs on, and has no cancellation contract
                EndWait(r, Unknown(r, UiCommandCode.CanceledAfterStart, "canceled after it started on the server: it may have run, or may still run"));
                return;
            }
            Send(new UiCommandWire.Cancel(r.Nonce, UiCommandWire.IdText(r.Id)));
        }
    }

    public Task<UiCommandResult> Status(string nonce, ulong requestId)
    {
        ArgumentException.ThrowIfNullOrEmpty(nonce);
        if (requestId == 0) throw new ArgumentException("request id 0 is no request (nothing was sent)", nameof(requestId));
        lock (_gate)
        {
            _receipts.TryGetValue((nonce, requestId), out var r);
            if (r is { Final: true }) return Task.FromResult(Result(r));
            if (nonce != _nonce)
            {
                if (r is not null) return Task.FromResult(Result(r));
                if (_nonce is null) throw new KilnUnavailableException(BlockUi.ServerCommandCapability, "not joined to a server that said hello on kiln:ui-command, and this game has no receipt of the request");
                return Task.FromResult(Unknown(nonce, requestId, UiCommandState.Final, UiCommandCode.UnknownRequest, "not a request of this connection, and this game has no receipt of it"));
            }
            var wait = new StatusWait(nonce, requestId, new TaskCompletionSource<UiCommandResult>(TaskCreationOptions.RunContinuationsAsynchronously), Clock() + WaitMs);
            if (!Send(new UiCommandWire.StatusRequest(nonce, UiCommandWire.IdText(requestId))))
                return Task.FromResult(r is not null ? Result(r) : Unknown(nonce, requestId, UiCommandState.Queued, UiCommandCode.TimedOutUnknown, "the status request could not be sent"));
            _statusWaits.Add(wait);
            return wait.Done.Task;
        }
    }

    // ---- the pump (main thread) ------------------------------------------------------------------------------------

    /// <summary>Ends the waits that are due and posts the events that wait.</summary>
    public void Pump()
    {
        var now = Clock();
        lock (_gate)
        {
            foreach (var r in _receipts.Values)
                if (r.Wait is not null && now >= r.Deadline) _due.Add(r);
            foreach (var r in _due) EndWait(r, Unknown(r, UiCommandCode.TimedOutUnknown, "no answer from the server in 15 s: the command may or may not have run"));
            _due.Clear();
            for (var i = _statusWaits.Count - 1; i >= 0; i--)
            {
                var s = _statusWaits[i];
                if (now < s.Deadline) continue;
                _statusWaits.RemoveAt(i);
                s.Done.TrySetResult(Snapshot(s.Nonce, s.Id) ?? Unknown(s.Nonce, s.Id, UiCommandState.Queued, UiCommandCode.TimedOutUnknown, "no answer from the server in 15 s"));
            }
        }
        Flush();
    }

    private readonly List<Receipt> _due = new();

    private void Flush()
    {
        while (_outbox.TryDequeue(out var e))
            try { Post(e); }
            catch (Exception ex) when (ex is not OutOfMemoryException) { BlocksBridge.Log(LogLevel.Warn, $"blocks: ui-command: an event handler failed: {ex.Message}"); }
    }

    // ---- helpers (under the lock) ----------------------------------------------------------------------------------

    private void EndWait(Receipt r, UiCommandResult result)
    {
        var wait = r.Wait!;
        r.Wait = null;
        r.Registration.Unregister();  // never Dispose under the lock: it waits for a running callback, which takes the lock
        _outbox.Enqueue(new UiCommandCompletedEvent(result));
        wait.TrySetResult(result);
        _ended.AddLast(r);
        while (_ended.Count > KeptReceipts)
        {
            var old = _ended.First!.Value;
            _ended.RemoveFirst();
            _receipts.Remove((old.Nonce, old.Id));
        }
    }

    private void Answer(string nonce, ulong id, UiCommandResult result)
    {
        for (var i = _statusWaits.Count - 1; i >= 0; i--)
            if (_statusWaits[i].Nonce == nonce && _statusWaits[i].Id == id)
            {
                _statusWaits[i].Done.TrySetResult(result);
                _statusWaits.RemoveAt(i);
            }
    }

    private UiCommandResult? Snapshot(string nonce, ulong id) => _receipts.TryGetValue((nonce, id), out var r) ? Result(r) : null;

    private static UiCommandResult Result(Receipt r) => r.Final
        ? new UiCommandResult(r.Ok, r.Ok ? null : r.Reason, r.Output, r.Id, r.Nonce, UiCommandState.Final, UiCommandWire.OutcomeOf(r.Code), r.Code, r.Truncated)
        : new UiCommandResult(false, r.Reason ?? $"it is {UiCommandWire.StateName(r.State)} on the server", r.Output, r.Id, r.Nonce, r.State, UiCommandOutcome.Unknown, r.Code, r.Truncated);

    private static UiCommandResult Unknown(Receipt r, UiCommandCode code, string reason) =>
        new(false, reason, r.Output, r.Id, r.Nonce, r.State, UiCommandOutcome.Unknown, code, r.Truncated);

    private static UiCommandResult Unknown(string nonce, ulong id, UiCommandState state, UiCommandCode code, string reason) =>
        new(false, reason, [], id, nonce, state, UiCommandOutcome.Unknown, code, false);

    // ---- a block's button or key (any thread) ----------------------------------------------------------------------

    /// <summary>A block action's server command: sent once, its outcome a toast (and a log line). Never the local console:
    /// without the server's hello nothing is sent and the toast says so.</summary>
    internal static void RunFromAction(string line, string mod, UiLifetime? lifetime)
    {
        Task<UiCommandResult> task;
        try
        {
            task = Shared.Execute(line, CancellationToken.None);
        }
        catch (KilnUnavailableException e)
        {
            BlocksBridge.Log(LogLevel.Info, $"blocks: {mod}: ui: server command \"{line}\": not sent ({e.Reason})");
            UiRuntime.Backend.Toast(KilnText.T("blocks.ui_command.unavailable"), ToastKind.Warning);
            return;
        }
        task.ContinueWith(t =>
        {
            if (!t.IsCompletedSuccessfully) return;
            var r = t.Result;
            void Show()
            {
                BlocksBridge.Log(r.Ok ? LogLevel.Info : LogLevel.Warn, $"blocks: {mod}: ui: server command \"{line}\" #{r.RequestId}: {(r.Ok ? "ok" : $"{r.Code}: {r.Reason}")}");
                if (ToastOf(r, line) is { } toast) UiRuntime.Backend.Toast(toast.Text, toast.Kind);
            }
            if (lifetime is not null) lifetime.Run(Show);  // an unloaded window drops its toast, not the server's transaction
            else Show();
        }, CancellationToken.None, TaskContinuationOptions.ExecuteSynchronously, TaskScheduler.Default);
    }

    /// <summary>What a player sees of a server command's outcome (DESIGN.md §5); null: nothing (a success without output).</summary>
    internal static (string Text, ToastKind Kind)? ToastOf(UiCommandResult r, string line)
    {
        if (r.Ok) return r.Output.Count > 0 && r.Output[0].Length > 0 ? (r.Output[0], ToastKind.Info) : null;
        return r.Code switch
        {
            UiCommandCode.RateLimited or UiCommandCode.QueueFull => (KilnText.T("blocks.ui_command.busy"), ToastKind.Warning),
            UiCommandCode.ExpiredBeforeStart => (KilnText.T("blocks.ui_command.expired"), ToastKind.Warning),
            UiCommandCode.TimedOutUnknown or UiCommandCode.ReplayExpired or UiCommandCode.CanceledAfterStart => (KilnText.T("blocks.ui_command.no_answer"), ToastKind.Warning),
            _ => (string.IsNullOrEmpty(r.Reason) ? KilnText.T("blocks.toast.command_failed", ("command", line)) : r.Reason, ToastKind.Error),
        };
    }
}
