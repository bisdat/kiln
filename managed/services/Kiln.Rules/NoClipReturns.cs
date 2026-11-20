// SPDX-License-Identifier: MIT
// NoClip across a leave: the last free position of every player in NoClip, kept by the player's stable identity (their
// character), so that a player who leaves, or whose world closes, inside the ground is put back there when they return
// (GitHub issue #25: "switching off never leaves the character inside solid ground", disconnect and world close too).
// The book lives in the world's Kiln data (Kiln.Saves WorldData, saved with every world save that succeeds), so a hosted
// world's next load and a dedicated server's restart see it, with a copy for the process in case a save fails
// (NoClipReturnStore); where that data is not available it lives in memory for the server's lifetime. The decisions are here and unit-tested; RulesRuntime reads the world.
using System.Text.Json.Serialization;

namespace Kiln;

/// <summary>One player's entry: where their character last was in open space while NoClip was on.</summary>
internal sealed class NoClipReturnEntry
{
    public double X { get; set; }
    public double Y { get; set; }
    public double Z { get; set; }

    /// <summary>The name the player had then (for the log).</summary>
    public string Name { get; set; } = "";

    /// <summary>When the position was taken (UTC).</summary>
    public DateTime At { get; set; }

    [JsonIgnore]
    public WorldPosition Position => new(X, Y, Z);
}

/// <summary>The book, as Kiln.Saves stores it (key: <see cref="NoClipReturns.KeyOf"/>).</summary>
internal sealed class NoClipReturnBook
{
    public Dictionary<string, NoClipReturnEntry> Players { get; set; } = new();
}

/// <summary>Where the book is kept: the world's data, or memory.</summary>
internal interface INoClipReturnStore
{
    /// <summary>The book now; null while it cannot be read yet (a hosted world before its load): decide nothing then.</summary>
    NoClipReturnBook? Book { get; }

    /// <summary>The book's entry for <paramref name="key"/> was written (<paramref name="entry"/>) or removed (null).</summary>
    void Wrote(string key, NoClipReturnEntry? entry);
}

/// <summary>
/// The store Kiln.Rules uses. With Kiln.Saves the book is the world's data, which reaches the disk only with a world save
/// that succeeds: a save that fails (or a hosted world closed without one) would drop what was written since. So every
/// write also goes into a copy per world that lives for the process, and a world's book that is loaded (again) gets the
/// entries of that copy it does not have. A hosted game that closes a world and loads it again in the same run restores
/// a player whatever the last save did; a server that stops after a failed save restores only what an earlier save
/// carried (the character's own position in the game's save is just as old). Without Kiln.Saves the book is memory for
/// the server's lifetime, dropped when a world closes.
/// </summary>
internal sealed class NoClipReturnStore(Func<bool> savesAvailable, Func<bool> worldReady, Func<string?> worldId,
    Func<NoClipReturnBook> worldBook) : INoClipReturnStore
{
    private NoClipReturnBook _memory = new();
    private readonly Dictionary<string, Dictionary<string, NoClipReturnEntry>> _kept = new();  // world id -> key -> entry
    private NoClipReturnBook? _merged;  // the world's book (object) the copy was last merged into

    public NoClipReturnBook? Book
    {
        get
        {
            if (!savesAvailable()) return _memory;
            if (!worldReady()) return null;
            var book = worldBook();
            if (!ReferenceEquals(book, _merged))
            {
                _merged = book;  // a world's data loaded (again): what was written in this run and not saved comes back
                if (_kept.TryGetValue(worldId() ?? "", out var kept))
                    foreach (var (k, v) in kept) book.Players.TryAdd(k, v);
            }
            return book;
        }
    }

    public void Wrote(string key, NoClipReturnEntry? entry)
    {
        if (!savesAvailable() || !worldReady()) return;
        var id = worldId() ?? "";
        if (!_kept.TryGetValue(id, out var kept)) _kept[id] = kept = new();
        if (entry is null) kept.Remove(key);
        else kept[key] = entry;
    }

    /// <summary>A world closes: the memory book (no Kiln.Saves) goes with it; the copies of the world's data stay.</summary>
    public void WorldClosing()
    {
        _memory = new();
        _merged = null;
    }
}

/// <summary>The rules of the book (pure: unit-tested with a store the test holds).</summary>
internal sealed class NoClipReturns(INoClipReturnStore store)
{
    /// <summary>How far the last free position must move before the entry is written again: the world's data is a live
    /// object, but there is no point in touching it every step.</summary>
    internal const double NoteEvery = 0.5;

    /// <summary>The key of a player: their character (the game's CharacterId under the account), else their Steam id;
    /// a lab's simulated character by its entity (it exists for that process only). Null: the player cannot be told
    /// apart from the next one, so nothing is kept for them.</summary>
    public static string? KeyOf(CharacterKey character, ulong steamId, EntityId simulated)
    {
        if (!simulated.IsNone) return $"lab-character/{simulated.Value}";
        if (!character.IsNone && !character.IsAccountOnly) return "character/" + character.Text;
        if (steamId != 0) return $"steam/{steamId}";
        return character.IsAccountOnly ? "character/" + character.Text : null;
    }

    /// <summary>True when the book holds anyone (cheap: the runtime asks it every step).</summary>
    public bool Any => store.Book is { Players.Count: > 0 };

    /// <summary>The player is in NoClip and was last free at <paramref name="lastFree"/>: keep it. Returns true when the
    /// book changed.</summary>
    public bool Note(string key, WorldPosition lastFree, string name, DateTime now)
    {
        if (store.Book is not { } book) return false;
        if (book.Players.TryGetValue(key, out var e) && Distance(e.Position, lastFree) < NoteEvery) return false;
        var entry = new NoClipReturnEntry { X = lastFree.X, Y = lastFree.Y, Z = lastFree.Z, Name = name, At = now };
        book.Players[key] = entry;
        store.Wrote(key, entry);
        return true;
    }

    /// <summary>The player's NoClip ended in the world (they were seen in open space, or put back there): nothing to keep.</summary>
    public bool Forget(string key)
    {
        if (store.Book is not { } book) return false;
        var had = book.Players.Remove(key);
        store.Wrote(key, null);
        return had;
    }

    /// <summary>The entry of a player who comes back; null when they did not leave in NoClip (or the book cannot be read).</summary>
    public NoClipReturnEntry? Of(string key) => store.Book is { } book && book.Players.TryGetValue(key, out var e) ? e : null;

    private static double Distance(WorldPosition a, WorldPosition b) =>
        Math.Sqrt((a.X - b.X) * (a.X - b.X) + (a.Y - b.Y) * (a.Y - b.Y) + (a.Z - b.Z) * (a.Z - b.Z));
}
