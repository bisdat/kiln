// SPDX-License-Identifier: MIT
namespace Kiln;

/// <summary>What a dispatch asks the host to run (the lease table's <c>KILN_LEASE_*</c>).</summary>
internal enum LeaseWork : uint
{
    /// <summary>The task's next segment on a pool worker: its first, or the one its armed registration resumed.</summary>
    Segment = 1,

    /// <summary>One tick hop the host queued with <see cref="TaskBackend.TickHop"/>, on the server world's tick (or, with
    /// <c>cancelled</c> set, wherever the world's end flushes it).</summary>
    TickHop = 2,
}

/// <summary>How a lease ended (the lease table's <c>finish</c> outcomes).</summary>
internal enum LeaseOutcome : uint
{
    Completed = 0,
    Faulted = 1,

    /// <summary>The task's own token ended it. The backend makes it Cancelled only if a cancel was requested on the lease,
    /// else Faulted (the native precedence).</summary>
    OwnTokenCancelled = 2,
}

/// <summary>Why the backend refused (kiln.tasks' <c>KILN_TASK_REFUSAL_*</c>).</summary>
internal enum TaskRefusal : uint
{
    None = 0,
    QueueFull = 1,
    OwnerSealed = 2,
    OwnerQuarantined = 3,
    Capability = 4,
    Argument = 5,
    Shutdown = 6,
    NoServerWorld = 7,
}

/// <summary>The host's one dispatcher, bound once: <paramref name="hostState"/> is what the host gave
/// <see cref="TaskBackend.Admit"/>. Runs on pool workers (Segment), on the server tick (TickHop) and on the thread that
/// ends the server world (its cancelled hops), possibly on several at once. It never throws back into the backend.</summary>
internal delegate void LeaseDispatch(ulong lease, LeaseWork what, bool cancelled, object hostState);

/// <summary>One row of <see cref="TaskBackend.Snapshot"/>: a lease that is running or suspended.</summary>
internal readonly record struct LeaseRow(ulong Owner, ulong Lease, KilnTaskState State, string Name, TimeSpan Running);

/// <summary>
/// The seam under <see cref="KilnTasks"/>: the C# face of the managed host's lease table on Kiln's one task pool
/// (<c>native/core/src/tasks_host.h</c>, kiln.internal.tasks.host v1), one member per entry. Kiln-internal: not an SDK
/// extension point, never public. The host binds the real pool here; tests bind a fake.
/// </summary>
/// <remarks>
/// <para>A C# task is ONE lease: one record against its owner's limits, admitted like a native submit. Each piece of the
/// method between two <c>await</c>s runs as one <see cref="LeaseWork.Segment"/> on a pool worker; segments of one lease
/// never overlap. A segment that suspends <see cref="Arm"/>s exactly one resumption before it returns, and that
/// registration is later spent once by <see cref="Resume"/> or <see cref="ResumeAfterStep"/>; a suspended lease holds no
/// worker. The segment in which the task ends calls <see cref="Finish"/> instead of arming. Continuations are never
/// refused by the owner's limits or by a seal, so a cancelled task can always finish.</para>
/// <para>Records: the backend drops a lease's record once it is terminal (finished, or cancelled before its first
/// segment); the host never releases one. A lease cancelled before its first segment never gets a dispatch:
/// <see cref="Cancel"/> reports it (<see cref="KilnTaskState.Cancelled"/>). The real pool also tells the host when a
/// lease is quiescent (<see cref="TaskRun.LeaseEnded"/>, the table's <c>bind_ended</c>), which covers the leases a seal,
/// a revoke or a session's end cancelled while they were queued.</para>
/// <para>Misuse (arming twice, resuming a registration that is not armed, finishing outside a segment) is a bug in
/// Kiln: the backend throws <see cref="InvalidOperationException"/> (the table's INTERNAL).</para>
/// </remarks>
internal abstract class TaskBackend
{
    private static readonly object InstallGate = new();
    private static TaskBackend? _current;

    /// <summary>The pool tasks run on in this process; null while none is bound (tasks.pool unavailable).</summary>
    internal static TaskBackend? Current => Volatile.Read(ref _current);

