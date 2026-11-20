// SPDX-License-Identifier: MIT
// Typed equipment decisions (07-cancelable-events S6; chapter 07 §4.4): a player's equipment input (the game's
// changeEquipmentAction and swapEquipmentSetAction) becomes one PlayerEquippingEvent whose verdict is shared with the
// generic PlayerClientActionEvent of the same input. Kiln.Players' Pre pump is the one reader of kiln.playerstate's held
// actions (ClientActionDecisions in Kiln.Api is the seam): it files one decision of kind 16 (one slot) or 17 (a
// collection swap), posts the generic event, and the broker's dispatch hands the request to the producers below. The
// game is told "cancel" when the generic OR the typed handlers canceled, once.
//
// Also here: the set model of chapter 10 §5 (EquipmentSet and its parts, InventorySnapshotToken), declared for
// SetEquipmentChange as chapter 10 has it; package 10-equipment-sets owns these types from its first commit on, and
// files the kind-18 decisions of a queued set (no producer of kind 18 here).
using System.Collections.ObjectModel;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

[assembly: InternalsVisibleTo("Kiln.Decisions.Tests")]

namespace Kiln;

// ---------------------------------------------------------------- the set model (chapter 10 §5, 10 owns it after) ----

/// <summary>Where an <see cref="EquipmentSet"/> is valid: this process, this server world, this life of the player. A
/// set is never re-resolved in another one. Since 0.2.0-dev.29.</summary>
/// <param name="ProcessSession">The process's session nonce (kiln.window_clock).</param>
/// <param name="WorldEpoch">The server world it was made in.</param>
/// <param name="Player">The player's life.</param>
public sealed record EquipmentSetScope(Guid ProcessSession, ulong WorldEpoch, EventEntityKey Player);

/// <summary>A set of equipment slots and what each should hold. Slots that are not listed stay as they are. Since
/// 0.2.0-dev.29.</summary>
/// <param name="Scope">Where the set is valid.</param>
/// <param name="Entries">One entry per slot (a copy is kept).</param>
public sealed record EquipmentSet(EquipmentSetScope Scope, IReadOnlyList<EquipmentSetEntry> Entries)
{
    /// <summary>One entry per slot (a copy of the list it was made with).</summary>
    public IReadOnlyList<EquipmentSetEntry> Entries { get; init; } = EquipmentLists.Freeze(Entries);
}

/// <summary>What one equipment slot of a set holds. Since 0.2.0-dev.29.</summary>
/// <param name="Slot">The slot.</param>
/// <param name="Selection">Empty, one item instance, or a plain stack.</param>
public sealed record EquipmentSetEntry(EquipmentSlot Slot, EquipmentSetSelection Selection);

/// <summary>What an equipment slot holds or is to hold: <see cref="EmptyEquipment"/>, <see cref="InstanceEquipment"/> (one
/// item with its own entity: gear, a ring) or <see cref="StackEquipment"/> (a plain stack with no item entity: arrows,
/// bait, spell ammunition). Since 0.2.0-dev.29.</summary>
public abstract record EquipmentSetSelection;

/// <summary>Nothing in the slot. Since 0.2.0-dev.29.</summary>
public sealed record EmptyEquipment : EquipmentSetSelection;

/// <summary>One item instance. Since 0.2.0-dev.29.</summary>
/// <param name="Item">The item.</param>
public sealed record InstanceEquipment(ItemInstanceRef Item) : EquipmentSetSelection;

/// <summary>A plain stack (no item entity): the item type, the inventory slot it is in, and how many it held when the
/// decision was made. Since 0.2.0-dev.29.</summary>
/// <param name="Item">The item type.</param>
/// <param name="Source">The inventory slot of the stack.</param>
/// <param name="ExpectedCount">The count read in Pre.</param>
public sealed record StackEquipment(ItemId Item, SlotId Source, int ExpectedCount) : EquipmentSetSelection;

