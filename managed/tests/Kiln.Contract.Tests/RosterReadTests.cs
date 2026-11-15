// SPDX-License-Identifier: MIT
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using Xunit;

namespace Kiln.Contract.Tests;

/// <summary>The roster's reads (package 17c S2, chapter 17.3 decision I2) on a fake kiln.knowledge table that answers by
/// the native rules (kiln_knowledge.h "the roster"; native/services/quests/roster_core.h): an older table names each
/// read's row, bad arguments throw before any native call, the native answers map to results without ever turning a
/// world that ended or a snapshot that is not ready into an unlock state, and a native refusal is the row's exception.</summary>
public sealed unsafe class RosterReadTests : FakeTableTest
{
    private static int _calls;
    // the fake's world: per kind (0 server, 2 client) live or not, its generation, a complete snapshot or not
    private static readonly bool[] Live = new bool[3];
    private static readonly uint[] Generation = new uint[3];
    private static readonly bool[] Ready = new bool[3];
    private static bool _dedicated;              // a dedicated server: the client world's reads are off
    private static uint _forcedReadiness;        // 0: by the rules; else what npc_unlock_at answers (an odd value too)
    private static readonly Dictionary<uint, bool> Unlocked = [];

    private static bool Off(uint kind) => kind == 2 && _dedicated;

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int FakeWorld(uint kind, uint* gen, uint* readiness)
    {
        _calls++;
        if (kind is not (0 or 2) || gen == null || readiness == null) return KnowledgeNative.InvalidArgument;
        if (Off(kind)) return KnowledgeNative.Unavailable;
        *gen = 0;
        if (!Live[kind]) *readiness = 2;  // NoWorld
        else if (!Ready[kind]) *readiness = 0;  // NotReady
        else
        {
            *readiness = 1;
            *gen = Generation[kind];
        }
        return KnowledgeNative.Ok;
    }

