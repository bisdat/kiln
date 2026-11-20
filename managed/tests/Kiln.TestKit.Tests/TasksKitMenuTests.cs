// SPDX-License-Identifier: MIT
// Kiln tasks' menu hops on the harness (#83): after LeaveWorld the game's main menu runs; KilnHarness.MenuFrame is its UI
// frame (it opens a menu session and runs the menu's hops), EnterWorld ends the menu session and cancels the menu hops
// still waiting. In a menu hop Game.IsTickThread is false and the tick-thread APIs refuse, on both client sides.
using TkEvents;
using Xunit;

// The task bodies stand in for mod code (their own waits, no test token).
#pragma warning disable xUnit1051

namespace Kiln.TestKit.Tests;

public sealed class TasksKitMenuTests
{
    private static (KilnHarness Kiln, ModHandle Mod) InMenu(HarnessSide side)
    {
        var kiln = new KilnHarness(new KilnHarnessOptions { Side = side });
        var mod = kiln.Load<TkEventsMod>(new ModManifest("tk-events"));
        kiln.Step();
        kiln.LeaveWorld();
        kiln.MenuFrame();
        return (kiln, mod);
    }

    [Theory]
    [InlineData(HarnessSide.JoinedClient)]
    [InlineData(HarnessSide.HostingClient)]
    public void A_menu_hop_runs_in_a_menu_frame_where_no_world_api_works(HarnessSide side)
    {
        var (kiln, mod) = InMenu(side);
        using var _ = kiln;
        var seen = new List<string>();
        KilnTask? task = null;
        kiln.OnMainThread(() => task = mod.Context.Tasks.Run("menu", async ctx =>
        {
            await ctx.ToTick(WorldKind.Menu);
            lock (seen) seen.Add($"menu hop: tick {Game.IsTickThread}");
            var refused = Assert.Throws<InvalidOperationException>(() => Game.AssertTickThread());
            lock (seen) seen.Add($"world API refused: {refused.Message.Contains("menu hop", StringComparison.Ordinal)}");
        }));
        kiln.Tasks.Settle();
        Assert.Equal(KilnTaskState.Completed, task!.State);
        Assert.Equal(["menu hop: tick False", "world API refused: True"], seen);
    }

    [Fact]
    public void Entering_a_world_cancels_the_menu_hops_still_waiting()
    {
        var (kiln, mod) = InMenu(HarnessSide.HostingClient);
        using var _ = kiln;
        var ran = false;
        KilnTask? task = null;
        kiln.OnMainThread(() => task = mod.Context.Tasks.Run("waiting", async ctx =>
        {
            await ctx.ToTick(WorldKind.Menu);
            ran = true;
        }));
        Assert.True(SpinWait.SpinUntil(() => kiln.Tasks.Live == 1 && !ran, TimeSpan.FromSeconds(20)));
        kiln.EnterWorld();  // no MenuFrame came: the hop is still waiting
        kiln.Tasks.Settle();
        Assert.False(ran);
        Assert.Equal(KilnTaskState.Cancelled, task!.State);
        Assert.Equal(0, kiln.MenuFrame());  // a world runs: no menu frame
    }

    [Fact]
    public void In_a_world_a_menu_hop_is_refused()
    {
        var kiln = new KilnHarness(new KilnHarnessOptions { Side = HarnessSide.HostingClient });
        using var _ = kiln;
        var mod = kiln.Load<TkEventsMod>(new ModManifest("tk-events"));
        kiln.Step();
        string? refused = null;
        var task = kiln.As(mod, () => mod.Context.Tasks.Run("in-world", ctx =>
        {
            refused = Assert.Throws<KilnUnavailableException>(() => ctx.ToTick(WorldKind.Menu)).Capability;
            return Task.CompletedTask;
        }));
        kiln.Tasks.Settle();
        Assert.Equal(KilnTaskState.Completed, task.State);
        Assert.Equal("tasks.pool", refused);
    }

    [Fact]
    public void A_dedicated_server_has_no_main_menu()
    {
        using var server = new KilnHarness(new KilnHarnessOptions { Side = HarnessSide.DedicatedServer });
        Assert.Throws<InvalidOperationException>(() => server.MenuFrame());
    }
}
