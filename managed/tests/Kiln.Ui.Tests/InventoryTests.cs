// SPDX-License-Identifier: MIT
// Ui.Inventory (0.2.0-dev.30) against a fake kiln.ui.inventory table: OnClick's filter and arguments, the click record
// made into a SlotClick (the item's identity), unsubscribe on Dispose and on the mod's retirement, the error rule, Hovered,
// and the marks' lookup (item marks before rules). The native press/hit-test/delivery rules are the host test's:
// native/core/tests/ui_values_test.cpp (S5b).
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using Kiln;
using Kiln.FakeUi;
using UiValuesTestSuite;
using Xunit;

namespace UiInventoryTestSuite;

internal static unsafe class FakeInventory
{
    public static UiInventoryNative.Table* Table;
    public static readonly List<(ulong Owner, UiInventoryNative.ClickFilter Filter, nint User)> Subscribed = [];
    public static readonly List<ulong> Unsubscribed = [];
    public static nint ClickFn;
    public static int SubscribeRc;
    public static uint HitOk;
    public static UiValuesNative.VisibleSlot HitSlot;
    public static ulong NextHandle = 500;

    public static void Install()
    {
        Free();
        Table = (UiInventoryNative.Table*)NativeMemory.AllocZeroed((nuint)sizeof(UiInventoryNative.Table));
        Table->Header = new ServiceHeader { Size = (uint)sizeof(UiInventoryNative.Table), Version = 1 };
        Table->SubscribeClicks = &SubscribeClicks;
        Table->SlotAt = &SlotAt;
        Table->GetMapping = &GetMapping;
        Table->Unsubscribe = &Unsubscribe;
        Subscribed.Clear();
        Unsubscribed.Clear();
        ClickFn = 0;
        SubscribeRc = 0;
        HitOk = 0;
        HitSlot = default;
        NextHandle = 500;
    }

