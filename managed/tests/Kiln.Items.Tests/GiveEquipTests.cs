// SPDX-License-Identifier: MIT
using System.Runtime.InteropServices;
using Kiln;
using Kiln.TestKit;
using Xunit;

namespace Kiln.ItemsTests;

public sealed unsafe class GiveEquipTests
{
    [Fact]
    public void AbiAppendPreservesExistingTableAndResult()
    {
        Assert.Equal(392, sizeof(ItemsNative.TableWithGiveEquip));
        Assert.Equal(384, (int)Marshal.OffsetOf<ItemsNative.TableWithGiveEquip>(nameof(ItemsNative.TableWithGiveEquip.GiveEquip)));
        Assert.Equal(24, sizeof(ItemsNative.GiveEquipRequest));
        Assert.Equal(256, sizeof(ItemsNative.EquipResult));
        Assert.Equal(80, (int)Marshal.OffsetOf<ItemsNative.EquipResult>(nameof(ItemsNative.EquipResult.Reserved)));
        ItemsNative.EquipResult n = default;
        Assert.Equal(0, Items.ToResult(n).ReplacedCount); // ordinary Equip never writes the new field
        n.Reserved[0] = 15;
        n.Replaced = 42;
        var r = Items.ToResult(n);
        Assert.Equal(15, r.ReplacedCount);
        Assert.Equal(new ItemId(42), r.ReplacedItem);
    }

    [Fact]
    public void FullBackpackAllowsEmptyAndTakeButRefusesBackpackReplacement()
    {
        using var kiln = new KilnHarness();
        var player = kiln.Players.Join("Player", 76561198000000952, position: new WorldPosition(0, 0, 0));
        var ring = kiln.Items.AddItem("TestRing", 1, ItemCategory.Equipment, gear: true);
        kiln.Items.SetEquipmentSlotType(ring, EquipmentSlot.Ring0);
        var wood = kiln.Items.AddItem("Wood", 1000, ItemCategory.Materials);
        kiln.Items.PlayerInventories = 1;
        kiln.Step(2);
        kiln.Items.Of(player.Entity)!.Put(wood, 8000);
        var changes = 0;
        using var sub = kiln.Run(() => Events.On<EquipChangedEvent>(_ => changes++));
        Task<EquipResult> Give(GiveEquipOptions? o = null) => kiln.Run(() => Items.GiveEquip(player.Entity, ring, 1, EquipmentSlot.Ring0, o));
        var empty = Give();
        Assert.False(empty.IsCompleted);
        kiln.Step();
        Assert.True(empty.Result.Ok);
        Assert.True(empty.Result.From.IsNone);
        Assert.False(empty.Result.To.IsNone);
        var refused = Give(); kiln.Step();
        Assert.Equal(ItemsStatus.NoRoom, refused.Result.Status);
        var taken = Give(new() { Replaced = Replaced.Take }); kiln.Step();
        Assert.True(taken.Result.Ok);
        Assert.Equal(ring, taken.Result.ReplacedItem);
        Assert.Equal(1, taken.Result.ReplacedCount);
        Assert.True(taken.Result.ReplacedTo.IsNone);
        kiln.Step();
        Assert.Equal(2, changes);
    }

    [Fact]
    public void DryRunAndWrongSlotDoNotReplaceTheEquippedItem()
    {
        using var kiln = new KilnHarness();
        var player = kiln.Players.Join("Player", 76561198000000953, position: new WorldPosition(0, 0, 0));
        var ring = kiln.Items.AddItem("Ring", 1, ItemCategory.Equipment, gear: true);
        kiln.Items.SetEquipmentSlotType(ring, EquipmentSlot.Ring0);
        kiln.Step(2);
        var initial = kiln.Run(() => Items.GiveEquip(player.Entity, ring, 1, EquipmentSlot.Ring0)); kiln.Step();
        var dry = kiln.Run(() => Items.GiveEquip(player.Entity, ring, 1, EquipmentSlot.Ring0, new() { Replaced = Replaced.Take, DryRun = true })); kiln.Step();
        Assert.True(dry.Result.Ok);
        Assert.True(dry.Result.DryRun);
        Assert.Equal(initial.Result.ItemEntity, dry.Result.ReplacedEntity);
        var wrong = kiln.Run(() => Items.GiveEquip(player.Entity, ring, 1, EquipmentSlot.Shield)); kiln.Step();
        Assert.Equal(ItemsStatus.Refused, wrong.Result.Status);
        var take = kiln.Run(() => Items.GiveEquip(player.Entity, ring, 1, EquipmentSlot.Ring0, new() { Replaced = Replaced.Take })); kiln.Step();
        Assert.Equal(initial.Result.ItemEntity, take.Result.ReplacedEntity);
    }

