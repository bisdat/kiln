// SPDX-License-Identifier: MIT
// Gem wave G1 lab test, player-free (docs/kiln/services/gems.md, the dossier's "Checks not done"). On a spawned
// player-template entity: a dagger with an empty socket and a loose Gem of Puncturing are given, the gem is written into
// the dagger's WeaponGemSlot (the probe's socket: the gem entity stays in its inventory slot as well, which the dossier
// allows for the probe; the forge's own Slot action is gem wave G3's), the dagger goes in hand. Then: Gems.Equipped,
// Gems.Socket, ImpactStartedEvent with the gem program, GemTriggeredEvent on a pierce hit on a Vukah, GemSocketChangedEvent,
// and a kiln.gamedata change of the gem's damage config that the running gem impact keeps until a hotbar switch.
using Kiln;

namespace GemsTest;

public sealed class GemsTestMod : Mod
{
    private int _pass, _fail;
    private static readonly Guid HitProgram = Guid.Parse("4b8e6f21-0c3a-4d5e-9f17-2a6b8c0d1e93");
    private const string GemName = "T1_Gem_GemOfRepetition", Dagger = "Weapon_T1_2H_Daggers_02_Parry";
    private const uint DamageConfig = 0x5e34f09b;
    private readonly List<EntityId> _spawned = new();
    private readonly List<ImpactStartedEvent> _started = new();
    private readonly List<GemTriggeredEvent> _triggered = new();
    private readonly List<GemSocketChangedEvent> _sockets = new();
    private uint _gemProgramId;
    private bool _landed;

    private void Info(string s) => Log.Info($"gems-test: INFO {s}");

    private void Check(bool ok, string what)
    {
        if (ok) { _pass++; Log.Info($"gems-test: PASS {what}"); }
        else { _fail++; Log.Error($"gems-test: FAIL {what}"); }
    }

    public override void OnLoad()
    {
        if (!Client.IsDedicatedServer) return;
        Directory.CreateDirectory(Path.GetFullPath(Path.Combine(Context.DataDirectory, "..", "skills")));
        File.WriteAllText(Path.GetFullPath(Path.Combine(Context.DataDirectory, "..", "skills", "lab-ops")), "");  // GameTree.GameRequest (hotbar)
        Events.On<ImpactSignalEvent>(e => { if (e.Tag == 0x701) _landed = true; });
        Events.On<ImpactStartedEvent>(e => { if (_gemProgramId != 0 && e.Impact.ProgramId == _gemProgramId) _started.Add(e); });
        Events.On<GemTriggeredEvent>(e =>
        {
            _triggered.Add(e);
            Info($"event: {e}");
        });
        Events.On<GemSocketChangedEvent>(e =>
        {
            _sockets.Add(e);
            Info($"event: {e}");
        });
        _ = Run();
    }

    private async Task Steps(int n)
    {
        for (var i = 0; i < n; i++) await Context.Scheduler.NextTick();
    }

    private static uint Fnv(string s)
    {
        var h = 0x811c9dc5u;
        foreach (var c in System.Text.Encoding.UTF8.GetBytes(s)) h = (h ^ c) * 0x1000193u;
        return h;
    }

    private static string Cfg(uint id) => $"0x{id:x8}";

    private async Task Run()
    {
        try
        {
            var sw = System.Diagnostics.Stopwatch.StartNew();
            while (!Spawn.WorldRunning && sw.Elapsed.TotalSeconds < 240) await Context.Scheduler.NextTick();
            await Steps(60);
            await Suite();
        }
        catch (Exception e)
        {
            Check(false, $"exception: {e}");
        }
        foreach (var e in _spawned) Spawn.Destroy(e);
        Log.Info($"gems-test done: {_pass} PASS, {_fail} FAIL");
    }

