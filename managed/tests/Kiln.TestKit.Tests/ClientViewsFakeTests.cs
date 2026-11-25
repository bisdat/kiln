// SPDX-License-Identifier: MIT
// The kit's fakes of kiln.stations.client and kiln.skills.client (Fakes/StationsClient, Fakes/SkillsClient; 0.2.0-dev.31.13):
// parity with the native services: the layouts are the headers' (and Kiln.Blocks' / Kiln.Skills' private mirrors), the
// station arithmetic gives native/services/items/tests/client_stations_test.cpp's numbers, the tables answer the header's
// codes (bounded buffers, a row switched off, no copy yet); and ClientSkills, the managed face of the skill view, on it.
using System.Runtime.InteropServices;
using Xunit;

namespace Kiln.TestKit.Tests;

public sealed unsafe class ClientViewsFakeTests
{
    private const long S = 1_000_000_000L;

    private static int SizeOf(Type anchor, string name) =>
        (int)typeof(System.Runtime.CompilerServices.Unsafe).GetMethod(nameof(System.Runtime.CompilerServices.Unsafe.SizeOf))!
            .MakeGenericMethod(anchor.Assembly.GetType(name, throwOnError: true)!).Invoke(null, null)!;

    [Fact]
    public void The_layouts_are_the_headers()
    {
        // kiln_stations_client.h: view 232, status 192, table 32; kiln_skills_client.h: node 32, link 16, state 216, table 32
        Assert.Equal(232, sizeof(StationsClientAbi.View));
        Assert.Equal(192, sizeof(StationsClientAbi.Status));
        Assert.Equal(32, sizeof(StationsClientAbi.Table));
        Assert.Equal(32, sizeof(SkillsClientAbi.Node));
        Assert.Equal(16, sizeof(SkillsClientAbi.Link));
        Assert.Equal(216, sizeof(SkillsClientAbi.State));
        Assert.Equal(32, sizeof(SkillsClientAbi.Table));
        Assert.Equal(16, (int)Marshal.OffsetOf<SkillsClientAbi.Node>(nameof(SkillsClientAbi.Node.X)));
        Assert.Equal(12, (int)Marshal.OffsetOf<SkillsClientAbi.Node>(nameof(SkillsClientAbi.Node.Flags)));
        Assert.Equal(40, (int)Marshal.OffsetOf<SkillsClientAbi.State>(nameof(SkillsClientAbi.State.Step)));
        Assert.Equal(96, (int)Marshal.OffsetOf<StationsClientAbi.View>(nameof(StationsClientAbi.View.RecipeStartNs)));
        Assert.Equal(144, (int)Marshal.OffsetOf<StationsClientAbi.View>(nameof(StationsClientAbi.View.Outputs)));
        Assert.Equal(208, (int)Marshal.OffsetOf<StationsClientAbi.View>(nameof(StationsClientAbi.View.Step)));
        // Kiln.Blocks' and Kiln.Skills' own mirrors (private to them) have the same sizes as the kit's
        var blocks = typeof(ClientStations);
        Assert.Equal(232, SizeOf(blocks, "Kiln.ClientStationsNative+View"));
        Assert.Equal(192, SizeOf(blocks, "Kiln.ClientStationsNative+Status"));
        Assert.Equal(32, SizeOf(blocks, "Kiln.ClientStationsNative+Table"));
        var skills = typeof(ClientSkills);
        Assert.Equal(32, SizeOf(skills, "Kiln.ClientSkillsNative+Node"));
        Assert.Equal(16, SizeOf(skills, "Kiln.ClientSkillsNative+Link"));
        Assert.Equal(216, SizeOf(skills, "Kiln.ClientSkillsNative+State"));
        Assert.Equal(32, SizeOf(skills, "Kiln.ClientSkillsNative+Table"));
    }

    [Fact]
    public void The_station_arithmetic_is_the_native_services()
    {
        // client_stations_test.cpp's vectors, one for one
        Assert.Equal(60 * S, ClientStationMath.ScaledNs(60 * S, 1f));
        Assert.Equal(30 * S, ClientStationMath.ScaledNs(60 * S, 0.5f));
        Assert.Equal(0, ClientStationMath.ScaledNs(60 * S, 0f));
        Assert.Equal(0, ClientStationMath.ScaledNs(60 * S, -1f));
        Assert.Equal(0, ClientStationMath.ScaledNs(60 * S, float.NaN));
        Assert.Equal(0, ClientStationMath.ScaledNs(0, 1f));
        Assert.Equal(1_000_000_000L, ClientStationMath.ScaledNs(1_000_000_001L, 1f));
        const long Max = long.MaxValue, Min = long.MinValue;
        Assert.Equal(60 * S * 4 - 30 * S, ClientStationMath.GameRemaining(60 * S, 3, 100 * S, 130 * S));
        Assert.Equal(30 * S, ClientStationMath.GameRemaining(60 * S, 0, 100 * S, 130 * S));
        Assert.Equal(-40 * S, ClientStationMath.GameRemaining(60 * S, 0, 100 * S, 200 * S));
        Assert.Equal(Max, ClientStationMath.GameRemaining(Max, 2, 100 * S, 130 * S));
        Assert.Equal(Min, ClientStationMath.GameRemaining(Min, 2, 100 * S, 130 * S));
        Assert.Equal(Max, ClientStationMath.GameRemaining(Max, 0, 100 * S, 130 * S));
        Assert.Equal(Max, ClientStationMath.GameRemaining(60 * S, 1, Max, 130 * S));
        Assert.Equal(Min, ClientStationMath.GameRemaining(60 * S, 1, Min, 130 * S));
        Assert.Equal(Min, ClientStationMath.GameRemaining(60 * S, 1, 100 * S, Max));
        Assert.Equal(120 * S, ClientStationMath.GameRemaining(60 * S, 1, Max, Max));
        Assert.Equal(0, ClientStationMath.GameRemaining(Max, 1, 100 * S, Max));
        Assert.Equal(unchecked((long)((ulong)(Max / 2) * 5u)), ClientStationMath.GameRemaining(Max / 2, 4, 0, 0));
    }

