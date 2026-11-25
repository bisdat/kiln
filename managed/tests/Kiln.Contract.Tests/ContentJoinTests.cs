// SPDX-License-Identifier: MIT
using System.Collections.Concurrent;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json.Nodes;
using Xunit;

namespace Kiln.Contract.Tests;

/// <summary>A server with added buffs and a joining Kiln client (Roger de R'lyeh's report, Kiln 0.2.0-dev.29.18, #mod-dev
/// 2026-11-20): a mod with two StatusBlocks (side server, clients kiln), hot-loaded with a player in, refused that player
/// with "kiln.abilities did not register the programs (4)"; after the mod was removed, the placeholders of its buffs kept
/// the server's added-buff check on, and every join was held at it and refused after 25 s with "Waiting for this game's
/// content and added-buff capacity check", from a cold start too.
/// <para>One process plays both roles over a loopback, as HeldHelloTests do: the client role is a game that joins and is
/// held at its Gameplay Ready, so its client world never steps and kiln.content's registries are not loaded there
/// (kiln.content becomes ready in the client world's steps only); the server role has kiln.gamedata's buff room with
/// two records reserved for the removed mod's placeholders. The server's content state is sent as ContentSync sends it.</para></summary>
public sealed unsafe class ContentJoinTests : FakeTableTest
{
    private const uint Peer = 0x81;
    private const string HeldAnswer = "kiln.content.held-answer";  // the handshake property: part of the wire contract
    private static readonly Guid Rage = Guid.Parse("3f0d6c2e-5b7a-5c1e-9d42-0a1b2c3d4e5f"), Hunt = Guid.Parse("8e2a4b6c-1d3f-5a7b-8c9d-0e1f2a3b4c5d");

    private readonly Gate _gate = new();
    private readonly Loop _loop = new();
    private readonly bool _modsLoaded = Lifecycle.ModsLoaded;
    private static byte _ready;        // kiln.content's registries on this "game"
    private static uint _capacity = 4096, _reserved = 2;
    private static string? _registryExport;
    private static ulong _registryGeneration;
    private static JsonObject _clientRegistry = new();

    private sealed class Gate : NetAdmission.IGate
    {
        public uint Mode = uint.MaxValue;
        public readonly List<uint> Released = new();
        public int Set(uint mode)
        {
            Mode = mode;
            return NetNative.Ok;
        }
        public int Release(uint peer)
        {
            Released.Add(peer);
            return NetNative.Ok;
        }
        public NetNative.GateInfo Read() => new() { Caps = NetNative.GateCanHold | NetAdmission.GateCanHoldAll, Mode = Mode };
        public int Kick(uint peer, uint reason) => NetNative.Ok;
    }

    private sealed class Loop : INetTransport
    {
        public readonly ConcurrentQueue<(uint From, byte[] Frame)> Inbox = new();
        public readonly ConcurrentQueue<(uint To, byte[] Frame)> Sent = new();

        public int Send(uint peer, byte[] frame)
        {
            Sent.Enqueue((peer, frame));
            if (peer == NetNative.PeerServer) Inbox.Enqueue((Peer, frame));           // client role -> server role
            else if (peer == Peer) Inbox.Enqueue((NetNative.PeerServer, frame));      // server role -> client role
            return NetNative.Ok;
        }

        public NetNative.Status Status() => new() { Roles = NetNative.RoleServer | NetNative.RoleClient, MaxFrame = 1200 };
        public NetNative.PeerInfo? Info(uint peer) =>
            new NetNative.PeerInfo { Peer = peer, Flags = peer == Peer ? NetNative.PeerProved | NetNative.PeerHeld : 0 };
        public int Kick(uint peer, uint reason) => NetNative.Ok;

        /// <summary>Set: the client role's frames arrive outside any Kiln tick, as in a game held at the join (kiln.net
        /// delivers them from the game's ClientReceive on its main thread; no Kiln tick runs while a world is there that does
        /// not step). The server role's frames stay on its tick thread.</summary>
        public OfflineBackend? OffTickClient;

