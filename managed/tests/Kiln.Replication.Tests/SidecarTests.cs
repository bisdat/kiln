// SPDX-License-Identifier: MIT
// The sidecar server and client cores (SidecarServerCore, SidecarClientCore) on fake hosts and fake frames: who gets
// frames (never a vanilla peer, an unbound fake or a peer without the extension), UPDATE/CLEAR per peer and generation,
// the budget, DEFS changes, RESYNC, late registration from the state map, the hosted world's own client; on the client:
// stale generations, a value that waits for its create and then asks again, DEFS changes, destroys, CLEAR.
using System.Reflection;
using System.Runtime.InteropServices;
using Kiln;
using Xunit;

namespace ReplicationTests;

[StructLayout(LayoutKind.Sequential, Pack = 1)]
public struct FakeFuel : IGameComponent<FakeFuel>
{
    public int Units;
    public static uint Hash => 0x1234;
    public static string Name => "FakeFuel";
    public static string QualifiedName => "keen::ecs::FakeFuel";
    public static int Size => 4;
    public static ulong Layout => 1;
}

[CollectionDefinition("sidecars", DisableParallelization = true)]
public sealed class SidecarCollection;

[Collection("sidecars")]
public sealed class SidecarTests : IDisposable
{
    private static int _n;
    private readonly string _name = $"test:water{Interlocked.Increment(ref _n)}";
    private readonly List<IDisposable> _dispose = [];

    public SidecarTests() => Sidecars.TestCaps = ReplNetNative.CapTrack;

    public void Dispose()
    {
        foreach (var d in _dispose) d.Dispose();
        Sidecars.TestCaps = null;
    }

    private sealed class Host : ISidecarHost
    {
        public readonly HashSet<uint> WithFuel = [];
        public readonly Dictionary<uint, (uint, uint)[]> Maps = new();
        public readonly List<(uint Peer, ushort Msg, byte[] Body)> Sent = [];
        public readonly Dictionary<uint, NetPeer> Net = new();
        public readonly List<string> Lines = [];
        public int Posted;
        public bool Has(uint entity, string component) => component == FakeFuel.Name && WithFuel.Contains(entity);
        public (uint Entity, uint CreateTick)[]? PeerEntities(uint peer) => Maps.TryGetValue(peer, out var m) ? m : null;
        public IReadOnlyCollection<uint>? Find(string component) => WithFuel;
        public bool Send(SidecarPeer peer, ushort message, byte[] body)
        {
            Sent.Add((peer.Id, message, body));
            return true;
        }
        public PeerFacts Resolve(uint peer, byte flags) => PeerFacts.Of(peer, flags, Net);
        public void PostToClient(Action work)
        {
            Posted++;
            work();
        }
        public int MaxMessageBytes => 256 * 1024;
        public void Log(LogLevel level, string line) => Lines.Add(line);
        public List<(uint Peer, ushort Msg, byte[] Body)> Of(uint peer, ushort msg) => Sent.Where(s => s.Peer == peer && s.Msg == msg).ToList();
    }

    private static NetPeer Peer(uint id, NetPeerState state, string? ext, bool channel = true)
    {
        var p = (NetPeer)Activator.CreateInstance(typeof(NetPeer), BindingFlags.NonPublic | BindingFlags.Instance, null, [id], null)!;
        typeof(NetPeer).GetProperty(nameof(NetPeer.State))!.SetValue(p, state);
        var props = new Dictionary<string, string>();
        if (ext is not null) props["repl.ext"] = ext;
        typeof(NetPeer).GetProperty(nameof(NetPeer.Properties))!.SetValue(p, props);
        if (channel) ((Dictionary<string, ushort>)typeof(NetPeer).GetField("Channels", BindingFlags.NonPublic | BindingFlags.Instance)!.GetValue(p)!)["kiln:repl"] = 7;
        return p;
    }

