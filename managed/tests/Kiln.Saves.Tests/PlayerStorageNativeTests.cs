// SPDX-License-Identifier: MIT
using System.Text;
using System.Text.Json.Nodes;
using Kiln.TestKit;
using Xunit;

namespace Kiln;

[Collection("instance data")]
public sealed unsafe class PlayerStorageNativeTests : IDisposable
{
    private readonly InstanceHarness _h = new();
    private readonly ItemsStorageFake _fake = new ItemsStorageFake().Install();
    private static readonly CharacterKey Character = new(1, PlayerStorageDocumentTests.Character);
    private static PlayerStorageStore Store => InstanceHarness.Store.Storage;
    public PlayerStorageNativeTests()
    {
        PlayerStorageNative.ResetForTests();
        PlayerStorageRuntime.ResetForTests();
        Assert.True(PlayerStorageNative.Attach());
    }
    public void Dispose()
    {
        PlayerStorageNative.ResetForTests();
        _fake.Dispose();
        _h.Dispose();
    }

    private static void Register(string key = "mod:bag", uint slots = 8)
    {
        var bytes = Encoding.UTF8.GetBytes(key);
        fixed (byte* p = bytes)
        {
            var def = new StorageNative.Def { Size = (uint)sizeof(StorageNative.Def), Slots = slots, Key = new NativeStr { Ptr = p, Len = (nuint)bytes.Length },
                Filter = new StorageNative.Filter { Size = (uint)sizeof(StorageNative.Filter) } };
            uint handle = 0;
            Assert.Equal(StorageNative.Ok, StorageNative.T->RegisterStorage(101, &def, &handle));
        }
    }

    private void Join(string? document = null, bool trust = true, bool extraMainPages = false)
    {
        _fake.JoinRead(0, Character.Account, Character.Id, 0x123, extraMainPages);
        _fake.JoinApply(0, 1);
        var witness = Store.Native!.WitnessOf(new EntityId(1));
        Assert.Equal("xxh3:0000000000000123", witness);
        Store.Join(Character, new EntityId(1), witness!, document, trust);
        _fake.Step();
    }

    private void Capture(uint revision, ulong witness)
    {
        _fake.SaveRevision = revision;
        _fake.SavesSeq = revision * 10;
        _fake.CaptureOf(0, witness);
        _fake.CapturesDone();
        InstanceHarness.Written();
    }

    [Fact]
    public void Registrations_come_from_a_connected_player_after_a_chest_lookup_or_that_player_leaving()
    {
        // Registrations lists through a connected player. InstancesReady asks WitnessOf about any holder (a chest), and the
        // player last asked about may leave: neither may empty the registrations (InstancesReady would call a player's
        // storages ready before their holders are, and a restore would park every storage).
        Register();
        Join();
        var native = Store.Native!;
        Assert.Equal(new[] { "mod:bag" }, native.Registrations.Keys);
        Assert.Null(native.WitnessOf(new EntityId(99)));  // a chest: no witness
        Assert.Equal(new[] { "mod:bag" }, native.Registrations.Keys);
        // a second player is asked about, then leaves: the first one still answers
        _fake.JoinRead(1, 2, Guid.Parse("00000000-0000-4000-8000-000000000222"), 0x777);
        _fake.JoinApply(1, 2);
        Assert.Equal("xxh3:0000000000000777", native.WitnessOf(new EntityId(2)));
        _fake.Leave(1);
        Assert.Equal(new[] { "mod:bag" }, native.Registrations.Keys);
    }

    [Fact]
    public void TestKit_capture_batch_stages_exact_bytes_and_publishes_only_on_world_written()
    {
        Register();
        Join(PlayerStorageDocumentTests.Doc("xxh3:0000000000000123", 1).Write(8));
        _fake.Put(1, "mod:bag", new StorageFakeRecord(1, 1, 3));
        Capture(1, 0x456);
        Assert.True(File.Exists(WorldStore.PendingPath(_h.Root, 1)));
        Assert.Empty(_fake.BatchTags);
        Assert.Null(Store.Carried(Character));
        InstanceHarness.Store.Commit(1, 1001, SaveReason.PlayerLeft);
        InstanceHarness.Written();
        var carried = PlayerStorageDocument.Read(Store.Carried(Character)!, Character.Id);
        Assert.Equal("xxh3:0000000000000456", carried.Data["current"]!["witness"]!.GetValue<string>());
        Assert.Equal(2, carried.Data["current"]!["storages"]!["mod:bag"]!["records"]!.AsArray().Count);
    }

