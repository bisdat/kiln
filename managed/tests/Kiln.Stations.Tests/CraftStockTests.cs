// SPDX-License-Identifier: MIT
// Kiln.Stations' crafting-stock snapshot for Kiln clients (GitHub #68 step 2), offline: what a player is sent (only the
// items of the recipes that take a lent output, nothing without the game's inventory right, nothing outside a base,
// never a cut snapshot), the session (a hello opens it, a keep-alive extends it, a close, a leave, a new world and 15 s of
// silence end it, a failed send is tried again, an unchanged snapshot is not sent twice) and the client (a crafting
// menu's open and close are told to the server, a snapshot after the close is not kept). What the game shows is the
// real-client check (docs/kiln/services/stations.md, C8 to C12).
using Kiln;
using Kiln.TestKit;
using Xunit;

namespace Kiln.StationsTests;

public sealed class CraftStockTests : IDisposable
{
    private const uint Coal = 0x10, Scrap = 0x11, Logs = 0x12, Twig = 0x13;
    private long _now = 1_000;
    private readonly List<(EntityId Player, CraftStockRows Rows)> _sent = [];
    private bool _sendOk = true;
    private readonly Dictionary<uint, CraftStockRows?> _rows = new();

    public CraftStockTests()
    {
        CraftStock.ResetForTests();
        CraftStock.Clock = () => _now;
        CraftStock.BuildHook = p => _rows.TryGetValue(p.Value, out var r) ? r : null;
        CraftStock.SendHook = (p, r) =>
        {
            _sent.Add((p, r));
            return _sendOk;
        };
    }

    public void Dispose()
    {
        CraftStock.ResetForTests();
        StationsCore.UseBackend(null);
    }

    private static FillKey Key(uint station, uint recipe, uint cat = 0, byte index = 0) =>
        new(station, recipe, new FillChoice(cat, index), default, default, default);

    private static CraftStockRows Rows(params (uint Item, uint Count)[] rows) => new([.. rows.Select(r => new CraftStockRow(r.Item, r.Count))]);

    private static Dictionary<uint, long> D(params (uint Item, long Count)[] rows) => rows.ToDictionary(r => r.Item, r => r.Count);

    // ---- what a player is sent ----

    [Fact]
    public void A_snapshot_counts_the_bag_the_chests_and_the_outputs_of_the_named_items_only()
    {
        var rows = CraftStock.Compose([Scrap, Coal, Twig], D((Coal, 8)), D((Coal, 3), (Scrap, 2), (Logs, 50)), D((Coal, 4), (0x99, 7)));
        Assert.NotNull(rows);
        Assert.Equal([new CraftStockRow(Coal, 15), new CraftStockRow(Scrap, 2)], rows!.Rows);  // sorted, no logs, no empty twig row
    }

    [Fact]
    public void More_items_than_one_snapshot_carries_sends_nothing_rather_than_a_cut_snapshot()
    {
        var items = Enumerable.Range(1, CraftStock.MaxRows + 1).Select(i => (uint)i).ToArray();
        var bag = items.ToDictionary(i => i, _ => 1L);
        Assert.Null(CraftStock.Compose(items, D(), bag, D()));
        Assert.Equal(CraftStock.MaxRows, CraftStock.Compose(items[..CraftStock.MaxRows], D(), bag, D())!.Rows.Length);
    }

    [Fact]
    public void A_visitor_without_the_inventory_right_and_a_player_outside_a_base_are_sent_nothing()
    {
        var asked = 0;
        (int, uint[]) Items(uint[] lent)
        {
            asked++;
            return (0, [.. lent, Scrap]);
        }
        IReadOnlyDictionary<uint, long> Bag() => D((Scrap, 2));
        IReadOnlyDictionary<uint, long> Chests() => D((Coal, 4));
        Assert.Null(CraftStock.From(false, true, D((Coal, 8)), Items, Bag, Chests, out _));
        Assert.Null(CraftStock.From(null, true, D((Coal, 8)), Items, Bag, Chests, out _));  // permissions not known
        Assert.Null(CraftStock.From(true, false, D((Coal, 8)), Items, Bag, Chests, out _));
        Assert.Null(CraftStock.From(true, true, D(), Items, Bag, Chests, out _));  // nothing lent
        Assert.Equal(0, asked);
        Assert.Null(CraftStock.From(true, true, D((Coal, 8)), _ => (StationsNative.Unavailable, []), Bag, Chests, out _));
        var rows = CraftStock.From(true, true, D((Coal, 8)), Items, Bag, Chests, out var tooMany);
        Assert.False(tooMany);
        Assert.Equal([new CraftStockRow(Coal, 12), new CraftStockRow(Scrap, 2)], rows!.Rows);
    }

