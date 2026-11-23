// SPDX-License-Identifier: MIT
// S8: the camera transport bridge (CameraBridgeHost) with no game: a dedicated server's bridge and a guest client's bridge
// joined by a fake network, or one hosting client's bridge; native kiln.camera's LOCAL endpoint is a fake that answers
// when the test says. What a server request becomes on each route, the refusals and their reasons, the answers' scope
// and frame, the client's own end, peers that leave, and that the channel is Kiln's whatever mod's code runs.
using Kiln.CameraTransport;
using Xunit;

namespace Kiln.CameraTests.Transport;

internal sealed class FakeNet : ICameraNet
{
    public bool Available { get; set; } = true;
    public readonly List<CameraPeerView> Peers = [];
    public CameraPeerView? Link;
    public readonly List<(object Peer, CameraMessage Id, byte[] Body)> Sent = [];
    public Action<object, ushort, byte[]>? Deliver;  // where a sent message arrives
    public int Opened;
    public ModContext? OpenedUnder = HostAccess.CreateContext(new ModInfo("sentinel", "sentinel", "1.0.0", "none"), 1, "none");

    public int Open(CameraRawHandler onRaw)
    {
        Opened++;
        OpenedUnder = ModContext.CurrentMod;
        return CameraWireAbi.MaxBody;
    }

    public IReadOnlyList<CameraPeerView> ServerPeers() => Peers.ToList();
    public CameraPeerView? ServerLink() => Link;

    public bool Send(object peer, ushort message, ReadOnlySpan<byte> body)
    {
        var bytes = body.ToArray();
        Sent.Add((peer, (CameraMessage)message, bytes));
        Deliver?.Invoke(peer, message, bytes);
        return true;
    }

    public bool IsClient { get; set; }
    public bool IsHosting { get; set; }
    public uint LocalPlayer { get; set; }
    public uint ServerWorld { get; set; }
    public uint ClientWorld { get; set; }
}

internal sealed class FakeLocal : ICameraLocalNative
{
    public ulong Caps = CameraWireAbi.RemoteParts;
    public int Rc;
    public readonly List<(string Op, ulong Owner, ulong Lease, CameraWireRequest? Request, nint Key)> Calls = [];
    public readonly List<ulong> Owners = [];
    private ulong _lease = 100, _owner = 1000;

    public ulong LocalCaps() => Caps;

    public ulong OwnerFor(ulong ownerToken)
    {
        Owners.Add(ownerToken);
        return ++_owner;
    }

    public int Apply(ulong owner, CameraWireRequest request, nint key, out ulong lease)
    {
        lease = Rc == 0 ? ++_lease : 0;
        Calls.Add(("apply", owner, lease, request, key));
        return Rc;
    }

    public int Update(ulong owner, ulong lease, byte timing, CameraWireRequest request, nint key)
    {
        Calls.Add((timing == CameraWireAbi.TimingRestart ? "update-restart" : "update", owner, lease, request, key));
        return Rc;
    }

    public int End(ulong owner, ulong lease, nint key)
    {
        Calls.Add(("end", owner, lease, null, key));
        return Rc;
    }
}

public sealed class BridgeHostTests
{
    private const uint Guest = 5, Host = 9;
    private long _now = 1000;
    private ulong _random = 0x1000;
    private readonly List<TransportResult> _results = [];
    private readonly List<RemoteEnded> _ended = [];

    private CameraBridgeHost NewHost(FakeNet net, FakeLocal local) => new(net, local, () => _now, () => ++_random, CameraWireAbi.MaxBody, _ended.Add);

    private static CameraWireRequest Lens(uint ms = 5000, int priority = 0) => new()
    {
        Parts = CameraWireAbi.PartLens, InputPolicy = CameraWireAbi.VisualOnly, DurationMs = ms, Priority = priority,
        LensSource = CameraWireAbi.LensConstant, FovDegrees = 70,
    };

