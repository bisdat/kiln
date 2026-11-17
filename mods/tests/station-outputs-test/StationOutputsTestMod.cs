// SPDX-License-Identifier: MIT
// Lab test of station outputs as crafting stock (GitHub #68), player-free, on the standard lab save (one base). It finds,
// among the game's production stations, a target whose recipe needs charcoal (a smelter or a forge) by placing each
// station template once, then places a charcoal kiln K (stopped, the charcoal recipe selected, logs queued in it, charcoal
// given into it as its finished items), the target F (stopped, that recipe selected) and a Magic chest, and checks
// against the inventories:
//   O1 opted out, a fill of F is the game's own: no charcoal from K, Plan lends nothing;
//   O2 bisdat's case: K opted in, F's fill takes its charcoal from K's outputs; K's logs stay; FromOutputs says how much;
//   O3 outputs last: with charcoal in the chests (a reserve lets through exactly one craft's worth), the chests pay first
//      and K only the rest;
//   O4 not enough: an exact fill beyond the chests and K moves nothing anywhere;
//   O5 another base: a kiln opted in in a second base (an altar placed far away; else a kiln in no base) is never used;
//   O6 leftovers: K's leftover (another of F's inputs, neither K's input nor output) and a kiln with no recipe never lend;
//   O7 an output that is also an input: a station whose recipe has an item on both sides never lends it (when the game's
//      data has such a recipe at a placeable station);
//   O8 a dry run moves nothing; UseStationOutputs = false is the game's fill;
//   O9 the opt-in is kept: K stays, opted in, for a second run after a restart, which checks it is back.
using System.Diagnostics;
using Kiln;

namespace StationOutputsTest;

public sealed class StationOutputsTestMod : Mod
{
    private const string Logs = "Material_T1_Wood_Logs", Charcoal = "Material_T1_Charcoal";
    private const string KilnTemplate = "T1_Charcoal_Kiln_Factory", MagicChest = "Prop_Decoration_T1_Storage_24_Magic",
        Altar = "Home_Base_FlameAltar";
    private static readonly string[] Factories =
    [
        "T1_Forge_Factory", "T2_Smelter_Factory", "T5_BlastFurnace_Factory", "T1_Charcoal_Kiln_Factory", "T1_Drying_Rack_Factory",
        "T1_Farmbed_Factory", "T1_HandGrinder_Factory", "T1_Seedling_Factory", "T2_ColoringStation_Factory", "T2_EctoplasmPress_Factory",
        "T2_Fireplace_Factory", "T2_HandSpindle_Factory", "T2_Kiln_Factory", "T2_Table_Saw_Factory", "T3_Alchemy_Station_Factory",
        "T3_Oven_Factory", "T3_Tanning_Station_Factory", "T3_butter_churn_factory", "T4_Grindstones_Factory", "T4_Honey_Factory",
        "T4_Laboratory_Factory", "T4_Loom_Factory", "T4_SpinningWheel_Factory", "T5_cardingMachine_Factory",
        "T6_Improved_Grinder_Factory", "T6_Medieval_Grill_Factory", "T6_coffeemaker_brewer_Factory", "T6_coffemaker_roaster_Factory",
        "T7_Force_Generator_Factory", "T7_Infusion_Station_Factory", "Base_Craftingstation_Factory",
    ];
    private static readonly BuildOptions InBases = new() { Flags = BuildFlags.InBases };
    private static readonly BuildOptions Altars = new() { Flags = BuildFlags.InBases | BuildFlags.AllowAltars };
    private int _pass, _fail;
    private readonly List<EntityId> _placed = new();

    private void Info(string s) => Log.Info($"station-outputs-test: INFO {s}");

    private bool Check(bool ok, string what)
    {
        if (ok) { _pass++; Log.Info($"station-outputs-test: PASS {what}"); }
        else { _fail++; Log.Error($"station-outputs-test: FAIL {what}"); }
        return ok;
    }

    public override void OnLoad()
    {
        if (!Client.IsDedicatedServer) return;
        // touch kiln.stations' managed side in OnLoad, as the guide asks of a mod that opts stations in: the kept opt-ins
        // come back from the world's first steps
        try { Events.On<StationPlacedEvent>(_ => { }); }
        catch (Exception e) { Info($"StationPlacedEvent: {e.Message}"); }
        _ = Run();
    }

