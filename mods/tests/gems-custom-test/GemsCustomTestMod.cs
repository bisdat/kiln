// SPDX-License-Identifier: MIT
// New gems in a server world (gem wave G2), player-free. OnLoad registers gems the way a mod does (Gems.Register: one per
// trigger kind, and a data gem with its own configs). When the world runs: the trigger programs are in the game's
// registry (served through kiln.assets), the gems are defined with them, the data gem has its numbers; then each trigger
// program runs on a player-template entity (as equipment_impacts would run it for a socketed gem) while the test makes
// hits through the game's applyDamage (impact/hit.kimp), and the program's kiln.Signal must carry the hit.
// The step from the signal to the mod's handler needs the gem in a socket (G1's Gems.Equipped finds it): logged as INFO.
using Kiln;

namespace GemsCustomTest;

public sealed class GemsCustomTestMod : Mod
{
    private static readonly Guid HitProgram = Guid.Parse("531786a3-5547-4ad0-a433-b878a3c08124");
    private static readonly Guid Bleeding = Guid.Parse("b199646f-31a1-495e-b3d1-d5fef2fcaab3");
    private static readonly (GemTriggerKind Kind, Guid Guid, uint Tag)[] Programs =
    [
        (GemTriggerKind.Hit, Guid.Parse("6b696c6e-6765-6d73-8000-000000000001"), 0x6b470001),
        (GemTriggerKind.Crit, Guid.Parse("6b696c6e-6765-6d73-8000-000000000002"), 0x6b470002),
        (GemTriggerKind.Kill, Guid.Parse("6b696c6e-6765-6d73-8000-000000000003"), 0x6b470003),
        (GemTriggerKind.Backstab, Guid.Parse("6b696c6e-6765-6d73-8000-000000000004"), 0x6b470004),
        (GemTriggerKind.HitTaken, Guid.Parse("6b696c6e-6765-6d73-8000-000000000005"), 0x6b470005),
        (GemTriggerKind.Passive, Guid.Parse("6b696c6e-6765-6d73-8000-000000000006"), 0x6b470006),
    ];

    private int _pass, _fail, _handlerCalls;
    private readonly List<GemTriggerContext> _calls = new();
    private readonly List<EntityId> _spawned = new();
    private readonly List<ImpactSignalEvent> _signals = new();
    private readonly List<GemHandle> _gems = new();
    private GemHandle? _data;
    private bool _landed;
    private WorldPosition _at;

    private void Info(string s) => Log.Info($"gems-custom-test: INFO {s}");
    private void Check(bool ok, string what)
    {
        if (ok) { _pass++; Log.Info($"gems-custom-test: PASS {what}"); }
        else { _fail++; Log.Error($"gems-custom-test: FAIL {what}"); }
    }

