// SPDX-License-Identifier: MIT
using Xunit;
namespace Kiln.TestKit.Tests;
[Collection("UiInventoryFake")]
public sealed class UiNativeTintsTests
{
    [Fact]
    public void RulesSeeCopiedStackIdentityAndComposeThenDispose()
    {
        using var h = new KilnHarness(); h.Step();
        var slot = new FakeInventorySlot(0xAA, 60, InventoryPage: 0x5000, SlotIndex: 3);
        UiItemView seen = default;
        using var a = h.Run(() => Ui.Inventory.TintWhen(v => { seen = v; return new Color(1, 2, 3, 4); }));
        using var pass = h.Run(() => Ui.Inventory.TintWhen(_ => null));
        Assert.Equal(new Color(1, 2, 3, 4), h.UiNativeTints.TintOf(slot, Color.White));
        Assert.Equal((new ItemId(0xAA), new EntityId(0x5000), (byte)3, 60u), (seen.Item, seen.InventoryPage, seen.SlotIndex, seen.Count));
        Assert.Equal(EntityId.None, seen.ClientEntity);
        Assert.Equal(4u, seen.Flags & 4);
        Assert.True(seen.UnitsPerCharge >= 1);
        using var b = h.Run(() => Ui.Inventory.TintWhen(_ => Color.Red, new ItemId(0xAA)));
        Assert.Equal(Color.Red, h.UiNativeTints.TintOf(slot, Color.White));
        b.Dispose(); b.Dispose();
        Assert.Equal(new Color(1, 2, 3, 4), h.UiNativeTints.TintOf(slot, Color.White));
        a.Dispose();
        Assert.Equal(Color.White, h.UiNativeTints.TintOf(slot, Color.White));
    }
    [Fact]
    public void ItemFiltersLeaveOtherSlotsAndEmptySlotsAlone()
    {
        using var h = new KilnHarness(); h.Step();
        using var r = h.Run(() => Ui.Inventory.TintWhen(_ => Color.Red, new ItemId(0xAA)));
        Assert.Equal(Color.White, h.UiNativeTints.TintOf(new FakeInventorySlot(0xBB), Color.White));
        Assert.Equal(Color.White, h.UiNativeTints.TintOf(default, Color.White));
    }
    [Fact]
    public void FiveConsecutiveFaultsStopTheRuleWithoutChangingTheColour()
    {
        using var h = new KilnHarness(); h.Step(); var calls = 0;
        var before = RegistrationCount();
        using var r = h.Run(() => Ui.Inventory.TintWhen(_ => { calls++; throw new InvalidOperationException("tint test"); }));
        for (var i = 0; i < 8; i++) Assert.Equal(Color.White, h.UiNativeTints.TintOf(new FakeInventorySlot(1), Color.White));
        Assert.Equal(5, calls);
        Assert.Equal(before, RegistrationCount());
    }
    [Fact]
    public void ArgumentsAndUnavailableCapabilityFailClosed()
    {
        using var h = new KilnHarness(); h.Step();
        Assert.Throws<ArgumentNullException>(() => h.Run(() => Ui.Inventory.TintWhen(null!)));
        h.Capabilities.Override(Ui.Inventory.TintCapability, false);
        Assert.Throws<KilnUnavailableException>(() => h.Run(() => Ui.Inventory.TintWhen(_ => null)));
    }
    [Fact]
    public unsafe void TintRegistrationsShareThePerOwnerValueBudget()
    {
        using var h = new KilnHarness(); h.Step();
        var t = (TintTable*)KitBackend.Instance.Service("kiln.ui.tints", 1);
        var values = (UiValuesAbi.Table*)KitBackend.Instance.Service("kiln.ui.values", 1);
        UiValuesAbi.Registration tint = new() { Size = (uint)sizeof(UiValuesAbi.Registration) };
        for (var i = 0; i < 128; i++)
        {
            UiValuesAbi.Registration value = new() { Size = (uint)sizeof(UiValuesAbi.Registration) };
            Assert.Equal(0, values->Subscribe(700, 1, 0, (void*)1, null, &value));
            Assert.Equal(1u, value.Ok);
        }
        t->Subscribe(700, 0, &QuotaTint, null, &tint);
        Assert.Equal(0u, tint.Ok);
        for (var i = 0; i < 65; i++)
        {
            t->Subscribe(701, 0, &QuotaTint, null, &tint); Assert.Equal(1u, tint.Ok);
            t->Subscribe(702, 0, &QuotaTint, null, &tint); Assert.Equal(1u, tint.Ok);
        }
    }
    [System.Runtime.InteropServices.UnmanagedCallersOnly(CallConvs = [typeof(System.Runtime.CompilerServices.CallConvCdecl)])]
    private static unsafe int QuotaTint(UiValuesAbi.ItemView* v, uint* rgba, void* user) => 0;

    private static int RegistrationCount() => ((System.Collections.ICollection)typeof(Ui).Assembly.GetType("Kiln.UiNativeTints")!
        .GetField("Rules", System.Reflection.BindingFlags.Static | System.Reflection.BindingFlags.NonPublic)!.GetValue(null)!).Count;
    private unsafe struct TintTable
    {
        public uint Size, Version;
        public delegate* unmanaged[Cdecl]<ulong, uint, delegate* unmanaged[Cdecl]<UiValuesAbi.ItemView*, uint*, void*, int>, void*, UiValuesAbi.Registration*, int> Subscribe;
    }

}
