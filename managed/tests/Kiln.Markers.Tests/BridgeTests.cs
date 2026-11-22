// SPDX-License-Identifier: MIT
// The built-in receiver's bridge (chapter 04 §4.4) on a fake kiln.markers.bridge table and a fake transport: it attaches
// once under no mod, synchronises every Network peer (Kiln, vanilla, pending) with flags and a generation per connection
// object, registers the hosted loopback on both roles keyed by both world epochs and the local player (and leaves the
// host's own Network row and the self-loop's frames out), copies inbound bytes to the right role, peer and generation, and
// hands the native pump's packets to the exact connection.
using Kiln;
using Xunit;

namespace MarkersTests;

public sealed class BridgeTests : MarkersFixture, IDisposable
{
    private readonly FakeTransport _t = new();

    public BridgeTests()
    {
        FakeBridge.Install();
        MarkersBridge.Reset();
    }

    public new void Dispose()
    {
        MarkersBridge.Reset();
        FakeBridge.Free();
        base.Dispose();
    }

    private static MarkerPeerView Peer(object key, uint id, NetPeerState state, uint player = 0, bool channel = true, bool lab = false, bool local = false) =>
        new(key, id, state, player, channel, lab, local);

    private static MarkersNative.Peer LastUp(uint role, uint id) => FakeBridge.Ups.Last(p => p.Role == role && p.Id == id);

    [Fact]
    public void It_attaches_once_under_no_mod_and_opens_the_channel()
    {
        var mod = Mod("some.mod", 3);
        As(mod, () => MarkersBridge.Start(_t));
        MarkersBridge.Start(_t);
        Assert.Equal("attached", MarkersBridge.State);
        Assert.Equal(1, FakeBridge.Attaches);
        Assert.Equal(1, _t.Opens);
        Assert.Equal(4242ul, FakeBridge.AttachedOwner);  // Kiln's own owner, not the mod's
        Assert.NotNull(_t.Handler);
    }

    [Fact]
    public void A_channel_limit_below_16384_leaves_the_link_off_and_says_why()
    {
        _t.Limit = 8192;
        MarkersBridge.Start(_t);
        Assert.Equal(0, FakeBridge.Attaches);
        Assert.False(Capabilities.Is("markers.send"));
        var row = Capabilities.Get("markers.send");
        Assert.Equal(CapabilityReason.Policy, row.Reason);
        Assert.Contains("16384", row.ReasonText);
        Assert.Contains("8192", row.ReasonText);
    }