    [Fact]
    public void A_base_with_more_item_types_than_a_snapshot_still_works_when_few_recipes_take_the_outputs()
    {
        // 600 kinds of item in the chests: only the ones the recipes of the lent coal take are sent
        var chests = Enumerable.Range(0x1000, 600).ToDictionary(i => (uint)i, _ => 5L);
        chests[Scrap] = 9;
        var rows = CraftStock.From(true, true, D((Coal, 8)), lent => (0, [.. lent, Scrap]), () => D(), () => chests, out var tooMany);
        Assert.False(tooMany);
        Assert.Equal([new CraftStockRow(Coal, 8), new CraftStockRow(Scrap, 9)], rows!.Rows);
    }

    [Fact]
    public void A_snapshot_and_a_hello_survive_the_wire()
    {
        var w = new NetWriter();
        CraftStockRows.Encode(Rows((Coal, 15), (Scrap, 2)), w);
        var r = new NetReader(w.ToArray());
        Assert.Equal(Rows((Coal, 15), (Scrap, 2)).Rows, CraftStockRows.Decode(ref r).Rows);
        var h = new NetWriter();
        CraftStockHello.Encode(new CraftStockHello(true), h);
        var hr = new NetReader(h.ToArray());
        Assert.True(CraftStockHello.Decode(ref hr).Open);
    }

    [Fact]
    public void A_fill_ask_and_its_answers_survive_the_wire()
    {
        var w = new NetWriter();
        CraftStockFillAsk.Encode(new CraftStockFillAsk([Key(1032, 0x3acc518b, 77, 2), Key(1040, 9)]), w);
        var r = new NetReader(w.ToArray());
        Assert.Equal([Key(1032, 0x3acc518b, 77, 2), Key(1040, 9)], CraftStockFillAsk.Decode(ref r).Keys);
        var a = new NetWriter();
        CraftStockFillAnswers.Encode(new CraftStockFillAnswers([new FillAnswer(Key(1032, 0x3acc518b, 77, 2), 10)]), a);
        var ar = new NetReader(a.ToArray());
        Assert.Equal([new FillAnswer(Key(1032, 0x3acc518b, 77, 2), 10)], CraftStockFillAnswers.Decode(ref ar).Rows);
        // never more than 16 on the wire
        var many = Enumerable.Range(1, 20).Select(i => Key((uint)i, 1)).ToArray();
        var m = new NetWriter();
        CraftStockFillAsk.Encode(new CraftStockFillAsk(many), m);
        var mr = new NetReader(m.ToArray());
        Assert.Equal(CraftStock.MaxFillKeys, CraftStockFillAsk.Decode(ref mr).Keys.Length);
    }

    [Fact]
    public void A_key_maps_to_the_native_row_and_to_a_fills_choices()
    {
        var k = new FillKey(1032, 0x3acc518b, new FillChoice(77, 2), default, new FillChoice(88, 1), default);
        var n = k.Native();
        Assert.Equal((1032u, 0x3acc518bu, 77u, (byte)2, 88u, (byte)1), (n.Station, n.Recipe, n.C0.Category, n.C0.Index, n.C2.Category, n.C2.Index));
        Assert.Equal(k, FillKey.Of(n));
        Assert.Equal([new CategoryChoice(77, 2), new CategoryChoice(88, 1)], k.FillChoices());
        Assert.Empty(Key(1032, 1).FillChoices());
    }

    // ---- the station screen's fill: the server ----

    private readonly List<(EntityId Player, CraftStockFillAnswers M)> _fills = [];
    private readonly Dictionary<FillKey, uint> _covers = new();

