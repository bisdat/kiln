// SPDX-License-Identifier: MIT
// Tick hops through the core's tick hop: its per-mod and per-step budgets, mods in turn, FIFO per mod, cancelled at a
// world's end and never carried into the next world; and that none of it goes through Scheduler.Post's queue.
using Kiln.Host;
using Xunit;

#pragma warning disable xUnit1031, xUnit1051

namespace Kiln.Host.Tests;

public sealed class TaskTickBudgetTests : HostTaskTest
{
    private KilnTask Posts(ModContext mod, int count, List<(string Mod, int N)> applied, TaskCompletionSource? hold = null) =>
        mod.Tasks.Run("posts", async ctx =>
        {
            for (var i = 0; i < count; i++)
            {
                var n = i;
                ctx.PostToTick(() =>
                {
                    lock (applied) applied.Add((mod.Info.Id, n));
                });
            }
            if (hold is not null) await hold.Task;
        });

    [Fact]
    public void A_mods_hops_run_in_order_at_most_64_a_step_and_the_rest_waits()
    {
        var mod = NewMod("alpha");
        var applied = new List<(string Mod, int N)>();
        var task = Posts(mod, 150, applied);
        Assert.True(Until(() => Native.PendingHops == 150));
        Assert.Equal(0, mod.Pending.Count);  // not Scheduler.Post's queue
        Assert.Equal(64, Native.Step());
        Assert.Equal(64, applied.Count);
        Assert.Equal(KilnTaskState.Running, task.State);  // alive until its posts are applied
        Assert.Equal(64, Native.Step());
        Assert.Equal(22, Native.Step());
        Assert.Equal(0, Native.Step());
        Assert.Equal(Enumerable.Range(0, 150).Select(n => ("alpha", n)), applied);  // FIFO, none dropped
        Ended(task);
        Assert.Equal(KilnTaskState.Completed, task.State);
        lock (Hops) Assert.Equal(150, Hops.Count);
        Quiesce();
    }

    [Fact]
    public void Mods_take_turns_within_the_step_budget_and_nobody_starves()
    {
        Native.HopCostNs = 500_000;  // each hop "takes" 0.5 ms: 4 per mod (2 ms), 8 per step (4 ms)
        var mods = Enumerable.Range(0, 5).Select(i => NewMod("mod" + i)).ToList();
        var applied = new List<(string Mod, int N)>();
        var tasks = mods.Select(m => Posts(m, 12, applied)).ToList();
        Assert.True(Until(() => Native.PendingHops == 60));
        var perStep = new List<List<string>>();
        var steps = 0;
        while (Native.PendingHops > 0 && steps++ < 50)
        {
            var before = applied.Count;
            var ran = Native.Step();
            Assert.True(ran <= 8, $"a step ran {ran} hops: the 4 ms step budget is 8 of them");
            perStep.Add([.. applied.Skip(before).Select(a => a.Mod)]);
        }
        Assert.Equal(60, applied.Count);
        foreach (var step in perStep)
            Assert.All(step.GroupBy(m => m), g => Assert.True(g.Count() <= 4, "a mod ran more than its 2 ms in one step"));
        // the turn moves on from step to step: within the first three steps every mod ran
        Assert.Equal(5, perStep.Take(3).SelectMany(s => s).Distinct().Count());
        foreach (var m in mods)  // FIFO per mod
            Assert.Equal(Enumerable.Range(0, 12), applied.Where(a => a.Mod == m.Info.Id).Select(a => a.N));
        foreach (var t in tasks) Ended(t);
        Quiesce();
    }

    [Fact]
    public void Hops_pending_when_the_world_stops_are_cancelled_and_none_is_carried_into_the_next_world()
    {
        var mod = NewMod("alpha");
        var applied = new List<(string Mod, int N)>();
        var hold = new TaskCompletionSource();
        var task = Posts(mod, 10, applied, hold);
        Assert.True(Until(() => Native.PendingHops == 10));
        Native.StopSession();  // on the thread that ends the world: not the tick
        Assert.Equal(0, Native.PendingHops);
        lock (Native.HopsRun) Assert.All(Native.HopsRun, h => Assert.True(h.Cancelled));
        lock (Hops) Assert.Empty(Hops);  // cancelled off the tick: no mod code ran on the tick
        Native.StartSession();
        Assert.Equal(0, Native.Step());
        Assert.Empty(applied);
        hold.SetResult();
        Ended(task);
        Assert.Equal(KilnTaskState.Cancelled, task.State);  // a task never ends Completed with a post unapplied
        Quiesce();
    }

    [Fact]
    public void A_world_that_stopped_without_a_stop_event_carries_no_hop_over_either()
    {
        var mod = NewMod("alpha");
        var applied = new List<(string Mod, int N)>();
        var task = Posts(mod, 3, applied);
        Assert.True(Until(() => Native.PendingHops == 3));
        Native.StartSession();  // the next world starts; the old one never said it stopped
        Assert.Equal(0, Native.Step());
        Assert.Empty(applied);
        Ended(task);
        Assert.Equal(KilnTaskState.Cancelled, task.State);
        Quiesce();
    }

    [Fact]
    public void The_old_scheduler_queue_is_separate_and_keeps_its_behaviour()
    {
        var mod = NewMod("alpha");
        var order = new List<string>();
        mod.Scheduler.Post(() => order.Add("scheduled"));
        var task = mod.Tasks.Run("post", ctx => ctx.PostToTick(() => order.Add("hop")));
        Assert.True(Until(() => Native.PendingHops == 1));
        Assert.Equal(1, mod.Pending.Count);  // Scheduler.Post's own queue: the hop is not in it
        Native.Step(() =>
        {
            // the mod's Pre, as the host runs it: its scheduled work, after the task hops of this step
            while (true)
            {
                Action? next;
                lock (mod.PendingLock)
                    if (!mod.Pending.TryDequeue(out next)) break;
                next();
            }
        });
        Assert.Equal(["hop", "scheduled"], order);
        Ended(task);
        Quiesce();
    }
}
