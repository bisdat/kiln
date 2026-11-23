// SPDX-License-Identifier: MIT
using System.Reflection;
using System.Text.Json.Nodes;
using Xunit;

namespace Kiln;

public sealed partial class PlayerStorageStoreTests
{
    /// <summary>A persistence whose native restore refuses (a stale join, a document over the native limits).</summary>
    private sealed class RefusingPersist : IPlayerStoragePersistence
    {
        public StorageFreeze? Frozen = new("saved", PlayerStorageDocumentTests.Storages(1));
        public IReadOnlyDictionary<string, int> Registrations { get; } = new Dictionary<string, int> { ["mod:bag"] = 8 };
        public StorageFreeze? Freeze(CharacterKey character, EntityId player, uint generation) => Frozen;
        public string? WitnessOf(EntityId player) => "join";
        public void Restore(EntityId player, JsonObject storages, bool paired, string source, long revision) { }
        public void Park(EntityId player, JsonObject parked) => throw new InvalidDataException("storage restore refused: 3");
    }

    private static void OtherModData() =>
        InstanceHarness.Store.GetBundleRaw("other-mod/world", "other-mod").Keys["progress"] = new WorldStore.Slot { Json = "{\"level\":7}" };

    [Fact]
    public void A_refused_storage_restore_never_stops_the_world_generation_from_being_staged()
    {
        using var h = new InstanceHarness();
        var store = InstanceHarness.Store.Storage;
        store.Native = new RefusingPersist();
        try
        {
            var character = new CharacterKey(1, PlayerStorageDocumentTests.Character);
            try { store.Join(character, new EntityId(1), "join", PlayerStorageDocumentTests.Doc("join", 5).Write(8), true); }
            catch (InvalidDataException) { /* the join may report the refusal; the save below must not */ }
            OtherModData();
            h.Save(1);
            Assert.NotNull(h.Bundle(1, "other-mod/world"));
            Assert.NotNull(store.Failure);
        }
        finally { store.Native = null; }
    }

    [Fact]
    public void An_oversized_storage_capture_keeps_the_previous_server_copy_and_the_world_generation()
    {
        using var h = new InstanceHarness();
        var persist = new Persist();
        var store = InstanceHarness.Store.Storage;
        store.Native = persist;
        try
        {
            var character = new CharacterKey(1, PlayerStorageDocumentTests.Character);
            store.Join(character, new EntityId(1), "join", PlayerStorageDocumentTests.Doc("join", 5).Write(8), true);
            h.Save(1);
            var big = new JsonArray();
            for (var i = 0; i < 3000; i++)
                big.Add(new JsonObject { ["storage"] = "mod:bag", ["record"] = Convert.ToBase64String(new byte[48]), ["raw"] = new string('x', 80) + i, ["reason"] = "test" });
            persist.Frozen = new("huge", PlayerStorageDocumentTests.Storages(2), null, big);
            OtherModData();
            h.Save(2);
            Assert.NotNull(h.Bundle(2, "other-mod/world"));
            Assert.Equal("saved", store.Read(character)!.Data["current"]!["witness"]!.GetValue<string>());
            Assert.NotNull(store.Failure);
        }
        finally { store.Native = null; }
    }

    [Fact]
    public void More_than_thirty_two_storage_keys_never_stop_a_join_or_a_save()
    {
        // Thirty-two parked keys of mods that are gone and a newly registered one: every key needs its InstanceData band.
        using var h = new InstanceHarness();
        var persist = new Persist { Definitions = new() { ["new:bag"] = 8 } };
        var store = InstanceHarness.Store.Storage;
        store.Native = persist;
        try
        {
            var character = new CharacterKey(1, PlayerStorageDocumentTests.Character);
            var client = PlayerStorageDocumentTests.Doc("join", 5);
            var parked = new JsonObject();
            for (var i = 0; i < 32; i++) parked[$"gone{i}:bag"] = PlayerStorageDocumentTests.Bag(0);
            client.Data["current"]!["storages"] = new JsonObject();
            client.Data["current"]!["parked"] = parked;
            store.Join(character, new EntityId(1), "join", client.Write(8), true);
            Assert.Equal(32, persist.Parked!.Count);
            OtherModData();
            h.Save(1);
            Assert.NotNull(h.Bundle(1, "other-mod/world"));
            Assert.Equal(32, store.Read(character)!.Data["current"]!["parked"]!.AsObject().Count);
        }
        finally { store.Native = null; }
    }

    [Fact]
    public void A_refused_storage_restore_does_not_abort_the_progress_offer()
    {
        // The offer handler restores storage first; its failure must not cost the progress import and the session's
        // later updates (a session that never speaks never receives its storage pairs).
        using var h = new InstanceHarness();
        var store = InstanceHarness.Store.Storage;
        store.Native = new RefusingPersist();
        try
        {
            var character = new CharacterKey(1, PlayerStorageDocumentTests.Character);
            var result = Offer(character, PlayerStorageDocumentTests.Doc("join", 5), progress: true, trusted: true);
            Assert.True(result.Accepted, result.Reason);
        }
        finally { store.Native = null; }
    }
}
