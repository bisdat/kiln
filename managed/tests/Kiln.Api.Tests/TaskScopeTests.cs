// SPDX-License-Identifier: MIT
// The execution scope ("this thread runs task X of owner Y"): installed around every segment and tick hop, restored
// after it (also after a fault, and when two mods share one worker; the fake records any leak), absent in work that left
// the task (Task.Run, new Thread, ConfigureAwait(false)) even though the flowed diagnostics identity reaches it, and
// CurrentMod's tick meaning unchanged outside segments. Then Kiln's internal component entry: one long-running runner
// under the component's own owner, a single admission, its own attribution and a bounded stop off the tick.
using Xunit;

// The task bodies stand in for mod code (their own delays, no test token), and the test thread is the mod's own thread,
// where blocking on a finished task is allowed.
#pragma warning disable xUnit1031, xUnit1051

namespace Kiln.Api.Tests;

public sealed class TaskScopeTests : TaskTest
{
    private static readonly TimeSpan Long = TimeSpan.FromSeconds(20);

    private sealed record Seen(int Thread, string? Owner, string? Task, string? Mod, bool InSegment, string? Identity);

    private static Seen Look() => new(
        Environment.CurrentManagedThreadId,
        TaskExecutionScope.Current?.OwnerName,
        TaskExecutionScope.Current?.Name,
        ModContext.CurrentMod?.Info.Id,
        TaskExecutionScope.Current is not null,
        TaskExecutionScope.FlowedIdentity);

    [Fact]
    public void Every_segment_runs_in_its_tasks_scope()
    {
        var mod = NewMod("alpha");
        var seen = new List<Seen>();
        var task = mod.Tasks.Run("scoped", async ctx =>
        {
            seen.Add(Look());
            await Task.Yield();
            seen.Add(Look());
            Assert.Same(ctx, TaskExecutionScope.Current!.Context);
        });
        Ended(task);
        Assert.Equal(KilnTaskState.Completed, task.State);
        Assert.All(seen, s => Assert.Equal(("alpha", "scoped", "alpha", true, "alpha/scoped"), (s.Owner, s.Task, s.Mod, s.InSegment, s.Identity)));
        Assert.Null(TaskExecutionScope.Current);  // the test's thread never had one
        // Pool.Leaks (checked at the end) is empty: every dispatch restored its thread
    }

    [Fact]
    public void The_scope_is_restored_after_a_fault()
    {
        UsePool(1);
        var mod = NewMod("alpha");
        var faulted = mod.Tasks.Run("faults", ctx => throw new FormatException());
        var asyncFaulted = mod.Tasks.Run("faults-later", async ctx =>
        {
            await Task.Yield();
            throw new FormatException();
        });
        Seen? next = null;
        var after = mod.Tasks.Run("after", ctx => next = Look());
        Ended(faulted);
        Ended(asyncFaulted);
        Ended(after);
        Assert.Equal(KilnTaskState.Faulted, faulted.State);
        Assert.Equal(KilnTaskState.Faulted, asyncFaulted.State);
        Assert.Equal("after", next!.Task);
        Pool.WaitIdle();
        Assert.Empty(Pool.Leaks);
    }

    [Fact]
    public void Two_mods_sharing_one_worker_each_see_their_own_scope()
    {
        UsePool(1);
        var a = NewMod("alpha");
        var b = NewMod("beta");
        var seen = new List<Seen>();
        var tasks = new List<KilnTask>();
        for (var i = 0; i < 3; i++)
        {
            tasks.Add(a.Tasks.Run("a" + i, async ctx =>
            {
                lock (seen) seen.Add(Look());
                await Task.Yield();
                lock (seen) seen.Add(Look());
            }));
            tasks.Add(b.Tasks.Run("b" + i, ctx =>
            {
                lock (seen) seen.Add(Look());
            }));
        }
        tasks.ForEach(t => Ended(t));
        Assert.Equal(9, seen.Count);
        Assert.Single(seen.Select(s => s.Thread).Distinct());  // one worker ran them all
        Assert.All(seen, s =>
        {
            Assert.Equal(s.Owner, s.Mod);
            Assert.StartsWith(s.Owner == "alpha" ? "a" : "b", s.Task);
            Assert.Equal($"{s.Owner}/{s.Task}", s.Identity);
        });
    }

    [Fact]
    public void Work_that_left_the_task_has_no_scope_but_carries_the_flowed_identity()
    {
        var mod = NewMod("alpha");
        Seen? taskRun = null, thread = null, configureAwaitFalse = null, back = null;
        var task = mod.Tasks.Run("leaves", async ctx =>
        {
            taskRun = await Task.Run(Look);
            var t = new Thread(() => thread = Look());
            t.Start();
            t.Join();
            await Task.Delay(5).ConfigureAwait(false);
            configureAwaitFalse = Look();
            await ctx.NextGateWindow();  // a Kiln await brings the work back into the task
            back = Look();
        });
        for (var i = 0; i < 50 && !task.WaitEnded(TimeSpan.FromMilliseconds(20)); i++) Pool.Step();
        Ended(task);
        foreach (var escaped in new[] { taskRun!, thread!, configureAwaitFalse! })
        {
            Assert.False(escaped.InSegment);
            Assert.Null(escaped.Mod);
            Assert.Equal("alpha/leaves", escaped.Identity);  // diagnostics only: it grants nothing
            Assert.False(Pool.IsWorker(escaped.Thread));
        }
        Assert.True(back!.InSegment);
        Assert.True(Pool.IsWorker(back.Thread));
    }

