// SPDX-License-Identifier: MIT
using Kiln;

namespace WorldProgramsQuota;

/// <summary>
/// kiln.world's program quota (chapter 17.4, package 17b S5): this mod declares 32 programs in its kiln.programs.json, the
/// most a file may hold; built as world-programs-quota-a and -b, the two declare 64 together. kiln.world gives the mods'
/// declarations what is left of its 64 slots after Kiln's own services, keeping the last 8 for services that register
/// later: the declarations it refused cannot be bound (the log says why), every bound program runs every step (a SYSTEM
/// program: no entity query, so it runs without a player too). Mod a reports the ledger.
/// </summary>
public sealed class QuotaMod : Mod
{
    private readonly long[] _runs = new long[32];
    private readonly List<IDisposable> _bound = [];
    private int _pass, _fail, _refused;
    private string Me => Context.Info.Id;

    private void Check(bool ok, string what)
    {
        if (ok) _pass++;
        else _fail++;
        Log.Info($"{Me}: {(ok ? "PASS" : "FAIL")} {what}");
    }

    public override void OnLoad()
    {
        for (var i = 0; i < 32; i++)
        {
            var k = i;
            try
            {
                _bound.Add(EcsPrograms.Bind($"p{k:D2}", _ => Interlocked.Increment(ref _runs[k])));
            }
            catch (InvalidOperationException e)
            {
                if (_refused++ == 0) Log.Info($"{Me}: p{k:D2} is not bound: {e.Message}");
            }
        }
        Log.Info($"{Me}: {_bound.Count} of 32 declarations bound, {_refused} not reserved");
        _ = Run();
    }

    public override void OnUnload()
    {
        foreach (var b in _bound) b.Dispose();
    }

    private async Task Run()
    {
        try
        {
            for (var n = 0; n < 1500 && World.Status.WorldSteps == 0; n++) await Context.Scheduler.NextTick();
            for (var n = 0; n < 50; n++) await Context.Scheduler.NextTick();
            var runs = _runs.Take(_bound.Count).Select(r => Volatile.Read(ref r)).ToArray();
            Check(_bound.Count > 0 && runs.All(r => r > 0), $"{runs.Count(r => r > 0)} of {_bound.Count} bound programs ran (runs {runs.Min()}..{runs.Max()})");
            var s = World.Status;
            Log.Info($"{Me}: World.Status kiln programs {s.KilnPrograms}, engine {s.EnginePrograms} of {s.EngineMax}");
#if QUOTA_A
            Check(_bound.Count == 32 && _refused == 0, $"a file of 32 declarations: {_bound.Count} bound, {_refused} refused");
#else
            Check(s.KilnPrograms <= 64 && _bound.Count + _refused == 32,
                $"64 declarations from two mods: kiln.world holds {s.KilnPrograms} of 64 programs; this mod's {_refused} beyond the mods' share are refused, not bound");
#endif
        }
        catch (Exception e)
        {
            Check(false, $"aborted: {e}");
        }
        Log.Info($"{Me}: done {_pass} PASS, {_fail} FAIL");
    }
}