    // A dedicated server's bridge and a guest's bridge on one fake link.
    private sealed class Pair
    {
        public readonly FakeNet ServerNet = new() { ServerWorld = 1 };
        public readonly FakeNet ClientNet = new() { IsClient = true, ClientWorld = 3, LocalPlayer = Guest };
        public readonly FakeLocal ServerLocal = new(), ClientLocal = new();
        public readonly object PeerKey = new(), LinkKey = new();
        public CameraBridgeHost Server = null!, Client = null!;
        public bool UpLost;  // the client's messages to the server are lost (a hitch, a loading screen)

        public void Connect(NetPeerState state = NetPeerState.Ready, bool channel = true)
        {
            ServerNet.Peers.Clear();
            ServerNet.Peers.Add(new CameraPeerView(PeerKey, 0x101, state, Guest, channel, false));
            ClientNet.Link = new CameraPeerView(LinkKey, uint.MaxValue, state, 0, channel, false);
            ServerNet.Deliver = (_, id, body) => Client.OnRaw(LinkKey, false, id, body);
            ClientNet.Deliver = (_, id, body) =>
            {
                if (!UpLost) Server.OnRaw(PeerKey, true, id, body);
            };
        }

        public void Pump(int n = 1)
        {
            for (var i = 0; i < n; i++)
            {
                Server.ServerPump();
                Client.ClientPump();
            }
        }
    }

    private Pair NewPair(NetPeerState state = NetPeerState.Ready, bool channel = true, ulong clientCaps = CameraWireAbi.RemoteParts)
    {
        var p = new Pair();
        p.ClientLocal.Caps = clientCaps;
        p.Server = NewHost(p.ServerNet, p.ServerLocal);
        p.Client = NewHost(p.ClientNet, p.ClientLocal);
        p.Connect(state, channel);
        p.Pump(2);  // the peer is seen, the client says Hello, the server welcomes it
        return p;
    }

    // completes every native LOCAL call the client's fake has not answered yet
    private static void Answer(CameraBridgeHost host, FakeLocal local, bool ok = true, ushort reason = 0, ulong frame = 9, int from = 0)
    {
        foreach (var c in local.Calls.Skip(from)) host.LocalDone(c.Key, new NativeOutcome(ok, reason, ok ? CameraWireAbi.CommitClientFrame : CameraWireAbi.CommitNone, 0, frame));
    }

    [Fact]
    public void A_vanilla_guest_is_refused_RequiresKilnClient_at_the_next_pump_never_inside_submit()
    {
        var p = NewPair(NetPeerState.Vanilla, channel: false);
        p.Server.Submit(11, TransportOp.Apply, 7, 1, Guest, Lens(), _results.Add);
        Assert.Empty(_results);
        p.Pump();
        var r = Assert.Single(_results);
        Assert.Equal((false, CameraReasons.RequiresKilnClient, 7ul, 1ul), (r.Ok, r.Reason, r.Lease, r.Revision));
        Assert.Contains("does not run Kiln", r.Diagnostic);
        Assert.DoesNotContain(p.ServerNet.Sent, s => s.Id is CameraMessage.Apply);
    }

    [Fact]
    public void A_Kiln_guest_without_the_camera_channel_is_RequiresKilnClient_too()
    {
        var p = NewPair(NetPeerState.Ready, channel: false);
        p.Server.Submit(11, TransportOp.Apply, 7, 1, Guest, Lens(), _results.Add);
        p.Pump();
        var r = Assert.Single(_results);
        Assert.Equal((false, CameraReasons.RequiresKilnClient), (r.Ok, r.Reason));
        Assert.Contains("without kiln.camera", r.Diagnostic);
    }

    [Fact]
    public void A_guest_whose_client_lacks_the_part_is_refused_ClientCapability()
    {
        var p = NewPair(clientCaps: CameraWireAbi.CapLocalPose | CameraWireAbi.CapLocalProfile);  // no camera.local_fov
        Assert.Equal(ServerBindingState.Bound, p.Server.BindingOf(Guest)!.State);
        Assert.True(p.Server.Caps(Guest, out var caps));
        Assert.Equal(CameraWireAbi.CapLocalPose | CameraWireAbi.CapLocalProfile, caps);
        p.Server.Submit(11, TransportOp.Apply, 7, 1, Guest, Lens(), _results.Add);
        p.Pump();
        var r = Assert.Single(_results);
        Assert.Equal((false, CameraReasons.ClientCapability), (r.Ok, r.Reason));
        Assert.Empty(p.ClientLocal.Calls);
    }

