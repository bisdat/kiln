// SPDX-License-Identifier: MIT
// A server binding and a client binding joined by a fake link (two queues, each with a drop rule) on a fake millisecond
// clock, with a fake native endpoint behind the client. No real time, no network, no statics.
using Kiln.CameraTransport;

namespace Kiln.CameraTests.Transport;

internal sealed class FakeEndpoint : IRemoteCameraEndpoint
{
    public readonly List<RemoteRequest> Started = [];
    public readonly List<ulong> Cancelled = [];
    public readonly List<(ulong Lease, byte Reason)> Released = [];
    /// <summary>Completes every started request at the next pump (Ok, ClientFrame).</summary>
    public bool Auto = true;
    /// <summary>A refusal Start answers with (None: accept).</summary>
    public Func<RemoteRequest, ushort> Refuse = _ => CameraReasons.None;
    public readonly Queue<ulong> ToComplete = new();

    public ushort Start(in RemoteRequest request)
    {
        var refused = Refuse(request);
        if (refused != CameraReasons.None) return refused;
        Started.Add(request);
        if (Auto) ToComplete.Enqueue(request.Serial);
        return CameraReasons.None;
    }

    public void Cancel(ulong serial) => Cancelled.Add(serial);

    public void Release(ulong lease, byte endReason) => Released.Add((lease, endReason));
}

internal sealed class Rig
{
    public long Now;
    public readonly Queue<(CameraMessage Id, byte[] Body)> ToClient = new(), ToServer = new();
    public readonly List<(CameraMessage Id, byte[] Body)> SentToClient = [], SentToServer = [];
    public Func<CameraMessage, byte[], bool> DropToClient = (_, _) => false;
    public Func<CameraMessage, byte[], bool> DropToServer = (_, _) => false;
    /// <summary>Delivers every body as a joined peer receives it over kiln.net's game transport (<see cref="GamePad"/>).</summary>
    public bool PadLikeGameTransport;
    public readonly FakeEndpoint Endpoint = new();
    public readonly List<TransportResult> Results = [];
    public readonly List<RemoteEnded> Ended = [];
    public ServerBinding Server;
    public ClientBinding Client;
    private ulong _tokens = 0x7000;

    public Rig(ulong clientNonce = 0xC11E47, uint clientEpoch = 3, int clientMaxBody = CameraWireAbi.MaxBody,
        ulong clientCaps = CameraWireAbi.RemoteParts, int serverMaxBody = CameraWireAbi.MaxBody)
    {
        Server = NewServer(serverMaxBody);
        Client = NewClient(clientNonce, clientEpoch, clientMaxBody, clientCaps);
    }

    public ServerBinding NewServer(int maxBody = CameraWireAbi.MaxBody, ulong incarnation = 77) => new(new ServerBindingOptions
    {
        Clock = () => Now,
        Send = (id, body) => { SentToClient.Add((id, body)); ToClient.Enqueue((id, body)); },
        Random = () => ++_tokens,
        ServerNonce = 0x5E4E,
        ServerEpoch = 9,
        Player = 42,
        Incarnation = incarnation,
        ChannelMaxBody = maxBody,
        OnEnded = Ended.Add,
    });

    public ClientBinding NewClient(ulong nonce, uint epoch, int maxBody = CameraWireAbi.MaxBody, ulong caps = CameraWireAbi.RemoteParts) => new(new ClientBindingOptions
    {
        Clock = () => Now,
        Send = (id, body) => { SentToServer.Add((id, body)); ToServer.Enqueue((id, body)); },
        Endpoint = Endpoint,
        ClientNonce = nonce,
        ClientEpoch = epoch,
        LocalEntity = 42,
        Caps = caps,
        ChannelMaxBody = maxBody,
    });

