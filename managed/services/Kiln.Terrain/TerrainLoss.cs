// SPDX-License-Identifier: MIT
// Ownership-loss detection (docs/kiln/services/terrain.md, "When the ledger is gone"). The ledger lives in the world's
// Kiln data (kiln/data/worlds/<id>/), next to the world save, not in it: if that data or the ledger key is lost while
// the edited ground stays, a kept plan with no record would be taken for a first install and write over what changed
// since. So Kiln keeps a witness in a second place: kiln/data/kiln.terrain/<world GUID>/<mod>.json lists the plans this
// mod has a record of in that world, each with the world file revision it was recorded at ("since"). A record goes into
// the Kiln data of every save after it was made, so:
//  - a plan with a witness entry, no record, and a world file newer than "since": the world had the record and lost it:
//    refused, nothing written;
//  - no witness entry: the world never had the plan (or lost the witness as well): a first install, whatever the
//    ground holds (a plan whose result the ground already shows is accepted);
//  - an entry whose "since" is not older than the world file: the record was never saved (the server stopped first),
//    or an older world file was put back: a first install, and the entry is made new.
// Forget takes the plan's entry away. Tick thread.
namespace Kiln;

/// <summary>One mod's witness in one world save: the plans it has had a ledger record of.</summary>
internal sealed class TerrainWitness
{
    public int Format { get; set; } = 1;
    public string Mod { get; set; } = "";
    public Dictionary<string, TerrainWitnessEntry> Plans { get; set; } = new(StringComparer.Ordinal);

    public void Validate()
    {
        if (Format != 1 || Plans is null || Plans.Count > 4 * TerrainPlan.MaxPlans || Plans.Any(p => !TerrainPlan.ValidId(p.Key) || p.Value is null))
            throw new InvalidDataException("the terrain witness is not in a form this Kiln reads");
    }
}

internal sealed class TerrainWitnessEntry
{
    /// <summary>The world file's revision when the record was made: a save after it carries the record.</summary>
    public uint Since { get; set; }
    public DateTimeOffset At { get; set; }
}

internal static class TerrainLoss
{
    /// <summary>The one call before a kept plan's first install (Apply and Inspect, once the plan's record is known to
    /// be missing from a valid <paramref name="book"/>). Null: a first install. Otherwise
    /// <see cref="TerrainState.Refused"/> and the reason, with nothing written and nothing recorded.</summary>
    public static (TerrainState State, string Reason)? Check(ITerrainBackend backend, TerrainPlan plan, TerrainBook book)
    {
        var patch = plan.Patch;
        if (patch.Persistence != TerrainPersistence.Kept || book.Plans.ContainsKey(patch.Id)) return null;
        var (id, revision) = backend.SaveIdentity;
        if (id is null) return null;   // a new world before its first save: nothing of it was ever saved
        TerrainWitness? witness;
        try { witness = backend.ReadWitness(id); }
        catch (InvalidDataException e)
        {
            var bad = $"ownership unknown: {backend.WitnessLocation(id)} cannot be read ({e.Message}), so whether this world had plan " +
                      $"'{patch.Id}' before cannot be told. Nothing was written. Restore the file, or remove it if this mod never applied plans in this world";
            Kiln.Backend.Current.Log(0, LogLevel.Warn, $"terrain: {backend.Mod}: {bad}");
            return (TerrainState.Refused, bad);
        }
        if (witness is null || !witness.Plans.TryGetValue(patch.Id, out var entry) || entry.Since >= revision) return null;
        var reason = Reason(patch.Id, entry.Since, revision, backend.LoadedData, backend.WitnessLocation(id));
        Kiln.Backend.Current.Log(0, LogLevel.Warn, $"terrain: {backend.Mod}: {reason}");
        return (TerrainState.Refused, reason);
    }

    /// <summary>The refusal's text: what was lost, why it is refused, and what an operator can do.</summary>
    internal static string Reason(string plan, uint since, uint revision, DataMatch data, string witness)
    {
        var cause = data == DataMatch.None
            ? "this world has no Kiln data that could be read, so no terrain ledger records it (the world's Kiln data was lost or is unreadable)"
            : "this mod's terrain ledger has no record of it (the ledger was lost or reset)";
        return $"ownership lost: this world had plan '{plan}' (recorded at world revision {since}; the world file is at revision {revision} now), " +
               $"but {cause}. Nothing was written, so whatever changed there since is kept. To own it again, restore this world's Kiln data " +
               "from a backup that matches the world save (Saves.RestoreOnNextStart, or a kilnctl backup). If this world truly never had " +
               $"the plan (an older world file was put back and played on), remove '{plan}' from {witness}";
    }

    /// <summary>Puts every plan of <paramref name="book"/> into the witness (Apply: when it starts, and once it made a new
    /// record, before the first write). Entries of plans the book does not have are left: they are the evidence. A new
    /// world has no id before its first save: then the witness is written once that save is.</summary>
    public static void Witness(ITerrainBackend backend, TerrainBook book)
    {
        var plans = book.Plans.Keys.ToList();
        if (plans.Count == 0) return;
        var (id, revision) = backend.SaveIdentity;
        if (id is not null) { Write(backend, id, revision, plans); return; }
        // the save that gives the world its id carries these records: "since" is the revision before it
        backend.AfterNextSave((saveId, written) => Write(backend, saveId, written == 0 ? 0 : written - 1, plans));
    }

    /// <summary>Terrain.Forget: the plan's entry goes, so its next apply is a first install.</summary>
    public static void Unwitness(ITerrainBackend backend, string plan)
    {
        if (backend.SaveIdentity.Id is not { } id) return;
        try
        {
            if (backend.ReadWitness(id) is not { } w || !w.Plans.Remove(plan)) return;
            backend.WriteWitness(id, w);
        }
        catch (Exception e) when (e is InvalidDataException or IOException or UnauthorizedAccessException)
        {
            Kiln.Backend.Current.Log(0, LogLevel.Warn, $"terrain: {backend.Mod}: the witness entry of forgotten plan '{plan}' could not be removed ({e.Message}); the ledger's tombstone still makes its next apply a first install");
        }
    }

    private static void Write(ITerrainBackend backend, string id, uint since, IReadOnlyList<string> plans)
    {
        try
        {
            var w = backend.ReadWitness(id) ?? new TerrainWitness { Mod = backend.Mod };
            var changed = false;
            foreach (var p in plans)
            {
                // a missing entry, or one from a world file this one does not descend from (newer than this revision)
                if (w.Plans.TryGetValue(p, out var e) && e.Since <= since) continue;
                w.Plans[p] = new TerrainWitnessEntry { Since = since, At = DateTimeOffset.UtcNow };
                changed = true;
            }
            if (changed) backend.WriteWitness(id, w);
        }
        catch (InvalidDataException e)
        {
            // never replace a witness that cannot be read: it may hold the only evidence of other plans
            Kiln.Backend.Current.Log(0, LogLevel.Warn, $"terrain: {backend.Mod}: {backend.WitnessLocation(id)} cannot be read ({e.Message}); it is left as it is");
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            Kiln.Backend.Current.Log(0, LogLevel.Warn, $"terrain: {backend.Mod}: {backend.WitnessLocation(id)} could not be written ({e.Message}); a lost ledger of this world cannot be told from a first install until it is");
        }
    }
}
