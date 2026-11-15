// SPDX-License-Identifier: MIT
// Equipment sets (chapter 10 §4.2, §5; plan 10-equipment-sets): a player's 20 physical equipment slots changed in one
// inventory transaction. The set itself (EquipmentSet, EquipmentSetScope, EquipmentSetEntry, the selections and
// InventorySnapshotToken) is in EquipDecisions.cs, where 07-cancelable-events declared it for SetEquipmentChange; this
// file has the rest of the model: what a capture, a plan and an apply return, and the argument rules every entry point
// checks (one error rule: a bad argument throws ArgumentException, a refusal of the game or a stale set is a result).
namespace Kiln;

/// <summary>What <c>Items.CaptureSet</c> returned: all 20 equipment slots of a player as one
/// <see cref="EquipmentSet"/> (empty slots as <see cref="EmptyEquipment"/>), scoped to this process, this server world
/// and this life of the player. Since 0.2.0-dev.29.</summary>
/// <param name="Ok">True when the set was read.</param>
/// <param name="Reason">Why not; null when <paramref name="Ok"/>.</param>
/// <param name="Set">The set; null when not <paramref name="Ok"/>.</param>
public sealed record EquipmentSetCaptureResult(bool Ok, string? Reason, EquipmentSet? Set) : IKilnResult;

/// <summary>One step of an <see cref="EquipmentSetPlan"/>: one of the game's equip or unequip calls, in the order the
/// transaction would run them. <see cref="EmptyEquipment"/> is an unequip (also a temporary one that frees a slot for an
/// item moving between two equipment slots). Advisory: Kiln plans again, inside the transaction, from what it checked;
/// a step is never replayed from a plan. Since 0.2.0-dev.29.</summary>
/// <param name="Index">Its place in the order, from 0.</param>
/// <param name="Target">The equipment slot the call works on.</param>
/// <param name="Selection">What goes in (<see cref="EmptyEquipment"/>: what is there comes off).</param>
public sealed record EquipmentSetStep(int Index, EquipmentSlot Target, EquipmentSetSelection Selection);

/// <summary>What one equipment slot held at one moment: the item type, the item entity (none for a plain stack), the
/// inventory slot it sits in and the count. All none / 0 for an empty slot. Since 0.2.0-dev.29.</summary>
/// <param name="Item">The item type; none when empty.</param>
/// <param name="Instance">The item entity; <see cref="ItemInstanceRef.None"/> for a plain stack or an empty slot.</param>
/// <param name="Source">The inventory slot that holds it (an equipment page's slot); none when empty.</param>
/// <param name="Count">How many; 0 when empty.</param>
public sealed record EquipmentSlotValue(ItemId Item, ItemInstanceRef Instance, SlotId Source, int Count);

/// <summary>One equipment slot a committed set changed: what it held before and after the transaction. Since
/// 0.2.0-dev.29.</summary>
/// <param name="Slot">The equipment slot.</param>
/// <param name="Before">Before the transaction.</param>
/// <param name="After">After its commit.</param>
public sealed record EquipmentSlotChange(EquipmentSlot Slot, EquipmentSlotValue Before, EquipmentSlotValue After);

/// <summary>
/// A set checked against a snapshot of the player's inventory (<c>Items.PlanSet</c>): the steps the transaction
/// would run and how many free backpack slots it needs on the way. Apply it with <c>Items.EquipSet</c>
/// before <see cref="ValidThroughStep"/>; anything that changes the player's inventories, equipment or item ownership
/// after the snapshot makes the apply come back <see cref="EquipmentSetStatus.StaleSnapshot"/> with nothing changed.
/// Since 0.2.0-dev.29.
/// </summary>
/// <remarks>A plan holds one of Kiln's snapshots (at most 16 per mod and 256 in all, for 250 steps): dispose a plan you
/// do not apply. Applying it uses the snapshot up (a plan applies once); disposing it afterwards does nothing.
/// <see cref="Steps"/> and <see cref="ValidThroughStep"/> are advisory: Kiln plans again inside the transaction and trusts
/// only its own snapshot.</remarks>
/// <param name="Player">The player.</param>
/// <param name="Set">The set that was planned (a copy).</param>
/// <param name="Snapshot">The snapshot the plan was checked against.</param>
/// <param name="ValidThroughStep">The last server step the snapshot can be applied in (its capture step + 250).</param>
/// <param name="Steps">The calls the transaction would make, in order (a copy).</param>
/// <param name="RequiredFreeSlots">The most free Generic backpack slots the steps need at one time (an unequip puts the
/// item into a free one).</param>
public sealed record EquipmentSetPlan(EntityId Player, EquipmentSet Set, InventorySnapshotToken Snapshot, ulong ValidThroughStep,
    IReadOnlyList<EquipmentSetStep> Steps, int RequiredFreeSlots) : IDisposable
{
    /// <summary>The calls the transaction would make, in order (a copy of the list it was made with).</summary>
    public IReadOnlyList<EquipmentSetStep> Steps { get; init; } = EquipmentLists.Freeze(Steps);

    // Shared by the copies a `with` makes: the snapshot is released or used up once for all of them.
    internal EquipmentSetLease? Lease { get; init; }

    /// <summary>Releases the plan's snapshot when it was not applied; nothing after an apply or a second call.</summary>
    [AnyThread]
    public void Dispose() => Lease?.Release();
}

