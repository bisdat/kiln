// SPDX-License-Identifier: MIT
// Lab test of kiln.component_changes (named component watches, chapter 07 S7) through Kiln.World's ComponentChanges:
// see kiln.mod.json. Player-free: the roots are spawned 1_Player templates (transient), no connected player. The rows:
//  WATCH-NET-ORIGIN  a change without a token (this mod's own world program writes Health) is Unknown; Combat.SetHealth
//                    is Kiln ("kiln.combat"); a tracked write (World.WriteAt) is Kiln (this mod's id); a token and a write
//                    without one, or two sources, in one interval are Mixed; A -> B -> A gives nothing
//  WATCH-LINKS       an expanded watch covers the root's linked inventories; Items.Give into the root shows up as an
//                    Inventory change of a linked page (kiln.items' token), seen by a handler filtered to the root
//  WATCH-BUDGET      4096 present pairs exactly are taken, one more is refused (Ok false, no watch, the reason); the 33rd
//                    watch is refused; a watch whose root is destroyed is suspended whole, with its last complete step
//  WATCH-COST        the window calls' p50 / p99 with 4096 pairs over 1600 steps (chapter 07 §8.5), from the service
// plain, then again with the lab's forced-parallel switch (kiln/control/systems.parallel) where it is there.
using System.Runtime.InteropServices;
using Kiln;

namespace ComponentWatchTest;

[StructLayout(LayoutKind.Sequential, Size = 40)]
internal struct Raw40
{
    public ulong A, B, C, D, E;
}

public sealed class ComponentWatchTestMod : Mod
{
    private const string Id = "component-watch-test";
    private int _passes, _failures;
    private IDisposable? _program;
    private readonly List<IDisposable> _subs = [];
    private readonly List<ComponentValueChangedEvent> _changes = [];
    private readonly List<AttributeValueChangedEvent> _attributes = [];
    private readonly List<ComponentWatchStateChangedEvent> _states = [];
    private readonly List<ComponentValueChangedEvent> _rootSeen = [];
    private readonly List<EntityId> _spawned = [];

    // the world program's write request: an entity whose Health current value it lowers by one, once (0: none)
    private int _rawTarget, _rawDone, _rawThread, _updates;

    private void Check(bool ok, string message)
    {
        if (ok) _passes++;
        else _failures++;
        Log.Info($"{Id}: {(ok ? "PASS" : "FAIL")} {message}");
    }

    public override void OnLoad()
    {
        try
        {
            _program = EcsPrograms.Bind("writer", InSimulation);
        }
        catch (Exception e)
        {
            Log.Warn($"{Id}: no world program ({e.Message}): the write without a token cannot be made here");
        }
        _ = Run();
    }

    public override void OnUnload()
    {
        foreach (var s in _subs) s.Dispose();
        _program?.Dispose();
    }

    // Inside the simulation (the world thread or a worker): a write no Kiln writer reports, as a game program's.
    private void InSimulation(EcsProgramContext context)
    {
        Interlocked.Increment(ref _updates);
        var target = (uint)Volatile.Read(ref _rawTarget);
        if (target == 0) return;
        var e = new EntityId(target);
        if (context.TryReadOther<Raw40>(e, "Health", out var h))
        {
            unsafe
            {
                var p = (byte*)&h;
                var offset = *(ushort*)(p + 4);
                if ((p[6] & 1) != 0 && offset + 4 <= 40) *(int*)(p + offset) -= 1;
            }
            if (context.TryWriteOther(e, "Health", h)) Volatile.Write(ref _rawDone, 1);
        }
        Volatile.Write(ref _rawThread, Environment.CurrentManagedThreadId);
        Volatile.Write(ref _rawTarget, 0);
    }

    private async Task Wait(int steps)
    {
        for (var i = 0; i < steps; i++) await Context.Scheduler.NextTick();
    }

    private async Task<bool> RawWrite(EntityId e)
    {
        Volatile.Write(ref _rawDone, 0);
        Volatile.Write(ref _rawTarget, (int)e.Value);
        for (var i = 0; i < 20 && Volatile.Read(ref _rawTarget) != 0; i++) await Wait(1);
        return Volatile.Read(ref _rawDone) == 1;
    }

    private static int Hp(EntityId e) => Combat.GetHealth(e)?.Current ?? int.MinValue;

    private string _mode = "full";

