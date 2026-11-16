// SPDX-License-Identifier: MIT
// Gem wave G2: Gems.Register and the dispatch of the Kiln gem trigger programs' signals, on a fake game side
// (IGemBackend: kiln.content, kiln.assets, G1's equipped gems, the clock). Kiln.Blocks.Tests is the test project with
// access to Kiln.Abilities' and Kiln.Api's internals; the programs' own layout is tools/impact/tests/test_gem_programs.py's.
using System.Text.Json.Nodes;
using Xunit;

namespace Kiln.Blocks.Tests;

[Collection("GemRegistry")]  // one static registry
public sealed class GemRegistryTests : IDisposable
{
    private sealed class FakeBackend : IGemBackend
    {
        public string? ContentMissing { get; set; }
        public string? AssetsMissing { get; set; }
        public readonly List<(ModContext Mod, Guid Guid, byte[] Blob)> Served = new();
        public readonly List<ItemDefinition> Items = new();
        public readonly List<(ModContext Mod, string Json, Func<bool> Wanted)> Configs = new();
        public readonly Dictionary<(uint Player, uint Impact), EquippedGem> Worn = new();
        public Exception? LookupThrows;
        public long NowMs { get; set; }
        public int Subscriptions, Unsubscriptions;
        public Action<ImpactSignalEvent>? Handler;

        public void ServeProgram(ModContext mod, Guid guid, byte[] blob) => Served.Add((mod, guid, blob));
        public void DefineItem(ItemDefinition item) => Items.Add(item);
        public void ApplyConfigs(ModContext mod, string documentJson, Func<bool> stillWanted) => Configs.Add((mod, documentJson, stillWanted));

        public EquippedGem? FindEquipped(EntityId player, EntityId impact)
        {
            if (LookupThrows is { } x) throw x;
            return Worn.TryGetValue((player.Value, impact.Value), out var g) ? g : null;
        }

        public IDisposable SubscribeSignals(Action<ImpactSignalEvent> handler)
        {
            Subscriptions++;
            Handler = handler;
            return new Off(this);
        }

        private sealed class Off(FakeBackend b) : IDisposable
        {
            public void Dispose()
            {
                b.Unsubscriptions++;
                b.Handler = null;
            }
        }
    }

    private readonly FakeBackend _game = new();
    private readonly IGemBackend _saved;
    private readonly ModContext? _outer;
    private readonly List<ModContext> _mods = new();

    public GemRegistryTests()
    {
        _saved = GemRegistry.Backend;
        GemRegistry.ResetForTests();
        GemRegistry.Backend = _game;
        _outer = ModContext.CurrentMod;
    }

    public void Dispose()
    {
        foreach (var m in _mods) m.Retire();
        GemRegistry.ResetForTests();
        GemRegistry.Backend = _saved;
        ModContext.CurrentMod = _outer;
    }

    private ModContext Mod(string id)
    {
        var m = HostAccess.CreateContext(new ModInfo(id, id, "1", AppContext.BaseDirectory), (ulong)(0x6700 + _mods.Count), AppContext.BaseDirectory);
        _mods.Add(m);
        return m;
    }

    private static GemHandle As(ModContext mod, GemDefinition d)
    {
        var outer = ModContext.CurrentMod;
        ModContext.CurrentMod = mod;
        try { return Gems.Register(d); }
        finally { ModContext.CurrentMod = outer; }
    }

    private static GemDefinition Frost(Action<GemTriggerContext> on, GemTrigger? trigger = null, string id = "frostmod:frostbite") => new()
    {
        Id = id,
        BaseOn = "T1_Gem_GemOfBleeding",
        DisplayName = "Gem of Frostbite",
        Texts = { ["de"] = new ItemTexts { Name = "Frostbiss-Edelstein" } },
        Icon = "icons/frostbite.png",
        Rarity = ItemRarity.Rare,
        Trigger = trigger ?? GemTrigger.OnHit(crit: true),
        OnTrigger = on,
    };

