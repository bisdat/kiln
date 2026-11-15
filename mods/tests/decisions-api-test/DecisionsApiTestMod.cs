// SPDX-License-Identifier: MIT
// Lab test of kiln.decisions and kiln.window_clock through Kiln.Api (07-cancelable-events, S3, S4): see kiln.mod.json.
// Player-free: it reads an entity the world already has, files decisions no game action hangs on, and kills spawned
// 1_Player templates to hold, refuse and allow their deaths (S4, events.death), and holds, refuses and allows walk-over
// pickups of spawned drops on the research's range-only fixture (S5, events.pickup). Both rows are on since 0.2.0-dev.29.
using Kiln;

namespace DecisionsApiTest;

public sealed class DecisionsApiTestMod : Mod
{
    private int _passes, _failures;
    private IDisposable? _program, _completed;

    // written by the world program's update (a game thread inside the simulation), read on the tick thread
    private int _target, _updates, _simPhase = -9, _simResult = -9, _simTickThread = -1, _simThread;
    private long _simEpoch, _simStep;

    private readonly Dictionary<DecisionId, List<(DecisionResult Result, ulong Step)>> _ends = [];
    private ulong _step;

    private void Check(bool ok, string message)
    {
        if (ok) _passes++;
        else _failures++;
        Log.Info($"decisions-api-test: {(ok ? "PASS" : "FAIL")} {message}");
    }

    public override void OnLoad()
    {
        try
        {
            _program = EcsPrograms.Bind("phase", InSimulation);
        }
        catch (Exception e)
        {
            Log.Warn($"decisions-api-test: no world program ({e.Message}); the refusal inside the simulation cannot be shown here");
        }
        _completed = Events.On<DecisionCompletedEvent>(e =>
        {
            if (!_ends.TryGetValue(e.Result.Id, out var list)) _ends[e.Result.Id] = list = [];
            list.Add((e.Result, _step));
        });
        Context.Tick.Pre += (in TickInfo t) => _step = t.Step;
        _ = Run();
    }

    public override void OnUnload()
    {
        _guard?.Dispose();
        _rangeTag?.Dispose();
        _interactionTag?.Dispose();
        _completed?.Dispose();
        _program?.Dispose();
    }

    // A world program's update: the game runs it inside its simulation step, on the world thread or on a worker.
    private void InSimulation(EcsProgramContext context)
    {
        Interlocked.Increment(ref _updates);
        PrepareFixtures(context);
        var target = (uint)Volatile.Read(ref _target);
        if (target == 0 || Volatile.Read(ref _simPhase) != -9) return;
        var phase = Decisions.LabWindow(out var epoch, out var step);
        var result = Decisions.LabResolveEntity(new EntityId(target), out _, out _);
        Volatile.Write(ref _simEpoch, (long)epoch);
        Volatile.Write(ref _simStep, (long)step);
        Volatile.Write(ref _simResult, result);
        Volatile.Write(ref _simTickThread, Game.IsTickThread ? 1 : 0);
        Volatile.Write(ref _simThread, Environment.CurrentManagedThreadId);
        Volatile.Write(ref _simPhase, phase);
    }

    private async Task Wait(int steps)
    {
        for (var i = 0; i < steps; i++) await Context.Scheduler.NextTick();
    }

