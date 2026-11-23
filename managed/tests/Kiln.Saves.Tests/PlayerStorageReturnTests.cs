// SPDX-License-Identifier: MIT
using System.Reflection;
using System.Text.Json.Nodes;
using Xunit;

namespace Kiln;

public sealed partial class PlayerStorageStoreTests
{
    private static PlayerStorageDocument ReturnedDocument(long revision = 20)
    {
        var doc = PlayerStorageDocumentTests.Doc("returned", revision);
        doc.Data["current"]!["storages"] = new JsonObject();
        doc.Data["returns"] = new JsonArray(new JsonObject { ["revision"] = 20L, ["fromWitness"] = "before", ["toWitness"] = "returned" });
        return doc;
    }

    private static CharResult Offer(CharacterKey character, PlayerStorageDocument client, bool progress, bool trusted)
    {
        var hidden = BindingFlags.NonPublic | BindingFlags.Public | BindingFlags.Instance;
        var type = typeof(CharacterSync).GetNestedType("Session", BindingFlags.NonPublic)!;
        var session = Activator.CreateInstance(type, hidden, null, [character, new EntityId(1), "storage player"], null)!;
        type.GetField("ProgressAllowed", hidden)!.SetValue(session, progress);
        type.GetField("StorageTrusted", hidden)!.SetValue(session, trusted);
        var offered = new Dictionary<string, Dictionary<string, string>> { ["kiln-items"] = new() { ["storage"] = client.Write(8) } };
        return (CharResult)typeof(CharacterSync).GetMethod("Decide", BindingFlags.NonPublic | BindingFlags.Static)!.Invoke(null, [session, 1L, offered])!;
    }

