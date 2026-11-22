// SPDX-License-Identifier: MIT
// Survivors without a game: argument rules, the native layout mirror, and how native states and results become records.
using Kiln;
using Xunit;

namespace Kiln.QuestTests;

public unsafe class SurvivorsTests
{
    [Fact]
    public void Layout_matches_the_header()
    {
        Assert.True(SurvivorsNative.LayoutMatches);
        Assert.Equal(88, sizeof(SurvivorsNative.Request));
        Assert.Equal(168, sizeof(SurvivorsNative.State));
        Assert.Equal(544, sizeof(SurvivorsNative.Result));
        Assert.Equal(8 + 11 * sizeof(nint), sizeof(SurvivorsNative.Table));
    }

    [Fact]
    public void Survivor_zero_is_a_bad_argument()
    {
        Assert.Throws<ArgumentException>(() => Survivors.NewRequest(new SurvivorId(0), SurvivorsNative.OpRecruit));
        var r = Survivors.NewRequest(new SurvivorId(0x672da6c9), SurvivorsNative.OpAssignBed);
        Assert.Equal(88u, r.Size);
        Assert.Equal(0x672da6c9u, r.NpcId);
    }

    [Fact]
    public void Authorization_rules_are_argument_errors()
    {
        var r = Survivors.NewRequest(new SurvivorId(1), SurvivorsNative.OpAssignBed);
        Assert.Throws<ArgumentException>(() => Survivors.Authorize(ref r, new SurvivorAssignmentOptions(EntityId.None)));
        Assert.Throws<ArgumentException>(() =>
            Survivors.Authorize(ref r, new SurvivorAssignmentOptions(new EntityId(3), SurvivorAuthorization.TrustedServer)));
        Assert.Throws<ArgumentException>(() => Survivors.Authorize(ref r, new SurvivorAssignmentOptions(new EntityId(3), (SurvivorAuthorization)7)));
        Survivors.Authorize(ref r, new SurvivorAssignmentOptions(new EntityId(3)));
        Assert.Equal(0u, r.Authorization);
        Assert.Equal(3u, r.Actor);
        Survivors.Authorize(ref r, SurvivorAssignmentOptions.Trusted);
        Assert.Equal(1u, r.Authorization);
        Assert.Equal(0u, r.Actor);
    }

    private static SurvivorsNative.State State(uint flags, uint unlocked = 1)
    {
        var s = new SurvivorsNative.State
        {
            Size = (uint)sizeof(SurvivorsNative.State), NpcId = 0x672da6c9, Entity = 2034, Bed = 2035, SummonItem = 0x171d1f69,
            Unlocked = unlocked, Flags = flags, Home = new SurvivorsNative.Vec3 { X = 3802, Y = 835, Z = 1500 },
        };
        var name = "NPC_Workshop_Blacksmith_Assistant_01"u8;
        for (var i = 0; i < name.Length; i++) s.Name[i] = name[i];
        SurvivorsNative.WriteGuid(new Guid("0b1c2d3e-4f50-6172-8394-a5b6c7d8e9f0"), s.BaseId);
        return s;
    }

    [Fact]
    public void A_live_survivor_reads_whole()
    {
        var info = SurvivorsNative.Info(State(SurvivorsNative.StateInWorld | SurvivorsNative.StateHasHome | SurvivorsNative.StateHasBase));
        Assert.Equal(new SurvivorId(0x672da6c9), info.Id);
        Assert.Equal("NPC_Workshop_Blacksmith_Assistant_01", info.DebugName);
        Assert.True(info.Unlocked);
        Assert.Equal(new ItemId(0x171d1f69), info.SummonTool);
        Assert.Equal(new EntityId(2034), info.Entity);
        Assert.Equal(new EntityId(2035), info.Bed);
        Assert.Equal(new Guid("0b1c2d3e-4f50-6172-8394-a5b6c7d8e9f0"), info.BaseId);
        Assert.Equal(new WorldPosition(3802, 835, 1500), info.Home);
    }

    [Fact]
    public void A_survivor_not_in_the_world_has_no_entity_bed_home_or_base()
    {
        var info = SurvivorsNative.Info(State(0, unlocked: 0xff));
        Assert.False(info.Unlocked);  // unknown is not unlocked
        Assert.Equal(EntityId.None, info.Entity);
        Assert.Equal(EntityId.None, info.Bed);
        Assert.Null(info.BaseId);
        Assert.Null(info.Home);
    }

    [Theory]
    [InlineData(0u, true, SurvivorCompletion.Done)]
    [InlineData(1u, true, SurvivorCompletion.NoChange)]
    [InlineData(2u, false, SurvivorCompletion.Refused)]
    [InlineData(3u, false, SurvivorCompletion.TimedOut)]
    [InlineData(4u, false, SurvivorCompletion.Partial)]
    [InlineData(5u, false, SurvivorCompletion.WorldEnded)]
    [InlineData(6u, false, SurvivorCompletion.Indeterminate)]
    public void Ok_only_for_done_and_no_change(uint status, bool ok, SurvivorCompletion completion)
    {
        var r = new SurvivorsNative.Result { Size = (uint)sizeof(SurvivorsNative.Result), Status = status, Effects = 1 | 2, Steps = 7 };
        r.Before = State(0, 0);
        r.After = State(SurvivorsNative.StateInWorld);
        var reason = "RegistryConfirmationTimeout: the NPC registry did not follow the unlock"u8;
        if (!ok) for (var i = 0; i < reason.Length; i++) r.Reason[i] = reason[i];
        var res = SurvivorsNative.ToResult(r);
        Assert.Equal(ok, res.Ok);
        Assert.Equal(completion, res.Completion);
        Assert.Equal(ok ? null : "RegistryConfirmationTimeout: the NPC registry did not follow the unlock", res.Reason);
        Assert.Equal(SurvivorEffects.KnowledgeWritten | SurvivorEffects.RegistryConfirmed, res.Effects);
        Assert.Equal(7, res.Steps);
        Assert.False(res.Before.Unlocked);
        Assert.Equal(EntityId.None, res.DisplacedNpc);
        IKilnResult k = res;
        Assert.Equal(ok, k.Ok);
    }

    [Fact]
    public void A_failure_without_a_reason_names_its_completion()
    {
        var r = new SurvivorsNative.Result { Size = (uint)sizeof(SurvivorsNative.Result), Status = 3, DisplacedEntity = 2050 };
        var res = SurvivorsNative.ToResult(r);
        Assert.Equal("TimedOut", res.Reason);
        Assert.Equal(new EntityId(2050), res.DisplacedNpc);
    }

    [Fact]
    public void Survivor_id_prints_as_hex() => Assert.Equal("0x672da6c9", new SurvivorId(0x672da6c9).ToString());
}
