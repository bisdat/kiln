// SPDX-License-Identifier: MIT
// Issue #39: on a hard drive every forced write of the save cycle held the server for up to 4 s. A staging whose manifest is
// the one already in its file writes nothing, and a generation with the previous one's bundles writes its pending manifest
// and its confirmation without forcing them to disk. The player's case: 5 bundles, nothing changed, two stagings per
// autosave (the save request's, then the snapshot staging at the next Post): no forced write, no file inside Post, and a
// crash at any point still finds the generation that matches the world file (a power loss: one with the same bundles).
using System.Reflection;
using Xunit;
using static Kiln.InstanceHarness;

namespace Kiln;

[Collection("instance data")]
public sealed class StageWriteTests : IDisposable
{
    private readonly InstanceHarness _h = new();

    public void Dispose() => _h.Dispose();

    private static long Writes => Interlocked.Read(ref WorldStore.FilesWritten);
    private static long Forced => Interlocked.Read(ref WorldStore.FilesForced);

    private string Pending(uint revision) => Path.Combine(_h.Root, "manifests", $"{revision:D10}.pending.json");

    /// <summary>Five mods' world data, as in the player's session (no item data).</summary>
    private static void FiveBundles(int value)
    {
        for (var i = 0; i < 5; i++)
            Store.GetBundleRaw($"world/mod-{i}", $"mod-{i}").Keys["state"] = new WorldStore.Slot { Json = $"{{\"v\":{value + i}}}" };
    }

    private static int Value(int mod) =>
        int.Parse(Store.FindBundle($"world/mod-{mod}")!.Keys["state"].Json!.Split(':')[1].TrimEnd('}'));

    /// <summary>Kiln.Saves' own event handling (Runtime.cs Dispatch), as the native service delivers a save.</summary>
    private static void Dispatch(uint kind, ulong seq, uint revision, ulong saveSeq = 0, uint unix = 0) =>
        typeof(SavesRuntime).GetMethod("Dispatch", BindingFlags.Static | BindingFlags.NonPublic)!.Invoke(null,
            [new SavesNative.Event { Kind = kind, Seq = seq, Revision = revision, SaveSeq = saveSeq, WorldUnix = unix, Reason = (uint)SaveReason.Autosave }]);

    [Fact]
    public void AnAutosaveWithNothingChangedForcesNoWriteAndWritesNoFileInsidePost()
    {
        FiveBundles(1);
        var (writes, forced) = (Writes, Forced);
        Dispatch(SavesNative.EvSaving, 101, 1);
        Dispatch(SavesNative.EvSnapshot, 102, 1, saveSeq: 101);
        Dispatch(SavesNative.EvWritten, 103, 1, saveSeq: 101, unix: 1001);   // generation 1: new data, every file forced
        Written();
        Assert.Equal((7L, 7L), (Writes - writes, Forced - forced));            // 5 blobs, the pending and the confirmed manifest
        for (uint gen = 2, seq = 200; gen <= 4; gen++, seq += 10)
        {
            (writes, forced) = (Writes, Forced);
            Dispatch(SavesNative.EvSaving, seq, gen);                          // the save request: handlers, first staging
            Assert.Equal((1L, 0L), (Writes - writes, Forced - forced));        // 0 new blobs; the pending manifest, not forced
            var bytes = File.ReadAllBytes(Pending(gen));
            Dispatch(SavesNative.EvSnapshot, seq + 1, gen, saveSeq: seq);      // the next Post: nothing changed since
            Assert.Equal((1L, 0L), (Writes - writes, Forced - forced));        // no file written inside Post
            Assert.Equal(bytes, File.ReadAllBytes(Pending(gen)));
            Assert.Equal(5, _h.Scopes(gen).Count(s => s.StartsWith("world/mod-", StringComparison.Ordinal)));
            Dispatch(SavesNative.EvWritten, seq + 2, gen, saveSeq: seq, unix: 1000 + gen);
            Written();
            Assert.Equal((2L, 0L), (Writes - writes, Forced - forced));        // the confirmation after the world write, not forced
            Assert.False(File.Exists(Pending(gen)));
        }
        (writes, forced) = (Writes, Forced);
        Store.GetBundleRaw("world/mod-3", "mod-3").Keys["state"].Json = "{\"v\":40}";   // a changed save forces as before
        Dispatch(SavesNative.EvSaving, 300, 5);
        Assert.Equal((2L, 2L), (Writes - writes, Forced - forced));            // its new blob and its manifest
        Dispatch(SavesNative.EvSnapshot, 301, 5, saveSeq: 300);
        Dispatch(SavesNative.EvWritten, 302, 5, saveSeq: 300, unix: 1005);
        Written();
        Assert.Equal((3L, 3L), (Writes - writes, Forced - forced));
        Assert.Empty(Directory.EnumerateFiles(_h.Root, "*.tmp-*", SearchOption.AllDirectories));
    }

