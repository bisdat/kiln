// SPDX-License-Identifier: MIT
// The boot content fingerprint of each boot mod a generation committed (docs/kiln/network.md "Boot content at the join"). OnBoot runs
// before the connection, so a setting it reads (a resource patch's value) gives each process its own game data, and a later config sync
// cannot rebuild the one-shot generation. The join compares this fingerprint instead: Kiln.Network reads Current() by reflection
// (Kiln.Network does not reference Kiln.Host) and sends it in HELLO and WELCOME.
//
// What it covers, for one mod: every document the IR envelope commits for it (each collector's {owner, key, document}: what its OnBoot
// staged, as Kiln.Blocks lowered it), and the "resourcePatches" of its blocks/*.json and the "gearRules" of its content/*.json, read as
// kiln.content's prepare reads them (gamedata_bootpatch.cpp file_docs; a key the file and OnBoot both have stays the file's). What it leaves out, because it differs per
// machine or per role and changes no committed data: the envelope's owner entry (roles, the inputs hash over the whole folder, the
// requires list), object key order, file-system order (entries are sorted by collector and key, except patches keep application order; files by name), number spelling (1500,
// 1500.0, 1.5e3), and the mod's own folder inside a string (an item icon's full path counts as "$mod/<path>"). Other
// files of the mod (its assets, VFX, quests) are not in it: the mod's version stands for them.
using System.Globalization;
using System.Numerics;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace Kiln.Host;

internal static class BootFingerprint
{
    /// <summary>The value of a boot mod that is in the generation's graph and was left out (or faulted since).</summary>
    internal const string Out = "out";

    /// <summary>The scheme: a change of what is hashed gives every mod another fingerprint, so the line names it.</summary>
    private const string Scheme = "kiln-boot-fingerprint 1";

    private static readonly object Gate = new();
    private static (ulong Generation, IReadOnlyDictionary<string, string?> Mods) _published = (0, new Dictionary<string, string?>());

    /// <summary>What generation <paramref name="generation"/> committed, by mod id (side both only): the fingerprint, or null for a mod
    /// that was left out of it. Set once, when the generation is published.</summary>
    internal static void Publish(ulong generation, IReadOnlyDictionary<string, string?> mods)
    {
        lock (Gate) _published = (generation, new Dictionary<string, string?>(mods, StringComparer.Ordinal));
    }

    /// <summary>Kiln.Network's view, by reflection (BootGate.cs): each side-both boot mod of this process's generation, id -> 64 hex
    /// digits, or <see cref="Out"/> when it was left out or its content faulted since (its state now). Empty without a published
    /// generation.</summary>
    internal static IReadOnlyDictionary<string, string> Current()
    {
        (ulong Generation, IReadOnlyDictionary<string, string?> Mods) p;
        lock (Gate) p = _published;
        var d = new Dictionary<string, string>(StringComparer.Ordinal);
        if (p.Generation == 0 || p.Generation != BootGeneration.Number) return d;
        foreach (var (id, fp) in p.Mods)
            d[id] = fp != null && BootGeneration.Report(id) is { State: BootModState.Committed } ? fp : Out;
        return d;
    }

    /// <summary>The fingerprint of mod <paramref name="modId"/> (folder <paramref name="directory"/>) over the envelope's
    /// <paramref name="collectors"/> (collector -> [{owner, key, document}]) and its blocks files' resource patches: lower-case SHA-256 hex.</summary>
    internal static string Of(string modId, string directory, JsonObject collectors)
    {
        var text = CanonicalText(Entries(modId, directory, collectors), Roots(directory));
        LabDump(modId, text);
        return Sha(text);
    }

    /// <summary>Labs only: while <c>kiln/control/boot-fingerprint.dump</c> exists, each fingerprint's hashed text is written to
    /// <c>kiln/state/boot-fingerprints/&lt;id&gt;.txt</c> (UTF-8, the exact bytes), so two processes' inputs can be compared line by line.
    /// Nothing creates the file in a release.</summary>
    internal const string DumpSwitch = "boot-fingerprint.dump";

