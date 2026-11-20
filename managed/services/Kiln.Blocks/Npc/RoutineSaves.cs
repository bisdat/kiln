// SPDX-License-Identifier: MIT
// Villager routines across a restart (chapter 16g §4.4): the world's saved assignments (RoutineBook, Kiln.Blocks' own
// bundle of the world's data, written with every world save) follow every assignment event, and when a world is ready
// after the mods loaded (checked every step from the world's start: its data is there and no mod's data files wait for
// their pass) each saved row is decided against the plans the mods defined (RoutineStore.Restore) and bound again
// through kiln.routines' restore: the same revision for a plain rebind, a new one for a migration, the manual suspension
// kept. Removed and refused rows say so with an event. A Routines call made before that runs the restore first (or is
// refused "WorldLoading" while it cannot), so no change of a mod lands under the saved floor or is undone by it.
// A mod's routines come with its live owner (its code's context; a data-only mod: its blocks' owner), named to
// kiln.routines with claim_mod; after a mod's reload its saved rows are bound again under its new owner.
// Server side only. Tick thread.
namespace Kiln;

internal static unsafe class RoutineSaves
{
    private static ModContext? _data;
    // the world whose book this session restored, and the book itself: a new world, or the same world loaded again (a new
    // book object), restores again
    private static string? _restored;
    private static RoutineBook? _book;
    private static bool _warned, _toldOff, _toldNoBook, _toldSaving;
    private static IDisposable? _tick;
    // mods whose owner went while the world ran (a reload or an unload): their saved rows are bound again once they are
    // back with their plans
    private static readonly HashSet<string> Rebind = new(StringComparer.Ordinal);
    // villagers whose saved routine was refused at the restore (VersionMismatch), by owner mod: Routines.Remove clears them
    private static readonly Dictionary<uint, string> Mismatch = new();

    /// <summary>Set by tests that run without a game: where the assignments are kept, instead of the world's data.</summary>
    internal static RoutineBook? TestBook;
    /// <summary>Set by tests: the world's id instead of Saves.WorldId.</summary>
    internal static string? TestWorld;
    /// <summary>Whether a mod is loaded now, and the owner its calls use (tests set both).</summary>
    internal static Func<string, bool> OwnerLoaded = Mods.IsLoaded;
    internal static Func<string, ulong> OwnerOf = DefaultOwnerOf;

    private static bool ServerSide => Client.IsDedicatedServer || Client.IsHosting;

    /// <summary>Kiln.Blocks' data tick: from then on the restore is looked at every step (its own service tick).</summary>
    public static void Tick()
    {
        if (TestBook == null) _tick ??= BlocksBridge.AddTick("kiln blocks routines", _ => Ensure());
        Ensure();
    }

    /// <summary>Whether a Routines call may go to kiln.routines now: null yes (restored, or nothing to restore here), else
    /// the reason it must wait ("WorldLoading": the world's data or a mod's data files are not there yet).</summary>
    public static string? Ensure()
    {
        if (TestBook == null && (!ServerSide || !Spawn.WorldRunning)) return null;
        var t = RoutineNative.T;
        if (t == null || !RoutineNative.HasRestore(t) || !Capabilities.Is(BlockCapabilities.RoutineDecisions))
        {
            if (!_toldOff && t != null)
                BlocksBridge.Log(LogLevel.Info, $"routines: saved routines wait: {(RoutineNative.HasRestore(t) ? "routines.decisions is off" : "kiln.routines has no restore")}");
            _toldOff = true;
            return null;
        }
        if (!Book(out var book, out var world)) return TestBook == null ? "WorldLoading" : null;
        if (_restored == world && ReferenceEquals(_book, book))
        {
            if (Rebind.Count > 0) RebindReloaded(t, book);
            return null;
        }
        // the plans of data files compile at the data pass: restore once none is due, so no saved plan reads as missing
        if (TestBook == null && BlocksStartup.PassDue) return "WorldLoading";
        _restored = world;
        _book = book;
        Mismatch.Clear();
        Rebind.Clear();
        Restore(t, book, world);
        BindNpcBlocks(t, book);
        return null;
    }