/// <summary>An opaque snapshot of a player's inventory that a set change is checked against (kiln.items keeps the
/// snapshot; this is its name). Since 0.2.0-dev.29.</summary>
/// <param name="Value">The token, never 0 for a real snapshot.</param>
/// <param name="WorldEpoch">The server world it was taken in.</param>
/// <param name="Player">The player.</param>
public readonly record struct InventorySnapshotToken(ulong Value, ulong WorldEpoch, EntityId Player);

// ------------------------------------------------------------------------------------ equipment changes (07 §5) ----

/// <summary>Which kind of equipment change a <see cref="PlayerEquippingEvent"/> is about. Since 0.2.0-dev.29.</summary>
public enum EquipmentChangeKind
{
    /// <summary>Put an item into one equipment slot.</summary>
    Equip = 1,
    /// <summary>Take the item out of one equipment slot.</summary>
    Unequip = 2,
    /// <summary>Swap a whole collection in (the game's own set swap: active equipment or vanity).</summary>
    Collection = 3,
    /// <summary>A Kiln set request (Items.EquipSet; filed by kiln.items).</summary>
    Set = 4,
}

/// <summary>One equipment change: <see cref="SingleEquipmentChange"/>, <see cref="CollectionEquipmentChange"/> or
/// <see cref="SetEquipmentChange"/>. Since 0.2.0-dev.29.</summary>
/// <param name="Kind">Which one.</param>
public abstract record EquipmentChange(EquipmentChangeKind Kind);

/// <summary>One slot: an equip (<see cref="Selection"/> is what goes in) or an unequip (<see cref="Selection"/> is what is
/// in the slot now; <see cref="EmptyEquipment"/> only when it is empty already). Since 0.2.0-dev.29.</summary>
/// <param name="Operation"><see cref="EquipmentChangeKind.Equip"/> or <see cref="EquipmentChangeKind.Unequip"/>.</param>
/// <param name="Slot">The equipment slot.</param>
/// <param name="Selection">The item, read in the Pre of the event.</param>
public sealed record SingleEquipmentChange(EquipmentChangeKind Operation, EquipmentSlot Slot, EquipmentSetSelection Selection)
    : EquipmentChange(Operation);

/// <summary>One stack of a collection as it was read in the Pre of the event. Since 0.2.0-dev.29.</summary>
/// <param name="Source">Its inventory slot.</param>
/// <param name="Item">The item type.</param>
/// <param name="Count">How many.</param>
/// <param name="Instance">The item instance; <see cref="ItemInstanceRef.None"/> for a plain stack.</param>
public sealed record CollectionStack(SlotId Source, ItemId Item, int Count, ItemInstanceRef Instance);

/// <summary>The game's own collection swap (swapEquipmentSetAction): the player swaps the stacks of
/// <see cref="TargetInventory"/> into its active equipment or its vanity. This is the game's per-slot program, not an
/// atomic set: on allow the game checks the inventory again and equips slot by slot. <see cref="Contents"/> is what the
/// collection held in the Pre of the event, for context; it is not a promise. Since 0.2.0-dev.29.</summary>
/// <param name="TargetInventory">The collection's inventory.</param>
/// <param name="Vanity">True: into the vanity slots; false: the active equipment.</param>
/// <param name="Version">The input's version.</param>
/// <param name="Contents">The collection's stacks (a copy is kept).</param>
public sealed record CollectionEquipmentChange(EventEntityKey TargetInventory, bool Vanity, uint Version, IReadOnlyList<CollectionStack> Contents)
    : EquipmentChange(EquipmentChangeKind.Collection)
{
    /// <summary>The collection's stacks (a copy of the list it was made with).</summary>
    public IReadOnlyList<CollectionStack> Contents { get; init; } = EquipmentLists.Freeze(Contents);
}

/// <summary>A Kiln set request (chapter 10): one decision for the whole set, never one per slot. Since 0.2.0-dev.29.</summary>
/// <param name="ItemsTicket">kiln.items' ticket of the request.</param>
/// <param name="Snapshot">The snapshot the set is checked against.</param>
/// <param name="Set">The set.</param>
/// <param name="DryRun">True: only checked, nothing is changed.</param>
public sealed record SetEquipmentChange(ulong ItemsTicket, InventorySnapshotToken Snapshot, EquipmentSet Set, bool DryRun)
    : EquipmentChange(EquipmentChangeKind.Set);

