// SPDX-License-Identifier: MIT
// A fake of Kiln's task pool for the C# task API's tests: the managed host's lease table (native/core/src/tasks_host.h)
// over real worker threads and a real tick thread, with the native rules: segments of one lease never overlap, one armed
// registration at a time, finish only inside a running segment, tick hops only in a live server session, after-step
// resumptions tied to their session, a queued lease cancelled without a dispatch. Misuse the native table answers with
// INTERNAL is recorded in Errors (and thrown); a dispatch that leaves its thread's scope, mod, context or flowed identity
// changed is recorded in Leaks. Both must stay empty.
using System.Collections.Concurrent;
using Xunit;

namespace Kiln.Api.Tests;

internal sealed class FakeTaskBackend : TaskBackend, IDisposable
{
    internal sealed class LeaseRecord
    {
        public ulong Id, Owner;
        public string Name = "";
        public KilnTaskOptions Options;
        public object HostState = null!;
        public KilnTaskState State = KilnTaskState.Queued;
        public bool InReady, SegmentRunning, Armed, ResumePending, AfterPending, Waiting, Finished, Cancel;
        public ulong Registration, Registrations;
        public (ulong Session, ulong Edge) AfterAt, WaitAt;
        public LeaseOutcome Outcome;
        public int HopsPending;
        public bool Terminal => State is KilnTaskState.Completed or KilnTaskState.Faulted or KilnTaskState.Cancelled;
    }

    private readonly object _lock = new();
    private readonly List<Thread> _workers = [];
    private readonly Thread _tickThread;
    private readonly BlockingCollection<Action> _tickJobs = new();
    private readonly LinkedList<LeaseRecord> _ready = new();
    private readonly Dictionary<ulong, LeaseRecord> _leases = new();
    private readonly List<LeaseRecord> _hops = [];
    private readonly HashSet<ulong> _sealed = [];
    private LeaseDispatch? _dispatch;
    private ulong _nextLease = 0x1000, _session, _sessions, _edge;
    private int _running;
    private bool _disposed;
    [ThreadStatic] private static FakeTaskBackend? t_worker;

    public readonly List<string> Errors = [];
    public readonly List<string> Leaks = [];
    public readonly List<(ulong Owner, string Name, KilnTaskOptions Options)> Admitted = [];
    public int OwnerCapacity = 1024;
    public string? Unavailable;
    public int HopsQueued;
    public int Edges;

    public FakeTaskBackend(int workers = 2)
    {
        for (var i = 0; i < workers; i++)
        {
            var t = new Thread(WorkerLoop) { IsBackground = true, Name = "fake-task-" + i };
            _workers.Add(t);
            t.Start();
        }
        _tickThread = new Thread(TickLoop) { IsBackground = true, Name = "fake-tick" };
        _tickThread.Start();
    }

    public int TickThreadId => _tickThread.ManagedThreadId;
    public bool IsTickThread => Thread.CurrentThread == _tickThread;
    public bool IsWorker(int managedThreadId) => _workers.Any(w => w.ManagedThreadId == managedThreadId);

    // ---- TaskBackend ----

    public override string? UnavailableReason => Unavailable;
    public override bool OnPoolThread => t_worker == this;
    public override bool OnServerTick => IsTickThread;

    public override ulong ServerSession
    {
        get
        {
            lock (_lock) return _session;
        }
    }

    public override void Bind(LeaseDispatch dispatch) => _dispatch = dispatch;

    public override TaskRefusal Admit(ulong owner, string name, KilnTaskOptions options, object hostState, out ulong lease)
    {
        lock (_lock)
        {
            lease = 0;
            if (_sealed.Contains(owner)) return TaskRefusal.OwnerSealed;
            if (_leases.Values.Count(l => l.Owner == owner) >= OwnerCapacity) return TaskRefusal.QueueFull;
            var l = new LeaseRecord { Id = ++_nextLease, Owner = owner, Name = name, Options = options, HostState = hostState };
            _leases[l.Id] = l;
            Admitted.Add((owner, name, options));
            Ready(l);
            lease = l.Id;
            return TaskRefusal.None;
        }
    }

    public override ulong Arm(ulong lease)
    {
        lock (_lock)
        {
            var l = Find(lease);
            if (l is null || !l.SegmentRunning || l.Armed || l.ResumePending || l.AfterPending || l.Finished) throw Misuse("arm", l);
            l.Armed = true;
            l.Registration = ++l.Registrations;
            return l.Registration;
        }
    }

    public override void Resume(ulong lease, ulong registration)
    {
        lock (_lock)
        {
            var l = Find(lease);
            if (l is null || !l.Armed || l.Registration != registration) throw Misuse("resume", l);
            l.Armed = false;
            l.Registration = 0;
            if (l.SegmentRunning) l.ResumePending = true;  // segments never overlap
            else Ready(l);
        }
    }

