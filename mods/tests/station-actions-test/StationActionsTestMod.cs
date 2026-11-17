// SPDX-License-Identifier: MIT
// kiln.stations lab test, player-free (a dedicated server with the standard lab save: one base). The test places a charcoal
// kiln, a Magic storage chest (the game's crafting stock), a plain chest and later a well and a seedling bed in the base,
// then checks, against the inventories and the stations' own state:
//   L1 the bases (Bases.All, Of, Storage: the Magic chest is listed, the plain chest is not);
//   L2 the game's fill from chests (Plan, a dry run, up to, add, unchanged, an exact fill short of stock, reserves, the
//      ledger names the Magic chest, the plain chest untouched) and Select's unlock check;
//   L4/L5 resume, stop now (inputs back in the station), stop after the current craft (nothing lost; one StationCraftedEvent
//      and one ItemCraftedEvent for the cycle), the state events naming this mod;
//   Collect into the base;
//   L3 water: a well filled with water is a water source and a seedling fill pours from it;
//   L8 what reads cost;
//   a subscription made in OnLoad, before the world runs, is accepted;
//   Select refuses a recipe no one has unlocked (a blast furnace's) unless allowLocked.
// Everything placed is removed at the end, except one stopped, filled kiln: start the server again (after a SIGINT
// stop, which saves) and the second run checks that the stop, the selection and the inputs came back.
// The lab save's base has Magic chests of its own: the counts are the base's stock, not one chest's.
using System.Diagnostics;
using Kiln;

namespace StationActionsTest;

public sealed class StationActionsTestMod : Mod
{
    private const string Logs = "Material_T1_Wood_Logs", Charcoal = "Material_T1_Charcoal";
    private const string KilnTemplate = "T1_Charcoal_Kiln_Factory", MagicChest = "Prop_Decoration_T1_Storage_24_Magic",
        PlainChest = "Prop_Decoration_T1_Storage_24", Well = "Prop_NPC_Water_Well_a", SeedBed = "T1_Seedling_Factory";
    private static readonly BuildOptions InBases = new() { Flags = BuildFlags.InBases };
    private int _pass, _fail;
    private readonly List<EntityId> _placed = new();
    private readonly List<StationStateChangedEvent> _states = new();
    private readonly List<StationCraftedEvent> _stationCrafts = new();
    private readonly List<ItemCraftedEvent> _crafts = new();
    private readonly List<StationChangedEvent> _changes = new();
    private EntityId _watched;
    private string _early = "not tried";
    private readonly List<StationPlacedEvent> _placedSeen = new();
    private const string BlastFurnace = "T5_BlastFurnace_Factory";

    private void Info(string s) => Log.Info($"station-actions-test: INFO {s}");

    private bool Check(bool ok, string what)
    {
        if (ok) { _pass++; Log.Info($"station-actions-test: PASS {what}"); }
        else { _fail++; Log.Error($"station-actions-test: FAIL {what}"); }
        return ok;
    }

    public override void OnLoad()
    {
        if (!Client.IsDedicatedServer) return;
        // before any world runs: the station events take the subscription (kiln.stations reports its events on without a world)
        try
        {
            Events.On<StationPlacedEvent>(_placedSeen.Add);
            _early = "accepted";
        }
        catch (Exception e) { _early = $"{e.GetType().Name}: {e.Message}"; }
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
            try { if (World.Exists(e)) Build.RemoveProp(e, InBases); }
            catch (Exception x) { Info($"remove {e}: {x.Message}"); }
        await Steps(3);
        Log.Info($"station-actions-test done: {_pass} PASS, {_fail} FAIL");
    }

    private static ItemId Id(string item) => new(Content.Items.Get(item)?.Id ?? throw new InvalidOperationException($"no item {item}"));
    private static int Count(EntityId e, ItemId item) => Items.Read(e)?.Count(item) ?? -1;
    private static WorldPosition Near(WorldPosition at, int index) => new(at.X + 2.0 * index, at.Y, at.Z + 3);