    [Fact]
    public void A_player_with_no_connection_here_is_PeerUnavailable()
    {
        var p = NewPair();
        Assert.False(p.Server.Caps(77, out _));
        p.Server.Submit(11, TransportOp.Apply, 7, 1, 77, Lens(), _results.Add);
        p.Pump();
        Assert.Equal((false, CameraReasons.PeerUnavailable), (_results[0].Ok, _results[0].Reason));
        // an End or an Update of a lease with no route: nothing left to end / the connection is gone
        p.Server.Submit(11, TransportOp.End, 8, 1, 0, null, _results.Add);
        p.Server.Submit(11, TransportOp.UpdatePreserve, 8, 2, 0, Lens(), _results.Add);
        p.Pump();
        Assert.Equal((true, CameraWireAbi.FlagAlreadyEnded), (_results[1].Ok, _results[1].Flags));
        Assert.Equal((false, CameraReasons.PeerUnavailable), (_results[2].Ok, _results[2].Reason));
    }

    [Fact]
    public void A_guests_apply_is_Ok_only_on_its_clients_frame_and_end_releases_it()
    {
        var p = NewPair();
        p.Server.Submit(11, TransportOp.Apply, 7, 1, Guest, Lens(), _results.Add);
        p.Pump();
        var call = Assert.Single(p.ClientLocal.Calls);
        Assert.Equal("apply", call.Op);
        Assert.True(call.Request!.SameAs(Lens()));  // the request crossed the wire unchanged
        Assert.Empty(_results);
        Answer(p.Client, p.ClientLocal, frame: 42);
        p.Pump(2);
        var r = Assert.Single(_results);
        Assert.Equal((true, CameraWireAbi.CommitClientFrame, 42ul, 7ul, 1ul), (r.Ok, r.Scope, r.Frame, r.Lease, r.Revision));
        Assert.Equal(7ul, p.Client.ClientBinding!.ActiveLease);
        // update with a restarted timeline, then end
        p.Server.Submit(11, TransportOp.UpdateRestart, 7, 2, Guest, Lens(9000), _results.Add);
        p.Pump();
        Assert.Equal("update-restart", p.ClientLocal.Calls[^1].Op);
        Assert.Equal(call.Lease, p.ClientLocal.Calls[^1].Lease);
        Answer(p.Client, p.ClientLocal, frame: 43, from: 1);
        p.Pump(2);
        Assert.Equal((true, 2ul, 43ul), (_results[1].Ok, _results[1].Revision, _results[1].Frame));
        p.Server.Submit(11, TransportOp.End, 7, 2, 0, null, _results.Add);
        p.Pump();
        Assert.Equal(("end", call.Lease), (p.ClientLocal.Calls[^1].Op, p.ClientLocal.Calls[^1].Lease));
        Answer(p.Client, p.ClientLocal, frame: 44, from: 2);
        p.Client.LocalChanged(call.Lease, 3, CameraEndReasons.Released, 0, 44);  // its own ENDED: not the client's own end
        p.Pump(2);
        Assert.True(_results[2].Ok);
        Assert.Empty(_ended);
        Assert.Equal(0ul, p.Client.ClientBinding!.ActiveLease);
    }

    [Fact]
    public void The_client_ending_a_lease_by_itself_reaches_the_server_as_transport_ended()
    {
        var p = NewPair();
        p.Server.Submit(11, TransportOp.Apply, 7, 1, Guest, Lens(), _results.Add);
        p.Pump();
        Answer(p.Client, p.ClientLocal);
        p.Pump(2);
        var local = p.ClientLocal.Calls[0].Lease;
        p.Client.LocalChanged(local, 3, CameraEndReasons.TargetLost, CameraReasons.TargetUnavailable, 50);
        p.Pump(2);
        var e = Assert.Single(_ended);
        Assert.Equal((7ul, 1ul, CameraEndReasons.TargetLost, CameraReasons.TargetUnavailable), (e.Lease, e.Revision, e.EndReason, e.Reason));
    }

