// SPDX-License-Identifier: MIT
using System.Reflection;
using System.Text.Json.Nodes;
using Xunit;

namespace Kiln.Blocks.Tests;

/// <summary>Gem wave G1 (Kiln.Abilities/Gems): the read API (catalog, equipped gems, sockets), the impact -> gem map
/// behind <see cref="GemTriggeredEvent"/>, the polled <see cref="GemSocketChangedEvent"/> and the <c>gems.*</c> rows,
/// over a fake <see cref="IGemWorld"/> (the live reads are kiln.items, kiln.gamedata, kiln.world: lab-checked).</summary>
[Collection("gems")]
public sealed class GemsBaseTests : IDisposable
{
    // ---- a fake game ----

    sealed class FakeWorld : IGemWorld
    {
        public List<ItemInfo> Catalog = [];
        public Dictionary<uint, GemData> Gamedata = new();
        public HashSet<uint> Mods = [];
        public long Key = 1;
        public int DataReads;
        public Dictionary<uint, List<GemImpactRow>> Equipment = new();
        public int ImpactReads;
        public Dictionary<uint, GemItemRead> Entities = new();           // by raw entity id
        public Dictionary<uint, List<GemItemRead>> Holders = new();       // by player
        public List<EntityId> PlayerList = [];
        public Dictionary<uint, uint> Parents = new();

        public IReadOnlyList<ItemInfo> Items() => Catalog;
        public long CatalogKey() => Key;
        public GemData? Data(ItemInfo item)
        {
            DataReads++;
            return Gamedata.GetValueOrDefault(item.Id.Value);
        }
        public IReadOnlySet<uint> ModItems() => Mods;
        public IReadOnlyList<GemImpactRow>? Impacts(EntityId holder)
        {
            ImpactReads++;
            return Equipment.GetValueOrDefault(holder.Value);
        }
        public IReadOnlyList<GemItemRead?> ReadEntities(IReadOnlyList<uint> pides) =>
            pides.Select(p => Entities.TryGetValue(p, out var r) ? r : (GemItemRead?)null).ToList();
        public GemItemRead? Read(ItemInstanceRef item) =>
            Entities.TryGetValue(item.Entity.Id.Value, out var r) && r.Item == item ? r : null;
        public IReadOnlyList<GemItemRead>? ReadHolder(EntityId holder) => Holders.GetValueOrDefault(holder.Value);
        public IReadOnlyList<EntityId> Players() => PlayerList;
        public EntityId ParentImpact(EntityId impact) => new(Parents.GetValueOrDefault(impact.Value));
    }

    static readonly ItemId Bleeding = new(0x11110001), Necro = new(0x11110002), ModGem = new(0x22220001), Sword = new(0x33330001),
        Ring = new(0x33330002);

    static ItemInfo Info(ItemId id, string name, ItemCategory cat, ItemRarity rarity = ItemRarity.Common, uint flags = 0) =>
        new(id, name, name + " shown", 1, cat, rarity, cat != ItemCategory.WeaponGem, 1, 105, 0, flags, Guid.NewGuid());

    static ItemInstanceRef Ref(uint entity, ItemId item) => new(new EntityRef(new EntityId(entity), 7), item);

    static GemItemRead Item(uint entity, ItemId item, ItemId gem = default, int gemLevel = 0, int level = 0) =>
        new(Ref(entity, item), gem, gemLevel, level);

    readonly FakeWorld _w = new();
    readonly IGemWorld _was = GemReads.World;
    readonly long _keyCheck = GemReads.KeyCheckMs;