    private async Task Run()
    {
        try
        {
            for (var n = 0; n < 3000 && !Spawn.WorldRunning; n++) await Context.Scheduler.NextTick();
            await Wait(120);
            Check(Capabilities.Is("kiln.decisions") && Capabilities.Is("kiln.window_clock"),
                $"the services are on ({Capabilities.Why("kiln.decisions")} {Capabilities.Why("kiln.window_clock")})");

            // Pre, tick thread
            var phase = Decisions.LabWindow(out var epoch, out var step);
            Log.Info($"decisions-api-test: clock in Pre: phase {phase} epoch {epoch} step {step} thread {Environment.CurrentManagedThreadId} (tick thread: {Game.IsTickThread})");
            Check(phase == 1 && epoch >= 1 && step > 0 && Game.IsTickThread, $"the clock says Pre on the tick thread (phase {phase}, epoch {epoch}, step {step})");

            // an entity the service can tell from the next user of its id: one with a SpawnTime (and a position to report)
            EntityId entity = default;
            foreach (var e in World.Find("SpawnTime"))
            {
                if (World.Position(e) is null) continue;
                entity = e;
                break;
            }
            if (entity.IsNone) throw new InvalidOperationException("the world has no entity with SpawnTime and a position");
            var rc = Decisions.LabResolveEntity(entity, out var key, out var at);
            Check(rc == 0 && key.Entity == entity.Value && key.LifeSerial != 0 && (key.HasSpawnTime || key.PlayerHandle != 0) && at is not null,
                $"resolve_entity answers in Pre: rc {rc}, entity {key.Entity}, spawn time {(key.HasSpawnTime ? key.SpawnTime.ToString() : "none")}, life {key.LifeSerial}, at {at}");
            await Wait(3);
            var rc2 = Decisions.LabResolveEntity(entity, out var key2, out _);
            Check(rc2 == 0 && key2 == key, $"the same life has the same key three steps later (life {key2.LifeSerial})");
            // an entity without SpawnTime that is no player has no usable identity: never a made-up one
            var bare = World.Find("Health").FirstOrDefault(e => e.Value > 1023 && World.ReadBytes(e, "SpawnTime") is null);
            if (!bare.IsNone)
            {
                var rcBare = Decisions.LabResolveEntity(bare, out _, out _);
                Check(rcBare == 2, $"an entity without SpawnTime is refused, not given an identity (entity {bare.Value}, rc {rcBare})");
            }
            else Log.Info("decisions-api-test: every entity with Health has a SpawnTime here; the no-identity refusal was not exercised");
            var none = Decisions.LabResolveEntity(new EntityId(0x7ffffff0), out _, out _);
            Check(none == 2, $"an entity that is not there is not found (rc {none})");

            // another thread while the tick thread is in Pre
            var away = Task.Run(() => (Decisions.LabResolveEntity(entity, out _, out _), Environment.CurrentManagedThreadId));
            while (!away.IsCompleted) Thread.Yield();
            Check(away.Result.Item1 == 4, $"resolve_entity is refused off the world thread (rc {away.Result.Item1}, thread {away.Result.Item2})");

            // inside the simulation: the world program's update asks
            if (_program is null) Check(false, "no world program was bound: the refusal inside the simulation was not shown");
            else
            {
                Volatile.Write(ref _target, (int)entity.Value);
                for (var n = 0; n < 200 && Volatile.Read(ref _simPhase) == -9; n++) await Context.Scheduler.NextTick();
                var simPhase = Volatile.Read(ref _simPhase);
                Log.Info($"decisions-api-test: clock in a world program's update: phase {simPhase} epoch {Volatile.Read(ref _simEpoch)} step {Volatile.Read(ref _simStep)} " +
                         $"thread {Volatile.Read(ref _simThread)} (tick thread: {Volatile.Read(ref _simTickThread) == 1}); {Volatile.Read(ref _updates)} updates");
                Check(simPhase == 3, $"the clock says phase 3 inside a world program's update (phase {simPhase})");
                Check(Volatile.Read(ref _simResult) == 4,
                    $"resolve_entity is refused in phase 3 (rc {Volatile.Read(ref _simResult)}, on the tick thread: {Volatile.Read(ref _simTickThread) == 1})");
                // once more with the game's entity programs forced onto its workers (the lab parallel switch)
                if (Kiln.Testing.LabSystems.Enabled)
                {
                    Kiln.Testing.LabSystems.ForceParallel(60);
                    await Wait(2);
                    Volatile.Write(ref _simPhase, -9);
                    for (var n = 0; n < 50 && Volatile.Read(ref _simPhase) == -9; n++) await Context.Scheduler.NextTick();
                    var onTick = Volatile.Read(ref _simTickThread) == 1;
                    Log.Info($"decisions-api-test: clock in a world program's update, parallel forced (mask 0x{Kiln.Testing.LabSystems.PlayersMask:x4}): phase {Volatile.Read(ref _simPhase)} " +
                             $"step {Volatile.Read(ref _simStep)} thread {Volatile.Read(ref _simThread)} (tick thread: {onTick})");
                    Check(Volatile.Read(ref _simPhase) == 3 && Volatile.Read(ref _simResult) == 4,
                        $"with parallel forced: phase {Volatile.Read(ref _simPhase)}, resolve_entity rc {Volatile.Read(ref _simResult)}, {(onTick ? "still on the tick thread" : "on a worker thread")}");
                    Kiln.Testing.LabSystems.StopParallel();
                    await Wait(2);
                }
                else Log.Info("decisions-api-test: the lab parallel switch is off (no kiln/control/systems.parallel): the worker-thread run was skipped");
            }

            // decisions through the real table: filed in Pre, delivered at the next Pre, each completed once
            var filedAt = _step;
            var yes = Decisions.LabDecide(entity, true);
            var no = Decisions.LabDecide(entity, false);
            var never = Decisions.LabDecide(entity, null);
            Check(yes.Request != 0 && no.Request > yes.Request && never.Request > no.Request && yes.WorldEpoch == epoch,
                $"three decisions were filed (epoch {yes.WorldEpoch}, requests {yes.Request}, {no.Request}, {never.Request})");
            await Wait(2);
            Check(Ended(yes) is { Count: 1 } a && a[0].Result is { Status: DecisionTerminal.Allowed, Ok: true, Reason: null } && a[0].Result.Subject == entity,
                $"an allowed decision ends Allowed, once ({Describe(yes)})");
            Check(Ended(no) is { Count: 1 } b && b[0].Result is { Status: DecisionTerminal.Prevented, Ok: true } && b[0].Result.Subject == entity,
                $"a refused decision ends Prevented, once ({Describe(no)})");
            Check(Ended(never) is null, "an unanswered decision is still held one step after its delivery");
            await Wait(6);
            var late = Ended(never);
            Check(late is { Count: 1 } && late[0].Result is { Status: DecisionTerminal.TimeoutReleased, Ok: false, Reason: not null },
                $"an unanswered decision is released after its deadline, once ({Describe(never)})");
            if (late is { Count: 1 })
                Check(late[0].Step == filedAt + 4, $"the release is told four steps after the filing: at the window of capture + 3 (filed {filedAt}, told {late[0].Step})");
            await Wait(4);
            Check(_ends.Count == 3 && _ends.Values.All(l => l.Count == 1), $"no decision completed twice ({_ends.Count} decisions, {_ends.Values.Sum(l => l.Count)} events)");

            // S4: cancelable death, once plain and once with the game's entity programs forced onto its workers
            await DeathSuite("plain");
            if (Kiln.Testing.LabSystems.Enabled)
            {
                Kiln.Testing.LabSystems.ForceParallel(2400);
                await Wait(2);
                Log.Info($"decisions-api-test: parallel forced for the death suite (mask 0x{Kiln.Testing.LabSystems.PlayersMask:x4})");
                try
                {
                    await DeathSuite("parallel");
                }
                finally
                {
                    Kiln.Testing.LabSystems.StopParallel();
                    await Wait(2);
                    Log.Info($"decisions-api-test: parallel stopped (mask 0x{Kiln.Testing.LabSystems.PlayersMask:x4})");
                }
            }
            else Log.Info("decisions-api-test: the lab parallel switch is off (no kiln/control/systems.parallel): the death suite ran once");
            // dev.29 (review P1 "health-zero interval"): what else acts on health 0 while a death is held, plain and
            // with the entity programs on the workers
            await IntervalSuite("plain");
            if (Kiln.Testing.LabSystems.Enabled)
            {
                Kiln.Testing.LabSystems.ForceParallel(2400);
                await Wait(2);
                try
                {
                    await IntervalSuite("parallel");
                }
                finally
                {
                    Kiln.Testing.LabSystems.StopParallel();
                    await Wait(2);
                }
            }
            // S5: cancelable walk-over pickup on the range-only fixture, plain and parallel forced
            await PickupSuite("plain");
            if (Kiln.Testing.LabSystems.Enabled)
            {
                Kiln.Testing.LabSystems.ForceParallel(2400);
                await Wait(2);
                try
                {
                    await PickupSuite("parallel");
                }
                finally
                {
                    Kiln.Testing.LabSystems.StopParallel();
                    await Wait(2);
                }
            }
            // S6: the typed equipment verdict, plain and parallel forced (the hold point runs before the game's programs)
            await EquipSuite("plain");
            if (Kiln.Testing.LabSystems.Enabled)
            {
                Kiln.Testing.LabSystems.ForceParallel(2400);
                await Wait(2);
                try
                {
                    await EquipSuite("parallel");
                }
                finally
                {
                    Kiln.Testing.LabSystems.StopParallel();
                    await Wait(2);
                }
            }
            Check(_ends.Values.All(l => l.Count == 1), $"no decision completed twice over the whole run ({_ends.Count} decisions)");
        }
        catch (Exception e)
        {
            Check(false, $"the run threw: {e}");
        }
        Log.Info($"decisions-api-test done: {_passes} PASS, {_failures} FAIL");
    }

    // ------------------------------------------------------------------------------------------------- deaths ----

    private static bool DeadMark(EntityId e) => World.ReadBytes(e, "Actor") is { Length: > 0xbf8 + 8 } b && (BitConverter.ToUInt64(b, 0xbf8) & 0x80) != 0;
    private static int DeathPending(EntityId e) => World.ReadBytes(e, "Actor") is { Length: > 0xeaa } b ? b[0xeaa] : -1;
    private static int Hp(EntityId e) => Combat.GetHealth(e)?.Current ?? int.MinValue;

    private DecisionResult? EndOf(Func<DecisionResult, bool> match) =>
        _ends.Values.SelectMany(l => l).Select(x => x.Result).LastOrDefault(match);

