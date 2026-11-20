// SPDX-License-Identifier: MIT
// The gem wave's four packages together (docs/kiln/services/gems.md) on a dedicated lab server, player-free (a spawned
// 1_Player stand-in next to a spawned Shroud Forge), two starts.
// First start: Gems.AllowSockets(Rings) (G4); Gems.AddSocket on a plain ring, read back (Done, nothing missing); a
// Gems.Register kill gem (G2) socketed into a ring with Gems.Socket at the forge and upgraded there (G3), the ring worn
// (G1's Gems.Equipped lists a ring slot), a killing blow of the wearer runs the gem's C# handler with the ring as its
// carrier; Gems.ReturnSocketedGems (G4 through G3's forgeless return) takes the gem back out of the worn ring;
// GemCraftedEvent and GemSocketChangedEvent (G1) for each step. A second gem with a backstab trigger is registered, so the
// world records it; the world is saved and kiln/data/gems-integration-test/phase2.txt written.
// Second start: the backstab gem is not registered. Its kiln.content placeholder must not run its base gem's program (it
// used to): Kiln serves the backstab trigger program the placeholder names (no registered gem does), the placeholder runs
// it and calls nothing; without Kiln's copy kiln.content would make it none (refs.cpp). Socketed in a worn ring it runs the
// trigger program, no handler; the kill gem in the same worn ring runs too (control).
using Kiln;

namespace GemsIntegrationTest;

public sealed class GemsIntegrationTestMod : Mod
{
    private const string Id = "gems-integration-test";
    private const string ForgeTemplate = "Shroud_Gem_Forge_01_Grassland", EssenceName = "Material_T1_Fognest_Ressource";
    private const uint RingLight = 0x79442800;  // Ring_T3_Ring_of_Light (fillers: the first two rings given are worn)
    private const uint RingShock = 0x4623bcb0;  // Ring_T6_Ring_of_Shock
    private static readonly Guid HitProgram = Guid.Parse("531786a3-5547-4ad0-a433-b878a3c08124");

    private int _pass, _fail;
    private readonly List<EntityId> _spawned = new();
    private readonly List<GemTriggerContext> _kills = new();
    private readonly List<GemCraftedEvent> _crafted = new();
    private readonly List<GemSocketChangedEvent> _changed = new();
    private readonly List<GemTriggeredEvent> _triggered = new();
    private GemHandle? _reaper, _ghost;
    private bool _phase2, _probe, _backpack, _landed;
    private string _marker = "";
    private ulong _step;  // the server world's step (Context.Tick.Pre)
    private WorldPosition _at;

    private void Info(string s) => Log.Info($"{Id}: INFO {s}");

    private void Check(bool ok, string what)
    {
        if (ok) { _pass++; Log.Info($"{Id}: PASS {what}"); }
        else { _fail++; Log.Error($"{Id}: FAIL {what}"); }
    }

