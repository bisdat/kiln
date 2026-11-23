// SPDX-License-Identifier: MIT
using System.Text.Json.Nodes;
using Xunit;

namespace Kiln;

[Collection("instance data")]
public sealed partial class PlayerStorageStoreTests
{
    private sealed class Persist : IPlayerStoragePersistence
    {
        public JsonObject? Restored, Parked;
        public bool ExtraPages;
        public string JoinWitness = "join";
        public bool ExtraMainPages(EntityId player) => ExtraPages;
        public StorageFreeze? Frozen = new("saved", PlayerStorageDocumentTests.Storages(1));
        public Dictionary<string, int> Definitions = new() { ["mod:bag"] = 8 };
        public readonly List<StorageInstanceHolder> Parts = [];
        public IReadOnlyList<StorageInstanceHolder> InstanceHolders(EntityId player) => Parts;
        public IReadOnlyDictionary<string, int> Registrations => Definitions;
        public StorageFreeze? Freeze(CharacterKey character, EntityId player, uint generation) => Frozen;
        public string? WitnessOf(EntityId player) => JoinWitness;
        public void Restore(EntityId player, JsonObject storages, bool paired, string source, long revision) => Restored = (JsonObject)storages.DeepClone();
        public void Park(EntityId player, JsonObject parked) => Parked = (JsonObject)parked.DeepClone();
    }

    [Fact]
    public void Staged_generation_survives_failure_and_only_written_generation_is_carried()
    {
        using var h = new InstanceHarness();
        var persist = new Persist();
        var store = InstanceHarness.Store.Storage;
        store.Native = persist;
        try
        {
            var character = new CharacterKey(1, PlayerStorageDocumentTests.Character);
            store.Join(character, new EntityId(1), "join", PlayerStorageDocumentTests.Doc("join", 5).Write(8), true);
            h.StageOnly(1);
            InstanceHarness.Written();
            Assert.NotNull(h.Bundle(1, PlayerStorageStore.Prefix + character.Text));
            Assert.Null(store.Carried(character));
            // No world-written event: pending manifest stays, nothing is published.
            Assert.True(File.Exists(WorldStore.PendingPath(h.Root, 1)));
            InstanceHarness.Store.Commit(1, 1001, SaveReason.Request);
            InstanceHarness.Written();
            Assert.NotNull(store.Carried(character));
            Assert.Equal(DataMatch.Exact, InstanceHarness.Store.Match);
        }
        finally { store.Native = null; }
    }

    [Theory]
    [InlineData(0u)]
    [InlineData(1u)]
    public void Pending_pair_is_candidate_after_crash_before_or_after_world_write(uint worldRevision)
    {
        using var h = new InstanceHarness();
        var store = InstanceHarness.Store.Storage;
        store.Native = new Persist();
        try
        {
            var character = new CharacterKey(1, PlayerStorageDocumentTests.Character);
            store.Join(character, new EntityId(1), "join", PlayerStorageDocumentTests.Doc("join", 1).Write(8), true);
            h.StageOnly(1);
            InstanceHarness.Written();
            h.Restart(worldRevision, new FakeInstanceWorld(), unix: worldRevision == 0 ? 0u : 1001u, tick: false);
            var chosen = store.Join(character, new EntityId(1), "saved", null, true);
            Assert.NotNull(chosen);
            Assert.True(chosen.Paired);
            Assert.Equal("saved", chosen.Pair["witness"]!.GetValue<string>());
        }
        finally { store.Native = null; }
    }

