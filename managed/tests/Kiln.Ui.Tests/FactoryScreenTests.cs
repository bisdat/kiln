// SPDX-License-Identifier: MIT
// A production station's screen (HUD FactoryCrafting, 28) as a crafting screen (0.2.0-dev.29.15). The game draws it with its
// chest-style transfer screen, not CraftingMenu, so the native screen names no crafting menu there: before, GameScreen.Crafting
// was null at a kiln and Ui.Crafting panels never showed (Busy Hands' station panel).
using Xunit;

namespace Kiln;

[Collection(FakeUi.UiSerial.Name)]
public sealed class FactoryScreenTests : IDisposable
{
    // Kiln.Ui (the class) is hidden behind this assembly's namespace Kiln.Ui.Tests: its nested Crafting through reflection
    private static readonly Type Crafting = typeof(CraftingScreen).Assembly.GetType("Kiln.Ui+Crafting", throwOnError: true)!;
    private const System.Reflection.BindingFlags Any = System.Reflection.BindingFlags.Static | System.Reflection.BindingFlags.Public |
                                                       System.Reflection.BindingFlags.NonPublic;

    private static void ScreenChanged(GameScreen prev, GameScreen cur) => Crafting.GetMethod("ScreenChanged", Any)!.Invoke(null, [prev, cur]);
    private static bool PanelsVisible => (bool)Crafting.GetProperty("Visible", Any)!.GetValue(null)!;
    private static int VisiblePanels => (int)Crafting.GetProperty("VisiblePanels", Any)!.GetValue(null)!;
    private static int InteractivePanels => (int)Crafting.GetProperty("InteractivePanels", Any)!.GetValue(null)!;
    private static CraftingScreen PanelScreen => (CraftingScreen)Crafting.GetProperty("PanelScreen", Any)!.GetValue(null)!;

    private static IDisposable AddPanel(CraftingPanelOptions options) =>
        (IDisposable)Crafting.GetMethod("AddPanel", Any)!.Invoke(null, [(UiText)"test", (Action<UiDraw, CraftingScreen>)((_, _) => { }), options])!;

    public FactoryScreenTests() => UiNative.ScreenTargetOverride = () => 12205;

    public void Dispose()
    {
        UiNative.ScreenTargetOverride = null;
        ScreenChanged(GameScreen.None, GameScreen.None);
    }

    private static UiNative.Screen Native(byte hud, byte crafting = 0, uint interaction = 0, uint station = 0, uint workshop = 0) => new()
    {
        Size = (uint)System.Runtime.CompilerServices.Unsafe.SizeOf<UiNative.Screen>(), UiState = (byte)GameUiState.Play,
        HudScreen = hud, Crafting = crafting, Available = 1, Interaction = interaction, Station = station, Workshop = workshop,
    };

    [Fact]
    public void AKilnsScreenIsAFactoryCraftingScreenWithItsStation()
    {
        var g = GameScreen.From(Native((byte)HudScreen.FactoryCrafting, interaction: 0x2546ae49));
        Assert.Equal(ScreenKind.Crafting, g.Kind);
        Assert.NotNull(g.Crafting);
        var c = g.Crafting!.Value;
        Assert.True(c.AtStation && c.IsFactory && !c.Journal);
        Assert.Equal(0x2546aE49u, c.Workshop);                          // play data +0x2e48, as the game keeps it
        Assert.Equal(0u, c.Recipe);
        Assert.Equal(new EntityId(12205), c.Station);

        // the crafting screen is open there; which panels show is their own choice (AtFactories)
        Assert.False(PanelsVisible);
        ScreenChanged(GameScreen.None, g);
        Assert.True(PanelsVisible);
    }

    [Fact]
    public void OnlyPanelsThatAskForFactoryScreensShowThere()
    {
        // panels written for crafting menus (a recipe, the menu's tabs) do not show up at a kiln by themselves after 29.15:
        // CraftingPanelOptions.AtFactories (default false) asks for them
        using var menuPanel = AddPanel(new CraftingPanelOptions());
        using var stationPanel = AddPanel(new CraftingPanelOptions { AtFactories = true, Interactive = false });
        Assert.False(new CraftingPanelOptions().AtFactories);
        ScreenChanged(GameScreen.None, GameScreen.From(Native((byte)HudScreen.FactoryCrafting, interaction: 0x2546ae49)));
        Assert.Equal(1, VisiblePanels);                                    // the AtFactories one
        Assert.Equal(0, InteractivePanels);                                // it takes no mouse; the menu panel is not there
        ScreenChanged(GameScreen.None, GameScreen.From(Native((byte)HudScreen.StationBasedCrafting, crafting: 1, interaction: 0x4da88f73)));
        Assert.Equal(2, VisiblePanels);                                    // at a bench both show
        Assert.Equal(1, InteractivePanels);
        stationPanel.Dispose();
        ScreenChanged(GameScreen.None, GameScreen.From(Native((byte)HudScreen.FactoryCrafting, interaction: 0x2546ae49)));
        Assert.Equal(0, VisiblePanels);                                    // no panel for factories: nothing is drawn there
    }

    [Fact]
    public void APanelSeesTheStationTheGameNamesNowNotTheOneWhenTheScreenOpened()
    {
        // the game may write the screen's target a frame after the screen opens (or move it to another station without
        // closing the screen): the panel's CraftingScreen reads it each frame
        uint target = 0;
        UiNative.ScreenTargetOverride = () => target;
        ScreenChanged(GameScreen.None, GameScreen.From(Native((byte)HudScreen.FactoryCrafting, interaction: 0x2546ae49)));
        Assert.Equal(EntityId.None, PanelScreen.Station);
        target = 12205;
        Assert.Equal(new EntityId(12205), PanelScreen.Station);
        target = 12300;
        Assert.Equal(new EntityId(12300), PanelScreen.Station);
        Assert.True(PanelScreen.IsFactory);
        // not a factory screen: no station entity, whatever the native value says
        ScreenChanged(GameScreen.None, GameScreen.From(Native((byte)HudScreen.StationBasedCrafting, crafting: 1, interaction: 0x4da88f73)));
        Assert.Equal(EntityId.None, PanelScreen.Station);
    }

    [Fact]
    public void ABenchsMenuIsNoFactoryAndNamesNoStationEntity()
    {
        var g = GameScreen.From(Native((byte)HudScreen.StationBasedCrafting, crafting: 1, interaction: 0x4da88f73, station: 0x4da88f73));
        var c = g.Crafting!.Value;
        Assert.True(c.AtStation);
        Assert.False(c.IsFactory);
        Assert.Equal(EntityId.None, c.Station);
        Assert.Equal(0x4da88f73u, c.Workshop);
    }

    [Fact]
    public void WithoutItsEntityAFactoryScreenStillShowsAndOtherScreensAreNoCraftingScreen()
    {
        UiNative.ScreenTargetOverride = () => 0;                          // ui/play.target did not resolve, or an older kiln.ui
        var c = GameScreen.From(Native((byte)HudScreen.FactoryCrafting)).Crafting!.Value;
        Assert.True(c.IsFactory);
        Assert.Equal(EntityId.None, c.Station);
        Assert.Null(GameScreen.From(Native((byte)HudScreen.StorageChest)).Crafting);
        Assert.Null(GameScreen.From(Native((byte)HudScreen.InGame)).Crafting);
        // not in a world (no play data): never a factory screen
        var menu = Native((byte)HudScreen.FactoryCrafting);
        menu.UiState = (byte)GameUiState.PreGame;
        Assert.Null(GameScreen.From(menu).Crafting);
    }
}
