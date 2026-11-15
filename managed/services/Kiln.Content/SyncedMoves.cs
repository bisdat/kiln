// SPDX-License-Identifier: MIT
using System.Text.Json.Nodes;
namespace Kiln;

// Receiving-side admission, after sequence definitions/rules and before any item slot is published.
internal static class SyncedMoves
{
    internal static void Filter(JsonObject document, IReadOnlyList<GameDataDefinition> definitions,
        Func<Guid, IReadOnlyDictionary<string, (string State, string? Error)>> worlds,
        Func<Guid, IReadOnlyList<(Guid State, PlayerAnimStateKind Kind)>?> states,
        Func<Guid, IReadOnlyList<AnimationReferenceAnswer>> graph, Action<string> log)
    {
        if (document["itemdata"] is not JsonObject items) return;
        var names = definitions.Where(d => d.Section == GameDataSection.Sequences).GroupBy(d => d.Name, StringComparer.OrdinalIgnoreCase).ToDictionary(g => g.Key, g => g.First().Guid, StringComparer.OrdinalIgnoreCase);
        var clones = definitions.Where(d => d.Section == GameDataSection.Sequences).Select(d => d.Guid).ToHashSet();
        var routed = definitions.Where(d => d.Section == GameDataSection.Sequences && RouteName(d.Name)).Select(d => d.Guid).ToHashSet();
        // Include definitions that this client could not register, not just the native registry's successful entries.
        if (document["sequences"]?["add"] is JsonArray adds)
            foreach (var add in adds.OfType<JsonObject>())
                if (Guid.TryParse((string?)add["guid"], out var id) && (string?)add["name"] is { } name)
                { names[name] = id; clones.Add(id); if (RouteName(name) || (bool?)add["routeGeneration"] == true) routed.Add(id); }
        Guid? Reference(JsonNode? value) => value is JsonValue v && v.TryGetValue<string>(out var text)
            ? Guid.TryParse(text, out var id) ? id : names.TryGetValue(text, out id) ? id : null : null;
        var checkedSequences = new Dictionary<Guid, string?>();
        string? Check(Guid sequence)
        {
            if (checkedSequences.TryGetValue(sequence, out var result)) return result;
            string? why = null;
            if (clones.Contains(sequence))
            {
                var outcomes = worlds(sequence);
                if (outcomes.Count == 0) why = "no client registration outcome";
                foreach (var (world, status) in outcomes)
                    if (status.State != "registered") { why = $"{world} world: {status.State}: {status.Error}"; break; }
            }
            if (why == null)
            {
                var entries = states(sequence);
                if (entries == null) why = $"sequence {sequence} could not be read";
                else foreach (var (id, kind) in entries)
                    if (!graph(id).Any(s => s.Live && s.Kind == kind)) { why = $"animation {id} is absent, not live, or the wrong kind in the client's graph"; break; }
            }
            checkedSequences[sequence] = why;
            return why;
        }
        foreach (var (weapon, node) in items)
        {
            if (node is not JsonObject rules) continue;
            var references = new List<Guid>();
            var slotPaths = new HashSet<string>(StringComparer.Ordinal);
            foreach (var (path, value) in rules)
            {
                if (path.StartsWith("sequences[", StringComparison.Ordinal) && path.EndsWith(".sequence", StringComparison.Ordinal))
                { slotPaths.Add(path); if (Reference(value) is { } id) references.Add(id); }
                if (path == "sequences" && value is JsonArray list)
                    for (var index = 0; index < list.Count; index++)
                        if (list[index] is JsonObject row)
                        { slotPaths.Add($"sequences[{index}].sequence"); if (Reference(row["sequence"]) is { } seq) references.Add(seq); }
            }
            if (!references.Any(routed.Contains)) continue;
            // Later chain links do not have an initial item slot. Admit the complete immutable generation too.
            var suffixes = names.Where(p => references.Contains(p.Value) && routed.Contains(p.Value) && RouteName(p.Key))
                .Select(p => p.Key[^14..]).ToHashSet(StringComparer.Ordinal);
            references.AddRange(names.Where(p => RouteName(p.Key) && suffixes.Contains(p.Key[^14..])).Select(p => p.Value));
            string? reason = null;
            try { foreach (var sequence in references.Distinct()) if ((reason = Check(sequence)) != null) break; }
            catch (Exception e) when (e is not OutOfMemoryException) { reason = $"receiving-side validation failed: {e.Message}"; }
            if (reason == null) continue;
            foreach (var path in rules.Select(p => p.Key).Where(p => p == "sequences" || p.StartsWith("sequences[", StringComparison.Ordinal)).ToList()) rules.Remove(path);
            // Reference leaves support mul 1 (original GUID, with the sequenceId paired by native content).
            // ItemSequence blob-array headers are not writable through the generic content API.
            foreach (var path in slotPaths) rules[path] = new JsonObject { ["mul"] = 1 };
            log($"weapon {weapon}: routed slots not applied: {reason}; using base sequences");
        }
    }

    private static bool RouteName(string name) => name.Length >= 14 && name[^14..^12] == ".r" && name[^12..].All(Uri.IsHexDigit);

    internal static IReadOnlyList<(Guid State, PlayerAnimStateKind Kind)>? ReadStates(Guid sequence)
    {
        var result = new List<(Guid, PlayerAnimStateKind)>();
        var found = false;
        var registry = Content.Sequences;
        for (var i = 0; i < registry.Count; i++)
        {
            var entry = registry[i];
            if (entry.Guid != sequence) continue;
            found = true;
            var value = entry.GetJson("events");
            if (value is JsonObject wrapper && wrapper.ContainsKey("path")) value = wrapper["value"];
            if (value is not JsonArray events) return null;
            foreach (var ev in events.OfType<JsonObject>())
            {
                var type = ((string?)ev["$type"] ?? "").Split(':')[^1];
                var field = type == "SetAnimationEvent" ? "animationName" : type == "SetAnimationOverlayEvent" ? "overlayName" : null;
                if (field == null) continue;
                if (!Guid.TryParse((string?)ev[field], out var state)) return null;
                result.Add((state, field == "animationName" ? PlayerAnimStateKind.State : PlayerAnimStateKind.Overlay));
            }
        }
        return found ? result : null;
    }
}