    [Fact]
    public void CurrentMod_keeps_its_tick_meaning_outside_task_segments()
    {
        var a = NewMod("alpha");
        var b = NewMod("beta");
        Seen? onTickBefore = null, onTickAfter = null, inTask = null;
        KilnTask? task = null;
        TickAs(a, () =>
        {
            onTickBefore = Look();
            task = b.Tasks.Run("from-a-tick", ctx => inTask = Look());
            task.WaitEnded(Long);  // the test's probe only: real tick code never waits
            onTickAfter = Look();
        });
        Ended(task!);
        Assert.Equal("alpha", onTickBefore!.Mod);
        Assert.Equal("alpha", onTickAfter!.Mod);
        Assert.False(onTickAfter.InSegment);
        Assert.Equal("beta", inTask!.Mod);
        Assert.Null(ModContext.CurrentMod);
    }

    // ---- Kiln's own components ----

    private const ulong ComponentOwner = 0xC0FFEE;

    [Fact]
    public void A_component_starts_one_long_running_runner_however_many_threads_ask()
    {
        using var stop = new ManualResetEventSlim();
        var runner = new ComponentTask(ComponentOwner, "kiln.test", "writer", ctx => stop.Wait(Long));
        Assert.Null(runner.Running);
        const int Askers = 8;
        var started = new KilnTask[Askers];
        using var barrier = new Barrier(Askers);
        var threads = Enumerable.Range(0, Askers).Select(i => new Thread(() =>
        {
            barrier.SignalAndWait();
            started[i] = runner.Start();
        })).ToList();
        threads.ForEach(t => t.Start());
        threads.ForEach(t => Assert.True(t.Join(Long)));
        Assert.All(started, t => Assert.Same(started[0], t));
        Assert.Same(started[0], runner.Running);
        var admitted = Assert.Single(Pool.Admitted);
        Assert.Equal((ComponentOwner, "writer", true), (admitted.Owner, admitted.Name, admitted.Options.LongRunning));
        stop.Set();
        Ended(started[0]);
        Assert.Null(runner.Running);
        var again = runner.Start();  // after the runner ended, the next job starts a new one
        Assert.NotSame(started[0], again);
        Ended(again);
    }

    [Fact]
    public void A_component_runner_is_its_components_own_not_a_mods()
    {
        var mod = NewMod("alpha");
        Seen? seen = null;
        var runner = new ComponentTask(ComponentOwner, "kiln.test", "writer", ctx => seen = Look());
        var task = runner.Start();
        Ended(task);
        Assert.Equal(("kiln.test", "writer", (string?)null, true), (seen!.Owner, seen.Task, seen.Mod, seen.InSegment));
        Pool.SealOwner(mod.Owner);  // a mod's unload never reaches the component's owner
        Ended(runner.Start());
        Assert.Throws<ArgumentException>(() => new ComponentTask(0, "kiln.test", "writer", _ => { }));
        Assert.Throws<ArgumentException>(() => new ComponentTask(ComponentOwner, "kiln.test", "", _ => { }));
    }

    [Fact]
    public void Stop_cancels_the_runner_and_joins_it_within_the_bound()
    {
        var loops = 0;
        var runner = new ComponentTask(ComponentOwner, "kiln.test", "writer", ctx =>
        {
            while (!ctx.Cancellation.IsCancellationRequested)
            {
                Interlocked.Increment(ref loops);
                ctx.Cancellation.WaitHandle.WaitOne(5);
            }
        });
        var task = runner.Start();
        Assert.True(runner.Stop(Long));
        Assert.Equal(KilnTaskState.Completed, task.State);  // it returned normally after the cancel
        Assert.Null(runner.Running);
        Assert.True(new ComponentTask(ComponentOwner, "kiln.test", "idle", _ => { }).Stop(TimeSpan.Zero));  // never started
    }

    [Fact]
    public void Stop_gives_up_after_its_bound_on_a_runner_that_ignores_cancellation()
    {
        using var started = new ManualResetEventSlim();
        using var release = new ManualResetEventSlim();
        var runner = new ComponentTask(ComponentOwner, "kiln.test", "stuck", ctx =>
        {
            started.Set();
            release.Wait(Long);
        });
        var task = runner.Start();
        Assert.True(started.Wait(Long));
        var clock = System.Diagnostics.Stopwatch.StartNew();
        Assert.False(runner.Stop(TimeSpan.FromMilliseconds(100)));
        Assert.InRange(clock.ElapsedMilliseconds, 90, 10000);
        Assert.Same(task, runner.Running);  // left running and reported, never aborted
        Assert.Equal(KilnTaskState.Running, task.State);
        release.Set();
        Ended(task);
    }

    [Fact]
    public void Stop_is_refused_on_the_tick_and_on_a_worker()
    {
        var runner = new ComponentTask(ComponentOwner, "kiln.test", "idle", _ => { });
        Pool.OnTick(() => Assert.Throws<InvalidOperationException>(() => runner.Stop(TimeSpan.Zero)));
        var mod = NewMod("alpha");
        Exception? refused = null;
        Ended(mod.Tasks.Run("stopper", ctx =>
        {
            try
            {
                runner.Stop(TimeSpan.Zero);
            }
            catch (Exception e)
            {
                refused = e;
            }
        }));
        Assert.IsType<InvalidOperationException>(refused);
    }
}