    private string Show(EntityId e) =>
        Stations.Read(e) is { } s
            ? $"{s} [{string.Join(", ", s.Slots.Where(x => !x.IsEmpty).Select(x => $"{Content.Items.Get(x.Item)?.Name ?? x.Item.ToString()} x{x.Count}"))}]"
            : $"{e}: no station";

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

    private async Task<bool> Until(Func<bool> cond, double seconds)
    {
        var sw = Stopwatch.StartNew();
        while (!cond())
        {
            if (sw.Elapsed.TotalSeconds > seconds) return false;
            await Steps(1);
        }
        return true;
    }

    private static long Stock(BaseId b, ItemId item) => Bases.Stock(b, item);

    private static HashSet<EntityId> InventoriesOf(IEnumerable<EntityId> containers) =>
        containers.SelectMany(c => (Items.Read(c)?.Slots ?? []).Select(x => x.Slot.Inventory).Append(c)).ToHashSet();

    private async Task AfterRestart(string marker)
    {
        var saved = System.Text.Json.JsonDocument.Parse(File.ReadAllText(marker)).RootElement;
        var at = new WorldPosition(saved.GetProperty("x").GetDouble(), saved.GetProperty("y").GetDouble(), saved.GetProperty("z").GetDouble());
        var recipe = saved.GetProperty("recipe").GetUInt32();
        var loaded = saved.GetProperty("loaded").GetInt32();
        Info($"after the restart: looking for the kiln left at {at}, recipe 0x{recipe:x8}, {loaded} crafts loaded");
        StationInfo? found = null;
        foreach (var e in Stations.All())
            if (Stations.Read(e) is { } s && Math.Abs(s.Position.X - at.X) < 0.5 && Math.Abs(s.Position.Z - at.Z) < 0.5) found = s;
        if (Check(found is not null, $"the kiln left stopped is there after the restart: {found}"))
        {
            Check(found!.StoppedByPlayer && found.State == StationState.Stopped, $"its stop came back: {found.State}, stopped {found.StoppedByPlayer}");
            Check(found.SelectedRecipe == recipe && found.CraftsLoaded == loaded, $"its selection and inputs came back: 0x{found.SelectedRecipe:x8}, {found.CraftsLoaded} crafts");
            Check(Bases.Of(found.Entity) is not null, "it stands in a base after the restart");
            Build.RemoveProp(found.Entity, InBases);
        }
        File.Delete(marker);
    }