    // the 48-byte $hit block of the trigger programs
    private static byte[] Hit(uint attacker, uint target, int health = -30, bool crit = false, bool kill = false, bool enemy = true,
        float x = 1, float y = 2, float z = 3, uint weapon = 77, uint part = 0, bool backstab = false, bool blocked = false)
    {
        var b = new byte[GemHit.Size];
        BitConverter.TryWriteBytes(b.AsSpan(0), attacker);
        BitConverter.TryWriteBytes(b.AsSpan(4), target);
        BitConverter.TryWriteBytes(b.AsSpan(8), part == 0 ? target : part);
        BitConverter.TryWriteBytes(b.AsSpan(12), health);
        BitConverter.TryWriteBytes(b.AsSpan(16), x);
        BitConverter.TryWriteBytes(b.AsSpan(20), y);
        BitConverter.TryWriteBytes(b.AsSpan(24), z);
        BitConverter.TryWriteBytes(b.AsSpan(28), weapon);
        BitConverter.TryWriteBytes(b.AsSpan(32), 88u);
        b[36] = (byte)(crit ? 1 : 0);
        b[37] = (byte)(kill ? 1 : 0);
        b[38] = (byte)(backstab ? 1 : 0);
        b[39] = (byte)(blocked ? 1 : 0);
        b[40] = (byte)(enemy ? 1 : 0);
        return b;
    }

    private void Wear(uint player, uint impact, GemHandle gem, int level = 7, GemImpactSlot slot = GemImpactSlot.Held) =>
        _game.Worn[(player, impact)] = new EquippedGem(new EntityId(player), slot,
            new SocketedGem(new ItemInstanceRef(default, new ItemId(0xabcd)), gem.Item, level), new EntityId(impact));

    private static int Signal(GemTriggerKind kind, uint impact, byte[] value, ulong step = 5)
    {
        var p = GemRegistry.ProgramOf(kind);
        return GemRegistry.Dispatch(p.Id, p.Tag, new EntityId(impact), value, step);
    }

    // ---------------------------------------------------------------------------------------------- registration

    [Fact]
    public void A_trigger_gem_is_a_clone_whose_program_is_the_trigger_program_served_under_its_mod()
    {
        var mod = Mod("frostmod");
        var h = As(mod, Frost(_ => { }));

        Assert.True(h.Ok, h.Reason);
        Assert.Null(h.Reason);
        Assert.Equal(GemRegistry.GuidOf("frostmod:frostbite"), h.Guid);
        Assert.Equal("Gem_frostmod_frostbite", h.Name);
        Assert.Equal(NewContent.IdOf(h.Guid), h.Item.Value);

        var item = Assert.Single(_game.Items);
        Assert.Equal(h.Guid, item.Guid);
        Assert.Equal("Gem_frostmod_frostbite", item.Name);
        Assert.Equal("T1_Gem_GemOfBleeding", item.BaseOn);
        Assert.Equal("Gem of Frostbite", item.DisplayName);
        Assert.Equal("icons/frostbite.png", item.Icon);
        Assert.Equal("Frostbiss-Edelstein", item.Texts!["de"].Name);
        Assert.Equal("Rare", item.Fields!["rarity"]);
        Assert.Equal(1, item.Fields["itemLevelRange.minLevel"]);
        Assert.Equal(105, item.Fields["itemLevelRange.maxLevel"]);
        var crit = GemRegistry.ProgramOf(GemTriggerKind.Crit);
        Assert.Equal(crit.Guid.ToString(), item.Refs!["equipment.program"]);

        var served = Assert.Single(_game.Served);
        Assert.Same(mod, served.Mod);
        Assert.Equal(crit.Guid, served.Guid);
        // a keen::ImpactProgram blob: id (FNV-1a of the GUID) at +0, the GUID at +4
        Assert.Equal(crit.Id, BitConverter.ToUInt32(served.Blob, 0));
        Assert.Equal(crit.Guid, new Guid(served.Blob.AsSpan(4, 16)));
        Assert.Equal(1, _game.Subscriptions);
        Assert.True(GemRegistry.Listening);
    }

    [Fact]
    public void Every_trigger_program_is_embedded_with_its_own_GUID_and_id()
    {
        foreach (var p in GemRegistry.Programs)
        {
            var b = GemRegistry.Blob(p);
            Assert.Equal(p.Guid, new Guid(b.AsSpan(4, 16)));
            Assert.Equal(p.Id, BitConverter.ToUInt32(b, 0));
            Assert.Equal(Abilities.Programs.IdOf(p.Guid), p.Id);
        }
        Assert.Equal(GemRegistry.Programs.Length, GemRegistry.Programs.Select(p => p.Tag).Distinct().Count());
    }

