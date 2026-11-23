// SPDX-License-Identifier: MIT
// Items.FindInventoryOwners (0.2.0-dev.31, GitHub issue #138) on the kit's kiln.items.inventory_kinds: the owners
// FindInventories finds (unchanged), each with its kind; an ordinary chest is Unknown; the flags and the player storage
// opt-in; codes this build does not know; the capability and the thread rule. The kinds' native rules and the table's
// buffer semantics are native/services/items/tests/inventory_kinds_test.cpp.
using Kiln;
using Kiln.TestKit;
using Xunit;

namespace Kiln.ItemsTests;

public sealed class InventoryOwnersTests
{
    private static readonly ulong AdaSteam = 76561198000000101;

    [Fact]
    public void Owners_are_FindInventories_with_their_kinds()
    {
        using var kiln = new KilnHarness();
        var ada = kiln.Players.Join("Ada", AdaSteam, position: new WorldPosition(0, 0, 0));
        kiln.Step();
        var twigs = kiln.Items.AddItem("Material_T1_Twigs", 50);
        var chest = kiln.Entities.Create(new WorldPosition(2, 0, 0));
        kiln.Items.AddContainer(chest).Put(twigs, 3);
        var forge = kiln.Entities.Create(new WorldPosition(3, 0, 0));
        kiln.Items.AddContainer(forge).Kind = InventoryOwnerKind.ProductionStation;
        var magic = kiln.Entities.Create(new WorldPosition(4, 0, 0));
        var m = kiln.Items.AddContainer(magic);
        m.Kind = InventoryOwnerKind.BaseStorage;
        Assert.Equal(InventoryOwnerKind.BaseStorage, m.Kind);
        ada.Inventory.Put(twigs, 1);

        foreach (var kinds in new[] { InventoryKinds.Containers, InventoryKinds.All, InventoryKinds.Players, InventoryKinds.NonEmpty,
                     InventoryKinds.Containers | InventoryKinds.NonEmpty, (InventoryKinds)0 })
        {
            var owners = kiln.Run(() => Items.FindInventoryOwners(kinds));
            Assert.Equal(kiln.Run(() => Items.FindInventories(kinds)), owners.Select(o => o.Entity));  // the same entities, same order
        }
        var all = kiln.Run(() => Items.FindInventoryOwners(InventoryKinds.All)).ToDictionary(o => o.Entity, o => o.Kind);
        Assert.Equal(InventoryOwnerKind.Player, all[ada.Entity]);
        Assert.Equal(InventoryOwnerKind.Unknown, all[chest]);  // an ordinary chest: nothing proves what it is
        Assert.Equal(InventoryOwnerKind.ProductionStation, all[forge]);
        Assert.Equal(InventoryOwnerKind.BaseStorage, all[magic]);
        Assert.Equal([new InventoryOwnerInfo(chest, InventoryOwnerKind.Unknown)],
            kiln.Run(() => Items.FindInventoryOwners(InventoryKinds.Containers | InventoryKinds.NonEmpty)));
        // the default: containers, as FindInventories
        Assert.Equal(3, kiln.Run(() => Items.FindInventoryOwners()).Count);
        Assert.DoesNotContain(kiln.Run(() => Items.FindInventoryOwners()), o => o.Entity == ada.Entity);
        // nothing changed for FindInventories
        Assert.Equal([ada.Entity, chest], kiln.Run(() => Items.FindInventories(InventoryKinds.NonEmpty)));
        // a flag kiln.items does not know: invalid argument, as FindInventories
        Assert.ThrowsAny<Exception>(() => kiln.Run(() => Items.FindInventoryOwners((InventoryKinds)32)));
        Assert.True(kiln.Run(() => Capabilities.Is(Items.InventoryKindsCapability)));
    }

    [Fact]
    public void Codes_this_build_does_not_know_read_as_Unknown()
    {
        Assert.Equal(InventoryOwnerKind.BaseStorage, Items.KindOf(5));
        Assert.Equal(InventoryOwnerKind.Unknown, Items.KindOf(6));
        Assert.Equal(InventoryOwnerKind.Unknown, Items.KindOf(uint.MaxValue));
        // the stable codes (kiln_items_inventory_kinds.h)
        Assert.Equal([0u, 1u, 2u, 3u, 4u, 5u], Enum.GetValues<InventoryOwnerKind>().Select(k => (uint)k));
    }

    [Fact]
    public unsafe void The_mirror_is_the_approved_layout()
    {
        Assert.Equal(16, sizeof(InventoryKindsNative.Table));
        Assert.Equal(8, (int)System.Runtime.InteropServices.Marshal.OffsetOf<InventoryKindsNative.Table>(nameof(InventoryKindsNative.Table.Find)));
        Assert.Equal(8, sizeof(InventoryKindsNative.Row));
        Assert.Equal(4, (int)System.Runtime.InteropServices.Marshal.OffsetOf<InventoryKindsNative.Row>(nameof(InventoryKindsNative.Row.Kind)));
    }

    [Fact]
    public void Off_the_tick_thread_it_throws()
    {
        using var kiln = new KilnHarness();
        kiln.Step();
        Assert.Throws<InvalidOperationException>(() => Items.FindInventoryOwners());
    }
}