    private static void LabDump(string modId, string text)
    {
        try
        {
            if (!File.Exists(Path.Combine(KilnPaths.Root, "control", DumpSwitch))) return;
            var dir = Directory.CreateDirectory(Path.Combine(KilnPaths.State, "boot-fingerprints")).FullName;
            File.WriteAllBytes(Path.Combine(dir, modId + ".txt"), Encoding.UTF8.GetBytes(text));
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or ArgumentException)
        {
            // a lab aid: the fingerprint stands without it
        }
    }

    /// <summary>The mod folder as a path inside a document may name it: as given, and with its links resolved (Kiln.Blocks names a mod
    /// file by its resolved path, BlockFiles.InMod).</summary>
    internal static string[] Roots(string directory)
    {
        var full = Path.GetFullPath(directory);
        return new[] { NormalizeRoot(full), NormalizeRoot(Resolved(full)) }.Distinct(StringComparer.OrdinalIgnoreCase).ToArray();
    }

    // Every link on the way followed, as BlockFiles.Resolve does; the path as it is when that fails.
    private static string Resolved(string path, int links = 64)
    {
        try
        {
            if (links == 0) return path;
            var current = Path.GetPathRoot(path)!;
            foreach (var part in path[current.Length..].Split(Path.DirectorySeparatorChar, StringSplitOptions.RemoveEmptyEntries))
            {
                current = Path.Combine(current, part);
                FileSystemInfo info = Directory.Exists(current) ? new DirectoryInfo(current) : new FileInfo(current);
                if (info.LinkTarget != null) current = Resolved(info.ResolveLinkTarget(true)!.FullName, links - 1);
            }
            return current;
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or ArgumentException or NotSupportedException)
        {
            return path;
        }
    }

    /// <summary>The documents counted, as (collector, key, document), not yet sorted.</summary>
    internal static List<(string Collector, string Key, JsonNode? Document)> Entries(string modId, string directory, JsonObject collectors)
    {
        var list = new List<(string, string, JsonNode?)>();
        var fileKeys = new HashSet<string>(StringComparer.Ordinal);
        foreach (var (key, doc) in FilePatches(directory))
        {
            list.Add(("patches", key, doc));
            fileKeys.Add(key);
        }
        foreach (var (collector, docs) in collectors)
        {
            if (docs is not JsonArray array) continue;
            foreach (var d in array)
            {
                if (d is not JsonObject o || o["owner"] is not JsonValue owner || !owner.TryGetValue<string>(out var id) || id != modId) continue;
                var key = o["key"] is JsonValue k && k.TryGetValue<string>(out var ks) ? ks : "";
                if (collector == "patches" && fileKeys.Contains(key)) continue;  // kiln.content keeps the file's
                list.Add((collector, key, o["document"]));
            }
        }
        return list;
    }

    /// <summary>The hash of <paramref name="entries"/>: sorted by collector and key (ordinal), except patches retain native application order (files by
    /// name, then keys within each file), each document in its canonical form against the mod folder's <paramref name="roots"/> (<see cref="NormalizeRoot"/>).</summary>
    internal static string Hash(IEnumerable<(string Collector, string Key, JsonNode? Document)> entries, IReadOnlyList<string> roots) =>
        Sha(CanonicalText(entries, roots));

    private static string Sha(string text) => Convert.ToHexStringLower(SHA256.HashData(Encoding.UTF8.GetBytes(text)));

    /// <summary>What <see cref="Hash"/> hashes: the scheme line, then one line per document (collector, key, canonical JSON).</summary>
    internal static string CanonicalText(IEnumerable<(string Collector, string Key, JsonNode? Document)> entries, IReadOnlyList<string> roots)
    {
        var sb = new StringBuilder(Scheme).Append('\n');
        foreach (var (collector, key, doc) in entries.OrderBy(e => e.Collector, StringComparer.Ordinal)
                     .ThenBy(e => e.Collector == "patches" ? "" : e.Key, StringComparer.Ordinal))
        {
            Str(sb, collector);
            sb.Append(' ');
            Str(sb, key);
            sb.Append(' ');
            Canonical(sb, doc, roots);
            sb.Append('\n');
        }
        return sb.ToString();
    }