    // kiln/data/component-watch-test/mode: full (the default), keep, fresh, release
    private string Mode()
    {
        try
        {
            var f = Path.Combine(Context.DataDirectory, "mode");
            return File.Exists(f) ? File.ReadAllText(f).Trim() : "full";
        }
        catch (IOException)
        {
            return "full";
        }
    }

    // ---- RELEASE: a mod (component-watch-helper) that unloads and reloads with live watches ----
    private const string Helper = "component-watch-helper";

    private string HelperFile(string name) => Path.GetFullPath(Path.Combine(Context.DataDirectory, "..", Helper, name));

    private string[] HelperLines(string name)
    {
        try
        {
            return File.Exists(HelperFile(name)) ? File.ReadAllLines(HelperFile(name)) : [];
        }
        catch (IOException)
        {
            return [];
        }
    }

    // The helper's last ready line: "<nonce> <root> <watch ids...>", once its watches are made.
    private async Task<(string Nonce, EntityId Root)?> HelperReady(string? not)
    {
        for (var i = 0; i < 1200; i++)
        {
            var l = HelperLines("ready.txt").LastOrDefault()?.Split(' ');
            if (l is { Length: >= 2 } && l[0] != not && uint.TryParse(l[1], out var root)) return (l[0], new EntityId(root));
            await Wait(1);
        }
        return null;
    }

    private int EventsOf(string nonce) => HelperLines("events.txt").Count(l => l.StartsWith(nonce + " ", StringComparison.Ordinal));

    // The host's own check that unloaded mod contexts were collected (kiln/control "gc", Kiln.Host).
    private async Task<string> Gc(string tag)
    {
        var dir = Path.GetFullPath(Path.Combine(Context.DataDirectory, "..", "..", "control"));
        Directory.CreateDirectory(dir);
        var name = $"cwt-gc-{tag}-{Environment.TickCount64}";
        var result = Path.Combine(dir, name + ".result.json");
        File.WriteAllText(Path.Combine(dir, name + ".cmd.tmp"), "gc");
        File.Move(Path.Combine(dir, name + ".cmd.tmp"), Path.Combine(dir, name + ".cmd"));
        for (var i = 0; i < 600 && !File.Exists(result); i++) await Wait(1);
        await Wait(2);
        try
        {
            return File.Exists(result) ? File.ReadAllText(result).Replace("\n", " ").Replace("\r", "").Replace("\n", "") : "(no result)";
        }
        catch (IOException e)
        {
            return $"(unreadable: {e.Message})";
        }
    }

    private static bool GcClean(string json) => json.Contains("\"ok\": true") && json.Contains("\"leaked\": []");

    // Writers in the steps around the release: Combat.SetHealth in Pre and a tracked write (kiln.world's program, inside
    // the simulation; on a worker when the players mask is forced) on the root the helper watches.
    private async Task Writers(EntityId root, int steps)
    {
        for (var i = 0; i < steps && Spawn.Exists(root); i++)
        {
            var hp = Hp(root);
            if (hp > 10) Combat.SetHealth(root, hp - 1);
            var h = World.ReadBytes(root, "Health");
            if (h is { Length: >= 8 }) _ = World.WriteAt(root, "Health", BitConverter.ToUInt16(h, 4), Math.Max(2, hp - 2));
            await Wait(1);
        }
    }

    private string? _lastNonce;