    [Fact]
    public void Every_network_peer_is_synchronised_with_its_flags_once_per_change()
    {
        MarkersBridge.Start(_t);
        object kiln = new(), vanilla = new(), pending = new(), lab = new(), labVanilla = new();
        _t.Peers.Add(Peer(kiln, 0x81, NetPeerState.Ready, player: 11));
        _t.Peers.Add(Peer(vanilla, 0x82, NetPeerState.Vanilla, player: 12, channel: false));
        _t.Peers.Add(Peer(pending, 0x83, NetPeerState.Connecting));
        _t.Peers.Add(Peer(lab, 0x80000001, NetPeerState.Ready, lab: true));
        _t.Peers.Add(Peer(labVanilla, 0x80000002, NetPeerState.Connecting, channel: false, lab: true));
        MarkersBridge.ServerPump();
        Assert.Equal(5, FakeBridge.Ups.Count);
        Assert.Equal(1u, LastUp(0, 0x80000002).Flags);  // a lab vanilla peer stays Connecting: a player without Kiln
        _t.Peers.RemoveAt(4);
        MarkersBridge.ServerPump();
        FakeBridge.Ups.RemoveAll(u => u.Id == 0x80000002);
        var k = LastUp(0, 0x81);
        Assert.Equal(1u | 2u | 4u, k.Flags);
        Assert.Equal(11u, k.Entity);
        Assert.Equal(1u, k.Route);
        Assert.Equal(16384u, k.MaxMessageBytes);
        Assert.Equal(1u | 8u, LastUp(0, 0x82).Flags);  // vanilla: counted as refused, never sent to
        Assert.Equal(4u, LastUp(0, 0x83).Flags);        // the handshake is pending: no player yet
        Assert.Equal(1u | 2u | 4u, LastUp(0, 0x80000001).Flags);  // a lab peer stands for a player
        Assert.Equal(4, FakeBridge.Ups.Select(u => u.Generation).Distinct().Count());
        Assert.Contains(0u, FakeBridge.Pumps);
        // nothing changed: nothing registered again
        MarkersBridge.ServerPump();
        Assert.Equal(4, FakeBridge.Ups.Count);
        // the pending one gets its player: the same generation, new flags
        var gen = LastUp(0, 0x83).Generation;
        _t.Peers[2] = Peer(pending, 0x83, NetPeerState.Ready, player: 13);
        MarkersBridge.ServerPump();
        Assert.Equal(gen, LastUp(0, 0x83).Generation);
        Assert.Equal(1u | 2u | 4u, LastUp(0, 0x83).Flags);
        // a new connection object on the same id: the old generation goes down, a new one comes up
        var oldGen = LastUp(0, 0x81).Generation;
        object kiln2 = new();
        _t.Peers[0] = Peer(kiln2, 0x81, NetPeerState.Ready, player: 11);
        MarkersBridge.ServerPump();
        Assert.Contains((0u, 0x81u, oldGen), FakeBridge.Downs);
        Assert.NotEqual(oldGen, LastUp(0, 0x81).Generation);
        // a peer that left: down with its exact generation
        var vgen = LastUp(0, 0x82).Generation;
        _t.Peers.RemoveAt(1);
        MarkersBridge.ServerPump();
        Assert.Contains((0u, 0x82u, vgen), FakeBridge.Downs);
    }

    [Fact]
    public void Inbound_bytes_go_to_the_role_peer_and_generation_of_their_connection()
    {
        MarkersBridge.Start(_t);
        object p = new();
        _t.Peers.Add(Peer(p, 0x81, NetPeerState.Ready, player: 11));
        MarkersBridge.ServerPump();
        var gen = LastUp(0, 0x81).Generation;
        byte[] body = [1, 2, 3];
        _t.Handler!(p, true, 8, body);
        body[0] = 9;
        var got = Assert.Single(FakeBridge.Received);
        Assert.Equal((0u, 0x81u, gen, 8u), (got.Role, got.Peer, got.Gen, got.Message));
        Assert.Equal(new byte[] { 1, 2, 3 }, got.Bytes);
        _t.Handler!(new object(), true, 8, body);  // a connection the bridge does not know: dropped
        Assert.Single(FakeBridge.Received);
        // the client role: its server link
        _t.IsClient = true;
        object server = new();
        _t.Link = Peer(server, 0xFFFFFFFF, NetPeerState.Ready);
        _t.ClientTick!();
        var up = LastUp(1, 0xFFFFFFFF);
        Assert.Equal(2u | 4u, up.Flags);
        Assert.Equal(1u, up.Route);
        _t.Handler!(server, false, 10, body);
        Assert.Equal((1u, 0xFFFFFFFFu, up.Generation, 10u), (FakeBridge.Received[^1].Role, FakeBridge.Received[^1].Peer, FakeBridge.Received[^1].Gen, FakeBridge.Received[^1].Message));
    }

