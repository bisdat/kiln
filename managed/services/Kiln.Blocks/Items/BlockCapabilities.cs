// SPDX-License-Identifier: MIT
// The capability rows of item blocks' features (docs/kiln/services/caps.md): blocks.weapon_moves (a weapon's moves, a wand's and a staff cast's animation and
// hitAt) needs kiln.gamedata's sequence clones and item data; blocks.animated_held (a look's animated held model) needs kiln.gamedata's template clones and
// kiln.assets. Reported once, when the first item block compiles: a row a managed service registers exists only from its first use, so a manifest's
// features cannot require it. The state is decided from what this process has; it does not change while it runs.
namespace Kiln;

internal static class BlockCapabilities
{
    internal const string WeaponMoves = "blocks.weapon_moves", AnimatedHeld = "blocks.animated_held", MoveFollowUps = "blocks.move_followups";

    /// <summary>A row: its state, reason and text.</summary>
    internal readonly record struct Row(CapabilityState State, CapabilityReason Reason, string? Text);

    /// <summary>The moves' row, from whether kiln.gamedata is here. Pure.</summary>
    internal static Row MovesRow(bool gamedata) => gamedata
        ? new Row(CapabilityState.Available, CapabilityReason.None, null)
        : new Row(CapabilityState.Unavailable, CapabilityReason.ServiceMissing, "kiln.gamedata is not loaded: a move is a sequence clone with rules, and the item's slots are item data");

    /// <summary>The follow-ups' row (a weapon's moves applied as one generation), from whether kiln.gamedata is here. Pure.
    /// Open on both sides since dev.29; guided checks 16c-b and 16c-d still describe unverified client behavior.</summary>
    internal static Row FollowUpsRow(bool gamedata) => gamedata
        ? new Row(CapabilityState.Available, CapabilityReason.None, null)
        : new Row(CapabilityState.Unavailable, CapabilityReason.ServiceMissing, "kiln.gamedata is not loaded: a routed move is a sequence clone with rules, and the switch is item data");

    /// <summary>Whether the client gate holds a row, and whether kiln.gamedata is here (tests set them).</summary>
    internal static Func<string, bool> Held = name => Capabilities.IsHeld(name);
    internal static Func<bool> GameData = () => Content.GameDataAvailable;

    /// <summary>Throws <see cref="KilnUnavailableException"/> naming blocks.move_followups: at a definition when the row is held (a tool or a test with no game
    /// still compiles the block); at a <paramref name="query"/> also when kiln.gamedata is not here, since nothing can be applied then.</summary>
    internal static void RequireFollowUps(bool query = false)
    {
        Ensure();
        bool held;
        try { held = Held(MoveFollowUps); }
        catch (Exception e) when (e is InvalidOperationException or NotSupportedException or DllNotFoundException or EntryPointNotFoundException) { held = false; }
        if (held) throw new KilnUnavailableException(MoveFollowUps, "held by capability policy");
        if (!query) return;
        bool gamedata;
        try { gamedata = GameData(); }
        catch (Exception e) when (e is InvalidOperationException or NotSupportedException or DllNotFoundException or EntryPointNotFoundException) { gamedata = false; }
        if (!gamedata) throw new KilnUnavailableException(MoveFollowUps, FollowUpsRow(false).Text);
    }

    /// <summary>The animated held model's row, from whether kiln.gamedata and kiln.assets are here. Pure.</summary>
    internal static Row HeldRow(bool gamedata, bool assets) => (gamedata, assets) switch
    {
        (true, true) => new Row(CapabilityState.Available, CapabilityReason.None, null),
        (false, _) => new Row(CapabilityState.Unavailable, CapabilityReason.ServiceMissing, "kiln.gamedata is not loaded: an animated held model is a template clone"),
        _ => new Row(CapabilityState.Unavailable, CapabilityReason.ServiceMissing, "kiln.assets is not loaded: it says whether the moving model and its graph are cooked here"),
    };

    /// <summary>Where the rows go: the capability table (tests set it).</summary>
    internal static Action<string, Row> Report = (name, row) => Capabilities.Report(name, row.State, row.Reason, row.Text);

    private static long _reported = -1;  // the registry binding they were reported under

