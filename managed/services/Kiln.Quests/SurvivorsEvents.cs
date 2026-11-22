// SPDX-License-Identifier: MIT
// The events of Survivors (kiln.survivors' event ring). Tick thread, after the change. Observe only.
namespace Kiln;

/// <summary>The base of the survivor events: the survivor, its state before and after, the request (0: the game).</summary>
public abstract class SurvivorEvent : KilnEvent
{
    private protected SurvivorEvent(SurvivorId survivor, SurvivorInfo before, SurvivorInfo after, ulong request, ulong step)
    {
        Survivor = survivor;
        Before = before;
        After = after;
        Request = request;
        Step = step;
    }

    /// <summary>The survivor.</summary>
    public SurvivorId Survivor { get; }
    /// <summary>Its state before the change.</summary>
    public SurvivorInfo Before { get; }
    /// <summary>Its state after the change.</summary>
    public SurvivorInfo After { get; }
    /// <summary>The kiln.survivors request that made it; 0 when the game made it (a player, the game's upkeep).</summary>
    public ulong Request { get; }
    /// <summary>The server-world step.</summary>
    public ulong Step { get; }
    /// <summary>True when a Kiln request made the change.</summary>
    public bool ByKiln => Request != 0;
}

/// <summary>A survivor was recruited for the world by <see cref="Survivors.RecruitWorld"/> or <see cref="Survivors.Summon"/>
/// (the NPC registry confirmed the unlock). A rescue by a player raises <c>KnowledgeChangedEvent</c> and
/// <c>RosterUnlockChangedEvent</c>, not this.</summary>
public sealed class SurvivorRecruitedEvent : SurvivorEvent
{
    internal SurvivorRecruitedEvent(SurvivorId s, SurvivorInfo b, SurvivorInfo a, ulong r, ulong st) : base(s, b, a, r, st) { }
    /// <inheritdoc/>
    public override string ToString() => $"survivor {Survivor} ({After.DebugName}) recruited";
}

/// <summary>A survivor's summon tool went into a player's inventory (<see cref="Survivors.RecruitWorld"/>).</summary>
public sealed class SurvivorToolGivenEvent : SurvivorEvent
{
    internal SurvivorToolGivenEvent(SurvivorId s, SurvivorInfo b, SurvivorInfo a, ulong r, ulong st) : base(s, b, a, r, st) { }
    /// <inheritdoc/>
    public override string ToString() => $"survivor {Survivor} ({After.DebugName}): summon tool given";
}

/// <summary>A survivor was brought into a base (<see cref="Survivors.Summon"/>).</summary>
public sealed class SurvivorSummonedEvent : SurvivorEvent
{
    internal SurvivorSummonedEvent(SurvivorId s, SurvivorInfo b, SurvivorInfo a, ulong r, ulong st) : base(s, b, a, r, st) { }
    /// <inheritdoc/>
    public override string ToString() => $"survivor {Survivor} ({After.DebugName}) summoned to {After.Home}";
}

/// <summary>A survivor's bed changed: by <see cref="Survivors.AssignBed"/>/<see cref="Survivors.UnassignBed"/>
/// (<see cref="SurvivorEvent.ByKiln"/>), or by the game (a player's summon tool, a destroyed bed, the game's bed upkeep;
/// seen at most once a second). The survivor that lost its bed to an assignment gets its own event.</summary>
public sealed class SurvivorAssignmentChangedEvent : SurvivorEvent
{
    internal SurvivorAssignmentChangedEvent(SurvivorId s, SurvivorInfo b, SurvivorInfo a, ulong r, ulong st) : base(s, b, a, r, st) { }
    /// <summary>The bed before; None when it had none.</summary>
    public EntityId OldBed => Before.Bed;
    /// <summary>The bed after; None when it has none now.</summary>
    public EntityId NewBed => After.Bed;
    /// <inheritdoc/>
    public override string ToString() => $"survivor {Survivor} ({After.DebugName}): bed {OldBed} -> {NewBed}{(ByKiln ? "" : " (game)")}";
}
