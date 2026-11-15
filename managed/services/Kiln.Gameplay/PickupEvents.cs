// SPDX-License-Identifier: MIT
// Cancelable walk-over pickup (07-cancelable-events S5; chapter 07 §4.3): kiln.decisions holds the game's walk-over
// pickup of a drop by a receiver at the entry of its range helper (native/services/decisions/pickup.cpp) and files a
// request of kind 2; in the next Pre Kiln.Api's dispatcher hands it to PickupDecisions, which posts
// ItemPickingUpEvent. A cancel suppresses this sampled exposure (the drop stays, the receiver's inventory is unchanged,
// the game tries again and is refused again); an allow lets the helper run once, at the game's next attempt.
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

[assembly: InternalsVisibleTo("Kiln.Decisions.Tests")]

namespace Kiln;

/// <summary>
/// The game is about to give a receiver an item it walked over (an automatic pickup in range, not an interaction:
/// interaction loot is <see cref="LootEvent"/>). Posted on the tick thread in Pre, one or two steps after the game first
/// tried: until the answer the item stays on the ground. <see cref="CancelableEvent.Cancel"/> refuses it for this
/// exposure: the item stays, the receiver gets nothing, and the game's later attempts are refused without asking again
/// until the receiver leaves the item's range and comes back, the item or its amount changes, or a mod calls
/// <see cref="Decisions.ReevaluatePickup"/>. Not canceling lets the game's next attempt run, once, within two steps of
/// the first; if that attempt gives the receiver nothing (no room), the attempt after it is a new decision. Every
/// decision ends in one <see cref="DecisionCompletedEvent"/> with the same <see cref="Id"/> (its <c>Subject</c> is the
/// item on the ground, its <c>Actor</c> the receiver).
/// </summary>
/// <remarks>
/// <para>Exposures are sampled before and after each step's simulation: leaving and coming back wholly between two
/// samples is not seen. <see cref="Amount"/> is read only (changing it could destroy the whole drop after a partial
/// grant); a partial grant changes the drop's amount, and the rest is a new exposure and a new decision.</para>
/// <para>Which pickups are held: those whose receiver or item a handler's filter names
/// (<see cref="EventFilter.ForEntity(EntityId)"/>); a handler without a filter holds every walk-over pickup.</para>
/// <para>Capability <c>events.pickup</c>: while it is off, <c>Events.On&lt;ItemPickingUpEvent&gt;</c> throws <see cref="KilnUnavailableException"/>
/// naming it. Since 0.2.0-dev.29.</para>
/// </remarks>
[ServerOnly, TickThreadOnly]
public sealed class ItemPickingUpEvent : CancelableEvent, IEntityEvent
{
    // The first subscription (or construction) registers the producer: Events.On checks its capability afterwards.
    static ItemPickingUpEvent() => PickupDecisions.Ensure();

    /// <summary>Makes the event (Kiln posts it; a mod has no reason to).</summary>
    public ItemPickingUpEvent(DecisionId id, EventEntityKey receiver, EventEntityKey ground, ItemId item, int amount, WorldPosition? position,
        ulong exposureEpoch)
    {
        Id = id;
        ReceiverKey = receiver;
        GroundKey = ground;
        Receiver = new EntityId(receiver.Entity);
        GroundEntity = new EntityId(ground.Entity);
        Item = item;
        Amount = amount;
        Position = position;
        ExposureEpoch = exposureEpoch;
    }

    /// <summary>The decision; the same id is on its <see cref="DecisionCompletedEvent"/>.</summary>
    public DecisionId Id { get; }

    /// <summary>Which life of the receiver.</summary>
    public EventEntityKey ReceiverKey { get; }

    /// <summary>Which life of the item on the ground.</summary>
    public EventEntityKey GroundKey { get; }

    /// <summary>The entity that would get the item (a player, or a character standing in for one).</summary>
    public EntityId Receiver { get; }

    /// <summary>The item on the ground (its drop entity).</summary>
    public EntityId GroundEntity { get; }

    /// <summary>The item.</summary>
    public ItemId Item { get; }

    /// <summary>The drop's raw amount (read only).</summary>
    public int Amount { get; }

    /// <summary>Where the drop was when the step began; null when it had no position then.</summary>
    public WorldPosition? Position { get; }

    /// <summary>Kiln's number of this sampled exposure (unique in the process): a new one after the receiver left and
    /// came back, after the item or amount changed, and after <see cref="Decisions.ReevaluatePickup"/>.</summary>
    public ulong ExposureEpoch { get; }

    /// <summary>Whether the event is about <paramref name="entity"/> (the receiver or the item on the ground).</summary>
    public bool Involves(EntityId entity) => !entity.IsNone && (entity == Receiver || entity == GroundEntity);
}

/// <summary>Kind 2 of kiln.decisions for the managed dispatcher: a request becomes an <see cref="ItemPickingUpEvent"/>.</summary>
internal sealed class PickupDecisions : DecisionProducer
{
    /// <summary>The capability row of a cancelable walk-over pickup (a sigdb feature, lowered if its hook is unavailable).</summary>
    public const string Row = "events.pickup";

    internal static readonly PickupDecisions Instance = new();
    private static int _registered;

    public override uint Kind => DecisionsNative.KindWalkover;
    public override string Capability => Row;
    public override IReadOnlyList<Type> EventTypes { get; } = [typeof(ItemPickingUpEvent)];

    /// <summary>Registers the producer once (ItemPickingUpEvent's static constructor; tests).</summary>
    internal static void Ensure()
    {
        if (Interlocked.Exchange(ref _registered, 1) == 0) DecisionBroker.Register(Instance);
    }

    /// <summary>Test hook: the producer goes away (and comes back with the next <see cref="Ensure"/>).</summary>
    internal static void ResetForTests()
    {
        DecisionBroker.Unregister(Instance);
        Volatile.Write(ref _registered, 0);
    }

    public override DecisionVerdict Decide(DecisionRequest request)
    {
        var p = request.Read<PickupRequest>();
        WorldPosition? at = request.HasPosition ? new WorldPosition(p.X, p.Y, p.Z) : null;
        var e = Events.Post(new ItemPickingUpEvent(request.Id, request.Actor, request.Subject, new ItemId(p.Item), (int)p.RawCount, at,
            request.ExposureEpoch));
        return e.Canceled ? DecisionVerdict.Canceled() : DecisionVerdict.Allowed;
    }

    /// <summary><c>KilnPickupRequestV1</c> (kiln_decisions.h), 40 bytes.</summary>
    [StructLayout(LayoutKind.Sequential)]
    internal struct PickupRequest
    {
        public uint Item, RawCount, Feedback, Entry;
        public double X, Y, Z;
    }
}
