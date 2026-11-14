// SPDX-License-Identifier: MIT
// A villager's day plan as a block: time windows of the game day, each with one of the game's own villager tasks, and a
// fallback. The game's code still walks, sits, finds the bed and plays the animations; the plan only says which task the
// villager's chooser takes at its next decision. docs/kiln/services/routines.md.
using System.Text.Json.Serialization;

namespace Kiln;

/// <summary>
/// A day plan for a villager: <see cref="Windows"/> of the game day, each with a task, and the <see cref="Fallback"/> for the
/// minutes no window covers. Define it once (<c>Blocks.Define("day", new RoutineBlock { ... })</c>, or a block of the
/// <c>routines</c> section of blocks/*.json) and bind the handle to a villager. It asks nothing of players' games: only the
/// server chooses tasks.
/// <para>At each decision the villager takes the window with the highest <see cref="RoutineWindow.Priority"/> that covers the
/// game minute; equal priorities go to the window written first. A plan that changes keeps its identity when it says
/// <see cref="Was"/> after a rename, and says a higher <see cref="Version"/> when its content changes: a saved assignment
/// follows a higher version, and refuses a changed plan of the same version.</para>
/// </summary>
/// <remarks>What is wrong with a plan throws <see cref="BlockError"/> at <c>Blocks.Define</c>: more than 64 windows, a minute
/// outside 0..1439, a window whose start equals its end without <see cref="RoutineWindow.AllDay"/>, a <see cref="Version"/> of
/// 0, a <see cref="Was"/> that is no local name, or a <see cref="Fallback"/> other than <see cref="VanillaTask"/>,
/// <see cref="IdleTask"/> or <see cref="RoamTask"/>.</remarks>
[BlockClients(ClientsLevel.None)]
public sealed record RoutineBlock : INpcBlock
{
    /// <summary>The version of the plan's content, 1 or more. Raise it when the windows or the fallback change: an assignment
    /// saved with a lower version moves to this one; one saved with the same version and other content is refused
    /// (<c>VersionNotBumped</c>) and stays suspended.</summary>
    public uint Version { get; init; } = 1;
    /// <summary>The block name this plan had before a rename: its identity stays <c>&lt;mod&gt;:&lt;Was&gt;</c>, so saved
    /// assignments still find it. A local name (letters, digits, '-', '_', '.'), never another mod's.</summary>
    public string? Was { get; init; }
    /// <summary>The windows of the day, at most 64.</summary>
    public IReadOnlyList<RoutineWindow> Windows { get; init; } = [];
    /// <summary>The task for a minute no window covers, and after a <see cref="SleepTask"/> the game does not allow now:
    /// <see cref="VanillaTask"/> (the default), <see cref="IdleTask"/> or <see cref="RoamTask"/>.</summary>
    public RoutineTask Fallback { get; init; } = new VanillaTask();
}

/// <summary>One window of a <see cref="RoutineBlock"/>: from <paramref name="StartMinute"/> up to, not including,
/// <paramref name="EndMinute"/>, in minutes of the game day (0..1439; 0 is midnight). A start after the end runs over
/// midnight (<c>new(1320, 360, new SleepTask())</c> is 22:00 to 06:00). Equal start and end is refused, except 0 and 0 with
/// <paramref name="AllDay"/>, which covers the whole day.</summary>
/// <param name="StartMinute">The first minute of the window.</param>
/// <param name="EndMinute">The first minute after the window.</param>
/// <param name="Task">What the villager does in it.</param>
/// <param name="Priority">Among the windows that cover a minute the highest wins; equal ones go to the window written first.</param>
/// <param name="AllDay">The whole day; then both minutes are 0.</param>
public sealed record RoutineWindow(int StartMinute, int EndMinute, RoutineTask Task, int Priority = 0, bool AllDay = false);

/// <summary>What a villager does in a window: one of the game's own villager tasks. In JSON <c>{ "kind": "idle" }</c> with
/// <c>vanilla</c>, <c>idle</c>, <c>roam</c>, <c>sleep</c>, <c>sit</c> or <c>inspect</c>. A task takes no values in this
/// version: how long an idle lasts and how far a roam goes are the villager's own, and JSON that names a duration or a
/// radius is refused.</summary>
[JsonConverter(typeof(RoutineTaskConverter))]
public abstract record RoutineTask
{
    // the kinds are Kiln's: a mod cannot add one
    private protected RoutineTask() { }
}

