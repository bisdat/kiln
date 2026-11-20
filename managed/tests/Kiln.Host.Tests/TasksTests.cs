// SPDX-License-Identifier: MIT
// The host bridge's binding and lease lifecycle: the real backend (HostTasks) on a fake of the core's task tables.
// What it checks before it calls the table, how a task maps onto a lease, that every lease ends exactly once, that a
// lease the core cancelled before its first segment still ends its task, and that the pins are gone afterwards.
using Kiln.Host;
using Xunit;

// The task bodies stand in for mod code (their own delays, no test token); the test thread is the mod's own thread.
#pragma warning disable xUnit1031, xUnit1051

namespace Kiln.Host.Tests;

public sealed class TasksTests : HostTaskTest
{
    // ---- the table is checked before it is used ----

    [Fact]
    public void A_matching_table_is_bound_once_and_tasks_run()
    {
        Assert.Null(Tasks.UnavailableReason);
        Assert.Equal(1, Native.Binds);
        Assert.Equal(1, Native.EndedBinds);
        Assert.Same(Tasks, TaskBackend.Current);
        var mod = NewMod("alpha");
        var task = mod.Tasks.Run("first", _ => { });
        Ended(task);
        Assert.Equal(KilnTaskState.Completed, task.State);
        Quiesce();
    }

    [Fact]
    public void A_core_without_the_table_leaves_tasks_off_with_the_reason()
    {
        Use(2, offer: false);
        Assert.Contains("kiln.internal.tasks.host", Tasks.UnavailableReason);
        Assert.Equal(0, Native.Binds + Native.EndedBinds);
        var e = Assert.Throws<KilnUnavailableException>(() => NewMod("alpha").Tasks.Run("x", _ => { }));
        Assert.Contains("tasks.pool", e.Message);
        Assert.Contains("kiln.internal.tasks.host", e.Message);
        Assert.True(Host.Has("C# tasks are off"));
        Assert.Equal(0, Tasks.LivePins);
    }

    [Fact]
    public void A_short_table_is_refused_and_nothing_of_it_is_called()
    {
        Use(2, n => n.SetHeader(size: 96));  // the table before its last four members
        Assert.Contains("shorter", Tasks.UnavailableReason);
        Assert.Equal(0, Native.Binds + Native.EndedBinds);
        Assert.Throws<KilnUnavailableException>(() => NewMod("alpha").Tasks.Run("x", _ => { }));
    }

    [Fact]
    public void Another_version_is_refused()
    {
        Use(2, n => n.SetHeader(version: 2));
        Assert.Contains("version 2", Tasks.UnavailableReason);
        Assert.Equal(0, Native.Binds + Native.EndedBinds);
    }

    [Fact]
    public void Another_build_is_refused_and_both_stamps_are_named()
    {
        Use(2, coreStamp: "0.2.0-test+20260102.1234567");
        Assert.Contains("0.2.0-test+20260102.1234567", Tasks.UnavailableReason);
        Assert.Contains(FakeNativeTasks.Stamp, Tasks.UnavailableReason);
        Assert.Equal(0, Native.Binds + Native.EndedBinds);
        Assert.Throws<KilnUnavailableException>(() => NewMod("alpha").Tasks.Run("x", _ => { }));
    }

    [Fact]
    public void A_core_or_a_host_without_a_build_stamp_is_refused()
    {
        Use(2, coreStamp: "");
        Assert.Contains("without a build stamp", Tasks.UnavailableReason);
        Assert.Equal(0, Native.Binds + Native.EndedBinds);
        Use(2, ownStamp: null);
        Assert.Contains("Kiln.Api was built without a build stamp", Tasks.UnavailableReason);
        Assert.Equal(0, Native.Binds + Native.EndedBinds);
    }

    [Fact]
    public void A_table_with_an_empty_member_is_refused()
    {
        Use(2, n => n.ClearTickHop());
        Assert.Contains("empty member", Tasks.UnavailableReason);
        Assert.Equal(0, Native.Binds + Native.EndedBinds);
    }

