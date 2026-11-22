// SPDX-License-Identifier: MIT
// Routines.Resume ends a Follow at the call (chapter 16g §4.3; review item 3): Kiln.Blocks' own code on the kit's
// kiln.routines, a villager of the kit's world following a fake player. A Follow still running when the Resume is made
// would send its next GoTo within a second and suspend the routine again, so it must end before the Resume is queued.
using System.Globalization;
using Xunit;

namespace Kiln.TestKit.Tests;

public sealed class RoutinesResumeFollowTests
{
    private sealed class FollowMod : Mod
    {
        private BlockHandle<RoutineBlock> _day = null!;
        public static Guid Template;
        private Task<RoutineResult>? _pending;

        public override void OnLoad()
        {
            _day = Blocks.Define("day", new RoutineBlock { Windows = [new RoutineWindow(0, 0, new RoamTask(), AllDay: true)], Fallback = new IdleTask() });
            Commands.Register("rt", async ctx =>
            {
                if (ctx.Args[0] == "spawn")
                {
                    var e = await Spawn.Entity("Villager_Kit", new WorldPosition(11, 0, 10), 0);
                    ctx.Reply($"spawned {e.Value}");
                    return;
                }
                // the kit's types have no villager record (DynamicActiveNpcState), so the villager is found by its template
                var v = Villagers.Find(Template);
                if (v is null)
                {
                    ctx.Reply("no villager");
                    return;
                }
                try
                {
                switch (ctx.Args[0])
                {
                    case "assign":
                        _pending = Routines.Assign(v, _day);  // completes at the next step
                        ctx.Reply("assign queued");
                        break;
                    case "result":
                        ctx.Reply(_pending is { IsCompleted: true } done ? $"result {done.Result.Ok}" : "result pending");
                        break;
                    case "follow":
                        v.Follow(new EntityId(uint.Parse(ctx.Args[2], CultureInfo.InvariantCulture)));
                        ctx.Reply($"following {v.IsFollowing}");
                        break;
                    case "resume":
                        _pending = Routines.Resume(v);
                        ctx.Reply($"following after the call {v.IsFollowing}");
                        break;
                }
                }
                catch (Exception x)
                {
                    ctx.Reply($"threw {x.GetType().Name}: {x.Message}");
                }
            });
        }
    }

    private static string Said(CommandResult r) => string.Join("\n", r.Error is null ? r.Output : r.Output.Append(r.Error));

    [Fact]
    public void Resume_ends_a_Follow_at_the_call_before_the_resume_is_queued()
    {
        using var kiln = new KilnHarness();
        FollowMod.Template = kiln.Spawned.AddTemplate("Villager_Kit", TemplateKind.Npc, "Kit villager");
        kiln.Load<FollowMod>(new ModManifest("resume-follow"));
        var ada = kiln.Players.Join("Ada", 76561198000000777, position: new WorldPosition(10, 0, 10));
        kiln.Step(2);
        var spawned = Said(kiln.RunCommand("rt spawn"));
        Assert.StartsWith("spawned ", spawned);
        var villager = uint.Parse(spawned["spawned ".Length..], CultureInfo.InvariantCulture);
        var assigned = Said(kiln.RunCommand($"rt assign {villager}"));
        if (assigned != "assign queued") Assert.Fail(assigned);
        kiln.Step(2);
        Assert.Equal("result True", Said(kiln.RunCommand($"rt result {villager}")));
        Assert.Equal("following True", Said(kiln.RunCommand($"rt follow {villager} {ada.Entity.Value}")));
        Assert.Equal("following after the call False", Said(kiln.RunCommand($"rt resume {villager}")));
        kiln.Step(2);
        Assert.Equal("result True", Said(kiln.RunCommand($"rt result {villager}")));
    }
}
