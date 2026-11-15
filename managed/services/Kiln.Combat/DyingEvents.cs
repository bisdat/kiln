// SPDX-License-Identifier: MIT
// Cancelable death (07-cancelable-events S4; chapter 07 §4.2 "Death"): kiln.decisions holds an entity whose health
// reached 0 at the game's death boundary (native/services/decisions/death.cpp) and files a request of kind 1; in the
// next Pre Kiln.Api's dispatcher (DecisionBroker) hands it to DeathDecisions, which posts EntityDyingEvent and says
// what the handlers decided. A canceled death is carried out by the service with kiln.combat's own health setter;
// the write is told to CombatChangedEvent listeners once (CombatChangedBridge), as every Kiln health write is.
using System.Runtime.CompilerServices;

[assembly: InternalsVisibleTo("Kiln.Decisions.Tests")]

namespace Kiln;

/// <summary>
/// An entity's health reached 0 or less and the game is about to let it die: a handler may <see cref="CancelableEvent.Cancel"/>
/// to keep it alive with <see cref="SurvivalHealth"/> (1 unless a handler sets more; clamped to the entity's maximum).
/// Posted on the tick thread in Pre, one or two steps after the lethal step: the game's death program skipped this
/// entity until the answer. This is not damage cancellation: the lethal hit has already changed the health and shown
/// its effects (refuse the hit itself in <see cref="EntityHurtEvent"/>). Every decision ends in one
/// <see cref="DecisionCompletedEvent"/> with the same <see cref="Id"/>: <c>Prevented</c> when the entity was kept alive,
/// <c>Allowed</c> when no handler canceled (the game then lets it die), <c>AlreadyAlive</c> when its health was positive
/// again before the answer (a heal in a handler wins; nothing is written), and a failure status with a reason
/// otherwise (<c>NoPositiveRange</c>, <c>WriteRefused</c>, <c>SourceChanged</c>, <c>StaleEntity</c>,
/// <c>TimeoutReleased</c>, ...). An entity that is already dead (a corpse) is never asked about again.
/// </summary>
/// <remarks>
/// <para>Only deaths from health are held (not a script that kills an actor directly). The entities held are the ones
/// handlers ask for: a handler with <see cref="EventFilter.ForEntity(EntityId)"/> holds those entities only; a handler
/// without a filter (also one for a base class such as <see cref="KilnEvent"/>) holds every death in the world.</para>
/// <para>Capability <c>events.death</c>: while it is off, <c>Events.On&lt;EntityDyingEvent&gt;</c> throws <see cref="KilnUnavailableException"/>
/// naming it. Since 0.2.0-dev.29.</para>
/// </remarks>
[ServerOnly, TickThreadOnly]
public sealed class EntityDyingEvent : CancelableEvent, IEntityEvent
{
    private int _survivalHealth = 1;

    // The first subscription (or construction) registers the producer: Events.On checks its capability afterwards.
    static EntityDyingEvent() => DeathDecisions.Ensure();

    /// <summary>Makes the event (Kiln posts it; a mod has no reason to).</summary>
    public EntityDyingEvent(DecisionId id, EventEntityKey key, int health, int maximumHealth)
    {
        Id = id;
        Key = key;
        Entity = new EntityId(key.Entity);
        Health = health;
        MaximumHealth = maximumHealth;
    }

    /// <summary>The decision; the same id is on its <see cref="DecisionCompletedEvent"/>.</summary>
    public DecisionId Id { get; }

    /// <summary>Which life of the entity id is dying.</summary>
    public EventEntityKey Key { get; }

    /// <summary>The dying entity.</summary>
    public EntityId Entity { get; }

    /// <summary>Its health, read in this Pre (0 or less).</summary>
    public int Health { get; }

    /// <summary>Its maximum health, read in this Pre (0 when it could not be read).</summary>
    public int MaximumHealth { get; }

    /// <summary>The health a canceled death leaves the entity with: positive, 1 by default, clamped to the entity's
    /// maximum when it is written.</summary>
    /// <exception cref="ArgumentOutOfRangeException">0 or less.</exception>
    public int SurvivalHealth
    {
        get => _survivalHealth;
        set => _survivalHealth = value > 0 ? value : throw new ArgumentOutOfRangeException(nameof(value), "survival health must be positive");
    }

    /// <summary>Whether the event is about <paramref name="entity"/>.</summary>
    public bool Involves(EntityId entity) => !entity.IsNone && entity == Entity;
}

/// <summary>Kind 1 of kiln.decisions for the managed dispatcher: a request becomes an <see cref="EntityDyingEvent"/>;
/// a canceled one is answered with its survival health, and the health it then has is told to CombatChangedEvent
/// listeners once.</summary>
internal sealed class DeathDecisions : DecisionProducer
{
    /// <summary>The capability row of a cancelable death (a sigdb feature, lowered if its hook is unavailable).</summary>
    public const string Row = "events.death";

    internal static readonly DeathDecisions Instance = new();
    private static int _registered;

