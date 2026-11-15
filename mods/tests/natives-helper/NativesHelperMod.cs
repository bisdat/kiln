// SPDX-License-Identifier: MIT
// Lab helper of abilities-test (NativesV2.cs, NativesV2.Review.cs, NativesV2.Retire.cs). Three synchronous natives:
//  - natives-helper.Count(args[0] = Out sint32): Out = calls so far, answers 1 (unload during a running cast);
//  - natives-helper.Hold(Out): writes data/entered (its thread), waits until data/gate-open exists (at most 20 s), then
//    uses what this mod owns (its log, its data folder, its own state) and writes data/finished: the handler that is
//    inside while the test asks for this mod's unload, reload, or the world's stop;
//  - natives-helper.Probe(Out): counts its calls and those off the tick thread into data/probe (the worker rows).
// data/loaded and data/unloaded carry this load's nonce and the time.
using System.Diagnostics;
using Kiln;

namespace NativesHelper;

public sealed class NativesHelperMod : Mod
{
    private static int _calls;
    private int _held, _probes, _probesOff, _tick;
    private readonly string _nonce = Guid.NewGuid().ToString("N")[..8];

    private string Data(string name) => Path.Combine(Context.DataDirectory, name);
    private static string Now => DateTime.UtcNow.ToString("O");

    public override void OnLoad()
    {
        _tick = Environment.CurrentManagedThreadId;
        Directory.CreateDirectory(Context.DataDirectory);
        // kiln/data/natives-helper/argc2: Count takes two arguments (abilities-test's revive rows, NativesV2.Review.cs)
        var argc = File.Exists(Data("argc2")) ? 2 : 1;
        var r = Abilities.Natives.RegisterSync("Count", argc, (ref NativeCall c) =>
        {
            c.SetU32(0, (uint)Interlocked.Increment(ref _calls));
            return 1;
        });
        Log.Info($"natives-helper: RegisterSync(Count, argc {argc}): {r}");
        File.WriteAllText(Data("result.txt"), $"{argc} {r.Ok} {r.Reason}");
        var hold = Abilities.Natives.RegisterSync("Hold", 1, (ref NativeCall c) =>
        {
            var me = Environment.CurrentManagedThreadId;
            File.WriteAllText(Data("entered"), $"{me} {(me == _tick ? "tick" : "worker")} {_nonce} {Now}");
            var sw = Stopwatch.StartNew();
            while (!File.Exists(Data("gate-open")) && sw.Elapsed.TotalSeconds < 20) Thread.Sleep(5);
            // what this mod owns, after the gate opened: its context's log and data folder, its own state
            Log.Info($"natives-helper: Hold let go after {sw.Elapsed.TotalSeconds:F1} s (load {_nonce})");
            _held++;
            File.WriteAllText(Data("finished"), $"ok {Context.Info.Id} {_nonce} {_held} {Now}");
            c.SetU32(0, (uint)_held);
            return 1;
        });
        var probe = Abilities.Natives.RegisterSync("Probe", 1, (ref NativeCall c) =>
        {
            var calls = Interlocked.Increment(ref _probes);
            var off = Environment.CurrentManagedThreadId != _tick ? Interlocked.Increment(ref _probesOff) : Volatile.Read(ref _probesOff);
            File.WriteAllText(Data("probe"), $"{calls} {off}");
            c.SetU32(0, (uint)calls);
            return 1;
        });
        Log.Info($"natives-helper: Hold {hold.Ok}, Probe {probe.Ok} (load {_nonce})");
        File.WriteAllText(Data("loaded"), $"{_nonce} {Now}");
        // data/fill-before-world (review P1 item 4): 30 more natives; the world's registry has room for 19 Kiln natives.
        // Registered before a world exists (a client's main menu), the rest are refused by the world's setup and reported
        // (NativeRegistrationRefusedEvent); with a world's registry there already, they are refused at once
        if (File.Exists(Data("fill-before-world")))
        {
            int waiting = 0, live = 0, refusedAtOnce = 0;
            for (var i = 0; i < 30; i++)
            {
                var f = Abilities.Natives.RegisterSync($"Fill{i}", 0, (ref NativeCall c) => 1);
                if (!f.Ok) { refusedAtOnce++; continue; }
                _fills.Add(f.Hash);
                if (Abilities.Natives.StateOf(f.Hash) == NativeState.Waiting) waiting++;
                else live++;
            }
            // on a dedicated server the world's natives are set up before mods load: the registration decides at once
            Log.Info($"natives-helper: fill-before-world: of 30, {waiting} accepted and waiting for a world, {live} live at once, {refusedAtOnce} refused at once (Ok=false)");
            _ = ReportFills();
        }
    }

    private readonly List<uint> _fills = new();
    private int _refusedEvents;

    [Subscribe] private void OnRefused(NativeRegistrationRefusedEvent e) => _refusedEvents++;

    private async Task ReportFills()
    {
        for (var i = 0; i < 6000 && !_fills.All(h => Abilities.Natives.StateOf(h) is NativeState.Live or NativeState.Refused); i++)
            await Context.Scheduler.NextTick();
        for (var i = 0; i < 5; i++) await Context.Scheduler.NextTick();
        var live = _fills.Count(h => Abilities.Natives.StateOf(h) == NativeState.Live);
        var refused = _fills.Count(h => Abilities.Natives.StateOf(h) == NativeState.Refused);
        Log.Info($"natives-helper: fill-before-world after the world's setup: {live} live, {refused} refused, {_refusedEvents} NativeRegistrationRefusedEvent(s)");
        File.WriteAllText(Data("fill-result"), $"{live} {refused} {_refusedEvents}");
    }

    public override void OnUnload()
    {
        Log.Info($"natives-helper: unloading after {Volatile.Read(ref _calls)} call(s) (load {_nonce})");
        File.WriteAllText(Data("unloaded"), $"{_nonce} {Now}");
    }
}
