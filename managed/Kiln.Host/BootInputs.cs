// SPDX-License-Identifier: MIT
// The frozen effective inputs of a boot mod (chapter 08 §4.4): what a later runtime load of the mod compares against the boot generation
// that holds its content. The set is what the boot phase reads for the mod, by name, and nothing else:
//
//   kiln.mod.json                     the manifest (the boot graph, the roles, "clients", "boot")
//   *.dll at the top of the folder    the entry assembly and its helper assemblies (the author's load context loads them from there)
//   blocks/, blocks.tables.json       the data files: the boot lowering's shadow check, kiln.worldtables, kiln.content's patches, kiln.vfx,
//                                     kiln.quests (blocks/generated/ is a baked file and part of it)
//   assets/                           kiln.assets' assets.json and every file it names, cooked/ and cook.json (kiln.quests reads the latter)
//   quests/, skills/                  kiln.quests and kiln.skills
//
// Each file counts by its content. A JSON file (*.json) counts by its parsed value with key order kept: a tool that re-writes it with other
// white space, comments, line endings or a byte-order mark (a launcher, an editor, a checkout) changes nothing, any other edit does. A
// file that does not parse counts by its bytes. Not inputs: names starting with "." at any depth (.disabled, .kiln-source.json and the
// launcher's other markers and temporary files) and "*.kiln-tmp"; the mod's data folder and kiln/config (what the runtime and the mod
// write while they run: pending caches, config files the runtime re-writes from its running values; Config cannot be read in OnBoot, so
// no configuration is in a boot generation). A link is not followed: its target counts as its content.
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace Kiln.Host;

/// <summary>A boot mod's frozen inputs: each input file's digest by its path in the mod folder (with "/"), and one hash over all of them.</summary>
internal sealed class BootInputs
{
    /// <summary>The folders under the mod folder whose every file is an input.</summary>
    internal static readonly string[] Trees = ["blocks", "assets", "quests", "skills"];

    /// <summary>The single files at the top of the mod folder that are inputs (besides the top-level assemblies).</summary>
    internal static readonly string[] TopFiles = [Manifest.FileName, "blocks.tables.json"];

    /// <summary>A JSON file larger than this counts by its bytes (it is not parsed to be compared).</summary>
    internal const int MaxParsed = 16 << 20;

    private readonly SortedDictionary<string, string> _files;

    private BootInputs(SortedDictionary<string, string> files, string hash, bool changedDuringBoot)
    {
        _files = files;
        Hash = hash;
        ChangedDuringBoot = changedDuringBoot;
    }

    /// <summary>Each input's digest (lower-case hex SHA-256 of its counted content), by path.</summary>
    public IReadOnlyDictionary<string, string> Files => _files;

    /// <summary>One SHA-256 over every path and digest, in ordinal path order: the fingerprint the IR envelope carries.</summary>
    public string Hash { get; }

    /// <summary>The inputs differed between the author's OnBoot and the commit: what the generation holds matches neither, so no
    /// later load matches it.</summary>
    public bool ChangedDuringBoot { get; }

    /// <summary>The inputs of the mod in <paramref name="directory"/> as they are now.</summary>
    public static BootInputs Read(string directory)
    {
        var files = new SortedDictionary<string, string>(StringComparer.Ordinal);
        var root = new DirectoryInfo(directory);
        if (root.Exists)
        {
            foreach (var name in TopFiles)
                Add(files, new FileInfo(Path.Combine(directory, name)), name);
            foreach (var dll in Safe(() => root.EnumerateFiles("*.dll", SearchOption.TopDirectoryOnly)))
                if (Counted(dll.Name)) Add(files, dll, dll.Name);
            foreach (var tree in Trees)
                Walk(files, new DirectoryInfo(Path.Combine(directory, tree)), tree, 0);
        }
        return new BootInputs(files, Root(files), false);
    }