    public GemsBaseTests()
    {
        GemReads.KeyCheckMs = 0;
        GemReads.Forget();
        GemWatch.Reset();
        _w.Catalog.AddRange([
            Info(Bleeding, "T1_Gem_GemOfBleeding", ItemCategory.WeaponGem, ItemRarity.Uncommon),
            Info(Necro, "T5_Gem_GemOfNecromancy", ItemCategory.WeaponGem, ItemRarity.Epic),
            Info(ModGem, "MyMod_Gem_Frostbite", ItemCategory.WeaponGem, ItemRarity.Rare),
            Info(new ItemId(0x44440001), "Blueprint_Gem", ItemCategory.WeaponGem, flags: 0x8),
            Info(Sword, "Sword", ItemCategory.Weapons),
            Info(Ring, "Ring", ItemCategory.Equipment)]);
        _w.Gamedata[Bleeding.Value] = new GemData(new Guid("4f7f2cc6-6a1e-4d7e-8b1b-111111111111"),
            new Dictionary<uint, GemConfigValue> { [0x5e34f09b] = new(0x5e34f09b, null, "ScaledSint32", 8, 2.6) }, false);
        _w.Gamedata[ModGem.Value] = new GemData(Guid.Empty, new Dictionary<uint, GemConfigValue>(), true);
        _w.Mods.Add(ModGem.Value);
    }

    public void Dispose()
    {
        GemReads.World = _was;
        GemReads.KeyCheckMs = _keyCheck;
        GemReads.Forget();
        GemWatch.Reset();
    }

    // ---- catalog ----

    [Fact]
    public void TheCatalogHoldsEveryGemItemWithItsProgramAndConfigsAndSaysWhichCameFromMods()
    {
        var all = GemReads.Catalog(_w);
        Assert.Equal([Bleeding, Necro, ModGem], all.Select(g => g.Item));  // not the blueprint, not other categories
        var b = all[0];
        Assert.Equal(("T1_Gem_GemOfBleeding", "T1_Gem_GemOfBleeding shown", ItemRarity.Uncommon, 1, 105), (b.Name, b.DisplayName, b.Rarity, b.LevelMin, b.LevelMax));
        Assert.Equal(new Guid("4f7f2cc6-6a1e-4d7e-8b1b-111111111111"), b.Program);
        Assert.Equal(8, b.Configs[0x5e34f09b].Value);
        Assert.False(b.FromMod);
        Assert.Equal(Guid.Empty, all[1].Program);  // kiln.gamedata does not know it: no program, no configs
        Assert.Empty(all[1].Configs);
        Assert.True(all[2].FromMod);
        Assert.Same(all[0], GemReads.Get(_w, Bleeding));
        Assert.Null(GemReads.Get(_w, Sword));
        Assert.Null(GemReads.Get(_w, ItemId.None));
    }

    [Fact]
    public void TheCatalogIsKeptUntilItsKeyChangesOrItIsOld()
    {
        var first = GemReads.Catalog(_w);
        var reads = _w.DataReads;
        Assert.Same(first, GemReads.Catalog(_w));
        Assert.Equal(reads, _w.DataReads);
        _w.Key++;  // a new item, a pack applied
        GemReads.KeyCheckMs = 60_000;
        Assert.Same(first, GemReads.Catalog(_w));  // the key is asked for at most every KeyCheckMs
        GemReads.KeyCheckMs = 0;
        Assert.NotSame(first, GemReads.Catalog(_w));
        var was = GemReads.MaxAgeMs;
        try
        {
            GemReads.MaxAgeMs = -1;  // a value edit the key did not see: read again when old
            var second = GemReads.Catalog(_w);
            Assert.NotSame(second, GemReads.Catalog(_w));
        }
        finally
        {
            GemReads.MaxAgeMs = was;
        }
    }

    // ---- gamedata JSON ----

    [Fact]
    public void AGemsProgramIsReadFromItsReferenceAndNoneIsEmpty()
    {
        Assert.Equal(new Guid("e9652fd8-2d7a-4ace-b913-eae47cf178b3"),
            GemJson.Program(JsonNode.Parse("""{"path":"equipment.program","type":"keen::ImpactProgramReference","value":"e9652fd8-2d7a-4ace-b913-eae47cf178b3","writable":true}""")));
        Assert.Equal(Guid.Empty, GemJson.Program(JsonNode.Parse("""{"path":"equipment.program","value":"none"}""")));
        Assert.Equal(new Guid("e9652fd8-2d7a-4ace-b913-eae47cf178b3"),
            GemJson.Program(JsonNode.Parse("""{"value":{"guid":"e9652fd8-2d7a-4ace-b913-eae47cf178b3"}}""")));
        Assert.Equal(Guid.Empty, GemJson.Program(null));
    }

