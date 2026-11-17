// SPDX-License-Identifier: MIT
// GitHub issue 61 (0.2.0-dev.29.15) offline, on the fake kiln.gemcraft of GemCraftingTests: separate gem and carrier
// holders, payers and the return in the request; offered chest gems (which slots an offer lists, the rows and payer
// counts a client gets, the wire, the capability row, the forge session's hello and the client's outcome). What the game
// does is the lab test (mods/tests/gemforge-chest-check); the native logic is native/services/items/tests/gems_test.cpp.
using Kiln;
using Kiln.TestKit;
using Xunit;

namespace Kiln.AbilitiesTests;

public sealed class GemSourcesTests : IDisposable
{
    private static readonly ItemId Sword = new(0x4c0d3e02), Bleeding = new(0x5adb2644), Essence = new(0x2b0cf1d5);
    private readonly GemCraftingTests _fixture = new();
    private GemCraftingTests.Fake Fake => _fixture.Backend;
    private static readonly EntityId Player = new(1), Chest = new(5000), Forge = new(3300);

    public GemSourcesTests() => GemSources.ResetForTests();

    public void Dispose()
    {
        GemSources.ResetForTests();
        _fixture.Dispose();
    }

    private static InventorySlot Slot(SlotId at, ItemId item, uint entity, int count = 1, int level = 1) =>
        new(at, item, count, entity == 0 ? EntityId.None : new EntityId(entity), level, ItemRarity.Common, default, 0, true, false, false, 0);

    private static ItemInstanceRef Ref(uint entity, ItemId item) => new(new EntityRef(new EntityId(entity), 0), item);

    // The calls return a task but throw before one exists: run them as a plain call.
    private static void Call(Func<Task> f) => _ = f();

    // ---- part 1: two holders in one request ----

    [Fact]
    public void TheGemAndTheCarrierAreLookedUpInTheirOwnHolders()
    {
        var chestSlot = SlotId.Of(new EntityId(5001), 4);
        var chest = new[] { Slot(chestSlot, Bleeding, 2560) };
        var backpack = new[] { Slot(SlotId.Of(new EntityId(2546), 0), Sword, 2554) };
        var (r, refusal) = GemCrafting.Build(GemCraftAction.Socket, Player, Ref(2554, Sword), Ref(2560, Bleeding), Forge,
            new GemCraftOptions { GemHolder = Chest }, chest, backpack, id => id == Bleeding);
        Assert.Null(refusal);
        Assert.Equal((2554u, chestSlot.Value, 2560u), (r.Carrier, r.GemSlot, r.Gem));
        Assert.Equal((Chest.Value, 0u, 0u), (r.GemHolder, r.CarrierHolder, r.Holder));
        Assert.False(GemCrafting.Routed(r));
        // the gem in the carrier's holder (the backpack) is not where the request says it is
        var (_, wrong) = GemCrafting.Build(GemCraftAction.Socket, Player, Ref(2554, Sword), Ref(2560, Bleeding), Forge,
            new GemCraftOptions { GemHolder = Chest }, backpack, backpack, id => id == Bleeding);
        Assert.Equal(GemCraftStatus.NotFound, wrong!.Status);
        // Upgrade of a loose gem in the chest: by its slot there
        var (up, _) = GemCrafting.Build(GemCraftAction.Upgrade, Player, Ref(2560, Bleeding), default, Forge,
            new GemCraftOptions { GemHolder = Chest }, chest, backpack, id => id == Bleeding);
        Assert.Equal((chestSlot.Value, 0u), (up.GemSlot, up.Carrier));
        // a carrier in a chest of its own
        var (c, _) = GemCrafting.Build(GemCraftAction.Upgrade, Player, Ref(2554, Sword), default, Forge,
            new GemCraftOptions { CarrierHolder = Chest }, chest, backpack, id => id == Bleeding);
        Assert.Equal((2554u, 0ul, Chest.Value), (c.Carrier, c.GemSlot, c.CarrierHolder));
    }

