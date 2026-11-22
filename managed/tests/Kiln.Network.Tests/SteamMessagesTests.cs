// SPDX-License-Identifier: MIT
// Kiln.Network's Steam peer messages (SteamMessages.cs) against a fake kiln.net v1 table of 184 bytes: the binding's
// layout is kiln_net.h's byte for byte, the lease passes what the header asks for (the mod's owner on every lease member),
// kiln.net's answers become the documented results, the checks that throw run before any call (the "steamMessages"
// opt-in too), and a lease closes with its mod.
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using Xunit;

namespace Kiln.Network.Tests;

[Collection(nameof(SteamGuardRelayTests))]  // Backend.Current and NetNative's table: never at once with the engine tests
public sealed unsafe class SteamMessagesTests : IDisposable
{
    private readonly IKilnBackend _previous = Backend.Current;
    private readonly FakeBackend _backend = new();
    private readonly ModContext _mod;
    private readonly ModContext? _outerMod = ModContext.CurrentMod;

    public SteamMessagesTests()
    {
        Backend.Current = _backend;
        FakeSteamTable.Reset();
        _backend.Tables["kiln.net"] = FakeSteamTable.Create(184);
        _mod = HostAccess.CreateContext(new ModInfo("steam-test", "steam-test", "1.0.0", Path.GetTempPath()) { SteamMessages = true }, 5151, Path.GetTempPath());
    }

    public void Dispose()
    {
        _mod.Retire();  // closes the test's open leases (and keeps Kiln.Network's list of them clean)
        ModContext.CurrentMod = _outerMod;
        Backend.Current = _previous;
        // NetNative keeps the first kiln.net table it finds: the engine tests after this one must not see the fake
        var field = typeof(NetNative).GetField("_table", BindingFlags.NonPublic | BindingFlags.Static)!;
        field.SetValue(null, Pointer.Box(null, field.FieldType));
    }

    private void Enter() => ModContext.CurrentMod = _mod;

    private SteamMessageLease OpenLease(ushort first = 100, ushort last = 103, bool sessions = false)
    {
        Enter();
        var r = SteamMessages.Open(first, last, sessions);
        Assert.True(r.Ok, r.Reason);
        return r.Lease!;
    }

    private static int Offset<T>(T* start, void* field) where T : unmanaged => (int)((byte*)field - (byte*)start);

    [Fact]
    public void The_binding_is_kiln_net_h_byte_for_byte()
    {
        Assert.Equal(48, sizeof(SteamNative.Status));
        Assert.Equal(16, sizeof(SteamNative.Open));
        Assert.Equal(24, sizeof(SteamNative.AcceptPolicy));
        Assert.Equal(24, sizeof(SteamNative.MessageInfo));
        Assert.Equal(184, sizeof(SteamNative.Table));
        var s = default(SteamNative.Status);
        Assert.Equal(8, Offset(&s, &s.LocalSteamId));
        Assert.Equal(32, Offset(&s, &s.AcceptMode));
        Assert.Equal(36, Offset(&s, &s.Listed));
        Assert.Equal(40, Offset(&s, &s.Reserved));
        var o = default(SteamNative.Open);
        Assert.Equal(8, Offset(&o, &o.FirstChannel));
        Assert.Equal(12, Offset(&o, &o.LastChannel));
        var p = default(SteamNative.AcceptPolicy);
        Assert.Equal(8, Offset(&p, &p.Ids));
        Assert.Equal(16, Offset(&p, &p.Count));
        var m = default(SteamNative.MessageInfo);
        Assert.Equal(8, Offset(&m, &m.PeerSteamId));
        Assert.Equal(16, Offset(&m, &m.Channel));
        Assert.Equal(20, Offset(&m, &m.Flags));
        var t = default(SteamNative.Table);
        Assert.Equal(120, Offset(&t, &t.LabSelftest));
        Assert.Equal(128, Offset(&t, &t.SteamStatus));
        Assert.Equal(136, Offset(&t, &t.SteamOpen));
        Assert.Equal(144, Offset(&t, &t.SteamClose));
        Assert.Equal(152, Offset(&t, &t.SteamMaintain));
        Assert.Equal(160, Offset(&t, &t.SteamSend));
        Assert.Equal(168, Offset(&t, &t.SteamReceive));
        Assert.Equal(176, Offset(&t, &t.SteamAccept));
        // the header's values
        Assert.Equal(new uint[] { 0, 1, 2, 4, 8, 16, 32 }, Enum.GetValues<SteamMessagesFlags>().Select(v => (uint)v).Order());
        Assert.Equal(new uint[] { 0, 8, 32 }, Enum.GetValues<SteamSendFlags>().Select(v => (uint)v).Order());
        Assert.Equal(new uint[] { 0, 1, 2 }, Enum.GetValues<SteamAcceptMode>().Select(v => (uint)v));
        Assert.Equal(524288, SteamMessages.MaxMessageBytes);
    }

