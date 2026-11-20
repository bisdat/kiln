// SPDX-License-Identifier: MIT
// kiln.routines lab test. Prints "routines-test: PASS ..." / "FAIL ..." / "INFO ..." lines to kiln.log, then
// "routines-test: RUN<n> DONE pass=<p> fail=<f>". Two boots of one world: run 1 assigns, decides, keeps awake, checks
// the sleep check runs once, suspends by a manual command and resumes; the lab script stops the server (its shutdown
// save) and starts it again; run 2 finds the assignments again at their revisions and decides by them.
//
// LAB ONLY: the lab has no player, so the villagers' state machines stay paused and never reach a decision of their own.
// A hook on the worker's ProcessCommands (it runs inside the villager's own worker job, before the pause) asks for one:
// it puts the villager's state to DecideTask and calls the game's DecideTask, as the research probe did (chapter 16g §1,
// "force-ready only under an explicitly lab-only probe"). The same hook calls the game's SleepCheck to stand for the
// interrupt pass. The offsets below are this game build's (sigdb routines/ctx.*), read by this test only.
using System.Collections.Concurrent;
using Kiln;

namespace RoutinesTest;

// The bed seam (out/16g-villager-routines/NEEDS-FROM-18.md): package 18's Survivors.AssignBed / Find(..).Bed are not on
// this branch. Until 18 is merged the stub says so and the bed steps are skipped (INFO, never PASS); then the real
// implementation is three calls (AssignBed with TrustedServer, UnassignBed, Find(id)?.Bed).
public interface IBedFixture
{
    bool Available { get; }
    Task<bool> AssignTrusted(uint npcId, EntityId bed);
    EntityId BedOf(uint npcId);
}

public sealed class NoBeds : IBedFixture
{
    public bool Available => false;
    public Task<bool> AssignTrusted(uint npcId, EntityId bed) => Task.FromResult(false);
    public EntityId BedOf(uint npcId) => EntityId.None;
}

public sealed class RoutineTestRun
{
    public int Run { get; set; }
    public Dictionary<string, ulong> Revisions { get; set; } = new();
    public List<uint> Suspended { get; set; } = [];
}

public sealed class RoutinesTestMod : Mod
{
    private const int CtxDynamic = 0x1b0, CtxCommon = 0x300, CtxTemplate = 0x450, CommonSleep = 0x50, CommonWake = 0x54;
    private const byte DecideTaskState = 4, IdleState = 3, FindBedState = 2;

    private BlockHandle<RoutineBlock> _awake = null!, _sleepy = null!;
    private static readonly IBedFixture Beds = new NoBeds();
    private readonly List<HookHandle> _hooks = [];
    private readonly ConcurrentDictionary<uint, char> _ask = new();     // NpcId -> 'd' decide, 's' sleep check
    private readonly ConcurrentDictionary<uint, string> _answer = new(); // NpcId -> what the worker saw
    private readonly List<RoutineTaskChangedEvent> _tasks = [];
    private readonly List<RoutineAssignmentChangedEvent> _assignments = [];
    private nuint _decide, _sleep;
    private long _sleepEntry, _sleepGame;
    private int _pass, _fail;

    private void Info(string s) => Log.Info("routines-test: INFO " + s);
    private bool Check(bool ok, string what)
    {
        if (ok) _pass++;
        else _fail++;
        Log.Info($"routines-test: {(ok ? "PASS" : "FAIL")} {what}");
        return ok;
    }

    [Subscribe]
    private void OnTask(RoutineTaskChangedEvent e)
    {
        _tasks.Add(e);
        Info($"task event: {e}");
    }

    [Subscribe]
    private void OnAssignment(RoutineAssignmentChangedEvent e)
    {
        _assignments.Add(e);
        Info($"assignment event: {e}");
    }

