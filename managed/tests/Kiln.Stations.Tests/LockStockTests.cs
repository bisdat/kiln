// SPDX-License-Identifier: MIT
// GitHub #147: CraftSourceOptions.Locks on the real managed API against the kit's kiln.stations fake, the server half of
// the lock snapshot (asks, rows, delivery) and the client half (apply, asks), with their test hooks. The game's lock
// check and interaction payment are native (native/services/items/tests/lock_sources_test.cpp).
using Kiln;
using Kiln.TestKit;
using Xunit;
namespace Kiln.StationsTests;

public sealed class LockStockTests
{
    [Fact]
    public void Locks_is_off_by_default_and_recorded_when_asked_for()
    {
        Assert.False(new CraftSourceOptions().Locks);
        using var h = new KilnHarness();
        var player = h.Players.Join("Picker", rights: PlayerPermissions.AccessInventories);
        h.Step();
        var pick = h.Items.AddItem("lockpick", 20);
        var a = h.Entities.Create(); h.Items.AddContainer(a).Put(pick, 2);
        var b = h.Entities.Create(); h.Items.AddContainer(b).Put(pick, 3);
        using var crafting = h.Run(() => Crafting.AddSource(player.Entity, a));
        using var locks = h.Run(() => Crafting.AddSource(player.Entity, b, new CraftSourceOptions { Locks = true }));
        Assert.Equal([false, true], h.Crafting.Sources.Select(s => s.Locks));
        // crafting counts both links; a lock only the opted-in one
        Assert.Equal(5, h.Crafting.Stock(player.Entity)[pick]);
        Assert.Equal(3, h.Crafting.LockStock(player.Entity)[pick]);
        Assert.Equal(new StationsNative.Stack { Item = pick.Value, Count = 3 }, Assert.Single(h.Run(() => CraftingCore.LockStock(player.Entity)).Rows));
        locks.Dispose();
        Assert.Empty(h.Crafting.LockStock(player.Entity));
        Assert.Equal(2, h.Crafting.Stock(player.Entity)[pick]);
    }

    [Fact]
    public void A_lock_uses_the_links_filter_and_the_players_current_permission()
    {
        using var h = new KilnHarness();
        var player = h.Players.Join("Picker", rights: PlayerPermissions.AccessInventories);
        var visitor = h.Players.Join("Visitor");
        h.Step();
        var pick = h.Items.AddItem("lockpick", 20);
        var key = h.Items.AddItem("archaic lockpick", 20);
        var chest = h.Entities.Create(); var inv = h.Items.AddContainer(chest); inv.Put(pick, 4); inv.Put(key, 1);
        using var allow = h.Run(() => Crafting.AddSource(player.Entity, chest,
            new CraftSourceOptions { Locks = true, Filter = CraftSourceFilter.Allow, Items = [key] }));
        using var refused = h.Run(() => Crafting.AddSource(visitor.Entity, chest, new CraftSourceOptions { Locks = true }));
        Assert.Equal(1, Assert.Single(h.Crafting.LockStock(player.Entity)).Value);
        Assert.False(h.Crafting.LockStock(player.Entity).ContainsKey(pick));
        Assert.Empty(h.Crafting.LockStock(visitor.Entity));  // no CanAccessInventories: nothing counts
    }

    [Fact]
    public void The_server_answers_the_clients_asks_with_what_the_opted_in_links_hold()
    {
        using var h = new KilnHarness();
        LockStock.ResetForTests();
        try
        {
            var sent = new List<(EntityId Player, LockStockRows Rows)>();
            LockStock.SendHook = (p, m) => { sent.Add((p, m)); return true; };
            var player = h.Players.Join("Picker", rights: PlayerPermissions.AccessInventories);
            h.Step();
            var pick = h.Items.AddItem("lockpick", 20);
            var other = h.Items.AddItem("rope", 20);
            var chest = h.Entities.Create(); h.Items.AddContainer(chest).Put(pick, 2);
            // a crafting-only link: no session, an ask is not answered
            using (h.Run(() => Crafting.AddSource(player.Entity, chest)))
            {
                h.Run(() => LockStock.Ask(player.Entity, [pick.Value]));
                Assert.False(LockStock.SessionOf(player.Entity).Open);
            }
            var source = h.Run(() => Crafting.AddSource(player.Entity, chest, new CraftSourceOptions { Locks = true }));
            Assert.Equal((true, 1), (LockStock.SessionOf(player.Entity).Open, LockStock.SessionOf(player.Entity).Links));
            h.Run(() => LockStock.Pump(100));
            var first = Assert.Single(sent);
            Assert.True(first.Rows.Supported);
            Assert.Empty(first.Rows.Rows);  // supported, nothing asked yet: the client's checks note the items they meet
            sent.Clear();
            h.Run(() => LockStock.Pump(101));
            Assert.Empty(sent);  // unchanged: nothing again
            LockStock.Enqueue(player.Entity, [pick.Value, other.Value, 0]);
            h.Run(() => LockStock.Pump(102));
            var answer = Assert.Single(sent).Rows;
            Assert.Equal([new CraftStockRow(pick.Value, 2), new CraftStockRow(other.Value, 0)], answer.Rows.OrderByDescending(r => r.Count));
            sent.Clear();
            // a count changes: the next refresh sends it
            h.Items.Of(chest)!.Put(pick, 1);
            h.Run(() => LockStock.Pump(102 + LockStock.RefreshSteps));
            Assert.Contains(new CraftStockRow(pick.Value, 3), Assert.Single(sent).Rows.Rows);
            sent.Clear();
            // the last lock-opted link goes: "unsupported" (the game's own lock checks again)
            source.Dispose();
            Assert.Empty(sent);  // any thread: the server's next tick sends it
            h.Run(() => LockStock.Pump(200));
            Assert.False(Assert.Single(sent).Rows.Supported);
            Assert.False(LockStock.SessionOf(player.Entity).Open);
        }
        finally { LockStock.ResetForTests(); }
    }