/// <summary>
/// A player is about to change equipment: one slot (<see cref="SingleEquipmentChange"/>), the game's collection swap
/// (<see cref="CollectionEquipmentChange"/>), or a Kiln set (<see cref="SetEquipmentChange"/>). Posted on the tick thread
/// in Pre, one step after the player's input arrived: the game holds the input until the answer. Cancel to refuse it:
/// the game skips the input as it skips an old one, and nothing in the inventory changes.
/// </summary>
/// <remarks>
/// <para>One verdict for one input. The same input also posts <c>PlayerClientActionEvent</c> (Kiln.Players) first, with
/// the same <see cref="Id"/> (its <c>DecisionId</c>); this event starts with what those handlers decided
/// (<see cref="PriorCanceled"/>, <see cref="PriorCanceledBy"/>, and <see cref="CancelableEvent.Canceled"/> set to it).
/// The input is refused when either canceled: a handler here cannot undo a generic veto. Every decision ends in one
/// <see cref="DecisionCompletedEvent"/> with the same id (Subject and Actor: the player).</para>
/// <para>Which inputs: the game's changeEquipmentAction and swapEquipmentSetAction of players a filter names (or of
/// every player for a handler without a filter); only these inputs wait the step, not the player's other actions.
/// Not covered: equipment the game changes by itself (autoequip when an item is made or picked up, its own set swap
/// program called from elsewhere).</para>
/// <para>Capability <c>events.equip</c>: off where kiln.playerstate cannot hold an equipment input alone (an older
/// runtime) or kiln.decisions is not here; then <c>Events.On&lt;PlayerEquippingEvent&gt;</c> throws
/// <see cref="KilnUnavailableException"/> naming it. Since 0.2.0-dev.29.</para>
/// </remarks>
[ServerOnly, TickThreadOnly]
public sealed class PlayerEquippingEvent : CancelableEvent, IEntityEvent
{
    // The first subscription (or construction) registers the producers: Events.On checks their capability afterwards.
    static PlayerEquippingEvent() => EquipDecisions.Ensure();

    /// <summary>Makes the event (Kiln posts it; a mod has no reason to).</summary>
    public PlayerEquippingEvent(DecisionId id, EventEntityKey player, EquipmentChange change, bool priorCanceled, string? priorCanceledBy)
    {
        ArgumentNullException.ThrowIfNull(change);
        Id = id;
        PlayerKey = player;
        Player = new EntityId(player.Entity);
        Change = change;
        PriorCanceled = priorCanceled;
        PriorCanceledBy = priorCanceledBy;
        Canceled = priorCanceled;
        if (priorCanceled) CanceledBy = priorCanceledBy;
    }

    /// <summary>The decision; the same id is on the input's <c>PlayerClientActionEvent</c> and on its
    /// <see cref="DecisionCompletedEvent"/>.</summary>
    public DecisionId Id { get; }

    /// <summary>Which life of the player.</summary>
    public EventEntityKey PlayerKey { get; }

    /// <summary>The player.</summary>
    public EntityId Player { get; }

    /// <summary>What would change.</summary>
    public EquipmentChange Change { get; }

    /// <summary>True when a <c>PlayerClientActionEvent</c> handler of the same input canceled it already.</summary>
    public bool PriorCanceled { get; }

    /// <summary>The mod that canceled it there; null when none did.</summary>
    public string? PriorCanceledBy { get; }

    /// <summary>Whether the event is about <paramref name="entity"/>: the player, the item instance or the inventory a
    /// selection comes from, the collection swapped in or one of its stacks.</summary>
    public bool Involves(EntityId entity) => EquipmentInvolvement.Involves(Player, Change, entity);
}

