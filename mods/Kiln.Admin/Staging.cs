// SPDX-License-Identifier: MIT
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace Kiln.Admin;

internal sealed class StagingError(string message) : Exception(message);

/// <summary>Staged updates (docs/kiln/staged-updates.md): the update_* ops' side. Kiln.Admin only stages, cancels, requests
/// a rollback and lists; the next start applies (the proxy for Kiln, kiln-core for mods: native/core/src/staged.cpp,
/// whose path rules these mirror). A source is a folder the caller prepared under kiln/staged/incoming (the web mod
/// downloads and checks the signature first); it is copied, never moved or changed, and every file is hashed as it is
/// copied. A staged unit appears at once or not at all: it is built in kiln/staged/.stage-&lt;random&gt; with its
/// staged.json written last, then renamed into place. Nothing here reads or writes kiln/data. Plain System.* code, so a
/// test project links this file as it is.</summary>
internal static class Staging
{
    public const int Format = 1, MaxFiles = 20000, MaxRelPath = 240, ResultsListed = 20;
    private static readonly object Gate = new();  // one staging at a time
    /// <summary>Kiln's own folders, which the manual update of a server pack install replaces whole
    /// (docs/kiln/dedicated-servers.md, "Updating Kiln").</summary>
    internal static readonly string[] PackFolders = ["kiln/dotnet/", "kiln/host/", "kiln/services/", "kiln/sigdb/"];
    private static readonly string[] OwnFolders =
        ["kiln/staged", "kiln/data", "kiln/config", "kiln/logs", "kiln/state", "kiln/backups", "kiln/control", "kiln/crash-reports"];

    /// <summary>The server's folders as Kiln.Admin sees them.</summary>
    internal sealed class Layout(string kiln)
    {
        public string Kiln { get; } = Path.TrimEndingDirectorySeparator(Path.GetFullPath(kiln));
        public string Game => Path.GetDirectoryName(Kiln)!;
        public string Staged => Path.Combine(Kiln, "staged");
        public string Incoming => Path.Combine(Staged, "incoming");
        public string Mods => Path.Combine(Kiln, "mods");
        public string Receipt => Path.Combine(Kiln, ".install.json");
        public string StagedDir(string unit) => unit == "kiln" ? Path.Combine(Staged, "kiln") : Path.Combine(Staged, "mods", Folder(unit));
        public string PreviousDir(string unit) => unit == "kiln" ? Path.Combine(Staged, "previous", "kiln") : Path.Combine(Staged, "previous", "mods", Folder(unit));
        public string RollbackFile(string unit) => Path.Combine(Staged, "rollback", unit + ".json");
        public string ProbationFile(string unit) => Path.Combine(Staged, "probation", unit + ".json");
        public string TxDir(string unit) => Path.Combine(Staged, "tx", unit);
        public string AttemptsFile(string unit) => Path.Combine(Staged, "attempts", unit);
        private static string Folder(string unit) => unit["mod.".Length..];
    }

    // ---- path rules (native/core/src/staged.cpp rel_path_ok, kiln_path_ok, mod_folder_ok) ----
    public static bool RelPathOk(string rel, out string why)
    {
        why = "";
        string Fail(string text) => $"path {JsonSerializer.Serialize(rel)}: {text}";
        if (rel.Length == 0) { why = Fail("empty"); return false; }
        int bytes;
        try { bytes = new UTF8Encoding(false, true).GetByteCount(rel); }
        catch (EncoderFallbackException) { why = Fail("not UTF-8"); return false; }
        if (bytes > MaxRelPath) { why = Fail($"longer than {MaxRelPath} bytes"); return false; }
        foreach (var c in rel)
        {
            if (c < 0x20 || c == 0x7F) { why = Fail("a control character"); return false; }
            if (c == '\\') { why = Fail("a backslash (paths use /)"); return false; }
            if (":*?\"<>|".Contains(c)) { why = Fail($"the character {c}"); return false; }
        }
        if (rel[0] == '/') { why = Fail("absolute"); return false; }
        foreach (var seg in rel.Split('/'))
        {
            if (seg.Length == 0) { why = Fail("an empty segment"); return false; }
            if (seg is "." or "..") { why = Fail("a . or .. segment"); return false; }
            if (seg[^1] is '.' or ' ') { why = Fail("a segment ending in a dot or a space"); return false; }
            if (DeviceName(seg)) { why = Fail("a device name"); return false; }
        }
        return true;
    }

