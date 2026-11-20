// SPDX-License-Identifier: MIT
using System.Diagnostics;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace Kiln;

/// <summary>
/// Kiln.Saves' background writer (since 0.2.0-dev.29.3, issue #39): every file of a save (blobs, the pending manifest, the
/// confirmation, the pending manifest's delete, pruning's reads and deletes) is written here, off the world's tick, so the
/// world's tick never waits for a disk. The tick thread only captures the mod data (serialized bundles, in memory) and
/// hands it over. Since 0.2.0-dev.30 the writer is a task on Kiln's own pool (kiln.tasks), not a thread of its own.
/// </summary>
/// <remarks>
/// <para><b>The seam.</b> This class is only where and in what order the jobs run: it starts with the first job,
/// takes jobs (<see cref="Stage"/>, <see cref="Commit"/>), waits for them with a bounded wait (<see cref="Drain"/>) and
/// gives its runner back (<see cref="Stop"/>). What a job writes is <see cref="SidecarFiles"/>'s, which never sees a
/// thread.</para>
/// <para><b>The runner (0.2.0-dev.30).</b> ONE Kiln-internal LongRunning task, owner <c>kiln.saves</c>, named
/// <c>SaveWriter</c> (<see cref="ComponentTask"/>), runs the loop over this class's queue: it is admitted with the first
/// job, takes the jobs one at a time in queue order, and returns its pool thread once the queue has been empty for
/// <see cref="IdleLinger"/>, so it holds no thread between saves and the pool's watchdog names it only when a write is
/// really stuck. The next job admits a new one. One runner at a time is this class's own state (under its lock); one job
/// at a time is <see cref="_running"/>, whoever runs it. When its cancellation is asked (the process exit seals every
/// owner; a stop) the runner still writes every job already queued, each one whole, and only then returns: the token
/// never cuts a file job in half.</para>
/// <para><b>Without the pool.</b> When Kiln's task pool is not available here (safe idle, C# tasks off, the test kit, the
/// process exiting, <c>KILN_SAVES_WRITER=inline</c>) a job runs on the calling thread, after everything queued before it,
/// as before 0.2.0-dev.29.3 (logged once per reason). A runner that was admitted but has not started after
/// <see cref="StartGrace"/> (the pool's LongRunning threads all busy) is treated the same way, and every thread that waits
/// for the writer (<see cref="Drain"/>, <see cref="TryExclusive{T}"/>, the synchronous path) takes queued jobs itself, in
/// order, while no runner is in its loop: a runner that never starts can delay a write, never strand it.</para>
/// </remarks>
/// <remarks>
/// <para><b>Order.</b> Stagings go first, in the order they were made; confirmations (and their pruning) after them, in
/// order. A generation's pending manifest is therefore written before its confirmation, and a confirmation's delete of
/// the pending manifest comes after the confirmation is in place, as before.</para>
/// <para><b>The world write.</b> The tick thread used to write generation R's pending manifest before the game's snapshot,
/// so the world file of revision R never reached the disk before it. Now the pump reports each staging it hands over
/// (a growing number, <c>stage_progress(issued, 0)</c>) and the writer the newest it has on disk
/// (<c>stage_progress(0, done)</c>); kiln.saves holds the game's world write on its Save2Thread until done reaches what was
/// issued (native/services/saves/stage_barrier.h; bounded and logged there). Without that barrier (an older kiln.saves, a
/// build without the world write's hook, the test kit's fake) nothing would hold the world write, so the tick thread runs
/// each job itself, in turn with the writer, as before 0.2.0-dev.29.3.</para>
/// <para><b>Back-pressure.</b> A staging that comes while an earlier staging of the same generation is still waiting
/// replaces it (the save request's staging and the snapshot staging one step later): only the newest is written, and
/// the world write waits for that one. Stagings of different generations are never merged: every world file needs its
/// own manifest. More than one save cannot pile up, since the world write of the first waits for its staging and the
/// game starts no snapshot while its previous world write runs. Pruning stops between two files when a staging is
/// waiting and runs again after the next confirmation.</para>
/// <para><b>Stops, reloads, exits.</b> A world load or reload and a backup read the folder only once the writer has
/// written everything, and while it writes nothing (<see cref="TryExclusive{T}"/>, at most <see cref="DrainLimit"/>: a
/// load that cannot read then is deferred, a backup fails). A world close and the shutdown save's confirmation wait for it
/// at most <see cref="DrainLimit"/> (logged). A process that ends with jobs
/// queued (a crash, kill -9, or after that wait) leaves what an end at that point always left: no torn file (each one
/// goes to a temp name and is renamed), a staging is on disk before its world file while the world write's wait holds,
/// and a confirmation not written yet means the next start loads that generation as <c>Staged</c>, with the same
/// data. At a normal exit the pool asks the runner to stop: it writes what is queued (logged) while the pool's exit waits
/// its own bound for it.</para>
/// </remarks>
internal sealed unsafe class SaveWriter
{
    /// <summary>The process's one writer (one runner at a time, started at the first save).</summary>
    internal static readonly SaveWriter Shared = new();

