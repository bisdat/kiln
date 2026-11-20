// SPDX-License-Identifier: MIT
// Kiln.Saves' writer on Kiln's task pool (0.2.0-dev.30, "Kiln's own save writing runs on the task pool"): one
// Kiln-internal LongRunning task kiln.saves/SaveWriter instead of a thread of its own, the same jobs in the same order,
// every queued job written when the pool asks it to stop (the process exit), the synchronous path when the pool cannot
// run it (logged once), a runner that never starts never strands a job, and the pool and the synchronous path write the
// same files.
using System.Diagnostics;
using System.Reflection;
using System.Text.Json.Nodes;
using Xunit;
using static Kiln.InstanceHarness;

#pragma warning disable xUnit1031  // the test thread is the tick here: it blocks on the fakes as the game would

namespace Kiln;

[Collection("instance data")]
public sealed class WriterPoolTests : IDisposable
{
    private static readonly TimeSpan Long = TimeSpan.FromSeconds(30);
    private readonly InstanceHarness _h = new();
    private readonly Action<string>? _hook = WorldStore.IoHook;
    private readonly IKilnBackend _backend = Backend.Current;
    private readonly Func<string, ulong>? _owners = ComponentTask.Owners;
    private readonly Dictionary<string, object?> _knobs = [];
    private readonly List<ManualResetEventSlim> _gates = [];
    private readonly LogCapture _log = new();

    public WriterPoolTests()
    {
        Assert.True(Store.Writer.Stop("test start"), "the writer did not stop");  // no runner of an earlier test
        Backend.Current = _log;
    }

    public void Dispose()
    {
        foreach (var g in _gates) g.Set();
        WorldStore.IoHook = _hook;
        ComponentTask.Owners = _owners;
        foreach (var (name, value) in _knobs) Knob(name).SetValue(null, value);
        try
        {
            Store.Writer.Stop("test end");
        }
        finally
        {
            Backend.Current = _backend;
            _h.Dispose();
        }
    }

    // ---- helpers ----

    private static FieldInfo Knob(string name) =>
        typeof(SaveWriter).GetField(name, BindingFlags.Static | BindingFlags.NonPublic) ?? throw new MissingFieldException(nameof(SaveWriter), name);

    private void SetKnob(string name, object value)
    {
        var f = Knob(name);
        if (!_knobs.ContainsKey(name)) _knobs[name] = f.GetValue(null);
        f.SetValue(null, value);
    }

    private string Pending(uint revision) => Path.Combine(_h.Root, "manifests", $"{revision:D10}.pending.json");
    private string Confirmed(uint revision, uint unix) => Path.Combine(_h.Root, "manifests", $"{revision:D10}-{unix}.json");

    private static void Dispatch(uint kind, ulong seq, uint revision, ulong saveSeq = 0, uint unix = 0, SaveReason reason = SaveReason.Autosave) =>
        typeof(SavesRuntime).GetMethod("Dispatch", BindingFlags.Static | BindingFlags.NonPublic)!.Invoke(null,
            [new SavesNative.Event { Kind = kind, Seq = seq, Revision = revision, SaveSeq = saveSeq, WorldUnix = unix, Reason = (uint)reason }]);

    private static void Bundles(int value)
    {
        for (var i = 0; i < 5; i++)
            Store.GetBundleRaw($"world/mod-{i}", $"mod-{i}").Keys["state"] = new WorldStore.Slot { Json = $"{{\"v\":{value + i}}}" };
    }

    /// <summary>A whole save through the events, the world write waiting as kiln.saves' barrier makes it wait.</summary>
    private static void Save(uint gen, SaveReason reason = SaveReason.Autosave)
    {
        var seq = gen * 100UL;
        Dispatch(SavesNative.EvSaving, seq, gen, reason: reason);
        Dispatch(SavesNative.EvSnapshot, seq + 1, gen, saveSeq: seq, reason: reason);
        FakeSavesTable.WaitLikeTheWorldWrite(Long);
        Dispatch(SavesNative.EvWritten, seq + 2, gen, saveSeq: seq, unix: 1000 + gen, reason: reason);
    }

    /// <summary>The threads every file operation of the save path ran on.</summary>
    private List<(int Thread, string Op)> WatchThreads()
    {
        var seen = new List<(int, string)>();
        WorldStore.IoHook = op =>
        {
            lock (seen) seen.Add((Environment.CurrentManagedThreadId, op));
        };
        return seen;
    }

