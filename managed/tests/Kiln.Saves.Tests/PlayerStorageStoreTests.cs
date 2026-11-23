// SPDX-License-Identifier: MIT
using System.Text.Json.Nodes;
using Xunit;

namespace Kiln;

[Collection("instance data")]
public sealed class PlayerStorageStoreTests
{
    private sealed class Persist : IPlayerStoragePersistence
    {
        public JsonObject? Restored, Parked;
        public StorageFreeze? Frozen = new("saved", PlayerStorageDocumentTests.Storages(1));
        public Dictionary<string, int> Definitions = new() { ["mod:bag"] = 8 };
        public IReadOnlyDictionary<string, int> Registrations => Definitions;
        public StorageFreeze? Freeze(EntityId player, uint generation) => Frozen;
        public string? WitnessOf(EntityId player) => "join";
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
            h.Restart(worldRevision, new FakeInstanceWorld(), unix: worldRevision == 0 ? 0 : 1001, tick: false);
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
            Assert.True(File.Exists(WorldStore.PendingPath(h.Root, 1)));
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