    [Fact]
    public void Two_saves_first_failed_never_publish_the_wrong_capture()
    {
        using var h = new InstanceHarness();
        var persist = new Persist();
        var store = InstanceHarness.Store.Storage;
        store.Native = persist;
        try
        {
            var character = new CharacterKey(1, PlayerStorageDocumentTests.Character);
            store.Join(character, new EntityId(1), "join", PlayerStorageDocumentTests.Doc("join", 1).Write(8), true);
            h.StageOnly(1);
            Assert.Null(store.Carried(character));
            persist.Frozen = new("second", PlayerStorageDocumentTests.Storages(2));
            h.Save(2);
            Assert.Equal("second", JsonNode.Parse(store.Carried(character)!)!["current"]!["witness"]!.GetValue<string>());
            var document = PlayerStorageDocument.Read(store.Carried(character)!, character.Id);
            Assert.True(PlayerStorageDocument.Choose("saved", document, null)!.Paired);
        }
        finally { store.Native = null; }
    }

    [Fact]
    public void Last_committed_pair_stays_a_candidate_when_failed_stages_outlive_history_depth()
    {
        using var h = new InstanceHarness();
        var store = InstanceHarness.Store.Storage;
        var persist = new Persist();
        store.Native = persist;
        try
        {
            var character = new CharacterKey(1, PlayerStorageDocumentTests.Character);
            store.Join(character, new EntityId(1), "join", PlayerStorageDocumentTests.Doc("join", 1).Write(8), true);
            h.Save(1); // last published character and last committed world
            for (var i = 0; i < 34; i++)
            {
                persist.Frozen = new("failed-" + i, PlayerStorageDocumentTests.Storages(i % 8));
                h.StageOnly(2); // repeated failed writes keep the same target world revision
            }
            InstanceHarness.Written();
            var failedRevision = store.Read(character)!.Revision;
            var choice = store.Join(character, new EntityId(1), "saved", null, true)!;
            Assert.True(choice.Paired);
            Assert.Equal("saved", choice.Pair["witness"]!.GetValue<string>());
            persist.Frozen = new("recovered", PlayerStorageDocumentTests.Storages(4));
            h.Save(2);
            Assert.True(store.Read(character)!.Revision > failedRevision);
            var fields = System.Reflection.BindingFlags.NonPublic | System.Reflection.BindingFlags.Instance;
            var pinned = (System.Collections.IDictionary)typeof(PlayerStorageStore).GetField("_committedServer", fields)!.GetValue(store)!;
            var waiting = (System.Collections.IDictionary)typeof(PlayerStorageStore).GetField("_pendingServer", fields)!.GetValue(store)!;
            Assert.Equal(1, pinned.Count);
            Assert.Equal("recovered", JsonNode.Parse((string)pinned[character]!)!["current"]!["witness"]!.GetValue<string>());
            Assert.Equal(0, waiting.Count);
        }
        finally { store.Native = null; }
    }

    [Fact]
    public void Waiting_save_and_leave_cannot_turn_missing_capture_into_empty_storage()
    {
        using var h = new InstanceHarness();
        var persist = new Persist { Frozen = null };
        var store = InstanceHarness.Store.Storage;
        store.Native = persist;
        try
        {
            var character = new CharacterKey(1, PlayerStorageDocumentTests.Character);
            var old = PlayerStorageDocumentTests.Doc("old", 1).Write(32);
            InstanceHarness.Store.GetBundleRaw(PlayerStorageStore.Prefix + character.Text, "kiln-items").Keys["storage"] = new WorldStore.Slot { Json = old };
            h.StageOnly(1); // no offer/decision yet
            InstanceHarness.Store.Commit(1, 1001, SaveReason.PlayerLeft);
            InstanceHarness.Written();
            Assert.Equal(old, InstanceHarness.Store.FindBundle(PlayerStorageStore.Prefix + character.Text)!.Keys["storage"].Json);
            Assert.Null(store.Carried(character));
        }
        finally { store.Native = null; }
    }

