// SPDX-License-Identifier: MIT
// The hash a server advertises for another engine's mod (required[], AdvertiseEngines.cs), the same while the mod runs
// (0.2.0-dev.29.18; kiln-manage modpack::engine_identity computes the same, keep in step). Until then it was the kdh1 of
// the whole folder the server runs, with the player's settings and whatever the mod writes into its own folder (logs,
// bin/native-status.txt), so it changed about every minute and was never the Library's.
//
//   Kiln's copy (the launcher placed it in this game folder: .kiln-library.json and kiln/state/library-placements.json;
//   or synced it to this server: .kiln-source.json and kiln/state/server-library.json), whose files are still the ones
//   placed: the Library entry's kdh1. Its settings, its switch, its logs and the files the mod adds do not count.
//   Any other folder: the kdh1 of its files without logs (*.log), ShroudForge's extended.mod.json (switch and settings)
//   and the kind's settings files.
//
// The receipts are kept outside the mod folders (a package cannot forge one). They name the folder by the launcher's
// path (a Linux path under Wine or Proton, where this side sees D:\ or Z:\), so a receipt is matched by the folder's name
// and its parent's name together with the marker in the folder; kiln-manage matches them the same way. The fixture
// schema/fixtures/engine-identity (a Python reference wrote its expected hashes) is read by this side's tests and by
// kiln-manage's: change the rule in all three.
using System.Security.Cryptography;
using System.Text.Json;

namespace Kiln;

internal static partial class NetAdvertise
{
    // kiln-manage engine_mods::settings_files: the settings files an engine documents in a mod's top folder
    private static string[] SettingsFiles(string kind) => kind switch
    {
        "eml" => ["config.txt", "config.ini"],
        "set" => ["settings.json"],
        _ => [],
    };

    // kiln-manage settings_files::SETTINGS_EXTENSIONS and LOG_EXTENSIONS
    private static readonly string[] SettingsExtensions = ["txt", "ini", "cfg", "json", "toml", "yaml", "yml"];

    private static string Extension(string rel)
    {
        var name = rel[(rel.LastIndexOfAny(['/', '\\']) + 1)..];
        var dot = name.LastIndexOf('.');
        return dot <= 0 ? "" : AsciiLower(name[(dot + 1)..]);
    }

    // as Rust's to_ascii_lowercase (kiln-manage compares names so)
    private static string AsciiLower(string s) => string.Create(s.Length, s, (span, src) =>
    {
        for (var i = 0; i < src.Length; i++) span[i] = src[i] is >= 'A' and <= 'Z' ? (char)(src[i] + 32) : src[i];
    });

    internal static bool IsLog(string rel) => Extension(rel) == "log";

    /// <summary>A file of an engine mod's folder that is not part of what it advertises (kiln-manage modpack::left_out):
    /// ShroudForge's extended.mod.json of a ShroudForge or EML package, the kind's settings files at its top, and in a
    /// game-folder copy (an EML mod a DLL reads in the game's mods folder) any settings file a DLL may write there, not
    /// its manifest, its Lua or XHL-Core's native-plugin.ini (kiln-manage engine_mods::is_game_settings_file). Case
    /// ignored.</summary>
    internal static bool LeftOut(string kind, string rel, bool gameFolder)
    {
        var top = !rel.Contains('/');
        var low = AsciiLower(rel);
        return (top && kind is "shroudforge" or "eml" && low == "extended.mod.json")
            || (top && SettingsFiles(kind).Any(s => s == low))
            || (gameFolder && low is not ("mod.json" or ".kiln-library.json" or "native-plugin.ini") && !low.StartsWith("src/", StringComparison.Ordinal)
                && SettingsExtensions.Contains(Extension(rel)));
    }

    /// <summary>The advertised hash of an engine mod folder (see the file's head). <paramref name="game"/>: the game or
    /// server folder (its kiln/state holds the receipts).</summary>
    internal static string EngineHash(string kind, string dir, string game)
    {
        // any receipt that names this folder with the marker it holds, while the files it lists are still there as listed
        foreach (var copy in KilnsCopies(kind, dir, game))
            if (copy.Files.All(f => SameFile(dir, f.Rel, f.Sha)))
                return copy.Library;
        var files = DirHash.Files(dir).Where(f => !IsLog(Rel(f.Key)) && !LeftOut(kind, Rel(f.Key), false)).ToList();
        return DirHash.HashOf(files);
    }

    /// <summary>What decides <see cref="EngineHash"/>: the tree's signature and the receipts' (a receipt written
    /// anew, a marker changed).</summary>
    internal static string EngineSignature(string dir, string game)
    {
        var sb = new System.Text.StringBuilder(DirHash.Signature(dir));
        foreach (var f in new[] { Path.Combine(dir, ".kiln-library.json"), Path.Combine(dir, ".kiln-source.json"),
                     Path.Combine(game, "kiln", "state", "library-placements.json"), Path.Combine(game, "kiln", "state", "server-library.json") })
        {
            var fi = new FileInfo(f);
            sb.Append('\n').Append(fi.Exists ? $"{fi.Length}:{fi.LastWriteTimeUtc.Ticks}" : "-");
        }
        return sb.ToString();
    }

    private static string Rel(byte[] key) => System.Text.Encoding.UTF8.GetString(key);

