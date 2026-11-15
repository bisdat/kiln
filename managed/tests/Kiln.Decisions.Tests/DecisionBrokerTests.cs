// SPDX-License-Identifier: MIT
// Kiln.Api's dispatcher of decisions (DecisionBroker) on the test kit's kiln.decisions (plan 07 S3, chapter 07 §4.2 and
// the offline list of §6): a request is dispatched once whatever is read twice, every terminal status ends in one
// DecisionCompletedEvent, a world and a process that turn over, the kinds' interest, and Events.On's refusal by name.
// One class: the broker and the bus are process-wide, and one harness runs at a time.
using Kiln;
using Kiln.TestKit;
using Xunit;
using N = Kiln.DecisionsNative;

namespace Kiln.DecisionsTests;

/// <summary>The event of the tests' death-like kind.</summary>
public sealed class TestDyingEvent(DecisionId id, EntityId entity) : CancelableEvent, IEntityEvent
{
    public DecisionId Id { get; } = id;
    public EntityId Entity { get; } = entity;
    public int Survival { get; set; } = 1;
    public bool Involves(EntityId e) => !e.IsNone && e == Entity;
}

/// <summary>The event of the tests' client-input kind.</summary>
public sealed class TestInputEvent(DecisionId id, EntityId entity) : CancelableEvent, IEntityEvent
{
    public DecisionId Id { get; } = id;
    public EntityId Entity { get; } = entity;
    public bool Involves(EntityId e) => !e.IsNone && e == Entity;
}

public sealed unsafe class DecisionBrokerTests : IDisposable
{
    private const string DeathRow = "test.decisions.death", InputRow = "test.decisions.input";

    private sealed class DeathProducer : DecisionProducer
    {
        public bool Throw;
        public readonly List<string> Calls = [];
        public override uint Kind => N.KindDeath;
        public override string Capability => DeathRow;
        public override IReadOnlyList<Type> EventTypes { get; } = [typeof(TestDyingEvent)];

        public override DecisionVerdict Decide(DecisionRequest request)
        {
            Calls.Add($"decide {request.Id.Request}");
            if (Throw) throw new InvalidOperationException("a producer's fault");
            Assert.Equal(24, request.Payload.Length);
            var e = Events.Post(new TestDyingEvent(request.Id, new EntityId(request.Subject.Entity)));
            return e.Canceled ? DecisionVerdict.Canceled(e.Survival) : DecisionVerdict.Allowed;
        }

        public override void Answering(DecisionRequest request, DecisionVerdict verdict) => Calls.Add($"answering {request.Id.Request}");
        public override void Answered(DecisionRequest request, DecisionVerdict verdict, DecisionReceipt receipt) =>
            Calls.Add($"answered {request.Id.Request} {receipt.Accepted} {receipt.Status}");
        public override void Completed(DecisionResult result) => Calls.Add($"completed {result.Id.Request} {result.Status}");
    }

    private sealed class InputProducer : DecisionProducer
    {
        public override uint Kind => N.KindEquipSingle;
        public override string Capability => InputRow;
        public override IReadOnlyList<Type> EventTypes { get; } = [typeof(TestInputEvent)];

        public override DecisionVerdict Decide(DecisionRequest request)
        {
            var e = Events.Post(new TestInputEvent(request.Id, new EntityId(request.Subject.Entity)));
            return new DecisionVerdict(!e.Canceled);
        }
    }

    private readonly DeathProducer _death = new();
    private readonly InputProducer _input = new();
    private readonly List<IDisposable> _subs = [];
    private readonly List<DecisionResult> _completed = [];

    public DecisionBrokerTests()
    {
        FakeTables.Features([DeathRow, InputRow]);
        DecisionBroker.Register(_death);
        DecisionBroker.Register(_input);
    }

    public void Dispose()
    {
        foreach (var s in _subs) s.Dispose();
        DecisionBroker.Unregister(_death);
        DecisionBroker.Unregister(_input);
    }

    private KilnHarness Started()
    {
        var kiln = new KilnHarness();
        kiln.Step(2);
        kiln.Decisions.Model.SupportedHookKinds = 1ul << (int)N.KindDeath;
        _subs.Add(Events.On<DecisionCompletedEvent>(e => _completed.Add(e.Result)));
        return kiln;
    }

    private void On<T>(Action<T> handler, EventFilter? filter = null) where T : KilnEvent =>
        _subs.Add(filter is null ? Events.On(handler) : Events.On(handler, filter));

