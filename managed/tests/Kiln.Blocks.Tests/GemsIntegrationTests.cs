// SPDX-License-Identifier: MIT
// Gem wave integration: the packages together on a fake game. G1's Equipped over every one of the 11 equipment impact
// slots (G4 puts sockets on rings, armour, shields and other gear) feeds GemTriggeredEvent and G2's trigger dispatch
// (GemRegistry with G1's own Equipped logic, not a list of its own), and a placeholder of a removed mod's trigger gem
// (kiln.content keeps its program when another mod serves it) calls nothing.
using Xunit;

namespace Kiln.Blocks.Tests;

/// <summary>A game with one player whose 11 impact slots each hold an item with a socketed gem.</summary>
internal sealed class ElevenSlotWorld : IGemWorld
{
    public const uint Player = 1;
    public static readonly ItemId Bleeding = new(0x11110001), Ring = new(0x33330002), Sword = new(0x33330001);
    public readonly Dictionary<uint, GemImpactRow> Rows = new();
    public readonly Dictionary<uint, GemItemRead> Entities = new();

    public static uint CarrierOf(int slot) => 2000u + (uint)slot;
    public static uint GemEntityOf(int slot) => 3000u + (uint)slot;
    public static uint ImpactOf(int slot) => 5000u + (uint)slot;
    public static ItemInstanceRef CarrierRef(int slot) =>
        new(new EntityRef(new EntityId(CarrierOf(slot)), 7), slot is 7 or 8 ? Ring : Sword);

    /// <summary>Slot <paramref name="slot"/>'s carrier holds <paramref name="gem"/> at <paramref name="level"/> and the
    /// game spawned its gem impact.</summary>
    public void Socket(int slot, ItemId gem, int level)
    {
        Rows[(uint)slot] = new GemImpactRow(slot, CarrierOf(slot), CarrierRef(slot).Item, new EntityId(ImpactOf(slot)), new EntityId(GemEntityOf(slot)));
        Entities[CarrierOf(slot)] = new GemItemRead(CarrierRef(slot), gem, level, 0);
    }

    public IReadOnlyList<ItemInfo> Items() =>
    [
        new(Bleeding, "T1_Gem_GemOfBleeding", "Gem of Bleeding", 1, ItemCategory.WeaponGem, ItemRarity.Common, false, 1, 105, 0, 0, Guid.NewGuid()),
    ];
    public long CatalogKey() => 1;
    public GemData? Data(ItemInfo item) => null;
    public IReadOnlySet<uint> ModItems() => new HashSet<uint>();
    public IReadOnlyList<GemImpactRow>? Impacts(EntityId holder) =>
        holder.Value != Player ? null
            : Enumerable.Range(0, 11).Select(i => Rows.TryGetValue((uint)i, out var r) ? r : new GemImpactRow(i, 0, ItemId.None, EntityId.None, EntityId.None)).ToList();
    public IReadOnlyList<GemItemRead?> ReadEntities(IReadOnlyList<uint> pides) =>
        pides.Select(p => Entities.TryGetValue(p, out var r) ? r : (GemItemRead?)null).ToList();
    public GemItemRead? Read(ItemInstanceRef item) => Entities.TryGetValue(item.Entity.Id.Value, out var r) && r.Item == item ? r : null;
    public IReadOnlyList<GemItemRead>? ReadHolder(EntityId holder) => null;
    public IReadOnlyList<EntityId> Players() => [new EntityId(Player)];
    public EntityId ParentImpact(EntityId impact) => EntityId.None;
}

[Collection("gems")]
public sealed class GemsEverySlotTests : IDisposable
{
    readonly IGemWorld _was = GemReads.World;

    public GemsEverySlotTests()
    {
        GemReads.Forget();
        GemTriggers.Reset();
    }

    public void Dispose()
    {
        GemReads.World = _was;
        GemReads.Forget();
        GemTriggers.Reset();
    }

    static ImpactDamageEvent Damage(uint self, uint root, uint target, ulong step)
    {
        var args = new uint[15];
        var values = new uint[15];
        args[1] = 0xFFFF0000; values[1] = root;
        args[2] = 0xFFFF0001; values[2] = target;
        return new ImpactDamageEvent(new NativeSnapshot
        {
            Hash = 0xb04d0e7a, Impact = new ImpactRef(77, 0x1234, new EntityId(self), new EntityId(root)), Step = step,
            Args = args, Values = values, Offsets = Enumerable.Repeat((byte)0xFF, 15).ToArray(), Sizes = new byte[15], Payload = new byte[136],
        });
    }

