// SPDX-License-Identifier: MIT
// Villager routines from a mod (docs/kiln/services/routines.md). A plan is a block: windows of the game day, each with one
// of the game's own villager tasks, and a fallback. The game's code still walks, sits, finds the bed and plays the
// animations; the plan only says which task a villager takes at its next decision.
//
//   day       (this file)               06:00-12:00 inspect, 12:00-14:00 sit, 14:00-21:00 roam, 21:00-06:00 sleep
//                                       (priority 5), fallback idle
//   day-json  (blocks/routines.json)    the same plan in data
//   awake     (this file)               idle all day: the villager stays up at night (keep-awake)
//
// In game (admin):
//   /routine list                       the villagers of the world, with their ids and routines
//   /routine assign [plan] [villager]   gives a plan to the villager nearest to you, or to one by its id (from the list)
//   /routine resume [villager]          ends the suspension a go to / teleport / stay / follow put on its routine
//   /routine remove [villager]          takes the plan off: the game chooses for the villager again
//   /routine show [villager]            what its routine does now (Routines.Inspect)
//   /routine follow [villager]          the villager follows you (a manual command: its routine is suspended)
//   /routine stop [villager]            it stops following (its routine stays suspended until /routine resume)
//   /routine time <time>                sets the time of day (06:30, 9pm, noon), to walk through the windows
//
// Sleep is the game's own sleep: at night the villager goes to its bed when the game's sleep time for it has come. A plan
// cannot make it sleep by day; before that time the Sleep is refused and the fallback (idle) runs until its next
// decision. Raise a plan's Version whenever you change its windows or its fallback: assignments are saved with the world,
// and a changed plan with the same version is refused at the next start.
using System.Globalization;
using Kiln;

namespace VillagerRoutine;

public sealed class VillagerRoutineMod : Mod
{
    private const string ModId = "villager-routine";
    private const string Decisions = "routines.decisions";
    private BlockHandle<RoutineBlock> _day = null!, _awake = null!;

    public override void OnLoad()
    {
        _day = Blocks.Define("day", new RoutineBlock
        {
            Version = 1,
            Windows =
            [
                new RoutineWindow(6 * 60, 12 * 60, new InspectTask()),
                new RoutineWindow(12 * 60, 14 * 60, new SitTask()),
                new RoutineWindow(14 * 60, 21 * 60, new RoamTask()),
                // over midnight: 21:00 to 06:00
                new RoutineWindow(21 * 60, 6 * 60, new SleepTask(), Priority: 5),
            ],
            Fallback = new IdleTask(),
        });
        _awake = Blocks.Define("awake", new RoutineBlock
        {
            Windows = [new RoutineWindow(0, 0, new IdleTask(), AllDay: true)],
            Fallback = new IdleTask(),
        });

        // What our routines do. Both events come on the tick thread after the step, except when the world stops
        // (Teardown): then only log, do not touch villagers or entities.
        Events.On<RoutineAssignmentChangedEvent>(e =>
        {
            if (e.After.Owner == ModId) Log.Info($"routines: {e}");
        });
        Events.On<RoutineTaskChangedEvent>(e =>
        {
            if (e.Key.StartsWith(ModId + ":", StringComparison.Ordinal)) Log.Info($"routines: {e}");
        });
    }

    [Command("routine list", Description = "the villagers of this world, their ids and routines", Permission = CommandPermission.Admin)]
    private void List(CommandContext c)
    {
        var at = c.Source.Player.IsNone ? null : World.Position(c.Source.Player);
        var on = Capabilities.Is(Decisions);
        var all = Villagers.List();
        if (all.Count == 0)
        {
            c.Reply("routine: no villagers in this world");
            return;
        }
        foreach (var v in all.OrderBy(v => at is { } p && v.Position is { } q ? p.DistanceTo(q) : double.MaxValue))
        {
            var far = at is { } p && v.Position is { } q ? $", {p.DistanceTo(q):0} m" : "";
            c.Reply($"routine: {v.NpcId:x8} {v.State}{far}: {Describe(on ? Routines.Inspect(v) : null)}");
        }
    }

    [Command("routine assign", Description = "gives a plan (day, day-json, awake) to the nearest villager or to one by its id", Permission = CommandPermission.Admin)]
    private async Task Assign(CommandContext c, [Choice("day", "day-json", "awake")] string plan = "day", string villager = "")
    {
        if (!Capabilities.Is(Decisions)) throw Off();
        var v = Pick(c, villager);
        var r = await Routines.Assign(v, Plan(plan));
        // Ok says the assignment changed, not that a task ran: what the villager does comes as task events (kiln.log)
        c.Reply(r.Ok ? $"routine: {plan} given to {v.NpcId:x8} (revision {r.Revision})" : $"routine: {v.NpcId:x8}: refused: {r.Reason}");
    }