    public override void OnLoad()
    {
        if (!Client.IsDedicatedServer) return;
        Directory.CreateDirectory(Path.GetFullPath(Path.Combine(Context.DataDirectory, "..", "skills")));
        File.WriteAllText(Path.GetFullPath(Path.Combine(Context.DataDirectory, "..", "skills", "lab-ops")), "");  // GameTree.GameRequest (hotbar)
        Info($"gems.triggers: {Capabilities.Is("gems.triggers")} ({Capabilities.Why("gems.triggers")})");
        if (!Capabilities.Is("gems.triggers"))
        {
            Check(false, "gems.triggers is off on this server");
            return;
        }
        var triggers = new (string Name, GemTrigger Trigger)[]
        {
            ("hit", GemTrigger.OnHit()), ("crit", GemTrigger.OnHit(crit: true)), ("kill", GemTrigger.OnKill()),
            ("backstab", GemTrigger.OnBackstab()), ("taken", GemTrigger.OnHitTaken()), ("aura", GemTrigger.Passive(TimeSpan.FromSeconds(1))),
        };
        foreach (var (name, trigger) in triggers)
            _gems.Add(Gems.Register(new GemDefinition
            {
                Id = $"gems-custom-test:{name}", BaseOn = "T1_Gem_GemOfBleeding", DisplayName = $"Test gem ({name})",
                Trigger = trigger, OnTrigger = ctx => { _handlerCalls++; _calls.Add(ctx); Info($"handler {ctx.Definition.Id}: {ctx.Trigger} by {ctx.Player.Value} on {ctx.Target.Value}, level {ctx.Level}"); },
            }));
        _data = Gems.Register(new GemDefinition
        {
            Id = "gems-custom-test:data", BaseOn = "T1_Gem_GemOfBleeding", Program = Bleeding, Rarity = ItemRarity.Epic,
            LevelMin = 3, LevelMax = 40, Configs = { ["0x5e34f09b"] = 14, ["0x3ff81f2b"] = TimeSpan.FromSeconds(9) },
        });
        var again = Gems.Register(new GemDefinition { Id = "gems-custom-test:hit", BaseOn = "T1_Gem_GemOfBleeding" });
        Check(_gems.All(g => g.Ok) && _data.Ok, $"Gems.Register took {_gems.Count + 1} gems in OnLoad: {string.Join(", ", _gems.Append(_data))}");
        Check(!again.Ok && again.Reason!.Contains("registered already"), $"a second gem with the same id is refused: {again.Reason}");
        Events.On<ImpactSignalEvent>(e =>
        {
            if (e.Tag == 0x701) _landed = true;
            if (Programs.Any(p => p.Tag == e.Tag)) _signals.Add(e);
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

    private static string Cfg(string name) => $"0x{Fnv(name):x8}";

    private async Task Run()
    {
        try
        {
            var sw = System.Diagnostics.Stopwatch.StartNew();
            while (!Spawn.WorldRunning && sw.Elapsed.TotalSeconds < 240) await Context.Scheduler.NextTick();
            await Steps(60);
            Registry();
            await Signals();
        }
        catch (Exception e)
        {
            Check(false, $"exception: {e}");
        }
        foreach (var e in _spawned) Spawn.Destroy(e);
        Log.Info($"gems-custom-test done: {_pass} PASS, {_fail} FAIL");
    }

    private void Registry()
    {
        foreach (var (kind, guid, _) in Programs)
        {
            var info = Abilities.Programs.Info(Abilities.Programs.IdOf(guid));
            Check(info is { FromKiln: true } && info.Guid == guid,
                $"the {kind} trigger program {guid} is in the game's registry (served by kiln.assets): {info}");
        }
        var defs = NewContent.Definitions();
        foreach (var g in _gems.Append(_data!))
        {
            var d = defs.FirstOrDefault(x => x.Guid == g.Guid);
            Check(d is { State: "registered" } && d.Warnings.Count == 0,
                $"gem {g.Definition.Id} is a registered item of the server world with no warning: {d} {string.Join("; ", d?.Warnings ?? [])}");
            var prog = Content.ItemData.Get(g.Name)?.Get("equipment.program").Guid;
            var want = g.Definition.Trigger is { } t ? Programs.First(p => p.Kind == t.Kind).Guid : g.Definition.Program ?? Guid.Empty;
            Check(prog == want, $"{g.Name}'s equipment.program is {prog} (want {want})");
        }
        var data = Content.Items.Get(_data!.Name);
        Check(data is not null && data.Config("0x5e34f09b").Get().Number == 14,
            $"the data gem's Scaled_Damage_Cut is 14: {data?.Config("0x5e34f09b").Get()}, its base keeps {Content.Items.Get("T1_Gem_GemOfBleeding")?.Config("0x5e34f09b").Get()}");
        Info($"data gem: duration {data?.Config("0x3ff81f2b").Get()}, rarity {data?.Get("rarity")}, levels {data?.Get("itemLevelRange.minLevel")}..{data?.Get("itemLevelRange.maxLevel")}");
        Check(data?.Get("itemLevelRange.minLevel").Integer == 3 && data.Get("itemLevelRange.maxLevel").Integer == 40, "the data gem's level range is 3..40");
    }

    private async Task<EntityId> Enemy()
    {
        var e = await Spawn.Entity("Enemy_Vukah_Melee", new WorldPosition(_at.X + 3, _at.Y, _at.Z), 0, new SpawnOptions { Tag = "gemtest", Flags = SpawnFlags.Transient });
        _spawned.Add(e);
        await Steps(10);
        return e;
    }

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
            Tag = "gemtest",
        });
        await Steps(12);
        Spawn.Destroy(he);
        Info($"hit {attacker.Value} -> {target.Value} ({cut} cut): health {h0?.Current} -> {Combat.GetHealth(target)?.Current}{(_landed ? "" : " (the hit did not land)")}");
    }

