// SPDX-License-Identifier: MIT
// Lab test of opted-in station outputs as the last source of a player's craft (GitHub #68 step 2), on a dedicated server.
// The payer is a spawned player character (1_Player): it has a bag and a place in the base, as a player has. A lab has no
// logged-in player, so the game's CanAccessInventories test (BaseStockForPlayer, player ids 1..1023 only) refuses it; the
// lab starts the server with KILN_STATIONS_CRAFT_LAB=1, which hands a spawned character the base's stock list the game
// hands a player with the right (docs/re/stations.md §10; never a real player). Every case reads Kiln's own counters
// (kiln-items.dll's kiln_stations_craft_stats), so "nothing moved" is Kiln's refusal, not a payment that never ran:
//  1. opted out: Kiln stands aside (idle), nothing moves;
//  2. opted in: the bag, then the base's chests, then the kiln's finished charcoal pay one craft; its queued logs stay;
//  3. not enough: Kiln counts, refuses, nothing moves;
//  4. the bag covers it: Kiln counts and leaves the payment to the game, the kiln is untouched;
//  5. craft x3: two crafts the bag pays, the third the kiln's outputs finish;
//  6. a kiln opted in in another base is never used;
//  7. a payer that is no player character (a Magic chest) is refused by the game's own right, even in the lab.
// The game's charcoal workbench recipe is locked (RecipeApply returns 4 before InventoryPay), so this mod registers an
// always-unlocked workbench recipe whose only shortfall is charcoal.
using System.Diagnostics;
using System.Runtime.InteropServices;
using Kiln;

namespace StationCraftTest;

public sealed class StationCraftTestMod : Mod
{
    private const string Logs = "Material_T1_Wood_Logs", Charcoal = "Material_T1_Charcoal", Scraps = "Material_T1_Metal_Scraps",
        Twigs = "Material_T1_Twigs";
    private const string KilnTemplate = "T1_Charcoal_Kiln_Factory", MagicChest = "Prop_Decoration_T1_Storage_24_Magic",
        Workbench = "Craftingstation_Workbench", Altar = "Home_Base_FlameAltar", PlayerTemplate = "1_Player";
    private static readonly Guid RecipeGuid = new("7c3e1a20-4b6d-4f1a-9c2e-8a5d0b7e1f44");
    private const int Per = 200;  // charcoal per craft: more than the lab save's chests hold
    private static readonly BuildOptions InBases = new() { Flags = BuildFlags.InBases };
    private static readonly BuildOptions Altars = new() { Flags = BuildFlags.InBases | BuildFlags.AllowAltars };
    private int _pass, _fail;
    private readonly List<EntityId> _placed = new(), _spawned = new();
    private readonly List<string> _crafted = new();
    private uint _recipe;

    // kiln_stations_craft_stats' order
    private enum St { Calls, Idle, OtherWorld, Mode0, NoAccess, Considered, Covered, Short, Paid, FromOutputs, Rollback, Skipped, Lab, Count }
    private static readonly string[] StatNames =
        ["calls", "idle", "other_world", "mode0", "no_access", "considered", "covered", "short", "paid", "from_outputs", "rollback", "skipped", "lab"];

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate uint StatsFn([In, Out] uint[] values, uint n);
    [DllImport("kernel32", CharSet = CharSet.Unicode)]
    private static extern nint GetModuleHandleW(string name);
    [DllImport("kernel32", CharSet = CharSet.Ansi, BestFitMapping = false)]
    private static extern nint GetProcAddress(nint module, string name);
    private StatsFn? _stats;

    private void Info(string s) => Log.Info($"station-craft-test: INFO {s}");

    private bool Check(bool ok, string what)
    {
        if (ok) { _pass++; Log.Info($"station-craft-test: PASS {what}"); }
        else { _fail++; Log.Error($"station-craft-test: FAIL {what}"); }
        return ok;
    }