    private async Task Suite(string marker)
    {
        Check(_early == "accepted", $"a StationPlacedEvent subscription in OnLoad, before the world: {_early}");
        foreach (var c in new[] { Stations.ReadCapability, Stations.FillCapability, Stations.ControlCapability, Stations.EventsCapability })
            Check(Capabilities.Is(c), $"{c} is available ({Capabilities.Why(c)})");
        if (!Capabilities.Is(Stations.ReadCapability)) return;
        Events.On<StationStateChangedEvent>(e => { if (e.Station == _watched) { _states.Add(e); Info($"event: {e}"); } });
        Events.On<StationCraftedEvent>(e => { if (e.Station == _watched) { _stationCrafts.Add(e); Info($"event: {e}"); } });
        Events.On<ItemCraftedEvent>(e => { if (e.Station == _watched) _crafts.Add(e); });
        Events.On<StationChangedEvent>(e => { if (e.Station == _watched) _changes.Add(e); });
        var placedSeen = _placedSeen;
        await Steps(3);

        // ---- L1: the bases ----
        var bases = Bases.All;
        foreach (var b in bases) Info($"{b}");
        if (!Check(bases.Count >= 1, $"Bases.All: {bases.Count} base(s) in the lab save")) return;
        var home = bases[0];
        WorldPosition? spot = null;
        foreach (var s in Stations.All())
            if (Stations.Read(s) is { BaseIndex: >= 0 } info) { spot = info.Position; break; }
        if (spot is null && home.Altars.Count > 0 && World.Position(home.Altars[0].Entity) is { } altar) spot = new WorldPosition(altar.X + 4, altar.Y, altar.Z);
        if (!Check(spot is not null, $"a spot in the base: {spot}")) return;
        var at = spot!.Value;
        var magic = await Place(MagicChest, Near(at, 0));
        var plain = await Place(PlainChest, Near(at, 1));
        var kiln = await Place(KilnTemplate, Near(at, 2));
        _watched = kiln;
        Info($"placed: magic chest {magic}, plain chest {plain}, kiln {kiln}");
        if (!Check(!magic.IsNone && !plain.IsNone && !kiln.IsNone, "the kiln and both chests are placed")) return;
        await Steps(40);
        var baseOf = Bases.Of(kiln);
        Check(baseOf is not null && Bases.Of(magic)?.Id == baseOf.Id, $"Bases.Of: the kiln and the Magic chest stand in base {baseOf?.Id}");
        if (baseOf is null) return;
        var storage = Bases.Storage(baseOf.Id);
        Check(storage.Contains(magic), $"Bases.Storage lists the Magic chest ({string.Join(", ", storage)})");
        Check(!storage.Contains(plain), "Bases.Storage does not list the plain chest (the game's own rule)");
        Check(Stations.IsStation(kiln) && !Stations.IsStation(magic), "IsStation: the kiln is one, the chest is not");
        Check(Stations.InBase(baseOf.Id).Contains(kiln), "Stations.InBase lists the kiln");
        Check(Bases.At(Near(at, 2))?.Id == baseOf.Id, "Bases.At: the kiln's spot is in the base");
        Check(baseOf.Altars.Count >= 1 && baseOf.Id == home.Id && baseOf.Altars.Any(a => a.Id == baseOf.Id),
            $"the base's id is one of its altars' own ids ({string.Join(", ", baseOf.Altars)})");
        // lab 2026-11-16: false for the flame altar: per-base data is kept by the altars' own ids (BaseAltar.Id), not EntityData
        Info($"EntityData.IsPersistent of the altars: {string.Join(", ", baseOf.Altars.Select(a => EntityData.IsPersistent(a.Entity)))}");
        Check(EntityData.IsPersistent(kiln) && EntityData.IsPersistent(magic), "EntityData.IsPersistent: a placed kiln and Magic chest");
        var deposit = Bases.Storage(baseOf.Id, deposit: true);
        Check(deposit.Contains(magic) && deposit.All(storage.Contains), $"Bases.Storage(deposit): the Magic chest takes deposits ({deposit.Count} of {storage.Count})");
        Check(Stations.Scanned, "Stations.Scanned: the first scan is done");
        Check(Stations.Nearest(magic, null, 8) == kiln || Stations.Nearest(magic, null, 8) is not null, $"Stations.Nearest from the chest: {Stations.Nearest(magic, null, 8)}");
        Check(Stations.Read(kiln)?.Template == KilnTemplate, $"StationInfo.Template: {Stations.Read(kiln)?.Template}");

        // ---- L2: select, fill ----
        var charcoal = Id(Charcoal);
        var logs = Id(Logs);
        var recipe = Stations.RecipesOf(kiln).Select(Stations.Recipe).FirstOrDefault(r => r is not null && r.Outputs.Any(o => o.Item == charcoal));
        if (!Check(recipe is not null, $"the kiln's recipes include charcoal: {recipe}")) return;
        var inputs = Stations.ResolveInputs(recipe!.Id);
        Info($"recipe 0x{recipe.Id:x8}: {string.Join(", ", inputs)} -> {string.Join(", ", recipe.Outputs)}, {recipe.Duration.TotalSeconds} s, unlocked {recipe.AlwaysUnlocked}");
        var stop0 = await Stations.Stop(kiln);
        Check(stop0.Ok && Stations.Read(kiln)!.StoppedByPlayer, $"Stop on a new kiln sets the stop: {stop0}");
        var sel = await Stations.Select(kiln, recipe.Id, keepStopped: true);
        Info($"Select without allowLocked: {sel}");
        if (sel.Status == StationActionStatus.Locked)
        {
            Check(sel.Reason is { Length: > 0 }, $"a recipe no one has unlocked is refused, with why: {sel.Reason}");
            sel = await Stations.Select(kiln, recipe.Id, keepStopped: true, allowLocked: true);
        }
        await Steps(2);
        var k0 = Stations.Read(kiln)!;
        Check(sel.Ok && k0.SelectedRecipe == recipe.Id && k0.StoppedByPlayer, $"Select keepStopped: {sel}; {Show(kiln)}");
        var unchanged = await Stations.Select(kiln, recipe.Id, keepStopped: true, allowLocked: true);
        Check(unchanged.Status == StationActionStatus.Unchanged, $"the same selection again: {unchanged.Status}");

        var per = inputs.FirstOrDefault(i => i.Item == logs).Count;
        if (!Check(per > 0, $"the charcoal recipe takes logs: {per} a craft")) return;
        // logs for 12 crafts, the other inputs for 40: the logs are what runs short
        foreach (var i in inputs) Info($"give the Magic chest {i.Count * (i.Item == logs ? 12 : 40)} x {i.Item}: {await Items.Give(magic, i.Item, i.Count * (i.Item == logs ? 12 : 40))}");
        Info($"give the plain chest: {await Items.Give(plain, logs, 200)}");
        await Steps(5);
        var home0 = baseOf.Id;
        var stock0 = Stock(home0, logs);
        var plain0 = Count(plain, logs);
        Info($"the base's stock: {stock0} logs in {storage.Count} container(s)");

        var plan = Stations.Plan(kiln, new FillOptions { Crafts = 5 });
        Check(plan.Ok && plan.CraftsLoaded == 0 && plan.CraftsPlanned == 5 && plan.CraftsAffordable >= 5, $"Plan up to 5: {plan}");
        var dry = await Stations.Fill(kiln, new FillOptions { Crafts = 5, DryRun = true });
        await Steps(2);
        Check(dry.Ok && dry.DryRun && Stock(home0, logs) == stock0 && Stations.Read(kiln)!.CraftsLoaded == 0, $"a dry run moves nothing: {dry}");
        var f1 = await Stations.Fill(kiln, new FillOptions { Crafts = 5 });
        // a stopped station that gets its inputs starts a cycle for one step and the stop aborts it (the game's own
        // order, the same after its fill button): read it a few steps later
        await Steps(8);
        Info($"fill: {f1}; ledger {string.Join(" ", f1.Changes.Select(c => $"{c.From}->{c.To} {c.Item} x{c.Amount}"))}");
        Check(f1.Status == StationActionStatus.Done && f1.CraftsAfter == 5 && Stations.Read(kiln)!.CraftsLoaded == 5, $"Fill up to 5 from 0: {Show(kiln)}");
        Check(Stock(home0, logs) == stock0 - 5 * per, $"the base's Magic chests gave {5 * per} logs: {stock0} -> {Stock(home0, logs)}");
        var stockInv = InventoriesOf(Bases.Storage(home0));
        Check(f1.Changes.Count > 0 && f1.Changes.Where(c => !c.From.IsNone).All(c => stockInv.Contains(c.From.Inventory)),
            $"the ledger takes from the base's Magic chests only ({string.Join(", ", f1.Changes.Where(c => !c.From.IsNone).Select(c => c.From.Inventory).Distinct())})");
        var f2 = await Stations.Fill(kiln, new FillOptions { Crafts = 2, Mode = FillMode.Add });
        await Steps(8);
        Check(f2.Status == StationActionStatus.Done && Stations.Read(kiln)!.CraftsLoaded == 7, $"Add 2: {f2}; {Show(kiln)}");
        var f3 = await Stations.Fill(kiln, new FillOptions { Crafts = 7 });
        Check(f3.Status == StationActionStatus.Unchanged, $"up to 7 when 7 are there: {f3.Status}");
        var left = Stock(home0, logs);
        var exact = await Stations.Fill(kiln, new FillOptions { Crafts = 7 + (int)(left / per) + 3, Exact = true });
        await Steps(8);
        Check(exact.Status == StationActionStatus.NotEnough && Stock(home0, logs) == left && Stations.Read(kiln)!.CraftsLoaded == 7,
            $"an exact fill short of stock moves nothing: {exact}");
        var reserve = left - 2 * per;  // room for exactly two more crafts
        var keep = await Stations.Fill(kiln, new FillOptions { Crafts = 100, Keep = new Dictionary<ItemId, long> { [logs] = reserve } });
        await Steps(8);
        Check(keep.Status == StationActionStatus.Partial && keep.CraftsAfter == 9 && Stock(home0, logs) == reserve,
            $"a reserve stays in the base: {keep}; {Stock(home0, logs)} logs left, {reserve} kept");
        Check(Count(plain, logs) == plain0, $"the plain chest is untouched: {Count(plain, logs)} logs");
        var loaded = Stations.Read(kiln)!.CraftsLoaded;
        Info($"loaded now: {Show(kiln)}");

        // ---- a waiting stop is withdrawn by a later resume of Kiln's ----
        await Stations.Resume(kiln);
        await Until(() => Stations.Read(kiln) is { State: StationState.Running }, 15);
        var waiting = Stations.Stop(kiln, StopWhen.AfterCurrentCraft);
        await Steps(4);
        Check(!waiting.IsCompleted && Stations.Read(kiln)!.StopPending, $"StopPending while the stop waits: {Show(kiln)}");
        var second = await Stations.Stop(kiln, StopWhen.AfterCurrentCraft);
        Check(second.Status == StationActionStatus.Unchanged, $"a second stop after craft finds one waiting: {second}");
        var stopNow0 = await Stations.Stop(kiln);  // stop now withdraws the waiting one
        await Steps(3);
        var withdrawn = waiting.IsCompleted ? waiting.Result : null;
        Check(withdrawn is { Status: StationActionStatus.Canceled } && stopNow0.Status == StationActionStatus.Done && !Stations.Read(kiln)!.StopPending,
            $"stop now withdrew the waiting stop: {withdrawn}; {stopNow0}");
        await Steps(4);
        Check(Stations.Read(kiln)!.CraftsLoaded == loaded, $"the stop now gave the cycle's inputs back: {Show(kiln)}");

        // ---- L4/L5: resume, stop now, stop after the current craft ----
        _states.Clear();
        var resume = await Stations.Resume(kiln);
        var running = await Until(() => Stations.Read(kiln) is { State: StationState.Running }, 15);
        Check(resume.Status == StationActionStatus.Done && running, $"Resume: it runs: {Show(kiln)}");
        await Steps(4);
        Check(_states.Any(e => e.Resumed && e.ByMod == Context.Info.Id), $"the resume's state event names this mod ({string.Join("; ", _states)})");
        await Steps(60);
        var mid = Stations.Read(kiln)!;
        Info($"mid-cycle: {Show(kiln)}");
        _states.Clear();
        var stopNow = await Stations.Stop(kiln);
        await Steps(3);
        var afterStop = Stations.Read(kiln)!;
        Check(stopNow.Status == StationActionStatus.Done && afterStop.State == StationState.Stopped && afterStop.StoppedByPlayer,
            $"Stop now: {stopNow}; {Show(kiln)}");
        Check(afterStop.CraftsLoaded == loaded && Count(kiln, charcoal) == 0,
            $"stop now puts the running cycle's inputs back ({mid.CraftsLoaded} while running -> {afterStop.CraftsLoaded}, {loaded} before)");
        Check(_states.Any(e => e.Stopped && e.ByMod == Context.Info.Id), $"the stop's state event names this mod ({string.Join("; ", _states)})");

        _crafts.Clear();
        _stationCrafts.Clear();
        await Stations.Resume(kiln);
        await Until(() => Stations.Read(kiln) is { State: StationState.Running }, 15);
        var cycle = Stations.Read(kiln)!.Duration;
        Info($"a cycle takes {cycle.TotalSeconds:F1} s here");
        var after = Stations.Stop(kiln, StopWhen.AfterCurrentCraft);
        await Steps(5);
        Check(!after.IsCompleted && Stations.Read(kiln)!.State == StationState.Running, "stop after the current craft waits for the cycle's end");
        var sw = Stopwatch.StartNew();
        while (!after.IsCompleted && sw.Elapsed < cycle + TimeSpan.FromSeconds(40)) await Steps(5);
        var stopped = after.IsCompleted ? after.Result : null;
        await Steps(4);
        var k2 = Stations.Read(kiln)!;
        var made = recipe.Outputs.First(o => o.Item == charcoal).Count;
        Check(stopped is { Status: StationActionStatus.Done } && k2.StoppedByPlayer && k2.State == StationState.Stopped,
            $"stopped after the cycle: {stopped}; {Show(kiln)}");
        Check(Count(kiln, charcoal) == made && k2.CraftsLoaded == loaded - 1, $"nothing lost: {Count(kiln, charcoal)} charcoal, {k2.CraftsLoaded} crafts left");
        Check(_stationCrafts.Count == 1 && _stationCrafts[0].Outputs.Any(o => o.Item == charcoal && o.Count == made) && _stationCrafts[0].Base == home0,
            $"one StationCraftedEvent for the cycle, with its base ({_stationCrafts.Count})");
        Check(_crafts.Count(e => e.Succeeded) == 1, $"one ItemCraftedEvent for the cycle, at its end ({_crafts.Count}: {string.Join("; ", _crafts.Select(c => c.Succeeded))})");

        // ---- collect ----
        var c0 = Stock(home0, charcoal);
        var collect = await Stations.CollectToBase(kiln);
        await Steps(3);
        Check(collect.Status == StationActionStatus.Done && Stock(home0, charcoal) == c0 + made && Count(kiln, charcoal) == 0,
            $"CollectToBase: the charcoal went into the base's Magic chests ({c0} -> {Stock(home0, charcoal)}): {collect}");
        var again = await Stations.Collect(kiln, magic);
        Check(again.Status == StationActionStatus.Unchanged, $"nothing more to collect: {again.Status}");
        Check(_changes.Count >= 6, $"StationChangedEvent for Kiln's own writes ({_changes.Count}: {string.Join(", ", _changes.Select(c => c.Action))})");
        Check(placedSeen.Any(e => e.Station == kiln), $"StationPlacedEvent for the kiln ({placedSeen.Count} placed seen)");

        // ---- L3: water ----
        var well = await Place(Well, Near(at, 4));
        var seed = await Place(SeedBed, Near(at, 6));
        Info($"well {well}, seedling bed {seed}");
        if (!well.IsNone && !seed.IsNone)
        {
            Info($"fill the well: {await Items.Fuel.Fill(well)}");
            await Steps(20);
            var water = Bases.WaterSources(baseOf.Id);
            Check(water.Contains(well), $"Bases.WaterSources lists the filled well ({string.Join(", ", water)})");
            var wrec = Stations.RecipesOf(seed).Select(Stations.Recipe).FirstOrDefault(r => r is { WaterCharges: > 0 });
            if (wrec is not null)
            {
                foreach (var i in Stations.ResolveInputs(wrec.Id)) await Items.Give(magic, i.Item, i.Count * 3);
                await Stations.Stop(seed);
                var ws = await Stations.Select(seed, wrec.Id, keepStopped: true, allowLocked: true);
                await Steps(2);
                var wp = Stations.Plan(seed, new FillOptions { Crafts = 2 });
                Check(wp.WaterPerCraft == wrec.WaterCharges * 64 && wp.WaterStock > 0, $"Plan shows the water: {wp.WaterPerCraft} a craft, {wp.WaterHave} in, {wp.WaterStock} in the base");
                var w0 = Stations.Read(seed)!.Water ?? -1;
                var wf = await Stations.Fill(seed, new FillOptions { Crafts = 2 });
                await Steps(3);
                var w1 = Stations.Read(seed)!.Water ?? -1;
                Info($"water recipe 0x{wrec.Id:x8} ({wrec.WaterCharges} charge(s)): select {ws}; fill {wf}; water {w0} -> {w1}");
                Check(wf.Ok && w1 > w0, $"a water recipe's fill pours water from the well: {w0} -> {w1}");
                await Steps(6);
                var noWater = await Stations.Fill(seed, new FillOptions { Crafts = 2 });
                Check(noWater.Status == StationActionStatus.Unchanged, $"up to 2 when 2 crafts' items and water are in: {noWater}");
                // take the water out: the items for 2 crafts are there, the water is not: a fill pours it (the game's count)
                await Items.Fuel.Set(seed, 0);
                await Steps(3);
                var k2w = Stations.Read(seed)!;
                var refill = await Stations.Fill(seed, new FillOptions { Crafts = 2 });
                await Steps(3);
                Check(k2w.CraftsLoaded == 0 && refill.Status == StationActionStatus.Done && (Stations.Read(seed)!.Water ?? 0) >= 2 * wrec.WaterCharges * 64,
                    $"water counts in loaded crafts: {k2w.CraftsLoaded} loaded without water; refill {refill}; water {Stations.Read(seed)!.Water}");
            }
            else Info("the seedling bed has no water recipe here: L3 not run");
        }

        // ---- Select's unlock check: a late recipe no one here has unlocked ----
        var furnace = await Place(BlastFurnace, Near(at, 8));
        if (!furnace.IsNone)
        {
            await Stations.Stop(furnace);
            StationResult? locked = null;
            uint lockedId = 0;
            foreach (var id in Stations.RecipesOf(furnace))
            {
                var r = await Stations.Select(furnace, id, keepStopped: true);
                if (r.Status == StationActionStatus.Locked) { locked = r; lockedId = id; break; }
            }
            Info($"blast furnace {furnace}: {Stations.RecipesOf(furnace).Count} recipe(s); locked: {locked}");
            if (Check(locked is not null && locked.Reason is { Length: > 0 }, $"Select refuses a recipe no one has unlocked, with why: {locked?.Reason}"))
            {
                var allowed = await Stations.Select(furnace, lockedId, keepStopped: true, allowLocked: true);
                await Steps(2);
                Check(allowed.Status == StationActionStatus.Done && Stations.Read(furnace)!.SelectedRecipe == lockedId, $"with allowLocked it is selected: {allowed}");
            }
        }
        else Info("no blast furnace placed: the unlock refusal is not run");

        // ---- L4: a stopped, filled kiln for the restart ----
        var keepKiln = await Place(KilnTemplate, Near(at, 10));
        _placed.Remove(keepKiln);  // stays for the restart
        if (!keepKiln.IsNone)
        {
            await Stations.Stop(keepKiln);
            await Stations.Select(keepKiln, recipe.Id, keepStopped: true, allowLocked: true);
            foreach (var i in inputs) await Items.Give(magic, i.Item, i.Count * 3);
            await Stations.Fill(keepKiln, new FillOptions { Crafts = 3 });
            await Steps(8);
            var k = Stations.Read(keepKiln)!;
            Info($"left for the restart: {Show(keepKiln)}");
            if (Check(k.StoppedByPlayer && k.CraftsLoaded == 3, "a stopped kiln with 3 crafts is left for the restart"))
            {
                Directory.CreateDirectory(Context.DataDirectory);
                File.WriteAllText(marker, System.Text.Json.JsonSerializer.Serialize(new
                {
                    x = k.Position.X, y = k.Position.Y, z = k.Position.Z, recipe = recipe.Id, loaded = k.CraftsLoaded,
                }));
            }
        }

        // ---- L1 again, L8: reads ----
        Check(Bases.Storage(baseOf.Id).Contains(magic), "after the run, Bases.Storage still lists the Magic chest");
        var t = Stopwatch.StartNew();
        var all = Stations.All();
        var reads = all.Select(Stations.Read).Count(x => x is not null);
        var tRead = t.Elapsed.TotalMilliseconds;
        t.Restart();
        for (var i = 0; i < 100; i++) _ = Bases.All;
        Info($"cost: Stations.All + Read of {reads}/{all.Count} station(s) {tRead:F2} ms; Bases.All {t.Elapsed.TotalMilliseconds / 100:F3} ms a call");
    }
}
