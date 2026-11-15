// SPDX-License-Identifier: MIT
// Kiln.Combat's producer of cancelable deaths (DeathDecisions, EntityDyingEvent; plan 07 S4, chapter 07 §4.2 "Death")
// on the test kit's kiln.decisions and kiln.combat. The game's half (the hook at the death boundary and the health
// write of the answer) is native and has its host tests (native/services/decisions/tests/death_test.cpp); here the
// fake's finisher stands in for the native one, written to the same rules, so the managed half can be read end to end:
// the event and its fields, the verdict, a heal that comes first, the CombatChangedEvent of the write, the statuses that
// fail (NoPositiveRange, WriteRefused) reaching DecisionCompletedEvent, the interest pushed to the service, and the
// refusal by name while events.death is off.
using Kiln;
using Kiln.TestKit;
using Xunit;
using N = Kiln.DecisionsNative;

[assembly: CollectionBehavior(DisableTestParallelization = true)]

namespace Kiln.DecisionsTests;

public sealed class DeathDecisionsTests : IDisposable
{
    private readonly List<IDisposable> _subs = [];
    private readonly List<DecisionResult> _completed = [];

    public DeathDecisionsTests()
    {
        FakeTables.Features([DeathDecisions.Row]);
        DeathDecisions.Ensure();
    }

    public void Dispose()
    {
        foreach (var s in _subs) s.Dispose();
        DeathDecisions.ResetForTests();
        FakeTables.Features([DeathDecisions.Row]);
    }

    private KilnHarness Started(out EntityId entity, int health = 0, int max = 100)
    {
        var kiln = new KilnHarness();
        kiln.Step(2);
        kiln.Decisions.Model.SupportedHookKinds = 1ul << (int)N.KindDeath;
        kiln.Decisions.Model.Finisher = a => Finish(kiln, a);
        entity = kiln.Entities.Create(new WorldPosition(1, 2, 3));
        kiln.Combat.SetHealth(entity, health, max);
        _subs.Add(Events.On<DecisionCompletedEvent>(e => _completed.Add(e.Result)));
        return kiln;
    }

    // The native finisher's rules (death_core.h finish), on the kit's combat: what a test scripts with Refuse/NoRange.
    private bool _refuse, _noRange;

    private FakeDecisionFinish? Finish(KilnHarness kiln, FakeDecisionAnswer a)
    {
        if (a.Kind != N.KindDeath) return null;
        var hp = kiln.CombatFake!.HealthOf(a.Subject.Value);
        if (hp is null) return new(DecisionTerminal.StaleEntity, "no health");
        if (hp.Current > 0) return new(DecisionTerminal.AlreadyAlive, "positive again", hp.Current, hp.Current);
        if (a.Allow) return new(DecisionTerminal.Allowed, null, hp.Current, hp.Current);
        var max = _noRange ? 0 : hp.Max;
        if (max <= 0) return new(DecisionTerminal.NoPositiveRange, "a maximum health of 0", hp.Current, hp.Current);
        if (_refuse) return new(DecisionTerminal.WriteRefused, "kiln.combat refused the health write", hp.Current, hp.Current);
        kiln.CombatFake.Set(a.Subject.Value, Math.Clamp(a.SurvivalHealth, 1, max), out var applied);
        return new(DecisionTerminal.Prevented, null, hp.Current, applied);
    }

    private static DecisionId Capture(KilnHarness kiln, EntityId entity, int health = 0)
    {
        var payload = new byte[24];
        BitConverter.TryWriteBytes(payload.AsSpan(0, 4), health);
        BitConverter.TryWriteBytes(payload.AsSpan(16, 4), 1);
        Assert.Equal(FakeTables.Ok, kiln.Decisions.Model.Capture(N.KindDeath, entity.Value, 0, payload, N.FlagHasPosition, 0, out var id));
        return id;
    }

    private DecisionResult Single(DecisionId id) => Assert.Single(_completed, r => r.Id == id);

