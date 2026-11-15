// SPDX-License-Identifier: MIT
// abilities-test rows of package 06c (Impact natives v2, since 0.2.0-dev.29): synchronous natives. Player-free.
// Runs after the main suite, or alone with kiln/data/abilities-test/natives-only. With kiln/control/systems.parallel
// every row runs a second time inside a forced-parallel window (impact_simulation stays on the world thread there:
// the rows show nothing changes and the in-flight counter holds while other programs run on workers).
// "unload during a running cast" needs the helper mod mods/tests/natives-helper in kiln/mods.
using Kiln;
using Kiln.Testing;

namespace AbilitiesTest;

public sealed partial class AbilitiesTestMod
{
    private const uint Sint32 = 0x9859f31e;
    private uint _nvSync, _nvSusp, _nvAccum, _nvWait;
    private long _nvSyncCalls, _nvSyncOffTick, _nvAccumCalls;
    private int _nvTick;

    private async Task RunNativesV2(Func<double, double, WorldPosition> at)
    {
        _nvTick = Environment.CurrentManagedThreadId;
        Info($"natives v2: capability abilities.natives.sync: {Capabilities.Get("abilities.natives.sync")}");
        if (!Capabilities.Is("abilities.natives.sync")) { Fail("abilities.natives.sync is off: " + Capabilities.Why("abilities.natives.sync")); return; }
        RegisterNativesV2();
        await NativesV2Pass(1, at);
        if (LabSystems.Enabled)
        {
            var forced = LabSystems.ForceParallel(1500);
            Info($"natives v2: forced-parallel window: {forced}, players mask {LabSystems.PlayersMask:x4}");
            await Steps(3);
            if (!Kiln.Mods.All.Any(m => m.Id == "natives-helper" && m.IsLoaded) && Kiln.Mods.All.Any(m => m.Id == "natives-helper"))
                Info($"natives v2: natives-helper loaded again: {await Kiln.Mods.LoadAsync("natives-helper")}");
            await NativesV2Pass(2, at);
            LabSystems.StopParallel();
        }
        else Info("natives v2: no kiln/control/systems.parallel: serial pass only");
        await TestRegistryFull();  // last: the natives it adds stay in this world's registry
    }

    private void RegisterNativesV2()
    {
        _nvSusp = Abilities.Natives.Register("NvSusp", 2, call => { call.SetInt(0, call.GetInt(1) * 2); return 1; });
        _nvWait = Abilities.Natives.Register("NvWait", 1, _ => 1);  // paces a loop: one step per call
        var sync = Abilities.Natives.RegisterSync("NvSync", 2, (ref NativeCall c) =>
        {
            Interlocked.Increment(ref _nvSyncCalls);
            if (Environment.CurrentManagedThreadId != _nvTick) Interlocked.Increment(ref _nvSyncOffTick);
            c.SetU32(0, c.GetU32(1) * 2);
            return 1;
        });
        Check(sync.Ok && sync.Hash == Abilities.HashOf("abilities-test.NvSync"), $"RegisterSync(NvSync): {sync}");
        _nvSync = sync.Hash;
        // Sum += I; I += 1; answers 1 while I < 5 (the program loops on it)
        var accum = Abilities.Natives.RegisterSync("NvAccum", 2, (ref NativeCall c) =>
        {
            Interlocked.Increment(ref _nvAccumCalls);
            var i = c.GetU32(1);
            c.SetU32(0, c.GetU32(0) + i);
            c.SetU32(1, i + 1);
            return (byte)(i + 1 < 5 ? 1 : 0);
        });
        Check(accum.Ok, $"RegisterSync(NvAccum): {accum}");
        _nvAccum = accum.Hash;
        var again = Abilities.Natives.RegisterSync("NvSync", 2, (ref NativeCall c) => 0);
        Check(!again.Ok && again.Reason?.Contains("registered") == true, $"a second RegisterSync of one name is Ok=false: {again.Reason}");
        var info = Abilities.Natives.All.FirstOrDefault(n => n.Hash == _nvSync);
        Info($"natives v2: Natives.All lists {info}");
    }

