// SPDX-License-Identifier: MIT
// Kiln.Replication's own code on the test kit's kiln.repl (Fakes/Repl): the counters' hold per mod, rows as records,
// watches started mid-stream, their last event, Dispose, a mod's unload, bad arguments and the capability rows.
using System.Reflection;
using Kiln;
using Kiln.TestKit;
using Xunit;

namespace ReplicationTests;

public sealed class ReplicationTests
{
    private sealed class ReplMod : Mod;

    private const uint Player = 0x0001_0003, Other = 0x0002_0004;
    private static readonly EntityId Entity = new(4242);

    private static readonly FakeReplComponent Transform = new("CurrentTransform", 7, 12, CustomCodec: true, PredictionChannel: true);
    private static readonly FakeReplComponent Health = new("KilnReplTestHealth", 9, 4, Versioned: true);

    private static KilnHarness Started(out ModHandle mod, string id = "repl-a")
    {
        var kiln = new KilnHarness();
        kiln.Repl.Connect(3, Player);
        kiln.Repl.Connect(4, Other);
        kiln.Repl.AddComponent(Transform);
        kiln.Repl.AddComponent(Health);
        mod = kiln.Load<ReplMod>(new ModManifest(id));
        kiln.Step(2);
        return kiln;
    }

    /// <summary>What a watch sees, in order.</summary>
    private sealed class Seen
    {
        private readonly List<object> _all = [];
        public void Add(object e) { lock (_all) _all.Add(e); }
        public IReadOnlyList<object> All { get { lock (_all) return [.. _all]; } }
        public IReadOnlyList<ReplSent> Sent => All.OfType<ReplSent>().ToList();
        public IReadOnlyList<ReplReceived> Received => All.OfType<ReplReceived>().ToList();
    }

    private static ReplWatch Watch(KilnHarness kiln, ModHandle? mod, Seen seen, EntityId entity, NetPeer? peer = null)
    {
        ReplWatch Start()
        {
            var w = Replication.Watch(entity, peer);
            w.Sent += seen.Add;
            w.Received += seen.Add;
            w.Ended += seen.Add;
            return w;
        }
        return mod is null ? kiln.Run(Start) : kiln.As(mod, Start);
    }

    private static NetPeer PeerOf(uint id) =>
        (NetPeer)Activator.CreateInstance(typeof(NetPeer), BindingFlags.NonPublic | BindingFlags.Instance, null, [id], null)!;

    private static string NameOf(KilnHarness kiln, FakeReplComponent c) =>
        kiln.Run(() => GameTypes.ByHash(Hash(c.Name))) is null ? "#" + c.Index : c.Name;

    private static uint Hash(string name)
    {
        var h = 2166136261u;
        foreach (var b in System.Text.Encoding.UTF8.GetBytes("keen::ecs::" + name)) h = (h ^ b) * 16777619u;
        return h;
    }

    private static FakeReplMessage Update(uint tick, params (string, byte[])[] components) =>
        new(Entity, FakeReplKind.Update, tick) { Components = components, Seq = tick, BaselineTick = tick - 4, PayloadBytes = 6 };

    // ---- the counters ----

    [Fact]
    public void Stats_turns_the_counters_on_once_per_mod_and_the_first_call_has_no_rows()
    {
        using var kiln = Started(out var a);
        kiln.Repl.Send(Player, Update(10));  // nothing holds the counters: not counted
        Assert.False(kiln.Repl.Counting);
        Assert.Empty(kiln.As(a, Replication.Stats));
        Assert.True(kiln.Repl.Counting);
        kiln.Repl.Send(Player, new FakeReplMessage(Entity, FakeReplKind.Create, 11) { PayloadBytes = 40 });
        var row = Assert.Single(kiln.As(a, Replication.Stats));
        Assert.Equal((ReplChannel.Client, 1L, 1L, 40L, 1L), (row.Channel, row.Frames, row.Messages, row.Bytes, row.Creates));
        kiln.As(a, Replication.Stats);
        Assert.Single(kiln.Repl.StatsCalls, c => c.On);  // once for the mod, however often it reads

        var b = kiln.Load<ReplMod>(new ModManifest("repl-b"));
        Assert.Single(kiln.As(b, Replication.Stats));  // not the first hold of all: the counters run already
        Assert.Equal(2, kiln.Repl.StatsCalls.Count(c => c.On));
        Assert.Equal(2, kiln.Repl.StatsCalls.Select(c => c.Owner).Distinct().Count());

        kiln.Unload(a, checkCollected: false);
        Assert.True(kiln.Repl.Counting);  // b still holds them
        kiln.Unload(b, checkCollected: false);
        Assert.False(kiln.Repl.Counting);  // the last hold went with its mod's owner
        Assert.DoesNotContain(kiln.Repl.StatsCalls, c => !c.On);  // the native hold goes with the owner, not with a call
    }

