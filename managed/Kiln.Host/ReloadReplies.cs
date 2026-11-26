// SPDX-License-Identifier: MIT
// The kiln/control `reload <id>` reply with what the reload left waiting for a restart in the registry (0.2.0-dev.31.14).
// A reload's registry work happens after the reload itself: the mod's data packs apply again at kiln.content's next Pre
// (their registry sections are composed again there), and its HotReload / ModLoad pass runs at the next tick
// (RegistryDispatch.FlushLoads). So the reply waits until both are done, a few steps (at most Timeout), then reads
// registry.json's rows (kiln.registry report_json) whose reason starts with "RestartRequired: " for the reloaded mods:
// a restart field a reloaded pack changed, a field the start never read, a section or entry operation added after the
// start, a runtime pass's restart-only edit. The reply names the count and each mod and field; its data carries them
// (data.registry: restartRequired, settled, edits[]), the hook the Web Admin mod and other tools read.
using System.Text.Json;

namespace Kiln.Host;

internal sealed class ReloadReplies
{
    /// <summary>One edit that waits for a restart: registry.json's refused row (mod, registry, id, field, reason, source).</summary>
    internal sealed record Edit(string Mod, string Registry, string Id, string Field, string Reason, string Source)
    {
        /// <summary>Where it is, as the reply names it: <c>items[game:wood].maxStack</c>, <c>balance.levelCap</c>, else the reason.</summary>
        public string Place => Field.Length == 0 ? Registry.Length == 0 ? Reason : Registry
            : Registry.Length == 0 ? Field
            : Id.Length == 0 || Id == "*" ? $"{Registry}.{Field}" : $"{Registry}[{Id}].{Field}";
    }

    /// <summary>What the registry left for a restart, for some mods. <paramref name="Total"/>: rows of theirs (the report may
    /// leave rows out: <paramref name="Edits"/> holds the ones it has). <paramref name="Settled"/>: the reload's registry work
    /// was done when it was read.</summary>
    internal sealed record Outcome(int Total, IReadOnlyList<Edit> Edits, bool Settled, bool Known);

    internal const string Token = "RestartRequired: ";
    internal const int MinSteps = 4;  // the reload's event, Kiln.Content's pack list, kiln.content's Pre, the registry's report
    internal static readonly TimeSpan Timeout = TimeSpan.FromSeconds(5);
    internal const int Shown = 8;     // edits the message names; data has every one the report has

    private sealed class Waiting
    {
        internal required string ResultFile, Command;
        internal required ModOperationResult Result;
        internal required string[] Mods;
        internal int Steps;
        internal long Deadline;
    }
    private readonly List<Waiting> _waiting = [];

    internal int Count => _waiting.Count;

    /// <summary>Holds the reply of a reload that succeeded until its registry work is done.</summary>
    internal void Add(string resultFile, string command, ModOperationResult result, IEnumerable<string> mods, long now) =>
        _waiting.Add(new Waiting
        {
            ResultFile = resultFile, Command = command, Result = result, Mods = mods.Distinct(StringComparer.Ordinal).ToArray(),
            Deadline = now + (long)Timeout.TotalMilliseconds,
        });

    /// <summary>One step. <paramref name="pending"/>: whether registry work for these mods still waits (their runtime pass, a
    /// queued data-pack application). <paramref name="report"/>: registry.json's text, null when there is no registry.
    /// <paramref name="write"/> gets each reply that is due: (result file, command, ok, message, data).</summary>
    internal void Step(long now, Func<IReadOnlyCollection<string>, bool> pending, Func<string?> report,
        Action<string, string, bool, string, object> write) => Step(now, pending, report, _ => [], write);

    /// <summary><paramref name="notes"/>: what waits for a restart outside registry.json, (mod, reason) for these mods
    /// (a GameStart author whose code changed: RegistryDispatch.RestartNotes).</summary>
    internal void Step(long now, Func<IReadOnlyCollection<string>, bool> pending, Func<string?> report,
        Func<IReadOnlyCollection<string>, IReadOnlyList<(string Mod, string Reason)>> notes, Action<string, string, bool, string, object> write)
    {
        if (_waiting.Count == 0) return;
        string? text = null;
        var read = false;
        foreach (var w in _waiting.ToArray())
        {
            ++w.Steps;
            var settled = w.Steps >= MinSteps && !pending(w.Mods);
            if (!settled && now < w.Deadline) continue;
            _waiting.Remove(w);
            if (!read) { read = true; try { text = report(); } catch { text = null; } }
            var outcome = With(Of(text, w.Mods, settled), notes(w.Mods));
            write(w.ResultFile, w.Command, w.Result.Ok, w.Result.Message + Suffix(outcome), Data(w.Result, outcome));
        }
    }