    private async Task NativesV2Pass(int pass, Func<double, double, WorldPosition> at)
    {
        var tag = $"pass {pass}{(LabSystems.Forced ? ", forced parallel" : "")}";
        await TestSyncSameStep(pass, tag, at(20, 60 + pass * 8));
        await TestSyncLoop(pass, tag, at(24, 60 + pass * 8));
        await TestUnloadDuringCast(pass, tag, at(28, 60 + pass * 8));
        var c = Abilities.Natives.LabCounters;
        Check(c is { Inflight: 0 }, $"{tag}: the in-flight counter is 0 in Pre ({c})");
    }

    // A: the suspending native answers one step after the start; B: the synchronous one in the start step.
    private async Task TestSyncSameStep(int pass, string tag, WorldPosition p)
    {
        var guidA = new Guid($"06c00001-0000-4000-8000-0000000000{pass:x2}");
        var guidB = new Guid($"06c00002-0000-4000-8000-0000000000{pass:x2}");
        Abilities.Programs.Register(ProgramBuilder.DoubleThenSignal(guidA, _nvSusp, 21, 0xA1), out var waitA);
        Abilities.Programs.Register(ProgramBuilder.DoubleThenSignal(guidB, _nvSync, 21, 0xB1), out var waitB);
        if (waitA || waitB) Info($"{tag}: programs wait for natives: A {waitA}, B {waitB}");
        _signals.Clear();
        _started.Clear();
        var calls0 = Interlocked.Read(ref _nvSyncCalls);
        var off0 = Interlocked.Read(ref _nvSyncOffTick);
        var eA = await Abilities.RunProgram(guidA, p);
        var eB = await Abilities.RunProgram(guidB, p with { X = p.X + 2 });
        _spawned.Add(eA);
        _spawned.Add(eB);
        await Steps(8);
        var stA = _started.FirstOrDefault(s => s.Impact.Self == eA);
        var stB = _started.FirstOrDefault(s => s.Impact.Self == eB);
        var sA = _signals.FirstOrDefault(s => s.Impact.Self == eA);
        var sB = _signals.FirstOrDefault(s => s.Impact.Self == eB);
        long D(ImpactSignalEvent? s, ImpactStartedEvent? st) => s is null || st is null ? -1 : (long)s.Step - (long)st.Step;
        Check(sB is { AsInt: 42 } && D(sB, stB) == 0 && sA is { AsInt: 42 } && D(sA, stA) == 1,
            $"{tag}: sync native: a program's signal arrives in its start step ({D(sB, stB)} steps; the suspending native: " +
            $"{D(sA, stA)}); values {sB?.AsInt}/{sA?.AsInt}; {Interlocked.Read(ref _nvSyncCalls) - calls0} sync call(s), " +
            $"{Interlocked.Read(ref _nvSyncOffTick) - off0} off the tick thread");
    }

    // A loop of five ECALLs of one sync native inside one instance: all in the start step.
    private async Task TestSyncLoop(int pass, string tag, WorldPosition p)
    {
        const uint hI = 0xFFFF0000, hSum = 0xFFFF0001, tagC = 0xC1;
        uint signal = Abilities.HashOf("kiln.Signal");
        uint[] code =
        [
            11, hI, 11, hSum, 19, 0x06c00011, _nvAccum,  // 0: args[0] = Sum, args[1] = I
            9, 0,                                       // 7: BRT 0 (loop while it answers 1)
            11, tagC, 11, hSum, 19, 0x06c00012, signal, 27,
            30,
        ];
        var guid = new Guid($"06c00003-0000-4000-8000-0000000000{pass:x2}");
        Abilities.Programs.Register(ProgramBuilder.Raw(guid, code, [30],
            [("I::value", Sint32, 4, new byte[4]), ("Sum::value", Sint32, 4, new byte[4])]), out _);
        _signals.Clear();
        _started.Clear();
        var calls0 = Interlocked.Read(ref _nvAccumCalls);
        var e = await Abilities.RunProgram(guid, p);
        _spawned.Add(e);
        await Steps(6);
        var st = _started.FirstOrDefault(s => s.Impact.Self == e);
        var sig = _signals.FirstOrDefault(s => s.Impact.Self == e);
        var calls = Interlocked.Read(ref _nvAccumCalls) - calls0;
        Check(calls == 5 && sig is { Tag: tagC, AsInt: 10 } && st is not null && sig.Step == st.Step,
            $"{tag}: a loop over 5 targets with a sync native finishes in 1 step: {calls} call(s), sum {sig?.AsInt}, " +
            $"started step {st?.Step}, signal step {sig?.Step}");
    }

