// SPDX-License-Identifier: MIT
// Lab test (managed part):
//  1. Kiln.Testing.LabSystems, the lab parallel switch, at world steps 60-64 (before the native part uses the switch).
//  2. Kiln.Systems, once the native part (native/systems_test.cpp) is done (it creates kiln/control/systems-native.done):
//     a second Replace (systems-test-peer holds one), a throw that disables only its stage, a tick-thread API inside a
//     handler, per-entity stages, handlers on the game's workers with the switch on; with the switch on again, writes
//     through entity views (one ComponentWrittenEvent per entity and component per step, on the tick thread) and
//     Attribute32.Changed; then `reload-test systems-test 20` through the control folder (each reload makes wraps again;
//     the old instance's must go with it).
using System.Collections.Concurrent;
using Kiln;
using Kiln.Testing;

namespace SystemsTest;

public sealed class SystemsTestMod : Mod
{
    private const ulong From = 60, Latest = 200;  // world steps: the window must end before the native part's
    private ulong _start;
    private int _pass, _fail;
    private bool _labDone, _done, _reload;
    private ulong _t0;

    private void Check(string name, bool ok, object? detail = null)
    {
        if (ok) _pass++;
        else _fail++;
        Log.Info($"{(ok ? "PASS" : "FAIL")} {name}{(detail is null ? "" : ": " + detail)}");
    }

    private string KilnDir => Path.GetFullPath(Path.Combine(Context.DataDirectory, "..", ".."));
    private string Control(string leaf) => Path.Combine(KilnDir, "control", leaf);

    public override void OnLoad()
    {
        Directory.CreateDirectory(Context.DataDirectory);
        var pidFile = Path.Combine(Context.DataDirectory, "pid");
        var pid = Environment.ProcessId.ToString();
        _reload = File.Exists(pidFile) && File.ReadAllText(pidFile) == pid;
        File.WriteAllText(pidFile, pid);
        if (_reload)
        {
            // a reload of reload-test: wraps that this instance's unload must take with it
            var w = Systems.Wrap("fuel_network").Wrap;
            w?.Before((ref Slice s) => { }).AfterEach(e => { });
            return;
        }
        if (File.Exists(Path.Combine(KilnDir, "data", "systems-test-mark.txt"))) _done = true;  // the boot after the restart
        Context.Tick.Post += OnPost;
    }

    private void OnPost(in TickInfo tick)
    {
        if (!_labDone) LabWindow(tick);
        else if (!_done) WrapRows(tick);
    }

    // ---- 1. the lab switch ----
    private void LabWindow(in TickInfo tick)
    {
        if (tick.Step < From) return;
        if (_start == 0)
        {
            _start = tick.Step;
            if (_start > Latest)
            {
                Check("LabSystems window", false, $"the first step seen is {_start}, after the native part may use the switch");
                _labDone = true;
                return;
            }
        }
        try
        {
            switch (tick.Step - _start)
            {
                case 0:
                    Check("LabSystems.Enabled", LabSystems.Enabled, "kiln/control/systems.parallel is there");
                    Check("LabSystems refuses a window outside 1..3000 steps",
                        Throws<ArgumentOutOfRangeException>(() => LabSystems.ForceParallel(0)) &&
                        Throws<ArgumentOutOfRangeException>(() => LabSystems.ForceParallel(3001)));
                    Check("LabSystems.ForceParallel(40)", LabSystems.PlayersMask == 0 && LabSystems.ForceParallel(40),
                        $"mask {LabSystems.PlayersMask:x4} before");
                    break;
                case 2:
                    Check("LabSystems: forced from the next step", LabSystems.Forced && LabSystems.PlayersMask == 0x8000 &&
                        LabSystems.StepsLeft is > 30 and <= 40, $"mask {LabSystems.PlayersMask:x4}, {LabSystems.StepsLeft} steps left");
                    Check("LabSystems.StopParallel", LabSystems.StopParallel());
                    break;
                case 4:
                    Check("LabSystems: let go in the next step", !LabSystems.Forced && LabSystems.PlayersMask == 0,
                        $"mask {LabSystems.PlayersMask:x4}");
                    _labDone = true;
                    break;
            }
        }
        catch (Exception e)
        {
            Check($"step {tick.Step - _start}", false, e.Message);
            _labDone = true;
        }
    }

    // ---- 2. Kiln.Systems ----
    private ProgramWrap? _throwing, _guard, _each, _replace;
    private long _afterCalls, _beforeSlices, _sliceEntities, _eachEntities, _guardCalls;
    private string? _guardError;
    private int _guardTried;
    private WrapDisabledEvent? _disabled;
    private readonly ConcurrentDictionary<int, byte> _threads = new(), _forcedThreads = new();
    private volatile bool _counting;
    private WrapResult<ProgramWrap>? _second;
    // writes through entity views (S6)
    private ProgramWrap? _writer;
    private EventSubscription? _writtenSub;
    private volatile bool _writing, _attrWanted;
    private int _tickThread, _attrDone;
    private readonly ConcurrentDictionary<uint, byte> _targets = new();
    private readonly ConcurrentDictionary<int, byte> _writeThreads = new();
    private readonly ConcurrentDictionary<(uint, ulong), byte> _wrote = new();
    private readonly ConcurrentDictionary<uint, int> _eventsPer = new();
    private int _badEvents;
    private string? _badEvent, _attrResult;