    private void FillHooks()
    {
        CraftStock.FillBuildHook = (_, keys) => [.. keys.Select(k => new FillAnswer(k, _covers.GetValueOrDefault(k)))];
        CraftStock.FillSendHook = (p, m) =>
        {
            _fills.Add((p, m));
            return _sendOk;
        };
    }

    [Fact]
    public void Asked_fills_are_answered_at_once_then_only_when_an_answer_changes()
    {
        FillHooks();
        var p = new EntityId(5);
        _rows[5] = Rows((Coal, 20), (Scrap, 38));
        CraftStock.Enqueue(p, true);
        CraftStock.Pump(7);
        Assert.Empty(_fills);  // nothing asked: nothing answered
        var forge = Key(1032, 0x3acc518b);
        _covers[forge] = 10;
        CraftStock.EnqueueAsk(p, [forge]);
        CraftStock.Pump(8);
        Assert.Equal([new FillAnswer(forge, 10)], Assert.Single(_fills).M.Rows);
        for (ulong s = 9; s <= 90; s++) CraftStock.Pump(s);  // refreshes, nothing changed
        Assert.Single(_fills);
        _covers[forge] = 9;  // a craft's charcoal went in meanwhile
        CraftStock.Pump(120);
        Assert.Equal(2, _fills.Count);
        Assert.Equal([new FillAnswer(forge, 9)], _fills[1].M.Rows);
        // a second fill is added to the first; both are answered
        var other = Key(1040, 0x55, 77, 1);
        CraftStock.EnqueueAsk(p, [other]);
        CraftStock.Pump(121);
        Assert.Equal([new FillAnswer(forge, 9), new FillAnswer(other, 0)], _fills[^1].M.Rows);
        Assert.Equal([forge, other], CraftStock.AskedOf(p));
        // asked again (the client has no answer): the answers go out again even unchanged
        var n = _fills.Count;
        CraftStock.EnqueueAsk(p, [forge]);
        CraftStock.Pump(122);
        Assert.Equal(n + 1, _fills.Count);
        Assert.Equal([forge, other], CraftStock.AskedOf(p));
    }

    [Fact]
    public void An_ask_without_an_open_menu_is_dropped_and_a_session_keeps_at_most_16()
    {
        FillHooks();
        var p = new EntityId(5);
        CraftStock.EnqueueAsk(p, [Key(1032, 1)]);
        CraftStock.Pump(7);
        Assert.Empty(_fills);
        Assert.Empty(CraftStock.AskedOf(p));
        _rows[5] = Rows((Coal, 3));
        CraftStock.Enqueue(p, true);
        CraftStock.Pump(8);
        CraftStock.EnqueueAsk(p, [.. Enumerable.Range(1, 20).Select(i => Key((uint)i, 1)), Key(0, 1), Key(3, 0)]);
        CraftStock.Pump(9);
        var held = CraftStock.AskedOf(p);
        Assert.Equal(CraftStock.MaxFillKeys, held.Count);
        Assert.Equal(Key(5, 1), held[0]);  // the oldest went; a key without a station or recipe never counts
        // the menu closes: the session and its asks end
        CraftStock.Enqueue(p, false);
        CraftStock.Pump(10);
        Assert.Empty(CraftStock.AskedOf(p));
    }

    [Fact]
    public void With_no_station_outputs_to_count_an_asked_fill_is_answered_with_nothing()
    {
        FillHooks();
        var p = new EntityId(5);
        _rows[5] = Rows((Coal, 3));
        CraftStock.Enqueue(p, true);
        CraftStock.Pump(7);
        _covers[Key(1032, 1)] = 5;
        CraftStock.EnqueueAsk(p, [Key(1032, 1)]);
        CraftStock.Pump(8);
        Assert.Single(_fills[^1].M.Rows);
        _rows[5] = null;  // the kiln's outputs were collected: no snapshot, and no answer that could raise a button
        CraftStock.Pump(30);
        Assert.Empty(_fills[^1].M.Rows);
    }

