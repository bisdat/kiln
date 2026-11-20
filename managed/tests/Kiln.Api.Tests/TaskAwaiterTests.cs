// SPDX-License-Identifier: MIT
// KilnTask as an awaitable: continuations registered before, at and after the end run exactly once and never inline on
// the thread that ends the task; a cancel that races the end; a burst of wake-ups; nested awaits; awaiting from the tick.
// The races are forced at fixed points (TaskRun.TestHook), not left to timing.
using Xunit;

// The task bodies stand in for mod code (their own delays, no test token), and the test thread is the mod's own thread,
// where blocking on a finished task is allowed.
#pragma warning disable xUnit1031, xUnit1051

namespace Kiln.Api.Tests;

public sealed class TaskAwaiterTests : TaskTest
{
    private static readonly TimeSpan Long = TimeSpan.FromSeconds(20);

    /// <summary>Counts runs of one continuation and remembers the thread of the first.</summary>
    private sealed class Probe
    {
        private int _runs;
        public int Thread;
        public readonly ManualResetEventSlim Ran = new();
        public int Runs => Volatile.Read(ref _runs);

        public void Run()
        {
            if (Interlocked.Increment(ref _runs) == 1) Thread = Environment.CurrentManagedThreadId;
            Ran.Set();
        }
    }

    [Fact]
    public void A_continuation_registered_after_the_end_runs_once_and_not_inline()
    {
        var mod = NewMod("alpha");
        var task = mod.Tasks.Run("done", ctx => { });
        Ended(task);
        var probe = new Probe();
        var awaiter = task.GetAwaiter();
        Assert.True(awaiter.IsCompleted);
        awaiter.UnsafeOnCompleted(probe.Run);
        Assert.True(probe.Ran.Wait(Long));
        Assert.NotEqual(Environment.CurrentManagedThreadId, probe.Thread);  // queued, not run by the registering call
        Thread.Sleep(20);
        Assert.Equal(1, probe.Runs);
    }

    [Theory]
    [InlineData((int)TaskRun.Point.BeforeFinish)]
    [InlineData((int)TaskRun.Point.BeforePublish)]
    [InlineData((int)TaskRun.Point.AfterPublish)]
    public void A_registration_racing_the_end_runs_exactly_once_in_every_window(int at)
    {
        var window = (TaskRun.Point)at;
        var mod = NewMod("alpha");
        var probe = new Probe();
        var regular = new Probe();
        var hookThread = 0;
        TaskRun.TestHook = (point, run) =>
        {
            if (point != window || run.Name != "racing") return;
            hookThread = Environment.CurrentManagedThreadId;
            run.Task.GetAwaiter().OnCompleted(probe.Run);  // registered by the thread that is ending the task, in this window
        };
        var task = mod.Tasks.Run("racing", ctx => { });
        task.GetAwaiter().UnsafeOnCompleted(regular.Run);
        Ended(task);
        Assert.True(probe.Ran.Wait(Long));
        Assert.True(regular.Ran.Wait(Long));
        Thread.Sleep(20);
        Assert.Equal(1, probe.Runs);
        Assert.Equal(1, regular.Runs);
        Assert.NotEqual(hookThread, probe.Thread);  // never inline on the ending thread
        Assert.Equal(KilnTaskState.Completed, task.State);
    }

    [Fact]
    public void A_cancel_racing_the_end_never_changes_a_normal_finish()
    {
        var mod = NewMod("alpha");
        TaskRun.TestHook = (point, run) =>
        {
            if (point == TaskRun.Point.BeforeFinish) run.Task.Cancel();  // the work has returned; the lease is finishing
            if (point == TaskRun.Point.AfterPublish) run.Task.Cancel();  // after the end: nothing changes
        };
        var task = mod.Tasks.Run<int>("finishing", async ctx =>
        {
            await Task.Yield();
            return 5;
        });
        Ended(task);
        Assert.Equal(KilnTaskState.Completed, task.State);
        Assert.Equal(5, task.Result);
        task.Cancel();
        Assert.Equal(KilnTaskState.Completed, task.State);
    }

    [Fact]
    public void The_end_is_published_once()
    {
        var mod = NewMod("alpha");
        var probe = new Probe();
        var task = mod.Tasks.Run("once", ctx => { });
        task.GetAwaiter().UnsafeOnCompleted(probe.Run);
        Ended(task);
        Assert.False(task.Publish(KilnTaskState.Faulted, new FormatException(), null));
        Assert.False(task.Publish(KilnTaskState.Cancelled, null, new OperationCanceledException()));
        Assert.Equal(KilnTaskState.Completed, task.State);
        Assert.Null(task.Fault);
        Assert.True(probe.Ran.Wait(Long));
        Thread.Sleep(20);
        Assert.Equal(1, probe.Runs);
    }