    /// <summary>How long a world load, a world close, the shutdown save or a backup waits for the writer.</summary>
    internal static TimeSpan DrainLimit = TimeSpan.FromSeconds(10);

    /// <summary>How long an idle runner waits for the next job before it gives its pool thread back.</summary>
    internal static TimeSpan IdleLinger = TimeSpan.FromSeconds(5);

    /// <summary>How long jobs may wait for a runner that was admitted but has not started before the next job is written
    /// on the calling thread instead.</summary>
    internal static TimeSpan StartGrace = TimeSpan.FromSeconds(2);

    /// <summary>The runner's poll of a cancellation the pool asked on its own (the process exit seals every owner) while it
    /// waits for jobs: the pool's exit waits 500 ms for running bodies.</summary>
    private static readonly TimeSpan Poll = TimeSpan.FromMilliseconds(50);

    /// <summary>The runner's owner and name on Kiln's pool (kiln.log, tick-stats, crash reports: <c>kiln.saves/SaveWriter</c>).</summary>
    internal const string Component = "kiln.saves", TaskName = "SaveWriter";

    /// <summary><c>KILN_SAVES_WRITER=inline</c>: every job on the calling thread, the pool unused (labs, diagnosis).</summary>
    internal static bool ForceInline = string.Equals(Environment.GetEnvironmentVariable("KILN_SAVES_WRITER"), "inline", StringComparison.OrdinalIgnoreCase);

    private readonly object _lock = new();
    private readonly List<StageJob> _stages = [];
    private readonly Queue<Job> _others = new();
    private Job? _running;
    private ulong _issued, _done;  // under _lock
    private volatile int _stagesWaiting;

    // the runner, under _lock
    private int _runnerGen;        // one per admission
    private bool _admitted;        // a runner was admitted and has not given up (it may not have started yet)
    private bool _inLoop;          // ... and it is in its loop, taking jobs
    private long _admittedAt;      // Stopwatch timestamp of the admission
    private KilnTask? _runner;     // the admitted runner's task
    private bool _graceLogged;     // the "has not started" line of this admission
    private bool _stopping;        // Stop / the shutdown save: the runner returns as soon as nothing is queued
    private readonly HashSet<string> _fallbacks = new(StringComparer.Ordinal);  // reasons already logged (once each)

    private readonly SidecarFiles _files;

    public SaveWriter() => _files = new SidecarFiles(() => _stagesWaiting > 0);

