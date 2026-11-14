// SPDX-License-Identifier: MIT
// What a world keeps of villager routines, and what a restart does with it. Pure code: the book is a plain object the
// world's data holds (Kiln.Blocks' own bundle, as ContainerRuntime's: BlocksBridge.AsData(ref ctx, "kiln.blocks", ...) over
// WorldData, so no mod's keys and no change in Kiln.Saves), written with every world save.
// Kept per assignment: the villager's NpcId, the owner mod, the plan's key, version and content hash, whether a manual
// command suspended it, and its revision. Never an entity id, a pointer, a ticket or the task that was running: after a
// restart the plan is found again by its key in the definitions the mods made in OnLoad, and the time of then decides.
using System.Text.Json.Serialization;

namespace Kiln;

/// <summary>One saved assignment.</summary>
internal sealed class SavedRoutine
{
    [JsonInclude] public uint NpcId { get; set; }
    [JsonInclude] public string Owner { get; set; } = "";
    [JsonInclude] public string Key { get; set; } = "";
    [JsonInclude] public uint PlanVersion { get; set; }
    [JsonInclude] public string ContentHash { get; set; } = "";
    [JsonInclude] public bool ManualSuspended { get; set; }
    [JsonInclude] public ulong Revision { get; set; }
}

/// <summary>The routines of one world.</summary>
internal sealed class RoutineBook
{
    public const int CurrentFormat = 1;

    [JsonInclude] public int Format { get; set; } = CurrentFormat;
    /// <summary>The world these belong to (Saves.WorldId); null until the world has one.</summary>
    [JsonInclude] public string? World { get; set; }
    /// <summary>The highest revision ever given in this world: the next change of any assignment takes the next one, also
    /// after the assignment it belonged to was removed.</summary>
    [JsonInclude] public ulong LastRevision { get; set; }
    [JsonInclude] public List<SavedRoutine> Assignments { get; set; } = [];

    public SavedRoutine? Find(uint npcId) => Assignments.FirstOrDefault(a => a.NpcId == npcId);

    /// <summary>Records the assignment a completed change left. A result older than what the book holds for the villager
    /// changes nothing (false): a late callback cannot put an earlier state back.</summary>
    public bool Record(uint npcId, string owner, RoutinePlan plan, bool manualSuspended, ulong revision)
    {
        var have = Find(npcId);
        if (have != null && revision < have.Revision) return false;
        if (have == null && revision <= RemovedAt(npcId)) return false;
        if (have == null) Assignments.Add(have = new SavedRoutine { NpcId = npcId });
        (have.Owner, have.Key, have.PlanVersion, have.ContentHash, have.ManualSuspended, have.Revision) = (owner, plan.Key, plan.Version, plan.ContentHash, manualSuspended, revision);
        _removed.Remove(npcId);
        LastRevision = Math.Max(LastRevision, revision);
        return true;
    }

    /// <summary>Records a removal at <paramref name="revision"/>. Older than the book's row: nothing (false).</summary>
    public bool Remove(uint npcId, ulong revision)
    {
        var have = Find(npcId);
        if (have == null || revision < have.Revision) return false;
        Assignments.Remove(have);
        _removed[npcId] = revision;
        LastRevision = Math.Max(LastRevision, revision);
        return true;
    }

    /// <summary>The next revision of this world.</summary>
    public ulong NextRevision() => ++LastRevision;

    // removals of this session, so a result older than a removal does not bring the assignment back (not saved: tickets do not outlive the process)
    private readonly Dictionary<uint, ulong> _removed = new();

    private ulong RemovedAt(uint npcId) => _removed.GetValueOrDefault(npcId);
}

/// <summary>What a restart does with one saved assignment.</summary>
internal enum RoutineRestore
{
    /// <summary>Same version and content: bound again at the saved revision. No event.</summary>
    Rebind,
    /// <summary>The plan is defined with a higher version: bound to it, at the next revision.</summary>
    Migrate,
    /// <summary>Same version, other content: refused, the assignment stays saved and suspended.</summary>
    VersionNotBumped,
    /// <summary>The plan is defined with a lower version: refused, the assignment stays saved and suspended.</summary>
    Downgrade,
    /// <summary>The owner mod is not loaded, the plan is not defined, or the book is another world's: removed, at the next revision.</summary>
    Remove,
}

