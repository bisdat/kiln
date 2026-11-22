// SPDX-License-Identifier: MIT
// The kit's fake kiln.render (Fakes/Render/): Ui.World.Outline's own code on it (0.2.0-dev.30.3, GitHub issue #111). The
// requests as recorded, the rules of the next step (priority then newest, the game's highlight and the player's setting,
// an entity without a model, two colours and a shared third, durations on virtual time), Despawn, EndWorld, SetState,
// ClearOutlines and the row a test switches off.
using System.Runtime.InteropServices;
using Xunit;

namespace Kiln.TestKit.Tests;

[Collection("RenderFake")]
public sealed unsafe class RenderFakeTests
{
    private static readonly EntityId Chest = new(4100);
    private static readonly EntityId Bench = new(4200);

    [Fact]
    public void TheTableIsTheHeadersV2()
    {
        // kiln_render.h v2: the header, v1's thirteen functions and the five outline functions
        var t = (uint*)KitBackend.Instance.Service("kiln.render", 1);  // KilnServiceHeader { size, version }
        Assert.True(t != null);
        Assert.Equal((152u, 3u), (t[0], t[1]));  // v3 (0.2.0-dev.30.5): through walls, no new member
    }

    [Fact]
    public void ARequestIsRecordedAndShowsFromTheNextStep()
    {
        using var kiln = new KilnHarness();
        kiln.Step();
        Assert.True(kiln.Run(() => Ui.World.OutlinesAvailable));
        var outline = kiln.Run(() => Ui.World.Outline(Chest, new OutlineOptions
        {
            Color = Color.Green, Pulse = true, Priority = 2, Duration = TimeSpan.FromSeconds(8),
        }));
        var r = Assert.Single(kiln.Outlines.Live);
        Assert.Equal((Chest, Color.Green, true, 2, (TimeSpan?)TimeSpan.FromSeconds(8)), (r.Entity, r.Color, r.Pulse, r.Priority, r.Duration));
        Assert.Equal((OutlineState.Waiting, OutlineReason.Pending), (r.State, r.Reason));
        Assert.Equal(OutlineState.Waiting, outline.State);
        kiln.Step();
        Assert.Equal((OutlineState.Shown, OutlineReason.None), (outline.State, outline.Reason));
        Assert.Equal(4, kiln.Outlines.Showing(Chest)!.Group);
        Assert.Equal(kiln.Outlines.Of(outline), kiln.Outlines.Showing(Chest));
        outline.Dispose();
        Assert.Empty(kiln.Outlines.Live);
        Assert.Equal((OutlineState.Ended, OutlineReason.Removed), (kiln.Outlines.All[0].State, kiln.Outlines.All[0].Reason));
        Assert.Equal(OutlineState.Ended, outline.State);
    }

    [Fact]
    public void ThroughWallsIsUnavailableWithoutTheOverlay()
    {
        using var kiln = new KilnHarness();
        kiln.Step();
        Assert.False(kiln.Run(() => Ui.World.ThroughWallsAvailable));
        var e = Assert.Throws<KilnUnavailableException>(() => kiln.Run(() =>
            Ui.World.Outline(Chest, new OutlineOptions { ThroughWalls = true, Color = Color.Green })));
        Assert.Equal(Ui.World.ThroughWallsCapability, e.Capability);
        Assert.Equal(CapabilityReason.ServiceMissing, kiln.Run(() => Capabilities.Get(Ui.World.ThroughWallsCapability).Reason));
        Assert.Contains(kiln.Run(() => Capabilities.Why(Ui.World.ThroughWallsCapability)), e.Message);
        Assert.Empty(kiln.Outlines.All);
        using var plain = kiln.Run(() => Ui.World.Outline(Chest));
        Assert.False(kiln.Outlines.Of(plain)!.ThroughWalls);
    }

    [Fact]
    public void TheNativeFakeRecordsTheThroughWallsFlagWithoutModellingDrawing()
    {
        using var kiln = new KilnHarness(); kiln.Step();
        var table = (nint*)KitBackend.Instance.Service("kiln.render", 1);
        uint* options = stackalloc uint[6] { 24, Color.Green.Rgba, 0, 2, 0, 0 };
        ulong handle = 0;
        Assert.Equal(0, ((delegate* unmanaged[Cdecl]<ulong, uint, void*, ulong*, int>)table[14])(1, Chest.Value, options, &handle));
        Assert.True(Assert.Single(kiln.Outlines.All).ThroughWalls);
    }