    private static bool DeviceName(string segment)
    {
        var dot = segment.IndexOf('.');
        var b = Lower(dot < 0 ? segment : segment[..dot]).TrimEnd(' ');
        if (b is "con" or "prn" or "aux" or "nul" or "conin$" or "conout$") return true;
        var port = b.StartsWith("com", StringComparison.Ordinal) || b.StartsWith("lpt", StringComparison.Ordinal);
        return port && b.Length == 4 && (char.IsAsciiDigit(b[3]) || b[3] is '¹' or '²' or '³');
    }

    private static string Lower(string s)
    {
        var chars = s.ToCharArray();
        for (var i = 0; i < chars.Length; i++)
            if (chars[i] is >= 'A' and <= 'Z') chars[i] = (char)(chars[i] + 32);
        return new string(chars);
    }

    public static bool KilnPathOk(string rel, out string why)
    {
        if (!RelPathOk(rel, out why)) return false;
        var l = Lower(rel);
        if (l == "dinput8.dll") return true;
        if (!l.StartsWith("kiln/", StringComparison.Ordinal))
        {
            why = $"path {JsonSerializer.Serialize(rel)}: outside kiln/ (a Kiln update writes dinput8.dll and kiln/... only)";
            return false;
        }
        foreach (var own in OwnFolders)
        {
            if (l == own || l.StartsWith(own + "/", StringComparison.Ordinal))
            {
                why = $"path {JsonSerializer.Serialize(rel)}: under {own}, which holds the server's own files";
                return false;
            }
        }
        return true;
    }

    public static bool ModFolderOk(string folder) =>
        RelPathOk(folder, out _) && !folder.Contains('/') && Encoding.UTF8.GetByteCount(folder) <= 64 && folder[0] != '.';

    /// <summary>"kiln" or "mod.&lt;folder&gt;", checked.</summary>
    public static string Unit(string? unit)
    {
        if (unit == "kiln") return unit;
        if (unit is not null && unit.StartsWith("mod.", StringComparison.Ordinal) && ModFolderOk(unit["mod.".Length..])) return unit;
        throw new StagingError("unit must be \"kiln\" or \"mod.<folder>\" (a folder under kiln/mods)");
    }

    // ---- files ----
    private static bool IsLink(FileSystemInfo i) => (i.Attributes & FileAttributes.ReparsePoint) != 0 || i.LinkTarget is not null;

    /// <summary>The source folder: under kiln/staged/incoming, a real folder, no link on the way.</summary>
    private static string Source(Layout p, string? source)
    {
        if (string.IsNullOrWhiteSpace(source)) throw new StagingError("source required (a folder under kiln/staged/incoming)");
        var incoming = Path.GetFullPath(p.Incoming);
        var full = Path.TrimEndingDirectorySeparator(Path.GetFullPath(Path.IsPathRooted(source) ? source : Path.Combine(incoming, source)));
        var cmp = OperatingSystem.IsWindows() ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal;
        if (!full.StartsWith(incoming + Path.DirectorySeparatorChar, cmp))
            throw new StagingError($"source must be a folder under {incoming} (sources are copied from there only)");
        for (var d = new DirectoryInfo(full); d is not null && !string.Equals(d.FullName.TrimEnd(Path.DirectorySeparatorChar), incoming, cmp); d = d.Parent)
        {
            if (!d.Exists) throw new StagingError($"source {full} does not exist");
            if (IsLink(d)) throw new StagingError($"source {full} leads through a link ({d.FullName})");
        }
        return full;
    }

    internal sealed record FileEntry(string Path, string Sha256, long Size);

    /// <summary>Every regular file under <paramref name="root"/> (relative, '/'), links and other kinds refused.</summary>
    private static List<(string Rel, string Full)> Walk(string root)
    {
        var found = new List<(string, string)>();
        var stack = new Stack<DirectoryInfo>([new DirectoryInfo(root)]);
        while (stack.Count > 0)
        {
            var dir = stack.Pop();
            foreach (var entry in dir.EnumerateFileSystemInfos())
            {
                var rel = System.IO.Path.GetRelativePath(root, entry.FullName).Replace('\\', '/');
                if (IsLink(entry)) throw new StagingError($"the source holds a link: {rel}");
                if (entry is DirectoryInfo d) stack.Push(d);
                else
                {
                    if (!RelPathOk(rel, out var why)) throw new StagingError(why);
                    if (found.Count >= MaxFiles) throw new StagingError($"more than {MaxFiles} files");
                    found.Add((rel, entry.FullName));
                }
            }
        }
        found.Sort((a, b) => string.CompareOrdinal(a.Item1, b.Item1));
        var seen = new HashSet<string>(StringComparer.Ordinal);
        foreach (var (rel, _) in found)
            if (!seen.Add(Lower(rel))) throw new StagingError($"{rel} is in the source twice (letter case does not count on Windows)");
        return found;
    }