    // what kiln.items and kiln.gamedata say about the gems, raw (the catalog's inputs)
    private void Diagnostics()
    {
        foreach (var i in Items.All.Where(i => i.Name.Contains("_Gem_", StringComparison.Ordinal)))
            Info($"item {i.Name}: category {i.Category}, flags 0x{i.Flags:x}, levels {i.LevelMin}-{i.LevelMax}, {i.Id}");
        var e = Content.ItemData.Get("T1_Gem_GemOfBleeding");
        string Cut(string? x, int n = 3000) => x is null ? "null" : x.Length > n ? x[..n] + "..." : x;
        foreach (var path in new[] { "equipment.program", "impactValues", "impactValues.scaled", "impactValues.simple", "impactValues.0x5e34f09b", "impactValues.BuffType" })
        {
            try { Info($"Bleeding GetJson({path}) = {Cut(e?.GetJson(path)?.ToJsonString())}"); }
            catch (Exception x) { Info($"Bleeding GetJson({path}) threw {x.GetType().Name}: {x.Message}"); }
        }
        var vocab = Content.ConfigVocabulary();
        Info($"vocabulary: {vocab.Length} chars, starts {Cut(vocab, 400)}");
        foreach (var id in new[] { "0x5e34f09b", "0x5adb2644", "0x3ff81f2b" })
        {
            var at = vocab.IndexOf(id, StringComparison.OrdinalIgnoreCase);
            Info($"vocabulary at {id}: {(at < 0 ? "absent" : Cut(vocab.Substring(Math.Max(0, at - 20), Math.Min(300, vocab.Length - Math.Max(0, at - 20)))))}");
        }
    }

    private void CatalogChecks()
    {
        Diagnostics();
        Check(Capabilities.Is("gems.read"), $"gems.read is available ({Capabilities.Why("gems.read")})");
        Check(Capabilities.Is("gems.triggers"), $"gems.triggers is available ({Capabilities.Why("gems.triggers")})");
        var all = Gems.Catalog;
        Info($"catalog: {all.Count} gems: {string.Join(", ", all.Select(g => $"{g.Name} ({g.Configs.Count} configs, program {g.Program})"))}");
        var game = all.Where(g => !g.FromMod).ToList();
        var registry = Items.All.Count(i => i.Category == ItemCategory.WeaponGem && !i.IsBlueprint);
        // the client's data has 29 gems; this server's item registry has 28 (no T5_Gem_GemOfNecromancy): lab 2026-11-16
        Check(game.Count == registry && registry >= 28, $"the catalog lists every gem of the item registry ({game.Count} of {registry})");
        Check(game.All(g => g.Program != Guid.Empty), $"each gem has its program ({game.Count(g => g.Program == Guid.Empty)} without)");
        var necroCount = game.Count(g => g.Name.Contains("Necromancy", StringComparison.Ordinal));
        Check(game.Select(g => g.Program).Distinct().Count() == game.Count - Math.Max(0, necroCount - 1),
            $"one program per gem, the Necromancy gems sharing one ({game.Select(g => g.Program).Distinct().Count()} programs)");
        Check(game.All(g => g.Configs.Count > 0), $"each gem has configs ({string.Join(", ", game.Where(g => g.Configs.Count == 0).Select(g => g.Name))})");
        var bleeding = all.FirstOrDefault(g => g.Name == "T1_Gem_GemOfBleeding");
        Info($"Bleeding: {(bleeding is null ? "missing" : string.Join("; ", bleeding.Configs.Values.Select(c => $"{c}")))}");
        Check(bleeding is { Program: var p } && p == Guid.Parse("b199646f-31a1-495e-b3d1-d5fef2fcaab3"), "Bleeding's program is b199646f-...");
        Check(bleeding?.Configs.GetValueOrDefault(DamageConfig) is { Value: 8, ScaleFactor: { } s } && Math.Abs(s - 2.6) < 1e-4,
            $"Bleeding's 0x5e34f09b is 8, scale 2.6 ({bleeding?.Configs.GetValueOrDefault(DamageConfig)})");
        Check(bleeding?.Configs.GetValueOrDefault(0x5adb2644u)?.Text is { } t && t.StartsWith("e9fad497", StringComparison.OrdinalIgnoreCase),
            $"Bleeding's BuffType 0x5adb2644 is the bleed buff ({bleeding?.Configs.GetValueOrDefault(0x5adb2644u)?.Text})");
        Check(bleeding?.Configs.GetValueOrDefault(0x3ff81f2bu) is { Value: 6, ScaleFactor: { } d } && Math.Abs(d - 0.5) < 1e-4,
            $"Bleeding's 0x3ff81f2b (a time) is 6 s, scale 0.5 ({bleeding?.Configs.GetValueOrDefault(0x3ff81f2bu)})");
        var necro = all.Where(g => g.Name.Contains("Necromancy", StringComparison.Ordinal)).ToList();
        Check(necro.Count >= 2 && necro.Select(g => g.Program).Distinct().Count() == 1, $"the Necromancy gems ({necro.Count}) share one program");
        Check(Gems.Get(Items.Lookup(Dagger)!.Id) is null, "a dagger is not a gem");
    }

