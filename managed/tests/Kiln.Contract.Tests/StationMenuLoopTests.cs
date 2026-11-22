// SPDX-License-Identifier: MIT
// Station menus' server and client loops (review 16a M1, S1, S3): the snapshot follows the content state the server serves,
// a refused snapshot is not sent again every tick, and a client that cannot use the server's menus stops holding its own
// workshops empty. The fakes of StationMenuSyncTests.cs (FakeMenuBackend) and a fake kiln.net.
using System.Collections.Concurrent;
using System.Reflection;
using System.Runtime.CompilerServices;
using Xunit;

namespace Kiln.Contract.Tests;

[Collection("kiln.network engine")]
public sealed class StationMenuLoopTests : IDisposable
{
    private const uint P = 0xC1, Q = 0xC2;
    private static readonly BindingFlags SF = BindingFlags.Static | BindingFlags.NonPublic;

    private sealed class Gate : NetAdmission.IGate
    {
        public uint Caps = NetNative.GateCanHold | NetAdmission.GateCanHoldAll, Mode = uint.MaxValue;
        public int Set(uint mode) { Mode = mode; return NetNative.Ok; }
        public int Release(uint peer) => NetNative.Ok;
        public NetNative.GateInfo Read() => new() { Caps = Caps, Mode = Mode };
        public int Kick(uint peer, uint reason) => NetNative.Ok;
    }

    private sealed class Net : INetTransport
    {
        public readonly ConcurrentDictionary<uint, uint> Flags = new();
        public readonly ConcurrentQueue<(uint Peer, byte[] Frame)> Sent = new();
        public int Send(uint peer, byte[] frame) { Sent.Enqueue((peer, frame)); return NetNative.Ok; }
        public NetNative.Status Status() => new() { Roles = NetNative.RoleServer, MaxFrame = 1200 };
        public NetNative.PeerInfo? Info(uint peer) => new NetNative.PeerInfo { Peer = peer, Flags = Flags.GetValueOrDefault(peer) };
        public int Kick(uint peer, uint reason) => NetNative.Ok;
    }

    private readonly Gate _gate = new();
    private readonly Net _net = new();
    private readonly FakeMenuBackend _store = new();

    public StationMenuLoopTests()
    {
        NetNative.TestTransport = _net;
        NetEngine.TestServerRole = true;
        NetAdmission.ResetForTests();
        NetAdmission.TestGate = _gate;
        StationMenuStore.ResetForTests();
        StationMenuSync.ResetForTests();
        StationMenuStore.TestBackend = _store;
        StationMenuSync.TestDeclared = () => true;
    }

    public void Dispose()
    {
        var ch = typeof(StationMenuSync).GetField("_channel", SF)!;
        (ch.GetValue(null) as IDisposable)?.Dispose();
        ch.SetValue(null, null);
        typeof(ContentSync).GetField("_export", SF)!.SetValue(null, null);
        foreach (var id in new[] { P, Q }) NetEngine.OnPeer(id, false);
        NetAdmission.ResetForTests();
        NetAdmission.TestGate = null;
        NetEngine.UseConfigForTests(new NetworkConfig());
        NetNative.TestTransport = null;
        NetEngine.TestServerRole = null;
        StationMenuSync.ResetForTests();
        StationMenuStore.ResetForTests();
        StationMenuStore.TestBackend = null;
    }

    private static void Call(string method, params object[] args) => typeof(StationMenuSync).GetMethod(method, SF)!.Invoke(null, args);

    private NetPeer Join(uint id)
    {
        NetEngine.OnPeer(id, true);
        var hello = new Wire.Hello(Wire.Protocol, Game.KilnVersion, 7, [],
            [new Wire.ChannelEntry(0, StationMenuSync.ChannelName, StationMenuSync.ChannelVersion, false)], new Dictionary<string, string>());
        NetEngine.OnFrame(id, Wire.EncodeHello(hello));
        return NetEngine.ServerPeers().Single(x => x.Id == id);
    }

    private int FramesTo(uint id) => _net.Sent.Count(s => s.Peer == id);

    // ContentSync serves content state id 'state' (what ContentSync.Serving answers on a server).
    private static void Serve(uint state)
    {
        var t = typeof(ContentSync).GetNestedType("Export", BindingFlags.NonPublic)!;
        var e = RuntimeHelpers.GetUninitializedObject(t);
        t.GetField("StateId")!.SetValue(e, state);
        t.GetField("Parts")!.SetValue(e, new byte[][] { [1] });
        typeof(ContentSync).GetField("_export", SF)!.SetValue(null, e);
    }

    private static uint SnapshotContentState()
    {
        var s = typeof(StationMenuSync).GetField("_snapshot", SF)!.GetValue(null)!;
        return (uint)s.GetType().GetField("ContentStateId")!.GetValue(s)!;
    }

    private const string Def = "724d7b7b-a362-566b-b0a7-5088f219266f";
    private static string Record() => $"{{\"definition\":{{\"id\":\"{Def}\"}},\"format\":1,\"origin\":{{\"workshopId\":1}},\"tabs\":[]}}";

