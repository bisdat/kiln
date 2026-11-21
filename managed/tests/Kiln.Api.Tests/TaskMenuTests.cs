// SPDX-License-Identifier: MIT
// Menu hops (#83): ToTick(WorldKind.Menu), OnTick(WorldKind.Menu, ...) and PostToTick(WorldKind.Menu, ...) land on the
// game's UI frame while no world runs. Each menu stretch between two worlds is its own session: a menu hop still waiting
// when a world starts ends the task Cancelled through its own token, quietly, and a task bound to one menu stretch never
// hops into the next. With a world running there is no menu: a menu hop throws KilnUnavailableException at once. World
// APIs refuse inside a menu hop with a message that says so. On the fake pool: the menu frame on the fake main thread.
using Xunit;

// The task bodies stand in for mod code (their own waits, no test token).
#pragma warning disable xUnit1031, xUnit1051

namespace Kiln.Api.Tests;

public sealed class TaskMenuTests : TaskTest
{
    private static readonly TimeSpan Long = TimeSpan.FromSeconds(20);

    // The main menu: no world (TaskTest starts a server session; the player has left it) and the UI frame has run once.
    private void InMenu()
    {
        Pool.StopSession();
        Pool.MenuFrame();
    }

    private KilnTask StartOnMain(ModContext mod, string name, Func<KilnTaskContext, Task> work)
    {
        KilnTask? task = null;
        Pool.OnMain(() => task = mod.Tasks.Run(name, work));
        return task!;
    }

    private void FramesUntilEnded(KilnTask task, int frames = 50)
    {
        for (var i = 0; i < frames && !task.IsTerminalForTests(); i++)
        {
            Pool.WaitIdle();
            Pool.MenuFrame();
        }
        Ended(task);
    }

    private List<string> Errors()
    {
        lock (Backend.Lines)
            return [.. Backend.Lines.Where(l => l.StartsWith("Error:", StringComparison.Ordinal) || l.StartsWith("Warn:", StringComparison.Ordinal))];
    }

    [Fact]
    public void A_task_started_in_the_menu_hops_to_the_menu_frame()
    {
        InMenu();
        var mod = NewMod("alpha");
        var seen = new List<string>();
        var task = StartOnMain(mod, "menu", async ctx =>
        {
            seen.Add($"home {ctx.TickWorld}");
            await ctx.ToTick(WorldKind.Menu);
            seen.Add($"ToTick(Menu) on main: {Pool.IsMainThread}, tick: {Game.IsTickThread}");
            await Task.Yield();  // a later await in a menu hop comes back to the menu frame
            seen.Add($"after an await on main: {Pool.IsMainThread}");
            var n = await ctx.OnTick(WorldKind.Menu, () => Pool.IsMainThread);  // already there: at once
            seen.Add($"OnTick(Menu) at once: {n}");
        });
        Pool.WaitIdle();
        Assert.Equal(1, Pool.PendingHopsOf(WorldKind.Menu));
        FramesUntilEnded(task);
        Assert.Equal(KilnTaskState.Completed, task.State);
        Assert.Equal(["home Client", "ToTick(Menu) on main: True, tick: False", "after an await on main: True", "OnTick(Menu) at once: True"], seen);
        Assert.Empty(Errors());
    }

    [Fact]
    public void OnTick_and_PostToTick_reach_the_menu_from_a_worker()
    {
        InMenu();
        var mod = NewMod("alpha");
        var seen = new List<string>();
        var task = StartOnMain(mod, "menu-calls", async ctx =>
        {
            var main = await ctx.OnTick(WorldKind.Menu, () => Pool.IsMainThread);
            seen.Add($"OnTick(Menu) on main: {main}, back on a worker: {Pool.IsWorker(Environment.CurrentManagedThreadId)}");
            ctx.PostToTick(WorldKind.Menu, () => seen.Add($"PostToTick(Menu) on main: {Pool.IsMainThread}"));
        });
        FramesUntilEnded(task);
        Assert.Equal(KilnTaskState.Completed, task.State);
        Assert.Equal(["OnTick(Menu) on main: True, back on a worker: True", "PostToTick(Menu) on main: True"], seen);
    }

    [Fact]
    public void A_menu_hop_waiting_when_a_world_starts_ends_the_task_Cancelled_quietly()
    {
        InMenu();
        var mod = NewMod("alpha");
        var ran = false;
        var posted = false;
        var finallyOnWorker = false;
        var task = StartOnMain(mod, "pending", async ctx =>
        {
            try
            {
                ctx.PostToTick(WorldKind.Menu, () => posted = true);
                await ctx.ToTick(WorldKind.Menu);
                ran = true;
            }
            finally
            {
                finallyOnWorker = Pool.IsWorker(Environment.CurrentManagedThreadId);
            }
        });
        Pool.WaitIdle();
        Assert.Equal(2, Pool.PendingHopsOf(WorldKind.Menu));
        Pool.OnTick(() => Pool.StartSession());  // the player hosts: the server world's first step ends the menu
        Ended(task);
        Assert.Equal(KilnTaskState.Cancelled, task.State);
        Assert.False(ran);
        Assert.False(posted);
        Assert.True(finallyOnWorker);
        Assert.Equal(0, Pool.PendingHopsOf(WorldKind.Menu));
        Assert.Empty(Errors());
    }