    [Fact]
    public void InvalidArgumentsAndOfflineCapabilityRefuse()
    {
        using var kiln = new KilnHarness();
        kiln.Run(() =>
        {
            Assert.Throws<ArgumentException>(() => { _ = Items.GiveEquip(default, new ItemId(1), 1, EquipmentSlot.Ring0); });
            Assert.Throws<ArgumentOutOfRangeException>(() => { _ = Items.GiveEquip(new EntityId(1), new ItemId(1), 0, EquipmentSlot.Ring0); });
        });
        kiln.Capabilities.Override("items.give_equip", false);
        kiln.Run(() => Assert.Throws<KilnUnavailableException>(() => { _ = Items.GiveEquip(new EntityId(1), new ItemId(1), 1, EquipmentSlot.Ring0); }));
    }
    [Fact]
    public void CapabilityRefusalNamesTheUnavailableDependency()
    {
        using var kiln = new KilnHarness();
        kiln.Step(2);
        Assert.True(kiln.Run(() => Items.GiveEquipAvailable));
        kiln.Capabilities.Override("items.transaction", false, "transactions disabled by the test");
        Assert.False(kiln.Run(() => Items.GiveEquipAvailable));
        Assert.Contains("items.transaction", kiln.Run(() => Capabilities.Why("items.give_equip")));
    }

    [Fact]
    public void ADeadPlayerIsRefusedWithoutCreatingEquipment()
    {
        using var kiln = new KilnHarness();
        var player = kiln.Players.Join("Player", 76561198000000954, position: new WorldPosition(0, 0, 0));
        var ring = kiln.Items.AddItem("Ring", 1, ItemCategory.Equipment, gear: true);
        kiln.Items.SetEquipmentSlotType(ring, EquipmentSlot.Ring0);
        kiln.Step(2);
        kiln.Combat.SetHealth(player.Entity, 0, 100);
        var task = kiln.Run(() => Items.GiveEquip(player.Entity, ring, 1, EquipmentSlot.Ring0));
        kiln.Step();
        Assert.Equal(ItemsStatus.Refused, task.Result.Status);
        Assert.Contains("dead", task.Result.Reason);
        Assert.True(task.Result.ItemEntity.IsNone);
    }

    // #139: arrows, spells and bait are selected in place in a backpack cell, never in a spare equipment cell.
    [Fact]
    public void AmmoNeedsTheAmmunitionCategoryAndAFreeBackpackCell()
    {
        using var kiln = new KilnHarness();
        var player = kiln.Players.Join("Player", 76561198000000955, position: new WorldPosition(0, 0, 0));
        var spell = kiln.Items.AddItem("Spell", 99, ItemCategory.Ammunition);
        kiln.Items.SetEquipmentSlotType(spell, EquipmentSlot.Spell);
        var notAmmo = kiln.Items.AddItem("SpellShapedMaterial", 99, ItemCategory.Materials);
        kiln.Items.SetEquipmentSlotType(notAmmo, EquipmentSlot.Spell);
        var wood = kiln.Items.AddItem("Wood", 1000, ItemCategory.Materials);
        kiln.Items.PlayerInventories = 1;
        kiln.Step(2);
        Task<EquipResult> Give(ItemId item, int count = 50, GiveEquipOptions? o = null) =>
            kiln.Run(() => Items.GiveEquip(player.Entity, item, count, EquipmentSlot.Spell, o));
        var wrong = Give(notAmmo); kiln.Step();
        Assert.Equal(ItemsStatus.Refused, wrong.Result.Status);
        Assert.Contains("Ammunition", wrong.Result.Reason);
        var inventory = kiln.Items.Of(player.Entity)!;
        inventory.Put(wood, 8000);
        var full = Give(spell, o: new() { Replaced = Replaced.Take }); kiln.Step();
        Assert.Equal(ItemsStatus.NoRoom, full.Result.Status);
        inventory.Clear();
        var given = Give(spell); kiln.Step();
        Assert.True(given.Result.Ok);
        Assert.Equal(given.Result.To, inventory.Slots.Single(s => s.Item == spell).Slot);
        Assert.Equal(50, inventory.Count(spell));
    }