/// <summary>What <c>Items.PlanSet</c> returned. Since 0.2.0-dev.29.</summary>
/// <param name="Ok">True when the set can be applied now as planned.</param>
/// <param name="Reason">Why not (a stale scope, the snapshot budget, an item that is not there, no room, a form Kiln
/// refuses); null when <paramref name="Ok"/>.</param>
/// <param name="Plan">The plan; null when not <paramref name="Ok"/>.</param>
public sealed record EquipmentSetPlanResult(bool Ok, string? Reason, EquipmentSetPlan? Plan) : IKilnResult;

/// <summary>How an <c>Items.EquipSet</c> ended. Since 0.2.0-dev.29.</summary>
public enum EquipmentSetStatus
{
    /// <summary>Every slot of the set changed, in one committed transaction.</summary>
    Done = 0,
    /// <summary>Every slot already held what the set asks (or the set has no entries): nothing ran, no decision.</summary>
    NoChange = 1,
    /// <summary>A dry run: the transaction ran and was rolled back, and the inventory was checked to be as before.</summary>
    Simulated = 2,
    /// <summary>A <c>PlayerEquippingEvent</c> handler canceled the set: nothing ran.</summary>
    Canceled = 3,
    /// <summary>The set's scope, the snapshot or the player's inventory changed since the set was captured or planned
    /// (another process, another server world, the player left or rejoined, an item moved): nothing ran.</summary>
    StaleSnapshot = 4,
    /// <summary>Refused before or during the transaction (the game refused a step, no room, a form Kiln does not run,
    /// the snapshot budget, too many requests); the transaction was rolled back and checked: nothing changed.</summary>
    Refused = 5,
    /// <summary>The game's commit rolled the transaction back by itself, and the inventory was checked to be as before:
    /// nothing changed.</summary>
    CommitRolledBack = 6,
    /// <summary>Never ran: the server world stopped, the asking mod unloaded first, or the request waited 250 steps.</summary>
    Dropped = 7,
    /// <summary>After the transaction the inventory was neither as asked nor as before (or the game faulted inside it):
    /// something may have changed (<see cref="EquipmentSetResult.MayHaveMutated"/>). Equipment sets are switched off
    /// until the server restarts, and <see cref="EquipmentSetFaultedEvent"/> is posted.</summary>
    InvariantFailure = 8,
    /// <summary>Equipment sets went off after the call was accepted.</summary>
    Unavailable = 9,
}