    // kiln.gamedata's GetJson("impactValues.simple") and GetJson("impactValues.scaled") of the Gem of Bleeding, as the
    // lab (server 26430253, 2026-11-16, gems-test) logged them
    const string BleedingSimple = """
    {"path":"impactValues.simple","type":"keen::BlobArray\u003Ckeen::BlobVariant\u003Ckeen::impact::ImpactConfig\u003E\u003E","address":2260816920,"value":[{"$type":"BuffImpactConfig","locaTag":"none","isSigned":false,"valueFormat":"Normal","id":"136b905c-a359-47d0-8394-62f37799a733","configId":1524311620,"type":811144921,"configGuid":"136b905c-a359-47d0-8394-62f37799a733","objectId":"8402a416-18e9-4f40-9ee0-25cc69de928b","value":"e9fad497-c3f8-4741-9c50-64378084717d"},{"$type":"TimeImpactConfig","locaTag":"none","isSigned":true,"valueFormat":"Normal","id":"3ad8b5c9-b576-4fba-8eba-76a80d372512","configId":3793439602,"type":3756319748,"configGuid":"3ad8b5c9-b576-4fba-8eba-76a80d372512","objectId":"55b42543-8103-4a39-90e6-97070508cd02","value":0.5},{"$type":"FloatImpactConfig","locaTag":"none","isSigned":true,"valueFormat":"Normal","id":"cf8556df-77d6-4c36-a4ae-be1f9488f9bd","configId":2236524794,"type":3902764048,"configGuid":"cf8556df-77d6-4c36-a4ae-be1f9488f9bd","objectId":"3f32e48f-2486-4d6c-ba0c-9b35caaa4f7f","value":0.5}]}
    """;
    const string BleedingScaled = """
    {"path":"impactValues.scaled","type":"keen::BlobArray\u003Ckeen::BlobVariant\u003Ckeen::impact::ImpactConfig\u003E\u003E","address":2260816928,"value":[{"$type":"ScaledSint32ImpactConfig","locaTag":"e905eb03-5efd-44c1-af0e-5f0df235590c","isSigned":false,"valueFormat":"Normal","id":"8009e37f-297c-4a42-bc51-65a8f5c0cc8f","configId":1580527771,"type":2556031774,"configGuid":"8009e37f-297c-4a42-bc51-65a8f5c0cc8f","objectId":"794d7d6a-9f6f-4576-b132-862ed6e7b7a6","scaleFactor":2.5999999046325684,"function":"WeaponPowerCreep","source":{"sourceEntity":"Self","attributeRef":"bbf879a8-9af2-4516-830d-e201d7370f02"},"value":8},{"$type":"ScaledTimeImpactConfig","locaTag":"ce00b6c5-2ccf-413b-86fa-a73c059c245c","isSigned":false,"valueFormat":"Duration","id":"e397f834-f106-42e1-8ca0-d9216d966d96","configId":1073225515,"type":3756319748,"configGuid":"e397f834-f106-42e1-8ca0-d9216d966d96","objectId":"c297b942-b69a-49e6-b6f4-39637bb946e7","scaleFactor":0.5,"function":"WeaponPowerCreep","source":{"sourceEntity":"Self","attributeRef":"bbf879a8-9af2-4516-830d-e201d7370f02"},"value":6.0}]}
    """;