internal static class EquipmentInvolvement
{
    internal static bool Involves(EntityId player, EquipmentChange change, EntityId entity)
    {
        if (entity.IsNone) return false;
        if (entity == player) return true;
        static bool Selection(EquipmentSetSelection s, EntityId e) => s switch
        {
            InstanceEquipment i => i.Item.Entity.Id == e,
            StackEquipment a => a.Source.Inventory == e,
            _ => false,
        };
        return change switch
        {
            SingleEquipmentChange s => Selection(s.Selection, entity),
            CollectionEquipmentChange c => c.TargetInventory.Entity == entity.Value ||
                                           c.Contents.Any(x => x.Source.Inventory == entity || x.Instance.Entity.Id == entity),
            SetEquipmentChange s => s.Set.Entries.Any(x => Selection(x.Selection, entity)),
            _ => false,
        };
    }
}

internal static class EquipmentLists
{
    internal static IReadOnlyList<T> Freeze<T>(IReadOnlyList<T> list)
    {
        ArgumentNullException.ThrowIfNull(list);
        return list is ReadOnlyCollection<T> frozen && frozen.Count == list.Count ? frozen : Array.AsReadOnly(list.ToArray());
    }
}

// ------------------------------------------------------------------------------------------------- the producers ----

/// <summary>Kinds 16 and 17 of kiln.decisions for the managed dispatcher, and Kiln.Items' side of
/// <see cref="ClientActionDecisions"/>.</summary>
internal sealed unsafe class EquipDecisions : ClientActionDecisions.ITyped
{
    /// <summary>The capability row of the typed equipment verdict.</summary>
    public const string Row = "events.equip";

    internal const string ChangeAction = "changeEquipmentAction", SwapAction = "swapEquipmentSetAction";

    internal static readonly EquipDecisions Instance = new();
    internal static readonly Producer Single = new(DecisionsNative.KindEquipSingle);
    internal static readonly Producer Collection = new(DecisionsNative.KindEquipCollection);
    private static int _registered;
    private static int _describeFaults;

    /// <summary>Registers the producers once (PlayerEquippingEvent's static constructor; tests).</summary>
    internal static void Ensure()
    {
        if (Interlocked.Exchange(ref _registered, 1) != 0) return;
        DecisionBroker.Register(Single);
        DecisionBroker.Register(Collection);
        // after the broker's own entry for the type: the refusal also says when the intake cannot hold selectively
        EventProducers.Register(typeof(PlayerEquippingEvent), Row, WhyOff);
        ClientActionDecisions.SetTyped(Instance);
    }

    /// <summary>Test hook: the producers go away (and come back with the next <see cref="Ensure"/>).</summary>
    internal static void ResetForTests()
    {
        DecisionBroker.Unregister(Single);
        DecisionBroker.Unregister(Collection);
        ClientActionDecisions.SetTyped(null);
        Volatile.Write(ref _registered, 0);
    }

    // Why PlayerEquippingEvent cannot be produced now although events.equip reads on; null: it can. Decided at the call.
    internal static string? WhyOff()
    {
        if (!DecisionBroker.Available) return "kiln.decisions is not here";
        var kinds = DecisionBroker.SupportedKinds;
        if ((kinds >> (int)DecisionsNative.KindEquipSingle & 1) == 0 || (kinds >> (int)DecisionsNative.KindEquipCollection & 1) == 0)
            return "kiln.decisions takes no equipment decisions in this build";
        // Kiln.Players starts at the first service tick, after a mod's OnLoad: until then this is not decided here
        // (the row would read on once it is), and the intake lowers events.equip itself where it cannot hold
        return ClientActionDecisions.Intake?.WhyNotSelective();
    }

    // ---- ClientActionDecisions.ITyped ----

    public bool On => Events.HasSubscribers<PlayerEquippingEvent>() && Capabilities.Is(Row) && WhyOff() is null;

    public uint KindOf(string actionName) => actionName switch
    {
        ChangeAction => DecisionsNative.KindEquipSingle,
        SwapAction => DecisionsNative.KindEquipCollection,
        _ => 0,
    };

    public DecisionInterest Interest() => Single.Interest();

    public byte[]? Payload(ClientActionDecisions.Held held)
    {
        try
        {
            return held.Name switch
            {
                ChangeAction => DescribeSingle(held),
                SwapAction => DescribeCollection(held),
                _ => null,
            };
        }
        catch (Exception e) when (e is KilnUnavailableException or InvalidOperationException or ArgumentException)
        {
            if (++_describeFaults <= 5)
                Backend.Current.Log(0, LogLevel.Warn, $"kiln items: equipment input {held.Name} of {held.Entity} could not be described ({e.Message}); only its generic event decides it");
            return null;
        }
    }