    public override void OnLoad()
    {
        _awake = Blocks.Define("awake", new RoutineBlock { Windows = [new RoutineWindow(0, 0, new IdleTask(), AllDay: true)], Fallback = new RoamTask() });
        _sleepy = Blocks.Define("sleepy", new RoutineBlock { Windows = [new RoutineWindow(0, 0, new SleepTask(), AllDay: true)], Fallback = new VanillaTask() });
        _decide = Symbols.Get("routines/DecideTask");
        _sleep = Symbols.Get("routines/SleepCheck");
        // before kiln.routines' own links (priority 0): the request runs outside its command watch, inside its worker
        _hooks.Add(Hooks.Detour("routines/ProcessCommands", Commands, priority: 100));
        _hooks.Add(Hooks.Mid("routines/SleepCheck", (ref Registers r) => Interlocked.Increment(ref _sleepEntry), priority: 100));
        _hooks.Add(Hooks.Mid("routines/SleepCheck", (ref Registers r) => Interlocked.Increment(ref _sleepGame), priority: -100));
        _ = RunGuarded();
    }

    public override void OnUnload()
    {
        foreach (var h in _hooks)
            try { h.Remove(); } catch { }
        _hooks.Clear();
    }

    private static unsafe uint NpcOf(nuint c) => c == 0 ? 0 : *(uint*)*(nuint*)(c + CtxTemplate);
    private static unsafe byte* Dyn(nuint c) => *(byte**)(c + CtxDynamic);
    private static unsafe byte* Common(nuint c) => *(byte**)(c + CtxCommon);

    // Worker thread: the villager's own job.
    private unsafe nuint Commands(HookCall call)
    {
        var ret = call.CallOriginal();
        var c = call[0];
        var npc = NpcOf(c);
        if (npc == 0 || !_ask.TryRemove(npc, out var what)) return ret;
        var tid = Environment.CurrentManagedThreadId;
        if (what is 'd' or 'h')
        {
            var before = Dyn(c)[0];
            if (what == 'h')
            {
                // LAB ONLY: a spawned villager's sleep and wake hours are not drawn yet (0 and 0: the game's sleep check
                // says yes at any hour); give it the Fisher template's kind of hours so noon is outside its sleep window
                *(float*)(Common(c) + CommonSleep) = 21.5f;
                *(float*)(Common(c) + CommonWake) = 6.0f;
            }
            Dyn(c)[0] = DecideTaskState;  // LAB ONLY: the paused state machine's decision, asked for
            ((delegate* unmanaged<nuint, byte, void>)_decide)(c, 1);
            _answer[npc] = $"decided state {before}->{Dyn(c)[0]} decision {Dyn(c)[2]} tid {tid}";
        }
        else
        {
            var state = Dyn(c)[0];
            var decision = Dyn(c)[2];
            var yes = ((delegate* unmanaged<nuint, byte>)_sleep)(c);
            _answer[npc] = $"sleepcheck {yes} state {state}->{Dyn(c)[0]} decision {decision}->{Dyn(c)[2]} tid {tid}";
        }
        return ret;
    }

    private async Task Steps(int n)
    {
        for (var i = 0; i < n; i++) await Context.Scheduler.NextTick();
    }

    // Asks the villager's worker for something and waits for its answer (the worker runs every step or so).
    private async Task<string?> Ask(Villager v, char what)
    {
        _answer.TryRemove(v.NpcId, out _);
        _ask[v.NpcId] = what;
        for (var i = 0; i < 200; i++)
        {
            await Steps(1);
            if (_answer.TryRemove(v.NpcId, out var a)) return a;
        }
        _ask.TryRemove(v.NpcId, out _);
        return null;
    }

    private async Task RunGuarded()
    {
        try { await Run(); }
        catch (Exception e) { Check(false, "threw: " + e); }
        Log.Info($"routines-test: RUN{_run?.Run ?? 0} DONE {_pass} PASS, {_fail} FAIL");
    }

    private RoutineTestRun? _run;

