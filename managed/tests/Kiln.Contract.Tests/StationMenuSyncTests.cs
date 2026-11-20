// SPDX-License-Identifier: MIT
// Station menus (chapter 16a, dev31 DESIGN.md): the kiln:station-menu codec (parts, hash, staging, envelope), the store
// facade's publication rules and events, and the admission decider through Kiln.Network's real hello path on a fake
// kiln.net: an older Kiln client without the channel is refused before it enters the world under every modSync, a client
// with it is let in, a client without Kiln is not concerned, and joins wait while the menus are not declared.
using System.Collections.Concurrent;
using System.Text;
using Xunit;

namespace Kiln.Contract.Tests;

internal sealed class FakeMenuBackend : IStationMenuBackend
{
    public readonly Dictionary<ulong, (ulong Owner, string Record)> Layers = new();
    public ulong Gen, NextHandle = 1;
    public string? LastRemote;
    public bool Pending;
    public int Defines;
    public bool Available => true;

    public (int Rc, ulong Handle, ulong Generation, string State, string Error) Define(ulong owner, string record)
    {
        Defines++;
        if (record.Contains("\"bad\"")) return (StationMenuNative.InvalidArgument, 0, Gen, "", "unknown member bad");
        var id = System.Text.Json.JsonDocument.Parse(record).RootElement.GetProperty("definition").GetProperty("id").GetString();
        foreach (var (h, l) in Layers)
            if (l.Owner == owner && l.Record.Contains(id!))
            {
                if (l.Record == record) return (0, h, Gen, "unchanged", "");
                Layers[h] = (owner, record);
                return (0, h, ++Gen, "replaced", "");
            }
        var handle = NextHandle++;
        Layers[handle] = (owner, record);
        return (0, handle, ++Gen, "defined", "");
    }

    public (int Rc, ulong Generation) Remove(ulong owner, ulong handle)
    {
        if (!Layers.TryGetValue(handle, out var l)) return (StationMenuNative.NotFound, Gen);
        if (l.Owner != owner) return (StationMenuNative.InvalidArgument, Gen);
        Layers.Remove(handle);
        return (0, ++Gen);
    }

    public string? Export() => $"{{\"format\":1,\"generation\":{Gen},\"menus\":[{string.Join(",", Layers.Values.Select(l => l.Record))}]}}";
    public (int Rc, string Error) RemoteReplace(ulong owner, string snapshot) { LastRemote = snapshot; return (0, ""); }
    public int RemoteClear(ulong owner) { LastRemote = null; return 0; }
    public int SetPending(bool on) { Pending = on; return 0; }
    public string? DrainApplied() => "[]";
    public StationMenuNative.Info? Info() => new StationMenuNative.Info { Render = 1 };
}

public sealed class StationMenuCodecTests
{
    private static readonly string Session = Guid.Parse("0b6c8f6e-5d7a-4c55-9a5e-2b0c8c1f4a11").ToString("D");

    private static string Record(string def, uint workshop) =>
        $"{{\"baseWorkshopId\":1302892403,\"definition\":{{\"id\":\"{def}\",\"key\":\"forge.menu\",\"mod\":\"m\"}},\"format\":1," +
        $"\"origin\":{{\"mod\":\"m\",\"stationKey\":\"forge\",\"workshopGuid\":\"66b9e41a-34d5-51c4-8cec-36c189ab26cc\",\"workshopId\":{workshop}}},\"tabs\":[]}}";

    private static (byte[] Raw, StationMenuSync.MenuPartMsg[] Parts) Snapshot(string menus, int partBytes = 64, ulong generation = 3)
    {
        var raw = Encoding.UTF8.GetBytes(StationMenuSync.Envelope(Guid.Parse(Session), generation, 0x1234, menus));
        var id = StationMenuSync.StateIdOf(raw);
        var parts = StationMenuSync.Cut(StationMenuSync.Compress(raw), partBytes);
        return (raw, parts.Select((d, i) => new StationMenuSync.MenuPartMsg(1, id, Session, generation, (uint)raw.Length, (ushort)i, (ushort)parts.Length, 1, d)).ToArray());
    }

