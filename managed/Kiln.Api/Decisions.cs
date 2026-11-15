// SPDX-License-Identifier: MIT
using System.ComponentModel;
using System.Diagnostics.CodeAnalysis;

namespace Kiln;

/// <summary>Names one decision (a held action a mod may allow or refuse) for as long as the process runs:
/// the server world's epoch and the request's number in it. The same id is on the cancelable event, on the typed
/// event of the same action and on the <see cref="DecisionCompletedEvent"/> that ends it. Since 0.2.0-dev.29.</summary>
/// <param name="WorldEpoch">Counts the server worlds of this process, from 1. Not saved.</param>
/// <param name="Request">The request's number, never 0, never reused in this process.</param>
public readonly record struct DecisionId(ulong WorldEpoch, ulong Request);

/// <summary>Which life of an entity id an event is about. Entity ids are reused; this key tells one user of an id
/// from the next: the entity's spawn time where it has one, the player's session for a player, and a serial Kiln
/// gives every life it sees. Sampled before the step's simulation, so it is the entity as it was when the step
/// began. Since 0.2.0-dev.29.</summary>
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
/// decision did not take effect, and why. Since 0.2.0-dev.29.</summary>
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
/// whose health was read, null otherwise. Since 0.2.0-dev.29.</summary>
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
/// action: whatever the handlers of the cancelable event said, this is what came of it. Since 0.2.0-dev.29.</summary>
[ServerOnly, TickThreadOnly]
public sealed class DecisionCompletedEvent(DecisionResult result) : KilnEvent, IEntityEvent
{
    /// <summary>How the decision ended.</summary>
    public DecisionResult Result { get; } = result;

    /// <inheritdoc/>
    public bool Involves(EntityId entity) => !entity.IsNone && (entity == Result.Subject || entity == Result.Actor);
}

/// <summary>What <see cref="Decisions.ReevaluatePickup"/> did. Since 0.2.0-dev.29.</summary>
/// <param name="Ok">True when at least one exposure was reopened.</param>
/// <param name="Reason"><c>NoActiveExposure</c> when the pair had none (no pickup was held for it, or the receiver
/// left the item's range, or its decision is still open); null when <see cref="Ok"/>.</param>
/// <param name="ExposuresReset">How many exposures were reopened.</param>
public sealed record PickupReevaluationResult(bool Ok, string? Reason, int ExposuresReset) : IKilnResult;

/// <summary>Decisions: things the game is about to do that a mod may refuse (a death, a walk-over pickup, an equipment
/// change). The events themselves are in the service that owns them (<c>EntityDyingEvent</c> in Kiln.Combat, ...); each
/// carries a <see cref="DecisionId"/> and ends in one <see cref="DecisionCompletedEvent"/>. Since 0.2.0-dev.29.</summary>
public static class Decisions
{
    private const string LabUrl = "https://github.com/frostdev-ops/kiln/blob/main/docs/kiln/modding/analyzers.md#kilnx001";

    /// <summary>The capability row of a cancelable walk-over pickup.</summary>
    internal const string PickupRow = "events.pickup";

    /// <summary>Asks again about walk-over pickups that were refused (or allowed) for their current exposure: each
    /// exposure of <paramref name="drop"/> by <paramref name="receiver"/> that is still there gets a new exposure epoch,
    /// so the game's next attempt posts a new <c>ItemPickingUpEvent</c>. <see cref="EntityId.None"/> for either means
    /// any. Use it when what made a handler refuse has changed (the receiver earned the right to the item). An
    /// exposure whose decision is still open is left alone. Tick thread, Pre or Post.</summary>
    /// <returns><c>Ok</c> with the count of exposures reopened; <c>Ok = false</c>, reason <c>NoActiveExposure</c>,
    /// when there was none.</returns>
    /// <exception cref="KilnUnavailableException"><c>events.pickup</c> is unavailable on this game build.</exception>
    [ServerOnly, TickThreadOnly, RequiresCapability(PickupRow)]
    public static PickupReevaluationResult ReevaluatePickup(EntityId drop, EntityId receiver)
    {
        Services.RequireTickThread("Decisions.ReevaluatePickup");
        if (!Capabilities.Is(PickupRow)) throw ServiceCaps.Unavailable(PickupRow, "walk-over pickups are not held here");
        var rc = DecisionBroker.ReevaluatePickup(drop, receiver, out var reset);
        return rc switch
        {
            DecisionsNative.Ok when reset > 0 => new PickupReevaluationResult(true, null, reset),
            DecisionsNative.Ok => new PickupReevaluationResult(false, "NoActiveExposure", 0),
            DecisionsNative.WrongThread => throw new InvalidOperationException("Decisions.ReevaluatePickup: not in Pre or Post of the server world"),
            _ => throw new KilnUnavailableException(PickupRow, "kiln.decisions has no walk-over producer here"),
        };
    }

    /// <summary>Lab: where the server world's step is right now. Returns the phase (1 Pre, 2 preparing, 3 the game's
    /// simulation, 4 after the world's jobs, 5 Post, 6 stopping, 0 no world or no step yet), or -1 when this build has
    /// no step clock. Any thread. For lab mods only (<c>KILNX001</c>).</summary>
    /// <param name="worldEpoch">Counts the server worlds of this process, from 1.</param>
    /// <param name="step">The step.</param>
    [Experimental("KILNX001", UrlFormat = LabUrl)]
    [EditorBrowsable(EditorBrowsableState.Never)]
    [AnyThread]
    public static int LabWindow(out ulong worldEpoch, out ulong step)
    {
        worldEpoch = 0;
        step = 0;
        if (DecisionBroker.Window() is not { } now) return -1;
        worldEpoch = now.WorldEpoch;
        step = now.Step;
        return (int)now.Phase;
    }

    /// <summary>Lab: asks kiln.decisions for the identity of <paramref name="entity"/> as its events would name it, and
    /// returns the service's result code: 0 (found), 2 (no such entity, or none with a usable identity), 3 (no
    /// kiln.decisions), 4 (not the world thread in a phase in which the world may be read: also the world thread
    /// inside the simulation). Any thread, to show the refusal. For lab mods only (<c>KILNX001</c>).</summary>
    [Experimental("KILNX001", UrlFormat = LabUrl)]
    [EditorBrowsable(EditorBrowsableState.Never)]
    [AnyThread]
    public static int LabResolveEntity(EntityId entity, out EventEntityKey key, out WorldPosition? position) =>
        DecisionBroker.ResolveEntity(entity, out key, out position);

    /// <summary>Lab, tick thread: files a decision about <paramref name="subject"/> that no game action hangs on, and
    /// answers it as told when it is delivered at the next step: true allows, false refuses, null never answers (the
    /// service releases it after its deadline). Its end comes as a <see cref="DecisionCompletedEvent"/> like any
    /// other. Returns the decision's id, or the default id when kiln.decisions took nothing. For lab mods only
    /// (<c>KILNX001</c>).</summary>
    [Experimental("KILNX001", UrlFormat = LabUrl)]
    [EditorBrowsable(EditorBrowsableState.Never)]
    [ServerOnly, TickThreadOnly]
    public static DecisionId LabDecide(EntityId subject, bool? allow)
    {
        if (subject.IsNone) throw new ArgumentException("a decision is about an entity", nameof(subject));
        return DecisionBroker.LabDecide(subject, allow, out _) ?? default;
    }
}