/// <summary>The game's own choice: its sleep check, its social targets and its weighted pick run as without a plan.</summary>
public sealed record VanillaTask : RoutineTask;

/// <summary>Stand idle, for the time the game's villager rolls for an idle.</summary>
public sealed record IdleTask : RoutineTask;

/// <summary>Walk to a random spot around home, by the villager's own roaming range.</summary>
public sealed record RoamTask : RoutineTask;

/// <summary>Go to the assigned bed and sleep, when the game lets this villager sleep now (its own sleep and wake times).
/// Outside those times the task is refused with <c>NativeSleepNotCurrentlyAllowed</c> and the plan's fallback runs: a plan
/// cannot force sleep by day.</summary>
public sealed record SleepTask : RoutineTask;

/// <summary>Find a seat nearby, walk to it and sit.</summary>
public sealed record SitTask : RoutineTask;

/// <summary>Find something nearby to look at or work on, walk to it and interact (the game's "inspect").</summary>
public sealed record InspectTask : RoutineTask;

/// <summary>The answer to a change of a villager's routine (assign, remove, resume). <see cref="Ok"/> says the assignment
/// changed (or already was as asked), not that a task has run: what the villager's tasks do comes as task events.</summary>
/// <param name="Ok">True when the change happened or was already in effect.</param>
/// <param name="Reason">Why not: <c>OwnedBy:&lt;mod&gt;</c> (another mod's routine is on this villager), <c>WorldStopped</c>,
/// <c>Revoked</c>, <c>QueueFull</c>. Null when <paramref name="Ok"/>.</param>
/// <param name="NpcId">The villager.</param>
/// <param name="Revision">The assignment's revision after the change: it rises with every change of the assignment and never
/// falls. Unchanged (the last one) when nothing changed.</param>
public sealed record RoutineResult(bool Ok, string? Reason, uint NpcId, ulong Revision) : IKilnResult;

/// <summary>Where a villager's routine assignment stands.</summary>
public enum RoutineAssignmentState
{
    /// <summary>Bound and choosing tasks.</summary>
    Assigned,
    /// <summary>A manual command (go to, teleport, stay, follow) took the villager: the routine waits for an explicit resume.</summary>
    ManualSuspended,
    /// <summary>Saved, and the villager is not in the world now (not spawned yet, or the world is closing).</summary>
    PendingSpawn,
    /// <summary>The saved assignment does not fit the plan as defined now (changed without a higher version, or a lower
    /// version): it stays suspended until the plan is fixed or assigned again.</summary>
    VersionMismatch,
    /// <summary>Removed.</summary>
    Removed,
}

/// <summary>Where one task of a routine stands.</summary>
public enum RoutineTaskState
{
    /// <summary>No task.</summary>
    None,
    /// <summary>Chosen at a decision; the game has not entered its state yet.</summary>
    Chosen,
    /// <summary>The game entered the task's first state.</summary>
    Started,
    /// <summary>The task's last state finished.</summary>
    Completed,
    /// <summary>The game refused or failed it (no seat, no bed, no path, sleep not allowed now).</summary>
    Refused,
    /// <summary>Interrupted: water, a build cursor, a manual command, or the world closing.</summary>
    Suspended,
}

/// <summary>A villager's routine at one moment.</summary>
/// <param name="NpcId">The villager.</param>
/// <param name="Owner">The mod whose plan it is.</param>
/// <param name="Key">The plan's identity, <c>&lt;mod&gt;:&lt;name&gt;</c>.</param>
/// <param name="PlanVersion">The plan's <see cref="RoutineBlock.Version"/>.</param>
/// <param name="Revision">The assignment's revision.</param>
/// <param name="Assignment">Where the assignment stands.</param>
/// <param name="WindowIndex">The window of the current task; -1: the fallback, or no task.</param>
/// <param name="Invocation">The serial of the current task: each task the routine starts has the next one.</param>
/// <param name="Task">The current task, or null.</param>
/// <param name="TaskState">Where it stands.</param>
/// <param name="NativeState">The game's state of the villager.</param>
/// <param name="LastReason">The last refusal or suspension, or null.</param>
public sealed record RoutineSnapshot(uint NpcId, string Owner, string Key, uint PlanVersion, ulong Revision, RoutineAssignmentState Assignment,
    int WindowIndex, ulong Invocation, RoutineTask? Task, RoutineTaskState TaskState, VillagerState NativeState, string? LastReason);