    [Fact]
    public void The_envelope_has_the_chapters_keys_in_canonical_order()
    {
        var text = StationMenuSync.Envelope(Guid.Parse(Session), 7, 9, "[]");
        Assert.Equal($"{{\"contentStateId\":9,\"format\":1,\"generation\":7,\"menus\":[],\"session\":\"{Session}\"}}", text);
        Assert.Equal("{\"format\":1,\"generation\":7,\"menus\":[]}", StationMenuSync.Container(7, "[]"));
        var id = StationMenuSync.StateIdOf(Encoding.UTF8.GetBytes(text));
        Assert.Equal(64, id.Length);
        Assert.Equal(id, id.ToLowerInvariant());
        Assert.Equal(Convert.ToHexStringLower(System.Security.Cryptography.SHA256.HashData(Encoding.UTF8.GetBytes(text))), id);
    }

    [Fact]
    public void Parts_in_any_order_give_the_exact_bytes_back()
    {
        var menus = "[" + Record("724d7b7b-a362-566b-b0a7-5088f219266f", 2947643217) + "]";
        var (raw, parts) = Snapshot(menus);
        Assert.True(parts.Length > 2);
        StationMenuSync.StagingState? s = null;
        StationMenuSync.Staged? done = null;
        foreach (var p in parts.Reverse())
        {
            var (d, refuse, next) = StationMenuSync.Accept(s, p, DateTime.UtcNow);
            Assert.Null(refuse);
            s = next;
            done = d ?? done;
        }
        Assert.NotNull(done);
        Assert.Equal(raw, done!.Raw);
        var parsed = StationMenuSync.ParseEnvelope(done.Raw, Session, 3);
        Assert.Null(parsed.Error);
        Assert.Equal(1, parsed.Menus);
        Assert.Equal(0x1234u, parsed.ContentStateId);
        Assert.Equal("{\"format\":1,\"generation\":3,\"menus\":" + menus + "}", parsed.Container);
    }

    [Fact]
    public void A_duplicate_part_counts_once()
    {
        var (raw, parts) = Snapshot("[]", partBytes: 8);
        StationMenuSync.StagingState? s = null;
        StationMenuSync.Staged? done = null;
        foreach (var p in parts.Take(1).Concat(parts))
        {
            var r = StationMenuSync.Accept(s, p, DateTime.UtcNow);
            s = r.Next;
            done = r.Done ?? done;
        }
        Assert.Equal(raw, done!.Raw);
    }

    [Fact]
    public void A_hash_that_does_not_match_is_refused()
    {
        var (_, parts) = Snapshot("[]");
        var wrong = parts.Select(p => p with { StateId = new string('a', 64) }).ToArray();
        StationMenuSync.StagingState? s = null;
        string? refused = null;
        foreach (var p in wrong)
        {
            var r = StationMenuSync.Accept(s, p, DateTime.UtcNow);
            s = r.Next;
            refused ??= r.Refuse;
        }
        Assert.Contains("hash", refused);
    }

    [Fact]
    public void Bad_metadata_is_refused_before_anything_is_staged()
    {
        var (_, parts) = Snapshot("[]");
        var p = parts[0];
        Assert.Contains("format", StationMenuSync.Accept(null, p with { Format = 2 }, DateTime.UtcNow).Refuse);
        Assert.Contains("codec", StationMenuSync.Accept(null, p with { Codec = 0 }, DateTime.UtcNow).Refuse);
        Assert.Contains("state id", StationMenuSync.Accept(null, p with { StateId = "ABC" }, DateTime.UtcNow).Refuse);
        Assert.Contains("session", StationMenuSync.Accept(null, p with { Session = "nope" }, DateTime.UtcNow).Refuse);
        Assert.Contains("part", StationMenuSync.Accept(null, p with { Parts = 0 }, DateTime.UtcNow).Refuse);
        Assert.Contains("part", StationMenuSync.Accept(null, p with { Part = p.Parts }, DateTime.UtcNow).Refuse);
        Assert.Contains("part", StationMenuSync.Accept(null, p with { Parts = 4097 }, DateTime.UtcNow).Refuse);
        Assert.Contains("64 MiB", StationMenuSync.Accept(null, p with { RawLength = (64u << 20) + 1 }, DateTime.UtcNow).Refuse);
        Assert.Contains("96 KiB", StationMenuSync.Accept(null, p with { Data = new byte[96 * 1024 + 1] }, DateTime.UtcNow).Refuse);
    }

    [Fact]
    public void Another_session_or_generation_never_shares_the_pending_parts()
    {
        var (_, a) = Snapshot("[]", partBytes: 8, generation: 3);
        var (rawB, b) = Snapshot("[]", partBytes: 8, generation: 4);
        Assert.True(a.Length > 1);
        var r = StationMenuSync.Accept(null, a[0], DateTime.UtcNow);
        Assert.Null(r.Done);
        StationMenuSync.StagingState? s = r.Next;
        StationMenuSync.Staged? done = null;
        foreach (var p in b.Skip(1).Concat(b.Take(1)))
        {
            var x = StationMenuSync.Accept(s, p, DateTime.UtcNow);
            Assert.Null(x.Refuse);
            s = x.Next;
            done = x.Done ?? done;
        }
        Assert.Equal(rawB, done!.Raw);
    }

