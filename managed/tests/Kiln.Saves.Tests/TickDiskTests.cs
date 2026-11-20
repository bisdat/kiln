// SPDX-License-Identifier: MIT
// Issue #39, after dev.29: on a hard drive the save path's file work still held the server's tick for seconds. The player's
// dev.29 log: a confirmation in Pre of 3955.7 ms (pruning 3167.6 ms), one of 4620.6 ms with nothing forced to disk and
// nearly all of it not itemised, a save request's staging of 3425.4 ms. These tests slow every file operation of the save
// path down (WorldStore.IoHook) and check that the tick thread's calls (the game's save request, the snapshot staging in
// Post, the confirmation in Pre) touch no file and return at once, and that the files are still there, in the same order,
// for the game's world write and for the next start.
using System.Diagnostics;
using System.Reflection;
using Xunit;
using static Kiln.InstanceHarness;

namespace Kiln;

[Collection("instance data")]
public sealed class TickDiskTests : IDisposable
{
    private const int SlowMs = 400;                         // every file operation, on whatever thread does it
    private static readonly TimeSpan Bound = TimeSpan.FromMilliseconds(SlowMs / 2);   // a tick-thread call: under one of them
    private static readonly TimeSpan Writer = TimeSpan.FromSeconds(60);

    private readonly InstanceHarness _h = new();
    private readonly List<(string Op, int Thread)> _ops = [];
    private readonly Action<string>? _hook = WorldStore.IoHook;

    public void Dispose()
    {
        WorldStore.IoHook = _hook;
        _h.Dispose();
    }

    private string Pending(uint revision) => Path.Combine(_h.Root, "manifests", $"{revision:D10}.pending.json");
    private string Confirmed(uint revision, uint unix) => Path.Combine(_h.Root, "manifests", $"{revision:D10}-{unix}.json");

    private static void FiveBundles(int value)
    {
        for (var i = 0; i < 5; i++)
            Store.GetBundleRaw($"world/mod-{i}", $"mod-{i}").Keys["state"] = new WorldStore.Slot { Json = $"{{\"v\":{value + i}}}" };
    }

    private static int Value(int mod) =>
        int.Parse(Store.FindBundle($"world/mod-{mod}")!.Keys["state"].Json!.Split(':')[1].TrimEnd('}'));

    private static void Dispatch(uint kind, ulong seq, uint revision, ulong saveSeq = 0, uint unix = 0) =>
        typeof(SavesRuntime).GetMethod("Dispatch", BindingFlags.Static | BindingFlags.NonPublic)!.Invoke(null,
            [new SavesNative.Event { Kind = kind, Seq = seq, Revision = revision, SaveSeq = saveSeq, WorldUnix = unix, Reason = (uint)SaveReason.Autosave }]);

    /// <summary>Every file operation of the save path takes <see cref="SlowMs"/> (only those whose name passes
    /// <paramref name="which"/>), and is recorded with the thread that did it.</summary>
    private void SlowDisk(Func<string, bool>? which = null) =>
        WorldStore.IoHook = op =>
        {
            lock (_ops) _ops.Add((op, Environment.CurrentManagedThreadId));
            if (which == null || which(op)) Thread.Sleep(SlowMs);
        };

    private void FastDisk() => WorldStore.IoHook = _hook;

    /// <summary>One tick-thread call: it returns within <see cref="Bound"/> and does no file operation on this thread.</summary>
    private void OnTheTick(string what, Action call)
    {
        int before;
        lock (_ops) before = _ops.Count;
        var sw = Stopwatch.StartNew();
        call();
        var ms = sw.Elapsed.TotalMilliseconds;
        string[] here;
        lock (_ops) here = _ops.Skip(before).Where(o => o.Thread == Environment.CurrentManagedThreadId).Select(o => o.Op).ToArray();
        Assert.True(here.Length == 0, $"{what}: {here.Length} file operation(s) on the tick thread ({string.Join(", ", here)}), {ms:F0} ms");
        Assert.True(sw.Elapsed < Bound, $"{what} held the tick thread {ms:F0} ms (each file operation takes {SlowMs} ms)");
    }