    [Fact]
    public void ASnapshotStagingThatFindsAChangeForcesItsWriteAfterAnUnforcedRequestStaging()
    {
        FiveBundles(1);
        _h.Save(1);
        var (writes, forced) = (Writes, Forced);
        Store.Stage(2, SaveReason.Autosave, late: false);
        Assert.Equal((1L, 0L), (Writes - writes, Forced - forced));
        Store.GetBundleRaw("world/mod-1", "mod-1").Keys["state"].Json = "{\"v\":50}";   // the last simulation step changed it
        Store.Stage(2, SaveReason.Autosave, late: false, snapshot: true);
        Assert.Equal((3L, 2L), (Writes - writes, Forced - forced));            // its new blob and the manifest, both forced
        Assert.Equal(50, _h.Bundle(2, "world/mod-1")!["keys"]!["state"]!["v"]!.GetValue<int>());
    }

    [Fact]
    public void TheFirstSaveOfASessionForcesItsManifestEvenWhenNothingChanged()
    {
        FiveBundles(1);
        _h.Save(1);
        Assert.Equal(DataMatch.Exact, _h.Restart(1, new FakeInstanceWorld()));   // a new process: nothing confirmed by it yet
        var forced = Forced;
        Store.Stage(2, SaveReason.Autosave, late: false);
        Assert.Equal(1L, Forced - forced);
        Store.Commit(2, 1002, SaveReason.Autosave);
        Written();
        Assert.Equal(2L, Forced - forced);                                     // and so is its confirmation
        forced = Forced;
        Store.Stage(3, SaveReason.Autosave, late: false);                      // the next one has generation 2 to rely on
        Assert.Equal(0L, Forced - forced);
        Store.Stage(5, SaveReason.Autosave, late: false);                      // not the generation right after the confirmed one
        Assert.Equal(1L, Forced - forced);
    }

    [Fact]
    public void APowerLossThatTearsAnUnforcedManifestLoadsThePreviousGenerationWithTheSameData()
    {
        FiveBundles(3);
        _h.Save(1);
        Store.Stage(2, SaveReason.Autosave, late: false);                      // not forced: the bundles of generation 1
        File.WriteAllBytes(Pending(2), new byte[64]);                          // what the disk can hold after a power loss
        Assert.Equal(DataMatch.Older, _h.Restart(2, new FakeInstanceWorld(), unix: 9999));   // world file 2 on disk
        Assert.Equal(1u, Store.Generation);
        Assert.Equal(3, Value(0));
        Assert.Equal(7, Value(4));
        File.Delete(Pending(2));                                               // or the rename was lost: the same result
        Assert.Equal(DataMatch.Older, _h.Restart(2, new FakeInstanceWorld(), unix: 9999));
        Assert.Equal(3, Value(0));
    }

    [Fact]
    public void APowerLossAfterUnforcedConfirmationsFallsBackToTheNewestForcedGenerationWhichPruningKeeps()
    {
        FiveBundles(5);
        _h.Save(1);                                                            // forced: the base of what follows
        for (uint gen = 2; gen <= 41; gen++) _h.Save(gen);                     // unchanged: nothing forced; pruned at 11, 21, 31, 41
        var manifests = Path.Combine(_h.Root, "manifests");
        Assert.True(File.Exists(Path.Combine(manifests, "0000000001-1001.json")));   // kept beyond the newest 30
        Assert.False(File.Exists(Path.Combine(manifests, "0000000005-1005.json")));  // an unforced one beyond them is not
        foreach (var f in Directory.EnumerateFiles(manifests).Where(f => !f.EndsWith("0000000001-1001.json", StringComparison.Ordinal)))
            File.WriteAllBytes(f, new byte[32]);                               // the system wrote none of the unforced ones
        Assert.Equal(DataMatch.Older, _h.Restart(41, new FakeInstanceWorld()));
        Assert.Equal(1u, Store.Generation);
        Assert.Equal(5, Value(0));
        Assert.Equal(9, Value(4));
    }