    [Fact]
    public void Staging_older_than_the_receipt_deadline_starts_over()
    {
        var (_, parts) = Snapshot("[]", partBytes: 8);
        var t0 = DateTime.UtcNow;
        var r = StationMenuSync.Accept(null, parts[0], t0);
        var later = StationMenuSync.Accept(r.Next, parts[1], t0 + TimeSpan.FromSeconds(31));
        Assert.Equal(1, later.Next!.Have);
    }

    [Fact]
    public void The_envelope_is_checked_before_the_collection_is_replaced()
    {
        string Env(string menus, string session = "", ulong generation = 3, string extra = "") =>
            $"{{\"contentStateId\":1,\"format\":1,\"generation\":{generation},\"menus\":{menus},\"session\":\"{(session.Length > 0 ? session : Session)}\"{extra}}}";
        byte[] B(string s) => Encoding.UTF8.GetBytes(s);
        var a = Record("724d7b7b-a362-566b-b0a7-5088f219266f", 1);
        var b = Record("11111111-a362-566b-b0a7-5088f219266f", 2);
        Assert.Null(StationMenuSync.ParseEnvelope(B(Env($"[{a},{b}]")), Session, 3).Error);
        Assert.Contains("session", StationMenuSync.ParseEnvelope(B(Env("[]", Guid.NewGuid().ToString("D"))), Session, 3).Error);
        Assert.Contains("generation", StationMenuSync.ParseEnvelope(B(Env("[]", generation: 4)), Session, 3).Error);
        Assert.Contains("twice", StationMenuSync.ParseEnvelope(B(Env($"[{a},{a.Replace("\"workshopId\":1", "\"workshopId\":5")}]")), Session, 3).Error);
        Assert.Contains("workshop", StationMenuSync.ParseEnvelope(B(Env($"[{a},{b.Replace("\"workshopId\":2", "\"workshopId\":1")}]")), Session, 3).Error);
        Assert.Contains("unknown member", StationMenuSync.ParseEnvelope(B(Env("[]", extra: ",\"x\":1")), Session, 3).Error);
        Assert.Contains("format", StationMenuSync.ParseEnvelope(B(Env("[]").Replace("\"format\":1", "\"format\":2")), Session, 3).Error);
        Assert.Contains("not JSON", StationMenuSync.ParseEnvelope(B("{"), Session, 3).Error);
        var empty = StationMenuSync.ParseEnvelope(B(Env("[]")), Session, 3);
        Assert.Null(empty.Error);
        Assert.Equal(0, empty.Menus);  // the tombstone
    }

    [Fact]
    public void The_messages_round_trip_through_the_wire()
    {
        var m = new StationMenuSync.MenuPartMsg(1, new string('b', 64), Session, 9, 100, 1, 3, 1, [1, 2, 3]);
        var w = new NetWriter();
        m.Encode(w);
        var r = new NetReader(w.ToArray());
        var back = StationMenuSync.MenuPartMsg.Decode(ref r);
        Assert.Equal(m with { Data = back.Data }, back);
        Assert.Equal(m.Data, back.Data);
        var ack = new StationMenuSync.MenuAckMsg(new string('c', 64), Session, 9, StationMenuSync.StageApplied, "");
        w = new NetWriter();
        ack.Encode(w);
        r = new NetReader(w.ToArray());
        Assert.Equal(ack, StationMenuSync.MenuAckMsg.Decode(ref r));
    }

    [Fact]
    public void The_applied_queue_reads_into_events()
    {
        var list = StationMenuSync.ReadApplied("[{\"definition\":\"724d7b7b-a362-566b-b0a7-5088f219266f\",\"workshop\":2947643217,\"generation\":4,\"fromServer\":true,\"ok\":false,\"reason\":\"IconSourceMissing\"}]");
        var e = Assert.Single(list);
        Assert.Equal(2947643217u, e.Workshop);
        Assert.True(e.FromServer);
        Assert.False(e.Ok);
        Assert.Equal("IconSourceMissing", e.Reason);
        Assert.Empty(StationMenuSync.ReadApplied("not json"));
    }
}

[CollectionDefinition("kiln.network engine", DisableParallelization = true)]
public sealed class StationMenuEngineCollection;

[Collection("kiln.network engine")]
public sealed class StationMenuAdmissionTests : IDisposable
{
    private const uint P = 0xC1, Q = 0xC2;

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
    private IDisposable? _decider;
    private bool _declared = true;

    public StationMenuAdmissionTests()
    {
        NetNative.TestTransport = _net;
        NetEngine.TestServerRole = true;
        NetAdmission.ResetForTests();
        NetAdmission.TestGate = _gate;
        StationMenuStore.ResetForTests();
        StationMenuSync.ResetForTests();
        StationMenuStore.TestBackend = _store;
        StationMenuSync.TestDeclared = () => _declared;
        StationMenuSync.TestChannelOpen = true;
    }

    public void Dispose()
    {
        _decider?.Dispose();
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

    private NetPeer Up(uint id)
    {
        NetEngine.OnPeer(id, true);
        return NetEngine.ServerPeers().Single(x => x.Id == id);
    }

    private static void Hello(uint id, params (string Name, string Version)[] channels)
    {
        var hello = new Wire.Hello(Wire.Protocol, Game.KilnVersion, 7, [],
            channels.Select(c => new Wire.ChannelEntry(0, c.Name, c.Version, false)).ToList(), new Dictionary<string, string>());
        NetEngine.OnFrame(id, Wire.EncodeHello(hello));
    }

    private string? LastReject(uint id) => _net.Sent.Where(s => s.Peer == id && Wire.TryHeader(s.Frame, out var t) && t == Wire.Type.Reject)
        .Select(s => Wire.DecodeReject(s.Frame).Reason).LastOrDefault();

    private static string Record(string def = "724d7b7b-a362-566b-b0a7-5088f219266f") =>
        $"{{\"definition\":{{\"id\":\"{def}\"}},\"format\":1,\"origin\":{{\"workshopId\":1}},\"tabs\":[]}}";

    private void AMenu()
    {
        var r = StationMenuStore.Define(42, "m", Record(), Guid.Parse("724d7b7b-a362-566b-b0a7-5088f219266f"), 1);
        Assert.True(r.Ok, r.Reason);
        _decider = NetAdmission.OnJoin(StationMenuSync.Holder, StationMenuSync.Decide);
    }

    [Theory]
    [InlineData(ModSyncPolicy.Warn)]
    [InlineData(ModSyncPolicy.Off)]
    [InlineData(ModSyncPolicy.Refuse)]
    public void An_older_Kiln_client_is_refused_before_the_world_under_every_modsync(ModSyncPolicy policy)
    {
        NetEngine.UseConfigForTests(new NetworkConfig { ModSync = policy });
        AMenu();
        var p = Up(P);
        Hello(P, ("kiln:content", "1"));
        Assert.Equal(NetPeerState.Rejected, p.State);
        Assert.Equal(StationMenuSync.RefusalText, LastReject(P));
        Assert.False(NetAdmission.InWorld(p));
    }

    [Fact]
    public void Version_zero_is_refused_and_version_one_is_let_in()
    {
        AMenu();
        var p = Up(P);
        Hello(P, (StationMenuSync.ChannelName, "0"));
        Assert.Equal(NetPeerState.Rejected, p.State);
        var q = Up(Q);
        Hello(Q, (StationMenuSync.ChannelName, "1"));
        Assert.Equal(NetPeerState.Ready, q.State);
        Assert.True(NetAdmission.MayRelease(q, out _));
    }

    [Fact]
    public void Without_a_menu_nobody_is_refused_and_no_decider_is_needed()
    {
        _decider = NetAdmission.OnJoin(StationMenuSync.Holder, StationMenuSync.Decide);
        var p = Up(P);
        Hello(P, ("kiln:content", "1"));
        Assert.Equal(NetPeerState.Ready, p.State);
        Assert.Null(LastReject(P));
    }

    [Fact]
    public void A_client_without_Kiln_is_not_refused_for_a_menu()
    {
        AMenu();
        Assert.False(NetAdmission.VanillaRefused(false, out var reason), reason);
    }

    [Fact]
    public void A_ready_client_that_drops_the_channel_is_refused()
    {
        var p = Up(P);
        Hello(P, (StationMenuSync.ChannelName, "1"));
        Assert.Equal(NetPeerState.Ready, p.State);
        AMenu();
        Hello(P, ("kiln:content", "1"));  // its render went off: announced again without the channel
        Assert.Contains(p, StationMenuSync.LackingPeers());
        var tick = typeof(StationMenuSync).GetMethod("AdmissionTick", System.Reflection.BindingFlags.Static | System.Reflection.BindingFlags.NonPublic)!;
        tick.Invoke(null, null);
        Assert.Equal(NetPeerState.Rejected, p.State);
        Assert.Equal(StationMenuSync.RefusalText, LastReject(P));
    }

    [Fact]
    public void Joins_wait_while_the_menus_are_not_declared_then_are_decided()
    {
        _declared = false;
        _decider = NetAdmission.OnJoin(StationMenuSync.Holder, StationMenuSync.Decide);
        var old = Up(P);
        Hello(P, ("kiln:content", "1"));
        var fresh = Up(Q);
        Hello(Q, (StationMenuSync.ChannelName, "1"));
        Assert.False(NetAdmission.MayRelease(old, out var holder));
        Assert.Equal(StationMenuSync.Holder, holder);
        Assert.False(NetAdmission.MayRelease(fresh, out _));
        // the mods declare a menu, then the latch: the older client is refused, the new one let in
        StationMenuStore.Define(42, "m", Record(), Guid.Parse("724d7b7b-a362-566b-b0a7-5088f219266f"), 1);
        _declared = true;
        var tick = typeof(StationMenuSync).GetMethod("AdmissionTick", System.Reflection.BindingFlags.Static | System.Reflection.BindingFlags.NonPublic)!;
        tick.Invoke(null, null);
        Assert.Equal(NetPeerState.Rejected, old.State);
        Assert.True(NetAdmission.MayRelease(fresh, out _));
    }

    [Fact]
    public void A_first_menu_is_refused_while_a_connected_Kiln_client_lacks_the_channel()
    {
        var p = Up(P);
        Hello(P, ("kiln:content", "1"));
        Assert.Equal(NetPeerState.Ready, p.State);
        p.InGameSince = DateTime.UtcNow;  // in the world: it could only be kicked
        var r = StationMenuStore.Define(42, "m", Record(), Guid.NewGuid(), 1);
        Assert.False(r.Ok);
        Assert.StartsWith(StationMenuStore.PeersLack, r.Reason);
        Assert.Empty(_store.Layers);  // nothing published
        Assert.Equal(0, _store.Defines);
    }

    [Fact]
    public void Without_the_hold_gate_a_menu_is_refused_with_a_capability_reason()
    {
        _gate.Caps = NetNative.GateCanHold;  // no hold-all
        var r = StationMenuStore.Define(42, "m", Record(), Guid.NewGuid(), 1);
        Assert.False(r.Ok);
        Assert.StartsWith(StationMenuStore.NoHoldGate, r.Reason);
    }

    [Fact]
    public void Content_sync_off_refuses_publication()
    {
        var was = SyncSettings.Current.SendContent;
        SyncSettings.Current.SendContent = false;
        try
        {
            var r = StationMenuStore.Define(42, "m", Record(), Guid.NewGuid(), 1);
            Assert.False(r.Ok);
            Assert.StartsWith(StationMenuStore.ContentSyncDisabled, r.Reason);
        }
        finally { SyncSettings.Current.SendContent = was; }
    }

    [Fact]
    public void Events_follow_accepted_changes_only()
    {
        var seen = new List<StationMenuDefinitionChangedEvent>();
        using var sub = Events.On<StationMenuDefinitionChangedEvent>(e => seen.Add(e));
        var def = Guid.Parse("724d7b7b-a362-566b-b0a7-5088f219266f");
        var a = StationMenuStore.Define(42, "m", Record(), def, 1);
        var again = StationMenuStore.Define(42, "m", Record(), def, 1);  // identical: no event, same generation
        Assert.Equal(a.Generation, again.Generation);
        Assert.Equal(a.Handle, again.Handle);
        Assert.Throws<ArgumentException>(() => StationMenuStore.Define(42, "m", Record().Replace("\"tabs\"", "\"bad\""), def, 1));
        Assert.Throws<ArgumentException>(() => StationMenuStore.Remove(43, a.Handle));  // another owner
        var removed = StationMenuStore.Remove(42, a.Handle);
        Assert.True(removed.Ok);
        Assert.True(StationMenuStore.Remove(42, a.Handle).Ok);  // a second time: nothing
        Assert.Equal(2, seen.Count);
        Assert.False(seen[0].Removed);
        Assert.True(seen[1].Removed);
        Assert.Equal(def, seen[1].DefinitionId);
    }
}