    public override void OnLoad()
    {
        if (!Client.IsDedicatedServer) return;
        try
        {
            var def = Content.Recipes.Register(new RecipeDefinition
            {
                Guid = RecipeGuid,
                Name = "StationCraft_Charcoal_Recipe",
                Station = "0x4da88f73",
                Unlock = RecipeUnlock.Always,
                Inputs = [(Charcoal, Per), (Scraps, 2)],
                Outputs = [(Twigs, 1)],
            });
            _recipe = def.Id;
            Info($"registered 0x{_recipe:x8} ({def.State})");
        }
        catch (Exception e) { Info($"recipe register: {e.Message}"); }
        try { Events.On<ItemCraftedEvent>(e => _crafted.Add($"{e.Result} ({e.RawResult})")); }
        catch (Exception e) { Info($"ItemCraftedEvent: {e.Message}"); }
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
            while (!(Spawn.WorldRunning && Build.Available && Stations.Available && Gameplay.Available) && sw.Elapsed.TotalSeconds < 240)
                await Context.Scheduler.NextTick();
            await Steps(150);
            await Suite();
        }
        catch (Exception e)
        {
            Check(false, $"exception: {e}");
        }
        foreach (var e in _placed.ToArray())
            try
            {
                Stations.SetOutputSource(e, on: false);
                if (World.Exists(e)) Build.RemoveProp(e, Altars);
            }
            catch (Exception x) { Info($"remove {e}: {x.Message}"); }
        foreach (var e in _spawned) Spawn.Destroy(e);
        await Steps(3);
        Log.Info($"station-craft-test done: {_pass} PASS, {_fail} FAIL");
    }

    private uint[] Stats()
    {
        var v = new uint[(int)St.Count];
        if (_stats is null)
        {
            var m = GetModuleHandleW("kiln-items.dll");
            var f = m == 0 ? 0 : GetProcAddress(m, "kiln_stations_craft_stats");
            if (f != 0) _stats = Marshal.GetDelegateForFunctionPointer<StatsFn>(f);
        }
        _stats?.Invoke(v, (uint)v.Length);
        return v;
    }

    private static long D(uint[] before, uint[] after, St s) => (long)after[(int)s] - before[(int)s];

    private static string Delta(uint[] before, uint[] after) =>
        string.Join(", ", Enumerable.Range(0, (int)St.Count).Where(i => after[i] != before[i]).Select(i => $"{StatNames[i]} +{after[i] - before[i]}"));

    private static ItemId Id(string item) => new(Content.Items.Get(item)?.Id ?? throw new InvalidOperationException($"no item {item}"));
    private static int Count(EntityId e, ItemId item) => Items.Read(e)?.Count(item) ?? -1;
    private static WorldPosition Near(WorldPosition at, int index) => new(at.X + 2.5 * index, at.Y, at.Z + 3);

    private async Task<EntityId> Place(string template, WorldPosition at, BuildOptions? options = null)
    {
        EntityId e;
        try { e = Build.PlaceProp(template, at, null, 0, options ?? InBases); }
        catch (Exception x)
        {
            Info($"place {template}: {x.Message}");
            return EntityId.None;
        }
        if (!e.IsNone) _placed.Add(e);
        await Steps(10);
        return e;
    }

    // One injected craft request (handle_crafting_events runs CraftRecipe `amount` times); returns Kiln's counters' change.
    private async Task<(uint[] Before, uint[] After, string Results)> Craft(EntityId who, EntityId station, int amount, string what)
    {
        _crafted.Clear();
        var before = Stats();
        Gameplay.Inject.Craft(who, station, _recipe, amount);
        await Steps(10);
        var after = Stats();
        var results = _crafted.Count == 0 ? "(no ItemCraftedEvent)" : string.Join(", ", _crafted);
        Info($"{what}: craft x{amount} by {who} at {station}: {results}; Kiln: {Delta(before, after)}");
        return (before, after, results);
    }

    private async Task Suite()
    {
        foreach (var c in new[] { Stations.ReadCapability, Stations.OutputSourcesCapability, Stations.CraftSourcesCapability, "gameplay.emit", "content.new" })
            Check(Capabilities.Is(c), $"{c} is available ({Capabilities.Why(c)})");
        if (!Capabilities.Is(Stations.CraftSourcesCapability) || !Capabilities.Is("gameplay.emit") || _recipe == 0) return;
        Stats();
        if (!Check(_stats is not null, "kiln-items.dll's craft payment counters are readable")) return;
        foreach (var s in Stations.OutputSources()) Stations.SetOutputSource(s, on: false);  // a save's earlier opt-ins
        var seen = false;
        for (var i = 0; i < 100 && !seen; i++)
        {
            seen = Stations.Recipe(_recipe) is not null;
            if (!seen) await Steps(10);
        }
        if (!Check(seen, $"the unlocked recipe is in the world: 0x{_recipe:x8}")) return;

        var bases = Bases.All;
        if (!Check(bases.Count >= 1, $"Bases.All: {bases.Count} base(s)")) return;
        WorldPosition? spot = null;
        foreach (var s in Stations.All())
            if (Stations.Read(s) is { BaseIndex: >= 0 } info) { spot = info.Position; break; }
        if (spot is null && bases[0].Altars.Count > 0 && World.Position(bases[0].Altars[0].Entity) is { } a0)
            spot = new WorldPosition(a0.X + 4, a0.Y, a0.Z);
        if (!Check(spot is not null, $"a spot in the base: {spot}")) return;
        var at = spot!.Value;
        var charcoal = Id(Charcoal);
        var logs = Id(Logs);
        var scraps = Id(Scraps);
        var twigs = Id(Twigs);

        var recipeWs = Stations.Recipe(_recipe)?.Workshop ?? 0;
        EntityId bench = EntityId.None;
        foreach (var e in World.Find("Crafting"))
        {
            if (!World.TryRead(e, "Crafting", out CraftHead head) || head.Workshop != recipeWs) continue;
            if (World.Position(e) is { } p && Bases.Of(e)?.Id == bases[0].Id) { bench = e; at = p; break; }
        }
        var magic = await Place(MagicChest, Near(at, 0));
        var kiln = await Place(KilnTemplate, Near(at, 2));
        if (bench.IsNone) bench = await Place(Workbench, Near(at, 4));
        if (!Check(!magic.IsNone && !kiln.IsNone && !bench.IsNone, $"placed: chest {magic}, kiln {kiln}, workbench {bench}")) return;
        var who = await Spawn.Entity(PlayerTemplate, new WorldPosition(at.X + 1.5, at.Y + 0.5, at.Z + 2), 0,
            new SpawnOptions { Tag = "station-craft", Flags = SpawnFlags.Transient });
        if (!who.IsNone) _spawned.Add(who);
        await Steps(40);
        var home = Bases.Of(kiln)?.Id ?? BaseId.None;
        if (!Check(!home.IsNone && Bases.Of(bench)?.Id == home, $"the kiln and the workbench stand in base {home}")) return;
        if (!Check(!who.IsNone && World.Has(who, "PlayerInput") && Items.Read(who) is { Initialized: true } && Bases.Of(who)?.Id == home,
                $"the payer is a player character in that base: {who} (base {Bases.Of(who)?.Id})")) return;

        var kilnRecipe = Stations.RecipesOf(kiln).Select(Stations.Recipe).FirstOrDefault(r => r is not null && r.Outputs.Any(o => o.Item == charcoal));
        if (!Check(kilnRecipe is not null, $"the kiln's charcoal recipe: {kilnRecipe?.Name}")) return;
        await Stations.Stop(kiln);
        var selected = await Stations.Select(kiln, kilnRecipe!.Id, keepStopped: true, allowLocked: true);
        Check(selected.Ok, $"the kiln is stopped on its charcoal recipe: {selected}");
        var logsPer = Math.Max(1, Stations.ResolveInputs(kilnRecipe.Id).FirstOrDefault(i => i.Item == logs).Count);
        Info($"kiln logs: {await Items.Give(kiln, logs, logsPer * 3)}; player scraps: {await Items.Give(who, scraps, 20)}");

        // the bag, the chests and the kiln, so that one craft is short of the bag and the chests and the kiln covers it
        var stock = (int)Bases.Stock(home, charcoal);
        if (!Check(stock < Per - 1, $"the base's chests hold {stock} charcoal, less than one craft ({Per})")) return;
        var bag = Math.Min(20, Per - 1 - stock);
        var lend = Per - bag - stock + 30;
        Info($"player charcoal: {await Items.Give(who, charcoal, bag)}; kiln charcoal: {await Items.Give(kiln, charcoal, lend)}");
        await Steps(5);
        var logs0 = Count(kiln, logs);
        var coal0 = Count(kiln, charcoal);
        var twigs0 = Count(who, twigs);
        Check(logs0 == logsPer * 3 && coal0 == lend && Count(who, charcoal) == bag,
            $"the kiln holds {logs0} queued logs and {coal0} finished charcoal; the bag {Count(who, charcoal)}, the chests {stock}");

        // 1. opted out: Kiln stands aside, the game's payment refuses, nothing moves
        var r = await Craft(who, bench, 1, "opted out");
        Check(D(r.Before, r.After, St.Idle) >= 1 && D(r.Before, r.After, St.Considered) == 0 && D(r.Before, r.After, St.Paid) == 0,
            $"opted out: Kiln left the payment to the game untouched ({Delta(r.Before, r.After)})");
        Check(Count(kiln, charcoal) == coal0 && Count(kiln, logs) == logs0 && Count(who, charcoal) == bag && Bases.Stock(home, charcoal) == stock &&
              Count(who, twigs) == twigs0, $"opted out: nothing moved (kiln {Count(kiln, charcoal)}/{Count(kiln, logs)}, bag {Count(who, charcoal)}, chests {Bases.Stock(home, charcoal)})");

        // 2. opted in: bag, chests, then the kiln's finished charcoal; its logs stay
        Stations.SetOutputSource(kiln);
        Check(Stations.IsOutputSource(kiln), "the kiln is opted in");
        await Steps(3);
        var shortfall = Per - bag - stock;
        r = await Craft(who, bench, 1, "opted in");
        Check(D(r.Before, r.After, St.Paid) == 1 && D(r.Before, r.After, St.FromOutputs) == shortfall && D(r.Before, r.After, St.Rollback) == 0,
            $"opted in: Kiln paid the craft, {D(r.Before, r.After, St.FromOutputs)} from station outputs (the shortfall {shortfall}) ({Delta(r.Before, r.After)})");
        Check(Count(kiln, charcoal) == coal0 - shortfall && Count(kiln, logs) == logs0,
            $"opted in: the kiln gave {coal0 - Count(kiln, charcoal)} charcoal and kept its {Count(kiln, logs)} logs");
        Check(Count(who, charcoal) == 0 && Bases.Stock(home, charcoal) == 0 && Count(who, twigs) == twigs0 + 1,
            $"opted in: the bag ({Count(who, charcoal)} left) and the chests ({Bases.Stock(home, charcoal)} left) paid first; the craft made its twigs ({Count(who, twigs) - twigs0})");

        // 3. not enough: Kiln counts and refuses, the game refuses, nothing moves
        var coal1 = Count(kiln, charcoal);
        r = await Craft(who, bench, 1, "not enough");
        Check(D(r.Before, r.After, St.Considered) >= 1 && D(r.Before, r.After, St.Short) >= 1 && D(r.Before, r.After, St.Paid) == 0,
            $"not enough: Kiln counted and refused ({Delta(r.Before, r.After)})");
        Check(Count(kiln, charcoal) == coal1 && Count(kiln, logs) == logs0 && Count(who, twigs) == twigs0 + 1,
            $"not enough: nothing moved (kiln {Count(kiln, charcoal)}, logs {Count(kiln, logs)})");

        // 4. the bag covers it: Kiln counts and leaves the payment to the game
        await Items.Give(who, charcoal, Per);
        await Steps(3);
        r = await Craft(who, bench, 1, "covered");
        Check(D(r.Before, r.After, St.Covered) == 1 && D(r.Before, r.After, St.Paid) == 0,
            $"covered: Kiln left the payment to the game ({Delta(r.Before, r.After)})");
        Check(Count(kiln, charcoal) == coal1 && Count(who, charcoal) == 0 && Count(who, twigs) == twigs0 + 2,
            $"covered: the game took the bag's charcoal ({Count(who, charcoal)} left), the kiln is untouched ({Count(kiln, charcoal)})");

        // 5. craft x3: the bag pays two, the kiln's outputs finish the third
        var more = Per * 3 - 2 * Per - 50 - coal1;  // the kiln then holds exactly the third craft's rest
        await Items.Give(who, charcoal, 2 * Per + 50);
        if (more > 0) await Items.Give(kiln, charcoal, more);
        await Steps(3);
        var coal2 = Count(kiln, charcoal);
        r = await Craft(who, bench, 3, "craft x3");
        Check(D(r.Before, r.After, St.Covered) == 2 && D(r.Before, r.After, St.Paid) == 1 && D(r.Before, r.After, St.FromOutputs) == Per - 50,
            $"craft x3: two crafts the bag covered, the third paid {D(r.Before, r.After, St.FromOutputs)} from outputs ({Delta(r.Before, r.After)})");
        Check(Count(who, charcoal) == 0 && Count(kiln, charcoal) == coal2 - (Per - 50) && Count(kiln, logs) == logs0 && Count(who, twigs) == twigs0 + 5,
            $"craft x3: bag {Count(who, charcoal)}, kiln {Count(kiln, charcoal)} of {coal2}, logs {Count(kiln, logs)}, twigs {Count(who, twigs) - twigs0}");

        // 6. a kiln opted in in another base is never used
        Stations.SetOutputSource(kiln, on: false);
        var farAt = new WorldPosition(at.X + 400, at.Y, at.Z + 400);
        var farAltar = await Place(Altar, farAt, Altars);
        await Steps(60);
        var farKiln = await Place(KilnTemplate, new WorldPosition(farAt.X + 4, farAt.Y, farAt.Z), Altars);
        await Steps(20);
        var farBase = farKiln.IsNone ? null : Bases.Of(farKiln);
        Info($"second altar {farAltar}; far kiln {farKiln} in {(farBase is null ? "no base" : farBase.Id.ToString())}");
        if (Check(!farKiln.IsNone && farBase is not null && farBase.Id != home, "a kiln stands in a second base"))
        {
            await Stations.Stop(farKiln);
            await Stations.Select(farKiln, kilnRecipe.Id, keepStopped: true, allowLocked: true);
            await Items.Give(farKiln, logs, logsPer * 3);
            await Items.Give(farKiln, charcoal, Per + 50);
            await Items.Give(who, charcoal, 20);
            Stations.SetOutputSource(farKiln);
            await Steps(3);
            var far0 = Count(farKiln, charcoal);
            var farLogs = Count(farKiln, logs);
            r = await Craft(who, bench, 1, "another base");
            Check(D(r.Before, r.After, St.Considered) >= 1 && D(r.Before, r.After, St.Short) >= 1 && D(r.Before, r.After, St.Paid) == 0,
                $"another base: Kiln counted, found no lender in the payer's base and refused ({Delta(r.Before, r.After)})");
            Check(Count(farKiln, charcoal) == far0 && Count(farKiln, logs) == farLogs && Count(who, charcoal) == 20,
                $"another base is never used: far kiln {Count(farKiln, charcoal)} of {far0}, its logs {Count(farKiln, logs)}, bag {Count(who, charcoal)}");
            Stations.SetOutputSource(farKiln, on: false);
        }

        // 7. a payer that is no player character: the game's right refuses it, even in the lab
        Stations.SetOutputSource(kiln);
        await Items.Give(kiln, charcoal, Per);
        await Items.Give(magic, scraps, 4);
        await Steps(3);
        var coal3 = Count(kiln, charcoal);
        r = await Craft(magic, bench, 1, "a chest pays");
        Check(D(r.Before, r.After, St.NoAccess) >= 1 && D(r.Before, r.After, St.Paid) == 0 && D(r.Before, r.After, St.Lab) == 0,
            $"a Magic chest as the payer: the game's inventory right refused it, Kiln lent nothing ({Delta(r.Before, r.After)})");
        Check(Count(kiln, charcoal) == coal3, $"a Magic chest as the payer: the kiln is untouched ({Count(kiln, charcoal)} of {coal3})");
        var all = Stats();
        Info($"Kiln's craft payment counters: {string.Join(", ", Enumerable.Range(0, (int)St.Count).Select(i => $"{StatNames[i]} {all[i]}"))}");
    }

    [StructLayout(LayoutKind.Explicit, Size = 0x14)]
    private struct CraftHead
    {
        [FieldOffset(0x10)] public uint Workshop;
    }
}