    /// <summary>The "resourcePatches" sections of the mod's blocks/*.json (files by name, ordinal), as kiln.content reads them: a file that
    /// does not name the section, or is not JSON with comments and trailing commas, gives none. Then the "gearRules" of its data packs
    /// (content/*.json, any case of .json, comments but no trailing commas), one entry "gearRules@&lt;file&gt;" each: kiln.content lowers
    /// them to a BalancingTable patch at the boot (gamedata_bootpatch.cpp gear_rule_docs); the section as written is what is hashed.</summary>
    internal static List<(string Key, JsonNode? Document)> FilePatches(string directory)
    {
        var list = new List<(string, JsonNode?)>();
        var blocks = Path.Combine(directory, "blocks");
        var relaxed = new JsonDocumentOptions { CommentHandling = JsonCommentHandling.Skip, AllowTrailingCommas = true, MaxDepth = 64 };
        foreach (var (_, text) in Files(blocks, f => Path.GetExtension(f) == ".json"))
        {
            try
            {
                if (!text.Contains("resourcePatches", StringComparison.Ordinal)) continue;
                using var parsed = JsonDocument.Parse(text, relaxed);
                if (LastValues(parsed.RootElement) is not JsonObject root || root["resourcePatches"] is not JsonObject section) continue;
                foreach (var (key, doc) in section.OrderBy(p => p.Key, StringComparer.Ordinal)) list.Add((key, doc));
            }
            catch (Exception e) when (e is JsonException or ArgumentException or InvalidOperationException)
            {
                // kiln.content reports such a file (and refuses a required mod for it); it adds no patch
            }
        }
        var packs = new JsonDocumentOptions { CommentHandling = JsonCommentHandling.Skip, AllowTrailingCommas = false, MaxDepth = 64 };
        foreach (var (name, text) in Files(Path.Combine(directory, "content"), f => Path.GetExtension(f).Equals(".json", StringComparison.OrdinalIgnoreCase)))
        {
            try
            {
                if (!text.Contains("gearRules", StringComparison.Ordinal)) continue;
                using var parsed = JsonDocument.Parse(text, packs);
                if (LastValues(parsed.RootElement) is JsonObject root && root["gearRules"] is { } rules) list.Add(("gearRules@" + name, rules.DeepClone()));
            }
            catch (Exception e) when (e is JsonException or ArgumentException or InvalidOperationException)
            {
                // kiln.content's packs report such a file at the world's load; the boot reads no gearRules from it
            }
        }
        return list;
    }