    internal abstract class Job
    {
        public required int Epoch;
        public required string Root;
        public required string? WorldId;
        public long Queued;
        public double TickMs;
        /// <summary>Run on the calling (tick) thread: kiln.saves has no stage barrier here, or Kiln's task pool cannot run
        /// the writer (<see cref="InlineWhy"/>), so the tick thread writes its files itself, as before 0.2.0-dev.29.3.</summary>
        public bool Inline;
        /// <summary>Why a job of a build with the stage barrier ran on the calling thread; null: no barrier.</summary>
        public string? InlineWhy;

        /// <summary>A log line's account of the threads, up to the file part's label ("writer": the writer's pool task;
        /// "files": the tick thread wrote them), and the whole time.</summary>
        public (string Tick, double Total) TickPart(double queued, double work)
        {
            var total = TickMs + queued + work;
            return Inline
                ? ($"tick thread {total:F1} ms, all of it ({InlineWhy ?? "this kiln.saves does not hold the world write"}, so the tick " +
                   $"thread writes the files itself, as before 0.2.0-dev.29.3; {queued:F1} ms waiting for the writer); files", total)
                : ($"tick thread {TickMs:F1} ms; {queued:F1} ms queued; writer", total);
        }
    }

    /// <summary>A staging: the bundles as the tick thread serialized them.</summary>
    internal sealed class StageJob : Job
    {
        public required uint Gen;
        public required SaveReason Reason;
        public required bool Late, Snapshot;
        public required List<(string Scope, byte[] Data)> Bundles;
        public required long CreatedUnixMs;
        public required string Kiln;
        public bool Replaced;  // it replaced an earlier staging of its generation that was not written yet
        public ulong Ticket;
        public delegate* unmanaged[Cdecl]<ulong, ulong, int> Barrier;
        // the writer's
        public JsonObject? Manifest;
        public string? Content, Key;
        public bool Written;
        public string? Head, DiskText;  // the log line's parts (null: the write failed and was logged)
        public double QueuedMs, WorkMs;

        public (LogLevel Level, string Text) Line()
        {
            var (tick, total) = TickPart(QueuedMs, WorkMs);
            return (Late ? LogLevel.Warn : WorldStore.TimingLevel(total), $"{Head} in {total:F1} ms ({tick} {WorkMs:F1} ms: {DiskText})");
        }
    }

    /// <summary>A confirmation after the world write, with the store's pruning on the 1st, 11th, 21st... of a process.</summary>
    internal sealed class CommitJob : Job
    {
        public required uint Rev, Unix;
        public required SaveReason Reason;
        public required bool Prune;
        public string? WorldFile;  // when the event had no file time: its header is read here
        public string? SaveDir;    // pruning keeps the generations of the world files in it (and its backup/)
    }

    /// <summary>The tick thread hands a staging over. With kiln.saves' stage barrier and a runner on the pool it returns
    /// at once (the game's world write waits for it instead); without the barrier, or without a runner, the tick thread
    /// writes it itself, in turn with the writer, as before.</summary>
    public void Stage(StageJob job)
    {
        job.Barrier = SavesNative.StageBarrier;
        if (job.Barrier == null)
        {
            RunInline(job);
            return;
        }
        string? inline, note;
        lock (_lock)
        {
            job.Ticket = NextTicket();
            for (var i = _stages.Count - 1; i >= 0; i--)
                if (_stages[i].Epoch == job.Epoch && _stages[i].Gen == job.Gen && _stages[i].Root == job.Root)
                {
                    _stages.RemoveAt(i);
                    job.Replaced = true;
                }
            _stagesWaiting = _stages.Count;
            inline = EnsureRunner(out note);
            if (inline == null) Enqueue(job);
        }
        if (note != null) SavesRuntime.Log(LogLevel.Warn, note);
        job.Barrier(job.Ticket, 0);  // before the pump returns: the world write of this save waits for it
        if (inline != null)
        {
            job.InlineWhy = inline;
            RunInline(job);
        }
    }

