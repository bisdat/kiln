// SPDX-License-Identifier: MIT
// What kiln-eml (the GPL-3 EML plugin, gpl/kiln-eml) did this start, for Kiln mods: the managed port of EML's
// `loader.has_mod` plus the run's result. Reads <game>/kiln/state/eml.json (format 1), which kiln-eml writes before the
// game starts; Kiln's own code, no GPL code involved.
using System.Text.Json;
using System.Text.Json.Serialization;

namespace Kiln;

/// <summary>The EML mods of this start and what kiln-eml did with them (docs/kiln/eml.md).</summary>
[AnyThread]
public static class Eml
{
    private static readonly Lazy<EmlStatus?> Current = new(() => Load(DefaultStateFile, currentProcess: true));

    /// <summary><c>&lt;game&gt;/kiln/state/eml.json</c>, found next to Kiln.Api (<c>&lt;game&gt;/kiln/host/</c>).</summary>
    public static string DefaultStateFile =>
        Path.GetFullPath(Path.Combine(Path.GetDirectoryName(typeof(Game).Assembly.Location) ?? ".", "..", "state", "eml.json"));

    /// <summary>This start's result, or null when kiln-eml did not run (not installed, an older Kiln).</summary>
    public static EmlStatus? Status => Current.Value;

    /// <summary>True when the EML mod <paramref name="id"/> ran this start (EML <c>loader.has_mod</c>).</summary>
    public static bool IsLoaded(string id) => Status is { Phase: "done" } s && s.Mods.Any(m => m.State == "loaded" && m.Id == id);

    /// <summary>Reads a state file; null when it is missing or not format 1.</summary>
    public static EmlStatus? Load(string path, bool currentProcess = false)
    {
        try
        {
            var s = JsonSerializer.Deserialize<EmlStatus>(File.ReadAllText(path), Options);
            if (s is not { Format: 1, Mods: not null } || s.Mods.Any(m => m is null)) return null;
            if (currentProcess && (s.Pid != Environment.ProcessId ||
                File.GetLastWriteTimeUtc(path) < System.Diagnostics.Process.GetCurrentProcess().StartTime.ToUniversalTime())) return null;
            return s;
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or JsonException)
        {
            return null;
        }
    }

    private static readonly JsonSerializerOptions Options = new() { PropertyNameCaseInsensitive = true };
}

/// <summary>kiln-eml's run (state/eml.json).</summary>
public sealed record EmlStatus
{
    /// <summary>The state file's format number (1).</summary>
    public int Format { get; init; }
    /// <summary>The id of the game process this run belongs to (the Windows pid under Wine and Proton).</summary>
    public int Pid { get; init; }
    /// <summary>The version of the kiln-eml plugin that wrote the file.</summary>
    [JsonPropertyName("kilnEml")] public string? KilnEmlVersion { get; init; }
    /// <summary>The EML release the plugin carries.</summary>
    [JsonPropertyName("eml")] public string? EmlVersion { get; init; }
    /// <summary><c>done</c> (patched or unchanged since the last run), <c>failed</c> (game data restored to vanilla),
    /// <c>aside</c> (EML's own loader ran instead), <c>idle</c> (no EML mods).</summary>
    public string Phase { get; init; } = "";
    /// <summary>The game's data file: <c>enshrouded</c> or <c>enshrouded_server</c>.</summary>
    public string? File { get; init; }
    /// <summary>How many game resources the run patched.</summary>
    public int Patched { get; init; }
    /// <summary>True when the result of an earlier start was reused and nothing was patched again.</summary>
    public bool Cached { get; init; }
    /// <summary>True when the run put the game data back to vanilla.</summary>
    public bool Restored { get; init; }
    /// <summary>How long the run took, in milliseconds.</summary>
    public double Ms { get; init; }
    /// <summary>Why the run failed; null when it did not.</summary>
    public string? Error { get; init; }
    /// <summary>With phase <c>aside</c>: what made kiln-eml stand aside (EML's own loader ran instead); null otherwise.</summary>
    public string? Aside { get; init; }
    /// <summary>The EML mod folders kiln-eml looked at, with what it did with each.</summary>
    public IReadOnlyList<EmlModStatus> Mods { get; init; } = [];
}

/// <summary>One EML mod folder kiln-eml looked at.</summary>
public sealed record EmlModStatus
{
    /// <summary>The mod's manifest id; null when it has none.</summary>
    public string? Id { get; init; }
    /// <summary>The mod's version; null when it has none.</summary>
    public string? Version { get; init; }
    /// <summary>The mod's display name; null when it has none.</summary>
    public string? Name { get; init; }
    /// <summary><c>profile</c>, <c>kiln-dropin</c> (kiln/eml/mods), <c>game-mods</c> (EML's mods folder), or the engine
    /// plugin that added it.</summary>
    public string Source { get; init; } = "";
    /// <summary>The mod's folder.</summary>
    public string Path { get; init; } = "";
    /// <summary><c>loaded</c>, <c>failed</c>, <c>skipped</c> (not an EML mod), <c>disabled</c>, <c>duplicate</c>.</summary>
    public string State { get; init; } = "";
    /// <summary>Why the mod failed or was skipped, when it was; null otherwise.</summary>
    public string? Reason { get; init; }
    /// <summary>The capabilities the mod declares in its <c>mod.json</c> (<c>patch</c>, <c>export</c>, <c>runtime</c>, <c>runtime-register-dll</c>).</summary>
    public IReadOnlyList<string> Capabilities { get; init; } = [];
}