    [Fact]
    public void Open_passes_the_owner_and_the_range_and_Dispose_closes_once()
    {
        var lease = OpenLease(100, 103, sessions: true);
        Assert.Equal(5151ul, FakeSteamTable.LastOpenOwner);
        Assert.Equal((16u, 1u, 100u, 103u), (FakeSteamTable.LastOpen.Size, FakeSteamTable.LastOpen.Flags, FakeSteamTable.LastOpen.FirstChannel, FakeSteamTable.LastOpen.LastChannel));
        Assert.True(lease.IsOpen);
        Assert.Equal(((ushort)100, (ushort)103, true), (lease.FirstChannel, lease.LastChannel, lease.AnswersSessions));
        Assert.Equal("Steam lease 100..103 (sessions)", lease.ToString());
        lease.Dispose();
        lease.Dispose();
        Assert.False(lease.IsOpen);
        Assert.Equal(1, FakeSteamTable.Closes);
        Assert.Equal(FakeSteamTable.Handle, FakeSteamTable.LastClosed);
        var sent = lease.Send(7, 100, [1, 2, 3]);
        Assert.Equal(SteamMessagesRefusal.LeaseClosed, sent.Refusal);
        Assert.Equal(0, FakeSteamTable.Sends);
        Assert.EndsWith("(closed)", lease.ToString());
    }

    [Fact]
    public void A_lease_closes_when_its_mod_unloads()
    {
        var lease = OpenLease();
        _mod.Retire();
        Assert.False(lease.IsOpen);
        Assert.Equal(1, FakeSteamTable.Closes);
    }

    [Fact]
    public void Open_refusals_are_results()
    {
        Enter();
        FakeSteamTable.OpenRc = 3;
        var notPermitted = SteamMessages.Open(1, 1);
        Assert.Equal(SteamMessagesRefusal.NotPermitted, notPermitted.Refusal);
        Assert.Contains("steam-test", notPermitted.Reason);
        FakeSteamTable.StatusFlags = 32;  // closed for exit
        Assert.Equal(SteamMessagesRefusal.ClosedForExit, SteamMessages.Open(1, 1).Refusal);
        FakeSteamTable.StatusFlags = 1;
        FakeSteamTable.OpenRc = 7;
        Assert.Equal(SteamMessagesRefusal.Fault, SteamMessages.Open(1, 1).Refusal);
        Assert.Null(SteamMessages.Open(1, 1).Lease);
    }

    [Fact]
    public void An_overlap_with_another_mods_lease_is_Taken_and_names_it()
    {
        var lease = OpenLease(100, 110);
        FakeSteamTable.OpenRc = 1;
        var r = SteamMessages.Open(105, 120);
        Assert.False(r.Ok);
        Assert.Equal(SteamMessagesRefusal.Taken, r.Refusal);
        Assert.Contains("steam-test", r.Reason);
        Assert.Contains("100..110", r.Reason);
        lease.Dispose();
        var other = SteamMessages.Open(105, 120);
        Assert.Equal(SteamMessagesRefusal.Taken, other.Refusal);
        Assert.Contains("another part of Kiln", other.Reason);
    }

    [Fact]
    public void Open_checks_before_calling()
    {
        Enter();
        Assert.Throws<ArgumentException>(() => SteamMessages.Open(5, 4));
        ModContext.CurrentMod = null;
        Assert.Throws<InvalidOperationException>(() => SteamMessages.Open(1, 2));
        Assert.Equal(0, FakeSteamTable.Opens);
    }

