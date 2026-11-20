// SPDX-License-Identifier: MIT
// Lab test of Kiln's base rules (docs/kiln/services/build.md "Base rules and base limits") on a dedicated server, with three
// mods that change the Flame Altar counts and build-zone sizes in one start:
//   base-rules-test (this mod): a BaseRulesBlock staged in OnBoot, "roomy": at least 6 altars, altar level 1 at 70 m,
//     level 5 at 250 x 200 x 250 m;
//   base-rules-data (fixtures/data, a data-only pack): blocks/bases.json "baseRules" "wide": 3 more altars, build areas at 120%;
//   base-rules-mfa (fixtures/mfa, data-only): More Flame Altars 0.4.0's own resource patch, exactly its shape (Altars 4,
//     ExtraAltars 2, BuildArea 150): altarsPerFlameLevel[*] {add 2, min 4, max 16, raiseOnly}, buildzoneSizesPerAltarLevel[*].x/y/z
//     {mul 1.5, raiseOnly}.
// Once the world is up it reads Build.BaseLimits and checks: nobody was refused, every Flame level's count and every altar
// level's size is the largest any of the three asks for (from the game's own table, read back as OriginalCounts), the
// served table as Kiln.Quests reads it agrees, and a Flame Altar placed now gets the composed level-1 zone (its half-extent
// read back through Build.Bases). The lab script starts the server twice: the second start must read the same (the rules
// are applied again at every start). Lines "base-rules-test: PASS|FAIL|INFO ..." and a closing "DONE run=<n> pass= fail=".
using System.Numerics;
using Kiln;

namespace BaseRulesTest;

public sealed class BaseRulesTestMod : Mod
{
    private int _pass, _fail;

    private void Check(bool ok, string what)
    {
        if (ok) { _pass++; Log.Info($"base-rules-test: PASS {what}"); }
        else { _fail++; Log.Error($"base-rules-test: FAIL {what}"); }
    }

    private void Info(string what) => Log.Info($"base-rules-test: INFO {what}");

    public override void OnBoot(BootContext boot)
    {
        var staged = BootDefinitions.For(boot).Define("roomy", new BaseRulesBlock
        {
            MinimumAltars = 6,
            Sizes = [new AltarSize(1, new Vector3(70, 70, 70)), new AltarSize(5, new Vector3(250, 200, 250))],
        });
        if (!staged.Ok) throw new InvalidOperationException($"base-rules-test: roomy was not staged: {staged.Reason}");
    }

    public override void OnLoad()
    {
        if (Mods.Side != ModSide.Server) return;
        _ = Run();
    }

    // the three mods' rules, of the game's own value
    private static int Mfa(int g) => Math.Max(g, Math.Min(16, Math.Max(4, g + 2)));
    private static int Roomy(int g) => Math.Max(g, Math.Min(16, Math.Max(6, g)));
    private static int Wide(int g) => Math.Max(g, Math.Min(16, g + 3));
    private static float MfaSize(float g) => Math.Max(g, g * 1.5f);
    private static float WideSize(float g) => Math.Max(g, Math.Min(256f, g * 1.2f));
    private static float RoomySize(int level, int axis, float g) =>
        level == 1 ? Math.Max(g, 70f) : level == 5 ? Math.Max(g, axis == 1 ? 200f : 250f) : g;

    private static float Axis(Vector3 v, int a) => a == 0 ? v.X : a == 1 ? v.Y : v.Z;