    /// <summary>Binds <paramref name="backend"/> (null unbinds) and hands it the one dispatcher. The host does it once at
    /// boot; tests bind their fake.</summary>
    internal static void Install(TaskBackend? backend)
    {
        lock (InstallGate)
        {
            backend?.Bind(TaskRun.Dispatch);
            Volatile.Write(ref _current, backend);
        }
    }

    /// <summary>Null when tasks can run here, else why tasks.pool is off (the capability row's reason).</summary>
    public abstract string? UnavailableReason { get; }

    /// <summary>True on one of the pool's worker threads.</summary>
    public abstract bool OnPoolThread { get; }

    /// <summary>True on the server world's tick thread, where tick hops run.</summary>
    public abstract bool OnServerTick { get; }

    /// <summary>The live server world's session (a dedicated server, or a client while it hosts); 0 with none (the main
    /// menu, a client joined to another server). Hops and after-step resumptions need one.</summary>
    public abstract ulong ServerSession { get; }

    /// <summary>The table's <c>bind</c>: the one dispatcher (once per process).</summary>
    public abstract void Bind(LeaseDispatch dispatch);

    /// <summary>The table's <c>admit</c>: a new logical task for <paramref name="owner"/>, its first segment queued. Never
    /// blocks. <paramref name="lease"/> is 0 on a refusal.</summary>
    public abstract TaskRefusal Admit(ulong owner, string name, KilnTaskOptions options, object hostState, out ulong lease);

    /// <summary>The table's <c>arm</c>: from inside the lease's running segment, before it returns; exactly one resumption
    /// may follow.</summary>
    public abstract ulong Arm(ulong lease);

    /// <summary>The table's <c>resume</c>: queue that one continuation segment (held until the arming segment returned).
    /// Any thread.</summary>
    public abstract void Resume(ulong lease, ulong registration);

    /// <summary>The table's <c>resume_after_step</c>: resume at a later Post edge of the live server
    /// <paramref name="session"/>, or with the cancellation set as soon as that session ends; never in another session.
    /// <see cref="TaskRefusal.NoServerWorld"/> (the registration stays armed) when <paramref name="session"/> is not the
    /// live one.</summary>
    public abstract TaskRefusal ResumeAfterStep(ulong lease, ulong registration, ulong session);

    /// <summary>The table's <c>tick_hop</c>: one <see cref="LeaseWork.TickHop"/> dispatch on the server tick before the
    /// owner's Pre (bounded per owner and per step; the rest waits a step, never dropped).
    /// <see cref="TaskRefusal.NoServerWorld"/> without a live server world. Any thread.</summary>
    public abstract TaskRefusal TickHop(ulong lease);

    /// <summary>The table's <c>finish</c>: from inside the lease's running segment (not armed), exactly once.</summary>
    public abstract void Finish(ulong lease, LeaseOutcome outcome);

    /// <summary>The table's <c>is_cancelled</c>: a cancel, seal, revoke or session end asked the lease to stop.</summary>
    public abstract bool IsCancelled(ulong lease);

    /// <summary>kiln.tasks' <c>cancel</c> on the lease's handle: a queued lease ends Cancelled at once (and is never
    /// dispatched), a running one has its cancellation set. The state after it.</summary>
    public abstract KilnTaskState Cancel(ulong owner, ulong lease);

    /// <summary>kiln.tasks' <c>status</c> on the lease's handle; <see cref="KilnTaskState.None"/> once its record is
    /// gone.</summary>
    public abstract KilnTaskState Status(ulong owner, ulong lease);

    /// <summary>The table's <c>seal_owner</c> (<c>drain_owner(0)</c>): no new tasks for the owner, its queued ones
    /// cancelled, running ones signalled. Never waits; the number of its leases that are not quiescent yet.</summary>
    public abstract int SealOwner(ulong owner);

    /// <summary>The table's <c>snapshot</c>: the running and suspended leases, for diagnostics. Never blocks on a
    /// task.</summary>
    public abstract IReadOnlyList<LeaseRow> Snapshot();
}