    [Fact]
    public void Open_is_refused_in_OnBoot_before_anything_is_called()
    {
        // OnBoot runs a transient instance under an owner of its own, which kiln.net never admits: Open says so plainly
        Enter();
        BootFrame.Current = new BootContext("steam-test", 1, BootRoles.DedicatedServer, new UnavailableGameData());
        try
        {
            var e = Assert.Throws<InvalidOperationException>(() => SteamMessages.Open(1, 2));
            Assert.Contains("SteamMessages.Open is not available in OnBoot", e.Message);
        }
        finally
        {
            BootFrame.Current = null;
        }
        Assert.Equal(0, FakeSteamTable.Opens);
    }

    [Fact]
    public void A_mod_without_the_steamMessages_opt_in_cannot_open_and_nothing_is_called()
    {
        var plain = HostAccess.CreateContext(new ModInfo("plain", "plain", "1.0.0", Path.GetTempPath()), 6161, Path.GetTempPath());
        try
        {
            ModContext.CurrentMod = plain;
            var e = Assert.Throws<InvalidOperationException>(() => SteamMessages.Open(1, 2));
            Assert.Contains("'plain'", e.Message);
            Assert.Contains("\"steamMessages\": true", e.Message);
            Assert.Equal(0, FakeSteamTable.Opens);
        }
        finally
        {
            plain.Retire();
        }
    }

    [Fact]
    public void Every_lease_member_passes_the_mods_owner()
    {
        var lease = OpenLease(100, 103, sessions: true);
        Assert.True(lease.Send(7, 100, [1]).Ok);
        Assert.Equal(SteamMessagesRefusal.NoMessage, lease.Receive(100, new byte[16]).Refusal);
        Assert.True(lease.Accept(7).Ok);
        lease.SetAcceptPolicy(SteamAcceptMode.Any);
        lease.Dispose();
        Assert.Equal(new[] { "send", "receive", "accept", "maintain", "close" }, FakeSteamTable.LeaseOwners.Select(c => c.Member));
        Assert.All(FakeSteamTable.LeaseOwners, c => Assert.Equal(5151ul, c.Owner));
    }

    [Fact]
    public void A_revoked_owner_reads_as_a_closed_lease_and_an_unloading_mod()
    {
        var lease = OpenLease(100, 103, sessions: true);
        FakeSteamTable.SendRc = SteamNative.Revoked;
        Assert.Equal(SteamMessagesRefusal.LeaseClosed, lease.Send(7, 100, [1]).Refusal);
        FakeSteamTable.ReceiveRc = SteamNative.Revoked;
        Assert.Equal(SteamMessagesRefusal.LeaseClosed, lease.Receive(100, new byte[4]).Refusal);
        FakeSteamTable.AcceptRc = SteamNative.Revoked;
        Assert.Equal(SteamMessagesRefusal.LeaseClosed, lease.Accept(7).Refusal);
        FakeSteamTable.MaintainRc = SteamNative.Revoked;
        Assert.Equal(SteamMessagesRefusal.LeaseClosed, lease.SetAcceptPolicy(SteamAcceptMode.Any).Refusal);
        FakeSteamTable.OpenRc = SteamNative.Revoked;
        var open = SteamMessages.Open(200, 201);
        Assert.Equal(SteamMessagesRefusal.NotPermitted, open.Refusal);
        Assert.Contains("unloading", open.Reason);
    }

    [Fact]
    public void An_unavailable_row_throws_naming_it_and_a_pending_row_does_not()
    {
        Enter();
        Capabilities.Report(SteamMessages.Capability, CapabilityState.Unavailable, CapabilityReason.Policy, "not started in safe idle");
        var e = Assert.Throws<KilnUnavailableException>(() => SteamMessages.Open(1, 2));
        Assert.Equal(SteamMessages.Capability, e.Capability);
        Assert.Contains("safe idle", e.Message);
        Assert.False(SteamMessages.Available);

        Capabilities.Report(SteamMessages.Capability, CapabilityState.Pending, CapabilityReason.Session, "no call has found Steam ready yet");
        FakeSteamTable.StatusFlags = 0;  // not ready
        Assert.False(SteamMessages.Available);
        FakeSteamTable.StatusFlags = 1;
        Assert.True(SteamMessages.Available);  // asked kiln.net: Steam is ready now
        Assert.True(SteamMessages.Open(1, 2).Ok);
    }

