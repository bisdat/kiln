// SPDX-License-Identifier: MIT
// Villager routines across a restart (chapter 16g §4.4): the world's saved assignments (RoutineBook, Kiln.Blocks' own
// bundle of the world's data, written with every world save) follow every assignment event, and when a world is ready
// after the mods loaded (the first data tick with the world's data) each saved row is decided against the plans the mods
// defined in OnLoad (RoutineStore.Restore) and bound again through kiln.routines' restore: the same revision for a plain
// rebind, a new one for a migration, the manual suspension kept. Removed and refused rows say so with an event.
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

    /// <summary>Set by tests that run without a game: where the assignments are kept, instead of the world's data.</summary>
    internal static RoutineBook? TestBook;
    /// <summary>Set by tests: the world's id instead of Saves.WorldId.</summary>
    internal static string? TestWorld;
    /// <summary>Whether a mod is loaded now, and the owner its calls use (tests set both).</summary>
    internal static Func<string, bool> OwnerLoaded = Mods.IsLoaded;
    internal static Func<string, ulong> OwnerOf = DefaultOwnerOf;

    private static bool ServerSide => Client.IsDedicatedServer || Client.IsHosting;

    /// <summary>The data tick (Kiln.Blocks' OnTick, after RoutineRuntime.Start).</summary>
    public static void Tick()
    {
        if (TestBook == null && (!ServerSide || !Spawn.WorldRunning)) return;
        var t = RoutineNative.T;
        if (t == null || !RoutineNative.HasRestore(t) || !Capabilities.Is(BlockCapabilities.RoutineDecisions))
        {
            if (!_toldOff && t != null)
                BlocksBridge.Log(LogLevel.Info, $"routines: saved routines wait: {(RoutineNative.HasRestore(t) ? "routines.decisions is off" : "kiln.routines has no restore")}");
            _toldOff = true;
            return;
        }
        if (!Book(out var book, out var world)) return;
        if (_restored == world && ReferenceEquals(_book, book)) return;
        _restored = world;
        _book = book;
        Restore(t, book, world);
        BindNpcBlocks(t, book);
    }

    // NpcBlock.Routine: a villager an NPC block makes gets the block's routine when the world has none saved for it (a
    // saved one, also a removed-by-its-owner one that is gone from the book, is the owner's later word: a saved row wins).
    // The owner is the mod's blocks' owner (peer E's ruling for data-only mods).
    private static void BindNpcBlocks(RoutineNative.Table* t, RoutineBook book)
    {
        foreach (var (mod, key, npcId) in RoutineRegistry.NpcBindings)
        {
            if (npcId == 0 || book.Find(npcId) != null) continue;
            var owner = OwnerOf(mod);
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
            if (e.Reason == "OwnerUnloaded") book.LastRevision = Math.Max(book.LastRevision, after.Revision);
            else book.Remove(after.NpcId, after.Revision);
            return;
        }
        var plan = RoutineRegistry.Find(after.Key)?.Plan;
        if (plan == null || plan.Version != after.PlanVersion) return;  // not this process's plan: the saved row stays as it was
        book.Record(after.NpcId, after.Owner, plan, after.Assignment == RoutineAssignmentState.ManualSuspended, after.Revision);
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
    }

    private static void Restore(RoutineNative.Table* t, RoutineBook book, string? world)
    {
        var rows = RoutineStore.Restore(book, world, OwnerLoaded, key => RoutineRegistry.Find(key)?.Plan);
        if (book.LastRevision > 0) t->SetRevisionFloor(book.LastRevision);
        int bound = 0, refused = 0, removed = 0;
        foreach (var (saved, d) in rows)
        {
            switch (d.Action)
            {
                case RoutineRestore.Rebind:
                case RoutineRestore.Migrate:
                {
                    var row = book.Find(saved.NpcId)!;
                    var owner = OwnerOf(saved.Owner);
                    if (owner == 0 || !Bind(t, owner, row, d.Plan!, d.Action == RoutineRestore.Migrate ? RoutineStore.ReasonMigrated : ""))
                    {
                        refused++;
                        BlocksBridge.Log(LogLevel.Warn, $"routines: the saved routine {saved.Key} of villager 0x{saved.NpcId:x8} was not bound again: kiln.routines refused it");
                        continue;
                    }
                    bound++;
                    break;
                }
                case RoutineRestore.Remove:
                    removed++;
                    Post(saved, RoutineAssignmentState.Removed, book.LastRevision, d.Reason!);
                    break;
                default:
                    refused++;
                    Post(saved, RoutineAssignmentState.VersionMismatch, saved.Revision, d.Reason!);
                    break;
            }
        }
        BlocksBridge.Log(LogLevel.Info, $"routines: world {world}: {bound} saved routine(s) bound again, {removed} removed, {refused} refused (last revision {book.LastRevision})");
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

    // The owner the mod's own calls use: its blocks' owner (a mod with code: its context's; a data-only mod: the one
    // Kiln made for its blocks). 0 when the mod has none in this process.
    private static ulong DefaultOwnerOf(string mod)
    {
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