/// <summary>
/// What <c>Items.EquipSet</c> did. <see cref="Ok"/> only for
/// <see cref="EquipmentSetStatus.Done"/>, <see cref="EquipmentSetStatus.NoChange"/> and
/// <see cref="EquipmentSetStatus.Simulated"/>. <see cref="Changes"/> lists the slots a committed transaction really
/// changed; it is empty for a dry run (the plan's steps say what it would do) and for anything that did not commit.
/// Since 0.2.0-dev.29.
/// </summary>
/// <param name="Status">The outcome.</param>
/// <param name="Reason">Why, for anything but Done, NoChange and Simulated.</param>
/// <param name="Player">The player.</param>
/// <param name="Ticket">kiln.items' ticket of the request (0 when it was refused before it was queued).</param>
/// <param name="Decision">The <c>PlayerEquippingEvent</c> decision of the set; null when none was filed (no change, a
/// refusal before the decision, no decision service).</param>
/// <param name="DryRun">Asked with <see cref="EquipOptions.DryRun"/> (kept on every result, refusals included).</param>
/// <param name="Committed">The game's commit ran and changed the inventory.</param>
/// <param name="MayHaveMutated">The inventory may differ from both the set and the state before
/// (<see cref="EquipmentSetStatus.InvariantFailure"/>).</param>
/// <param name="FailedOperation">The index of the step the game refused (see <see cref="EquipmentSetPlan.Steps"/>);
/// null when no step was refused.</param>
/// <param name="GameError">The game's error code of that step (0x11 refused, 0x15 no item, 0x1f no equipment or no
/// room); 0 none.</param>
/// <param name="Changes">The slots a commit changed (a copy).</param>
public sealed record EquipmentSetResult(EquipmentSetStatus Status, string? Reason, EntityId Player, ulong Ticket, DecisionId? Decision,
    bool DryRun, bool Committed, bool MayHaveMutated, int? FailedOperation, int GameError, IReadOnlyList<EquipmentSlotChange> Changes)
    : IKilnResult
{
    /// <summary>The slots a commit changed (a copy of the list it was made with).</summary>
    public IReadOnlyList<EquipmentSlotChange> Changes { get; init; } = EquipmentLists.Freeze(Changes);

    /// <summary>True for <see cref="EquipmentSetStatus.Done"/>, <see cref="EquipmentSetStatus.NoChange"/> and
    /// <see cref="EquipmentSetStatus.Simulated"/>.</summary>
    public bool Ok => Status is EquipmentSetStatus.Done or EquipmentSetStatus.NoChange or EquipmentSetStatus.Simulated;

    /// <summary>A result for a request that never reached the transaction (nothing changed).</summary>
    internal static EquipmentSetResult NotRun(EquipmentSetStatus status, string reason, EntityId player, bool dryRun, ulong ticket = 0,
        DecisionId? decision = null) =>
        new(status, reason, player, ticket, decision, dryRun, false, false, null, 0, []);
}

/// <summary>
/// A set changed a player's equipment: posted once per committed set (Kiln's <c>Items.EquipSet</c>), on the tick thread
/// in the Pre its result arrives in, before the asking mod's task continues; also when that mod unloaded first. One event
/// for the whole set, with one change per slot that really changed. <see cref="Outcome"/> is
/// <see cref="EquipmentSetStatus.Done"/>, or <see cref="EquipmentSetStatus.InvariantFailure"/> when the transaction left
/// the inventory neither as asked nor as before and these are the changes that could still be read. Not for a dry run,
/// a refusal or a cancel. Not cancelable (it already happened). Capability <c>items.equip_sets</c>. Since
/// 0.2.0-dev.29.
/// </summary>
[ServerOnly, TickThreadOnly]
public sealed class EquipmentSetChangedEvent : KilnEvent, IEntityEvent
{
    /// <summary>Makes the event (Kiln posts it; a mod has no reason to).</summary>
    public EquipmentSetChangedEvent(EntityId player, ulong ticket, EquipmentSetStatus outcome, IEnumerable<EquipmentSlotChange> changes)
    {
        ArgumentNullException.ThrowIfNull(changes);
        Player = player;
        Ticket = ticket;
        Outcome = outcome;
        Changes = Array.AsReadOnly(changes.ToArray());
    }

    /// <summary>The player.</summary>
    public EntityId Player { get; }

    /// <summary>kiln.items' ticket of the set (<see cref="EquipmentSetResult.Ticket"/>).</summary>
    public ulong Ticket { get; }

    /// <summary>Done, or InvariantFailure (see the class).</summary>
    public EquipmentSetStatus Outcome { get; }

    /// <summary>The slots that changed, one each.</summary>
    public IReadOnlyList<EquipmentSlotChange> Changes { get; }

    /// <summary>Whether the event is about <paramref name="entity"/>: the player.</summary>
    public bool Involves(EntityId entity) => !entity.IsNone && entity == Player;
}