    private static DecisionId Capture(KilnHarness kiln, EntityId entity)
    {
        Assert.Equal(FakeTables.Ok, kiln.Decisions.Model.Capture(N.KindDeath, entity.Value, 0, new byte[24], 0, 0, out var id));
        return id;
    }

    private DecisionResult Single(DecisionId id) => Assert.Single(_completed, r => r.Id == id);

    [Fact]
    public void The_mirrors_are_the_headers()
    {
        Assert.True(N.LayoutMatchesNative);
        Assert.Equal((24, 40, 56, 56, 824, 328, 0, 0), (N.PayloadSize(1), N.PayloadSize(2), N.PayloadSize(16), N.PayloadSize(17),
            N.PayloadSize(18), N.PayloadSize(32), N.PayloadSize(33), N.PayloadSize(3)));
        var t = (N.Table*)KitBackend.Instance.Service(N.Service, 1);
        Assert.True(t != null);
        Assert.Equal(((uint)sizeof(N.TableTail), 1u), (t->Header.Size, t->Header.Version));  // the whole dev.29 table
        Assert.True(N.Has(t, N.PrepareKindOffset) && ((N.TableTail*)t)->PrepareKind != null);
    }

    [Fact]
    public void A_held_decision_is_dispatched_answered_and_completed_once()
    {
        using var kiln = Started();
        var e = kiln.Entities.Create(new WorldPosition(1, 2, 3));
        var seen = new List<DecisionId>();
        On<TestDyingEvent>(d =>
        {
            Assert.True(Game.IsTickThread);
            seen.Add(d.Id);
            d.Survival = 7;
            d.Cancel();
        });
        var id = Capture(kiln, e);
        Assert.Equal(1, kiln.Decisions.Active);
        Assert.Empty(seen);  // nothing runs where the action was held: handlers run in the next Pre
        kiln.Step();
        Assert.Equal([id], seen);
        var answer = Assert.Single(kiln.Decisions.Model.Answers);
        Assert.Equal((id, false, 7), (answer.Id, answer.Allow, answer.SurvivalHealth));
        var done = Single(id);
        Assert.Equal((DecisionTerminal.Prevented, true, e, EntityId.None, (int?)7, null), (done.Status, done.Ok, done.Subject, done.Actor, done.RequestedHealth, done.Reason));
        Assert.Null(done.HealthBefore);
        Assert.Equal(0, kiln.Decisions.Active);
        Assert.Equal([$"decide {id.Request}", $"answering {id.Request}", $"answered {id.Request} True Prevented", $"completed {id.Request} Prevented"], _death.Calls);
        kiln.Step(5);
        Assert.Single(seen);
        Assert.Single(_completed);
        Assert.Equal(done, Assert.Single(kiln.Decisions.Outcomes));
    }

    [Fact]
    public void Nobody_cancels_so_the_action_is_allowed()
    {
        using var kiln = Started();
        var e = kiln.Entities.Create();
        On<TestDyingEvent>(_ => { });
        var id = Capture(kiln, e);
        kiln.Step();
        Assert.True(Assert.Single(kiln.Decisions.Model.Answers).Allow);
        Assert.Equal((DecisionTerminal.Allowed, true, null, null), (Single(id).Status, Single(id).Ok, Single(id).RequestedHealth, Single(id).Reason));
    }

    [Fact]
    public void A_batch_that_is_read_again_does_not_dispatch_twice()
    {
        using var kiln = Started();
        var a = kiln.Entities.Create();
        var b = kiln.Entities.Create();
        var calls = new List<DecisionId>();
        On<TestDyingEvent>(d =>
        {
            calls.Add(d.Id);
            d.Cancel();
        });
        var model = kiln.Decisions.Model;

        // the read fails once: nothing was consumed, the same batch is dispatched one step later, once
        model.FailRequestReads = 1;
        var first = Capture(kiln, a);
        kiln.Step();
        Assert.Empty(calls);
        kiln.Step();
        Assert.Equal([first], calls);

        // the acknowledge fails once: the batch is still open and is read again at the next step, with the same record
        model.FailRequestAcks = 1;
        var second = Capture(kiln, b);
        var reads = Interlocked.Read(ref DecisionBroker.BatchesRead);
        kiln.Step();
        Assert.Equal([first, second], calls);
        var third = Capture(kiln, a);  // arrives while the old batch is open: the next batch
        kiln.Step();
        Assert.True(Interlocked.Read(ref DecisionBroker.BatchesRead) >= reads + 4);  // the old batch again, then the new one (and outcomes)
        Assert.Equal([first, second, third], calls);
        Assert.Equal(3, model.Answers.Count);
        Assert.Equal([first, second, third], model.Answers.Select(x => x.Id));

        // the outcomes' acknowledge fails once: the batch is read again, the event is not posted again
        model.FailOutcomeAcks = 1;
        var fourth = Capture(kiln, b);
        kiln.Step(3);
        Assert.Equal(4, _completed.Count);
        Assert.Equal([first, second, third, fourth], _completed.Select(r => r.Id));
        Assert.All(_completed, r => Assert.Equal(DecisionTerminal.Prevented, r.Status));
    }

