// SPDX-License-Identifier: MIT
// Menu hops through the host bridge (#83): the host's UI frame calls the lease table's menu_frame, which opens the menu's
// session while no world runs and dispatches its hops as KILN_LEASE_MENU_HOP; the bridge runs them on the main thread
// through the host's menu frame (Game.IsTickThread false there). A world's start ends the menu session: its waiting hops
// end their tasks Cancelled, quietly.
using Kiln.Host;
using Xunit;

#pragma warning disable xUnit1031, xUnit1051

namespace Kiln.Host.Tests;

public sealed class TaskMenuHostTests : HostTaskTest
{
    private KilnTask OnMain(ModContext mod, string name, Func<KilnTaskContext, Task> work)
    {
        KilnTask? task = null;
        Native.OnMain(() => task = mod.Tasks.Run(name, work));
        return task!;
    }

    private uint MenuFrame()
    {
        uint ran = 0;
        Native.OnMain(() => ran = Tasks.MenuFrame());
        return ran;
    }

    [Fact]
    public void A_menu_hop_runs_on_the_hosts_menu_frame()
    {
        Native.StopSession();  // the player left the world: the main menu
        Assert.Equal(0u, MenuFrame());
        Assert.NotEqual(0u, Native.MenuSession);
        var mod = NewMod("alpha");
        var seen = new List<string>();
        var task = OnMain(mod, "menu", async ctx =>
        {
            await ctx.ToTick(WorldKind.Menu);
            seen.Add($"main {Native.IsMainThread}, tick {Game.IsTickThread}");
        });
        Assert.True(Until(() => Native.PendingMenuHops == 1));
        Native.ClientStep();  // no client world: nothing of it runs there
        Assert.Equal(1, Native.PendingMenuHops);
        Assert.Equal(1u, MenuFrame());
        Ended(task);
        Assert.Equal(KilnTaskState.Completed, task.State);
        Assert.Equal(["main True, tick False"], seen);
        lock (MenuHops) Assert.Equal([(mod.Owner, true, false)], MenuHops);
        lock (Hops) Assert.Empty(Hops);
        lock (ClientHops) Assert.Empty(ClientHops);
        Quiesce();
    }

    [Fact]
    public void A_worlds_start_cancels_the_menu_hops_still_waiting()
    {
        Native.StopSession();
        MenuFrame();
        var mod = NewMod("alpha");
        var ran = false;
        var task = OnMain(mod, "waiting", async ctx =>
        {
            await ctx.ToTick(WorldKind.Menu);
            ran = true;
        });
        Assert.True(Until(() => Native.PendingMenuHops == 1));
        Native.OnMain(Native.StartClientSession);  // the player joins a server: the client world's first step
        Ended(task);
        Assert.Equal(KilnTaskState.Cancelled, task.State);
        Assert.False(ran);
        Assert.Equal(0u, Native.MenuSession);
        Assert.Equal(0u, MenuFrame());  // a world runs: no menu
        Assert.False(Host.Has("Error"), Host.Dump());
        Quiesce();
    }

    [Fact]
    public void In_a_world_a_menu_hop_is_refused_at_once()
    {
        var mod = NewMod("alpha");
        string? refused = null;
        KilnTask? task = null;
        Native.OnTick(() => task = mod.Tasks.Run("in-world", ctx =>
        {
            refused = Assert.Throws<KilnUnavailableException>(() => ctx.ToTick(WorldKind.Menu)).Message;
            return Task.CompletedTask;
        }));
        Ended(task!);
        Assert.Equal(KilnTaskState.Completed, task!.State);
        Assert.Contains("main menu", refused);
        Quiesce();
    }
}