    private async Task Steps(int n)
    {
        for (var i = 0; i < n; i++) await Context.Scheduler.NextTick();
    }

    private async Task Run()
    {
        try
        {
            var sw = Stopwatch.StartNew();
            while (!(Spawn.WorldRunning && Build.Available && Stations.Available) && sw.Elapsed.TotalSeconds < 240) await Context.Scheduler.NextTick();
            await Steps(150);
            var marker = Path.Combine(Context.DataDirectory, "persist.json");
            if (File.Exists(marker)) await AfterRestart(marker);
            else await Suite(marker);
        }
        catch (Exception e)
        {
            Check(false, $"exception: {e}");
        }
        foreach (var e in _placed)
            try { if (World.Exists(e)) Build.RemoveProp(e, Altars); }
            catch (Exception x) { Info($"remove {e}: {x.Message}"); }
        await Steps(3);
        Log.Info($"station-outputs-test done: {_pass} PASS, {_fail} FAIL");
    }

    private static ItemId Id(string item) => new(Content.Items.Get(item)?.Id ?? throw new InvalidOperationException($"no item {item}"));
    private static int Count(EntityId e, ItemId item) => Items.Read(e)?.Count(item) ?? -1;
    private static WorldPosition Near(WorldPosition at, int index) => new(at.X + 2.5 * index, at.Y, at.Z + 3);

    private string Show(EntityId e) =>
        Stations.Read(e) is { } s
            ? $"{s} [{string.Join(", ", s.Slots.Where(x => !x.IsEmpty).Select(x => $"{Content.Items.Get(x.Item)?.Name ?? x.Item.ToString()} x{x.Count}"))}]"
            : $"{e}: no station";

    private async Task<EntityId> Place(string template, WorldPosition at, BuildOptions? options = null, bool keep = true)
    {
        EntityId e;
        try { e = Build.PlaceProp(template, at, null, 0, options ?? InBases); }
        catch (Exception x)
        {
            Info($"place {template}: {x.Message}");
            return EntityId.None;
        }
        if (!e.IsNone && keep) _placed.Add(e);
        await Steps(10);
        return e;
    }

    private async Task Remove(EntityId e)
    {
        try { if (World.Exists(e)) Build.RemoveProp(e, Altars); }
        catch (Exception x) { Info($"remove {e}: {x.Message}"); }
        _placed.Remove(e);
        await Steps(3);
    }

    // Stopped, with this recipe selected (also when no one here has unlocked it).
    private async Task<bool> Prepare(EntityId station, uint recipe)
    {
        await Stations.Stop(station);
        var r = await Stations.Select(station, recipe, keepStopped: true, allowLocked: true);
        await Steps(2);
        return r.Ok && Stations.Read(station) is { StoppedByPlayer: true } s && s.SelectedRecipe == recipe;
    }

    private async Task<StationResult> Fill(EntityId station, FillOptions o)
    {
        var r = await Stations.Fill(station, o);
        // a stopped station that gets its inputs starts a cycle for one step and its stop aborts it: read it a few steps later
        await Steps(8);
        return r;
    }

    private async Task AfterRestart(string marker)
    {
        var saved = System.Text.Json.JsonDocument.Parse(File.ReadAllText(marker)).RootElement;
        var at = new WorldPosition(saved.GetProperty("x").GetDouble(), saved.GetProperty("y").GetDouble(), saved.GetProperty("z").GetDouble());
        Info($"after the restart: looking for the opted-in kiln left at {at}");
        EntityId found = EntityId.None;
        foreach (var e in Stations.All())
            if (Stations.Read(e) is { } s && Math.Abs(s.Position.X - at.X) < 0.5 && Math.Abs(s.Position.Z - at.Z) < 0.5) found = e;
        if (Check(!found.IsNone, $"the kiln left opted in is there after the restart: {found}"))
        {
            var sw = Stopwatch.StartNew();
            while (!Stations.IsOutputSource(found) && sw.Elapsed.TotalSeconds < 90) await Steps(10);
            Check(Stations.IsOutputSource(found), $"its opt-in came back after the restart ({sw.Elapsed.TotalSeconds:F0} s; EntityData waiting " +
                                                  $"{(EntityData.Available ? EntityData.Waiting : -1)})");
            Stations.SetOutputSource(found, on: false);
            Check(!Stations.IsOutputSource(found), "opted out again");
            Build.RemoveProp(found, InBases);
        }
        File.Delete(marker);
    }