    [Fact]
    public void Every_terminal_status_reaches_one_DecisionCompletedEvent()
    {
        using var kiln = Started();
        var model = kiln.Decisions.Model;
        var e = kiln.Entities.Create();
        var cancel = false;
        On<TestDyingEvent>(d =>
        {
            if (cancel) d.Cancel();
        });
        var expected = new List<(DecisionId Id, DecisionTerminal Status)>();

        void Answered(DecisionTerminal status, bool canceled, FakeDecisionFinish? finish)
        {
            cancel = canceled;
            model.Finisher = finish is null ? null : _ => finish;
            var id = Capture(kiln, e);
            kiln.Step();
            expected.Add((id, status));
        }

        Answered(DecisionTerminal.Allowed, false, null);
        Answered(DecisionTerminal.Prevented, true, null);
        Answered(DecisionTerminal.AlreadyAlive, true, new(DecisionTerminal.AlreadyAlive, null, 40, 40));
        Answered(DecisionTerminal.NoPositiveRange, true, new(DecisionTerminal.NoPositiveRange, "maximum health 0"));
        Answered(DecisionTerminal.WriteRefused, true, new(DecisionTerminal.WriteRefused, "the health write was refused", 0, 0));
        model.Finisher = null;
        cancel = true;

        // ended by the service before any answer: the request is still delivered, but no handler runs for it
        var handled = _death.Calls.Count(c => c.StartsWith("decide"));
        foreach (var status in new[] { DecisionTerminal.StaleEntity, DecisionTerminal.CapabilityReleased, DecisionTerminal.SourceChanged })
        {
            var id = Capture(kiln, e);
            Assert.True(model.Complete(id.Request, status, status == DecisionTerminal.SourceChanged ? null : $"scripted {status}"));
            kiln.Step();
            expected.Add((id, status));
        }
        Assert.Equal(handled, _death.Calls.Count(c => c.StartsWith("decide")));

        // nobody answers (the reads fail): released at the window of capture + 3, told in the Pre after
        model.FailRequestReads = 100;
        var late = Capture(kiln, e);
        kiln.Step(3);
        Assert.Equal(1, kiln.Decisions.Active);
        Assert.DoesNotContain(_completed, r => r.Id == late);
        kiln.Step();
        expected.Add((late, DecisionTerminal.TimeoutReleased));
        Assert.Equal(0, kiln.Decisions.Active);
        model.FailRequestReads = 0;
        kiln.Step();  // the request's copy arrives now: it is terminal, no handler runs
        Assert.Equal(handled, _death.Calls.Count(c => c.StartsWith("decide")));

        // the world ends under a held action
        var ended = Capture(kiln, e);
        kiln.Decisions.RestartWorld();
        kiln.Step();
        expected.Add((ended, DecisionTerminal.WorldEnded));

        // the queue is full: the 4097th is never held
        model.FailRequestReads = 1;  // keep the 4096 open for one step
        for (var i = 0; i < DecisionsModel.MaxActive; i++) Capture(kiln, e);
        Assert.Equal(FakeTables.BufferTooSmall, model.Capture(N.KindDeath, e.Value, 0, new byte[24], 0, 0, out var dropped));
        Assert.Equal(1ul, model.Counters.QueueDropped);
        kiln.Step(2);
        expected.Add((dropped, DecisionTerminal.QueueDropped));
        Assert.Equal(0, kiln.Decisions.Active);

        Assert.Equal(11, expected.Select(x => x.Status).Distinct().Count());
        Assert.Equal(Enum.GetValues<DecisionTerminal>().Order(), expected.Select(x => x.Status).Order());
        foreach (var (id, status) in expected)
        {
            var done = Single(id);
            Assert.Equal(status, done.Status);
            var ok = status is DecisionTerminal.Allowed or DecisionTerminal.Prevented or DecisionTerminal.AlreadyAlive;
            Assert.Equal(ok, done.Ok);
            Assert.Equal(ok, done.Reason is null);  // a reason exactly when it did not take effect (SourceChanged had none: a default)
            Assert.Equal(e, done.Subject);
        }
        Assert.Equal((40, 40), (Single(expected[2].Id).HealthBefore, Single(expected[2].Id).AppliedHealth));
        Assert.Equal("maximum health 0", Single(expected[3].Id).Reason);
        Assert.Equal(expected.Count + DecisionsModel.MaxActive, _completed.Count);
        Assert.Equal(_completed.Count, _completed.Select(r => r.Id).Distinct().Count());
        kiln.Step(4);
        Assert.Equal(expected.Count + DecisionsModel.MaxActive, _completed.Count);
    }