    private async Task Release(string run)
    {
        var first = await HelperReady(_lastNonce);
        if (first is not { } a)
        {
            Check(false, $"[{run}] RELEASE: the helper never said it was ready ({HelperFile("ready.txt")})");
            return;
        }
        await Wait(5);
        var live = Counter("watches");
        Check(live >= 3 && Counter("pairs") > 0, $"[{run}] RELEASE: {Helper} (load {a.Nonce}) holds live watches: {Counters()}");
        var hp = Hp(a.Root);
        Combat.SetHealth(a.Root, hp - 3);
        await Wait(4);
        var before = EventsOf(a.Nonce);
        Check(before >= 1, $"[{run}] RELEASE: the helper's handler is told of a change of its root {a.Root.Value} ({before} event line(s))");

        // unload with writers running in the same steps
        var writing = Writers(a.Root, 8);
        var t0 = Environment.TickCount64;
        var unloaded = await Mods.UnloadAsync(Helper);
        var ms = Environment.TickCount64 - t0;
        await writing;
        await Wait(1);
        var windows = Counter("windows");
        var after = Counters();
        Check(unloaded.Ok && Counter("watches") == 0 && Counter("pairs") == 0,
            $"[{run}] RELEASE: the unload ({unloaded.Ok} {unloaded.Message}, {ms} ms) released the watches by owner and dropped the pairs: {after}");
        await Wait(10);
        Check(Counter("windows") == windows, $"[{run}] RELEASE: no window call once nothing is watched (window_wanted 0): windows {windows} -> {Counter("windows")} over 10 steps");
        var gone = EventsOf(a.Nonce);
        Combat.SetHealth(a.Root, Math.Max(5, Hp(a.Root) - 3));
        await Wait(5);
        Check(EventsOf(a.Nonce) == gone, $"[{run}] RELEASE: no event reaches the unloaded load context ({gone} -> {EventsOf(a.Nonce)} event line(s) of load {a.Nonce})");
        var gc = await Gc(run + "-unload");
        Check(GcClean(gc), $"[{run}] RELEASE: the host finds the unloaded context collected: {gc}");

        // the next load watches again
        var loaded = await Mods.LoadAsync(Helper);
        var second = await HelperReady(a.Nonce);
        await Wait(5);
        var ok = second is { } b && Counter("watches") == live;
        if (second is { } bb)
        {
            Combat.SetHealth(bb.Root, Hp(bb.Root) - 3);
            await Wait(4);
        }
        Check(loaded.Ok && ok && second is { } c2 && EventsOf(c2.Nonce) >= 1,
            $"[{run}] RELEASE: the next load ({loaded.Ok} {loaded.Message}) watches again: load {second?.Nonce}, {Counters()}, its events {(second is { } c3 ? EventsOf(c3.Nonce) : -1)}");

        // a reload with live watches: only the new instance's remain
        var writing2 = second is { } w2 ? Writers(w2.Root, 8) : Task.CompletedTask;
        var reloaded = await Mods.ReloadAsync(Helper);
        await writing2;
        var third = await HelperReady(second?.Nonce);
        await Wait(5);
        Check(reloaded.Ok && third is not null && Counter("watches") == live,
            $"[{run}] RELEASE: a reload with live watches ({reloaded.Ok} {reloaded.Message}) leaves only the new load's ({third?.Nonce}): {Counters()}");
        var gc2 = await Gc(run + "-reload");
        Check(GcClean(gc2), $"[{run}] RELEASE: after the reload the old context is collected too: {gc2}");

        _lastNonce = third?.Nonce;
        // leave the helper unloaded for the next run (it loads again there)
        var end = await Mods.UnloadAsync(Helper);
        await Wait(3);
        Check(end.Ok && Counter("watches") == 0, $"[{run}] RELEASE: unloaded at the end: {Counters()}");
        if (run == "plain") _ = await Mods.LoadAsync(Helper);  // the parallel run starts from a fresh load
    }

    private static string Counters() => ComponentChanges.LabCounters() ?? "(no counters)";

    private static long Counter(string name)
    {
        foreach (var part in Counters().Split(' '))
            if (part.StartsWith(name + "=", StringComparison.Ordinal) && long.TryParse(part[(name.Length + 1)..], out var v)) return v;
        return -1;
    }

    private async Task<List<EntityId>> SpawnPlayers(int n, WorldPosition at)
    {
        var made = new List<EntityId>();
        for (var i = 0; i < n; i++)
        {
            var e = await Spawn.Entity("1_Player", new WorldPosition(at.X + 2 * (i % 16), at.Y, at.Z + 2 * (i / 16)),
                options: new SpawnOptions { Tag = Id, Flags = SpawnFlags.Transient });
            if (!e.IsNone) made.Add(e);
        }
        _spawned.AddRange(made);
        return made;
    }