    [Fact]
    public void An_older_kiln_net_has_no_Steam_messages_and_its_row_says_why()
    {
        var older = new FakeBackend();
        older.Tables["kiln.net"] = FakeSteamTable.Create(128);  // main's table before 0.2.0-dev.30.3
        Backend.Current = older;
        Enter();
        Assert.False(SteamMessages.Available);
        Assert.False(SteamMessages.Status.Answered);
        var e = Assert.Throws<KilnUnavailableException>(() => SteamMessages.Open(1, 2));
        Assert.Contains("a Kiln before 0.2.0-dev.30.3", e.Message);
        var row = Capabilities.Get(SteamMessages.Capability);
        Assert.Equal(CapabilityReason.ServiceOlder, row.Reason);
        Assert.Contains("128-byte table", row.ReasonText);
    }

    [Fact]
    public void Without_kiln_net_status_is_not_answered_and_Open_throws()
    {
        Backend.Current = new FakeBackend();
        Enter();
        var s = SteamMessages.Status;
        Assert.False(s.Answered);
        Assert.Equal("kiln.net is not here", s.Reason);
        Assert.Throws<KilnUnavailableException>(() => SteamMessages.Open(1, 2));
    }

    [Fact]
    public void Status_maps_every_field()
    {
        FakeSteamTable.StatusFlags = 1 | 4 | 8 | 16;
        var s = SteamMessages.Status;
        Assert.True(s.Answered);
        Assert.Null(s.Reason);
        Assert.True(s.SteamReady);
        Assert.False(s.ClosedForExit);
        Assert.Equal(SteamMessagesFlags.SteamReady | SteamMessagesFlags.HasIdentity | SteamMessagesFlags.SessionsAnswered | SteamMessagesFlags.SessionLeaseOpen, s.Flags);
        Assert.Equal(76561198000000001ul, s.LocalSteamId);
        Assert.Equal((5L, 2L, SteamAcceptMode.Listed, 3), (s.SessionsAccepted, s.SessionsRefused, s.AcceptMode, s.ListedIds));
        FakeSteamTable.StatusRc = 3;
        FakeSteamTable.StatusFlags = 32;
        var exiting = SteamMessages.Status;
        Assert.True(exiting.ClosedForExit);
        Assert.Equal("the game is exiting", exiting.Reason);
    }

    [Fact]
    public void Send_passes_the_message_and_maps_Steams_answer()
    {
        var lease = OpenLease();
        var ok = lease.Send(76561198000000002ul, 101, [9, 8, 7]);
        Assert.True(ok.Ok, ok.Reason);
        Assert.Equal(1, ok.SteamResult);
        Assert.Equal((FakeSteamTable.Handle, 76561198000000002ul, 101u, 8u), (FakeSteamTable.LastSendLease, FakeSteamTable.LastSendPeer, FakeSteamTable.LastSendChannel, FakeSteamTable.LastSendFlags));
        Assert.Equal(new byte[] { 9, 8, 7 }, FakeSteamTable.LastSendData);

        Assert.True(lease.Send(5, 100, [], SteamSendFlags.Unreliable | SteamSendFlags.RestartBrokenSession).Ok);
        Assert.Equal(32u, FakeSteamTable.LastSendFlags);
        Assert.Empty(FakeSteamTable.LastSendData!);

        FakeSteamTable.SendEResult = 3;  // k_EResultNoConnection
        var refused = lease.Send(5, 100, [1]);
        Assert.Equal((false, SteamMessagesRefusal.SteamRefused, 3), (refused.Ok, refused.Refusal, refused.SteamResult));
        FakeSteamTable.SendRc = 3;
        FakeSteamTable.StatusFlags = 0;
        Assert.Equal(SteamMessagesRefusal.SteamNotReady, lease.Send(5, 100, [1]).Refusal);
        FakeSteamTable.StatusFlags = 32;
        FakeSteamTable.StatusRc = 3;
        Assert.Equal(SteamMessagesRefusal.ClosedForExit, lease.Send(5, 100, [1]).Refusal);
        FakeSteamTable.SendRc = 2;
        Assert.Equal(SteamMessagesRefusal.LeaseClosed, lease.Send(5, 100, [1]).Refusal);
        FakeSteamTable.SendRc = 7;
        Assert.Equal(SteamMessagesRefusal.Fault, lease.Send(5, 100, [1]).Refusal);
    }

