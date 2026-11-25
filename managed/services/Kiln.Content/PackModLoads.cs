// SPDX-License-Identifier: MIT
namespace Kiln;

/// <summary>Which mods of the data-pack list were loaded again since the last look: a mod's packs follow its load, not
/// only its id. A reload keeps the id, and keeps the list as it was when the mod stays in its place (the last loaded mod,
/// a mod hot-loaded with <c>load</c>); an unload and a load between two looks do the same. Each load has an owner of its
/// own (Kiln.Host makes one per load), so a new owner under the same id is a new load (since 0.2.0-dev.31.13).</summary>
internal sealed class PackModLoads
{
    private Dictionary<string, (ulong Load, string Version)> _last = new(StringComparer.Ordinal);
    private readonly Dictionary<string, string?> _reloaded = new(StringComparer.Ordinal);  // ModReloadedEvent: id -> the mod it was reloaded with

    /// <summary>A <see cref="ModReloadedEvent"/> named <paramref name="id"/>: reloaded itself (<paramref name="with"/>
    /// null) or as a dependent of <paramref name="with"/>.</summary>
    public void Reloaded(string id, string? with)
    {
        if (!_reloaded.ContainsKey(id) || with is null) _reloaded[id] = with;
    }

    /// <summary>The loaded mods now (id, the owner of its load, 0 = not known, version). Returns the mods that were
    /// loaded before and are loaded again under a new load, with the reason for the log line.</summary>
    public List<(string Id, string Reason)> Next(IEnumerable<(string Id, ulong Load, string Version)> loaded)
    {
        var again = new List<(string, string)>();
        var now = new Dictionary<string, (ulong, string)>(StringComparer.Ordinal);
        foreach (var (id, load, version) in loaded)
        {
            now[id] = (load, version);
            if (load == 0 || !_last.TryGetValue(id, out var was) || was.Load == 0 || was.Load == load) continue;
            var how = _reloaded.TryGetValue(id, out var with)
                ? with is null ? "was reloaded" : $"was reloaded with {with}"
                : "was unloaded and loaded again";
            again.Add((id, was.Version == version ? $"{how} ({version})" : $"{how} ({was.Version} -> {version})"));
        }
        _last = now;
        _reloaded.Clear();
        return again;
    }
}
