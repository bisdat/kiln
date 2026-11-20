// SPDX-License-Identifier: MIT
// Lab test: ordinary chests as a named fill source, before opted-in station outputs, and collect into Magic chests.
// Player-free, on the standard lab save. Places a Magic chest, an ordinary chest, a charcoal kiln and a station whose
// recipe needs charcoal. At the end it stages the same props for mods/tests/station-native-test (the native calls) when
// that mod is loaded, and checks what its calls did to the chests.
using System.Diagnostics;
using System.Runtime.InteropServices;
using Kiln;

namespace StationChestsTest;

public sealed class StationChestsTestMod : Mod
{
    private const string Logs = "Material_T1_Wood_Logs", Charcoal = "Material_T1_Charcoal";
    private const string KilnTemplate = "T1_Charcoal_Kiln_Factory";
    private const string MagicChest = "Prop_Decoration_T1_Storage_24_Magic";
    private const string PlainChest = "Prop_Decoration_T1_Storage_24";
    private static readonly string[] Factories = ["T1_Forge_Factory", "T2_Smelter_Factory", "T5_BlastFurnace_Factory"];
    private static readonly BuildOptions InBases = new() { Flags = BuildFlags.InBases };
    private static readonly BuildOptions Altars = new() { Flags = BuildFlags.InBases | BuildFlags.AllowAltars };
    private int _pass, _fail;
    private readonly List<EntityId> _placed = new();

    private void Info(string s) => Log.Info($"station-chests-test: INFO {s}");

    private bool Check(bool ok, string what)
    {
        if (ok) { _pass++; Log.Info($"station-chests-test: PASS {what}"); }
        else { _fail++; Log.Error($"station-chests-test: FAIL {what}"); }
        return ok;
    }

