// SPDX-License-Identifier: MIT
// Ui.Inventory's screens (0.2.0-dev.30, slice 2): which backpack or chest screen is open with the chest's entity
// (GameScreen.Target, InventoryScreen.Of), the ScreenChanged events, AddPanel's rules (the row, kiln.ui.inventory v2, the
// screens a panel shows on, When) and where a panel docks beside the slots. The native screen view's rules are the host
// test's: native/core/tests/ui_values_test.cpp (S5c).
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using Kiln;
using Kiln.FakeUi;
using UiValuesTestSuite;
using Xunit;

namespace UiInventoryTestSuite;

[Collection(UiSerial.Name)]
public sealed unsafe class InventoryScreenTests : IDisposable
{
    private readonly IKilnBackend _previous;
    private static readonly HashSet<string> Rows = ["ui.slots.inspect", "ui.slots.click", "ui.inventory.panels"];
    private static UiInventoryNative.ScreenView _view;
    private static int _viewCalls;

    public InventoryScreenTests()
    {
        _previous = Backend.Current;
        FakeUiValues.Install();
        FakeInventory.Install();
        UpgradeToV2();
        Use(new FakeInventory.Backend(Rows));
        UiNative.ScreenTargetOverride = () => 4242;
        _view = default;
        _viewCalls = 0;
    }

