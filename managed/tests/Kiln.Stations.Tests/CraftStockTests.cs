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

    public void Dispose() => CraftStock.ResetForTests();

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