    public override void OnLoad()
    {
        if (!Client.IsDedicatedServer) return;
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
            await Suite();
        }
        catch (Exception e)
        {
            Check(false, $"exception: {e}");
        }
        foreach (var e in _placed.ToArray())
            try { if (World.Exists(e)) Build.RemoveProp(e, Altars); }
            catch (Exception x) { Info($"remove {e}: {x.Message}"); }
        await Steps(3);
        Log.Info($"station-chests-test done: {_pass} PASS, {_fail} FAIL");
    }

    private static ItemId Id(string item) => new(Content.Items.Get(item)?.Id ?? throw new InvalidOperationException($"no item {item}"));
    private static int Count(EntityId e, ItemId item) => Items.Read(e)?.Count(item) ?? -1;
    private static WorldPosition Near(WorldPosition at, int index) => new(at.X + 2.5 * index, at.Y, at.Z + 3);

    private async Task<EntityId> Place(string template, WorldPosition at)
    {
        EntityId e;
        try { e = Build.PlaceProp(template, at, null, 0, InBases); }
        catch (Exception x)
        {
            Info($"place {template}: {x.Message}");
            return EntityId.None;
        }
        if (!e.IsNone) _placed.Add(e);
        await Steps(10);
        return e;
    }

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
        await Steps(8);
        return r;
    }

    private async Task Suite()
    {
        Check(Capabilities.Is(Stations.FillCapability), $"stations.fill is available ({Capabilities.Why(Stations.FillCapability)})");
        if (!Capabilities.Is(Stations.FillCapability)) return;
        var bases = Bases.All;
        if (!Check(bases.Count >= 1, $"Bases.All: {bases.Count} base(s)")) return;
        var home = bases[0];
        WorldPosition? spot = null;
        foreach (var s in Stations.All())
            if (Stations.Read(s) is { BaseIndex: >= 0 } info) { spot = info.Position; break; }
        if (spot is null && home.Altars.Count > 0 && World.Position(home.Altars[0].Entity) is { } a0) spot = new WorldPosition(a0.X + 4, a0.Y, a0.Z);
        if (!Check(spot is not null, $"a spot in the base: {spot}")) return;
        var at = spot!.Value;
        var charcoal = Id(Charcoal);
        var logs = Id(Logs);

        string? targetTemplate = null;
        uint target = 0;
        foreach (var t in Factories)
        {
            var e = await Place(t, Near(at, 1));
            if (e.IsNone) continue;
            foreach (var rid in Stations.RecipesOf(e))
            {
                if (Stations.Recipe(rid) is not { } recipe) continue;
                var ins = Stations.ResolveInputs(rid);
                if (recipe.WaterCharges == 0 && ins.Any(i => i.Item == charcoal) && ins.All(i => i.Count > 0) &&
                    ins.Select(i => i.Item).Distinct().Count() == ins.Count)
                {
                    targetTemplate = t;
                    target = rid;
                    break;
                }
            }
            await Remove(e);
            if (targetTemplate is not null) break;
        }
        if (!Check(targetTemplate is not null, $"a station whose recipe needs charcoal: {targetTemplate}")) return;
        var tin = Stations.ResolveInputs(target);
        var cPer = tin.First(i => i.Item == charcoal).Count;
        Info($"target {targetTemplate} recipe 0x{target:x8}: {string.Join(", ", tin)}");

        var magic = await Place(MagicChest, Near(at, 0));
        var plain = await Place(PlainChest, Near(at, 2));
        var kiln = await Place(KilnTemplate, Near(at, 4));
        var forge = await Place(targetTemplate!, Near(at, 6));
        if (!Check(!magic.IsNone && !plain.IsNone && !kiln.IsNone && !forge.IsNone, $"placed magic {magic}, ordinary {plain}, kiln {kiln}, target {forge}")) return;
        await Steps(40);
        var baseId = Bases.Of(forge)?.Id ?? BaseId.None;
        if (!Check(!baseId.IsNone && Bases.Of(plain)?.Id == baseId && Bases.Of(magic)?.Id == baseId, $"all four stand in base {baseId}")) return;
        Check(Bases.Storage(baseId).Contains(magic) && !Bases.Storage(baseId).Contains(plain), "Bases.Storage lists the Magic chest and not the ordinary chest");

        var kilnRecipe = Stations.RecipesOf(kiln).Select(Stations.Recipe).FirstOrDefault(r => r is not null && r.Outputs.Any(o => o.Item == charcoal));
        if (!Check(kilnRecipe is not null && await Prepare(kiln, kilnRecipe!.Id) && await Prepare(forge, target), "both stations stopped with their recipes")) return;

        await EmptyMagic(baseId, charcoal);  // the save's Magic chests may hold charcoal: the order checks need none there
        foreach (var i in tin.Where(i => i.Item != charcoal))
            Info($"magic chest {i.Count * 20} x {i.Item}: {await Items.Give(magic, i.Item, i.Count * 20)}");
        Info($"ordinary chest {cPer * 6} charcoal: {await Items.Give(plain, charcoal, cPer * 6)}");
        var logsPer = Math.Max(1, Stations.ResolveInputs(kilnRecipe!.Id).FirstOrDefault(i => i.Item == logs).Count);
        Info($"kiln logs: {await Items.Give(kiln, logs, logsPer * 3)}; charcoal outputs: {await Items.Give(kiln, charcoal, cPer * 4)}");
        await Steps(5);
        var plain0 = Count(plain, charcoal);
        var kilnCoal0 = Count(kiln, charcoal);
        var kilnLogs0 = Count(kiln, logs);
        Check(plain0 == cPer * 6 && kilnCoal0 == cPer * 4 && kilnLogs0 > 0, $"ordinary chest {plain0} charcoal; kiln {kilnCoal0} charcoal and {kilnLogs0} logs");

        // unnamed: the ordinary chest is not a source
        var unnamed = new FillOptions { Crafts = 2, Mode = FillMode.Add, Exact = true, Recipe = target };
        var p0 = Stations.Plan(forge, unnamed);
        var coalIn = p0.Inputs.First(i => i.Item == charcoal);
        Check(coalIn.Chests == 0 && coalIn.Stock == 0, $"unnamed: the plan does not count the ordinary chest ({coalIn})");
        var f0 = await Fill(forge, unnamed);
        Check(f0.Status == StationActionStatus.NotEnough && f0.FromChests == 0 && Count(plain, charcoal) == plain0 && Count(forge, charcoal) == 0,
            $"unnamed: nothing moves ({f0.Status}, ordinary chest still {Count(plain, charcoal)})");

        // a Magic chest named as an ordinary chest is refused
        var namedMagic = await Fill(forge, unnamed with { Chests = [magic] });
        Check(namedMagic.Status == StationActionStatus.Refused && Count(plain, charcoal) == plain0, $"naming the Magic chest is refused: {namedMagic}");

        // The kiln lends its finished charcoal from here on: the ordinary chest must still pay before it.
        Check(Capabilities.Is(Stations.OutputSourcesCapability) && Stations.SetOutputSource(kiln) && Stations.IsOutputSource(kiln),
            $"the kiln is opted in as an output source ({Capabilities.Why(Stations.OutputSourcesCapability)})");

        // Magic chests pay first, the ordinary chest the rest. One craft's charcoal goes into the Magic chest.
        Info($"magic chest {cPer} charcoal: {await Items.Give(magic, charcoal, cPer)}");
        await Steps(3);
        var magicBefore = Bases.Stock(baseId, charcoal);
        var plainBefore = Count(plain, charcoal);
        var named = unnamed with { Chests = [plain] };
        var p1 = Stations.Plan(forge, named);
        var lent = p1.Inputs.First(i => i.Item == charcoal);
        Check(lent.Chests == plainBefore && lent.Stock == magicBefore && lent.Outputs == kilnCoal0 && p1.Status == StationActionStatus.Done,
            $"the plan counts Magic stock {lent.Stock}, ordinary chests {lent.Chests}, kiln outputs {lent.Outputs}: {p1.Status}");
        var f1 = await Fill(forge, named);
        var magicAfter = Bases.Stock(baseId, charcoal);
        var plainAfter = Count(plain, charcoal);
        Check(f1.Status == StationActionStatus.Done && magicAfter == magicBefore - cPer && plainAfter == plainBefore - cPer && f1.FromChests == cPer &&
              f1.FromOutputs == 0,
            $"ordinary chest after Magic: Magic gave {magicBefore - magicAfter}, ordinary gave {plainBefore - plainAfter}, FromChests {f1.FromChests}, FromOutputs {f1.FromOutputs}: {f1.Status}");
        Check(Count(kiln, charcoal) == kilnCoal0 && Count(kiln, logs) == kilnLogs0, "chests before station outputs: the opted-in kiln's charcoal and queued logs were not used");

        // the ordinary chest holds one craft's charcoal, the Magic chests none: two crafts take the chest's, then the kiln's
        Info($"ordinary chest down to {cPer}: {await Items.Take(plain, charcoal, Count(plain, charcoal) - cPer)}");
        await EmptyMagic(baseId, charcoal);
        var magicThen = Bases.Stock(baseId, charcoal);
        var f3 = await Fill(forge, named);
        Check(magicThen == 0 && f3.Status == StationActionStatus.Done && f3.FromChests == cPer && f3.FromOutputs == cPer && Count(plain, charcoal) == 0 &&
              Count(kiln, charcoal) == kilnCoal0 - cPer && Count(kiln, logs) == kilnLogs0,
            $"station outputs pay last: FromChests {f3.FromChests}, FromOutputs {f3.FromOutputs} (want {cPer} each); ordinary {Count(plain, charcoal)}, kiln {Count(kiln, charcoal)} of {kilnCoal0}: {f3.Status}");

        // exact, and short of everything: nothing moves
        Info($"ordinary chest {cPer * 2} charcoal: {await Items.Give(plain, charcoal, cPer * 2)}");
        await Steps(3);
        var left = Count(plain, charcoal);
        var magicNow = Bases.Stock(baseId, charcoal);
        var forgeNow = Count(forge, charcoal);
        var kilnNow = Count(kiln, charcoal);
        var tooMany = (left + (int)magicNow + kilnNow) / cPer + 3;
        var f2 = await Fill(forge, named with { Crafts = tooMany });
        Check(f2.Status == StationActionStatus.NotEnough && Count(plain, charcoal) == left && Bases.Stock(baseId, charcoal) == magicNow &&
              Count(forge, charcoal) == forgeNow && Count(kiln, charcoal) == kilnNow,
            $"a short exact fill moves nothing: {f2.Status}; ordinary {Count(plain, charcoal)} of {left}, kiln {Count(kiln, charcoal)} of {kilnNow}");
        Stations.SetOutputSource(kiln, false);

        // collect the kiln's finished charcoal into the base's Magic chests
        var beforeCollect = Bases.Stock(baseId, charcoal);
        var plainCollect = Count(plain, charcoal);
        var kilnCollect = Count(kiln, charcoal);
        var collected = await Stations.CollectToBase(kiln, charcoal);
        await Steps(8);
        Check(collected.Ok && collected.Changed && !collected.DryRun && Bases.Stock(baseId, charcoal) == beforeCollect + kilnCollect && Count(kiln, charcoal) == 0 &&
              Count(plain, charcoal) == plainCollect && Count(kiln, logs) == kilnLogs0,
            $"collect into Magic chests: {collected.Status}; Magic {beforeCollect} -> {Bases.Stock(baseId, charcoal)}; kiln charcoal {Count(kiln, charcoal)}; logs stay {Count(kiln, logs)}");

        await NativeStage(forge, kiln, plain, baseId, charcoal, cPer);
    }

    // ---- the native half: mods/tests/station-native-test, when it is loaded beside this mod ----
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate int StageFn(uint station, uint kiln, uint item, uint perCraft, uint chest);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate int StateFn(out int pass, out int fail);

    // The base's Magic chests (the save's too) give up every `item`.
    private async Task EmptyMagic(BaseId baseId, ItemId item)
    {
        foreach (var c in Bases.Storage(baseId))
        {
            var n = Count(c, item);
            if (n > 0) Info($"Magic chest {c} gives up {n} x {item}: {await Items.Take(c, item, n)}");
        }
        await Steps(3);
    }

    private async Task NativeStage(EntityId forge, EntityId kiln, EntityId plain, BaseId baseId, ItemId charcoal, int cPer)
    {
        // a native mod's drop-in folder is <game>/kiln/native/<folder>/ (this mod is in <game>/kiln/mods/<folder>/)
        var dll = Path.GetFullPath(Path.Combine(Context.Info.Directory, "..", "..", "native", "station-native-test", "station-native-test.dll"));
        if (!File.Exists(dll))
        {
            Info($"station-native-test is not installed ({dll}): the native calls are not checked here");
            return;
        }
        var lib = NativeLibrary.Load(dll);  // the module Kiln loaded: the same globals
        try
        {
            var stage = Marshal.GetDelegateForFunctionPointer<StageFn>(NativeLibrary.GetExport(lib, "station_native_test_stage"));
            var state = Marshal.GetDelegateForFunctionPointer<StateFn>(NativeLibrary.GetExport(lib, "station_native_test_state"));
            // The Magic chests hold no charcoal, the ordinary chest two crafts' worth, the kiln (not opted in) three.
            await EmptyMagic(baseId, charcoal);
            var plainCoal = Count(plain, charcoal);
            if (plainCoal > 0) Info($"ordinary chest emptied: {await Items.Take(plain, charcoal, plainCoal)}");
            Info($"ordinary chest {cPer * 2} charcoal: {await Items.Give(plain, charcoal, cPer * 2)}");
            Info($"kiln {cPer * 3} finished charcoal: {await Items.Give(kiln, charcoal, cPer * 3)}");
            Stations.SetOutputSource(kiln, false);
            await Steps(5);
            var magic0 = Bases.Stock(baseId, charcoal);
            if (!Check(magic0 == 0 && Count(plain, charcoal) == cPer * 2 && Count(kiln, charcoal) == cPer * 3,
                    $"native stage: Magic {magic0}, ordinary {Count(plain, charcoal)}, kiln {Count(kiln, charcoal)} charcoal")) return;
            if (!Check(stage(forge.Value, kiln.Value, charcoal.Value, (uint)cPer, plain.Value) == 1,
                    "station-native-test took the stage (0: Kiln did not load it, see kiln/state/native.json)")) return;
            var sw = Stopwatch.StartNew();
            int phase;
            while ((phase = state(out _, out _)) != 100 && sw.Elapsed.TotalSeconds < 60) await Steps(5);
            state(out var pass, out var fail);
            Check(phase == 100 && fail == 0 && pass >= 9, $"station-native-test finished: {pass} PASS, {fail} FAIL (phase {phase})");
            await Steps(5);
            Check(Count(plain, charcoal) == cPer && Count(kiln, charcoal) == 0 && Bases.Stock(baseId, charcoal) == cPer * 3,
                $"after the native calls: ordinary {Count(plain, charcoal)} (want {cPer}), kiln {Count(kiln, charcoal)} (want 0), Magic {Bases.Stock(baseId, charcoal)} (want {cPer * 3})");
        }
        finally
        {
            NativeLibrary.Free(lib);
        }
    }

    private async Task Remove(EntityId e)
    {
        try { if (World.Exists(e)) Build.RemoveProp(e, Altars); }
        catch (Exception x) { Info($"remove {e}: {x.Message}"); }
        _placed.Remove(e);
        await Steps(3);
    }
}