    private void WrapRows(in TickInfo tick)
    {
        _tickThread = Environment.CurrentManagedThreadId;
        try
        {
            if (_t0 == 0)
            {
                if (tick.Step % 25 != 0 || !File.Exists(Control("systems-native.done"))) return;
                _t0 = tick.Step;
                Log.Info($"systems-test (managed): the native part is done; wraps from step {_t0}");
            }
            var n = tick.Step - _t0;
            switch (n)
            {
                case 0:
                {
                    var info = Systems.Find("fuel_network");
                    Check("Systems.Find", info is { Runs: true, World: WorldKind.Server } && Systems.All().Count > 800 &&
                        info.Writes.Contains("NetworkFuel") && Systems.Find("client_event_tracking") is { Runs: false },
                        $"{Systems.All().Count} programs; fuel_network writes {string.Join(",", info?.Writes ?? [])}");
                    var unknown = Systems.Wrap("no_such_program");
                    var client = Systems.Wrap("client_event_tracking");
                    Check("Systems.Wrap refuses a program the world lacks or cannot run", !unknown.Ok && !client.Ok,
                        $"{unknown.Reason} / {client.Reason}");
                    _throwing = Systems.Wrap("fuel_network").Wrap!;
                    _throwing.Disabled += e => _disabled = e;
                    _throwing.Before((ref Slice s) => throw new InvalidOperationException("the wrap test's throw"))
                             .After((ref Slice s) => Interlocked.Increment(ref _afterCalls));
                    _guard = Systems.Wrap("is_actor_in_shape").Wrap!;
                    _guard.Before((ref Slice s) =>
                    {
                        Interlocked.Increment(ref _guardCalls);
                        (_counting ? _forcedThreads : _threads).TryAdd(Environment.CurrentManagedThreadId, 0);
                        if (Interlocked.Exchange(ref _guardTried, 1) == 0)
                            try { GameSystems.Profile(false); _guardError = "none"; }
                            catch (InvalidOperationException e) { _guardError = e.Message; }
                    });
                    _each = Systems.Wrap("fuel_network").Wrap!;
                    _each.Before((ref Slice s) => { Interlocked.Increment(ref _beforeSlices); Interlocked.Add(ref _sliceEntities, s.Count); })
                         .BeforeEach(e => Interlocked.Increment(ref _eachEntities));
                    break;
                }
                case 20:
                    _replace = Systems.Wrap("fuel_setup").Wrap!;
                    _second = _replace.Replace((ref Slice s) => s.RunOriginal());
                    Check("a second Replace is refused: Ok=false, reason names the first mod", _second is { Ok: false } &&
                        _second.Reason!.Contains("systems-test-peer"), _second.Reason);
                    break;
                case 120:
                    Check("a managed throw disables only that stage", _disabled is { Stage: WrapStage.Before } &&
                        _disabled.Reason.Contains("InvalidOperationException") && Interlocked.Read(ref _afterCalls) > 50 &&
                        _throwing!.Stats.Faults == 1,
                        $"Disabled: {_disabled?.Reason}; the After ran {Interlocked.Read(ref _afterCalls)} times since");
                    Check("tick-thread API inside a handler throws", _guardError?.Contains("program wrap") == true, _guardError);
                    Check("per entity: BeforeEach ran once per entity of every slice", Interlocked.Read(ref _eachEntities) > 0 &&
                        Interlocked.Read(ref _eachEntities) == Interlocked.Read(ref _sliceEntities),
                        $"{Interlocked.Read(ref _eachEntities)} entities, {Interlocked.Read(ref _beforeSlices)} slices holding {Interlocked.Read(ref _sliceEntities)}");
                    Check("serial: managed handlers on the world's thread only", _threads.Count == 1 && _guard!.State == WrapState.Active,
                        $"{_threads.Count} thread(s)");
                    Check("LabSystems.ForceParallel(300)", LabSystems.ForceParallel(300));
                    break;
                case 122:
                    _counting = true;
                    break;
                case 400:
                {
                    _counting = false;
                    var stats = _guard!.Stats;
                    Check("managed handlers ran on N>1 threads (forced)", _forcedThreads.Count > 1 && stats.Faults == 0,
                        $"{_forcedThreads.Count} threads, {stats.Slices} slices, {stats.Threads} threads by kiln.world's count");
                    LabSystems.StopParallel();
                    break;
                }
                case 404:
                    Check("LabSystems.ForceParallel(200) for the writes", LabSystems.ForceParallel(200));
                    _writtenSub = Events.On<ComponentWrittenEvent>(OnWritten);
                    _writer = Systems.Wrap("fuel_setup").Wrap!;
                    _writer.BeforeEach(WriteBefore).AfterEach(WriteAfter);
                    break;
                case 410:
                    _writing = true;  // three steps of writes
                    break;
                case 413:
                    _writing = false;
                    break;
                case 420:
                {
                    var expected = _wrote.Keys.GroupBy(k => k.Item1).ToDictionary(g => g.Key, g => g.Count());
                    var same = expected.Count > 0 && expected.Count == _eventsPer.Count &&
                        expected.All(kv => _eventsPer.TryGetValue(kv.Key, out var n) && n == kv.Value);
                    Check("write event: one ComponentWrittenEvent per entity and component per step, Writer \"systems-test/wrap:fuel_setup\", raised on the tick thread",
                        same && _badEvents == 0,
                        $"{_wrote.Count} (entity, step) pairs written by two stages each on {_writeThreads.Count} thread(s), " +
                        $"{_eventsPer.Values.Sum()} events for {_eventsPer.Count} entities; {_badEvents} wrong: {_badEvent ?? "none"}");
                    _attrWanted = true;
                    break;
                }
                case 424:
                    Check("Attribute32.Changed clamps to max", _attrResult?.StartsWith("clamped") == true, _attrResult ?? "no entity with a maximum seen");
                    break;
                case 430:
                    foreach (var w in new[] { _throwing, _guard, _each, _replace, _writer }) w?.Dispose();
                    _writtenSub?.Dispose();
                    LabSystems.StopParallel();
                    Check("Dispose: every wrap removed", _throwing!.State == WrapState.Removed && _guard!.State == WrapState.Removed &&
                        _writer!.State == WrapState.Removed);
                    break;
                case 440:
                    Log.Info($"systems-test (managed): {_pass} PASS, {_fail} FAIL");
                    _done = true;
                    File.WriteAllText(Control("systems-reload.cmd"), "reload-test systems-test 20\n");
                    break;
            }
        }
        catch (Exception e)
        {
            Check($"wrap step {tick.Step - _t0}", false, e.ToString());
            Log.Info($"systems-test (managed): {_pass} PASS, {_fail} FAIL");
            _done = true;
        }
    }

