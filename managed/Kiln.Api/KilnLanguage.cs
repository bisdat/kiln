// SPDX-License-Identifier: MIT
// The language of Kiln's own UI, for mods (since 0.2.0-dev.29.19): KilnText holds the table; this is the public view of its
// language, its English fallback while the overlay's glyphs load, and its change. Mods' own texts (Mod.Text, ModLocale) follow
// the game's language (Game.Language); a mod whose pages sit beside Kiln's and should read in the same language follows this.
namespace Kiln;

/// <summary>The language Kiln's own UI shows (the Kiln menu and its pages, Kiln's windows and toasts, the Keys page, the
/// lines kiln-ui.dll adds to the game's menus) and when it changes. kiln.json <c>ui.language</c> picks it: <c>"auto"</c> (the
/// default) follows the game's text language (<see cref="Game.Language"/>), a tag such as <c>"zh-CN"</c> forces one; the
/// player changes it in game under Mods &gt; Kiln settings &gt; Language. A mod's own texts (<see cref="Mod.Text"/>) follow the
/// game's language; a mod whose texts should match Kiln's pages instead reads <see cref="Current"/> and listens to
/// <see cref="Changed"/>. Both sides (a dedicated server follows its game language, <c>"en-us"</c>). Since 0.2.0-dev.29.19.</summary>
[AnyThread]
public static class KilnLanguage
{
    private static readonly object Gate = new();
    private static (Action Handler, ModContext? Owner)[] _handlers = [];

    /// <summary>The language Kiln's UI shows now, as a lower-case tag (<c>"en-us"</c>, <c>"zh-cn"</c>, <c>"de"</c>):
    /// <c>ui.language</c>'s tag, or the game's text language while it is <c>"auto"</c>. <see cref="ModLocale.LookupOrder"/>
    /// turns it into the files a mod would try; <see cref="ModLocale.GetIn"/> reads a mod's text in it. Any thread.</summary>
    public static string Current => KilnText.Language;

    /// <summary>kiln.json <c>ui.language</c> as set: <c>"auto"</c> or the tag the player chose. Any thread.</summary>
    public static string Setting => KilnText.Setting;

    /// <summary>True while Kiln's overlay shows its texts in English although <see cref="Current"/> is another language: the
    /// overlay cannot draw that language's script yet (the game's own Chinese, Japanese or Korean font, or its Cyrillic one,
    /// is still loading, or could not be loaded). Text a mod draws in Kiln's overlay (windows, screens, the HUD) in such a
    /// language shows as boxes meanwhile; a mod that follows Kiln's language shows English too while this is true. Texts the
    /// game draws itself are not affected. False on a dedicated server and while the overlay draws nothing. Any thread.</summary>
    public static bool OverlayInEnglish => KilnText.Gated;

    /// <summary>Kiln's texts changed: <see cref="Current"/> is another language (the player's setting, or the game's language
    /// while it is <c>"auto"</c>), <see cref="OverlayInEnglish"/> switched, or Kiln read its locale files again. Raised on the
    /// thread that made the change (often the overlay's render thread, or the window thread for the in-game setting), after
    /// the new values are in place: read <see cref="Current"/> and <see cref="OverlayInEnglish"/> inside. Keep it short; post
    /// heavier work. A handler that throws is logged under its mod and does not stop the others. A mod's handlers are dropped
    /// when the mod unloads.</summary>
    public static event Action? Changed
    {
        add
        {
            if (value is null) return;
            var owner = ModContext.CurrentMod ?? ModContext.OwnerOf(value);
            lock (Gate) _handlers = [.. _handlers, (value, owner)];
            if (owner is not null)
            {
                owner.Retiring += () => RemoveOwnedBy(owner);
                if (!owner.Alive) RemoveOwnedBy(owner);
            }
        }
        remove
        {
            if (value is null) return;
            lock (Gate)
            {
                var i = Array.FindLastIndex(_handlers, h => h.Handler == value);
                if (i >= 0) _handlers = [.. _handlers[..i], .. _handlers[(i + 1)..]];
            }
        }
    }

    /// <summary>KilnText, after its table changed (outside its lock).</summary>
    internal static void Raise()
    {
        var handlers = _handlers;
        foreach (var (handler, owner) in handlers)
        {
            if (owner is { Alive: false }) continue;
            var previous = ModContext.CurrentMod;
            ModContext.CurrentMod = owner ?? previous;
            try
            {
                handler();
            }
            catch (Exception e)
            {
                if (owner is not null) owner.Log.Error(e, "a KilnLanguage.Changed handler threw");
                else
                    try { Backend.Current.Log(0, LogLevel.Warn, "texts: a KilnLanguage.Changed handler threw: " + e.Message); }
                    catch (Exception) { /* no backend (a unit test without one) */ }
            }
            finally
            {
                ModContext.CurrentMod = previous;
            }
        }
    }

    private static void RemoveOwnedBy(ModContext mod)
    {
        lock (Gate) _handlers = _handlers.Where(h => !ReferenceEquals(h.Owner, mod)).ToArray();
    }

    /// <summary>Tests: no handlers.</summary>
    internal static void ResetForTests()
    {
        lock (Gate) _handlers = [];
    }

    internal static int HandlerCount => _handlers.Length;
}
