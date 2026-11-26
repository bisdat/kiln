// SPDX-License-Identifier: MIT
// A ShroudForge mod's news in the Kiln menu when Kiln hosts ShroudForge (the Legacy branch: hosted or installed).
//
// Why: a ShroudForge Lua mod has no way to draw on the game's HUD. What it has to tell the player goes to
// shroudforge.notifications.publish, which ShroudForge's own Modloader window shows in its news feed, and a mod that wants
// the player to read it now calls shroudforge.ui.toggle_modloader() to open that window. Kiln's bundled runtime has no
// such window: the toggle opens Kiln's ShroudForge page, which until now showed none of the news. netjunk, 2026-11-26:
// Altar Cleanup's "Ask before cleaning up" (Nexus 417) scans, publishes what it found and "press Keypad 1 again within 60
// seconds to clean up", and opens the window; under Kiln the page opened with no question on it.
//
// Where ShroudForge keeps them (fork pin 6cf01443): notifications.publish (mod-runtime create_mod_api.rs) checks the
// notice and calls shroudforge_news::write_event(game, "<mod>-<id>", payload) (notifications/write_notification_event.rs),
// which puts the payload under the state file's "events" section as "<mod>-<id>.json": {id: "<mod>.<id>", modId, title,
// message, level, actionUrl, updatedAt (Unix seconds)}. The state file is <state>/state.json (loader-config
// resolve_installation_paths.rs state_file; <state> is ShroudForge's paths.state, by default <game>/shroudforge), the file
// ModCatalog already reads for the runtime's per-mod state. A later publish with the same id replaces the entry.
//
// Threads: Read does the file IO (a worker, or the menu thread for a mod's toggle). The render thread asks Cached, which
// starts a read on a worker at most once a second and returns what the last one found.
using System.Text.Json;

namespace Kiln;

/// <summary>One notice a ShroudForge mod published, as its runtime keeps it.</summary>
internal sealed record SfNews(string Id, string ModId, string Title, string Message, string Level, long UpdatedAt);

internal static class ShroudForgeNews
{
    /// <summary>The most notices kept from one read (newest first) and the largest state file read.</summary>
    internal const int MaxNotices = 512;
    internal const long MaxStateBytes = 32L * 1024 * 1024;

    /// <summary>ShroudForge's state file (tests point it elsewhere). Asked only for a hosted or installed start's rows.</summary>
    internal static Func<string> StatePath = () => Path.Combine(ShroudForgePackages.Folder(ShroudForgeUi.GameDir(), "state"), "state.json");

    private static volatile IReadOnlyList<SfNews> _cached = [];
    private static long _readAtMs = long.MinValue / 2;
    private static int _reading;

    /// <summary>The notices in the state file at <paramref name="path"/>, newest first; none when it is missing, too big or
    /// not ShroudForge's. An entry that is not what notifications.publish writes is left out, never changed.</summary>
    public static IReadOnlyList<SfNews> Read(string path)
    {
        try
        {
            var info = new FileInfo(path);
            if (!info.Exists || info.Length > MaxStateBytes || info.LinkTarget is not null) return [];
            using var doc = JsonDocument.Parse(File.ReadAllBytes(path), new JsonDocumentOptions { MaxDepth = 64 });
            if (doc.RootElement.ValueKind != JsonValueKind.Object || !doc.RootElement.TryGetProperty("events", out var events) ||
                events.ValueKind != JsonValueKind.Object) return [];
            var list = new List<SfNews>();
            foreach (var e in events.EnumerateObject())
                if (Entry(e.Value) is { } n) list.Add(n);
            return list.OrderByDescending(n => n.UpdatedAt).ThenBy(n => n.Id, StringComparer.Ordinal).Take(MaxNotices).ToList();
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or JsonException) { return []; }
    }

    private static SfNews? Entry(JsonElement v)
    {
        if (v.ValueKind != JsonValueKind.Object) return null;
        static string? S(JsonElement o, string k, int max) =>
            o.TryGetProperty(k, out var x) && x.ValueKind == JsonValueKind.String && x.GetString() is { } s && s.Length <= max ? s : null;
        if (S(v, "id", 200) is not { Length: > 0 } id || S(v, "modId", 80) is not { Length: > 0 } mod || S(v, "title", 120) is not { } title ||
            S(v, "message", 2000) is not { } message) return null;
        if (!v.TryGetProperty("updatedAt", out var at) || at.ValueKind != JsonValueKind.Number || !at.TryGetInt64(out var seconds) || seconds < 0)
            return null;
        return new SfNews(id, mod, title, message, S(v, "level", 16) ?? "info", seconds);
    }

    /// <summary>The notices of mod <paramref name="modId"/>, in the order given.</summary>
    public static IReadOnlyList<SfNews> ForMod(IReadOnlyList<SfNews> news, string modId) =>
        news.Where(n => string.Equals(n.ModId, modId, StringComparison.Ordinal)).ToList();

    /// <summary>The mod whose notice is the newest one dated from <paramref name="sinceSeconds"/> to a little after
    /// <paramref name="nowSeconds"/> (the runtime's clock and ours may differ by a second); null when none is.</summary>
    public static string? Fresh(IReadOnlyList<SfNews> news, long nowSeconds, long sinceSeconds) =>
        news.Where(n => n.UpdatedAt >= sinceSeconds && n.UpdatedAt <= nowSeconds + 2).MaxBy(n => n.UpdatedAt)?.ModId;

    /// <summary>The last read's notices (render thread: no file access); starts a read on a worker when the last one is
    /// older than a second.</summary>
    public static IReadOnlyList<SfNews> Cached()
    {
        if (Environment.TickCount64 - Interlocked.Read(ref _readAtMs) > 1000 && Interlocked.CompareExchange(ref _reading, 1, 0) == 0)
            Task.Run(() =>
            {
                try { Reload(); }
                finally { Volatile.Write(ref _reading, 0); }
            });
        return _cached;
    }

    /// <summary>Reads the state file now and keeps what it holds for <see cref="Cached"/> (a worker, a test).</summary>
    public static IReadOnlyList<SfNews> Reload()
    {
        IReadOnlyList<SfNews> news;
        try { news = Read(StatePath()); }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or ArgumentException) { news = []; }
        _cached = news;
        Interlocked.Exchange(ref _readAtMs, Environment.TickCount64);
        return news;
    }

    internal static void ResetForTests()
    {
        _cached = [];
        Interlocked.Exchange(ref _readAtMs, long.MinValue / 2);
        StatePath = () => Path.Combine(ShroudForgePackages.Folder(ShroudForgeUi.GameDir(), "state"), "state.json");
    }
}