    public static void Free()
    {
        if (Table != null) NativeMemory.Free(Table);
        Table = null;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int SubscribeClicks(ulong owner, UiInventoryNative.ClickFilter* f,
        delegate* unmanaged[Cdecl]<UiInventoryNative.Click*, void*, void> fn, void* user, UiValuesNative.Registration* r)
    {
        Subscribed.Add((owner, *f, (nint)user));
        ClickFn = (nint)fn;
        if (SubscribeRc != 0) return SubscribeRc;
        r->Ok = 1;
        r->Handle = NextHandle++;
        return 0;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int SlotAt(ulong owner, float x, float y, float w, float h, UiInventoryNative.Hit* out_)
    {
        out_->Ok = HitOk;
        out_->Slot = HitSlot;
        return 0;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int GetMapping(ulong owner, float w, float h, UiInventoryNative.Mapping* out_)
    {
        out_->Ok = 0;
        return 0;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Unsubscribe(ulong h)
    {
        Unsubscribed.Add(h);
        return 0;
    }

    public sealed class Backend(HashSet<string> features, bool withTable = true) : IKilnBackend
    {
        public string Version => "test";
        public bool IsTickThread => true;
        public bool HasFeature(string name) => features.Contains(name);
        public void Log(ulong owner, LogLevel level, string text) { }
        public nint Service(string name, uint minVersion) => name switch
        {
            UiInventoryNative.Service => withTable ? (nint)Table : 0,
            UiValuesNative.Service => (nint)FakeUiValues.Table,
            "kiln.ui" => (nint)FakeUiTable.Table,
            _ => 0,
        };
        public ulong OwnerCreate(string id) => 99;
    }
}

[Collection(UiSerial.Name)]
public sealed unsafe class InventoryTests : IDisposable
{
    private readonly IKilnBackend _previous;
    private static readonly HashSet<string> Rows = ["ui.slots.inspect", "ui.slots.click"];

    public InventoryTests()
    {
        _previous = Backend.Current;
        FakeUiValues.Install();
        FakeInventory.Install();
        Use(new FakeInventory.Backend(Rows));
    }

    private static void Use(IKilnBackend b)
    {
        Backend.Current = b;
        UiValuesNative.ResetForTests();
        UiInventoryBridge.ResetForTests();
        Events.Reset();
    }

    public void Dispose()
    {
        UiInventoryBridge.ResetForTests();
        Backend.Current = _previous;
        UiValuesNative.ResetForTests();
        FakeInventory.Free();
        FakeUiValues.Free();
        Events.Reset();
    }

    private static UiValuesNative.VisibleSlot Slot(uint item, uint pide, uint page = 0x5000, uint index = 2)
    {
        var s = new UiValuesNative.VisibleSlot
        {
            Size = (uint)sizeof(UiValuesNative.VisibleSlot), Available = UiValuesNative.OuterRect, InventoryPage = page,
            SlotIndex = index, Item = item, Count = 1, ClientEntity = pide, ViewFlags = 1 | 4, Holder = 77, HolderFlags = 3,
            OuterUi = new UiValuesNative.Rect { X = 10, Y = 20, Width = 80, Height = 80 },
        };
        return s;
    }

    private static void Deliver(nint user, uint button, uint mods, UiValuesNative.VisibleSlot slot, uint taken = 1)
    {
        var c = new UiInventoryNative.Click
        {
            Size = (uint)sizeof(UiInventoryNative.Click), Button = button, Mods = mods, Taken = taken, Slot = slot,
            Screen = new UiNative.Screen { Size = (uint)sizeof(UiNative.Screen), UiState = 7, HudScreen = 22 },
        };
        var fn = (delegate* unmanaged[Cdecl]<UiInventoryNative.Click*, void*, void>)FakeInventory.ClickFn;
        fn(&c, (void*)user);
    }

    [Fact]
    public void OnClick_registers_its_filter_and_hands_the_item_identity_to_the_handler()
    {
        var got = new List<SlotClick>();
        using var reg = Ui.Inventory.OnClick(new SlotClickOptions { Modifiers = SlotModifiers.Ctrl }, got.Add);
        var (_, f, user) = Assert.Single(FakeInventory.Subscribed);
        Assert.Equal((uint)1, f.Button);
        Assert.Equal((uint)SlotModifiers.Ctrl, f.Mods);
        Assert.Equal(UiInventoryNative.ClickTake, f.Flags);
        Deliver(user, 1, 1, Slot(0xAA, 900));
        var c = Assert.Single(got);
        Assert.Equal(new EntityId(900), c.ItemEntity);
        Assert.Equal(new ItemId(0xAA), c.Item);
        Assert.Equal(new EntityId(77), c.Holder);
        Assert.Equal(new EntityId(0x5000), c.Slot.InventoryPage);
        Assert.Equal(2, c.Slot.SlotIndex);
        Assert.Equal(SlotButton.Left, c.Button);
        Assert.Equal(SlotModifiers.Ctrl, c.Modifiers);
        Assert.True(c.Taken);
        Assert.Equal(HudScreen.StorageChest, c.Screen.Hud);
    }

    [Fact]
    public void Watching_clicks_does_not_take_them_and_Dispose_unsubscribes()
    {
        var reg = Ui.Inventory.OnClick(new SlotClickOptions { Button = SlotButton.Right, Take = false }, _ => { });
        var (_, f, _) = Assert.Single(FakeInventory.Subscribed);
        Assert.Equal((uint)3, f.Button);
        Assert.Equal(0u, f.Flags);
        reg.Dispose();
        reg.Dispose();
        Assert.Equal([500UL], FakeInventory.Unsubscribed.Where(h => h == 500).ToArray());
    }

    [Fact]
    public void A_click_after_Dispose_reaches_nobody()
    {
        var got = new List<SlotClick>();
        var reg = Ui.Inventory.OnClick(new SlotClickOptions(), got.Add);
        var user = FakeInventory.Subscribed[0].User;
        reg.Dispose();
        Deliver(user, 1, 0, Slot(0xAA, 900));
        Assert.Empty(got);
    }

    [Fact]
    public void Bad_options_are_ArgumentExceptions()
    {
        Assert.Throws<ArgumentException>(() => Ui.Inventory.OnClick(new SlotClickOptions { Button = (SlotButton)2 }, _ => { }));
        Assert.Throws<ArgumentException>(() =>
            Ui.Inventory.OnClick(new SlotClickOptions { Modifiers = (SlotModifiers)8 }, _ => { }));
        Assert.Throws<ArgumentNullException>(() => Ui.Inventory.OnClick(new SlotClickOptions(), null!));
        Assert.Throws<ArgumentException>(() => Ui.Inventory.Mark(default, new SlotMark(Color.Red)));
        Assert.Throws<ArgumentException>(() => Ui.Inventory.Mark(new EntityId(5), new SlotMark(Color.Red, Thickness: float.NaN)));
        Assert.Empty(FakeInventory.Subscribed);
    }

    [Fact]
    public void A_missing_table_or_row_is_KilnUnavailable()
    {
        Use(new FakeInventory.Backend(Rows, withTable: false));
        Assert.Throws<KilnUnavailableException>(() => Ui.Inventory.OnClick(new SlotClickOptions(), _ => { }));
        Use(new FakeInventory.Backend(["ui.slots.inspect"]));
        Assert.Throws<KilnUnavailableException>(() => Ui.Inventory.OnClick(new SlotClickOptions(), _ => { }));
    }

    [Fact]
    public void A_refused_native_subscribe_throws_and_keeps_nothing()
    {
        FakeInventory.SubscribeRc = UiValuesNative.Unavailable;
        Assert.Throws<KilnUnavailableException>(() => Ui.Inventory.OnClick(new SlotClickOptions(), _ => { }));
    }

    [Fact]
    public void A_retiring_mod_unsubscribes_its_clicks()
    {
        var mod = HostAccess.CreateContext(new ModInfo("inventory-unload", "Unload", "1.0.0", Path.GetTempPath()), 72,
            Path.GetTempPath());
        var previous = HostAccess.Current;
        HostAccess.Current = mod;
        try { Ui.Inventory.OnClick(new SlotClickOptions(), _ => { }); }
        finally { HostAccess.Current = previous; }
        mod.Retire();
        Assert.Contains(500UL, FakeInventory.Unsubscribed);
    }

    [Fact]
    public void Hovered_is_the_slot_under_the_cursor_or_null()
    {
        Assert.Null(Ui.Inventory.Hovered);
        FakeInventory.HitOk = 1;
        FakeInventory.HitSlot = Slot(0xBB, 901);
        Assert.Equal(new EntityId(901), Ui.Inventory.Hovered!.ClientEntity);
    }

    [Fact]
    public void An_item_mark_comes_before_the_rules_and_a_rule_marks_the_rest()
    {
        var red = new SlotMark(Color.Red);
        var tint = new SlotMark(Color.Yellow.WithAlpha(80), SlotMarkStyle.Fill);
        Ui.Inventory.Mark(new EntityId(900), red);
        using var rule = Ui.Inventory.MarkWhen(s => s.Item.Value == 0xCC ? tint : null);
        var rules = UiInventoryBridge.Find(UiInventoryBridge.ToVisible(Slot(0xAA, 900)), UiInventoryBridge.ByItem(),
            [(s => s.Item.Value == 0xAA ? tint : null, null)]);
        Assert.Equal(red, rules);
        var byItem = UiInventoryBridge.ByItem();
        Assert.Equal(tint, UiInventoryBridge.Find(UiInventoryBridge.ToVisible(Slot(0xCC, 0)), byItem,
            [(s => s.Item.Value == 0xCC ? tint : null, null)]));
        Assert.Null(UiInventoryBridge.Find(UiInventoryBridge.ToVisible(Slot(0xDD, 902)), byItem, []));
        Ui.Inventory.Unmark(new EntityId(900));
        Assert.Empty(UiInventoryBridge.ByItem());
    }

    [Fact]
    public void A_throwing_rule_marks_nothing()
    {
        var mark = UiInventoryBridge.Find(UiInventoryBridge.ToVisible(Slot(0xAA, 0)), [],
            [(_ => throw new InvalidOperationException("rule"), null)]);
        Assert.Null(mark);
    }
}
