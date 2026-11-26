// SPDX-License-Identifier: MIT
// ClientStations (0.2.0-dev.31.13, mod API 0.2.22) on the kit's kiln.stations.client: a joined client's read-only station
// view. Times as the game's HUD computes them (only while Running), the client's speed factor, finished outputs only with
// the station's inventory, a recipe the client does not know, Near's filter and order, Read and its null, the row that
// switches off, and the raw table's conversion. The native rules: native/services/items/tests/client_stations_test.cpp.
using Kiln;
using Kiln.TestKit;
using Xunit;

namespace Kiln.StationsTests;

public sealed unsafe class ClientStationsTests
{
    private const uint Forge = 0x56d94f8u, Ingots = 0x1234abcdu;
    private static readonly ItemId Bar = new(501), Ore = new(601), Coal = new(602);

    private static (KilnHarness Kiln, FakeClientStation Forge) Joined()
    {
        var kiln = new KilnHarness();
        kiln.Step();
        kiln.ClientStations.SetRecipe(Ingots, TimeSpan.FromSeconds(60), [Bar], [Ore, Coal]);
        var f = kiln.ClientStations.Add(new EntityId(4242), Forge, new WorldPosition(1024.3, 51.5, 2048), new BaseId(Guid.Parse("a0a1a2a3-a4a5-a6a7-a8a9-aaabacadaeaf")));
        f.Run(Ingots, craftsLoaded: 2, started: TimeSpan.FromSeconds(100));
        f.Inventory = [new StationStack(Bar, 2), new StationStack(Ore, 4), new StationStack(Bar, 3)];
        f.Choices.Add(new CategoryChoice(0x77, 2));
        f.Water = 128;
        f.WaterMax = 640;
        kiln.ClientStations.Clock = TimeSpan.FromSeconds(147);
        return (kiln, f);
    }

    [Fact]
    public void A_running_station_finishes_in_the_time_the_games_HUD_shows()
    {
        var (kiln, _) = Joined();
        using (kiln)
        {
            var s = kiln.Run(() => ClientStations.Read(new EntityId(4242)))!;
            Assert.Equal(StationState.Running, s.State);
            Assert.True(s.IsRunning);
            Assert.Equal(Forge, s.Workshop);
            Assert.Equal(Ingots, s.SelectedRecipe);
            Assert.Equal(2, s.CraftsLoaded);
            Assert.Equal(TimeSpan.FromSeconds(60), s.CraftDuration);
            Assert.Equal(TimeSpan.FromSeconds(13), s.CraftRemaining);           // 60 - 47 s
            Assert.Equal(TimeSpan.FromSeconds(133), s.Remaining);               // 60 x 3 - 47 s: "finishes in 2:13"
            Assert.Equal($"{(int)s.Remaining!.Value.TotalMinutes}:{s.Remaining.Value.Seconds:00}", "2:13");
            Assert.Equal(47f / 60f, s.Progress!.Value, 4);
            Assert.Equal([new StationStack(Bar, 5)], s.Outputs!);  // the ore is an input, not an output
            Assert.Equal([new CategoryChoice(0x77, 2)], s.SelectedChoices);
            Assert.Equal(128, s.Water);
            Assert.Equal(640, s.WaterMax);
            Assert.Equal(new WorldPosition(1024.25, 51.5, 2048), s.Position);  // the cache's 0.25 m steps
            Assert.Equal(Guid.Parse("a0a1a2a3-a4a5-a6a7-a8a9-aaabacadaeaf"), s.Base.Value);
        }
    }

    [Fact]
    public void Overdue_times_floor_at_zero_and_the_speed_factor_scales_the_craft()
    {
        var (kiln, _) = Joined();
        using (kiln)
        {
            kiln.ClientStations.Clock = TimeSpan.FromSeconds(1000);
            var s = kiln.Run(() => ClientStations.Read(new EntityId(4242)))!;
            Assert.Equal(TimeSpan.Zero, s.CraftRemaining);
            Assert.Equal(TimeSpan.Zero, s.Remaining);
            Assert.Equal(1f, s.Progress);
            kiln.ClientStations.Clock = TimeSpan.FromSeconds(110);
            kiln.ClientStations.SpeedFactor = 0.5f;
            s = kiln.Run(() => ClientStations.Read(new EntityId(4242)))!;
            Assert.Equal(TimeSpan.FromSeconds(30), s.CraftDuration);
            Assert.Equal(TimeSpan.FromSeconds(20), s.CraftRemaining);
            Assert.Equal(TimeSpan.FromSeconds(80), s.Remaining);
            kiln.ClientStations.SpeedFactor = 0;  // no factory speed: no time
            s = kiln.Run(() => ClientStations.Read(new EntityId(4242)))!;
            Assert.Null(s.CraftDuration);
            Assert.Null(s.Remaining);
        }
    }

    [Fact]
    public void Only_a_running_station_has_a_time_as_on_the_games_HUD()
    {
        var (kiln, f) = Joined();
        using (kiln)
        {
            foreach (var state in new[] { StationState.Stopped, StationState.Paused, StationState.MissingMaterials, StationState.NoRecipe })
            {
                f.State = state;
                var s = kiln.Run(() => ClientStations.Read(f.Station))!;
                Assert.Equal(state, s.State);
                Assert.Null(s.CraftRemaining);
                Assert.Null(s.Remaining);
                Assert.Null(s.Progress);
                Assert.Equal(TimeSpan.FromSeconds(60), s.CraftDuration);  // the recipe is still known
                Assert.NotNull(s.Outputs);
            }
        }
    }