    [Fact]
    public void Send_checks_before_calling()
    {
        var lease = OpenLease(100, 103);
        Assert.Throws<ArgumentException>(() => lease.Send(0, 100, [1]));
        Assert.Throws<ArgumentOutOfRangeException>(() => lease.Send(5, 99, [1]));
        Assert.Throws<ArgumentOutOfRangeException>(() => lease.Send(5, 104, [1]));
        Assert.Throws<ArgumentOutOfRangeException>(() => lease.Send(5, 100, new byte[SteamMessages.MaxMessageBytes + 1]));
        Assert.Throws<ArgumentException>(() => lease.Send(5, 100, [1], (SteamSendFlags)1));
        Assert.Equal(0, FakeSteamTable.Sends);
        Assert.True(lease.Send(5, 103, new byte[SteamMessages.MaxMessageBytes]).Ok);
    }

    [Fact]
    public void Receive_copies_one_message_and_maps_the_answers()
    {
        var lease = OpenLease();
        var buffer = new byte[64];
        Assert.Equal(SteamMessagesRefusal.NoMessage, lease.Receive(102, buffer).Refusal);
        Assert.Equal((102u, 64u), (FakeSteamTable.LastReceiveChannel, FakeSteamTable.LastReceiveCap));

        FakeSteamTable.Incoming = (new byte[] { 4, 5, 6, 7 }, 76561198000000003ul, 8u);
        var r = lease.Receive(102, buffer);
        Assert.True(r.Ok, r.Reason);
        Assert.Equal(new SteamMessageInfo(76561198000000003ul, 102, 4, true), r.Message);
        Assert.Equal(new byte[] { 4, 5, 6, 7 }, buffer[..r.Message.Length]);

        FakeSteamTable.Incoming = (new byte[] { 1 }, 9ul, 0u);
        Assert.False(lease.Receive(102, buffer).Message.Reliable);

        FakeSteamTable.Incoming = (new byte[65], 9ul, 0u);  // bigger than the buffer: kiln.net drops it
        var dropped = lease.Receive(102, buffer);
        Assert.Equal(SteamMessagesRefusal.Dropped, dropped.Refusal);
        Assert.False(dropped.Ok);
        FakeSteamTable.ReceiveRc = 7;
        Assert.Equal(SteamMessagesRefusal.Fault, lease.Receive(102, buffer).Refusal);
        Assert.Throws<ArgumentOutOfRangeException>(() => lease.Receive(104, buffer));
    }

    [Fact]
    public void Accept_maps_Steams_answer()
    {
        var lease = OpenLease();
        Assert.True(lease.Accept(42).Ok);
        Assert.Equal((FakeSteamTable.Handle, 42ul), (FakeSteamTable.LastAcceptLease, FakeSteamTable.LastAcceptPeer));
        FakeSteamTable.Accepted = 0;
        Assert.Equal(SteamMessagesRefusal.SteamRefused, lease.Accept(42).Refusal);
        Assert.Throws<ArgumentException>(() => lease.Accept(0));
    }

    [Fact]
    public void The_accept_policy_is_copied_and_in_force_only_when_Steam_is_ready()
    {
        var lease = OpenLease(sessions: true);
        var listed = lease.SetAcceptPolicy(SteamAcceptMode.Listed, [11, 22, 33]);
        Assert.True(listed.Ok, listed.Reason);
        Assert.Equal((24u, 2u, 3u), (FakeSteamTable.LastPolicy.Size, FakeSteamTable.LastPolicy.Mode, FakeSteamTable.LastPolicy.Count));
        Assert.Equal(new ulong[] { 11, 22, 33 }, FakeSteamTable.LastPolicyIds);
        Assert.True(listed.Status.SteamReady);

        FakeSteamTable.StatusFlags = 0;  // Steam not ready: only None is in force
        var any = lease.SetAcceptPolicy(SteamAcceptMode.Any);
        Assert.Equal((false, SteamMessagesRefusal.SteamNotReady), (any.Ok, any.Refusal));
        var none = lease.SetAcceptPolicy(SteamAcceptMode.None);
        Assert.True(none.Ok);
        Assert.Equal(0u, FakeSteamTable.LastPolicy.Count);
        Assert.Empty(FakeSteamTable.LastPolicyIds!);

        FakeSteamTable.MaintainRc = 7;
        Assert.Equal(SteamMessagesRefusal.Fault, lease.SetAcceptPolicy(SteamAcceptMode.None).Refusal);
        FakeSteamTable.MaintainRc = 2;
        Assert.Equal(SteamMessagesRefusal.LeaseClosed, lease.SetAcceptPolicy(SteamAcceptMode.None).Refusal);
    }

