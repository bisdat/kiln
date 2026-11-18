using System.Runtime.InteropServices;
using Xunit;

namespace Kiln.MainMenuFooterTests;

// Kiln's line in the main menu's footer: the count it shows, what kiln.ui is told, and the table slot (kiln_ui.h:
// menu_footer after set_next_window_size_constraints).
// It installs the fake kiln.ui table and its backend, which every Kiln.Ui test class shares: it runs alone, as they do
// (FakeUiTable.Install frees a table another class's test may be drawing with).
[Collection(Kiln.FakeUi.UiSerial.Name)]
public unsafe class MainMenuFooterTests
{
    private static CatalogMod Mod(string engine, string id, ModRunState state) => new() { Engine = engine, Id = id, Name = id, State = state };

    [Fact]
    public void Counts_the_mods_that_loaded_of_every_engine()
    {
        var s = new CatalogSnapshot
        {
            Mods =
            [
                Mod("kiln", "a", ModRunState.Loaded), Mod("eml", "b", ModRunState.Loaded), Mod("xhl", "c", ModRunState.Loaded),
                Mod("shroudforge", "d", ModRunState.Loaded), Mod("chain", "e", ModRunState.Loaded),
                Mod("kiln", "off", ModRunState.Disabled), Mod("emf", "late", ModRunState.Waiting), Mod("kiln", "bad", ModRunState.Failed),
                Mod("set", "other side", ModRunState.NotLoaded),
            ],
        };
        Assert.Equal(5, MainMenuFooter.Loaded(s));
        Assert.Equal(0, MainMenuFooter.Loaded(CatalogSnapshot.Empty));
    }

    [Fact]
    public void Tells_kiln_ui_the_count_and_the_setting()
    {
        var s = new CatalogSnapshot { Mods = [Mod("kiln", "a", ModRunState.Loaded)] };
        Assert.Equal((1, UiNative.FooterShown), MainMenuFooter.Native(true, s));
        Assert.Equal((1, 0u), MainMenuFooter.Native(false, s));
        Assert.Equal((-1, UiNative.FooterShown), MainMenuFooter.Native(true, null));  // no snapshot yet: no line
    }

    [Fact]
    public void The_setting_is_on_by_default()
    {
        Assert.True(new MenuSettings().MainMenuFooter);
        Assert.True(GameMenuEntries.FooterShown);  // before the config is registered
    }

    [Fact]
    public void The_slot_follows_the_size_constraints_and_checks_its_end()
    {
        Assert.Equal(896, (int)Marshal.OffsetOf<UiNative.TableFooter>(nameof(UiNative.TableFooter.MenuFooter)));
        Assert.Equal(904, sizeof(UiNative.TableFooter));
        var previous = Backend.Current;
        Kiln.FakeUi.FakeUiTable.Install();
        Backend.Current = new Kiln.FakeUi.FakeUiTable.Backend();
        var old = Kiln.FakeUi.FakeUiTable.Table;
        var t = (UiNative.TableFooter*)NativeMemory.AllocZeroed((nuint)sizeof(UiNative.TableFooter));
        try
        {
            ((UiNative.Table*)t)->Header.Size = (uint)sizeof(UiNative.TableConstraints);
            ((UiNative.Table*)t)->Header.Version = 7;
            Kiln.FakeUi.FakeUiTable.Table = (UiNative.Table4*)t;
            UiNative.ResetForTests();
            t->MenuFooter = &Footer;
            Assert.True(UiNative.MenuFooterSlot == null);  // a kiln-ui.dll from before 0.2.0-dev.29.11
            UiNative.T->Header.Size = (uint)sizeof(UiNative.TableFooter);
            Assert.True(UiNative.MenuFooterSlot == t->MenuFooter);
            t->MenuFooter = null;
            Assert.True(UiNative.MenuFooterSlot == null);
        }
        finally
        {
            Kiln.FakeUi.FakeUiTable.Table = old;
            Backend.Current = previous;
            UiNative.ResetForTests();
            Kiln.FakeUi.FakeUiTable.Free();
            NativeMemory.Free(t);
        }
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(System.Runtime.CompilerServices.CallConvCdecl)])]
    private static void Footer(int mods, uint flags) { }
}
