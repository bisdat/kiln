// SPDX-License-Identifier: MIT
// S8's server routing policy, before any native delegate/channel is opened. Host-self is a cross-world queue, never
// a packet; negotiated guest part bits are refusals, while a vanilla guest must get RequiresKilnClient.
using Kiln.CameraTransport;
using Xunit;

namespace Kiln.CameraTests.Transport;

public sealed class BridgeRoutingTests
{
    [Fact]
    public void A_vanilla_guest_is_refused_RequiresKilnClient_not_a_missing_part()
    {
        var route = CameraBridge.DecideRoute(CameraWireAbi.PartLens, true, false, CameraBridgePeer.Vanilla, 0);
        Assert.Equal((CameraBridgeRoute.Refused, CameraReasons.RequiresKilnClient), (route.Route, route.Reason));
    }

    [Fact]
    public void A_disconnected_player_is_refused_before_any_route()
    {
        foreach (var self in new[] { false, true })
        {
            var route = CameraBridge.DecideRoute(CameraWireAbi.PartLens, false, self, CameraBridgePeer.Ready, CameraWireAbi.RemoteParts);
            Assert.Equal((CameraBridgeRoute.Refused, CameraReasons.PeerUnavailable), (route.Route, route.Reason));
        }
    }

    [Fact]
    public void The_host_self_route_uses_local_part_checks_not_the_remote_row_or_peer_handshake()
    {
        var parts = CameraWireAbi.PartProfile | CameraWireAbi.PartLens;
        var route = CameraBridge.DecideRoute(parts, true, true, CameraBridgePeer.Unavailable, 0);
        Assert.Equal((CameraBridgeRoute.HostSelf, CameraReasons.None), (route.Route, route.Reason));
        Assert.Equal(CameraWireAbi.CapLocalProfile | CameraWireAbi.CapLocalFov, route.RequiredParts);
        Assert.Equal(0ul, route.RequiredParts & (ulong)Kiln.CameraCapability.Remote);
    }

    [Theory]
    [InlineData(CameraWireAbi.PartPath, CameraWireAbi.CapLocalPose)]
    [InlineData(CameraWireAbi.PartProfile, CameraWireAbi.CapLocalProfile)]
    [InlineData(CameraWireAbi.PartLens, CameraWireAbi.CapLocalFov)]
    [InlineData(CameraWireAbi.PartShake, CameraWireAbi.CapLocalShake)]
    public void A_ready_guest_needs_each_requested_part(uint parts, ulong capability)
    {
        var missing = CameraBridge.DecideRoute(parts, true, false, CameraBridgePeer.Ready, CameraWireAbi.RemoteParts & ~capability);
        Assert.Equal((CameraBridgeRoute.Refused, CameraReasons.ClientCapability), (missing.Route, missing.Reason));
        var ready = CameraBridge.DecideRoute(parts, true, false, CameraBridgePeer.Ready, capability);
        Assert.Equal((CameraBridgeRoute.Remote, CameraReasons.None, capability), (ready.Route, ready.Reason, ready.RequiredParts));
    }

    [Fact]
    public void Combinations_require_all_parts_and_a_lens_alone_needs_no_pose()
    {
        var combined = CameraBridge.DecideRoute(CameraWireAbi.PartPath | CameraWireAbi.PartLens, true, false,
            CameraBridgePeer.Ready, CameraWireAbi.CapLocalFov);
        Assert.Equal(CameraReasons.ClientCapability, combined.Reason);
        var lens = CameraBridge.DecideRoute(CameraWireAbi.PartLens, true, false, CameraBridgePeer.Ready, CameraWireAbi.CapLocalFov);
        Assert.Equal(CameraBridgeRoute.Remote, lens.Route);
    }

    [Fact]
    public void Negotiating_peers_wait_for_the_binding_deadline_rather_than_being_mislabelled_vanilla()
    {
        var route = CameraBridge.DecideRoute(CameraWireAbi.PartLens, true, false, CameraBridgePeer.Negotiating, 0);
        Assert.Equal((CameraBridgeRoute.AwaitNegotiation, CameraReasons.None), (route.Route, route.Reason));
        var gone = CameraBridge.DecideRoute(CameraWireAbi.PartLens, true, false, CameraBridgePeer.Unavailable, 0);
        Assert.Equal((CameraBridgeRoute.Refused, CameraReasons.PeerUnavailable), (gone.Route, gone.Reason));
    }

    [Theory]
    [InlineData(0u)]
    [InlineData(CameraWireAbi.PartMode)]
    [InlineData(CameraWireAbi.PartMode | CameraWireAbi.PartLens)]
    [InlineData(CameraWireAbi.PartPath | CameraWireAbi.PartProfile)]
    [InlineData(0x80000000u)]
    public void The_bridge_accepts_only_validated_visual_parts(uint parts) =>
        Assert.Throws<ArgumentException>(() => CameraBridge.DecideRoute(parts, true, true, CameraBridgePeer.Ready, ulong.MaxValue));
}