    // changeEquipmentAction (16 bytes): source slot +0, version +8, equipment slot +0xc, equip 0 / unequip 1 +0xd
    // (chapter 10 §2).
    internal static byte[]? DescribeSingle(ClientActionDecisions.Held held)
    {
        var p = held.Payload;
        if (p.Length < 14) return null;
        var source = new SlotId(BitConverter.ToUInt64(p, 0) & 0xFF_FFFF_FFFFUL);
        var version = BitConverter.ToUInt32(p, 8);
        if (Items.SlotOfGame(p[0xc]) is not { } slot) return null;
        if (OperationOf(p[0xd]) is not { } op) return null;
        var player = new EntityId(held.Entity);
        var at = op == EquipmentChangeKind.Equip ? source : EquippedSlot(player, slot);
        var (selection, stack) = SelectionAt(player, at);
        held.Context = new SingleEquipmentChange(op, slot, selection);
        var r = new SingleRequest
        {
            Operation = (uint)op,
            EquipmentSlot = Items.GameSlot(slot),
            ActionIndex = held.Index,
            InputVersion = version,
            Selection = OpOf(slot, selection, stack),
        };
        return Bytes(ref r);
    }

    // keen::ecs::ChangeEquipment by its own names (GameTypes), read once; without type reflection 0 is an equip and 1 an
    // unequip (the values the playerstate lab equips with).
    private static IReadOnlyList<(string Name, long Value)>? _changeKinds;

    internal static EquipmentChangeKind? OperationOf(byte value)
    {
        _changeKinds ??= GameTypes.Available ? GameTypes.Find("keen::ecs::ChangeEquipment")?.EnumValues ?? [] : [];
        foreach (var (name, v) in _changeKinds)
        {
            if (v != value) continue;
            if (name.Contains("unequip", StringComparison.OrdinalIgnoreCase)) return EquipmentChangeKind.Unequip;
            if (name.Contains("equip", StringComparison.OrdinalIgnoreCase)) return EquipmentChangeKind.Equip;
            return null;
        }
        if (_changeKinds.Count > 0) return null;  // a value the game's enum does not name
        return value switch { 0 => EquipmentChangeKind.Equip, 1 => EquipmentChangeKind.Unequip, _ => null };
    }

    // swapEquipmentSetAction (12 bytes): target inventory +0, ActiveEquipment 0 / Vanity 1 +4, version +8.
    internal static byte[]? DescribeCollection(ClientActionDecisions.Held held)
    {
        var p = held.Payload;
        if (p.Length < 12) return null;
        var target = new EntityId(BitConverter.ToUInt32(p, 0));
        var which = BitConverter.ToUInt32(p, 4);
        var version = BitConverter.ToUInt32(p, 8);
        if (target.IsNone || which > 1) return null;
        var key = DecisionBroker.ResolveEntity(target, out var k, out _) == DecisionsNative.Ok ? k : new EventEntityKey(target.Value, false, 0, 0, 0, 0);
        var contents = new List<CollectionStack>();
        if (Items.Read(target) is { } inv)
            foreach (var s in inv.Slots)
                if (!s.IsEmpty && s.Slot.Inventory == target)
                    contents.Add(new CollectionStack(s.Slot, s.Item, s.Count, s.Ref));
        held.Context = new CollectionEquipmentChange(key, which == 1, version, contents);
        var r = new CollectionRequest
        {
            TargetInventory = DecisionsNative.EntityKey.From(key),
            Target = which,
            InputVersion = version,
            ActionIndex = held.Index,
        };
        return Bytes(ref r);
    }

    // The inventory slot an equipment slot points at; none when it is empty (or this build has no such field).
    private static SlotId EquippedSlot(EntityId player, EquipmentSlot slot)
    {
        var sel = PlayerSlots.Selection(player);
        return sel == null ? default : PlayerSlots.Equipment(sel, slot);
    }

