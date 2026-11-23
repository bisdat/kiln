// SPDX-License-Identifier: MIT
// S8, first slice: the server's routing/admission decision, kept pure so it is tested without a game or a channel.
// The native bridge binding, framework startup and the two owning-world queues are the next slice. This policy is
// not wired to the service yet: camera.remote must remain unavailable until that persistent bridge is bound.
namespace Kiln.CameraTransport;

internal enum CameraBridgePeer
{
    Unavailable,
    Vanilla,
    Negotiating,
    Ready,
}

internal enum CameraBridgeRoute
{
    Refused,
    HostSelf,
    AwaitNegotiation,
    Remote,
}

internal readonly record struct CameraBridgeDecision(CameraBridgeRoute Route, ushort Reason, ulong RequiredParts);

internal static partial class CameraBridge
{
    /// <summary>Chooses the physical route after native request validation. The persistent bridge must already exist;
    /// its absence is a local camera.remote capability error, not a game refusal. Player/host/peer identities come
    /// from framework snapshots, never from an author's claimed role or a packet's player field.</summary>
    /// <remarks>A HostSelf decision posts to CLIENT Pre and checks the actual local part rows there; it does not
    /// require camera.remote or a network handshake. Guest negotiation waits in ServerBinding for its 2 s deadline.
    /// A ready guest's missing part is ClientCapability, whereas a known vanilla guest is RequiresKilnClient even
    /// when its part mask is empty. No path silently substitutes a stock Mode for requested visuals.</remarks>
    internal static CameraBridgeDecision DecideRoute(uint parts, bool playerConnected, bool hostSelf, CameraBridgePeer peer, ulong recipientCaps)
    {
        const uint visual = CameraWireAbi.PartPath | CameraWireAbi.PartProfile | CameraWireAbi.PartLens | CameraWireAbi.PartShake;
        if (parts == 0 || (parts & ~visual) != 0 || (parts & (CameraWireAbi.PartPath | CameraWireAbi.PartProfile)) == (CameraWireAbi.PartPath | CameraWireAbi.PartProfile))
            throw new ArgumentException("the camera bridge takes a validated visual request, not Mode or unknown parts", nameof(parts));
        if (!Enum.IsDefined(peer)) throw new ArgumentOutOfRangeException(nameof(peer));
        ulong required = 0;
        if ((parts & CameraWireAbi.PartPath) != 0) required |= CameraWireAbi.CapLocalPose;
        if ((parts & CameraWireAbi.PartProfile) != 0) required |= CameraWireAbi.CapLocalProfile;
        if ((parts & CameraWireAbi.PartLens) != 0) required |= CameraWireAbi.CapLocalFov;
        if ((parts & CameraWireAbi.PartShake) != 0) required |= CameraWireAbi.CapLocalShake;

        CameraBridgeDecision Refused(ushort reason) => new(CameraBridgeRoute.Refused, reason, required);
        if (!playerConnected) return Refused(CameraReasons.PeerUnavailable);
        if (hostSelf) return new(CameraBridgeRoute.HostSelf, CameraReasons.None, required);
        return peer switch
        {
            CameraBridgePeer.Vanilla => Refused(CameraReasons.RequiresKilnClient),
            CameraBridgePeer.Negotiating => new(CameraBridgeRoute.AwaitNegotiation, CameraReasons.None, required),
            CameraBridgePeer.Ready when (recipientCaps & required) != required => Refused(CameraReasons.ClientCapability),
            CameraBridgePeer.Ready => new(CameraBridgeRoute.Remote, CameraReasons.None, required),
            _ => Refused(CameraReasons.PeerUnavailable),
        };
    }
}
