// SPDX-License-Identifier: MIT
// The typed equipment verdict (plan 07 S6, chapter 07 §4.4): Kiln.Items' producers of kinds 16 and 17
// (EquipDecisions, PlayerEquippingEvent) and the intake's two steps in Kiln.Api (ClientActionDecisions.Begin, Finish),
// which Kiln.Players' Pre pump runs for every held client action, on the test kit's kiln.decisions and kiln.items. The
// game's half (kiln.playerstate holding the input, its selective lease) is native: the lease has its host test
// (native/services/players/tests/client_leases_test.cpp), the hold its lab rows. Here a held changeEquipmentAction or
// swapEquipmentSetAction is handed to the intake as kiln.playerstate lists it: one decision per input, the generic vote
// first and the typed one after it, cancel = generic OR typed, the event's fields, involvement, and the refusal by name.
using Kiln;
using Kiln.TestKit;
using Xunit;
using N = Kiln.DecisionsNative;

namespace Kiln.DecisionsTests;

public sealed class EquipDecisionsTests : IDisposable
{
    private readonly List<IDisposable> _subs = [];
    private readonly List<DecisionResult> _completed = [];

    public EquipDecisionsTests()
    {
        FakeTables.Features([EquipDecisions.Row]);
        EquipDecisions.Ensure();
    }

    public void Dispose()
    {
        foreach (var s in _subs) s.Dispose();
        EquipDecisions.ResetForTests();
        ClientActionDecisions.ResetForTests();
        FakeTables.Features([EquipDecisions.Row]);
    }

    private sealed class FakeIntake(string? whyNot) : ClientActionDecisions.IIntake
    {
        public int Starts;
        public void Start() => Starts++;
        public string? WhyNotSelective() => whyNot;
    }

    private KilnHarness Started(out EntityId player, out ItemId arrows, out FakeInventory inventory)
    {
        var kiln = new KilnHarness();
        arrows = kiln.Items.AddItem("Ammo_T1_Arrow", 99, ItemCategory.Ammunition);
        kiln.Step(2);
        player = kiln.Entities.Create(new WorldPosition(1, 2, 3));
        inventory = kiln.Items.AddContainer(player, 2);
        inventory.Put(arrows, 40);
        // kiln.playerstate's intake as Kiln.Players offers it where the table holds selectively (the kit has no
        // kiln.playerstate: Kiln.Players' own intake, which the harness's first tick may have started, says so)
        ClientActionDecisions.SetIntake(new FakeIntake(null));
        _subs.Add(Events.On<DecisionCompletedEvent>(e => _completed.Add(e.Result)));
        return kiln;
    }

    // changeEquipmentAction as the client sends it: source slot +0, version +8, equipment slot +0xc, equip 0 / unequip 1 +0xd
    private static byte[] Change(SlotId source, uint version, byte gameSlot, byte type)
    {
        var b = new byte[16];
        BitConverter.TryWriteBytes(b.AsSpan(0, 8), source.Value);
        BitConverter.TryWriteBytes(b.AsSpan(8, 4), version);
        b[0xc] = gameSlot;
        b[0xd] = type;
        return b;
    }

    // swapEquipmentSetAction: target inventory +0, ActiveEquipment 0 / Vanity 1 +4, version +8
    private static byte[] Swap(EntityId target, uint which, uint version)
    {
        var b = new byte[12];
        BitConverter.TryWriteBytes(b.AsSpan(0, 4), target.Value);
        BitConverter.TryWriteBytes(b.AsSpan(4, 4), which);
        BitConverter.TryWriteBytes(b.AsSpan(8, 4), version);
        return b;
    }

    // What Kiln.Players' Pre pump does for one held input: Begin, the generic vote, Finish. Returns the decision and the
    // answer the game gets (true: cancel); id default when no decision was filed.
    private static (DecisionId Id, bool Cancel, bool Filed) Held(KilnHarness kiln, EntityId player, string name, byte[] payload, bool genericCancel = false,
        string? genericBy = null) => kiln.Run(() =>
    {
        var held = ClientActionDecisions.Begin(ClientActionDecisions.Typed, player.Value, 7, 1, name, payload, out var id);
        if (held is null) return (default(DecisionId), genericCancel, false);
        held.GenericCanceled = genericCancel;
        held.GenericCanceledBy = genericBy;
        var verdict = Assert.Single(ClientActionDecisions.Finish(kiln.CurrentStep, [(id, held)]));
        Assert.Same(held, verdict.Held);
        Assert.Equal(0, ClientActionDecisions.OpenCount);
        return (id, verdict.Cancel, true);
    });