    // DEATH-VETO, DEATH-ALREADY-ALIVE, DEATH-FILTER (with the catch-all's widening and its disposal), the allowed
    // control. Spawned 1_Player templates (transient), no connected player.
    private async Task DeathSuite(string run)
    {
        var who = new List<EntityId>();
        var subs = new List<IDisposable>();
        try
        {
            Check(Capabilities.Is("events.death"), $"[{run}] events.death is on ({Capabilities.Why("events.death")})");
            if (!Capabilities.Is("events.death")) return;
            var at = new WorldPosition(3845, 835, 1480);
            for (var i = 0; i < 4; i++)
                who.Add(await Spawn.Entity("1_Player", new WorldPosition(at.X + 2 * i, at.Y, at.Z),
                    options: new SpawnOptions { Tag = "decisions-api-test", Flags = SpawnFlags.Transient }));
            await Wait(45);
            EntityId p = who[0], q = who[1], r = who[2], s = who[3];
            var max = Combat.GetHealth(p)?.Max ?? 0;
            Check(max > 0 && who.All(e => Hp(e) > 0 && !DeadMark(e)), $"[{run}] four characters alive ({string.Join(", ", who.Select(e => $"{e.Value}: {Hp(e)}"))}, max {max})");

            // DEATH-VETO: a filtered handler cancels; one event, one Prevented outcome, health 1, no raw dead or pending death
            var mode = "cancel";
            var dying = new List<EntityDyingEvent>();
            subs.Add(Events.On<EntityDyingEvent>(d =>
            {
                dying.Add(d);
                if (mode == "heal") Combat.SetHealth(d.Entity, d.MaximumHealth);
                if (mode != "allow")
                {
                    d.SurvivalHealth = 1;
                    d.Cancel();
                }
            }, EventFilter.ForEntity(p)));
            await Wait(1);
            Combat.SetHealth(p, 0);
            await Wait(5);
            var veto = dying.Count == 1 ? EndOf(x => x.Id == dying[0].Id) : null;
            Check(dying.Count == 1 && dying[0].Entity == p && dying[0].Health <= 0 && dying[0].MaximumHealth == max && dying[0].Key.LifeSerial != 0 &&
                  veto is { Status: DecisionTerminal.Prevented, Ok: true, RequestedHealth: 1, AppliedHealth: 1 } && Hp(p) == 1 && !DeadMark(p) && DeathPending(p) == 0,
                $"[{run}] DEATH-VETO: {dying.Count} event(s), outcome {veto?.Status} requested {veto?.RequestedHealth} before {veto?.HealthBefore} applied {veto?.AppliedHealth}, " +
                $"hp {Hp(p)}, raw dead {DeadMark(p)}, pending death {DeathPending(p)}");

            // DEATH-ALREADY-ALIVE: the handler heals to the maximum before the answer; nothing overwrites the heal
            mode = "heal";
            dying.Clear();
            Combat.SetHealth(p, 0);
            await Wait(5);
            var alive = dying.Count == 1 ? EndOf(x => x.Id == dying[0].Id) : null;
            Check(dying.Count == 1 && alive is { Status: DecisionTerminal.AlreadyAlive, Ok: true } && Hp(p) == max && !DeadMark(p),
                $"[{run}] DEATH-ALREADY-ALIVE: {dying.Count} event(s), outcome {alive?.Status} applied {alive?.AppliedHealth}: {alive?.Reason}, hp {Hp(p)} of {max}");

            // DEATH-FILTER: P and Q lethal, only P watched: Q is never held (no request, no outcome) and dies at once
            mode = "cancel";
            dying.Clear();
            var before = _ends.Count;
            Combat.SetHealth(p, 0);
            Combat.SetHealth(q, 0);
            await Wait(5);
            var aboutQ = EndOf(x => x.Subject == q);
            Check(dying.Count == 1 && dying[0].Entity == p && aboutQ is null && DeadMark(q) && Hp(p) == 1 && !DeadMark(p),
                $"[{run}] DEATH-FILTER: events {string.Join(",", dying.Select(d => d.Entity.Value))}, outcome about Q {aboutQ?.Status.ToString() ?? "none"}, " +
                $"Q raw dead {DeadMark(q)}, P hp {Hp(p)}, {_ends.Count - before} new outcome(s)");

            // a catch-all widens the interest to every death: R is held (and allowed); disposed, S is not held again
            var catchAll = Events.On<EntityDyingEvent>(_ => { });
            await Wait(1);
            Combat.SetHealth(r, 0);
            await Wait(5);
            var aboutR = EndOf(x => x.Subject == r);
            Check(aboutR is { Status: DecisionTerminal.Allowed, Ok: true } && DeadMark(r),
                $"[{run}] DEATH-FILTER widened by an unfiltered handler: R outcome {aboutR?.Status.ToString() ?? "none"}, R raw dead {DeadMark(r)}");
            catchAll.Dispose();
            await Wait(2);
            Combat.SetHealth(s, 0);
            await Wait(5);
            var aboutS = EndOf(x => x.Subject == s);
            Check(aboutS is null && DeadMark(s), $"[{run}] DEATH-FILTER narrowed again after the dispose: S outcome {aboutS?.Status.ToString() ?? "none"}, S raw dead {DeadMark(s)}");

            // the allowed control: nobody cancels, the game lets P die
            mode = "allow";
            dying.Clear();
            Combat.SetHealth(p, 0);
            await Wait(5);
            var allowed = dying.Count == 1 ? EndOf(x => x.Id == dying[0].Id) : null;
            Check(dying.Count == 1 && allowed is { Status: DecisionTerminal.Allowed, Ok: true } && DeadMark(p),
                $"[{run}] the allowed control: outcome {allowed?.Status}, P raw dead {DeadMark(p)}, hp {Hp(p)}");
            await Wait(3);
            Check(EndOf(x => x.Subject == p && x.Id != allowed?.Id && x.Id.Request > (allowed?.Id.Request ?? 0)) is null,
                $"[{run}] a corpse is not asked about again");
        }
        catch (Exception e)
        {
            Check(false, $"[{run}] the death suite threw: {e}");
        }
        finally
        {
            foreach (var sub in subs) sub.Dispose();
            foreach (var e in who)
                if (Spawn.Exists(e)) Spawn.Destroy(e);
            await Wait(3);
        }
    }

    // ------------------------------------------------------------------------------------------- the interval ----
    // A refused lethal loss leaves health at 0 from the lethal write until the answer in the next Pre (the game skips the
    // entity's death meanwhile). Does anything else of the game treat that 0 as dead? A forced-ready Vukah (its AI runs)
    // and a spawned 1_Player stand-in are each refused once; every step from the lethal write to 20 steps after the restore
    // records health, the Actor's raw dead bit (+0xbf8 bit 7), its pending death (+0xeaa) and +0xeab (the gate of the
    // enemy AI's Death condition), the enemy AI state, the AI's own state changes, Kiln.Combat's EntityDeathEvent and the
    // item drops near the body. A second Vukah whose death is allowed is the control: the same instruments must see it die.

    private static int ActorByte(EntityId e, int at) => World.ReadBytes(e, "Actor") is { } b && b.Length > at ? b[at] : -1;

    private static int DropsNear(EntityId e)
    {
        if (World.Position(e) is not { } at) return -1;
        var n = 0;
        foreach (var d in World.Find("PickupItem"))
            if (World.Position(d) is { } p && Math.Abs(p.X - at.X) < 12 && Math.Abs(p.Z - at.Z) < 12 && Math.Abs(p.Y - at.Y) < 12) n++;
        return n;
    }

    private sealed record Sample(ulong Step, string When, int Hp, bool Dead, int Pending, int Eab, string Ai, int Drops, bool Exists)
    {
        public override string ToString() => $"step {Step} {When}: hp {Hp}, raw dead {Dead}, pending death {Pending}, +0xeab {Eab}, AI {Ai}, drops near {Drops}, there {Exists}";
    }

    private Sample Take(EntityId e, string when, bool creature) => new(_step, when, Hp(e), DeadMark(e), DeathPending(e), ActorByte(e, 0xeab),
        creature ? Creatures.Of(e).State.ToString() : "-", DropsNear(e), Spawn.Exists(e));

