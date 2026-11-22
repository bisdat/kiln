// SPDX-License-Identifier: MIT
// Kiln.Replication's peer selectors (Peers.cs) and the per-peer "has this extension" decision from the handshake property
// repl.ext (ReplicatedExtension.cs), on kiln.net peers made here (no game, no harness).
using System.Reflection;
using Kiln;
using Xunit;

namespace ReplicationTests;

public sealed class PeersTests
{
    private sealed class Ext(string name, int version, int size, uint hash) : ReplicatedExtension(name, version, size, hash)
    {
        internal override IReadOnlyList<SidecarProvider> Providers => [];

        internal override void RaiseChanged(EntityId entity, uint createTick, ReadOnlySpan<byte> value, NetPeer? peer)
        {
        }
    }

    private static NetPeer Peer(uint id, NetPeerState state, string? ext = null, bool cleared = false)
    {
        var p = (NetPeer)Activator.CreateInstance(typeof(NetPeer), BindingFlags.NonPublic | BindingFlags.Instance, null, [id], null)!;
        typeof(NetPeer).GetProperty(nameof(NetPeer.State))!.SetValue(p, state);
        var props = new Dictionary<string, string> { ["nettest.client"] = "x" };
        if (ext is not null) props["repl.ext"] = ext;
        typeof(NetPeer).GetProperty(nameof(NetPeer.Properties))!.SetValue(p, props);
        typeof(NetPeer).GetField("Cleared", BindingFlags.NonPublic | BindingFlags.Instance)!.SetValue(p, cleared);
        return p;
    }

    private static readonly Ext Water = new("bigcan:water", 1, 8, 0x1234abcd);

    [Fact]
    public void All_and_Vanilla()
    {
        var kiln = Peer(0x0001_0003, NetPeerState.Ready);
        var vanilla = Peer(0x0002_0004, NetPeerState.Vanilla);
        var connecting = Peer(0x0003_0005, NetPeerState.Connecting);
        Assert.True(Peers.All.Matches(kiln) && Peers.All.Matches(vanilla));
        Assert.False(Peers.Vanilla.Matches(kiln));
        Assert.True(Peers.Vanilla.Matches(vanilla));
        Assert.True(Peers.Vanilla.Matches(connecting));  // no Kiln handshake yet: treated as vanilla until it finishes
        // machines without a NetPeer: an unbound lab fake is vanilla, a hosted world's own client never is
        Assert.True(Peers.Vanilla.Matches(new PeerFacts(0x8000_0010, null, false, true)));
        Assert.False(Peers.Vanilla.Matches(new PeerFacts(0x0001_0000, null, true, false)));
        Assert.Equal("vanilla peers", Peers.Vanilla.ToString());
    }

    [Fact]
    public void Facts_from_kiln_repl_rows_resolve_bound_fakes_only()
    {
        var lab = Peer(0x4000_0006, NetPeerState.Ready);
        var table = new Dictionary<uint, NetPeer> { [lab.Id] = lab };
        var bound = PeerFacts.Of(lab.Id, ReplNative.PeerFake | ReplNative.PeerBound, table);
        Assert.Same(lab, bound.Peer);
        Assert.True(bound.Fake && !bound.Local);
        Assert.Null(PeerFacts.Of(lab.Id, ReplNative.PeerFake, table).Peer);   // a fake without a bound lab peer has none
        Assert.Null(PeerFacts.Of(lab.Id, ReplNative.PeerLocal, table).Peer);  // the local client has none either
        Assert.True(PeerFacts.Of(0x0001_0000, ReplNative.PeerLocal, table).Local);
        Assert.True(PeerFacts.Of(Peer(0x0001_0000, NetPeerState.Ready)).Local);  // machine 0's connection on a host
        Assert.False(PeerFacts.Of(Peer(0x0001_0003, NetPeerState.Ready)).Local);
    }

    [Fact]
    public void With_and_Without_follow_the_handshake_property()
    {
        var same = Peer(1, NetPeerState.Ready, "other:thing@3:4:0000ffff; bigcan:water@1:8:1234abcd");
        var none = Peer(2, NetPeerState.Ready);
        var version = Peer(3, NetPeerState.Ready, "bigcan:water@2:8:1234abcd");
        var size = Peer(4, NetPeerState.Ready, "bigcan:water@1:12:1234abcd");
        var layout = Peer(5, NetPeerState.Ready, "bigcan:water@1:8:1234abce");
        var vanilla = Peer(6, NetPeerState.Vanilla, "bigcan:water@1:8:1234abcd");  // a property without the handshake: no
        var with = Peers.With(Water);
        var without = Peers.Without(Water);
        Assert.True(with.Matches(same) && !without.Matches(same));
        foreach (var p in new[] { none, version, size, layout, vanilla })
        {
            Assert.False(with.Matches(p));
            Assert.True(without.Matches(p));
        }
        Assert.True(Water.IsAvailableFor(same));
        Assert.False(Water.IsAvailableFor(layout));
        Assert.Equal("peers with bigcan:water@1", with.Description);
        // the local client has every registered extension; an unbound fake none
        Assert.True(with.Matches(new PeerFacts(0x0001_0000, null, true, false)));
        Assert.True(without.Matches(new PeerFacts(0x8000_0010, null, false, true)));
        var gone = new Ext("gone:ext", 1, 4, 1);
        gone.MarkUnregistered();
        Assert.False(Peers.With(gone).Matches(new PeerFacts(0x0001_0000, null, true, false)));
        Assert.False(gone.IsAvailableFor(Peer(7, NetPeerState.Ready, "gone:ext@1:4:00000001")));
    }