    private static ReplNetNative.TrackEvent Ev(uint peer, byte kind, uint entity, uint tick, byte machine = 3, byte flags = 0) =>
        new() { Peer = peer, Kind = kind, Entity = entity, Tick = tick, Machine = machine, PeerFlags = flags };

    private ReplicatedExtension<Water> Register(Func<uint, Water?> value)
    {
        var ext = Replication.Extension<Water>(_name, 1);
        _dispose.Add(ext);
        _dispose.Add(ext.On<FakeFuel>((EntityId e, ref Water v) =>
        {
            if (value(e.Value) is not { } w) return false;
            v = w;
            return true;
        }));
        return ext;
    }

    private string Advert(ReplicatedExtension ext) => ExtensionAdvert.Format(ext.Name, ext.Version, ext.Size, ext.LayoutHash);

    private static List<SidecarWire.Value> Updates(Host h, uint peer, Func<ushort, int> size)
    {
        var all = new List<SidecarWire.Value>();
        foreach (var (_, _, body) in h.Of(peer, SidecarWire.Update))
        {
            var r = new NetReader(body);
            all.AddRange(SidecarWire.ReadUpdate(ref r, size).Items);
        }
        return all;
    }

    private static List<SidecarWire.Key> Clears(Host h, uint peer)
    {
        var all = new List<SidecarWire.Key>();
        foreach (var (_, _, body) in h.Of(peer, SidecarWire.Clear))
        {
            var r = new NetReader(body);
            all.AddRange(SidecarWire.ReadClear(ref r).Keys);
        }
        return all;
    }

    private static uint Units(SidecarWire.Value v) => MemoryMarshal.Read<Water>(v.Bytes.Span).Charges;

    [Fact]
    public void Only_peers_with_the_extension_get_frames_and_values_follow_the_provider()
    {
        var charges = new Dictionary<uint, uint> { [5000] = 2000 };
        var ext = Register(e => charges.TryGetValue(e, out var c) && c > 1023 ? new Water { Charges = c * 64, Max = 128000 } : null);
        var h = new Host();
        h.WithFuel.Add(5000);
        const uint with = 0x0001_0003, without = 0x0002_0004, vanilla = 0x0003_0005, fake = 0x8000_0006;
        h.Net[with] = Peer(with, NetPeerState.Ready, Advert(ext));
        h.Net[without] = Peer(without, NetPeerState.Ready, null);
        h.Net[vanilla] = Peer(vanilla, NetPeerState.Vanilla, Advert(ext), channel: false);
        var core = new SidecarServerCore(h);
        core.OnEvents([Ev(with, ReplNetNative.TrackCreated, 5000, 100), Ev(without, ReplNetNative.TrackCreated, 5000, 101, 4),
            Ev(vanilla, ReplNetNative.TrackCreated, 5000, 102, 5), Ev(fake, ReplNetNative.TrackCreated, 5000, 103, 6, ReplNative.PeerFake)]);
        core.Step(10, 1);
        Assert.Single(h.Of(with, SidecarWire.Defs));
        var u = Updates(h, with, id => id == ext.Id ? 8 : -1);
        Assert.Equal((5000u, 100u, ext.Id, 128000u), (u[0].Entity, u[0].CreateTick, u[0].ExtId, Units(u[0])));
        Assert.Empty(h.Sent.Where(s => s.Peer != with));  // no frame at all to the others, not even DEFS
        Assert.True(core.PeerStates[with].FramesSent >= 2 && core.PeerStates[without].FramesSent == 0 && core.PeerStates[vanilla].FramesSent == 0);
        Assert.Contains(h.Lines, l => l == $"repl: peer 0x{with:x} has {ext}");

        // the same value: nothing new; a new value: an UPDATE; at 1,023 charges or less: a CLEAR
        h.Sent.Clear();
        core.Step(10.1, 2);
        Assert.Empty(h.Sent);
        charges[5000] = 1500;
        core.Step(10.2, 3);
        Assert.Equal(96000u, Units(Assert.Single(Updates(h, with, id => 8))));
        charges[5000] = 1023;
        core.Step(10.3, 4);
        Assert.Equal(new SidecarWire.Key(5000, 100, ext.Id), Assert.Single(Clears(h, with)));
        core.Step(10.4, 5);
        Assert.Single(Clears(h, with));  // said once
    }