    [Fact]
    public void A_canceled_death_keeps_the_entity_alive_and_says_so_once()
    {
        using var kiln = Started(out var e);
        var seen = new List<EntityDyingEvent>();
        var changes = new List<CombatChangedEvent>();
        _subs.Add(Events.On<EntityDyingEvent>(d =>
        {
            seen.Add(d);
            d.SurvivalHealth = 25;
            d.Cancel();
        }, EventFilter.ForEntity(e)));
        _subs.Add(Events.On<CombatChangedEvent>(changes.Add));
        kiln.Step();
        Assert.False(kiln.Decisions.Model.InterestOf(N.KindDeath).All);
        Assert.Equal(new[] { e.Value }, kiln.Decisions.Model.InterestOf(N.KindDeath).Ids);
        var id = Capture(kiln, e);
        kiln.Step();
        var d = Assert.Single(seen);
        Assert.Equal((id, e, 0, 100, e.Value), (d.Id, d.Entity, d.Health, d.MaximumHealth, d.Key.Entity));
        Assert.True(d.Key.HasSpawnTime && d.Key.LifeSerial != 0);
        var done = Single(id);
        Assert.Equal((DecisionTerminal.Prevented, true, (int?)25, (int?)0, (int?)25), (done.Status, done.Ok, done.RequestedHealth, done.HealthBefore, done.AppliedHealth));
        Assert.Equal(25, kiln.Combat.Health(e)!.Current);
        var change = Assert.Single(changes);
        Assert.Equal((e, "Health", 0, 25, "kiln.decisions"), (change.Entity, change.What, change.Before, change.After, change.Origin));
        kiln.Step(4);
        Assert.Single(seen);
        Assert.Single(changes);  // once per decision
        Assert.Single(_completed);
    }

    [Fact]
    public void Nobody_cancels_so_the_death_is_allowed_and_nothing_is_written()
    {
        using var kiln = Started(out var e);
        var changes = new List<CombatChangedEvent>();
        _subs.Add(Events.On<EntityDyingEvent>(_ => { }));
        _subs.Add(Events.On<CombatChangedEvent>(changes.Add));
        kiln.Step();
        Assert.True(kiln.Decisions.Model.InterestOf(N.KindDeath).All);  // an unfiltered handler holds every death
        var id = Capture(kiln, e);
        kiln.Step();
        Assert.Equal((DecisionTerminal.Allowed, true, (int?)null), (Single(id).Status, Single(id).Ok, Single(id).RequestedHealth));
        Assert.Equal(0, kiln.Combat.Health(e)!.Current);
        Assert.Empty(changes);
    }

    [Fact]
    public void A_heal_that_comes_first_wins_and_no_stale_event_is_posted()
    {
        using var kiln = Started(out var e);
        var seen = 0;
        _subs.Add(Events.On<EntityDyingEvent>(d =>
        {
            seen++;
            d.Cancel();
        }, EventFilter.ForEntity(e)));
        var id = Capture(kiln, e);
        kiln.Combat.SetHealth(e, 60, 100);  // healed between the capture and the next Pre
        kiln.Step();
        Assert.Equal(0, seen);
        var done = Single(id);
        Assert.Equal((DecisionTerminal.AlreadyAlive, true, (int?)60), (done.Status, done.Ok, done.AppliedHealth));
        Assert.Equal(60, kiln.Combat.Health(e)!.Current);
    }

    [Fact]
    public void A_handler_that_heals_before_the_answer_is_not_overwritten()
    {
        using var kiln = Started(out var e);
        _subs.Add(Events.On<EntityDyingEvent>(d =>
        {
            Combat.SetHealth(d.Entity, d.MaximumHealth);
            d.SurvivalHealth = 1;
            d.Cancel();
        }, EventFilter.ForEntity(e)));
        var id = Capture(kiln, e);
        kiln.Step();
        Assert.Equal((DecisionTerminal.AlreadyAlive, (int?)100), (Single(id).Status, Single(id).AppliedHealth));
        Assert.Equal(100, kiln.Combat.Health(e)!.Current);
    }

    [Fact]
    public void No_positive_range_and_a_refused_write_fail_with_a_reason()
    {
        using var kiln = Started(out var e);
        _subs.Add(Events.On<EntityDyingEvent>(d => d.Cancel(), EventFilter.ForEntity(e)));
        _noRange = true;
        var a = Capture(kiln, e);
        kiln.Step();
        _noRange = false;
        _refuse = true;
        var b = Capture(kiln, e);
        kiln.Step();
        Assert.Equal((DecisionTerminal.NoPositiveRange, false), (Single(a).Status, Single(a).Ok));
        Assert.False(string.IsNullOrEmpty(Single(a).Reason));
        Assert.Equal((DecisionTerminal.WriteRefused, false), (Single(b).Status, Single(b).Ok));
        Assert.False(string.IsNullOrEmpty(Single(b).Reason));
        Assert.Equal(0, kiln.Combat.Health(e)!.Current);  // released: no immortality
        Assert.Equal(0, kiln.Decisions.Active);
    }