    [Fact]
    public void ConfigsAreReadFromTheRawListsWithTheirIdsKindsValuesAndScaling()
    {
        var aliases = new Dictionary<uint, string> { [0x5adb2644] = "Buff1", [0x5e34f09b] = "Scaled_Damage_Cut" };
        var c = GemJson.Configs(JsonNode.Parse(BleedingSimple), JsonNode.Parse(BleedingScaled), id => aliases.GetValueOrDefault(id));
        Assert.Equal(5, c.Count);
        Assert.Equal(new GemConfigValue(0x5adb2644, "Buff1", "BuffImpactConfig", 0, null) { Text = "e9fad497-c3f8-4741-9c50-64378084717d" }, c[0x5adb2644]);
        Assert.Equal(new GemConfigValue(0xe21b4b72, null, "TimeImpactConfig", 0.5, null), c[0xe21b4b72]);    // seconds
        Assert.Equal(new GemConfigValue(0x854ea8fa, null, "FloatImpactConfig", 0.5, null), c[0x854ea8fa]);
        Assert.Equal(("Scaled_Damage_Cut", "ScaledSint32ImpactConfig", 8.0), (c[0x5e34f09b].Alias, c[0x5e34f09b].Kind, c[0x5e34f09b].Value));
        Assert.Equal(2.6, c[0x5e34f09b].ScaleFactor!.Value, 5);
        Assert.Equal((6.0, 0.5), (c[0x3ff81f2b].Value, c[0x3ff81f2b].ScaleFactor!.Value));
        Assert.Empty(GemJson.Configs(null, JsonNode.Parse("""{"value":[{"$type":"FloatImpactConfig","value":1}]}"""), _ => null));  // no id: left out
        Assert.Equal(1, GemJson.Configs(JsonNode.Parse("""[{"$type":"FloatImpactConfig","configId":7,"value":1}]"""), null, _ => null).Count);  // a bare list
    }

    [Fact]
    public void TheVocabularyGivesEachIdItsAlias()
    {
        var d = GemJson.AliasIndex("""[{"id":"0x5e34f09b","names":["Damage","Scaled_Damage_Cut"],"alias":"Scaled_Damage_Cut","programs":40},{"id":"0x3ff81f2b","names":["DebuffDuration"],"programs":5},{"id":"0x5adb2644","alias":"Buff1"}]""");
        Assert.Equal("Scaled_Damage_Cut", d[0x5e34f09b]);
        Assert.Equal("Buff1", d[0x5adb2644]);
        Assert.False(d.ContainsKey(0x3ff81f2b));  // no alias: kiln.gamedata writes 0x3ff81f2b
        Assert.Empty(GemJson.AliasIndex("not json"));
    }

    // ---- equipment ----

    [Fact]
    public void TheServerLayoutFixtureIsTheReflectionDumpOfItsBuild()
    {
        var root = AppContext.BaseDirectory;
        while (root is not null && !Directory.Exists(Path.Combine(root, "sigdb", "types", "server"))) root = Path.GetDirectoryName(root);
        Assert.NotNull(root);
        var dump = JsonNode.Parse(File.ReadAllText(Path.Combine(root!, "sigdb", "types", "server",
            "f0d824be7fcd48ebfe2458ff5c28da26060c2074e3ec1fcac587e5a1d508aeb2.json")))!;
        JsonNode T(string name) => dump["types"]!.AsArray().First(t => (string?)t!["name"] == name)!;
        int Off(JsonNode t, string f) => (int)t["fields"]!.AsArray().First(x => (string?)x!["name"] == f)!["offset"]!;
        var eq = T("Equipment");
        var imp = T("ItemImpact");
        var inst = T("ItemInstanceInfo");
        var l = EquipmentLayout.Server26430253;
        Assert.Equal(Off(eq, "equipmentImpacts"), l.ImpactsOffset);
        Assert.Equal((int)imp["size"]!, l.Stride);
        Assert.Equal("keen::StaticArray<keen::ecs::ItemImpact,11>", (string?)eq["fields"]!.AsArray().First(x => (string?)x!["name"] == "equipmentImpacts")!["type"]);
        Assert.Equal(11, l.Count);
        Assert.Equal(Off(imp, "itemInstance") + Off(inst, "pide"), l.PideOffset);
        Assert.Equal(Off(imp, "itemInstance") + Off(inst, "itemId"), l.ItemIdOffset);
        Assert.Equal(Off(imp, "weaponGemImpactEntityId"), l.GemImpactOffset);
        Assert.Equal(Off(imp, "weaponGemEntityId"), l.GemEntityOffset);
    }