    // A folder's files that match, by name (ordinal), with their text; unreadable ones are left out.
    private static IEnumerable<(string Name, string Text)> Files(string folder, Func<string, bool> match)
    {
        if (!Directory.Exists(folder)) yield break;
        string[] files;
        try { files = Directory.GetFiles(folder).Where(match).OrderBy(Path.GetFileName, StringComparer.Ordinal).ToArray(); }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { yield break; }
        foreach (var f in files)
        {
            string text;
            try { text = File.ReadAllText(f); }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException) { continue; }
            yield return (Path.GetFileName(f), text);
        }
    }

    // nlohmann::json keeps the last occurrence of duplicate object keys, including nested keys.
    private static JsonNode? LastValues(JsonElement e)
    {
        if (e.ValueKind == JsonValueKind.Object)
        {
            var o = new JsonObject();
            foreach (var p in e.EnumerateObject()) o[p.Name] = LastValues(p.Value);
            return o;
        }
        if (e.ValueKind == JsonValueKind.Array) return new JsonArray(e.EnumerateArray().Select(LastValues).ToArray());
        return JsonNode.Parse(e.GetRawText());
    }

    /// <summary>A mod folder as strings are compared with it: forward slashes, no <c>\\?\</c> prefix, no trailing slash.</summary>
    internal static string NormalizeRoot(string fullPath) => Slashes(fullPath).TrimEnd('/');

    private static string Slashes(string p)
    {
        var s = p.Replace('\\', '/');
        if (s.StartsWith("//?/UNC/", StringComparison.OrdinalIgnoreCase)) return "//" + s[8..];
        return s.StartsWith("//?/", StringComparison.Ordinal) ? s[4..] : s;
    }

    /// <summary>A string as it is hashed: text unchanged, except a path inside the mod folder <paramref name="root"/> as <c>$mod/&lt;path&gt;</c>
    /// (any separator, the folder's case ignored: Windows paths).</summary>
    internal static string Text(string s, string root) => Text(s, [root]);

    /// <inheritdoc cref="Text(string, string)"/>
    internal static string Text(string s, IReadOnlyList<string> roots)
    {
        if (s.IndexOf('/') < 0 && s.IndexOf('\\') < 0) return s;
        var p = Slashes(s);
        foreach (var root in roots)
            if (root.Length > 0 && p.Length > root.Length && p[root.Length] == '/' && p.StartsWith(root, StringComparison.OrdinalIgnoreCase))
                return "$mod" + p[root.Length..];
        return s;
    }

    /// <summary>An exact decimal value, without rounding through a binary floating-point type or expanding a huge exponent.</summary>
    internal static string Number(string raw)
    {
        var negative = raw.StartsWith('-');
        var value = negative ? raw[1..] : raw;
        var at = value.IndexOfAny(['e', 'E']);
        var exponent = at < 0 ? BigInteger.Zero : BigInteger.Parse(value[(at + 1)..], CultureInfo.InvariantCulture);
        var mantissa = at < 0 ? value : value[..at];
        var dot = mantissa.IndexOf('.');
        if (dot >= 0) exponent -= mantissa.Length - dot - 1;
        var digits = mantissa.Replace(".", "", StringComparison.Ordinal).TrimStart('0');
        if (digits.Length == 0) return "0";
        var trimmed = digits.TrimEnd('0');
        exponent += digits.Length - trimmed.Length;
        digits = trimmed;
        var position = digits.Length + exponent;
        string result;
        if (exponent >= 0 && position <= 32) result = digits + new string('0', (int)exponent);
        else if (exponent < 0 && position > 0 && position < digits.Length) result = digits.Insert((int)position, ".");
        else if (position <= 0 && position >= -16) result = "0." + new string('0', (int)-position) + digits;
        else result = digits + "e" + exponent.ToString(CultureInfo.InvariantCulture);
        return negative ? "-" + result : result;
    }

    private static void Canonical(StringBuilder sb, JsonNode? node, IReadOnlyList<string> roots)
    {
        if (node is null)
        {
            sb.Append("null");
            return;
        }
        using var doc = JsonDocument.Parse(node.ToJsonString());
        Canonical(sb, doc.RootElement, roots);
    }

    private static void Canonical(StringBuilder sb, JsonElement e, IReadOnlyList<string> roots)
    {
        switch (e.ValueKind)
        {
            case JsonValueKind.Object:
            {
                sb.Append('{');
                var first = true;
                foreach (var p in e.EnumerateObject().OrderBy(p => p.Name, StringComparer.Ordinal))
                {
                    if (!first) sb.Append(',');
                    first = false;
                    Str(sb, p.Name);
                    sb.Append(':');
                    Canonical(sb, p.Value, roots);
                }
                sb.Append('}');
                return;
            }
            case JsonValueKind.Array:
            {
                sb.Append('[');
                var first = true;
                foreach (var x in e.EnumerateArray())
                {
                    if (!first) sb.Append(',');
                    first = false;
                    Canonical(sb, x, roots);
                }
                sb.Append(']');
                return;
            }
            case JsonValueKind.String: Str(sb, Text(e.GetString()!, roots)); return;
            case JsonValueKind.Number: sb.Append(Number(e.GetRawText())); return;
            case JsonValueKind.True: sb.Append("true"); return;
            case JsonValueKind.False: sb.Append("false"); return;
            default: sb.Append("null"); return;
        }
    }

    // A JSON string with a fixed escaping of its own (quote, backslash, control characters), so no encoder's version changes the bytes.
    private static void Str(StringBuilder sb, string s)
    {
        sb.Append('"');
        foreach (var c in s)
        {
            if (c == '"') sb.Append("\\\"");
            else if (c == '\\') sb.Append("\\\\");
            else if (c < 0x20) sb.Append("\\u").Append(((int)c).ToString("x4", CultureInfo.InvariantCulture));
            else sb.Append(c);
        }
        sb.Append('"');
    }
}
