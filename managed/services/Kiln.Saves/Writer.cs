// SPDX-License-Identifier: MIT
using System.Diagnostics;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace Kiln;

/// <summary>
/// Kiln.Saves' background writer (since 0.2.0-dev.30, issue #39): every file of a save (blobs, the pending manifest, the
/// confirmation, the pending manifest's delete, pruning's reads and deletes) is written here, on its own thread, so the
/// world's tick never waits for a disk. The tick thread only captures the mod data (serialized bundles, in memory) and
/// hands it over.
/// </summary>
/// <remarks>
/// <para><b>The seam.</b> This class is only where and in what order the jobs run: it starts with the first job,
/// takes jobs (<see cref="Stage"/>, <see cref="Commit"/>), waits for them with a bounded wait (<see cref="Drain"/>) and
/// gives its thread back (<see cref="Stop"/>). What a job writes is <see cref="SidecarFiles"/>'s, which never sees a
/// thread. Today it is one dedicated thread; a Kiln-owned executor can run the same jobs, one at a time and in this
/// order, without touching the save logic.</para>
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
/// each job itself, in turn with the writer, as before 0.2.0-dev.30.</para>
/// <para><b>Back-pressure.</b> A staging that comes while an earlier staging of the same generation is still waiting
/// replaces it (the save request's staging and the snapshot staging one step later): only the newest is written, and
/// the world write waits for that one. Stagings of different generations are never merged: every world file needs its
/// own manifest. More than one save cannot pile up, since the world write of the first waits for its staging and the
/// game starts no snapshot while its previous world write runs. Pruning stops between two files when a staging is
/// waiting and runs again after the next confirmation.</para>
/// <para><b>Stops, reloads, exits.</b> A world load or reload, a world close, the shutdown save's confirmation and a
/// backup wait for the writer (bounded, logged): files that are not written yet still are, before anything reads the
/// folder. A process that ends with jobs queued (a crash, kill -9) leaves what an end at that point always left: no torn
/// file (each one goes to a temp name and is renamed), a staging is on disk before its world file, and a confirmation not
/// written yet means the next start loads that generation as <c>Staged</c>, with the same data.</para>
/// </remarks>
internal sealed unsafe class SaveWriter
{
    /// <summary>The process's one writer (one thread, started at the first save).</summary>
    internal static readonly SaveWriter Shared = new();

    /// <summary>How long a world load, a world close, the shutdown save or a backup waits for the writer.</summary>
    internal static TimeSpan DrainLimit = TimeSpan.FromSeconds(10);

    private readonly object _lock = new();
    private readonly List<StageJob> _stages = [];
    private readonly Queue<Job> _others = new();
    private Job? _running;
    private Thread? _thread;
    private ulong _issued, _done;  // under _lock
    private volatile int _stagesWaiting;

    private bool _stopping;
    private readonly SidecarFiles _files;

    public SaveWriter() => _files = new SidecarFiles(() => _stagesWaiting > 0);

    internal abstract class Job
    {
        public required int Epoch;
        public required string Root;
        public required string? WorldId;
        public long Queued;
        public double TickMs;
        /// <summary>Run on the calling (tick) thread: kiln.saves has no stage barrier here, so the tick thread writes its
        /// files itself, as before 0.2.0-dev.30.</summary>
        public bool Inline;