    [Fact]
    public void An_answer_that_did_not_go_out_is_tried_again()
    {
        FillHooks();
        var p = new EntityId(5);
        _rows[5] = Rows((Coal, 3));
        CraftStock.Enqueue(p, true);
        CraftStock.Pump(7);
        _sendOk = false;
        CraftStock.EnqueueAsk(p, [Key(1032, 1)]);
        CraftStock.Pump(8);
        var n = _fills.Count;
        _sendOk = true;
        CraftStock.Pump(30);
        Assert.Equal(n + 1, _fills.Count);
    }

    // ---- the station screen's fill: the client ----

    [Fact]
    public void The_client_asks_once_per_set_of_refused_fills_again_after_5_s_and_keeps_answers_only_while_open()
    {
        using var t = new StationsTests();
        var fake = t.Backend;
        StationsCore.UseBackend(fake);
        var asked = new List<FillKey[]>();
        CraftStock.AskHook = asked.Add;
        CraftStock.HelloHook = _ => { };
        CraftStock.ServerSideHook = () => true;
        using var k = new KilnHarness();
        k.Capabilities.Override(CraftStock.FillUiCapability, true);
        k.Capabilities.Override(Stations.CraftUiCapability, false);  // the station screen's fill alone still opens the session
        CraftStock.PollAsks(10_000);  // no menu open: nothing
        CraftStock.ClientChanged(Screen(ScreenKind.InGame), Screen(ScreenKind.Crafting));
        CraftStock.PollAsks(10_000);  // nothing refused yet
        Assert.Empty(asked);
        var forge = Key(1032, 0x3acc518b, 77, 2);
        fake.Asks = [forge.Native()];
        CraftStock.PollAsks(10_300);
        Assert.Equal([forge], Assert.Single(asked));
        CraftStock.PollAsks(10_400);  // within 250 ms: not even read
        CraftStock.PollAsks(11_000);  // the same set: not again
        Assert.Single(asked);
        CraftStock.PollAsks(15_400);  // 5 s on, still unanswered: once more
        Assert.Equal(2, asked.Count);
        var kiln2 = Key(1040, 9);
        fake.Asks = [forge.Native(), kiln2.Native()];
        CraftStock.PollAsks(15_700);
        Assert.Equal([forge, kiln2], asked[^1]);
        // the answers go to native while the menu is open
        Assert.True(CraftStock.ApplyFill(new CraftStockFillAnswers([new FillAnswer(forge, 10)])));
        var row = Assert.Single(fake.FillAnswers);
        Assert.Equal((1032u, 10u, 77u, (byte)2), (row.Key.Station, row.Crafts, row.Key.C0.Category, row.Key.C0.Index));
        // the menu closes; a late answer is not kept
        CraftStock.ApplyHook = _ => true;
        CraftStock.ClientChanged(Screen(ScreenKind.Crafting), Screen(ScreenKind.InGame));
        CraftStock.ClientFill(new CraftStockFillAnswers([new FillAnswer(forge, 10)]));
        Assert.Single(fake.FillAnswers);  // unchanged: the late rows never reached native (the close cleared native itself)
        fake.Asks = [kiln2.Native()];
        var n = asked.Count;
        CraftStock.PollAsks(30_000);
        Assert.Equal(n, asked.Count);  // closed: no asks
        // an older kiln-items without the table's members: nothing asked, nothing applied
        fake.HasFill = false;
        CraftStock.ClientChanged(Screen(ScreenKind.InGame), Screen(ScreenKind.Crafting));
        CraftStock.PollAsks(40_000);
        Assert.Equal(n, asked.Count);
        Assert.False(CraftStock.ApplyFill(new CraftStockFillAnswers([new FillAnswer(forge, 10)])));
    }

    // ---- the part of a count only the player's linked crafting sources hold (GitHub #132) ----

    private static CraftStockRows Linked(CraftStockRows rows, params (uint Item, uint Count)[] linked) =>
        rows with { Linked = [.. linked.Select(r => new CraftStockRow(r.Item, r.Count))] };