    /// <summary>The tick thread hands a confirmation over: written off the tick thread where kiln.saves has its stage
    /// barrier (every build whose world write is hooked, which is where confirmations come from) and a runner can run.</summary>
    public void Commit(CommitJob job)
    {
        if (SavesNative.StageBarrier == null)
        {
            RunInline(job);
            return;
        }
        string? inline, note;
        lock (_lock)
        {
            inline = EnsureRunner(out note);
            if (inline == null) Enqueue(job);
        }
        if (note != null) SavesRuntime.Log(LogLevel.Warn, note);
        if (inline != null)
        {
            job.InlineWhy = inline;
            RunInline(job);
        }
    }

    /// <summary>Under <see cref="_lock"/>: makes sure a runner will take the next queued job, admitting one if none is.
    /// Null when one will; else why the job runs on the calling thread (and <paramref name="note"/>: a line to log, once
    /// per reason, after the lock).</summary>
    private string? EnsureRunner(out string? note)
    {
        note = null;
        if (ForceInline) return Fallback("KILN_SAVES_WRITER=inline", out note);
        if (_admitted)
        {
            if (_inLoop) return null;
            if (_runner is { IsTerminal: true })
            {
                // cancelled while it was still queued (a seal at the exit): it never ran and never will
                _admitted = false;
                _runner = null;
            }
            else if (Stopwatch.GetElapsedTime(_admittedAt) < StartGrace)
            {
                return null;
            }
            else
            {
                if (!_graceLogged)
                {
                    _graceLogged = true;
                    note = $"Kiln.Saves: the writer's task ({Component}/{TaskName}) has not started {Stopwatch.GetElapsedTime(_admittedAt).TotalSeconds:F1} s " +
                           "after it was admitted (Kiln's LongRunning task threads may all be busy); until it starts, a save's files are written on " +
                           "the calling thread";
                }
                return "the writer's pool task has not started yet";
            }
        }
        try
        {
            var gen = ++_runnerGen;
            var task = ComponentTask.Create(Component, TaskName, ctx => RunnerBody(ctx, gen)).Start();
            _admitted = true;
            _inLoop = false;
            _admittedAt = Stopwatch.GetTimestamp();
            _runner = task;
            _graceLogged = false;
            return null;
        }
        catch (Exception e) when (e is KilnUnavailableException or InvalidOperationException or KilnTaskQueueFullException or ArgumentException)
        {
            return Fallback(e.Message, out note);
        }
    }

    private string Fallback(string why, out string? note)
    {
        note = _fallbacks.Count < 16 && _fallbacks.Add(why)
            ? $"Kiln.Saves: the writer cannot run on Kiln's task pool here ({why}); a save's files are written on the calling thread, " +
              "as before 0.2.0-dev.29.3 (logged once)"
            : null;
        return $"Kiln's task pool cannot run the writer: {why}";
    }

    /// <summary>Runs a job on the calling thread, after everything handed to the writer before it. While no runner is in
    /// its loop this thread writes the queued jobs first, in order. It waits at most <see cref="DrainLimit"/> for a job
    /// that runs elsewhere (a disk that does not answer); then it queues the job behind it instead (logged).</summary>
    private void RunInline(Job job)
    {
        job.Inline = true;
        job.Queued = Stopwatch.GetTimestamp();
        var sw = Stopwatch.StartNew();
        while (true)
        {
            Job? help = null;
            string? queuedBehind = null, note = null;
            lock (_lock)
            {
                if (job is StageJob { Ticket: 0 } s) s.Ticket = NextTicket();
                while (true)
                {
                    if (_running == null)
                    {
                        if (!_inLoop && TakeNext() is { } next)
                        {
                            help = next;
                            break;
                        }
                        if (_stages.Count == 0 && _others.Count == 0)
                        {
                            _running = job;
                            break;
                        }
                    }
                    if (sw.Elapsed >= DrainLimit)
                    {
                        // a job that runs elsewhere does not end (a disk that does not answer): this one waits in the queue
                        job.Inline = false;
                        queuedBehind = $"Kiln.Saves: a save's {(job is StageJob ? "staging" : "confirmation")} waited {sw.Elapsed.TotalSeconds:F0} s " +
                                       "for the writer's job before it (the disk does not answer); it is queued behind it";
                        Enqueue(job);
                        EnsureRunner(out note);
                        break;
                    }
                    Monitor.Wait(_lock, Min(DrainLimit - sw.Elapsed, Poll));
                }
            }
            if (queuedBehind != null)
            {
                SavesRuntime.Log(LogLevel.Warn, queuedBehind);
                if (note != null) SavesRuntime.Log(LogLevel.Warn, note);
                return;
            }
            if (help == null) break;
            Execute(help);
            Finish(help);
        }
        Execute(job);
        Finish(job);
    }