/// <summary>
/// A set's transaction left the inventory neither as asked nor as before, or the game faulted inside it
/// (<see cref="EquipmentSetStatus.InvariantFailure"/>): something may have changed, and equipment sets are off until
/// the server restarts. Posted once, on the tick thread, with the set's result; the changes that could still be read
/// come as an <see cref="EquipmentSetChangedEvent"/> as well. Capability <c>items.equip_sets</c>. Since 0.2.0-dev.29.
/// </summary>
[ServerOnly, TickThreadOnly]
public sealed class EquipmentSetFaultedEvent(EquipmentSetResult result) : KilnEvent, IEntityEvent
{
    /// <summary>The set's result (Status InvariantFailure, MayHaveMutated).</summary>
    public EquipmentSetResult Result { get; } = result ?? throw new ArgumentNullException(nameof(result));

    /// <summary>Whether the event is about <paramref name="entity"/>: the player.</summary>
    public bool Involves(EntityId entity) => !entity.IsNone && entity == Result.Player;
}

/// <summary>A plan's snapshot: released once (Dispose) or used up once (an accepted apply), whichever comes first. The
/// release runs on any thread (kiln.items' set_release_snapshot is any-thread).</summary>
internal sealed class EquipmentSetLease(ulong token, ulong owner, Action<ulong, ulong>? release, ulong validThrough = ulong.MaxValue)
{
    private int _state;  // 0 held, 1 released, 2 used

    internal ulong Token => token;
    internal ulong Owner => owner;
    /// <summary>The last step kiln.items keeps the snapshot (it is gone natively after that).</summary>
    internal ulong ValidThrough => validThrough;
    internal bool Held => Volatile.Read(ref _state) == 0;

    /// <summary>Dispose: gives the snapshot back to kiln.items unless it was used or released already.</summary>
    internal void Release()
    {
        if (Interlocked.CompareExchange(ref _state, 1, 0) != 0) return;
        try
        {
            release?.Invoke(owner, token);
        }
        catch
        {
            // a released snapshot also expires by itself (250 steps); Dispose never throws
        }
    }

    /// <summary>An apply takes the snapshot: true when it was still held (and is now used), false when it was released
    /// or used before.</summary>
    internal bool TryUse() => Interlocked.CompareExchange(ref _state, 2, 0) == 0;

    /// <summary>The apply was refused before kiln.items took the snapshot: it is held again.</summary>
    internal void Unuse() => Interlocked.CompareExchange(ref _state, 0, 2);
}

/// <summary>The argument rules of chapter 10 §4.2, shared by every entry point and the session JSON.</summary>
internal static class EquipmentSetRules
{
    /// <summary>At most this many steps (the game's equip and unequip calls) in one set's transaction.</summary>
    internal const int MaxSteps = 128;

    /// <summary>Equipment slots of a player (SlotSelection.equipment): every slot a set can name.</summary>
    internal const int SlotCount = 20;

    /// <summary>Checks a set and returns a copy of its entries (the record's list can be replaced with <c>with</c>).
    /// Throws <see cref="ArgumentException"/> for a null set or scope, a zero scope, a null entry or selection, a slot that
    /// is not an <see cref="EquipmentSlot"/> (the building materials, MainHand), two entries for one slot, two entries
    /// for one item or one stack, and a malformed selection (an item ref without a kiln.items stamp, a stack without an
    /// item, a source slot or a positive count).</summary>
    internal static EquipmentSetEntry[] Check(EquipmentSet? set, string param)
    {
        if (set is null) throw new ArgumentNullException(param);
        CheckScope(set.Scope, param);
        if (set.Entries is null) throw new ArgumentException("the set has no entry list", param);
        var entries = set.Entries.ToArray();
        if (entries.Length > SlotCount) throw new ArgumentException($"a set names at most {SlotCount} slots, not {entries.Length}", param);
        var slots = new HashSet<EquipmentSlot>();
        var items = new HashSet<uint>();
        var sources = new HashSet<ulong>();
        for (var i = 0; i < entries.Length; i++)
        {
            var e = entries[i] ?? throw new ArgumentException($"entry {i} is null", param);
            if (!Enum.IsDefined(e.Slot)) throw new ArgumentException($"entry {i}: {(int)e.Slot} is not an equipment slot (the building materials and the main hand are not)", param);
            if (!slots.Add(e.Slot)) throw new ArgumentException($"entry {i}: slot {e.Slot} is named twice", param);
            switch (e.Selection)
            {
                case null:
                    throw new ArgumentException($"entry {i} ({e.Slot}) has no selection", param);
                case EmptyEquipment:
                    break;
                case InstanceEquipment inst:
                    CheckInstance(inst.Item, $"entry {i} ({e.Slot})", param);
                    if (!items.Add(inst.Item.Entity.Id.Value)) throw new ArgumentException($"entry {i} ({e.Slot}): item entity {inst.Item.Entity.Id} is selected twice", param);
                    break;
                case StackEquipment stack:
                    CheckStack(stack, $"entry {i} ({e.Slot})", param);
                    if (!sources.Add(stack.Source.Value)) throw new ArgumentException($"entry {i} ({e.Slot}): the stack in {stack.Source} is selected twice", param);
                    break;
                default:
                    throw new ArgumentException($"entry {i} ({e.Slot}): {e.Selection.GetType().Name} is not a selection Kiln knows", param);
            }
        }
        return entries;
    }