    private static StationsClientAbi.Table* StationsTable => (StationsClientAbi.Table*)KitBackend.Instance.Service(StationsClientAbi.Service, 1);
    private static SkillsClientAbi.Table* SkillsTable => (SkillsClientAbi.Table*)KitBackend.Instance.Service(SkillsClientAbi.Service, 1);

    [Fact]
    public void The_station_table_answers_the_headers_codes()
    {
        using var kiln = new KilnHarness();
        kiln.Step();
        for (uint i = 1; i <= 3; i++) kiln.ClientStations.Add(new EntityId(10 + i), 1, default);
        var t = StationsTable;
        var p = (StationsClientAbi.View*)NativeMemory.AllocZeroed(4, (nuint)sizeof(StationsClientAbi.View));
        var n = (uint*)NativeMemory.AllocZeroed(1, 4);
        var st = (StationsClientAbi.Status*)NativeMemory.AllocZeroed(1, (nuint)sizeof(StationsClientAbi.Status));
        try
        {
            for (var i = 0; i < 4; i++) p[i].Size = (uint)sizeof(StationsClientAbi.View);
            Assert.Equal(FakeTables.Ok, t->Stations(p, 4, n));
            Assert.Equal(3u, *n);
            Assert.Equal(13u, p[2].Entity);
            Assert.Equal(FakeTables.BufferTooSmall, t->Stations(p, 2, n));
            Assert.Equal(3u, *n);
            Assert.Equal(FakeTables.BufferTooSmall, t->Stations(null, 0, n));
            Assert.Equal(FakeTables.InvalidArgument, t->Stations(null, 2, n));
            p[1].Size = 8;
            Assert.Equal(FakeTables.InvalidArgument, t->Stations(p, 2, n));
            p[1].Size = (uint)sizeof(StationsClientAbi.View);
            Assert.Equal(FakeTables.NotFound, t->Station(99, p));
            Assert.Equal(FakeTables.Ok, t->Station(12, p));
            Assert.Equal(12u, p[0].Entity);
            kiln.Capabilities.Override(ClientStations.Capability, false);
            Assert.Equal(FakeTables.Unavailable, t->Stations(p, 4, n));
            Assert.Equal(FakeTables.Unavailable, t->Station(12, p));
            st->Size = (uint)sizeof(StationsClientAbi.Status);
            Assert.Equal(FakeTables.Ok, t->GetStatus(st));  // status always answers
            Assert.Equal(0, st->On);
            st->Size = 4;
            Assert.Equal(FakeTables.InvalidArgument, t->GetStatus(st));
        }
        finally
        {
            NativeMemory.Free(p);
            NativeMemory.Free(n);
            NativeMemory.Free(st);
        }
    }

    private static KilnHarness Tree()
    {
        var kiln = new KilnHarness();
        kiln.Step();
        var c = kiln.ClientSkills;
        c.Total = 30;
        c.AddNode(0xffd5f940, SkillNodeType.PassiveSkill, cost: 1, uiX: 10, uiY: 20);          // double jump
        c.AddNode(0xcafe0001, SkillNodeType.ActiveSkill, cost: 2, maxLevel: 3, uiX: 120.5f, uiY: -40, classAlignment: 1);
        c.AddNode(0x7cb432ed, SkillNodeType.Attribute, cost: 1, shadow: true);                    // past the game's caps
        c.AddNode(0x00000004, SkillNodeType.Attribute, cost: 1);
        c.Link(0xffd5f940, 0xcafe0001);
        c.Link(0xcafe0001, 0x7cb432ed, bidirectional: true);
        c.Unlock(0xffd5f940);
        c.Unlock(0xcafe0001, 2);
        c.Unlock(0x7cb432ed);
        return kiln;
    }