    [Fact]
    public void Instance_bands_keep_registration_order_and_temporary_return_holders_distinct()
    {
        using var h = new InstanceHarness();
        var store = InstanceHarness.Store.Storage;
        var persist = new Persist();
        persist.Parts.Add(new("mod:bag", new EntityId(2000)));
        persist.Parts.Add(new("mod:bag", new EntityId(2001), true));
        store.Native = persist;
        try
        {
            var character = new CharacterKey(1, PlayerStorageDocumentTests.Character);
            store.Join(character, new EntityId(1), "join", PlayerStorageDocumentTests.Doc("join", 1).Write(8), true);
            var live = store.NormalizeInstance(new(new EntityId(2000), 3));
            var temporary = store.NormalizeInstance(new(new EntityId(2001), 3));
            Assert.Equal(1027, live.FlatSlot);
            Assert.Equal(1091, temporary.FlatSlot);
            Assert.Equal(3, store.InstanceParts(new EntityId(1)).Count);
            persist.Definitions["aaa:bag"] = 8;
            persist.Parts.Add(new("aaa:bag", new EntityId(2002)));
            h.Save(1);
            Assert.Equal(live, store.NormalizeInstance(new(new EntityId(2000), 3)));
            Assert.NotEqual(live.FlatSlot, store.NormalizeInstance(new(new EntityId(2002), 3)).FlatSlot);
            persist.Parts.Clear(); // delayed destruction notices still need the old logical owner and slot
            Assert.Equal(temporary, store.NormalizeInstance(new(new EntityId(2001), 3)));
            Assert.DoesNotContain("instanceBands", store.Carried(character)!);
        }
        finally { store.Native = null; }
    }

    [Fact]
    public void Larger_server_cap_can_keep_pending_pair_when_irreducible_sidecar_is_refused()
    {
        using var h = new InstanceHarness();
        var store = InstanceHarness.Store.Storage;
        var persist = new Persist();
        store.Native = persist;
        try
        {
            var character = new CharacterKey(1, PlayerStorageDocumentTests.Character);
            var old = PlayerStorageDocumentTests.Doc("join", 1);
            old.Data["current"]!["storages"]!["mod:bag"]!["opaque"] = new string('a', 36000);
            store.Join(character, new EntityId(1), "join", old.Write(8), true);
            var next = PlayerStorageDocumentTests.Storages(1);
            next["mod:bag"]!["opaque"] = new string('b', 36000);
            persist.Frozen = new("new", next);
            h.Save(1);
            Assert.Null(store.Carried(character));
            Assert.NotNull(store.Failure);
            var server = InstanceHarness.Store.FindBundle(PlayerStorageStore.Prefix + character.Text)!.Keys["storage"].Json!;
            Assert.True(System.Text.Encoding.UTF8.GetByteCount(server) > PlayerStorageDocument.MaxBytes);
            Assert.Equal("new", store.Read(character)!.Data["current"]!["witness"]!.GetValue<string>());
        }
        finally { store.Native = null; }
    }

    [Theory]
    [InlineData("join")]
    [InlineData("next")]
    public void Join_then_equal_capture_shares_contents_instead_of_duplicating_history(string capturedWitness)
    {
        using var h = new InstanceHarness();
        var store = InstanceHarness.Store.Storage;
        store.Native = new Persist { Frozen = new(capturedWitness, PlayerStorageDocumentTests.Storages()) };
        try
        {
            var character = new CharacterKey(1, PlayerStorageDocumentTests.Character);
            store.Join(character, new EntityId(1), "join", PlayerStorageDocumentTests.Doc("join", 1).Write(8), true);
            h.Save(1);
            var carried = PlayerStorageDocument.Read(store.Carried(character)!, character.Id);
            Assert.Empty(carried.Data["history"]!.AsArray());
            Assert.True(PlayerStorageDocument.Choose("join", carried, null)!.Paired);
        }
        finally { store.Native = null; }
    }

    [Fact]
    public void New_visit_never_receives_previous_visits_committed_update_before_its_own_save()
    {
        using var h = new InstanceHarness();
        var store = InstanceHarness.Store.Storage;
        store.Native = new Persist();
        try
        {
            var character = new CharacterKey(1, PlayerStorageDocumentTests.Character);
            store.Join(character, new EntityId(1), "join", PlayerStorageDocumentTests.Doc("join", 1).Write(8), true);
            h.Save(1);
            Assert.NotNull(store.Carried(character));
            store.Join(character, new EntityId(1), "travel", PlayerStorageDocumentTests.Doc("travel", 100, 7).Write(8), true);
            Assert.Null(store.Carried(character));
        }
        finally { store.Native = null; }
    }