    /// <summary>A scope with a process session, a server world and a player (all three non-zero).</summary>
    internal static void CheckScope(EquipmentSetScope? scope, string param)
    {
        if (scope is null) throw new ArgumentException("the set has no scope", param);
        if (scope.ProcessSession == Guid.Empty) throw new ArgumentException("the set's scope has no process session", param);
        if (scope.WorldEpoch == 0) throw new ArgumentException("the set's scope has no server world (epoch 0)", param);
        if (scope.Player.Entity == 0) throw new ArgumentException("the set's scope has no player", param);
    }

    // An item instance as kiln.items names one: an entity in the range it stamps, its stamp, the item type.
    private static void CheckInstance(ItemInstanceRef item, string what, string param)
    {
        if (item.IsNone) throw new ArgumentException($"{what}: the item ref is none (use EmptyEquipment for an empty slot)", param);
        if (item.Item.IsNone) throw new ArgumentException($"{what}: the item ref has no item type", param);
        if (!ItemRefs.Stampable(item.Entity.Id.Value))
            throw new ArgumentException($"{what}: {item.Entity.Id} is not an item entity id (kiln.items stamps 1024..2^31-1)", param);
        if (StampOf(item) == 0)
            throw new ArgumentException($"{what}: the item ref carries no kiln.items stamp (take it from Items.Read, Items.Equipped or a capture)", param);
    }

    private static void CheckStack(StackEquipment stack, string what, string param)
    {
        if (stack.Item.IsNone) throw new ArgumentException($"{what}: the stack has no item type", param);
        if (stack.Source.IsNone) throw new ArgumentException($"{what}: the stack has no source slot", param);
        if ((stack.Source.Value >> 40) != 0 || stack.Source.Inventory.IsNone)
            throw new ArgumentException($"{what}: {stack.Source.Value:x} is not an inventory slot", param);
        if (stack.ExpectedCount <= 0) throw new ArgumentException($"{what}: the stack's expected count must be positive, not {stack.ExpectedCount}", param);
    }

    /// <summary>kiln.items' stamp of an item ref (the native u32 the wire carries); 0 when the ref has none.</summary>
    internal static uint StampOf(ItemInstanceRef item)
    {
        var serial = item.Entity.Serial;
        if (IdentitySources.PlaceOf(serial) != EntitySourceOrder.Items) return 0;
        var value = serial & IdentitySources.MaxStampValue;
        return value is > 0 and <= uint.MaxValue ? (uint)value : 0;
    }

    /// <summary>Why a set's scope is not the current one (another process, another server world, another life of the
    /// player), or null when it is. A stale scope is a result (StaleSnapshot), never an exception.</summary>
    internal static string? Stale(EquipmentSetScope scope, Guid session, ulong worldEpoch, EventEntityKey player)
    {
        if (scope.ProcessSession != session) return "the set was made in another process (the server restarted since): sets are kept for one session only";
        if (scope.WorldEpoch != worldEpoch) return $"the set was made in another server world (epoch {scope.WorldEpoch}, now {worldEpoch})";
        if (scope.Player.Entity != player.Entity) return $"the set is player {scope.Player.Entity}'s, not {player.Entity}'s";
        if (scope.Player != player) return $"player {player.Entity} is not the session the set was made for (the player left or rejoined since)";
        return null;
    }
}
