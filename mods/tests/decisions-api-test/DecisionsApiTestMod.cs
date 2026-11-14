// SPDX-License-Identifier: MIT
// Lab test of kiln.decisions and kiln.window_clock through Kiln.Api (07-cancelable-events, S3): see kiln.mod.json.
// Player-free: it reads an entity the world already has and files decisions no game action hangs on.
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

            EntityId entity = default;
            foreach (var e in World.Find("Health"))
            {
                if (World.Position(e) is null) continue;
                entity = e;
                break;
            }
            if (entity.IsNone) throw new InvalidOperationException("the world has no entity with Health and a position");
            var rc = Decisions.LabResolveEntity(entity, out var key, out var at);
            Check(rc == 0 && key.Entity == entity.Value && key.LifeSerial != 0 && (key.HasSpawnTime || key.PlayerHandle != 0) && at is not null,
                $"resolve_entity answers in Pre: rc {rc}, entity {key.Entity}, spawn time {(key.HasSpawnTime ? key.SpawnTime.ToString() : "none")}, life {key.LifeSerial}, at {at}");
            await Wait(3);
            var rc2 = Decisions.LabResolveEntity(entity, out var key2, out _);
            Check(rc2 == 0 && key2 == key, $"the same life has the same key three steps later (life {key2.LifeSerial})");
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
        }
        catch (Exception e)
        {
            Check(false, $"the run threw: {e}");
        }
        Log.Info($"decisions-api-test done: {_passes} PASS, {_failures} FAIL");
    }

    private List<(DecisionResult Result, ulong Step)>? Ended(DecisionId id) => _ends.TryGetValue(id, out var list) ? list : null;

    private string Describe(DecisionId id) =>
        Ended(id) is { } list ? string.Join("; ", list.Select(x => $"{x.Result.Status} at step {x.Step}{(x.Result.Reason is { } r ? $": {r}" : "")}")) : "no event";
}