    [Fact]
    public void Rows_map_to_records_per_peer_and_channel()
    {
        using var kiln = Started(out var mod);
        var fake = kiln.Repl.ConnectFake(5);
        var bound = kiln.Repl.ConnectFake(6, labPeer: 0x4000_0006);
        kiln.Repl.ConnectLocal(0x0001_0000);
        Assert.Empty(kiln.As(mod, Replication.Stats));

        kiln.Repl.Send(Player, new FakeReplMessage(Entity, FakeReplKind.Create, 20) { PayloadBytes = 100 });
        kiln.Repl.Send(Player, Update(21));
        kiln.Repl.Destroy(Player, new EntityId(77), 21);
        kiln.Repl.Count(Player, unchanged: 7, tooBig: 1, refused: 2, acks: 4, resets: 1);
        kiln.Repl.Drop(Player, channel: 5, records: 3);
        kiln.Repl.Send(fake, Update(21));
        kiln.Repl.Send(bound, Update(21) with { Channel = 5 });
        kiln.Repl.Send(0x0001_0000, Update(21));

        var rows = kiln.As(mod, Replication.Stats);
        Assert.Equal(5, rows.Count);
        var client = rows.Single(r => r.PeerId == Player && r.Channel == ReplChannel.Client);
        Assert.Equal(new ReplPeerStats(null, ReplChannel.Client, Frames: 1, Messages: 3, Bytes: 114, Refused: 2, Creates: 1, Updates: 1,
            DestroyLists: 1, DestroyedIds: 1, Unchanged: 7, TooBig: 1, Acks: 4, Resets: 1, Dropped: 0) { PeerId = Player }, client);
        var prediction = rows.Single(r => r.PeerId == Player && r.Channel == ReplChannel.Prediction);
        Assert.Equal((0L, 3L), (prediction.Messages, prediction.Dropped));
        // no kiln.net peers in the kit: the rows say who by id and flags
        var f = rows.Single(r => r.PeerId == fake);
        Assert.Null(f.Peer);
        Assert.Equal((true, false), (f.Fake, f.Local));
        var b = rows.Single(r => r.PeerId == bound);
        Assert.Equal((ReplChannel.Prediction, true, false), (b.Channel, b.Fake, b.Local));
        var local = rows.Single(r => r.PeerId == 0x0001_0000);
        Assert.Null(local.Peer);
        Assert.Equal((false, true), (local.Fake, local.Local));
        Assert.DoesNotContain(rows, r => r.PeerId == Other);  // nothing sent to it: no row

        kiln.Step();
        kiln.Repl.Send(Player, Update(30));
        Assert.Equal(2, kiln.As(mod, Replication.Stats).Single(r => r.PeerId == Player && r.Channel == ReplChannel.Client).Frames);

        kiln.Repl.Leave(Player);  // a peer's rows start over when its slot changes
        Assert.DoesNotContain(kiln.As(mod, Replication.Stats), r => r.PeerId == Player);
    }

    // ---- watches ----

    [Fact]
    public void A_watch_started_mid_stream_gets_full_values_by_name_at_the_next_step()
    {
        using var kiln = Started(out var mod);
        var transform = new byte[] { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12 };
        var health = new byte[] { 0x64, 0, 0, 0 };
        kiln.Repl.Send(Player, Update(10, ("CurrentTransform", transform)));  // before the watch: no record for it
        var seen = new Seen();
        using var w = Watch(kiln, mod, seen, Entity);
        Assert.True(w.IsActive);
        Assert.Equal((Entity, (NetPeer?)null), (w.Entity, w.Peer));

        kiln.Repl.Send(Player, Update(105, ("CurrentTransform", transform), ("KilnReplTestHealth", health)));
        Assert.Empty(seen.All);  // events come at the start of the next step
        kiln.Step();
        var e = Assert.Single(seen.Sent);
        Assert.Null(e.Peer);
        Assert.Equal((Player, false, false), (e.PeerId, e.Local, e.Fake));
        Assert.Equal((ReplChannel.Client, ReplKind.Update, Entity, 105u, 105u, 101u, 6, false),
            (e.Channel, e.Kind, e.Entity, e.Tick, e.Seq, e.BaselineTick, e.PayloadBytes, e.Gap));
        string t = NameOf(kiln, Transform), hp = NameOf(kiln, Health);
        Assert.Equal("#9", hp);  // no type of that hash: the registry index names it
        Assert.Equal(new[] { hp, t }.Order(StringComparer.Ordinal).ToArray(), e.Components.Keys.Order(StringComparer.Ordinal).ToArray());
        Assert.Equal(transform, e.Components[t].ToArray());
        Assert.Equal(health, e.Components[hp].ToArray());
        Assert.Equal(new[] { t }, e.PackedByGame.ToArray());
    }