    [Fact]
    public void EveryImpactSlotsGemIsEquippedWithItsSlotCarrierAndLevel()
    {
        var w = new ElevenSlotWorld();
        for (var s = 0; s <= 10; s++) w.Socket(s, ElevenSlotWorld.Bleeding, 10 + s);
        var list = GemReads.Equipped(w, new EntityId(ElevenSlotWorld.Player));
        Assert.Equal(11, list.Count);
        for (var s = 0; s <= 10; s++)
        {
            Assert.Equal((GemImpactSlot)s, list[s].Slot);
            Assert.Equal(new SocketedGem(ElevenSlotWorld.CarrierRef(s), ElevenSlotWorld.Bleeding, 10 + s), list[s].Socket);
            Assert.Equal(new EntityId(ElevenSlotWorld.ImpactOf(s)), list[s].Impact);
        }
        Assert.Equal(
            [GemImpactSlot.Held, GemImpactSlot.Shield, GemImpactSlot.Head, GemImpactSlot.Chest, GemImpactSlot.Hands, GemImpactSlot.Legs,
             GemImpactSlot.Feet, GemImpactSlot.Ring1, GemImpactSlot.Ring2, GemImpactSlot.Ammo, GemImpactSlot.Spell],
            list.Select(g => g.Slot));
    }

    [Fact]
    public void AGemsNativeFromAnySlotIsPostedAsGemTriggeredWithThatSlot()
    {
        var w = new ElevenSlotWorld();
        for (var s = 0; s <= 10; s++) w.Socket(s, ElevenSlotWorld.Bleeding, 5);
        GemReads.World = w;
        var seen = new List<GemTriggeredEvent>();
        using var sub = Events.On<GemTriggeredEvent>(seen.Add);
        for (var s = 0; s <= 10; s++) GemTriggers.Deliver(Damage(ElevenSlotWorld.ImpactOf(s), ElevenSlotWorld.Player, 9000, 40), 40);
        Assert.Equal(Enumerable.Range(0, 11).Select(s => (GemImpactSlot)s), seen.Select(e => e.Gem.Slot));
        Assert.All(seen, e => Assert.Equal(new EntityId(9000), e.Target));
        Assert.Equal(ElevenSlotWorld.CarrierRef(8), seen[8].Gem.Socket.Carrier);  // Ring2's carrier is the ring
    }
}

[Collection("GemRegistry")]  // one static registry
public sealed class GemTriggersInEverySlotTests : IDisposable
{
    // G2's game side with G1's Equipped logic over a fake world (the live backend calls Gems.Equipped, which is this)
    sealed class Backend(IGemWorld world) : IGemBackend
    {
        public string? ContentMissing => null;
        public string? AssetsMissing => null;
        public void ServeProgram(ModContext mod, Guid guid, byte[] blob) { }
        public void DefineItem(ItemDefinition item) { }
        public void ApplyConfigs(ModContext mod, string documentJson, Func<bool> stillWanted) { }
        public IReadOnlyList<EquippedGem> Equipped(EntityId player) => GemReads.Equipped(world, player);
        public long NowMs => 0;
        public IDisposable SubscribeSignals(Action<ImpactSignalEvent> handler) => new Off();
        sealed class Off : IDisposable { public void Dispose() { } }
    }

    readonly ElevenSlotWorld _world = new();
    readonly IGemBackend _saved;
    readonly ModContext? _outer = ModContext.CurrentMod;
    readonly List<ModContext> _mods = new();

    public GemTriggersInEverySlotTests()
    {
        _saved = GemRegistry.Backend;
        GemRegistry.ResetForTests();
        GemRegistry.Backend = new Backend(_world);
    }

    public void Dispose()
    {
        foreach (var m in _mods) m.Retire();
        GemRegistry.ResetForTests();
        GemRegistry.Backend = _saved;
        ModContext.CurrentMod = _outer;
    }

    GemHandle Register(string mod, string name, Action<GemTriggerContext> on)
    {
        var m = HostAccess.CreateContext(new ModInfo(mod, mod, "1", AppContext.BaseDirectory), (ulong)(0x6800 + _mods.Count), AppContext.BaseDirectory);
        _mods.Add(m);
        ModContext.CurrentMod = m;
        try
        {
            return Gems.Register(new GemDefinition
            {
                Id = $"{mod}:{name}", BaseOn = "T1_Gem_GemOfBleeding", Trigger = GemTrigger.OnKill(), OnTrigger = on,
            });
        }
        finally { ModContext.CurrentMod = _outer; }
    }

    static byte[] Kill(uint attacker, uint target)
    {
        var b = new byte[GemHit.Size];
        BitConverter.TryWriteBytes(b.AsSpan(0), attacker);
        BitConverter.TryWriteBytes(b.AsSpan(4), target);
        BitConverter.TryWriteBytes(b.AsSpan(8), target);
        BitConverter.TryWriteBytes(b.AsSpan(12), -500);
        b[37] = 1;  // kill
        b[40] = 1;  // enemy
        return b;
    }