    // ---- part 3: payers and the return ----

    [Fact]
    public void PayersAndTheReturnBecomeTheRequestsFields()
    {
        var slots = Array.Empty<InventorySlot>();
        var (chest, _) = GemCrafting.Build(GemCraftAction.Upgrade, Player, Ref(2554, Sword), default, Forge,
            new GemCraftOptions { PayFrom = GemPayer.From(Chest) }, slots, _ => false);
        Assert.Equal(((uint)GemPayerKind.Container, Chest.Value), (chest.PayFrom, chest.PayHolder));
        Assert.True(GemCrafting.Routed(chest));
        var (bases, _) = GemCrafting.Build(GemCraftAction.Socket, Player, Ref(2554, Sword), default, Forge,
            new GemCraftOptions { PayFrom = GemPayer.PlayerThenBaseStorage }, slots, _ => false);
        Assert.Equal(((uint)GemPayerKind.PlayerThenBaseStorage, 0u), (bases.PayFrom, bases.PayHolder));
        var (back, _) = GemCrafting.Build(GemCraftAction.Unsocket, Player, Ref(2554, Sword), default, Forge,
            new GemCraftOptions { ReturnTo = Chest }, slots, _ => false);
        Assert.Equal(Chest.Value, back.ReturnTo);
        Assert.True(GemCrafting.Routed(back));
        // free pays nothing: its PayFrom routes nothing; Recycle pays nothing either
        var (free, _) = GemCrafting.Build(GemCraftAction.Upgrade, Player, Ref(2554, Sword), default, Forge,
            new GemCraftOptions { PayFrom = GemPayer.BaseStorage, Cost = GemCost.Free }, slots, _ => false);
        Assert.False(GemCrafting.Routed(free));
        var (recycle, _) = GemCrafting.Build(GemCraftAction.Recycle, Player, Ref(2554, Sword), default, Forge,
            new GemCraftOptions { PayFrom = GemPayer.BaseStorage }, slots, _ => false);
        Assert.False(GemCrafting.Routed(recycle));
        // a replaced price is the player's: with another payer it is refused
        Assert.Throws<ArgumentException>(() => GemCrafting.Build(GemCraftAction.Upgrade, Player, Ref(2554, Sword), default, Forge,
            new GemCraftOptions { PayFrom = GemPayer.From(Chest), Cost = new GemCost(Essence, 3) }, slots, _ => false));
        Assert.Throws<ArgumentException>(() => GemPayer.From(EntityId.None));
        Assert.Equal("player", GemPayer.Player.ToString());
        Assert.Equal("base storage", GemPayer.BaseStorage.ToString());
        Assert.True(default(GemPayer).IsPlayer);
    }

    [Fact]
    public void APayerOtherThanThePlayerNeedsGemsPayers()
    {
        using var kiln = new KilnHarness();
        kiln.Capabilities.Override(Gems.CraftCapability, true);
        var off = kiln.Run(() => Assert.Throws<KilnUnavailableException>(() => Call(() => Gems.Upgrade(Player, Ref(2560, Bleeding), Forge,
            new GemCraftOptions { PayFrom = GemPayer.BaseStorage }))));
        Assert.Equal(Gems.PayersCapability, off.Capability);
        Assert.Empty(Fake.Submitted);
        kiln.Capabilities.Override(Gems.PayersCapability, true);
        _ = kiln.Run(() => Gems.Upgrade(Player, Ref(2560, Bleeding), Forge, new GemCraftOptions { PayFrom = GemPayer.BaseStorage, ReturnTo = Chest }));
        var (r, _) = Assert.Single(Fake.Submitted);
        Assert.Equal(((uint)GemPayerKind.BaseStorage, Chest.Value), (r.PayFrom, r.ReturnTo));
    }

    // ---- part 2: offers ----

