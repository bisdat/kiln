// SPDX-License-Identifier: MIT
// Kiln.Gameplay's producer of cancelable walk-over pickups (PickupDecisions, ItemPickingUpEvent; plan 07 S5, chapter 07
// §4.3) and Decisions.ReevaluatePickup on the test kit's kiln.decisions. The game's half (the hook at the range helper,
// the sampled exposures, the refusal) is native and has its host tests (native/services/decisions/tests/pickup_test.cpp);
// here a capture stands in for the hook: the event and its fields, the verdict, the outcome's subject and actor, the
// interest a filter pushes, the reevaluation of a canceled exposure, and the refusals by name while events.pickup is off.
using Kiln;
using Kiln.TestKit;
using Xunit;
using N = Kiln.DecisionsNative;

namespace Kiln.DecisionsTests;

public sealed class PickupDecisionsTests : IDisposable
{
    private readonly List<IDisposable> _subs = [];
    private readonly List<DecisionResult> _completed = [];

    public PickupDecisionsTests()
    {
        FakeTables.Features([PickupDecisions.Row]);
        PickupDecisions.Ensure();
    }

    public void Dispose()
    {
        foreach (var s in _subs) s.Dispose();
        PickupDecisions.ResetForTests();
        FakeTables.Features([PickupDecisions.Row]);
    }

    private KilnHarness Started(out EntityId receiver, out EntityId drop)
    {
        var kiln = new KilnHarness();
        kiln.Step(2);
        kiln.Decisions.Model.SupportedHookKinds = 1ul << (int)N.KindWalkover;
        receiver = kiln.Entities.Create(new WorldPosition(1, 2, 3));
        drop = kiln.Entities.Create(new WorldPosition(2, 2, 3));
        _subs.Add(Events.On<DecisionCompletedEvent>(e => _completed.Add(e.Result)));
        return kiln;
    }

    private static DecisionId Capture(KilnHarness kiln, EntityId drop, EntityId receiver, ulong exposure, uint item = 0x2b0cf1d5, uint amount = 3)
    {
        var payload = new byte[40];
        BitConverter.TryWriteBytes(payload.AsSpan(0, 4), item);
        BitConverter.TryWriteBytes(payload.AsSpan(4, 4), amount);
        BitConverter.TryWriteBytes(payload.AsSpan(12, 4), 1u);
        BitConverter.TryWriteBytes(payload.AsSpan(16, 8), 10.5);
        BitConverter.TryWriteBytes(payload.AsSpan(24, 8), 20.0);
        BitConverter.TryWriteBytes(payload.AsSpan(32, 8), -3.25);
        Assert.Equal(FakeTables.Ok, kiln.Decisions.Model.Capture(N.KindWalkover, drop.Value, receiver.Value, payload, N.FlagHasPosition, exposure, out var id));
        return id;
    }

    private DecisionResult Single(DecisionId id) => Assert.Single(_completed, r => r.Id == id);

    [Fact]
    public void A_canceled_pickup_names_drop_and_receiver_and_can_be_asked_again()
    {
        using var kiln = Started(out var p, out var d);
        var seen = new List<ItemPickingUpEvent>();
        var cancel = true;
        _subs.Add(Events.On<ItemPickingUpEvent>(e =>
        {
            seen.Add(e);
            if (cancel) e.Cancel();
        }, EventFilter.ForEntity(p)));
        kiln.Step();
        Assert.False(kiln.Decisions.Model.InterestOf(N.KindWalkover).All);
        Assert.Equal(new[] { p.Value }, kiln.Decisions.Model.InterestOf(N.KindWalkover).Ids);
        var id = Capture(kiln, d, p, 7);
        kiln.Step();
        var e = Assert.Single(seen);
        Assert.Equal((id, p, d, 0x2b0cf1d5u, 3, 7ul), (e.Id, e.Receiver, e.GroundEntity, e.Item.Value, e.Amount, e.ExposureEpoch));
        Assert.Equal(new WorldPosition(10.5, 20.0, -3.25), e.Position);
        Assert.Equal((p.Value, d.Value), (e.ReceiverKey.Entity, e.GroundKey.Entity));
        Assert.True(e.Involves(p) && e.Involves(d) && !e.Involves(EntityId.None));
        var done = Single(id);
        Assert.Equal((DecisionTerminal.Prevented, true, d, p, (int?)null), (done.Status, done.Ok, done.Subject, done.Actor, done.RequestedHealth));

        PickupReevaluationResult again = null!;
        kiln.Call(() => again = Decisions.ReevaluatePickup(d, p));
        Assert.Equal(new PickupReevaluationResult(true, null, 1), again);
        kiln.Call(() => again = Decisions.ReevaluatePickup(d, p));
        Assert.Equal(new PickupReevaluationResult(false, "NoActiveExposure", 0), again);

        cancel = false;
        var next = Capture(kiln, d, p, 8);
        kiln.Step();
        Assert.Equal((DecisionTerminal.Allowed, true), (Single(next).Status, Single(next).Ok));
        kiln.Call(() => again = Decisions.ReevaluatePickup(EntityId.None, p));
        Assert.False(again.Ok);  // an allowed exposure was never suppressed here
    }

    [Fact]
    public void A_pickup_without_position_has_none()
    {
        using var kiln = Started(out var p, out var d);
        ItemPickingUpEvent? seen = null;
        _subs.Add(Events.On<ItemPickingUpEvent>(e => seen = e));
        kiln.Step();
        Assert.True(kiln.Decisions.Model.InterestOf(N.KindWalkover).All);
        Assert.Equal(FakeTables.Ok, kiln.Decisions.Model.Capture(N.KindWalkover, d.Value, p.Value, new byte[40], 0, 3, out var id));
        kiln.Step();
        Assert.NotNull(seen);
        Assert.Null(seen!.Position);
        Assert.Equal(DecisionTerminal.Allowed, Single(id).Status);
    }

    [Fact]
    public void While_events_pickup_is_off_both_the_event_and_the_reevaluation_are_refused_by_name()
    {
        using var kiln = new KilnHarness();
        kiln.Step(2);
        kiln.Decisions.Model.SupportedHookKinds = 1ul << (int)N.KindWalkover;
        KitBackend.Instance.Capabilities.Report(KitBackend.CapabilityOwner, PickupDecisions.Row, CapState.Unavailable, CapReason.Policy,
            "game test pending: 07-events.decision-rollback", KitBackend.CapabilityOwner);
        var off = Assert.Throws<KilnUnavailableException>(() => Events.On<ItemPickingUpEvent>(_ => { }));
        Assert.Equal("events.pickup", off.Capability);
        Exception? thrown = null;
        kiln.Call(() =>
        {
            try { Decisions.ReevaluatePickup(new EntityId(5000), new EntityId(5001)); }
            catch (Exception e) { thrown = e; }
        });
        Assert.Equal("events.pickup", Assert.IsType<KilnUnavailableException>(thrown).Capability);
        FakeTables.Features([PickupDecisions.Row]);
        // on, but the service has no walk-over producer in this build
        kiln.Decisions.Model.SupportedHookKinds = 0;
        off = Assert.Throws<KilnUnavailableException>(() => Events.On<ItemPickingUpEvent>(_ => { }));
        Assert.Equal("events.pickup", off.Capability);
        thrown = null;
        kiln.Call(() =>
        {
            try { Decisions.ReevaluatePickup(new EntityId(5000), new EntityId(5001)); }
            catch (Exception e) { thrown = e; }
        });
        Assert.Equal("events.pickup", Assert.IsType<KilnUnavailableException>(thrown).Capability);
    }
}
