// SPDX-License-Identifier: MIT
// A sequence event that consumes the used item, played with no used item, takes the server down: the game's spawn path reads
// ItemRegistryFind(0)+0x42c without a check (0x42586). A player's own use always has an item, and every potion and food sequence has such an
// event, so Abilities.PlaySequence, which has no item to give, refuses them. Reads the live data, so a layer that turned consume off counts.
using System.Text.Json.Nodes;

namespace Kiln;

internal static class SequenceGuard
{
    private static readonly object Gate = new();
    private static ulong _generation = ulong.MaxValue;
    private static Dictionary<Guid, List<GameDataEntry>>? _subs;

    /// <summary>The first event of a sequence, in any of its sub-sequences, that consumes the used item, as "sub-sequence: type"; or null.</summary>
    public static string? ConsumingEvent(Guid sequence) => FirstIn(sequence, ConsumingEventIn);

    /// <summary>The first PayUsageCost event of a sequence, in any of its sub-sequences, with <c>consume</c> on, as
    /// "sub-sequence: type"; or null. It takes the trigger context's cost item, which a use of the item itself puts there
    /// (GitHub #118: the repair kit is used up this way and no other way).</summary>
    public static string? PayingEvent(Guid sequence) => FirstIn(sequence, PayingEventIn);

    private static string? FirstIn(Guid sequence, Func<JsonNode?, string?> find)
    {
        if (!Content.GameDataAvailable) return null;
        var status = Content.Status();
        if (status is not { Ready: true }) return null;
        lock (Gate)
        {
            if (_generation != status.Generation || _subs == null)
            {
                _generation = status.Generation;
                _subs = new Dictionary<Guid, List<GameDataEntry>>();
                var all = Content.Sequences;
                for (var i = 0; i < all.Count; i++)
                {
                    var e = all[i];
                    if (!_subs.TryGetValue(e.Guid, out var list)) _subs[e.Guid] = list = new List<GameDataEntry>();
                    list.Add(e);
                }
            }
            if (!_subs.TryGetValue(sequence, out var subs)) return null;
            foreach (var sub in subs)
                if (find(sub.GetJson("events")?["value"]) is { } type)
                    return $"{sub.Name}: {type}";
            return null;
        }
    }

    /// <summary>The type of the first event of one sub-sequence's events (kiln.gamedata's JSON array) that consumes the used
    /// item, or null. PayUsageCost does not count: it takes the cost item, not the used item, and the cost payer checks for
    /// no item (0x40d00), so PlaySequence plays it safely (staff attacks have it on); <see cref="PayingEventIn"/> finds
    /// it.</summary>
    internal static string? ConsumingEventIn(JsonNode? events)
    {
        if (events is not JsonArray list) return null;
        foreach (var ev in list)
            if (ev?["consume"] is JsonValue v && v.TryGetValue<bool>(out var consume) && consume && (string?)ev["$type"] is { } type
                && !type.EndsWith("PayUsageCost", StringComparison.Ordinal))
                return type;
        return null;
    }

    /// <summary>The type of the first PayUsageCost event of one sub-sequence's events with <c>consume</c> on, or null.</summary>
    internal static string? PayingEventIn(JsonNode? events)
    {
        if (events is not JsonArray list) return null;
        foreach (var ev in list)
            if (ev?["consume"] is JsonValue v && v.TryGetValue<bool>(out var consume) && consume && (string?)ev["$type"] is { } type
                && type.EndsWith("PayUsageCost", StringComparison.Ordinal))
                return type;
        return null;
    }
}