    [Fact]
    public void A_world_hop_never_runs_in_the_menu()
    {
        Pool.StartSession(WorldKind.Client);  // a hosting client
        var mod = NewMod("alpha");
        var ran = false;
        var task = StartOnMain(mod, "client", async ctx =>
        {
            await ctx.ToTick(WorldKind.Client);
            ran = true;
        });
        Pool.WaitIdle();
        Pool.OnMain(() => Pool.StopSession(WorldKind.Client));
        Pool.StopSession();
        Pool.MenuFrame();
        Ended(task);
        Assert.Equal(KilnTaskState.Cancelled, task.State);
        Assert.False(ran);
    }

    [Fact]
    public void In_a_world_there_is_no_menu_and_the_task_stays_usable()
    {
        // TaskTest's server world runs (a dedicated server, or a hosting client in game)
        var mod = NewMod("alpha");
        KilnUnavailableException? menu = null;
        var landed = false;
        KilnTask? task = null;
        Pool.OnTick(() => task = mod.Tasks.Run("in-world", async ctx =>
        {
            menu = Assert.Throws<KilnUnavailableException>(() => ctx.ToTick(WorldKind.Menu));
            Assert.Throws<KilnUnavailableException>(() => ctx.PostToTick(WorldKind.Menu, () => { }));
            Assert.False(ctx.Cancellation.IsCancellationRequested);
            await ctx.ToTick();
            landed = true;
        }));
        for (var i = 0; i < 50 && !task!.IsTerminalForTests(); i++)
        {
            Pool.WaitIdle();
            Pool.Step();
        }
        Ended(task!);
        Assert.Equal(KilnTaskState.Completed, task!.State);
        Assert.True(landed);
        Assert.Equal("tasks.pool", menu?.Capability);
        Assert.Contains("main menu", menu!.Message);
        Assert.Equal(0, Pool.PendingHopsOf(WorldKind.Menu));
    }

    [Fact]
    public void A_task_bound_to_one_menu_stretch_never_hops_into_the_next()
    {
        InMenu();
        var mod = NewMod("alpha");
        var gate = new ManualResetEventSlim();
        var ran = 0;
        var task = StartOnMain(mod, "stretch", async ctx =>
        {
            await ctx.ToTick(WorldKind.Menu);  // bound to menu stretch 1 (it was started there anyway)
            ran++;
            await Task.Run(() => gate.Wait(Long));  // computing while the player plays a world and leaves it
            await ctx.ToTick(WorldKind.Menu);
            ran++;
        });
        for (var i = 0; i < 5 && ran == 0; i++)
        {
            Pool.WaitIdle();
            Pool.MenuFrame();
        }
        Assert.Equal(1, ran);  // it computes now
        Pool.OnTick(() => Pool.StartSession());
        Pool.OnTick(() => Pool.StopSession());
        Pool.MenuFrame();  // menu stretch 2
        gate.Set();
        FramesUntilEnded(task);
        Assert.Equal(KilnTaskState.Cancelled, task.State);
        Assert.Equal(1, ran);
        Assert.Empty(Errors());
    }

    [Fact]
    public void A_task_started_in_a_menu_hop_belongs_to_the_menu()
    {
        InMenu();
        var mod = NewMod("alpha");
        KilnTask? child = null;
        var childHome = WorldKind.Server;
        var childOnMain = false;
        var task = StartOnMain(mod, "parent", async ctx =>
        {
            await ctx.ToTick(WorldKind.Menu);
            child = mod.Tasks.Run("child", async c =>
            {
                childHome = c.TickWorld;
                await c.ToTick();
                childOnMain = Pool.IsMainThread;
            });
        });
        FramesUntilEnded(task);
        FramesUntilEnded(child!);
        Assert.Equal(KilnTaskState.Completed, child!.State);
        Assert.Equal(WorldKind.Menu, childHome);
        Assert.True(childOnMain);
    }

    [Fact]
    public void World_APIs_refuse_in_a_menu_hop_and_say_why()
    {
        InMenu();
        var mod = NewMod("alpha");
        var messages = new List<string>();
        var task = StartOnMain(mod, "world-api", async ctx =>
        {
            await ctx.ToTick(WorldKind.Menu);
            messages.Add(Assert.Throws<InvalidOperationException>(() => Services.RequireTickThread("Spawn.Npc")).Message);
            messages.Add(Assert.Throws<InvalidOperationException>(() => Game.AssertTickThread()).Message);
            messages.Add(Game.IsTickThread ? "tick" : "not the tick");
            Assert.Throws<ArgumentOutOfRangeException>(() => ctx.NextGateWindow(WorldKind.Menu));
        });
        FramesUntilEnded(task);
        Assert.Equal(KilnTaskState.Completed, task.State);
        Assert.Contains("Spawn.Npc", messages[0]);
        Assert.All(messages.Take(2), m => Assert.Contains("menu hop", m));
        Assert.Equal("not the tick", messages[2]);
    }

    [Fact]
    public void The_menu_is_no_gate_window_and_unknown_kinds_still_refuse()
    {
        InMenu();
        var mod = NewMod("alpha");
        var thrown = new List<string>();
        var task = StartOnMain(mod, "kinds", ctx =>
        {
            foreach (var call in new Action[]
                     {
                         () => ctx.NextGateWindow(WorldKind.Menu),
                         () => ctx.ToTick((WorldKind)7),
                         () => ctx.ToTick(WorldKind.Prediction),
                     })
                thrown.Add(Record.Exception(call)?.GetType().Name ?? "none");
            return Task.CompletedTask;
        });
        Ended(task);
        Assert.All(thrown, t => Assert.Equal(nameof(ArgumentOutOfRangeException), t));
        Assert.Equal(0, Pool.HopsQueued);
    }
}