    [Fact]
    public void A_new_world_has_a_new_epoch_and_a_new_process_starts_over()
    {
        var calls = new List<DecisionId>();
        DecisionId first, old, next;
        using (var kiln = Started())
        {
            On<TestDyingEvent>(d => calls.Add(d.Id));
            var e = kiln.Entities.Create();
            first = Capture(kiln, e);
            kiln.Step();
            old = Capture(kiln, e);
            kiln.Decisions.RestartWorld();
            next = Capture(kiln, e);
            kiln.Step();
            Assert.Equal((1ul, 1ul, 2ul), (first.WorldEpoch, old.WorldEpoch, next.WorldEpoch));
            Assert.True(next.Request > old.Request);  // request numbers only grow in a process
            Assert.Equal([first, next], calls);       // the old world's request never reaches a handler
            Assert.Equal(DecisionTerminal.WorldEnded, Single(old).Status);
            Assert.Equal(DecisionTerminal.Allowed, Single(next).Status);
            Assert.True(kiln.Decisions.Model.Attached);
        }
        // another harness is another process to the service: its ids start again, and nothing of the last one may hide them
        _completed.Clear();
        calls.Clear();
        using (var kiln = Started())
        {
            On<TestDyingEvent>(d => calls.Add(d.Id));  // a harness takes the subscriptions made in it with it
            var e = kiln.Entities.Create();
            var again = Capture(kiln, e);
            Assert.Equal(first, again);  // the same numbers as in the last process
            kiln.Step();
            Assert.True(kiln.Decisions.Model.Attached);
            Assert.Equal([again], calls);
            Assert.Equal(DecisionTerminal.Allowed, Single(again).Status);
        }
    }

    [Fact]
    public void A_client_input_is_filed_dispatched_and_answered_in_the_same_Pre()
    {
        using var kiln = Started();
        var e = kiln.Entities.Create();
        var seen = 0;
        On<TestInputEvent>(i =>
        {
            seen++;
            i.Cancel();
        });
        kiln.Step();
        var id = kiln.Run(() =>
        {
            Assert.Equal(N.Ok, DecisionBroker.ResolveEntity(e, out var key, out _));
            var made = DecisionBroker.Enqueue(N.KindEquipSingle, key, default, new byte[56], 0, out var rc);
            Assert.Equal(N.Ok, rc);
            Assert.Equal(0, seen);
            DecisionBroker.Pump(kiln.CurrentStep);
            Assert.Equal(1, seen);
            return made!.Value;
        });
        Assert.Equal(DecisionTerminal.Prevented, Single(id).Status);
        Assert.False(Assert.Single(kiln.Decisions.Model.Answers).Allow);
        Assert.Throws<ArgumentException>(() => kiln.Run(() => DecisionBroker.Enqueue(N.KindEquipSingle, default, default, new byte[8], 0, out _)));

        // one nobody answers (no producer of kind 17 here... it is answered "allow": the game goes on)
        var open = kiln.Run(() =>
        {
            DecisionBroker.ResolveEntity(e, out var key, out _);
            var made = DecisionBroker.Enqueue(N.KindEquipCollection, key, default, new byte[56], 0, out _);
            DecisionBroker.Pump(kiln.CurrentStep);
            return made!.Value;
        });
        Assert.Equal(DecisionTerminal.Allowed, Single(open).Status);
    }

    [Fact]
    public void The_lab_decision_is_allowed_refused_or_left_to_its_deadline()
    {
        using var kiln = Started();
        var e = kiln.Entities.Create();
        var (yes, no, never) = kiln.Run(() => (Decisions.LabDecide(e, true), Decisions.LabDecide(e, false), Decisions.LabDecide(e, null)));
        Assert.Equal(3, new[] { yes, no, never }.Distinct().Count());
        Assert.Empty(_completed);
        kiln.Step();
        Assert.Equal(DecisionTerminal.Allowed, Single(yes).Status);
        Assert.Equal(DecisionTerminal.Prevented, Single(no).Status);
        Assert.Equal(2, _completed.Count);
        kiln.Step(2);
        Assert.Equal(2, _completed.Count);
        kiln.Step(2);
        var late = Single(never);
        Assert.Equal((DecisionTerminal.TimeoutReleased, false), (late.Status, late.Ok));
        Assert.NotNull(late.Reason);
        Assert.Throws<ArgumentException>(() => kiln.Run(() => Decisions.LabDecide(EntityId.None, true)));
    }

