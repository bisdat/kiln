// SPDX-License-Identifier: MIT
using Xunit;

namespace Kiln.Contract.Tests;

/// <summary>Kiln.Blocks by the contract (no native table of its own; the affix and item-block rows exist from its first
/// use). Affixes have their own tests (Kiln.Blocks.Tests).</summary>
public sealed class BlocksContractTests : ContractTest
{
    [Fact]
    public void The_rows_are_reported_again_under_a_new_registry_binding()
    {
        StartRows(typeof(Blocks).Assembly);
        foreach (var row in (string[])["blocks.weapon_moves", "blocks.animated_held", "blocks.affixes", "blocks.affixes.effects"])
            Assert.NotEqual(CapabilityReason.Unknown, Capabilities.Get(row).Reason);
    }

    // Found by the 16f lab: blocks.npc_body was reported only when Factions ran, so a mod asking Capabilities for it read "no capability
    // of this name" although NpcBodies.Status worked. An NPC body status asks for the rows as an NPC block's definition does.
    [Fact]
    public void Asking_an_NPC_body_status_reports_the_blocks_rows()
    {
        Assert.Equal(CapabilityReason.Unknown, Capabilities.Get("blocks.npc_body").Reason);
        try { NpcBodies.Status("some-mod:tobin"); }
        catch (KilnUnavailableException) { }  // no game here: its dependencies are off
        Assert.NotEqual(CapabilityReason.Unknown, Capabilities.Get("blocks.npc_body").Reason);
    }

    [Fact]
    public void Factions_without_kiln_npcai_throw_unavailable_naming_it()
    {
        AssertUnavailable("kiln.npcai", () => Factions.Fights(Faction.Player, Faction.Fogger));
        AssertUnavailable("kiln.npcai", () => Factions.Between(new EntityId(1), new EntityId(2)));
        Assert.Equal(Relation.Game, Factions.Get(Faction.Player, Faction.Fogger));  // answers without it
        AssertRequires(Method(typeof(Factions), "Between", typeof(EntityId), typeof(EntityId)), "kiln.npcai");
        AssertHas<TickThreadOnlyAttribute>(Method(typeof(Factions), "Between", typeof(EntityId), typeof(EntityId)));
    }
}