    [Fact]
    public void Outputs_need_the_stations_inventory_and_a_recipe_the_client_knows()
    {
        var (kiln, f) = Joined();
        using (kiln)
        {
            f.Inventory = null;  // the client world has no copy of the station's inventory
            Assert.Null(kiln.Run(() => ClientStations.Read(f.Station))!.Outputs);
            f.Inventory = [];
            Assert.Empty(kiln.Run(() => ClientStations.Read(f.Station))!.Outputs!);
            f.Inventory = [new StationStack(Bar, 1)];
            f.Recipe = 0x99;  // a recipe the client's data does not have (a mod's new one)
            var s = kiln.Run(() => ClientStations.Read(f.Station))!;
            Assert.Null(s.Outputs);
            Assert.Null(s.CraftDuration);
            Assert.Null(s.Remaining);
            Assert.Equal(StationState.Running, s.State);
        }
    }

    [Fact]
    public void All_Near_and_Read()
    {
        var (kiln, _) = Joined();
        using (kiln)
        {
            kiln.ClientStations.Add(new EntityId(77), 0x1111, new WorldPosition(1030, 51.5, 2048));
            kiln.ClientStations.Add(new EntityId(78), 0x2222, new WorldPosition(1400, 51.5, 2048));
            var all = kiln.Run(ClientStations.All);
            Assert.Equal([4242u, 77u, 78u], all.Select(s => s.Station.Value));
            var near = kiln.Run(() => ClientStations.Near(new WorldPosition(1031, 51.5, 2048), 16));
            Assert.Equal([77u, 4242u], near.Select(s => s.Station.Value));  // nearest first, the far one left out
            Assert.Empty(kiln.Run(() => ClientStations.Near(new WorldPosition(0, 0, 0), 16)));
            Assert.Throws<ArgumentOutOfRangeException>(() => kiln.Run(() => ClientStations.Near(default, -1)));
            Assert.Null(kiln.Run(() => ClientStations.Read(new EntityId(9))));
            var idle = kiln.Run(() => ClientStations.Read(new EntityId(77)))!;
            Assert.Equal(StationState.NoRecipe, idle.State);
            Assert.Equal(0u, idle.SelectedRecipe);
            Assert.Null(idle.CraftDuration);
            Assert.Null(idle.Outputs);
            Assert.True(kiln.ClientStations.Remove(new EntityId(78)));
            Assert.Equal(2, kiln.Run(ClientStations.All).Count);
            Assert.True(kiln.ClientStations.Reads > 0);
        }
    }

    [Fact]
    public void The_row_switches_the_view_off_cleanly()
    {
        var (kiln, _) = Joined();
        using (kiln)
        {
            Assert.True(kiln.Run(() => ClientStations.Available));
            Assert.True(kiln.Run(() => Capabilities.Is(ClientStations.Capability)));
            kiln.Capabilities.Override(ClientStations.Capability, false);
            Assert.False(kiln.Run(() => ClientStations.Available));
            var e = Assert.Throws<KilnUnavailableException>(() => kiln.Run(ClientStations.All));
            Assert.Equal(ClientStations.Capability, e.Capability);
            Assert.Throws<KilnUnavailableException>(() => kiln.Run(() => ClientStations.Read(new EntityId(4242))));
            Assert.Throws<KilnUnavailableException>(() => kiln.Run(() => ClientStations.Near(default)));
            Assert.Throws<KilnUnavailableException>(() => kiln.Run(() => ClientStations.Open));
            Assert.False(kiln.Run(() => ClientStations.Status)!.Value.On);  // Status answers off, it does not throw
            kiln.Capabilities.ClearOverride(ClientStations.Capability);
            Assert.Single(kiln.Run(ClientStations.All));
        }
    }

    [Fact]
    public void No_station_screen_open_means_no_open_station()
    {
        var (kiln, _) = Joined();
        using (kiln) Assert.Null(kiln.Run(() => ClientStations.Open));  // the kit has no game screen open
    }

    [Fact]
    public void The_raw_view_converts_flag_by_flag()
    {
        Assert.True(ClientStationsNative.LayoutOk);
        Assert.Equal(232, sizeof(ClientStationsNative.View));
        Assert.Equal(192, sizeof(ClientStationsNative.Status));
        ClientStationsNative.View v = default;
        v.Size = (uint)sizeof(ClientStationsNative.View);
        v.Entity = 5;
        v.State = (byte)StationState.Running;
        v.CraftNs = 60_000_000_000;
        v.RemainingNs = 1_500_000_000;
        v.CraftRemainingNs = 500_000_000;
        v.Progress = 0.25f;
        v.OutputCount = 1;
        v.Outputs[0] = 501;
        v.Outputs[1] = 7;
        var none = ClientStations.Convert(in v);
        Assert.Null(none.CraftDuration);  // no flag: nothing is known, whatever the bytes
        Assert.Null(none.Remaining);
        Assert.Null(none.Progress);
        Assert.Null(none.Outputs);
        Assert.False(none.NeedsEnergy);
        v.Flags = ClientStationsNative.FlagRecipe | ClientStationsNative.FlagTime | ClientStationsNative.FlagOutputs | ClientStationsNative.FlagEnergy;
        var all = ClientStations.Convert(in v);
        Assert.Equal(TimeSpan.FromSeconds(60), all.CraftDuration);
        Assert.Equal(TimeSpan.FromMilliseconds(1500), all.Remaining);
        Assert.Equal(TimeSpan.FromMilliseconds(500), all.CraftRemaining);
        Assert.Equal(0.25f, all.Progress);
        Assert.Equal([new StationStack(new ItemId(501), 7)], all.Outputs!);
        Assert.True(all.NeedsEnergy);
        Assert.Empty(all.SelectedChoices);
    }
}