    [Fact]
    public void TestKit_unregistered_bytes_ride_unchanged_and_registration_unparks_without_second_restore()
    {
        var doc = PlayerStorageDocumentTests.Doc("xxh3:0000000000000123", 1);
        doc.Data["current"]!["storages"]!["mod:bag"]!["opaque"] = "keep me";
        Join(doc.Write(8));
        var parked = Store.Read(Character); // not saved yet
        Assert.Null(parked);
        Capture(1, 0x234);
        var saved = Store.Read(Character)!;
        Assert.Equal("keep me", saved.Data["current"]!["parked"]!["mod:bag"]!["opaque"]!.GetValue<string>());
        Register();
        _fake.Step();
        Assert.Single(_fake.Contents(0, "mod:bag").Live);
        Capture(2, 0x345);
        saved = Store.Read(Character)!;
        Assert.Single(saved.Data["current"]!["storages"]!["mod:bag"]!["records"]!.AsArray());
        Assert.Empty(saved.Data["current"]!["parked"]!.AsObject());
    }

    [Fact]
    public void TestKit_unrestored_capture_and_leave_keep_previous_server_document()
    {
        Register();
        var old = PlayerStorageDocumentTests.Doc("old", 1).Write(32);
        InstanceHarness.Store.GetBundleRaw(PlayerStorageStore.Prefix + Character.Text, "kiln-items").Keys["storage"] = new WorldStore.Slot { Json = old };
        _fake.JoinRead(0, Character.Account, Character.Id, 0x123);
        _fake.JoinApply(0, 1);
        Capture(1, 0x234);
        _fake.Leave(0);
        Assert.Equal(old, InstanceHarness.Store.FindBundle(PlayerStorageStore.Prefix + Character.Text)!.Keys["storage"].Json);
        Assert.Null(Store.Carried(Character));
    }

    [Fact]
    public void TestKit_untrusted_newer_offer_holds_only_server_records_until_explicit_return()
    {
        Register();
        InstanceHarness.Store.GetBundleRaw(PlayerStorageStore.Prefix + Character.Text, "kiln-items").Keys["storage"] =
            new WorldStore.Slot { Json = PlayerStorageDocumentTests.Doc("old", 1).Write(32) };
        Join(PlayerStorageDocumentTests.Doc("xxh3:0000000000000123", 100, 7).Write(8), false);
        var contents = _fake.Contents(0, "mod:bag");
        Assert.Empty(contents.Live);
        var held = Assert.Single(contents.Parked);
        Assert.Equal(StorageNative.ParkStale, held.Reason);
        Assert.Equal(0, held.Record.Slot); // the client's slot7 record was never instantiated
        var bytes = Encoding.UTF8.GetBytes("mod:bag");
        fixed (byte* p = bytes)
        {
            ulong ticket = 0;
            Assert.Equal(StorageNative.Ok, StorageNative.T->ReturnParked(101, 1, new NativeStr { Ptr = p, Len = (nuint)bytes.Length }, 99, null, null, &ticket));
        }
        _fake.Step();
        Capture(1, 0x345);
        Assert.Empty(Store.Read(Character)!.Data["current"]!["parked"]!.AsObject());
    }