    private DecisionResult Single(DecisionId id) => Assert.Single(_completed, r => r.Id == id);

    [Fact]
    public void A_typed_veto_refuses_the_input_once_and_the_event_describes_the_stack()
    {
        using var kiln = Started(out var p, out var arrows, out var inv);
        var seen = new List<PlayerEquippingEvent>();
        _subs.Add(Events.On<PlayerEquippingEvent>(e =>
        {
            seen.Add(e);
            e.Cancel();
        }, EventFilter.ForEntity(p)));
        kiln.Step();
        Assert.True(ClientActionDecisions.Typed!.On);
        var source = inv.Slots.First(s => s.Item == arrows).Slot;
        var (id, cancel, filed) = Held(kiln, p, EquipDecisions.ChangeAction, Change(source, 5, 14, 0));  // 14: keen::EquipmentSlot.Arrow
        Assert.True(filed && cancel);
        var e = Assert.Single(seen);
        Assert.Equal((id, p, false, (string?)null), (e.Id, e.Player, e.PriorCanceled, e.PriorCanceledBy));
        Assert.Equal(p.Value, e.PlayerKey.Entity);
        var single = Assert.IsType<SingleEquipmentChange>(e.Change);
        Assert.Equal((EquipmentChangeKind.Equip, EquipmentChangeKind.Equip, EquipmentSlot.Arrow), (single.Kind, single.Operation, single.Slot));
        // a plain stack: ItemId, the slot it is in and its count, never ItemInstanceRef.None as an identity
        Assert.Equal(new StackEquipment(arrows, source, 40), single.Selection);
        Assert.True(e.Involves(p) && e.Involves(source.Inventory) && !e.Involves(EntityId.None) && !e.Involves(new EntityId(4242)));
        var done = Single(id);
        Assert.Equal((DecisionTerminal.Prevented, true, p, p), (done.Status, done.Ok, done.Subject, done.Actor));
        Assert.False(Assert.Single(kiln.Decisions.Model.Answers).Allow);
    }

    [Fact]
    public void A_generic_veto_stays_a_veto_whatever_the_typed_handlers_do()
    {
        using var kiln = Started(out var p, out var arrows, out var inv);
        var seen = new List<PlayerEquippingEvent>();
        // a typed handler that sees canceled events and un-cancels: it cannot undo the generic phase's veto
        var uncancel = Events.On<PlayerEquippingEvent>(e =>
        {
            seen.Add(e);
            e.Canceled = false;
        }, receiveCanceled: true);
        _subs.Add(uncancel);
        kiln.Step();
        var source = inv.Slots.First(s => s.Item == arrows).Slot;
        var (id, cancel, filed) = Held(kiln, p, EquipDecisions.ChangeAction, Change(source, 6, 14, 0), genericCancel: true, genericBy: "mod-a");
        Assert.True(filed && cancel);
        var e = Assert.Single(seen);
        Assert.True(e.PriorCanceled);
        Assert.Equal("mod-a", e.PriorCanceledBy);
        Assert.False(e.Canceled);  // the typed phase's own vote
        Assert.Equal(DecisionTerminal.Prevented, Single(id).Status);
        // a typed handler that does not take canceled events never sees a vetoed input
        uncancel.Dispose();
        var quiet = new List<PlayerEquippingEvent>();
        _subs.Add(Events.On<PlayerEquippingEvent>(quiet.Add));
        kiln.Step();
        var (id2, cancel2, _) = Held(kiln, p, EquipDecisions.ChangeAction, Change(source, 7, 14, 0), genericCancel: true, genericBy: "mod-a");
        Assert.True(cancel2);
        Assert.Empty(quiet);
        Assert.Equal(DecisionTerminal.Prevented, Single(id2).Status);
    }

