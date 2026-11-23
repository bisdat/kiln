// SPDX-License-Identifier: MIT
using Xunit;

namespace Kiln.TestKit.Tests;

/// <summary>The kit's NPC body rows (Fakes/BootPack): what a test scripts is what NpcBodies.Status and NpcBodyBuildChangedEvent see.</summary>
public sealed class NpcBodiesFakeTests
{
    private static readonly Guid Model = new("aaaaaaaa-0000-0000-0000-000000000001");

    [Fact]
    public void Scripted_rows_answer_Status_and_each_change_is_one_event_at_the_next_step()
    {
        using var kiln = new KilnHarness();
        var seen = new List<NpcBodyBuildChangedEvent>();
        kiln.Run(() => Events.On<NpcBodyBuildChangedEvent>(seen.Add));
        kiln.NpcBodies.Ready("body-mod", "tobin", model: Model, generation: 2);
        kiln.NpcBodies.Failed("body-mod", "bram", "refused: no clearance");
        kiln.NpcBodies.Removed("body-mod", "old");

        var tobin = kiln.Run(() => NpcBodies.Status("body-mod:tobin"));
        Assert.Equal((true, NpcBodyBuildState.Ready, Model, 2UL), (tobin.Ok, tobin.State, tobin.Model, tobin.Generation));
        Assert.Equal((NpcBodyBuildState.Failed, "refused: no clearance"), (kiln.Run(() => NpcBodies.Status("body-mod:bram")) is var b ? (b.State, b.Reason) : default));
        Assert.Equal(NpcBodyBuildState.Removed, kiln.Run(() => NpcBodies.Status("body-mod:old")).State);
        Assert.Equal("NoDefinition", kiln.Run(() => NpcBodies.Status("body-mod:nobody")).Reason);

        kiln.Step(2);
        Assert.Equal(3, seen.Count);
        kiln.NpcBodies.Ready("body-mod", "tobin", model: Model, generation: 3);
        kiln.Step(2);
        var e = Assert.Single(seen.Skip(3));
        Assert.Equal((2UL, 3UL, false), (e.Before.Generation, e.After.Generation, e.ClientSide));
    }

    [Fact]
    public void The_row_switched_off_makes_Status_throw_and_a_new_harness_starts_without_rows()
    {
        using (var kiln = new KilnHarness())
        {
            kiln.NpcBodies.Ready("body-mod", "tobin");
            kiln.Capabilities.Override("blocks.npc_body", false);
            var e = Assert.Throws<KilnUnavailableException>(() => kiln.Run(() => NpcBodies.Status("body-mod:tobin")));
            Assert.Equal("blocks.npc_body", e.Capability);
            kiln.Capabilities.ClearOverride("blocks.npc_body");
            Assert.Equal(NpcBodyBuildState.Ready, kiln.Run(() => NpcBodies.Status("body-mod:tobin")).State);
        }
        using (var kiln = new KilnHarness())
            Assert.Equal("NoDefinition", kiln.Run(() => NpcBodies.Status("body-mod:tobin")).Reason);
    }
}
