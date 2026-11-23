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

    private void Join(string? document = null, bool trust = true)
    {
        _fake.JoinRead(0, Character.Account, Character.Id, 0x123);
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
}