    // kiln.ui.inventory v2: the v1 fake with screen_view appended
    private static void UpgradeToV2()
    {
        var v2 = (UiInventoryNative.TableV2*)NativeMemory.AllocZeroed((nuint)sizeof(UiInventoryNative.TableV2));
        v2->Base = *FakeInventory.Table;
        v2->Base.Header = new ServiceHeader { Size = (uint)sizeof(UiInventoryNative.TableV2), Version = 2 };
        v2->GetScreenView = &ScreenView;
        NativeMemory.Free(FakeInventory.Table);
        FakeInventory.Table = (UiInventoryNative.Table*)v2;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int ScreenView(ulong owner, float w, float h, UiInventoryNative.ScreenView* o)
    {
        _viewCalls++;
        var size = o->Size;
        *o = _view;
        o->Size = size;
        return 0;
    }

    private static void Use(IKilnBackend b)
    {
        Backend.Current = b;
        UiValuesNative.ResetForTests();
        UiInventoryBridge.ResetForTests();
        UiInventoryScreens.ResetForTests();
        Events.Reset();
    }

    public void Dispose()
    {
        UiInventoryScreens.ResetForTests();
        UiInventoryBridge.ResetForTests();
        UiNative.ScreenTargetOverride = null;
        Backend.Current = _previous;
        UiValuesNative.ResetForTests();
        FakeInventory.Free();
        FakeUiValues.Free();
        Events.Reset();
    }

    private static UiNative.Screen Native(HudScreen hud, byte crafting = 0, GameUiState state = GameUiState.Play) => new()
    {
        Size = (uint)Unsafe.SizeOf<UiNative.Screen>(), UiState = (byte)state, HudScreen = (byte)hud, Crafting = crafting, Available = 1,
    };

    private static GameScreen Screen(HudScreen hud, byte crafting = 0, GameUiState state = GameUiState.Play) =>
        GameScreen.From(Native(hud, crafting, state));

    [Fact]
    public void AChestScreenNamesItsEntityOtherScreensDoNot()
    {
        Assert.Equal(new EntityId(4242), Screen(HudScreen.StorageChest).Target);
        Assert.Equal(new EntityId(4242), Screen(HudScreen.CommerceProductChest).Target);
        Assert.Equal(new EntityId(4242), Screen(HudScreen.FactoryCrafting).Target);
        Assert.Equal(new EntityId(4242), Screen(HudScreen.FactoryCrafting).Crafting!.Value.Station);  // one read, both places
        Assert.Equal(EntityId.None, Screen(HudScreen.Backpack).Target);
        Assert.Equal(EntityId.None, Screen(HudScreen.InGame).Target);
        Assert.Equal(EntityId.None, Screen(HudScreen.StorageChest, state: GameUiState.PreGame).Target);
        UiNative.ScreenTargetOverride = () => 0;
        Assert.Equal(EntityId.None, Screen(HudScreen.StorageChest).Target);  // not named yet / not read on this build
    }

    [Fact]
    public void InventoryScreensAreTheBackpackAndChests()
    {
        Assert.Equal(new InventoryScreen(InventoryScreenKind.Backpack, HudScreen.Backpack, EntityId.None),
            InventoryScreen.Of(Screen(HudScreen.Backpack)));
        Assert.Equal(InventoryScreenKind.Backpack, InventoryScreen.Of(Screen(HudScreen.InGameMenu))?.Kind);
        Assert.Null(InventoryScreen.Of(Screen(HudScreen.InGameMenu, crafting: 1)));  // its crafting tab: Ui.Crafting
        Assert.Equal(new InventoryScreen(InventoryScreenKind.Chest, HudScreen.StorageChest, new EntityId(4242)),
            InventoryScreen.Of(Screen(HudScreen.StorageChest)));
        Assert.Equal(InventoryScreenKind.Chest, InventoryScreen.Of(Screen(HudScreen.CommerceProductChest))?.Kind);
        foreach (var other in new[] { HudScreen.InGame, HudScreen.FactoryCrafting, HudScreen.StationBasedCrafting,
                     HudScreen.ReadOnlyLootChest, HudScreen.Map, HudScreen.ShroudForge })
            Assert.Null(InventoryScreen.Of(Screen(other)));
        Assert.Null(InventoryScreen.Of(Screen(HudScreen.Backpack, state: GameUiState.PreGame)));
        Assert.Null(InventoryScreen.Of(GameScreen.None));
    }

    [Fact]
    public void ScreenChangedSaysOpenChangeAndClose()
    {
        var seen = new List<(InventoryScreen? Prev, InventoryScreen? Cur)>();
        Ui.Inventory.ScreenChanged += (p, c) => seen.Add((p, c));
        UiInventoryScreens.ScreenChanged(Screen(HudScreen.InGame));
        Assert.Empty(seen);                                                     // no inventory screen before or after
        UiNative.ScreenTargetOverride = () => 0;
        UiInventoryScreens.ScreenChanged(Screen(HudScreen.StorageChest));       // the chest opens, its entity not named yet
        UiNative.ScreenTargetOverride = () => 777;
        UiInventoryScreens.ScreenChanged(Screen(HudScreen.StorageChest));       // the game names it a frame later
        UiInventoryScreens.ScreenChanged(Screen(HudScreen.StorageChest));       // the same: no event
        UiInventoryScreens.ScreenChanged(Screen(HudScreen.InGame));             // closed
        Assert.Equal(3, seen.Count);
        Assert.Null(seen[0].Prev);
        Assert.Equal(new InventoryScreen(InventoryScreenKind.Chest, HudScreen.StorageChest, EntityId.None), seen[0].Cur);
        Assert.Equal(new EntityId(777), seen[1].Cur?.Container);
        Assert.Equal(EntityId.None, seen[1].Prev?.Container);
        Assert.Equal(new EntityId(777), seen[2].Prev?.Container);
        Assert.Null(seen[2].Cur);
        Assert.Null(Ui.Inventory.Screen);  // no kiln.ui here: Ui.Screens.Current is None
    }

    [Fact]
    public void ScreenChangedIsPostedOnTheBus()
    {
        var got = new List<InventoryScreenChangedEvent>();
        Events.On<InventoryScreenChangedEvent>(got.Add);
        UiInventoryScreens.ScreenChanged(Screen(HudScreen.Backpack));
        Events.DrainQueued();
        Assert.Single(got);
        Assert.Equal(InventoryScreenKind.Backpack, got[0].Current?.Kind);
        Assert.Null(got[0].Previous);
    }

    [Fact]
    public void AddPanelNeedsTheRowAndScreenViewAndShowsOnItsScreens()
    {
        Use(new FakeInventory.Backend(["ui.slots.inspect", "ui.slots.click"]));
        Assert.Throws<KilnUnavailableException>(() => Ui.Inventory.AddPanel("x", (_, _) => { }));
        Use(new FakeInventory.Backend(Rows));
        Assert.Throws<ArgumentException>(() => Ui.Inventory.AddPanel("x", (_, _) => { }, new InventoryPanelOptions { Width = 0 }));
        Assert.Throws<ArgumentException>(() => Ui.Inventory.AddPanel("x", (_, _) => { }, new InventoryPanelOptions { Screens = 0 }));

        using var both = Ui.Inventory.AddPanel("both", (_, _) => { });
        using var chests = Ui.Inventory.AddPanel("chests", (_, _) => { }, new InventoryPanelOptions { Screens = InventoryScreens.Chest, Interactive = false });
        using var known = Ui.Inventory.AddPanel("known chest", (_, _) => { },
            new InventoryPanelOptions { Screens = InventoryScreens.Chest, When = s => s.Container.Value != 0 });
        Assert.Equal(0, UiInventoryScreens.VisiblePanels);                     // no inventory screen open
        UiInventoryScreens.ScreenChanged(Screen(HudScreen.Backpack));
        Assert.Equal(["both"], UiInventoryScreens.Showing());
        Assert.Equal(1, UiInventoryScreens.InteractivePanels);
        UiNative.ScreenTargetOverride = () => 0;
        UiInventoryScreens.ScreenChanged(Screen(HudScreen.StorageChest));
        Assert.Equal(["both", "chests"], UiInventoryScreens.Showing());
        Assert.Equal(3, UiInventoryScreens.VisiblePanels);                     // the overlay draws for every candidate
        Assert.Equal(2, UiInventoryScreens.InteractivePanels);
        UiNative.ScreenTargetOverride = () => 31;
        UiInventoryScreens.ScreenChanged(Screen(HudScreen.StorageChest));
        Assert.Equal(["both", "chests", "known chest"], UiInventoryScreens.Showing());
        chests.Dispose();
        Assert.Equal(["both", "known chest"], UiInventoryScreens.Showing());
        UiInventoryScreens.ScreenChanged(Screen(HudScreen.FactoryCrafting));   // a factory: Ui.Crafting's panels, not these
        Assert.Empty(UiInventoryScreens.Showing());
        Assert.Equal(0, UiInventoryScreens.VisiblePanels);
    }

    [Fact]
    public void AnOlderKilnCoreWithoutScreenViewRefusesPanels()
    {
        // a v1 table (40 bytes): T2 is null, AddPanel says why
        var v1 = (UiInventoryNative.Table*)NativeMemory.AllocZeroed((nuint)sizeof(UiInventoryNative.Table));
        *v1 = *FakeInventory.Table;
        v1->Header = new ServiceHeader { Size = (uint)sizeof(UiInventoryNative.Table), Version = 1 };
        NativeMemory.Free(FakeInventory.Table);
        FakeInventory.Table = v1;
        Use(new FakeInventory.Backend(Rows));
        Assert.True(UiInventoryNative.T != null);
        Assert.True(UiInventoryNative.T2 == null);
        var e = Assert.Throws<KilnUnavailableException>(() => Ui.Inventory.AddPanel("x", (_, _) => { }));
        Assert.Contains("v2", e.Message);
    }

    [Fact]
    public void APanelDocksBesideTheSlots()
    {
        var all = new UiInventoryScreens.Box(400, 200, 900, 500);       // both grids: x 400..1300
        var chest = new UiInventoryScreens.Box(900, 260, 400, 300);     // the chest's grid on the right
        // right of every slot, level with the chest's top
        Assert.Equal((1316f, 260f), UiInventoryScreens.Dock(InventoryPanelSide.Right, all, chest, 300, 0, 1920, 1080, 1));
        // a second panel on that side goes under the first
        Assert.Equal((1316f, 400f), UiInventoryScreens.Dock(InventoryPanelSide.Right, all, chest, 300, 140, 1920, 1080, 1));
        // scale 2: the gap doubles
        Assert.Equal(1332f, UiInventoryScreens.Dock(InventoryPanelSide.Right, all, chest, 300, 0, 1920, 1080, 2).X);
        // no room on the right: left of the slots
        Assert.Equal(84f, UiInventoryScreens.Dock(InventoryPanelSide.Right, all, chest, 300, 0, 1500, 1080, 1).X);
        // room on neither side: kept on the screen at its right edge
        var wide = new UiInventoryScreens.Box(100, 200, 1700, 500);
        Assert.Equal(1612f, UiInventoryScreens.Dock(InventoryPanelSide.Right, wide, wide, 300, 0, 1920, 1080, 1).X);
        // Left: left of the slots, else right of them, else the left margin
        Assert.Equal(84f, UiInventoryScreens.Dock(InventoryPanelSide.Left, all, chest, 300, 0, 1920, 1080, 1).X);
        Assert.Equal(1116f, UiInventoryScreens.Dock(InventoryPanelSide.Left, all with { X = 200, W = 900 }, chest, 300, 0, 1920, 1080, 1).X);
        Assert.Equal(8f, UiInventoryScreens.Dock(InventoryPanelSide.Left, wide, wide, 300, 0, 1920, 1080, 1).X);
        // Below: under every slot, at the chest's left edge
        Assert.Equal((900f, 716f), UiInventoryScreens.Dock(InventoryPanelSide.Below, all, chest, 300, 0, 1920, 1080, 1));
        // never off the screen
        var (bx, by) = UiInventoryScreens.Dock(InventoryPanelSide.Below, all with { H = 2000 }, chest, 300, 0, 1920, 1080, 1);
        Assert.True(bx >= 8 && by <= 1072);
    }
}