    [Fact]
    public void Neither_vote_cancels_so_the_input_runs_and_completes_Allowed()
    {
        using var kiln = Started(out var p, out var arrows, out var inv);
        var seen = 0;
        _subs.Add(Events.On<PlayerEquippingEvent>(_ => seen++));
        kiln.Step();
        var source = inv.Slots.First(s => s.Item == arrows).Slot;
        var (id, cancel, filed) = Held(kiln, p, EquipDecisions.ChangeAction, Change(source, 8, 14, 0));
        Assert.True(filed);
        Assert.False(cancel);
        Assert.Equal(1, seen);
        Assert.Equal((DecisionTerminal.Allowed, true), (Single(id).Status, Single(id).Ok));
        Assert.True(Assert.Single(kiln.Decisions.Model.Answers).Allow);
    }

    [Fact]
    public void Actions_the_typed_side_does_not_decide_file_no_decision()
    {
        using var kiln = Started(out var p, out _, out _);
        _subs.Add(Events.On<PlayerEquippingEvent>(_ => { }));
        kiln.Step();
        var (_, cancel, filed) = Held(kiln, p, "actionbarSelectionAction", new byte[8], genericCancel: true);
        Assert.False(filed);
        Assert.True(cancel);  // the generic vote alone, as before
        Assert.Empty(_completed);
        Assert.Empty(kiln.Decisions.Model.Answers);
        Assert.Equal(0u, ClientActionDecisions.Typed!.KindOf("inventoryTransferAction"));
        Assert.Equal(N.KindEquipSingle, ClientActionDecisions.Typed.KindOf(EquipDecisions.ChangeAction));
        Assert.Equal(N.KindEquipCollection, ClientActionDecisions.Typed.KindOf(EquipDecisions.SwapAction));
        // an equipment slot value that is no equipment slot (MainHand 25), or an unknown operation: generic only
        var (_, _, filedMain) = Held(kiln, p, EquipDecisions.ChangeAction, Change(SlotId.Of(p, 0), 9, 25, 0));
        var (_, _, filedOp) = Held(kiln, p, EquipDecisions.ChangeAction, Change(SlotId.Of(p, 0), 9, 14, 7));
        Assert.False(filedMain || filedOp);
    }

    [Fact]
    public void An_unequip_whose_slot_cannot_be_read_is_left_to_the_generic_vote()
    {
        // the test kit has no game type reflection: SlotSelection cannot be read, so the occupant is unknown and the
        // typed side files nothing rather than guessing Empty
        using var kiln = Started(out var p, out _, out _);
        var seen = 0;
        _subs.Add(Events.On<PlayerEquippingEvent>(_ => seen++));
        kiln.Step();
        var (_, cancel, filed) = Held(kiln, p, EquipDecisions.ChangeAction, Change(default, 3, 10, 1));  // unequip Ring0
        Assert.False(filed || cancel);
        Assert.Equal(0, seen);
        Assert.Empty(_completed);
    }

    [Fact]
    public void The_collection_swap_carries_its_target_and_contents_as_read()
    {
        using var kiln = Started(out var p, out var arrows, out _);
        var set = kiln.Entities.Create(new WorldPosition(4, 4, 4));
        var setInv = kiln.Items.AddContainer(set);
        setInv.Put(arrows, 12);
        var seen = new List<PlayerEquippingEvent>();
        _subs.Add(Events.On<PlayerEquippingEvent>(e => seen.Add(e), EventFilter.ForEntity(set)));
        kiln.Step();
        var (id, cancel, filed) = Held(kiln, p, EquipDecisions.SwapAction, Swap(set, 1, 4));
        Assert.True(filed);
        Assert.False(cancel);
        var e = Assert.Single(seen);  // the filter names the collection, not the player: Involves finds it
        var c = Assert.IsType<CollectionEquipmentChange>(e.Change);
        Assert.Equal((EquipmentChangeKind.Collection, set.Value, true, 4u), (c.Kind, c.TargetInventory.Entity, c.Vanity, c.Version));
        var stack = Assert.Single(c.Contents);
        Assert.Equal((arrows, 12, true), (stack.Item, stack.Count, stack.Instance.IsNone));
        Assert.Equal(set, stack.Source.Inventory);
        Assert.Equal(DecisionTerminal.Allowed, Single(id).Status);
        // a target of 2 (neither ActiveEquipment nor Vanity) is no collection swap the typed side describes
        Assert.False(Held(kiln, p, EquipDecisions.SwapAction, Swap(set, 2, 5)).Filed);
    }