    private static string Hex(byte[] hash) => Convert.ToHexStringLower(hash);

    /// <summary>The SHA-256 of a file, or null when it cannot be read.</summary>
    internal static string? Sha256Of(string file)
    {
        try
        {
            using var s = File.OpenRead(file);
            return Hex(SHA256.HashData(s));
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            return null;
        }
    }

    /// <summary>Copies one file, hashing what it copies, flushed to the disk.</summary>
    private static FileEntry Copy(string from, string to, string rel, CancellationToken ct)
    {
        Directory.CreateDirectory(System.IO.Path.GetDirectoryName(to)!);
        using var hash = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
        long size = 0;
        using (var src = new FileStream(from, FileMode.Open, FileAccess.Read, FileShare.Read, 1 << 16))
        using (var dst = new FileStream(to, FileMode.CreateNew, FileAccess.Write, FileShare.None, 1 << 16))
        {
            var buffer = new byte[1 << 16];
            int n;
            while ((n = src.Read(buffer, 0, buffer.Length)) > 0)
            {
                ct.ThrowIfCancellationRequested();
                hash.AppendData(buffer, 0, n);
                dst.Write(buffer, 0, n);
                size += n;
            }
            dst.Flush(flushToDisk: true);
        }
        return new FileEntry(rel, Hex(hash.GetHashAndReset()), size);
    }

    private static void WriteFlushed(string file, string text)
    {
        using var s = new FileStream(file, FileMode.CreateNew, FileAccess.Write, FileShare.None);
        var bytes = Encoding.UTF8.GetBytes(text);
        s.Write(bytes);
        s.Flush(flushToDisk: true);
    }

    private static void WriteAtomic(string file, string text)
    {
        Directory.CreateDirectory(System.IO.Path.GetDirectoryName(file)!);
        var tmp = file + ".tmp";
        File.Delete(tmp);
        WriteFlushed(tmp, text);
        File.Move(tmp, file, overwrite: true);
    }

    private static void TryDeleteTree(string dir)
    {
        try
        {
            if (Directory.Exists(dir)) Directory.Delete(dir, recursive: true);
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            // a stale .stage-* folder is removed by kiln-core at the next start
        }
    }

    private static string NewStageDir(Layout p) => Path.Combine(p.Staged, ".stage-" + Convert.ToHexStringLower(RandomNumberGenerator.GetBytes(8)));

    private static readonly JsonDocumentOptions Lenient = new() { CommentHandling = JsonCommentHandling.Skip, AllowTrailingCommas = true };

    private static JsonObject? ReadObject(string file, bool lenient = false)
    {
        try
        {
            return File.Exists(file) ? JsonNode.Parse(File.ReadAllText(file), documentOptions: lenient ? Lenient : default) as JsonObject : null;
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or JsonException)
        {
            return null;
        }
    }

    private static string? Text(JsonNode? o, string key) => o is JsonObject j && j[key] is JsonValue v && v.TryGetValue<string>(out var s) ? s : null;

    private static string Now() => DateTime.UtcNow.ToString("yyyy-MM-dd'T'HH:mm:ss.fff'Z'");

    private static JsonArray FilesJson(IEnumerable<FileEntry> files) =>
        new([.. files.Select(f => (JsonNode)new JsonObject { ["path"] = f.Path, ["sha256"] = f.Sha256, ["size"] = f.Size })]);

    /// <summary>The caller's own list (from the archive it checked the signature of) must be exactly what was copied.</summary>
    private static void Expect(IReadOnlyDictionary<string, string>? expect, IReadOnlyList<FileEntry> files)
    {
        if (expect is null) return;
        var got = files.ToDictionary(f => f.Path, f => f.Sha256, StringComparer.Ordinal);
        foreach (var (path, sha) in expect)
        {
            if (!got.TryGetValue(path, out var have)) throw new StagingError($"files: {path} is not in the source");
            if (!string.Equals(have, sha, StringComparison.OrdinalIgnoreCase))
                throw new StagingError($"files: {path} is {have} in the source, the list says {sha}");
        }
        foreach (var f in files)
            if (!expect.ContainsKey(f.Path)) throw new StagingError($"files: the source has {f.Path}, which the list does not name");
    }