/// <summary>The decision for one saved assignment.</summary>
/// <param name="Action">What happens.</param>
/// <param name="State">The assignment's state after it, before the villager is found in the world.</param>
/// <param name="Plan">The plan to bind (Rebind, Migrate), else null: nothing of an earlier plan is kept to run.</param>
/// <param name="NewRevision">The assignment takes the world's next revision (Migrate, Remove).</param>
/// <param name="Reason">For the event and the log; null for a plain rebind.</param>
internal readonly record struct RoutineRestoreDecision(RoutineRestore Action, RoutineAssignmentState State, RoutinePlan? Plan, bool NewRevision, string? Reason)
{
    /// <summary>An event tells subscribers: every outcome but the plain rebind.</summary>
    public bool Announced => Action != RoutineRestore.Rebind;
}

internal static class RoutineStore
{
    public const string WorldKey = "routines";

    public const string ReasonMigrated = "Migrated", ReasonVersionNotBumped = "VersionNotBumped", ReasonDowngrade = "Downgrade",
        ReasonOwnerMissing = "OwnerMissing", ReasonDefinitionMissing = "DefinitionMissing", ReasonOtherWorld = "OtherWorld";

    /// <summary>The decision table. <paramref name="ownerLoaded"/>: the owner mod is loaded and enabled now;
    /// <paramref name="current"/>: the plan defined under the saved key now, or null.</summary>
    public static RoutineRestoreDecision Decide(SavedRoutine saved, bool ownerLoaded, RoutinePlan? current)
    {
        if (!ownerLoaded) return new(RoutineRestore.Remove, RoutineAssignmentState.Removed, null, true, ReasonOwnerMissing);
        // a key of another mod under this owner cannot be bound: the service refuses it, so it is as if not defined
        if (current == null || RoutineIdentity.ModOf(current.Key) != saved.Owner || current.Key != saved.Key)
            return new(RoutineRestore.Remove, RoutineAssignmentState.Removed, null, true, ReasonDefinitionMissing);
        if (current.Version > saved.PlanVersion) return new(RoutineRestore.Migrate, AfterRestart, current, true, ReasonMigrated);
        if (current.Version < saved.PlanVersion) return new(RoutineRestore.Downgrade, RoutineAssignmentState.VersionMismatch, null, false, ReasonDowngrade);
        if (!string.Equals(current.ContentHash, saved.ContentHash, StringComparison.Ordinal))
            return new(RoutineRestore.VersionNotBumped, RoutineAssignmentState.VersionMismatch, null, false, ReasonVersionNotBumped);
        return new(RoutineRestore.Rebind, AfterRestart, current, false, null);
    }

    // Bound again, the villager not found yet: every rebound or migrated row starts PendingSpawn, also a suspended one.
    // The manual suspension is not lost by that: it is the saved row's own flag (SavedRoutine.ManualSuspended), which
    // Restore never clears, and the assignment made from the row at world-ready (S5) carries it back to ManualSuspended.
    private const RoutineAssignmentState AfterRestart = RoutineAssignmentState.PendingSpawn;

    /// <summary>Decides every assignment of a book against the definitions of now and applies it to the book: removed rows
    /// go, migrated rows take the new version, hash and revision. Returns each row (as it was saved) with its decision, in
    /// the book's order. <paramref name="world"/>: the world's id now; a book of another world loses all its rows.</summary>
    public static IReadOnlyList<(SavedRoutine Saved, RoutineRestoreDecision Decision)> Restore(RoutineBook book, string? world, Func<string, bool> ownerLoaded, Func<string, RoutinePlan?> definition)
    {
        var otherWorld = book.World != null && world != null && !string.Equals(book.World, world, StringComparison.Ordinal);
        var results = new List<(SavedRoutine, RoutineRestoreDecision)>(book.Assignments.Count);
        foreach (var saved in book.Assignments.ToList())
        {
            var was = new SavedRoutine
            {
                NpcId = saved.NpcId, Owner = saved.Owner, Key = saved.Key, PlanVersion = saved.PlanVersion, ContentHash = saved.ContentHash,
                ManualSuspended = saved.ManualSuspended, Revision = saved.Revision,
            };
            var d = otherWorld
                ? new RoutineRestoreDecision(RoutineRestore.Remove, RoutineAssignmentState.Removed, null, true, ReasonOtherWorld)
                : Decide(saved, ownerLoaded(saved.Owner), definition(saved.Key));
            switch (d.Action)
            {
                case RoutineRestore.Remove:
                    book.Assignments.Remove(saved);
                    book.NextRevision();
                    break;
                case RoutineRestore.Migrate:
                    (saved.PlanVersion, saved.ContentHash, saved.Revision) = (d.Plan!.Version, d.Plan.ContentHash, book.NextRevision());
                    break;
            }
            results.Add((was, d));
        }
        if (world != null) book.World = world;
        return results;
    }
}
