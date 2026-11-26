// SPDX-License-Identifier: MIT
// Who wants what an NPC says, and what it says because of that: the rule of Kiln.Npcs without the game. Per NPC, each
// owner (a mod) has at most one override (Suppress or Replace, with a priority) and the NPC has at most one Say. The
// effective override is the highest priority, on a tie the most recent; a Say shows over it until its time. Two layers:
// Local (calls in this process) and Remote (what the server sent this joined client); Local wins over Remote, so a
// player's own client-only mod has the last word on their own screen. docs/kiln/services/npc-speech.md.
namespace Kiln;

/// <summary>What an NPC's greeting bubble does now.</summary>
public enum SpeechMode : byte
{
    /// <summary>The game's own lines.</summary>
    Game = 0,
    /// <summary>No greeting bubble and no greeting voice-over.</summary>
    Suppress = 1,
    /// <summary>The NPC picks from a mod's lines instead of its own, with no voice-over.</summary>
    Replace = 2,
}

/// <summary>The effective speech of one NPC: its override and its running Say.</summary>
internal sealed record SpeechEffect(SpeechMode Mode, IReadOnlyList<UiText> Lines, string? Owner, UiText? Say, long SayUntilMs, string? SayOwner)
{
    public static readonly SpeechEffect None = new(SpeechMode.Game, [], null, null, 0, null);

    /// <summary>Same override and Say (texts compared by their JSON).</summary>
    public bool SameAs(SpeechEffect o) =>
        Mode == o.Mode && SayUntilMs == o.SayUntilMs && SpeechText.Json(Say) == SpeechText.Json(o.Say) &&
        Lines.Count == o.Lines.Count && Lines.Select(SpeechText.Json).SequenceEqual(o.Lines.Select(SpeechText.Json));
}

internal enum SpeechLayer : byte { Local = 0, Remote = 1 }

/// <summary>The rule, thread-safe (one lock). Times are ms of a clock the caller gives.</summary>
internal sealed class SpeechBook
{
    private sealed record Entry(string Owner, SpeechMode Mode, UiText[] Lines, int Priority, long Seq);
    private sealed record SayEntry(string Owner, UiText Text, long UntilMs);

    private sealed class Npc
    {
        public readonly List<Entry>[] Overrides = [new(), new()];
        public readonly SayEntry?[] Says = new SayEntry?[2];
        public bool Empty => Overrides[0].Count == 0 && Overrides[1].Count == 0 && Says[0] is null && Says[1] is null;
    }

    private readonly object _gate = new();
    private readonly Dictionary<uint, Npc> _npcs = new();
    private long _seq;

    public int Count { get { lock (_gate) return _npcs.Count; } }

    /// <summary>The NPCs that have anything.</summary>
    public uint[] Npcs() { lock (_gate) return _npcs.Keys.ToArray(); }

    public void Set(SpeechLayer layer, uint npc, string owner, SpeechMode mode, UiText[] lines, int priority)
    {
        lock (_gate)
        {
            var n = Get(npc);
            var list = n.Overrides[(int)layer];
            list.RemoveAll(e => e.Owner == owner);
            if (mode != SpeechMode.Game) list.Add(new Entry(owner, mode, lines, priority, ++_seq));
            Drop(npc, n);
        }
    }

    public void Say(SpeechLayer layer, uint npc, string owner, UiText? text, long untilMs)
    {
        lock (_gate)
        {
            var n = Get(npc);
            n.Says[(int)layer] = text is null ? null : new SayEntry(owner, text, untilMs);
            Drop(npc, n);
        }
    }

    /// <summary>Ends this owner's Say of the NPC (another owner's stays).</summary>
    public bool StopSay(SpeechLayer layer, uint npc, string owner)
    {
        lock (_gate)
        {
            if (!_npcs.TryGetValue(npc, out var n) || n.Says[(int)layer]?.Owner != owner) return false;
            n.Says[(int)layer] = null;
            Drop(npc, n);
            return true;
        }
    }

    /// <summary>A Say is running for the NPC (not past its time).</summary>
    public bool Saying(SpeechLayer layer, uint npc, long nowMs)
    {
        lock (_gate) return _npcs.TryGetValue(npc, out var n) && n.Says[(int)layer] is { } s && s.UntilMs > nowMs;
    }

    /// <summary>Drops everything an owner set (its mod unloaded); returns the NPCs whose state changed.</summary>
    public uint[] DropOwner(string owner)
    {
        lock (_gate)
        {
            var touched = new List<uint>();
            foreach (var (npc, n) in _npcs.ToArray())
            {
                var hit = false;
                foreach (var list in n.Overrides) hit |= list.RemoveAll(e => e.Owner == owner) > 0;
                for (var i = 0; i < n.Says.Length; i++)
                    if (n.Says[i]?.Owner == owner) { n.Says[i] = null; hit = true; }
                if (hit) touched.Add(npc);
                Drop(npc, n);
            }
            return touched.ToArray();
        }
    }

    /// <summary>Drops a layer (a world ended: Local; the server connection ended: Remote); returns the NPCs touched.</summary>
    public uint[] DropLayer(SpeechLayer layer)
    {
        lock (_gate)
        {
            var touched = new List<uint>();
            foreach (var (npc, n) in _npcs.ToArray())
            {
                if (n.Overrides[(int)layer].Count > 0 || n.Says[(int)layer] is not null) touched.Add(npc);
                n.Overrides[(int)layer].Clear();
                n.Says[(int)layer] = null;
                Drop(npc, n);
            }
            return touched.ToArray();
        }
    }