    private static TimeSpan Min(TimeSpan a, TimeSpan b) => a < b ? (a > TimeSpan.Zero ? a : TimeSpan.Zero) : b;

    /// <summary>Under <see cref="_lock"/>, with nothing running: the next queued job, now <see cref="_running"/>.</summary>
    private Job? TakeNext()
    {
        Job job;
        if (_stages.Count > 0)
        {
            job = _stages[0];
            _stages.RemoveAt(0);
            _stagesWaiting = _stages.Count;
        }
        else if (_others.Count > 0)
        {
            job = _others.Dequeue();
        }
        else
        {
            return null;
        }
        _running = job;
        return job;
    }

    /// <summary>Staging numbers grow by one and skip 0 (stage_progress's "unchanged") when they wrap; they are compared as
    /// serial numbers (<see cref="Later"/>, stage_barrier.h), so the wrap after 2^64 stagings changes nothing.</summary>
    private ulong NextTicket()
    {
        if (++_issued == 0) _issued = 1;
        return _issued;
    }

    internal static bool Later(ulong a, ulong b) => (long)(a - b) > 0;

    private void Enqueue(Job job)
    {
        job.Queued = Stopwatch.GetTimestamp();
        _stopping = false;
        if (job is StageJob s)
        {
            _stages.Add(s);
            _stagesWaiting = _stages.Count;
        }
        else
        {
            _others.Enqueue(job);
        }
        Monitor.PulseAll(_lock);
    }

    /// <summary>Nothing queued and nothing running.</summary>
    public bool Idle
    {
        get
        {
            lock (_lock) return _running == null && _stages.Count == 0 && _others.Count == 0;
        }
    }

    /// <summary>Waits until the writer has written everything handed to it, at most <paramref name="limit"/> (default
    /// <see cref="DrainLimit"/>); logs a wait over 250 ms, and one that ran out. Returns whether it is idle. While no runner
    /// is in its loop, this thread writes the queued jobs itself, in order.</summary>
    public bool Drain(string why, TimeSpan? limit = null)
    {
        var max = limit ?? DrainLimit;
        var sw = Stopwatch.StartNew();
        bool idle;
        int left;
        while (true)
        {
            Job? help = null;
            lock (_lock)
            {
                while (!(idle = _running == null && _stages.Count == 0 && _others.Count == 0) && sw.Elapsed < max)
                {
                    if (_running == null && !_inLoop && TakeNext() is { } next)
                    {
                        help = next;
                        break;
                    }
                    Monitor.Wait(_lock, Min(max - sw.Elapsed, Poll));
                }
                left = _stages.Count + _others.Count + (_running != null ? 1 : 0);
            }
            if (help == null) break;
            Execute(help);
            Finish(help);
        }
        var ms = sw.Elapsed.TotalMilliseconds;
        if (!idle)
            SavesRuntime.Log(LogLevel.Warn, $"Kiln.Saves: {why}: the writer still has {left} write(s) to do after {ms:F0} ms; they go on in the " +
                                            "background (a crash before they are done loads an older generation, or this one as staged)");
        else if (ms > WorldStore.SlowMs)
            SavesRuntime.Log(LogLevel.Info, $"Kiln.Saves: {why}: waited {ms:F1} ms for the writer to finish its files");
        return idle;
    }