    public override void OnLoad()
    {
        if (!Client.IsDedicatedServer) return;
        _marker = Path.Combine(Context.DataDirectory, "phase2.txt");
        _phase2 = File.Exists(_marker);
        _probe = File.Exists(Path.Combine(Context.DataDirectory, "probe.txt"));
        _backpack = File.Exists(Path.Combine(Context.DataDirectory, "backpack.txt"));
        Context.Tick.Pre += (in TickInfo t) => _step = t.Step;
        Info($"start {(_backpack ? "backpack" : _probe ? "probe" : _phase2 ? "2" : "1")}");
        var setup = Gems.AllowSockets(GemSocketKinds.Rings);
        Check(setup.Server, $"AllowSockets(Rings) works on the server: {setup}");
        _reaper = Gems.Register(new GemDefinition
        {
            Id = $"{Id}:reaper", BaseOn = "T1_Gem_GemOfBleeding", DisplayName = "Gem of the Reaper (lab)",
            Trigger = GemTrigger.OnKill(), OnTrigger = ctx =>
            {
                _kills.Add(ctx);
                Info($"reaper handler: {ctx.Trigger} by {ctx.Player} on {ctx.Target}, level {ctx.Level}, slot {ctx.Slot}, carrier {ctx.Carrier}");
            },
        });
        Check(_reaper.Ok, $"Gems.Register: {_reaper}");
        if (!_phase2)
        {
            _ghost = Gems.Register(new GemDefinition
            {
                Id = $"{Id}:ghost", BaseOn = "T1_Gem_GemOfBleeding", DisplayName = "Gem of the Ghost (lab)",
                Trigger = GemTrigger.OnBackstab(), OnTrigger = ctx => Info($"ghost handler: {ctx.Trigger}"),
            });
            Check(_ghost.Ok, $"Gems.Register: {_ghost}");
        }
        Events.On<GemCraftedEvent>(e =>
        {
            _crafted.Add(e);
            Info($"event: {e}");
        });
        Events.On<GemSocketChangedEvent>(e =>
        {
            _changed.Add(e);
            Info($"event: {e}");
        });
        Events.On<GemTriggeredEvent>(e => _triggered.Add(e));
        Events.On<ImpactSignalEvent>(e =>
        {
            if (e.Tag == 0x701) _landed = true;
        });
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
            var sw = System.Diagnostics.Stopwatch.StartNew();
            while (!Spawn.WorldRunning && sw.Elapsed.TotalSeconds < 240) await Context.Scheduler.NextTick();
            await Steps(60);
            if (_backpack) await Backpack();
            else if (_probe) await Probe();
            else if (_phase2) await Phase2();
            else await Phase1();
        }
        catch (Exception e)
        {
            Check(false, $"exception: {e}");
        }
        foreach (var e in _spawned) Spawn.Destroy(e);
        await Steps(3);
        Log.Info($"{Id} done: {_pass} PASS, {_fail} FAIL");
    }

    // ---- helpers ----

    private static int Count(EntityId p, ItemId item) => Items.Read(p)?.Slots.Where(s => !s.IsEmpty && s.Item == item).Sum(s => s.Count) ?? -1;

    private static InventorySlot? SlotOf(EntityId holder, EntityId pide) =>
        Items.Read(holder)?.Slots.FirstOrDefault(s => !s.IsEmpty && s.ItemEntity == pide);

    private static bool HasSocket(EntityId pide) => !pide.IsNone && World.Has(pide, "WeaponGemSlot");

    private async Task<InventorySlot?> GiveOne(EntityId to, ItemId item, Gear? gear = null)
    {
        var r = await Items.Give(to, item, 1, gear, GiveOptions.NoMerge);
        if (r.Status != ItemsStatus.Done || r.Changes.Count == 0)
        {
            Info($"give {item} to {to}: {r}");
            return null;
        }
        await Steps(2);
        return SlotOf(to, r.Changes[0].ItemEntity);
    }

    private async Task<(EntityId Player, EntityId Forge)?> Stage()
    {
        WorldPosition? anchor = null;
        foreach (var f in new[] { "FlameAltar", "Altar", "Townsfolk", "NPC_" })
        {
            var id = Spawn.FindByName(f, max: 1).FirstOrDefault();
            if (!id.IsNone && Spawn.Inspect(id) is { } inf)
            {
                anchor = inf.Position;
                break;
            }
        }
        if (anchor is not { } a)
        {
            Check(false, "no anchor entity in the world");
            return null;
        }
        _at = new WorldPosition(a.X + 40, a.Y + 0.5, a.Z + 40);
        var p = await Spawn.Entity("1_Player", _at, 0, new SpawnOptions { Tag = "gemsint", Flags = SpawnFlags.Transient });
        _spawned.Add(p);
        var forge = await Spawn.Entity(ForgeTemplate, new WorldPosition(a.X + 43, a.Y + 0.5, a.Z + 40), 0,
            new SpawnOptions { Tag = "gemsint", Flags = SpawnFlags.Transient });
        _spawned.Add(forge);
        await Steps(30);
        Check(!p.IsNone && World.Has(p, "PlayerInput") && Items.Read(p) is { Initialized: true }, $"player character {p} with an inventory");
        Check(!forge.IsNone && Spawn.Exists(forge), $"a {ForgeTemplate} spawned: {forge}");
        if (p.IsNone || forge.IsNone) return null;
        // the first two rings a player gets are worn: two fillers, so the socketed rings stay in the backpack
        await GiveOne(p, new ItemId(RingLight));
        await GiveOne(p, new ItemId(RingLight));
        await Steps(4);
        return (p, forge);
    }

    private async Task<EntityId> Enemy()
    {
        var e = await Spawn.Entity("Enemy_Vukah_Melee", new WorldPosition(_at.X + 3, _at.Y, _at.Z), 0,
            new SpawnOptions { Tag = "gemsint", Flags = SpawnFlags.Transient });
        _spawned.Add(e);
        await Steps(10);
        return e;
    }

    private static uint Fnv(string s)
    {
        var h = 0x811c9dc5u;
        foreach (var c in System.Text.Encoding.UTF8.GetBytes(s)) h = (h ^ c) * 0x1000193u;
        return h;
    }

    private static string Cfg(string name) => $"0x{Fnv(name):x8}";

    // a hit of `attacker` on `target` through the game's applyDamage (impact/hit.kimp, gems-custom-test's program)
    private async Task Hit(EntityId attacker, EntityId target, Guid weapon, float cut)
    {
        var at = World.Position(target)!.Value;
        var h0 = Combat.GetHealth(target);
        _landed = false;
        var he = await Abilities.RunProgram(HitProgram, at, new ImpactOptions
        {
            Root = attacker,
            Configs = new ImpactConfigs
            {
                [Cfg("gemtest.Target")] = target, [Cfg("gemtest.Weapon")] = weapon, [Cfg("gemtest.Damage")] = new DamageSetup { Cut = cut },
                [Cfg("gemtest.At")] = new[] { (float)at.X, (float)at.Y, (float)at.Z }.SelectMany(BitConverter.GetBytes).ToArray(),
            },
            Tag = "gemsint",
        });
        await Steps(12);
        Spawn.Destroy(he);
        Info($"hit {attacker} -> {target} ({cut} cut): health {h0?.Current} -> {Combat.GetHealth(target)?.Current}{(_landed ? "" : " (the hit did not land)")}");
    }

    // ---- probe (kiln/data/gems-integration-test/probe.txt): a new item entity at the moment its give is delivered ----
    // What a read right at a transaction's completion (ItemsGivenEvent, Pre of the step after it ran) sees of a new ring
    // given at level 20 (its range starts at 40), Epic, 2 upgrades, and of AddSocket's re-creation of it, then step by
    // step: ItemState's UI rarity / level, the Level attribute's two values, PerkContainerNew (owner's client report:
    // AddSocket read back level 40 and Common where the ring was 42 and Epic).

    private string Snap(EntityId p, EntityId pide)
    {
        var sl = SlotOf(p, pide);
        var lv = World.ReadBytes(pide, "Level");
        var pk = World.ReadBytes(pide, "PerkContainerNew");
        return $"step {_step}: ItemState rarity {sl?.Rarity} level {sl?.Level}; Level values " +
               (lv is { Length: >= 16 } ? $"{BitConverter.ToUInt32(lv, 8)}/{BitConverter.ToUInt32(lv, 12)}" : "none") +
               $"; PerkContainerNew {(pk is null ? "none" : Convert.ToHexString(pk))}; socket {HasSocket(pide)}";
    }

    private async Task Probe()
    {
        File.Delete(Path.Combine(Context.DataDirectory, "probe.txt"));
        if (await Stage() is not { } st) return;
        var (p, _) = st;
        var delivered = new List<EntityId>();
        using var sub = Events.On<ItemsGivenEvent>(e =>
        {
            if (e.Item.Value != RingShock || e.Result.Changes.Count == 0) return;
            var pide = e.Result.Changes[0].ItemEntity;
            delivered.Add(pide);
            Info($"probe {pide} at delivery, {Snap(p, pide)}");
        });
        async Task Follow(EntityId pide)
        {
            for (var i = 1; i <= 4; i++)
            {
                await Steps(1);
                Info($"probe {pide} after {i} tick(s), {Snap(p, pide)}");
            }
        }
        var give = await Items.Give(p, new ItemId(RingShock), 1, new Gear(20, ItemRarity.Epic) { Unlocked = 2 }, GiveOptions.NoMerge);
        Info($"probe give: {give}; the mod's await came back at step {_step}");
        if (delivered.Count == 0) { Check(false, "probe: no ItemsGivenEvent for the ring"); return; }
        await Follow(delivered[0]);
        var ring = SlotOf(p, delivered[0]);
        if (ring is null) { Check(false, "probe: the ring is not in the inventory"); return; }
        var add = await Gems.AddSocket(ring.Ref);
        Info($"probe AddSocket: {add}; the mod's await came back at step {_step}");
        if (delivered.Count < 2) { Check(false, "probe: no ItemsGivenEvent for the re-creation"); return; }
        await Follow(delivered[1]);
        Check(true, "probe done");
    }

    // ---- backpack (kiln/data/gems-integration-test/backpack.txt): ReturnSocketedGems with room only in a worn backpack ----
    // Owner's client 2026-11-16: "no free backpack slot for the gem" with free slots on the backpack's pages, which a
    // player's InventorySetup reaches through a Virtual (5) link to the backpack's own setup; Kiln's precheck read only
    // top-level Generic links.

    private static (uint Link, byte Category)[] Links(EntityId e) =>
        World.ReadBytes(e, "InventorySetup") is { Length: >= 0x50 } b
            ? Enumerable.Range(0, 16).Select(k => (BitConverter.ToUInt32(b, 4 * k), b[0x40 + k])).TakeWhile(l => l.Item1 != 0).ToArray()
            : [];

    private static string Tree(EntityId e, int depth = 0) =>
        string.Join(" ", Links(e).Select(l => l.Category == 5 && depth < 3 ? $"{l.Link}:V[{Tree(new EntityId(l.Link), depth + 1)}]" : $"{l.Link}:{l.Category}"));

    // the Generic pages a setup reaches, each with whether it sits behind a Virtual link
    private static List<(uint Page, bool Virtual)> GenericPages(EntityId e, bool behind = false, int depth = 0)
    {
        var list = new List<(uint, bool)>();
        foreach (var (link, cat) in Links(e))
            if (cat == 5 && depth < 3) list.AddRange(GenericPages(new EntityId(link), true, depth + 1));
            else if (cat == 4) list.Add((link, behind));
        return list;
    }

    private async Task Backpack()
    {
        File.Delete(Path.Combine(Context.DataDirectory, "backpack.txt"));
        if (await Stage() is not { } st) return;
        var (p, forge) = st;
        Info($"backpack: the stand-in's setup before: {Tree(p)}");
        var packId = Items.Lookup("Backpack_T1_NEW")?.Id ?? ItemId.None;
        var pack = await GiveOne(p, packId);
        var eqPack = pack is null ? null : await Items.Equip(p, pack.Slot, EquipmentSlot.Backpack);
        await Steps(10);
        Info($"backpack: {packId} given {pack?.Slot}, equip {eqPack?.Status} {eqPack?.Message}; setup now: {Tree(p)}");
        var ring = await GiveOne(p, new ItemId(RingShock), new Gear(20, ItemRarity.Rare) { GemSlot = true });
        var gem = await GiveOne(p, Items.Lookup("T1_Gem_GemOfBleeding")!.Id, new Gear(5));
        if (ring is null || gem is null) { Check(false, $"backpack: ring {ring}, gem {gem}"); return; }
        var put = await Gems.Socket(p, ring.Ref, gem.Ref, forge, new GemCraftOptions { Cost = GemCost.Free });
        var eq = await Items.Equip(p, SlotOf(p, ring.ItemEntity)!.Slot, EquipmentSlot.Ring0);
        await Steps(4);
        Check(put.Ok && eq.Status == ItemsStatus.Done && Gems.Socket(ring.Ref)?.Gem == gem.Item,
            $"backpack: a worn ring with a Gem of Bleeding ({put.Status}, equip {eq.Status})");
        var pages = GenericPages(p);
        Info($"backpack: Generic pages {string.Join(" ", pages.Select(x => $"{x.Page}{(x.Virtual ? "(V)" : "")}"))}");
        if (!pages.Any(x => x.Virtual))
        {
            Check(false, "backpack: the stand-in's setup has no Virtual link to a backpack's pages: the owner's structure is not reproduced");
            return;
        }
        // fill every Generic slot (rings: one per slot), then free exactly one on a backpack page
        var filler = new ItemId(RingLight);
        for (var i = 0; i < 300; i++)
        {
            var r = await Items.Give(p, filler, 1, null, GiveOptions.NoMerge);
            if (r.Status != ItemsStatus.Done) { Info($"backpack: full after {i} filler(s): {r.Status} {r.Message}"); break; }
        }
        await Steps(4);
        bool OnPage(InventorySlot sl, bool @virtual) => pages.Any(x => x.Page == sl.Slot.Inventory.Value && x.Virtual == @virtual);
        var inBackpack = Items.Read(p)!.Slots.FirstOrDefault(sl => !sl.IsEmpty && sl.Item == filler && OnPage(sl, true));
        if (inBackpack is null) { Check(false, "backpack: no filler on a backpack page to take"); return; }
        var take = await Items.Take(p, filler, 1, inBackpack.Slot, exactSlot: true, itemEntity: inBackpack.ItemEntity);
        await Steps(4);
        var slots = Items.Read(p)!.Slots;
        var topFree = slots.Count(sl => sl.IsEmpty && OnPage(sl, false));
        var packFree = slots.Count(sl => sl.IsEmpty && OnPage(sl, true));
        Check(take.Status == ItemsStatus.Done && topFree == 0 && packFree == 1,
            $"backpack: the only free Generic slot is on a backpack page ({take.Status}; top-level free {topFree}, backpack free {packFree})");
        var back = await Gems.ReturnSocketedGems(p);
        await Steps(4);
        Info($"backpack: ReturnSocketedGems: {string.Join(" | ", back)}");
        var landed = SlotOf(p, gem.ItemEntity);
        Check(back.Count == 1 && back[0].Returned && landed is not null && OnPage(landed, true) && Gems.Socket(ring.Ref) is null,
            $"backpack: ReturnSocketedGems puts the gem into the backpack's free slot ({landed?.Slot}): {string.Join(" | ", back)}");
    }

    // ---- first start ----

    private async Task Phase1()
    {
        foreach (var row in new[] { "gems.read", "gems.triggers", "gems.craft", "gems.sockets" })
            Check(Capabilities.Is(row), $"{row} is on ({Capabilities.Why(row)})");
        if (await Stage() is not { } st) return;
        var (p, forge) = st;
        var essence = Items.Lookup(EssenceName)!.Id;
        var sword = Content.ItemData.Get("Weapon_T1_1H_SwordEpic_01")?.Get("weaponCategoryReference").Guid ?? Guid.Empty;

        // G4: a ring with an empty socket, and AddSocket on a plain one (read back: nothing missing)
        var ring = await GiveOne(p, new ItemId(RingShock), new Gear(20, ItemRarity.Rare) { GemSlot = true });
        Check(ring is { IsEquipped: false } && HasSocket(ring.ItemEntity) && Gems.Socket(ring.Ref) is null,
            $"Gear.GemSlot: a ring with an empty socket in the backpack ({ring})");
        if (ring is null) return;
        var plain = await GiveOne(p, new ItemId(RingShock), new Gear(20, ItemRarity.Epic) { Unlocked = 2 });
        if (plain is not null)
        {
            var add = await Gems.AddSocket(plain.Ref);
            Check(add.Status == GemSocketStatus.Done && add.Missing.Count == 0 && HasSocket(add.After.Entity.Id) && Game.IsTickThread,
                $"AddSocket read back: {add} (missing: {string.Join(", ", add.Missing)})");
        }
        else Check(false, "a plain ring for AddSocket");

        Info($"give essence: {await Items.Give(p, EssenceName, 300)}");
        var gem = await GiveOne(p, _reaper!.Item, new Gear(7));
        Check(Gems.Get(_reaper.Item)?.DisplayName == "Gem of the Reaper (lab)",
            $"Gems.Get gives a registered gem the DisplayName it was registered with: {Gems.Get(_reaper.Item)?.DisplayName}");
        Check(gem is { Level: 7 } && !gem.ItemEntity.IsNone && Gems.Get(_reaper.Item) is { FromMod: true },
            $"the registered gem in the backpack at level 7 ({gem}); Gems.Get: {Gems.Get(_reaper.Item)}");
        if (gem is null) return;
        await Steps(30);  // the socket poll reads the player once: changes count from its next read

        // G3 on G2's gem and G4's ring: Socket at the forge, the game's cost
        _crafted.Clear();
        var e0 = Count(p, essence);
        var socket = await Gems.Socket(p, ring.Ref, gem.Ref, forge);
        await Steps(2);
        Check(socket.Ok && Gems.Socket(ring.Ref) is { } sg && sg.Gem == _reaper.Item && sg.Level == 7,
            $"Gems.Socket put the registered gem into the ring: {socket}; the ring's socket: {Gems.Socket(ring.Ref)}");
        Check(socket.Paid.Count > 0 && socket.Paid.Item == essence && socket.Paid.Count == e0 - Count(p, essence),
            $"Socket paid the forge's price {socket.Paid} ({e0 - Count(p, essence)} counted)");
        Check(_crafted.Count(c => c.ByCode && c.Action == GemCraftAction.Socket && c.Carrier.Entity.Id == ring.ItemEntity) == 1,
            $"one GemCraftedEvent (Socket, by code, the ring): {_crafted.Count}");
        await Steps(10);
        var up = await Gems.Upgrade(p, ring.Ref, forge);
        await Steps(2);
        Check(up.Ok && up.LevelBefore == 7 && up.LevelAfter == 8 && Gems.Socket(ring.Ref)?.Level == 8,
            $"Gems.Upgrade of the ring's gem: {up}");
        Check(_crafted.Count(c => c.ByCode && c.Action == GemCraftAction.Upgrade && c.LevelAfter == 8) == 1, "one GemCraftedEvent (Upgrade 7 -> 8)");
        await Steps(20);
        var mine = _changed.Where(c => c.Owner == p && c.Carrier.Entity.Id == ring.ItemEntity).ToList();
        Check(mine.Any(c => c.Change == GemSocketChange.Socketed && c.After?.Gem == _reaper.Item) && mine.Last().After?.Level == 8,
            $"GemSocketChangedEvent after the forge's events: {string.Join(" | ", mine)}");
        Info($"GemSocketChangedEvent LevelChanged seen: {mine.Any(c => c.Change == GemSocketChange.LevelChanged)} (the upgrade and the socket may fall between two reads)");

        // G1 on G4: the worn ring runs the gem
        var eq = await Items.Equip(p, SlotOf(p, ring.ItemEntity)!.Slot, EquipmentSlot.Ring0);
        await Steps(10);
        var worn = Gems.Equipped(p).FirstOrDefault(g => g.Socket.Gem == _reaper.Item);
        Check(worn is { Slot: GemImpactSlot.Ring1 or GemImpactSlot.Ring2, Impact.IsNone: false } && worn.Socket.Carrier.Entity.Id == ring.ItemEntity && worn.Socket.Level == 8,
            $"Gems.Equipped: the worn ring runs the gem ({eq.Status}): {worn}");

        // G2 on G1 + G4: a killing blow of the wearer runs the handler with the ring
        var vukah = await Enemy();
        _kills.Clear();
        await Hit(p, vukah, sword, 5);
        Check(_kills.Count == 0, $"a hit that does not kill calls no kill handler ({_kills.Count})");
        await Hit(p, vukah, sword, 5000);
        await Steps(4);
        var k = _kills.FirstOrDefault();
        Check(_kills.Count == 1 && k!.Player == p && k.Target == vukah && k.KillingBlow && k.Gem == _reaper.Item && k.Level == 8 &&
              k.Slot == worn?.Slot && k.Carrier.Entity.Id == ring.ItemEntity && k.Impact == worn?.Impact,
            $"the ring's registered gem ran its C# handler on the kill: {_kills.Count} call(s); {(k is null ? "" : $"slot {k.Slot}, carrier {k.Carrier}, level {k.Level}, damage {k.Damage}")}");
        Check(_triggered.All(t => t.Gem.Socket.Gem != _reaper.Item),
            $"no GemTriggeredEvent for a C#-handler gem (its program sends kiln.Signal only): {_triggered.Count(t => t.Gem.Socket.Gem == _reaper.Item)}");

        // G4 through G3: the uninstall path takes the gem out of the worn ring, without a forge, free
        _crafted.Clear();
        _changed.Clear();
        var e1 = Count(p, essence);
        var back = await Gems.ReturnSocketedGems(p);
        await Steps(2);
        Info($"ReturnSocketedGems: {string.Join(" | ", back)}");
        var entry = back.FirstOrDefault(b => b.Carrier.Entity.Id == ring.ItemEntity);
        Check(back.Count == 1 && entry is { Returned: true } && entry.Gem == _reaper.Item && entry.Level == 8,
            "ReturnSocketedGems: one entry, the ring's gem returned (the AddSocket ring's socket is empty)");
        Check(Gems.Socket(ring.Ref) is null && SlotOf(p, gem.ItemEntity) is { Level: 8 } && Count(p, essence) == e1,
            $"the same gem entity is back at level 8, the ring's socket empty, nothing paid ({SlotOf(p, gem.ItemEntity)})");
        Check(_crafted.Count(c => c.ByCode && c.Action == GemCraftAction.Unsocket && c.Forge.IsNone && c.Carrier.Entity.Id == ring.ItemEntity) == 1,
            $"one GemCraftedEvent (Unsocket, by code, no forge): {_crafted.Count}");
        await Steps(20);
        Check(_changed.Any(c => c.Carrier.Entity.Id == ring.ItemEntity && c.Change == GemSocketChange.Unsocketed),
            $"GemSocketChangedEvent Unsocketed for the ring: {string.Join(" | ", _changed)}");
        Check(Gems.Equipped(p).All(g => g.Socket.Gem != _reaper.Item), "the worn ring runs no gem now");

        // the backstab gem is recorded by this world (missing content): saved for the second start
        var saved = await Saves.Request();
        Info($"save: {saved}");
        Directory.CreateDirectory(Path.GetDirectoryName(_marker)!);
        File.WriteAllText(_marker, $"{_ghost!.Guid} {_ghost.Item.Value} {_ghost.Name}");
        Log.Info($"{Id}: RESTART the lab now: the second start checks the backstab gem's placeholder");
    }

    // ---- second start: a removed mod's trigger gem ----

    private async Task Phase2()
    {
        var parts = File.ReadAllText(_marker).Split(' ');
        File.Delete(_marker);
        var guid = Guid.Parse(parts[0]);
        var ghostItem = new ItemId(uint.Parse(parts[1], System.Globalization.CultureInfo.InvariantCulture));
        var name = parts[2];
        var ph = NewContent.Placeholders().FirstOrDefault(d => d.Guid == guid);
        Check(ph is not null, $"the unregistered backstab gem is a placeholder: {ph}");
        var backstab = Guid.Parse("6b696c6e-6765-6d73-8000-000000000004");
        var bleeding = Guid.Parse("b199646f-31a1-495e-b3d1-d5fef2fcaab3");
        Guid? program = null;
        for (var i = 0; i < 200; i++)  // kiln.content retries a placeholder's references every ~2 s
        {
            program = Content.ItemData.Get(name)?.Get("equipment.program").Guid;
            if (program == backstab) break;
            await Steps(1);
        }
        Check(program == backstab, $"the placeholder runs the backstab trigger program Kiln serves (not its base's {bleeding}): {program}");
        Check(Gems.Get(ghostItem) is { } gi && gi.Program == backstab, $"Gems.Get of the placeholder: {Gems.Get(ghostItem)}");
        var kill = Content.ItemData.Get(_reaper!.Name)?.Get("equipment.program").Guid;
        Check(kill == Guid.Parse("6b696c6e-6765-6d73-8000-000000000003"), $"the registered kill gem keeps its trigger program: {kill}");

        if (await Stage() is not { } st) return;
        var (p, forge) = st;
        var ring = await GiveOne(p, new ItemId(RingShock), new Gear(20, ItemRarity.Rare) { GemSlot = true });
        var ghost = await GiveOne(p, ghostItem, new Gear(5));
        Check(ring is not null && ghost is { ItemEntity.IsNone: false }, $"a socketed ring ({ring}) and the placeholder gem ({ghost}) in the backpack");
        if (ring is null || ghost is null) return;
        var s = await Gems.Socket(p, ring.Ref, ghost.Ref, forge, new GemCraftOptions { Cost = GemCost.Free });
        await Steps(2);
        Check(s.Ok && Gems.Socket(ring.Ref)?.Gem == ghostItem, $"the placeholder gem socketed into the ring: {s}");
        var eq = await Items.Equip(p, SlotOf(p, ring.ItemEntity)!.Slot, EquipmentSlot.Ring0);
        await Steps(15);
        var worn = Items.Equipped(p, EquipmentSlot.Ring0);
        var gems = Gems.Equipped(p);
        Check(worn.Entity.Id == ring.ItemEntity && gems.Any(g => g.Socket.Gem == ghostItem && g.Slot is GemImpactSlot.Ring1 or GemImpactSlot.Ring2),
            $"worn ({eq.Status}, Ring0 {worn}), the placeholder gem runs its trigger program: [{string.Join(", ", gems)}]");
        Check(_triggered.All(t => t.Gem.Socket.Gem != ghostItem), "the placeholder gem's program does nothing a GemTriggeredEvent reports");
        var back = await Gems.ReturnSocketedGems(p);
        await Steps(2);
        Check(back.Count == 1 && back[0].Returned && back[0].Gem == ghostItem, $"ReturnSocketedGems takes the placeholder gem back: {string.Join(" | ", back)}");

        // control: the registered kill gem in the same worn ring runs its program
        var reaper = await GiveOne(p, _reaper.Item, new Gear(3));
        if (reaper is null) { Check(false, "the kill gem for the control"); return; }
        var s2 = await Gems.Socket(p, ring.Ref, reaper.Ref, forge, new GemCraftOptions { Cost = GemCost.Free });
        await Steps(10);
        Check(s2.Ok, $"Gems.Socket into a worn ring: {s2}");
        var control = Gems.Equipped(p).FirstOrDefault(g => g.Socket.Gem == _reaper.Item);
        Check(control is { Slot: GemImpactSlot.Ring1 or GemImpactSlot.Ring2, Impact.IsNone: false },
            $"control: the kill gem in the same worn ring runs its program: {control}");
        Check(Spawn.WorldRunning, "the server world runs on");
    }
}