    private static void RefuseBusy(Layout p, string unit, string name)
    {
        if (Directory.Exists(p.TxDir(unit)) && File.Exists(Path.Combine(p.TxDir(unit), "journal.json")))
            throw new StagingError($"a change of {name} is still open from an earlier start; restart the server first");
        if (ReadObject(Path.Combine(p.StagedDir(unit), "staged.json")) is { } s)
            throw new StagingError($"an update of {name} to {Text(s, "version")} is staged already; cancel it first (update_cancel)");
        if (File.Exists(p.RollbackFile(unit))) throw new StagingError($"a rollback of {name} is requested; cancel it first (update_cancel)");
    }

    // Puts a built .stage-* folder in place (one rename) and clears the attempts of an earlier staged change.
    private static void Commit(Layout p, string unit, string stage)
    {
        var target = p.StagedDir(unit);
        Directory.CreateDirectory(Path.GetDirectoryName(target)!);
        if (Directory.Exists(target)) Directory.Delete(target, recursive: true);  // a leftover without staged.json (RefuseBusy saw none)
        Directory.Move(stage, target);
        if (File.Exists(p.AttemptsFile(unit))) File.Delete(p.AttemptsFile(unit));
    }

    // ---- a mod ----
    /// <summary>Stages <paramref name="source"/> (a mod folder with kiln.mod.json) as the next version of
    /// kiln/mods/&lt;folder&gt;: the folder that holds that id now, else the id.</summary>
    public static JsonObject StageMod(string kiln, string? source, string? id, string? folder, IReadOnlyDictionary<string, string>? expect,
        string? label, string by, CancellationToken ct = default)
    {
        lock (Gate)
        {
            var p = new Layout(kiln);
            var src = Source(p, source);
            var manifest = ReadObject(Path.Combine(src, "kiln.mod.json"), lenient: true)
                           ?? throw new StagingError("the source has no readable kiln.mod.json at its root");
            var mid = Text(manifest, "id");
            var version = Text(manifest, "version");
            if (string.IsNullOrEmpty(mid) || string.IsNullOrEmpty(version)) throw new StagingError("kiln.mod.json has no id or no version");
            if (id is not null && id != mid) throw new StagingError($"kiln.mod.json says id {mid}, not {id}");
            folder ??= FolderOf(p, mid) ?? mid;
            if (!ModFolderOk(folder)) throw new StagingError($"{JsonSerializer.Serialize(folder)} cannot be a folder under kiln/mods (pass folder)");
            var unit = "mod." + folder;
            if (KilnOwns(p, $"kiln/mods/{folder}/"))
                throw new StagingError($"kiln/mods/{folder} is part of Kiln (its install receipt lists it); it is updated with Kiln");
            RefuseBusy(p, unit, $"mod folder {folder}");
            var stage = NewStageDir(p);
            try
            {
                var files = Walk(src).Select(f => Copy(f.Full, Path.Combine(stage, "files", f.Rel), f.Rel, ct)).ToList();
                Expect(expect, files);
                var from = Text(ReadObject(Path.Combine(p.Mods, folder, "kiln.mod.json"), lenient: true), "version");
                var doc = new JsonObject
                {
                    ["format"] = Format, ["kind"] = "mod", ["folder"] = folder, ["id"] = mid, ["version"] = version, ["from"] = from,
                    ["staged"] = Now(), ["by"] = by, ["label"] = label, ["files"] = FilesJson(files),
                };
                WriteFlushed(Path.Combine(stage, "staged.json"), doc.ToJsonString(new JsonSerializerOptions { WriteIndented = true }));
                Commit(p, unit, stage);
                return new JsonObject { ["unit"] = unit, ["staged"] = StagedSummary(doc), ["note"] = "applied at the next start of the server" };
            }
            catch
            {
                TryDeleteTree(stage);
                throw;
            }
        }
    }

    /// <summary>The kiln/mods folder whose kiln.mod.json says <paramref name="id"/> (the only one), if any.</summary>
    private static string? FolderOf(Layout p, string id)
    {
        if (!Directory.Exists(p.Mods)) return null;
        var hits = Directory.EnumerateDirectories(p.Mods)
            .Where(d => Text(ReadObject(Path.Combine(d, "kiln.mod.json"), lenient: true), "id") == id)
            .Select(d => Path.GetFileName(d)!).ToList();
        return hits.Count switch
        {
            0 => null,
            1 => hits[0],
            _ => throw new StagingError($"mod {id} is in more than one folder ({string.Join(", ", hits)}); pass folder"),
        };
    }

    private static bool KilnOwns(Layout p, string prefix)
    {
        if (ReadObject(p.Receipt)?["files"] is not JsonArray files) return false;
        return files.Any(f => Text(f, "path") is { } path && Lower(path).StartsWith(Lower(prefix), StringComparison.Ordinal));
    }