    [Fact]
    public void EquipmentBytesGiveEachSlotsCarrierAndGemImpact()
    {
        var l = EquipmentLayout.Server26430253;
        var b = new byte[1380];
        void Put(int slot, int off, uint v) => BitConverter.TryWriteBytes(b.AsSpan(l.ImpactsOffset + slot * l.Stride + off, 4), v);
        Put(0, l.PideOffset, 2001); Put(0, l.ItemIdOffset, Sword.Value); Put(0, l.GemImpactOffset, 5001); Put(0, l.GemEntityOffset, 3001);
        Put(7, l.PideOffset, 2002); Put(7, l.ItemIdOffset, Ring.Value);
        var rows = l.Parse(b);
        Assert.Equal(11, rows.Count);
        Assert.Equal(new GemImpactRow(0, 2001, Sword, new EntityId(5001), new EntityId(3001)), rows[0]);
        Assert.Equal(new GemImpactRow(7, 2002, Ring, EntityId.None, EntityId.None), rows[7]);
        Assert.Equal(3, l.Parse(new byte[8 + 3 * 72 + 10]).Count);  // a short component: only whole slots
    }

    void Equip(uint player, params GemImpactRow[] rows)
    {
        var all = Enumerable.Range(0, 11).Select(i => new GemImpactRow(i, 0, ItemId.None, EntityId.None, EntityId.None)).ToList();
        foreach (var r in rows) all[r.Slot] = r;
        _w.Equipment[player] = all;
    }

    [Fact]
    public void EquippedListsTheSlotsWhoseGemImpactRunsWithCarrierGemAndLevel()
    {
        Equip(1, new GemImpactRow(0, 2001, Sword, new EntityId(5001), new EntityId(3001)),
            new GemImpactRow(7, 2002, Ring, new EntityId(5002), new EntityId(3002)),
            new GemImpactRow(2, 2003, Ring, EntityId.None, EntityId.None));  // an item, no gem impact
        _w.Entities[2001] = Item(2001, Sword, Bleeding, 12);
        _w.Entities[3002] = Item(3002, Necro, level: 30);  // slot 7's carrier is not readable: its gem entity says
        var list = GemReads.Equipped(_w, new EntityId(1));
        Assert.Equal(2, list.Count);
        Assert.Equal(new EquippedGem(new EntityId(1), GemImpactSlot.Held, new SocketedGem(Ref(2001, Sword), Bleeding, 12), new EntityId(5001)), list[0]);
        Assert.Equal(new EquippedGem(new EntityId(1), GemImpactSlot.Ring1, new SocketedGem(ItemInstanceRef.None, Necro, 30), new EntityId(5002)), list[1]);
        Assert.Empty(GemReads.Equipped(_w, new EntityId(2)));  // no Equipment
        Assert.Empty(GemReads.Equipped(_w, EntityId.None));
    }

    [Fact]
    public void SocketReadsTheCarriersGem()
    {
        _w.Entities[2001] = Item(2001, Sword, Bleeding, 4);
        _w.Entities[2002] = Item(2002, Sword);
        Assert.Equal(new SocketedGem(Ref(2001, Sword), Bleeding, 4), GemReads.Socket(_w, Ref(2001, Sword)));
        Assert.Null(GemReads.Socket(_w, Ref(2002, Sword)));  // empty or no socket
        Assert.Null(GemReads.Socket(_w, Ref(2009, Sword)));  // gone
    }

    // ---- GemTriggeredEvent ----