    /// <summary><paramref name="o"/> with the notes first (no place: the reason names it).</summary>
    internal static Outcome With(Outcome o, IReadOnlyList<(string Mod, string Reason)> notes)
    {
        if (notes.Count == 0) return o;
        var edits = notes.Select(n => new Edit(n.Mod, "", "", "OnRegistry (GameStart)", n.Reason, "")).Concat(o.Edits).ToList();
        return o with { Total = o.Total + notes.Count, Edits = edits, Known = true };
    }

    /// <summary>The reply's data: the reload's affected mods and the registry part.</summary>
    internal static object Data(ModOperationResult result, Outcome o) => new
    {
        affected = result.Affected,
        registry = new
        {
            known = o.Known, settled = o.Settled, restartRequired = o.Total,
            edits = o.Edits.Select(e => new { mod = e.Mod, registry = e.Registry, id = e.Id, field = e.Field, reason = e.Reason, source = e.Source }),
        },
    };

    /// <summary>The message's registry part: "" when nothing waits.</summary>
    internal static string Suffix(Outcome o)
    {
        if (!o.Known) return "";
        var parts = new List<string>();
        if (!o.Settled) parts.Add("its edits were not all applied yet (they apply once the world is ready; kilnctl status says what waits for a restart then)");
        if (o.Total > 0)
        {
            var shown = o.Edits.Take(Shown).Select(e => $"{e.Mod} {e.Place}").ToList();
            var more = o.Total - shown.Count;
            parts.Add($"{o.Total} edit{(o.Total == 1 ? "" : "s")} need{(o.Total == 1 ? "s" : "")} a restart: {string.Join(", ", shown)}" +
                      (more > 0 ? $" and {more} more" : "") + " (kiln/state/registry.json has each reason)");
        }
        return parts.Count == 0 ? "" : "; registry: " + string.Join("; ", parts);
    }

    /// <summary>The rows of registry.json (its text) that wait for a restart and belong to <paramref name="mods"/>.</summary>
    internal static Outcome Of(string? report, IReadOnlyCollection<string> mods, bool settled = true)
    {
        if (string.IsNullOrEmpty(report)) return new(0, [], settled, false);
        try
        {
            using var doc = JsonDocument.Parse(report);
            var edits = new List<Edit>();
            if (doc.RootElement.TryGetProperty("refused", out var refused) && refused.ValueKind == JsonValueKind.Array)
                foreach (var row in refused.EnumerateArray())
                {
                    var reason = Text(row, "reason");
                    var mod = Text(row, "mod");
                    if (!reason.StartsWith(Token, StringComparison.Ordinal) || !mods.Contains(mod)) continue;
                    // a raw path is the Advanced tier's: named as such, never as the game's member
                    var raw = row.TryGetProperty("raw", out var r) && r.ValueKind == JsonValueKind.True;
                    edits.Add(new Edit(mod, Text(row, "registry"), Text(row, "id"), raw ? "(raw path)" : Text(row, "field"), reason, Text(row, "source")));
                }
            // rows the report's cap left out are not known by mod: the total is what it has
            return new(edits.Count, edits, settled, true);
        }
        catch (JsonException) { return new(0, [], settled, false); }
    }

    private static string Text(JsonElement row, string name) =>
        row.TryGetProperty(name, out var v) && v.ValueKind == JsonValueKind.String ? v.GetString() ?? "" : "";
}

/// <summary>kiln.content's status (kiln_content.h KilnContentStatus, 208 bytes): only whether a data-pack application is
/// queued for its next Pre (a reload's packs apply there). Kiln.Host reads it for the reload reply; no mod sees it.</summary>
internal static unsafe class ContentStatusProbe
{
    [System.Runtime.InteropServices.StructLayout(System.Runtime.InteropServices.LayoutKind.Sequential)]
    private struct Status
    {
        internal uint Size;
        internal byte Ready, Registries, StackGuard, ReloadPending;
        internal uint Items, Recipes, Loot, Drops, Packs, PackErrors, Conflicts, Changed;
        internal ulong Generation;
        internal fixed byte Detail[160];
    }
    [System.Runtime.InteropServices.StructLayout(System.Runtime.InteropServices.LayoutKind.Sequential)]
    private struct Head
    {
        internal uint Size, Version;
        internal delegate* unmanaged[Cdecl]<Status*, int> Status;
    }
    internal const string Name = "kiln.content";
    internal const uint Version = 4;

    /// <summary>True while a data-pack application is queued (reload_pending). False without kiln.content.</summary>
    internal static bool ReloadPending(IHostServices host)
    {
        var table = (Head*)host.Service(Name, Version);
        if (table == null || table->Size < 16 || table->Status == null) return false;
        var s = new Status { Size = (uint)sizeof(Status) };
        return table->Status(&s) == 0 && s.ReloadPending != 0;
    }
}