    private async Task<EntityId> Trigger(GemTriggerKind kind, EntityId wearer)
    {
        var e = await Abilities.RunProgram(Programs.First(p => p.Kind == kind).Guid, World.Position(wearer)!.Value,
            new ImpactOptions { Root = wearer, Tag = "gemtest" });
        await Steps(4);
        _signals.Clear();
        return e;
    }

    private List<GemHit> Hits(GemTriggerKind kind, EntityId impact)
    {
        var tag = Programs.First(p => p.Kind == kind).Tag;
        return _signals.Where(s => s.Tag == tag && s.Impact.Self == impact).Select(s => GemHit.Of(s.Value)).ToList();
    }

    private async Task Signals()
    {
        var sword = Content.ItemData.Get("Weapon_T1_1H_SwordEpic_01")?.Get("weaponCategoryReference").Guid ?? Guid.Empty;
        Check(sword != Guid.Empty, $"the Bronze Sword's weapon category: {sword}");
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
        _at = new WorldPosition(a.X + 40, a.Y + 0.5, a.Z + 40);
        var wearer = await Spawn.Entity("1_Player", _at, 0, new SpawnOptions { Tag = "gemtest", Flags = SpawnFlags.Transient });
        _spawned.Add(wearer);
        await Steps(20);
        _handlerCalls = 0;

        // on hit: each hit the wearer lands
        var p = await Trigger(GemTriggerKind.Hit, wearer);
        var vukah = await Enemy();
        await Hit(wearer, vukah, sword, 5);
        var hits = Hits(GemTriggerKind.Hit, p);
        Check(hits.Count == 1 && hits[0].Attacker == wearer.Value && hits[0].Target == vukah.Value && hits[0].Health < 0 && hits[0].Enemy && !hits[0].Kill,
            $"on_hit signals the wearer's hit on a Vukah once: {string.Join(" | ", hits)}");
        var crit = _signals.Count(s => s.Tag == 0x6b470002);
        await Hit(vukah, wearer, Guid.Empty, 1);
        Check(Hits(GemTriggerKind.Hit, p).Count == 1, "on_hit ignores a hit the wearer takes");
        Spawn.Destroy(p);
        await Steps(4);

        // on crit: a crit is the game's roll; a plain hit must not signal
        p = await Trigger(GemTriggerKind.Crit, wearer);
        await Hit(wearer, vukah, sword, 5);
        hits = Hits(GemTriggerKind.Crit, p);
        Check(hits.All(h => h.Crit), $"on_crit signals only critical hits: {hits.Count} signal(s) {string.Join(" | ", hits)}");
        Info($"on_crit: {hits.Count} crit(s) in one hit (the game rolls crits; 0 is the usual answer for a player-template entity)");
        Spawn.Destroy(p);
        await Steps(4);

        // on kill: a light hit does not signal, a killing one does
        p = await Trigger(GemTriggerKind.Kill, wearer);
        await Hit(wearer, vukah, sword, 5);
        Check(Hits(GemTriggerKind.Kill, p).Count == 0, "on_kill: a hit that does not kill does not signal");
        await Hit(wearer, vukah, sword, 5000);
        hits = Hits(GemTriggerKind.Kill, p);
        Check(hits.Count == 1 && hits[0].Kill && hits[0].Target == vukah.Value && hits[0].Enemy,
            $"on_kill signals the killing blow: {string.Join(" | ", hits)}");
        Spawn.Destroy(p);
        await Steps(4);

        // on hit taken: the wearer is the target; the payload's attacker is the Vukah
        var vukah2 = await Enemy();
        p = await Trigger(GemTriggerKind.HitTaken, wearer);
        await Hit(vukah2, wearer, Guid.Empty, 1);
        hits = Hits(GemTriggerKind.HitTaken, p);
        Check(hits.Count == 1 && hits[0].Target == wearer.Value && hits[0].Attacker == vukah2.Value && hits[0].Enemy,
            $"on_hit_taken signals a Vukah's hit on the wearer: {string.Join(" | ", hits)}");
        await Hit(wearer, vukah2, sword, 5);
        Check(Hits(GemTriggerKind.HitTaken, p).Count == 1, "on_hit_taken ignores the wearer's own hits");
        Spawn.Destroy(p);
        await Steps(4);

        // backstab: a hit from the front is no backstab
        p = await Trigger(GemTriggerKind.Backstab, wearer);
        await Hit(wearer, vukah2, sword, 5);
        hits = Hits(GemTriggerKind.Backstab, p);
        Check(hits.All(h => h.Backstab), $"on_backstab signals only backstabs: {hits.Count} signal(s)");
        Spawn.Destroy(p);
        await Steps(4);

        // passive: every 250 ms, the wearer as the value
        p = await Trigger(GemTriggerKind.Passive, wearer);
        var t0 = DateTime.UtcNow;
        while ((DateTime.UtcNow - t0).TotalSeconds < 1.6) await Steps(1);
        var aura = _signals.Where(s => s.Tag == 0x6b470006 && s.Impact.Self == p).ToList();
        Check(aura.Count is >= 4 and <= 8 && aura.All(s => s.AsEntity == wearer),
            $"passive signals about every 250 ms with the wearer: {aura.Count} in 1.6 s");
        Spawn.Destroy(p);
        await Steps(4);

        Check(_handlerCalls == 0, $"no handler ran for programs no socketed gem runs: {_handlerCalls} call(s)");
        _ = crit;
        await Socketed(wearer, sword);
    }