    [Fact]
    public void TwoIdenticalStagingsWriteOnceAndChangedDataIsWrittenAgain()
    {
        FiveBundles(1);
        var before = Writes;
        Store.Stage(1, SaveReason.Autosave, late: false);
        Assert.Equal(6, Writes - before);                                      // 5 blobs and the manifest
        Store.Stage(1, SaveReason.Autosave, late: false, snapshot: true);
        Assert.Equal(6, Writes - before);
        Store.GetBundleRaw("world/mod-2", "mod-2").Keys["state"].Json = "{\"v\":100}";  // the last simulation step changed it
        Store.Stage(1, SaveReason.Autosave, late: false, snapshot: true);
        Assert.Equal(8, Writes - before);                                      // its new blob and the manifest again
        Assert.Equal(100, _h.Bundle(1, "world/mod-2")!["keys"]!["state"]!["v"]!.GetValue<int>());
        Store.GetBundleRaw("world/mod-2", "mod-2").Keys["state"].Json = "{\"v\":3}";    // back to a blob already on disk
        Store.Stage(1, SaveReason.Autosave, late: false, snapshot: true);
        Assert.Equal(9, Writes - before);                                      // no blob, but the manifest names it again
        Assert.Equal(3, _h.Bundle(1, "world/mod-2")!["keys"]!["state"]!["v"]!.GetValue<int>());
    }

    [Fact]
    public void AManifestThatIsNoLongerOnDiskIsWrittenAgainEvenWhenNothingChanged()
    {
        FiveBundles(1);
        Store.Stage(1, SaveReason.Autosave, late: false);
        File.Delete(Pending(1));
        var before = Writes;
        Store.Stage(1, SaveReason.Autosave, late: false, snapshot: true);
        Assert.Equal(1, Writes - before);
        Assert.True(File.Exists(Pending(1)));
    }

    [Fact]
    public void EveryGenerationStillHasItsOwnManifestBeforeTheWorldWrite()
    {
        FiveBundles(1);
        _h.Save(1);
        var before = Writes;
        Store.Stage(2, SaveReason.Autosave, late: false);                     // same data as generation 1
        Assert.Equal(1, Writes - before);
        Assert.True(File.Exists(Pending(2)));                                   // a crash after the world write finds it
        Assert.Equal(DataMatch.Staged, _h.Restart(2, new FakeInstanceWorld(), unix: 9999));
        Assert.Equal(1, Value(0));
    }

    private sealed class Capture : IKilnBackend
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

    [Fact]
    public void TheTimingLevelIsInfoAbove250Ms()
    {
        Assert.Equal(250, WorldStore.SlowMs);
        Assert.Equal(LogLevel.Debug, WorldStore.TimingLevel(250));
        Assert.Equal(LogLevel.Info, WorldStore.TimingLevel(250.1));
    }

    // "writer 12.3 ms: <items>" and "pruning 4.5 ms: <items>": every number of the items, the "else" included, adds up to
    // that part's time (to the rounding of each item), so a line from a slow disk says where all of its time went.
    private static readonly System.Text.RegularExpressions.Regex Part = new(@"(writer|files|pruning) (\d+\.\d) ms(?: \([^)]*\))?: (.*?)(?=; pruning |\)$)");

    private static double Ms(string text) => double.Parse(text, System.Globalization.CultureInfo.InvariantCulture);

    private static void ItemsAddUp(string line)
    {
        var parts = Part.Matches(line);
        Assert.True(parts.Count >= 1, line);
        foreach (System.Text.RegularExpressions.Match m in parts)
        {
            var items = System.Text.RegularExpressions.Regex.Matches(m.Groups[3].Value, @"(\d+\.\d) ms").Select(x => Ms(x.Groups[1].Value)).ToList();
            Assert.True(items.Count >= 2, line);
            var whole = Ms(m.Groups[2].Value);
            Assert.True(Math.Abs(items.Sum() - whole) <= 0.05 * items.Count + 0.1, $"{m.Groups[1].Value}: items {items.Sum():F2} ms of {whole} ms in: {line}");
        }
    }

