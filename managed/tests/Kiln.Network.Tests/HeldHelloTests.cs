// SPDX-License-Identifier: MIT
using System.Collections.Concurrent;
using Xunit;

namespace Kiln.Network.Tests;

/// <summary>0.2.0-dev.29's join deadlock (inbox section 30): a server with an OnJoin decider (Kiln.Content's "kiln.content")
/// keeps its gate in "hold all", so kiln.net holds even a proved Kiln client until Kiln.Network lets it in, and that waits
/// for the client's HELLO. A held client gets no world data, so its client world never steps; its HELLO must not wait
/// for that step. Here one engine plays both roles over a loopback (as SteamGuardRelayTests): the client world's tick never
/// runs and no frame reaches the client before its HELLO; only the game's UI frames run.</summary>
[Collection(nameof(SteamGuardRelayTests))]  // the engine's statics: never at once with the other engine tests
public sealed class HeldHelloTests : IDisposable
{
    private const uint Peer = 0x101;
    private readonly AdmissionTests.FakeGate _gate = new();
    private readonly Loop _loop = new();
    private readonly bool _modsLoaded = Lifecycle.ModsLoaded;

    private sealed class Loop : INetTransport
    {
        public readonly ConcurrentQueue<(uint From, byte[] Frame)> Inbox = new();
        public readonly ConcurrentQueue<(uint To, byte[] Frame)> Sent = new();
        public uint PeerFlags = NetNative.PeerProved | NetNative.PeerHeld;  // knocked, then held at its Gameplay Ready

        public int Send(uint peer, byte[] frame)
        {
            Sent.Enqueue((peer, frame));
            if (peer == NetNative.PeerServer) Inbox.Enqueue((Peer, frame));           // client role -> server role
            else if (peer == Peer) Inbox.Enqueue((NetNative.PeerServer, frame));      // server role -> client role
            return NetNative.Ok;
        }

        public NetNative.Status Status() => new() { Roles = NetNative.RoleServer | NetNative.RoleClient, MaxFrame = 1200 };
        public NetNative.PeerInfo? Info(uint peer) => new NetNative.PeerInfo { Peer = peer, Flags = peer == Peer ? PeerFlags : 0 };
        public int Kick(uint peer, uint reason) => NetNative.Ok;

        public void Pump()
        {
            while (Inbox.TryDequeue(out var f)) NetEngine.OnFrame(f.From, f.Frame);
        }
    }

    public HeldHelloTests()
    {
        NetNative.TestTransport = _loop;
        NetEngine.TestServerRole = true;
        NetAdmission.ResetForTests();
        NetAdmission.TestGate = _gate;
        Lifecycle.ModsLoaded = true;  // the client loaded its mods in the main menu
    }

    public void Dispose()
    {
        NetEngine.OnPeer(NetNative.PeerServer, false);
        NetEngine.OnPeer(Peer, false);
        NetAdmission.ResetForTests();
        NetAdmission.TestGate = null;
        NetEngine.UseConfigForTests(new NetworkConfig());
        NetNative.TestTransport = null;
        NetEngine.TestServerRole = null;
        Lifecycle.ModsLoaded = _modsLoaded;
    }

    private string? RejectTo(uint peer) => _loop.Sent.Where(s => s.To == peer && Wire.TryHeader(s.Frame, out var t) && t == Wire.Type.Reject)
        .Select(s => Wire.DecodeReject(s.Frame).Reason).LastOrDefault();

    [Fact]
    public void A_held_kiln_client_whose_world_never_steps_sends_its_hello_and_is_let_in()
    {
        // The server: Kiln.Content's decider (no buff hold for this peer), so the gate holds every join.
        using var decider = NetAdmission.OnJoin("kiln.content", _ => null);
        Assert.Equal(NetAdmission.GateHoldAll, _gate.Mode);

        // The client connects: the server role sees its machine, proved by its knock and held at its Gameplay Ready.
        NetEngine.OnPeer(Peer, true);
        var p = NetEngine.ServerPeers().Single(x => x.Id == Peer);
        NetEngine.RefreshInfo(p);
        Assert.NotNull(p.HeldSince);

        // The client role's transport is up; its "handshake in 1 s" is due. Its client world never steps (no
        // Client.Tick), and nothing comes from the server before a HELLO: only the game's UI frames run.
        NetEngine.OnPeer(NetNative.PeerServer, true);
        NetEngine.ServerLink!.NextHelloAt = DateTime.UtcNow.AddSeconds(-1);
        for (var frame = 0; frame < 3; frame++)
        {
            NetEngine.ClientUiFrame();
            _loop.Pump();
        }

        // The server got its HELLO while holding it, answered Welcome, and lets it in at its next admission pass.
        Assert.True(_loop.Sent.Any(s => s.To == NetNative.PeerServer && Wire.TryHeader(s.Frame, out var t) && t == Wire.Type.Hello),
            "the held client never sent its hello");
        Assert.Equal(NetPeerState.Ready, p.State);
        Assert.Equal(NetPeerState.Ready, NetEngine.ServerLink!.State);
        NetAdmission.Step(DateTime.UtcNow);
        Assert.Contains(Peer, _gate.Released);

        // vanillaHoldSeconds later (the server's own timer): a Ready peer is not judged as a client without Kiln, and
        // nothing refuses it with "Your Kiln handshake did not complete before admission timed out".
        var later = p.HeldSince!.Value.AddSeconds(new NetworkConfig().VanillaHoldSeconds + 1);
        if (VanillaGate.TimeIsUp(p.State, p.IsLab, p.HeldSince, p.InGameSince, later, new NetworkConfig()))
        {
            p.State = NetPeerState.Vanilla;
            NetAdmission.DecideVanilla(p, false, "", later);
        }
        Assert.Null(RejectTo(Peer));
        Assert.Null(p.KickAt);
    }

    [Fact]
    public void The_ui_frame_does_not_send_a_hello_before_the_mods_are_loaded_nor_before_it_is_due()
    {
        NetEngine.OnPeer(Peer, true);
        NetEngine.OnPeer(NetNative.PeerServer, true);
        NetEngine.ClientUiFrame();  // "handshake in 1 s": not yet
        Assert.DoesNotContain(_loop.Sent, s => s.To == NetNative.PeerServer);
        NetEngine.ServerLink!.NextHelloAt = DateTime.UtcNow.AddSeconds(-1);
        Lifecycle.ModsLoaded = false;  // a HELLO lists the loaded mods: none before they are
        NetEngine.ClientUiFrame();
        Assert.DoesNotContain(_loop.Sent, s => s.To == NetNative.PeerServer);
        Lifecycle.ModsLoaded = true;
        NetEngine.ClientUiFrame();
        Assert.Single(_loop.Sent, s => s.To == NetNative.PeerServer);
        NetEngine.ClientUiFrame();  // the retry waits its 3 s
        Assert.Single(_loop.Sent, s => s.To == NetNative.PeerServer);
    }
}