    public override TaskRefusal ResumeAfterStep(ulong lease, ulong registration, ulong session)
    {
        lock (_lock)
        {
            var l = Find(lease);
            if (l is null || !l.Armed || l.Registration != registration || session == 0) throw Misuse("resume_after_step", l);
            if (_session == 0 || session != _session) return TaskRefusal.NoServerWorld;  // still armed
            l.Armed = false;
            l.Registration = 0;
            if (l.SegmentRunning)
            {
                l.AfterPending = true;
                l.AfterAt = (session, _edge);
            }
            else
            {
                PlaceAfter(l, (session, _edge));
            }
            return TaskRefusal.None;
        }
    }

    public override TaskRefusal TickHop(ulong lease)
    {
        lock (_lock)
        {
            var l = Find(lease);
            if (l is null || l.Terminal) throw Misuse("tick_hop", l);
            if (_session == 0) return TaskRefusal.NoServerWorld;
            l.HopsPending++;
            _hops.Add(l);
            HopsQueued++;
            return TaskRefusal.None;
        }
    }

    public override void Finish(ulong lease, LeaseOutcome outcome)
    {
        lock (_lock)
        {
            var l = Find(lease);
            if (l is null || !l.SegmentRunning || l.Finished || l.Armed || l.ResumePending || l.AfterPending) throw Misuse("finish", l);
            l.Finished = true;
            l.Outcome = outcome;
        }
    }

    public override bool IsCancelled(ulong lease)
    {
        lock (_lock) return Find(lease)?.Cancel ?? false;
    }

    public override KilnTaskState Cancel(ulong owner, ulong lease)
    {
        lock (_lock)
        {
            var l = Find(lease);
            if (l is null || l.Owner != owner) return KilnTaskState.None;
            if (l.State == KilnTaskState.Queued)
            {
                _ready.Remove(l);
                l.InReady = false;
                l.State = KilnTaskState.Cancelled;  // never dispatched
                MaybeDrop(l);
                return KilnTaskState.Cancelled;
            }
            if (l.State == KilnTaskState.Running)
            {
                l.Cancel = true;
                ReleaseContinuation(l);
            }
            return l.State;
        }
    }

    public override KilnTaskState Status(ulong owner, ulong lease)
    {
        lock (_lock)
        {
            var l = Find(lease);
            return l is null || l.Owner != owner ? KilnTaskState.None : l.State;
        }
    }

    public override int SealOwner(ulong owner)
    {
        lock (_lock)
        {
            _sealed.Add(owner);
            foreach (var l in _leases.Values.Where(l => l.Owner == owner).ToList())
            {
                if (l.State == KilnTaskState.Queued)
                {
                    _ready.Remove(l);
                    l.InReady = false;
                    l.State = KilnTaskState.Cancelled;
                    MaybeDrop(l);
                }
                else if (l.State == KilnTaskState.Running)
                {
                    l.Cancel = true;
                    ReleaseContinuation(l);
                }
            }
            return _leases.Values.Count(l => l.Owner == owner);
        }
    }

    public override IReadOnlyList<LeaseRow> Snapshot()
    {
        lock (_lock)
            return [.. _leases.Values.Where(l => l.State == KilnTaskState.Running)
                .Select(l => new LeaseRow(l.Owner, l.Id, l.State, l.Name, TimeSpan.Zero))];
    }

    // ---- the server world ----

    public void StartSession()
    {
        lock (_lock)
        {
            _session = ++_sessions;
            _edge = 0;
        }
    }

    /// <summary>The live session's STOPPING, on the calling thread (the one that ends the world): every pending hop is
    /// dispatched cancelled, every after-step resumption of the session is released with its cancellation set.</summary>
    public void StopSession()
    {
        List<LeaseRecord> hops;
        lock (_lock)
        {
            var ended = _session;
            _session = 0;
            hops = [.. _hops];
            _hops.Clear();
            foreach (var l in _leases.Values.Where(l => l.Waiting && l.WaitAt.Session == ended).ToList())
            {
                l.Waiting = false;
                l.Cancel = true;
                Ready(l);
            }
        }
        foreach (var h in hops) DispatchHop(h, cancelled: true);
    }