        /// <summary>A log line's account of the threads, up to the file part's label ("writer": its own thread; "files": the
        /// tick thread wrote them), and the whole time.</summary>
        public (string Tick, double Total) TickPart(double queued, double work)
        {
            var total = TickMs + queued + work;
            return Inline
                ? ($"tick thread {total:F1} ms, all of it (this kiln.saves does not hold the world write, so the tick thread writes " +
                   $"the files itself, as before 0.2.0-dev.30; {queued:F1} ms waiting for the writer); files", total)
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

    /// <summary>The tick thread hands a staging over. With kiln.saves' stage barrier it returns at once (the game's world
    /// write waits for it instead); without it, the tick thread writes it itself, in turn with the writer, as before.</summary>
    public void Stage(StageJob job)
    {
        job.Barrier = SavesNative.StageBarrier;
        if (job.Barrier == null)
        {
            RunInline(job);
            return;
        }
        lock (_lock)
        {
            job.Ticket = ++_issued;
            for (var i = _stages.Count - 1; i >= 0; i--)
                if (_stages[i].Epoch == job.Epoch && _stages[i].Gen == job.Gen && _stages[i].Root == job.Root)
                {
                    _stages.RemoveAt(i);
                    job.Replaced = true;
                }
            Enqueue(job);
        }
        job.Barrier(job.Ticket, 0);  // before the pump returns: the world write of this save waits for it
    }

    /// <summary>The tick thread hands a confirmation over: written off the tick thread where kiln.saves has its stage
    /// barrier (every build whose world write is hooked, which is where confirmations come from).</summary>
    public void Commit(CommitJob job)
    {
        if (SavesNative.StageBarrier == null)
        {
            RunInline(job);
            return;
        }
        lock (_lock) Enqueue(job);
    }

    /// <summary>Runs a job on the calling thread, after everything handed to the writer before it.</summary>
    private void RunInline(Job job)
    {
        job.Inline = true;
        job.Queued = Stopwatch.GetTimestamp();
        lock (_lock)
        {
            if (job is StageJob s) s.Ticket = ++_issued;
            while (_running != null || _stages.Count > 0 || _others.Count > 0) Monitor.Wait(_lock);
            _running = job;
        }
        Execute(job);
        Finish(job);
    }

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
        if (_thread == null)
        {
            _thread = new Thread(Run) { IsBackground = true, Name = "kiln saves writer" };
            _thread.Start();
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
    /// <see cref="DrainLimit"/>); logs a wait over 250 ms, and one that ran out. Returns whether it is idle.</summary>
    public bool Drain(string why, TimeSpan? limit = null)
    {
        var max = limit ?? DrainLimit;
        var sw = Stopwatch.StartNew();
        bool idle;
        int left;
        lock (_lock)
        {
            while (!(idle = _running == null && _stages.Count == 0 && _others.Count == 0) && sw.Elapsed < max)
                Monitor.Wait(_lock, max - sw.Elapsed);
            left = _stages.Count + _others.Count + (_running != null ? 1 : 0);
        }
        var ms = sw.Elapsed.TotalMilliseconds;
        if (!idle)
            SavesRuntime.Log(LogLevel.Warn, $"Kiln.Saves: {why}: the writer still has {left} write(s) to do after {ms:F0} ms; they go on in the " +
                                            "background (a crash before they are done loads an older generation, or this one as staged)");
        else if (ms > WorldStore.SlowMs)
            SavesRuntime.Log(LogLevel.Info, $"Kiln.Saves: {why}: waited {ms:F1} ms for the writer to finish its files");
        return idle;
    }

    /// <summary>Drains (bounded, as <see cref="Drain"/>) and ends the writer's thread; the next job starts one again.
    /// Returns whether everything was written. Nothing in Kiln stops it today (a process ends with it); the seam has it
    /// for an executor that wants its thread back.</summary>
    public bool Stop(string why, TimeSpan? limit = null)
    {
        var drained = Drain(why, limit);
        if (!drained) return false;
        lock (_lock)
        {
            if (_thread == null) return true;
            _stopping = true;
            Monitor.PulseAll(_lock);
            while (_thread != null && _stopping) Monitor.Wait(_lock, TimeSpan.FromSeconds(1));
        }
        return true;
    }

    /// <summary>The writer has a thread now (tests).</summary>
    internal bool Started
    {
        get
        {
            lock (_lock) return _thread != null;
        }
    }

    // ---- the writer thread ----

    private void Run()
    {
        while (true)
        {
            Job job;
            lock (_lock)
            {
                while (_running != null || (_stages.Count == 0 && _others.Count == 0))
                {
                    if (_stopping && _running == null)
                    {
                        _stopping = false;
                        _thread = null;  // the next job starts a thread again
                        Monitor.PulseAll(_lock);
                        return;
                    }
                    Monitor.Wait(_lock);
                }
                if (_stages.Count > 0)
                {
                    job = _stages[0];
                    _stages.RemoveAt(0);
                    _stagesWaiting = _stages.Count;
                }
                else
                {
                    job = _others.Dequeue();
                }
                _running = job;
            }
            Execute(job);
            Finish(job);
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
            if (job is StageJob s && s.Ticket > _done) _done = s.Ticket;
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
        Directory.CreateDirectory(path);
        FolderMs += Since(t);
        Folders++;
    }
}