    // A server whose mods use no decision event has no producer: the lab's decisions alone keep the dispatcher going,
    // until the last of them ended (the lab server of decisions-api-test is such a server).
    [Fact]
    public void Without_any_producer_the_lab_decisions_still_end()
    {
        DecisionBroker.Unregister(_death);
        DecisionBroker.Unregister(_input);
        using var kiln = Started();
        var e = kiln.Entities.Create();
        kiln.Step();
        Assert.False(kiln.Decisions.Model.Attached);  // nothing to dispatch: the consumer's place stays free
        var (yes, never) = kiln.Run(() => (Decisions.LabDecide(e, true), Decisions.LabDecide(e, null)));
        Assert.True(kiln.Decisions.Model.Attached);
        kiln.Step();
        Assert.Equal(DecisionTerminal.Allowed, Single(yes).Status);
        kiln.Step(4);
        Assert.Equal(DecisionTerminal.TimeoutReleased, Single(never).Status);
        Assert.Equal(2, _completed.Count);
    }

    [Fact]
    public void A_producer_that_throws_lets_the_game_go_on()
    {
        using var kiln = Started();
        var e = kiln.Entities.Create();
        On<TestDyingEvent>(d => d.Cancel());
        _death.Throw = true;
        var id = Capture(kiln, e);
        kiln.Step();
        Assert.True(Assert.Single(kiln.Decisions.Model.Answers).Allow);
        Assert.Equal(DecisionTerminal.Allowed, Single(id).Status);
        Assert.True(kiln.HasLog("DeathProducer threw"));
    }

    [Fact]
    public void Subscribing_to_an_event_whose_capability_is_off_throws_by_name()
    {
        using var kiln = Started();
        kiln.Capabilities.Override(DeathRow, false, "switched off by the test");
        var off = Assert.Throws<KilnUnavailableException>(() => Events.On<TestDyingEvent>(_ => { }));
        Assert.Equal(DeathRow, off.Capability);
        Assert.Contains("TestDyingEvent", off.Message);
        Assert.Throws<KilnUnavailableException>(() => Events.On<TestDyingEvent>(_ => { }, EventFilter.ForEntity(new EntityId(2000))));
        Assert.False(Events.HasSubscribers<TestDyingEvent>());  // nothing stayed subscribed
        // a handler of a base class is never refused, and the other kind is not touched
        On<CancelableEvent>(_ => { });
        On<TestInputEvent>(_ => { });
        kiln.Capabilities.ClearOverride(DeathRow);
        On<TestDyingEvent>(_ => { });

        // the service itself is the second thing the events need
        kiln.Capabilities.Override(N.Service, false);
        var none = Assert.Throws<KilnUnavailableException>(() => Events.On<TestDyingEvent>(_ => { }));
        Assert.Equal(DeathRow, none.Capability);  // the event's own row is the name, the reason says what is missing
        Assert.Contains("kiln.decisions", none.Message);
        kiln.Capabilities.ClearOverride(N.Service);
        // and a service without a producer of the kind (an older runtime): asked at the call, not remembered
        kiln.Decisions.Model.SupportedHookKinds = 0;
        Assert.Throws<KilnUnavailableException>(() => Events.On<TestDyingEvent>(_ => { }));
        kiln.Decisions.Model.SupportedHookKinds = 1ul << (int)N.KindDeath;
        On<TestDyingEvent>(_ => { });
    }

    private sealed class Handlers
    {
        public int Calls;
        [Subscribe] private void Ok(DecisionCompletedEvent e) => Calls++;
        [Subscribe] private void Dying(TestDyingEvent e) => Calls++;
    }

    [Fact]
    public void Register_subscribes_nothing_when_one_event_is_refused()
    {
        using var kiln = Started();
        kiln.Capabilities.Override(DeathRow, false);
        var before = Events.Subscriptions.Count;
        Assert.Throws<KilnUnavailableException>(() => Events.Register(new Handlers()));
        Assert.Equal(before, Events.Subscriptions.Count);
        kiln.Capabilities.ClearOverride(DeathRow);
        var target = new Handlers();
        Assert.Equal(2, Events.Register(target).Count);
        Assert.Equal(2, Events.Unregister(target));
    }