    [Fact]
    public void The_accept_policy_checks_before_calling()
    {
        var plain = OpenLease(1, 1);
        Assert.Throws<InvalidOperationException>(() => plain.SetAcceptPolicy(SteamAcceptMode.Any));
        var lease = OpenLease(2, 2, sessions: true);
        Assert.Throws<ArgumentException>(() => lease.SetAcceptPolicy(SteamAcceptMode.Any, [5]));
        Assert.Throws<ArgumentException>(() => lease.SetAcceptPolicy(SteamAcceptMode.Listed, [5, 0]));
        Assert.Throws<ArgumentOutOfRangeException>(() => lease.SetAcceptPolicy(SteamAcceptMode.Listed,
            Enumerable.Range(1, SteamMessages.MaxListedIds + 1).Select(i => (ulong)i).ToArray()));
        Assert.Throws<ArgumentOutOfRangeException>(() => lease.SetAcceptPolicy((SteamAcceptMode)3));
        Assert.Equal(0, FakeSteamTable.Maintains);
        Assert.True(lease.SetAcceptPolicy(SteamAcceptMode.Listed, Enumerable.Range(1, SteamMessages.MaxListedIds).Select(i => (ulong)i).ToArray()).Ok);
    }

    [Fact]
    public void The_surface_declares_its_thread()
    {
        Assert.True(Attribute.IsDefined(typeof(SteamMessages), typeof(AnyThreadAttribute)));
        Assert.True(Attribute.IsDefined(typeof(SteamMessageLease), typeof(AnyThreadAttribute)));
        Assert.False(Attribute.IsDefined(typeof(SteamMessages), typeof(ClientOnlyAttribute)));  // both sides
        Assert.False(Attribute.IsDefined(typeof(SteamMessages), typeof(ServerOnlyAttribute)));
    }

    internal sealed class FakeBackend : IKilnBackend
    {
        public readonly Dictionary<string, nint> Tables = new(StringComparer.Ordinal);
        public string Version => "test";
        public bool IsTickThread => true;
        public bool HasFeature(string name) => false;
        public void Log(ulong owner, LogLevel level, string text) { }
        public nint Service(string name, uint minVersion) => Tables.GetValueOrDefault(name);
        public ulong OwnerCreate(string id) => 1;
    }
}

/// <summary>A fake kiln.net v1 table (kiln_net.h): the frame transport's members answer unavailable, the Steam members
/// (when it is 184 bytes) answer from the test's script. Its memory lives for the process.</summary>
internal static unsafe class FakeSteamTable
{
    public const ulong Handle = 0x5745_0000_0000_0103;
    public static int StatusRc, OpenRc, SendRc, ReceiveRc, AcceptRc, MaintainRc;
    public static int Opens, Closes, Sends, Maintains;
    public static uint StatusFlags, Accepted;
    public static int SendEResult;
    public static ulong LastOpenOwner, LastClosed, LastSendLease, LastSendPeer, LastAcceptLease, LastAcceptPeer;
    public static List<(string Member, ulong Owner)> LeaseOwners = [];  // the owner each lease member was given
    public static uint LastSendChannel, LastSendFlags, LastReceiveChannel, LastReceiveCap;
    public static byte[]? LastSendData;
    public static ulong[]? LastPolicyIds;
    public static SteamNative.Open LastOpen;
    public static SteamNative.AcceptPolicy LastPolicy;
    public static (byte[] Data, ulong Peer, uint Flags)? Incoming;