    [Fact]
    public void Two_server_authors_get_two_client_owners_and_a_token_flood_is_capped()
    {
        var p = NewPair();
        p.Server.Submit(11, TransportOp.Apply, 7, 1, Guest, Lens(priority: 0), _results.Add);
        p.Pump();
        Answer(p.Client, p.ClientLocal);
        p.Pump(2);
        p.Server.Submit(22, TransportOp.Apply, 8, 1, Guest, Lens(priority: 100), _results.Add);
        p.Pump();
        Assert.Equal(2, p.ClientLocal.Owners.Distinct().Count());
        Assert.NotEqual(p.ClientLocal.Calls[0].Owner, p.ClientLocal.Calls[1].Owner);
        // the endpoint alone: a client never makes more than 32 native owners, whatever tokens a server sends
        var local = new FakeLocal();
        var endpoint = new LocalEndpoint(local);
        var refused = 0;
        for (ulong t = 1; t <= 40; t++)
            if (endpoint.Start(new RemoteRequest(CameraMessage.Apply, t, 1000 + t, 1, t, 0, Lens())) == CameraReasons.ResourceBusy) refused++;
        Assert.Equal(LocalEndpoint.MaxOwners, local.Owners.Count);
        Assert.Equal(8, refused);
    }

    [Fact]
    public void The_hosts_own_player_goes_through_a_local_queue_with_no_packet()
    {
        var net = new FakeNet { IsClient = true, IsHosting = true, LocalPlayer = Host, ServerWorld = 2, ClientWorld = 2 };
        var local = new FakeLocal { Caps = CameraWireAbi.CapLocalFov };
        var host = NewHost(net, local);
        host.ServerPump();
        host.ClientPump();
        Assert.True(host.Caps(Host, out var caps));
        Assert.Equal(CameraWireAbi.CapLocalFov, caps);  // the host's own local rows (G5)
        host.Submit(11, TransportOp.Apply, 7, 1, Host, Lens(), _results.Add);
        Assert.Empty(local.Calls);  // the client world's step takes it, not the server's
        host.ServerPump();
        host.ClientPump();
        Assert.Equal("apply", Assert.Single(local.Calls).Op);
        Answer(host, local, frame: 12);
        host.ClientPump();
        Assert.Empty(_results);
        host.ServerPump();
        var r = Assert.Single(_results);
        Assert.Equal((true, CameraWireAbi.CommitClientFrame, 12ul), (r.Ok, r.Scope, r.Frame));
        Assert.Empty(net.Sent);  // no packet, no handshake
        // the host's local row is off for a part: the local endpoint refuses it ClientCapability
        local.Rc = 3;  // KILN_ERR_UNAVAILABLE
        host.Submit(11, TransportOp.Apply, 8, 1, Host, Lens(), _results.Add);
        host.ServerPump();
        host.ClientPump();
        host.ServerPump();
        Assert.Equal((false, CameraReasons.ClientCapability), (_results[1].Ok, _results[1].Reason));
        // the host's client ends its lease by itself: transport_ended
        local.Rc = 0;
        host.LocalChanged(local.Calls[0].Lease, 3, CameraEndReasons.Expired, 0, 20);
        host.ClientPump();
        host.ServerPump();
        Assert.Equal((7ul, CameraEndReasons.Expired), (Assert.Single(_ended).Lease, _ended[0].EndReason));
        Assert.Empty(net.Sent);
    }

    [Fact]
    public void A_host_route_with_no_client_answer_times_out_MayHaveApplied()
    {
        var net = new FakeNet { IsClient = true, IsHosting = true, LocalPlayer = Host, ServerWorld = 2, ClientWorld = 2 };
        var host = NewHost(net, new FakeLocal());
        host.ServerPump();
        host.Submit(11, TransportOp.Apply, 7, 1, Host, Lens(), _results.Add);
        _now += HostSelfRoute.AnswerDeadlineMs;
        host.ServerPump();
        var r = Assert.Single(_results);
        Assert.Equal((false, CameraReasons.TransportTimeout, true), (r.Ok, r.Reason, r.MayHaveApplied));
    }