    private async Task IntervalSuite(string run)
    {
        var mine = new List<EntityId>();
        var subs = new List<IDisposable>();
        var states = new List<(EntityId E, EnemyState From, EnemyState To, StateCondition Why, ulong Step)>();
        var deaths = new List<(EntityId E, ulong Step)>();
        try
        {
            if (!Capabilities.Is("events.death")) return;
            var at = new WorldPosition(3845, 835, 1490);
            var vukah = await Spawn.Entity("Enemy_Vukah_Melee", at, 0, new SpawnOptions { Tag = "decisions-api-test", Flags = SpawnFlags.Transient });
            var control = await Spawn.Entity("Enemy_Vukah_Melee", new WorldPosition(at.X + 30, at.Y, at.Z), 0,
                new SpawnOptions { Tag = "decisions-api-test", Flags = SpawnFlags.Transient });
            var stand = await Spawn.Entity("1_Player", new WorldPosition(at.X - 6, at.Y, at.Z), options: new SpawnOptions { Tag = "decisions-api-test", Flags = SpawnFlags.Transient });
            mine.AddRange([vukah, control, stand]);
            // the game marks an enemy ready for gameplay only near a player; the lab has none: force it (its AI runs then).
            // The body's components come some steps after the spawn.
            async Task<bool> Ready(EntityId e)
            {
                for (var i = 0; i < 60; i++)
                {
                    try
                    {
                        Creatures.Of(e).ForceReady();
                        return true;
                    }
                    catch (NotSupportedException) when (i < 59)
                    {
                        await Wait(1);
                    }
                }
                return false;
            }
            var ready = await Ready(vukah) & await Ready(control);
            subs.Add(Events.On<CreatureStateEvent>(e =>
            {
                if (e.Entity == vukah || e.Entity == control) states.Add((e.Entity, e.From, e.To, e.Condition, _step));
            }));
            subs.Add(Events.On<EntityDeathEvent>(e =>
            {
                if (mine.Contains(e.Victim)) deaths.Add((e.Victim, _step));
            }));
            for (var i = 0; i < 60 && (Creatures.Of(vukah).State is EnemyState.Spawning or EnemyState.Start or EnemyState.Invalid); i++) await Wait(1);
            await Wait(10);
            Check(ready && Hp(vukah) > 0 && Hp(stand) > 0 && Creatures.Of(vukah).State is not (EnemyState.Spawning or EnemyState.Dead or EnemyState.Invalid),
                $"[{run}] INTERVAL fixture: a Vukah with its AI running (forced ready {ready}, {Creatures.Of(vukah).State}, hp {Hp(vukah)}), a stand-in player (hp {Hp(stand)})");

            var held = new List<Sample>();
            var log = new List<(EntityId E, Sample S)>();
            var refused = new HashSet<EntityId> { vukah, stand };
            var dying = new List<EntityDyingEvent>();
            subs.Add(Events.On<EntityDyingEvent>(d =>
            {
                dying.Add(d);
                // the state after the whole lethal step, before the answer: what the interval left
                var c = d.Entity == vukah;
                held.Add(Take(d.Entity, "held, before the answer", c));
                if (!refused.Contains(d.Entity)) return;
                d.SurvivalHealth = 1;
                d.Cancel();
            }, EventFilter.ForEntity(vukah)));
            subs.Add(Events.On<EntityDyingEvent>(d => { if (d.Entity == stand) { dying.Add(d); held.Add(Take(d.Entity, "held, before the answer", false)); d.SurvivalHealth = 1; d.Cancel(); } },
                EventFilter.ForEntity(stand)));
            void Trail(in TickInfo t)
            {
                foreach (var e in (EntityId[])[vukah, stand])
                    if (Spawn.Exists(e)) log.Add((e, Take(e, "Post", e == vukah)));
            }
            var dropsBefore = DropsNear(vukah);
            var stepBefore = _step;
            Context.Tick.Post += Trail;
            try
            {
                await Wait(3);  // the Post handler is in from the next step on: the samples begin before the lethal write
                Combat.SetHealth(vukah, 0);
                Combat.SetHealth(stand, 0);
                await Wait(22);
            }
            finally
            {
                Context.Tick.Post -= Trail;
            }
            foreach (var (e, smp) in log.Take(14)) Log.Info($"decisions-api-test: [{run}] interval {(e == vukah ? "Vukah" : "stand-in")} {e.Value}: {smp}");
            foreach (var smp in held) Log.Info($"decisions-api-test: [{run}] interval held sample: {smp}");
            foreach (var x in states.Where(x => x.E == vukah)) Log.Info($"decisions-api-test: [{run}] interval Vukah AI {x.From} -> {x.To} ({x.Why}) at step {x.Step}");
            var vEnd = dying.Where(d => d.Entity == vukah).Select(d => EndOf(x => x.Id == d.Id)).LastOrDefault();
            var sEnd = dying.Where(d => d.Entity == stand).Select(d => EndOf(x => x.Id == d.Id)).LastOrDefault();
            var vLog = log.Where(x => x.E == vukah).Select(x => x.S).ToList();
            var sLog = log.Where(x => x.E == stand).Select(x => x.S).ToList();
            var vHeld = held.FirstOrDefault(h => h.Ai != "-");
            var sHeld = held.FirstOrDefault(h => h.Ai == "-");
            Check(vEnd is { Status: DecisionTerminal.Prevented } && sEnd is { Status: DecisionTerminal.Prevented },
                $"[{run}] INTERVAL both deaths were held and refused (Vukah {vEnd?.Status}, stand-in {sEnd?.Status})");
            Check(vHeld is { Hp: <= 0, Dead: false, Pending: 0 } && vHeld.Ai != nameof(EnemyState.Dead) && vHeld.Drops == dropsBefore,
                $"[{run}] INTERVAL Vukah after the lethal step, before the answer: {vHeld}");
            Check(vLog.Count > 0 && vLog.Any(x => x.Hp <= 0) && vLog.All(x => !x.Dead && x.Pending == 0 && x.Ai != nameof(EnemyState.Dead) && x.Exists && x.Drops == dropsBefore) &&
                  vLog.Last().Hp == 1 && !states.Any(x => x.E == vukah && (x.To == EnemyState.Dead || x.Why == StateCondition.Death)) &&
                  !deaths.Any(x => x.E == vukah),
                $"[{run}] INTERVAL Vukah over {vLog.Count} steps ({vLog.Count(x => x.Hp <= 0)} Post sample(s) at health 0): never raw dead, never pending death, AI never Dead (no Death condition), no " +
                $"EntityDeathEvent, no drop, health {vLog.FirstOrDefault()?.Hp} -> {vLog.LastOrDefault()?.Hp}; AI {string.Join(",", vLog.Select(x => x.Ai).Distinct())}");
            Check(sHeld is { Hp: <= 0, Dead: false, Pending: 0 } && sLog.Count > 0 && sLog.All(x => !x.Dead && x.Pending == 0 && x.Exists) && sLog.Last().Hp == 1 &&
                  !deaths.Any(x => x.E == stand),
                $"[{run}] INTERVAL stand-in player: held {sHeld}; over {sLog.Count} steps never raw dead or pending, no EntityDeathEvent, health -> {sLog.LastOrDefault()?.Hp}");

            // the control: the same instruments see an allowed death
            subs.Add(Events.On<EntityDyingEvent>(d => { if (d.Entity == control) dying.Add(d); }, EventFilter.ForEntity(control)));
            Combat.SetHealth(control, 0);
            await Wait(12);
            var cEnd = dying.Where(d => d.Entity == control).Select(d => EndOf(x => x.Id == d.Id)).LastOrDefault();
            foreach (var x in states.Where(x => x.E == control)) Log.Info($"decisions-api-test: [{run}] interval control AI {x.From} -> {x.To} ({x.Why}) at step {x.Step}");
            Check(cEnd is { Status: DecisionTerminal.Allowed } && DeadMark(control) && deaths.Any(x => x.E == control) &&
                  (Creatures.Of(control).State == EnemyState.Dead || states.Any(x => x.E == control && x.To == EnemyState.Dead)),
                $"[{run}] INTERVAL control: the allowed death is seen by the same instruments (outcome {cEnd?.Status}, raw dead {DeadMark(control)}, " +
                $"EntityDeathEvent {deaths.Any(x => x.E == control)}, AI {Creatures.Of(control).State}, drops near {DropsNear(control)})");
        }
        catch (Exception e)
        {
            Check(false, $"[{run}] the interval suite threw: {e}");
        }
        finally
        {
            foreach (var sub in subs) sub.Dispose();
            foreach (var e in mine)
                if (Spawn.Exists(e)) Spawn.Destroy(e);
            await Wait(3);
        }
    }

