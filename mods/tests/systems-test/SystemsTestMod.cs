// SPDX-License-Identifier: MIT
// Lab test (managed part):
//  1. Kiln.Testing.LabSystems, the lab parallel switch, at world steps 60-64 (before the native part uses the switch).
//  2. Kiln.Systems, once the native part (native/systems_test.cpp) is done (it creates kiln/control/systems-native.done):
//     a second Replace (systems-test-peer holds one), a throw that disables only its stage, a tick-thread API inside a
//     handler, per-entity stages, handlers on the game's workers with the switch on; then `reload-test systems-test 20`
//     through the control folder (each reload makes wraps again; the old instance's must go with it).
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

    private void WrapRows(in TickInfo tick)
    {
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
                    break;
                }
                case 430:
                    foreach (var w in new[] { _throwing, _guard, _each, _replace }) w?.Dispose();
                    Check("Dispose: every wrap removed", _throwing!.State == WrapState.Removed && _guard!.State == WrapState.Removed);
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

    private static bool Throws<T>(Action a) where T : Exception
    {
        try { a(); return false; }
        catch (T) { return true; }
    }
}