    [Fact]
    public void A_gap_is_passed_through_once()
    {
        using var kiln = Started(out var mod);
        var seen = new Seen();
        using var w = Watch(kiln, mod, seen, Entity);
        kiln.Repl.Send(Player, Update(10));
        kiln.Repl.Drop(Player);
        kiln.Repl.Send(Player, Update(11));
        kiln.Repl.Send(Player, Update(12));
        kiln.Step();
        Assert.Equal(new[] { false, true, false }, seen.Sent.Select(s => s.Gap).ToArray());
    }

    [Fact]
    public void A_watch_on_every_peer_sees_each_peers_messages_in_order()
    {
        using var kiln = Started(out var mod);
        var seen = new Seen();
        using var w = Watch(kiln, mod, seen, Entity);
        kiln.Repl.Send(Player, Update(10) with { Kind = FakeReplKind.Create });
        kiln.Repl.Send(Other, Update(10) with { Channel = 5 });
        kiln.Repl.Send(Player, new FakeReplMessage(new EntityId(9999), FakeReplKind.Update, 10));  // another entity
        kiln.Step();
        Assert.Equal(new[] { (Player, ReplKind.Create, ReplChannel.Client), (Other, ReplKind.Update, ReplChannel.Prediction) },
            seen.Sent.Select(s => (s.PeerId, s.Kind, s.Channel)).ToArray());
    }

    [Fact]
    public void Id_reused_ends_the_watch_and_nothing_comes_after()
    {
        using var kiln = Started(out var mod);
        var seen = new Seen();
        using var w = Watch(kiln, mod, seen, Entity);
        kiln.Repl.Send(Player, Update(10) with { Kind = FakeReplKind.Create });
        kiln.Step();
        kiln.Repl.ReuseId(Entity);
        kiln.Repl.Send(Player, Update(11) with { Kind = FakeReplKind.Create });
        kiln.Repl.Send(Player, Update(12));
        kiln.Step();
        kiln.Repl.Send(Player, Update(13));
        kiln.Step(2);
        Assert.Equal(2, seen.All.Count);
        Assert.Equal(10u, Assert.IsType<ReplSent>(seen.All[0]).Tick);
        Assert.Equal(new ReplWatchEnded(Entity, "id reused"), seen.All[1]);
        Assert.Equal((false, true), (w.IsActive, w.HasEnded));
    }

    [Fact]
    public void Destroyed_comes_after_the_frames_messages_then_ended_last()
    {
        using var kiln = Started(out var mod);
        var seen = new Seen();
        using var w = Watch(kiln, mod, seen, Entity);
        kiln.Repl.Send(Player, Update(10));
        kiln.Repl.Destroy(Player, Entity, 10, seq: 11);
        kiln.Step();
        kiln.Repl.Send(Player, Update(12));
        kiln.Step(2);
        Assert.Equal(3, seen.All.Count);
        Assert.Equal(ReplKind.Update, Assert.IsType<ReplSent>(seen.All[0]).Kind);
        var d = Assert.IsType<ReplSent>(seen.All[1]);
        Assert.Equal((ReplKind.Destroyed, 10u, 11u, 0), (d.Kind, d.Tick, d.Seq, d.Components.Count));
        Assert.Empty(d.PackedByGame);
        Assert.Equal(new ReplWatchEnded(Entity, "destroyed"), seen.All[2]);
        Assert.False(w.IsActive);
    }

    [Fact]
    public void A_watch_on_one_peer_ends_when_that_peer_leaves()
    {
        using var kiln = Started(out var mod);
        var seen = new Seen();
        var all = new Seen();
        using var one = Watch(kiln, mod, seen, Entity, PeerOf(Player));
        using var every = Watch(kiln, mod, all, Entity);
        Assert.Equal(Player, one.Peer!.Id);
        kiln.Repl.Send(Other, Update(10));  // another peer: not this watch's
        kiln.Repl.Send(Player, Update(10));
        kiln.Repl.Leave(Player);
        kiln.Step();
        Assert.Equal(2, seen.All.Count);
        Assert.Equal(Player, Assert.IsType<ReplSent>(seen.All[0]).PeerId);
        Assert.Equal(new ReplWatchEnded(Entity, "peer left"), seen.All[1]);
        Assert.Equal(2, all.Sent.Count);
        Assert.True(every.IsActive);  // a watch on every peer goes on
    }