    // What Answering read, for Answered (tick thread only).
    private readonly Dictionary<DecisionId, (int Before, string? CanceledBy)> _writing = [];

    public override uint Kind => DecisionsNative.KindDeath;
    public override string Capability => Row;
    public override IReadOnlyList<Type> EventTypes { get; } = [typeof(EntityDyingEvent)];

    /// <summary>Registers the producer once (EntityDyingEvent's static constructor; tests).</summary>
    internal static void Ensure()
    {
        if (Interlocked.Exchange(ref _registered, 1) == 0) DecisionBroker.Register(Instance);
    }

    /// <summary>Test hook: the producer goes away (and comes back with the next <see cref="Ensure"/>).</summary>
    internal static void ResetForTests()
    {
        DecisionBroker.Unregister(Instance);
        Instance._writing.Clear();
        Volatile.Write(ref _registered, 0);
    }

    public override DecisionVerdict Decide(DecisionRequest request)
    {
        var entity = new EntityId(request.Subject.Entity);
        var health = request.Read<DeathRequest>().Health;
        var maximum = 0;
        if (Live(entity) is { } now)
        {
            // healed before the handlers ran (an earlier Pre subscriber, another mod): no stale dying event; the
            // service answers AlreadyAlive and writes nothing
            if (now.Current > 0) return DecisionVerdict.Allowed;
            health = now.Current;
            maximum = now.Max;
        }
        var e = Events.Post(new EntityDyingEvent(request.Id, request.Subject, health, maximum));
        if (!e.Canceled) return DecisionVerdict.Allowed;
        _writing[request.Id] = (int.MinValue, e.CanceledBy);
        return DecisionVerdict.Canceled(e.SurvivalHealth);
    }

    public override void Answering(DecisionRequest request, DecisionVerdict verdict)
    {
        if (verdict.Allow || !_writing.TryGetValue(request.Id, out var w)) return;
        // the full health right before the service writes (a handler may have healed it)
        _writing[request.Id] = (Live(new EntityId(request.Subject.Entity))?.Current ?? int.MinValue, w.CanceledBy);
    }

    public override void Answered(DecisionRequest request, DecisionVerdict verdict, DecisionReceipt receipt)
    {
        if (!_writing.Remove(request.Id, out var w) || w.Before == int.MinValue) return;
        var entity = new EntityId(request.Subject.Entity);
        if (Live(entity) is { } after) CombatChangedBridge.RecordDecisionWrite(request.Id, entity, w.Before, after.Current, w.CanceledBy);
    }

    public override void Completed(DecisionResult result) => _writing.Remove(result.Id);

    // Health now, without starting Kiln.Combat's hit listener; null without kiln.combat or without a Health.
    private static unsafe Health? Live(EntityId entity)
    {
        var t = CombatNative.T;
        if (t == null) return null;
        CombatNative.Health h = default;
        h.Size = (uint)sizeof(CombatNative.Health);
        if (t->GetHealth(entity.Value, &h) != CombatNative.Ok) return null;
        return new Health(h.Current, h.Max, h.Dead != 0, h.HasActor != 0, h.Invulnerable != 0);
    }

    /// <summary><c>KilnDeathRequestV1</c> (kiln_decisions.h), 24 bytes.</summary>
    [System.Runtime.InteropServices.StructLayout(System.Runtime.InteropServices.LayoutKind.Sequential)]
    internal struct DeathRequest
    {
        public int Health, MaximumHealth;
        public ulong EffectiveActorStates;
        public uint SuggestedSurvivalHealth, Reserved;
    }
}

/// <summary>Health writes Kiln makes for a decision (a canceled death's survival health), told to
/// <see cref="CombatChangedEvent"/> listeners like every other Kiln health write: once per decision, origin
/// <c>kiln.decisions</c>, never by calling the setter a second time.</summary>
internal static class CombatChangedBridge
{
    /// <summary>The <see cref="CombatChangedEvent.Origin"/> of a decision's write.</summary>
    public const string Origin = "kiln.decisions";

    private const int Memory = 4096;
    private static readonly HashSet<DecisionId> Recorded = [];
    private static readonly Queue<DecisionId> Order = new();

    /// <summary>Posts <see cref="CombatChangedEvent"/> (<c>Health</c>, <paramref name="before"/> to <paramref name="after"/>)
    /// for the write of decision <paramref name="id"/>, once; nothing when the value did not change.
    /// <paramref name="canceledBy"/> is the mod whose handler canceled: kept with the decision for the component
    /// watches' provenance (S7), not part of the event. Tick thread. Returns whether an event was posted.</summary>
    internal static bool RecordDecisionWrite(DecisionId id, EntityId entity, int before, int after, string? canceledBy)
    {
        if (before == after || !Events.HasSubscribers<CombatChangedEvent>()) return false;
        lock (Recorded)
        {
            if (!Recorded.Add(id)) return false;
            Order.Enqueue(id);
            while (Order.Count > Memory) Recorded.Remove(Order.Dequeue());
        }
        Combat.Changed(entity, "Health", before, after, Origin);
        return true;
    }
}
