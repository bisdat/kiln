// SPDX-License-Identifier: MIT
using System.Text.Json;
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
        File.Delete(Path.Combine(Context.DataDirectory, "quota-proof.json"));
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
        File.Delete(Path.Combine(Context.DataDirectory, "quota-proof.json"));
        foreach (var b in _bound) b.Dispose();
    }

    private async Task Run()
    {
        try
        {
            for (var n = 0; n < 1500 && World.Status.WorldSteps == 0; n++) await Context.Scheduler.NextTick();
            for (var n = 0; n < 50; n++) await Context.Scheduler.NextTick();
            var runs = _runs.Take(_bound.Count).Select(r => Volatile.Read(ref r)).ToArray();
            Check(_bound.Count > 0 && runs.All(r => r > 0), $"{runs.Count(r => r > 0)} of {_bound.Count} bound programs ran (runs {(runs.Length > 0 ? runs.Min() : 0)}..{(runs.Length > 0 ? runs.Max() : 0)})");
            var generation = World.GetProgramBudget().World?.Generation;
            var proof = Path.Combine(Context.DataDirectory, "quota-proof.json");
            Directory.CreateDirectory(Context.DataDirectory);
            File.WriteAllText(proof + ".tmp", JsonSerializer.Serialize(new { generation, ran = runs.Count(r => r > 0), minimumRuns = runs.Length > 0 ? runs.Min() : 0 }));
            File.Move(proof + ".tmp", proof, true);
            var s = World.Status;
            Log.Info($"{Me}: World.Status kiln programs {s.KilnPrograms}, engine {s.EnginePrograms} of {s.EngineMax}");
#if QUOTA_A
            Check(_bound.Count == 32 && _refused == 0, $"a file of 32 declarations: {_bound.Count} bound, {_refused} refused");
            // S6: the program budget for the server world, and for a client world a dedicated server does not have
            var budget = World.GetProgramBudget();
            var k = budget.KilnSlots;
            var e = budget.EngineSlots;
            Log.Info($"{Me}: program budget: Ok={budget.Ok} World={budget.World} KilnSlots {k.Capacity} used {k.KilnUsed} reserved {k.Reserved} " +
                     $"available {k.Available} expanded {k.Expanded}; EngineSlots {e.Capacity} game {e.GameUsed} kiln {e.KilnUsed} " +
                     $"other {e.Reserved} available {e.Available}; per file {budget.PerFileMax}; revision {k.Revision}/{e.Revision}");
            Check(budget.Ok && k.Capacity == 64 && k.Expanded && k.KilnUsed + k.Reserved == (uint)s.KilnPrograms && e.Capacity == (uint)s.EngineMax &&
                  e.GameUsed + e.KilnUsed + e.Reserved == (uint)s.EnginePrograms && e.KilnUsed > 0 && budget.PerFileMax == 32 &&
                  budget.AccessLimits.IteratorBytes == 4096,
                $"program budget: KilnSlots {k.Capacity} (Kiln {k.KilnUsed}, reserved {k.Reserved}) = World.Status {s.KilnPrograms}; " +
                $"EngineSlots {e.Capacity} (game {e.GameUsed}, Kiln {e.KilnUsed}, other {e.Reserved}) = World.Status {s.EnginePrograms} of {s.EngineMax}");
            var client = World.GetProgramBudget(CapacityWorldKind.Client);
            Check(!client.Ok && client.Reason == "NoWorld" && client.World is null && client.KilnSlots.Available is null,
                $"the client kind on a dedicated server: Ok={client.Ok} Reason={client.Reason}");
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
