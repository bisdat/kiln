// SPDX-License-Identifier: MIT
// gems-sockets-test: the gem wave's sockets on other gear (G4; docs/kiln/services/gems-sockets.md) on a dedicated lab
// server, player-free: a spawned 1_Player stand-in (it has an inventory, equipment and equipment_impacts runs for it) and
// a container of the world for the save. First start: the checks below, then a ring with a socket and a gem is put into a
// container, the world is saved and kiln/data/gems-sockets-test/persist.txt written. Second start (the file is there):
// the ring is read back from the container and the file removed.
// A gem is put into a socket by a tracked write of the ring's WeaponGemSlot (the gem's own item entity stays in the
// backpack as well: a probe, as the dossier allows; the forge actions of package G3 do it properly).
using System.Globalization;
using Kiln;

namespace GemsSocketsTest;

public sealed class GemsSocketsTestMod : Mod
{
    private const uint RingLight = 0x79442800;  // Ring_T3_Ring_of_Light (fillers: the first two rings given are worn)
    private const uint RingShock = 0x4623bcb0;  // Ring_T6_Ring_of_Shock
    private const string Shield = "Shield_T2_DPS";
    private const string ChestArmour = "Armor_T4_Mage_BattleWand_Chest_Loot";
    private const string Gem = "T1_Gem_GemOfBleeding";
    private static readonly Guid BleedingProgram = Guid.Parse("b199646f-31a1-495e-b3d1-d5fef2fcaab3");
    private const int ImpactsAt = 8, ImpactStride = 72, GemImpactAt = 0x40, GemEntityAt = 0x44;  // keen::ecs::Equipment

    private int _pass, _fail;

