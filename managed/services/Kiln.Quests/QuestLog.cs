// SPDX-License-Identifier: MIT
// A character's quests in one list: the game's own journal quests (read with the game's evaluator, as the client's
// journal shows them) and Kiln quests (the engine's books). Read-only; starting, advancing and completing stay with
// Quests.Start/Advance/Complete and Knowledge. Usage: docs/kiln/services/world-progress.md "Quests".
namespace Kiln;

/// <summary>Whose engine a quest belongs to.</summary>
public enum QuestKind : byte
{
    /// <summary>A quest of the game's journal (JournalRegistryResource), driven by knowledge.</summary>
    Vanilla = 0,
    /// <summary>A quest a mod defined (Kiln's quest engine).</summary>
    Kiln = 1,
}

/// <summary>One quest as a character stands in it.</summary>
/// <param name="Id">Vanilla: <c>quest:0x&lt;entryId&gt;</c>; Kiln: the quest's full id <c>&lt;mod&gt;:&lt;id&gt;</c>.</param>
/// <param name="Name">A readable name: vanilla quests the game's named query behind them
/// (<c>NPC_Alchemist_Hint01_HAS_Shelter_Journal</c>; null for the few without one), Kiln quests their name text.</param>
/// <param name="Status">Vanilla quests are Locked (not started), Active or Done; the game has no failed state.</param>
/// <param name="Step">The current step (0-based; the last step when done); -1 when not started.</param>
/// <param name="NameTag">Vanilla: the loca tag of the quest's title (the catalog's text).</param>
/// <param name="Kind">Whether this is a quest of the game's journal or one a mod defined.</param>
/// <param name="Steps">The number of steps the quest has.</param>
/// <param name="StepId">The current step's id: a Kiln quest's step id, or <c>step&lt;n&gt;</c> (1-based) for a vanilla
/// quest; null when it has none.</param>
/// <param name="Journal">The game's journal data for a vanilla quest; null for a Kiln quest.</param>
/// <param name="Definition">The definition of a Kiln quest; null for a vanilla quest.</param>
public sealed record QuestEntry(string Id, string? Name, QuestKind Kind, QuestStatus Status, int Step, int Steps, string? StepId,
    uint NameTag, JournalQuest? Journal, QuestDefinition? Definition)
{
    /// <summary>True when the quest is <see cref="QuestStatus.Active"/>.</summary>
    public bool Active => Status == QuestStatus.Active;
    /// <summary>True when the quest is <see cref="QuestStatus.Done"/>.</summary>
    public bool Done => Status == QuestStatus.Done;
    /// <summary>The quest's name, its status and the current step as "step n/m".</summary>
    public override string ToString() => $"{Name ?? Id}: {Status}{(Step >= 0 ? $" step {Step + 1}/{Steps}" : "")}";
}

/// <summary>A character's quests (<see cref="Quests.Of(EntityId)"/>), read when it is made.</summary>
public sealed class QuestLog
{
    internal QuestLog(EntityId player, IReadOnlyList<QuestEntry> all)
    {
        Player = player;
        All = all;
    }

    /// <summary>The player entity this list was made for.</summary>
    public EntityId Player { get; }
    /// <summary>Every quest: the game's journal quests, then Kiln quests.</summary>
    public IReadOnlyList<QuestEntry> All { get; }
    /// <summary>The quests that are active.</summary>
    public IEnumerable<QuestEntry> Active => All.Where(q => q.Status == QuestStatus.Active);
    /// <summary>The quests that are done.</summary>
    public IEnumerable<QuestEntry> Completed => All.Where(q => q.Status == QuestStatus.Done);
    /// <summary>The game's own journal quests.</summary>
    public IEnumerable<QuestEntry> Vanilla => All.Where(q => q.Kind == QuestKind.Vanilla);
    /// <summary>The quests mods defined.</summary>
    public IEnumerable<QuestEntry> Kiln => All.Where(q => q.Kind == QuestKind.Kiln);

    /// <summary>By id (<c>quest:0x8755c335</c>, <c>0x8755c335</c>, <c>mymod:tobin</c>, or a Kiln quest's id within its mod) or
    /// name (case-insensitive); null when there is none.</summary>
    public QuestEntry? Find(string idOrName)
    {
        ArgumentNullException.ThrowIfNull(idOrName);
        var s = idOrName.Trim();
        if (global::Kiln.Knowledge.TryParseId(s, out var raw)) s = $"quest:0x{raw:x8}";
        return All.FirstOrDefault(q => string.Equals(q.Id, s, StringComparison.OrdinalIgnoreCase))
               ?? All.FirstOrDefault(q => string.Equals(q.Name, s, StringComparison.OrdinalIgnoreCase))
               ?? All.FirstOrDefault(q => q.Kind == QuestKind.Kiln && q.Definition is { } d && string.Equals(d.Id, s, StringComparison.OrdinalIgnoreCase));
    }