    // ------------------------------------------------------------------------------------------------ pickups ----
    // The range-only fixture of the research (chapter 07 §6, lab-tagged-kiln.log): a spawned drop loses its
    // InteractionOfferRuntime and gets PickupItemZone and DynamicIsPlayerInRange (queued from the world program), and a
    // lab-only mid hook in the game's walk-over program writes the receiver into the drop's DynamicIsPlayerInRange and
    // steps over the program's reserved-player-id check (a spawned character's id is above 1023). Server primitive test
    // only: no physical walking, no client. Tags: two counting hooks at the two helpers' entries (range, interaction).

    private const uint PickupItem = 0x2b0cf1d5;
    private readonly Dictionary<uint, uint> _inject = [];  // drop -> receiver to write (0: write 0 and let the check skip)
    private readonly HashSet<uint> _prepare = [];          // drops to make range-only in the next world-program update
    private readonly object _fixtureLock = new();
    private long _rangeHits, _interactionHits;
    private HookHandle? _guard, _rangeTag, _interactionTag;
    private nuint _guardJump;
    private unsafe delegate* unmanaged<void*, uint*, uint*> _currentEntity;

    // the world program's update: the fixture's component changes are queued from inside the simulation
    private void PrepareFixtures(EcsProgramContext context)
    {
        uint[] drops;
        lock (_fixtureLock)
        {
            if (_prepare.Count == 0) return;
            drops = [.. _prepare];
            _prepare.Clear();
        }
        foreach (var d in drops)
        {
            var e = new EntityId(d);
            context.QueueRemove(e, "InteractionOfferRuntime");
            context.QueueAdd(e, "PickupItemZone", new byte[8]);
            context.QueueAdd(e, "DynamicIsPlayerInRange", new byte[16]);
        }
    }

    private static readonly byte?[] GuardBytes =
        [0x48, 0x8B, 0x44, 0x24, 0x48, 0x8B, 0x40, 0x04, 0x85, 0xC0, 0x0F, 0x84, null, null, null, null, 0xFF, 0xC8, 0x3D, 0xFE, 0x03, 0x00, 0x00, 0x0F, 0x87];

    private unsafe bool InstallFixture()
    {
        var program = Symbols.Get("events/LootRangeProgram");
        var at = program + 0x70;  // the reserved-player-id check of the build the research read (bytes checked below)
        var code = (byte*)at;
        for (var i = 0; i < GuardBytes.Length; i++)
            if (GuardBytes[i] is { } b && code[i] != b)
            {
                Log.Warn($"decisions-api-test: the walk-over program's receiver check is not where the fixture expects it (byte {i}); the pickup suite cannot run on this build");
                return false;
            }
        _guardJump = at + 0x1d;  // past the check: mov rcx,[rbp-0x50]
        _currentEntity = (delegate* unmanaged<void*, uint*, uint*>)Symbols.Get("world/CurrentEntityId");
        _guard = Hooks.Mid(at, (ref Registers r) =>
        {
            uint out_ = 0;
            var drop = *_currentEntity((void*)r.Rdi, &out_);
            uint receiver;
            lock (_fixtureLock)
                if (!_inject.TryGetValue(drop, out receiver)) return;
            var range = *(byte**)(r.Rsp + 0x48);
            *(uint*)(range + 4) = receiver;
            if (receiver != 0) r.Rip = _guardJump;
        }, priority: 300);
        _rangeTag = Hooks.Mid("events/PickupInRange", (ref Registers r) =>
        {
            if (Watched(*(uint*)(r.Rsp + 0x38))) Interlocked.Increment(ref _rangeHits);
        }, priority: 300);
        _interactionTag = Hooks.Mid("events/PickupToInventory", (ref Registers r) =>
        {
            if (Watched(*(uint*)(r.Rsp + 0x38))) Interlocked.Increment(ref _interactionHits);
        }, priority: 300);
        return true;
    }

    private uint _receiver;
    private bool Watched(uint receiver) => receiver != 0 && receiver == Volatile.Read(ref _receiver);

    private static long Count(EntityId who) => Items.Read(who)?.Slots.Where(s => s.Item.Value == PickupItem).Sum(s => (long)s.Count) ?? -1;

    private async Task<EntityId> RangeDrop(WorldPosition at)
    {
        var d = await Spawn.Item(PickupItem, 3, at, options: new SpawnOptions { Tag = "decisions-api-test", Flags = SpawnFlags.Transient });
        lock (_fixtureLock) _prepare.Add(d.Value);
        await Wait(4);
        return d;
    }

    private void Inject(EntityId drop, EntityId receiver)
    {
        lock (_fixtureLock) _inject[drop.Value] = receiver.Value;
    }

    private void StopInject(EntityId drop)
    {
        lock (_fixtureLock) _inject.Remove(drop.Value);
    }