    static int Signal(uint impact, byte[] value, ulong step)
    {
        var p = GemRegistry.ProgramOf(GemTriggerKind.Kill);
        return GemRegistry.Dispatch(p.Id, p.Tag, new EntityId(impact), value, step);
    }

    [Fact]
    public void ARegisteredTriggerGemInAnySlotCallsItsHandlerWithThatSlotCarrierAndLevel()
    {
        var calls = new List<GemTriggerContext>();
        var gem = Register("ringmod", "reaper", calls.Add);
        Assert.True(gem.Ok, gem.Reason);
        for (var s = 0; s <= 10; s++)
        {
            _world.Rows.Clear();
            _world.Entities.Clear();
            _world.Socket(s, gem.Item, 20 + s);  // a ring (G4) or any other carrier, socketed by the forge (G3)
            Assert.Equal(1, Signal(ElevenSlotWorld.ImpactOf(s), Kill(ElevenSlotWorld.Player, 77), (ulong)(100 + s)));
        }
        Assert.Equal(Enumerable.Range(0, 11).Select(s => (GemImpactSlot)s), calls.Select(c => c.Slot));
        for (var s = 0; s <= 10; s++)
        {
            Assert.Equal(ElevenSlotWorld.CarrierRef(s), calls[s].Carrier);
            Assert.Equal(20 + s, calls[s].Level);
            Assert.Equal(new EntityId(ElevenSlotWorld.ImpactOf(s)), calls[s].Impact);
            Assert.Equal(new EntityId(77), calls[s].Target);
        }
    }

    [Fact]
    public void APlaceholderGemRunningAnotherModsTriggerProgramCallsNothing()
    {
        // A removed mod's kill gem is kiln.content's placeholder now. While another mod's kill gem is registered, the
        // kill program is served, so the placeholder keeps it: its signals come in, but no registered gem is that item.
        var calls = 0;
        var live = Register("othermod", "reaper", _ => calls++);
        var removed = new ItemId(0x5eed0001);  // the gone mod's gem item (its id stays in the save)
        _world.Socket(7, removed, 30);         // in a ring
        _world.Socket(0, live.Item, 5);        // and the live gem in the held weapon
        Assert.Equal(0, Signal(ElevenSlotWorld.ImpactOf(7), Kill(ElevenSlotWorld.Player, 77), 200));
        Assert.Equal(0, calls);
        Assert.Equal(1, Signal(ElevenSlotWorld.ImpactOf(0), Kill(ElevenSlotWorld.Player, 77), 201));
        Assert.Equal(1, calls);
    }
}

// A removed mod's trigger gem (lab 2026-11-16): kiln.content makes a placeholder's program that nothing serves none, and
// the game then logs "Gem has no valid Impcat Program assigned" every step while that gem sits in an impact slot. For
// Kiln's own trigger programs Kiln serves the one a placeholder names when no registered gem serves it: the placeholder
// resolves to it, runs it, and calls nothing (no registered gem is that item).
public sealed class GemPlaceholderProgramsTests
{
    static readonly HashSet<GemTriggerKind> None = [];

    [Fact]
    public void A_placeholder_naming_a_trigger_program_nothing_serves_gets_it_from_Kiln()
    {
        string?[] placeholders =
        [
            """{"name":"Gem_gonemod_ghost","refs":{"equipment.program":"6b696c6e-6765-6d73-8000-000000000004"}}""",
            null,
            """{"name":"Gem_gonemod_data","program":"b199646f-31a1-495e-b3d1-d5fef2fcaab3"}""",  // a game program: resolves
            """{"name":"Gem_gonemod_kill","refs":{"equipment.program":"6B696C6E-6765-6D73-8000-000000000003"}}""",
        ];
        Assert.Equal([GemTriggerKind.Kill, GemTriggerKind.Backstab], GemPlaceholders.ToServe(placeholders, None, None).Select(p => p.Kind));
    }

    [Fact]
    public void A_trigger_program_a_registered_gem_or_Kiln_already_serves_is_not_served_again()
    {
        string?[] placeholders =
        [
            """{"refs":{"equipment.program":"6b696c6e-6765-6d73-8000-000000000003"}}""",
            """{"refs":{"equipment.program":"6b696c6e-6765-6d73-8000-000000000006"}}""",
        ];
        var kill = new HashSet<GemTriggerKind> { GemTriggerKind.Kill };
        Assert.Empty(GemPlaceholders.ToServe(placeholders, kill, new HashSet<GemTriggerKind> { GemTriggerKind.Passive }));
        Assert.Equal([GemTriggerKind.Passive], GemPlaceholders.ToServe(placeholders, kill, None).Select(p => p.Kind));
    }

    [Fact]
    public void Without_placeholders_Kiln_serves_nothing() =>
        Assert.Empty(GemPlaceholders.ToServe([], None, None));
}
