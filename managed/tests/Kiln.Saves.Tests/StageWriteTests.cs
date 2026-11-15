// SPDX-License-Identifier: MIT
// Issue #39: on a hard drive every forced write of the save cycle held the server for up to 4 s. A staging whose manifest is
// the one already in its file writes nothing, and the save request's manifest of a generation with the previous one's
// bundles is not forced to disk. The player's case: 5 bundles, nothing changed, two stagings per autosave (the save
// request's, then the snapshot staging at the next Post): one manifest written and not forced, no file inside Post, and a
// crash at any point still finds the generation that matches the world file.
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
    public void AnAutosaveWithNothingChangedWritesOneManifestNotForcedToDiskAndNoFileInsidePost()
    {
        FiveBundles(1);
        var (writes, forced) = (Writes, Forced);
        Dispatch(SavesNative.EvSaving, 101, 1);
        Dispatch(SavesNative.EvSnapshot, 102, 1, saveSeq: 101);
        Dispatch(SavesNative.EvWritten, 103, 1, saveSeq: 101, unix: 1001);   // generation 1: new data, every file forced
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
            Assert.Equal((2L, 1L), (Writes - writes, Forced - forced));        // the confirmation after the world write is forced
            Assert.False(File.Exists(Pending(gen)));
        }
        (writes, forced) = (Writes, Forced);
        Store.GetBundleRaw("world/mod-3", "mod-3").Keys["state"].Json = "{\"v\":40}";   // a changed save forces as before
        Dispatch(SavesNative.EvSaving, 300, 5);
        Assert.Equal((2L, 2L), (Writes - writes, Forced - forced));            // its new blob and its manifest
        Dispatch(SavesNative.EvSnapshot, 301, 5, saveSeq: 300);
        Dispatch(SavesNative.EvWritten, 302, 5, saveSeq: 300, unix: 1005);
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

    [Fact]
    public void ASlowStagingOrCommitLogsAtInfoWithItsForcedWritesTimedApart()
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
            var staged = log.Single("Kiln.Saves: staged mod data generation 1 ");
            Assert.Equal(LogLevel.Debug, staged.Level);
            Assert.Contains("5 bundles, 5 new blobs", staged.Text);
            Assert.Matches(@" in \d+\.\d ms \(6 files written: \d+\.\d ms forcing to disk, \d+\.\d ms renaming\)$", staged.Text);
            var confirmed = log.Single("Kiln.Saves: confirmed mod data generation 1 ");
            Assert.Equal(LogLevel.Debug, confirmed.Level);
            Assert.Matches(@"^Kiln\.Saves: confirmed mod data generation 1 \(autosave\) in \d+\.\d ms \(1 file written: \d+\.\d ms forcing to disk, \d+\.\d ms renaming; pruning \d+\.\d ms\)$", confirmed.Text);

            WorldStore.SlowMs = 0;                                               // every staging and commit is slow now
            Store.GetBundleRaw("world/mod-0", "mod-0").Keys["state"].Json = "{\"v\":7}";
            Store.Stage(2, SaveReason.Autosave, late: false);
            Store.Stage(2, SaveReason.Autosave, late: false, snapshot: true);
            Store.Commit(2, 1002, SaveReason.Autosave);
            staged = log.Single("Kiln.Saves: staged mod data generation 2 (autosave)");
            Assert.Equal(LogLevel.Info, staged.Level);
            Assert.Contains("5 bundles, 1 new blobs", staged.Text);
            Assert.Contains("(2 files written: ", staged.Text);
            var snapshot = log.Single("Kiln.Saves: staged mod data generation 2 (autosave, snapshot staging)");
            Assert.Equal(LogLevel.Info, snapshot.Level);
            Assert.Contains("manifest unchanged: nothing written", snapshot.Text);
            Assert.EndsWith("(no file written)", snapshot.Text);
            confirmed = log.Single("Kiln.Saves: confirmed mod data generation 2 ");
            Assert.Equal(LogLevel.Info, confirmed.Level);
            Assert.Contains("(1 file written: ", confirmed.Text);
            Assert.DoesNotContain("pruning", confirmed.Text);                    // the store prunes at commits 1, 11, 21, ...
        }
        finally
        {
            Backend.Current = backend;
            WorldStore.SlowMs = slow;
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
