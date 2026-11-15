// SPDX-License-Identifier: MIT
// The kit's fakes of kiln.decisions and kiln.window_clock (Fakes/Decisions/) are there and answer. Their batch protocol
// is tested against the header's rules in managed/tests/Kiln.Decisions.Tests, which can name Kiln.Api's mirrors.
using Xunit;

namespace Kiln.TestKit.Tests;

public sealed unsafe class DecisionsFakeTests
{
    [Fact]
    public void TheTablesAreTheHeadersV1()
    {
        var decisions = (uint*)KitBackend.Instance.Service("kiln.decisions", 1);
        var clock = (uint*)KitBackend.Instance.Service("kiln.window_clock", 1);
        Assert.True(decisions != null && clock != null);
        // header: size (21 members: the 16 of the first table, set_death_guard, enqueue_native, and since 0.2.0-dev.29
        // prepare_kind, window_wanted, window_faulted), version
        Assert.Equal((176u, 1u), (decisions[0], decisions[1]));
        Assert.Equal((16u, 1u), (clock[0], clock[1]));
        Assert.Contains(Coverage.All, r => r.Capabilities.Contains("kiln.decisions") && r.Capabilities.Contains("kiln.window_clock") && r.Kind == CoverageKind.RecordingFake);
    }

    [Fact]
    public void TheClockFollowsTheHarnessAndNothingIsOpen()
    {
        using var kiln = new KilnHarness();
        kiln.Step(3);
        var get = ((delegate* unmanaged[Cdecl]<byte*, int>*)KitBackend.Instance.Service("kiln.window_clock", 1))[1];
        var state = stackalloc byte[48];
        Assert.Equal(FakeTables.Ok, get(state));  // any thread
        Assert.Equal((48u, 1u, 1ul, kiln.CurrentStep), (*(uint*)state, *(uint*)(state + 4), *(ulong*)(state + 8), *(ulong*)(state + 16)));
        Assert.Empty(kiln.Decisions.Outcomes);
        Assert.Equal(0, kiln.Decisions.Active);
        kiln.Decisions.RestartWorld();
        Assert.Equal(FakeTables.Ok, get(state));
        Assert.Equal(2ul, *(ulong*)(state + 8));  // the next world's epoch
        Assert.True(kiln.Capabilities.Is("kiln.decisions"));
    }
}