    [Fact]
    public void The_pumps_packets_reach_the_exact_connection()
    {
        MarkersBridge.Start(_t);
        object a = new(), b = new();
        _t.Peers.Add(Peer(a, 0x81, NetPeerState.Ready, player: 11));
        _t.Peers.Add(Peer(b, 0x82, NetPeerState.Ready, player: 12));
        MarkersBridge.ServerPump();
        var ga = LastUp(0, 0x81).Generation;
        FakeBridge.Outgoing.Add((0, 0x81, ga, 1, [7, 7]));
        FakeBridge.Outgoing.Add((0, 0x82, ga, 1, [8]));        // a generation of another peer: not sent
        FakeBridge.Outgoing.Add((0, 0x81, ga + 1000, 1, [9])); // an old generation: not sent
        MarkersBridge.ServerPump();
        var sent = Assert.Single(_t.Sent);
        Assert.Same(a, sent.Peer);
        Assert.Equal((ushort)1, sent.Message);
        Assert.Equal(new byte[] { 7, 7 }, sent.Body);
        Assert.Equal(new byte[] { 1, 0, 0 }, FakeBridge.PumpResults.Select(r => r.Result).ToArray());
        // the transport says "later": 0, the native pump keeps the packet
        _t.SendOk = false;
        FakeBridge.Outgoing.Add((0, 0x81, ga, 6, [1]));
        MarkersBridge.ServerPump();
        Assert.Equal(0, FakeBridge.PumpResults[^1].Result);
    }

    [Fact]
    public void A_hosting_client_is_one_loopback_endpoint_counted_once()
    {
        MarkersBridge.Start(_t);
        _t.IsClient = true;
        _t.IsHosting = true;
        _t.LocalPlayer = 100;
        FakeMarkers.ServerEpoch = 5;
        FakeMarkers.ClientEpoch = 6;
        object own = new(), self = new(), guest = new();
        _t.Peers.Add(Peer(own, 0x80, NetPeerState.Ready, player: 100, local: true));  // the host's own machine (slot 0)
        _t.Peers.Add(Peer(guest, 0x81, NetPeerState.Ready, player: 101));
        _t.Link = Peer(self, 0xFFFFFFFF, NetPeerState.Ready);  // the host's client to its own server: not a second route
        MarkersBridge.ServerPump();
        _t.ClientTick!();
        Assert.DoesNotContain(FakeBridge.Ups, u => u.Role == 0 && u.Id == 0x80);
        Assert.DoesNotContain(FakeBridge.Ups, u => u.Role == 1 && u.Route == 1);
        var s = LastUp(0, 0);
        var c = LastUp(1, 0xFFFFFFFF);
        Assert.Equal(2u, s.Route);
        Assert.Equal(2u, c.Route);
        Assert.Equal(s.Generation, c.Generation);  // one generation for the epoch pair
        Assert.Equal(100u, s.Entity);
        Assert.Equal(1u | 2u | 4u, s.Flags);
        // the self-loop's marker frames are ignored on both roles
        _t.Handler!(own, true, 0, new byte[] { 1 });
        _t.Handler!(self, false, 1, new byte[] { 1 });
        Assert.Empty(FakeBridge.Received);
        // the guest is a normal route-1 peer
        Assert.Equal(1u, LastUp(0, 0x81).Route);
        // a new client world: a new loopback generation on both roles, the old one goes down
        FakeMarkers.ClientEpoch = 7;
        MarkersBridge.ServerPump();
        _t.ClientTick!();
        Assert.Contains((0u, 0u, s.Generation), FakeBridge.Downs);
        Assert.Contains((1u, 0xFFFFFFFFu, s.Generation), FakeBridge.Downs);
        Assert.NotEqual(s.Generation, LastUp(0, 0).Generation);
        Assert.Equal(LastUp(0, 0).Generation, LastUp(1, 0xFFFFFFFF).Generation);
        // the local player not known yet: no endpoint
        _t.LocalPlayer = 0;
        MarkersBridge.ServerPump();
        _t.ClientTick!();
        Assert.Contains((0u, 0u, LastUp(0, 0).Generation), FakeBridge.Downs);
    }

    [Fact]
    public void A_joined_client_has_no_server_role()
    {
        MarkersBridge.Start(_t);
        _t.IsClient = true;
        _t.Peers.Add(Peer(new object(), 0x81, NetPeerState.Ready, player: 11));
        MarkersBridge.ServerPump();
        Assert.DoesNotContain(FakeBridge.Ups, u => u.Role == 0);
        Assert.DoesNotContain(0u, FakeBridge.Pumps);
    }
}