    public static void Reset()
    {
        StatusRc = OpenRc = SendRc = ReceiveRc = AcceptRc = MaintainRc = 0;
        Opens = Closes = Sends = Maintains = 0;
        StatusFlags = 1;  // Steam ready
        Accepted = 1;
        SendEResult = 1;
        LastOpenOwner = LastClosed = LastSendLease = LastSendPeer = LastAcceptLease = LastAcceptPeer = 0;
        LeaseOwners = [];
        LastSendChannel = LastSendFlags = LastReceiveChannel = LastReceiveCap = 0;
        LastSendData = null;
        LastPolicyIds = null;
        LastOpen = default;
        LastPolicy = default;
        Incoming = null;
    }

    public static nint Create(uint size)
    {
        var t = (SteamNative.Table*)NativeMemory.AllocZeroed((nuint)sizeof(SteamNative.Table));
        t->Header.Size = size;
        t->Header.Version = 1;
        // the frame transport's members, as a kiln.net that has no transport running answers
        delegate* unmanaged[Cdecl]<void*, int> status = &NetStatus;
        t->NetStatus = (nint)status;
        if (size >= 184)
        {
            t->SteamStatus = &SteamStatus;
            t->SteamOpen = &SteamOpen;
            t->SteamClose = &SteamClose;
            t->SteamMaintain = &SteamMaintain;
            t->SteamSend = &SteamSend;
            t->SteamReceive = &SteamReceive;
            t->SteamAccept = &SteamAccept;
        }
        return (nint)t;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int NetStatus(void* status) => 3;

    private static void Fill(SteamNative.Status* s)
    {
        s->Flags = StatusFlags;
        s->LocalSteamId = (StatusFlags & 4) != 0 ? 76561198000000001ul : 0;
        s->SessionsAccepted = 5;
        s->SessionsRefused = 2;
        s->AcceptMode = 2;
        s->Listed = 3;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int SteamStatus(SteamNative.Status* s)
    {
        if (s->Size < 48) return 1;
        Fill(s);
        return StatusRc;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int SteamOpen(ulong owner, SteamNative.Open* request, ulong* lease)
    {
        Opens++;
        LastOpenOwner = owner;
        LastOpen = *request;
        *lease = OpenRc == 0 ? Handle : 0;
        return OpenRc;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int SteamClose(ulong owner, ulong lease)
    {
        LeaseOwners.Add(("close", owner));
        Closes++;
        LastClosed = lease;
        return 0;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int SteamMaintain(ulong owner, ulong lease, SteamNative.AcceptPolicy* policy, SteamNative.Status* s)
    {
        LeaseOwners.Add(("maintain", owner));
        Maintains++;
        LastPolicy = *policy;
        LastPolicyIds = new ReadOnlySpan<ulong>(policy->Ids, (int)policy->Count).ToArray();
        if (s != null) Fill(s);
        return MaintainRc;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int SteamSend(ulong owner, ulong lease, ulong peer, void* data, uint len, uint channel, uint flags, int* steamResult)
    {
        LeaseOwners.Add(("send", owner));
        Sends++;
        LastSendLease = lease;
        LastSendPeer = peer;
        LastSendData = new ReadOnlySpan<byte>(data, (int)len).ToArray();
        LastSendChannel = channel;
        LastSendFlags = flags;
        *steamResult = SendRc == 0 ? SendEResult : 0;
        return SendRc;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int SteamReceive(ulong owner, ulong lease, uint channel, void* buffer, uint cap, SteamNative.MessageInfo* info, uint* got)
    {
        LeaseOwners.Add(("receive", owner));
        LastReceiveChannel = channel;
        LastReceiveCap = cap;
        *got = 0;
        if (ReceiveRc != 0) return ReceiveRc;
        if (Incoming is not { } m) return 0;
        Incoming = null;
        if (m.Data.Length > cap) return SteamNative.ErrInvalidData;
        m.Data.CopyTo(new Span<byte>(buffer, (int)cap));
        info->Length = (uint)m.Data.Length;
        info->PeerSteamId = m.Peer;
        info->Channel = channel;
        info->Flags = m.Flags;
        *got = 1;
        return 0;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int SteamAccept(ulong owner, ulong lease, ulong peer, uint* accepted)
    {
        LeaseOwners.Add(("accept", owner));
        LastAcceptLease = lease;
        LastAcceptPeer = peer;
        *accepted = AcceptRc == 0 ? Accepted : 0;
        return AcceptRc;
    }
}