    // ---- Kiln ----
    /// <summary>Stages a Kiln runtime bundle (its files under <paramref name="source"/> laid out from the game folder, with
    /// kiln-runtime.json at the root) as an update of this install: the components its receipt (kiln/.install.json) has, as
    /// kilnctl's upgrade picks them (kiln_manage::install::resolve_components), only the files that change, the files the
    /// new version no longer has (removed only when unchanged), the player's config files kept, and the new receipt last.
    /// A server pack install has no receipt: then the server's default components (what the pack holds), and what the
    /// manual update in dedicated-servers.md does: Kiln's own folders (<see cref="PackFolders"/>) end up exactly the new
    /// version's, nothing else is removed, and a receipt is written so the next update knows the install.</summary>
    public static JsonObject StageKiln(string kiln, string? source, IReadOnlyDictionary<string, string>? expect, string? label,
        string? bundleSha256, string by, CancellationToken ct = default)
    {
        lock (Gate)
        {
            var p = new Layout(kiln);
            var src = Source(p, source);
            RefuseBusy(p, "kiln", "Kiln");
            var manifest = ReadObject(Path.Combine(src, "kiln-runtime.json")) ?? throw new StagingError("the source has no readable kiln-runtime.json");
            var version = Text(manifest, "version");
            if (string.IsNullOrEmpty(version)) throw new StagingError("kiln-runtime.json has no version");
            // no receipt: a server pack install (unzipped by hand); it gets one with this update
            var hasReceipt = File.Exists(p.Receipt);
            var receiptBytes = hasReceipt ? File.ReadAllBytes(p.Receipt) : null;
            JsonObject receipt;
            try
            {
                receipt = hasReceipt ? JsonNode.Parse(receiptBytes) as JsonObject ?? throw new StagingError("kiln/.install.json is not a JSON object")
                    : new JsonObject
                    {
                        ["format"] = 1, ["installId"] = Guid.NewGuid().ToString("D"), ["installed"] = Now(), ["side"] = "server",
                        ["runtime"] = new JsonObject { ["components"] = new JsonArray() }, ["files"] = new JsonArray(), ["replaced"] = new JsonArray(),
                    };
            }
            catch (JsonException)
            {
                throw new StagingError("kiln/.install.json is not JSON");
            }
            if (Text(receipt, "side") != "server") throw new StagingError("kiln/.install.json is not a server install");
            if (expect is not null)
                Expect(expect, Walk(src).Select(f => new FileEntry(f.Rel, Sha256Of(f.Full) ?? "", new FileInfo(f.Full).Length)).ToList());

            var components = ResolveComponents(manifest, receipt, out var deselected);
            var selected = new List<JsonObject>();
            var selectedPaths = new HashSet<string>(StringComparer.Ordinal);
            foreach (var f in manifest["files"] as JsonArray ?? throw new StagingError("kiln-runtime.json has no files"))
            {
                if (f is not JsonObject fo || !components.Contains(Text(fo, "component") ?? "")) continue;
                var path = Text(fo, "path") ?? "";
                if (!KilnPathOk(path, out var why)) throw new StagingError("kiln-runtime.json: " + why);
                if (!selectedPaths.Add(Lower(path))) throw new StagingError($"kiln-runtime.json: {path} twice");
                selected.Add(fo);
            }
            if (selected.Count == 0) throw new StagingError("the bundle has no files for this install's components");

            var receiptFiles = (receipt["files"] as JsonArray ?? []).OfType<JsonObject>().ToList();
            string? ReceiptSha(string path) => receiptFiles.Where(r => Lower(Text(r, "path") ?? "") == Lower(path)).Select(r => Text(r, "sha256")).FirstOrDefault();

            var stage = NewStageDir(p);
            try
            {
                var writes = new List<FileEntry>();
                foreach (var f in selected)
                {
                    ct.ThrowIfCancellationRequested();
                    var path = Text(f, "path")!;
                    var want = Text(f, "sha256") ?? "";
                    var target = Path.Combine(p.Game, path);
                    var config = f["config"] is JsonValue cv && cv.TryGetValue<bool>(out var isConfig) && isConfig;
                    if (File.Exists(target) && (config || string.Equals(Sha256Of(target), want, StringComparison.OrdinalIgnoreCase))) continue;
                    if (Lower(path) == "dinput8.dll" && File.Exists(target) &&
                        (hasReceipt ? !string.Equals(Sha256Of(target), ReceiptSha(path), StringComparison.OrdinalIgnoreCase) : !IsKilnProxy(target)))
                        throw new StagingError("dinput8.dll is not the one Kiln installed (another loader, or a changed file): update Kiln with kilnctl or the launcher");
                    var from = Path.Combine(src, path);
                    var info = new FileInfo(from);
                    if (!info.Exists || IsLink(info)) throw new StagingError($"the bundle has no file {path}");
                    var copied = Copy(from, Path.Combine(stage, "files", path), path, ct);
                    if (!string.Equals(copied.Sha256, want, StringComparison.OrdinalIgnoreCase))
                        throw new StagingError($"the bundle's {path} is not what kiln-runtime.json says (sha256 {copied.Sha256}, listed {want})");
                    writes.Add(copied);
                }
                var removes = new JsonArray();
                foreach (var r in receiptFiles)
                {
                    var path = Text(r, "path") ?? "";
                    var isConfig = r["config"] is JsonValue rv && rv.TryGetValue<bool>(out var c) && c;
                    if (isConfig || selectedPaths.Contains(Lower(path)) || !KilnPathOk(path, out _) || Text(r, "sha256") is not { Length: 64 } sha) continue;
                    if (File.Exists(Path.Combine(p.Game, path))) removes.Add(new JsonObject { ["path"] = path, ["sha256"] = sha });
                }
                if (!hasReceipt)  // a server pack install: Kiln's own folders become exactly the new version's
                {
                    foreach (var folder in PackFolders)
                    {
                        var dir = Path.Combine(p.Game, folder.TrimEnd('/'));
                        if (!Directory.Exists(dir)) continue;
                        foreach (var file in Directory.EnumerateFiles(dir, "*", SearchOption.AllDirectories))
                        {
                            var rel = Path.GetRelativePath(p.Game, file).Replace('\\', '/');
                            if (selectedPaths.Contains(Lower(rel)) || !KilnPathOk(rel, out _) || IsLink(new FileInfo(file))) continue;
                            if (Sha256Of(file) is { } sha) removes.Add(new JsonObject { ["path"] = rel, ["sha256"] = sha });
                        }
                    }
                }
                var installed = Text(receipt["runtime"], "version");
                if (writes.Count == 0 && removes.Count == 0 && installed == version) throw new StagingError($"Kiln {version} is installed already");

                // the new receipt, written last by the start that applies it
                var next = (JsonObject)receipt.DeepClone();
                var runtime = next["runtime"] as JsonObject ?? new JsonObject();
                next["runtime"] = runtime;
                runtime["version"] = version;
                if (bundleSha256 is { Length: 64 }) runtime["sha256"] = bundleSha256.ToLowerInvariant();
                else runtime.Remove("sha256");
                runtime["components"] = new JsonArray([.. components.Order(StringComparer.Ordinal).Select(c => (JsonNode)c)]);
                runtime["deselected"] = new JsonArray([.. deselected.Select(c => (JsonNode)c)]);
                next["updated"] = Now();
                next["installer"] = $"kiln-admin (staged update, {by})";
                next["files"] = new JsonArray([.. selected.Select(f => (JsonNode)new JsonObject
                {
                    ["path"] = Text(f, "path"), ["sha256"] = Text(f, "sha256"), ["size"] = f["size"]?.DeepClone(),
                    ["component"] = Text(f, "component"), ["config"] = f["config"]?.DeepClone() ?? false,
                })]);
                var receiptText = next.ToJsonString(new JsonSerializerOptions { WriteIndented = true });
                var receiptFile = Path.Combine(stage, "files", "kiln", ".install.json");
                Directory.CreateDirectory(Path.GetDirectoryName(receiptFile)!);
                WriteFlushed(receiptFile, receiptText);
                var receiptEntryBytes = Encoding.UTF8.GetBytes(receiptText);
                writes.RemoveAll(w => Lower(w.Path) == "kiln/.install.json");
                writes.Add(new FileEntry("kiln/.install.json", Hex(SHA256.HashData(receiptEntryBytes)), receiptEntryBytes.Length));

                var doc = new JsonObject
                {
                    ["format"] = Format, ["kind"] = "kiln", ["version"] = version, ["from"] = installed, ["staged"] = Now(), ["by"] = by,
                    ["label"] = label, ["bundleSha256"] = bundleSha256,
                    ["base"] = new JsonObject { ["receipt"] = receiptBytes is null ? null : Hex(SHA256.HashData(receiptBytes)) },
                    ["files"] = FilesJson(writes), ["remove"] = removes,
                };
                WriteFlushed(Path.Combine(stage, "staged.json"), doc.ToJsonString(new JsonSerializerOptions { WriteIndented = true }));
                Commit(p, "kiln", stage);
                return new JsonObject
                {
                    ["unit"] = "kiln", ["staged"] = StagedSummary(doc),
                    ["note"] = hasReceipt ? "applied at the next start of the server"
                        : "applied at the next start of the server; this install had no receipt (a server pack), so Kiln's own folders " +
                          string.Join(", ", PackFolders.Select(f => f.TrimEnd('/'))) + " are made exactly the new version's and kiln/.install.json is written",
                };
            }
            catch
            {
                TryDeleteTree(stage);
                throw;
            }
        }
    }