    [Fact]
    public void Destroys_and_leaves_clear_and_a_new_generation_is_sent_again()
    {
        var ext = Register(e => new Water { Charges = e, Max = 1 });
        var h = new Host();
        h.WithFuel.UnionWith([1, 2, 3]);
        const uint p = 0x0001_0003;
        h.Net[p] = Peer(p, NetPeerState.Ready, Advert(ext));
        var core = new SidecarServerCore(h);
        core.OnEvents([Ev(p, ReplNetNative.TrackCreated, 1, 10), Ev(p, ReplNetNative.TrackCreated, 2, 11), Ev(p, ReplNetNative.TrackCreated, 3, 12)]);
        core.Step(1, 1);
        Assert.Equal(3, Updates(h, p, _ => 8).Count);
        h.Sent.Clear();
        core.OnEvents([Ev(p, ReplNetNative.TrackDestroyed, 1, 20), Ev(p, ReplNetNative.TrackLeft, 2, 20), Ev(p, ReplNetNative.TrackCreated, 3, 21)]);
        core.Step(2, 2);
        Assert.Equal([new SidecarWire.Key(1, 10, ext.Id), new SidecarWire.Key(2, 11, ext.Id)], Clears(h, p).OrderBy(k => k.Entity).ToList());
        var again = Assert.Single(Updates(h, p, _ => 8));
        Assert.Equal((3u, 21u), (again.Entity, again.CreateTick));  // the id's new generation gets its value under its tick
    }

    [Fact]
    public void The_budget_holds_values_back_and_the_newest_one_goes()
    {
        var value = 1u;
        var ext = Register(_ => new Water { Charges = value, Max = 0 });
        var h = new Host();
        const uint p = 0x0001_0003;
        h.Net[p] = Peer(p, NetPeerState.Ready, Advert(ext));
        var core = new SidecarServerCore(h) { Budget = 1024 };
        var events = new List<ReplNetNative.TrackEvent>();
        for (uint e = 1; e <= 200; e++)
        {
            h.WithFuel.Add(e);
            events.Add(Ev(p, ReplNetNative.TrackCreated, e, 5));
        }
        core.OnEvents(events.ToArray());
        core.Step(0, 1);
        var first = Updates(h, p, _ => 8).Count;
        Assert.Equal(1024 / 18, first);  // 18 bytes an item: 56 of 200 within the first second's 1,024 bytes
        h.Sent.Clear();
        value = 2;  // what waits is sent with its newest value
        core.Step(0.5, 2);
        var half = Updates(h, p, _ => 8);
        Assert.InRange(half.Count, 27, 29);
        Assert.All(half, v => Assert.Equal(2u, Units(v)));
        core.Step(1.5, 3);
        core.Step(2.5, 4);
        core.Step(3.5, 5);
        core.Step(4.5, 6);
        Assert.Equal(200, core.PeerStates[p].Sent.Count);
        Assert.Empty(core.PeerStates[p].Queue);
    }

    [Fact]
    public void A_definition_change_sends_DEFS_and_every_value_again()
    {
        var ext = Register(e => new Water { Charges = 7, Max = 7 });
        var h = new Host();
        h.WithFuel.Add(9);
        const uint p = 0x0001_0003;
        h.Net[p] = Peer(p, NetPeerState.Ready, Advert(ext));
        var core = new SidecarServerCore(h);
        core.OnEvents([Ev(p, ReplNetNative.TrackCreated, 9, 40)]);
        core.Step(1, 1);
        h.Sent.Clear();
        using var other = Replication.Extension<WithUShort>(_name + ".b", 1);  // a hot reload elsewhere: the ids' table changes
        core.Step(1.1, 2);
        var defs = h.Of(p, SidecarWire.Defs);
        Assert.Single(defs);
        var r = new NetReader(defs[0].Body);
        Assert.Contains(SidecarWire.ReadDefs(ref r), d => d.Name == _name + ".b");
        Assert.Single(Updates(h, p, _ => 8));  // the value again, after the new DEFS
        Assert.True(h.Sent.FindIndex(s => s.Msg == SidecarWire.Defs) < h.Sent.FindIndex(s => s.Msg == SidecarWire.Update));
    }