    [Fact]
    public void AnOfferListsTheLooseGemsOfTheContainerOnly()
    {
        var slots = new[]
        {
            Slot(SlotId.Of(new EntityId(5001), 0), Bleeding, 2560, level: 3),
            Slot(SlotId.Of(new EntityId(5001), 1), Sword, 2554),                 // a weapon, not a gem
            Slot(SlotId.Of(new EntityId(5001), 2), Essence, 0, count: 40),       // a stack
            Slot(SlotId.Of(new EntityId(5002), 0), Bleeding, 0),                 // a gem without its item entity
            Slot(SlotId.Of(new EntityId(5002), 1), Bleeding, 2561, level: 5),
        };
        var o = new GemSourceOptions { PayFrom = GemPayer.From(Chest), ReturnTo = Chest };
        var e = GemSources.Entries(Player, Chest, slots, o, GemSources.LifetimeMs(o.Lifetime), id => id == Bleeding);
        Assert.Equal(2, e.Count);
        Assert.Equal((2560u, 3u, SlotId.Of(new EntityId(5001), 0).Value), (e[0].Gem, e[0].Level, e[0].Slot));
        Assert.Equal((2561u, 5u), (e[1].Gem, e[1].Level));
        // review 2026-11-17: lifetimes are real time (a dedicated server's step rate is not fixed)
        Assert.All(e, x => Assert.Equal((Player.Value, Chest.Value, 1u, Chest.Value, Chest.Value, 30_000u),
            (x.Player, x.Holder, x.PayFrom, x.PayHolder, x.ReturnTo, x.LifetimeMs)));
        Assert.Equal(1, GemSources.LifetimeMs(TimeSpan.FromMilliseconds(1)));
        Assert.Equal(120_000, GemSources.LifetimeMs(TimeSpan.FromSeconds(120)));
        Assert.Throws<ArgumentException>(() => GemSources.LifetimeMs(TimeSpan.Zero));
        Assert.Throws<ArgumentException>(() => GemSources.LifetimeMs(TimeSpan.FromSeconds(121)));
    }

    private static InventoryContents Contents(bool initialized, byte flags, params InventorySlot[] slots) =>
        new(Chest, initialized, flags, slots.Length, slots.Length, [], slots);

    [Fact]
    public void OfferChestGemsMintsTokensAndRefusesWhatIsNoContainer()
    {
        using var kiln = new KilnHarness();
        kiln.Capabilities.Override(Gems.CraftCapability, true);
        var gem = Slot(SlotId.Of(new EntityId(5001), 0), Bleeding, 2560, level: 3);
        var offer = kiln.Run(() => GemSources.Offer(Player, Chest, new GemSourceOptions(), Contents(true, 0, gem), id => id == Bleeding));
        Assert.True(offer.Ok);
        var g = Assert.Single(offer.Gems);
        Assert.Equal((0x7000ul, 3), (g.Token, g.Level));
        Assert.Single(Fake.Offers);
        Assert.False(kiln.Run(() => GemSources.Offer(Player, Chest, new GemSourceOptions(), Contents(false, 0, gem), id => id == Bleeding)).Ok);
        var player = kiln.Run(() => GemSources.Offer(Player, Chest, new GemSourceOptions(), Contents(true, 1, gem), id => id == Bleeding));
        Assert.Contains("player", player.Refusal);
        kiln.Run(() => Assert.Throws<ArgumentException>(() => GemSources.Offer(Player, Player, new GemSourceOptions(), null, null)));
        // a payer other than the player needs gems.payers
        var off = kiln.Run(() => Assert.Throws<KilnUnavailableException>(() =>
            GemSources.Offer(Player, Chest, new GemSourceOptions { PayFrom = GemPayer.From(Chest) }, Contents(true, 0, gem), id => id == Bleeding)));
        Assert.Equal(Gems.PayersCapability, off.Capability);
        // kiln.gemcraft refuses (too many): nothing offered, a refusal
        Fake.OfferResult = GemcraftNative.BufferTooSmall;
        Assert.Contains("256", kiln.Run(() => GemSources.Offer(Player, Chest, new GemSourceOptions(), Contents(true, 0, gem), id => id == Bleeding)).Refusal);
        // the public call checks the row by name (here kiln.net is missing: the players' clients would never get the offers)
        var row = kiln.Run(() => Assert.Throws<KilnUnavailableException>(() => Gems.OfferChestGems(Player, Chest)));
        Assert.Equal(Gems.SourcesCapability, row.Capability);
        // an older kiln.gemcraft: the capability is off by name
        Fake.OfferResult = 0;
        Fake.ExtrasResult = null;
        var older = kiln.Run(() => Assert.Throws<KilnUnavailableException>(() => GemSources.Offer(Player, Chest, new GemSourceOptions(), Contents(true, 0, gem), id => id == Bleeding)));
        Assert.Equal(Gems.SourcesCapability, older.Capability);
    }

