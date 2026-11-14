// SPDX-License-Identifier: MIT
// The frozen effective inputs of a boot mod (chapter 08 §4.4): what a later runtime load of the mod compares against the boot generation
// that holds its content.
//
// Files: every file of the mod's folder, at any depth, except what is never an input or is written while the game runs:
//   - names starting with "." at any depth (.disabled, .kiln-source.json, the launcher's .kiln-cooked-prev-* and other markers and
//     temporary files) and "*.kiln-tmp";
//   - at the top of the folder: *.deps.json and *.runtimeconfig.json (no loader reads them; a deploy leaves them out), Kiln.Api.dll,
//     a Kiln.*.dll that Kiln.Host has beside it and the other files of those (Kiln.*.pdb, .xml): the loader takes Kiln's assemblies
//     from the host, never from the mod, and a deploy leaves them out; unless the manifest names one as its entry assembly or an export;
//   - the mod's data folder and kiln/config, should they lie inside the mod's folder (what the mod and Kiln write while they run).
// Everything else counts, so a file OnBoot reads itself is covered wherever it is in the folder. Each file counts by its content. A JSON
// file (*.json) counts by its parsed value with key order kept: a tool that re-writes it with other white space, comments, line endings or
// a byte-order mark changes nothing, any other edit does. A file that does not parse counts by its bytes. A link is not followed: its
// target counts as its content.
//
// Speed: a file's digest is kept in this process by its full path, size and last write time, so a second reading (the check at the
// commit, every later load of the mod) hashes only the files whose size or time changed. The first reading of a start hashes all. A file
// written in the last two seconds is not kept (a coarse file time could hide a second edit of the same size).
//
// Configuration: Config.Register works in OnBoot (read only). Each config the mod's OnBoot registered is frozen by its effective values
// (Config.BootValues: the file's values validated and clamped, else the defaults), not by the file's bytes: the runtime registration's
// canonical rewrite of the file changes nothing. A later load compares them with the values the file gives now (BootConfigs.Changed).
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace Kiln.Host;

/// <summary>A boot mod's frozen inputs: each input file's digest by its path in the mod folder (with "/"), one hash over all of them, and
/// the configs its OnBoot read.</summary>
internal sealed class BootInputs
{
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

    /// <summary>Why the inputs could not be read (they then match no later reading), or null.</summary>
    public string? Error { get; private init; }

    /// <summary>The configs the mod's OnBoot registered, with the effective values it read.</summary>
    public IReadOnlyList<Config.BootValues> Configs { get; private init; } = [];

    /// <summary>How the reading went: files, bytes, how many were hashed (the others came from the digest cache), milliseconds.</summary>
    public (int Files, long Bytes, int Hashed, long Milliseconds) Cost { get; private init; }

    /// <summary>These inputs with the configs the mod's OnBoot read.</summary>
    public BootInputs WithConfigs(IReadOnlyList<Config.BootValues> configs) =>
        new(_files, Hash, ChangedDuringBoot) { Error = Error, Configs = configs, Cost = Cost };

    /// <summary>The inputs of the mod in <paramref name="directory"/> as they are now. <paramref name="runtime"/>: folders written
    /// while the game runs (the mod's data folder, kiln/config), left out should they lie inside it. Never throws: a reading that
    /// fails has <see cref="Error"/> set (the boot phase goes on; such a mod loads again only after a restart).</summary>
    public static BootInputs Read(string directory, params string[] runtime)
    {
        var files = new SortedDictionary<string, string>(StringComparer.Ordinal);
        var started = System.Diagnostics.Stopwatch.StartNew();
        var walk = new Walker(files, runtime.Where(r => r.Length > 0).Select(Full).ToArray());
        try
        {
            var root = new DirectoryInfo(directory);
            if (root.Exists)
            {
                walk.Own = OwnAssemblies(Path.Combine(directory, Manifest.FileName));
                walk.Folder(root, "", 0);
            }
        }
        catch (Exception e)
        {
            return new BootInputs(files, "", false) { Error = $"{e.GetType().Name}: {e.Message}" };
        }
        return new BootInputs(files, Root(files), false) { Cost = (files.Count, walk.Bytes, walk.Hashed, started.ElapsedMilliseconds) };
    }

    /// <summary>The inputs read before the author's OnBoot, checked against <paramref name="now"/> (read at the commit): unchanged, or
    /// marked <see cref="ChangedDuringBoot"/>.</summary>
    public BootInputs Recheck(BootInputs now) => Error == null && now.Error == null && now.Hash == Hash
            ? this
            : new BootInputs(_files, Hash, true) { Error = Error ?? now.Error, Configs = Configs, Cost = Cost };

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

    // ---- the walk ----

    private static readonly string HostDirectory = Path.GetDirectoryName(typeof(BootInputs).Assembly.Location) ?? "";

    /// <summary>Digests by full path, with the size and write time they were made for (this process only).</summary>
    private static readonly Dictionary<string, (long Length, long Ticks, string Digest)> Digests = new(StringComparer.Ordinal);

    private static string Full(string path) => Path.TrimEndingDirectorySeparator(Path.GetFullPath(path));

    private sealed class Walker(SortedDictionary<string, string> files, string[] runtime)
    {
        public HashSet<string> Own = new(StringComparer.OrdinalIgnoreCase);
        public long Bytes;
        public int Hashed;