    /// <summary>Delivers everything queued both ways (the drop rules apply), and the endpoint's completions.</summary>
    public void Pump()
    {
        for (var guard = 0; guard < 10000; guard++)
        {
            if (ToServer.TryDequeue(out var up))
            {
                if (!DropToServer(up.Id, up.Body)) Server.Receive(up.Id, PadLikeGameTransport ? GamePad(up.Body) : up.Body);
                continue;
            }
            if (ToClient.TryDequeue(out var down))
            {
                if (!DropToClient(down.Id, down.Body)) Client.Receive(down.Id, PadLikeGameTransport ? GamePad(down.Body) : down.Body);
                continue;
            }
            if (Endpoint.ToComplete.TryDequeue(out var serial))
            {
                Client.Complete(serial, new NativeOutcome(true, CameraReasons.None, CameraWireAbi.CommitClientFrame, 0, 500 + serial));
                continue;
            }
            return;
        }
        throw new InvalidOperationException("the link did not settle");
    }

    /// <summary>One step: both ticks, then the link.</summary>
    public void Tick()
    {
        Server.Tick();
        Client.Tick();
        Pump();
        Server.Tick();  // hand over what the link brought in this step
    }

    /// <summary>Advances the clock by <paramref name="ms"/> in steps of <paramref name="stride"/> ms.</summary>
    public void Advance(long ms, long stride = 50)
    {
        var end = Now + ms;
        while (Now < end)
        {
            Now = Math.Min(end, Now + stride);
            Tick();
        }
    }

    public void Bind()
    {
        Server.PeerReady();
        Client.Start();
        Pump();
        Tick();
    }

    public void Submit(TransportOp op, ulong lease, ulong revision = 1, CameraWireRequest? request = null, ulong owner = 5) =>
        Server.Submit(op, owner, lease, revision, op == TransportOp.End ? null : request ?? Requests.Lens(), Results.Add);

    /// <summary>A body as it arrives over kiln.net's game transport: the frame is padded with zeros to a multiple of 4 bytes
    /// and Kiln.Network's Data header is 8 bytes, so the body gets up to 3 zero bytes; one over 7992 bytes (8000 minus the
    /// header) goes in fragments and arrives exact.</summary>
    public static byte[] GamePad(byte[] body) => body.Length > 7992 || body.Length % 4 == 0 ? body : [.. body, .. new byte[4 - body.Length % 4]];

    public int Count(CameraMessage id, bool toClient = true) => (toClient ? SentToClient : SentToServer).Count(m => m.Id == id);
}

internal static class Requests
{
    public static CameraWireRequest Lens(float fov = 60, uint durationMs = 10000) => new()
    {
        Parts = CameraWireAbi.PartLens, LensSource = CameraWireAbi.LensConstant, FovDegrees = fov, DurationMs = durationMs,
    };

    /// <summary>A full path: 256 keys, keyed lens, shake.</summary>
    public static CameraWireRequest Path(int keys = 256, uint durationMs = 120000) => new()
    {
        Parts = CameraWireAbi.PartPath | CameraWireAbi.PartLens | CameraWireAbi.PartShake,
        InputPolicy = CameraWireAbi.VisualOnly,
        DurationMs = durationMs,
        BlendInMs = 500,
        BlendOutMs = 250,
        Priority = -7,
        LensSource = CameraWireAbi.LensKeys,
        Amplitude = 0.25f,
        Frequency = 7.5f,
        FalloffMs = durationMs / 2,
        Seed = 123,
        Keys = [.. Enumerable.Range(0, keys).Select(i => new WireKey((uint)(i * 400), 1000.5 + i, -2.25 * i, 1e6 - i, 0, (float)Math.Sin(0.01 * i), 0,
            (float)Math.Cos(0.01 * i), 40 + i % 100))],
    };

    public static CameraWireRequest Profile() => new()
    {
        Parts = CameraWireAbi.PartProfile | CameraWireAbi.PartShake, DurationMs = 8000, FollowEntity = 77, RotationMode = CameraWireAbi.FollowHeading,
        Distance = 12.5f, OffsetX = 1, OffsetY = 2, OffsetZ = -3, OffsetWorldY = 4.5f, RotX = 0.1045285f, RotW = 0.9945219f,
        Amplitude = 0.1f, Frequency = 2, FalloffMs = 0, Seed = 0,
    };
}
