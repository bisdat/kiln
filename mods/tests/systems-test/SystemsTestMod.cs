// SPDX-License-Identifier: MIT
// Lab test (managed part):
//  1. Kiln.Testing.LabSystems, the lab parallel switch, at world steps 60-64 (before the native part uses the switch).
//  2. Kiln.Systems, once the native part (native/systems_test.cpp) is done (it creates kiln/control/systems-native.done):
//     a second Replace (systems-test-peer holds one), a throw that disables only its stage, a tick-thread API inside a
//     handler, per-entity stages, handlers on the game's workers with the switch on; with the switch on again, writes
//     through entity views (one ComponentWrittenEvent per entity and component per step, on the tick thread) and
//     Attribute32.Changed, and EntityView.Attribute refusing a write slot that is not an attribute; then
//     `reload-test systems-test 20` through the control folder (each reload makes wraps again; the old instance's must
//     go with it).
//  3. The boot after the restart (the native part's mark file is there): the same per-entity wrap on fuel_setup, made
//     again by name in a new process, is applied, filters by archetype and reads Fuel through the view.
//  4. After the typed rows: Systems.Define (S8). OnLoad binds FuelQuery to the program kiln.world reserved at boot from
//     kiln.programs.json (what Kiln.Sdk's KilnPrograms task writes for it); the rows check it ran for fuel_setup's entities,
//     that a query with no reservation is Ok=false, and that its handler ran on the game's workers with the switch on. Then
//     the INIT row: an AfterEach on fuel_setup_init (an INIT program) sees a well the test spawns.
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
        // A reload of reload-test, or the first load in this process: the AppDomain outlives a mod's load context. (The
        // process id cannot tell: under Wine the next boot often gets the same one.)
        _reload = AppDomain.CurrentDomain.GetData("systems-test.loaded") is not null;
        AppDomain.CurrentDomain.SetData("systems-test.loaded", "yes");
        if (_reload)
        {
            // a reload of reload-test: wraps and a defined program's binding that this instance's unload must take with it
            var w = Systems.Wrap("fuel_network").Wrap;
            w?.Before((ref Slice s) => { }).AfterEach(e => { });
            Systems.Define(FuelQuery.Key, new ProgramOptions(), (ref FuelQuery q) => { });
            return;
        }
        if (!File.Exists(Path.Combine(KilnDir, "data", "systems-test-mark.txt")))
        {
            // the program of the mod's own (S8), bound in OnLoad: kiln.world reserved it at boot
            _defined = Systems.Define(FuelQuery.Key, new ProgramOptions(), (ref FuelQuery q) =>
            {
                Interlocked.Increment(ref _definedCalls);
                if (q.Context.TryGet<Ecs.Fuel>(out var fuel) && fuel.Values.Length >= 2) Interlocked.Increment(ref _definedFuel);
                if (_defineCounting) _definedThreads.TryAdd(Environment.CurrentManagedThreadId, 0);
            });
        }
        if (File.Exists(Path.Combine(KilnDir, "data", "systems-test-mark.txt"))) _restart = true;  // the boot after the restart
        _defineB = File.Exists(Control("systems-test-define-b"));  // the boot the lab reserved declaration B in
        Context.Tick.Post += OnPost;
    }

    private void OnPost(in TickInfo tick)
    {
        if (_defineB) DefineBCheck(tick);  // independent of the restart/wrap machine (runs in the same boot)
        if (_restart) RestartRows(tick);
        else if (!_labDone) LabWindow(tick);
        else if (!_done) WrapRows(tick);
    }

    // ---- 3. the boot after the restart ----
    private bool _restart;
    private ulong _r0;
    private ProgramWrap? _again;
    private long _againEntities, _againFuel, _againBad;
    private int _againFaults;

    private void RestartRows(in TickInfo tick)
    {
        if (_done) return;
        try
        {
            if (_r0 == 0)
            {
                // after the native part's restart row (it starts once the world has loaded its entities)
                if (tick.Step % 25 != 0 || !File.Exists(Control("systems-native.done")) || Systems.Find("fuel_setup") is null) return;
                _r0 = tick.Step;
                var r = Systems.Wrap("fuel_setup");
                _again = r.Wrap;
                _again?.WhenArchetype(["Fuel"]).BeforeEach((ref EntityView e) =>
                {
                    Interlocked.Increment(ref _againEntities);
                    var fuel = e.Attribute("Fuel");
                    if (fuel.HasValues && fuel.Count >= 2 && fuel[1] >= 0) Interlocked.Increment(ref _againFuel);
                    else if (!fuel.HasValues) Interlocked.Increment(ref _againBad);
                });
                if (_again is not null) _again.Disabled += _ => Interlocked.Increment(ref _againFaults);
                Check("the next boot: Systems.Wrap(\"fuel_setup\") again by name", r.Ok && _again is not null, r.Reason);
                return;
            }
            switch (tick.Step - _r0)
            {
                case 100:
                {
                    var stats = _again!.Stats;
                    Check("the next boot: the same per-entity wrap is applied in the new world and reads Fuel through its view",
                        _again.State == WrapState.Active && Interlocked.Read(ref _againEntities) > 0 &&
                        Interlocked.Read(ref _againFuel) == Interlocked.Read(ref _againEntities) && stats.Faults == 0 &&
                        _againFaults == 0,
                        $"state {_again.State}, {Interlocked.Read(ref _againEntities)} entities, Fuel read on " +
                        $"{Interlocked.Read(ref _againFuel)} ({Interlocked.Read(ref _againBad)} without values), {stats.Slices} slices, " +
                        $"{stats.Faults} faults");
                    _again.Dispose();
                    break;
                }
                case 104:
                    Check("the next boot: Dispose removes it", _again!.State == WrapState.Removed);
                    Log.Info($"systems-test (managed, after the restart): {_pass} PASS, {_fail} FAIL");
                    _done = true;
                    break;
            }
        }
        catch (Exception e)
        {
            Check($"restart step {tick.Step - _r0}", false, e.ToString());
            Log.Info($"systems-test (managed, after the restart): {_pass} PASS, {_fail} FAIL");
            _done = true;
        }
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
    private int _badEvents, _nonAttrTried;
    private string? _badEvent, _attrResult, _nonAttr;

    private void WrapRows(in TickInfo tick)
    {
        _tickThread = Environment.CurrentManagedThreadId;
        try
        {
            if (_t0 == 0)
            {
                if (tick.Step % 25 != 0 || !File.Exists(Control("systems-native.done"))) return;
                _t0 = tick.Step;
                var done = File.ReadAllText(Control("systems-native.done")).Split(' ', '\n');
                if (done.Length > 1 && done[0] == "mark") uint.TryParse(done[1], out _marked);
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
                    Check("EntityView.Attribute refuses fuel_setup's DynamicFuelSetup (a write slot that is not an attribute) " +
                        "before marking it or running the game's formulas", _nonAttr?.StartsWith("refused") == true &&
                        _writer!.Stats.Faults == 0, _nonAttr ?? "not tried");
                    break;
                case 430:
                    foreach (var w in new[] { _throwing, _guard, _each, _replace, _writer }) w?.Dispose();
                    _writtenSub?.Dispose();
                    LabSystems.StopParallel();
                    Check("Dispose: every wrap removed", _throwing!.State == WrapState.Removed && _guard!.State == WrapState.Removed &&
                        _writer!.State == WrapState.Removed);
                    break;
                default:
                    if (n >= 440 && _defineFrom == 0 && Typed(n - 440)) _defineFrom = n + 1;
                    else if (_defineFrom != 0 && n >= _defineFrom && DefineRows(n - _defineFrom))
                    {
                        Log.Info($"systems-test (managed): {_pass} PASS, {_fail} FAIL");
                        _done = true;
                        File.WriteAllText(Control("systems-reload.cmd"), "reload-test systems-test 20\n");
                    }
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

    // ---- 2b. typed views (S7): Systems.Wrap<Ecs.Programs.FuelSetup>, the contract, AroundEach ----
    private const int Window = 1200;  // steps (49 s at 24.5 Hz): about nine of a well's own fills
    private ProgramWrap<Ecs.Programs.FuelSetup>? _typed, _room, _around;
    private readonly ConcurrentDictionary<uint, (byte[] Transform, bool Same)> _typedSeen = new();
    private uint _marked;  // the native part's mark row entity: its Fuel must reach the save as written (boot B)
    private readonly ConcurrentDictionary<uint, int> _fillers = new();   // entity -> Fuel maximum (640 and more)
    private readonly ConcurrentDictionary<int, byte> _aroundThreads = new();
    private string? _typedBad, _doctored;
    private long _gameAdded1, _gameFills1, _gameAdded2, _gameFills2, _aroundApplied;
    private volatile int _k = 1;  // 1: the vanilla window (observe), 4: four times what the game adds
    private Dictionary<uint, int> _snap1 = new(), _snap2 = new();
    private Task<EntityId>? _well;
    private ProgramWrap? _all, _withFuel, _noFuel;
    private long _allSlices, _withSlices, _withoutSlices;
    private string? _archetypeOff;

    // True when the typed rows are done.
    private bool Typed(ulong m)
    {
        switch (m)
        {
            case 0:
            {
                var r = Systems.Wrap<Ecs.Programs.FuelSetup>();
                Check("typed: Systems.Wrap<Ecs.Programs.FuelSetup>() is made: the live fuel_setup has the catalog's contract",
                    r.Ok && Ecs.Programs.FuelSetup.Contract == 0xE4713C0Bu, r.Reason ?? $"contract 0x{Ecs.Programs.FuelSetup.Contract:X8}");
                _typed = r.Wrap;
                _typed?.AfterEach((ref Ecs.Programs.FuelSetup v) =>
                {
                    if (!v.HasCurrentTransform) return;
                    // the view's position (read_opt 0) and kiln.world's name lookup must name one slot
                    ref readonly var typed = ref v.CurrentTransform;
                    ref readonly var named = ref v.View.Read<Ecs.CurrentTransform>("CurrentTransform");
                    var same = System.Runtime.CompilerServices.Unsafe.AreSame(ref System.Runtime.CompilerServices.Unsafe.AsRef(in typed),
                        ref System.Runtime.CompilerServices.Unsafe.AsRef(in named));
                    var values = v.FuelValues;
                    ref readonly var fuel = ref v.View.Read<Ecs.Fuel>("Fuel");
                    same &= values.Length > 0 && values[0] == fuel.dataStorage[0];
                    _typedSeen[v.Entity.Value] = (Bytes(in typed), same);
                });
                // WhenArchetype on the game's slices: every fuel_setup slice has Fuel (a write list), none lacks it. The first
                // read comes from the entity map; it puts kiln.world's slice-record link on the program runner, and the later
                // ones read the engine's slice record (kiln.log: "WhenArchetype reads each slice's archetype from the
                // engine's slice record", and the counts at the world's stop)
                _all = Systems.Wrap("fuel_setup").Wrap!.Before((ref Slice s) => Interlocked.Increment(ref _allSlices));
                _withFuel = Systems.Wrap("fuel_setup").Wrap!.WhenArchetype(["Fuel"]).Before((ref Slice s) => Interlocked.Increment(ref _withSlices));
                _noFuel = Systems.Wrap("fuel_setup").Wrap!.WhenArchetype([], ["Fuel"]).Before((ref Slice s) => Interlocked.Increment(ref _withoutSlices));
                _withFuel.Disabled += e => _archetypeOff ??= e.Reason;
                _noFuel.Disabled += e => _archetypeOff ??= e.Reason;
                var doctored = Systems.Wrap<DoctoredFuelSetup>();
                _doctored = doctored.Reason;
                Check("typed: a doctored contract is refused with the list that changed", !doctored.Ok &&
                    doctored.Reason?.Contains("write list changed: +DynamicFuelSetup -DynamicFuelSetupV2") == true, doctored.Reason);
                return false;
            }
            case 10:
                // the well of chapter 06a 9.13: the save's own containers (water barrels, 150 charges) fill by rain only; a
                // Prop_NPC_Water_Well_a tops itself up (fuel_setup's automatic fill). Next to the mark row's barrel.
                if (_marked != 0 && World.Position(new EntityId(_marked)) is { } at && Spawn.Available)
                    _well = Spawn.Entity("Prop_NPC_Water_Well_a", new WorldPosition(at.X + 4, at.Y, at.Z));
                return false;
            case 20:
            {
                // the typed slots against GetComponent on the tick thread (static wells: their transform does not move)
                int checkedN = 0, bad = 0;
                foreach (var (id, seen) in _typedSeen)
                {
                    checkedN++;
                    var got = Ecs.Get<Ecs.CurrentTransform>(new EntityId(id));
                    if (!seen.Same || got is not { } t || !Bytes(in t).AsSpan().SequenceEqual(seen.Transform)) { bad++; _typedBad ??= $"entity {id}"; }
                }
                Check("typed: Ecs.Programs.FuelSetup's slots are kiln.world's named slots and equal GetComponent",
                    checkedN > 0 && bad == 0 && _typed!.Stats.Faults == 0,
                    $"{checkedN} entities with a transform, {bad} differ ({_typedBad ?? "none"})");
                long all = Interlocked.Read(ref _allSlices), with = Interlocked.Read(ref _withSlices), without = Interlocked.Read(ref _withoutSlices);
                Check("WhenArchetype on the game's slices: has Fuel takes every fuel_setup slice, lacks Fuel none",
                    all > 0 && with == all && without == 0 && _archetypeOff is null,
                    $"{all} slices, {with} with Fuel, {without} without; {_archetypeOff ?? "no stage off"}");
                foreach (var x in new[] { _all, _withFuel, _noFuel }) x?.Dispose();
                _typed!.Dispose();
                // room: wells and barrels (Fuel maximum 640 or more) to a tenth, so a window of fills stays below the maximum
                _room = Systems.Wrap<Ecs.Programs.FuelSetup>().Wrap!;
                _room.BeforeEach((ref Ecs.Programs.FuelSetup v) =>
                {
                    var values = v.FuelValues;
                    if (values.Length < 2 || values[1] < 640 || v.Entity.Value == _marked || !_fillers.TryAdd(v.Entity.Value, values[1])) return;
                    var fuel = v.Fuel;
                    fuel[0] = values[1] / 10;
                    fuel.Changed();
                });
                return false;
            }
            case 40:
                _room!.Dispose();
                Check("the well of chapter 06a 9.13 is spawned and given room (a tenth of its water)",
                    _well is { IsCompletedSuccessfully: true } && _fillers.ContainsKey(_well.Result.Value),
                    _well is null ? "not spawned (no mark row entity, or kiln.spawn is off)"
                        : _well.IsCompletedSuccessfully ? $"entity {_well.Result}, {_fillers.Count} containers in all" : $"{_well.Status}: {_well.Exception?.InnerException?.Message}");
                // the vanilla window: what the game adds per fill (k = 1, nothing written)
                _around = Systems.Wrap<Ecs.Programs.FuelSetup>().Wrap!;
                _around.AroundEach(
                    capture: (in Ecs.Programs.FuelSetup v) => v.FuelValues.Length > 0 ? v.FuelValues[0] : int.MinValue,
                    apply: (ref Ecs.Programs.FuelSetup v, in int before) =>
                    {
                        if (before == int.MinValue || !_fillers.ContainsKey(v.Entity.Value)) return;
                        var now = v.FuelValues[0];
                        var added = now - before;
                        if (added <= 0) return;
                        _aroundThreads.TryAdd(Environment.CurrentManagedThreadId, 0);
                        if (_k == 1)
                        {
                            Interlocked.Add(ref _gameAdded1, added);
                            Interlocked.Increment(ref _gameFills1);
                            return;
                        }
                        Interlocked.Add(ref _gameAdded2, added);
                        Interlocked.Increment(ref _gameFills2);
                        var fuel = v.Fuel;
                        fuel[0] = now + (_k - 1) * added;  // four times what the game added in this update
                        fuel.Changed();
                        Interlocked.Add(ref _aroundApplied, fuel[0] - now);
                    });
                return false;
            case 41 + Window:
                _snap1 = Snapshot();
                _k = 4;
                _aroundThreads.Clear();
                Check("LabSystems.ForceParallel for the x4 window", LabSystems.ForceParallel(Window + 20));
                return false;
            case 41 + 2 * Window:
            {
                _snap2 = Snapshot();
                LabSystems.StopParallel();
                var stats = _around!.Stats;
                _around.Dispose();
                long external = 0;
                foreach (var (id, v) in _snap2) if (_snap1.TryGetValue(id, out var was)) external += v - was;
                double a1 = _gameFills1 > 0 ? (double)_gameAdded1 / _gameFills1 : 0, a2 = _gameFills2 > 0 ? (double)_gameAdded2 / _gameFills2 : 0;
                var landed = _gameAdded2 > 0 ? (double)external / _gameAdded2 : 0;
                var perFill = a1 > 0 ? a2 / a1 : 0;
                var rate = _gameAdded1 > 0 ? (double)external / _gameAdded1 : 0;
                Check("AroundEach fuel_setup scale x4 (±5 %): the wells gained 4.00 times what the game's own update added",
                    _gameFills1 > 0 && _gameFills2 > 0 && Math.Abs(landed - 4) <= 0.2 && Math.Abs(perFill - 1) <= 0.1 && stats.Faults == 0,
                    $"x{landed:F3} ({external} units in the window, the game added {_gameAdded2} in {_gameFills2} fills on " +
                    $"{_fillers.Count} wells); per fill {a2:F1} vs {a1:F1} in the vanilla window (x{perFill:F3}); over equal " +
                    $"windows x{rate:F2} the vanilla window's {_gameAdded1} units ({_gameFills1} fills)");
                Check("AroundEach (forced): captures and applies ran on N>1 threads", _aroundThreads.Count > 1,
                    $"{_aroundThreads.Count} thread(s), {stats.Slices} slices");
                return true;
            }
        }
        return false;
    }

    // ---- 4. Systems.Define (S8) and the INIT row ----
    private ProgramRegistration? _defined;
    private long _definedCalls, _definedFuel, _windowCalls;
    private volatile bool _defineCounting;
    private readonly ConcurrentDictionary<int, byte> _definedThreads = new();
    private ulong _defineFrom;
    private ProgramWrap? _init;
    private readonly ConcurrentDictionary<uint, byte> _initIds = new();
    private Task<EntityId>? _barrel;

    // True when the Define and INIT rows are done.
    private bool DefineRows(ulong m)
    {
        switch (m)
        {
            case 0:
            {
                long calls = Interlocked.Read(ref _definedCalls), fuel = Interlocked.Read(ref _definedFuel);
                Check("define: reserved at boot, bound, ran N times", _defined is { Ok: true } && calls > 0 && fuel == calls,
                    $"{_defined?.Reason ?? "bound as " + _defined?.Key}: {calls} entity calls since OnLoad, Fuel read through the query on {fuel}");
                var unreserved = Systems.Define(UnreservedQuery.Key, new ProgramOptions(), (ref UnreservedQuery q) => { });
                Check("define unreserved: Ok=false", !unreserved.Ok && unreserved.Reason == "not reserved at boot: rebuild the mod and restart the server",
                    unreserved.Reason);
                Check("LabSystems.ForceParallel(300) for the defined program", LabSystems.ForceParallel(300));
                return false;
            }
            case 10:
                _definedThreads.Clear();
                _windowCalls = Interlocked.Read(ref _definedCalls);
                _defineCounting = true;
                return false;
            case 290:
            {
                _defineCounting = false;
                LabSystems.StopParallel();
                var calls = Interlocked.Read(ref _definedCalls) - _windowCalls;
                Check("define (forced): its handler ran on N>1 threads", _definedThreads.Count > 1 && calls > 0 &&
                    Interlocked.Read(ref _definedFuel) == Interlocked.Read(ref _definedCalls),
                    $"{_definedThreads.Count} thread(s), {calls} entity calls in the window, Fuel read on every call so far");
                _defined!.Dispose();
                // INIT: fuel_setup_init (flags 0x8) runs for archetype groups that gained entities; a spawned well is one
                _init = Systems.Wrap("fuel_setup_init").Wrap!;
                _init.AfterEach(e => _initIds.TryAdd(e.Value, 0));
                return false;
            }
            case 292:
                if (_marked != 0 && World.Position(new EntityId(_marked)) is { } at && Spawn.Available)
                    _barrel = Spawn.Entity("Prop_NPC_Water_Well_a", new WorldPosition(at.X - 4, at.Y, at.Z));
                return false;
            case 340:
            {
                var stats = _init!.Stats;
                var id = _barrel is { IsCompletedSuccessfully: true } ? _barrel.Result.Value : 0;
                Check("INIT wrap: fuel_setup_init AfterEach ran for a spawned barrel", id != 0 && _initIds.ContainsKey(id) && stats.Faults == 0,
                    _barrel is null ? "not spawned (no mark row entity, or kiln.spawn is off)"
                        : id == 0 ? $"{_barrel.Status}: {_barrel.Exception?.InnerException?.Message}"
                        : $"well {id}: {(_initIds.ContainsKey(id) ? "seen" : "not seen")} among {_initIds.Count} entities in {stats.Slices} INIT slices");
                _init.Dispose();
                return false;
            }
            // define.reload.contract, boot A (reservation = declaration A): a mod reloaded with A unchanged binds again (the
            // old owner drained), then a changed declaration B under the SAME key is refused with the named difference and the
            // restart sentence, and no B handler runs (Codex's dev.29 repair). Boot B (declaration B reserved) is checked
            // independently below (DefineBCheck). The queried declaration of each attempt is the reason it carries.
            case 342:
                _reContract = Systems.Define(FuelQuery.Key, new ProgramOptions(), (ref FuelQuery q) =>
                {
                    Interlocked.Increment(ref _reCalls);
                    if (q.Context.TryGet<Ecs.Fuel>(out var f) && f.Values.Length >= 2) Interlocked.Increment(ref _reFuel);
                });
                Check("define.reload.contract: a mod reloaded with declaration A unchanged binds again (the old owner drained)",
                    _reContract is { Ok: true }, _reContract.Reason);
                return false;
            case 440:
            {
                var reA = Interlocked.Read(ref _reCalls);
                Check("define.reload.contract: the rebound declaration A ran, Fuel read through the query",
                    _reContract is { Ok: true } && reA > 0 && Interlocked.Read(ref _reFuel) == reA,
                    $"{reA} entity calls after the rebind, Fuel on {Interlocked.Read(ref _reFuel)}");
                var bWithout = Systems.Define(FuelQuery.Key, new ProgramOptions(), (ref BWithoutQuery q) => Interlocked.Increment(ref _bCalls));
                Check("define.reload.contract: a changed 'without' under the same key is Ok=false by the reservation comparison, names the list and says restart",
                    bWithout is { Ok: false } &&
                    bWithout.Reason!.StartsWith($"{FuelQuery.Key}: without differs from the boot reservation", StringComparison.Ordinal) &&
                    bWithout.Reason.Contains("rebuild is in the game only after a server restart"), bWithout.Reason);
                var bReadOpt = Systems.Define(FuelQuery.Key, new ProgramOptions(), (ref BReadOptQuery q) => Interlocked.Increment(ref _bCalls));
                Check("define.reload.contract: a changed required/optional split under the same key is Ok=false",
                    bReadOpt is { Ok: false } && (bReadOpt.Reason!.Contains("read") || bReadOpt.Reason.Contains("readOptional")) &&
                    bReadOpt.Reason.Contains("rebuild is in the game only after a server restart"), bReadOpt.Reason);
                var bFlags = Systems.Define(FuelQuery.Key, new ProgramOptions { Pinned = true }, (ref BFlagsQuery q) => Interlocked.Increment(ref _bCalls));
                Check("define.reload.contract: changed flags under the same key is Ok=false",
                    bFlags is { Ok: false } && bFlags.Reason!.Contains("flags") &&
                    bFlags.Reason.Contains("rebuild is in the game only after a server restart"), bFlags.Reason);
                Check("define.reload.contract: none of the refused declaration-B handlers ran", Interlocked.Read(ref _bCalls) == 0,
                    $"{Interlocked.Read(ref _bCalls)} calls");
                _reContract!.Dispose();
                return true;
            }
        }
        return false;
    }

    private ProgramRegistration? _reContract;
    private long _reCalls, _reFuel, _bCalls;

    // define.reload.contract, boot B: the lab reserved systems_test_fuel with declaration B (the Pinned-flag variant) and set
    // the control file. Independent of the restart/wrap state machine: after the world is up, declaration B binds and runs,
    // and declaration A (FuelQuery) is refused with the restart sentence. Runs in the same boot as the mark-restart rows.
    private bool _defineB, _defineBDone;
    private ulong _defineBFrom;
    private ProgramRegistration? _bBound, _aRefused;
    private long _bBoundCalls;

    private void DefineBCheck(in TickInfo tick)
    {
        if (_defineBDone) return;
        try
        {
            if (_defineBFrom == 0)
            {
                if (tick.Step % 25 != 0 || !File.Exists(Control("systems-native.done")) || Systems.Find("fuel_setup") is null) return;
                _defineBFrom = tick.Step;
                // The lab reserved declaration B as the Pinned-flag variant (valid components, unlike a 'without' of a name
                // the build does not know): it binds here, and declaration A (no flag) is refused.
                _bBound = Systems.Define(FuelQuery.Key, new ProgramOptions { Pinned = true }, (ref BFlagsQuery q) =>
                {
                    Interlocked.Increment(ref _bBoundCalls);
                });
                _aRefused = Systems.Define(FuelQuery.Key, new ProgramOptions(), (ref FuelQuery q) => { });
                Check("define.reload.contract (boot B): declaration B binds to the B reservation; declaration A is refused",
                    _bBound is { Ok: true } && _aRefused is { Ok: false } && _aRefused.Reason!.Contains("rebuild is in the game only after a server restart"),
                    $"B: {_bBound?.Reason ?? "ok"}; A: {_aRefused?.Reason}");
                return;
            }
            if (tick.Step - _defineBFrom >= 100)
            {
                var calls = Interlocked.Read(ref _bBoundCalls);
                Check("define.reload.contract (boot B): after a restart with declaration B, B binds and runs on the containers",
                    _bBound is { Ok: true } && calls > 0, $"{calls} entity calls");
                _bBound!.Dispose();
                _defineBDone = true;
                Log.Info($"systems-test (define.reload.contract boot B): done");
            }
        }
        catch (Exception e)
        {
            Check("define.reload.contract (boot B)", false, e.ToString());
            _defineBDone = true;
        }
    }

    private static byte[] Bytes<T>(in T value) where T : unmanaged =>
        System.Runtime.InteropServices.MemoryMarshal.AsBytes(
            System.Runtime.InteropServices.MemoryMarshal.CreateReadOnlySpan(ref System.Runtime.CompilerServices.Unsafe.AsRef(in value), 1)).ToArray();

    // The fillers' Fuel now, on the tick thread (GetComponent).
    private Dictionary<uint, int> Snapshot()
    {
        var d = new Dictionary<uint, int>();
        foreach (var id in _fillers.Keys)
            if (Ecs.Get<Ecs.Fuel>(new EntityId(id)) is { } f) d[id] = f.Values[0];
        return d;
    }

    // ---- writes through entity views: Before takes one unit off 20 containers, After gives it back ----
    private void WriteBefore(ref EntityView e)
    {
        if (!_writing) return;
        if (Interlocked.Exchange(ref _nonAttrTried, 1) == 0)
        {
            try
            {
                var bad = e.Attribute("DynamicFuelSetup");
                _nonAttr = $"not refused: {bad.Count} slots";
            }
            catch (ArgumentException x) { _nonAttr = $"refused: {x.Message}"; }
        }
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

// Systems.Define's lab program: fuel_setup's containers again (template FuelSetup, read Fuel), a program of the mod's own.
// kiln.programs.json beside this file is what Kiln.Sdk's KilnPrograms task writes for it (this lab mod is built without the
// Kiln SDK, so the file is kept by hand); UnreservedQuery is not in it.
[Query(Key = Key, Template = ["FuelSetup"], Read = ["Fuel"])]
internal ref struct FuelQuery : IQuery<FuelQuery>
{
    public const string Key = "systems_test_fuel";
    public EcsProgramContext Context;
    public static FuelQuery Bind(EcsProgramContext context) => new() { Context = context };
}

[Query(Key = Key, Read = ["Fuel"])]
internal ref struct UnreservedQuery : IQuery<UnreservedQuery>
{
    public const string Key = "systems_test_unreserved";
    public static UnreservedQuery Bind(EcsProgramContext context) => default;
}

// define.reload.contract's "declaration B" variants: the same key as FuelQuery, each one list or flag changed from A, so
// Define refuses them under A's boot reservation by comparing the declaration with the reserved one (the reason names the
// first list that differs). Each is a declaration this build accepts (Shelter is a template component of the game's
// programs), so the refusal can only be that comparison. Boot B reserves the Pinned-flag one (BFlagsQuery), which then
// binds.
[Query(Key = FuelQuery.Key, Template = ["FuelSetup"], Read = ["Fuel"], Without = ["Shelter"])]
internal ref struct BWithoutQuery : IQuery<BWithoutQuery>
{
    public EcsProgramContext Context;
    public static BWithoutQuery Bind(EcsProgramContext context) => new() { Context = context };
}

[Query(Key = FuelQuery.Key, Template = ["FuelSetup"], ReadOptional = ["Fuel"])]
internal ref struct BReadOptQuery : IQuery<BReadOptQuery>
{
    public static BReadOptQuery Bind(EcsProgramContext context) => default;
}

[Query(Key = FuelQuery.Key, Template = ["FuelSetup"], Read = ["Fuel"], Pinned = true)]
internal ref struct BFlagsQuery : IQuery<BFlagsQuery>
{
    public static BFlagsQuery Bind(EcsProgramContext context) => default;
}

// fuel_setup as a view made for another game build would see it: one write list entry renamed, so another contract.
internal ref struct DoctoredFuelSetup : IGameProgram<DoctoredFuelSetup>
{
    public static string Name => "fuel_setup";
    public static uint Contract => 0x0BADF00Du;
    public static string Lists => "tpl=FuelSetup\nread_opt=CurrentTransform,DynamicShelter\nwrite=Fuel,DynamicFuelSetupV2\n" +
        "sys_read=WeatherSystem,g38_daytime::Daytime,impact::AttributeRegistry\nsys_write=ServerFactoryInfo\n" +
        "sys_read_opt=0000000000000000\nsys_write_opt=0000000000000000\nsys_write_parallel=0000000000000001\nflags=00000000\ninterval=1\n";
    static DoctoredFuelSetup IGameProgram<DoctoredFuelSetup>.Bind(EntityView entity) => default;
}
