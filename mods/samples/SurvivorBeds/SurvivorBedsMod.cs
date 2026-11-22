// SPDX-License-Identifier: MIT
// Survivor beds: a sample of Survivors (docs/kiln/services/survivors.md).
//   /bed              the survivor nearest to you gets the sleeping place nearest to you. It runs with YOUR rights
//                     (SurvivorAuthorization.PlayerPermissions): you need the base-edit permission and the bed must be in a
//                     base whose rules let you build there. The answer says why not when it is refused.
//   /recruit <name>   admins: recruits a survivor by (part of) its roster name for the world and hands you its summon tool.
//                     RecruitWorld is a trusted server operation, so the command checks the permission itself.
using Kiln;

namespace SurvivorBeds;

public sealed class SurvivorBedsMod : Mod
{
    private readonly List<IDisposable> _commands = [];

    public override void OnLoad()
    {
        if (!Chat.Available) return;
        _commands.Add(Commands.Register("bed", c => _ = Bed(c), CommandPermission.Everyone, "", "Give the nearest survivor the nearest bed"));
        _commands.Add(Commands.Register("recruit", c => _ = Recruit(c), CommandPermission.Admin, "<name>", "Recruit a survivor and get its summon tool"));
    }

    public override void OnUnload()
    {
        foreach (var c in _commands) c.Dispose();
    }

    private static double Dist(WorldPosition a, WorldPosition b) =>
        Math.Sqrt((a.X - b.X) * (a.X - b.X) + (a.Y - b.Y) * (a.Y - b.Y) + (a.Z - b.Z) * (a.Z - b.Z));

    private static async Task Bed(CommandContext ctx)
    {
        if (!ctx.Source.IsPlayer || World.Position(ctx.Source.Player) is not { } me)
        {
            ctx.Reply("Run /bed in game, next to the bed.");
            return;
        }
        if (!Capabilities.Is(Survivors.AssignCapability))
        {
            ctx.Reply($"Beds cannot be assigned here: {Capabilities.Why(Survivors.AssignCapability)}");
            return;
        }
        var survivor = Survivors.List().Where(s => s.Home is not null && !s.Entity.IsNone)
            .OrderBy(s => Dist(s.Home!.Value, me)).FirstOrDefault();
        if (survivor is null)
        {
            ctx.Reply("No survivor lives in this world yet.");
            return;
        }
        EntityId bed = default;
        foreach (var e in Spawn.FindByName("Bed", me, 10))
        {
            try
            {
                Survivors.BedOccupant(e);  // throws for anything that is no sleeping place (a double bed's frame)
                bed = e;
                break;
            }
            catch (ArgumentException) { }
        }
        if (bed.IsNone)
        {
            ctx.Reply("Stand within 10 m of a bed.");
            return;
        }
        var r = await Survivors.AssignBed(survivor.Id, bed, new SurvivorAssignmentOptions(ctx.Source.Player));
        ctx.Reply(r.Ok ? $"{survivor.DebugName} sleeps here now." : $"Not assigned: {r.Reason}");
    }

    private static async Task Recruit(CommandContext ctx)
    {
        var name = ctx.Arg(0);
        var s = Survivors.List().FirstOrDefault(x => x.DebugName.Contains(name, StringComparison.OrdinalIgnoreCase));
        if (s is null)
        {
            ctx.Reply($"No survivor's name contains '{name}'.");
            return;
        }
        var me = ctx.Source.IsPlayer ? Players.Find(ctx.Source.Player) : null;
        var r = await Survivors.RecruitWorld(s.Id, new SurvivorRecruitOptions(me));
        ctx.Reply(r.Ok ? $"{s.DebugName}: {r.Completion} ({r.Effects})" : $"{s.DebugName}: {r.Completion}: {r.Reason}");
    }
}
