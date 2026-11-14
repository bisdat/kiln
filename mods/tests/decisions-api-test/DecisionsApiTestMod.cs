// SPDX-License-Identifier: MIT
// Lab test of kiln.decisions and kiln.window_clock through Kiln.Api (07-cancelable-events, S3, S4): see kiln.mod.json.
// Player-free: it reads an entity the world already has, files decisions no game action hangs on, and kills spawned
// 1_Player templates to hold, refuse and allow their deaths (S4, events.death: the lab needs "preview" in kiln.json).
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
        _completed?.Dispose();
        _program?.Dispose();
    }

    // A world program's update: the game runs it inside its simulation step, on the world thread or on a worker.
    private void InSimulation(EcsProgramContext context)
    {
        Interlocked.Increment(ref _updates);
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
            Check(Capabilities.Is("events.death"), $"[{run}] events.death is on ({Capabilities.Why("events.death")}; the lab needs \"preview\" in kiln.json)");
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

    private List<(DecisionResult Result, ulong Step)>? Ended(DecisionId id) => _ends.TryGetValue(id, out var list) ? list : null;

    private string Describe(DecisionId id) =>
        Ended(id) is { } list ? string.Join("; ", list.Select(x => $"{x.Result.Status} at step {x.Step}{(x.Result.Reason is { } r ? $": {r}" : "")}")) : "no event";
}