    [Fact]
    public void A_kinds_interest_follows_its_handlers()
    {
        using var kiln = Started();
        var model = kiln.Decisions.Model;
        var a = kiln.Entities.Create();
        var b = kiln.Entities.Create();
        string Interest() => model.InterestOf(N.KindDeath) is var i && i.All ? "all" : $"[{string.Join(",", model.InterestOf(N.KindDeath).Ids)}]";

        kiln.Step();
        Assert.Equal("[]", Interest());
        var one = kiln.Run(() => Events.On<TestDyingEvent>(_ => { }, EventFilter.ForEntity(b)));
        _subs.Add(one);
        Assert.Equal($"[{b.Value}]", Interest());  // subscribed on the tick thread in Pre: at once
        var two = Events.On<TestDyingEvent>(_ => { }, EventFilter.ForEntities([a, b]));  // from another thread: at the next Pre
        _subs.Add(two);
        kiln.Step();
        Assert.Equal($"[{a.Value},{b.Value}]", Interest());

        // a handler of a base class widens the kind to everything; when it goes, the filters are back
        var all = Events.On<KilnEvent>(_ => { });
        _subs.Add(all);
        kiln.Step();
        Assert.Equal("all", Interest());
        all.Dispose();
        kiln.Step();
        Assert.Equal($"[{a.Value},{b.Value}]", Interest());

        // the row off: nothing is held for the kind; on again: the same interest
        kiln.Capabilities.Override(DeathRow, false);
        kiln.Step();
        Assert.Equal("[]", Interest());
        kiln.Capabilities.ClearOverride(DeathRow);
        kiln.Step();
        Assert.Equal($"[{a.Value},{b.Value}]", Interest());

        // a kind the service has no producer for is not asked for
        model.SupportedHookKinds = 0;
        kiln.Step();
        Assert.Equal("[]", Interest());
        model.SupportedHookKinds = 1ul << (int)N.KindDeath;

        one.Dispose();
        two.Dispose();
        kiln.Step();
        Assert.Equal("[]", Interest());
    }

    [Fact]
    public void An_entity_is_resolved_in_Pre_and_refused_inside_the_simulation()
    {
        using var kiln = Started();
        var model = kiln.Decisions.Model;
        var e = kiln.Entities.Create(new WorldPosition(4, 5, 6));
        var bare = kiln.Entities.Create();

        var (rc, key, at) = kiln.Run(() => (Decisions.LabResolveEntity(e, out var k, out var p), k, p));
        Assert.Equal(N.Ok, rc);
        Assert.Equal((e.Value, true), (key.Entity, key.HasSpawnTime));
        Assert.NotEqual(0ul, key.LifeSerial);
        Assert.Equal(new WorldPosition(4, 5, 6), at);
        var again = kiln.Run(() => (Decisions.LabResolveEntity(e, out var k, out _), k).k);
        Assert.Equal(key, again);  // one life, one serial
        var other = kiln.Run(() => (Decisions.LabResolveEntity(bare, out var k, out _), k).k);
        Assert.NotEqual(key.LifeSerial, other.LifeSerial);

        Assert.Equal(1, kiln.Run(() => Decisions.LabWindow(out _, out _)));
        // the world thread inside the simulation: the clock says 3, and the service refuses
        var inSim = (Phase: -9, Result: -9);
        kiln.Run(() => model.InSimulation(() => inSim = (Decisions.LabWindow(out _, out _), Decisions.LabResolveEntity(e, out _, out _))));
        Assert.Equal((3, N.WrongThread), inSim);
        // another thread in Pre
        Assert.Equal(N.WrongThread, Decisions.LabResolveEntity(e, out _, out _));
        // no such entity: not found, and counted
        var misses = model.Counters.MetadataMisses;
        Assert.Equal(N.NotFound, kiln.Run(() => Decisions.LabResolveEntity(new EntityId(987654), out _, out _)));
        Assert.Equal(misses + 1, model.Counters.MetadataMisses);
        Assert.Equal(N.InvalidArgument, kiln.Run(() => Decisions.LabResolveEntity(EntityId.None, out _, out _)));

        var epoch = 0ul;
        var step = 0ul;
        kiln.Run(() => Decisions.LabWindow(out epoch, out step));
        Assert.Equal((1ul, kiln.CurrentStep), (epoch, step));
    }