    [Fact]
    public void An_unavailable_backend_answers_the_facts_without_touching_the_table()
    {
        Use(2, offer: false);
        Assert.False(Tasks.OnPoolThread);
        Assert.False(Tasks.OnServerTick);
        Assert.Equal(0ul, Tasks.ServerSession);
        Assert.Empty(Tasks.Snapshot());
        Assert.Equal(0, Tasks.SealOwner(7));
        Assert.Equal(0, Tasks.RetireOwner(7));
    }

    // ---- a task is one lease ----

    [Fact]
    public void Admission_carries_the_name_the_options_and_the_owner()
    {
        var mod = NewMod("alpha");
        var gate = new ManualResetEventSlim();
        var a = mod.Tasks.Run("plain", _ => gate.Wait());
        var b = mod.Tasks.Run("long one", _ => gate.Wait(), new KilnTaskOptions(LongRunning: true, Priority: KilnTaskPriority.High));
        var c = mod.Tasks.Run("späte Aufgabe", _ => { }, new KilnTaskOptions(Priority: KilnTaskPriority.Low));
        gate.Set();
        Ended(a);
        Ended(b);
        Ended(c);
        Assert.Equal([(mod.Owner, "plain", 0u, 0u), (mod.Owner, "long one", 1u, 2u), (mod.Owner, "späte Aufgabe", 0u, 1u)], Native.Admitted);
        Quiesce();
    }

    [Fact]
    public void A_refused_admission_holds_no_pin_and_names_the_refusal()
    {
        Native.OwnerCapacity = 2;
        var mod = NewMod("alpha");
        var gate = new TaskCompletionSource();
        var a = mod.Tasks.Run("a", async _ => await gate.Task);
        var b = mod.Tasks.Run("b", async _ => await gate.Task);
        Native.WaitIdle();
        Assert.Equal(2, Tasks.LivePins);
        Assert.Throws<KilnTaskQueueFullException>(() => mod.Tasks.Run("c", _ => { }));
        Assert.Equal(2, Tasks.LivePins);  // the refused one left nothing
        Assert.Equal(2, Native.Admitted.Count);
        gate.SetResult();
        Ended(a);
        Ended(b);
        Quiesce();
    }

    [Fact]
    public void Every_lease_ends_once_and_its_record_and_pin_go_with_it()
    {
        var mod = NewMod("alpha");
        var done = mod.Tasks.Run("done", _ => { });
        var faulted = mod.Tasks.Run("faulted", _ => throw new InvalidOperationException("boom"));
        var cancelled = mod.Tasks.Run("cancelled", async ctx => await Task.Delay(Timeout.Infinite, ctx.Cancellation));
        Native.WaitIdle();
        cancelled.Cancel();
        Ended(done);
        Ended(faulted);
        Ended(cancelled);
        Quiesce();
        Assert.Equal(KilnTaskState.Completed, done.State);
        Assert.Equal(KilnTaskState.Faulted, faulted.State);
        Assert.Equal(KilnTaskState.Cancelled, cancelled.State);
        lock (Native.Ends)
        {
            Assert.Equal(3, Native.Ends.Count);
            Assert.Equal(3, Native.Ends.Select(e => e.Lease).Distinct().Count());
            Assert.Equal([FakeNativeTasks.Completed, FakeNativeTasks.Faulted, FakeNativeTasks.Cancelled], Native.Ends.Select(e => e.State).Order());
        }
        Assert.Equal(1, Host.Count("task 'faulted' faulted"));
    }