    /// <summary>The inputs read before the author's OnBoot, checked again at the commit: unchanged, or marked
    /// <see cref="ChangedDuringBoot"/>.</summary>
    public BootInputs Recheck(string directory)
    {
        var now = Read(directory);
        return now.Hash == Hash ? this : new BootInputs(_files, Hash, true);
    }

    /// <summary>What differs from <paramref name="frozen"/> in this reading, as "path changed", "path added", "path removed" (ordinal order).</summary>
    public List<string> Differences(BootInputs frozen)
    {
        var list = new List<string>();
        foreach (var (path, digest) in frozen.Files)
            if (!Files.TryGetValue(path, out var now)) list.Add($"{path} removed");
            else if (now != digest) list.Add($"{path} changed");
        foreach (var path in Files.Keys)
            if (!frozen.Files.ContainsKey(path)) list.Add($"{path} added");
        list.Sort(StringComparer.Ordinal);
        return list;
    }

    private static bool Counted(string name) => !name.StartsWith('.') && !name.EndsWith(".kiln-tmp", StringComparison.OrdinalIgnoreCase);

    private static IEnumerable<T> Safe<T>(Func<IEnumerable<T>> list)
    {
        try { return list().ToList(); }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or System.Security.SecurityException) { return []; }
    }

    private static void Walk(SortedDictionary<string, string> files, DirectoryInfo dir, string path, int depth)
    {
        if (!dir.Exists) return;
        if (dir.LinkTarget is { } link)
        {
            files[path + "/"] = Digest(Encoding.UTF8.GetBytes("link:" + link));
            return;
        }
        if (depth > 32)
        {
            files[path + "/"] = Digest(Encoding.UTF8.GetBytes("too-deep"));
            return;
        }
        foreach (var e in Safe(dir.EnumerateFileSystemInfos))
        {
            if (!Counted(e.Name)) continue;
            var at = path + "/" + e.Name;
            if (e is DirectoryInfo d) Walk(files, d, at, depth + 1);
            else if (e is FileInfo f) Add(files, f, at);
        }
    }

    private static void Add(SortedDictionary<string, string> files, FileInfo file, string path)
    {
        if (!file.Exists) return;
        if (file.LinkTarget is { } link)
        {
            files[path] = Digest(Encoding.UTF8.GetBytes("link:" + link));
            return;
        }
        byte[] bytes;
        try { bytes = File.ReadAllBytes(file.FullName); }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or System.Security.SecurityException)
        {
            files[path] = Digest(Encoding.UTF8.GetBytes("unreadable"));  // readable later: a change (fail closed)
            return;
        }
        files[path] = Digest(path.EndsWith(".json", StringComparison.OrdinalIgnoreCase) ? JsonContent(bytes) : bytes);
    }

    /// <summary>A JSON file's counted content: its value written again without white space, comments or a byte-order mark, key order kept;
    /// its bytes when it does not parse (or is larger than <see cref="MaxParsed"/>).</summary>
    internal static byte[] JsonContent(byte[] bytes)
    {
        if (bytes.Length > MaxParsed) return bytes;
        try
        {
            var span = bytes.AsSpan();
            if (span.StartsWith((ReadOnlySpan<byte>)[0xEF, 0xBB, 0xBF])) span = span[3..];
            var node = JsonNode.Parse(span, documentOptions: new JsonDocumentOptions { CommentHandling = JsonCommentHandling.Skip, AllowTrailingCommas = true });
            return Encoding.UTF8.GetBytes("json:" + (node?.ToJsonString() ?? "null"));
        }
        catch (JsonException)
        {
            return bytes;
        }
    }

    private static string Digest(byte[] bytes) => Convert.ToHexString(SHA256.HashData(bytes)).ToLowerInvariant();

    private static string Root(SortedDictionary<string, string> files)
    {
        using var sha = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
        foreach (var (path, digest) in files) sha.AppendData(Encoding.UTF8.GetBytes(path + "\0" + digest + "\n"));
        return Convert.ToHexString(sha.GetHashAndReset()).ToLowerInvariant();
    }
}