    [Fact]
    public void TestKit_validation_quarantine_keeps_game_bytes_and_reason()
    {
        Register();
        var memory = (StorageFakeMemory)_fake.World;
        memory.Items[2] = new StorageFakeItem(1, ItemCategory.Materials);
        Join(PlayerStorageDocumentTests.Doc("xxh3:0000000000000123", 1).Write(8)); // item1 unknown in this catalog
        Capture(1, 0x456);
        var entry = Assert.Single(Store.Read(Character)!.Data["quarantine"]!.AsArray());
        Assert.Contains("unknown item", entry!["reason"]!.GetValue<string>());
        Assert.Equal(PlayerStorageDocumentTests.Record()["b64"]!.GetValue<string>(), entry["record"]!.GetValue<string>());
    }
    [Theory]
    [InlineData("requested", StorageNative.ParkRequested)]
    [InlineData("changed elsewhere since this server's copy", StorageNative.ParkStale)]
    [InlineData("locked", StorageNative.ParkLocked)]
    [InlineData("returned outside Kiln", StorageNative.ParkReturned)]
    public void Held_reason_survives_document_encode_native_public_event_and_capture(string reason, uint machineReason)
    {
        Register();
        var doc = PlayerStorageDocumentTests.Doc("xxh3:0000000000000123", 1);
        var bag = (JsonObject)doc.Data["current"]!["storages"]!["mod:bag"]!.DeepClone();
        bag["parkReason"] = reason;
        bag["nativeParkReason"] = StorageNative.ParkUnregistered; // Older machine metadata must not override the new hold.
        doc.Data["current"]!["storages"] = new JsonObject();
        doc.Data["current"]!["parked"] = new JsonObject { ["mod:bag"] = bag };
        var events = new List<PlayerStorageParkedEvent>();
        using var subscription = Events.On<PlayerStorageParkedEvent>(events.Add);
        Join(doc.Write(8));
        Assert.Equal(machineReason, Assert.Single(_fake.Contents(0, "mod:bag").Parked).Reason);
        typeof(PlayerStorageRuntime).GetMethod("Tick", System.Reflection.BindingFlags.NonPublic | System.Reflection.BindingFlags.Static)!.Invoke(null, [1ul]);
        Assert.Equal((PlayerStorageParkReason)machineReason, Assert.Single(events).Reason);
        Capture(1, 0x456);
        var saved = Store.Read(Character)!.Data["current"]!["parked"]!["mod:bag"]!;
        Assert.Equal(reason, saved["parkReason"]!.GetValue<string>());
        Assert.Equal(machineReason, saved["nativeParkReason"]!.GetValue<uint>());
    }

    [Fact]
    public void Vanilla_hold_is_paired_at_next_save_and_same_flag_does_not_hold_again()
    {
        Register();
        InstanceHarness.Store.GetBundleRaw(PlayerStorageStore.Prefix + Character.Text, "kiln-items").Keys["storage"] =
            new WorldStore.Slot { Json = PlayerStorageDocumentTests.Doc("old", 1000).Write(32) };
        Join(extraMainPages: true);
        Assert.Equal(StorageNative.ParkReturned, Assert.Single(_fake.Contents(0, "mod:bag").Parked).Reason);
        Capture(1, 0x456);
        InstanceHarness.Store.Commit(1, 1001, SaveReason.Request);
        InstanceHarness.Written();
        _fake.Leave(0);
        _fake.JoinRead(0, Character.Account, Character.Id, 0x456, extraMainPages: true);
        _fake.JoinApply(0, 1);
        var choice = Store.Join(Character, new EntityId(1), Store.Native!.WitnessOf(new(1))!, null, true);
        _fake.Step();
        Assert.True(choice!.Paired);
        Assert.Null(Store.Notice(Character));
        Assert.Empty(_fake.Contents(0, "mod:bag").Live);
        Assert.Equal(StorageNative.ParkReturned, Assert.Single(_fake.Contents(0, "mod:bag").Parked).Reason);
    }

    [Fact]
    public void False_positive_extra_pages_hold_can_be_explicitly_returned_without_deleting_or_repeating_records()
    {
        Register();
        InstanceHarness.Store.GetBundleRaw(PlayerStorageStore.Prefix + Character.Text, "kiln-items").Keys["storage"] =
            new WorldStore.Slot { Json = PlayerStorageDocumentTests.Doc("old", 1000).Write(32) };
        Join(extraMainPages: true);
        Assert.Single(_fake.Contents(0, "mod:bag").Parked);
        var bytes = Encoding.UTF8.GetBytes("mod:bag");
        fixed (byte* p = bytes)
        {
            ulong ticket = 0;
            Assert.Equal(StorageNative.Ok, StorageNative.T->ReturnParked(0, 1, new NativeStr { Ptr = p, Len = (nuint)bytes.Length }, 99, null, null, &ticket));
        }
        _fake.Step();
        Assert.Empty(_fake.Contents(0, "mod:bag").Parked);
        Assert.Single(((StorageFakeMemory)_fake.World).Delivered[99]);
        Capture(1, 0x456);
        Assert.Empty(Store.Read(Character)!.Data["current"]!["parked"]!.AsObject());
    }