    [Fact]
    public void World_stopped_ends_every_watch_also_at_the_harness_end()
    {
        var seen = new Seen();
        var late = new Seen();
        ReplWatch kept;
        using (var kiln = Started(out var mod))
        {
            using var w = Watch(kiln, mod, seen, Entity);
            kiln.Repl.StopWorld();
            kiln.Step();
            Assert.Equal(new ReplWatchEnded(Entity, "world stopped"), Assert.Single(seen.All));
            kept = Watch(kiln, null, late, new EntityId(31));  // Kiln's own (no mod): only the world's end ends it
        }
        Assert.Equal(new ReplWatchEnded(new EntityId(31), "world stopped"), Assert.Single(late.All));
        Assert.True(kept.HasEnded);
    }

    [Fact]
    public void Dispose_stops_events_and_is_idempotent_on_any_thread()
    {
        using var kiln = Started(out var mod);
        var seen = new Seen();
        var w = Watch(kiln, mod, seen, Entity);
        kiln.Repl.Send(Player, Update(10));
        Task.Run(w.Dispose).Wait();  // before the step that would deliver it
        w.Dispose();
        kiln.Run(w.Dispose);
        kiln.Step(2);
        Assert.Empty(seen.All);
        Assert.Equal((false, false), (w.IsActive, w.HasEnded));
        Assert.Equal(0, kiln.Repl.WatchSlots);
    }

    [Fact]
    public void A_mods_unload_disposes_its_watches()
    {
        using var kiln = Started(out var mod);
        var seen = new Seen();
        var w = Watch(kiln, mod, seen, Entity);
        kiln.Unload(mod, checkCollected: false);
        Assert.False(w.IsActive);
        kiln.Repl.Send(Player, Update(10));
        kiln.Step(2);
        Assert.Empty(seen.All);
        Assert.Equal(0, kiln.Repl.WatchSlots);
    }

    [Fact]
    public void An_ended_watch_gives_its_slot_back_without_a_dispose()
    {
        using var kiln = Started(out var mod);
        var seen = new Seen();
        var watches = new List<ReplWatch>();
        for (var i = 0; i < 64; i++)
        {
            var id = new EntityId((uint)(1000 + i));
            watches.Add(Watch(kiln, mod, seen, id));
            kiln.Repl.Destroy(Player, id, 10);
        }
        kiln.Step(2);
        Assert.Equal(64, seen.All.OfType<ReplWatchEnded>().Count());
        Assert.All(watches, w => Assert.True(w.HasEnded));
        Assert.Equal(0, kiln.Repl.WatchSlots);  // "dispose the watch afterwards (or not: it holds nothing then)"
        using var next = Watch(kiln, mod, seen, Entity);
        Assert.True(next.IsActive);
    }

    // ---- arguments and capabilities ----

    [Fact]
    public void Bad_arguments_throw_ArgumentException()
    {
        using var kiln = Started(out var mod);
        Assert.Throws<ArgumentException>(() => kiln.As(mod, () => Replication.Watch(EntityId.None)));
        var e = Assert.Throws<ArgumentException>(() => kiln.As(mod, () => Replication.Watch(Entity, PeerOf(0x0009_0009))));
        Assert.Equal("peer", e.ParamName);
        Assert.Throws<ArgumentException>(() => kiln.As(mod, () => Replication.WatchReceived(EntityId.None)));
        Assert.Throws<ArgumentException>(() => kiln.Run(() => Replication.Component("")));
        Assert.ThrowsAny<ArgumentException>(() => kiln.Run(() => Replication.Component(null!)));
        Assert.Equal(0, kiln.Repl.WatchSlots);
    }