    private async Task Run()
    {
        var run = 0;
        try
        {
            var marker = Path.Combine(Context.DataDirectory, "runs.txt");
            run = (File.Exists(marker) ? int.Parse(File.ReadAllText(marker).Trim()) : 0) + 1;
            File.WriteAllText(marker, run.ToString());
            Info($"run {run} (the lab starts the server twice: the second start applies the rules again)");
            var state = Mods.BootState(Context.Info.Id);
            Check(state.State == BootModState.Committed, $"run {run}: this mod's boot content is committed ({state.State}{(state.Issues.Count > 0 ? ": " + state.Issues[0].Reason : "")})");
            foreach (var other in new[] { "base-rules-data", "base-rules-mfa" })
            {
                var s = Mods.BootState(other);
                Check(s.State == BootModState.Committed, $"run {run}: {other} is not refused ({s.State}{(s.Issues.Count > 0 ? ": " + s.Issues[0].Reason : "")})");
            }
            Check(Capabilities.Is("build.base_rules"), $"run {run}: build.base_rules is on ({Capabilities.Why("build.base_rules")})");
            BaseLimits? limits = null;
            var sw = System.Diagnostics.Stopwatch.StartNew();
            while (sw.Elapsed.TotalSeconds < 240)
            {
                if (World.Available && Build.Available && (limits = Build.BaseLimits) is not null) break;
                await Context.Scheduler.NextTick();
            }
            Check(Capabilities.Is("build.base_limits"), $"run {run}: build.base_limits is on ({Capabilities.Why("build.base_limits")})");
            if (limits is null)
            {
                Check(false, $"run {run}: Build.BaseLimits answered nothing within 240 s");
                return;
            }
            Info($"run {run}: flame level {limits.FlameLevel}, {limits.CurrentAltars} altar(s); game's counts [{string.Join(",", limits.OriginalCounts)}], " +
                 $"effective [{string.Join(",", limits.EffectiveCounts)}]; game's sizes {string.Join(" ", limits.OriginalSizes.Select(s => $"L{s.AltarLevel}={s.Size.X}x{s.Size.Y}x{s.Size.Z}"))}, " +
                 $"effective {string.Join(" ", limits.EffectiveSizes.Select(s => $"L{s.AltarLevel}={s.Size.X}x{s.Size.Y}x{s.Size.Z}"))}");
            Check(limits.AppliedRules.Contains("base-rules-test:roomy") && limits.AppliedRules.Contains("base-rules-data:wide"),
                $"run {run}: both mods' rules applied [{string.Join(", ", limits.AppliedRules)}]");
            Check(limits.PatchedBy.Contains("base-rules-mfa"), $"run {run}: More Flame Altars' own patch applied too [{string.Join(", ", limits.PatchedBy)}]");
            Check(limits.OriginalCounts.Count > 1 && limits.OriginalCounts.Count == limits.EffectiveCounts.Count,
                $"run {run}: {limits.OriginalCounts.Count} Flame levels read from the game's table and from the served one");
            var counts = 0;
            for (var i = 0; i < limits.EffectiveCounts.Count; i++)
            {
                var g = limits.OriginalCounts[i];
                var want = Math.Max(Mfa(g), Math.Max(Roomy(g), Wide(g)));
                if (limits.EffectiveCounts[i] == want) counts++;
                else Check(false, $"run {run}: flame level {i}: {limits.EffectiveCounts[i]} altars, want {want} = max(mfa {Mfa(g)}, roomy {Roomy(g)}, wide {Wide(g)}) of the game's {g}");
            }
            Check(counts == limits.EffectiveCounts.Count, $"run {run}: effective count at every flame level is the max of the three (" +
                string.Join(", ", limits.EffectiveCounts.Select((c, i) => $"{i}:{limits.OriginalCounts[i]}->{c}")) + ")");
            Check(limits.EffectiveCount == limits.EffectiveCounts[Math.Min(limits.FlameLevel, limits.EffectiveCounts.Count - 1)] &&
                  limits.OriginalCount == limits.OriginalCounts[Math.Min(limits.FlameLevel, limits.OriginalCounts.Count - 1)],
                $"run {run}: at the world's flame level {limits.FlameLevel}: {limits.OriginalCount} -> {limits.EffectiveCount}");
            var sizes = 0;
            Check(limits.EffectiveSizes.Count == 5 && limits.OriginalSizes.Count == 5, $"run {run}: 5 altar levels of sizes");
            foreach (var e in limits.EffectiveSizes)
            {
                var o = limits.OriginalSizes.First(x => x.AltarLevel == e.AltarLevel);
                for (var a = 0; a < 3; a++)
                {
                    var g = Axis(o.Size, a);
                    var want = Math.Max(MfaSize(g), Math.Max(RoomySize(e.AltarLevel, a, g), WideSize(g)));
                    if (Math.Abs(Axis(e.Size, a) - want) < 0.01f) sizes++;
                    else Check(false, $"run {run}: altar level {e.AltarLevel} axis {"xyz"[a]}: {Axis(e.Size, a)} m, want {want} of the game's {g}");
                }
            }
            Check(sizes == 15, $"run {run}: effective sizes are the max of the three on every axis (" +
                string.Join(", ", limits.EffectiveSizes.Select(s => $"L{s.AltarLevel} {s.Size.X}x{s.Size.Y}x{s.Size.Z}")) + ")");
            // the served table as another existing API reads it (Kiln.Quests: WorldProgress.LimitsAt)
            var agree = Enumerable.Range(0, limits.EffectiveCounts.Count).All(i => WorldProgress.LimitsAt(i)?.Altars == limits.EffectiveCounts[i]);
            Check(agree, $"run {run}: WorldProgress.LimitsAt agrees at every flame level");
            Info($"run {run}: the game's own count getter (min(table, 16)) is not reachable through a Kiln API; the served table is what it reads");
            await PlaceAltar(run, limits);
        }
        catch (Exception e)
        {
            Check(false, $"run {run}: exception: {e}");
        }
        Log.Info($"base-rules-test: DONE run={run} pass={_pass} fail={_fail}");
    }

    // A Flame Altar placed now (transient: gone at the restart) gets the composed level-1 zone: half of the effective size.
    private async Task PlaceAltar(int run, BaseLimits limits)
    {
        var level1 = limits.EffectiveSizes.First(s => s.AltarLevel == 1).Size;
        var at = new WorldPosition(3800 + 90 * run, 835, 1500);
        EntityId altar;
        try
        {
            altar = Build.PlaceProp("Home_Base_FlameAltar", at, options: new BuildOptions { Flags = BuildFlags.AllowAltars | BuildFlags.Transient, Journal = false });
        }
        catch (Exception e) when (e is BuildException or ArgumentException)
        {
            Check(false, $"run {run}: a Flame Altar could not be placed at {at}: {e.Message}");
            return;
        }
        Check(!altar.IsNone, $"run {run}: a Flame Altar placed at {at} ({altar})");
        if (altar.IsNone) return;
        PlayerBase? zone = null;
        var sw = System.Diagnostics.Stopwatch.StartNew();
        while (sw.Elapsed.TotalSeconds < 60)
        {
            zone = Build.Bases.FirstOrDefault(b => b.Altar == altar);
            if (zone is not null && zone.Half.X > 0) break;
            await Context.Scheduler.NextTick();
        }
        if (zone is null)
        {
            Check(false, $"run {run}: the new altar {altar} has no base zone within 60 s");
            return;
        }
        Check(Math.Abs(zone.Half.X * 2 - level1.X) < 0.01f && Math.Abs(zone.Half.Y * 2 - level1.Y) < 0.01f && Math.Abs(zone.Half.Z * 2 - level1.Z) < 0.01f,
            $"run {run}: the new altar's zone half-extent {zone.Half.X}x{zone.Half.Y}x{zone.Half.Z} is half the composed level-1 size {level1.X}x{level1.Y}x{level1.Z}");
    }
}