    // PICKUP-RANGE-ONLY, PICKUP-KEYS, PICKUP-EXPOSURE.
    private async Task PickupSuite(string run)
    {
        var made = new List<EntityId>();
        var subs = new List<IDisposable>();
        try
        {
            Check(Capabilities.Is("events.pickup"), $"[{run}] events.pickup is on ({Capabilities.Why("events.pickup")})");
            if (!Capabilities.Is("events.pickup")) return;
            if (_guard is null && !InstallFixture())
            {
                Check(false, $"[{run}] the range-only fixture could not be installed");
                return;
            }
            var p = await Spawn.Entity("1_Player", new WorldPosition(3845, 835, 1480), options: new SpawnOptions { Tag = "decisions-api-test", Flags = SpawnFlags.Transient });
            made.Add(p);
            Volatile.Write(ref _receiver, p.Value);
            for (var n = 0; n < 200 && Items.Read(p) is not { Initialized: true }; n++) await Wait(1);
            await Wait(10);
            Check(Items.Read(p) is { Initialized: true }, $"[{run}] receiver {p.Value} has an inventory");

            var seen = new List<ItemPickingUpEvent>();
            Func<ItemPickingUpEvent, bool> cancel = _ => true;
            subs.Add(Events.On<ItemPickingUpEvent>(e =>
            {
                seen.Add(e);
                if (cancel(e)) e.Cancel();
            }, EventFilter.ForEntity(p)));

            // the control: a second character nobody listens for picks up the same kind of drop unheld. What it gains is
            // what one pickup gives on this server (the helper scales the drop by resourceDropStackAmountFactor), and
            // no event is posted for it: a pickup outside every filter passes Kiln's hook untouched
            var q = await Spawn.Entity("1_Player", new WorldPosition(3845, 835, 1478), options: new SpawnOptions { Tag = "decisions-api-test", Flags = SpawnFlags.Transient });
            made.Add(q);
            for (var n = 0; n < 200 && Items.Read(q) is not { Initialized: true }; n++) await Wait(1);
            await Wait(10);
            var d0 = await RangeDrop(new WorldPosition(3846, 835, 1478));
            made.Add(d0);
            var q0 = Count(q);
            Inject(d0, q);
            await Wait(8);
            StopInject(d0);
            var grant = Count(q) - q0;
            Check(grant > 0 && grant % 3 == 0 && !Spawn.Exists(d0) && !seen.Any(e => e.GroundEntity == d0),
                $"[{run}] the unheld control: a pickup by a character no handler names passes (drop there {Spawn.Exists(d0)}, inventory {q0}->{Count(q)}, " +
                $"{seen.Count(e => e.GroundEntity == d0)} event(s)): one pickup of 3 gives {grant} here");

            // PICKUP-RANGE-ONLY: the range helper is reached (tag), the interaction helper is not, a cancel leaves
            // inventory and drop as they were; ReevaluatePickup and an allow then grant it once
            var d1 = await RangeDrop(new WorldPosition(3846, 835, 1480));
            made.Add(d1);
            var have = Count(p);
            var (range0, inter0) = (Interlocked.Read(ref _rangeHits), Interlocked.Read(ref _interactionHits));
            Inject(d1, p);
            await Wait(8);
            var first = seen.FirstOrDefault(e => e.GroundEntity == d1);
            var firstEnd = first is null ? null : EndOf(x => x.Id == first.Id);
            var (range1, inter1) = (Interlocked.Read(ref _rangeHits), Interlocked.Read(ref _interactionHits));
            Check(first is { Receiver: var rr, Item.Value: PickupItem, Amount: 3, ExposureEpoch: > 0, Position: not null } && rr == p &&
                  firstEnd is { Status: DecisionTerminal.Prevented, Ok: true } && firstEnd.Subject == d1 && firstEnd.Actor == p &&
                  range1 > range0 && inter1 == inter0 && Spawn.Exists(d1) && Count(p) == have,
                $"[{run}] PICKUP-RANGE-ONLY refused: event {(first is null ? "none" : $"drop {first.GroundEntity.Value} by {first.Receiver.Value}, item {first.Item.Value:x8} x{first.Amount}, exposure {first.ExposureEpoch}, at {first.Position}")}, " +
                $"outcome {firstEnd?.Status} subject {firstEnd?.Subject.Value} actor {firstEnd?.Actor.Value}, range tag {range0}->{range1}, interaction tag {inter0}->{inter1}, " +
                $"drop there {Spawn.Exists(d1)}, inventory {have}->{Count(p)}");
            var events = seen.Count;
            await Wait(5);
            Check(seen.Count == events && Spawn.Exists(d1) && Count(p) == have && Interlocked.Read(ref _rangeHits) > range1,
                $"[{run}] PICKUP-RANGE-ONLY the canceled exposure is refused again without asking ({seen.Count - events} new events, range tag {range1}->{Interlocked.Read(ref _rangeHits)})");
            cancel = _ => false;
            var re = Decisions.ReevaluatePickup(d1, p);
            await Wait(8);
            var second = seen.LastOrDefault(e => e.GroundEntity == d1);
            Check(re is { Ok: true, ExposuresReset: 1 } && second is not null && second.ExposureEpoch > first!.ExposureEpoch && EndOf(x => x.Id == second.Id) is { Status: DecisionTerminal.Allowed } &&
                  !Spawn.Exists(d1) && Count(p) == have + grant && Interlocked.Read(ref _interactionHits) == inter0,
                $"[{run}] PICKUP-RANGE-ONLY allowed after ReevaluatePickup: reset {re.ExposuresReset} ({re.Reason}), exposure {first?.ExposureEpoch}->{second?.ExposureEpoch}, " +
                $"outcome {(second is null ? "none" : EndOf(x => x.Id == second.Id)?.Status.ToString())}, drop there {Spawn.Exists(d1)}, inventory {have}->{Count(p)} (once: +{grant}), interaction tag {Interlocked.Read(ref _interactionHits) - inter0}");
            StopInject(d1);

            // PICKUP-KEYS: two drops of the same item spawned together; allowing one never allows the other
            var t2 = Spawn.Item(PickupItem, 3, new WorldPosition(3846, 835, 1481), options: new SpawnOptions { Tag = "decisions-api-test", Flags = SpawnFlags.Transient });
            var t3 = Spawn.Item(PickupItem, 3, new WorldPosition(3846, 835, 1479), options: new SpawnOptions { Tag = "decisions-api-test", Flags = SpawnFlags.Transient });
            var d2 = await t2;
            var d3 = await t3;
            made.Add(d2);
            made.Add(d3);
            lock (_fixtureLock)
            {
                _prepare.Add(d2.Value);
                _prepare.Add(d3.Value);
            }
            await Wait(4);
            cancel = e => e.GroundEntity == d3;
            have = Count(p);
            Inject(d2, p);
            Inject(d3, p);
            await Wait(10);
            var e2 = seen.LastOrDefault(e => e.GroundEntity == d2);
            var e3 = seen.LastOrDefault(e => e.GroundEntity == d3);
            Check(e2 is not null && e3 is not null && e2.GroundKey != e3.GroundKey && e2.GroundKey.LifeSerial != e3.GroundKey.LifeSerial && e2.ExposureEpoch != e3.ExposureEpoch &&
                  EndOf(x => x.Id == e2.Id) is { Status: DecisionTerminal.Allowed } && EndOf(x => x.Id == e3.Id) is { Status: DecisionTerminal.Prevented } &&
                  !Spawn.Exists(d2) && Spawn.Exists(d3) && Count(p) == have + grant,
                $"[{run}] PICKUP-KEYS: spawn times {e2?.GroundKey.SpawnTime} / {e3?.GroundKey.SpawnTime}, lives {e2?.GroundKey.LifeSerial} / {e3?.GroundKey.LifeSerial}, " +
                $"exposures {e2?.ExposureEpoch} / {e3?.ExposureEpoch}, allowed drop there {Spawn.Exists(d2)}, refused drop there {Spawn.Exists(d3)}, inventory {have}->{Count(p)} (one grant: +{grant})");
            StopInject(d2);

            // PICKUP-EXPOSURE: receiver 0 at a sample closes d3's exposure; the next contact is a new epoch the old cancel
            // does not suppress; ReevaluatePickup then resets exactly one active exposure
            var old = e3!.ExposureEpoch;
            lock (_fixtureLock) _inject[d3.Value] = 0;
            await Wait(4);
            var quiet = seen.Count;
            Inject(d3, p);
            await Wait(8);
            var e4 = seen.LastOrDefault(e => e.GroundEntity == d3);
            Check(seen.Count == quiet + 1 && e4 is not null && e4.ExposureEpoch > old && EndOf(x => x.Id == e4.Id) is { Status: DecisionTerminal.Prevented } && Spawn.Exists(d3),
                $"[{run}] PICKUP-EXPOSURE: receiver 0 then back: exposure {old}->{e4?.ExposureEpoch}, {seen.Count - quiet} new event(s), outcome {(e4 is null ? "none" : EndOf(x => x.Id == e4.Id)?.Status.ToString())}");
            var none = Decisions.ReevaluatePickup(d2, p);
            var one = Decisions.ReevaluatePickup(EntityId.None, p);
            Check(none is { Ok: false, Reason: "NoActiveExposure", ExposuresReset: 0 } && one is { Ok: true, ExposuresReset: 1 },
                $"[{run}] PICKUP-EXPOSURE ReevaluatePickup: a gone drop {none.Ok}/{none.Reason}, any drop of the receiver reset {one.ExposuresReset}");
            cancel = _ => false;
            await Wait(8);
            var e5 = seen.LastOrDefault(e => e.GroundEntity == d3);
            Check(e5 is not null && e5.ExposureEpoch > e4!.ExposureEpoch && !Spawn.Exists(d3),
                $"[{run}] PICKUP-EXPOSURE the reopened exposure asks again and is allowed: exposure {e4?.ExposureEpoch}->{e5?.ExposureEpoch}, drop there {Spawn.Exists(d3)}");
            StopInject(d3);
        }
        catch (Exception e)
        {
            Check(false, $"[{run}] the pickup suite threw: {e}");
        }
        finally
        {
            Volatile.Write(ref _receiver, 0u);
            lock (_fixtureLock) _inject.Clear();
            foreach (var sub in subs) sub.Dispose();
            foreach (var e in made)
                if (Spawn.Exists(e)) Spawn.Destroy(e);
            await Wait(3);
        }
    }