    // S1: a client's Refused ack makes the server send the same snapshot again at the next server tick, without end.
    [Fact]
    public void A_refused_ack_does_not_resend_the_same_snapshot_every_tick()
    {
        Call("EnsureChannel");
        var p = Join(P);
        Assert.Equal(NetPeerState.Ready, p.State);
        Call("ServerTick", 5UL);
        var once = FramesTo(P);
        Assert.True(once > 0, "the tombstone snapshot was not sent at all");
        Call("ServerTick", 10UL);
        Assert.Equal(once, FramesTo(P));  // sent once
        var channel = typeof(StationMenuSync).GetField("_channel", SF)!.GetValue(null)!;
        var ctx = (NetContext)Activator.CreateInstance(typeof(NetContext), BindingFlags.Instance | BindingFlags.NonPublic, null,
            [channel, p, NetDirection.ToServer], null)!;
        for (var i = 0; i < 5; i++)
        {
            Call("OnAck", new StationMenuSync.MenuAckMsg(new string('a', 64), StationMenuSync.Session.ToString("D"), 0, StationMenuSync.StageRefused, "a deterministic refusal"), ctx);
            Call("ServerTick", 15UL + (ulong)i * 5);
        }
        Assert.Equal(once, FramesTo(P));  // c2021190: 6x as many frames, one full snapshot per refused ack
    }

    // M1: the snapshot keeps naming the content state that was served when the menus last changed; a client that joins after
    // the server's content state moved on waits for a state it never gets (pending: the station lists nothing).
    [Fact]
    public void The_snapshot_follows_the_content_state_the_server_serves()
    {
        Call("EnsureChannel");
        Join(P);
        Serve(0x1111);
        var r = StationMenuStore.Define(42, "m", Record(), Guid.Parse(Def), 1);
        Assert.True(r.Ok, r.Reason);
        Call("ServerTick", 5UL);
        Assert.Equal(0x1111u, SnapshotContentState());
        Serve(0x2222);  // a content change (a hot load, a value change, a new Kiln-only template): menus unchanged
        Call("ServerTick", 10UL);
        Join(Q);
        Call("ServerTick", 15UL);
        Assert.Equal(0x2222u, SnapshotContentState());  // c2021190: still 0x1111, also in what Q received
    }
    // ---- the client (review S3): every way the server's menus cannot be used ends the hold on the mod's own workshops ----

    private static NetContext FromServer()
    {
        var channel = (NetChannel)typeof(StationMenuSync).GetField("_channel", SF)!.GetValue(null)!;
        return new NetContext(channel, new NetPeer(NetNative.PeerServer), NetDirection.ToClient);
    }

    private static StationMenuSync.MenuPartMsg[] Parts(uint contentState, int partBytes = 4096)
    {
        var raw = System.Text.Encoding.UTF8.GetBytes(StationMenuSync.Envelope(Guid.NewGuid(), 3, contentState, "[" + Record() + "]"));
        var id = StationMenuSync.StateIdOf(raw);
        var parts = StationMenuSync.Cut(StationMenuSync.Compress(raw), partBytes);
        var session = System.Text.Json.JsonDocument.Parse(raw).RootElement.GetProperty("session").GetString()!;
        return parts.Select((d, i) => new StationMenuSync.MenuPartMsg(1, id, session, 3, (uint)raw.Length, (ushort)i, (ushort)parts.Length, 1, d)).ToArray();
    }

    private static void Part(StationMenuSync.MenuPartMsg m, NetContext ctx) => Call("OnPart", m, ctx);

    [Fact]
    public void A_refused_snapshot_ends_the_hold()
    {
        Call("EnsureChannel");
        StationMenuSync.BeginHold();
        Assert.True(_store.Pending);
        var p = Parts(0)[0];
        Part(p with { StateId = new string('b', 64) }, FromServer());  // the hash does not match: refused
        Assert.False(_store.Pending);
    }

    [Fact]
    public void A_snapshot_the_game_refuses_ends_the_hold()
    {
        Call("EnsureChannel");
        StationMenuSync.BeginHold();
        _store.RemoteRc = StationMenuNative.InvalidArgument;
        StationMenuSync.TestContentReady = _ => true;
        Part(Parts(0)[0], FromServer());
        Assert.Null(_store.LastRemote);
        Assert.False(_store.Pending);
    }

    [Fact]
    public void A_content_state_that_does_not_come_ends_the_hold_and_the_snapshot_still_applies_later()
    {
        Call("EnsureChannel");
        StationMenuSync.BeginHold();
        var ready = false;
        StationMenuSync.TestContentReady = _ => ready;
        StationMenuSync.ContentDeadline = TimeSpan.Zero;
        Part(Parts(0x77)[0], FromServer());
        Thread.Sleep(5);
        Call("ClientTick");
        Assert.False(_store.Pending);
        Assert.Null(_store.LastRemote);
        ready = true;  // the content state arrives after all
        Call("ClientTick");
        Assert.NotNull(_store.LastRemote);
    }

    [Fact]
    public void Parts_that_time_out_end_the_hold()
    {
        Call("EnsureChannel");
        StationMenuSync.BeginHold();
        var parts = Parts(0, partBytes: 64);
        Assert.True(parts.Length > 1);
        Part(parts[0], FromServer());
        StationMenuSync.ReceiptDeadline = TimeSpan.Zero;
        Thread.Sleep(5);
        Call("ClientTick");
        Assert.False(_store.Pending);
    }
}