    /// <summary>The first save of the session, at full speed: generation 1 confirmed, the code paths warm.</summary>
    private void FirstSave()
    {
        FiveBundles(1);
        Dispatch(SavesNative.EvSaving, 100, 1);
        Dispatch(SavesNative.EvSnapshot, 101, 1, saveSeq: 100);
        FakeSavesTable.WaitLikeTheWorldWrite(Writer);
        Dispatch(SavesNative.EvWritten, 102, 1, saveSeq: 100, unix: 1001);
        Written();
    }

    [Fact]
    public void AnAutosaveWithChangedDataTouchesNoFileOnTheTickThread()
    {
        using var native = new FakeSavesTable();
        FirstSave();
        Store.GetBundleRaw("world/mod-2", "mod-2").Keys["state"].Json = "{\"v\":20}";   // a new blob and a forced manifest
        SlowDisk();
        OnTheTick("the save request's staging", () => Dispatch(SavesNative.EvSaving, 200, 2));
        Store.GetBundleRaw("world/mod-3", "mod-3").Keys["state"].Json = "{\"v\":30}";   // the last simulation step changed it
        OnTheTick("the snapshot staging (Post)", () => Dispatch(SavesNative.EvSnapshot, 201, 2, saveSeq: 200));
        // The game's Save2Thread writes the world file only once the generation's pending manifest is on disk.
        FakeSavesTable.WaitLikeTheWorldWrite(Writer);
        Assert.True(File.Exists(Pending(2)));
        OnTheTick("the confirmation (Pre)", () => Dispatch(SavesNative.EvWritten, 202, 2, saveSeq: 200, unix: 1002));
        Written();                                                                       // the writer, on the slow disk
        FastDisk();
        Assert.Equal(DataMatch.Exact, _h.Restart(2, new FakeInstanceWorld()));   // a new process finds generation 2 confirmed
        Assert.Equal(20, Value(2));
        Assert.Equal(30, Value(3));
        Assert.True(File.Exists(Confirmed(2, 1002)));
        Assert.False(File.Exists(Pending(2)));
    }

    [Fact]
    public void AnUnchangedAutosaveTouchesNoFileOnTheTickThread()
    {
        // The player's 4620.6 ms: "bundles as in generation 1776: not forced to disk", 0.0 ms forcing, 0.4 ms renaming.
        using var native = new FakeSavesTable();
        FirstSave();
        SlowDisk(op => op != "flush");
        OnTheTick("the save request's staging", () => Dispatch(SavesNative.EvSaving, 200, 2));
        OnTheTick("the snapshot staging (Post)", () => Dispatch(SavesNative.EvSnapshot, 201, 2, saveSeq: 200));
        FakeSavesTable.WaitLikeTheWorldWrite(Writer);
        Assert.True(File.Exists(Pending(2)));
        OnTheTick("the confirmation (Pre)", () => Dispatch(SavesNative.EvWritten, 202, 2, saveSeq: 200, unix: 1002));
        Written();                                                                       // the writer, on the slow disk
        FastDisk();
        Assert.Equal(DataMatch.Exact, _h.Restart(2, new FakeInstanceWorld()));
        Assert.Equal(1, Value(0));
        Assert.Equal(5, Value(4));
    }

    [Fact]
    public void PruningAfterAConfirmationRunsOffTheTickThread()
    {
        // The player's 3955.7 ms confirmation, 3167.6 ms of it pruning (every manifest and world header read).
        using var native = new FakeSavesTable();
        FirstSave();
        typeof(WorldStore).GetField("_commits", BindingFlags.Instance | BindingFlags.NonPublic)!.SetValue(Store, 10);  // the 11th prunes
        Dispatch(SavesNative.EvSaving, 200, 2);
        Dispatch(SavesNative.EvSnapshot, 201, 2, saveSeq: 200);
        FakeSavesTable.WaitLikeTheWorldWrite(Writer);
        SlowDisk(op => op is "list" or "read" or "delete");
        OnTheTick("the confirmation that prunes (Pre)", () => Dispatch(SavesNative.EvWritten, 202, 2, saveSeq: 200, unix: 1002));
        Written();
        FastDisk();
        Assert.Equal(DataMatch.Exact, _h.Restart(2, new FakeInstanceWorld()));
        lock (_ops) Assert.Contains(_ops, o => o.Op == "list" && o.Thread != Environment.CurrentManagedThreadId);   // it did prune
    }
}