    [Fact]
    public void A_peer_whose_handshake_never_finishes_is_refused_Negotiation_after_two_seconds()
    {
        var p = NewPair(NetPeerState.Connecting);
        p.Server.Submit(11, TransportOp.Apply, 7, 1, Guest, Lens(), _results.Add);
        p.Pump();
        Assert.Empty(_results);  // it waits: a negotiating peer may still get ready
        _now += ServerBinding.NegotiationTimeoutMs;
        p.Pump();
        var r = Assert.Single(_results);
        Assert.Equal((false, CameraReasons.Negotiation), (r.Ok, r.Reason));
        Assert.DoesNotContain(p.ServerNet.Sent, s => s.Id is CameraMessage.Apply);
    }

    [Fact]
    public void A_guest_that_leaves_ends_its_leases_PeerDisconnected_and_a_new_world_starts_clean()
    {
        var p = NewPair();
        p.Server.Submit(11, TransportOp.Apply, 7, 1, Guest, Lens(), _results.Add);
        p.Pump();
        Answer(p.Client, p.ClientLocal);
        p.Pump(2);
        p.ServerNet.Peers.Clear();
        p.Pump();
        Assert.Equal((7ul, CameraEndReasons.PeerDisconnected), (Assert.Single(_ended).Lease, _ended[0].EndReason));
        Assert.Null(p.Server.BindingOf(Guest));
        // the server world stops: whatever waited is refused and the next world binds anew
        p.Connect();
        p.Pump(2);
        p.Server.Submit(11, TransportOp.Apply, 8, 1, Guest, Lens(), _results.Add);
        p.ServerNet.ServerWorld = 0;
        p.Pump();
        Assert.Equal((false, CameraReasons.PeerUnavailable), (_results[^1].Ok, _results[^1].Reason));
        Assert.Null(p.Server.BindingOf(Guest));
    }

    // review S8 M1 (its F1, inverted): an Update whose answer never comes times out TransportTimeout with MayHaveApplied;
    // the client is told to let go, so kiln.camera must hear that the active lease ended (TransportTimeout), else it holds
    // the player's slot forever, also after the player left.
    [Fact]
    public void A_guests_update_that_times_out_ends_its_lease_TransportTimeout()
    {
        var p = NewPair();
        p.Server.Submit(11, TransportOp.Apply, 7, 1, Guest, Lens(60000), _results.Add);
        p.Pump();
        Answer(p.Client, p.ClientLocal);
        p.Pump(2);
        Assert.True(_results[0].Ok);
        p.UpLost = true;
        p.Server.Submit(11, TransportOp.UpdateRestart, 7, 2, Guest, Lens(60000), _results.Add);
        p.Pump();
        Assert.Equal("update-restart", p.ClientLocal.Calls[^1].Op);
        Answer(p.Client, p.ClientLocal, from: 1);
        for (var t = 0; t < 8; t++)
        {
            _now += 500;
            p.Pump();
        }
        var timeout = _results[^1];
        Assert.Equal((false, CameraReasons.TransportTimeout, true, 2ul), (timeout.Ok, timeout.Reason, timeout.MayHaveApplied, timeout.Revision));
        Assert.Contains(p.ServerNet.Sent, s => s.Id == CameraMessage.End);  // the client is told to let go
        var e = Assert.Single(_ended);                                      // and kiln.camera hears the lease ended
        Assert.Equal((7ul, 1ul, CameraEndReasons.TransportTimeout, CameraReasons.TransportTimeout), (e.Lease, e.Revision, e.EndReason, e.Reason));
        p.UpLost = false;
        for (var t = 0; t < 4; t++)
        {
            _now += 1000;
            p.Pump();
        }
        Assert.Equal(0ul, p.Client.ClientBinding!.ActiveLease);
        p.ServerNet.Peers.Clear();  // the player leaves: nothing more to end, no second Ended
        p.Pump(2);
        Assert.Single(_ended);
    }