    /// <summary>kilnctl's choice for an upgrade (kiln_manage::install::resolve_components, no explicit request): the
    /// installed components the bundle still has for the server, plus every server default the player did not turn off,
    /// with what they require; the turned-off list stays as it was.</summary>
    internal static HashSet<string> ResolveComponents(JsonObject manifest, JsonObject receipt, out List<string> deselected)
    {
        const string side = "server";
        var comps = manifest["components"] as JsonObject ?? throw new StagingError("kiln-runtime.json has no components");
        bool ForServer(string name) => comps[name] is JsonObject c && c["sides"] is JsonArray s && s.Any(x => x?.GetValue<string>() == side);
        bool IsDefault(string name) => comps[name] is JsonObject c && c["default"] is JsonValue d && d.TryGetValue<bool>(out var b) && b;
        static List<string> Strings(JsonNode? n) => n is JsonArray a ? [.. a.Select(x => x is JsonValue v && v.TryGetValue<string>(out var s) ? s : null).OfType<string>()] : [];
        var before = Strings(receipt["runtime"]?["deselected"]);
        var want = Strings(receipt["runtime"]?["components"]).Where(c => !before.Contains(c) && ForServer(c)).ToList();
        foreach (var (name, _) in comps)
            if (IsDefault(name) && ForServer(name) && !before.Contains(name) && !want.Contains(name)) want.Add(name);
        for (var i = 0; i < want.Count; i++)
        {
            foreach (var r in Strings(comps[want[i]]?["requires"]))
            {
                if (want.Contains(r)) continue;
                if (comps[r] is null) throw new StagingError($"component {want[i]} requires unknown {r}");
                if (!ForServer(r)) throw new StagingError($"component {want[i]} requires {r}, which is not for the server side");
                want.Add(r);
            }
        }
        deselected = [.. before.Order(StringComparer.Ordinal)];
        return [.. want];
    }