    // ---- writes through entity views: Before takes one unit off 20 containers, After gives it back ----
    private void WriteBefore(ref EntityView e)
    {
        if (!_writing) return;
        var id = e.Entity.Value;
        if (!_targets.ContainsKey(id))
        {
            if (_targets.Count >= 20) return;
            var probe = e.Attribute("Fuel");
            if (!probe.HasValues || probe.Count < 2 || probe[0] <= 0) return;
            _targets.TryAdd(id, 0);
        }
        var fuel = e.Attribute("Fuel");
        if (!fuel.HasValues || fuel.Count < 2 || fuel[0] <= 0) return;
        fuel[0] -= 1;
        _wrote.TryAdd((id, e.Step), 0);
        _writeThreads.TryAdd(Environment.CurrentManagedThreadId, 0);
    }

    private void WriteAfter(ref EntityView e)
    {
        var id = e.Entity.Value;
        if (_wrote.ContainsKey((id, e.Step)))
        {
            e.Attribute("Fuel")[0] += 1;
            return;
        }
        if (!_attrWanted || Volatile.Read(ref _attrDone) != 0 || !e.Has("Fuel")) return;
        var a = e.Attribute("Fuel");  // Fuel: [0] current units, [1] the maximum
        if (!a.HasValues || a.Count < 2 || a[1] <= 0 || Interlocked.Exchange(ref _attrDone, 1) != 0) return;
        var (before, max) = (a[0], a[1]);
        a[0] = max + 1000;
        a.Changed();
        var after = a[0];
        a[0] = before;
        a.Changed();
        _attrResult = after == max ? $"clamped: {max + 1000} -> {after} (max {max}) on {e.Entity}, put back to {a[0]}"
            : $"not clamped: {max + 1000} -> {after} (max {max}) on {e.Entity}";
    }

    private void OnWritten(ComponentWrittenEvent e)
    {
        if (e.Writer is null || !e.Writer.EndsWith("/wrap:fuel_setup", StringComparison.Ordinal) || !_targets.ContainsKey(e.Entity.Value)) return;
        _eventsPer.AddOrUpdate(e.Entity.Value, 1, (_, n) => n + 1);
        string? bad = null;
        if (e.Writer != "systems-test/wrap:fuel_setup" || e.Writers.Count != 1) bad = $"writer {e.Writer} ({e.Writers.Count})";
        else if (e.Component != "Fuel") bad = $"component {e.Component}";
        else if (Environment.CurrentManagedThreadId != _tickThread) bad = $"thread {Environment.CurrentManagedThreadId}, tick {_tickThread}";
        else if (!e.Result.Changed || e.Result.Before.Length < 16) bad = "no change";
        if (bad is null) return;
        _badEvents++;
        _badEvent ??= bad;
    }

    private static bool Throws<T>(Action a) where T : Exception
    {
        try { a(); return false; }
        catch (T) { return true; }
    }
}