    [Fact]
    public void A_mod_serves_each_trigger_program_once_and_another_mod_serves_its_own_copy()
    {
        var a = Mod("amod");
        var b = Mod("bmod");
        Assert.True(As(a, Frost(_ => { }, GemTrigger.OnKill(), "amod:one")).Ok);
        Assert.True(As(a, Frost(_ => { }, GemTrigger.OnKill(), "amod:two")).Ok);
        Assert.True(As(a, Frost(_ => { }, GemTrigger.Passive(TimeSpan.FromSeconds(1)), "amod:three")).Ok);
        Assert.True(As(b, Frost(_ => { }, GemTrigger.OnKill(), "bmod:one")).Ok);

        Assert.Equal(3, _game.Served.Count);
        Assert.Equal(2, _game.Served.Count(s => s.Mod == a));
        Assert.Single(_game.Served, s => s.Mod == b);
        Assert.Equal(1, _game.Subscriptions);
    }

    [Fact]
    public void A_data_gem_keeps_or_swaps_the_program_and_takes_its_configs_as_a_layer_of_its_mod()
    {
        var mod = Mod("datamod");
        var keep = As(mod, new GemDefinition { Id = "datamod:bleed2", BaseOn = "T1_Gem_GemOfBleeding", Configs = { ["0x5e34f09b"] = 12 } });
        var swap = As(mod, new GemDefinition
        {
            Id = "datamod:skulls2", BaseOn = "T1_Gem_GemOfBleeding", Guid = new Guid("3f0c6a51-8e2d-4f0b-9a44-2b7d1c9e5a10"),
            Name = "DataMod_Gem_Skulls", Program = new Guid("8611b73a-2b76-4745-bbbf-222f87cf0199"),
            Configs = { ["DebuffDuration"] = TimeSpan.FromSeconds(10), ["0x5e34f09b.scaleFactor"] = 3.5, ["datamod.Radius"] = 4, ["BuffType"] = "T1_Buff" },
        });

        Assert.True(keep.Ok && swap.Ok);
        Assert.Empty(_game.Served);
        Assert.Equal(0, _game.Subscriptions);
        Assert.Null(_game.Items[0].Refs);
        Assert.Equal("8611b73a-2b76-4745-bbbf-222f87cf0199", _game.Items[1].Refs!["equipment.program"]);
        Assert.Equal(new Guid("3f0c6a51-8e2d-4f0b-9a44-2b7d1c9e5a10"), swap.Guid);

        Assert.Equal(2, _game.Configs.Count);
        var doc = JsonNode.Parse(_game.Configs[1].Json)!;
        Assert.Equal(1, (int)doc["format"]!);
        var rules = doc["items"]!["DataMod_Gem_Skulls"]!.AsObject();
        Assert.Equal(10.0, (double)rules["impactValues.DebuffDuration"]!);
        Assert.Equal(3.5, (double)rules["impactValues.0x5e34f09b.scaleFactor"]!);
        Assert.Equal(4, (long)rules["impactValues[datamod.Radius]"]!);
        Assert.Equal("T1_Buff", (string)rules["impactValues.BuffType"]!);
        Assert.Equal(12, (long)JsonNode.Parse(_game.Configs[0].Json)!["items"]!["Gem_datamod_bleed2"]!["impactValues.0x5e34f09b"]!);
        Assert.Same(mod, _game.Configs[0].Mod);

        Assert.True(_game.Configs[1].Wanted());
        swap.Dispose();
        Assert.False(_game.Configs[1].Wanted());  // the next world load applies nothing for a dropped gem
        Assert.True(_game.Configs[0].Wanted());
    }