    private async Task Suite()
    {
        CatalogChecks();
        var gemInfo = Gems.Catalog.First(g => g.Name == GemName);
        _gemProgramId = Abilities.IdOf(gemInfo.Program);
        var baseDamage = gemInfo.Configs.GetValueOrDefault(DamageConfig) ?? new GemConfigValue(DamageConfig, null, "?", 0, null);
        Info($"{GemName}: program {gemInfo.Program} (0x{_gemProgramId:x8}), {DamageConfig:x8} = {baseDamage}");

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
            return;
        }
        var at = new WorldPosition(a.X + 40, a.Y + 0.5, a.Z + 40);
        var wearer = await Spawn.Entity("1_Player", at, 0, new SpawnOptions { Tag = "gemstest", Flags = SpawnFlags.Transient });
        _spawned.Add(wearer);
        await Steps(20);
        var sel = World.ReadBytes(wearer, "SlotSelection");
        if (sel is null || World.ComponentSize(wearer, "Equipment") == 0)
        {
            Check(false, "the player-template entity has no SlotSelection or Equipment");
            return;
        }
        var bars = new[] { BitConverter.ToUInt32(sel, 0xa0), BitConverter.ToUInt32(sel, 0xa4) };

        var giveDagger = await Items.Give(wearer, Dagger, 1, new Gear(null, ItemRarity.Common) { GemSlot = true });
        Info($"give a dagger with a socket: {giveDagger}");
        var giveGem = await Items.Give(wearer, GemName, 1, new Gear(5));
        if (!giveGem.Succeeded) giveGem = await Items.Give(wearer, GemName, 1);
        Info($"give {GemName}: {giveGem}");
        await Steps(20);
        var inv = Items.Read(wearer);
        var dagger = inv?.Slots.FirstOrDefault(x => !x.IsEmpty && x.Item == Items.Lookup(Dagger)!.Id);
        var gem = inv?.Slots.FirstOrDefault(x => !x.IsEmpty && x.Item == gemInfo.Item);
        if (dagger is null || gem is null || dagger.ItemEntity.IsNone || gem.ItemEntity.IsNone)
        {
            Check(false, $"dagger {dagger?.Slot} and gem {gem?.Slot} in the inventory");
            return;
        }
        Info($"dagger {dagger.Ref} at {dagger.Slot} (bars {bars[0]}, {bars[1]}), gem {gem.Ref} level {gem.Level} at {gem.Slot}");
        Check(World.Has(dagger.ItemEntity, "WeaponGemSlot"), "the dagger has a socket (Gear.GemSlot)");
        Check(Gems.Socket(dagger.Ref) is null, "Gems.Socket: the new socket is empty");
        Check(Gems.Socket(gem.Ref) is null, "Gems.Socket: a loose gem holds no gem");

