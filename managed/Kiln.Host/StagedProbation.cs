// SPDX-License-Identifier: MIT
using System.Text.Json;
using System.Text.Json.Nodes;

namespace Kiln.Host;

/// <summary>The verdict on staged updates kiln-core and the proxy applied at this start (kiln/staged/probation,
/// docs/kiln/staged-updates.md; native/core/src/staged.cpp writes the records). A mod folder updated at the start is
/// watched while it loads: "loading" right before its code runs, then the record goes when it loaded, or "failed" with the
/// reason when it did not, and the next start rolls it back once. A start that ends while the mod loads leaves "loading",
/// which the next start counts as a failure too. A Kiln update is confirmed once the mods loaded: its record goes, so the
/// next start keeps it. Server only. Every write is atomic (a temp file, then a rename); one that fails is logged and
/// changes nothing else.</summary>
internal sealed class StagedProbation
{
    private const string KilnRecord = "kiln.json";
    private readonly string _directory;  // kiln/staged/probation
    private readonly string _mods;       // kiln/mods, full path
    private readonly Action<LogLevel, string> _log;
    private readonly Dictionary<string, JsonObject> _watched = new(StringComparer.OrdinalIgnoreCase);  // folder -> record

    public StagedProbation(string stagedDirectory, string modsDirectory, Action<LogLevel, string> log)
    {
        _directory = Path.Combine(stagedDirectory, "probation");
        _mods = Path.TrimEndingDirectorySeparator(Path.GetFullPath(modsDirectory));
        _log = log;
        if (!Directory.Exists(_directory)) return;
        foreach (var file in Directory.EnumerateFiles(_directory, "mod.*.json"))
        {
            var folder = Path.GetFileNameWithoutExtension(file)["mod.".Length..];
            if (Read(file) is { } record && Text(record, "state") == "pending" && Text(record, "unit") == "mod." + folder)
                _watched[folder] = record;
        }
    }

    /// <summary>Mod folders waiting for their first load after an update.</summary>
    public IReadOnlyCollection<string> Watched => _watched.Keys;

    /// <summary>A load failure the mod itself is answerable for: not one that only passes on a dependency's failure
    /// ("dependency X failed to load", "dependency X ... did not load"); the dependency's own record answers for that.</summary>
    internal static bool Blames(string reason) => !reason.StartsWith("dependency ", StringComparison.Ordinal);

    /// <summary>Right before the mod's code runs: a start that ends now leaves "loading" behind.</summary>
    public void Loading(string directory)
    {
        if (Find(directory) is not { } w) return;
        w.Record["state"] = "loading";
        w.Record["loading"] = DateTime.UtcNow.ToString("O");
        Write(w.Folder, w.Record);
    }

    public void Loaded(string directory)
    {
        if (Find(directory) is not { } w) return;
        _watched.Remove(w.Folder);
        try
        {
            System.IO.File.Delete(RecordFile(w.Folder));
            _log(LogLevel.Info, $"staged: mod {Text(w.Record, "id") ?? w.Folder} {Text(w.Record, "version")} loaded after its update; it stays");
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            _log(LogLevel.Warn, $"staged: mod folder {w.Folder}: its probation record could not be removed ({e.Message}); the next start may roll it back");
        }
    }

    public void Failed(string directory, string reason)
    {
        if (!Blames(reason) || Find(directory) is not { } w) return;
        _watched.Remove(w.Folder);
        w.Record["state"] = "failed";
        w.Record["error"] = reason.Length > 2000 ? reason[..2000] : reason;
        w.Record["failed"] = DateTime.UtcNow.ToString("O");
        if (Write(w.Folder, w.Record))
            _log(LogLevel.Error, $"staged: mod {Text(w.Record, "id") ?? w.Folder} failed to load right after its update to {Text(w.Record, "version")}: " +
                                 $"{reason}. Kiln rolls it back to the version it replaced at the next start (once)");
    }

    /// <summary>The mods loaded: a Kiln update this start booted is kept (the next start does not roll it back).</summary>
    public void ConfirmKiln()
    {
        var file = Path.Combine(_directory, KilnRecord);
        if (!System.IO.File.Exists(file)) return;
        var version = Read(file) is { } r ? Text(r, "version") : null;
        try
        {
            System.IO.File.Delete(file);
            _log(LogLevel.Info, $"staged: the Kiln update{(version is null ? "" : " to " + version)} is confirmed (the mods loaded); it stays");
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            _log(LogLevel.Warn, $"staged: the Kiln update could not be confirmed ({e.Message}); the next start may roll it back");
        }
    }

    private (string Folder, JsonObject Record)? Find(string directory)
    {
        if (_watched.Count == 0) return null;
        var full = Path.TrimEndingDirectorySeparator(Path.GetFullPath(directory));
        var parent = Path.GetDirectoryName(full);
        if (parent is null || !string.Equals(parent, _mods, OperatingSystem.IsWindows() ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal))
            return null;
        var folder = Path.GetFileName(full);
        return _watched.TryGetValue(folder, out var record) ? (folder, record) : null;
    }

    private string RecordFile(string folder) => Path.Combine(_directory, $"mod.{folder}.json");

    private bool Write(string folder, JsonObject record)
    {
        var file = RecordFile(folder);
        var tmp = file + ".tmp";
        try
        {
            System.IO.File.WriteAllText(tmp, record.ToJsonString(new JsonSerializerOptions { WriteIndented = true }));
            System.IO.File.Move(tmp, file, overwrite: true);
            return true;
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            _log(LogLevel.Warn, $"staged: mod folder {folder}: its probation record could not be written ({e.Message})");
            return false;
        }
    }

    private static JsonObject? Read(string file)
    {
        try
        {
            return JsonNode.Parse(System.IO.File.ReadAllText(file)) as JsonObject;
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or JsonException)
        {
            return null;
        }
    }

    private static string? Text(JsonObject o, string key) => o[key] is JsonValue v && v.TryGetValue<string>(out var s) ? s : null;
}