    [Fact]
    public void A_second_gem_with_the_same_id_GUID_or_name_is_refused_with_a_reason()
    {
        var a = Mod("amod");
        var b = Mod("bmod");
        Assert.True(As(a, Frost(_ => { }, id: "amod:x")).Ok);

        var sameId = As(b, Frost(_ => { }, id: "amod:x"));
        Assert.False(sameId.Ok);
        Assert.Contains("registered already (by amod)", sameId.Reason);

        var sameGuid = As(b, new GemDefinition { Id = "bmod:y", BaseOn = "x", Guid = GemRegistry.GuidOf("amod:x") });
        Assert.False(sameGuid.Ok);
        Assert.Contains("amod:x", sameGuid.Reason);

        var sameName = As(b, new GemDefinition { Id = "bmod:z", BaseOn = "T1_Gem_GemOfBleeding", Name = "Gem_amod_x" });
        Assert.False(sameName.Ok);
        Assert.Contains("debug name", sameName.Reason);
        Assert.Single(_game.Items);
        sameName.Dispose();  // a refused handle drops nothing
        Assert.Single(GemRegistry.Definitions);
    }

    [Theory]
    [InlineData("nocolon")]
    [InlineData("Upper:case")]
    [InlineData("mod:")]
    [InlineData("mod:bad name")]
    public void A_bad_id_throws(string id)
    {
        var e = Assert.Throws<ArgumentException>(() => As(Mod("m"), new GemDefinition { Id = id, BaseOn = "T1_Gem_GemOfBleeding" }));
        Assert.Contains("<modid>:<name>", e.Message);
        Assert.Empty(_game.Items);
    }

    [Fact]
    public void Definition_mistakes_throw_ArgumentException_and_register_nothing()
    {
        var m = Mod("m");
        void Bad(GemDefinition d, string text) => Assert.Contains(text, Assert.Throws<ArgumentException>(() => As(m, d)).Message);
        Bad(new GemDefinition { Id = "m:a", BaseOn = " " }, "BaseOn");
        Bad(new GemDefinition { Id = "m:a", BaseOn = "x", LevelMin = 0 }, "LevelMin");
        Bad(new GemDefinition { Id = "m:a", BaseOn = "x", LevelMax = 106 }, "LevelMax");
        Bad(new GemDefinition { Id = "m:a", BaseOn = "x", LevelMin = 20, LevelMax = 10 }, "above LevelMax");
        Bad(new GemDefinition { Id = "m:a", BaseOn = "x", Guid = Guid.Empty }, "Guid");
        Bad(new GemDefinition { Id = "m:a", BaseOn = "x", Program = Guid.NewGuid(), Trigger = GemTrigger.OnKill(), OnTrigger = _ => { } }, "at once");
        Bad(new GemDefinition { Id = "m:a", BaseOn = "x", Trigger = GemTrigger.OnKill() }, "needs OnTrigger");
        Bad(new GemDefinition { Id = "m:a", BaseOn = "x", OnTrigger = _ => { } }, "needs a Trigger");
        Bad(new GemDefinition { Id = "m:a", BaseOn = "x", Trigger = GemTrigger.OnKill(), OnTrigger = _ => { }, Configs = { ["Damage"] = 1 } }, "reads no configs");
        Bad(new GemDefinition { Id = "m:a", BaseOn = "x", Configs = { ["Damage"] = new object() } }, "no config value");
        Bad(new GemDefinition { Id = "m:a", BaseOn = "x", Name = "has space" }, "debug name");
        Assert.Throws<ArgumentOutOfRangeException>(() => GemTrigger.Passive(TimeSpan.FromMilliseconds(100)));
        Assert.Throws<ArgumentOutOfRangeException>(() => GemTrigger.Passive(TimeSpan.FromHours(2)));
        Assert.Empty(_game.Items);
        Assert.Empty(_game.Served);
        Assert.Empty(GemRegistry.Definitions);
    }

    [Fact]
    public void Without_new_items_or_kiln_assets_Register_throws_KilnUnavailable_naming_gems_triggers()
    {
        var m = Mod("m");
        _game.AssetsMissing = "kiln.assets is not loaded";
        // a data gem needs no kiln.assets
        Assert.True(As(m, new GemDefinition { Id = "m:data", BaseOn = "x" }).Ok);
        var e = Assert.Throws<KilnUnavailableException>(() => As(m, Frost(_ => { }, id: "m:trig")));
        Assert.Equal("gems.triggers", e.Capability);
        Assert.Contains("kiln.assets", e.Message);
        _game.ContentMissing = "no kiln.content";
        e = Assert.Throws<KilnUnavailableException>(() => As(m, new GemDefinition { Id = "m:data2", BaseOn = "x" }));
        Assert.Equal("gems.triggers", e.Capability);
        Assert.Single(_game.Items);
    }