    [Fact]
    public void A_filter_narrows_the_interest_and_a_catch_all_widens_it_until_disposed()
    {
        using var kiln = Started(out var p);
        var q = kiln.Entities.Create();
        _subs.Add(Events.On<EntityDyingEvent>(_ => { }, EventFilter.ForEntity(p)));
        kiln.Step();
        var (all, ids) = kiln.Decisions.Model.InterestOf(N.KindDeath);
        Assert.False(all);
        Assert.Equal(new[] { p.Value }, ids);
        var catchAll = Events.On<KilnEvent>(_ => { });
        kiln.Step();
        Assert.True(kiln.Decisions.Model.InterestOf(N.KindDeath).All);
        catchAll.Dispose();
        kiln.Step();
        (all, ids) = kiln.Decisions.Model.InterestOf(N.KindDeath);
        Assert.False(all);
        Assert.Equal(new[] { p.Value }, ids);
        Assert.DoesNotContain(q.Value, ids);
    }

    [Fact]
    public void Survival_health_must_be_positive()
    {
        var d = new EntityDyingEvent(new DecisionId(1, 1), new EventEntityKey(2000, true, 5, 6, 0, 0), 0, 100);
        Assert.Equal(1, d.SurvivalHealth);
        Assert.Throws<ArgumentOutOfRangeException>(() => d.SurvivalHealth = 0);
        Assert.Throws<ArgumentOutOfRangeException>(() => d.SurvivalHealth = -3);
        d.SurvivalHealth = 40;
        Assert.Equal(40, d.SurvivalHealth);
        Assert.True(d.Involves(new EntityId(2000)) && !d.Involves(new EntityId(2001)) && !d.Involves(EntityId.None));
    }

    [Fact]
    public void While_events_death_is_off_a_subscription_is_refused_by_name()
    {
        using var kiln = new KilnHarness();
        kiln.Step(2);
        kiln.Decisions.Model.SupportedHookKinds = 1ul << (int)N.KindDeath;
        KitBackend.Instance.Capabilities.Report(KitBackend.CapabilityOwner, DeathDecisions.Row, CapState.Unavailable, CapReason.Policy,
            "held by the owner's gate list (a test of a row that is off)", KitBackend.CapabilityOwner);
        var off = Assert.Throws<KilnUnavailableException>(() => Events.On<EntityDyingEvent>(_ => { }));
        Assert.Equal("events.death", off.Capability);
        FakeTables.Features([DeathDecisions.Row]);
        // on, but the service has no death producer in this build: refused too, by the same name
        kiln.Decisions.Model.SupportedHookKinds = 0;
        off = Assert.Throws<KilnUnavailableException>(() => Events.On<EntityDyingEvent>(_ => { }));
        Assert.Equal("events.death", off.Capability);
        kiln.Decisions.Model.SupportedHookKinds = 1ul << (int)N.KindDeath;
        _subs.Add(Events.On<EntityDyingEvent>(_ => { }));
    }

    // Since 0.2.0-dev.29 the death hook goes in when a mod first subscribes (kiln.decisions' prepare_kind), not at the
    // service's start: a harness that nobody subscribes in asks for nothing, a subscription asks, and an install that
    // fails at that moment lowers events.death with its reason and refuses the subscription by the row's name.
    [Fact]
    public void The_death_hook_goes_in_at_the_first_subscription_and_a_failed_install_refuses_it_by_name()
    {
        using var kiln = new KilnHarness();
        kiln.Step(2);
        kiln.Decisions.Model.SupportedHookKinds = 1ul << (int)N.KindDeath;
        kiln.Step(3);
        Assert.False(kiln.Decisions.Model.Prepared.ContainsKey(N.KindDeath));  // nobody listens: nothing prepared
        kiln.Decisions.Model.PrepareRefusals[N.KindDeath] = (DeathDecisions.Row, "kiln.hooks could not hook the target (see kiln.log)");
        var off = Assert.Throws<KilnUnavailableException>(() => Events.On<EntityDyingEvent>(_ => { }));
        Assert.Equal("events.death", off.Capability);
        Assert.Contains("could not hook the target", off.Message);
        Assert.Equal(1, kiln.Decisions.Model.Prepared[N.KindDeath]);
        Assert.False(Capabilities.Is(DeathDecisions.Row));  // the row reads off
        Assert.False(Events.HasSubscribers<EntityDyingEvent>());  // nothing was subscribed
        // a later subscription is refused as well (the row is off now), and nothing is held for anyone
        Assert.Throws<KilnUnavailableException>(() => Events.On<EntityDyingEvent>(_ => { }));
        kiln.Step(2);
        Assert.Equal(0, kiln.Decisions.Model.ActiveCount);
    }

    [Fact]
    public void A_subscription_prepares_the_death_kind_once_it_holds()
    {
        using var kiln = Started(out var entity);
        _subs.Add(Events.On<EntityDyingEvent>(_ => { }, EventFilter.ForEntity(entity)));
        Assert.True(kiln.Decisions.Model.Prepared.GetValueOrDefault(N.KindDeath) >= 1);
        Assert.True(Capabilities.Is(DeathDecisions.Row));
    }
}