    /// <summary>The file carries a Kiln proxy's marker ("KilnProxy/&lt;abi&gt; &lt;version&gt;", kiln_manage::pe).</summary>
    private static bool IsKilnProxy(string file)
    {
        try
        {
            var bytes = File.ReadAllBytes(file);
            return bytes.AsSpan().IndexOf("KilnProxy/"u8) >= 0;
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            return false;
        }
    }

    // ---- cancel, rollback, list ----
    public static JsonObject Cancel(string kiln, string? unitArg)
    {
        lock (Gate)
        {
            var p = new Layout(kiln);
            var unit = Unit(unitArg);
            var canceled = new JsonArray();
            var staged = p.StagedDir(unit);
            if (Directory.Exists(staged))
            {
                var gone = NewStageDir(p);  // one rename takes it out of the next start's sight; the rest is tidying up
                Directory.Move(staged, gone);
                TryDeleteTree(gone);
                canceled.Add("update");
            }
            if (File.Exists(p.RollbackFile(unit)))
            {
                File.Delete(p.RollbackFile(unit));
                canceled.Add("rollback");
            }
            if (canceled.Count == 0) throw new StagingError($"nothing is staged or requested for {unit}");
            return new JsonObject { ["unit"] = unit, ["canceled"] = canceled };
        }
    }

    public static JsonObject Rollback(string kiln, string? unitArg, string by)
    {
        lock (Gate)
        {
            var p = new Layout(kiln);
            var unit = Unit(unitArg);
            var kept = ReadObject(Path.Combine(p.PreviousDir(unit), "kept.json"))
                       ?? throw new StagingError($"no earlier version of {unit} is kept (only a change made by a staged update can be rolled back)");
            RefuseBusy(p, unit, unit);
            WriteAtomic(p.RollbackFile(unit), new JsonObject { ["format"] = Format, ["unit"] = unit, ["requested"] = Now(), ["by"] = by }.ToJsonString());
            return new JsonObject
            {
                ["unit"] = unit, ["requested"] = true, ["to"] = Text(kept, "version") ?? "", ["from"] = Text(kept, "replacedBy") ?? "",
                ["note"] = "rolled back at the next start of the server",
            };
        }
    }