    [Fact]
    public void Scripted_native_housekeeping_uses_readable_target_missing_from_list_and_defers_metadata_events()
    {
        Register();
        var player = _h.World.Join(Character, reuseId: new EntityId(1));
        Join(PlayerStorageDocumentTests.Doc("xxh3:0000000000000123", 1).Write(8));
        var holder = _h.World.Chest(Guid.NewGuid(), default);
        var fields = new InstanceFields(new ItemId(123), 1, 6, 0, 0, 0, 0, default, 0, 0, default, 0);
        var item = _h.World.Give(holder, 3, fields);
        var visible = true;
        InstanceHarness.Store.Instances.World = new StorageInstanceWorld(_h.World, Store.NormalizeInstance,
            p => p == player && visible ? [player, holder] : [p], _ => true);
        _fake.EmitHousekeeping(StorageNative.StageRestored, 0, "mod:bag", holder.Value, 1);
        Assert.Equal(new InstancePlace(player, 1027), Store.NormalizeInstance(new(holder, 3)));
        _h.Tag(item, 77);
        var io = new List<string>();
        var before = WorldStore.IoHook;
        WorldStore.IoHook = io.Add;
        try
        {
            // Target is intentionally absent from the fake storage list; handle0 represents a retired registration.
            _fake.EmitHousekeeping(StorageNative.StageParked, 0, "mod:bag", holder.Value, 0);
            Assert.Empty(_h.Events);
            Assert.Empty(io);
            visible = false;
            _h.World.Destroy(item);
            _h.World.Holders.Remove(holder.Value);
            holder = _h.World.Chest(Guid.NewGuid(), default);
            var rebuilt = _h.World.Give(holder, 3, fields);
            visible = true;
            _fake.EmitHousekeeping(StorageNative.StageRestored, 0, "mod:bag", holder.Value, 1);
            Assert.Empty(_h.Events);
            Assert.Empty(io);
            Assert.Equal(77, _h.Count(rebuilt)); // Already attached before Native's public restored notice can run.
        }
        finally { WorldStore.IoHook = before; }
        _h.Tick();
        Assert.Single(_h.Of<InstanceDataRestoredEvent>());
    }

    [Fact]
    public void Unregister_merges_64_live_records_with_held_bag_and_quarantines_overflow_without_loss()
    {
        Register(slots: 64);
        var doc = PlayerStorageDocumentTests.Doc("xxh3:0000000000000123", 1);
        var held = (JsonObject)doc.Data["current"]!["storages"]!["mod:bag"]!.DeepClone();
        held["parkReason"] = "returned outside Kiln";
        doc.Data["current"]!["storages"] = new JsonObject();
        doc.Data["current"]!["parked"] = new JsonObject { ["mod:bag"] = held };
        Join(doc.Write(8));
        for (var slot = 0; slot < 64; slot++) _fake.Put(1, "mod:bag", new StorageFakeRecord(slot, (uint)slot + 2, 1));
        _fake.Revoke(101);
        Capture(1, 0x456);
        var saved = Store.Read(Character)!;
        var bag = saved.Data["current"]!["parked"]!["mod:bag"]!;
        var records = bag["records"]!.AsArray();
        Assert.Equal(64, records.Count);
        Assert.Equal(64, records.Select(r => r!["s"]!.GetValue<int>()).Distinct().Count());
        Assert.Equal(Enumerable.Range(1, 64).Select(i => (uint)i), records.Select(r => r!["item"]!.GetValue<uint>()).Order());
        Assert.Equal("returned outside Kiln", bag["parkReason"]!.GetValue<string>());
        Assert.Equal(StorageNative.ParkReturned, bag["nativeParkReason"]!.GetValue<uint>());
        var overflow = Assert.Single(saved.Data["quarantine"]!.AsArray());
        Assert.Equal("parked storage full", overflow!["reason"]!.GetValue<string>());
        Assert.Equal(Convert.ToBase64String(new StorageFakeRecord(63, 65, 1).Bytes), overflow["record"]!.GetValue<string>());
        Assert.Equal(65, records.Count + saved.Data["quarantine"]!.AsArray().Count);
    }

    [Fact]
    public void Native_parked_full_quarantine_uses_the_plain_operator_reason()
    {
        var reason = typeof(PlayerStorageNative).GetMethod("QuarantineReason", System.Reflection.BindingFlags.NonPublic | System.Reflection.BindingFlags.Static)!;
        Assert.Equal("parked storage full", reason.Invoke(null, [(byte)6])); // KIS_QUARANTINE_FULL; no ABI row/layout change.
    }

}