    private async Task Suite(string marker)
    {
        foreach (var c in new[] { Stations.ReadCapability, Stations.FillCapability, Stations.OutputSourcesCapability })
            Check(Capabilities.Is(c), $"{c} is available ({Capabilities.Why(c)})");
        if (!Capabilities.Is(Stations.OutputSourcesCapability)) return;

        var bases = Bases.All;
        if (!Check(bases.Count >= 1, $"Bases.All: {bases.Count} base(s) in the lab save")) return;
        var home = bases[0];
        WorldPosition? spot = null;
        foreach (var s in Stations.All())
            if (Stations.Read(s) is { BaseIndex: >= 0 } info) { spot = info.Position; break; }
        if (spot is null && home.Altars.Count > 0 && World.Position(home.Altars[0].Entity) is { } a0) spot = new WorldPosition(a0.X + 4, a0.Y, a0.Z);
        if (!Check(spot is not null, $"a spot in the base: {spot}")) return;
        var at = spot!.Value;
        var charcoal = Id(Charcoal);
        var logs = Id(Logs);

        // ---- the game's stations: a target whose recipe needs charcoal; a recipe with an item on both sides ----
        string? targetTemplate = null;
        uint target = 0;
        (string Template, uint Recipe, ItemId Item)? loop = null;
        foreach (var t in Factories)
        {
            var e = await Place(t, Near(at, 1), keep: false);
            if (e.IsNone) continue;
            IReadOnlyList<uint> ids;
            try { ids = Stations.RecipesOf(e); }
            catch (KeyNotFoundException) { ids = []; }  // a workshop with no recipes of its own
            Info($"{t}: workshop 0x{Stations.Read(e)?.Workshop ?? 0:x8}, {ids.Count} recipe(s)");
            foreach (var rid in ids)
            {
                if (Stations.Recipe(rid) is not { } r) continue;
                var ins = Stations.ResolveInputs(rid);
                if (targetTemplate is null && r.WaterCharges == 0 && ins.Any(i => i.Item == charcoal) && ins.All(i => i.Count > 0) &&
                    ins.Select(i => i.Item).Distinct().Count() == ins.Count && t != KilnTemplate)
                {
                    targetTemplate = t;
                    target = rid;
                }
                // an item on both sides: an input itself, or an option of a category input (both count as inputs)
                var both = r.Outputs.Select(o => o.Item).FirstOrDefault(o => r.Inputs.Any(i =>
                    i.IsCategory ? Stations.Category(i.Category)?.Options.Any(x => x.Item == o) == true : i.Item == o));
                if (loop is null && !both.IsNone && r.WaterCharges == 0) loop = (t, rid, both);
            }
            await Remove(e);
            if (targetTemplate is not null && loop is not null) break;
        }
        Info($"target {targetTemplate} recipe 0x{target:x8}; a recipe with an item on both sides: {loop}");
        if (!Check(targetTemplate is not null, $"a station whose recipe needs charcoal: {targetTemplate}")) return;
        var tin = Stations.ResolveInputs(target);
        var cPer = tin.First(i => i.Item == charcoal).Count;
        Info($"target recipe 0x{target:x8}: {string.Join(", ", tin)} -> {string.Join(", ", Stations.Recipe(target)!.Outputs)}");

        // ---- the stations ----
        var magic = await Place(MagicChest, Near(at, 0));
        var kiln = await Place(KilnTemplate, Near(at, 2));
        var forge = await Place(targetTemplate!, Near(at, 4));
        if (!Check(!magic.IsNone && !kiln.IsNone && !forge.IsNone, $"placed: chest {magic}, kiln {kiln}, target {forge}")) return;
        await Steps(40);
        var baseId = Bases.Of(kiln)?.Id ?? BaseId.None;
        if (!Check(!baseId.IsNone && Bases.Of(forge)?.Id == baseId, $"the kiln and the target stand in base {baseId}")) return;
        var kilnRecipe = Stations.RecipesOf(kiln).Select(Stations.Recipe).FirstOrDefault(r => r is not null && r.Outputs.Any(o => o.Item == charcoal));
        if (!Check(kilnRecipe is not null, $"the kiln's charcoal recipe: {kilnRecipe}")) return;
        Check(await Prepare(kiln, kilnRecipe!.Id) && await Prepare(forge, target), $"both stopped with their recipes: {Show(kiln)}; {Show(forge)}");

        // the target's other inputs in the Magic chest (for 20 crafts); the kiln: logs queued (3 crafts), charcoal finished
        // (enough for 4 target crafts), and a leftover: one of the target's other inputs, if it has one
        var other = tin.Where(i => i.Item != charcoal).ToList();
        foreach (var i in other) Info($"give the Magic chest {i.Count * 20} x {i.Item}: {await Items.Give(magic, i.Item, i.Count * 20)}");
        var logsPer = Stations.ResolveInputs(kilnRecipe.Id).FirstOrDefault(i => i.Item == logs).Count;
        Info($"give the kiln logs: {await Items.Give(kiln, logs, Math.Max(1, logsPer) * 3)}; charcoal: {await Items.Give(kiln, charcoal, cPer * 4)}");
        var leftover = other.FirstOrDefault().Item;
        if (!leftover.IsNone) Info($"give the kiln a leftover ({leftover}): {await Items.Give(kiln, leftover, 50)}");
        await Steps(5);
        var k0 = Stations.Read(kiln)!;
        Info($"kiln: {Show(kiln)}; outputs {string.Join(", ", k0.Outputs.Select(o => $"{o.Item} x{o.Count}"))}; leftovers {k0.Leftovers.Count}");
        var logs0 = Count(kiln, logs);
        var coal0 = Count(kiln, charcoal);
        Check(logs0 > 0 && coal0 == cPer * 4 && k0.Outputs.Any(o => o.Item == charcoal), $"the kiln holds {logs0} logs (queued) and {coal0} charcoal (finished)");
        Check(leftover.IsNone || k0.Leftovers.Any(o => o.Item == leftover), $"the kiln's {leftover} is a leftover");
        // the chests' charcoal is held back by a reserve: what the chests may give is set per case
        var stockCoal = Bases.Stock(baseId, charcoal);
        Info($"the base's Magic chests hold {stockCoal} charcoal");
        FillOptions Opts(int crafts, long chestCoal, bool exact = true, FillMode mode = FillMode.Add) => new()
        {
            Crafts = crafts, Mode = mode, Exact = exact, Recipe = target,
            Keep = new Dictionary<ItemId, long> { [charcoal] = Math.Max(0, Bases.Stock(baseId, charcoal) - chestCoal) },
        };

        // ---- O1: opted out (the default) = the game's fill ----
        Check(!Stations.IsOutputSource(kiln), "a station is not opted in by default");
        var p1 = Stations.Plan(forge, Opts(2, 0));
        Check(p1.Inputs.First(i => i.Item == charcoal).Outputs == 0 && p1.Status == StationActionStatus.NotEnough, $"opted out: the plan lends nothing: {p1}");
        var f1 = await Fill(forge, Opts(2, 0));
        Check(f1.Status == StationActionStatus.NotEnough && f1.FromOutputs == 0 && Count(kiln, charcoal) == coal0 && Count(forge, charcoal) == 0,
            $"opted out: the fill is the game's (not enough, nothing from the kiln): {f1}");

        // ---- O2: bisdat's case ----
        var kept = Stations.SetOutputSource(kiln);
        Check(Stations.IsOutputSource(kiln) && Stations.OutputSources(baseId).Contains(kiln), $"the kiln is opted in (kept with the save: {kept})");
        var p2 = Stations.Plan(forge, Opts(2, 0));
        Check(p2.Inputs.First(i => i.Item == charcoal).Outputs == coal0 && p2.Status == StationActionStatus.Done, $"the plan counts the kiln's charcoal: {p2}");
        var dry = await Fill(forge, Opts(2, 0) with { DryRun = true });
        Check(dry.Ok && dry.DryRun && Count(kiln, charcoal) == coal0 && Count(forge, charcoal) == 0, $"O8 a dry run moves nothing: {dry}");
        var off = await Fill(forge, Opts(2, 0) with { UseStationOutputs = false });
        Check(off.Status == StationActionStatus.NotEnough && Count(kiln, charcoal) == coal0, $"O8 UseStationOutputs = false is the game's fill: {off}");
        var chest0 = Bases.Stock(baseId, charcoal);
        var f2 = await Fill(forge, Opts(2, 0));
        Info($"fill: {f2}; ledger {string.Join(" ", f2.Changes.Select(c => $"{c.From}->{c.To} {c.Item} x{c.Amount}"))}");
        Check(f2.Status == StationActionStatus.Done && Count(forge, charcoal) == 2 * cPer && Stations.Read(forge)!.CraftsLoaded == 2,
            $"the target's fill took its charcoal from the kiln's outputs: {Show(forge)}");
        Check(Count(kiln, charcoal) == coal0 - 2 * cPer && f2.FromOutputs == 2 * cPer, $"the kiln gave {coal0 - Count(kiln, charcoal)} charcoal (FromOutputs {f2.FromOutputs})");
        Check(Count(kiln, logs) == logs0, $"the kiln's queued logs stay: {Count(kiln, logs)} of {logs0}");
        Check(Bases.Stock(baseId, charcoal) == chest0, $"the chests' charcoal is untouched (all reserved): {Bases.Stock(baseId, charcoal)}");
        var kilnInv = (Items.Read(kiln)?.Slots ?? []).Select(x => x.Slot.Inventory).Append(kiln).ToHashSet();
        Check(f2.Changes.Any(c => !c.From.IsNone && kilnInv.Contains(c.From.Inventory)), "the ledger names a take from the kiln");

        // ---- O3: outputs last ----
        if (stockCoal < cPer)
        {
            Info($"give the Magic chest {cPer * 2} charcoal for the order check: {await Items.Give(magic, charcoal, cPer * 2)}");
            await Steps(3);
        }
        var chest1 = Bases.Stock(baseId, charcoal);
        var kilnC1 = Count(kiln, charcoal);
        var f3 = await Fill(forge, Opts(2, cPer));  // 2 more crafts: the chests may give one craft's charcoal
        Check(f3.Status == StationActionStatus.Done && Bases.Stock(baseId, charcoal) == chest1 - cPer && Count(kiln, charcoal) == kilnC1 - cPer && f3.FromOutputs == cPer,
            $"outputs last: the chests gave {chest1 - Bases.Stock(baseId, charcoal)}, the kiln {kilnC1 - Count(kiln, charcoal)}: {f3}");
        var chest2 = Bases.Stock(baseId, charcoal);
        var kilnC2 = Count(kiln, charcoal);
        var f3b = await Fill(forge, Opts(1, 10_000));  // the chests may give all: the kiln gives nothing
        Check(f3b.Status == StationActionStatus.Done && f3b.FromOutputs == 0 && Count(kiln, charcoal) == kilnC2 && Bases.Stock(baseId, charcoal) == chest2 - cPer,
            $"chests that cover it leave the kiln's outputs alone: {f3b}");
        Check(Count(kiln, logs) == logs0, $"the kiln's logs still stay: {Count(kiln, logs)}");

        // ---- O4: not enough: nothing moves ----
        var kilnC4 = Count(kiln, charcoal);
        var forgeC4 = Count(forge, charcoal);
        var chest4 = Bases.Stock(baseId, charcoal);
        var tooMany = (kilnC4 / cPer) + 3;
        var f4 = await Fill(forge, Opts(tooMany, 0));
        Check(f4.Status == StationActionStatus.NotEnough && Count(kiln, charcoal) == kilnC4 && Count(forge, charcoal) == forgeC4 && Bases.Stock(baseId, charcoal) == chest4,
            $"an exact fill beyond the chests and the outputs moves nothing: {f4}");

        // ---- O6: leftovers ----
        if (!leftover.IsNone)
        {
            var left0 = Count(kiln, leftover);
            var keepAll = Opts(1, 0);
            keepAll = keepAll with { Keep = new Dictionary<ItemId, long>(keepAll.Keep!) { [leftover] = Bases.Stock(baseId, leftover) } };
            var p6 = Stations.Plan(forge, keepAll);
            var f6 = await Fill(forge, keepAll);
            Check(p6.Inputs.First(i => i.Item == leftover).Outputs == 0 && f6.Status != StationActionStatus.Done && Count(kiln, leftover) == left0,
                $"the kiln's leftover {leftover} is never lent: plan {p6.Inputs.First(i => i.Item == leftover)}; fill {f6.Status}; {Count(kiln, leftover)} of {left0}");
        }
        var bare = await Place(KilnTemplate, Near(at, 6));
        if (!bare.IsNone)
        {
            await Stations.Stop(bare);
            await Items.Give(bare, charcoal, cPer * 2);
            Stations.SetOutputSource(bare);
            await Steps(3);
            var p6b = Stations.Plan(forge, Opts(1, 0));
            Check(p6b.Inputs.First(i => i.Item == charcoal).Outputs == Count(kiln, charcoal) && Count(bare, charcoal) == cPer * 2,
                $"a kiln with no recipe lends nothing (its charcoal is a leftover): lent {p6b.Inputs.First(i => i.Item == charcoal).Outputs}, the kiln has {Count(kiln, charcoal)}");
            Stations.SetOutputSource(bare, on: false);
        }

        // ---- O7: an output that is also an input ----
        if (loop is { } lp)
        {
            var q = await Place(lp.Template, Near(at, 8));
            var q2 = await Place(lp.Template, Near(at, 10));
            if (!q.IsNone && !q2.IsNone && await Prepare(q, lp.Recipe) && await Prepare(q2, lp.Recipe))
            {
                await Items.Give(q, lp.Item, 20);
                Stations.SetOutputSource(q);
                await Steps(3);
                var keepIt = new Dictionary<ItemId, long> { [lp.Item] = Bases.Stock(baseId, lp.Item) };
                var p7 = Stations.Plan(q2, new FillOptions { Crafts = 1, Mode = FillMode.Add, Keep = keepIt });
                var lent = p7.Inputs.FirstOrDefault(i => i.Item == lp.Item);
                var f7 = await Fill(q2, new FillOptions { Crafts = 1, Mode = FillMode.Add, Exact = true, Keep = keepIt });
                Check(lent.Outputs == 0 && Count(q, lp.Item) == 20, $"{lp.Template}'s {lp.Item}, an output and an input of 0x{lp.Recipe:x8}, is never lent: plan {lent}; fill {f7.Status}; {Count(q, lp.Item)} left");
                Stations.SetOutputSource(q, on: false);
            }
            else Info($"O7 not run: {lp.Template} could not be prepared");
        }
        else Info("O7 not run: no recipe of a placeable station has an item on both sides (the host test covers the rule)");

        // ---- O5: another base ----
        var far = new WorldPosition(at.X + 400, at.Y, at.Z + 400);
        var altar = await Place(Altar, far, Altars);
        await Steps(60);
        var other2 = await Place(KilnTemplate, new WorldPosition(far.X + 4, far.Y, far.Z), Altars);
        await Steps(20);
        var farBase = other2.IsNone ? null : Bases.Of(other2);
        Info($"second altar {altar}; the far kiln {other2} stands in {(farBase is null ? "no base" : farBase.Id.ToString())}; bases now {Bases.All.Count}");
        if (!other2.IsNone && farBase?.Id != baseId)
        {
            await Prepare(other2, kilnRecipe.Id);
            await Items.Give(other2, charcoal, cPer * 10);
            Stations.SetOutputSource(other2);
            await Steps(3);
            var kilnC5 = Count(kiln, charcoal);
            var p5 = Stations.Plan(forge, Opts(1, 0));
            var f5 = await Fill(forge, Opts(kilnC5 / cPer + 2, 0));
            Check(p5.Inputs.First(i => i.Item == charcoal).Outputs == kilnC5 && f5.Status == StationActionStatus.NotEnough && Count(other2, charcoal) == cPer * 10,
                $"a kiln in {(farBase is null ? "no base" : "another base")} is never used: lent {p5.Inputs.First(i => i.Item == charcoal).Outputs} (the home kiln has {kilnC5}); fill {f5.Status}; the far kiln still has {Count(other2, charcoal)}");
            Stations.SetOutputSource(other2, on: false);
        }
        else Info("O5 not run: no far kiln outside the home base");

        // ---- O9: the opt-in is kept for a second run ----
        _placed.Remove(kiln);
        var k = Stations.Read(kiln)!;
        Check(Stations.IsOutputSource(kiln), "the kiln stays opted in for the restart");
        Directory.CreateDirectory(Context.DataDirectory);
        File.WriteAllText(marker, System.Text.Json.JsonSerializer.Serialize(new { x = k.Position.X, y = k.Position.Y, z = k.Position.Z }));
    }
}