    /// <summary>Runs <paramref name="read"/> on the calling thread once the writer has written everything handed to it, and
    /// with nothing written while it runs, so a load or a backup reads the sidecar folder as one state (review 1: a reader
    /// that went on while a confirmation was being written could list a pending manifest, see it replaced by its
    /// confirmation and deleted, and take an older generation). Waits at most <paramref name="limit"/> (review 2: a disk
    /// that never answers must not hold the tick thread for good); false when the writer was still busy then, and
    /// <paramref name="read"/> did not run: the caller defers (a load) or fails (a backup). <paramref name="read"/> must not
    /// hand the writer a job (it would wait for itself). While no runner is in its loop, this thread writes the queued jobs
    /// first, in order.</summary>
    public bool TryExclusive<T>(string why, TimeSpan limit, Func<T> read, out T result)
    {
        var sw = Stopwatch.StartNew();
        int left;
        while (true)
        {
            Job? help = null;
            lock (_lock)
            {
                while ((left = _stages.Count + _others.Count + (_running != null ? 1 : 0)) > 0 && sw.Elapsed < limit)
                {
                    if (_running == null && !_inLoop && TakeNext() is { } next)
                    {
                        help = next;
                        break;
                    }
                    Monitor.Wait(_lock, Min(limit - sw.Elapsed, Poll));
                }
                if (help == null && left == 0) _running = Reader;
            }
            if (help == null) break;
            Execute(help);
            Finish(help);
        }
        var ms = sw.Elapsed.TotalMilliseconds;
        if (left > 0)
        {
            SavesRuntime.Log(LogLevel.Warn, $"Kiln.Saves: {why}: the writer still has {left} write(s) to do after {ms:F0} ms (the disk does " +
                                            "not answer); not read now");
            result = default!;
            return false;
        }
        if (ms > WorldStore.SlowMs) SavesRuntime.Log(LogLevel.Info, $"Kiln.Saves: {why}: waited {ms:F1} ms for the writer to finish its files");
        try
        {
            result = read();
            return true;
        }
        finally
        {
            lock (_lock)
            {
                _running = null;
                Monitor.PulseAll(_lock);
            }
        }
    }

    /// <summary>Generation <paramref name="gen"/> of the folder <paramref name="root"/>: its newest staging, or its staging
    /// written again at the confirmation, could not be written (its tries failed). Read while the writer is idle
    /// (<see cref="TryExclusive{T}"/>).</summary>
    public bool WriteFailed(string root, uint gen) => _files.Failed(root, gen);

    private sealed class ReaderJob : Job;

    /// <summary>What <see cref="_running"/> holds while <see cref="TryExclusive{T}"/> reads.</summary>
    private static readonly ReaderJob Reader = new() { Epoch = 0, Root = "", WorldId = null };

    /// <summary>Drains (bounded, as <see cref="Drain"/>) and lets the runner give its pool thread back now instead of after
    /// <see cref="IdleLinger"/>; the next job admits one again. Returns whether everything was written and the runner
    /// ended within <paramref name="limit"/>. The shutdown save and a world close use it, so the process's exit finds no
    /// runner waiting for jobs.</summary>
    public bool Stop(string why, TimeSpan? limit = null)
    {
        var max = limit ?? DrainLimit;
        var sw = Stopwatch.StartNew();
        var drained = Drain(why, max);
        if (!drained) return false;
        lock (_lock)
        {
            if (!_admitted) return true;
            _stopping = true;
            Monitor.PulseAll(_lock);
            while (_admitted && _stopping && _inLoop && sw.Elapsed < max) Monitor.Wait(_lock, Min(max - sw.Elapsed, Poll));
            return !_admitted || !_inLoop;
        }
    }

