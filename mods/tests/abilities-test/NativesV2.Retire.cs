// SPDX-License-Identifier: MIT
// abilities-test rows for the review's P1 (dev.29): a native's handler inside while its mod unloads, reloads, or the
// world stops. natives-helper.Hold blocks until data/gate-open exists; a background thread (not the tick: the tick
// waits for the step the handler holds) asks for the mod's unload or reload through kiln/control while the handler is
// inside, checks that nothing was retired yet, and only then opens the gate. With kiln/control/impact.unpin (lab) and
// the forced-parallel window, impact_simulation runs on the game's workers: the probe row shows where handlers ran.
// The world-stop row hands off to the shell that runs the lab (data/stop-request): its lines are read from kiln.log.
using Kiln;
using Kiln.Testing;

namespace AbilitiesTest;

public sealed partial class AbilitiesTestMod
{
    private string HelperFile(string name) => Path.Combine(HelperData, name);
    private string ControlDir => Path.GetFullPath(Path.Combine(Context.DataDirectory, "..", "..", "control"));

    private static (string[] Parts, DateTime At) ReadStamp(string file)
    {
        if (!File.Exists(file)) return ([], default);
        var parts = File.ReadAllText(file).Split(' ', StringSplitOptions.RemoveEmptyEntries);
        return (parts, parts.Length > 0 && DateTime.TryParse(parts[^1], null, System.Globalization.DateTimeStyles.RoundtripKind, out var t) ? t : default);
    }

