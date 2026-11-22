// SPDX-License-Identifier: MIT
// The records of Survivors (Survivors.cs). JSON names as the C# names in camelCase; enums by name.
namespace Kiln;

/// <summary>A survivor: the game's roster id of an NPC (its NpcId, FNV-1a-32 of the NPC template's GUID; the same value as
/// <c>Villager.NpcId</c>). Stable across restarts: store this, never an entity id.</summary>
/// <param name="Value">The NpcId.</param>
public readonly record struct SurvivorId(uint Value)
{
    /// <summary>The id as eight hex digits.</summary>
    public override string ToString() => $"0x{Value:x8}";
}

/// <summary>Who answers for a bed assignment (<see cref="SurvivorAssignmentOptions"/>).</summary>
public enum SurvivorAuthorization
{
    /// <summary>A player in game acts (<see cref="SurvivorAssignmentOptions.Actor"/>): they need the base-edit permission and
    /// every bed touched must lie in a loaded, kept base whose zone rules let them build there.</summary>
    PlayerPermissions = 0,

    /// <summary>The server mod itself acts, with no player: skips only the player and zone checks. Never pick it from a
    /// client's request (chat, RPC) without your own check.</summary>
    TrustedServer = 1,
}

/// <summary>How a bed assignment is authorized.</summary>
/// <param name="Actor">The player acting (PlayerPermissions); <see cref="EntityId.None"/> with TrustedServer.</param>
/// <param name="Authorization">PlayerPermissions (the default) or TrustedServer.</param>
public sealed record SurvivorAssignmentOptions(EntityId Actor,
    SurvivorAuthorization Authorization = SurvivorAuthorization.PlayerPermissions)
{
    /// <summary>TrustedServer with no actor: for server code that is its own authority.</summary>
    public static SurvivorAssignmentOptions Trusted { get; } = new(EntityId.None, SurvivorAuthorization.TrustedServer);
}

/// <summary>Options of <see cref="Survivors.RecruitWorld"/>.</summary>
/// <param name="GiveToolTo">A player in game who gets one summon tool after the unlock; null for none.</param>
public sealed record SurvivorRecruitOptions(Player? GiveToolTo = null);

/// <summary>What a survivor request really wrote (<see cref="SurvivorResult.Effects"/>). RegistryConfirmed is an
/// observation; the others are writes that stay, also when the request ends Partial.</summary>
[Flags]
public enum SurvivorEffects
{
    /// <summary>Nothing.</summary>
    None = 0,
    /// <summary>The survivor's unlock knowledge was written for the world.</summary>
    KnowledgeWritten = 1,
    /// <summary>The game's NPC registry shows it unlocked.</summary>
    RegistryConfirmed = 2,
    /// <summary>One summon tool went into the recipient's inventory.</summary>
    ToolGiven = 4,
    /// <summary>A new NPC entity was created.</summary>
    EntityCreated = 8,
    /// <summary>The NPC was moved and its home set (the game's Reset).</summary>
    HomeMoved = 16,
    /// <summary>The bed relation was written.</summary>
    BedChanged = 32,
}

/// <summary>How a survivor request ended.</summary>
public enum SurvivorCompletion
{
    /// <summary>Everything asked for happened and was read back.</summary>
    Done,
    /// <summary>It already was so; nothing was written.</summary>
    NoChange,
    /// <summary>Refused before anything was written.</summary>
    Refused,
    /// <summary>The 30 s budget ran out before anything was written.</summary>
    TimedOut,
    /// <summary>Something was written (<see cref="SurvivorResult.Effects"/>), then it could not finish. Nothing is taken back.</summary>
    Partial,
    /// <summary>The world stopped; Effects says what was written before.</summary>
    WorldEnded,
    /// <summary>A step Kiln handed to the game (the unlock's grant) gave no answer within the budget and 10 minutes more: it may
    /// still land. Effects lists only what was seen. Read <see cref="Survivors.Find"/> later to know. Since 0.2.0-dev.31.</summary>
    Indeterminate,
}

/// <summary>A roster survivor as the world has it now.</summary>
/// <param name="Id">Its NpcId.</param>
/// <param name="DebugName">The roster's debug name (the NPC template's name), e.g. "NPC_Workshop_Blacksmith_Assistant_01".</param>
/// <param name="Unlocked">Recruited for the world (the game's NPC registry).</param>
/// <param name="SummonTool">The item that summons it; <see cref="ItemId"/> 0 when it has none.</param>
/// <param name="Entity">The live NPC; <see cref="EntityId.None"/> when it is not in the world.</param>
/// <param name="Bed">Its bed (the NPC's side of the relation); None when it has none or is not in the world.</param>
/// <param name="BaseId">The base whose build zone holds its home; null when none.</param>
/// <param name="Home">The point it lives around (set by Summon and by the game's summon tool); null when not in the world.</param>
public sealed record SurvivorInfo(SurvivorId Id, string DebugName, bool Unlocked, ItemId SummonTool, EntityId Entity, EntityId Bed,
    Guid? BaseId, WorldPosition? Home);

/// <summary>The end of a survivor request. <see cref="Ok"/> only for Done and NoChange.</summary>
/// <param name="Ok">Done or NoChange.</param>
/// <param name="Reason">"Code: words" when not Ok (codes in docs/kiln/services/survivors.md).</param>
/// <param name="Before">The survivor when the request was taken.</param>
/// <param name="After">A readback at the end (<paramref name="AfterKnown"/>), else the last confirmed state.</param>
/// <param name="AfterKnown">After is a readback.</param>
/// <param name="Effects">What was really written.</param>
/// <param name="Completion">How it ended.</param>
/// <param name="DisplacedNpc">AssignBed: the NPC that lost the bed; None when none.</param>
/// <param name="Steps">World updates from acceptance to the end.</param>
public sealed record SurvivorResult(bool Ok, string? Reason, SurvivorInfo Before, SurvivorInfo After, bool AfterKnown,
    SurvivorEffects Effects, SurvivorCompletion Completion, EntityId DisplacedNpc, int Steps) : IKilnResult;
