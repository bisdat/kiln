// SPDX-License-Identifier: MIT
// Player storage whose world write completes before Kiln.Saves' writer has its staging on disk (0.2.0-dev.31.5): a slow
// writer is a delay, not an error. kiln.log says it as a warning once per world session and sums it up, the capability
// does not report a fault for it, durability is exactly as before (pairs kept in memory, the next save retries), and a
// staging that cannot be written, or one stuck for two minutes, is still an error.
using Xunit;

namespace Kiln;

public sealed partial class PlayerStorageStoreTests
{
    private sealed class Tap : IKilnBackend
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

        public List<(LogLevel Level, string Text)> With(string part)
        {
            lock (Lines) return Lines.Where(l => l.Text.Contains(part, StringComparison.Ordinal)).ToList();
        }

        public List<string> Errors()
        {
            lock (Lines) return Lines.Where(l => l.Level == LogLevel.Error).Select(l => l.Text).ToList();
        }
    }

    /// <summary>The writer's next file creation blocks until <paramref name="release"/> (a staging that takes seconds).</summary>
    private static void HoldTheWriter(ManualResetEventSlim reached, ManualResetEventSlim release)
    {
        var tick = Environment.CurrentManagedThreadId;
        WorldStore.IoHook = op =>
        {
            if (op == "create" && Environment.CurrentManagedThreadId != tick)
            {
                reached.Set();
                Assert.True(release.Wait(TimeSpan.FromSeconds(20)));
            }
        };
    }

    /// <summary>A save whose stagings the writer has on disk before the world write (with the barrier, the writer runs off
    /// this thread: InstanceHarness.Save would race it).</summary>
    private static void SaveInTime(InstanceHarness h, uint revision)
    {
        h.StageOnly(revision);
        InstanceHarness.Written();
        InstanceHarness.Store.Commit(revision, 1000 + revision, SaveReason.Autosave);
        InstanceHarness.Written();
    }

    [Fact]
    public void A_world_write_ahead_of_a_slow_writer_is_a_delay_warned_once_summed_up_and_its_catch_up_said()
    {
        using var h = new InstanceHarness();
        using var native = new FakeSavesTable();
        using var reached = new ManualResetEventSlim();
        using var release = new ManualResetEventSlim();
        var store = InstanceHarness.Store.Storage;
        var persist = new Persist();
        store.Native = persist;
        var oldHook = WorldStore.IoHook;
        var backend = Backend.Current;
        var tap = new Tap();
        var oldLag = store.Lag;
        var now = TimeSpan.FromHours(1);
        store.Lag = new DurabilityLag(() => now);
        try
        {
            Backend.Current = tap;
            var character = new CharacterKey(1, PlayerStorageDocumentTests.Character);
            store.Join(character, new EntityId(1), "join", PlayerStorageDocumentTests.Doc("join", 1).Write(8), true);
            SaveInTime(h, 1);
            Assert.NotNull(store.Carried(character));
            var carried = store.Carried(character);

            // Save 2: the writer takes longer than the world write's wait (the player's 20 s renames).
            HoldTheWriter(reached, release);
            persist.Frozen = new("second", PlayerStorageDocumentTests.Storages(2));
            h.StageOnly(2);
            Assert.True(reached.Wait(TimeSpan.FromSeconds(10)));
            Assert.Throws<TimeoutException>(() => FakeSavesTable.WaitLikeTheWorldWrite(TimeSpan.FromMilliseconds(25)));
            InstanceHarness.Store.Commit(2, 1002, SaveReason.Autosave);    // the world write went ahead and completed
            Assert.Null(store.Failure);                                     // a delay: no fault, the capability stays available
            Assert.Equal(carried, store.Carried(character));                // durability as before: nothing undurable is carried
            var warned = Assert.Single(tap.With("player-storage: world generation 2 was written before its storage reached the disk"));
            Assert.Equal(LogLevel.Warn, warned.Level);

            // Save 3, 30 s later, while the writer is still on save 2: counted, not logged again.
            now += TimeSpan.FromSeconds(30);
            persist.Frozen = new("third", PlayerStorageDocumentTests.Storages(3));
            h.StageOnly(3);
            InstanceHarness.Store.Commit(3, 1003, SaveReason.Autosave);
            Assert.Null(store.Failure);
            Assert.Single(tap.With("was written before its storage reached the disk"));

            // The writer catches up 10 s later: the first catch-up is said, the second counted.
            now += TimeSpan.FromSeconds(10);
            release.Set();
            InstanceHarness.Written();
            store.PollWrites();
            var caught = Assert.Single(tap.With("'s storage reached the disk"));
            Assert.Equal(LogLevel.Info, caught.Level);
            Assert.StartsWith("player-storage: generation 2's storage reached the disk 40.0 s after its world write", caught.Text);
            Assert.Equal(carried, store.Carried(character));                // still: its carried copy waits for the next save

            // A summary 5 minutes after the last line, then nothing more to say.
            now += TimeSpan.FromMinutes(5);
            store.PollWrites();
            var summary = Assert.Single(tap.With("player-storage: in the last 5 min"));
            Assert.Equal(LogLevel.Warn, summary.Level);
            Assert.Contains("1 more world save was written before its storage reached the disk; 1 reached it later, at worst 10.0 s after its world write", summary.Text);
            store.PollWrites();
            Assert.Single(tap.With("player-storage: in the last"));

            // The next save whose staging is on disk in time carries its pair, as before.
            WorldStore.IoHook = oldHook;
            persist.Frozen = new("fourth", PlayerStorageDocumentTests.Storages(4));
            SaveInTime(h, 4);
            Assert.Null(store.Failure);
            Assert.Equal("fourth", PlayerStorageDocument.Read(store.Carried(character)!, character.Id).Data["current"]!["witness"]!.GetValue<string>());
            Assert.Empty(tap.Errors());                                     // nothing of it counts as an error
        }
        finally
        {
            release.Set();
            WorldStore.IoHook = oldHook;
            InstanceHarness.Written();
            Backend.Current = backend;
            store.Lag = oldLag;
            store.Native = null;
        }
    }

    [Fact]
    public void A_staging_still_not_on_disk_two_minutes_after_its_world_write_is_an_error()
    {
        using var h = new InstanceHarness();
        using var native = new FakeSavesTable();
        using var reached = new ManualResetEventSlim();
        using var release = new ManualResetEventSlim();
        var store = InstanceHarness.Store.Storage;
        var persist = new Persist();
        store.Native = persist;
        var oldHook = WorldStore.IoHook;
        var backend = Backend.Current;
        var tap = new Tap();
        var oldLag = store.Lag;
        var now = TimeSpan.FromHours(1);
        store.Lag = new DurabilityLag(() => now);
        try
        {
            Backend.Current = tap;
            var character = new CharacterKey(1, PlayerStorageDocumentTests.Character);
            store.Join(character, new EntityId(1), "join", PlayerStorageDocumentTests.Doc("join", 1).Write(8), true);
            HoldTheWriter(reached, release);
            h.StageOnly(1);
            Assert.True(reached.Wait(TimeSpan.FromSeconds(10)));
            InstanceHarness.Store.Commit(1, 1001, SaveReason.Autosave);
            Assert.Null(store.Failure);
            now += TimeSpan.FromSeconds(119);
            store.PollWrites();
            Assert.Empty(tap.Errors());
            now += TimeSpan.FromSeconds(2);
            store.PollWrites();                                             // a disk that does not answer: now it is an error
            var error = Assert.Single(tap.Errors());
            Assert.StartsWith("player-storage: persistence degraded: world generation 1 was written 121 s ago and Kiln.Saves' writer has " +
                              "still not put its storage on disk", error);
            Assert.NotNull(store.Failure);
            Assert.Null(store.Carried(character));
            store.PollWrites();
            Assert.Single(tap.Errors());                                    // once
            now += TimeSpan.FromSeconds(9);
            release.Set();
            InstanceHarness.Written();
            store.PollWrites();
            Assert.Single(tap.With("(reported above as not on disk)"));
            WorldStore.IoHook = oldHook;
            SaveInTime(h, 2);
            Assert.Null(store.Failure);                                     // the next durable save clears it, as before
            Assert.NotNull(store.Carried(character));
        }
        finally
        {
            release.Set();
            WorldStore.IoHook = oldHook;
            InstanceHarness.Written();
            Backend.Current = backend;
            store.Lag = oldLag;
            store.Native = null;
        }
    }

    [Fact]
    public void A_staging_that_cannot_be_written_stays_an_error_with_the_barrier()
    {
        using var h = new InstanceHarness();
        using var native = new FakeSavesTable();
        var store = InstanceHarness.Store.Storage;
        store.Native = new Persist();
        var oldHook = WorldStore.IoHook;
        var backend = Backend.Current;
        var tap = new Tap();
        try
        {
            Backend.Current = tap;
            var character = new CharacterKey(1, PlayerStorageDocumentTests.Character);
            store.Join(character, new EntityId(1), "join", PlayerStorageDocumentTests.Doc("join", 1).Write(8), true);
            var tick = Environment.CurrentManagedThreadId;
            WorldStore.IoHook = op => { if (op == "create" && Environment.CurrentManagedThreadId != tick) throw new IOException("injected storage disk failure"); };
            h.StageOnly(1);
            FakeSavesTable.WaitLikeTheWorldWrite(TimeSpan.FromSeconds(20));  // the writer gave up after its tries
            InstanceHarness.Store.Commit(1, 1001, SaveReason.Autosave);
            var errors = tap.Errors();
            Assert.Contains(errors, e => e.StartsWith("player-storage: persistence degraded: pending generation 1 could not be written", StringComparison.Ordinal));
            Assert.Contains(errors, e => e.StartsWith("player-storage: persistence degraded: world generation 1 completed before storage became durable (its staging could not be written)", StringComparison.Ordinal));
            Assert.Empty(tap.With("a delay, not an error"));
            Assert.NotNull(store.Failure);
            Assert.Null(store.Carried(character));
        }
        finally
        {
            WorldStore.IoHook = oldHook;
            InstanceHarness.Written();
            Backend.Current = backend;
            store.Native = null;
        }
    }
}
