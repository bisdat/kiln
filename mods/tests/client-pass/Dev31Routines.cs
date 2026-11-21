// SPDX-License-Identifier: MIT
using Kiln;
using Kiln.Testing;
using static Kiln.Testing.ClientCheckTags;

namespace ClientPass;

// 16g-villager-routines (dev.31): villager day plans in a real game (docs/kiln/services/routines.md). The fixture is the
// sample mods/samples/VillagerRoutine (mod id villager-routine): its plans are its own, so the player gives them with the
// sample's chat commands (/routine assign, /routine resume) and this check watches what follows through Routines.Inspect
// and the two routine events. Hosted on L (the hosting client's world is a server world: the client binary's hooks); the
// dedicated-server and vanilla-client steps are in the package's CLIENT-CHECKS.md. Two parts: part 1 in a world, part 2
// after the world was left and hosted again (the assignments must come back). Orders 5100-5199
// (2000 + (28310 - 28000) * 10).
public sealed partial class ClientPassMod
{
    private const string VillagerRoutine = "villager-routine";
    private const string RoutinesCapability = "routines.decisions";

    [ClientCheck("16g-cc16g", "Natural routines, interruptions and arrival", Area = "Villager routines", Since = Dev31,
        Tags = [Visual], Needs = ClientCheckNeeds.Hosted, Order = 5100)]
    public async Task VillagerRoutines(CheckContext c)
    {
        if (!await Need(c, VillagerRoutine) || !await NeedPlayer(c)) return;
        // fail closed: without the capability nothing below can pass
        if (!c.Expect(Capabilities.Is(RoutinesCapability), $"{RoutinesCapability} is available in this hosted world",
                Capabilities.Why(RoutinesCapability))) return;

        var tasks = new System.Collections.Concurrent.ConcurrentQueue<RoutineTaskChangedEvent>();
        var assignments = new System.Collections.Concurrent.ConcurrentQueue<RoutineAssignmentChangedEvent>();
        var taskSub = Events.On<RoutineTaskChangedEvent>(e =>
        {
            if (!e.Key.StartsWith(VillagerRoutine + ":", StringComparison.Ordinal)) return;
            tasks.Enqueue(e);
            c.Log.Info($"client-pass: {c.Id}: {e}");
        });
        var assignSub = Events.On<RoutineAssignmentChangedEvent>(e =>
        {
            if (e.After.Owner != VillagerRoutine) return;
            assignments.Enqueue(e);
            c.Log.Info($"client-pass: {c.Id}: {e}");
        });
        c.OnEnd(() =>
        {
            taskSub.Dispose();
            assignSub.Dispose();
        });

        var stageFile = Path.Combine(c.Mod.DataDirectory, "16g-cc16g.stage");
        if (File.Exists(stageFile))
        {
            await RoutinesAfterRestart(c, stageFile, assignments);
            return;
        }

        // ---- set-up: two villagers with the sample's day plan -------------------------------------------------------
        c.Instruct("Host a world that has a base with at least two villagers, a chair or bench for each, a bed each and a " +
                   "crafting station, and stand in the base. In chat type /routine list, then for two villagers " +
                   "/routine assign day <id> (the ids from the list). Leave the others without a plan.");
        if (!await c.Until(() => Owned().Count >= 2, TimeSpan.FromMinutes(5), "two villagers have the sample's routine")) return;
        var two = Owned().Take(2).ToList();
        foreach (var v in two) c.Log.Info($"client-pass: {c.Id}: {v}: {Routines.Inspect(v)}");
        c.Expect(two.All(v => Routines.Inspect(v) is { Assignment: RoutineAssignmentState.Assigned, Key: VillagerRoutine + ":day" }),
            "both have the day plan, assigned");

        // ---- the windows, through midnight ---------------------------------------------------------------------------
        await RoutineWindow(c, tasks, two, 7, "inspect", t => t is InspectTask,
            "It is 07:00 (inspect window). Watch the two villagers: each should walk to something nearby (a station, a " +
            "chest) and work on it or look at it.");
        await RoutineWindow(c, tasks, two, 12.5, "sit", t => t is SitTask,
            "It is 12:30 (sit window). Each should walk to a free chair or bench and sit down.");
        await RoutineWindow(c, tasks, two, 15, "roam", t => t is RoamTask,
            "It is 15:00 (roam window). Each should walk around the base.");
        await RoutineWindow(c, tasks, two, 22.5, "sleep (or idle until the game's own bedtime)", t => t is SleepTask or IdleTask,
            "It is 22:30 (sleep window). Each should go to its own bed and lie down, or, if the game's own bedtime for it " +
            "has not come yet, stand idle and go to bed a little later. Wait a game hour if needed.");
        await RoutineWindow(c, tasks, two, 2, "sleep after midnight", t => t is SleepTask or IdleTask,
            "It is 02:00. They should still sleep (or go to bed now): no getting up and lying down again.");
        await RoutineWindow(c, tasks, two, 6.5, "wake to inspect", t => t is InspectTask,
            "It is 06:30. They should get up and go to work (inspect) at their next decision, not jump or teleport.");

        // ---- keep-awake: Idle all day at night ----------------------------------------------------------------------
        var awake = two[0];
        c.Instruct($"In chat type /routine assign awake {awake.NpcId:x8}. This villager now has Idle all day.");
        if (!await c.Until(() => Routines.Inspect(awake)?.Key == VillagerRoutine + ":awake", TimeSpan.FromMinutes(2), "the awake plan is on the villager")) return;
        Time.Set(23);
        var slept = false;
        var clock = System.Diagnostics.Stopwatch.StartNew();
        c.Instruct($"It is 23:00. Watch villager {awake.NpcId:x8} for a minute: it should stay up (stand, not walk to bed, not lie " +
                   "down and get up again).");
        while (clock.Elapsed < TimeSpan.FromSeconds(60))
        {
            if (awake.State is VillagerState.FindBed or VillagerState.Sleep) slept = true;
            await c.Ticks(10);
        }
        c.Expect(!slept, "keep-awake: the Idle-all-day villager never went to bed in 60 s at 23:00", awake.ToString());
        c.Instruct("Did it stay up the whole minute, without a flicker into bed and out? The other villager with the day plan " +
                   "should be asleep.");
        await c.Verdict();

        // ---- manual commands ---------------------------------------------------------------------------------------
        var m = two[1];
        Time.Set(15);
        m.Follow(c.Player);
        if (!await c.Until(() => Routines.Inspect(m)?.Assignment == RoutineAssignmentState.ManualSuspended, TimeSpan.FromSeconds(30),
                "Follow suspends the routine")) return;
        c.Expect(assignments.Any(e => e.After.NpcId == m.NpcId && e.Reason == "ManualSuspended"), "an event says ManualSuspended");
        c.Instruct($"Villager {m.NpcId:x8} now follows you (the check told it to). Walk 15 m away and back: it should follow.");
        await c.Verdict();
        m.StopFollowing();
        await c.Delay(TimeSpan.FromSeconds(5));
        c.Expect(Routines.Inspect(m)?.Assignment == RoutineAssignmentState.ManualSuspended, "StopFollowing does not resume",
            Routines.Inspect(m)?.ToString());
        m.Follow(c.Player);
        c.Instruct($"It follows you again. Walk 15 m away, then type /routine resume {m.NpcId:x8}.");
        if (!await c.Until(() => Routines.Inspect(m)?.Assignment == RoutineAssignmentState.Assigned, TimeSpan.FromMinutes(2), "Resume")) return;
        var at = m.Position;
        await c.Delay(TimeSpan.FromSeconds(3));
        c.Expect(!m.IsFollowing, "Resume ended the Follow");
        c.Expect(at is { } p0 && m.Position is { } p1 && p0.DistanceTo(p1) < 10, "no teleport on Resume (moved less than 10 m in 3 s)",
            $"{at} -> {m.Position}");
        c.Instruct("Did it stop following you and walk (not teleport) back to roam in the base?");
        await c.Verdict();

        // ---- distance --------------------------------------------------------------------------------------------
        c.Instruct("Walk more than 150 m away from the base (the villagers pause there), wait a minute, and walk back. Do " +
                   "they carry on with their plans, without teleports or stuck poses?");
        await c.Verdict();

        // ---- part 2 after a restart --------------------------------------------------------------------------------
        Directory.CreateDirectory(Path.GetDirectoryName(stageFile)!);
        File.WriteAllLines(stageFile, Owned().Select(v => Routines.Inspect(v) is { } s ? $"{s.NpcId:x8} {s.Revision} {s.Key}" : null)
            .OfType<string>());
        c.Instruct("Part 1 done. Leave the world to the main menu (it saves), host the same world again, stand in the base and " +
                   "run this check again: part 2 checks the plans came back.");
        await c.Verdict();
    }