    static ImpactDamageEvent Damage(uint self, uint root, uint target, ulong step)
    {
        var args = new uint[15];
        var values = new uint[15];
        args[1] = 0xFFFF0000; values[1] = root;
        args[2] = 0xFFFF0001; values[2] = target;
        var offs = Enumerable.Repeat((byte)0xFF, 15).ToArray();
        return new ImpactDamageEvent(new NativeSnapshot
        {
            Hash = 0xb04d0e7a, Impact = new ImpactRef(77, 0x1234, new EntityId(self), new EntityId(root)), Step = step,
            Args = args, Values = values, Offsets = offs, Sizes = new byte[15], Payload = new byte[136],
        });
    }

    [Fact]
    public void ANativeOfAGemImpactOrOfAnImpactItSpawnedIsThatGems()
    {
        Equip(1, new GemImpactRow(0, 2001, Sword, new EntityId(5001), new EntityId(3001)));
        _w.Entities[2001] = Item(2001, Sword, Bleeding, 12);
        _w.Parents[6001] = 5001;           // spawned by the gem impact
        _w.Parents[6002] = 6001;           // and one below it
        _w.Parents[6100] = 4000;           // the weapon's own impact: no gem above
        var gem = GemTriggers.Find(_w, new ImpactRef(1, 2, new EntityId(5001), new EntityId(1)), 10);
        Assert.Equal(GemImpactSlot.Held, gem!.Slot);
        Assert.Equal(new EntityId(5001), GemTriggers.Find(_w, new ImpactRef(1, 2, new EntityId(6002), new EntityId(1)), 10)!.Impact);
        Assert.Null(GemTriggers.Find(_w, new ImpactRef(1, 2, new EntityId(6100), new EntityId(1)), 10));
        Assert.Null(GemTriggers.Find(_w, new ImpactRef(1, 2, new EntityId(5001), EntityId.None), 10));
        Assert.Equal(1, _w.ImpactReads);  // one Equipment read per root and step
        GemTriggers.Find(_w, new ImpactRef(1, 2, new EntityId(5001), new EntityId(1)), 11);
        Assert.Equal(2, _w.ImpactReads);
    }

    [Fact]
    public void AGemsDamageIsPostedAsGemTriggeredWithItsTarget()
    {
        Equip(1, new GemImpactRow(0, 2001, Sword, new EntityId(5001), new EntityId(3001)));
        _w.Entities[2001] = Item(2001, Sword, Bleeding, 12);
        GemReads.World = _w;
        var seen = new List<GemTriggeredEvent>();
        using var sub = Events.On<GemTriggeredEvent>(seen.Add);
        GemTriggers.Deliver(Damage(5001, 1, 9000, 20), 20);
        GemTriggers.Deliver(Damage(4000, 1, 9000, 20), 20);  // the weapon's own hit: not a gem's
        var e = Assert.Single(seen);
        Assert.Equal((new EntityId(1), new EntityId(9000), 20ul, Bleeding, 12), (e.Player, e.Target, e.Step, e.Gem.Socket.Gem, e.Gem.Socket.Level));
        Assert.IsType<ImpactDamageEvent>(e.Native);
        Assert.True(e.Involves(new EntityId(9000)) && e.Involves(new EntityId(1)) && !e.Involves(new EntityId(5)));
    }

    [Fact]
    public void ASubscriberOfGemTriggeredTurnsOnTheNativeEvents()
    {
        var interest = typeof(AbilitiesDispatch).GetMethod("Interest", BindingFlags.Static | BindingFlags.NonPublic)!;
        var bit = AbilitiesNative.Bit(AbilitiesNative.EvNative);
        using (Events.On<GemTriggeredEvent>(_ => { }))
            Assert.NotEqual(0u, (uint)interest.Invoke(null, null)! & bit);
    }

    // ---- GemSocketChangedEvent ----

    static GemSnapshot Snap(Func<ItemId, bool> isGem, params GemItemRead[] items) => GemSnapshot.Of(items, isGem);

    bool IsGem(ItemId i) => GemReads.IsGem(_w, i);