    // The whole chain, where Gems.Equipped works (with gem wave G1): a dagger with a socket in hand, the kill gem written into
    // the socket (G1's probe: the gem entity stays in its inventory slot as well; the forge's Slot action is G3's), so the
    // game's equipment_impacts runs the gem's trigger program; a killing blow must reach the handler with the gem's data.
    private async Task Socketed(EntityId wearer, Guid sword)
    {
        try { _ = Gems.Equipped(wearer); }
        catch (NotImplementedException)
        {
            Info("Gems.Equipped is not on this build (gem wave G1): the socketed handler call is checked after the merge");
            return;
        }
        const string Dagger = "Weapon_T1_2H_Daggers_02_Parry";
        var killGem = _gems.First(g => g.Definition.Trigger!.Kind == GemTriggerKind.Kill);
        var sel = World.ReadBytes(wearer, "SlotSelection");
        if (sel is null)
        {
            Check(false, "the player-template entity has no SlotSelection");
            return;
        }
        var bars = new[] { BitConverter.ToUInt32(sel, 0xa0), BitConverter.ToUInt32(sel, 0xa4) };
        Info($"give dagger: {await Items.Give(wearer, Dagger, 1, new Gear(null, ItemRarity.Common) { GemSlot = true })}");
        Info($"give {killGem.Name}: {await Items.Give(wearer, killGem.Item, 1, new Gear(7))}");
        await Steps(20);
        var inv = Items.Read(wearer);
        var dagger = inv?.Slots.FirstOrDefault(x => !x.IsEmpty && x.Item == Items.Lookup(Dagger)!.Id);
        var gem = inv?.Slots.FirstOrDefault(x => !x.IsEmpty && x.Item == killGem.Item);
        if (dagger is null || gem is null || dagger.ItemEntity.IsNone || gem.ItemEntity.IsNone)
        {
            Check(false, $"dagger {dagger?.Slot} and gem {gem?.Slot} in the inventory");
            return;
        }
        var onBar = Array.IndexOf(bars, dagger.Slot.Inventory.Value);
        if (onBar < 0)
        {
            Check(false, "the dagger landed on no action bar");
            return;
        }
        GameTree.GameRequest(wearer, 5, (uint)(onBar * 8 + dagger.Slot.Index));
        await Steps(10);
        var w1 = await World.WriteField(dagger.ItemEntity, "WeaponGemSlot", "weaponGem.id.value", killGem.Item.Value);
        var w2 = await World.WriteField(dagger.ItemEntity, "WeaponGemSlot", "weaponGem.data.count", 1u);
        var w3 = await World.WriteField(dagger.ItemEntity, "WeaponGemSlot", "weaponGem.data.pide.id", gem.ItemEntity.Value);
        Info($"dagger {dagger.Ref} at {dagger.Slot}, socket component {World.Has(dagger.ItemEntity, "WeaponGemSlot")}, held {Items.Held(wearer)}; " +
             $"gem {gem.Ref} level {gem.Level}; writes {w1.Status} {w2.Status} {w3.Status} {w1.Reason}{w2.Reason}{w3.Reason}");
        Check(w1.Ok && w2.Ok && w3.Ok, $"the probe's socket holds {killGem.Name} (level {gem.Level})");
        await Steps(20);
        var held = Gems.Equipped(wearer).FirstOrDefault(g => g.Slot == GemImpactSlot.Held);
        Check(held is { Impact.IsNone: false } && held.Socket.Gem == killGem.Item, $"Gems.Equipped: the held dagger runs the new gem: {held}");
        var program = held is null ? null : Abilities.Programs.Info(Abilities.Programs.IdOf(Programs.First(p => p.Kind == GemTriggerKind.Kill).Guid));
        Info($"on_kill program in the registry: {program is not null}");
        _calls.Clear();
        var vukah = await Enemy();
        await Hit(wearer, vukah, sword, 5);
        Check(_calls.Count == 0, $"a hit that does not kill calls no kill handler ({_calls.Count})");
        await Hit(wearer, vukah, sword, 5000);
        await Steps(4);
        var c = _calls.FirstOrDefault();
        Check(_calls.Count == 1 && c!.Definition.Id == killGem.Definition.Id && c.Player == wearer && c.Target == vukah && c.KillingBlow
              && c.Level == gem.Level && c.Carrier == dagger.Ref && c.Slot == GemImpactSlot.Held && c.Impact == held!.Impact && c.Damage > 0,
            $"the socketed gem's C# handler ran on the kill: {_calls.Count} call(s); {(c is null ? "" : $"player {c.Player.Value}, target {c.Target.Value}, level {c.Level}, carrier {c.Carrier}, slot {c.Slot}, damage {c.Damage}")}");
        await World.WriteField(dagger.ItemEntity, "WeaponGemSlot", "weaponGem.data.pide.id", 0u);
        await World.WriteField(dagger.ItemEntity, "WeaponGemSlot", "weaponGem.data.count", 0u);
        await World.WriteField(dagger.ItemEntity, "WeaponGemSlot", "weaponGem.id.value", 0u);
    }
}

/// <summary>The 48-byte hit a trigger program sends (Kiln.Abilities' GemHit).</summary>
internal readonly record struct GemHit(uint Attacker, uint Target, uint Part, int Health, bool Crit, bool Kill, bool Backstab, bool Blocked, bool Enemy)
{
    public static GemHit Of(byte[] v) => v.Length < 41 ? default : new(BitConverter.ToUInt32(v, 0), BitConverter.ToUInt32(v, 4),
        BitConverter.ToUInt32(v, 8), BitConverter.ToInt32(v, 12), v[36] != 0, v[37] != 0, v[38] != 0, v[39] != 0, v[40] != 0);
}