    [Fact]
    public void Register_outside_mod_code_throws_InvalidOperation()
    {
        ModContext.CurrentMod = null;
        Assert.Throws<InvalidOperationException>(() => Gems.Register(new GemDefinition { Id = "m:a", BaseOn = "x" }));
    }

    [Fact]
    public void The_GUID_from_the_id_is_stable_name_based_and_case_blind()
    {
        var g = GemRegistry.GuidOf("mymod:frostbite");
        Assert.Equal(g, GemRegistry.GuidOf("mymod:frostbite"));
        Assert.Equal(g, GemRegistry.GuidOf("MyMod:Frostbite"));
        Assert.NotEqual(g, GemRegistry.GuidOf("mymod:frostbite2"));
        Assert.Equal(5, g.Version);
        Assert.Equal("Gem_my-mod_frost.bite", GemRegistry.NameOf("my-mod:frost.bite"));
    }

    // -------------------------------------------------------------------------------------------------- dispatch

    [Fact]
    public void A_crit_signal_calls_the_handler_as_its_mod_with_the_hit_the_gem_its_level_and_carrier()
    {
        var mod = Mod("frostmod");
        GemTriggerContext? seen = null;
        ModContext? during = null;
        var h = As(mod, Frost(c => { seen = c; during = ModContext.CurrentMod; }));
        Wear(player: 11, impact: 900, h, level: 23, slot: GemImpactSlot.Ring1);

        var n = Signal(GemTriggerKind.Crit, 900, Hit(attacker: 11, target: 44, health: -30, crit: true, part: 45, backstab: true), step: 1234);

        Assert.Equal(1, n);
        Assert.NotNull(seen);
        Assert.Same(mod, during);
        Assert.Same(_outer, ModContext.CurrentMod);
        Assert.Equal(GemTriggerKind.Crit, seen!.Trigger);
        Assert.Equal(new EntityId(11), seen.Player);
        Assert.Equal(new EntityId(44), seen.Target);
        Assert.Equal(new EntityId(45), seen.TargetPart);
        Assert.Equal(h.Item, seen.Gem);
        Assert.Equal(23, seen.Level);
        Assert.Equal(GemImpactSlot.Ring1, seen.Slot);
        Assert.Equal(new ItemId(0xabcd), seen.Carrier.Item);
        Assert.Equal(new EntityId(900), seen.Impact);
        Assert.Equal(30, seen.Damage);
        Assert.Equal(-30, seen.HealthChange);
        Assert.Equal(new WorldPosition(1, 2, 3), seen.Position);
        Assert.Equal(new EntityId(77), seen.Weapon);
        Assert.Equal(new EntityId(88), seen.Source);
        Assert.True(seen.Crit && seen.Backstab && seen.OtherIsEnemy);
        Assert.False(seen.KillingBlow || seen.Blocked);
        Assert.Equal(1234UL, seen.Step);
        Assert.Same(h.Definition, seen.Definition);
    }

    [Fact]
    public void The_signal_arrives_through_the_event_kiln_subscribed_as_itself()
    {
        var mod = Mod("frostmod");
        var calls = 0;
        var h = As(mod, Frost(_ => calls++));
        Wear(11, 900, h);
        var p = GemRegistry.ProgramOf(GemTriggerKind.Crit);
        _game.Handler!(new ImpactSignalEvent(new ImpactRef(3, p.Id, new EntityId(900), EntityId.None), p.Tag, 0x77, 0x32940bec,
            Hit(11, 44, crit: true), 9));
        Assert.Equal(1, calls);
    }