    private int LogCount(string text)
    {
        var log = Path.GetFullPath(Path.Combine(Context.DataDirectory, "..", "..", "logs", "kiln.log"));
        try
        {
            using var f = new FileStream(log, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
            using var r = new StreamReader(f);
            var all = r.ReadToEnd();
            var n = 0;
            for (var i = all.IndexOf(text, StringComparison.Ordinal); i >= 0; i = all.IndexOf(text, i + 1, StringComparison.Ordinal)) n++;
            return n;
        }
        catch (IOException) { return -1; }
    }

    private async Task<bool> HelperReady()
    {
        if (!Kiln.Mods.All.Any(m => m.Id == "natives-helper")) { Info("natives-helper is not deployed: retirement rows skipped"); return false; }
        if (!Kiln.Mods.All.Any(m => m.Id == "natives-helper" && m.IsLoaded)) Info($"natives-helper loaded for the retirement rows: {await LoadHelper(argc2: false)}");
        return true;
    }

    // Where the handlers run: with impact.unpin in the forced window, the game's workers.
    private async Task TestProbeThreads(WorldPosition p)
    {
        if (!await HelperReady()) return;
        File.Delete(HelperFile("probe"));
        var probe = Abilities.HashOf("natives-helper.Probe");
        uint[] code = [11, 0xFFFF0000, 19, 0x06c00091, probe, 27, 30];
        var spawned = new List<EntityId>();
        for (var i = 0; i < 12; i++)
        {
            var guid = new Guid($"06c0000c-0000-4000-8000-0000000000{i:x2}");
            Abilities.Programs.Register(ProgramBuilder.Raw(guid, code, [30], [("Out::value", Sint32, 4, new byte[4])]), out _);
            spawned.Add(await Abilities.RunProgram(guid, p with { X = p.X + i }));
        }
        _spawned.AddRange(spawned);
        await Steps(8);
        var (parts, _) = ReadStamp(HelperFile("probe"));
        var unpin = File.Exists(Path.Combine(ControlDir, "impact.unpin"));
        var calls = parts.Length > 0 ? int.Parse(parts[0]) : 0;
        var off = parts.Length > 1 ? int.Parse(parts[1]) : 0;
        var reads = parts.Length > 2 ? int.Parse(parts[2]) : -1;
        if (unpin && LabSystems.Forced)
        {
            Check(calls == 12 && off > 0, $"with impact_simulation unpinned in the forced window, sync native handlers run on the game's workers: {off} of {calls} call(s) off the tick thread");
            // the bridge's serial condition: off for this world with its reason, every TryRead refused, said once in kiln.log
            var row = Capabilities.Get("abilities.natives.ecs");
            var lines = LogCount("NativeContext's reads and writes of other entities are off for this world: impact_simulation does not run serially");
            Check(reads == 0 && row.State == CapabilityState.Unavailable && row.ReasonText.Contains("does not run serially") && lines == 1,
                $"with the pin cleared, the ECS bridge is off for this world: {reads} of {calls} TryRead(s) answered, abilities.natives.ecs " +
                $"{row.State} ({row.ReasonText}), {lines} kiln.log line(s) saying so");
        }
        else Info($"probe: {off} of {calls} call(s) off the tick thread (unpin {unpin}, forced {LabSystems.Forced})");
    }

    // op "unload" or "reload": requested while Hold's handler is inside; the retirement waits for it.
    private async Task TestRetireWhileInside(WorldPosition p, string op)
    {
        if (!await HelperReady()) return;
        foreach (var f in new[] { "entered", "gate-open", "finished", "unloaded" }) File.Delete(HelperFile(f));
        var (loaded0, _) = ReadStamp(HelperFile("loaded"));
        var hold = Abilities.HashOf("natives-helper.Hold");
        var guid = new Guid($"06c0000d-0000-4000-8000-0000000000{(op == "unload" ? 1 : 2):x2}");
        Abilities.Programs.Register(ProgramBuilder.Raw(guid, [11, 0xFFFF0000, 19, 0x06c000a1, hold, 27, 30], [30],
            [("Out::value", Sint32, 4, new byte[4])]), out _);
        var w0 = Abilities.Natives.LabRetire?.Watchdog ?? 0;
        var control = ControlDir;
        var entered = HelperFile("entered");
        var unloaded = HelperFile("unloaded");
        var loadedFile = HelperFile("loaded");
        var gate = HelperFile("gate-open");
        // the second thread: it asks while the handler is inside, then looks whether anything was retired, then lets go
        var asker = Task.Run(async () =>
        {
            var sw = System.Diagnostics.Stopwatch.StartNew();
            while (!File.Exists(entered) && sw.Elapsed.TotalSeconds < 20) await Task.Delay(5);
            var inside = File.Exists(entered);
            var requested = DateTime.UtcNow;
            File.WriteAllText(Path.Combine(control, $"r06c-{op}.cmd"), $"{op} natives-helper");
            await Task.Delay(1500);  // the request waits while the handler is inside (the watchdog names it after 1 s)
            var nonceNow = File.Exists(loadedFile) ? File.ReadAllText(loadedFile).Split(' ')[0] : "";
            File.WriteAllText(gate, "");
            return (inside, requested, early: File.Exists(unloaded), nonceNow);
        });
        var e = await Abilities.RunProgram(guid, p);
        _spawned.Add(e);
        var (wasInside, requested, early, nonceAtGate) = await asker;
        // the host takes the request in its next tick, after the step the handler held
        for (var i = 0; i < 400 && !File.Exists(unloaded); i++) await Steps(1);
        if (op == "reload") for (var i = 0; i < 400 && ReadStamp(loadedFile).Parts.FirstOrDefault() == loaded0.FirstOrDefault(); i++) await Steps(1);
        var (ent, _) = ReadStamp(entered);
        var (fin, finAt) = ReadStamp(HelperFile("finished"));
        var (unl, unlAt) = ReadStamp(unloaded);
        var oldNonce = loaded0.FirstOrDefault() ?? "";
        var w1 = Abilities.Natives.LabRetire?.Watchdog ?? 0;
        Check(wasInside && !early && nonceAtGate == oldNonce && fin is ["ok", "natives-helper", _, _, _] && fin[2] == oldNonce &&
              unl.FirstOrDefault() == oldNonce && requested < finAt && finAt <= unlAt,
            $"{op} asked for while natives-helper's handler is inside (thread: {(ent.Length > 1 ? ent[1] : "?")}): the retirement waits " +
            $"(nothing retired {1.5:F1} s after the request), the handler finishes with its mod's own state ({string.Join(' ', fin.Take(4))}), " +
            $"then the mod unloads ({(unlAt - finAt).TotalMilliseconds:F0} ms later)");
        Check(w1 > w0, $"{op}: the watchdog named the handler while it was inside ({w1 - w0} report(s), kiln.log has the line)");
        // no call after: the old load's handler gets none (unload: the hash is dead; reload: the new load answers)
        var c0 = Abilities.Natives.LabCounters;
        File.Delete(entered);
        File.WriteAllText(gate, "");
        var e2 = await Abilities.RunProgram(guid, p with { Z = p.Z + 2 });
        _spawned.Add(e2);
        await Steps(6);
        var (ent2, _) = ReadStamp(entered);
        var c1 = Abilities.Natives.LabCounters;
        if (op == "unload")
            Check(ent2.Length == 0 && (c1?.DeadCalls ?? 0) - (c0?.DeadCalls ?? 0) == 1,
                $"unload: no call reaches the unloaded mod's handler after its retirement (the next call is a removed native's)");
        else
            Check(ent2.Length > 2 && ent2[2] != oldNonce,
                $"reload: the next call reaches the new load ({(ent2.Length > 2 ? ent2[2] : "none")}), not the retired one ({oldNonce})");
        File.Delete(gate);
    }

    // The world's stop while the handler is inside: the shell that runs the lab watches data/stop-request and stops
    // the server; the handler is let go 3 s later. kiln.log then must show the handler's last line before
    // "the server world stops with no Kiln native call inside" (read by the shell, not by this mod).
    private async Task TestWorldStopWhileInside(WorldPosition p)
    {
        if (!File.Exists(Path.Combine(Context.DataDirectory, "world-stop-row")) || !await HelperReady()) return;
        foreach (var f in new[] { "entered", "gate-open", "finished" }) File.Delete(HelperFile(f));
        var hold = Abilities.HashOf("natives-helper.Hold");
        var guid = new Guid("06c0000d-0000-4000-8000-000000000003");
        Abilities.Programs.Register(ProgramBuilder.Raw(guid, [11, 0xFFFF0000, 19, 0x06c000a2, hold, 27, 30], [30],
            [("Out::value", Sint32, 4, new byte[4])]), out _);
        var entered = HelperFile("entered");
        var stop = HelperFile("stop-request");
        var gate = HelperFile("gate-open");
        _ = Task.Run(async () =>
        {
            var sw = System.Diagnostics.Stopwatch.StartNew();
            while (!File.Exists(entered) && sw.Elapsed.TotalSeconds < 20) await Task.Delay(5);
            File.WriteAllText(stop, DateTime.UtcNow.ToString("O"));
            await Task.Delay(3000);
            File.WriteAllText(gate, "");
        });
        Info("world stop row: the handler is held; the lab's shell stops the server now");
        _spawned.Add(await Abilities.RunProgram(guid, p));
        await Steps(4);
    }

    // Review round 2: hk_create / hk_run when Kiln's lock is held past their bound (lab fixture kiln/control/impact.lockhold):
    // the instance created then gets no hold and no tracking (no Starting, no Started event), its run is delayed and
    // still happens, the bypass counter grows; once the lock is free a run is complete again (held, started, signal).
    private async Task TestLockGiveUp(WorldPosition p)
    {
        if (!File.Exists(Path.Combine(ControlDir, "impact.lockhold"))) { Info("no kiln/control/impact.lockhold: the lock give-up row is skipped"); return; }
        const uint tag = 0x6C0E;
        var guid = new Guid("06c0000e-0000-4000-8000-000000000001");
        var id = Abilities.Programs.Register(ProgramBuilder.Raw(guid,
            [11, tag, 11, 0xFFFF0000, 19, 0x06c000e1, Abilities.HashOf("kiln.Signal"), 27, 30], [30],
            [("Out::value", Sint32, 4, BitConverter.GetBytes(7))]), out _);
        using var hold = Abilities.HoldPrograms(id);
        var b0 = Abilities.Natives.LabLockBypass ?? 0;
        _starting.Clear();
        _started.Clear();
        _signals.Clear();
        File.WriteAllText(Path.Combine(ControlDir, "impact.lockhold.now"), "300");
        await Steps(2);  // armed in a Pre: the next creation holds the lock
        var e1 = await Abilities.RunProgram(guid, p);
        _spawned.Add(e1);
        await Steps(20);
        var b1 = Abilities.Natives.LabLockBypass ?? 0;
        var sig1 = _signals.FirstOrDefault(s => s.Impact.Self == e1 && s.Tag == tag);
        var starting1 = _starting.Any(s => s.Impact.Self == e1);
        var started1 = _started.Any(s => s.Impact.Self == e1);
        Check(b1 > b0 && !starting1 && !started1 && sig1 is { AsInt: 7 },
            $"with Kiln's lock held past the bound, an instance's creation gives up: {b1 - b0} bypass(es), no hold " +
            $"({(starting1 ? "Starting seen" : "no Starting")}), no tracking ({(started1 ? "Started seen" : "no Started")}), and its run " +
            $"still happens (signal {sig1?.AsInt} at step {sig1?.Step})");
        // recovered: a complete run
        var e2 = await Abilities.RunProgram(guid, p with { Z = p.Z + 3 });
        _spawned.Add(e2);
        await Steps(8);
        var sig2 = _signals.FirstOrDefault(s => s.Impact.Self == e2 && s.Tag == tag);
        Check(_starting.Any(s => s.Impact.Self == e2) && _started.Any(s => s.Impact.Self == e2) && sig2 is { AsInt: 7 },
            $"once the lock is free again, a run is complete: held (Starting), tracked (Started), signal {sig2?.AsInt}");
    }
}