    private async Task Run()
    {
        try
        {
            for (var n = 0; n < 3000 && !Spawn.WorldRunning; n++) await Context.Scheduler.NextTick();
            await Wait(120);
            Check(Capabilities.Is(ComponentChanges.Capability) && Capabilities.Is("kiln.component_changes"),
                $"WATCH-ON: events.component_net and kiln.component_changes are on ({Capabilities.Why(ComponentChanges.Capability)}); {Counters()}");
            _subs.Add(Events.On<ComponentValueChangedEvent>(e => _changes.Add(e)));
            _subs.Add(Events.On<AttributeValueChangedEvent>(e => _attributes.Add(e)));
            _subs.Add(Events.On<ComponentWatchStateChangedEvent>(e => _states.Add(e)));
            var at = new WorldPosition(3845, 835, 1480);
            _mode = Mode();
            Log.Info($"{Id}: mode {_mode} ({Path.Combine(Context.DataDirectory, "mode")})");
            if (_mode == "keep")
            {
                // a world stop with live watches: the full budget, a Players watch, a suspended one, none disposed
                Check(Counter("watches") == 0, $"KEEP: no watch before this run ({Counters()})");
                await Budget(at, keep: true, cost: false);
                return;
            }
            if (_mode == "fresh")
            {
                // the boot after that stop: nothing of the first world's watches; a fresh one works; the same fill as
                // KEEP, all disposed before the stop (the stop's timing without watches)
                Check(Counter("watches") == 0 && Counter("pairs") == 0, $"FRESH: no watch from the first world exists ({Counters()})");
                var fresh = await SpawnPlayers(2, at);
                await Wait(45);
                await Origin("fresh", fresh[0]);
                await Budget(at, keep: false, cost: false);
                Check(Counter("watches") == 0, $"FRESH: every watch of this run disposed before the stop ({Counters()})");
                return;
            }
            if (_mode == "release")
            {
                await Release("plain");
                if (Kiln.Testing.LabSystems.Enabled)
                {
                    Kiln.Testing.LabSystems.ForceParallel(3000);
                    await Wait(2);
                    Log.Info($"{Id}: parallel forced (mask 0x{Kiln.Testing.LabSystems.PlayersMask:x4})");
                    try
                    {
                        await Release("parallel");
                    }
                    finally
                    {
                        Kiln.Testing.LabSystems.StopParallel();
                        await Wait(2);
                    }
                }
                else Log.Info($"{Id}: the lab parallel switch is off (no kiln/control/systems.parallel): the parallel release was skipped");
                return;
            }
            var roots = await SpawnPlayers(4, at);
            await Wait(45);
            Check(roots.Count == 4 && roots.All(e => Hp(e) > 0), $"four 1_Player stand-ins (no connected player): {string.Join(", ", roots.Select(e => $"{e.Value}: {Hp(e)}"))}");

            await Origin("plain", roots[0]);
            if (Kiln.Testing.LabSystems.Enabled)
            {
                Kiln.Testing.LabSystems.ForceParallel(2400);
                await Wait(2);
                Log.Info($"{Id}: parallel forced (mask 0x{Kiln.Testing.LabSystems.PlayersMask:x4})");
                try
                {
                    await Origin("parallel", roots[1]);
                }
                finally
                {
                    Kiln.Testing.LabSystems.StopParallel();
                    await Wait(2);
                }
            }
            else Log.Info($"{Id}: the lab parallel switch is off (no kiln/control/systems.parallel): the worker-thread run was skipped");

            await Links(roots[2]);
            await Budget(at);
        }
        catch (Exception e)
        {
            Check(false, $"the run threw: {e}");
        }
        finally
        {
            // keep and fresh leave their stand-ins for the stop (the two stops compare the same world)
            if (_mode is not ("keep" or "fresh"))
                foreach (var e in _spawned)
                    if (Spawn.Exists(e)) Spawn.Destroy(e);
            Log.Info($"{Id}: counters at the end: {Counters()}");
            Log.Info($"{Id} done: {_passes} PASS, {_failures} FAIL");
        }
    }

    private ComponentValueChangedEvent[] Of(EntityId e, string component) => _changes.Where(c => c.Entity == e && c.Component == component).ToArray();