    [Fact]
    public void A_skill_point_summary_on_a_joined_client()
    {
        using var kiln = Tree();
        var p = kiln.Run(() => ClientSkills.Points)!.Value;
        Assert.Equal(30, p.Total);
        Assert.Equal(1 + 2 * 2 + 1, p.Spent);  // the shadow node's cost counts, as the game's screen counts it with Kiln
        Assert.Equal(24, p.Available);
        Assert.Equal(3, p.UnlockedNodes);
        Assert.Equal(4, p.TreeNodes);
        Assert.Equal(new EntityId(1), p.Player);
        Assert.Equal("24 of 30 skill points left (6 spent, 3 nodes)", p.ToString());
        // more spent than there is (a respec the server has not sent yet): available never goes below 0
        kiln.ClientSkills.Total = 2;
        Assert.Equal(0, kiln.Run(() => ClientSkills.Points)!.Value.Available);
    }

    [Fact]
    public void The_tree_layout_levels_and_links()
    {
        using var kiln = Tree();
        var nodes = kiln.Run(() => ClientSkills.Nodes);
        Assert.Equal([0, 1, 2, 3], nodes.Select(n => n.Index));
        var ember = nodes[1];
        Assert.Equal(0xcafe0001u, ember.Id);
        Assert.Equal(SkillNodeType.ActiveSkill, ember.Type);
        Assert.Equal((2, 3, 2, 1), (ember.Level, ember.MaxLevel, ember.Cost, ember.ClassAlignment));
        Assert.True(ember.Unlocked);
        Assert.Equal(4, ember.Spent);
        Assert.Equal((120.5f, -40f), (ember.UiX, ember.UiY));
        Assert.Equal((1, 1), (ember.Incoming, ember.Outgoing));
        Assert.True(nodes[2].Shadow && nodes[2].Unlocked);
        Assert.False(nodes[3].Unlocked);
        Assert.Equal(0, nodes[3].Level);
        Assert.Equal([0xffd5f940u, 0xcafe0001u, 0x7cb432edu], kiln.Run(() => ClientSkills.Unlocked).Select(n => n.Id));
        var links = kiln.Run(() => ClientSkills.Links);
        Assert.Equal([new ClientSkillLink(1, 0xffd5f940, 0xcafe0001, false), new ClientSkillLink(2, 0xcafe0001, 0x7cb432ed, true)], links);
        Assert.Equal(ember, kiln.Run(() => ClientSkills.Find(0xcafe0001)));
        Assert.Null(kiln.Run(() => ClientSkills.Find(0x12345678)));
        kiln.ClientSkills.Lock(0xcafe0001);
        Assert.Equal(0, kiln.Run(() => ClientSkills.Find(0xcafe0001))!.Value.Level);
        Assert.Equal(2, kiln.Run(() => ClientSkills.Points)!.Value.Spent);
    }

    [Fact]
    public void Before_the_first_copy_there_is_nothing_to_read()
    {
        using var kiln = Tree();
        kiln.ClientSkills.Loaded = false;
        Assert.True(kiln.Run(() => ClientSkills.Available));
        Assert.Null(kiln.Run(() => ClientSkills.Points));
        Assert.Empty(kiln.Run(() => ClientSkills.Nodes));
        Assert.Empty(kiln.Run(() => ClientSkills.Links));
    }

    [Fact]
    public void The_skill_row_switches_the_view_off_cleanly()
    {
        using var kiln = Tree();
        kiln.Capabilities.Override(ClientSkills.Capability, false);
        Assert.False(kiln.Run(() => ClientSkills.Available));
        Assert.Equal(ClientSkills.Capability, Assert.Throws<KilnUnavailableException>(() => kiln.Run(() => ClientSkills.Points)).Capability);
        Assert.Throws<KilnUnavailableException>(() => kiln.Run(() => ClientSkills.Nodes));
        Assert.Throws<KilnUnavailableException>(() => kiln.Run(() => ClientSkills.Links));
        Assert.Throws<KilnUnavailableException>(() => kiln.Run(() => ClientSkills.Find(1)));
        var t = SkillsTable;
        var st = (SkillsClientAbi.State*)NativeMemory.AllocZeroed(1, (nuint)sizeof(SkillsClientAbi.State));
        st->Size = (uint)sizeof(SkillsClientAbi.State);
        Assert.Equal(FakeTables.Unavailable, t->GetState(st));
        Assert.Equal(0, st->On);
        NativeMemory.Free(st);
        kiln.Capabilities.ClearOverride(ClientSkills.Capability);
        Assert.Equal(24, kiln.Run(() => ClientSkills.Points)!.Value.Available);
    }

    [Fact]
    public void Both_fakes_have_their_coverage_rows()
    {
        var rows = Coverage.All;
        Assert.Contains(rows, r => r.Capabilities.Contains("kiln.stations.client") && r.Capabilities.Contains(ClientStations.Capability));
        Assert.Contains(rows, r => r.Capabilities.Contains("kiln.skills.client") && r.Capabilities.Contains(ClientSkills.Capability));
    }
}