    [Fact]
    public void A_suspended_task_holds_no_worker()
    {
        Use(1);  // one worker: a task parked on an await must not hold it
        var mod = NewMod("alpha");
        var gate = new TaskCompletionSource<int>(TaskCreationOptions.RunContinuationsAsynchronously);
        var parked = mod.Tasks.Run("parked", async _ => await gate.Task);
        Native.WaitIdle();
        Assert.Equal(KilnTaskState.Running, parked.State);
        var other = mod.Tasks.Run("other", _ => 0 is 0 ? Task.FromResult(7) : null!);
        Ended(other);
        Assert.Equal(7, other.Result);
        Assert.Equal(KilnTaskState.Running, parked.State);
        gate.SetResult(42);
        Ended(parked);
        Assert.Equal(42, parked.Result);
        Quiesce();
    }

    // ---- a lease cancelled before its first segment ----

    [Fact]
    public void A_task_the_core_cancelled_while_queued_ends_Cancelled_through_the_end_notification()
    {
        Use(1);
        var mod = NewMod("alpha");
        var hold = new ManualResetEventSlim();
        var blocker = NewMod("beta").Tasks.Run("blocker", _ => hold.Wait());
        Assert.True(Until(() => blocker.State == KilnTaskState.Running));
        var ran = false;
        var queued = mod.Tasks.Run("queued", _ => ran = true);
        Assert.Equal(KilnTaskState.Queued, queued.State);
        Native.RetireOwner(mod.Owner);  // the core retires the owner (a revoke): the host made no call for it
        Thread.Sleep(30);
        Assert.Equal(KilnTaskState.Queued, queued.State);  // its end comes from a worker, never inline on the retiring thread
        hold.Set();
        Ended(queued);
        Ended(blocker);
        Assert.Equal(KilnTaskState.Cancelled, queued.State);
        Assert.False(ran);
        Assert.False(Native.LeaseNamed("queued")?.Dispatched ?? false);
        Quiesce();
    }

    [Fact]
    public void Cancel_on_a_queued_task_ends_it_at_once_and_its_pin_goes_with_the_end_notification()
    {
        Use(1);
        var mod = NewMod("alpha");
        var hold = new ManualResetEventSlim();
        var blocker = mod.Tasks.Run("blocker", _ => hold.Wait());
        Assert.True(Until(() => blocker.State == KilnTaskState.Running));
        var queued = mod.Tasks.Run("queued", _ => { });
        Assert.Equal(2, Tasks.LivePins);
        queued.Cancel();
        Assert.Equal(KilnTaskState.Cancelled, queued.State);
        Assert.Equal(2, Tasks.LivePins);  // held until the core says nothing of it runs any more
        hold.Set();
        Ended(blocker);
        Quiesce();
    }

    // ---- retiring an owner ----

    [Fact]
    public void Retiring_an_owner_cancels_its_tasks_and_their_finalization_still_runs()
    {
        var mod = NewMod("alpha");
        var other = NewMod("beta");
        var unwound = 0;
        int finallyThread = 0;
        var waits = mod.Tasks.Run("waits", async ctx =>
        {
            try
            {
                await Task.Delay(Timeout.Infinite, ctx.Cancellation);
            }
            finally
            {
                await Task.Yield();  // a continuation after the seal: never refused
                finallyThread = Environment.CurrentManagedThreadId;
                Interlocked.Increment(ref unwound);
            }
        });
        var bystander = other.Tasks.Run("bystander", async ctx => await Task.Delay(Timeout.Infinite, ctx.Cancellation));
        Native.WaitIdle();
        Host.OwnerRevoke(mod.Owner);
        Ended(waits);
        Assert.Equal(KilnTaskState.Cancelled, waits.State);
        Assert.Equal(1, unwound);
        Assert.True(Native.IsWorker(finallyThread), "the finalization ran on a pool worker");
        Assert.Equal(KilnTaskState.Running, bystander.State);  // another owner's task is untouched
        var e = Assert.ThrowsAny<Exception>(() => mod.Tasks.Run("late", _ => { }));
        Assert.True(e is InvalidOperationException or KilnUnavailableException, e.ToString());
        bystander.Cancel();
        Ended(bystander);
        Quiesce();
    }