    // WATCH-NET-ORIGIN
    private async Task Origin(string run, EntityId p)
    {
        var w = ComponentChanges.Watch(new(ComponentWatchScope.Entities, [p], ObservedComponents.Health | ObservedComponents.Stamina | ObservedComponents.Mana));
        Check(w.Ok && w.Watch is { State: ComponentWatchState.Ready, World: WorldKind.Server } && w.Pairs >= 1,
            $"[{run}] a watch on {p.Value} is made: ok {w.Ok} pairs {w.Pairs} ({w.Reason})");
        if (!w.Ok) return;
        try
        {
            await Wait(3);
            var hp = Hp(p);
            var tokensBefore = Counter("tokens");
            var offBefore = Counter("tokens_off_world_thread");

            // a write no Kiln writer reports (this mod's world program, inside the simulation)
            _changes.Clear();
            var raw = await RawWrite(p);
            await Wait(3);
            var u = Of(p, "Health");
            Check(raw && u.Length == 1 && u[0].Origin == ChangeOrigin.Unknown && u[0].SourceMod is null && u[0].OperationTokens.Count == 0 &&
                  u[0].Before.Length == 40 && u[0].After.Length == 40 && u[0].WatchIds.SequenceEqual([w.Watch!.Id]),
                $"[{run}] WATCH-NET-ORIGIN unknown: a write without a token (world program on thread {_rawThread}, {_updates} updates) -> " +
                $"{u.Length} event(s), origin {u.FirstOrDefault()?.Origin}, source {u.FirstOrDefault()?.SourceMod ?? "none"}; never Game");

            // Combat.SetHealth in Pre: kiln.combat's token
            _changes.Clear();
            _attributes.Clear();
            hp = Hp(p);
            Combat.SetHealth(p, hp - 5);
            await Wait(3);
            var k = Of(p, "Health");
            var typed = _attributes.Where(a => a.Change.Entity == p && a.Attribute == ObservedAttribute.Health).ToArray();
            Check(k.Length == 1 && k[0].Origin == ChangeOrigin.Kiln && k[0].SourceMod == "kiln.combat" && k[0].OperationTokens.Count >= 1,
                $"[{run}] WATCH-NET-ORIGIN kiln: Combat.SetHealth {hp} -> {hp - 5}: {k.Length} event(s), origin {k.FirstOrDefault()?.Origin}, source {k.FirstOrDefault()?.SourceMod}");
            Check(typed.Length == 1 && typed[0].Before == hp && typed[0].After == hp - 5 && typed[0].MaximumBefore == typed[0].MaximumAfter && typed[0].MaximumAfter > 0,
                $"[{run}] the typed Health event: {typed.Length} event(s), {typed.FirstOrDefault()?.Before} -> {typed.FirstOrDefault()?.After} of {typed.FirstOrDefault()?.MaximumAfter}");

            // a tracked write (kiln.world's write program, inside the simulation): this mod's id
            _changes.Clear();
            var health = World.ReadBytes(p, "Health");
            var offset = health is { Length: >= 8 } ? BitConverter.ToUInt16(health, 4) : 0;
            hp = Hp(p);
            var written = offset >= 8 ? await World.WriteAt(p, "Health", offset, hp - 3) : null;
            await Wait(3);
            var t = Of(p, "Health");
            Check(written is { Ok: true } && t.Length == 1 && t[0].Origin == ChangeOrigin.Kiln && t[0].SourceMod == Id,
                $"[{run}] WATCH-NET-ORIGIN kiln: World.WriteAt of Health: {written?.Status} ({written?.Reason}), {t.Length} event(s), origin {t.FirstOrDefault()?.Origin}, source {t.FirstOrDefault()?.SourceMod}");

            // a token and a write without one in one interval: Mixed
            _changes.Clear();
            hp = Hp(p);
            Combat.SetHealth(p, hp - 2);
            raw = await RawWrite(p);
            await Wait(3);
            var m = _changes.Where(c => c.Entity == p && c.Component == "Health").ToArray();
            Check(raw && m.Length >= 1 && m.Any(c => c.Origin == ChangeOrigin.Mixed),
                $"[{run}] WATCH-NET-ORIGIN mixed (a gap): kiln.combat then a write without a token: {string.Join(", ", m.Select(c => $"{c.Origin}/{c.SourceMod ?? "none"}"))}");

            // two sources in one interval: Mixed, both named
            _changes.Clear();
            hp = Hp(p);
            Combat.SetHealth(p, hp - 1);
            health = World.ReadBytes(p, "Health");
            offset = health is { Length: >= 8 } ? BitConverter.ToUInt16(health, 4) : 0;
            var second = World.WriteAt(p, "Health", offset, hp - 4);
            await Wait(4);
            var two = _changes.Where(c => c.Entity == p && c.Component == "Health").ToArray();
            Check(two.Any(c => c.Origin == ChangeOrigin.Mixed && (c.SourceMod ?? "").Contains("kiln.combat") && (c.SourceMod ?? "").Contains(Id)),
                $"[{run}] WATCH-NET-ORIGIN mixed (two sources): {string.Join(", ", two.Select(c => $"{c.Origin}/{c.SourceMod ?? "none"}"))} ({(await second).Status})");

            // A -> B -> A inside one interval: nothing
            await Wait(2);
            _changes.Clear();
            hp = Hp(p);
            Combat.SetHealth(p, hp - 1);
            Combat.SetHealth(p, hp);
            await Wait(4);
            Check(Of(p, "Health").Length == 0 && Hp(p) == hp, $"[{run}] WATCH-NET-ORIGIN A -> B -> A gives no event ({Of(p, "Health").Length})");
            var tokens = Counter("tokens") - tokensBefore;
            Check(tokens >= 5, $"[{run}] kiln.component_changes took {tokens} token(s) in this run, {Counter("tokens_off_world_thread") - offBefore} off the world thread");
        }
        finally
        {
            w.Watch!.Dispose();
            await Wait(2);
        }
    }