    [Fact]
    public void ASlowStagingOrCommitLogsAtInfoWithEveryPartTimedAndTheTickThreadApart()
    {
        var backend = Backend.Current;
        var slow = WorldStore.SlowMs;
        var log = new Capture();
        try
        {
            Backend.Current = log;
            WorldStore.SlowMs = double.MaxValue;                                 // fast: Debug, as before
            FiveBundles(1);
            Store.Stage(1, SaveReason.Autosave, late: false);
            Store.Commit(1, 1001, SaveReason.Autosave);
            Written();
            var staged = log.Single("Kiln.Saves: staged mod data generation 1 ");
            Assert.Equal(LogLevel.Debug, staged.Level);
            Assert.Contains("5 bundles, 5 new blobs", staged.Text);
            // no stage barrier here (no native table): the tick thread wrote its files itself, as before, and says so
            Assert.Matches(@" in \d+\.\d ms \(tick thread \d+\.\d ms, all of it \(this kiln\.saves does not hold the world write, so the tick " +
                           @"thread writes the files itself, as before 0\.2\.0-dev\.29\.3; \d+\.\d ms waiting for the writer\); files \d+\.\d ms: 6 files " +
                           @"written: \d+\.\d ms creating, \d+\.\d ms writing, " +
                           @"\d+\.\d ms forcing to disk, \d+\.\d ms closing, \d+\.\d ms renaming; 5 checked in \d+\.\d ms; 7 folders made in \d+\.\d ms; \d+\.\d ms hashing and JSON; " +
                           @"\d+\.\d ms else\)$", staged.Text);
            ItemsAddUp(staged.Text);
            var confirmed = log.Single("Kiln.Saves: confirmed mod data generation 1 ");
            Assert.Equal(LogLevel.Debug, confirmed.Level);
            Assert.Matches(@"^Kiln\.Saves: confirmed mod data generation 1 \(autosave\) in \d+\.\d ms \(tick thread \d+\.\d ms, all of it \([^)]*\); " +
                           @"files \d+\.\d ms: 1 file written: \d+\.\d ms creating, \d+\.\d ms writing, \d+\.\d ms forcing to disk, \d+\.\d ms closing, " +
                           @"\d+\.\d ms renaming; 1 deleted in \d+\.\d ms; \d+\.\d ms hashing and JSON; \d+\.\d ms else; pruning \d+\.\d ms: no file written; " +
                           @"1 read in \d+\.\d ms; 3 folders listed in \d+\.\d ms; \d+\.\d ms hashing and JSON; \d+\.\d ms else\)$", confirmed.Text);
            ItemsAddUp(confirmed.Text);

            WorldStore.SlowMs = 0;                                               // every staging and commit is slow now
            Store.GetBundleRaw("world/mod-0", "mod-0").Keys["state"].Json = "{\"v\":7}";
            Store.Stage(2, SaveReason.Autosave, late: false);
            Store.Stage(2, SaveReason.Autosave, late: false, snapshot: true);
            Store.Commit(2, 1002, SaveReason.Autosave);
            Written();
            staged = log.Single("Kiln.Saves: staged mod data generation 2 (autosave)");
            Assert.Equal(LogLevel.Info, staged.Level);
            Assert.Contains("5 bundles, 1 new blobs", staged.Text);
            Assert.Contains("; files ", staged.Text);
            Assert.Contains(": 2 files written: ", staged.Text);
            ItemsAddUp(staged.Text);
            var snapshot = log.Single("Kiln.Saves: staged mod data generation 2 (autosave, snapshot staging)");
            Assert.Equal(LogLevel.Info, snapshot.Level);
            Assert.Contains("manifest unchanged: nothing written", snapshot.Text);
            Assert.Matches(@"files \d+\.\d ms: no file written; 6 checked in \d+\.\d ms; \d+\.\d ms hashing and JSON; \d+\.\d ms else\)$", snapshot.Text);
            ItemsAddUp(snapshot.Text);
            confirmed = log.Single("Kiln.Saves: confirmed mod data generation 2 ");
            Assert.Equal(LogLevel.Info, confirmed.Level);
            Assert.Contains(": 1 file written: ", confirmed.Text);
            Assert.DoesNotContain("pruning", confirmed.Text);                    // the store prunes at commits 1, 11, 21, ...
            ItemsAddUp(confirmed.Text);

            // The player's 4620.6 ms line: a generation with the bundles of the one before, not forced to disk. Now it says
            // what each part took: the tick thread's, the queue's, and every file operation on the writer.
            Store.Stage(3, SaveReason.Autosave, late: false);
            Store.Stage(3, SaveReason.Autosave, late: false, snapshot: true);
            Store.Commit(3, 1003, SaveReason.Autosave);
            Written();
            confirmed = log.Single("Kiln.Saves: confirmed mod data generation 3 ");
            Assert.Matches(@"^Kiln\.Saves: confirmed mod data generation 3 \(autosave\), bundles as in generation 2: not forced to disk in \d+\.\d ms " +
                           @"\(tick thread \d+\.\d ms, all of it \([^)]*\); files \d+\.\d ms: 1 file written \(1 not forced to disk\): \d+\.\d ms creating, " +
                           @"\d+\.\d ms writing, 0\.0 ms forcing to disk, \d+\.\d ms closing, \d+\.\d ms renaming; 1 checked in \d+\.\d ms; 1 deleted in " +
                           @"\d+\.\d ms; \d+\.\d ms hashing and JSON; \d+\.\d ms else\)$", confirmed.Text);
            ItemsAddUp(confirmed.Text);
        }
        finally
        {
            Backend.Current = backend;
            WorldStore.SlowMs = slow;
        }
    }