    [Fact]
    public void Without_the_capability_or_the_links_counts_the_server_sends_unsupported()
    {
        LockStock.ResetForTests();
        try
        {
            LockStock.SupportedHook = () => false;
            Assert.Same(LockStock.Off, LockStock.Build(new EntityId(1), [7]));
            LockStock.SupportedHook = () => true;
            LockStock.RowsHook = _ => (StationsNative.Unavailable, []);
            Assert.Same(LockStock.Off, LockStock.Build(new EntityId(1), [7]));
            LockStock.RowsHook = _ => (StationsNative.Ok, [new StationsNative.Stack { Item = 7, Count = 5 }, new StationsNative.Stack { Item = 8, Count = 1 }]);
            var m = LockStock.Build(new EntityId(1), [9, 7, 7]);
            Assert.True(m.Supported);
            Assert.Equal([new CraftStockRow(7, 5), new CraftStockRow(9, 0)], m.Rows);  // only asked items, sorted, once
        }
        finally { LockStock.ResetForTests(); }
    }

    [Fact]
    public void A_player_who_leaves_ends_the_session()
    {
        using var h = new KilnHarness();
        LockStock.ResetForTests();
        try
        {
            LockStock.SendHook = (_, _) => true;
            var player = h.Players.Join("Picker", rights: PlayerPermissions.AccessInventories);
            h.Step();
            var chest = h.Entities.Create(); h.Items.AddContainer(chest);
            using var source = h.Run(() => Crafting.AddSource(player.Entity, chest, new CraftSourceOptions { Locks = true }));
            Assert.True(LockStock.SessionOf(player.Entity).Open);
            player.Leave(); h.Step();
            Assert.False(LockStock.SessionOf(player.Entity).Open);
            source.Dispose();  // a late dispose of the old login's handle is a no-op
            Assert.False(LockStock.SessionOf(player.Entity).Open);
        }
        finally { LockStock.ResetForTests(); }
    }

    [Fact]
    public void The_client_applies_the_servers_rows_and_asks_about_the_items_its_checks_met()
    {
        LockStock.ResetForTests();
        try
        {
            var applied = new List<(bool On, StationsNative.Stack[] Rows)>();
            var asked = new List<uint[]>();
            LockStock.ApplyHook = (on, rows) => { applied.Add((on, rows)); return StationsNative.Ok; };
            LockStock.AskHook = items => asked.Add(items);
            LockStock.AsksHook = () => (StationsNative.Ok, [7u]);
            // a vanilla server (no Kiln): never on, never asks
            LockStock.ServerSideHook = () => false;
            Assert.True(LockStock.ClientApply(new LockStockRows(true, [new CraftStockRow(7, 2)])));
            Assert.False(applied[^1].On);
            LockStock.PollAsks(1000);
            Assert.Empty(asked);
            // a Kiln server: on with its rows; the asks go once per set, again after 5 s
            LockStock.ServerSideHook = () => true;
            LockStock.ClientApply(new LockStockRows(true, [new CraftStockRow(9, 2)]));
            Assert.True(applied[^1].On);
            Assert.Equal(9u, Assert.Single(applied[^1].Rows).Item);
            LockStock.PollAsks(2000);
            Assert.Equal([7u], Assert.Single(asked));
            LockStock.PollAsks(2300);
            Assert.Single(asked);
            LockStock.PollAsks(2000 + 5000 + 300);
            Assert.Equal(2, asked.Count);
            LockStock.ClientApply(LockStock.Off);
            Assert.False(applied[^1].On);
            LockStock.PollAsks(20000);
            Assert.Equal(2, asked.Count);
        }
        finally { LockStock.ResetForTests(); }
    }

    [Fact]
    public void The_messages_round_trip_and_bad_rows_are_refused()
    {
        var w = new NetWriter();
        LockStockRows.Encode(new LockStockRows(true, [new CraftStockRow(7, 2), new CraftStockRow(9, 0)]), w);
        var r = new NetReader(w.ToArray());
        var back = LockStockRows.Decode(ref r);
        Assert.True(back.Supported);
        Assert.Equal([new CraftStockRow(7, 2), new CraftStockRow(9, 0)], back.Rows);
        var bad = new NetWriter();
        bad.Bool(true).UInt16(2).UInt32(7).UInt32(1).UInt32(7).UInt32(1);
        Assert.Throws<InvalidDataException>(() => { var br = new NetReader(bad.ToArray()); LockStockRows.Decode(ref br); });
        var zero = new NetWriter();
        zero.Bool(true).UInt16(1).UInt32(0).UInt32(1);
        Assert.Throws<InvalidDataException>(() => { var zr = new NetReader(zero.ToArray()); LockStockRows.Decode(ref zr); });
        var ask = new NetWriter();
        LockStockAsk.Encode(new LockStockAsk([7, 9]), ask);
        var ar = new NetReader(ask.ToArray());
        Assert.Equal([7u, 9u], LockStockAsk.Decode(ref ar).Items);
    }

    [Fact]
    public void The_lock_members_need_the_0_2_0_dev_31_12_table()
    {
        Assert.False(CraftingCore.HasLockMembers(288));
        Assert.False(CraftingCore.HasLockMembers(303));
        Assert.True(CraftingCore.HasLockMembers(304));
    }
}