    // WATCH-LINKS
    private async Task Links(EntityId q)
    {
        var root = ComponentChanges.Watch(new(ComponentWatchScope.Entities, [q], ObservedComponents.Health | ObservedComponents.Stamina | ObservedComponents.Mana | ObservedComponents.SlotSelection));
        var rootPairs = root.Pairs;
        root.Watch?.Dispose();
        var w = ComponentChanges.Watch(new(ComponentWatchScope.Entities, [q], ObservedComponents.All, ComponentWatchExpansion.PlayerInventoryAndItems));
        var setup = World.ReadBytes(q, "InventorySetup");
        var links = 0;
        if (setup is { Length: >= 84 })
            for (var i = 0; i < 16 && BitConverter.ToUInt32(setup, 4 * i) != 0; i++) links++;
        Check(w.Ok && w.Pairs > rootPairs, $"WATCH-LINKS: the expanded watch on {q.Value} covers {w.Pairs} pair(s), the root alone {rootPairs} ({links} InventorySetup link(s)) ({w.Reason})");
        if (!w.Ok) return;
        try
        {
            _subs.Add(Events.On<ComponentValueChangedEvent>(e => _rootSeen.Add(e), EventFilter.ForEntity(q)));
            await Wait(3);
            _changes.Clear();
            _rootSeen.Clear();
            var given = await Items.Give(q, "Consumable_Grenade_Explosive_PowderBall_T1", 3);
            await Wait(4);
            var inv = _changes.Where(c => c.Component == "Inventory").ToArray();
            Check(given.Done > 0 && inv.Length >= 1 && inv.All(c => c.Entity != q && c.RootEntities.Contains(q) && !c.Key.HasSpawnTime && c.Key.LifeSerial != 0),
                $"WATCH-LINKS: Items.Give into {q.Value} ({given.Status}): {inv.Length} Inventory change(s) of linked page(s) {string.Join(", ", inv.Select(c => c.Entity.Value))}, roots [{string.Join(", ", inv.SelectMany(c => c.RootEntities).Distinct())}]");
            Check(inv.Length >= 1 && inv.All(c => c.Origin == ChangeOrigin.Kiln && c.SourceMod == "kiln.items"),
                $"WATCH-LINKS: the page's change is kiln.items' write: {string.Join(", ", inv.Select(c => $"{c.Origin}/{c.SourceMod ?? "none"}"))}");
            Check(_rootSeen.Any(c => c.Component == "Inventory" && c.Entity != q),
                $"WATCH-LINKS: a handler filtered to the root sees its linked page's change ({_rootSeen.Count} event(s))");
            // the same page entity keeps its life from step to step; a take is reported on it again
            var page = inv.FirstOrDefault()?.Key;
            _changes.Clear();
            var taken = await Items.Take(q, given.Item, 3);
            await Wait(4);
            var again = _changes.Where(c => c.Component == "Inventory").ToArray();
            Check(taken.Done > 0 && page is not null && again.Any(c => c.Key == page.Value),
                $"WATCH-LINKS: Items.Take ({taken.Status}): the same page life {page?.LifeSerial} is reported again ({again.Length} change(s))");
        }
        finally
        {
            w.Watch!.Dispose();
            await Wait(2);
        }
    }

    // WATCH-BUDGET and WATCH-COST. Bipeds are few (at most 64 1_Player stand-ins in all: a run with 100 ended the
    // server with the game's "Not all entities were updated in program 'server_biped_feet_ik_raycast'"); the rest of the
    // 4096 pairs are storage chests (3 inventory pages each) full of water pouches (each pouch is an item entity with
    // Fuel), all spawned transient.
    private const string Pouch = "Tool_Water_Pouch_Refillable", Chest = "Prop_Decoration_T1_Storage_24";

    private async Task<List<(EntityId Root, int Pairs)>> Probe(IEnumerable<EntityId> roots, ObservedComponents components = ObservedComponents.All)
    {
        await Wait(10);
        var found = new List<(EntityId, int)>();
        // the present pairs of each root alone (a watch made and disposed in the same Pre)
        foreach (var e in roots)
        {
            var probe = ComponentChanges.Watch(new(ComponentWatchScope.Entities, [e], components, ComponentWatchExpansion.PlayerInventoryAndItems));
            if (probe.Ok) found.Add((e, probe.Pairs));
            probe.Watch?.Dispose();
        }
        return found;
    }