    [Fact]
    public void A_snapshot_names_the_part_of_each_count_only_linked_sources_hold()
    {
        // coal: 8 from outputs, 3 in the bag, 4 in the chests, 6 only in linked sources; scrap: 2 in the bag
        var rows = CraftStock.Compose([Scrap, Coal, Twig], D((Coal, 14)), D((Coal, 3), (Scrap, 2)), D((Coal, 4)), D((Coal, 6), (Twig, 0), (Logs, 9)));
        Assert.Equal([new CraftStockRow(Coal, 21), new CraftStockRow(Scrap, 2)], rows!.Rows);
        Assert.Equal([new CraftStockRow(Coal, 6)], rows.Linked);  // no row of its own, never more than the count
        Assert.Empty(CraftStock.Compose([Coal], D((Coal, 8)), D(), D())!.Linked);
        var at = CraftStock.From(null, false, D((Coal, 6)), lent => (0, [.. lent]), () => D((Coal, 1)), () => D(), out _, registered: true,
            linked: D((Coal, 6)));
        Assert.Equal([new CraftStockRow(Coal, 7)], at!.Rows);
        Assert.Equal([new CraftStockRow(Coal, 6)], at.Linked);
    }

    [Fact]
    public void The_linked_part_rides_after_the_rows_where_an_older_client_does_not_read()
    {
        var w = new NetWriter();
        CraftStockRows.Encode(Linked(Rows((Coal, 15), (Scrap, 2)), (Coal, 6)), w);
        var bytes = w.ToArray();
        var r = new NetReader(bytes);
        var back = CraftStockRows.Decode(ref r);
        Assert.Equal(Rows((Coal, 15), (Scrap, 2)).Rows, back.Rows);
        Assert.Equal([new CraftStockRow(Coal, 6)], back.Linked);
        // an older client's decoder (0.2.0-dev.30.7 and before): the count, the rows, and nothing more is read
        var old = new NetReader(bytes);
        var n = old.UInt16();
        var rows = new CraftStockRow[n];
        for (var i = 0; i < n; i++) rows[i] = new CraftStockRow(old.UInt32(), old.UInt32());
        Assert.Equal(back.Rows, rows);
        // an older server's snapshot (no linked part): none is linked
        var plain = new NetWriter();
        plain.UInt16(1).UInt32(Coal).UInt32(4);
        var pr = new NetReader(plain.ToArray());
        var fromOld = CraftStockRows.Decode(ref pr);
        Assert.Equal([new CraftStockRow(Coal, 4)], fromOld.Rows);
        Assert.Empty(fromOld.Linked);
        // a linked row for an item the snapshot does not count, or above its count, is not believed
        var odd = new NetWriter();
        odd.UInt16(1).UInt32(Coal).UInt32(4).UInt16(2).UInt32(Scrap).UInt32(3).UInt32(Coal).UInt32(9);
        var or = new NetReader(odd.ToArray());
        Assert.Equal([new CraftStockRow(Coal, 4)], CraftStockRows.Decode(ref or).Linked);
    }

    [Fact]
    public void A_snapshot_that_only_moves_counts_between_linked_and_other_sources_is_sent_again()
    {
        Assert.NotEqual(CraftStock.Key(Linked(Rows((Coal, 9)), (Coal, 6))), CraftStock.Key(Linked(Rows((Coal, 9)), (Coal, 2))));
        Assert.NotEqual(CraftStock.Key(Linked(Rows((Coal, 9)), (Coal, 6))), CraftStock.Key(Rows((Coal, 9))));
        Assert.Equal(CraftStock.Key(Linked(Rows((Coal, 9)), (Coal, 6))), CraftStock.Key(Linked(Rows((Coal, 9)), (Coal, 6))));
    }