    /// <summary>One server step on the tick thread: the hops queued before it (each with its lease's cancellation), the
    /// step's own tick work (<paramref name="pre"/>), then the Post edge.</summary>
    public void Step(Action? pre = null) => OnTick(() =>
    {
        List<LeaseRecord> hops;
        lock (_lock)
        {
            hops = [.. _hops];
            _hops.Clear();
        }
        foreach (var h in hops)
        {
            bool cancel;
            lock (_lock) cancel = h.Cancel;
            DispatchHop(h, cancel);
        }
        pre?.Invoke();
        lock (_lock)
        {
            if (_session == 0) return;
            _edge++;
            Edges++;
            foreach (var l in _leases.Values.Where(l => l.Waiting && l.WaitAt.Session == _session && l.WaitAt.Edge < _edge).ToList())
            {
                l.Waiting = false;
                Ready(l);
            }
        }
    });

    /// <summary>Runs <paramref name="job"/> on the tick thread and waits for it.</summary>
    public void OnTick(Action job)
    {
        var done = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        _tickJobs.Add(() =>
        {
            try
            {
                job();
                done.SetResult();
            }
            catch (Exception e)
            {
                done.SetException(e);
            }
        });
        if (!done.Task.Wait(TimeSpan.FromSeconds(20))) throw new TimeoutException("the fake tick did not finish its job");
    }

    public int PendingHops
    {
        get
        {
            lock (_lock) return _hops.Count;
        }
    }

    public LeaseRecord? Lease(string name)
    {
        lock (_lock) return _leases.Values.LastOrDefault(l => l.Name == name);
    }

    /// <summary>Waits until no segment runs and none is ready: everything left is suspended, waiting for a hop, an edge
    /// or the test.</summary>
    public void WaitIdle(int timeoutMs = 20000)
    {
        var deadline = Environment.TickCount64 + timeoutMs;
        lock (_lock)
        {
            while (_ready.Count > 0 || _running > 0)
            {
                var left = deadline - Environment.TickCount64;
                if (left <= 0 || !Monitor.Wait(_lock, TimeSpan.FromMilliseconds(left)))
                    throw new TimeoutException($"the fake pool did not go idle ({_ready.Count} ready, {_running} running)");
            }
        }
    }

    public void Dispose()
    {
        lock (_lock)
        {
            _disposed = true;
            Monitor.PulseAll(_lock);
        }
        _tickJobs.CompleteAdding();
        foreach (var w in _workers) w.Join(TimeSpan.FromSeconds(5));
        _tickThread.Join(TimeSpan.FromSeconds(5));
    }

    // ---- internals ----

    private LeaseRecord? Find(ulong lease) => _leases.GetValueOrDefault(lease);

    private InvalidOperationException Misuse(string member, LeaseRecord? l)
    {
        var what = $"{member} misused on lease {(l is null ? "?" : l.Name)} (the native table's INTERNAL)";
        Errors.Add(what);
        return new InvalidOperationException(what);
    }

    private void Ready(LeaseRecord l)
    {
        l.InReady = true;
        _ready.AddLast(l);
        Monitor.PulseAll(_lock);
    }

    private void PlaceAfter(LeaseRecord l, (ulong Session, ulong Edge) at)
    {
        if (at.Session != _session)
        {
            l.Cancel = true;  // its session is over: released cancelled, never carried into another
            Ready(l);
        }
        else if (l.Cancel)
        {
            Ready(l);
        }
        else
        {
            l.Waiting = true;
            l.WaitAt = at;
        }
    }

    private void ReleaseContinuation(LeaseRecord l)
    {
        if (l.AfterPending)
        {
            l.AfterPending = false;
            l.ResumePending = true;
        }
        else if (l.Waiting)
        {
            l.Waiting = false;
            Ready(l);
        }
    }

    private void MaybeDrop(LeaseRecord l)
    {
        if (l.Terminal && l.HopsPending == 0) _leases.Remove(l.Id);  // the backend drops terminal records
    }

    private void SegmentEnded(LeaseRecord l)
    {
        l.SegmentRunning = false;
        if (l.Finished)
        {
            l.State = l.Outcome switch
            {
                LeaseOutcome.Completed => KilnTaskState.Completed,
                LeaseOutcome.OwnTokenCancelled when l.Cancel => KilnTaskState.Cancelled,
                _ => KilnTaskState.Faulted,
            };
            MaybeDrop(l);
            return;
        }
        if (l.ResumePending)
        {
            l.ResumePending = false;
            Ready(l);
            return;
        }
        if (l.AfterPending)
        {
            l.AfterPending = false;
            PlaceAfter(l, l.AfterAt);
            return;
        }
        if (l.Armed) return;
        Errors.Add($"lease {l.Name}: a segment returned neither armed nor finished");
        l.State = KilnTaskState.Faulted;
        MaybeDrop(l);
    }

    private (TaskRun?, ModContext?, SynchronizationContext?, string?) Observed() =>
        (TaskExecutionScope.Current, ModContext.CurrentMod, SynchronizationContext.Current, TaskExecutionScope.FlowedIdentity);