    /// <summary>The override one owner set on the NPC in a layer (Game when none).</summary>
    public SpeechMode ModeOf(SpeechLayer layer, uint npc, string owner)
    {
        lock (_gate)
            return _npcs.TryGetValue(npc, out var n) && n.Overrides[(int)layer].Find(e => e.Owner == owner) is { } e ? e.Mode : SpeechMode.Game;
    }

    /// <summary>What the NPC shows now: Local over Remote; the highest priority, then the most recent; a Say until its time.</summary>
    public SpeechEffect Effective(uint npc, long nowMs)
    {
        lock (_gate)
        {
            if (!_npcs.TryGetValue(npc, out var n)) return SpeechEffect.None;
            Entry? best = null;
            foreach (var layer in new[] { 0, 1 })
            {
                foreach (var e in n.Overrides[layer])
                    if (best is null || e.Priority > best.Priority || (e.Priority == best.Priority && e.Seq > best.Seq)) best = e;
                if (best is not null) break;  // a Local override hides the Remote ones
            }
            SayEntry? say = null;
            foreach (var layer in new[] { 0, 1 })
                if (n.Says[layer] is { } s && s.UntilMs > nowMs) { say = s; break; }
            return new SpeechEffect(best?.Mode ?? SpeechMode.Game, best?.Lines ?? [], best?.Owner, say?.Text, say?.UntilMs ?? 0, say?.Owner);
        }
    }

    private Npc Get(uint npc)
    {
        if (!_npcs.TryGetValue(npc, out var n)) _npcs[npc] = n = new Npc();
        return n;
    }

    private void Drop(uint npc, Npc n)
    {
        if (n.Empty) _npcs.Remove(npc);
    }
}

/// <summary>Texts of speech lines: their loca tag and JSON, and the limits.</summary>
internal static class SpeechText
{
    public const int MaxLines = 32;
    public const int MaxChars = 200;
    /// <summary>A line's JSON (every language of it) as UTF-8: one wire string, which a Kiln client reads with this
    /// limit (32 of them stay under the channel's 96 KiB).</summary>
    public const int MaxBytes = 2048;

    // Letters as they are ("Zurück", not "Zur\u00FCck"): six bytes for every non-ASCII letter would fill MaxBytes fast.
    // Quotes, backslashes and control characters are still escaped; the JSON only goes to kiln.content and to clients.
    private static readonly System.Text.Json.JsonSerializerOptions Options =
        new() { Encoder = System.Text.Encodings.Web.JavaScriptEncoder.UnsafeRelaxedJsonEscaping };

    /// <summary>{"en": "...", "*": "..."}: what kiln.content's loca hooks take, the languages in a stable order. A text
    /// with neither "en" nor "*" gets a "*" (the text UiText falls back to): kiln.content would leave every other
    /// language empty, an empty bubble.</summary>
    public static string Json(UiText? t)
    {
        if (t is null) return "";
        var map = new SortedDictionary<string, string>(StringComparer.Ordinal);
        foreach (var k in t.Languages.Select(k => k.Trim().ToLowerInvariant()).Distinct()) map[k] = t.For(k);
        if (!map.ContainsKey("en") && !map.ContainsKey("*")) map["*"] = t.For("en");
        return System.Text.Json.JsonSerializer.Serialize(map, Options);
    }

    /// <summary>The text back from its JSON (a joined client's copy of what the server sent).</summary>
    public static UiText FromJson(string json)
    {
        var map = System.Text.Json.JsonSerializer.Deserialize<Dictionary<string, string>>(json) ?? throw new FormatException("not a text map");
        if (map.Count == 0) throw new FormatException("a text without languages");
        return new UiText(map);
    }

    /// <summary>The loca tag Kiln's text answers to: FNV-1a-32 over "kiln.npcspeech" and the JSON (never 0). The same
    /// text always gets the same tag, so kiln.content keeps one entry per distinct text.</summary>
    public static uint Tag(string json)
    {
        var h = 2166136261u;
        foreach (var b in System.Text.Encoding.UTF8.GetBytes("kiln.npcspeech\n" + json))
        {
            h ^= b;
            h *= 16777619u;
        }
        return h == 0 ? 1u : h;
    }

    /// <summary>Null when the text is fine, else why not.</summary>
    public static string? Problem(UiText? t)
    {
        if (t is null) return "the text is null";
        if (t.Languages.Count == 0) return "the text has no language";
        foreach (var k in t.Languages)
        {
            var s = t.For(k);
            if (string.IsNullOrWhiteSpace(s)) return $"the text for '{k}' is empty";
            if (s.Length > MaxChars) return $"the text for '{k}' is longer than {MaxChars} characters";
        }
        var bytes = System.Text.Encoding.UTF8.GetByteCount(Json(t));
        if (bytes > MaxBytes)
            return $"its {t.Languages.Count} languages together are {bytes} bytes as sent to Kiln clients, more than {MaxBytes} (fewer languages or shorter texts)";
        return null;
    }
}