        public void Pump()
        {
            for (var i = 0; i < 64 && !Inbox.IsEmpty; i++)
                while (Inbox.TryDequeue(out var f))
                {
                    if (OffTickClient is { } b) b.TickThread = f.From != NetNative.PeerServer;
                    try
                    {
                        NetEngine.OnFrame(f.From, f.Frame);
                    }
                    finally
                    {
                        if (OffTickClient is { } b2) b2.TickThread = true;
                    }
                }
        }
    }

    // ---- kiln.content: loaded, its registries not (yet) ----
    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int FakeStatus(ContentNative.Status* s)
    {
        s->Ready = _ready;
        s->Generation = _registryGeneration;
        return 0;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int FakeExport(uint kinds, byte* buf, nuint cap, nuint* len)
    {
        if (_registryExport is null) { *len = 0; return 3; }
        var bytes = Encoding.UTF8.GetBytes(_registryExport);
        *len = (nuint)bytes.Length;
        if (cap <= *len) return ContentNative.BufferTooSmall;
        bytes.CopyTo(new Span<byte>(buf, (int)cap)); buf[bytes.Length] = 0;
        return 0;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int FakeFind(uint kind, NativeStr key, ContentNative.Entry* entry) => ContentNative.NotFound;

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int FakeRegistryApply(ulong owner, NativeStr document, NativeStr source, byte* report, nuint cap, nuint* len)
    {
        _clientRegistry = JsonNode.Parse(document.ToString())!.AsObject();
        var bytes = Encoding.UTF8.GetBytes("{\"applied\":2,\"errors\":[]}");
        *len = (nuint)bytes.Length;
        bytes.CopyTo(new Span<byte>(report, (int)cap)); report[bytes.Length] = 0;
        return 0;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static uint FakeRevert(ulong owner) { _clientRegistry = new(); return 0; }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int FakeLabel(ulong owner, NativeStr label) => 0;

    // ---- kiln.gamedata: the buff room, with the records the placeholders hold ----
    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int FakeBudget(KilnCapacityInfo* o)
    {
        var size = o->Size;
        *o = default;
        o->Size = size;
        o->Readiness = 1;
        o->Flags = KilnCapacityInfo.FlagProcess | KilnCapacityInfo.FlagExpanded | KilnCapacityInfo.FlagReady;
        o->WorldKind = KilnCapacityInfo.NoWorldKind;
        o->Capacity = _capacity;
        o->Reserved = _reserved;
        return 0;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int FakeDefinitions(byte* buf, nuint cap, nuint* len)
    {
        buf[0] = (byte)'[';
        buf[1] = (byte)']';
        buf[2] = 0;
        *len = 2;
        return 0;
    }

    public ContentJoinTests()
    {
        _ready = 0;
        _registryExport = null; _registryGeneration = 1; _clientRegistry = new();
        _capacity = 4096;
        _reserved = 2;
        NetNative.TestTransport = _loop;
        NetEngine.TestServerRole = true;
        NetAdmission.TestGate = _gate;
        Lifecycle.ModsLoaded = true;

        var content = Alloc<ContentNative.Table>();
        content->Header = new ServiceHeader { Size = (uint)sizeof(ContentNative.Table), Version = 4 };
        content->GetStatus = &FakeStatus;
        content->ExportChanges = &FakeExport;
        content->ApplyJson = &FakeRegistryApply;
        content->Find = &FakeFind;
        content->RevertOwner = &FakeRevert;
        content->UndefineOwner = &FakeRevert;
        content->LabelOwner = &FakeLabel;
        Swap(typeof(Content), "_table", Box(content));

        var gd = Alloc<GameDataNative.BuffTable>();
        gd->V1.Header = new ServiceHeader { Size = (uint)sizeof(GameDataNative.BuffTable), Version = GameDataNative.Version };
        gd->V1.Definitions = &FakeDefinitions;
        gd->BuffBudget = &FakeBudget;
        Serve(GameDataNative.Service, (GameDataNative.Table*)gd, typeof(Content), "_gd");
        Swap(typeof(Content), "_gdTried", false);

        ContentSync.Start();  // once per process: the kiln:content channel and its handlers
        NetAdmission.ResetForTests();  // after Start: one kiln.content decider, registered below
        Swap(typeof(ContentSync), "_owner", 77UL);
        Swap(typeof(ContentSync), "_appliedState", 0u);
        Swap(typeof(ContentSync), "_contentAdmission", null);
        Holds().Clear();
        typeof(ContentSync).GetMethod("EnsureContentAdmission", BindingFlags.Static | BindingFlags.NonPublic)!.Invoke(null, null);
    }

    public override void Dispose()
    {
        NetEngine.OnPeer(NetNative.PeerServer, false);  // the client role leaves: ContentSync forgets the state it got
        NetEngine.OnPeer(Peer, false);
        if (_registryExport is not null)
        {
            typeof(ContentSync).GetMethod("ResetServer", BindingFlags.Static | BindingFlags.NonPublic)!.Invoke(null, ["registry test ended"]);
            _registryExport = null;
        }
        Holds().Clear();
        NetAdmission.ResetForTests();
        NetAdmission.TestGate = null;
        NetEngine.UseConfigForTests(new NetworkConfig());
        NetNative.TestTransport = null;
        NetEngine.TestServerRole = null;
        Lifecycle.ModsLoaded = _modsLoaded;
        HostAccess.Current = null;  // Kiln's own property, as ContentSync.Start sets it
        Network.SetHandshakeProperty(HeldAnswer, static () => "1");
        base.Dispose();
    }

    private static System.Collections.IDictionary Holds() =>
        (System.Collections.IDictionary)typeof(ContentSync).GetField("ContentHolds", BindingFlags.Static | BindingFlags.NonPublic)!.GetValue(null)!;

    private string? RejectTo(uint peer) => _loop.Sent.Where(s => s.To == peer && Wire.TryHeader(s.Frame, out var t) && t == Wire.Type.Reject)
        .Select(s => Wire.DecodeReject(s.Frame).Reason).LastOrDefault();

    private static NetChannel ContentChannel => Network.Channels.Single(c => c.Name == ContentSync.ChannelName);

    /// <summary>The client connects and is held at its Gameplay Ready; its HELLO goes out from the game's UI frame (the 29.1
    /// fix) and the server answers Welcome. Returns the server's record of it.</summary>
    private NetPeer Join()
    {
        NetEngine.OnPeer(Peer, true);
        var p = NetEngine.ServerPeers().Single(x => x.Id == Peer);
        NetEngine.RefreshInfo(p);
        Assert.NotNull(p.HeldSince);
        NetEngine.OnPeer(NetNative.PeerServer, true);
        NetEngine.ServerLink!.NextHelloAt = DateTime.UtcNow.AddSeconds(-1);
        for (var frame = 0; frame < 3; frame++)
        {
            NetEngine.ClientUiFrame();
            _loop.Pump();
        }
        Assert.Equal(NetPeerState.Ready, p.State);
        Assert.Equal(NetPeerState.Ready, NetEngine.ServerLink!.State);
        return p;
    }

    /// <summary>The client role's own events off the tick thread (a held game: <see cref="Loop.OffTickClient"/>).</summary>
    private void OffTick(Action a)
    {
        Backend.TickThread = false;
        try
        {
            a();
        }
        finally
        {
            Backend.TickThread = true;
        }
    }

    /// <summary><see cref="Join"/> as a held game does it: every client-role event off the tick thread.</summary>
    private NetPeer JoinHeldGame()
    {
        _loop.OffTickClient = Backend;
        NetEngine.OnPeer(Peer, true);
        var p = NetEngine.ServerPeers().Single(x => x.Id == Peer);
        NetEngine.RefreshInfo(p);
        Assert.NotNull(p.HeldSince);
        OffTick(() => NetEngine.OnPeer(NetNative.PeerServer, true));
        NetEngine.ServerLink!.NextHelloAt = DateTime.UtcNow.AddSeconds(-1);
        for (var frame = 0; frame < 3; frame++)
        {
            OffTick(NetEngine.ClientUiFrame);
            _loop.Pump();
        }
        Assert.Equal(NetPeerState.Ready, p.State);
        Assert.Equal(NetPeerState.Ready, NetEngine.ServerLink!.State);
        return p;
    }

    /// <summary>The game leaves before its world ran (refused, or its join ended while loading): both roles see the
    /// connection end; the client role's off the tick thread, so its leave waits for a Kiln tick.</summary>
    private void LeaveBeforeTheWorldRan()
    {
        OffTick(() => NetEngine.OnPeer(NetNative.PeerServer, false));
        NetEngine.OnPeer(Peer, false);
    }

    /// <summary>What the client role sent the server on kiln:content (its acks), in order.</summary>
    private List<ContentSync.AckMsg> Acks()
    {
        var list = new List<ContentSync.AckMsg>();
        foreach (var (to, frame) in _loop.Sent)
        {
            if (to != NetNative.PeerServer || !Wire.TryHeader(frame, out var t) || t != Wire.Type.Data) continue;
            var r = new NetReader(frame.AsSpan(Wire.HeaderSize));
            if (r.UInt16() != ContentChannel.ServerId || r.UInt16() != 1) continue;
            list.Add(ContentSync.AckMsg.Decode(ref r));
        }
        return list;
    }

    private static void SetStatic(string field, object value) =>
        typeof(ContentSync).GetField(field, BindingFlags.Static | BindingFlags.NonPublic)!.SetValue(null, value);

    /// <summary>The server's content state with the placeholders of the removed mod's two buffs, sent as ContentSync sends it.</summary>
    private uint SendPlaceholderState(NetPeer p)
    {
        var doc = new JsonObject
        {
            ["format"] = 1,
            ["@fields"] = 0,
            ["buffs"] = new JsonObject
            {
                ["add"] = new JsonArray(
                    new JsonObject { ["guid"] = Rage.ToString(), ["name"] = "essai.essai-rage", ["placeholder"] = new JsonObject { ["mod"] = "essai" } },
                    new JsonObject { ["guid"] = Hunt.ToString(), ["name"] = "essai.essai-traque", ["placeholder"] = new JsonObject { ["mod"] = "essai" } }),
            },
        };
        var raw = Encoding.UTF8.GetBytes(doc.ToJsonString());
        var id = ContentSync.Fnv(raw);
        var packed = ContentSync.Compress(raw);
        Assert.True(ContentChannel.SendTo(p, new ContentSync.PartMsg(ContentSync.Format, id, 1, (uint)raw.Length, 0, 0, 1, ContentSync.CodecDeflate, packed)));
        _loop.Pump();
        return id;
    }

    [Fact]
    public void Registry_layers_resync_without_rejoining()
    {
        _reserved = 0; _ready = 1;
        var peer = Join();
        void Drive(string document)
        {
            // The native half proves this document is the effective registry/store export, not registry IR.
            _registryExport = document; ++_registryGeneration;
            typeof(ContentSync).GetMethod("ServerTick", BindingFlags.Static | BindingFlags.NonPublic)!.Invoke(null, [25UL]);
            _loop.Pump();
            typeof(ContentSync).GetMethod("ClientTick", BindingFlags.Static | BindingFlags.NonPublic)!.Invoke(null, null);
            _loop.Pump();
            Assert.Equal(NetPeerState.Ready, peer.State);
            Assert.Equal(ContentSyncState.Applied, ContentSync.Status.State);
            Assert.Contains(Acks(), a => a.StateId == ContentSync.Status.StateId && a.Result == 0);
        }
        string State(int value) => $"{{\"format\":1,\"@fields\":2,\"items\":{{\"0x00000111\":{{\"maxStackSize\":{value}}}}}," +
            $"\"recipes\":{{\"0x00000333\":{{\"craftingDuration\":{value / 10}}}}}}}";
        Drive(State(90)); // server's WorldLoad pass, before this join
        Assert.Equal(90, _clientRegistry["items"]!["0x00000111"]!["maxStackSize"]!.GetValue<int>());
        var first = ContentSync.Status.StateId;
        Drive(State(120)); // later HotReload pass: no new join
        Assert.NotEqual(first, ContentSync.Status.StateId);
        Assert.Equal(120, _clientRegistry["items"]!["0x00000111"]!["maxStackSize"]!.GetValue<int>());
        Assert.Equal(12, _clientRegistry["recipes"]!["0x00000333"]!["craftingDuration"]!.GetValue<int>());
        Drive("{\"format\":1,\"@fields\":0}"); // both registry layers removed
        Assert.Null(_clientRegistry["items"]);
        Assert.Null(_clientRegistry["recipes"]);
    }

    [Fact]
    public void A_held_client_whose_world_does_not_run_yet_answers_the_content_check_and_is_let_in()
    {
        var p = Join();
        Assert.Equal(NetAdmission.GateHoldAll, _gate.Mode);
        Assert.False(NetAdmission.MayRelease(p, out var holder));
        Assert.Equal("kiln.content", holder);

        SendPlaceholderState(p);
        NetAdmission.Step(DateTime.UtcNow);

        // its answer reached the server while it was held: no wait for its deadline, no refusal
        Assert.Contains(Peer, _gate.Released);
        Assert.Null(RejectTo(Peer));
        Assert.Empty(Holds());
        NetAdmission.Step(DateTime.UtcNow.AddSeconds(30));
        Assert.Null(RejectTo(Peer));
        Assert.Equal(ContentSyncState.Waiting, ContentSync.Status.State);  // applied once its world runs
    }

    [Fact]
    public void A_held_client_whose_buff_room_is_known_and_too_small_is_refused_at_the_join_with_the_numbers()
    {
        _capacity = 3;  // a game that joined before in this process: its room is reserved, and too small
        _reserved = 2;
        var p = Join();
        SendPlaceholderState(p);
        NetAdmission.Step(DateTime.UtcNow);

        Assert.DoesNotContain(Peer, _gate.Released);
        Assert.Contains("2 added buffs", RejectTo(Peer));
        Assert.Contains("room for 3", RejectTo(Peer));
    }

    [Fact]
    public void A_client_held_without_any_answer_is_refused_at_the_deadline_with_what_did_not_happen()
    {
        var p = Join();
        Assert.False(NetAdmission.MayRelease(p, out _));
        NetAdmission.Step(DateTime.UtcNow.AddSeconds(26));  // the state never reached it

        Assert.DoesNotContain(Peer, _gate.Released);
        Assert.Contains("did not answer", RejectTo(Peer));
        Assert.DoesNotContain(NetAdmission.Active, e => e.Kind == AdmissionKind.PeerHold);
    }

    [Fact]
    public void An_older_client_that_answers_only_once_its_world_runs_is_not_held_and_a_later_failure_refuses_it()
    {
        HostAccess.Current = null;
        Network.SetHandshakeProperty(HeldAnswer, static () => "0");  // its HELLO does not say it answers while held
        HostAccess.Current = Mod;
        var p = Join();
        Assert.True(NetAdmission.MayRelease(p, out _));
        NetAdmission.Step(DateTime.UtcNow);
        Assert.Contains(Peer, _gate.Released);

        // in the world, its game applies the state and fails: the server refuses it with its reason
        var id = SendPlaceholderState(p);
        // no WELCOME property, no early answer: it waits for its registries as before (not Backend.Lines: other tests' threads log into it)
        Assert.Equal("waiting for this game's registries", ContentSync.Status.Detail);
        ContentChannel.SendToServer(new ContentSync.AckMsg(id, 1, 0, 0, 1, 0, "client: definition essai.essai-rage: refused"));
        _loop.Pump();
        Assert.Equal("client: definition essai.essai-rage: refused", RejectTo(Peer));
    }

    [Fact]
    public void A_held_game_answers_from_outside_any_kiln_tick_and_is_let_in()
    {
        var p = JoinHeldGame();
        SendPlaceholderState(p);
        NetAdmission.Step(DateTime.UtcNow);

        Assert.Contains(Peer, _gate.Released);
        Assert.Null(RejectTo(Peer));
        Assert.Equal(ContentSync.AckDeferred, Assert.Single(Acks()).Result);
    }

    [Fact]
    public void A_game_held_again_after_a_join_that_ended_before_its_world_ran_answers_the_same_state_again()
    {
        // review of dev29/statusblock-join: the first join was answered (deferred) and let in, then ended before its
        // world ran (a refusal by another holder, a disconnect while loading). The leave that forgets that answer waits
        // for a Kiln tick, which this game does not get before it is held again at its next join.
        var p = JoinHeldGame();
        var id = SendPlaceholderState(p);
        NetAdmission.Step(DateTime.UtcNow);
        Assert.Equal(1, _gate.Released.Count(x => x == Peer));
        LeaveBeforeTheWorldRan();

        p = JoinHeldGame();
        Assert.Equal(id, SendPlaceholderState(p));  // the server's content did not change: the same state
        NetAdmission.Step(DateTime.UtcNow);

        Assert.Equal(2, _gate.Released.Count(x => x == Peer));
        Assert.Null(RejectTo(Peer));
        Assert.Equal(2, Acks().Count(a => a.StateId == id && a.Result == ContentSync.AckDeferred));
    }

    [Fact]
    public void A_game_that_rejoins_after_it_applied_the_state_answers_it_again_while_its_leave_waits_for_a_step()
    {
        // review of dev29/statusblock-join: the player played (its world applied the state), left, and joins the same
        // server again with the same content. Until the leave runs at a Kiln tick, the applied state is the last
        // connection's: it must not stand for an answer to this one.
        var p = JoinHeldGame();
        var id = SendPlaceholderState(p);
        NetAdmission.Step(DateTime.UtcNow);
        SetStatic("_appliedState", id);  // its world ran and applied it
        LeaveBeforeTheWorldRan();

        p = JoinHeldGame();
        SendPlaceholderState(p);
        NetAdmission.Step(DateTime.UtcNow);

        Assert.Equal(2, _gate.Released.Count(x => x == Peer));
        Assert.Null(RejectTo(Peer));
    }

    [Fact]
    public void A_held_game_without_kiln_content_says_so_at_once_instead_of_timing_out()
    {
        // review of dev29/statusblock-join: ClientTick answers this without registries, but no Kiln tick runs in a held
        // game: the player waited 25 s for "did not answer in time ... update Kiln", the hold's own text
        Swap(typeof(ContentSync), "_owner", 0UL);
        var p = JoinHeldGame();
        SendPlaceholderState(p);
        NetAdmission.Step(DateTime.UtcNow);

        Assert.DoesNotContain(Peer, _gate.Released);
        Assert.Contains("kiln.content", RejectTo(Peer));
    }

    [Fact]
    public void An_older_clients_program_refresh_error_on_a_joined_game_does_not_refuse_it()
    {
        // review of dev29/statusblock-join: a Kiln client before 0.2.0-dev.30 still reports kiln.abilities'
        // WRONG_THREAD for a state with programs (a StatusBlock's) although it applied it whole. The fixed server is not
        // to refuse it for that (at the join, or at a hot load with the player in).
        HostAccess.Current = null;
        Network.SetHandshakeProperty(HeldAnswer, static () => "0");
        HostAccess.Current = Mod;
        var p = Join();
        NetAdmission.Step(DateTime.UtcNow);
        var id = SendPlaceholderState(p);
        ContentChannel.SendToServer(new ContentSync.AckMsg(id, 1, 2, 0, 1, 3, "kiln.abilities did not register the programs (4)"));
        _loop.Pump();
        Assert.Null(RejectTo(Peer));

        // any other error besides it is a failure, as before
        ContentChannel.SendToServer(new ContentSync.AckMsg(id, 1, 2, 0, 2, 3, "kiln.abilities did not register the programs (4); definition essai.essai-rage: refused"));
        _loop.Pump();
        Assert.Equal("kiln.abilities did not register the programs (4); definition essai.essai-rage: refused", RejectTo(Peer));
    }

    [Fact]
    public void The_placeholders_of_a_removed_mods_buffs_keep_the_servers_added_buff_check_on()
    {
        _reserved = 0;
        Assert.False(ContentSync.RequiresBuffAdmission());
        _reserved = 2;  // the removed mod's placeholders
        Assert.True(ContentSync.RequiresBuffAdmission());
    }
}