    /// <summary>Lets the runner give its pool thread back as soon as nothing is queued, without waiting for it (any
    /// thread): the shutdown save and a world close, after their drain.</summary>
    public void Release()
    {
        lock (_lock)
        {
            if (!_admitted) return;
            _stopping = true;
            Monitor.PulseAll(_lock);
        }
    }

    /// <summary>Tests: the fallback lines are logged once per process; a test that checks one starts from none.</summary>
    internal void ForgetLoggedFallbacks()
    {
        lock (_lock) _fallbacks.Clear();
    }

    /// <summary>A runner is admitted on the pool now (tests).</summary>
    internal bool Started
    {
        get
        {
            lock (_lock) return _admitted;
        }
    }

    /// <summary>The admitted runner's task, while there is one (tests, diagnostics).</summary>
    internal KilnTask? Runner
    {
        get
        {
            lock (_lock) return _admitted ? _runner : null;
        }
    }

    // ---- the runner (a pool task) ----

    private void RunnerBody(KilnTaskContext ctx, int gen)
    {
        var mine = false;
        long cancelledAt = 0;
        int leftAtCancel = 0, written = 0;
        try
        {
            lock (_lock)
            {
                if (gen != _runnerGen || !_admitted) return;  // an older admission that was given up (it never ran)
                mine = true;
                _inLoop = true;
                Monitor.PulseAll(_lock);
            }
            while (true)
            {
                Job job;
                lock (_lock)
                {
                    var idleSince = Stopwatch.GetTimestamp();
                    while (_running != null || (_stages.Count == 0 && _others.Count == 0))
                    {
                        var cancelled = ctx.CancellationAsked;
                        if (_running == null && (cancelled || _stopping || Stopwatch.GetElapsedTime(idleSince) >= IdleLinger))
                        {
                            // nothing queued, nothing running: the pool thread goes back; the next job admits a runner again
                            _admitted = false;
                            _inLoop = false;
                            _runner = null;
                            _stopping = false;
                            mine = false;
                            Monitor.PulseAll(_lock);
                            return;
                        }
                        Monitor.Wait(_lock, Poll);
                    }
                    if (cancelledAt == 0 && ctx.CancellationAsked)
                    {
                        // the pool asked the runner to stop (the process exits): what is queued is still written, each job whole
                        cancelledAt = Stopwatch.GetTimestamp();
                        leftAtCancel = _stages.Count + _others.Count;
                    }
                    job = TakeNext()!;
                }
                Execute(job);
                Finish(job);
                if (cancelledAt != 0) written++;
            }
        }
        finally
        {
            if (mine)
                lock (_lock)
                {
                    // the body ended another way (a fault in this class): the next job admits a runner again
                    if (gen == _runnerGen)
                    {
                        _admitted = false;
                        _inLoop = false;
                        _runner = null;
                    }
                    Monitor.PulseAll(_lock);
                }
            if (cancelledAt != 0)
                SavesRuntime.Log(LogLevel.Info, $"Kiln.Saves: the writer's task was asked to stop (the game is exiting, or Kiln stopped it) with " +
                                                $"{leftAtCancel} write(s) queued; it wrote {written} before it ended, in " +
                                                $"{Stopwatch.GetElapsedTime(cancelledAt).TotalMilliseconds:F1} ms");
        }
    }

    /// <summary>One job, on whichever thread holds <see cref="_running"/>: never two at once.</summary>
    private void Execute(Job job)
    {
        try
        {
            _files.Run(job);
        }
        catch (Exception e)
        {
            SavesRuntime.Log(LogLevel.Error, $"Kiln.Saves: the writer failed: {e}");
        }
    }