    [Fact]
    public void Enemies_only_skips_hits_on_others_and_a_gem_without_it_takes_them()
    {
        var mod = Mod("m");
        int enemy = 0, any = 0;
        var e = As(mod, Frost(_ => enemy++, GemTrigger.OnHit(), "m:enemy"));
        var a = As(mod, Frost(_ => any++, GemTrigger.OnHit(enemiesOnly: false), "m:any"));
        Wear(11, 900, e);
        Wear(11, 901, a);

        Assert.Equal(0, Signal(GemTriggerKind.Hit, 900, Hit(11, 44, enemy: false)));
        Assert.Equal(1, Signal(GemTriggerKind.Hit, 900, Hit(11, 44, enemy: true)));
        Assert.Equal(1, Signal(GemTriggerKind.Hit, 901, Hit(11, 44, enemy: false)));
        Assert.Equal((1, 1), (enemy, any));
    }

    [Fact]
    public void Signals_of_other_programs_tags_gems_or_kinds_call_nothing()
    {
        var mod = Mod("m");
        var calls = 0;
        var kill = As(mod, Frost(_ => calls++, GemTrigger.OnKill(), "m:kill"));
        Wear(11, 900, kill);
        var p = GemRegistry.ProgramOf(GemTriggerKind.Kill);
        var v = Hit(11, 44, kill: true);

        Assert.Equal(0, GemRegistry.Dispatch(0x12345678, p.Tag, new EntityId(900), v, 1));        // a mod's own program
        Assert.Equal(0, GemRegistry.Dispatch(p.Id, 7, new EntityId(900), v, 1));                // another tag
        Assert.Equal(0, Signal(GemTriggerKind.Hit, 900, v));                                     // a kill gem on the hit program (data pointed it there)
        Assert.Equal(0, Signal(GemTriggerKind.Kill, 901, v));                                    // an impact that is no worn gem
        Assert.Equal(0, Signal(GemTriggerKind.Kill, 900, new byte[12]));                         // a short payload
        _game.Worn[(11, 902)] = new EquippedGem(new EntityId(11), GemImpactSlot.Held,
            new SocketedGem(default, new ItemId(0x1111), 3), new EntityId(902));
        Assert.Equal(0, Signal(GemTriggerKind.Kill, 902, v));                                    // a gem nobody registered
        Assert.Equal(1, Signal(GemTriggerKind.Kill, 900, v));
        Assert.Equal(1, calls);
    }

    [Fact]
    public void A_hit_taken_is_the_wearers_when_it_is_the_target_and_the_attacker_is_the_target_of_the_context()
    {
        var mod = Mod("m");
        GemTriggerContext? seen = null;
        var h = As(mod, Frost(c => seen = c, GemTrigger.OnHitTaken(), "m:thorns"));
        Wear(11, 900, h);

        Assert.Equal(1, Signal(GemTriggerKind.HitTaken, 900, Hit(attacker: 50, target: 11, health: -12)));
        Assert.Equal(new EntityId(11), seen!.Player);
        Assert.Equal(new EntityId(50), seen.Target);
        Assert.Equal(12, seen.Damage);
    }

    [Fact]
    public void A_passive_gem_runs_at_its_interval_on_the_programs_250_ms_signals()
    {
        var mod = Mod("m");
        var at = new List<long>();
        GemTriggerContext? last = null;
        var h = As(mod, Frost(c => { at.Add(_game.NowMs); last = c; }, GemTrigger.Passive(TimeSpan.FromSeconds(1)), "m:aura"));
        Wear(11, 900, h);
        var root = BitConverter.GetBytes(11u);
        for (_game.NowMs = 10_000; _game.NowMs <= 12_600; _game.NowMs += 250) Signal(GemTriggerKind.Passive, 900, root);

        Assert.Equal(new long[] { 10_000, 11_000, 12_000 }, at);
        Assert.Equal(new EntityId(11), last!.Player);
        Assert.Equal(EntityId.None, last.Target);
        // a second wearer's impact has its own clock
        Wear(12, 901, h);
        Assert.Equal(1, Signal(GemTriggerKind.Passive, 901, BitConverter.GetBytes(12u)));
    }

    [Fact]
    public void A_throwing_handler_is_logged_and_the_next_signal_still_runs_it()
    {
        var mod = Mod("m");
        var calls = 0;
        var h = As(mod, Frost(_ => { calls++; throw new InvalidOperationException("boom"); }, GemTrigger.OnKill(), "m:bad"));
        Wear(11, 900, h);
        for (var i = 0; i < 5; i++) Assert.Equal(1, Signal(GemTriggerKind.Kill, 900, Hit(11, 44, kill: true)));
        Assert.Equal(5, calls);
        Assert.Same(_outer, ModContext.CurrentMod);
    }

