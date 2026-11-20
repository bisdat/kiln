// SPDX-License-Identifier: MIT
// KilnLanguage (since 0.2.0-dev.29.19): the public view of the language of Kiln's own texts (KilnText), its English fallback
// while the overlay's glyphs load, and its change event, with each mod's handlers guarded and dropped at its unload.
using Xunit;

namespace Kiln.KilnLanguageTests;

[Collection(Kiln.FakeUi.UiSerial.Name)]
public sealed class KilnLanguageTests : IDisposable
{
    private readonly string _dir = Path.Combine(Path.GetTempPath(), "kiln-language-" + Guid.NewGuid().ToString("N"));
    private string _game = "en-us";

    public KilnLanguageTests()
    {
        Directory.CreateDirectory(_dir);
        KilnText.ResetForTests();
        KilnLanguage.ResetForTests();
        KilnText.Directory = _dir;
        GameLanguageSource.Override = () => _game;
    }

    public void Dispose()
    {
        KilnText.ResetForTests();
        KilnLanguage.ResetForTests();
        GameLanguageSource.ResetForTests();
        try { Directory.Delete(_dir, true); }
        catch (IOException) { }
    }

    [Fact]
    public void Current_and_Setting_are_Kilns_and_a_change_is_told_once()
    {
        KilnText.Configure("auto");
        var seen = new List<string>();
        void Handler() => seen.Add(KilnLanguage.Current);
        KilnLanguage.Changed += Handler;
        Assert.Equal("auto", KilnLanguage.Setting);
        Assert.Equal("en-us", KilnLanguage.Current);

        KilnText.Configure("zh-CN");                // the player forces a language
        Assert.Equal("zh-CN", KilnLanguage.Setting);
        Assert.Equal("zh-cn", KilnLanguage.Current);
        KilnText.Configure("zh-CN");                // the same again: nothing changed
        KilnText.Configure("auto");
        _game = "de-de";
        KilnText.Refresh();                         // auto follows the game's language
        Assert.Equal(["zh-cn", "en-us", "de-de"], seen);

        KilnLanguage.Changed -= Handler;
        KilnText.Configure("fr");
        Assert.Equal(3, seen.Count);
        Assert.Equal(0, KilnLanguage.HandlerCount);
    }

    [Fact]
    public void OverlayInEnglish_follows_the_glyphs_of_the_language()
    {
        KilnText.Configure("ja");
        var changes = 0;
        KilnLanguage.Changed += () => changes++;
        Assert.False(KilnLanguage.OverlayInEnglish);
        KilnText.Glyphs(false);                     // the game's Japanese font is loading
        Assert.True(KilnLanguage.OverlayInEnglish);
        Assert.Equal("ja", KilnLanguage.Current);    // the language stays; only the overlay's texts are English
        KilnText.Glyphs(true);
        Assert.False(KilnLanguage.OverlayInEnglish);
        Assert.Equal(2, changes);
        KilnText.Configure("de");                   // Latin needs no glyphs
        KilnText.Glyphs(false);
        Assert.False(KilnLanguage.OverlayInEnglish);
    }

    [Fact]
    public void A_handler_that_throws_does_not_stop_the_others()
    {
        var after = 0;
        KilnLanguage.Changed += () => throw new InvalidOperationException("boom");
        KilnLanguage.Changed += () => after++;
        KilnText.Configure("ko");
        Assert.Equal(1, after);
    }

    [Fact]
    public void A_mods_handler_runs_as_the_mod_and_goes_with_it()
    {
        var mod = HostAccess.CreateContext(new ModInfo("follower", "Follower", "1.0.0", "."), 92, ".");
        ModContext? seen = null;
        var calls = 0;
        var previous = HostAccess.Current;
        HostAccess.Current = mod;
        try
        {
            KilnLanguage.Changed += () => { calls++; seen = HostAccess.Current; };
        }
        finally
        {
            HostAccess.Current = previous;
        }
        KilnText.Configure("pl");
        Assert.Equal(1, calls);
        Assert.Same(mod, seen);
        mod.Retire();
        Assert.Equal(0, KilnLanguage.HandlerCount);
        KilnText.Configure("it");
        Assert.Equal(1, calls);
    }
}