    /// <summary>Reports both rows, once per registry binding (again after a reset, as a test kit's next harness has).</summary>
    internal static void Ensure()
    {
        var epoch = Capabilities.CurrentBinding.Epoch;
        if (Interlocked.Exchange(ref _reported, epoch) == epoch) return;
        try
        {
            var gamedata = Content.GameDataAvailable;
            Report(WeaponMoves, MovesRow(gamedata));
            Report(MoveFollowUps, FollowUpsRow(gamedata));
            Report(AnimatedHeld, HeldRow(gamedata, Assets.Available));
            ReportRoutines();
        }
        catch (Exception e) when (e is InvalidOperationException or ArgumentException or NotSupportedException or DllNotFoundException or EntryPointNotFoundException)
        {
            // no capability table in this process (a tool, a test): the blocks still compile
            Interlocked.Exchange(ref _reported, -1);
        }
    }

    /// <summary>For tests: report again at the next <see cref="Ensure"/>.</summary>
    internal static void Rearm() => Interlocked.Exchange(ref _reported, 0);

    // ---- roster bodies ---------------------------------------------------------------------------------------
    internal const string NpcBody = "blocks.npc_body";

    internal static Row NpcBodyRow(bool nativeQuests, Func<string, Capability> get)
    {
        if (!nativeQuests) return new(CapabilityState.Unavailable, CapabilityReason.ServiceMissing,
            "quests.boot_pack: the native quests backend is not loaded; custom roster bodies need boot publication");
        foreach (var name in new[] { "quests.boot_pack", "quests.npc_body", "quests.npc", "assets.resources", "assets.content" })
        {
            var dependency = get(name);
            if (!dependency.IsAvailable) return new(CapabilityState.Unavailable,
                dependency.Reason == CapabilityReason.Unknown ? CapabilityReason.ServiceMissing : dependency.Reason,
                $"{name}: {(string.IsNullOrEmpty(dependency.ReasonText) ? "the required native implementation is not available" : dependency.ReasonText)}");
        }
        return new(CapabilityState.Available, CapabilityReason.None, null);
    }

    internal static Row CurrentNpcBodyRow() => NpcBodyRow(ServiceCaps.Probe("kiln.knowledge"), Capabilities.Get);

    // ---- villager routines (0.2.0-dev.31; docs/kiln/services/routines.md) ------------------------------------------
    // routines.decisions is a sigdb feature kiln.routines serves (the villager task chooser and its sleep check); nothing
    // holds it (its guided check 16g-cc16g is the owner's game pass); Kiln.Blocks only lowers it where the service is not here (BlocksRows.Report:
    // a rows report, never a mod's own call, asks for the table), as Kiln.Queries does for the voxel ray. routines.anchors (a task that walks to an authored place) and routines.jobs (work at a station)
    // are names the wire format reserves: declared, and off in this version.
    internal const string RoutinesService = "kiln.routines", RoutineDecisions = "routines.decisions",
        RoutineAnchors = "routines.anchors", RoutineJobs = "routines.jobs";

    /// <summary>The row of the two reserved names. Pure.</summary>
    internal static Row RoutineLaterRow(string what) =>
        new(CapabilityState.Unavailable, CapabilityReason.Policy, $"not in this version of Kiln: {what}");

    /// <summary>Whether <see cref="RoutineDecisions"/> works here, from what the process has. Pure.
    /// <paramref name="service"/>: kiln.routines' table is here; <paramref name="feature"/>: the row reads available
    /// (the symbols resolved and the client gate does not hold it).</summary>
    internal static bool RoutinesOn(bool service, bool feature) => service && feature;

    private static void ReportRoutines()
    {
        Report(RoutineAnchors, RoutineLaterRow("a routine task that walks to an authored place"));
        Report(RoutineJobs, RoutineLaterRow("a routine task that works at a station"));
    }

    /// <summary>Throws <see cref="KilnUnavailableException"/> naming routines.decisions unless kiln.routines is here and
    /// the row is available.</summary>
    internal static void RequireRoutines()
    {
        Ensure();
        if (RoutinesOn(ServiceCaps.Probe(RoutinesService), Capabilities.Is(RoutineDecisions))) return;
        throw ServiceCaps.Unavailable(RoutineDecisions, ServiceCaps.Probe(RoutinesService) ? null : "kiln.routines is not loaded");
    }
}