    [Fact]
    public void Continuations_are_never_refused_by_the_owners_capacity()
    {
        Native.OwnerCapacity = 4;
        var mod = NewMod("alpha");
        var gate = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var finished = 0;
        var tasks = Enumerable.Range(0, 4).Select(i => mod.Tasks.Run("t" + i, async _ =>
        {
            await gate.Task;
            await Task.Yield();
            Interlocked.Increment(ref finished);
        })).ToList();
        Native.WaitIdle();
        Assert.Throws<KilnTaskQueueFullException>(() => mod.Tasks.Run("fresh", _ => { }));  // no way around the cap for a new task
        gate.SetResult();
        foreach (var t in tasks) Ended(t);
        Assert.Equal(4, finished);
        Assert.All(tasks, t => Assert.Equal(KilnTaskState.Completed, t.State));
        Quiesce();
        Ended(mod.Tasks.Run("fresh", _ => { }));  // the ended ones no longer count
        Quiesce();
    }

    // ---- diagnostics ----

    [Fact]
    public void Snapshot_lists_the_live_leases()
    {
        var mod = NewMod("alpha");
        var gate = new TaskCompletionSource();
        var task = mod.Tasks.Run("listed", async _ => await gate.Task);
        Native.WaitIdle();
        var row = Assert.Single(Tasks.Snapshot());
        Assert.Equal((mod.Owner, "listed", KilnTaskState.Running, TimeSpan.Zero), (row.Owner, row.Name, row.State, row.Running));
        Assert.Equal(task.Run.Lease, row.Lease);
        gate.SetResult();
        Ended(task);
        Quiesce();
        Assert.Empty(Tasks.Snapshot());
    }

    // ---- Kiln's own components ----

    [Fact]
    public void A_component_runs_one_long_running_task_under_its_own_owner()
    {
        var owners = new Dictionary<string, ulong>();
        ComponentTask.Owners = name => owners.TryGetValue(name, out var o) ? o : owners[name] = Host.OwnerCreate(name);
        var mod = NewMod("alpha");
        var jobs = new System.Collections.Concurrent.BlockingCollection<int>();
        var seen = new List<(int Job, ModContext? Mod, bool Pool)>();
        var writer = ComponentTask.Create("kiln.test-writer", "writer", ctx =>
        {
            try
            {
                foreach (var job in jobs.GetConsumingEnumerable(ctx.Cancellation))
                    lock (seen) seen.Add((job, ModContext.CurrentMod, TaskBackend.Current!.OnPoolThread));
            }
            catch (OperationCanceledException)
            {
            }
        });
        Assert.Null(writer.Running);
        var task = writer.Start();
        Assert.Same(task, writer.Start());  // one runner however often it is asked
        jobs.Add(1);
        jobs.Add(2);
        Assert.True(Until(() => { lock (seen) return seen.Count == 2; }));
        var admitted = Assert.Single(Native.Admitted);
        Assert.Equal((owners["kiln.test-writer"], "writer", 1u), (admitted.Owner, admitted.Name, admitted.Flags));
        Assert.NotEqual(mod.Owner, admitted.Owner);
        lock (seen) Assert.All(seen, s => Assert.True(s.Mod is null && s.Pool));  // no mod's identity, on the pool
        Assert.True(writer.Stop(Long));
        Assert.True(task.IsTerminal);
        Quiesce();
        Assert.Same(ComponentTask.Create("kiln.test-writer", "writer", _ => { }).GetType(), writer.GetType());
        Assert.Single(owners);  // the same component gets the same owner
    }

    [Fact]
    public void Without_a_host_a_component_task_is_an_honest_refusal()
    {
        ComponentTask.Owners = null;
        var e = Assert.Throws<KilnUnavailableException>(() => ComponentTask.Create("kiln.test-writer", "writer", _ => { }));
        Assert.Contains("tasks.pool", e.Message);
    }
}