    private async Task Run()
    {
        for (var i = 0; !Spawn.WorldRunning && i < 6000; i++) await Steps(1);
        await Steps(120);
        _run = WorldData.Get<RoutineTestRun>("routines-test");
        _run.Run++;
        Info($"run {_run.Run}; capability routines.decisions: {Capabilities.Is("routines.decisions")}");
        if (!Check(Capabilities.Is("routines.decisions"), "routines.decisions is available on the dedicated server")) return;
        var all = Villagers.List().OrderBy(v => v.NpcId).ToList();
        Info($"villagers: {string.Join(", ", all)}");
        if (_run.Run == 1 && all.Count < 2)
        {
            // the lab save has no villager: two of the game's townsfolk next to the base, saved with the world (run 2 finds them)
            foreach (var (template, dx) in new[] { ("Townsfolk_Male_villager_04_AntonGodwin", 0.0), ("Townsfolk_Male_villager_02_RamoneJones", 4.0) })
            {
                try
                {
                    // a roster NPC lives only while its unlock holds (the game destroys a locked one): grant it first
                    var npc = Knowledge.FindNpc(template);
                    Info($"roster: {npc?.ToString() ?? "not in the roster"}");
                    if (npc is { Unlocked: not true })
                    {
                        var g = await Knowledge.World.Grant(npc.Unlock.Id, Math.Max(1u, npc.Unlock.CompareValue));
                        await Steps(10);
                        Info($"granted its unlock: {g}; now {Knowledge.FindNpc(template)}");
                    }
                    var e = await Spawn.Entity(template, new WorldPosition(3870 + dx, 834, 1470), 90, new SpawnOptions { Flags = SpawnFlags.NoTrack });
                    await Steps(30);
                    Info($"spawned {template}: {e}, alive {Spawn.Exists(e)}, villager state {World.Has(e, "DynamicActiveNpcState")}");
                }
                catch (Exception ex) { Info($"spawn {template}: {ex.Message}"); }
            }
            await Steps(60);
            all = Villagers.List().OrderBy(v => v.NpcId).ToList();
            Info($"villagers now: {string.Join(", ", all)}");
        }
        if (!Check(all.Count >= 1, $"the lab world has villagers ({all.Count})")) return;
        var a = all[0];
        var b = all.Count > 1 ? all[1] : null;
        if (_run.Run == 1) await Run1(a, b);
        else await Run2(a, b);
    }

