// SPDX-License-Identifier: MIT
// kiln.items.storage v1's managed mirror (StorageNative) against the header's sizes and offsets
// (native/core/abi/services/kiln_items_storage.h; native/core/tests/abi_headers/items_storage.c holds the same numbers).
// A change here is an ABI break, never a test update.
using System.Runtime.InteropServices;
using Kiln;
using Xunit;

namespace Kiln.ItemsTests;

public sealed unsafe class StorageNativeLayoutTests
{
    private static int Off<T>(string field) where T : unmanaged => (int)Marshal.OffsetOf<T>(field);

    [Fact]
    public void EveryStructHasTheHeadersSize()
    {
        Assert.Equal(24, sizeof(StorageNative.Filter));
        Assert.Equal(56, sizeof(StorageNative.Def));
        Assert.Equal(192, sizeof(StorageNative.State));
        Assert.Equal(80, sizeof(StorageNative.Record));
        Assert.Equal(160, sizeof(StorageNative.Entry));
        Assert.Equal(80, sizeof(StorageNative.Doc));
        Assert.Equal(104, sizeof(StorageNative.Capture));
        Assert.Equal(80, sizeof(StorageNative.Batch));
        Assert.Equal(80, sizeof(StorageNative.Witness));
        Assert.Equal(240, sizeof(StorageNative.Event));
        Assert.Equal(208, sizeof(StorageNative.Status));
        Assert.Equal(208, sizeof(StorageNative.ReturnResult));
        Assert.Equal(128, sizeof(StorageNative.Table));
    }

    [Fact]
    public void TheTablesRowsSitAtTheHeadersOffsets()
    {
        string[] rows = ["RegisterStorage", "Holder", "GetState", "SetRef", "GetStatus", "List", "NextEvent", "ReturnParked",
            "PersistBind", "TakeFrozen", "ReleaseFrozen", "WitnessOf", "Restore", "Park", "Unpark"];
        for (var i = 0; i < rows.Length; i++) Assert.Equal(8 + 8 * i, Off<StorageNative.Table>(rows[i]));
    }

    [Fact]
    public void TheStructsMembersSitAtTheHeadersOffsets()
    {
        Assert.Equal(16, Off<StorageNative.Filter>("Ids"));
        Assert.Equal(8, Off<StorageNative.Def>("Key"));
        Assert.Equal(24, Off<StorageNative.Def>("Death"));
        Assert.Equal(32, Off<StorageNative.Def>("Filter"));
        Assert.Equal(136, Off<StorageNative.State>("Handle"));
        Assert.Equal(148, Off<StorageNative.State>("Holder"));
        Assert.Equal(172, Off<StorageNative.State>("Death"));
        Assert.Equal(176, Off<StorageNative.State>("Refs"));
        Assert.Equal(184, Off<StorageNative.State>("Revision"));
        Assert.Equal(4, Off<StorageNative.Record>("Item"));
        Assert.Equal(12, Off<StorageNative.Record>("Level"));
        Assert.Equal(13, Off<StorageNative.Record>("Reason"));
        Assert.Equal(16, Off<StorageNative.Record>("Data"));
        Assert.Equal(136, Off<StorageNative.Entry>("Slots"));
        Assert.Equal(148, Off<StorageNative.Entry>("Reason"));
        Assert.Equal(152, Off<StorageNative.Entry>("Refs"));
        Assert.Equal(24, Off<StorageNative.Doc>("Revision"));
        Assert.Equal(48, Off<StorageNative.Doc>("Character"));
        Assert.Equal(64, Off<StorageNative.Doc>("Entries"));
        Assert.Equal(72, Off<StorageNative.Doc>("Records"));
        Assert.Equal(32, Off<StorageNative.Capture>("Witness"));
        Assert.Equal(64, Off<StorageNative.Capture>("Character"));
        Assert.Equal(80, Off<StorageNative.Capture>("Player"));
        Assert.Equal(88, Off<StorageNative.Capture>("FirstEntry"));
        Assert.Equal(96, Off<StorageNative.Capture>("Reason"));
        Assert.Equal(16, Off<StorageNative.Batch>("SavesSeq"));
        Assert.Equal(24, Off<StorageNative.Batch>("Revision"));
        Assert.Equal(56, Off<StorageNative.Batch>("UnixMs"));
        Assert.Equal(64, Off<StorageNative.Batch>("WorldGuid"));
        Assert.Equal(16, Off<StorageNative.Witness>("Value"));
        Assert.Equal(56, Off<StorageNative.Witness>("Character"));
        Assert.Equal(72, Off<StorageNative.Witness>("Player"));
        Assert.Equal(76, Off<StorageNative.Witness>("Slot"));
        Assert.Equal(24, Off<StorageNative.Event>("Player"));
        Assert.Equal(56, Off<StorageNative.Event>("Slot"));
        Assert.Equal(64, Off<StorageNative.Event>("Revision"));
        Assert.Equal(80, Off<StorageNative.Event>("Tag"));
        Assert.Equal(96, Off<StorageNative.Event>("Character"));
        Assert.Equal(112, Off<StorageNative.Event>("Key"));
        Assert.Equal(7, Off<StorageNative.Status>("StagePoint"));
        Assert.Equal(8, Off<StorageNative.Status>("Reason"));
        Assert.Equal(32, Off<StorageNative.Status>("Captures"));
        Assert.Equal(48, Off<StorageNative.Status>("Detail"));
        Assert.Equal(8, Off<StorageNative.ReturnResult>("Ticket"));
        Assert.Equal(48, Off<StorageNative.ReturnResult>("Message"));
    }

    [Fact]
    public void TheKilnItemsValuesDoNotCollideWithKilnItems()
    {
        Assert.Equal(16, (int)InventoryKinds.PlayerStorage);
        Assert.Equal(StorageNative.FindPlayerStorage, (uint)InventoryKinds.PlayerStorage);
        Assert.Equal(0, (int)(InventoryKinds.All & InventoryKinds.PlayerStorage));
        Assert.Equal(StorageNative.ItemsFiltered, (uint)ItemsStatus.Filtered);
    }
}