    private static JsonObject StagedSummary(JsonObject doc)
    {
        var files = doc["files"] as JsonArray ?? [];
        return new JsonObject
        {
            ["version"] = Text(doc, "version"), ["from"] = Text(doc, "from"), ["staged"] = Text(doc, "staged"), ["by"] = Text(doc, "by"),
            ["label"] = Text(doc, "label"), ["files"] = files.Count,
            ["bytes"] = files.Sum(f => f?["size"] is JsonValue v && v.TryGetValue<long>(out var n) ? n : 0),
            ["remove"] = (doc["remove"] as JsonArray)?.Count,
        };
    }

    public static JsonObject List(string kiln)
    {
        var p = new Layout(kiln);
        var units = new SortedSet<string>(StringComparer.Ordinal);
        void Folders(string dir, string prefix)
        {
            if (!Directory.Exists(dir)) return;
            foreach (var d in Directory.EnumerateDirectories(dir))
                if (Path.GetFileName(d) is { } n && ModFolderOk(n)) units.Add(prefix + n);
        }
        void Names(string dir, string ext)
        {
            if (!Directory.Exists(dir)) return;
            foreach (var f in Directory.EnumerateFileSystemEntries(dir))
            {
                var n = Path.GetFileName(f);
                if (ext.Length > 0) n = n.EndsWith(ext, StringComparison.Ordinal) ? n[..^ext.Length] : "";
                if (n == "kiln" || (n.StartsWith("mod.", StringComparison.Ordinal) && ModFolderOk(n[4..]))) units.Add(n);
            }
        }
        Folders(Path.Combine(p.Staged, "mods"), "mod.");
        Folders(Path.Combine(p.Staged, "previous", "mods"), "mod.");
        if (Directory.Exists(Path.Combine(p.Staged, "kiln")) || Directory.Exists(Path.Combine(p.Staged, "previous", "kiln"))) units.Add("kiln");
        Names(Path.Combine(p.Staged, "rollback"), ".json");
        Names(Path.Combine(p.Staged, "probation"), ".json");
        Names(Path.Combine(p.Staged, "tx"), "");
        var list = new JsonArray();
        foreach (var unit in units)
        {
            var kilnUnit = unit == "kiln";
            var folder = kilnUnit ? null : unit[4..];
            var staged = ReadObject(Path.Combine(p.StagedDir(unit), "staged.json"));
            var kept = ReadObject(Path.Combine(p.PreviousDir(unit), "kept.json"));
            var rollback = ReadObject(p.RollbackFile(unit));
            var probation = ReadObject(p.ProbationFile(unit));
            var current = kilnUnit ? Text(ReadObject(p.Receipt)?["runtime"], "version") : Text(ReadObject(Path.Combine(p.Mods, folder!, "kiln.mod.json"), true), "version");
            list.Add(new JsonObject
            {
                ["unit"] = unit, ["kind"] = kilnUnit ? "kiln" : "mod", ["folder"] = folder,
                ["id"] = kilnUnit ? null : Text(ReadObject(Path.Combine(p.Mods, folder!, "kiln.mod.json"), true), "id") ?? Text(staged, "id"),
                ["current"] = current is null ? null : new JsonObject { ["version"] = current },
                ["staged"] = staged is null ? null : StagedSummary(staged),
                ["previous"] = kept is null ? null : new JsonObject
                {
                    ["version"] = Text(kept, "version"), ["replacedBy"] = Text(kept, "replacedBy"), ["kept"] = Text(kept, "kept"), ["reason"] = Text(kept, "reason"),
                },
                ["rollback"] = rollback is null ? null : new JsonObject { ["requested"] = Text(rollback, "requested"), ["by"] = Text(rollback, "by") },
                ["probation"] = probation is null ? null : new JsonObject
                {
                    ["state"] = Text(probation, "state"), ["version"] = Text(probation, "version"), ["error"] = Text(probation, "error"),
                },
                ["open"] = File.Exists(Path.Combine(p.TxDir(unit), "journal.json")),
            });
        }
        var results = new JsonArray();
        var resultsDir = Path.Combine(p.Staged, "results");
        if (Directory.Exists(resultsDir))
            foreach (var f in Directory.EnumerateFiles(resultsDir, "*.json").OrderByDescending(f => Path.GetFileName(f), StringComparer.Ordinal).Take(ResultsListed))
                if (ReadObject(f) is { } r) results.Add(r);
        return new JsonObject { ["units"] = list, ["results"] = results, ["incoming"] = p.Incoming };
    }
}