    private void CheckLeak(string where, (TaskRun?, ModContext?, SynchronizationContext?, string?) before)
    {
        var after = Observed();
        if (after != before) lock (_lock) Leaks.Add($"{where} left {after} (was {before})");
    }

    private void DispatchHop(LeaseRecord h, bool cancelled)
    {
        var before = Observed();
        try
        {
            _dispatch!(h.Id, LeaseWork.TickHop, cancelled, h.HostState);
        }
        catch (Exception e)
        {
            lock (_lock) Errors.Add($"the dispatcher threw on a hop: {e}");
        }
        CheckLeak("a tick hop", before);
        lock (_lock)
        {
            h.HopsPending--;
            MaybeDrop(h);
        }
    }

    private void WorkerLoop()
    {
        t_worker = this;
        while (true)
        {
            LeaseRecord l;
            bool cancel;
            lock (_lock)
            {
                while (!_disposed && _ready.Count == 0) Monitor.Wait(_lock);
                if (_disposed) return;
                l = _ready.First!.Value;
                _ready.RemoveFirst();
                l.InReady = false;
                l.SegmentRunning = true;
                l.State = KilnTaskState.Running;
                cancel = l.Cancel;
                _running++;
            }
            var before = Observed();
            try
            {
                _dispatch!(l.Id, LeaseWork.Segment, cancel, l.HostState);
            }
            catch (Exception e)
            {
                lock (_lock) Errors.Add($"the dispatcher threw on a segment: {e}");
            }
            CheckLeak("a segment", before);
            lock (_lock)
            {
                SegmentEnded(l);
                _running--;
                Monitor.PulseAll(_lock);
            }
        }
    }

    private void TickLoop()
    {
        foreach (var job in _tickJobs.GetConsumingEnumerable()) job();
    }
}

/// <summary>Kiln.Api's backend for these tests: the kit's recording backend, except that only the fake's tick thread is
/// the tick thread.</summary>
internal sealed class TickAwareBackend(Kiln.TestKit.RecordingBackend inner, FakeTaskBackend pool) : IKilnBackend
{
    public string Version => inner.Version;
    public bool IsTickThread => pool.IsTickThread;
    public bool HasFeature(string name) => inner.HasFeature(name);
    public void Log(ulong owner, LogLevel level, string text) => inner.Log(owner, level, text);
    public nint Service(string name, uint minVersion) => inner.Service(name, minVersion);
}

/// <summary>Fresh Kiln state plus a fake pool with a live server session, bound as Kiln's task backend.</summary>
public abstract class TaskTest : KilnTest
{
    internal FakeTaskBackend Pool { get; private set; }

    protected TaskTest()
    {
        Pool = NewPool(2);
    }

    internal FakeTaskBackend NewPool(int workers)
    {
        var pool = new FakeTaskBackend(workers);
        pool.StartSession();
        TaskBackend.Install(pool);
        Kiln.Backend.Current = new TickAwareBackend(Backend, pool);
        return pool;
    }

    /// <summary>Replaces the pool (e.g. with one worker).</summary>
    internal void UsePool(int workers)
    {
        var old = Pool;
        Pool = NewPool(workers);
        old.Dispose();
    }

    public override void Dispose()
    {
        try
        {
            TaskRun.TestHook = null;
            TaskBackend.Install(null);
            Pool.Dispose();
            Kiln.Backend.Current = Backend;
            Assert.Empty(Pool.Errors);  // the workers and the tick thread have stopped
            Assert.Empty(Pool.Leaks);
        }
        finally
        {
            base.Dispose();
        }
    }

    /// <summary>Waits until <paramref name="task"/> has ended (never on a game thread: this is the test's thread).</summary>
    internal static void Ended(KilnTask task, int timeoutMs = 20000) =>
        Assert.True(task.WaitEnded(TimeSpan.FromMilliseconds(timeoutMs)), $"the task '{task.Name}' did not end ({task.State})");

    /// <summary>Tick code of <paramref name="mod"/>, as the host runs it in Pre: its scheduled work first, with the mod's
    /// tick context and CurrentMod installed.</summary>
    internal void TickAs(ModContext mod, Action? body = null) => Pool.Step(() =>
    {
        var previous = SynchronizationContext.Current;
        SynchronizationContext.SetSynchronizationContext(mod.Scheduler.SynchronizationContext);
        HostAccess.Current = mod;
        try
        {
            while (true)
            {
                Action? next;
                lock (mod.PendingLock)
                    if (!mod.Pending.TryDequeue(out next)) break;
                next();
            }
            body?.Invoke();
        }
        finally
        {
            HostAccess.Current = null;
            SynchronizationContext.SetSynchronizationContext(previous);
        }
    });
}
