// SPDX-License-Identifier: MIT
// The two events of villager routines. kiln.routines sends them on Post, on the tick thread; when a world stops they
// come with Teardown set, from the thread that stops it, or at the start of the next step when that is not the tick
// thread: a handler must not assume the world is still there then.
namespace Kiln;

/// <summary>A villager's routine assignment changed: assigned, replaced, suspended by a manual command, resumed, or
/// removed. One event per change, with the snapshot before (null for a new assignment) and after. Posted after the
/// step the change was committed in; observe only.</summary>
/// <remarks>When the world stops, every assignment gets one event with <see cref="Teardown"/> true and
/// <see cref="After"/> in state <see cref="RoutineAssignmentState.PendingSpawn"/>; it comes while the world stops or at
/// the start of the next step: do not use the world (villagers, entities) in a handler when <see cref="Teardown"/> is set.</remarks>
[ServerOnly, AnyThread]
public sealed class RoutineAssignmentChangedEvent : KilnEvent
{
    internal RoutineAssignmentChangedEvent(RoutineSnapshot? before, RoutineSnapshot after, string reason, bool teardown)
    {
        Before = before;
        After = after;
        Reason = reason;
        Teardown = teardown;
    }

    /// <summary>The assignment before the change; null when the villager had none.</summary>
    public RoutineSnapshot? Before { get; }
    /// <summary>The assignment after it (state <see cref="RoutineAssignmentState.Removed"/> for a removal).</summary>
    public RoutineSnapshot After { get; }
    /// <summary>What happened: <c>Assigned</c>, <c>Replaced</c>, <c>ManualSuspended</c> (a Reset or GoTo command reached the
    /// villager), <c>Resumed</c>, <c>Removed</c>, <c>OwnerUnloaded</c>, <c>WorldStopped</c>; at a world's start after a restart
    /// <c>Migrated</c> (the plan's Version was raised), <c>VersionNotBumped</c> or <c>Downgrade</c> (refused, the assignment kept
    /// as <see cref="RoutineAssignmentState.VersionMismatch"/>), <c>OwnerMissing</c>, <c>DefinitionMissing</c> or
    /// <c>OtherWorld</c> (removed). A plain rebind after a restart sends no event.</summary>
    public string Reason { get; }
    /// <summary>The world is stopping: see the remarks.</summary>
    public bool Teardown { get; }

    public override string ToString() => $"routine of villager 0x{After.NpcId:x8} ({After.Key}): {Before?.Assignment.ToString() ?? "none"} -> {After.Assignment} ({Reason})";
}

/// <summary>A task of a villager's routine changed state: chosen at a decision, started, completed, refused by the game
/// (no seat, no bed, sleep not allowed now), or suspended. What tasks do is reported here only: the answer to
/// <c>Routines.Assign</c> says nothing about them. Observe only.</summary>
/// <remarks>The same threading as <see cref="RoutineAssignmentChangedEvent"/>: on the tick thread after the step, except
/// with <see cref="Teardown"/> set. Unlike assignment events, task events can be dropped under load: the service keeps
/// 16 task changes per villager and step, and when a villager changes its task more often in one step the newest is
/// kept and the ones in between are lost (kiln.log counts them). Do not count on seeing every intermediate state.</remarks>
[ServerOnly, AnyThread]
public sealed class RoutineTaskChangedEvent : KilnEvent
{
    internal RoutineTaskChangedEvent(uint npcId, string key, uint planVersion, ulong revision, ulong invocation, RoutineTask? task, RoutineTaskState from,
        RoutineTaskState to, string? reason, bool teardown)
    {
        NpcId = npcId;
        Key = key;
        PlanVersion = planVersion;
        Revision = revision;
        Invocation = invocation;
        Task = task;
        From = from;
        To = to;
        Reason = reason;
        Teardown = teardown;
    }

    /// <summary>The villager.</summary>
    public uint NpcId { get; }
    /// <summary>The plan's identity, <c>&lt;mod&gt;:&lt;name&gt;</c>.</summary>
    public string Key { get; }
    public uint PlanVersion { get; }
    /// <summary>The assignment's revision when the change was reported.</summary>
    public ulong Revision { get; }
    /// <summary>The task's serial: every task a routine starts has its own.</summary>
    public ulong Invocation { get; }
    /// <summary>The task; null when there is none.</summary>
    public RoutineTask? Task { get; }
    public RoutineTaskState From { get; }
    public RoutineTaskState To { get; }
    /// <summary>Why it was refused or suspended (<c>NativeSleepNotCurrentlyAllowed</c>, <c>TaskFailed</c>, <c>NoTransition</c>,
    /// <c>SafetyInterrupt</c>, <c>Interrupted</c>, <c>Replaced</c>, <c>Removed</c>, <c>WorldStopped</c>); else null.</summary>
    public string? Reason { get; }
    /// <summary>The world is stopping.</summary>
    public bool Teardown { get; }

    public override string ToString() => $"routine task {Invocation} of villager 0x{NpcId:x8} ({Key}): {From} -> {To}{(Reason is null ? "" : $" ({Reason})")}";
}