    /// <summary>The writer stops before its next file operation named <paramref name="op"/> off this thread until the
    /// returned gate is set; <paramref name="reached"/> is set when it got there.</summary>
    private ManualResetEventSlim FreezeAt(string op, out ManualResetEventSlim reached)
    {
        var gate = new ManualResetEventSlim();
        var at = new ManualResetEventSlim();
        _gates.Add(gate);
        var tick = Environment.CurrentManagedThreadId;
        var once = 0;
        WorldStore.IoHook = o =>
        {
            if (o != op || Environment.CurrentManagedThreadId == tick || Interlocked.Exchange(ref once, 1) != 0) return;
            at.Set();
            gate.Wait(Long);
        };
        reached = at;
        return gate;
    }

    private static void Until(Func<bool> condition, string what) => Assert.True(SpinWait.SpinUntil(condition, Long), what);

    // ---- the runner ----

    [Fact]
    public void TheWriterIsOneLongRunningTaskOfKilnSavesOnKilnsPool()
    {
        using var native = new FakeSavesTable();
        var before = TestPool.WriterAdmissions();
        var threads = WatchThreads();
        Bundles(1);
        Save(1);
        Bundles(10);
        Save(2);
        Written();
        Assert.True(File.Exists(Confirmed(2, 1002)));
        Assert.Equal(1, TestPool.WriterAdmissions() - before);  // one runner for both saves (it waits a while for the next job)
        lock (TestPool.Pool.Admitted)
            Assert.True(TestPool.Pool.Admitted.Last(a => a.Name == SaveWriter.TaskName).Options.LongRunning);
        lock (threads)
        {
            Assert.NotEmpty(threads);
            Assert.All(threads, t => Assert.True(TestPool.Pool.IsWorker(t.Thread), $"'{t.Op}' ran on thread {t.Thread}, not on a pool worker"));
        }
        Assert.DoesNotContain(_log.Lines, l => l.Text.Contains("cannot run on Kiln's task pool"));
    }

    [Fact]
    public void JobsRunOneAtATimeInTheOrderTheyWereHandedOverWhileOtherOwnersTasksRun()
    {
        using var native = new FakeSavesTable();
        using var stop = new CancellationTokenSource();
        var others = Enumerable.Range(0, 2).Select(i => new ComponentTask(0x77000 + (ulong)i, "other", "busy", ctx =>
        {
            while (!stop.IsCancellationRequested && !ctx.Cancellation.IsCancellationRequested) Thread.SpinWait(1000);
        }).Start()).ToList();
        var inside = 0;
        var overlaps = 0;
        WorldStore.IoHook = _ =>
        {
            if (Interlocked.Increment(ref inside) > 1) Interlocked.Increment(ref overlaps);
            Thread.Sleep(1);
            Interlocked.Decrement(ref inside);
        };
        for (uint gen = 1; gen <= 6; gen++)
        {
            Bundles((int)gen * 10);
            Save(gen);
        }
        Written();
        stop.Cancel();
        foreach (var t in others) Assert.True(t.WaitEnded(Long));
        Assert.Equal(0, Volatile.Read(ref overlaps));
        Assert.Equal(FakeSavesTable.Issued, FakeSavesTable.Done);
        for (uint gen = 1; gen <= 6; gen++)
        {
            Assert.True(File.Exists(Confirmed(gen, 1000 + gen)), $"generation {gen} confirmed");
            Assert.False(File.Exists(Pending(gen)), $"generation {gen}'s pending manifest deleted");
        }
        List<uint> staged;
        lock (_log.Lines)
            staged = _log.Lines.Select(l => l.Text).Where(t => t.StartsWith("Kiln.Saves: staged mod data generation ", StringComparison.Ordinal))
                .Select(t => uint.Parse(t["Kiln.Saves: staged mod data generation ".Length..].Split(' ')[0])).ToList();
        Assert.Equal(staged.Order().ToList(), staged);  // written in the order they were handed over
        Assert.Equal(DataMatch.Exact, _h.Restart(6, new FakeInstanceWorld()));
    }

    [Fact]
    public void AnIdleRunnerGivesItsThreadBackAndTheNextSaveAdmitsAnother()
    {
        SetKnob("IdleLinger", TimeSpan.FromMilliseconds(100));
        using var native = new FakeSavesTable();
        var before = TestPool.WriterAdmissions();
        Bundles(1);
        Save(1);
        Written();
        Until(() => !Store.Writer.Started, "the idle runner gave its thread back");
        Bundles(2);
        Save(2);
        Written();
        Assert.Equal(2, TestPool.WriterAdmissions() - before);
        Assert.True(File.Exists(Confirmed(2, 1002)));
        Assert.Equal(DataMatch.Exact, _h.Restart(2, new FakeInstanceWorld()));
        Assert.Equal(6, int.Parse(Store.FindBundle("world/mod-4")!.Keys["state"].Json!.Split(':')[1].TrimEnd('}')));
    }