    [Command("routine resume", Description = "ends the suspension a manual command put on the villager's routine", Permission = CommandPermission.Admin)]
    private async Task Resume(CommandContext c, string villager = "")
    {
        if (!Capabilities.Is(Decisions)) throw Off();
        var v = Pick(c, villager);
        // no teleport: the plan decides again at the villager's next decision; a Follow on it ends
        var r = await Routines.Resume(v);
        c.Reply(r.Ok ? $"routine: {v.NpcId:x8} resumed: {Describe(Routines.Inspect(v))}" : $"routine: {v.NpcId:x8}: refused: {r.Reason}");
    }

    [Command("routine remove", Description = "takes this mod's plan off the villager", Permission = CommandPermission.Admin)]
    private async Task Remove(CommandContext c, string villager = "")
    {
        if (!Capabilities.Is(Decisions)) throw Off();
        var v = Pick(c, villager);
        var r = await Routines.Remove(v);
        c.Reply(r.Ok ? $"routine: {v.NpcId:x8} has no routine of this mod now" : $"routine: {v.NpcId:x8}: refused: {r.Reason}");
    }

    [Command("routine show", Description = "what the villager's routine does now", Permission = CommandPermission.Admin)]
    private void Show(CommandContext c, string villager = "")
    {
        if (!Capabilities.Is(Decisions)) throw Off();
        var v = Pick(c, villager);
        c.Reply($"routine: {v.NpcId:x8} {v.State} at {v.Position}: {Describe(Routines.Inspect(v))}");
    }

    // A manual command, to see a routine suspend: Follow is the game's GoTo, sent again as you move.
    [Command("routine follow", Description = "the villager follows you (Villager.Follow): its routine is suspended until /routine resume", Permission = CommandPermission.Admin)]
    private void Follow(CommandContext c, string villager = "")
    {
        if (c.Source.Player.IsNone) throw new CommandException("only a player in the world can be followed");
        var v = Pick(c, villager);
        v.Follow(c.Source.Player);
        c.Reply($"routine: {v.NpcId:x8} follows you");
    }

    [Command("routine stop", Description = "the villager stops following (Villager.StopFollowing): this does not resume its routine", Permission = CommandPermission.Admin)]
    private void Stop(CommandContext c, string villager = "")
    {
        var v = Pick(c, villager);
        v.StopFollowing();
        c.Reply($"routine: {v.NpcId:x8} stopped following; {Describe(Capabilities.Is(Decisions) ? Routines.Inspect(v) : null)}");
    }

    [Command("routine time", Description = "sets the time of day: /routine time 06:30 (also 9pm, noon)", Permission = CommandPermission.Admin)]
    private void SetTime(CommandContext c, [TimeOfDay] double time)
    {
        if (!Capabilities.Is("kiln.time")) throw new CommandException($"setting the time is off here: {Capabilities.Why("kiln.time")}");
        Time.Set(time);
        c.Reply($"routine: the time is {Time.Format(time)}; villagers take the new window at their next decision");
    }

    private BlockHandle<RoutineBlock> Plan(string name) => name switch
    {
        "day" => _day,
        "awake" => _awake,
        // a plan from blocks/*.json: its handle by name
        _ => Blocks.Find(name, ModId) as BlockHandle<RoutineBlock> ?? throw new CommandException($"no routine {name} in this mod"),
    };

    private static CommandException Off() => new($"routines are off here: {Capabilities.Why(Decisions)}");

    // A villager by its id (hex, as /routine list shows it), else the one nearest to the player who typed the command.
    private static Villager Pick(CommandContext c, string villager)
    {
        var all = Villagers.List();
        if (villager.Length > 0)
        {
            var hex = villager.StartsWith("0x", StringComparison.OrdinalIgnoreCase) ? villager[2..] : villager;
            if (!uint.TryParse(hex, NumberStyles.HexNumber, CultureInfo.InvariantCulture, out var id))
                throw new CommandException($"{villager} is not a villager id (/routine list shows them)");
            return all.FirstOrDefault(v => v.NpcId == id) ?? throw new CommandException($"no villager {id:x8} in this world");
        }
        if (c.Source.Player.IsNone || World.Position(c.Source.Player) is not { } at)
            throw new CommandException("name the villager by its id (/routine list shows them)");
        return all.Where(v => v.Position is not null).OrderBy(v => v.Position!.Value.DistanceTo(at)).FirstOrDefault()
               ?? throw new CommandException("no villager in this world");
    }

    private static string Describe(RoutineSnapshot? s) => s is null
        ? "no routine"
        : $"{s.Key} v{s.PlanVersion} of {s.Owner}, {s.Assignment} (revision {s.Revision}), task {s.Task?.GetType().Name ?? "none"} {s.TaskState}" +
          (s.WindowIndex < 0 ? " (fallback)" : $" (window {s.WindowIndex})") + (s.LastReason is null ? "" : $", last: {s.LastReason}");
}