    [Fact]
    public void Resync_sends_again_once_a_second_and_late_tracking_reads_the_state_map()
    {
        var ext = Register(e => new Water { Charges = e, Max = 0 });
        var h = new Host();
        h.WithFuel.UnionWith([70, 71]);
        const uint p = 0x0001_0003;
        h.Net[p] = Peer(p, NetPeerState.Ready, Advert(ext));
        h.Maps[p] = [(70, 0), (71, 33)];  // 70 was created before the tracking began: createTick 0
        var core = new SidecarServerCore(h);
        core.Seen(p, 3, 0);
        core.Step(1, 1);
        var u = Updates(h, p, _ => 8).OrderBy(v => v.Entity).ToList();
        Assert.Equal([(70u, 0u), (71u, 33u)], u.Select(v => (v.Entity, v.CreateTick)).ToList());
        h.Sent.Clear();
        Assert.True(core.OnResync(p, [new SidecarWire.ResyncItem(71, 33)], 2));
        Assert.False(core.OnResync(p, [new SidecarWire.ResyncItem(70, 0)], 2.5));  // at most one a second
        core.Step(2.6, 2);
        Assert.Equal(71u, Assert.Single(Updates(h, p, _ => 8)).Entity);
        // a gap: the map is read again; what is no longer in it is cleared
        h.Sent.Clear();
        h.Maps[p] = [(71, 33)];
        core.OnEvents([Ev(p, ReplNetNative.TrackGap, 0, 0)]);
        core.Step(3, 3);
        Assert.Equal(new SidecarWire.Key(70, 0, ext.Id), Assert.Single(Clears(h, p)));
        // the peer goes: its state with it
        core.OnEvents([Ev(p, ReplNetNative.TrackPeerGone, 0, 0)]);
        Assert.False(core.PeerStates.ContainsKey(p));
    }

    [Fact]
    public void An_announcement_moves_a_peer_in_and_out()
    {
        var ext = Register(_ => new Water { Charges = 1, Max = 2 });
        var h = new Host();
        h.WithFuel.Add(5);
        const uint p = 0x0001_0003;
        h.Net[p] = Peer(p, NetPeerState.Ready, null);
        var core = new SidecarServerCore(h);
        core.OnEvents([Ev(p, ReplNetNative.TrackCreated, 5, 9)]);
        core.Step(1, 1);
        Assert.Empty(h.Sent);
        ExtensionAdvert.Announce(p, Advert(ext));  // a hot load on the client
        try
        {
            core.Step(2, 2);
            Assert.Single(h.Of(p, SidecarWire.Defs));
            Assert.Single(Updates(h, p, _ => 8));
            ExtensionAdvert.Announce(p, "");  // and unloaded again
            core.Step(3, 3);
            Assert.Contains(h.Lines, l => l == $"repl: peer 0x{p:x} lost {ext}");
            Assert.Empty(core.PeerStates[p].Sent);
        }
        finally
        {
            ExtensionAdvert.Forget(p);
        }
    }

