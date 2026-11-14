// SPDX-License-Identifier: MIT
namespace Kiln;

/// <summary>Names one decision (a held action a mod may allow or refuse) for as long as the process runs:
/// the server world's epoch and the request's number in it. The same id is on the cancelable event, on the typed
/// event of the same action and on the <see cref="DecisionCompletedEvent"/> that ends it. Since 0.2.0-dev.28.</summary>
/// <param name="WorldEpoch">Counts the server worlds of this process, from 1. Not saved.</param>
/// <param name="Request">The request's number, never 0, never reused in this process.</param>
public readonly record struct DecisionId(ulong WorldEpoch, ulong Request);

/// <summary>Which life of an entity id an event is about. Entity ids are reused; this key tells one user of an id
/// from the next: the entity's spawn time where it has one, the player's session for a player, and a serial Kiln
/// gives every life it sees. Sampled before the step's simulation, so it is the entity as it was when the step
/// began. Since 0.2.0-dev.28.</summary>
/// <param name="Entity">The entity id.</param>
/// <param name="HasSpawnTime">False for a player that is told apart by its session alone; then
/// <paramref name="SpawnTime"/> is 0 and means nothing.</param>
/// <param name="SpawnTime">The entity's spawn time as the game stores it.</param>
/// <param name="LifeSerial">Kiln's serial of this life, unique in this process. Not saved, and not
/// <see cref="EntityRef.Serial"/>.</param>
/// <param name="PlayerHandle">A player's connection handle; 0 for anything else.</param>
/// <param name="JoinedStep">The step at which Kiln saw the player join; 0 for anything else.</param>
public readonly record struct EventEntityKey(uint Entity, bool HasSpawnTime, ulong SpawnTime, ulong LifeSerial, ulong PlayerHandle, ulong JoinedStep);

/// <summary>How a decision ended. <see cref="Allowed"/>, <see cref="Prevented"/> and <see cref="AlreadyAlive"/> are
/// the outcomes a handler asked for or can live with (<see cref="DecisionResult.Ok"/>); every other one says that the
/// decision did not take effect, and why. Since 0.2.0-dev.28.</summary>
public enum DecisionTerminal
{
    /// <summary>No handler canceled: the action was released to the game (which may still not carry it out).</summary>
    Allowed = 1,
    /// <summary>A handler canceled and the action did not happen.</summary>
    Prevented = 2,
    /// <summary>A canceled death whose entity had positive health again before the answer: nothing was written.</summary>
    AlreadyAlive = 3,
    /// <summary>The entity was gone, or its id belonged to another life, before the answer.</summary>
    StaleEntity = 4,
    /// <summary>A canceled death of an entity whose maximum health is not positive: it cannot be kept alive.</summary>
    NoPositiveRange = 5,
    /// <summary>The game refused the write that would have carried out the cancel.</summary>
    WriteRefused = 6,
    /// <summary>No answer arrived in time; the action was released.</summary>
    TimeoutReleased = 7,
    /// <summary>The capability or the listener went away while the action was held; it was released.</summary>
    CapabilityReleased = 8,
    /// <summary>What was held changed under the hold (the actor is already dead, the input was replaced).</summary>
    SourceChanged = 9,
    /// <summary>Too many decisions were open; this action was never held.</summary>
    QueueDropped = 10,
    /// <summary>The world stopped.</summary>
    WorldEnded = 11,
}

/// <summary>The end of one decision. <see cref="Ok"/> is true for <see cref="DecisionTerminal.Allowed"/>,
/// <see cref="DecisionTerminal.Prevented"/> and <see cref="DecisionTerminal.AlreadyAlive"/>; otherwise
/// <see cref="Reason"/> says why the decision did not take effect. The health fields are set for a death decision
/// whose health was read, null otherwise. Since 0.2.0-dev.28.</summary>
/// <param name="Id">The decision.</param>
/// <param name="Status">How it ended.</param>
/// <param name="Subject">The entity the action was about (the dying entity, the item on the ground, the player).</param>
/// <param name="Actor">The entity that acted (the player picking up), or <see cref="EntityId.None"/>.</param>
/// <param name="RequestedHealth">The health a canceling handler asked for.</param>
/// <param name="HealthBefore">The health read right before the answer was carried out.</param>
/// <param name="AppliedHealth">The health read back afterwards.</param>
/// <param name="Reason">Why, when not <see cref="Ok"/>.</param>
public sealed record DecisionResult(DecisionId Id, DecisionTerminal Status, EntityId Subject, EntityId Actor, int? RequestedHealth,
    int? HealthBefore, int? AppliedHealth, string? Reason) : IKilnResult
{
    /// <inheritdoc/>
    public bool Ok => Status is DecisionTerminal.Allowed or DecisionTerminal.Prevented or DecisionTerminal.AlreadyAlive;
}

/// <summary>Posted once for every decision when it has ended, in the Pre of a later step than the one that held the
/// action: whatever the handlers of the cancelable event said, this is what came of it. Since 0.2.0-dev.28.</summary>
[ServerOnly, TickThreadOnly]
public sealed class DecisionCompletedEvent(DecisionResult result) : KilnEvent, IEntityEvent
{
    /// <summary>How the decision ended.</summary>
    public DecisionResult Result { get; } = result;

    /// <inheritdoc/>
    public bool Involves(EntityId entity) => !entity.IsNone && (entity == Result.Subject || entity == Result.Actor);
}