    [Fact]
    public void WhenThePoolAsksTheRunnerToStopItStillWritesEveryQueuedJobAndThenEnds()
    {
        const ulong exiting = 0xE817;  // an owner of its own: the test seals it as the process exit seals every owner
        ComponentTask.Owners = c => c == SaveWriter.Component ? exiting : 0;
        using var native = new FakeSavesTable();
        Bundles(1);
        Save(1);
        Written();
        Bundles(20);
        var gate = FreezeAt("rename", out var reached);    // the runner is in the middle of the save request's staging
        Dispatch(SavesNative.EvSaving, 200, 2);
        Assert.True(reached.Wait(Long), "the runner never got there");
        Dispatch(SavesNative.EvSnapshot, 201, 2, saveSeq: 200);            // the snapshot staging waits in the queue
        Dispatch(SavesNative.EvWritten, 202, 2, saveSeq: 200, unix: 1002);  // and the confirmation behind it
        var runner = Store.Writer.Runner;
        Assert.NotNull(runner);
        TestPool.Pool.SealOwner(exiting);                  // the exit: every owner sealed, running bodies signalled
        gate.Set();
        Assert.True(runner!.WaitEnded(Long), "the runner did not end after its queue");
        Assert.Equal(KilnTaskState.Completed, runner.State);
        Assert.True(File.Exists(Confirmed(2, 1002)), "the confirmation queued at the exit was written");
        Assert.False(File.Exists(Pending(2)));
        Assert.Empty(Directory.EnumerateFiles(_h.Root, "*.tmp-*", SearchOption.AllDirectories));
        var line = _log.Single("Kiln.Saves: the writer's task was asked to stop");
        Assert.True(line.Text.Contains("with 2 write(s) queued; it wrote 2 before it ended"), line.Text);

        // after the exit began the pool takes no task: the next save is written on the calling thread, logged once
        var threads = WatchThreads();
        Bundles(30);
        Save(3);
        Bundles(40);
        Save(4);
        Assert.True(Store.Writer.Idle);
        Assert.True(File.Exists(Confirmed(4, 1004)));
        lock (threads) Assert.All(threads, t => Assert.Equal(Environment.CurrentManagedThreadId, t.Thread));
        Assert.Single(_log.Lines, l => l.Text.StartsWith("Kiln.Saves: the writer cannot run on Kiln's task pool here", StringComparison.Ordinal));
    }

    [Fact]
    public void WithoutThePoolASaveIsWrittenOnTheCallingThreadAsBeforeLoggedOnce()
    {
        ComponentTask.Owners = null;  // no host bound Kiln's task pool here (safe idle, C# tasks off)
        using var native = new FakeSavesTable();
        var threads = WatchThreads();
        Bundles(1);
        Dispatch(SavesNative.EvSaving, 100, 1);
        Dispatch(SavesNative.EvSnapshot, 101, 1, saveSeq: 100);
        Assert.Equal(FakeSavesTable.Issued, FakeSavesTable.Done);  // on disk before the pump returned: the world write never waits
        Assert.True(File.Exists(Pending(1)));
        Dispatch(SavesNative.EvWritten, 102, 1, saveSeq: 100, unix: 1001);
        Assert.True(File.Exists(Confirmed(1, 1001)));
        Bundles(2);
        Save(2);
        Assert.True(File.Exists(Confirmed(2, 1002)));
        Assert.False(Store.Writer.Started);
        lock (threads)
        {
            Assert.NotEmpty(threads);
            Assert.All(threads, t => Assert.Equal(Environment.CurrentManagedThreadId, t.Thread));
        }
        var line = _log.Single("Kiln.Saves: the writer cannot run on Kiln's task pool here");
        Assert.Contains("Kiln's task pool is not running in this process", line.Text);
        Assert.Contains(_log.Lines, l => l.Text.StartsWith("Kiln.Saves: staged mod data generation 2", StringComparison.Ordinal) &&
                                         l.Text.Contains("tick thread") && l.Text.Contains("all of it (Kiln's task pool cannot run the writer"));
        Assert.Equal(DataMatch.Exact, _h.Restart(2, new FakeInstanceWorld()));
    }