    [Fact]
    public void The_hosted_worlds_own_client_gets_no_frame_but_its_values_and_Changed()
    {
        var ext = Register(e => new Water { Charges = 42, Max = 43 });
        var changed = new List<ReplExtensionChanged<Water>>();
        ext.Changed += changed.Add;
        var h = new Host();
        h.WithFuel.Add(8);
        const uint local = 0x0001_0000;
        var core = new SidecarServerCore(h);
        core.OnEvents([Ev(local, ReplNetNative.TrackCreated, 8, 50, 0, ReplNative.PeerLocal)]);
        core.Step(1, 1);
        Assert.Empty(h.Sent);
        Assert.Equal(1, h.Posted);
        var c = Assert.Single(changed);
        Assert.Equal((8u, 50u, 42u), (c.Entity.Value, c.CreateTick, c.Value!.Value.Charges));
        Assert.Null(c.Peer);
        Assert.True(ext.TryGet(new EntityId(8), out var v) && v.Max == 43);  // the providers' view, from any thread
        Assert.False(ext.TryGet(new EntityId(9), out _));
    }

    [Fact]
    public void A_throwing_provider_changes_nothing()
    {
        var throwNow = false;
        var ext = Register(_ => throwNow ? throw new InvalidOperationException("boom") : new Water { Charges = 5, Max = 5 });
        var h = new Host();
        h.WithFuel.Add(4);
        const uint p = 0x0001_0003;
        h.Net[p] = Peer(p, NetPeerState.Ready, Advert(ext));
        var core = new SidecarServerCore(h);
        core.OnEvents([Ev(p, ReplNetNative.TrackCreated, 4, 1)]);
        core.Step(1, 1);
        h.Sent.Clear();
        throwNow = true;
        core.Step(2, 2);
        Assert.Empty(h.Sent);  // no CLEAR: the peer keeps the value it has
        Assert.Single(core.PeerStates[p].Sent);
    }

    // ---- the client ----

    private (ReplicatedExtension<Water> Ext, SidecarClientCore Core, List<ReplExtensionChanged<Water>> Changed) ClientWith(bool tracking)
    {
        var ext = Replication.Extension<Water>(_name, 1);
        _dispose.Add(ext);
        var changed = new List<ReplExtensionChanged<Water>>();
        ext.Changed += changed.Add;
        var core = new SidecarClientCore { Tracking = tracking };
        core.OnDefs([new SidecarWire.Def(77, ext.Name, 1, 8, ext.LayoutHash), new SidecarWire.Def(78, "other:thing", 1, 4, 5)]);
        return (ext, core, changed);
    }

    private static SidecarWire.Value V(uint entity, uint ct, uint charges, ushort ext = 77)
    {
        var w = new Water { Charges = charges, Max = 128000 };
        var b = new byte[8];
        MemoryMarshal.Write(b, in w);
        return new SidecarWire.Value(entity, ct, ext, b);
    }

    [Fact]
    public void A_client_keeps_the_generation_it_has_and_drops_stale_ones()
    {
        var (ext, core, changed) = ClientWith(tracking: true);
        core.OnTrack(ReplNetNative.TrackCreated, 600, 200);
        core.OnUpdate([V(600, 200, 96000), V(600, 100, 1), V(601, 5, 2, ext: 78)], 0);
        core.Publish();
        Assert.True(ext.TryGet(new EntityId(600), out var v) && v.Charges == 96000);  // the stale one (100) was dropped
        Assert.Equal((600u, 200u, 96000u), (changed[0].Entity.Value, changed[0].CreateTick, changed[0].Value!.Value.Charges));
        // createTick 0: the generation the client has
        core.OnUpdate([V(600, 0, 50)], 0);
        core.Publish();
        Assert.True(ext.TryGet(new EntityId(600), out v) && v.Charges == 50);
        // CLEAR of another generation: kept; of this one: gone
        core.OnClear([new SidecarWire.Key(600, 150, 77)]);
        core.Publish();
        Assert.True(ext.TryGet(new EntityId(600), out _));
        core.OnClear([new SidecarWire.Key(600, 200, 77)]);
        core.Publish();
        Assert.False(ext.TryGet(new EntityId(600), out _));
        Assert.Null(changed[^1].Value);
    }