    [Fact]
    public void A_production_station_screen_counts_no_linked_source_its_fill_cannot_take()
    {
        var applied = new List<CraftStockRows>();
        CraftStock.HelloHook = _ => { };
        CraftStock.ApplyHook = r =>
        {
            applied.Add(r);
            return true;
        };
        CraftStock.ServerSideHook = () => true;
        using var k = new KilnHarness();
        k.Capabilities.Override(CraftStock.StockUiCapability, true);
        var snapshot = Linked(Rows((Coal, 21), (Scrap, 2), (Twig, 5)), (Coal, 6), (Twig, 5));
        // a production station's screen: its rows and their transfer buttons see the bag, the chests and the outputs
        CraftStock.ClientChanged(Screen(ScreenKind.InGame), Factory());
        CraftStock.ClientRows(snapshot);
        Assert.Equal([new CraftStockRow(Coal, 15), new CraftStockRow(Scrap, 2)], applied[^1].Rows);
        // a crafting menu (a crafting station's, the inventory's, an NPC's): its craft pays from the sources too
        CraftStock.ClientChanged(Factory(), Screen(ScreenKind.Crafting));
        Assert.Equal(snapshot.Rows, applied[^1].Rows);
        var n = applied.Count;
        CraftStock.ClientChanged(Screen(ScreenKind.Crafting), Screen(ScreenKind.Crafting));  // no change, nothing re-applied
        Assert.Equal(n, applied.Count);
        CraftStock.ClientChanged(Screen(ScreenKind.Crafting), Factory());
        Assert.Equal([new CraftStockRow(Coal, 15), new CraftStockRow(Scrap, 2)], applied[^1].Rows);
        CraftStock.ClientRows(Rows((Coal, 4)));  // an older server: no linked part, the snapshot as it is
        Assert.Equal([new CraftStockRow(Coal, 4)], applied[^1].Rows);
        // closed: the game's counts, also when the next screen is a factory's
        CraftStock.ClientChanged(Factory(), Screen(ScreenKind.InGame));
        Assert.Empty(applied[^1].Rows);
        n = applied.Count;
        CraftStock.ClientRows(snapshot);  // late: not kept
        Assert.Equal(n, applied.Count);
    }

    private static GameScreen Factory() => Screen(ScreenKind.Crafting) with
    {
        Hud = HudScreen.FactoryCrafting,
        Crafting = new CraftingScreen(true, 0, 0, false) { IsFactory = true, Station = new EntityId(1032) },
    };

    // ---- the session ----

    [Fact]
    public void Nothing_is_sent_to_a_player_whose_menu_never_said_hello()
    {
        _rows[5] = Rows((Coal, 3));
        for (ulong s = 1; s <= 120; s++) CraftStock.Pump(s);
        Assert.Empty(_sent);
    }

    [Fact]
    public void An_open_menu_is_sent_a_snapshot_at_once_then_only_when_it_changes()
    {
        var p = new EntityId(5);
        _rows[5] = Rows((Coal, 3));
        CraftStock.Enqueue(p, true);
        CraftStock.Pump(7);
        Assert.Single(_sent);
        Assert.Equal(CraftStock.Key(Rows((Coal, 3))), CraftStock.SessionOf(p).Sent);
        for (ulong s = 8; s <= 90; s++) CraftStock.Pump(s);  // two refreshes, nothing changed
        Assert.Single(_sent);
        _rows[5] = Rows((Coal, 2));
        CraftStock.Pump(91);  // not a refresh step
        Assert.Single(_sent);
        CraftStock.Pump(120);
        Assert.Equal(2, _sent.Count);
        Assert.Equal(Rows((Coal, 2)).Rows, _sent[1].Rows.Rows);
        // nothing to raise any more: the client is told (an empty snapshot), once
        _rows[5] = null;
        CraftStock.Pump(150);
        CraftStock.Pump(180);
        Assert.Equal(3, _sent.Count);
        Assert.Empty(_sent[2].Rows.Rows);
    }

    [Fact]
    public void A_send_that_did_not_go_out_is_tried_again()
    {
        var p = new EntityId(5);
        _rows[5] = Rows((Coal, 3));
        _sendOk = false;  // the client's peer is not ready yet (its handshake), or has no channel
        CraftStock.Enqueue(p, true);
        CraftStock.Pump(7);
        Assert.Single(_sent);
        Assert.Null(CraftStock.SessionOf(p).Sent);
        _sendOk = true;
        CraftStock.Pump(30);
        Assert.Equal(2, _sent.Count);
        Assert.NotNull(CraftStock.SessionOf(p).Sent);
    }