    // natives-helper.Count is called every step by a looping program (a suspending native paces it); the helper mod is
    // unloaded while it runs: from the Pre that removed it no call reaches a handler, the next ECALL answers 0x1F
    // and the instance stops with Error.
    private async Task TestUnloadDuringCast(int pass, string tag, WorldPosition p)
    {
        var helper = Kiln.Mods.All.FirstOrDefault(m => m.Id == "natives-helper");
        if (helper is not { IsLoaded: true })
        {
            Info($"{tag}: natives-helper is not loaded ({helper?.State}): unload row skipped (deploy mods/tests/natives-helper)");
            return;
        }
        var count = Abilities.HashOf("natives-helper.Count");
        const uint hOut = 0xFFFF0000;
        uint[] code =
        [
            11, hOut, 19, 0x06c00021, count, 27,    // 0: natives-helper.Count(args[0] = Out); POP
            11, hOut, 19, 0x06c00022, _nvWait, 27,  // 6: abilities-test.NvWait(Out): suspends one step; POP
            8, 0,                                  // 12: BR 0
            30,
        ];
        var guid = new Guid($"06c00004-0000-4000-8000-0000000000{pass:x2}");
        var id = Abilities.Programs.Register(ProgramBuilder.Raw(guid, code, [30], [("Out::value", Sint32, 4, new byte[4])]), out _);
        _started.Clear();
        var e = await Abilities.RunProgram(guid, p);
        _spawned.Add(e);
        await Steps(10);
        var running = Abilities.Natives.LabCounters;
        var r = await Kiln.Mods.UnloadAsync("natives-helper");
        var at = Abilities.Natives.LabCounters;  // this Pre: the helper's Retire took the native off
        await Steps(10);
        var after = Abilities.Natives.LabCounters;
        var st = _started.FirstOrDefault(s => s.Impact.Self == e);
        var inst = st is null ? null : Abilities.Instance(st.Impact.ImpactId);
        Check(r.Ok && at is { } a && after is { } b && b.SyncCalls - a.SyncCalls == 0 && b.DeadCalls - a.DeadCalls == 1,
            $"{tag}: unload during a running cast: 0 calls after the Pre that removed it (sync calls before the unload " +
            $"{running?.SyncCalls}, at it {at?.SyncCalls}, 10 steps later {after?.SyncCalls}; calls of the removed native " +
            $"{(after?.DeadCalls ?? 0) - (at?.DeadCalls ?? 0)}; unload {r.Ok} {r.Message}; program 0x{id:x8} instance now: " +
            $"{(inst is null ? "gone" : inst.Flags.ToString())})");
    }

    // Sync natives until a game-state limit says no: Ok = false with the reason (the registry keeps one entry free).
    private async Task TestRegistryFull()
    {
        var n = 0;
        NativeResult? last = null;
        for (var i = 0; i < 300; i++)
        {
            last = Abilities.Natives.RegisterSync($"NvFill{i}", 0, (ref NativeCall c) => 1);
            if (!last.Ok) break;
            n++;
        }
        var status = Abilities.Status;
        Check(last is { Ok: false } && (last.Reason?.Contains("registry full") == true || last.Reason?.Contains("slot") == true),
            $"registry full is Ok=false after {n} more sync native(s): \"{last?.Reason}\" (registry {status?.VanillaNatives} vanilla + " +
            $"{status?.KilnNatives} Kiln, capacity {status?.NativeCapacity})");
        await Task.CompletedTask;
    }
}