    /// <summary>The owner a mod's Routines calls come with, named to kiln.routines (claim_mod).</summary>
    public static void Claim(RoutineNative.Table* t, ulong owner, string mod) => RoutineNative.Claim(t, owner, mod);

    /// <summary>Routines.Remove of a villager whose saved routine the restore refused (VersionMismatch): the saved row
    /// goes, with its event. Returns the revision, or 0 when there was none of this mod's.</summary>
    public static ulong RemoveMismatch(uint npcId, string mod)
    {
        if (!Mismatch.TryGetValue(npcId, out var owner) || owner != mod || !Book(out var book, out _)) return 0;
        Mismatch.Remove(npcId);
        var row = book.Find(npcId);
        if (row == null) return 0;
        var revision = book.NextRevision();
        book.Remove(npcId, revision);
        MarkRemoved(book, npcId);
        Post(row, RoutineAssignmentState.Removed, revision, "Removed");
        return revision;
    }

    // NpcBlock.Routine: a villager an NPC block makes gets the block's routine at a world start when the world has none
    // saved for it and its owner did not remove one (RoutineBook.RemovedByOwner: the owner's removal sticks). The owner is
    // the mod's live owner (OwnerOf).
    private static void BindNpcBlocks(RoutineNative.Table* t, RoutineBook book)
    {
        foreach (var (mod, key, npcId) in RoutineRegistry.NpcBindings)
        {
            if (npcId == 0 || book.Find(npcId) != null || book.RemovedByOwner.Contains(npcId)) continue;
            var owner = OwnerOf(mod);
            Claim(t, owner, mod);
            var plan = RoutineRegistry.Find(key)?.Plan;
            if (owner == 0 || plan == null) continue;
            var wire = System.Text.Encoding.UTF8.GetBytes(plan.Wire);
            ulong ticket = 0;
            int rc;
            fixed (byte* p = wire) rc = t->Assign(owner, npcId, new NativeStr { Ptr = p, Len = (nuint)wire.Length }, &Done, null, &ticket);
            if (rc != RoutineNative.Ok) BlocksBridge.Log(LogLevel.Warn, $"routines: {mod}: the NPC block's routine {key} was not given to villager 0x{npcId:x8} (kiln.routines answered {rc})");
        }
    }

    /// <summary>An assignment event, as Kiln.Blocks posts it: the book follows. The world's teardown leaves the book as it
    /// is (the next world, or this one loaded again, restores from it).</summary>
    public static void Observe(RoutineAssignmentChangedEvent e)
    {
        if (e.Teardown)
        {
            _restored = null;
            _book = null;
            Rebind.Clear();
            Mismatch.Clear();
            return;
        }
        if (!Book(out var book, out _))
        {
            if (!_toldNoBook) BlocksBridge.Log(LogLevel.Info, "routines: an assignment changed before the world's data is ready: not saved yet");
            _toldNoBook = true;
            return;
        }
        var after = e.After;
        if (after.Assignment == RoutineAssignmentState.Removed)
        {
            // An owner that unloads is not the owner removing its routine: the server's shutdown unloads every mod before
            // the world's last save, and a mod switched off comes back with the next start. The saved row stays; the next
            // world start binds it again when the mod and its plan are there, or removes it (OwnerMissing).
            if (e.Reason == "OwnerUnloaded")
            {
                book.LastRevision = Math.Max(book.LastRevision, after.Revision);
                Rebind.Add(RoutineIdentity.ModOf(after.Key));  // bound again when the mod is back (a reload)
            }
            else if (book.Remove(after.NpcId, after.Revision)) MarkRemoved(book, after.NpcId);
            return;
        }
        var plan = RoutineRegistry.Find(after.Key)?.Plan;
        if (plan == null || plan.Version != after.PlanVersion) return;  // not this process's plan: the saved row stays as it was
        book.Record(after.NpcId, after.Owner, plan, after.Assignment == RoutineAssignmentState.ManualSuspended, after.Revision);
        book.RemovedByOwner.Remove(after.NpcId);
        Mismatch.Remove(after.NpcId);
        if (!_toldSaving) BlocksBridge.Log(LogLevel.Info, $"routines: the world's saved routines follow the assignments ({book.Assignments.Count} now)");
        _toldSaving = true;
    }