    [Theory]
    [InlineData(false, false)]
    [InlineData(false, true)]
    [InlineData(true, false)]
    [InlineData(true, true)]
    public void Explicit_return_withholds_newer_fork_even_when_progress_or_client_trust_are_off(bool progress, bool trusted)
    {
        using var h = new InstanceHarness();
        var store = InstanceHarness.Store.Storage;
        var persist = new Persist { JoinWitness = "after-return" };
        store.Native = persist;
        try
        {
            var character = new CharacterKey(1, PlayerStorageDocumentTests.Character);
            InstanceHarness.Store.GetBundleRaw(PlayerStorageStore.Prefix + character.Text, "kiln-items").Keys["storage"] =
                new WorldStore.Slot { Json = PlayerStorageDocumentTests.Doc("fork", 1000).Write(32) };
            var result = Offer(character, ReturnedDocument(), progress, trusted);
            if (!progress) Assert.False(result.Accepted);
            Assert.Empty(persist.Restored!);
            Assert.Equal("returned outside Kiln", persist.Parked!["mod:bag"]!["parkReason"]!.GetValue<string>());
            Assert.Equal(0, persist.Parked["mod:bag"]!["records"]![0]!["s"]!.GetValue<int>());
        }
        finally { store.Native = null; }
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public void Vanilla_extra_main_pages_hold_unpaired_server_copy_without_a_sidecar(bool trusted)
    {
        using var h = new InstanceHarness();
        var store = InstanceHarness.Store.Storage;
        var persist = new Persist { ExtraPages = true };
        store.Native = persist;
        try
        {
            var character = new CharacterKey(1, PlayerStorageDocumentTests.Character);
            InstanceHarness.Store.GetBundleRaw(PlayerStorageStore.Prefix + character.Text, "kiln-items").Keys["storage"] =
                new WorldStore.Slot { Json = PlayerStorageDocumentTests.Doc("old", 1000).Write(32) };
            store.Join(character, new EntityId(1), "vanilla-returned", null, trusted);
            Assert.Empty(persist.Restored!);
            Assert.Equal("returned outside Kiln", persist.Parked!["mod:bag"]!["parkReason"]!.GetValue<string>());
        }
        finally { store.Native = null; }
    }

    [Fact]
    public void Paired_copy_bypasses_return_holds_and_ordinary_vanilla_without_extra_pages_is_unchanged()
    {
        using var h = new InstanceHarness();
        var store = InstanceHarness.Store.Storage;
        var persist = new Persist { ExtraPages = true };
        store.Native = persist;
        try
        {
            var character = new CharacterKey(1, PlayerStorageDocumentTests.Character);
            InstanceHarness.Store.GetBundleRaw(PlayerStorageStore.Prefix + character.Text, "kiln-items").Keys["storage"] =
                new WorldStore.Slot { Json = PlayerStorageDocumentTests.Doc("same", 1000).Write(32) };
            Assert.True(store.Join(character, new EntityId(1), "same", ReturnedDocument().Write(8), false)!.Paired);
            Assert.Single(persist.Restored!);
            persist.ExtraPages = false;
            store.Join(character, new EntityId(1), "ordinary-vanilla", null, true);
            Assert.Single(persist.Restored!);
        }
        finally { store.Native = null; }
    }

    [Fact]
    public void No_return_marker_does_not_hold_a_fork_just_because_it_is_outside_client_lineage()
    {
        using var h = new InstanceHarness();
        var store = InstanceHarness.Store.Storage;
        var persist = new Persist();
        store.Native = persist;
        try
        {
            var character = new CharacterKey(1, PlayerStorageDocumentTests.Character);
            InstanceHarness.Store.GetBundleRaw(PlayerStorageStore.Prefix + character.Text, "kiln-items").Keys["storage"] =
                new WorldStore.Slot { Json = PlayerStorageDocumentTests.Doc("fork", 1000).Write(32) };
            store.Join(character, new EntityId(1), "vanilla", PlayerStorageDocumentTests.Doc("elsewhere", 20).Write(8), false);
            Assert.Single(persist.Restored!);
        }
        finally { store.Native = null; }
    }

    [Fact]
    public void Pre_return_history_is_not_an_acknowledged_post_return_server_copy()
    {
        using var h = new InstanceHarness();
        var store = InstanceHarness.Store.Storage;
        var persist = new Persist();
        store.Native = persist;
        try
        {
            var character = new CharacterKey(1, PlayerStorageDocumentTests.Character);
            var old = PlayerStorageDocumentTests.Doc("before", 9);
            InstanceHarness.Store.GetBundleRaw(PlayerStorageStore.Prefix + character.Text, "kiln-items").Keys["storage"] = new WorldStore.Slot { Json = old.Write(32) };
            var client = ReturnedDocument();
            var historical = (JsonObject)old.Data["current"]!.DeepClone();
            historical["revision"] = 9L;
            client.Data["history"]!.AsArray().Add(historical);
            store.Join(character, new EntityId(1), "returned", client.Write(8), false);
            Assert.Equal("returned outside Kiln", persist.Parked!["mod:bag"]!["parkReason"]!.GetValue<string>());
        }
        finally { store.Native = null; }
    }

    [Fact]
    public void Acknowledged_post_return_lineage_does_not_create_a_new_hold()
    {
        using var h = new InstanceHarness();
        var store = InstanceHarness.Store.Storage;
        var persist = new Persist();
        store.Native = persist;
        try
        {
            var character = new CharacterKey(1, PlayerStorageDocumentTests.Character);
            var server = PlayerStorageDocumentTests.Doc("known", 1000);
            InstanceHarness.Store.GetBundleRaw(PlayerStorageStore.Prefix + character.Text, "kiln-items").Keys["storage"] = new WorldStore.Slot { Json = server.Write(32) };
            var client = ReturnedDocument(1000);
            client.Data["current"]!["storages"] = server.Data["current"]!["storages"]!.DeepClone();
            var historical = (JsonObject)server.Data["current"]!.DeepClone();
            historical["revision"] = 1000L;
            client.Data["history"]!.AsArray().Add(historical);
            store.Join(character, new EntityId(1), "ordinary-vanilla", client.Write(8), false);
            Assert.Single(persist.Restored!);
        }
        finally { store.Native = null; }
    }
    [Fact]
    public void Newer_untrusted_offer_does_not_downgrade_an_existing_return_hold_to_stale()
    {
        using var h = new InstanceHarness();
        var store = InstanceHarness.Store.Storage;
        var persist = new Persist();
        store.Native = persist;
        var character = new CharacterKey(1, PlayerStorageDocumentTests.Character);
        var server = PlayerStorageDocumentTests.Doc("old", 1000);
        var held = server.Data["current"]!["storages"]!.DeepClone();
        held["mod:bag"]!["parkReason"] = "returned outside Kiln";
        server.Data["current"]!["storages"] = new JsonObject();
        server.Data["current"]!["parked"] = held;
        InstanceHarness.Store.GetBundleRaw(PlayerStorageStore.Prefix + character.Text, "kiln-items").Keys["storage"] = new WorldStore.Slot { Json = server.Write(32) };
        store.Join(character, new EntityId(1), "elsewhere", PlayerStorageDocumentTests.Doc("client", 1001).Write(8), false);
        Assert.Equal("returned outside Kiln", persist.Parked!["mod:bag"]!["parkReason"]!.GetValue<string>());
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public void Launcher_produced_sidecar_return_marker_holds_a_newer_unpaired_server_copy(bool trusted)
    {
        // Exact synthetic Fixture::produced() JSON from crates/kiln-manage/src/saves/editor_storage.rs.
        // Its AA== history record is deliberately a stub; Saves quarantines it rather than interpreting it as an item.
        const string sidecar = """
        {"character":"01010101-0101-0101-0101-010101010101","format":1,"mods":{"kiln-items":{"storage":{"character":"01010101-0101-0101-0101-010101010101","current":{"parked":{},"storages":{"mod:bag":{"records":[],"slots":8}},"witness":"xxh3:0000000000000002","worldRev":10},"history":[{"parked":{},"revision":5,"storages":{"mod:bag":{"records":[{"b64":"AA==","item":1,"n":1,"s":0}],"slots":8}},"witness":"xxh3:0000000000000001","worldRev":10}],"quarantine":[],"returns":[{"fromWitness":"xxh3:0000000000000001","revision":6,"toWitness":"xxh3:0000000000000002"}],"revision":6,"v":1}},"other":{"keep":1}},"revision":11}
        """;
        var envelope = JsonNode.Parse(sidecar)!;
        var character = new CharacterKey(1, Guid.Parse(envelope["character"]!.GetValue<string>()));
        var carried = envelope["mods"]!["kiln-items"]!["storage"]!.ToJsonString();
        var parsed = PlayerStorageDocument.Read(carried, character.Id);
        Assert.Equal(6, parsed.LatestReturnRevision);
        Assert.Equal("xxh3:0000000000000002", parsed.Data["current"]!["witness"]!.GetValue<string>());
        Assert.Single(parsed.Data["quarantine"]!.AsArray());
        using var h = new InstanceHarness();
        var store = InstanceHarness.Store.Storage;
        var persist = new Persist();
        store.Native = persist;
        var server = PlayerStorageDocumentTests.Doc("fork", 1000);
        server.Data["character"] = character.Id.ToString();
        InstanceHarness.Store.GetBundleRaw(PlayerStorageStore.Prefix + character.Text, "kiln-items").Keys["storage"] = new WorldStore.Slot { Json = server.Write(32) };
        store.Join(character, new EntityId(1), "xxh3:0000000000000003", carried, trusted);
        Assert.Empty(persist.Restored!);
        Assert.Equal("returned outside Kiln", persist.Parked!["mod:bag"]!["parkReason"]!.GetValue<string>());
    }

}