        // in hand: the hotbar slot it landed in
        var onBar = Array.IndexOf(bars, dagger.Slot.Inventory.Value);
        if (onBar < 0)
        {
            Check(false, "the dagger landed on no action bar");
            return;
        }
        var index = (uint)(onBar * 8 + dagger.Slot.Index);
        GameTree.GameRequest(wearer, 5, index);
        await Steps(10);
        Check(Items.Held(wearer) == dagger.Ref, $"the dagger is in hand (hotbar slot {index}): {Items.Held(wearer)}");
        Check(Gems.Equipped(wearer).Count == 0, $"Gems.Equipped: an empty socket runs no gem ({Gems.Equipped(wearer).Count})");

        // the probe's socket
        var w1 = await World.WriteField(dagger.ItemEntity, "WeaponGemSlot", "weaponGem.id.value", gemInfo.Item.Value);
        var w2 = await World.WriteField(dagger.ItemEntity, "WeaponGemSlot", "weaponGem.data.count", 1u);
        var w3 = await World.WriteField(dagger.ItemEntity, "WeaponGemSlot", "weaponGem.data.pide.id", gem.ItemEntity.Value);
        Info($"socket written: {w1.Status} {w2.Status} {w3.Status} {w1.Reason}{w2.Reason}{w3.Reason}");
        Check(w1.Ok && w2.Ok && w3.Ok, "the probe's socket is written (WeaponGemSlot)");
        await Steps(20);

        var socket = Gems.Socket(dagger.Ref);
        Info($"Gems.Socket(dagger) = {socket}");
        Check(socket is { } s1 && s1.Gem == gemInfo.Item && s1.Level == gem.Level && s1.Carrier == dagger.Ref,
            $"Gems.Socket: the dagger holds {GemName} at the gem's level {gem.Level}");
        var eq = Gems.Equipped(wearer);
        Info($"Gems.Equipped = [{string.Join(", ", eq)}]");
        var held = eq.FirstOrDefault(g => g.Slot == GemImpactSlot.Held);
        Check(held is { Impact.IsNone: false } && held.Socket.Gem == gemInfo.Item && held.Socket.Carrier == dagger.Ref,
            "Gems.Equipped: the held dagger's gem runs (slot Held, a gem impact entity, carrier and gem)");
        Check(eq.Count == 1, $"only the held item's gem ({eq.Count})");
        if (held is null) return;
        Check(_started.Any(i => i.Impact.Self == held.Impact), $"ImpactStartedEvent: the gem program started on that impact entity ({_started.Count} started)");
        if (!_started.Any(i => i.Impact.Self == held.Impact)) return;
        var inst = _started.First(i => i.Impact.Self == held.Impact).Instance;
        var v1 = inst.Find(DamageConfig) is { } var1 ? inst.Read(var1) : null;
        var damage1 = v1 is { Length: 4 } ? BitConverter.ToInt32(v1) : int.MinValue;
        Info($"the running gem impact's {DamageConfig:x8} = {damage1} (base {baseDamage.Value}, scale {baseDamage.ScaleFactor}, level {gem.Level})");
        Check(damage1 != int.MinValue, "the gem impact has the damage config");
        Check(_sockets.Any(e => e.Carrier == dagger.Ref && e.Change == GemSocketChange.Socketed && e.After?.Gem == gemInfo.Item),
            $"GemSocketChangedEvent: Socketed for the dagger ({_sockets.Count} event(s))");

