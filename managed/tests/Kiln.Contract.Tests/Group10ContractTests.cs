// SPDX-License-Identifier: MIT
using Xunit;

namespace Kiln.Contract.Tests;

/// <summary>Kiln.Items by the contract (it followed it since dev.26; dev.27 adds the rows' restart, the unavailable
/// errors of its table versions and the thread marks).</summary>
public sealed class ItemsContractTests : ContractTest
{
    [Fact]
    public void With_no_service_every_row_is_off_and_the_rows_follow_a_new_registry()
    {
        StartRows(typeof(Items).Assembly);   // a new binding: the rows are reported again (dev.26 reported them once a process)
        AssertOffline("kiln.items");
        Assert.Equal(CapabilityState.Unavailable, Capabilities.Get("items.result_peek").State);
        foreach (var name in (string[])["items.fuel", "items.move", "items.instances", "items.equip", "items.equip_sets", "items.pide_options"])
            Assert.False(Capabilities.Is(name), name);
    }

    [Fact]
    public void A_call_that_needs_the_service_throws_unavailable_naming_it()
    {
        AssertUnavailable("kiln.items", () => Items.Read(new EntityId(5000)));
        AssertUnavailable("kiln.items", () => Items.Fuel.Read(new EntityId(5000)));
        AssertUnavailable("kiln.items", () => Items.ContainerName(new EntityId(5000)));
    }

    [Fact]
    public void Equipment_sets_name_their_row_when_the_service_is_not_there()
    {
        // 0.2.0-dev.29 (kiln.items v6): no table, so no v6 members: each entry point throws naming items.equip_sets
        AssertUnavailable("items.equip_sets", () => Items.CaptureSet(new EntityId(1)));
        var set = new EquipmentSet(new EquipmentSetScope(Guid.NewGuid(), 1, new EventEntityKey(1, false, 0, 1, 0, 0)), []);
        AssertUnavailable("items.equip_sets", () => Items.PlanSet(new EntityId(1), set));
        AssertUnavailable("items.equip_sets", () => Items.EquipSet(new EntityId(1), set));
    }

    [Fact]
    public void A_refused_fuel_change_is_a_kiln_result()
    {
        IKilnResult r = new FuelResult(ItemsStatus.NoInventory, new EntityId(5), new ItemId(7), 0, 0, [], "no container", 0, false);
        Assert.False(r.Ok);
        Assert.Equal("no container", r.Reason);
    }

    [Fact]
    public void Members_carry_their_thread_and_capability()
    {
        AssertHas<TickThreadOnlyAttribute>(Method(typeof(Items), "Read", typeof(EntityId)));
        AssertHas<TickThreadOnlyAttribute>(Method(typeof(Items.Fuel), "Read", typeof(EntityId)));
        AssertHas<TickThreadOnlyAttribute>(Method(typeof(Items), "CaptureSet", typeof(EntityId)));
        AssertHas<ServerOnlyAttribute>(Method(typeof(Items), "PlanSet", typeof(EntityId), typeof(EquipmentSet)));
        AssertHas<RequiresCapabilityAttribute>(Method(typeof(Items), "EquipSet", typeof(EntityId), typeof(EquipmentSet), typeof(EquipOptions)));
    }
}

/// <summary>Kiln.Items on an older (v2) kiln.items table: each v3 member names its own row when it throws.</summary>
public sealed unsafe class ItemsOlderTableTests : FakeTableTest
{
    public ItemsOlderTableTests()
    {
        var t = Alloc<ItemsNative.TableV2>();
        t->V1.Header = new ServiceHeader { Size = (uint)sizeof(ItemsNative.TableV2), Version = ItemsNative.Version };
        Swap(typeof(Items), "_v3", Box<ItemsNative.TableV3>(null));
        Serve("kiln.items", (ItemsNative.Table*)t, typeof(Items), "_table");
    }

    [Fact]
    public void Names_and_transfers_name_their_own_row_not_moves()
    {
        AssertUnavailable("items.names", () => Items.ContainerName(new EntityId(5000)));
        AssertUnavailable("items.transfer", () => Items.Transfers.Deny(new EntityId(5000)));
        AssertUnavailable("items.move", () => Items.QuickStack(new EntityId(5000), new EntityId(5001)));
    }
}