    // ------------------------------------------------------------------------------------------------ equipment ----
    // A spawned 1_Player (watched by kiln.playerstate) gets two rings and arrows; the lab writes the game's own inputs into
    // its PlayerInput in Pre, as a client's would arrive (playerstate-test's way): changeEquipmentAction at
    // PlayerInput.fromClient +0x1c0 {slot u64, version u32, equipment slot u8, type u8}, actionbarSelectionAction at +0x60
    // {version u32, index u8}. EQUIP-OR-VETO, EQUIP-STACK, an unequip, and the counter: an unrelated client action is
    // not held while only a typed listener holds equipment inputs.

    private const uint RingLight = 0x79442800, RingShock = 0x4623bcb0;
    private const int ChangeAction = 0x08 + 0x1c0, BarAction = 0x08 + 0x60;
    private nuint _world;

    private static unsafe nuint ComponentOf(nuint world, string comp, EntityId e)
    {
        ulong* r = stackalloc ulong[6];
        for (var i = 0; i < 6; i++) r[i] = 0;
        ((delegate* unmanaged<ulong*, nuint, uint, uint, ulong*>)Symbols.Get("GetComponent"))(r, world, World.ComponentHash(comp), e.Value);
        return (nuint)r[0];
    }

    private unsafe uint SendChange(EntityId e, SlotId slot, byte gameSlot, byte type)
    {
        var pi = ComponentOf(_world, "PlayerInput", e);
        var v = *(uint*)(pi + ChangeAction + 8) + 1;
        *(ulong*)(pi + ChangeAction) = slot.Value;
        *(byte*)(pi + ChangeAction + 0xc) = gameSlot;
        *(byte*)(pi + ChangeAction + 0xd) = type;
        *(uint*)(pi + ChangeAction + 8) = v;
        return v;
    }

    private unsafe void SendBar(EntityId e, byte index)
    {
        var pi = ComponentOf(_world, "PlayerInput", e);
        *(byte*)(pi + BarAction + 4) = index;
        *(uint*)(pi + BarAction) = *(uint*)(pi + BarAction) + 1;
    }

    private static uint ConsumedChange(EntityId e) => World.ReadBytes(e, "ServerConsumedPlayerInput") is { Length: >= 0x7c } b ? BitConverter.ToUInt32(b, 0x78) : 0;

    private static uint Ring1(EntityId e) => PlayerStates.EquipmentOf(e)?[PlayerEquipSlot.Ring1].Id ?? 0;

