// SPDX-License-Identifier: MIT
// Issue #39: on a hard drive every forced write of the save cycle held the server for up to 4 s. A staging whose manifest is
// the one already in its file writes nothing. The player's case: 5 bundles, nothing changed, two stagings per autosave (the
// save request's, then the snapshot staging at the next Post): the snapshot staging writes no file, and a crash at any point
// still finds the generation that matches the world file.
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
    public void TheSnapshotStagingOfAnAutosaveWithNothingChangedWritesNoFile()
    {
        FiveBundles(1);
        Dispatch(SavesNative.EvSaving, 101, 1);
        Dispatch(SavesNative.EvSnapshot, 102, 1, saveSeq: 101);
        Dispatch(SavesNative.EvWritten, 103, 1, saveSeq: 101, unix: 1001);   // generation 1: its blobs and manifest are on disk
        for (uint gen = 2, seq = 200; gen <= 4; gen++, seq += 10)
        {
            var before = Writes;
            Dispatch(SavesNative.EvSaving, seq, gen);                          // the save request: handlers, first staging
            Assert.Equal(1, Writes - before);                                  // 0 new blobs, this generation's pending manifest
            var bytes = File.ReadAllBytes(Pending(gen));
            Dispatch(SavesNative.EvSnapshot, seq + 1, gen, saveSeq: seq);      // the next Post: nothing changed since
            Assert.Equal(1, Writes - before);                                  // no file written inside Post
            Assert.Equal(bytes, File.ReadAllBytes(Pending(gen)));
            Assert.Equal(5, _h.Scopes(gen).Count(s => s.StartsWith("world/mod-", StringComparison.Ordinal)));
            Dispatch(SavesNative.EvWritten, seq + 2, gen, saveSeq: seq, unix: 1000 + gen);
            Assert.False(File.Exists(Pending(gen)));
        }
        Assert.Empty(Directory.EnumerateFiles(_h.Root, "*.tmp-*", SearchOption.AllDirectories));
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
