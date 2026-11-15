// SPDX-License-Identifier: MIT
// Equipment sets' one decision (plan 10-equipment-sets S5; chapter 10 §4.3, chapter 07 §4.4): kiln.items files one
// kind-18 request per set in Pre (native, enqueue_native); Kiln.Items' producer turns it into one PlayerEquippingEvent with
// a SetEquipmentChange and answers with its handlers' verdict. Here the request is filed into the test kit's fake
// kiln.decisions as kiln.items files it, and the dispatcher runs on it. What kiln.items does with the verdict (Canceled,
// the set runs) is native (equip.cpp) and the lab's (mods/tests/equipment-sets-test, SET-SINGLE-DECISION).
using System.Runtime.InteropServices;
using Kiln;
using Kiln.TestKit;
using Xunit;

namespace Kiln.DecisionsTests;

public sealed unsafe class SetDecisionsTests
{
    private static readonly Guid Session = Guid.Parse("01234567-89ab-4cde-8123-0123456789ab");
    private static readonly EventEntityKey Player = new(1, true, 123, 9, 4, 20);
    private static readonly EquipmentSetScope Scope = new(Session, 7, Player);

    private static ItemInstanceRef Ref(uint entity, uint item, uint stamp) =>
        new(new EntityRef(new EntityId(entity), IdentitySources.Stamp(EntitySourceOrder.Items, stamp)), new ItemId(item));

    // kiln.playerstate's intake as Kiln.Players offers it where the table holds selectively (the kit has no
    // kiln.playerstate): events.equip's producer asks it before a PlayerEquippingEvent subscription is taken
    private sealed class SelectiveIntake : ClientActionDecisions.IIntake
    {
        public void Start() { }
        public string? WhyNotSelective() => null;
    }

    private static EquipmentSetEntry[] OneRing() => [new(EquipmentSlot.Ring0, new InstanceEquipment(Ref(5002, 0x4623bcb0, 1002)))];

    private static byte[] Payload(ulong ticket, ulong token, bool dry, params EquipDecisions.SetOp[] ops)
    {
        var p = new ItemsNative.SetDecisionPayload { ItemsTicket = ticket, SnapshotToken = token, Count = (uint)ops.Length, Flags = dry ? 1u : 0u };
        var e = (EquipDecisions.SetOp*)p.Entries;
        for (var i = 0; i < ops.Length; i++) e[i] = ops[i];
        var b = new byte[sizeof(ItemsNative.SetDecisionPayload)];
        MemoryMarshal.Write(b, in p);
        return b;
    }

    [Fact]
    public void ASetIsOneDecisionAndACancelRefusesIt()
    {
        Items.ResetSetsForTests();
        FakeTables.Features([EquipDecisions.Row]);  // events.equip: PlayerEquippingEvent can be subscribed
        try
        {
            using var kiln = new KilnHarness();
            kiln.Step(2);
            var player = kiln.Entities.Create(new WorldPosition(1, 2, 3));
            ClientActionDecisions.SetIntake(new SelectiveIntake());
            Items.EnsureSetDecisions();
            var seen = new List<PlayerEquippingEvent>();
            var cancel = true;
            using var sub = Events.On<PlayerEquippingEvent>(e =>
            {
                seen.Add(e);
                if (cancel) e.Cancel();
            });
            kiln.Step();
            // a set Kiln.Items submitted under ticket 77: the event names that set, not the payload's copy
            var scope = Scope with { Player = Player with { Entity = player.Value } };
            var tracked = new TaskCompletionSource<EquipmentSetResult>();
            var key = Items.TrackSet(tracked, null, player, scope, OneRing(), true, 41, 7, 0, 77);
            var ops = Items.OpsOf(OneRing());
            Assert.Equal(0, kiln.Decisions.Model.Capture(DecisionsNative.KindEquipSet, player.Value, player.Value, Payload(77, 41, true, ops), DecisionsNative.FlagDryRun, 0, out var id));
            kiln.Step();
            var e1 = Assert.Single(seen);
            Assert.Equal(id, e1.Id);
            var set = Assert.IsType<SetEquipmentChange>(e1.Change);
            Assert.Equal((77UL, 41UL, true), (set.ItemsTicket, set.Snapshot.Value, set.DryRun));
            Assert.Equal(scope, set.Set.Scope);
            Assert.Equal(OneRing(), set.Set.Entries);
            Assert.True(e1.Involves(player) && e1.Involves(new EntityId(5002)));  // the player and the item the set selects
            Assert.False(Assert.Single(kiln.Decisions.Model.Answers).Allow);  // canceled: the set does not run
            Items.CompleteSet(key, EquipmentSetResult.NotRun(EquipmentSetStatus.Canceled, "canceled", player, true, 77));

            // a native caller's set (no managed entry): the change is read from the payload; allowed
            cancel = false;
            seen.Clear();
            Assert.Equal(0, kiln.Decisions.Model.Capture(DecisionsNative.KindEquipSet, player.Value, player.Value, Payload(99, 50, false, ops), 0, 0, out var id2));
            kiln.Step();
            var e2 = Assert.Single(seen);
            var native = Assert.IsType<SetEquipmentChange>(e2.Change);
            Assert.Equal((99UL, 50UL, false), (native.ItemsTicket, native.Snapshot.Value, native.DryRun));
            Assert.Equal(EquipmentSlot.Ring0, Assert.Single(native.Set.Entries).Slot);
            Assert.Equal(new InstanceEquipment(Ref(5002, 0x4623bcb0, 1002)), native.Set.Entries[0].Selection);
            Assert.True(kiln.Decisions.Model.Answers[^1].Allow);
            Assert.Equal(id2, kiln.Decisions.Model.Answers[^1].Id);
        }
        finally
        {
            // events.equip's producers too (kinds 16 and 17, registered by the subscription): the broker is process-wide and
            // DecisionBrokerTests registers its own kind-16 producer
            Items.ResetSetsForTests();
            EquipDecisions.ResetForTests();
            ClientActionDecisions.ResetForTests();
            FakeTables.Features([EquipDecisions.Row]);
        }
    }
}