    private async Task EquipSuite(string run)
    {
        var made = new List<EntityId>();
        var subs = new List<IDisposable>();
        try
        {
            Check(Capabilities.Is("events.equip"), $"[{run}] events.equip is on ({Capabilities.Why("events.equip")})");
            if (!Capabilities.Is("events.equip")) return;
            if (_world == 0)
            {
                var hook = Hooks.Detour("GetComponent", c => { if (_world == 0) _world = (nuint)c[1]; return c.CallOriginal(); });
                for (var n = 0; n < 200 && _world == 0; n++) await Wait(1);
                hook.Remove();
            }
            if (_world == 0)
            {
                Check(false, $"[{run}] no world pointer for the input writes");
                return;
            }
            var kinds = GameTypes.Available ? GameTypes.Find("keen::ecs::ChangeEquipment")?.EnumValues : null;
            Log.Info($"decisions-api-test: keen::ecs::ChangeEquipment = {(kinds is null ? "unknown" : string.Join(", ", kinds.Select(k => $"{k.Name} {k.Value}")))}");
            var unequip = (byte)(kinds?.Where(k => k.Name.Contains("nequip", StringComparison.OrdinalIgnoreCase)).Select(k => (long?)k.Value).FirstOrDefault() ?? 1);
            var p = await Spawn.Entity("1_Player", new WorldPosition(3845, 835, 1476), options: new SpawnOptions { Tag = "decisions-api-test", Flags = SpawnFlags.Transient });
            made.Add(p);
            PlayerStates.Diagnostics.Watch(p);
            for (var n = 0; n < 200 && Items.Read(p) is not { Initialized: true }; n++) await Wait(1);
            await Items.Give(p, new ItemId(RingLight), 1);
            await Items.Give(p, new ItemId(RingShock), 1);
            // a player's arrows (not an enemy projectile's ammunition, which the game does not put in the arrow slot)
            var arrowInfo = Items.All.Where(i => i.Category == ItemCategory.Ammunition && i.Name.Contains("Arrow", StringComparison.OrdinalIgnoreCase) &&
                                                 !i.Name.Contains("Enemy", StringComparison.OrdinalIgnoreCase))
                .OrderBy(i => i.Name.Length).FirstOrDefault();
            if (arrowInfo is not null) await Items.Give(p, arrowInfo.Id, 20);
            await Wait(12);
            var start = PlayerStates.EquipmentOf(p);
            Log.Info($"decisions-api-test: [{run}] equipment at the start: ring0 0x{start?[PlayerEquipSlot.Ring0].Id:x8}, ring1 0x{start?[PlayerEquipSlot.Ring1].Id:x8}, arrow 0x{start?[PlayerEquipSlot.Arrow].Id:x8}; arrows {arrowInfo?.Name ?? "none"}");

            var generic = new List<PlayerClientActionEvent>();
            var typed = new List<PlayerEquippingEvent>();
            var genericCancel = false;
            Func<PlayerEquippingEvent, bool?> typedVote = _ => null;  // null: leave it, true: cancel, false: un-cancel
            subs.Add(Events.On<PlayerClientActionEvent>(e =>
            {
                if (e.Player.Entity != p || e.Name != "changeEquipmentAction") return;
                generic.Add(e);
                if (genericCancel) e.Cancel();
            }));
            subs.Add(Events.On<PlayerEquippingEvent>(e =>
            {
                typed.Add(e);
                if (typedVote(e) is { } v) e.Canceled = v;
            }, EventFilter.ForEntity(p), receiveCanceled: true));
            await Wait(2);

            // the ring that is not in Ring1: a run of the input changes Ring1
            SlotId RingSlot(uint other) => Items.Read(p)?.Slots.FirstOrDefault(s => s.Item.Value == other)?.Slot ?? default;
            async Task<(PlayerClientActionEvent? G, PlayerEquippingEvent? T, DecisionResult? End, uint Version, uint Before, uint After)> Attempt()
            {
                generic.Clear();
                typed.Clear();
                var before = Ring1(p);
                var other = before == RingLight ? RingShock : RingLight;
                var v = SendChange(p, RingSlot(other), 11, 0);  // keen::EquipmentSlot.Ring1, equip
                await Wait(6);
                var t = typed.LastOrDefault();
                return (generic.LastOrDefault(), t, t is null ? null : EndOf(x => x.Id == t.Id), v, before, Ring1(p));
            }

            // EQUIP-OR-VETO: the generic phase vetoes, a typed handler un-cancels: one native cancel
            genericCancel = true;
            typedVote = _ => false;
            var a = await Attempt();
            Check(a.G is { CanCancel: true } && a.T is { PriorCanceled: true, Canceled: false } && a.G.DecisionId == a.T.Id && a.T.Id.Request != 0 &&
                  a.End is { Status: DecisionTerminal.Prevented } && a.After == a.Before && ConsumedChange(p) == a.Version,
                $"[{run}] EQUIP-OR-VETO generic veto, typed un-cancel: generic id {a.G?.DecisionId.Request}, typed id {a.T?.Id.Request} prior {a.T?.PriorCanceled} by {a.T?.PriorCanceledBy}, " +
                $"outcome {a.End?.Status}, ring1 0x{a.Before:x8}->0x{a.After:x8}, consumed {ConsumedChange(p)} of v{a.Version}");
            // the reverse: the generic phase lets it be, the typed handler vetoes
            genericCancel = false;
            typedVote = _ => true;
            var b = await Attempt();
            Check(b.G is not null && b.T is { PriorCanceled: false, Canceled: true } && b.G.DecisionId == b.T.Id && b.End is { Status: DecisionTerminal.Prevented } &&
                  b.After == b.Before && ConsumedChange(p) == b.Version,
                $"[{run}] EQUIP-OR-VETO typed veto: outcome {b.End?.Status}, change {b.T?.Change}, ring1 0x{b.Before:x8}->0x{b.After:x8}, consumed {ConsumedChange(p)} of v{b.Version}");
            // the control: nobody vetoes, the game runs it (one step late)
            typedVote = _ => null;
            var c = await Attempt();
            Check(c.T is { Canceled: false, Change: SingleEquipmentChange { Operation: EquipmentChangeKind.Equip, Slot: EquipmentSlot.Ring1, Selection: InstanceEquipment } } &&
                  c.End is { Status: DecisionTerminal.Allowed } && c.After != c.Before && c.After != 0,
                $"[{run}] EQUIP-OR-VETO allowed control: outcome {c.End?.Status}, change {c.T?.Change}, ring1 0x{c.Before:x8}->0x{c.After:x8}");

            // an unequip names what is in the slot now; refused, the ring stays
            typed.Clear();
            typedVote = _ => true;
            var worn = Ring1(p);
            SendChange(p, default, 11, unequip);
            await Wait(6);
            var u = typed.LastOrDefault();
            Check(u is { Change: SingleEquipmentChange { Operation: EquipmentChangeKind.Unequip, Slot: EquipmentSlot.Ring1, Selection: InstanceEquipment ui } } && ui.Item.Item.Value == worn &&
                  EndOf(x => x.Id == u.Id) is { Status: DecisionTerminal.Prevented } && Ring1(p) == worn,
                $"[{run}] an unequip names the worn ring and is refused: change {u?.Change}, worn 0x{worn:x8}, ring1 now 0x{Ring1(p):x8}");

            // EQUIP-STACK: arrows are a plain stack: item, source slot and count, no instance as identity
            if (arrowInfo is null) Check(false, $"[{run}] EQUIP-STACK: this build has no arrow item to equip");
            else
            {
                typed.Clear();
                typedVote = _ => null;
                var stack = Items.Read(p)?.Slots.FirstOrDefault(s => s.Item == arrowInfo.Id && !s.IsEquipped) ?? Items.Read(p)?.Slots.FirstOrDefault(s => s.Item == arrowInfo.Id);
                if (stack is not null)
                {
                    SendChange(p, stack.Slot, 14, 0);  // keen::EquipmentSlot.Arrow
                    await Wait(6);
                }
                var st = typed.LastOrDefault();
                Check(stack is not null && st is { Change: SingleEquipmentChange { Slot: EquipmentSlot.Arrow, Selection: StackEquipment se } } && se.Item == arrowInfo.Id &&
                      se.Source == stack.Slot && se.ExpectedCount == stack.Count && EndOf(x => x.Id == st.Id) is { Status: DecisionTerminal.Allowed } &&
                      PlayerStates.EquipmentOf(p)?[PlayerEquipSlot.Arrow].Id == arrowInfo.Id.Value,
                    $"[{run}] EQUIP-STACK: change {st?.Change}, stack {stack?.Slot} x{stack?.Count}, outcome {(st is null ? "none" : EndOf(x => x.Id == st.Id)?.Status.ToString())}, arrow slot 0x{PlayerStates.EquipmentOf(p)?[PlayerEquipSlot.Arrow].Id:x8}");
            }

            // the counter: with only the typed listener holding, an action bar input runs in the step it arrives in;
            // with a generic listener (the legacy hold of every action) it runs one step later
            foreach (var sub in subs) sub.Dispose();
            subs.Clear();
            subs.Add(Events.On<PlayerEquippingEvent>(_ => { }, EventFilter.ForEntity(p)));
            await Wait(3);
            var bar0 = PlayerStates.EquipmentOf(p)?.HotbarIndex ?? -1;
            SendBar(p, (byte)(bar0 == 1 ? 2 : 1));
            await Wait(1);
            var typedOnly = PlayerStates.EquipmentOf(p)?.HotbarIndex ?? -1;
            subs.Add(Events.On<PlayerClientActionEvent>(_ => { }));
            await Wait(3);
            var bar1 = PlayerStates.EquipmentOf(p)?.HotbarIndex ?? -1;
            SendBar(p, (byte)(bar1 == 1 ? 2 : 1));
            await Wait(1);
            var heldOneStep = PlayerStates.EquipmentOf(p)?.HotbarIndex ?? -1;
            await Wait(2);
            var heldLater = PlayerStates.EquipmentOf(p)?.HotbarIndex ?? -1;
            Check(typedOnly != bar0 && heldOneStep == bar1 && heldLater != bar1,
                $"[{run}] no unrelated hold: an action bar input with only a typed equipment listener ran at once ({bar0}->{typedOnly} after 1 step); with a generic listener it waited a step ({bar1}->{heldOneStep} after 1, {heldLater} after 3)");
        }
        catch (Exception e)
        {
            Check(false, $"[{run}] the equipment suite threw: {e}");
        }
        finally
        {
            foreach (var sub in subs) sub.Dispose();
            foreach (var e in made)
            {
                PlayerStates.Diagnostics.Watch(e, false);
                if (Spawn.Exists(e)) Spawn.Destroy(e);
            }
            await Wait(3);
        }
    }

    private List<(DecisionResult Result, ulong Step)>? Ended(DecisionId id) => _ends.TryGetValue(id, out var list) ? list : null;

    private string Describe(DecisionId id) =>
        Ended(id) is { } list ? string.Join("; ", list.Select(x => $"{x.Result.Status} at step {x.Step}{(x.Result.Reason is { } r ? $": {r}" : "")}")) : "no event";
}