    [Fact]
    public void SocketChangesAreToldApart()
    {
        var swordA = Ref(2001, Sword);
        var before = Snap(IsGem, Item(2001, Sword), Item(2002, Sword, Bleeding, 5), Item(2003, Sword, Bleeding, 5),
            Item(2004, Sword, Necro, 9), Item(2005, Sword, Bleeding, 3), Item(2006, Sword, Bleeding, 2), Item(2007, Sword, Necro, 1),
            Item(2008, Sword));
        var after = Snap(IsGem,
            Item(2001, Sword, Bleeding, 1),      // socketed
            Item(2002, Sword),                   // unsocketed: a loose Bleeding came back
            Item(2003, Sword, Bleeding, 6),      // upgraded
            Item(2004, Sword, Bleeding, 1),      // swapped
            Item(2007, Sword),                   // recycled: no Necro came back
            Item(2008, Sword),                   // never had one
            Item(3001, Bleeding, level: 5),      // the loose gem
            Item(2010, Sword, Necro, 4));        // first seen with a gem: not a change
        // 2005 moved to a chest with its gem (still exists), 2006 was destroyed with it
        _w.Entities[2005] = Item(2005, Sword, Bleeding, 3);
        var diffs = GemWatch.Diff(before, after, _w.Read);
        Assert.Equal([
            new GemSocketDiff(swordA, null, new SocketedGem(swordA, Bleeding, 1), GemSocketChange.Socketed),
            new GemSocketDiff(Ref(2002, Sword), new SocketedGem(Ref(2002, Sword), Bleeding, 5), null, GemSocketChange.Unsocketed),
            new GemSocketDiff(Ref(2003, Sword), new SocketedGem(Ref(2003, Sword), Bleeding, 5), new SocketedGem(Ref(2003, Sword), Bleeding, 6), GemSocketChange.LevelChanged),
            new GemSocketDiff(Ref(2004, Sword), new SocketedGem(Ref(2004, Sword), Necro, 9), new SocketedGem(Ref(2004, Sword), Bleeding, 1), GemSocketChange.Socketed),
            new GemSocketDiff(Ref(2006, Sword), new SocketedGem(Ref(2006, Sword), Bleeding, 2), null, GemSocketChange.Removed),
            new GemSocketDiff(Ref(2007, Sword), new SocketedGem(Ref(2007, Sword), Necro, 1), null, GemSocketChange.Removed),
        ], diffs);
    }

    [Fact]
    public void ThePollReadsTwoPlayersAStepAndReportsFromTheSecondReadOn()
    {
        _w.PlayerList.AddRange([new EntityId(1), new EntityId(2), new EntityId(3)]);
        foreach (var p in new uint[] { 1, 2, 3 }) _w.Holders[p] = [Item(2000 + p, Sword)];
        Assert.Empty(GemWatch.Step(_w, 1, post: false));  // players 1, 2: first sight
        Assert.Empty(GemWatch.Step(_w, 2, post: false));  // 3, then 1 again: nothing changed
        _w.Holders[2] = [Item(2002, Sword, Bleeding, 3)];
        _w.Holders[3] = [Item(2003, Sword, Necro, 1)];
        var found = GemWatch.Step(_w, 3, post: false);     // players 2, 3
        Assert.Equal([(new EntityId(2), GemSocketChange.Socketed), (new EntityId(3), GemSocketChange.Socketed)],
            found.Select(f => (f.Owner, f.Diff.Change)));
        _w.Holders.Remove(1);                               // not initialized now: skipped, its snapshot kept
        Assert.Empty(GemWatch.Step(_w, 4, post: false));
    }

