// SPDX-License-Identifier: MIT
using System.Text.Json.Nodes;
using Xunit;

namespace Kiln;

public sealed class PlayerStorageDocumentTests
{
    internal static readonly Guid Character = Guid.Parse("00000000-0000-4000-8000-000000000128");
    internal static JsonObject Record(int slot = 0) => new() { ["s"] = slot, ["b64"] = Convert.ToBase64String([(byte)slot, 1, 0, 0, 0, 1, 0, 0]), ["item"] = 1, ["n"] = 1 };
    internal static JsonObject Bag(int slot = 0) => new() { ["slots"] = 8, ["records"] = new JsonArray(Record(slot)), ["refs"] = new JsonObject { ["arrow"] = slot } };
    internal static JsonObject Storages(int slot = 0) => new() { ["mod:bag"] = Bag(slot) };
    internal static PlayerStorageDocument Doc(string witness, long revision, int slot = 0) => PlayerStorageDocument.Read(new JsonObject
    {
        ["v"] = 1, ["character"] = Character.ToString(), ["revision"] = revision,
        ["current"] = new JsonObject { ["witness"] = witness, ["worldRev"] = 3, ["storages"] = Storages(slot), ["parked"] = new JsonObject() },
        ["history"] = new JsonArray(), ["quarantine"] = new JsonArray(),
    }.ToJsonString(), Character);

    [Theory]
    [InlineData(false, false, "old", 1)]
    [InlineData(true, false, "old", 1)]
    [InlineData(false, true, "new", 2)]
    [InlineData(true, true, "new", 2)]
    public void Every_crash_order_selects_pair_for_character_file(bool sidecarWritten, bool fileWritten, string expectedWitness, long expectedRevision)
    {
        var server = Doc("old", 1);
        server.Capture("new", 4, Storages(1), new());
        var client = sidecarWritten ? PlayerStorageDocument.Read(server.Write(8), Character) : Doc("old", 1);
        var chosen = PlayerStorageDocument.Choose(fileWritten ? "new" : "old", server, client)!;
        Assert.True(chosen.Paired);
        Assert.Equal(expectedWitness, chosen.Pair["witness"]!.GetValue<string>());
        Assert.Equal(expectedRevision, chosen.Revision);
    }

    [Theory]
    [InlineData("server", 2, "client", 100, "server", true, "server")]
    [InlineData("server", 100, "client", 2, "client", true, "client")]
    [InlineData("same", 2, "same", 3, "same", true, "client")]
    [InlineData("same", 3, "same", 3, "same", true, "server")]
    [InlineData("a", 2, "b", 3, "vanilla", false, "client")]
    public void Pairing_travel_vanilla_and_ties(string sw, long sr, string cw, long cr, string arriving, bool paired, string source)
    {
        var result = PlayerStorageDocument.Choose(arriving, Doc(sw, sr), Doc(cw, cr))!;
        Assert.Equal(paired, result.Paired);
        Assert.Equal(source, result.Source);
    }

    [Fact]
    public void Trust_false_ignores_even_paired_newer_client()
    {
        var choice = PlayerStorageDocument.Choose("client", Doc("server", 1), Doc("client", 100), false)!;
        Assert.Equal("server", choice.Source);
        Assert.False(choice.Paired);
        Assert.Null(PlayerStorageDocument.Choose("client", null, Doc("client", 100), false));
    }

    [Fact]
    public void Offline_return_new_current_pair_beats_old_server_and_cannot_restore_returned_records()
    {
        var client = Doc("before", 8);
        client.Capture("returned", 3, new(), new());
        var result = PlayerStorageDocument.Choose("returned", Doc("before", 8), client)!;
        Assert.Equal(9, result.Revision);
        Assert.Empty(result.Pair["storages"]!.AsObject());
        Assert.True(result.Paired);
    }

    [Fact]
    public void Offline_return_sidecar_ahead_of_character_uses_full_history_pair()
    {
        var sidecar = Doc("before-return", 8);
        sidecar.Capture("after-return", 4, new(), new());
        var chosen = PlayerStorageDocument.Choose("before-return", null, PlayerStorageDocument.Read(sidecar.Write(8), Character))!;
        Assert.True(chosen.Paired);
        Assert.Equal(8, chosen.Revision);
        Assert.Single(chosen.Pair["storages"]!.AsObject());
    }