    /// <summary>The status of the quest <paramref name="idOrName"/> (as for <see cref="Find"/>);
    /// <see cref="QuestStatus.Locked"/> when there is no such quest.</summary>
    public QuestStatus StatusOf(string idOrName) => Find(idOrName)?.Status ?? QuestStatus.Locked;
}

public static partial class Quests
{
    /// <summary>A player's quests, vanilla and Kiln (tick thread). Vanilla quests need kiln.knowledge; on a build without it
    /// the list has only Kiln quests.</summary>
    [TickThreadOnly]
    public static QuestLog Of(EntityId player)
    {
        Game.AssertTickThread();
        WorldIndex.EnsureStarted();  // the vanilla quests' names come from the game's query db
        var list = new List<QuestEntry>();
        if (!player.IsNone && global::Kiln.Knowledge.Available) list.AddRange(VanillaOf(player));
        if (!player.IsNone) list.AddRange(KilnOf(QuestHolder.Of(player)));
        return new QuestLog(player, list);
    }

    /// <summary>A player's quests, vanilla and Kiln, as <see cref="Of(EntityId)"/> (tick thread).</summary>
    [TickThreadOnly]
    public static QuestLog Of(Player player) => Of(player.Entity);

    /// <summary>A vanilla quest's state for a player as an entry (tick thread).</summary>
    internal static QuestEntry VanillaEntry(JournalQuestState s)
    {
        var q = s.Quest;
        var status = s.Rewarded == true || s.Completed == true ? QuestStatus.Done : s.Started ? QuestStatus.Active : QuestStatus.Locked;
        // the client's journal forces every step before a visible one to visible and done: the current step is the last visible
        var step = -1;
        for (var i = s.Steps.Count - 1; i >= 0; i--)
            if (s.Steps[i].Visible == true)
            {
                step = i;
                break;
            }
        if (status == QuestStatus.Done) step = q.Steps.Count - 1;
        return new QuestEntry(q.Name, VanillaNames.Of(q.EntryId), QuestKind.Vanilla, status, step, q.Steps.Count,
            step >= 0 && step < q.Steps.Count ? $"step{step + 1}" : null, q.NameTag, q, null);
    }

    private static IEnumerable<QuestEntry> VanillaOf(EntityId player)
    {
        IReadOnlyList<JournalQuestState> states;
        try
        {
            states = Journal.States(player);
        }
        catch (Exception e) when (e is NotSupportedException or InvalidOperationException)
        {
            yield break;  // the journal is not available on this build, or the player left
        }
        foreach (var s in states) yield return VanillaEntry(s);
    }

    internal static QuestEntry KilnEntry(QuestDefinition q, QuestHolder h)
    {
        var status = StatusOf(q, h);
        var r = RecordFor(q, q.Scope == QuestScope.World ? QuestHolder.World : h);
        var step = r is null ? -1 : r.Status == QuestStatus.Done ? q.Steps.Count - 1 : r.StepIndex;
        return new QuestEntry(q.FullId, q.Name.ToString() is { Length: > 0 } n ? n : q.Id, QuestKind.Kiln, status, step, q.Steps.Count,
            r?.StepId, 0, null, q);
    }

    private static IEnumerable<QuestEntry> KilnOf(QuestHolder h)
    {
        foreach (var q in Definitions)
            if (!q.Hidden || RecordFor(q, q.Scope == QuestScope.World ? QuestHolder.World : h) is not null)
                yield return KilnEntry(q, h);
    }
}

/// <summary>Readable names of the game's journal quests (the named query behind each), from world progress's index.</summary>
internal static class VanillaNames
{
    private static Dictionary<uint, string>? _names;
    private static int _quests;

    public static string? Of(uint entryId)
    {
        var map = _names;
        if (map is null || _quests == 0)
        {
            var places = WorldIndex.PlacesIfBuilt;
            if (places is null) return null;  // the index is not read yet: no names yet
            IReadOnlyList<JournalQuest> quests;
            try
            {
                quests = Journal.Quests();
            }
            catch (Exception e) when (e is NotSupportedException or InvalidOperationException)
            {
                return null;
            }
            map = WorldCatalogBuilder.QuestNames(quests.Where(q => q.Steps.Count > 0)
                .Select(q => (q.EntryId, q.Steps[0].Visible, q.Steps[^1].Done)), places.QueryNames);
            _names = map;
            _quests = quests.Count;
        }
        return map.TryGetValue(entryId, out var n) ? n : null;
    }
}
