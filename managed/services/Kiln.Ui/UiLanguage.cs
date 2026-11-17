// SPDX-License-Identifier: MIT
// The language of Kiln's own in-game UI (docs/kiln/translating-game.md): kiln.json ui.language ("auto" follows the game's
// text language; a tag forces one), read with the look (PlayerLook) and changed on Kiln's menu (Mods > Kiln settings >
// Language). KilnText (Kiln.Api) holds the one table of Kiln's texts; this hands kiln.ui the texts it draws itself (the
// "native." keys: the Mods entry, the footer's line, the fallback window) and the language, at start and whenever the table
// changes, and follows the game's language while the setting is auto.
using System.Runtime.InteropServices;
using System.Text;

namespace Kiln;

internal static unsafe class UiLanguage
{
    private static int _started;

    /// <summary>Kiln.Ui's start: the setting from kiln.json, the native texts handed over, the game's language followed.</summary>
    internal static void Start()
    {
        if (Interlocked.Exchange(ref _started, 1) != 0) return;
        KilnText.Configure(PlayerLook.Current.Language);
        KilnText.Refresh(GameLanguage.Current);
        KilnText.Changed += Push;
        Push();
        UiEngine.Log(LogLevel.Info, $"texts: {Describe()}");
    }

    /// <summary>The start of an overlay frame (render thread): the game's language, while the setting is auto.</summary>
    internal static void Frame() => KilnText.Refresh(GameLanguage.Current);

    /// <summary>The setting changed (kiln.json, or Kiln's menu): applied at once.</summary>
    internal static void Apply(string setting)
    {
        if (string.Equals(KilnText.Setting, string.IsNullOrWhiteSpace(setting) ? KilnText.Auto : setting.Trim(), StringComparison.OrdinalIgnoreCase)) return;
        KilnText.Configure(setting);
        UiEngine.Log(LogLevel.Info, $"texts: {Describe()}");
    }

    internal static string Describe() =>
        KilnText.Setting == KilnText.Auto ? $"the game's language ({KilnText.Language})" : $"{KilnText.Language} (kiln.json ui.language)";

    /// <summary>Hands kiln.ui its texts and the language (set_texts). Any thread.</summary>
    internal static void Push()
    {
        var set = UiNative.SetTextsSlot;
        if (set == null) return;
        var pairs = KilnText.Native();
        var strings = new List<string>(pairs.Count * 2 + 1) { KilnText.Language };
        foreach (var (k, v) in pairs)
        {
            strings.Add(k);
            strings.Add(v);
        }
        // one block of UTF-8 for every string, then the two arrays of (pointer, length)
        var bytes = strings.Select(Encoding.UTF8.GetBytes).ToArray();
        var total = bytes.Sum(b => b.Length);
        var block = (byte*)NativeMemory.Alloc((nuint)Math.Max(1, total));
        var keys = (NativeStr*)NativeMemory.Alloc((nuint)(Math.Max(1, pairs.Count) * sizeof(NativeStr)));
        var values = (NativeStr*)NativeMemory.Alloc((nuint)(Math.Max(1, pairs.Count) * sizeof(NativeStr)));
        try
        {
            var at = 0;
            NativeStr Put(byte[] b)
            {
                b.CopyTo(new Span<byte>(block + at, b.Length));
                var s = new NativeStr { Ptr = block + at, Len = (nuint)b.Length };
                at += b.Length;
                return s;
            }
            UiNative.Texts t = default;
            t.Size = (uint)sizeof(UiNative.Texts);
            t.Count = (uint)pairs.Count;
            t.Language = Put(bytes[0]);
            for (var i = 0; i < pairs.Count; i++)
            {
                keys[i] = Put(bytes[1 + 2 * i]);
                values[i] = Put(bytes[2 + 2 * i]);
            }
            t.Keys = keys;
            t.Values = values;
            var rc = set(&t);
            if (rc != UiNative.Ok) UiEngine.Log(LogLevel.Warn, $"texts: kiln.ui refused Kiln's texts ({rc})");
        }
        finally
        {
            NativeMemory.Free(block);
            NativeMemory.Free(keys);
            NativeMemory.Free(values);
        }
    }

    internal static void ResetForTests()
    {
        if (Interlocked.Exchange(ref _started, 0) != 0) KilnText.Changed -= Push;
    }
}