    [Fact]
    public void TheClientGetsRowsWithPayerGroupsCountsWithoutThePlayerAndLifetimes()
    {
        GemcraftNative.Source Src(ulong token, uint slotPage, byte index, uint payFrom, uint payHolder, ulong expires = 50_000) => new()
        {
            Player = 1, Holder = 5000, Slot = ((ulong)index << 32) | slotPage, Gem = 2560 + (uint)token, Item = Bleeding.Value, Level = 4,
            PayFrom = payFrom, PayHolder = payHolder, LifetimeMs = 30_000, Rarity = 2, Token = token, ExpiresMs = expires,
        };
        var live = new[]
        {
            Src(1, 5001, 3, 0, 0),
            Src(2, 5001, 4, (uint)GemPayerKind.Container, 5000),
            Src(3, 5002, 0, (uint)GemPayerKind.Container, 5000),
            Src(4, 5002, 1, (uint)GemPayerKind.PlayerThenBaseStorage, 0),
            Src(5, 5002, 2, 0, 0, expires: 19_999),
        };
        var counts = new Dictionary<(uint, uint), uint>
        {
            [(5000, Essence.Value)] = 40, [(6000, Essence.Value)] = 7, [(1, Essence.Value)] = 99, [(5000, 7u)] = 1000,
        };
        var m = GemSources.Rows(live, Essence.Value, 1, nowMs: 20_000,
            payersOf: s => s.PayFrom == (uint)GemPayerKind.Container ? [5000u] : [1u, 6000u],
            countIn: (e, item) => counts.TryGetValue((e, item), out var n) ? n : 0);
        Assert.Equal(4, m.Rows.Length);  // the fifth lapsed (real time)
        Assert.Equal((5001u, (byte)3, (byte)0), (m.Rows[0].Page, m.Rows[0].Slot, m.Rows[0].Group));
        Assert.Equal(((byte)1, (byte)1, (byte)2), (m.Rows[1].Group, m.Rows[2].Group, m.Rows[3].Group));  // one group per payer
        Assert.Equal(30_000u, m.Rows[0].LifetimeMs);  // expires at 50 000 ms, now 20 000
        // review 2026-11-17: the counts are of the forge's own upgrade material only (what containers pay in)
        Assert.Equal(2, m.Payers.Length);
        Assert.Equal(new GemForgeOfferPayer(1, false, Essence.Value, 40), m.Payers[0]);
        Assert.Equal(new GemForgeOfferPayer(2, true, Essence.Value, 7), m.Payers[1]);  // the player's own 99 are the client's own count
        // the forge's material not known (no forge near, not read yet): rows, no counts
        Assert.Empty(GemSources.Rows(live, 0, 1, 20_000, _ => [5000u], (_, _) => 40).Payers);
    }