    [Fact]
    public void ADurationRunsOutOnVirtualTime()
    {
        using var kiln = new KilnHarness();
        kiln.Step();
        var outline = kiln.Run(() => Ui.World.Outline(Chest, new OutlineOptions { Duration = TimeSpan.FromSeconds(1) }));
        kiln.Step();
        Assert.Equal(OutlineState.Shown, outline.State);
        for (var i = 0; i < 30; i++) kiln.Step();  // 40.8 ms a step
        Assert.Equal((OutlineState.Ended, OutlineReason.Expired), (outline.State, outline.Reason));
    }

    [Fact]
    public void OnOneEntityTheHighestPriorityThenTheNewestShows()
    {
        using var kiln = new KilnHarness();
        kiln.Step();
        var low = kiln.Run(() => Ui.World.Outline(Chest));
        var high = kiln.Run(() => Ui.World.Outline(Chest, new OutlineOptions { Priority = 5, Color = Color.Red }));
        var newer = kiln.Run(() => Ui.World.Outline(Chest, new OutlineOptions { Color = Color.Cyan }));
        kiln.Step();
        Assert.Equal(OutlineState.Shown, high.State);
        Assert.Equal((OutlineState.Waiting, OutlineReason.Outshone), (low.State, low.Reason));
        Assert.Equal(OutlineReason.Outshone, newer.Reason);
        high.Dispose();
        kiln.Step();
        Assert.Equal(OutlineState.Shown, newer.State);  // the same priority: the newest
        Assert.Equal(OutlineReason.Outshone, low.Reason);
        kiln.Run(Ui.World.ClearOutlines);
    }

    [Fact]
    public void TheGamesHighlightAndThePlayersSettingDecide()
    {
        using var kiln = new KilnHarness();
        kiln.Step();
        var outline = kiln.Run(() => Ui.World.Outline(Chest));
        kiln.Outlines.GameHighlight(Chest);
        kiln.Step();
        Assert.Equal((OutlineState.GameHighlight, OutlineReason.None), (outline.State, outline.Reason));
        kiln.Outlines.Setting = FakeOutlineSetting.ModsFirst;
        kiln.Step();
        Assert.Equal(OutlineState.Shown, outline.State);
        kiln.Outlines.Setting = FakeOutlineSetting.Off;
        kiln.Step();
        Assert.Equal((OutlineState.Waiting, OutlineReason.SwitchedOff), (outline.State, outline.Reason));
        kiln.Outlines.Setting = FakeOutlineSetting.GameFirst;
        kiln.Outlines.GameHighlight(Chest, false);
        kiln.Outlines.HideModel(Chest);
        kiln.Step();
        Assert.Equal((OutlineState.Waiting, OutlineReason.NoModel), (outline.State, outline.Reason));
        kiln.Outlines.HideModel(Chest, false);
        kiln.Step();
        Assert.Equal(OutlineState.Shown, outline.State);
        Assert.Equal(1, kiln.Outlines.Despawn(Chest));
        Assert.Equal((OutlineState.Ended, OutlineReason.Despawned), (outline.State, outline.Reason));
        outline.Dispose();
        Assert.Equal(OutlineReason.Despawned, outline.Reason);
    }

    [Fact]
    public void TwoColoursShowAndAThirdSharesTheNearest()
    {
        using var kiln = new KilnHarness();
        kiln.Step();
        var green = kiln.Run(() => Ui.World.Outline(new EntityId(1), new OutlineOptions { Color = new Color(0, 200, 0) }));
        var red = kiln.Run(() => Ui.World.Outline(new EntityId(2), new OutlineOptions { Color = new Color(220, 0, 0) }));
        var green2 = kiln.Run(() => Ui.World.Outline(new EntityId(3), new OutlineOptions { Color = new Color(0, 200, 0) }));
        var orange = kiln.Run(() => Ui.World.Outline(new EntityId(4), new OutlineOptions { Color = new Color(240, 60, 0) }));
        kiln.Step();
        Assert.Equal((false, false, false, true), (green.ColorShared, red.ColorShared, green2.ColorShared, orange.ColorShared));
        Assert.Equal([4, 5, 4, 5], kiln.Outlines.Live.Select(o => o.Group));
        kiln.Run(Ui.World.ClearOutlines);
        Assert.Empty(kiln.Outlines.Live);
        Assert.All(kiln.Outlines.All, o => Assert.Equal(OutlineReason.Removed, o.Reason));
        Assert.Equal(OutlineState.Ended, orange.State);
    }