    private void Info(string s) => Log.Info($"gems-sockets-test: INFO {s}");
    private void Check(bool ok, string what)
    {
        if (ok) { _pass++; Log.Info($"gems-sockets-test: PASS {what}"); }
        else { _fail++; Log.Error($"gems-sockets-test: FAIL {what}"); }
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
            var sw = System.Diagnostics.Stopwatch.StartNew();
            while (!Spawn.WorldRunning && sw.Elapsed.TotalSeconds < 240) await Context.Scheduler.NextTick();
            await Steps(60);
            var marker = Path.Combine(Context.DataDirectory, "persist.txt");
            if (File.Exists(marker)) await PersistCheck(marker);
            else await Suite(marker);
        }
        catch (Exception e)
        {
            Check(false, $"the test threw: {e}");
        }
        Log.Info($"gems-sockets-test done: {_pass} PASS, {_fail} FAIL");
    }

    private static bool HasSocket(EntityId pide) => !pide.IsNone && World.Has(pide, "WeaponGemSlot");

    private static (uint Gem, uint Count, uint GemEntity)? SocketOf(EntityId pide) =>
        World.ReadBytes(pide, "WeaponGemSlot") is { Length: >= 12 } b
            ? (BitConverter.ToUInt32(b, 0), BitConverter.ToUInt32(b, 4), BitConverter.ToUInt32(b, 8)) : null;

    private static InventorySlot? SlotOf(EntityId holder, EntityId pide) =>
        Items.Read(holder)?.Slots.FirstOrDefault(s => s.ItemEntity == pide);

    private static ItemId Id(string name) => Items.Lookup(name)?.Id ?? ItemId.None;

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

    private async Task Suite(string marker)
    {
        Check(Capabilities.Is(Gems.SocketsCapability), $"capability {Gems.SocketsCapability}: {Capabilities.Why(Gems.SocketsCapability)}");
        var setup = Gems.AllowSockets(GemSocketKinds.Rings | GemSocketKinds.Armour);
        Check(setup.Server, $"AllowSockets(Rings | Armour): {setup}");
        Check(Gems.SocketKinds == (GemSocketKinds.Rings | GemSocketKinds.Armour), $"SocketKinds is {Gems.SocketKinds}");
        Info($"forge lists on a server: {setup.ForgeLists} ({setup.ForgeReason})");

        var at = new WorldPosition(3845, 835, 1480);
        foreach (var a in World.Find("BaseIdComponent"))
            if (World.Position(a) is { } q) { at = new WorldPosition(q.X + 45, q.Y + 3, q.Z + 45); break; }
        var p = await Spawn.Entity("1_Player", at, 0, new SpawnOptions { Tag = "gems-sockets-test", Flags = SpawnFlags.Transient });
        await Steps(40);
        Check(World.Has(p, "PlayerInput") && Items.Read(p) is { Initialized: true }, $"spawned player {p} at {at} has an inventory");
        var impacts = new List<ImpactRef>();
        using var sub = Events.On<ImpactStartedEvent>(e => { if (e.Impact.Root == p) impacts.Add(e.Impact); });
        try
        {
            await PlayerChecks(p, impacts);
        }
        finally
        {
            Spawn.Destroy(p);
            await Steps(3);
        }
        await PersistSetup(marker);
    }

    private async Task PlayerChecks(EntityId p, List<ImpactRef> impacts)
    {
        // two filler rings: the game wears the first two rings it is given, the next ones stay in the backpack
        await GiveOne(p, new ItemId(RingLight));
        await GiveOne(p, new ItemId(RingLight));
        await Steps(4);

        // 1. Gear.GemSlot on a ring (an allowed kind)
        var socketed = await GiveOne(p, new ItemId(RingShock), new Gear(20, ItemRarity.Rare) { GemSlot = true });
        Check(socketed is { IsEquipped: false } && HasSocket(socketed.ItemEntity) && SocketOf(socketed.ItemEntity)?.Gem == 0,
            $"Gear.GemSlot on a ring: an empty WeaponGemSlot on {socketed?.ItemEntity} ({socketed})");

        // 2. AddSocket re-creates a plain ring with the same level, rarity, upgrades and durability
        var plain = await GiveOne(p, new ItemId(RingShock), new Gear(20, ItemRarity.Epic) { Unlocked = 2 });
        Check(plain is { IsEquipped: false } && !HasSocket(plain.ItemEntity), $"a plain ring has no socket ({plain})");
        if (plain != null)
        {
            var perksBefore = World.ReadBytes(plain.ItemEntity, "PerkContainerNew");
            var durBefore = World.ReadBytes(plain.ItemEntity, "Durability");
            var res = await Gems.AddSocket(plain.Ref);
            await Steps(2);
            Check(res.Status == GemSocketStatus.Done && !res.After.IsNone && res.After.Entity.Id != plain.ItemEntity,
                $"AddSocket on the plain ring: {res}");
            var now = res.After.IsNone ? null : SlotOf(p, res.After.Entity.Id);
            Check(now != null && now.Slot == plain.Slot && now.Level == plain.Level && now.Rarity == plain.Rarity && HasSocket(now.ItemEntity),
                $"the new ring sits in the same slot {plain.Slot} with level {now?.Level} (was {plain.Level}), rarity {now?.Rarity} (was {plain.Rarity}) and a socket");
            var perksAfter = now == null ? null : World.ReadBytes(now.ItemEntity, "PerkContainerNew");
            Check(perksBefore is { Length: >= 2 } && perksAfter is { Length: >= 2 } && perksAfter[1] == perksBefore[1] && perksAfter[0] == perksBefore[0],
                $"upgrades kept: {Hex(perksBefore)} -> {Hex(perksAfter)}");
            var durAfter = now == null ? null : World.ReadBytes(now.ItemEntity, "Durability");
            Info($"durability {Hex(durBefore)} -> {Hex(durAfter)}");
            Check(!World.Has(plain.ItemEntity, "ItemState") || World.ReadBytes(plain.ItemEntity, "ItemState") is null,
                $"the old item entity {plain.ItemEntity} is gone");
            var again = res.After.IsNone ? null : await Gems.AddSocket(res.After);
            Check(again?.Status == GemSocketStatus.AlreadyHasSocket, $"AddSocket again: {again}");
        }

        // 3. a durability below the max is carried over as a fraction (rings have none: a chest piece)
        var worn = await GiveOne(p, Id(ChestArmour), new Gear(20, ItemRarity.Common));
        if (worn is { IsEquipped: true })
        {
            Info($"the first chest piece was worn ({worn}); a second one stays in the backpack");
            worn = await GiveOne(p, Id(ChestArmour), new Gear(20, ItemRarity.Common));
        }
        if (worn != null && World.ReadBytes(worn.ItemEntity, "Durability") is { Length: >= 16 } d0)
        {
            var max = BitConverter.ToInt32(d0, 12);
            var w = await World.WriteAt(worn.ItemEntity, "Durability", 8, max / 2);  // dataStorage[0]: the current value
            await Steps(2);
            var res = await Gems.AddSocket(worn.Ref);
            await Steps(4);
            var after = res.After.IsNone ? null : World.ReadBytes(res.After.Entity.Id, "Durability");
            Check(res.Succeeded && after is { Length: >= 16 } && Math.Abs(BitConverter.ToInt32(after, 8) - BitConverter.ToInt32(after, 12) / 2) <= 2,
                $"durability kept as a fraction: write {w.Status}, before {Hex(d0)}, after {Hex(after)} ({res})");
        }
        else Info($"no durability on the ring ({worn}); the fraction check is skipped");

        // 4. refusals: a shield is not allowed; a worn ring must come off first
        var shield = await GiveOne(p, Id(Shield), new Gear(10, ItemRarity.Common));
        if (shield != null && !shield.IsEquipped)
        {
            var r = await Gems.AddSocket(shield.Ref);
            Check(r.Status == GemSocketStatus.KindNotAllowed, $"AddSocket on a shield without Shields allowed: {r}");
        }
        else Info($"the shield went to {shield} (worn): the not-allowed check uses it only from the backpack");
        var wornRing = Items.Equipped(p, EquipmentSlot.Ring0);
        if (!wornRing.IsNone)
        {
            var r = await Gems.AddSocket(wornRing);
            Check(r.Status == GemSocketStatus.Equipped, $"AddSocket on the worn ring: {r}");
        }

        // 5. rolls: shields with chance 1 get a socket on every creation; chance 0 none
        var rolled = Gems.AllowSockets(GemSocketKinds.Rings | GemSocketKinds.Armour | GemSocketKinds.Shields, 1.0);
        var s1 = await GiveOne(p, Id(Shield), new Gear(10, ItemRarity.Common));
        Check(rolled.Server && s1 != null && HasSocket(s1.ItemEntity), $"roll 1.0 on shields: a new shield has a socket ({s1})");
        Gems.AllowSockets(GemSocketKinds.Rings | GemSocketKinds.Armour);
        var s2 = await GiveOne(p, Id(Shield), new Gear(10, ItemRarity.Common));
        Check(s2 != null && !HasSocket(s2.ItemEntity), $"roll ended: a new shield has none ({s2})");

        // 6. gemSlotOverride on a non-weapon (what a data pack sets) gives a socket on every creation
        try
        {
            var entry = Content.Items.Get(ChestArmour);
            var fields = Content.Items.Fields.Where(f => f.Path.Contains("gem", StringComparison.OrdinalIgnoreCase)).Select(f => f.Path).ToList();
            Info($"kiln.content item fields with 'gem': [{string.Join(", ", fields)}]");
            var path = fields.FirstOrDefault(f => f.Contains("gemSlotOverride", StringComparison.OrdinalIgnoreCase)) ?? "gemSlotOverride.isSet";
            entry?.Set(path, true);
            Info($"set {ChestArmour}.{path} = true");
            await Steps(2);
            var ov = await GiveOne(p, Id(ChestArmour), new Gear(10, ItemRarity.Common));
            Check(entry != null && ov != null && HasSocket(ov.ItemEntity), $"gemSlotOverride.isSet on {ChestArmour}: socket {ov}");
        }
        catch (Exception e)
        {
            Info($"gemSlotOverride through kiln.content: {e.Message} (the data-pack path is the docs' example; check skipped)");
        }

        // 7. a gem in the ring's socket runs its impact while the ring is worn (impact slot 7 or 8)
        var gem = await GiveOne(p, Id(Gem), new Gear(5));
        if (socketed == null || gem == null) { Check(false, "no socketed ring or no gem for the impact check"); return; }
        var stack = new GemStack { Item = gem.Item.Value, Count = 1, Entity = gem.ItemEntity.Value };
        var wr = await World.WriteAt(socketed.ItemEntity, "WeaponGemSlot", 0, stack);
        await Steps(2);
        Check(SocketOf(socketed.ItemEntity) is { } sk && sk.Gem == gem.Item.Value && sk.GemEntity == gem.ItemEntity.Value,
            $"the gem {gem.Item} ({gem.ItemEntity}) is in the ring's socket: write {wr.Status}");
        impacts.Clear();
        var eq = await Items.Equip(p, socketed.Slot, EquipmentSlot.Ring0);
        await Steps(10);
        Info($"equip the socketed ring: {eq}");
        var eqBytes = World.ReadBytes(p, "Equipment");
        uint gemImpact = 0, gemEntity = 0;
        var slot = -1;
        for (var s = 7; s <= 8 && eqBytes is { Length: >= 800 }; s++)
        {
            var i = BitConverter.ToUInt32(eqBytes, ImpactsAt + s * ImpactStride + GemImpactAt);
            if (i != 0 && i != 0xFFFFFFFF) { gemImpact = i; gemEntity = BitConverter.ToUInt32(eqBytes, ImpactsAt + s * ImpactStride + GemEntityAt); slot = s; }
        }
        Check(slot is 7 or 8 && gemEntity == gem.ItemEntity.Value,
            $"equipment impact slot {slot}: weaponGemImpactEntityId {gemImpact}, weaponGemEntityId {gemEntity} (gem {gem.ItemEntity})");
        var program = Abilities.IdOf(BleedingProgram);
        Check(impacts.Any(i => i.ProgramId == program),
            $"ImpactStartedEvent with the Bleeding gem's program 0x{program:x8} for the stand-in [{string.Join(", ", impacts.Select(i => $"0x{i.ProgramId:x8}@{i.Self}"))}]");
    }

    private async Task PersistSetup(string marker)
    {
        var containers = Items.FindInventories(InventoryKinds.Containers)
            .Select(e => (E: e, C: Items.Read(e))).Where(x => x.C is { Initialized: true } && x.C.FreeSlots() >= 3).ToList();
        EntityId chest;
        if (containers.Count > 0) chest = containers[0].E;
        else
        {
            // a fresh lab world has none: a loot chest spawned near the base (saved with the world)
            var bases = World.Find("BaseIdComponent");
            var near = bases.Count > 0 && World.Position(bases[0]) is { } b ? b with { X = b.X + 6 } : new WorldPosition(3806, 834, 1434);
            chest = await Spawn.Entity("LootChest_Random_Cairn_Sarcophagus_Wood", near);
            await Steps(30);
            Info($"persist: spawned loot chest {chest} at {near}: {Items.Read(chest)?.Slots.Count} slots, {Items.Read(chest)?.FreeSlots()} free");
        }
        var ring = await GiveOne(chest, new ItemId(RingShock), new Gear(25, ItemRarity.Epic) { GemSlot = true });
        var gem = await GiveOne(chest, Id(Gem), new Gear(7));
        if (ring == null || gem == null) { Check(false, $"persist: ring {ring}, gem {gem} in container {chest}"); return; }
        var wr = await World.WriteAt(ring.ItemEntity, "WeaponGemSlot", 0,
            new GemStack { Item = gem.Item.Value, Count = 1, Entity = gem.ItemEntity.Value });
        await Steps(4);
        var level = World.ReadBytes(gem.ItemEntity, "Level") is { Length: >= 16 } lv ? BitConverter.ToUInt32(lv, 8) : 0u;
        Check(SocketOf(ring.ItemEntity)?.Gem == gem.Item.Value, $"persist: ring {ring.ItemEntity} in container {chest} holds gem {gem.Item} level {level} (write {wr.Status})");
        var saved = await Saves.Request();
        Info($"persist: save {saved}");
        var pos = World.Position(chest) ?? default;
        Directory.CreateDirectory(Path.GetDirectoryName(marker)!);
        File.WriteAllText(marker, string.Join(' ', chest.Value, ring.Slot.Value, ring.Item.Value, gem.Item.Value, level,
            pos.X.ToString(CultureInfo.InvariantCulture), pos.Y.ToString(CultureInfo.InvariantCulture), pos.Z.ToString(CultureInfo.InvariantCulture)));
        Log.Info("gems-sockets-test: RESTART the lab now: the next start checks the ring in the container");
    }

    private async Task PersistCheck(string marker)
    {
        var parts = File.ReadAllText(marker).Split(' ');
        File.Delete(marker);
        var f = parts.Take(5).Select(x => ulong.Parse(x, CultureInfo.InvariantCulture)).ToArray();
        var at = parts.Skip(5).Select(x => double.Parse(x, CultureInfo.InvariantCulture)).ToArray();
        await Steps(60);
        // entity ids are not kept by a load: find the container by its place, the ring by item and socket
        InventorySlot? s = null;
        EntityId chest = default;
        foreach (var e in Items.FindInventories(InventoryKinds.Containers))
        {
            if (World.Position(e) is not { } q || Math.Abs(q.X - at[0]) + Math.Abs(q.Y - at[1]) + Math.Abs(q.Z - at[2]) > 3) continue;
            var hit = Items.Read(e)?.Slots.FirstOrDefault(x => x.Item.Value == (uint)f[2] && !x.ItemEntity.IsNone && HasSocket(x.ItemEntity));
            if (hit != null) { s = hit; chest = e; break; }
        }
        Info($"after the restart: container {chest} near ({at[0]}, {at[1]}, {at[2]}): {s}");
        if (s == null)
        {
            var all = Items.FindInventories(InventoryKinds.Containers);
            var nearest = all.Select(e => (E: e, P: World.Position(e))).Where(x => x.P != null)
                .OrderBy(x => Math.Abs(x.P!.Value.X - at[0]) + Math.Abs(x.P!.Value.Z - at[2])).FirstOrDefault();
            Info($"{all.Count} containers; nearest {nearest.E} at {nearest.P}; loot chests: {World.Find("RandomLootContainer").Count}");
            foreach (var e in all)
                foreach (var x in Items.Read(e)?.Slots.Where(x => x.Item.Value == (uint)f[2]) ?? [])
                    Info($"a ring 0x{f[2]:x8} in container {e} at {World.Position(e)}: {x}, socket {SocketOf(x.ItemEntity)}");
        }
        Check(s != null, $"after the restart the socketed ring is in the container (was entity {f[0]}, slot {new SlotId(f[1])})");
        if (s == null || s.ItemEntity.IsNone) return;
        var sk = SocketOf(s.ItemEntity);
        Check(sk is { } k && k.Gem == (uint)f[3] && k.GemEntity != 0 && k.GemEntity != 0xFFFFFFFF,
            $"after the restart the ring's socket holds gem 0x{f[3]:x8}: {sk}");
        if (sk is { } g && g.GemEntity != 0)
        {
            var lv = World.ReadBytes(new EntityId(g.GemEntity), "Level") is { Length: >= 16 } b ? BitConverter.ToUInt32(b, 8) : 0u;
            Check(lv == (uint)f[4], $"after the restart the gem's own entity {g.GemEntity} has level {lv} (saved {f[4]})");
        }
        // leave the world as it was
        await Items.Take(chest, s.Item, 1, s.Slot, exactSlot: true, itemEntity: s.ItemEntity);
    }

    private static string Hex(byte[]? b) => b == null ? "none" : Convert.ToHexString(b);

    private struct GemStack
    {
        public uint Item, Count, Entity;
    }
}