    [Fact]
    public void A_burst_of_wakeups_resumes_the_task_once_per_suspension_and_runs_each_continuation_once()
    {
        var mod = NewMod("alpha");
        const int Posters = 8;
        var ran = new int[Posters];
        SynchronizationContext? context = null;
        using var suspended = new ManualResetEventSlim();
        var release = new TaskCompletionSource();
        var task = mod.Tasks.Run("burst", async ctx =>
        {
            context = SynchronizationContext.Current;
            suspended.Set();
            await release.Task;  // suspended: the posts below arrive while it holds no worker
        });
        Assert.True(suspended.Wait(Long));
        Pool.WaitIdle();
        using var barrier = new Barrier(Posters);
        var threads = Enumerable.Range(0, Posters).Select(i => new Thread(() =>
        {
            barrier.SignalAndWait();
            context!.Post(_ => Interlocked.Increment(ref ran[i]), null);
        })).ToList();
        threads.ForEach(t => t.Start());
        threads.ForEach(t => Assert.True(t.Join(Long)));
        release.SetResult();
        Ended(task);
        Assert.All(ran, n => Assert.Equal(1, n));
        Assert.Equal(KilnTaskState.Completed, task.State);
        // Pool.Errors (checked at the end) holds no duplicate resume or arm
    }

    [Fact]
    public void A_nested_await_resumes_in_the_awaiting_task_with_the_inner_result_or_fault()
    {
        var mod = NewMod("alpha");
        var other = NewMod("beta");
        var inner = other.Tasks.Run<int>("inner", async ctx =>
        {
            await Task.Delay(5);
            return 9;
        });
        var failing = other.Tasks.Run("failing", async ctx =>
        {
            await Task.Yield();
            throw new FormatException("inner");
        });
        TaskRun? outerRun = null, after = null;
        var afterThread = 0;
        Exception? caught = null;
        var outer = mod.Tasks.Run<int>("outer", async ctx =>
        {
            outerRun = TaskExecutionScope.Current;
            var value = await inner;
            after = TaskExecutionScope.Current;
            afterThread = Environment.CurrentManagedThreadId;
            try
            {
                await failing;
            }
            catch (FormatException e)
            {
                caught = e;
            }
            return value * 2;
        });
        Ended(outer);
        Assert.Equal(18, outer.Result);
        Assert.Same(outerRun, after);
        Assert.Equal("alpha", after!.OwnerName);
        Assert.True(Pool.IsWorker(afterThread));
        Assert.Same(failing.Fault, caught);
    }

    [Fact]
    public void Awaiting_a_task_from_the_tick_comes_back_to_the_tick_and_never_blocks_it()
    {
        var mod = NewMod("alpha");
        using var go = new ManualResetEventSlim();
        var task = mod.Tasks.Run<string>("slow", async ctx =>
        {
            await Task.Yield();
            go.Wait(Long);
            return "found";
        });
        string? got = null;
        var gotThread = 0;
        async void TickCode()
        {
            got = await task;
            gotThread = Environment.CurrentManagedThreadId;
        }
        TickAs(mod, TickCode);  // returns while the task still runs: the tick never waits
        Assert.Null(got);
        go.Set();
        Ended(task);
        Thread.Sleep(20);  // the continuation went to the mod's tick queue, not to the worker that ended the task
        Assert.Null(got);
        TickAs(mod);
        Assert.Equal("found", got);
        Assert.Equal(Pool.TickThreadId, gotThread);
    }

    [Fact]
    public void An_AsTask_fault_is_observed_and_composes()
    {
        var mod = NewMod("alpha");
        var unobserved = 0;
        EventHandler<UnobservedTaskExceptionEventArgs> handler = (_, e) =>
        {
            if (e.Exception.InnerExceptions.Any(x => x.Message == "composed")) Interlocked.Increment(ref unobserved);
        };
        TaskScheduler.UnobservedTaskException += handler;
        try
        {
            var a = mod.Tasks.Run<int>("a", ctx => Task.FromResult(1));
            var b = mod.Tasks.Run("b", ctx => throw new FormatException("composed"));
            var asTask = b.AsTask();  // made before the end
            Ended(a);
            Ended(b);
            var all = Task.WhenAll(a.AsTask(), asTask);
            Assert.Throws<FormatException>(() => all.GetAwaiter().GetResult());
            Assert.Equal(1, a.AsTask().Result);
            _ = mod.Tasks.Run("c", ctx => throw new FormatException("composed")).AsTask();
            Pool.WaitIdle();
            GC.Collect();
            GC.WaitForPendingFinalizers();
            GC.Collect();
            Assert.Equal(0, Volatile.Read(ref unobserved));
        }
        finally
        {
            TaskScheduler.UnobservedTaskException -= handler;
        }
    }
}
