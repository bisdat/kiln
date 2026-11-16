// SPDX-License-Identifier: MIT
// Kiln's line in the main menu's footer: Kiln's mark, "Kiln <version> - <N> Mods Loaded", above the game's build number
// (kiln.ui's menu_footer, native/services/ui/menu_footer.cpp; docs/kiln/ui.md "Kiln's line in the main menu's footer").
// It takes the place of XHL-Core's "All mods loaded" line. kiln.ui draws it; this tells kiln.ui the count and whether the
// player wants it (Kiln's setting mainMenuFooter in kiln/config/menus-client.json, on the Mods page under "Kiln
// settings"). Until a count is known kiln.ui leaves the footer to the game.
//
// N counts every mod that loaded this start, of every engine Kiln runs, as Kiln's Mods page counts them (ModCatalog: the
// rows whose state is Loaded): Kiln mods, native DLL mods, native plugins that are not Kiln's own, chain DLLs, and the
// mods of EMF, EML, XHL, ShroudForge, SET, Greeneye, Flame, Shroudtopia, Enshwick, shroudlink and Ember. Kiln's own
// services and plugins are not mods. Mods switched off, waiting, failed or not loaded do not count.
namespace Kiln;

internal static unsafe class MainMenuFooter
{
    private static int _started;
    private static long _lastRead;
    private static int _sentMods = int.MinValue;
    private static uint _sentFlags = uint.MaxValue;
    private static long _seenVersion = -1;
    private static int _count;
    private static readonly object Lock = new();
    private static IDisposable? _tick;

    /// <summary>How often the count is read again while the title or the main menu shows (ms).</summary>
    internal const long ReadEveryMs = 2000;

    /// <summary>The count the line shows (pure: tests).</summary>
    internal static int Loaded(CatalogSnapshot s) => s.Mods.Count(m => m.State == ModRunState.Loaded);

    /// <summary>What kiln.ui is told: the count (-1 while no snapshot has landed) and KILN_UI_FOOTER_* (pure: tests).</summary>
    internal static (int Mods, uint Flags) Native(bool shown, CatalogSnapshot? snapshot) =>
        (snapshot is null ? -1 : Loaded(snapshot), shown ? UiNative.FooterShown : 0u);

    /// <summary>Starts telling kiln.ui (a game client whose kiln-ui.dll has menu_footer; once).</summary>
    internal static void Start()
    {
        if (UiNative.MenuFooterSlot == null) return;
        if (Interlocked.Exchange(ref _started, 1) != 0) return;
        _tick = ServiceTick.Add("kiln main menu footer", (in TickInfo _) => Tick());
    }

    private static void Tick()
    {
        var now = Environment.TickCount64;
        if (now - Interlocked.Read(ref _lastRead) >= ReadEveryMs)
        {
            Interlocked.Exchange(ref _lastRead, now);
            GameScreen screen;
            try { screen = Ui.Screens.Current; }
            catch (Exception) { screen = GameScreen.None; }
            // the footer is only on the main menu; the title screen comes first, so the count is ready when it shows
            if (screen.Kind is ScreenKind.MainMenu or ScreenKind.Title or ScreenKind.None)
                ModCatalog.Snapshot(TimeSpan.FromMilliseconds(ReadEveryMs));  // a refresh on a worker when the last is old
        }
        Push();  // cheap when nothing changed: a new snapshot lands between two reads
    }

    /// <summary>Tells kiln.ui the setting and the last count when either changed. Any thread.</summary>
    internal static void Push()
    {
        var slot = UiNative.MenuFooterSlot;
        if (slot == null || Volatile.Read(ref _started) == 0) return;
        var snapshot = ModCatalog.Loading ? null : ModCatalog.Snapshot(TimeSpan.MaxValue);
        int mods;
        uint flags = GameMenuEntries.FooterShown ? UiNative.FooterShown : 0u;
        lock (Lock)
        {
            if (snapshot is not null && snapshot.Version != _seenVersion)
            {
                _seenVersion = snapshot.Version;
                _count = Loaded(snapshot);
            }
            mods = snapshot is null ? -1 : _count;
            if (mods == _sentMods && flags == _sentFlags) return;
            _sentMods = mods;
            _sentFlags = flags;
        }
        slot(mods, flags);
    }

    /// <summary>Forgets what was sent (tests).</summary>
    internal static void ResetForTests()
    {
        _tick?.Dispose();
        _tick = null;
        _started = 0;
        _lastRead = 0;
        _sentMods = int.MinValue;
        _sentFlags = uint.MaxValue;
        _seenVersion = -1;
        _count = 0;
    }
}