    // #139: an eternal spell is an item entity. GiveEquip creates it once in its backpack cell and keeps entities apart.
    [Fact]
    public void EternalSpellsAreCreatedWithTheirEntityAndReplacedEntitiesKeepTheirIdentity()
    {
        using var kiln = new KilnHarness();
        var player = kiln.Players.Join("Player", 76561198000000956, position: new WorldPosition(0, 0, 0));
        var eternal = kiln.Items.AddItem("EternalPillarOfFlames", 1, ItemCategory.Ammunition, gear: true);
        var other = kiln.Items.AddItem("EternalIceBolt", 1, ItemCategory.Ammunition, gear: true);
        var plain = kiln.Items.AddItem("Fireball", 99, ItemCategory.Ammunition);
        foreach (var item in new[] { eternal, other, plain }) kiln.Items.SetEquipmentSlotType(item, EquipmentSlot.Spell);
        kiln.Items.PlayerInventories = 1;
        kiln.Step(2);
        var inventory = kiln.Items.Of(player.Entity)!;
        Task<EquipResult> Give(ItemId item, int count = 1, GiveEquipOptions? o = null) =>
            kiln.Run(() => Items.GiveEquip(player.Entity, item, count, EquipmentSlot.Spell, o));
        var two = Give(eternal, 2); kiln.Step();
        Assert.Equal(ItemsStatus.Refused, two.Result.Status);
        var first = Give(eternal); kiln.Step();
        Assert.True(first.Result.Ok);
        Assert.False(first.Result.ItemEntity.IsNone);
        var cell = inventory.Slots.Single(s => s.Item == eternal);
        Assert.Equal(first.Result.To, cell.Slot);
        Assert.Equal(first.Result.ItemEntity, cell.ItemEntity);
        var dry = Give(other, o: new() { DryRun = true }); kiln.Step();
        Assert.True(dry.Result.Ok);
        Assert.Equal(first.Result.ItemEntity, dry.Result.ReplacedEntity);
        Assert.Equal(0, inventory.Count(other));
        var second = Give(other); kiln.Step();
        Assert.True(second.Result.Ok);
        Assert.Equal(first.Result.To, second.Result.To);
        Assert.Equal(first.Result.ItemEntity, second.Result.ReplacedEntity);
        var kept = inventory.Slots.Single(s => s.Item == eternal);
        Assert.Equal(second.Result.ReplacedTo, kept.Slot);
        Assert.Equal(first.Result.ItemEntity, kept.ItemEntity);
        var taken = Give(plain, 20, new() { Replaced = Replaced.Take }); kiln.Step();
        Assert.True(taken.Result.Ok);
        Assert.Equal(second.Result.ItemEntity, taken.Result.ReplacedEntity);
        Assert.Equal(0, inventory.Count(other));
        Assert.Equal(20, inventory.Count(plain));
        Assert.Equal(1, inventory.Count(eternal));
    }

    [Fact]
    public void WithoutTheInstanceAmmoFeatureEternalSpellsAreRefusedAndPlainAmmoIsNot()
    {
        using var kiln = new KilnHarness();
        var player = kiln.Players.Join("Player", 76561198000000957, position: new WorldPosition(0, 0, 0));
        var eternal = kiln.Items.AddItem("EternalSpell", 1, ItemCategory.Ammunition, gear: true);
        var plain = kiln.Items.AddItem("Spell", 99, ItemCategory.Ammunition);
        kiln.Items.SetEquipmentSlotType(eternal, EquipmentSlot.Spell);
        kiln.Items.SetEquipmentSlotType(plain, EquipmentSlot.Spell);
        kiln.Items.PlayerInventories = 1;
        kiln.Capabilities.Override("items.give_equip.instance_ammo", false, "not resolved on this build");
        kiln.Step(2);
        var eternalGive = kiln.Run(() => Items.GiveEquip(player.Entity, eternal, 1, EquipmentSlot.Spell)); kiln.Step();
        Assert.Equal(ItemsStatus.Refused, eternalGive.Result.Status);
        Assert.Contains("items.give_equip.instance_ammo", eternalGive.Result.Reason);
        Assert.Equal(0, kiln.Items.Of(player.Entity)!.Count(eternal));
        var plainGive = kiln.Run(() => Items.GiveEquip(player.Entity, plain, 5, EquipmentSlot.Spell)); kiln.Step();
        Assert.True(plainGive.Result.Ok);
    }

    [Fact]
    public void AJoinedClientHasNoAuthoritativeGiveEquip()
    {
        using var kiln = new KilnHarness(new KilnHarnessOptions { Side = HarnessSide.JoinedClient });
        kiln.Step(2);
        Assert.False(kiln.Run(() => Items.GiveEquipAvailable));
        kiln.Run(() => Assert.Throws<KilnUnavailableException>(() =>
        {
            _ = Items.GiveEquip(new EntityId(1), new ItemId(1), 1, EquipmentSlot.Ring0);
        }));
    }

}