    [Fact]
    public void Repeated_write_failure_is_degraded_bounded_and_next_save_retries_fresh_pair()
    {
        using var h = new InstanceHarness();
        var store = InstanceHarness.Store.Storage;
        var persist = new Persist();
        store.Native = persist;
        var oldHook = WorldStore.IoHook;
        try
        {
            var character = new CharacterKey(1, PlayerStorageDocumentTests.Character);
            store.Join(character, new EntityId(1), "join", PlayerStorageDocumentTests.Doc("join", 1).Write(8), true);
            WorldStore.IoHook = op => { if (op == "create") throw new IOException("injected storage disk failure"); };
            h.Save(1);
            h.Save(2);
            Assert.NotNull(store.Failure);
            Assert.Null(store.Carried(character));
            var pending = (System.Collections.IDictionary)typeof(PlayerStorageStore).GetField("_pending", System.Reflection.BindingFlags.NonPublic | System.Reflection.BindingFlags.Instance)!.GetValue(store)!;
            Assert.True(pending.Count <= 2);
            WorldStore.IoHook = oldHook;
            persist.Frozen = new("fresh", PlayerStorageDocumentTests.Storages(3));
            h.Save(3);
            Assert.Null(store.Failure);
            var saved = PlayerStorageDocument.Read(store.Carried(character)!, character.Id);
            Assert.Equal("fresh", saved.Data["current"]!["witness"]!.GetValue<string>());
        }
        finally { WorldStore.IoHook = oldHook; store.Native = null; }
    }

    [Fact]
    public void Captures_done_staging_rearms_barrier_and_timeout_never_publishes_unwritten_storage()
    {
        using var h = new InstanceHarness();
        using var native = new FakeSavesTable();
        using var reached = new ManualResetEventSlim();
        using var release = new ManualResetEventSlim();
        var store = InstanceHarness.Store.Storage;
        var persist = new Persist { Frozen = null };
        store.Native = persist;
        var oldHook = WorldStore.IoHook;
        try
        {
            var character = new CharacterKey(1, PlayerStorageDocumentTests.Character);
            store.Join(character, new EntityId(1), "join", PlayerStorageDocumentTests.Doc("join", 1).Write(8), true);
            h.StageOnly(1); // earlier snapshot has no current frozen capture
            InstanceHarness.Written();
            Assert.Equal(FakeSavesTable.Issued, FakeSavesTable.Done);
            var tick = Environment.CurrentManagedThreadId;
            WorldStore.IoHook = op =>
            {
                if (op == "create" && Environment.CurrentManagedThreadId != tick)
                { reached.Set(); Assert.True(release.Wait(TimeSpan.FromSeconds(20))); }
            };
            persist.Frozen = new("capture", PlayerStorageDocumentTests.Storages(1));
            InstanceHarness.Store.Stage(1, SaveReason.PlayerLeft, late: false, snapshot: true);
            Assert.True(reached.Wait(TimeSpan.FromSeconds(10)));
            Assert.True(FakeSavesTable.Issued > FakeSavesTable.Done);
            Assert.Throws<TimeoutException>(() => FakeSavesTable.WaitLikeTheWorldWrite(TimeSpan.FromMilliseconds(25)));
            // Existing bounded native wait is allowed to expire; world-written must not publish undurable storage.
            InstanceHarness.Store.Commit(1, 1001, SaveReason.PlayerLeft);
            Assert.NotNull(store.Failure);
            Assert.Null(store.Carried(character));
            release.Set();
            InstanceHarness.Written();
        }
        finally { release.Set(); WorldStore.IoHook = oldHook; store.Native = null; InstanceHarness.Written(); }
    }