    private static bool SameFile(string dir, string rel, string sha)
    {
        try
        {
            var path = Path.Combine(dir, rel.Replace('/', Path.DirectorySeparatorChar));
            var fi = new FileInfo(path);
            if (!fi.Exists || (fi.Attributes & FileAttributes.ReparsePoint) != 0) return false;
            using var fs = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete, 1 << 16);
            return string.Equals(Convert.ToHexStringLower(SHA256.HashData(fs)), sha, StringComparison.OrdinalIgnoreCase);
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or ArgumentException or NotSupportedException) { return false; }
    }

    /// <summary>Kiln's copy: the Library entry's hash and the files to check (relative path, sha256), without the ones
    /// that may change (<see cref="LeftOut"/>, logs).</summary>
    internal sealed record KilnCopy(string Library, List<(string Rel, string Sha)> Files);

    /// <summary>What the receipts say this folder is: one copy for each receipt entry that names the folder (by its
    /// name and its parent's, case ignored for ASCII letters) with the marker the folder holds and has a file list.
    /// Server sync first, then activation (kiln-manage modpack::kilns_copies, the same order). A receipt that cannot be
    /// read gives none; one entry that is not as expected is passed over, the others still count.</summary>
    internal static List<KilnCopy> KilnsCopies(string kind, string dir, string game)
    {
        var list = new List<KilnCopy>();
        var name = Path.GetFileName(Path.TrimEndingDirectorySeparator(dir));
        var parent = Path.GetFileName(Path.GetDirectoryName(Path.TrimEndingDirectorySeparator(dir)) ?? "");
        // synced to this server by the launcher
        try
        {
            if (Json(Path.Combine(dir, ".kiln-source.json")) is { ValueKind: JsonValueKind.Object } s && Format(s) == 1 &&
                s.TryGetProperty("library", out var sl) && Ref(sl) is { } lib && lib.Kind == kind && lib.Id == name &&
                Json(Path.Combine(game, "kiln", "state", "server-library.json")) is { ValueKind: JsonValueKind.Object } synced)
                foreach (var e in synced.EnumerateObject())
                    if (e.Value.ValueKind == JsonValueKind.Object && e.Value.TryGetProperty("library", out var el) && Ref(el) == lib &&
                        LeafNames(e.Name, name, parent) && Copy(lib.Hash, e.Value, kind, false) is { } c)
                        list.Add(c);
        }
        catch (Exception x) when (x is IOException or UnauthorizedAccessException or JsonException or ArgumentException) { }
        // placed in this game folder by the launcher's activation
        try
        {
            if (Json(Path.Combine(dir, ".kiln-library.json")) is { ValueKind: JsonValueKind.Object } m && Format(m) == 1 && Ref(m) is { } want &&
                want.Kind == kind && want.Id == name &&
                Json(Path.Combine(game, "kiln", "state", "library-placements.json")) is { ValueKind: JsonValueKind.Array } placed)
                foreach (var r in placed.EnumerateArray())
                    if (r.ValueKind == JsonValueKind.Object && r.TryGetProperty("marker", out var rm) && rm.ValueKind == JsonValueKind.Object &&
                        Format(rm) == 1 && Ref(rm) == want && Names(r, "path", name, parent) &&
                        Copy(want.Hash, r, kind, GameFolder(r)) is { } c)
                        list.Add(c);
        }
        catch (Exception x) when (x is IOException or UnauthorizedAccessException or JsonException or ArgumentException) { }
        return list;
    }

    // kiln-manage writes the receipt's field as game_folder (engine_mods::Placement has no serde rename)
    private static bool GameFolder(JsonElement r) =>
        (r.TryGetProperty("game_folder", out var g) || r.TryGetProperty("gameFolder", out g)) && g.ValueKind == JsonValueKind.True;

    private static KilnCopy? Copy(string hash, JsonElement receipt, string kind, bool gameFolder)
    {
        if (!receipt.TryGetProperty("files", out var files) || files.ValueKind != JsonValueKind.Object) return null;
        var list = new List<(string, string)>();
        foreach (var f in files.EnumerateObject())
            if (!f.Name.EndsWith('/') && f.Value.ValueKind == JsonValueKind.String && !LeftOut(kind, f.Name, gameFolder) && !IsLog(f.Name))
                list.Add((f.Name, f.Value.GetString()!));
        return new KilnCopy(hash, list);
    }

    private sealed record LibRef(string Kind, string Id, string Version, string Hash);

    private static LibRef? Ref(JsonElement o) =>
        Str(o, "kind") is { } k && Str(o, "id") is { } i && Str(o, "version") is { } v && Str(o, "hash") is { } h ? new LibRef(k, i, v, h) : null;

    private static int Format(JsonElement o) => o.TryGetProperty("format", out var f) && f.ValueKind == JsonValueKind.Number && f.TryGetInt32(out var n) ? n : 0;

    private static string? Str(JsonElement o, string key) =>
        o.ValueKind == JsonValueKind.Object && o.TryGetProperty(key, out var v) && v.ValueKind == JsonValueKind.String ? v.GetString() : null;

    private static bool Names(JsonElement r, string key, string name, string parent) => Str(r, key) is { } p && LeafNames(p, name, parent);

    /// <summary>The receipt's path (either slash) ends in <c>&lt;parent&gt;/&lt;name&gt;</c>, the case of ASCII letters
    /// ignored (as kiln-manage's leaf_names: Windows paths of one folder may differ in case).</summary>
    internal static bool LeafNames(string path, string name, string parent)
    {
        var parts = path.Replace('\\', '/').TrimEnd('/').Split('/');
        return parts.Length >= 2 && AsciiLower(parts[^1]) == AsciiLower(name) && AsciiLower(parts[^2]) == AsciiLower(parent);
    }

    private static JsonElement? Json(string path)
    {
        var fi = new FileInfo(path);
        if (!fi.Exists || fi.Length > 16 << 20) return null;
        using var doc = JsonDocument.Parse(File.ReadAllBytes(path));
        return doc.RootElement.Clone();
    }
}