    [Fact]
    public void ATestPinsAStateAndEndsTheWorld()
    {
        using var kiln = new KilnHarness();
        kiln.Step();
        var a = kiln.Run(() => Ui.World.Outline(Chest));
        var b = kiln.Run(() => Ui.World.Outline(Bench));
        kiln.Outlines.SetState(a, OutlineState.GameHighlight);
        kiln.Step();
        Assert.Equal(OutlineState.GameHighlight, a.State);  // pinned: the step keeps it
        Assert.Equal(OutlineState.Shown, b.State);
        Assert.Throws<ArgumentException>(() => kiln.Outlines.SetState(a, OutlineState.Ended));
        Assert.Throws<ArgumentException>(() => kiln.Outlines.SetState(a, OutlineState.Waiting, OutlineReason.Despawned));
        kiln.Outlines.SetState(a, OutlineState.Ended, OutlineReason.Failed);
        Assert.Equal((OutlineState.Ended, OutlineReason.Failed), (a.State, a.Reason));
        kiln.Outlines.EndWorld();
        Assert.Equal((OutlineState.Ended, OutlineReason.WorldEnded), (b.State, b.Reason));
        Assert.Empty(kiln.Outlines.Live);
    }

    [Fact]
    public void ARowSwitchedOffIsKilnUnavailable()
    {
        using var kiln = new KilnHarness();
        kiln.Step();
        kiln.Capabilities.Override(Ui.World.OutlineCapability, false);
        Assert.False(kiln.Run(() => Ui.World.OutlinesAvailable));
        var e = Assert.Throws<KilnUnavailableException>(() => kiln.Run(() => Ui.World.Outline(Chest)));
        Assert.Equal(Ui.World.OutlineCapability, e.Capability);
        kiln.Capabilities.ClearOverride(Ui.World.OutlineCapability);
        Assert.Throws<ArgumentException>(() => kiln.Run(() => Ui.World.Outline(EntityId.None)));
        Assert.Empty(kiln.Outlines.All);
    }
    // Copied unchanged from the RenderNative.OutlineInfo shipped with mod API 0.2.13, not today's managed mirror.
    [StructLayout(LayoutKind.Sequential)]
    private struct ReleasedOutlineInfo013
    {
        public uint Size, State, Reason, Entity, Group, Merged;
    }

    [Fact]
    public void The_released_managed_outline_record_works_with_only_four_byte_alignment()
    {
        using var kiln = new KilnHarness(); kiln.Step();
        var table = (nint*)KitBackend.Instance.Service("kiln.render", 1);
        uint* options = stackalloc uint[6] { 24, Color.Green.Rgba, 0, 0, 0, 0 };
        ulong handle = 0;
        Assert.Equal(0, ((delegate* unmanaged[Cdecl]<ulong, uint, void*, ulong*, int>)table[14])(1, Chest.Value, options, &handle));
        byte* storage = stackalloc byte[96];
        byte* aligned = (byte*)(((nuint)storage + 7) & ~(nuint)7);
        new Span<byte>(aligned, 80).Fill(0xA5);
        var legacy = (ReleasedOutlineInfo013*)(aligned + 4);
        legacy->Size = (uint)sizeof(ReleasedOutlineInfo013);
        Assert.Equal(24u, legacy->Size);
        Assert.Equal((nuint)4, (nuint)legacy % 8);
        Assert.Equal(0, ((delegate* unmanaged[Cdecl]<ulong, ReleasedOutlineInfo013*, int>)table[15])(handle, legacy));
        Assert.Equal((24u, Chest.Value, 0u, 1u), (legacy->Size, legacy->Entity, legacy->State, legacy->Reason));
        for (var i = 0; i < 4; i++) Assert.Equal((byte)0xA5, aligned[i]);
        for (var i = 28; i < 80; i++) Assert.Equal((byte)0xA5, aligned[i]);
    }

    [Theory]
    [InlineData(24u)]
    [InlineData(32u)]
    public void Old_outline_info_records_are_bounded_copies(uint size)
    {
        using var kiln = new KilnHarness(); kiln.Step();
        var table = (nint*)KitBackend.Instance.Service("kiln.render", 1);
        uint* options = stackalloc uint[6] { 24, Color.Green.Rgba, 0, 0, 0, 0 };
        ulong handle = 0;
        Assert.Equal(0, ((delegate* unmanaged[Cdecl]<ulong, uint, void*, ulong*, int>)table[14])(1, Chest.Value, options, &handle));
        byte* bytes = stackalloc byte[80]; new Span<byte>(bytes, 80).Fill(0xA5); *(uint*)bytes = size;
        Assert.Equal(0, ((delegate* unmanaged[Cdecl]<ulong, void*, int>)table[15])(handle, bytes));
        Assert.Equal(24u, *(uint*)bytes);
        for (var i = 24; i < 80; i++) Assert.Equal((byte)0xA5, bytes[i]);
    }
}
