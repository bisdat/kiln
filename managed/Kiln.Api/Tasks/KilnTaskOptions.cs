// SPDX-License-Identifier: MIT
namespace Kiln;

/// <summary>Where a Kiln task stands (<see cref="KilnTask.State"/>). The values are kiln.tasks' states
/// (<c>kiln_tasks.h</c>).</summary>
public enum KilnTaskState
{
    /// <summary>Not a task's state: kiln.tasks' "no such task". A <see cref="KilnTask"/> never reports it.</summary>
    None = 0,

    /// <summary>Accepted, waiting for a worker.</summary>
    Queued = 1,

    /// <summary>Started: running on a worker or on the tick, or suspended at an <c>await</c>.</summary>
    Running = 2,

    /// <summary>Ended normally: the work returned, and everything it posted to the tick was applied.</summary>
    Completed = 3,

    /// <summary>Ended with an exception (<see cref="KilnTask.Fault"/>): the work threw, or something it posted to the tick
    /// did.</summary>
    Faulted = 4,

    /// <summary>Ended by its cancellation: cancelled before it started, ended by an
    /// <see cref="OperationCanceledException"/> of its own <see cref="KilnTaskContext.Cancellation"/>, or work it posted to
    /// the tick could not be applied.</summary>
    Cancelled = 5,
}

/// <summary>A task's place in its mod's own queue. It orders only that mod's queued tasks; it never gives a mod a bigger
/// share of the pool or raises a thread's OS priority.</summary>
public enum KilnTaskPriority
{
    /// <summary>The default.</summary>
    Normal = 0,

    /// <summary>After the mod's Normal and High tasks.</summary>
    Low = 1,

    /// <summary>Before the mod's Normal and Low tasks.</summary>
    High = 2,
}

/// <summary>How <see cref="KilnTasks.Run(string, Action{KilnTaskContext}, KilnTaskOptions?)"/> runs a task.</summary>
/// <param name="LongRunning">The task gets a dedicated worker instead of one of the shared ones: for work that runs for
/// seconds or loops until cancelled. A mod has at most 2 of them at once, Kiln at most 8; more wait their turn.</param>
/// <param name="Priority">The task's place in the mod's own queue.</param>
public readonly record struct KilnTaskOptions(bool LongRunning = false, KilnTaskPriority Priority = KilnTaskPriority.Normal);

/// <summary>The mod already holds as many tasks as Kiln allows one mod (1024 that have not ended). Thrown by
/// <see cref="KilnTasks"/>' <c>Run</c> for exactly that reason and no other; nothing was started. Kiln never drops a
/// mod's older tasks to make room.</summary>
public sealed class KilnTaskQueueFullException : InvalidOperationException
{
    internal KilnTaskQueueFullException(string message) : base(message) { }
}