    [Fact]
    public void ARunnerThatDoesNotStartNeverStrandsAJob()
    {
        SetKnob("StartGrace", TimeSpan.FromMilliseconds(200));
        using var native = new FakeSavesTable();
        Bundles(1);
        Save(1);
        Written();
        Assert.True(Store.Writer.Stop("before the busy pool"));
        // every pool worker busy with another owner's work: an admitted runner stays queued
        using var release = new ManualResetEventSlim();
        _gates.Add(release);
        var running = 0;
        var blockers = Enumerable.Range(0, 4).Select(i => new ComponentTask(0x88000 + (ulong)i, "other", "blocker", _ =>
        {
            Interlocked.Increment(ref running);
            release.Wait(Long);
        }).Start()).ToList();
        Until(() => Volatile.Read(ref running) == 4, "the blockers hold every worker");
        Bundles(10);
        Dispatch(SavesNative.EvSaving, 200, 2);
        Dispatch(SavesNative.EvSnapshot, 201, 2, saveSeq: 200);  // queued for the runner, which cannot start
        Assert.True(Store.Writer.Started);
        Assert.False(File.Exists(Pending(2)));
        Thread.Sleep(300);                                          // past the grace
        Dispatch(SavesNative.EvWritten, 202, 2, saveSeq: 200, unix: 1002);  // written here, after the queued staging
        Assert.True(Store.Writer.Idle);
        Assert.True(File.Exists(Confirmed(2, 1002)));
        Assert.False(File.Exists(Pending(2)));
        Assert.Equal(FakeSavesTable.Issued, FakeSavesTable.Done);
        _log.Single("Kiln.Saves: the writer's task (kiln.saves/SaveWriter) has not started");
        release.Set();
        foreach (var b in blockers) Assert.True(b.WaitEnded(Long));
        Bundles(20);
        Save(3);                                                    // the runner started late, found nothing, or a new one runs
        Written();
        Assert.True(File.Exists(Confirmed(3, 1003)));
        Assert.Equal(DataMatch.Exact, _h.Restart(3, new FakeInstanceWorld()));
    }

    [Fact]
    public void ThePoolAndTheSynchronousPathWriteTheSameFiles()
    {
        using var native = new FakeSavesTable();
        var pool = Folder(onPool: true);
        _h.Dispose();
        using var other = new InstanceHarness();
        ComponentTask.Owners = null;
        var sync = Folder(onPool: false, other);
        var a = Files(pool);
        var b = Files(sync);
        Assert.Equal(a.Keys.Order(), b.Keys.Order());
        foreach (var (name, bytes) in a)
            Assert.True(bytes.AsSpan().SequenceEqual(b[name]),
                $"{name} differs between the pool and the synchronous path:
pool: {System.Text.Encoding.UTF8.GetString(bytes)}
sync: {System.Text.Encoding.UTF8.GetString(b[name])}");

        Dictionary<string, byte[]> Files(string root) => Directory.EnumerateFiles(root, "*", SearchOption.AllDirectories)
            .ToDictionary(f => Path.GetRelativePath(root, f).Replace('\\', '/'), f => Normalize(f));
    }

    /// <summary>Three saves of the same data into this harness's folder; a copy of the folder after them.</summary>
    private string Folder(bool onPool, InstanceHarness? h = null)
    {
        h ??= _h;
        var threads = WatchThreads();
        for (var i = 0; i < 5; i++) Store.GetBundleRaw($"world/mod-{i}", $"mod-{i}").Keys.Clear();  // what an earlier run left
        Bundles(1);
        Save(1);
        Store.GetBundleRaw("world/mod-0", "mod-0").Keys["state"].Json = "{\"v\":100}";
        Save(2);
        Store.GetBundleRaw("world/mod-3", "mod-3").Keys["extra"] = new WorldStore.Slot { Json = "{\"list\":[1,2,3]}" };
        Save(3, SaveReason.Request);
        Written();
        lock (threads) Assert.Equal(onPool, threads.All(t => TestPool.Pool.IsWorker(t.Thread)));
        var copy = Path.Combine(AppContext.BaseDirectory, "instance-tests", "same-" + Guid.NewGuid().ToString("N"));
        foreach (var f in Directory.EnumerateFiles(h.Root, "*", SearchOption.AllDirectories))
        {
            var to = Path.Combine(copy, Path.GetRelativePath(h.Root, f));
            Directory.CreateDirectory(Path.GetDirectoryName(to)!);
            File.Copy(f, to);
        }
        return copy;
    }

    /// <summary>A file's bytes; a manifest's with its creation time taken out (the only field that names the clock).</summary>
    private static byte[] Normalize(string file)
    {
        var bytes = File.ReadAllBytes(file);
        if (!file.Contains($"{Path.DirectorySeparatorChar}manifests{Path.DirectorySeparatorChar}")) return bytes;
        var doc = JsonNode.Parse(bytes)!.AsObject();
        doc.Remove("createdUnixMs");
        return System.Text.Encoding.UTF8.GetBytes(doc.ToJsonString());
    }

    private sealed class LogCapture : IKilnBackend
    {
        public readonly List<(LogLevel Level, string Text)> Lines = [];
        public string Version => "test";
        public bool IsTickThread => true;
        public bool HasFeature(string name) => false;
        public nint Service(string name, uint minVersion) => 0;

        public void Log(ulong owner, LogLevel level, string text)
        {
            lock (Lines) Lines.Add((level, text));
        }

        public (LogLevel Level, string Text) Single(string start)
        {
            lock (Lines) return Assert.Single(Lines, l => l.Text.StartsWith(start, StringComparison.Ordinal));
        }
    }
}