    [Fact]
    public void TheWireRoundTrips()
    {
        var w = new NetWriter();
        GemForgeHello.Encode(new GemForgeHello(true, [Essence.Value, 7]), w);
        var r = new NetReader(w.WrittenSpan);
        var h = GemForgeHello.Decode(ref r);
        Assert.True(h.Open);
        Assert.Equal(new[] { Essence.Value, 7u }, h.Wants);
        w = new NetWriter();
        var offers = new GemForgeOffers([new GemForgeOfferRow(5001, 3, Bleeding.Value, 2560, 4, 2, 1, 0xdead_beef_0000_0001, 29_000)],
            [new GemForgeOfferPayer(1, true, Essence.Value, 40)]);
        GemForgeOffers.Encode(offers, w);
        r = new NetReader(w.WrittenSpan);
        var back = GemForgeOffers.Decode(ref r);
        Assert.Equal(offers.Rows, back.Rows);
        Assert.Equal(offers.Payers, back.Payers);
        w = new NetWriter();
        GemForgeOutcome.Encode(new GemForgeOutcome(9, 2, "the payers cannot pay the forge's cost"), w);
        r = new NetReader(w.WrittenSpan);
        Assert.Equal(new GemForgeOutcome(9, 2, "the payers cannot pay the forge's cost"), GemForgeOutcome.Decode(ref r));
        // a hostile hello cannot ask for more than 8 materials
        w = new NetWriter();
        w.Bool(true).Byte(200);
        for (var i = 0; i < 200; i++) w.UInt32((uint)i + 1);
        r = new NetReader(w.WrittenSpan);
        Assert.Equal(8, GemForgeHello.Decode(ref r).Wants.Length);
    }

    [Fact]
    public void TheSourcesRowFollowsTheForgeActionsAndTheService()
    {
        var on = (GemcraftNative.HasPayers | GemcraftNative.HasSources, "");
        Assert.Equal(CapabilityState.Available, GemsCaps.DecideSources(null, true, on, true).State);
        Assert.Equal(CapabilityReason.ServiceMissing, GemsCaps.DecideSources(null, false, on, true).Reason);
        Assert.Equal(CapabilityReason.ServiceOlder, GemsCaps.DecideSources(null, true, null, true).Reason);
        Assert.Equal(CapabilityReason.ServiceMissing, GemsCaps.DecideSources(null, true, on, false).Reason);
        var pending = GemsCaps.DecideSources(null, true, (0u, "the program kiln_items_gems is not registered or broke"), true);
        Assert.Equal(CapabilityState.Pending, pending.State);
    }