    [Fact]
    public void Parked_keys_survive_save_and_late_registration_restores_overflow()
    {
        using var h = new InstanceHarness();
        var persist = new Persist { Frozen = new("saved", new()) };
        persist.Definitions.Clear();
        var store = InstanceHarness.Store.Storage;
        store.Native = persist;
        try
        {
            var character = new CharacterKey(1, PlayerStorageDocumentTests.Character);
            store.Join(character, new EntityId(1), "join", PlayerStorageDocumentTests.Doc("join", 5, 7).Write(8), true);
            Assert.Empty(persist.Restored!);
            var original = persist.Parked!["mod:bag"]!.ToJsonString();
            h.Save(1);
            var saved = JsonNode.Parse(store.Carried(character)!)!;
            Assert.Equal(original, saved["current"]!["parked"]!["mod:bag"]!.ToJsonString());
            persist.Definitions["mod:bag"] = 8;
            h.StageOnly(2);
            Assert.Single(persist.Restored!["mod:bag"]!["records"]!.AsArray());
        }
        finally { store.Native = null; }
    }

    [Theory]
    [InlineData("travel", 9)]
    [InlineData("returned", 10)]
    [InlineData("forged", 999999)]
    public void Untrusted_newer_client_can_withhold_but_cannot_add(string witness, long revision)
    {
        using var h = new InstanceHarness();
        var persist = new Persist();
        var store = InstanceHarness.Store.Storage;
        store.Native = persist;
        try
        {
            var character = new CharacterKey(1, PlayerStorageDocumentTests.Character);
            var old = PlayerStorageDocumentTests.Doc("old", 1).Write(32);
            InstanceHarness.Store.GetBundleRaw(PlayerStorageStore.Prefix + character.Text, "kiln-items").Keys["storage"] = new WorldStore.Slot { Json = old };
            store.Join(character, new EntityId(1), witness, PlayerStorageDocumentTests.Doc(witness, revision, 7).Write(8), false);
            Assert.True(persist.Restored is null || persist.Restored.Count == 0);
            Assert.Equal(0, persist.Parked!["mod:bag"]!["records"]![0]!["s"]!.GetValue<int>());
        }
        finally { store.Native = null; }
    }

    [Fact]
    public void No_client_copy_on_untrusted_server_restores_server_as_before()
    {
        using var h = new InstanceHarness();
        var persist = new Persist();
        var store = InstanceHarness.Store.Storage;
        store.Native = persist;
        try
        {
            var character = new CharacterKey(1, PlayerStorageDocumentTests.Character);
            InstanceHarness.Store.GetBundleRaw(PlayerStorageStore.Prefix + character.Text, "kiln-items").Keys["storage"] = new WorldStore.Slot { Json = PlayerStorageDocumentTests.Doc("old", 1).Write(32) };
            store.Join(character, new EntityId(1), "vanilla", null, false);
            Assert.Single(persist.Restored!);
        }
        finally { store.Native = null; }
    }

    [Fact]
    public void Future_client_blocks_all_restore_and_save_over_old_server()
    {
        using var h = new InstanceHarness();
        var persist = new Persist();
        var store = InstanceHarness.Store.Storage;
        store.Native = persist;
        try
        {
            var character = new CharacterKey(1, PlayerStorageDocumentTests.Character);
            var original = PlayerStorageDocumentTests.Doc("join", 1).Write(32);
            InstanceHarness.Store.GetBundleRaw(PlayerStorageStore.Prefix + character.Text, "kiln-items").Keys["storage"] = new WorldStore.Slot { Json = original };
            Assert.Null(store.Join(character, new EntityId(1), "join", "{\"v\":99}", true));
            Assert.Null(persist.Restored);
            h.Save(1);
            Assert.Equal(original, InstanceHarness.Store.FindBundle(PlayerStorageStore.Prefix + character.Text)!.Keys["storage"].Json);
            Assert.Null(store.Carried(character));
        }
        finally { store.Native = null; }
    }
}