    private void Finish(Job job)
    {
        lock (_lock)
        {
            _running = null;
            if (job is StageJob s && Later(s.Ticket, _done)) _done = s.Ticket;
            Monitor.PulseAll(_lock);
        }
        if (job is not StageJob st) return;
        if (st.Barrier != null) st.Barrier(0, st.Ticket);  // the world write of its save may go now
        if (st.Head != null)
        {
            var (level, text) = st.Line();
            SavesRuntime.Log(level, text);
        }
    }
}

/// <summary>What one write of the save path spent on its files, item by item, so a log line from a slow disk says where
/// the time went (issue #39: a confirmation of 4.6 s with 0.4 ms of it itemised). Every file operation goes through
/// here and through <see cref="WorldStore.IoHook"/>.</summary>
internal struct DiskTime
{
    public int Files, Unforced, Checks, Reads, Deletes, Lists, Folders;
    public double CreateMs, WriteMs, FlushMs, CloseMs, RenameMs, CheckMs, ReadMs, DeleteMs, ListMs, FolderMs, CpuMs;

    public readonly double Itemised =>
        CreateMs + WriteMs + FlushMs + CloseMs + RenameMs + CheckMs + ReadMs + DeleteMs + ListMs + FolderMs + CpuMs;

    private static string S(int n) => n == 1 ? "" : "s";

    /// <param name="total">The whole time of the step these files belong to: what is not itemised shows as "else".</param>
    public readonly string Text(double total)
    {
        var sb = new StringBuilder();
        if (Files == 0) sb.Append("no file written");
        else
            sb.Append($"{Files} file{S(Files)} written{(Unforced > 0 ? $" ({Unforced} not forced to disk)" : "")}: {CreateMs:F1} ms creating, " +
                      $"{WriteMs:F1} ms writing, {FlushMs:F1} ms forcing to disk, {CloseMs:F1} ms closing, {RenameMs:F1} ms renaming");
        if (Checks > 0) sb.Append($"; {Checks} checked in {CheckMs:F1} ms");
        if (Reads > 0) sb.Append($"; {Reads} read in {ReadMs:F1} ms");
        if (Deletes > 0) sb.Append($"; {Deletes} deleted in {DeleteMs:F1} ms");
        if (Lists > 0) sb.Append($"; {Lists} folder{S(Lists)} listed in {ListMs:F1} ms");
        if (Folders > 0) sb.Append($"; {Folders} folder{S(Folders)} made in {FolderMs:F1} ms");
        sb.Append($"; {CpuMs:F1} ms hashing and JSON; {Math.Max(0, total - Itemised):F1} ms else");
        return sb.ToString();
    }

    private static long Now() => Stopwatch.GetTimestamp();
    private static double Since(long t) => Stopwatch.GetElapsedTime(t).TotalMilliseconds;

    public bool Exists(string path)
    {
        var t = Now();
        WorldStore.Io("check");
        var r = File.Exists(path);
        CheckMs += Since(t);
        Checks++;
        return r;
    }

    public byte[] ReadAll(string path)
    {
        var t = Now();
        try
        {
            WorldStore.Io("read");
            return File.ReadAllBytes(path);
        }
        finally
        {
            ReadMs += Since(t);
            Reads++;
        }
    }

    public WorldStore.Header? ReadHeader(string file)
    {
        var t = Now();
        var h = WorldStore.ReadHeader(file);
        ReadMs += Since(t);
        Reads++;
        return h;
    }

    public void Delete(string path)
    {
        var t = Now();
        try
        {
            WorldStore.Io("delete");
            File.Delete(path);
        }
        finally
        {
            DeleteMs += Since(t);
            Deletes++;
        }
    }

    public T List<T>(Func<T> list)
    {
        var t = Now();
        try
        {
            WorldStore.Io("list");
            return list();
        }
        finally
        {
            ListMs += Since(t);
            Lists++;
        }
    }

    public void CreateFolder(string path)
    {
        var t = Now();
        WorldStore.Io("folder");
        Directory.CreateDirectory(path);
        FolderMs += Since(t);
        Folders++;
    }
}