    [Fact]
    public void AHelloPostsTheEventAndSendsTheOffersAndACloseWithdrawsThem()
    {
        using var kiln = new KilnHarness();
        kiln.Capabilities.Override(Gems.CraftCapability, true);
        var sent = new List<(EntityId, object)>();
        GemSources.SendHook = (p, m) => sent.Add((p, m));
        long clock = 1_000_000;
        GemSources.Clock = () => clock;
        GemSources.ForgeNearHook = _ => Forge;
        Fake.ForgeMaterials[Forge.Value] = Essence.Value;
        Fake.PayerList = [Chest.Value];
        var seen = new List<GemForgeOpenedEvent>();
        var gem = Slot(SlotId.Of(new EntityId(5001), 0), Bleeding, 2560, level: 3);
        kiln.Run(() => Events.On<GemForgeOpenedEvent>(e =>
        {
            seen.Add(e);
            GemSources.Offer(e.Player, Chest, new GemSourceOptions(), Contents(true, 0, gem), id => id == Bleeding);
        }));
        kiln.Run(() => GemSources.Hello(Player, new GemForgeHello(true, [Essence.Value])));
        kiln.Run(GemSources.PumpServer);
        var e1 = Assert.Single(seen);
        Assert.False(e1.Refresh);
        Assert.Equal(Forge, e1.Forge);
        var (to, msg) = Assert.Single(sent);
        Assert.Equal(Player, to);
        var rows = Assert.IsType<GemForgeOffers>(msg);
        Assert.Equal(2560u, Assert.Single(rows.Rows).Pide);
        // within a second a client's hellos post nothing more (a hostile client cannot make the mods offer every frame)
        sent.Clear();
        clock += 200;
        kiln.Run(() => GemSources.Hello(Player, new GemForgeHello(true, [])));
        Assert.Single(seen);
        // review 2026-11-17: a close and an open within the second post no event either (the limit is the player's,
        // kept across a close); the pump posts the opening once the second is over
        kiln.Run(() => GemSources.Hello(Player, new GemForgeHello(false, [])));
        kiln.Run(() => GemSources.Hello(Player, new GemForgeHello(true, [])));
        kiln.Run(GemSources.PumpServer);
        Assert.Single(seen);
        clock += 900;
        kiln.Run(GemSources.PumpServer);
        Assert.Equal(2, seen.Count);
        Assert.False(seen[1].Refresh);  // the screen's opening, posted late
        // review 2026-11-17: the client's materials are ignored: the counts are of the forge's upgrade material only
        sent.Clear();
        clock += 1_100;
        kiln.Run(() => GemSources.Hello(Player, new GemForgeHello(true, [7u, 0x2b0cf1d6])));
        kiln.Run(GemSources.PumpServer);
        var counted = Assert.IsType<GemForgeOffers>(Assert.Single(sent).Item2);
        Assert.All(counted.Payers, x => Assert.Equal(Essence.Value, x.Item));
        // the screen closed: every owner's offers to the player go (owner 0), nothing more is sent
        kiln.Run(() => GemSources.Hello(Player, new GemForgeHello(false, [])));
        Assert.Contains((0ul, Player.Value, 0u), Fake.Withdrawals);
        sent.Clear();
        kiln.Run(GemSources.PumpServer);
        Assert.Empty(sent);
    }

    [Fact]
    public void TheForgesMaterialIsAskedAgainUntilTheProgramReadIt()
    {
        using var kiln = new KilnHarness();
        kiln.Capabilities.Override(Gems.CraftCapability, true);
        var sent = new List<(EntityId, object)>();
        GemSources.SendHook = (p, m) => sent.Add((p, m));
        GemSources.ForgeNearHook = _ => Forge;
        Fake.PayerList = [Chest.Value];
        var gem = Slot(SlotId.Of(new EntityId(5001), 0), Bleeding, 2560, level: 3);
        kiln.Run(() => Events.On<GemForgeOpenedEvent>(e =>
            GemSources.Offer(e.Player, Chest, new GemSourceOptions { PayFrom = GemPayer.From(Chest) }, Contents(true, 0, gem), id => id == Bleeding)));
        kiln.Capabilities.Override(Gems.PayersCapability, true);
        kiln.Run(() => GemSources.Hello(Player, new GemForgeHello(true, [])));
        kiln.Run(GemSources.PumpServer);
        Assert.Empty(Assert.IsType<GemForgeOffers>(Assert.Single(sent).Item2).Payers);  // not read yet: rows without counts
        Fake.ForgeMaterials[Forge.Value] = Essence.Value;  // the gem program read it
        sent.Clear();
        kiln.Run(GemSources.PumpServer);
        var p = Assert.Single(Assert.IsType<GemForgeOffers>(Assert.Single(sent).Item2).Payers);
        Assert.Equal((Essence.Value, (byte)1), (p.Item, p.Group));
    }