    [Fact]
    public void Match_on_nonfirst_alias_uses_that_alias_revision()
    {
        var doc = Doc("first", 1);
        doc.Capture("second", 4, Storages(), new());
        doc.Capture("third", 5, Storages(), new());
        var selected = PlayerStorageDocument.Choose("first", PlayerStorageDocument.Read(doc.Write(8), Character), null)!;
        Assert.True(selected.Paired);
        Assert.Equal(1, selected.Revision);
    }

    [Theory]
    [InlineData(8)]
    [InlineData(32)]
    public void History_is_bounded_and_retains_matching_witnesses_when_contents_equal(int limit)
    {
        var doc = Doc("w0", 1);
        for (var i = 1; i < 40; i++) doc.Capture("w" + i, 3, Storages(i % 8), new());
        var read = PlayerStorageDocument.Read(doc.Write(limit), Character);
        Assert.Equal(limit, read.Data["history"]!.AsArray().Count);
        Assert.True(PlayerStorageDocument.Choose("w38", read, null)!.Paired);
    }

    [Fact]
    public void Consecutive_equal_storages_share_history_entry_without_losing_witness_pairing()
    {
        var doc = Doc("a", 1);
        doc.Capture("b", 4, Storages(), new());
        doc.Capture("c", 5, Storages(), new());
        Assert.True(doc.Data["history"]!.AsArray().Count <= 1);
        var reread = PlayerStorageDocument.Read(doc.Write(8), Character);
        Assert.True(PlayerStorageDocument.Choose("a", reread, null)!.Paired);
        Assert.True(PlayerStorageDocument.Choose("b", reread, null)!.Paired);
    }

    [Fact]
    public void Higher_version_is_parked_whole_readonly_and_not_chosen()
    {
        const string raw = "{\"v\":99,\"future\":{\"keep\":true}}";
        var doc = PlayerStorageDocument.Read(raw, Character);
        Assert.True(doc.ReadOnly);
        Assert.Equal(raw, doc.Write(8));
        Assert.Null(PlayerStorageDocument.Choose("x", doc, null));
    }

    [Fact]
    public void Lower_version_migrates_and_wrong_character_is_rejected()
    {
        var raw = Doc("a", 1).Data;
        raw["v"] = 0;
        Assert.Equal(1, PlayerStorageDocument.Read(raw.ToJsonString(), Character).Data["v"]!.GetValue<int>());
        Assert.Throws<InvalidDataException>(() => PlayerStorageDocument.Read(raw.ToJsonString(), Guid.NewGuid()));
    }

    [Fact]
    public void Game_record_bytes_drive_every_optional_mirror_field()
    {
        var bytes = new byte[] { 0, 1, 0, 0, 0, 1, 0, 63, 9,
            10, 0, 0, 0, 20, 0, 0, 0, 30, 0, 0, 0, 40, 0, 0, 0,
            0x12, 0x34, 0x56, 0x78, 4, 2, 0x11, 0x22, 0x33, 0x44, 5 };
        var raw = Doc("a", 1).Data;
        raw["current"]!["storages"]!["mod:bag"]!["records"]![0]!["b64"] = Convert.ToBase64String(bytes);
        var read = PlayerStorageDocument.Read(raw.ToJsonString(), Character);
        var record = read.Data["current"]!["storages"]!["mod:bag"]!["records"]![0]!;
        Assert.Equal(9, record["level"]!.GetValue<int>());
        Assert.Equal(10u, record["durability"]!.GetValue<uint>());
        Assert.Equal(20u, record["maxDurability"]!.GetValue<uint>());
        Assert.Equal(30u, record["liquid"]!.GetValue<uint>());
        Assert.Equal(40u, record["maxLiquid"]!.GetValue<uint>());
        Assert.Equal("12345678", record["appearance"]!.GetValue<string>());
        Assert.Equal(4, record["maxUpgrades"]!.GetValue<int>());
        Assert.Equal(2, record["upgrades"]!.GetValue<int>());
        Assert.Equal("1122334405", record["socket"]!.GetValue<string>());
    }

    [Fact]
    public void Bad_storage_record_and_history_are_isolated_with_original_bytes_and_reason()
    {
        var raw = Doc("a", 1).Data;
        raw["current"]!["storages"]!["bad:bag"] = "broken";
        raw["current"]!["storages"]!["mod:bag"]!["records"]!.AsArray().Add(new JsonObject { ["s"] = 1, ["b64"] = "not-base64" });
        raw["history"]!.AsArray().Add("broken-history");
        var doc = PlayerStorageDocument.Read(raw.ToJsonString(), Character);
        Assert.Single(doc.Data["current"]!["storages"]!["mod:bag"]!["records"]!.AsArray());
        Assert.Equal(3, doc.Data["quarantine"]!.AsArray().Count);
        Assert.All(doc.Data["quarantine"]!.AsArray(), q => { Assert.NotEmpty(q!["record"]!.GetValue<string>()); Assert.NotEmpty(q["reason"]!.GetValue<string>()); });
    }