    private static uint Resolve(uint kind, uint gen) =>
        !Live[kind] || Generation[kind] != gen ? 3u /* WorldEnded */ : !Ready[kind] ? 0u /* NotReady */ : 1u;

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int FakeBudget(uint kind, uint gen, KilnCapacityInfo* o)
    {
        _calls++;
        if (kind is not (0 or 2) || gen == 0 || o == null || o->Size < (uint)sizeof(KilnCapacityInfo)) return KnowledgeNative.InvalidArgument;
        if (Off(kind)) return KnowledgeNative.Unavailable;
        var size = o->Size;
        *o = default;
        o->Size = size;
        o->Flags = KilnCapacityInfo.FlagWorld | KilnCapacityInfo.FlagExpanded;
        o->WorldKind = kind;
        o->Generation = gen;
        o->Revision = 12;
        o->Readiness = Resolve(kind, gen);
        if (o->Readiness != 1) return KnowledgeNative.Ok;
        o->Flags |= KilnCapacityInfo.FlagReady;
        o->Capacity = 128;
        o->GameUsed = 35;
        o->KilnUsed = 93;
        return KnowledgeNative.Ok;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int FakeUnlock(uint kind, uint gen, uint id, KnowledgeNative.NpcUnlockReply* r)
    {
        _calls++;
        if (kind is not (0 or 2) || gen == 0 || id == 0 || r == null || r->Size < 40) return KnowledgeNative.InvalidArgument;
        if (Off(kind)) return KnowledgeNative.Unavailable;
        var size = r->Size;
        *r = default;
        r->Size = size;
        r->WorldKind = kind;
        r->Generation = gen;
        r->NpcId = id;
        r->Revision = 12;
        r->Readiness = _forcedReadiness != 0 ? _forcedReadiness : Resolve(kind, gen);
        if (r->Readiness == 1 && !Unlocked.ContainsKey(id)) r->Readiness = 7;  // UnknownNpc
        if (r->Readiness != 1) return KnowledgeNative.Ok;
        r->Unlocked = Unlocked[id] ? 1u : 0u;
        r->KnowledgeVersion = 0x1234;
        r->Revision = 13;
        return KnowledgeNative.Ok;
    }

    private void ServeTable(bool appended)
    {
        _calls = 0;
        _dedicated = false;
        _forcedReadiness = 0;
        Unlocked.Clear();
        Array.Clear(Live);
        Array.Clear(Generation);
        Array.Clear(Ready);
        if (appended)
        {
            var t = Alloc<KnowledgeNative.RosterTable>();
            t->V1.Header = new ServiceHeader { Size = (uint)sizeof(KnowledgeNative.RosterTable), Version = 1 };
            t->RosterWorld = &FakeWorld;
            t->RosterBudget = &FakeBudget;
            t->NpcUnlockAt = &FakeUnlock;
            Swap(typeof(KnowledgeNative), "_looked", false);
            Serve("kiln.knowledge", (KnowledgeNative.Table*)t, typeof(KnowledgeNative), "_table");
        }
        else
        {
            var t = Alloc<KnowledgeNative.Table>();
            t->Header = new ServiceHeader { Size = (uint)sizeof(KnowledgeNative.Table), Version = 1 };
            Swap(typeof(KnowledgeNative), "_looked", false);
            Serve("kiln.knowledge", t, typeof(KnowledgeNative), "_table");
        }
    }

    private static void World(uint kind, uint gen, bool ready = true)
    {
        Live[kind] = true;
        Generation[kind] = gen;
        Ready[kind] = ready;
    }

    [Fact]
    public void The_appended_table_is_the_v1_table_and_three_pointers()
    {
        Assert.Equal(240, sizeof(KnowledgeNative.Table));
        Assert.Equal(264, sizeof(KnowledgeNative.RosterTable));
        Assert.Equal(240, (int)Marshal.OffsetOf<KnowledgeNative.RosterTable>(nameof(KnowledgeNative.RosterTable.RosterWorld)));
        Assert.Equal(248, (int)Marshal.OffsetOf<KnowledgeNative.RosterTable>(nameof(KnowledgeNative.RosterTable.RosterBudget)));
        Assert.Equal(256, (int)Marshal.OffsetOf<KnowledgeNative.RosterTable>(nameof(KnowledgeNative.RosterTable.NpcUnlockAt)));
        Assert.Equal(40, sizeof(KnowledgeNative.NpcUnlockReply));
        Assert.Equal(32, (int)Marshal.OffsetOf<KnowledgeNative.NpcUnlockReply>(nameof(KnowledgeNative.NpcUnlockReply.Revision)));
    }

    [Fact]
    public void An_older_kiln_knowledge_names_each_reads_row_and_makes_no_call()
    {
        ServeTable(appended: false);
        var token = new CapacityWorldToken(CapacityWorldKind.Server, 1);
        var e = Assert.Throws<KilnUnavailableException>(() => NativeQuests.GetServerRosterWorld());
        Assert.Equal("npcs.roster_read", e.Capability);
        Assert.Contains("update Kiln", e.Message, StringComparison.Ordinal);
        Assert.Equal("npcs.client_roster_read", Assert.Throws<KilnUnavailableException>(() => NativeQuests.GetClientRosterWorld()).Capability);
        Assert.Equal("npcs.roster_read", Assert.Throws<KilnUnavailableException>(() => NativeQuests.GetUnlock(5, token)).Capability);
        Assert.Equal("npcs.roster_budget", Assert.Throws<KilnUnavailableException>(() => NativeQuests.GetRosterBudget(token)).Capability);
        Assert.Equal(0, _calls);
    }

    [Fact]
    public void A_world_is_a_token_only_when_its_snapshot_is_complete()
    {
        ServeTable(appended: true);
        var none = NativeQuests.GetServerRosterWorld();
        Assert.False(none.Ok);
        Assert.Equal("NoWorld", none.Reason);
        Assert.Null(none.World);
        World(0, 5, ready: false);
        var notReady = NativeQuests.GetServerRosterWorld();
        Assert.Equal("NotReady", notReady.Reason);
        Assert.Null(notReady.World);
        World(0, 5);
        World(2, 3);
        var server = NativeQuests.GetServerRosterWorld();
        var client = NativeQuests.GetClientRosterWorld();
        Assert.True(server.Ok);
        Assert.Null(server.Reason);
        Assert.Equal(new CapacityWorldToken(CapacityWorldKind.Server, 5), server.World);
        Assert.Equal(new CapacityWorldToken(CapacityWorldKind.Client, 3), client.World);
        Assert.NotEqual(server.World, client.World);
    }

    [Fact]
    public void Bad_arguments_throw_before_any_native_call()
    {
        ServeTable(appended: true);
        World(0, 5);
        World(2, 3);
        var server = new CapacityWorldToken(CapacityWorldKind.Server, 5);
        var client = new CapacityWorldToken(CapacityWorldKind.Client, 3);
        Assert.Throws<ArgumentException>(() => NativeQuests.GetUnlock(0, server));
        Assert.Throws<ArgumentException>(() => NativeQuests.GetUnlock(7, client));                    // the other kind
        Assert.Throws<ArgumentException>(() => NativeQuests.GetClientUnlock(7, server));
        Assert.Throws<ArgumentException>(() => NativeQuests.GetUnlock(7, new CapacityWorldToken(CapacityWorldKind.Server, 0)));
        Assert.Throws<ArgumentException>(() => NativeQuests.GetUnlock(7, new CapacityWorldToken((CapacityWorldKind)1, 5)));  // prediction
        Assert.Throws<ArgumentException>(() => NativeQuests.GetRosterBudget(new CapacityWorldToken(CapacityWorldKind.Client, 0)));
        Assert.Throws<ArgumentException>(() => NativeQuests.GetRosterBudget(new CapacityWorldToken((CapacityWorldKind)1, 5)));
        Assert.Equal(0, _calls);
    }

    [Fact]
    public void An_unlock_state_is_an_answer_only_from_a_ready_snapshot_of_the_tokens_world()
    {
        ServeTable(appended: true);
        World(0, 5);
        Unlocked[0x1000] = true;
        Unlocked[0x1040] = false;
        var token = new CapacityWorldToken(CapacityWorldKind.Server, 5);
        var on = NativeQuests.GetUnlock(0x1000, token);
        Assert.True(on.Ok);
        Assert.Null(on.Reason);
        Assert.True(on.Unlocked);
        Assert.Equal((ushort)0x1234, on.KnowledgeVersion);
        Assert.Equal(13UL, on.SnapshotRevision);
        var off = NativeQuests.GetUnlock(0x1040, token);
        Assert.True(off.Ok);
        Assert.False(off.Unlocked);
        // an id the roster does not have
        var unknown = NativeQuests.GetUnlock(0x4242, token);
        Assert.False(unknown.Ok);
        Assert.Equal("UnknownNpc", unknown.Reason);
        Assert.Null(unknown.Unlocked);
        Assert.Null(unknown.KnowledgeVersion);
        // the world stopped and another runs: the old token ends, it never reads the new world's state
        World(0, 6);
        var ended = NativeQuests.GetUnlock(0x1000, token);
        Assert.False(ended.Ok);
        Assert.Equal("WorldEnded", ended.Reason);
        Assert.Null(ended.Unlocked);
        Assert.Null(ended.KnowledgeVersion);
        Assert.Equal(token, ended.World);  // the token stays in a failed read
        // the same world without a complete snapshot
        World(0, 6, ready: false);
        var notReady = NativeQuests.GetUnlock(0x1000, new CapacityWorldToken(CapacityWorldKind.Server, 6));
        Assert.Equal("NotReady", notReady.Reason);
        Assert.Null(notReady.Unlocked);
        // a readiness the reads do not know is no answer either
        World(0, 6);
        _forcedReadiness = 9;
        var odd = NativeQuests.GetUnlock(0x1000, new CapacityWorldToken(CapacityWorldKind.Server, 6));
        Assert.False(odd.Ok);
        Assert.Null(odd.Unlocked);
    }

    [Fact]
    public void The_client_worlds_reads_on_a_dedicated_server_throw_their_row()
    {
        ServeTable(appended: true);
        _dedicated = true;
        World(0, 5);
        Assert.Equal("npcs.client_roster_read", Assert.Throws<KilnUnavailableException>(() => NativeQuests.GetClientRosterWorld()).Capability);
        var client = new CapacityWorldToken(CapacityWorldKind.Client, 1);
        Assert.Equal("npcs.client_roster_read", Assert.Throws<KilnUnavailableException>(() => NativeQuests.GetClientUnlock(5, client)).Capability);
        Assert.Equal("npcs.client_roster_read", Assert.Throws<KilnUnavailableException>(() => NativeQuests.GetRosterBudget(client)).Capability);
        Assert.True(NativeQuests.GetServerRosterWorld().Ok);  // the server world's reads are unaffected
    }

    [Fact]
    public void The_budget_maps_its_world_and_never_reports_room_for_an_ended_world()
    {
        ServeTable(appended: true);
        World(0, 5);
        var token = new CapacityWorldToken(CapacityWorldKind.Server, 5);
        var b = NativeQuests.GetRosterBudget(token);
        Assert.True(b.Ok);
        Assert.Equal(128u, b.Budget.Capacity);
        Assert.Equal(35u, b.Budget.GameUsed);
        Assert.Equal(93u, b.Budget.KilnUsed);
        Assert.Equal(0u, b.Budget.Available);
        Assert.True(b.Budget.Expanded);
        Assert.Equal(CapacityScope.World, b.Budget.Scope);
        Assert.Equal(token, b.Budget.World);
        World(0, 6);
        var ended = NativeQuests.GetRosterBudget(token);
        Assert.False(ended.Ok);
        Assert.Equal("WorldEnded", ended.Reason);
        Assert.Null(ended.Budget.Available);
        Assert.Equal(0u, ended.Budget.Capacity);
    }

    [Fact]
    public void The_reads_carry_their_side_thread_and_capability_attributes()
    {
        static MethodInfo M(string name) => typeof(NativeQuests).GetMethod(name, BindingFlags.Public | BindingFlags.Static)!;
        foreach (var (name, side, row) in new[]
                 {
                     ("GetServerRosterWorld", typeof(ServerOnlyAttribute), "npcs.roster_read"),
                     ("GetUnlock", typeof(ServerOnlyAttribute), "npcs.roster_read"),
                     ("GetClientRosterWorld", typeof(ClientOnlyAttribute), "npcs.client_roster_read"),
                     ("GetClientUnlock", typeof(ClientOnlyAttribute), "npcs.client_roster_read"),
                     ("GetRosterBudget", null, "npcs.roster_budget"),
                 })
        {
            var m = M(name);
            Assert.NotNull(m.GetCustomAttribute<AnyThreadAttribute>());
            if (side != null) Assert.NotNull(m.GetCustomAttribute(side));
            Assert.Equal(row, m.GetCustomAttribute<RequiresCapabilityAttribute>()!.Name);
        }
        Assert.True(typeof(IKilnResult).IsAssignableFrom(typeof(RosterWorldResult)));
        Assert.True(typeof(IKilnResult).IsAssignableFrom(typeof(NpcUnlockResult)));
    }
}
