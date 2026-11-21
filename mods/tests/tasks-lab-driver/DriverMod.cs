// SPDX-License-Identifier: MIT
// Reloads tasks-lab-test once its first load wrote ready-1 (its resident tasks running: one waiting for its cancel, one
// ignoring it for 3 s, one suspended, a reader, pending hops), and checks that the server's step never waited for them,
// that the waiting task unwound, the ignoring one ended late, and the mod came back (ready-2).
// "tasks-lab-driver: PASS|FAIL ...", then "tasks-lab-driver done: N PASS, M FAIL".
using System.Diagnostics;
using Kiln;

namespace TasksLabDriver;

public sealed class DriverMod : Mod
{
    private const string Me = "tasks-lab-driver", Target = "tasks-lab-test";
    private int _pass, _fail;

    private void Check(bool ok, string what)
    {
        if (ok) { _pass++; Log.Info($"{Me}: PASS {what}"); }
        else { _fail++; Log.Error($"{Me}: FAIL {what}"); }
    }

    public override void OnLoad()
    {
        if (!Client.IsDedicatedServer) return;
        _ = Run().ContinueWith(t =>
        {
            if (t.Exception != null) Check(false, $"the driver threw: {t.Exception.InnerException}");
            Log.Info($"{Me} done: {_pass} PASS, {_fail} FAIL");
        });
    }

    private string TargetData(string name) => Path.GetFullPath(Path.Combine(Context.DataDirectory, "..", Target, name));

    private bool Mine(string name) => File.Exists(TargetData(name)) && File.ReadAllText(TargetData(name)) == $"{Environment.ProcessId}";

    private async Task<bool> WaitFor(Func<bool> what, int steps)
    {
        for (var i = 0; i < steps; i++)
        {
            if (what()) return true;
            await Context.Scheduler.NextTick();
        }
        return what();
    }

    private async Task<(ModOperationResult? Result, List<double> Gaps, long Ms)> Reload()
    {
        var gaps = new List<double>();
        var sw = Stopwatch.StartNew();
        var reload = Mods.ReloadAsync(Target);
        var last = sw.Elapsed.TotalMilliseconds;
        while (!(reload.IsCompleted && sw.ElapsedMilliseconds > 6000) && sw.ElapsedMilliseconds < 60_000)
        {
            await Context.Scheduler.NextTick();
            var now = sw.Elapsed.TotalMilliseconds;
            gaps.Add(now - last);
            last = now;
        }
        return (reload.IsCompleted ? await reload : null, gaps, sw.ElapsedMilliseconds);
    }

    private async Task Run()
    {
        // the first load's checks take a few minutes (the load phase and the resident minute among them)
        if (!await WaitFor(() => Mine("ready-1"), 25 * 60 * 20)) { Check(false, $"{Target} wrote ready-1 within 20 minutes"); return; }
        foreach (var f in new[] { "unwound-1", "ignored-1" }) if (File.Exists(TargetData(f))) File.Delete(TargetData(f));

        var (r, gaps, ms) = await Reload();
        Check(r is { Ok: true }, $"Mods.ReloadAsync({Target}) with resident tasks in flight: {r?.Ok} {r?.Message}");
        Check(File.Exists(TargetData("unwound-1")), "the resident task waiting for its cancel unwound (its finally ran)");
        Check(await WaitFor(() => File.Exists(TargetData("ignored-1")), 25 * 10),
            $"the resident task that ignores its cancel ended 3 s after it: {(File.Exists(TargetData("ignored-1")) ? File.ReadAllText(TargetData("ignored-1")) + " ms" : "not yet")}");
        if (!await WaitFor(() => Mine("ready-2"), 25 * 120)) { Check(false, $"{Target} loaded again (ready-2)"); return; }

        // the control: the same reload with no task in flight
        var (r2, gaps2, ms2) = await Reload();
        Check(r2 is { Ok: true }, $"the control reload, no task in flight: {r2?.Ok} {r2?.Message}");
        var withTasks = gaps.DefaultIfEmpty(0).Max();
        var without = gaps2.DefaultIfEmpty(0).Max();
        Check(withTasks < without + 500,
            $"the server step did not wait for the unloading mod's tasks: the longest step during the reload with tasks in flight {withTasks:0} ms ({gaps.Count} steps in {ms} ms), without them {without:0} ms ({gaps2.Count} steps in {ms2} ms)");
        if (!await WaitFor(() => Mine("ready-3"), 25 * 300)) { Check(false, $"{Target} loaded a third time and ran its resident tasks (ready-3)"); return; }

        // again with tasks in flight, and the next load starts its own at once while the old load's task that ignores its
        // cancel still runs (the load context stays pinned meanwhile)
        var (r3, gaps3, ms3) = await Reload();
        Check(r3 is { Ok: true } && gaps3.DefaultIfEmpty(0).Max() < without + 500,
            $"a third reload with tasks in flight: {r3?.Ok} {r3?.Message}; the longest step {gaps3.DefaultIfEmpty(0).Max():0} ms ({gaps3.Count} steps in {ms3} ms)");
        Check(await WaitFor(() => Mine("ready-4"), 25 * 300), $"{Target} loaded a fourth time, its resident tasks ran beside the old load's pinned one (ready-4)");
        Log.Info($"{Me}: READY exit");
    }
}