    [Fact]
    public void A_value_for_a_create_not_seen_yet_waits_then_asks_again()
    {
        var (ext, core, _) = ClientWith(tracking: true);
        core.OnUpdate([V(700, 300, 1000)], 10);
        core.Publish();
        Assert.False(ext.TryGet(new EntityId(700), out _));
        Assert.Equal(1, core.PendingCount);
        core.OnTrack(ReplNetNative.TrackCreated, 700, 300);  // the create arrives: the value is applied
        core.Publish();
        Assert.True(ext.TryGet(new EntityId(700), out var v) && v.Charges == 1000);
        Assert.Equal(0, core.PendingCount);
        // one that never meets its create: after 5 s a RESYNC names it, at most once a second
        core.OnUpdate([V(701, 400, 1), V(702, 401, 2)], 20);
        Assert.Empty(core.Due(24.9));
        var due = core.Due(25.1);
        Assert.Equal([new SidecarWire.ResyncItem(701, 400), new SidecarWire.ResyncItem(702, 401)], due.OrderBy(d => d.Entity).ToList());
        core.OnUpdate([V(703, 402, 3)], 20);
        Assert.Empty(core.Due(25.5));  // within the second
        Assert.Single(core.Due(26.2));
    }

    [Fact]
    public void Destroys_definition_changes_and_disconnects_drop_values()
    {
        var (ext, core, changed) = ClientWith(tracking: true);
        core.OnTrack(ReplNetNative.TrackCreated, 800, 1);
        core.OnTrack(ReplNetNative.TrackCreated, 801, 1);
        core.OnUpdate([V(800, 1, 1), V(801, 1, 2)], 0);
        core.Publish();
        core.OnTrack(ReplNetNative.TrackDestroyed, 800, 2);
        core.Publish();
        Assert.False(ext.TryGet(new EntityId(800), out _));
        Assert.True(ext.TryGet(new EntityId(801), out _));
        // a new DEFS: every value goes (the server sends them again under the new ids)
        core.OnDefs([new SidecarWire.Def(90, ext.Name, 1, 8, ext.LayoutHash)]);
        core.Publish();
        Assert.False(ext.TryGet(new EntityId(801), out _));
        Assert.Null(changed[^1].Value);
        core.OnUpdate([V(801, 1, 3, ext: 90)], 0);
        core.Publish();
        Assert.True(ext.TryGet(new EntityId(801), out var v) && v.Charges == 3);
        // the old id means nothing now
        core.OnUpdate([V(801, 1, 4, ext: 77)], 0);
        core.Publish();
        Assert.True(ext.TryGet(new EntityId(801), out v) && v.Charges == 3);
        core.Reset();
        core.Publish();
        Assert.False(ext.TryGet(new EntityId(801), out _));
    }

    [Fact]
    public void Without_the_receive_track_the_newest_generation_wins()
    {
        var (ext, core, _) = ClientWith(tracking: false);
        core.OnUpdate([V(900, 10, 1)], 0);
        core.OnUpdate([V(900, 5, 2)], 0);   // older: dropped
        core.OnUpdate([V(900, 12, 3)], 0);  // newer: wins
        core.Publish();
        Assert.True(ext.TryGet(new EntityId(900), out var v) && v.Charges == 3);
        Assert.Equal(0, core.PendingCount);
    }

    [Fact]
    public void A_definition_that_differs_is_read_past()
    {
        var ext = Replication.Extension<Water>(_name, 1);
        _dispose.Add(ext);
        var core = new SidecarClientCore();
        core.OnDefs([new SidecarWire.Def(5, ext.Name, 1, 8, ext.LayoutHash ^ 1)]);  // another layout
        core.OnUpdate([V(1, 1, 1, ext: 5)], 0);
        core.Publish();
        Assert.False(ext.TryGet(new EntityId(1), out _));
        Assert.Equal(8, core.SizeOf(5));  // still read past (the message's other values stay readable)
    }
}