    [Fact]
    public void A_wearer_lookup_that_is_not_there_calls_nothing()
    {
        var mod = Mod("m");
        var calls = 0;
        var h = As(mod, Frost(_ => calls++, GemTrigger.OnKill(), "m:k"));
        Wear(11, 900, h);
        _game.LookupThrows = new NotImplementedException("gem wave G1");
        Assert.Equal(0, Signal(GemTriggerKind.Kill, 900, Hit(11, 44, kill: true)));
        Assert.Equal(0, Signal(GemTriggerKind.Kill, 900, Hit(11, 44, kill: true)));
        _game.LookupThrows = null;
        Assert.Equal(1, Signal(GemTriggerKind.Kill, 900, Hit(11, 44, kill: true)));
        Assert.Equal(1, calls);
    }

    [Fact]
    public void Unloading_the_mod_or_disposing_drops_its_gems_and_the_last_one_ends_the_subscription()
    {
        var a = Mod("amod");
        var b = Mod("bmod");
        var calls = 0;
        var ha = As(a, Frost(_ => calls++, GemTrigger.OnKill(), "amod:k"));
        var hb = As(b, Frost(_ => calls++, GemTrigger.OnKill(), "bmod:k"));
        Wear(11, 900, ha);
        Wear(11, 901, hb);

        a.Retire();
        Assert.Equal(0, Signal(GemTriggerKind.Kill, 900, Hit(11, 44, kill: true)));
        Assert.Equal(1, Signal(GemTriggerKind.Kill, 901, Hit(11, 44, kill: true)));
        Assert.True(GemRegistry.Listening);

        hb.Dispose();
        Assert.Equal(0, Signal(GemTriggerKind.Kill, 901, Hit(11, 44, kill: true)));
        Assert.False(GemRegistry.Listening);
        Assert.Equal(1, _game.Unsubscriptions);
        Assert.Equal(1, calls);
        Assert.Empty(GemRegistry.Definitions);

        // registered again (a reload): served again under the new load, listening again
        var a2 = Mod("amod");
        Assert.True(As(a2, Frost(_ => calls++, GemTrigger.OnKill(), "amod:k")).Ok);
        Assert.Equal(3, _game.Served.Count);
        Assert.Equal(2, _game.Subscriptions);
    }

    [Fact]
    public void A_data_only_registration_does_not_listen_to_signals()
    {
        var m = Mod("m");
        As(m, new GemDefinition { Id = "m:data", BaseOn = "x" });
        Assert.False(GemRegistry.Listening);
        Assert.Equal(0, _game.Subscriptions);
    }

    // ------------------------------------------------------------------------------------------------- the row

    [Fact]
    public void The_row_says_what_Register_needs()
    {
        Assert.Equal((CapabilityState.Available, CapabilityReason.None, ""), GemRegistry.DecideRow(null, null, true));
        Assert.Equal(CapabilityReason.ServiceMissing, GemRegistry.DecideRow(null, null, false).Reason);
        Assert.Contains("kiln.content", GemRegistry.DecideRow("v2", null, true).Text);
        Assert.Contains("kiln.assets", GemRegistry.DecideRow(null, "gone", true).Text);
        Assert.Equal(CapabilityState.Unavailable, GemRegistry.DecideRow(null, "gone", true).State);
    }

    [Theory]
    [InlineData("BuffType", "impactValues.BuffType")]
    [InlineData("0x5e34f09b", "impactValues.0x5e34f09b")]
    [InlineData("0x5e34f09b.scaleFactor", "impactValues.0x5e34f09b.scaleFactor")]
    [InlineData("mymod.Radius", "impactValues[mymod.Radius]")]
    [InlineData("mymod.Radius.scaleFactor", "impactValues[mymod.Radius].scaleFactor")]
    [InlineData("[mymod.Radius]", "impactValues[mymod.Radius]")]
    public void Config_keys_become_kiln_gamedata_item_paths(string key, string path) => Assert.Equal(path, GemRegistry.ConfigPath(key));
}
