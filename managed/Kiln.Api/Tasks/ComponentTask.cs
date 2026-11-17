// SPDX-License-Identifier: MIT
namespace Kiln;

/// <summary>
/// Kiln's own components only (friend assemblies, never mods, never public): ONE long-running task under the component's
/// own owner, on the same pool and limits as the mods' tasks, so the component's worker is counted, named and cancelled
/// like theirs instead of being a raw thread. It starts lazily (<see cref="Start"/>, at the first job), at most one runs at
/// a time, and <see cref="Stop"/> cancels it and waits a bounded time, off the tick.
/// </summary>
internal sealed class ComponentTask
{
    private readonly object _gate = new();
    private readonly TaskRun.Owner _owner;
    private readonly string _name;
    private readonly Action<KilnTaskContext> _work;
    private KilnTask? _task;

    /// <param name="owner">The component's own native owner (not a mod's); never 0.</param>
    /// <param name="component">The component, for logs and diagnostics (e.g. <c>kiln.saves</c>).</param>
    /// <param name="name">The task's name (the task name rule of <see cref="KilnTasks"/>).</param>
    /// <param name="work">The runner: loops until its <see cref="KilnTaskContext.Cancellation"/> is signalled.</param>
    internal ComponentTask(ulong owner, string component, string name, Action<KilnTaskContext> work)
    {
        if (owner == 0) throw new ArgumentException("a component task needs its component's own owner", nameof(owner));
        ArgumentException.ThrowIfNullOrEmpty(component);
        TaskRun.ValidateName(name);
        ArgumentNullException.ThrowIfNull(work);
        _owner = new TaskRun.Owner(null, owner, component);
        _name = name;
        _work = work;
    }

    /// <summary>The runner while it has not ended; null before the first <see cref="Start"/> and after it ended.</summary>
    internal KilnTask? Running
    {
        get
        {
            lock (_gate) return _task is { IsTerminal: false } t ? t : null;
        }
    }

    /// <summary>Starts the runner unless one is running (then returns that one): one admission however many threads ask.
    /// After the runner has ended, the next call starts a new one. Any thread; never blocks.</summary>
    /// <exception cref="KilnUnavailableException">Kiln's task pool is not available (tasks.pool).</exception>
    internal KilnTask Start()
    {
        lock (_gate)
        {
            if (_task is { IsTerminal: false } running) return running;
            _task = TaskRun.Start(_owner, _name, _work, new KilnTaskOptions(LongRunning: true), new KilnTask(_name));
            return _task;
        }
    }

    /// <summary>Cancels the runner and waits up to <paramref name="timeout"/> for it to end; true when it ended (or none
    /// ran). A runner that ignores its cancellation is left running and reported by the pool's watchdog, never
    /// aborted.</summary>
    /// <exception cref="InvalidOperationException">On the tick, a Kiln task worker or inside a task: they never
    /// wait.</exception>
    internal bool Stop(TimeSpan timeout)
    {
        if (TaskRun.BlockingRefused() is { } why) throw new InvalidOperationException(why);
        KilnTask? task;
        lock (_gate) task = _task;
        if (task is null) return true;
        task.Cancel();
        return task.WaitEnded(timeout);
    }
}