    private async Task<List<EntityId>> Chests(int n, WorldPosition at, int pouches)
    {
        var made = new List<EntityId>();
        for (var i = 0; i < n; i++)
        {
            var e = await Spawn.Entity(Chest, new WorldPosition(at.X + 3 * (i % 10), at.Y, at.Z + 3 * (i / 10)), 0,
                new SpawnOptions { Tag = Id, Flags = SpawnFlags.Transient });
            if (!e.IsNone) made.Add(e);
        }
        _spawned.AddRange(made);
        await Wait(5);
        if (pouches > 0)
            foreach (var e in made) await Items.Give(e, Pouch, pouches);
        return made;
    }

    private async Task Budget(WorldPosition at, bool keep = false, bool cost = true)
    {
        // 60 more stand-ins (64 in all), each given water pouches into its own inventory
        var bipeds = await SpawnPlayers(60, new WorldPosition(at.X, at.Y, at.Z + 20));
        await Wait(20);
        var given = 0;
        foreach (var e in bipeds) given += (await Items.Give(e, Pouch, 64)).Done;
        var all = await Probe(bipeds);
        Log.Info($"{Id}: 60 1_Player stand-ins: {all.Count} watchable, {given} pouches given, {all.Sum(x => x.Pairs)} present pairs (all six components, inventories expanded)");
        // chests of 24 pouches until the pairs reach 4096
        for (var round = 0; all.Sum(x => x.Pairs) < 4096 + 30 && round < 16; round++)
            all.AddRange(await Probe(await Chests(20, new WorldPosition(at.X - 40, at.Y, at.Z + 40 + 7 * round), 24)));
        var chosen = new List<(EntityId Root, int Pairs)>();
        var sum = 0;
        foreach (var x in all)
            if (sum + x.Pairs <= 4096 && chosen.Count < 20 * 128)
            {
                chosen.Add(x);
                sum += x.Pairs;
            }
        var need = 4096 - sum;
        // the rest exactly: a chest given `need` pouches, watched for Fuel only (one pair a pouch); and one more chest with
        // one pouch for the refusal
        var filler = need > 0 ? (await Chests(1, new WorldPosition(at.X + 40, at.Y, at.Z + 40), Math.Min(24, need))).FirstOrDefault() : default;
        var extra = (await Chests(1, new WorldPosition(at.X + 44, at.Y, at.Z + 40), 1)).FirstOrDefault();
        await Wait(10);
        Log.Info($"{Id}: {all.Count} roots probed ({all.Sum(x => x.Pairs)} pairs), {chosen.Count} chosen ({sum} pairs), {need} left for a filler chest");
        var watches = new List<ComponentWatch>();
        var total = 0;
        try
        {
            foreach (var g in chosen.Chunk(Math.Max(1, (chosen.Count + 19) / 20)))
            {
                var r = ComponentChanges.Watch(new(ComponentWatchScope.Entities, g.Select(x => x.Root).ToArray(), ObservedComponents.All,
                    ComponentWatchExpansion.PlayerInventoryAndItems));
                if (r.Ok) { watches.Add(r.Watch!); total += r.Pairs; }
                else Log.Warn($"{Id}: a group of roots was refused: {r.Reason}");
            }
            if (!filler.IsNone)
            {
                var r = ComponentChanges.Watch(new(ComponentWatchScope.Entities, [filler], ObservedComponents.Fuel, ComponentWatchExpansion.PlayerInventoryAndItems));
                if (r.Ok) { watches.Add(r.Watch!); total += r.Pairs; }
                else Log.Warn($"{Id}: the filler chest was refused: {r.Reason}");
            }
            await Wait(3);
            var present = Counter("present");
            Check(present == 4096, $"WATCH-BUDGET: exactly 4096 present pairs are taken ({watches.Count} watches, {total} pairs by their results, service says {present})");
            var one = extra.IsNone ? null : ComponentChanges.Watch(new(ComponentWatchScope.Entities, [extra], ObservedComponents.Fuel,
                ComponentWatchExpansion.PlayerInventoryAndItems));
            Check(one is { Ok: false, Watch: null, Pairs: 1 } && one.Reason!.Contains("4097"),
                $"WATCH-BUDGET: one more disjoint pair is refused: ok {one?.Ok}, watch {(one?.Watch is null ? "none" : "made")}, reason: {one?.Reason}");
            if (keep)
            {
                // the stop's run: a Players watch beside the full budget (no player in a lab: no pair)
                var players = ComponentChanges.Watch(new(ComponentWatchScope.Players, [], ObservedComponents.Health | ObservedComponents.Stamina));
                Check(players is { Ok: true, Pairs: 0 }, $"KEEP: a Players watch is made beside the full budget: {players.Ok} {players.Pairs} {players.Reason}");
                if (players.Watch is { } pw) watches.Add(pw);
            }
            // overlap is free: up to 32 watches; the 33rd is refused
            while (!keep && watches.Count < 32)
            {
                var r = ComponentChanges.Watch(new(ComponentWatchScope.Entities, [all[0].Root], ObservedComponents.Health));
                if (!r.Ok) break;
                watches.Add(r.Watch!);
            }
            var many = keep ? null : ComponentChanges.Watch(new(ComponentWatchScope.Entities, [all[0].Root], ObservedComponents.Health));
            if (many is not null)
            Check(watches.Count == 32 && !many.Ok && many.Watch is null && (many.Reason ?? "").Contains("32 watches"),
                $"WATCH-BUDGET: the 33rd watch is refused: {many.Reason}");
            // a Players watch with no player in the game: made, no pair (no connected player in a lab)
            var everyone = keep ? null : ComponentChanges.Watch(new(ComponentWatchScope.Players, [], ObservedComponents.Health));
            if (everyone is not null)
            Check(!everyone.Ok && (everyone.Reason ?? "").Contains("32 watches"), $"WATCH-BUDGET: a 33rd watch of scope Players is refused the same way ({everyone.Reason})");

            // WATCH-COST: 1600 steps with 4096 pairs; the stand-ins' own stamina and health recharge as the game runs them
            _changes.Clear();
            var start = Counter("changes");
            await Wait(cost ? 1600 : 30);
            var c = Counters();
            if (cost)
            Check(Counter("samples") > 0 && Counter("present") == 4096 && Counter("overflows") == 0,
                $"WATCH-COST: 4096 pairs over 1600 steps: begin p50 {Counter("begin_p50_ns") / 1000.0:F1} us p99 {Counter("begin_p99_ns") / 1000.0:F1} us max {Counter("begin_max_ns") / 1000.0:F1} us, " +
                $"end p50 {Counter("end_p50_ns") / 1000.0:F1} us p99 {Counter("end_p99_ns") / 1000.0:F1} us max {Counter("end_max_ns") / 1000.0:F1} us; " +
                $"{Counter("changes") - start} change(s) ({_changes.Count} delivered, {_changes.Count(x => x.Origin == ChangeOrigin.Unknown)} Unknown); {c}");

            // a root destroyed: its watch is suspended whole, with its last complete step, and says why
            var victim = watches.FirstOrDefault(x => x.Request.Entities.Count > 1);
            if (victim is not null)
            {
                _states.Clear();
                _changes.Clear();
                var gone = victim.Request.Entities[0];
                var other = victim.Request.Entities[1];
                Spawn.Destroy(gone);
                await Wait(4);
                Combat.SetHealth(other, Hp(other) - 1);
                await Wait(4);
                var s = _states.FirstOrDefault(x => x.WatchId == victim.Id);
                Check(s is { State: ComponentWatchState.SuspendedMetadata } && s.LastCompleteStep > 0 && victim.State == ComponentWatchState.SuspendedMetadata &&
                      !_changes.Any(x => x.Entity == other && x.WatchIds.SequenceEqual([victim.Id])),
                    $"WATCH-BUDGET: a destroyed root suspends its watch whole: {s?.State} ({s?.Reason}), last complete step {s?.LastCompleteStep}; the other root's change not reported for it");
                var resumed = ComponentChanges.Resume(victim);
                Check(!resumed.Ok && (resumed.Reason ?? "").Contains("gone"), $"WATCH-BUDGET: Resume answers why it cannot: {resumed.Reason}");
            }
            else Check(false, "WATCH-BUDGET: no filler watch with two roots to suspend");
        }
        finally
        {
            if (keep)
            {
                // nothing is disposed: the server's stop has to end them
                Log.Info($"{Id}: KEEP: {watches.Count} watch(es) stay live for the stop ({string.Join(", ", watches.GroupBy(w => w.State).Select(g => $"{g.Count()} {g.Key}"))}): {Counters()}");
            }
            else
            {
                foreach (var w in watches) w.Dispose();
                await Wait(2);
                Log.Info($"{Id}: watches disposed: {Counters()}");
            }
        }
    }
}