    [Fact]
    public void A_close_a_leave_a_rejoin_and_a_new_world_end_the_session_and_a_new_hello_sends_again()
    {
        var p = new EntityId(5);
        _rows[5] = Rows((Coal, 3));
        CraftStock.Enqueue(p, true);
        CraftStock.Pump(7);
        CraftStock.Enqueue(p, false);
        CraftStock.Pump(8);
        Assert.False(CraftStock.SessionOf(p).Open);
        CraftStock.Pump(30);
        Assert.Single(_sent);
        // reopened with the same stock: sent again, the client cleared its counts at the close
        CraftStock.Enqueue(p, true);
        CraftStock.Pump(31);
        Assert.Equal(2, _sent.Count);
        // the player left, or someone joined into the entity
        CraftStock.Gone(p);
        CraftStock.Pump(60);
        Assert.Equal(2, _sent.Count);
        CraftStock.Enqueue(p, true);  // the same entity, the same stock: a new session sends
        CraftStock.Pump(61);
        Assert.Equal(3, _sent.Count);
        // a new world: its step count starts again
        CraftStock.Pump(5);
        Assert.False(CraftStock.SessionOf(p).Open);
    }

    [Fact]
    public void A_menu_that_stops_saying_it_is_open_ends_after_15_s_and_a_keep_alive_keeps_it()
    {
        var p = new EntityId(5);
        _rows[5] = Rows((Coal, 3));
        CraftStock.Enqueue(p, true);
        CraftStock.Pump(7);
        _now += 10_000;
        CraftStock.Enqueue(p, true);  // the keep-alive: no snapshot sent again for it
        CraftStock.Pump(8);
        Assert.Single(_sent);
        _now += 14_000;
        CraftStock.Pump(9);
        Assert.True(CraftStock.SessionOf(p).Open);
        _now += 2_000;
        CraftStock.Pump(10);
        Assert.False(CraftStock.SessionOf(p).Open);
    }

    // ---- the client ----

    private static GameScreen Screen(ScreenKind kind) => GameScreen.None with { State = GameUiState.Play, Kind = kind };

    [Fact]
    public void The_client_tells_the_server_when_a_crafting_menu_opens_and_closes_and_drops_a_late_snapshot()
    {
        var hellos = new List<bool>();
        var applied = new List<CraftStockRows>();
        CraftStock.HelloHook = hellos.Add;
        CraftStock.ApplyHook = r =>
        {
            applied.Add(r);
            return true;
        };
        CraftStock.ServerSideHook = () => true;
        using var k = new KilnHarness();
        k.Capabilities.Override(Stations.CraftUiCapability, true);
        CraftStock.ClientChanged(Screen(ScreenKind.InGame), Screen(ScreenKind.Crafting));
        CraftStock.ClientChanged(Screen(ScreenKind.Crafting), Screen(ScreenKind.Crafting));
        Assert.Equal([true], hellos);
        CraftStock.ClientRows(Rows((Coal, 3)));
        Assert.Single(applied);
        CraftStock.ClientChanged(Screen(ScreenKind.Crafting), Screen(ScreenKind.InGame));
        Assert.Equal([true, false], hellos);
        Assert.Empty(applied[^1].Rows);  // the counts are the game's again
        CraftStock.ClientRows(Rows((Coal, 4)));  // sent before the server took the close
        Assert.Equal(2, applied.Count);
    }

    [Fact]
    public void The_ingredient_rows_count_alone_still_opens_the_session()
    {
        // stations.stock_ui (the "available" counts) needs the same snapshot even where the other client rows are off
        var hellos = new List<bool>();
        CraftStock.HelloHook = hellos.Add;
        CraftStock.ServerSideHook = () => true;
        using var k = new KilnHarness();
        k.Capabilities.Override(Stations.CraftUiCapability, false);
        k.Capabilities.Override(CraftStock.FillUiCapability, false);
        k.Capabilities.Override(CraftStock.StockUiCapability, true);
        CraftStock.ClientChanged(Screen(ScreenKind.InGame), Screen(ScreenKind.Crafting));
        Assert.Equal([true], hellos);
    }

    [Fact]
    public void A_client_whose_server_has_no_Kiln_never_says_hello()
    {
        var hellos = new List<bool>();
        CraftStock.HelloHook = hellos.Add;
        CraftStock.ServerSideHook = () => false;  // a vanilla server
        using var k = new KilnHarness();
        k.Capabilities.Override(Stations.CraftUiCapability, true);
        CraftStock.ClientChanged(Screen(ScreenKind.InGame), Screen(ScreenKind.Crafting));
        Assert.Empty(hellos);
    }
}