    internal static void Forget()
    {
        _restored = null;
        _book = null;
        _data = null;
        TestBook = null;
        TestWorld = null;
        OwnerLoaded = Mods.IsLoaded;
        OwnerOf = DefaultOwnerOf;
        Rebind.Clear();
        Mismatch.Clear();
        _tick?.Dispose();
        _tick = null;
    }

    private static void MarkRemoved(RoutineBook book, uint npcId)
    {
        foreach (var (_, _, id) in RoutineRegistry.NpcBindings)
            if (id == npcId && !book.RemovedByOwner.Contains(npcId)) book.RemovedByOwner.Add(npcId);
    }

    // A mod whose owner went while the world ran is back: its saved rows that are not bound now are decided and bound
    // again under its new owner, as at a world start.
    private static void RebindReloaded(RoutineNative.Table* t, RoutineBook book)
    {
        if (TestBook == null && BlocksStartup.PassDue) return;
        foreach (var mod in Rebind.ToList())
        {
            if (!OwnerLoaded(mod) || OwnerOf(mod) == 0) continue;
            Rebind.Remove(mod);
            var owner = OwnerOf(mod);
            Claim(t, owner, mod);
            foreach (var row in book.Assignments.Where(a => a.Owner == mod).ToList())
                Apply(t, book, row, RoutineStore.Decide(row, true, RoutineRegistry.Find(row.Key)?.Plan), owner, live: true);
        }
    }

    private static void Restore(RoutineNative.Table* t, RoutineBook book, string? world)
    {
        var rows = RoutineStore.Restore(book, world, OwnerLoaded, key => RoutineRegistry.Find(key)?.Plan);
        if (book.LastRevision > 0) t->SetRevisionFloor(book.LastRevision);
        int bound = 0, refused = 0, removed = 0;
        foreach (var mod in rows.Select(r => r.Saved.Owner).Distinct()) Claim(t, OwnerOf(mod), mod);
        foreach (var (saved, d) in rows)
            switch (Apply(t, book, saved, d, OwnerOf(saved.Owner), live: false))
            {
                case 1: bound++; break;
                case 2: removed++; break;
                default: refused++; break;
            }
        BlocksBridge.Log(LogLevel.Info, $"routines: world {world}: {bound} saved routine(s) bound again, {removed} removed, {refused} refused (last revision {book.LastRevision})");
    }

    // One decided row: 1 bound, 2 removed, 0 refused. `live`: decided now (a reload), so the book still has to change
    // (RoutineStore.Restore already changed it for a world start).
    private static int Apply(RoutineNative.Table* t, RoutineBook book, SavedRoutine saved, RoutineRestoreDecision d, ulong owner, bool live)
    {
        switch (d.Action)
        {
            case RoutineRestore.Rebind:
            case RoutineRestore.Migrate:
            {
                var row = book.Find(saved.NpcId)!;
                if (live && d.Action == RoutineRestore.Migrate)
                    (row.PlanVersion, row.ContentHash, row.Revision) = (d.Plan!.Version, d.Plan.ContentHash, book.NextRevision());
                if (owner == 0 || !Bind(t, owner, row, d.Plan!, d.Action == RoutineRestore.Migrate ? RoutineStore.ReasonMigrated : ""))
                {
                    BlocksBridge.Log(LogLevel.Warn, $"routines: the saved routine {saved.Key} of villager 0x{saved.NpcId:x8} was not bound again: kiln.routines refused it");
                    return 0;
                }
                return 1;
            }
            case RoutineRestore.Remove:
            {
                if (live)
                {
                    book.Assignments.Remove(saved);
                    book.NextRevision();
                }
                Post(saved, RoutineAssignmentState.Removed, book.LastRevision, d.Reason!);
                return 2;
            }
            default:
                Mismatch[saved.NpcId] = saved.Owner;
                Post(saved, RoutineAssignmentState.VersionMismatch, saved.Revision, d.Reason!);
                return 0;
        }
    }