    // review S8 M1 (its F1b, inverted): the same on the host's own route (its 3 s answer deadline).
    [Fact]
    public void A_host_route_update_that_times_out_ends_its_lease_TransportTimeout()
    {
        var net = new FakeNet { IsClient = true, IsHosting = true, LocalPlayer = Host, ServerWorld = 2, ClientWorld = 2 };
        var local = new FakeLocal();
        var host = NewHost(net, local);
        host.ServerPump();
        host.ClientPump();
        host.Submit(11, TransportOp.Apply, 7, 1, Host, Lens(60000), _results.Add);
        host.ServerPump();
        host.ClientPump();
        Answer(host, local, frame: 5);
        host.ClientPump();
        host.ServerPump();
        Assert.True(_results[0].Ok);
        host.Submit(11, TransportOp.UpdatePreserve, 7, 2, Host, Lens(60000), _results.Add);
        host.ServerPump();  // the client world does not step (a hitch, a loading screen)
        _now += HostSelfRoute.AnswerDeadlineMs;
        host.ServerPump();
        Assert.Equal((false, CameraReasons.TransportTimeout, true), (_results[^1].Ok, _results[^1].Reason, _results[^1].MayHaveApplied));
        var e = Assert.Single(_ended);
        Assert.Equal((7ul, 1ul, CameraEndReasons.TransportTimeout, CameraReasons.TransportTimeout), (e.Lease, e.Revision, e.EndReason, e.Reason));
        host.ClientPump();
        Assert.Contains(local.Calls, c => c.Op == "end");  // the host's client let go of it
        host.ServerPump();
        Assert.Single(_ended);
    }

    // review S8 N1 (its F4, inverted): a refused apply never becomes a lease and a lease the client ended is gone: neither
    // keeps a route, so a mod retrying against a refusing client does not grow the bridge's table.
    [Fact]
    public void Refused_applies_and_ended_leases_keep_no_route()
    {
        var p = NewPair();
        p.Server.Submit(11, TransportOp.Apply, 100, 1, Guest, Lens(60000), _results.Add);  // in flight
        p.Pump();
        for (ulong l = 101; l <= 150; l++) p.Server.Submit(11, TransportOp.Apply, l, 1, Guest, Lens(60000), _results.Add);
        p.Pump();
        Assert.Equal(50, _results.Count(x => x.Reason == CameraReasons.Busy));
        Assert.Equal(1, p.Server.RouteCount);
        Answer(p.Client, p.ClientLocal);
        p.Pump(2);
        Assert.True(_results[^1].Ok);
        var local = p.ClientLocal.Calls[0].Lease;
        p.Client.LocalChanged(local, 3, CameraEndReasons.Expired, 0, 0);  // the client ends it by itself
        p.Pump(2);
        Assert.Single(_ended);
        Assert.Equal(0, p.Server.RouteCount);
    }

    [Fact]
    public void The_channel_and_the_bridge_are_Kilns_whatever_mods_code_starts_them()
    {
        CameraBridgeStartup.Reset();
        try
        {
            var net = new FakeNet { ServerWorld = 1 };
            var mod = HostAccess.CreateContext(new ModInfo("first.caller", "first.caller", "1.0.0", "none"), 77, "none");
            var outer = HostAccess.Current;
            HostAccess.Current = mod;
            ulong bound = 0;
            try
            {
                CameraBridgeStartup.Start(net, new FakeLocal(), owner =>
                {
                    bound = owner;
                    Assert.Null(ModContext.CurrentMod);  // binding runs as Kiln too
                    return 0;
                });
                Assert.Same(mod, ModContext.CurrentMod);  // the caller's context is back afterwards
            }
            finally
            {
                HostAccess.Current = outer;
            }
            Assert.Equal(1, net.Opened);
            Assert.Null(net.OpenedUnder);  // not the first calling mod's channel: no mod owns it, its unload cannot close it
            // the bridge's native owner is Kiln's own (kiln.camera.transport), never the mod's 77; a null backend has none
            Assert.NotEqual(77ul, bound);
            Assert.True(CameraBridgeStartup.State is "bound" or "no owner", CameraBridgeStartup.State);
            // a second start (another mod's first call) changes nothing
            CameraBridgeStartup.Start(net, new FakeLocal(), _ => 0);
            Assert.Equal(1, net.Opened);
        }
        finally
        {
            CameraBridgeStartup.Reset();
        }
    }
}