    [Fact]
    public void Bad_current_pair_does_not_hide_good_history()
    {
        var raw = Doc("good", 1).Data;
        var history = (JsonObject)raw["current"]!.DeepClone();
        history["revision"] = 1L;
        raw["history"]!.AsArray().Add(history);
        raw["current"]!["witness"] = new JsonObject();
        var read = PlayerStorageDocument.Read(raw.ToJsonString(), Character);
        Assert.True(PlayerStorageDocument.Choose("good", read, null)!.Paired);
        Assert.Single(read.Data["quarantine"]!.AsArray());
    }

    [Fact]
    public void Byte_and_structure_limits_are_checked_before_record_decode()
    {
        Assert.Throws<InvalidDataException>(() => PlayerStorageDocument.Read(new string(' ', PlayerStorageDocument.MaxBytes + 1), Character));
        var raw = Doc("a", 1).Data;
        var bags = raw["current"]!["storages"]!.AsObject();
        for (var i = 0; i < 32; i++) bags["mod:b" + i] = Bag();
        var doc = PlayerStorageDocument.Read(raw.ToJsonString(), Character);
        Assert.Empty(doc.Data["current"]!["storages"]!.AsObject());
        Assert.Single(doc.Data["quarantine"]!.AsArray());
        raw = Doc("a", 1).Data;
        raw["current"]!["storages"]!["mod:bag"]!["slots"] = 0;
        var invalid = PlayerStorageDocument.Read(raw.ToJsonString(), Character);
        Assert.Empty(invalid.Data["current"]!["storages"]!.AsObject());
    }

    [Fact]
    public void Byte_budget_trims_old_pairs_but_keeps_newest_offline_return_history()
    {
        var doc = Doc("w0", 1);
        for (var i = 1; i <= 6; i++)
        {
            var bags = Storages();
            bags["mod:bag"]!["opaque"] = new string((char)('a' + i), 21000);
            doc.Capture("w" + i, (uint)i, bags, new());
        }
        var json = doc.Write(8);
        Assert.True(System.Text.Encoding.UTF8.GetByteCount(json) <= PlayerStorageDocument.MaxBytes);
        var read = PlayerStorageDocument.Read(json, Character);
        Assert.True(PlayerStorageDocument.Choose("w5", read, null)!.Paired);
        Assert.False(PlayerStorageDocument.Choose("w0", read, null)!.Paired);
        Assert.NotEmpty(read.Data["history"]!.AsArray());
    }

    [Fact]
    public void Current_plus_pinned_history_over_budget_refuses_without_mutating_document()
    {
        var doc = Doc("before", 1);
        doc.Data["current"]!["storages"]!["mod:bag"]!["opaque"] = new string('a', 36000);
        var returned = Storages(1);
        returned["mod:bag"]!["opaque"] = new string('b', 36000);
        doc.Capture("after", 2, returned, new());
        var before = doc.Data.ToJsonString();
        Assert.Throws<InvalidDataException>(() => doc.Write(8));
        Assert.Equal(before, doc.Data.ToJsonString());
    }

    [Fact]
    public void Unregistered_and_overflow_records_ride_unchanged_then_registration_restores_them()
    {
        var bag = Bag(7);
        var original = bag.ToJsonString();
        var (active, parked) = PlayerStorageDocument.Partition(new() { ["mod:bag"] = bag }, new(), new Dictionary<string, int>());
        Assert.Empty(active);
        Assert.Equal(original, parked["mod:bag"]!.ToJsonString());
        (active, parked) = PlayerStorageDocument.Partition(active, parked, new Dictionary<string, int> { ["mod:bag"] = 4 });
        Assert.Empty(active["mod:bag"]!["records"]!.AsArray());
        Assert.Single(parked["mod:bag"]!["records"]!.AsArray());
        (active, parked) = PlayerStorageDocument.Partition(active, parked, new Dictionary<string, int> { ["mod:bag"] = 8 });
        Assert.Single(active["mod:bag"]!["records"]!.AsArray());
        Assert.Empty(parked);
    }
}