        public void Folder(DirectoryInfo dir, string path, int depth)
        {
            if (depth > 0 && dir.LinkTarget is { } link)
            {
                files[path + "/"] = Digest(Encoding.UTF8.GetBytes("link:" + link));
                return;
            }
            if (depth > 32)
            {
                files[path + "/"] = Digest(Encoding.UTF8.GetBytes("too-deep"));
                return;
            }
            var full = Full(dir.FullName);
            if (depth > 0 && runtime.Any(r => string.Equals(r, full, OperatingSystem.IsWindows() ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal)))
                return;
            foreach (var e in Safe(dir.EnumerateFileSystemInfos))
            {
                if (!Counted(e.Name, depth)) continue;
                var at = path.Length == 0 ? e.Name : path + "/" + e.Name;
                if (e is DirectoryInfo d) Folder(d, at, depth + 1);
                else if (e is FileInfo f) File(f, at);
            }
        }

        private bool Counted(string name, int depth)
        {
            if (name.StartsWith('.') || name.EndsWith(".kiln-tmp", StringComparison.OrdinalIgnoreCase)) return false;
            if (depth > 0) return true;
            if (name.EndsWith(".deps.json", StringComparison.OrdinalIgnoreCase) || name.EndsWith(".runtimeconfig.json", StringComparison.OrdinalIgnoreCase)) return false;
            if (name.StartsWith("Kiln.", StringComparison.Ordinal))
            {
                var extension = Path.GetExtension(name);
                var assembly = Path.GetFileNameWithoutExtension(name) + ".dll";
                if ((extension.Equals(".dll", StringComparison.OrdinalIgnoreCase) ||
                     extension.Equals(".pdb", StringComparison.OrdinalIgnoreCase) ||
                     extension.Equals(".xml", StringComparison.OrdinalIgnoreCase)) &&
                    Shared(assembly) && !Own.Contains(assembly)) return false;
            }
            return true;
        }

        private void File(FileInfo file, string path)
        {
            if (file.LinkTarget is { } link)
            {
                files[path] = Digest(Encoding.UTF8.GetBytes("link:" + link));
                return;
            }
            long length, ticks;
            try
            {
                length = file.Length;
                ticks = file.LastWriteTimeUtc.Ticks;
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException)
            {
                files[path] = Digest(Encoding.UTF8.GetBytes("unreadable"));
                return;
            }
            Bytes += length;
            var key = file.FullName;
            lock (Digests)
                if (Digests.TryGetValue(key, out var known) && known.Length == length && known.Ticks == ticks)
                {
                    files[path] = known.Digest;
                    return;
                }
            byte[] bytes;
            try { bytes = System.IO.File.ReadAllBytes(key); }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException or System.Security.SecurityException)
            {
                files[path] = Digest(Encoding.UTF8.GetBytes("unreadable"));  // readable later: a change (fail closed)
                return;
            }
            Hashed++;
            var digest = Digest(path.EndsWith(".json", StringComparison.OrdinalIgnoreCase) ? JsonContent(bytes) : bytes);
            files[path] = digest;
            // kept only when the file did not change while it was read, and not when it was written in the last two seconds: a file system
            // stamps times coarsely (milliseconds on Linux), so a quick second edit of the same size could carry the same time (as git's
            // index, a racily clean entry is hashed again)
            if (bytes.LongLength == length && DateTime.UtcNow.Ticks - ticks > 2 * TimeSpan.TicksPerSecond &&
                new FileInfo(key) is { Exists: true } after && after.Length == length && after.LastWriteTimeUtc.Ticks == ticks)
                lock (Digests) Digests[key] = (length, ticks, digest);
        }
    }

    /// <summary>An assembly every mod takes from Kiln.Host, not from its folder (ModLoadContext): Kiln.Api, and Kiln's shared Kiln.*.</summary>
    private static bool Shared(string file) =>
        file.Equals("Kiln.Api.dll", StringComparison.OrdinalIgnoreCase) ||
        (file.StartsWith("Kiln.", StringComparison.Ordinal) && HostDirectory.Length > 0 && System.IO.File.Exists(Path.Combine(HostDirectory, file)));

    /// <summary>The assemblies the manifest names (entryAssembly, exports): loaded from the mod's folder whatever their name.</summary>
    private static HashSet<string> OwnAssemblies(string manifest)
    {
        var own = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        try
        {
            if (JsonNode.Parse(System.IO.File.ReadAllText(manifest), documentOptions: new JsonDocumentOptions { CommentHandling = JsonCommentHandling.Skip, AllowTrailingCommas = true }) is JsonObject m)
            {
                if (m["entryAssembly"] is JsonValue e && e.TryGetValue<string>(out var entry)) own.Add(entry);
                if (m["exports"] is JsonArray exports)
                    foreach (var x in exports)
                        if (x is JsonValue v && v.TryGetValue<string>(out var name)) own.Add(name);
            }
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or JsonException or ArgumentException or InvalidOperationException) { }
        return own;
    }

    private static IEnumerable<T> Safe<T>(Func<IEnumerable<T>> list)
    {
        try { return list().ToList(); }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or System.Security.SecurityException) { return []; }
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
        catch (Exception e) when (e is JsonException or ArgumentException or InvalidOperationException)
        {
            return bytes;  // not JSON (or a duplicate key, too deep): its bytes count
        }
    }

    private static string Digest(byte[] bytes) => Convert.ToHexString(SHA256.HashData(bytes)).ToLowerInvariant();

    private static string Root(SortedDictionary<string, string> files)
    {
        using var sha = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
        foreach (var (path, digest) in files) sha.AppendData(Encoding.UTF8.GetBytes(path + "\0" + digest + "\n"));
        return Convert.ToHexString(sha.GetHashAndReset()).ToLowerInvariant();
    }

    /// <summary>Test hook: forget the digest cache (a test that edits a file within the file system's time resolution).</summary>
    internal static void ForgetDigests()
    {
        lock (Digests) Digests.Clear();
    }
}