    // What an inventory slot of the player holds, read now.
    private static (EquipmentSetSelection Selection, InventorySlot? Stack) SelectionAt(EntityId player, SlotId at)
    {
        if (at.IsNone) return (new EmptyEquipment(), null);
        var stack = (Items.Read(player)?.Slots ?? []).FirstOrDefault(s => s.Slot == at) ??
                    (Items.Read(at.Inventory)?.Slots ?? []).FirstOrDefault(s => s.Slot == at);
        if (stack is null || stack.IsEmpty) return (new EmptyEquipment(), null);
        if (!stack.Ref.IsNone) return (new InstanceEquipment(stack.Ref), stack);
        return (new StackEquipment(stack.Item, at, stack.Count), stack);
    }

    private static SetOp OpOf(EquipmentSlot slot, EquipmentSetSelection selection, InventorySlot? stack)
    {
        var op = new SetOp { Size = (uint)sizeof(SetOp), EquipmentSlot = Items.GameSlot(slot) };
        switch (selection)
        {
            case InstanceEquipment i:
                op.Kind = 1;
                op.ItemEntity = i.Item.Entity.Id.Value;
                op.ItemId = i.Item.Item.Value;
                op.ExpectedCount = (uint)(stack?.Count ?? 1);
                op.InstanceStamp = IdentitySources.PlaceOf(i.Item.Entity.Serial) == EntitySourceOrder.Items ? i.Item.Entity.Serial & IdentitySources.MaxStampValue : 0;
                op.SourceSlot = stack?.Slot.Value ?? 0;
                break;
            case StackEquipment a:
                op.Kind = 2;
                op.ItemId = a.Item.Value;
                op.ExpectedCount = (uint)a.ExpectedCount;
                op.SourceSlot = a.Source.Value;
                break;
        }
        return op;
    }

    private static byte[] Bytes<T>(ref T value) where T : unmanaged
    {
        var b = new byte[sizeof(T)];
        MemoryMarshal.Write(b, in value);
        return b;
    }

    // ---- the dispatch (DecisionBroker.Pump, tick thread, Pre) ----

    internal static DecisionVerdict Decide(DecisionRequest request)
    {
        // only decisions the intake opened carry a change (a lab's, or one of an older pump, has none: allow)
        if (ClientActionDecisions.Of(request.Id) is not { Context: EquipmentChange change } held) return DecisionVerdict.Allowed;
        var e = Events.Post(new PlayerEquippingEvent(request.Id, held.Player, change, held.GenericCanceled, held.GenericCanceledBy));
        held.TypedCanceled = e.Canceled;
        return held.GenericCanceled || e.Canceled ? DecisionVerdict.Canceled() : DecisionVerdict.Allowed;
    }

    internal sealed class Producer(uint kind) : DecisionProducer
    {
        public override uint Kind { get; } = kind;
        public override string Capability => Row;
        public override IReadOnlyList<Type> EventTypes { get; } = [typeof(PlayerEquippingEvent)];
        public override DecisionVerdict Decide(DecisionRequest request) => EquipDecisions.Decide(request);
    }

    /// <summary><c>KilnSetOpV1</c> (kiln_decisions.h), 40 bytes.</summary>
    [StructLayout(LayoutKind.Sequential)]
    internal struct SetOp
    {
        public uint Size;
        public byte EquipmentSlot, Kind;
        public ushort Reserved;
        public uint ItemEntity, ItemId, ExpectedCount, Reserved2;
        public ulong InstanceStamp, SourceSlot;
    }

    /// <summary><c>KilnSingleEquipRequestV1</c>, 56 bytes.</summary>
    [StructLayout(LayoutKind.Sequential)]
    internal struct SingleRequest
    {
        public uint Operation, EquipmentSlot, ActionIndex, InputVersion;
        public SetOp Selection;
    }

    /// <summary><c>KilnCollectionEquipRequestV1</c>, 56 bytes.</summary>
    [StructLayout(LayoutKind.Sequential)]
    internal struct CollectionRequest
    {
        public DecisionsNative.EntityKey TargetInventory;
        public uint Target, InputVersion, ActionIndex, Reserved;
    }
}