    [Fact]
    public void Events_On_is_refused_by_name_where_the_intake_cannot_hold_an_input_alone()
    {
        using var kiln = Started(out _, out _, out _);
        // Kiln.Players not started yet (a mod's OnLoad comes before the first service tick): not refused, decided later
        ClientActionDecisions.SetIntake(null);
        Events.On<PlayerEquippingEvent>(_ => { }).Dispose();
        var old = new FakeIntake("this kiln-players.dll is older than 0.2.0-dev.29");
        ClientActionDecisions.SetIntake(old);
        Assert.Equal(1, old.Starts);  // the typed side was there first: the intake's pump starts
        var e = Assert.Throws<KilnUnavailableException>(() => Events.On<PlayerEquippingEvent>(_ => { }));
        Assert.Equal(EquipDecisions.Row, e.Capability);
        Assert.Contains("older than 0.2.0-dev.29", e.Message);
        Assert.False(ClientActionDecisions.Typed!.On);  // never widened to every client action instead
        // a base-class handler is never refused
        Events.On<CancelableEvent>(_ => { }).Dispose();
        // and events.equip off refuses by name as well (Kiln.Players lowers it where the table cannot hold selectively)
        ClientActionDecisions.SetIntake(new FakeIntake(null));
        Events.On<PlayerEquippingEvent>(_ => { }).Dispose();
        KitBackend.Instance.Capabilities.Report(KitBackend.CapabilityOwner, EquipDecisions.Row, CapState.Unavailable, CapReason.ServiceOlder,
            "this kiln-players.dll is older than 0.2.0-dev.29", KitBackend.CapabilityOwner);
        var off = Assert.Throws<KilnUnavailableException>(() => Events.On<PlayerEquippingEvent>(_ => { }));
        Assert.Equal(EquipDecisions.Row, off.Capability);
        Assert.False(ClientActionDecisions.Typed.On);
    }

    [Fact]
    public void The_records_keep_copies_and_say_what_they_involve()
    {
        var list = new List<CollectionStack> { new(SlotId.Of(new EntityId(30), 1), new ItemId(5), 2, ItemInstanceRef.None) };
        var c = new CollectionEquipmentChange(new EventEntityKey(30, true, 1, 2, 0, 0), false, 9, list);
        list.Clear();
        Assert.Single(c.Contents);  // a copy at the public boundary
        var entries = new List<EquipmentSetEntry> { new(EquipmentSlot.Ring0, new InstanceEquipment(new ItemInstanceRef(new EntityRef(new EntityId(77), 5), new ItemId(9)))) };
        var set = new EquipmentSet(new EquipmentSetScope(Guid.NewGuid(), 1, new EventEntityKey(3, true, 1, 1, 0, 0)), entries);
        entries.Clear();
        Assert.Single(set.Entries);
        var player = new EntityId(3);
        Assert.True(EquipmentInvolvement.Involves(player, c, new EntityId(30)));
        Assert.False(EquipmentInvolvement.Involves(player, c, new EntityId(31)));
        var s = new SetEquipmentChange(12, new InventorySnapshotToken(4, 1, player), set, DryRun: false);
        Assert.Equal(EquipmentChangeKind.Set, s.Kind);
        Assert.True(EquipmentInvolvement.Involves(player, s, new EntityId(77)) && EquipmentInvolvement.Involves(player, s, player));
        Assert.False(EquipmentInvolvement.Involves(player, s, EntityId.None));
        var e = new PlayerEquippingEvent(new DecisionId(1, 2), new EventEntityKey(3, true, 1, 1, 0, 0), s, true, "mod-b");
        Assert.True(e.Canceled && e.PriorCanceled);
        Assert.Equal("mod-b", e.CanceledBy);
        Assert.Throws<ArgumentNullException>(() => new PlayerEquippingEvent(default, default, null!, false, null));
    }
}