    [Fact]
    public void WithNoSubscriberThePollReadsNothing()
    {
        _w.PlayerList.Add(new EntityId(1));
        _w.Holders[1] = [Item(2001, Sword)];
        GemReads.World = _w;
        Assert.Empty(GemWatch.Step(_w, 1, post: true));
        _w.Holders[1] = [Item(2001, Sword, Bleeding, 1)];
        var seen = new List<GemSocketChangedEvent>();
        using (Events.On<GemSocketChangedEvent>(seen.Add))
        {
            GemWatch.Step(_w, 2, post: true);  // first sight (nothing was kept while nobody listened)
            Assert.Empty(seen);
            _w.Holders[1] = [Item(2001, Sword, Bleeding, 2)];
            GemWatch.Step(_w, 3, post: true);
            var e = Assert.Single(seen);
            Assert.Equal((new EntityId(1), GemSocketChange.LevelChanged, 3ul), (e.Owner, e.Change, e.Step));
            Assert.True(e.Involves(new EntityId(2001)));
        }
    }

    // ---- capability rows ----

    static Capability Row(string name, CapabilityState s, CapabilityReason r = CapabilityReason.None, string text = "") => new(name, s, r, text, "test");

    [Fact]
    public void GemsReadFollowsItemInstancesAndTheEquipmentLayout()
    {
        var on = Row("items.instances", CapabilityState.Available);
        Assert.Equal(CapabilityState.Available, GemsCaps.DecideRead(true, null, on, true, true, true, true).State);
        Assert.Contains("without kiln.gamedata", GemsCaps.DecideRead(true, null, on, true, true, true, false).Text);
        Assert.Equal((CapabilityState.Unavailable, CapabilityReason.ServiceMissing),
            Pair(GemsCaps.DecideRead(false, null, null, true, true, true, true)));
        var off = Row("items.instances", CapabilityState.Unavailable, CapabilityReason.ServiceOlder, "v3");
        var r = GemsCaps.DecideRead(true, null, off, true, true, true, true);
        Assert.Equal((CapabilityState.Unavailable, CapabilityReason.ServiceOlder, "needs items.instances: v3"), r);
        Assert.Equal((CapabilityState.Unavailable, CapabilityReason.SymbolMissing),
            Pair(GemsCaps.DecideRead(true, null, on, true, false, true, true)));
        Assert.Equal((CapabilityState.Unavailable, CapabilityReason.ServiceMissing),
            Pair(GemsCaps.DecideRead(true, null, on, false, null, true, true)));
        Assert.Equal((CapabilityState.Pending, CapabilityReason.Session),
            Pair(GemsCaps.DecideRead(true, null, Row("items.instances", CapabilityState.Pending, CapabilityReason.Session, "no world"), true, true, true, true)));
    }

    [Fact]
    public void GemsTriggersNeedsGemsReadAndTheNativeEvents()
    {
        Assert.Equal(CapabilityState.Available, GemsCaps.DecideTriggers(CapabilityState.Available, "", true, null).State);
        Assert.Equal((CapabilityState.Unavailable, CapabilityReason.ServiceMissing),
            Pair(GemsCaps.DecideTriggers(CapabilityState.Available, "", false, null)));
        var nat = Row("abilities.natives", CapabilityState.Unavailable, CapabilityReason.SymbolMissing, "symbol x");
        Assert.Equal((CapabilityState.Unavailable, CapabilityReason.SymbolMissing, "needs abilities.natives: symbol x"),
            GemsCaps.DecideTriggers(CapabilityState.Available, "", true, nat));
        Assert.Equal(CapabilityState.Pending, GemsCaps.DecideTriggers(CapabilityState.Pending, "no world", true, null).State);
        Assert.Contains("gems.read", GemsCaps.DecideTriggers(CapabilityState.Unavailable, "x", true, null).Text);
    }

    [Fact]
    public void AnotherServicesMissingCapabilityIsReportedAsGemsRead()
    {
        var e = GemsCaps.AsRead(new KilnUnavailableException("items.instances", "off"));
        Assert.Equal("gems.read", e.Capability);
        Assert.Contains("items.instances", e.Message);
        var same = new KilnUnavailableException("gems.read", "x");
        Assert.Same(same, GemsCaps.AsRead(same));
    }

    static (CapabilityState, CapabilityReason) Pair((CapabilityState S, CapabilityReason R, string T) x) => (x.S, x.R);
}