        // a hit: Puncturing reacts to the wearer's pierce hits on an enemy
        var weapon = Content.ItemData.Get(Dagger)!.Get("weaponCategoryReference").Guid;
        var enemy = await Spawn.Entity("Enemy_Vukah_Melee", new WorldPosition(at.X + 3, at.Y, at.Z), 0, new SpawnOptions { Tag = "gemstest", Flags = SpawnFlags.Transient });
        _spawned.Add(enemy);
        await Steps(10);
        var before = _triggered.Count;
        var tat = World.Position(enemy)!.Value;
        var he = await Abilities.RunProgram(HitProgram, tat, new ImpactOptions
        {
            Root = wearer,
            Configs = new ImpactConfigs
            {
                [Cfg(Fnv("gemstest.Target"))] = enemy, [Cfg(Fnv("gemstest.Weapon"))] = weapon,
                [Cfg(Fnv("gemstest.At"))] = new[] { (float)tat.X, (float)tat.Y, (float)tat.Z }.SelectMany(BitConverter.GetBytes).ToArray(),
            },
            Tag = "gemstest",
        });
        await Steps(16);
        Spawn.Destroy(he);
        Info($"pierce hit landed: {_landed}");
        var mine = _triggered.Skip(before).ToList();
        Check(mine.Any(e => e.Gem.Impact == held.Impact && e.Player == wearer),
            $"GemTriggeredEvent: the gem reacted to the wearer's hit ({mine.Count}: {string.Join(", ", mine.Select(e => e.Native.NativeName + "->" + e.Target))})");
        Check(mine.Any(e => e.Native is ImpactBuffEvent && e.Target == enemy), "its applyBuff names the Vukah as the target");

        // a data-pack change: the running impact keeps its value, the next spawn reads the new one
        var data = Content.ItemData.Get(GemName)!;
        data.SetJson("impactValues.0x5e34f09b", "{\"mul\": 2}");
        await Steps(5);
        var still = inst.Find(DamageConfig) is { } var2 ? inst.Read(var2) : null;
        Check(still is { Length: 4 } && BitConverter.ToInt32(still) == damage1, $"the running gem impact keeps {damage1} after the change");
        await Steps(60);  // the catalog is read again within 2 s
        Check(Gems.Get(gemInfo.Item)?.Configs.GetValueOrDefault(DamageConfig)?.Value == baseDamage.Value * 2,
            $"Gems.Catalog follows the change ({Gems.Get(gemInfo.Item)?.Configs.GetValueOrDefault(DamageConfig)})");
        var other = index == 0 ? 1u : 0u;
        GameTree.GameRequest(wearer, 5, other);
        await Steps(10);
        Check(Gems.Equipped(wearer).Count == 0, "another hotbar slot: the gem impact is gone");
        GameTree.GameRequest(wearer, 5, index);
        await Steps(10);
        var back = Gems.Equipped(wearer).FirstOrDefault(g => g.Slot == GemImpactSlot.Held);
        Check(back is { Impact.IsNone: false } && back.Impact != held.Impact, $"back on the dagger: a new gem impact ({held.Impact} -> {back?.Impact})");
        var inst2 = _started.LastOrDefault(i => back is not null && i.Impact.Self == back.Impact)?.Instance;
        var v2 = inst2?.Find(DamageConfig) is { } var3 ? inst2.Read(var3) : null;
        var damage2 = v2 is { Length: 4 } ? BitConverter.ToInt32(v2) : int.MinValue;
        Info($"after the hotbar switch: {damage1} -> {damage2}");
        Check(damage2 > damage1, $"the new gem impact reads the changed config ({damage1} -> {damage2})");
        data.Reset("impactValues.0x5e34f09b");

        // the probe's socket emptied: the gem never left the inventory, so the poll says Removed
        await World.WriteField(dagger.ItemEntity, "WeaponGemSlot", "weaponGem.data.pide.id", 0u);
        await World.WriteField(dagger.ItemEntity, "WeaponGemSlot", "weaponGem.data.count", 0u);
        await World.WriteField(dagger.ItemEntity, "WeaponGemSlot", "weaponGem.id.value", 0u);
        await Steps(20);
        Check(Gems.Socket(dagger.Ref) is null, "Gems.Socket: empty again");
        Check(Gems.Equipped(wearer).Count == 0, "Gems.Equipped: no gem runs");
        Check(_sockets.Any(e => e.Carrier == dagger.Ref && e.Change is GemSocketChange.Removed or GemSocketChange.Unsocketed && e.After is null),
            $"GemSocketChangedEvent: the socket emptied ({string.Join(", ", _sockets.Select(e => e.Change))})");
    }
}