    [Fact]
    public void Each_member_names_its_capability_when_it_is_off()
    {
        using var kiln = Started(out var mod);
        // the kit's mods run on the server side: the receive side is a game client's
        Assert.Equal("repl.client", Assert.Throws<KilnUnavailableException>(() => kiln.As(mod, () => Replication.WatchReceived(Entity))).Capability);
        Assert.Equal("repl.client", Assert.Throws<KilnUnavailableException>(() => kiln.As(mod, Replication.ReceivedStats)).Capability);
        Assert.Empty(kiln.Repl.StatsCalls);  // nothing held on the way

        kiln.Capabilities.Override("repl.observe", false);
        Assert.Equal("repl.observe", Assert.Throws<KilnUnavailableException>(() => kiln.As(mod, Replication.Stats)).Capability);
        Assert.Equal("repl.observe", Assert.Throws<KilnUnavailableException>(() => kiln.As(mod, () => Replication.Watch(Entity))).Capability);
        Assert.Empty(kiln.Repl.StatsCalls);

        kiln.Capabilities.Override("repl.observe", true);
        kiln.Capabilities.Override("kiln.repl", false);  // the service itself
        Assert.Equal("repl.observe", Assert.Throws<KilnUnavailableException>(() => kiln.As(mod, Replication.Stats)).Capability);
        Assert.Equal("repl.observe", Assert.Throws<KilnUnavailableException>(() => kiln.As(mod, () => Replication.Watch(Entity))).Capability);
        Assert.Null(kiln.Run(() => Replication.Component("CurrentTransform")));

        kiln.Capabilities.Override("kiln.repl", true);
        Assert.Empty(kiln.As(mod, Replication.Stats));
    }

    [Fact]
    public void Component_reads_the_registry_from_the_first_step()
    {
        using var kiln = new KilnHarness();
        kiln.Repl.AddComponent(Transform);
        Assert.Null(kiln.Run(() => Replication.Component("CurrentTransform")));  // no world seen yet
        kiln.Step();
        Assert.Equal(new ReplComponentInfo("CurrentTransform", 7, 12, Hash("CurrentTransform"), ReplicationBit: true, Versioned: false,
            CustomCodec: true, MayBeOnClientChannel: true, MayBeOnPredictionChannel: true), kiln.Run(() => Replication.Component("CurrentTransform")));
        Assert.Null(kiln.Run(() => Replication.Component("NoSuchComponent")));
    }

    // ---- the receive side ----

    [Fact]
    public void A_receive_watch_gets_received_events_then_ended()
    {
        using var kiln = new KilnHarness(new KilnHarnessOptions { Side = HarnessSide.JoinedClient });
        kiln.Repl.AddComponent(Health);
        kiln.Step();
        var seen = new Seen();
        // Replication.WatchReceived's own path: the kit's mods are server-side mods, so its row says wrong side
        using var w = kiln.Run(() =>
        {
            var watch = ReplWatch.Start(Replication.OwnerOfCaller(), null, Entity, 0xFFFFFFFF, null, 2 /* KILN_REPL_RECEIVE */);
            watch.Sent += seen.Add;
            watch.Received += seen.Add;
            watch.Ended += seen.Add;
            return watch;
        });
        var bytes = new byte[] { 7, 0, 0, 0 };
        kiln.Repl.Receive(new FakeReplMessage(Entity, FakeReplKind.Create, 50) { Components = [("KilnReplTestHealth", bytes)] });
        kiln.Repl.Receive(new FakeReplMessage(Entity, FakeReplKind.Update, 51) { Gap = true });
        kiln.Repl.ReceiveDestroy(Entity, 52);
        kiln.Step();
        Assert.Equal(4, seen.All.Count);
        var c = Assert.IsType<ReplReceived>(seen.All[0]);
        Assert.Equal((ReplKind.Create, Entity, 50u, false), (c.Kind, c.Entity, c.Tick, c.Gap));
        Assert.Equal(bytes, c.Components[NameOf(kiln, Health)].ToArray());
        var u = Assert.IsType<ReplReceived>(seen.All[1]);
        Assert.Equal((ReplKind.Update, 51u, true, 0), (u.Kind, u.Tick, u.Gap, u.Components.Count));
        Assert.Equal(ReplKind.Destroyed, Assert.IsType<ReplReceived>(seen.All[2]).Kind);
        Assert.Equal(new ReplWatchEnded(Entity, "destroyed"), seen.All[3]);
        Assert.Empty(seen.Sent);
    }

    [Fact]
    public void A_dedicated_server_has_no_receive_side()
    {
        using var kiln = Started(out _);
        var e = Assert.Throws<KilnUnavailableException>(() => kiln.Run(() =>
            ReplWatch.Start(Replication.OwnerOfCaller(), null, Entity, 0xFFFFFFFF, null, 2 /* KILN_REPL_RECEIVE */)));
        Assert.Equal("repl.client", e.Capability);
    }
}
