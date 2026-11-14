// SPDX-License-Identifier: MIT
// NoClip across a leave: the last free position of every player in NoClip, kept by the player's stable identity (their
// character), so that a player who leaves, or whose world closes, inside the ground is put back there when they return
// (GitHub issue #25: "switching off never leaves the character inside solid ground", disconnect and world close too).
// The book lives in the world's Kiln data (Kiln.Saves WorldData, saved with every world save), so a hosted world's next
// load and a dedicated server's restart see it; where that data is not available it lives in memory for the server's
// lifetime. The decisions are here and unit-tested; RulesRuntime reads the world.
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
        book.Players[key] = new NoClipReturnEntry { X = lastFree.X, Y = lastFree.Y, Z = lastFree.Z, Name = name, At = now };
        return true;
    }

    /// <summary>The player's NoClip ended in the world (they were seen in open space, or put back there): nothing to keep.</summary>
    public bool Forget(string key) => store.Book is { } book && book.Players.Remove(key);

    /// <summary>The entry of a player who comes back; null when they did not leave in NoClip (or the book cannot be read).</summary>
    public NoClipReturnEntry? Of(string key) => store.Book is { } book && book.Players.TryGetValue(key, out var e) ? e : null;

    private static double Distance(WorldPosition a, WorldPosition b) =>
        Math.Sqrt((a.X - b.X) * (a.X - b.X) + (a.Y - b.Y) * (a.Y - b.Y) + (a.Z - b.Z) * (a.Z - b.Z));
}