    // 0.2.0-dev.31.5: one player's writer lines said "9152.5 ms renaming" for 3 files and nothing more. A line whose slowest
    // single file operation took over a quarter second names it (which operation, which file, how long), so the next report
    // says which rename waits; outside the itemised part, so its items still add up.
    [Fact]
    public void AWriterLineNamesItsSlowestFileOperation()
    {
        var backend = Backend.Current;
        var oldHook = WorldStore.IoHook;
        var slowOp = DiskTime.SlowOpMs;
        var log = new Capture();
        try
        {
            Backend.Current = log;
            FiveBundles(1);
            DiskTime.SlowOpMs = 40;
            var renames = 0;
            WorldStore.IoHook = op => { if (op == "rename" && ++renames == 6) Thread.Sleep(60); };  // the 6th: the pending manifest
            Store.Stage(1, SaveReason.Autosave, late: false);
            WorldStore.IoHook = oldHook;
            Store.Commit(1, 1001, SaveReason.Autosave);
            Written();
            var staged = log.Single("Kiln.Saves: staged mod data generation 1 ");
            Assert.Matches(@"; \d+\.\d ms else; slowest: renaming 0000000001\.pending\.json, 0\.\d\d s\)$", staged.Text);
            var confirmed = log.Single("Kiln.Saves: confirmed mod data generation 1 ");
            Assert.DoesNotContain("slowest", confirmed.Text);                     // nothing over the threshold there
            var blob = WorldStore.BlobPath(_h.Root, new string('3', 64));
            var t = new DiskTime();
            t.Note("forcing to disk", blob, 1234.5);
            t.Note("renaming", Pending(2), 99);
            Assert.EndsWith("; slowest: forcing to disk blob 333333333333, 1.23 s", t.Text(2000));
        }
        finally
        {
            Backend.Current = backend;
            WorldStore.IoHook = oldHook;
            DiskTime.SlowOpMs = slowOp;
        }
    }

    [Fact]
    public void KillNineAfterTheWorldWriteOfAnUnchangedSaveLoadsItsStagedGeneration()
    {
        FiveBundles(1);
        _h.Save(1);
        FiveBundles(10);
        _h.Save(2);
        _h.StageOnly(3);                                                        // nothing changed: the snapshot staging wrote nothing
        Assert.Equal(DataMatch.Staged, _h.Restart(3, new FakeInstanceWorld(), unix: 9999));   // world file 3 on disk, never confirmed
        Assert.Equal(3u, Store.Generation);
        Assert.Equal(10, Value(0));
        Assert.Equal(14, Value(4));
        Assert.Equal(DataMatch.Exact, _h.Restart(2, new FakeInstanceWorld()));    // the world file of revision 2 restored
        Assert.Equal(10, Value(0));
        Assert.Equal(DataMatch.Exact, _h.Restart(1, new FakeInstanceWorld()));
        Assert.Equal(1, Value(0));
    }
}