    private static bool Bind(RoutineNative.Table* t, ulong owner, SavedRoutine row, RoutinePlan plan, string reason)
    {
        var wire = System.Text.Encoding.UTF8.GetBytes(plan.Wire);
        var why = System.Text.Encoding.UTF8.GetBytes(reason);
        ulong ticket = 0;
        fixed (byte* p = wire)
        fixed (byte* r = why)
        {
            var rc = t->Restore(owner, row.NpcId, new NativeStr { Ptr = p, Len = (nuint)wire.Length }, row.Revision,
                row.ManualSuspended ? RoutineNative.RestoreSuspended : 0, new NativeStr { Ptr = r, Len = (nuint)why.Length }, &Done, null, &ticket);
            return rc == RoutineNative.Ok;
        }
    }

    [System.Runtime.InteropServices.UnmanagedCallersOnly(CallConvs = [typeof(System.Runtime.CompilerServices.CallConvCdecl)])]
    private static void Done(RoutineNative.Result* r, void* user)
    {
        // a refusal at the commit (another mod assigned the villager before the world was ready): that mod's routine holds
        if (r != null && r->Ok == 0)
            BlocksBridge.Log(LogLevel.Warn, $"routines: a saved routine of villager 0x{r->NpcId:x8} was not bound again: {r->Reason}");
    }

    // The owner the mod's own Routines calls come with: a mod with code, its live context's owner (Routines.* use
    // ModContext.Owner); a data-only mod, the owner Kiln made for its blocks. 0 when the mod has none in this process.
    private static ulong DefaultOwnerOf(string mod)
    {
        if (Mods.Get(mod)?.Info is { } info && ModContext.FindLive(info) is { Owner: not 0 } live) return live.Owner;
        foreach (var m in Blocks.AllMods)
            if (m.Id == mod) return m.Owner;
        return 0;
    }

    private static void Post(SavedRoutine saved, RoutineAssignmentState state, ulong revision, string reason)
    {
        var before = new RoutineSnapshot(saved.NpcId, saved.Owner, saved.Key, saved.PlanVersion, saved.Revision,
            saved.ManualSuspended ? RoutineAssignmentState.ManualSuspended : RoutineAssignmentState.PendingSpawn, -1, 0, null, RoutineTaskState.None, VillagerState.Invalid, null);
        var after = before with { Assignment = state, Revision = revision, LastReason = reason };
        Events.Post(new RoutineAssignmentChangedEvent(before, after, reason, false));
    }

    private static bool Book(out RoutineBook book, out string? world)
    {
        world = TestWorld;
        if (TestBook != null)
        {
            book = TestBook;
            return true;
        }
        book = null!;
        if (!BlocksBridge.WorldDataReady) return false;
        world = Saves.WorldId;
        try
        {
            book = BlocksBridge.AsData(ref _data, "kiln.blocks", () => WorldData.Get<RoutineBook>(RoutineStore.WorldKey));
            return true;
        }
        catch (Exception e) when (e is NotSupportedException or InvalidOperationException or InvalidDataException)
        {
            if (!_warned) BlocksBridge.Log(LogLevel.Warn, $"routines: the world's data is not usable, routines are not kept across a restart: {e.Message}");
            _warned = true;
            return false;
        }
    }
}