    [Fact]
    public void APlayerWhoLeftLosesTheSessionAndEveryOffer()
    {
        // review 2026-11-17: the entity id is the player slot + 1; the next player in the slot never inherits anything
        using var kiln = new KilnHarness();
        kiln.Capabilities.Override(Gems.CraftCapability, true);
        var sent = new List<(EntityId, object)>();
        GemSources.SendHook = (p, m) => sent.Add((p, m));
        var gem = Slot(SlotId.Of(new EntityId(5001), 0), Bleeding, 2560, level: 3);
        kiln.Run(() => Events.On<GemForgeOpenedEvent>(e =>
            GemSources.Offer(e.Player, Chest, new GemSourceOptions(), Contents(true, 0, gem), id => id == Bleeding)));
        kiln.Run(() => GemSources.Hello(Player, new GemForgeHello(true, [])));
        kiln.Run(GemSources.PumpServer);
        Assert.Single(Fake.Live);
        kiln.Run(() => GemSources.PlayerGone(Player));
        Assert.Contains((0ul, Player.Value, 0u), Fake.Withdrawals);
        Assert.Empty(Fake.Live);
        sent.Clear();
        kiln.Run(GemSources.PumpServer);
        Assert.Empty(sent);  // no session: nothing is sent to whoever is in the slot now
        // the next player's first hello opens a new session and posts at once (the old player's limit went with them)
        var seen = 0;
        kiln.Run(() => Events.On<GemForgeOpenedEvent>(_ => seen++));
        kiln.Run(() => GemSources.Hello(Player, new GemForgeHello(true, [])));
        Assert.Equal(1, seen);
    }

    [Fact]
    public void TheClientWatchesUiFramesOnlyWhileTheForgeScreenIsOpen()
    {
        // review 2026-11-17: no per-frame work from startup; the frame watch runs only while the forge screen is open
        var switches = new List<bool>();
        GemSources.TickSwitchHook = on => switches.Add(on);
        GemSources.ServerSideHook = () => true;
        var forge = new GameScreen(GameUiState.Play, HudScreen.ShroudForge, ScreenKind.Other, 0, null, 0, 1);
        var play = new GameScreen(GameUiState.Play, HudScreen.Invalid, ScreenKind.InGame, 0, null, 0, 2);
        GemSources.ClientChanged(GameScreen.None, play);
        Assert.Empty(switches);
        GemSources.ClientChanged(play, forge);
        Assert.Equal([true], switches);
        GemSources.ClientChanged(forge, forge with { Frame = 3 });
        Assert.Equal([true], switches);
        GemSources.ClientChanged(forge, play);
        Assert.Equal([true, false], switches);
        // a server without Kiln: the forge screen starts nothing
        GemSources.ServerSideHook = () => false;
        GemSources.ClientChanged(play, forge);
        Assert.Equal([true, false], switches);
    }

    [Fact]
    public void AnActionOnAnOfferedGemTellsItsClientAndPostsGemCraftedOnlyWhenDone()
    {
        using var kiln = new KilnHarness();
        kiln.Capabilities.Override(Gems.CraftCapability, true);
        var sent = new List<(EntityId, object)>();
        GemSources.SendHook = (p, m) => sent.Add((p, m));
        var crafted = new List<GemCraftedEvent>();
        kiln.Run(() => Events.On<GemCraftedEvent>(crafted.Add));
        var done = GemCraftingTests.NewCraft(GemCraftAction.Socket, GemcraftNative.SourceFlag);
        done.Ticket = 0x7000;
        var refused = GemCraftingTests.NewCraft(GemCraftAction.Upgrade, GemcraftNative.SourceFlag);
        refused.Status = (int)GemCraftStatus.NotEnough;
        refused.Ticket = 0x7001;
        Fake.CraftedQueue.Enqueue(done);
        Fake.CraftedQueue.Enqueue(refused);
        kiln.Run(GemCrafting.PumpOnce);
        var ev = Assert.Single(crafted);
        Assert.False(ev.ByCode);  // the player's own action
        Assert.Equal(2, sent.Count);
        Assert.Equal(0, ((GemForgeOutcome)sent[0].Item2).Status);
        var no = (GemForgeOutcome)sent[1].Item2;
        Assert.Equal((0x7001ul, (int)GemCraftStatus.NotEnough), (no.Token, no.Status));
        Assert.NotEmpty(no.Message);
    }
}