    // The fake's table against the header's batch rules (chapter 07 §6, QUEUE-RETRY), called as a consumer calls it.
    [Fact]
    public void The_fakes_batches_are_frozen_not_consumed_by_a_read_and_acked_once()
    {
        DecisionBroker.Unregister(_death);  // no managed dispatcher in this test: the test is the consumer
        DecisionBroker.Unregister(_input);
        using var kiln = new KilnHarness();
        kiln.Step(2);
        var model = kiln.Decisions.Model;
        var t = (N.Table*)KitBackend.Instance.Service(N.Service, 1);
        const ulong owner = 4242;
        var e = kiln.Entities.Create();
        var mem = (byte*)System.Runtime.InteropServices.NativeMemory.AllocZeroed(16384);
        try
        {
            var batch = (ulong*)mem;
            var count = (uint*)(mem + 8);
            var required = (uint*)(mem + 16);
            var answer = (N.Answer*)(mem + 32);
            var receipt = (N.Receipt*)(mem + 128);
            var records = (N.Request*)(mem + 1024);    // room for 3
            var second = (N.Request*)(mem + 8192);
            var outcomes = (N.Outcome*)(mem + 12288);

            Assert.Equal(FakeTables.WrongThread, t->Attach(owner));  // not the world thread
            Assert.False(model.Attached);
            Assert.Equal(FakeTables.Unavailable, model.Capture(N.KindDeath, e.Value, 0, new byte[24], 0, 0, out _));  // no consumer: nothing is held
            Assert.Equal(FakeTables.Ok, kiln.Run(() => t->Attach(owner)));
            Assert.Equal(FakeTables.Unavailable, kiln.Run(() => t->Attach(owner + 1)));  // one consumer
            Assert.Equal(FakeTables.InvalidArgument, model.Capture(N.KindDeath, e.Value, 0, new byte[8], 0, 0, out _));  // not the kind's payload
            Assert.Equal(FakeTables.InvalidArgument, model.Capture(N.KindTerraformReserved, e.Value, 0, new byte[24], 0, 0, out _));

            Assert.Equal(FakeTables.Ok, model.Capture(N.KindDeath, e.Value, 0, new byte[24], 0, 0, out var a));
            Assert.Equal(FakeTables.Ok, model.Capture(N.KindDeath, e.Value, 0, new byte[24], 0, 0, out var b));
            kiln.Step();
            // freeze
            Assert.Equal(FakeTables.Ok, kiln.Run(() => t->BeginRequests(owner, batch, count)));
            var id = *batch;
            Assert.Equal((true, 2u), (id != 0, *count));
            Assert.Equal(FakeTables.Ok, model.Capture(N.KindDeath, e.Value, 0, new byte[24], 0, 0, out var c));  // the next batch's
            Assert.Equal(FakeTables.Ok, kiln.Run(() => t->BeginRequests(owner, batch, count)));
            Assert.Equal((id, 2u), (*batch, *count));  // begin again: the same batch
            // a probe and a too small read: the count, nothing written, nothing consumed
            Assert.Equal(FakeTables.BufferTooSmall, kiln.Run(() => t->ReadRequests(owner, id, null, 0, 0, required)));
            Assert.Equal(2u, *required);
            records[0].RequestId = 0xdead;
            Assert.Equal(FakeTables.BufferTooSmall, kiln.Run(() => t->ReadRequests(owner, id, records, 1, (uint)sizeof(N.Request), required)));
            Assert.Equal((2u, 0xdeadul), (*required, records[0].RequestId));
            Assert.Equal(FakeTables.InvalidArgument, kiln.Run(() => t->ReadRequests(owner, id, records, 2, 100, required)));  // a stride below the record
            Assert.Equal(FakeTables.NotFound, kiln.Run(() => t->ReadRequests(owner, id + 99, records, 2, (uint)sizeof(N.Request), required)));
            // the full read, twice: the same ids and bytes
            Assert.Equal(FakeTables.Ok, kiln.Run(() => t->ReadRequests(owner, id, records, 2, (uint)sizeof(N.Request), required)));
            Assert.Equal(FakeTables.Ok, kiln.Run(() => t->ReadRequests(owner, id, second, 3, (uint)sizeof(N.Request), required)));
            Assert.True(new ReadOnlySpan<byte>(records, 2 * sizeof(N.Request)).SequenceEqual(new ReadOnlySpan<byte>(second, 2 * sizeof(N.Request))));
            Assert.Equal((a.Request, b.Request), (records[0].RequestId, records[1].RequestId));
            var step = kiln.CurrentStep;
            Assert.Equal((1168u, 1u, N.KindDeath, 24u, 1ul, step - 1, step + 1, e.Value), (records[0].Size, records[0].Version, records[0].Kind,
                records[0].PayloadBytes, records[0].WorldEpoch, records[0].CaptureStep, records[0].DeadlineStep, records[0].Subject.Entity));
            // one answer; the same answer again is told the status and decides nothing
            *answer = new N.Answer { Size = (uint)sizeof(N.Answer), Kind = N.KindDeath, RequestId = a.Request, WorldEpoch = 1, Allow = 0, SurvivalHealth = 3 };
            Assert.Equal(FakeTables.Ok, kiln.Run(() => t->Answer(owner, answer, receipt)));
            Assert.Equal((1u, 2u), (receipt->Accepted, receipt->TerminalStatus));
            Assert.Equal(FakeTables.Ok, kiln.Run(() => t->Answer(owner, answer, receipt)));
            Assert.Equal((0u, 2u), (receipt->Accepted, receipt->TerminalStatus));
            Assert.Single(model.Answers);
            answer->WorldEpoch = 2;
            Assert.Equal(FakeTables.NotFound, kiln.Run(() => t->Answer(owner, answer, receipt)));  // the key is (epoch, id, kind)
            answer->WorldEpoch = 1;
            answer->RequestId = b.Request;
            answer->Kind = N.KindWalkover;
            Assert.Equal(FakeTables.InvalidArgument, kiln.Run(() => t->Answer(owner, answer, receipt)));
            Assert.Equal(2, model.ActiveCount);  // b and c are still held: a read consumed nothing, an ack will not either
            // ack; again within two steps; the next batch is the arrival
            Assert.Equal(FakeTables.Ok, kiln.Run(() => t->AckRequests(owner, id)));
            Assert.Equal(FakeTables.Ok, kiln.Run(() => t->AckRequests(owner, id)));
            Assert.Equal(2, model.ActiveCount);
            Assert.Equal(FakeTables.Ok, kiln.Run(() => t->BeginRequests(owner, batch, count)));
            Assert.Equal((true, 1u), (*batch > id, *count));
            var next = *batch;
            Assert.Equal(FakeTables.Ok, kiln.Run(() => t->ReadRequests(owner, next, records, 3, (uint)sizeof(N.Request), required)));
            Assert.Equal(c.Request, records[0].RequestId);
            Assert.Equal(FakeTables.Ok, kiln.Run(() => t->AckRequests(owner, next)));
            // outcomes: the same rules
            Assert.Equal(FakeTables.Ok, kiln.Run(() => t->BeginOutcomes(owner, batch, count)));
            var ob = *batch;
            Assert.Equal(1u, *count);
            Assert.Equal(FakeTables.BufferTooSmall, kiln.Run(() => t->ReadOutcomes(owner, ob, null, 0, 0, required)));
            Assert.Equal(FakeTables.Ok, kiln.Run(() => t->ReadOutcomes(owner, ob, outcomes, 1, (uint)sizeof(N.Outcome), required)));
            Assert.Equal((328u, N.KindDeath, 2u, a.Request, 3, e.Value), (outcomes[0].Size, outcomes[0].Kind, outcomes[0].Status, outcomes[0].RequestId,
                outcomes[0].RequestedHealth, outcomes[0].Subject.Entity));
            Assert.Equal(FakeTables.Ok, kiln.Run(() => t->AckOutcomes(owner, ob)));
            // b (captured at step - 1) is released at the window of capture + 3; c one step later; no answer is needed
            kiln.Step(2);
            Assert.Equal(2, model.ActiveCount);
            kiln.Step();
            Assert.Equal(1, model.ActiveCount);
            Assert.Equal(DecisionTerminal.TimeoutReleased, model.Record.Single(r => r.Id == b).Status);
            kiln.Step();
            Assert.Equal(0, model.ActiveCount);
            Assert.Equal(2ul, model.Counters.Timeouts);
            Assert.Equal(FakeTables.NotFound, kiln.Run(() => t->AckRequests(owner, id)));  // the repeated ack is taken for two steps only
            // detach releases what is held
            Assert.Equal(FakeTables.Ok, model.Capture(N.KindDeath, e.Value, 0, new byte[24], 0, 0, out var d));
            Assert.Equal(FakeTables.Unavailable, kiln.Run(() => t->Detach(owner + 1)));
            Assert.Equal(FakeTables.Ok, kiln.Run(() => t->Detach(owner)));
            Assert.Equal(DecisionTerminal.CapabilityReleased, model.Record.Single(r => r.Id == d).Status);
            Assert.Equal(FakeTables.Unavailable, kiln.Run(() => t->BeginRequests(owner, batch, count)));
            Assert.Equal(FakeTables.Unavailable, kiln.Run(() => t->ReevaluatePickup(0, 0, required)));
        }
        finally
        {
            System.Runtime.InteropServices.NativeMemory.Free(mem);
        }
    }
}