    private static async Task RoutinesAfterRestart(CheckContext c, string stageFile,
        System.Collections.Concurrent.ConcurrentQueue<RoutineAssignmentChangedEvent> assignments)
    {
        var saved = File.ReadAllLines(stageFile).Select(l => l.Split(' ')).Where(p => p.Length == 3)
            .Select(p => (Id: Convert.ToUInt32(p[0], 16), Revision: ulong.Parse(p[1]), Key: p[2])).ToList();
        c.Expect(saved.Count >= 2, "part 1 saved at least two assignments", $"{saved.Count}");
        foreach (var (id, revision, key) in saved)
        {
            var ok = await c.Until(() => Villagers.List().FirstOrDefault(v => v.NpcId == id) is { } v && Routines.Inspect(v) is not null,
                TimeSpan.FromMinutes(1), $"villager {id:x8} has its routine again");
            var s = Villagers.List().FirstOrDefault(v => v.NpcId == id) is { } w ? Routines.Inspect(w) : null;
            c.Log.Info($"client-pass: {c.Id}: after the restart {id:x8}: {s}");
            c.Expect(ok && s is not null && s.Key == key && s.Revision == revision && s.Assignment != RoutineAssignmentState.VersionMismatch,
                $"restart: {id:x8} has {key} again at revision {revision}", s?.ToString());
        }
        c.Expect(!assignments.Any(e => e.Reason is "OwnerMissing" or "DefinitionMissing" or "VersionNotBumped" or "Downgrade"),
            "no saved routine was removed or refused");
        try { File.Delete(stageFile); } catch (IOException) { }
        c.Instruct("Part 2. The plans are back (see the assertions). Watch the villagers for a minute: do they carry on with the " +
                   "plan of the time of day, without teleports? The /routine list command shows the same.");
        await c.Verdict();
    }

    // The villagers that hold one of the sample's routines now.
    private static List<Villager> Owned() =>
        Villagers.List().Where(v => Routines.Inspect(v)?.Owner == VillagerRoutine).OrderBy(v => v.NpcId).ToList();

    // Sets the time into a window and waits (2 min at most) until each villager started a task of that window at a
    // decision of its own; then asks what the player sees.
    private static async Task RoutineWindow(CheckContext c, System.Collections.Concurrent.ConcurrentQueue<RoutineTaskChangedEvent> tasks,
        List<Villager> villagers, double hour, string what, Func<RoutineTask?, bool> expected, string text)
    {
        tasks.Clear();
        Time.Set(hour);
        c.Instruct(text + " (Kiln set the time; give them up to two minutes to reach a decision.)");
        foreach (var v in villagers)
            c.Expect(await c.Until(() => tasks.Any(e => e.NpcId == v.NpcId && e.To == RoutineTaskState.Started && expected(e.Task)),
                    TimeSpan.FromMinutes(2), $"{v.NpcId:x8} started {what}"),
                $"{Time.Format(hour)}: {v.NpcId:x8} started {what}", Routines.Inspect(v)?.ToString());
        c.Instruct(text + " Is that what you see?");
        await c.Verdict();
    }
}