    [Fact]
    public void A_difference_is_named()
    {
        Assert.Null(ExtensionAdvert.Difference(Water, new AdvertisedExtension(1, 8, 0x1234abcd)));
        Assert.Equal("version 1 here, 2 there", ExtensionAdvert.Difference(Water, new AdvertisedExtension(2, 8, 0x1234abcd)));
        Assert.Equal("size 8 here, 12 there", ExtensionAdvert.Difference(Water, new AdvertisedExtension(1, 12, 0x1234abcd)));
        Assert.Equal("layout 1234abcd here, 1234abce there", ExtensionAdvert.Difference(Water, new AdvertisedExtension(1, 8, 0x1234abce)));
    }

    [Fact]
    public void The_property_round_trips_and_skips_what_it_does_not_read()
    {
        var b = new Ext("a:b.c-d", 12, 1024, 0xfedcba98);
        var value = ExtensionAdvert.Join([Water, b]);
        Assert.Equal("bigcan:water@1:8:1234abcd;a:b.c-d@12:1024:fedcba98", value);
        var parsed = ExtensionAdvert.Parse(value + ";broken;@1:2:3;x@y:1:2;later@1:2:00000003:extra");
        Assert.Equal(new AdvertisedExtension(1, 8, 0x1234abcd), parsed["bigcan:water"]);
        Assert.Equal(new AdvertisedExtension(12, 1024, 0xfedcba98), parsed["a:b.c-d"]);
        Assert.Equal(new AdvertisedExtension(1, 2, 3), parsed["later"]);  // a later field after the hash is ignored
        Assert.Equal(3, parsed.Count);
    }

    [Fact]
    public void LackingContent_is_every_peer_without_the_current_content_state()
    {
        var vanilla = Peer(1, NetPeerState.Vanilla);
        var loading = Peer(2, NetPeerState.Ready, cleared: false);
        var current = Peer(3, NetPeerState.Ready, cleared: true);
        var serving = Peers.ServingContent;
        try
        {
            Peers.ServingContent = () => true;  // the server serves a content state: a Kiln client must have acked it
            Assert.True(Peers.LackingContent.Matches(vanilla));
            Assert.True(Peers.LackingContent.Matches(loading));
            Assert.False(Peers.LackingContent.Matches(current));
            Peers.ServingContent = () => false;  // no content state, no Kiln-only template: nothing to lack, nothing to ack
            Assert.False(Peers.LackingContent.Matches(loading));
            Assert.True(Peers.LackingContent.Matches(vanilla));  // a vanilla peer still counts (it has no Kiln at all)
        }
        finally
        {
            Peers.ServingContent = serving;
        }
        Assert.False(Peers.LackingContent.Matches(new PeerFacts(0x0001_0000, null, true, false)));  // the host's own client
        Assert.True(Peers.LackingContent.Matches(new PeerFacts(0x8000_0010, null, false, true)));   // an unbound lab fake
        var saved = Peers.ContentCurrent;
        try
        {
            Peers.ContentCurrent = _ => false;  // the content part's word wins
            Assert.True(Peers.LackingContent.Matches(current));
        }
        finally
        {
            Peers.ContentCurrent = saved;
        }
    }

    [Fact]
    public void Null_arguments_throw()
    {
        Assert.Throws<ArgumentNullException>(() => Peers.With(null!));
        Assert.Throws<ArgumentNullException>(() => Peers.Without(null!));
        Assert.Throws<ArgumentNullException>(() => Peers.All.Matches((NetPeer)null!));
        Assert.Throws<ArgumentNullException>(() => Water.IsAvailableFor(null!));
    }
    [Fact]
    public void LackingContent_follows_the_acked_content_state_not_the_template_count()
    {
        var current = Peer(3, NetPeerState.Ready, cleared: true);
        var (served, acked) = (Peers.ServedState, Peers.AckedState);
        try
        {
            Peers.ServedState = () => 7u;
            Peers.AckedState = _ => 7u;
            Assert.False(Peers.LackingContent.Matches(current));
            // a hot-loaded mod adds a Kiln-only item without a template: the served state moves on, the peer's ack does not
            Peers.ServedState = () => 8u;
            Assert.True(Peers.LackingContent.Matches(current));
            Peers.AckedState = _ => null;  // never acked
            Assert.True(Peers.LackingContent.Matches(current));
        }
        finally
        {
            (Peers.ServedState, Peers.AckedState) = (served, acked);
        }
    }

    [Fact]
    public void RemapItems_of_all_peers_covers_every_machine_peer()
    {
        // FilterHost.PeersOf(Peers.All) answers [] (every peer, for filter_add); the remap's peer list needs the ids
        Assert.Equal(new uint[] { 0x10002, 0x10005 }, ItemRemap.ExpandAll([], [0, 0, 0x10002, 0, 0, 0x10005]));
        Assert.Equal(new uint[] { 0x10002 }, ItemRemap.ExpandAll([0x10002], [0, 0, 0x10002, 0, 0, 0x10005]));
        Assert.Empty(ItemRemap.ExpandAll([0], [0, 0, 0x10002]));  // [0]: the selector picked none
    }
}