    private async Task Run1(Villager a, Villager? b)
    {
        Time.Set(12);
        await Steps(10);
        var r = await Routines.Assign(a, _awake);
        Check(r.Ok && r.Revision > 0, $"Assign {a} -> awake: {r}");
        Check(_assignments.Any(e => e.After.NpcId == a.NpcId && e.Reason == "Assigned"), "an assignment event says Assigned");
        var answer = await Ask(a, 'd');
        Info($"decide: {answer}");
        Check(a.State == VillagerState.Idle, $"the plan's Idle runs: {a} ({answer})");
        Check(_tasks.Any(e => e.NpcId == a.NpcId && e.Task is IdleTask && e.To == RoutineTaskState.Started), "a task event says the Idle started");
        Check(Routines.Inspect(a) is { Task: IdleTask, Assignment: RoutineAssignmentState.Assigned }, $"Inspect: {Routines.Inspect(a)}");

        // the sleep check at night while the plan says Idle: no, without the game's check running
        Time.Set(23.5);
        await Steps(20);
        var g0 = Interlocked.Read(ref _sleepGame);
        answer = await Ask(a, 's');
        Info($"keep-awake: {answer}");
        Check(answer != null && answer.StartsWith("sleepcheck 0") && Interlocked.Read(ref _sleepGame) == g0 && a.State == VillagerState.Idle,
            $"keep-awake: at 23:30 the plan's Idle keeps it up: SleepCheck answered no, the game's check did not run (game calls +{Interlocked.Read(ref _sleepGame) - g0}), state {a.State}");

        // SleepCheck once: a Sleep the game does not allow (noon), then the vanilla fallback whose chooser asks again
        var sv = b ?? a;
        Time.Set(12);
        await Steps(20);
        r = await Routines.Assign(sv, _sleepy);
        Check(r.Ok, $"Assign {sv} -> sleepy: {r}");
        long e0 = Interlocked.Read(ref _sleepEntry), s0 = Interlocked.Read(ref _sleepGame);
        _tasks.Clear();
        answer = await Ask(sv, 'h');
        long entries = Interlocked.Read(ref _sleepEntry) - e0, games = Interlocked.Read(ref _sleepGame) - s0;
        Info($"sleep at noon: {answer}; SleepCheck entries {entries}, game runs {games}");
        Check(games == 1, $"SleepCheck once: the game's check ran {games} time(s) in one decision ({entries} call(s) reached it)");
        Check(_tasks.Any(e => e.NpcId == sv.NpcId && e.Task is SleepTask && e.To == RoutineTaskState.Refused && e.Reason == "NativeSleepNotCurrentlyAllowed"),
            "the Sleep was refused with NativeSleepNotCurrentlyAllowed");
        Check(_tasks.Any(e => e.NpcId == sv.NpcId && e.Task is VanillaTask && e.To == RoutineTaskState.Started), "the vanilla fallback took over");
        // at night the game allows it: its own check, once, takes FindBed
        Time.Set(23.5);
        await Steps(20);
        s0 = Interlocked.Read(ref _sleepGame);
        answer = await Ask(sv, 'd');
        games = Interlocked.Read(ref _sleepGame) - s0;
        Info($"sleep at night: {answer}; game runs {games}");
        Check(games == 1 && answer != null && answer.Contains($"->{FindBedState} "), $"at 23:30 the game's own check, once, takes FindBed ({answer})");
        if (Beds.Available) Info($"bed of {sv}: {Beds.BedOf(sv.NpcId)}");
        else Info("beds: package 18 not merged, bed steps skipped");

        if (b != null)
        {
            // back to the awake plan for a, and b stays on sleepy
        }
        else
        {
            r = await Routines.Assign(a, _awake);
            Check(r.Ok, $"a back on awake: {r}");
        }

        // a manual command suspends; Resume ends it
        Time.Set(12);
        a.Stay();
        await Steps(30);
        Check(Routines.Inspect(a)?.Assignment == RoutineAssignmentState.ManualSuspended, $"Stay suspends the routine: {Routines.Inspect(a)}");
        Check(_assignments.Any(e => e.After.NpcId == a.NpcId && e.Reason == "ManualSuspended"), "an assignment event says ManualSuspended");
        r = await Routines.Resume(a);
        Check(r.Ok && Routines.Inspect(a)?.Assignment == RoutineAssignmentState.Assigned, $"Resume: {r} {Routines.Inspect(a)}");
        answer = await Ask(a, 'd');
        Check(a.State == VillagerState.Idle, $"after Resume the plan decides again: {answer}");
        if (b != null)
        {
            b.Stay();
            await Steps(30);
            Check(Routines.Inspect(b)?.Assignment == RoutineAssignmentState.ManualSuspended, $"b suspended for the restart: {Routines.Inspect(b)}");
        }

        // what run 2 compares
        foreach (var v in new[] { a, b })
        {
            if (v == null || Routines.Inspect(v) is not { } s) continue;
            _run!.Revisions[$"{v.NpcId:x8}"] = s.Revision;
            if (s.Assignment == RoutineAssignmentState.ManualSuspended) _run.Suspended.Add(v.NpcId);
            Info($"saved for run 2: {s}");
        }
    }

    private async Task Run2(Villager a, Villager? b)
    {
        Time.Set(12);
        await Steps(30);
        foreach (var v in new[] { a, b })
        {
            if (v == null || !_run!.Revisions.TryGetValue($"{v.NpcId:x8}", out var rev)) continue;
            var s = Routines.Inspect(v);
            Info($"after the restart: {s}");
            Check(s != null && s.Revision == rev, $"restart: {v} has its routine again at revision {rev}: {s}");
            var suspended = _run.Suspended.Contains(v.NpcId);
            Check(s != null && (s.Assignment == RoutineAssignmentState.ManualSuspended) == suspended,
                $"restart: the manual suspension is {(suspended ? "kept" : "not invented")}: {s?.Assignment}");
        }
        var answer = await Ask(a, 'd');
        Check(a.State == VillagerState.Idle, $"after the restart the plan decides: {answer}");
        var r = await Routines.Remove(a);
        Check(r.Ok && Routines.Inspect(a) == null, $"Remove: {r}");
        var vanilla = await Ask(a, 'd');
        Info($"after Remove the game decides: {vanilla}");
        if (b != null)
        {
            r = await Routines.Remove(b);
            Check(r.Ok, $"Remove b: {r}");
        }
    }
}
