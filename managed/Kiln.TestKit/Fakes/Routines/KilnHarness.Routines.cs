// SPDX-License-Identifier: MIT
namespace Kiln.TestKit;

public sealed partial class KilnHarness
{
    private HarnessRoutines? _routines;

    /// <summary>The kit's fake kiln.routines of this harness: the game time and the villagers' decisions a test plays,
    /// for the routines a mod assigns with <c>Routines.Assign</c>. Since 0.2.0-dev.28.</summary>
    public HarnessRoutines Routines => _routines ??= new HarnessRoutines(this);
}

/// <summary>
/// The fake kiln.routines of a harness (<see cref="KilnHarness.Routines"/>): Kiln.Blocks' own code (<c>Routines.Assign</c>,
/// <c>Remove</c>, <c>Resume</c>, <c>Inspect</c>, <c>RoutineAssignmentChangedEvent</c>, <c>RoutineTaskChangedEvent</c>)
/// runs on it. Since 0.2.0-dev.28.
/// </summary>
/// <remarks>
/// <para><b>Modelled.</b> One routine per villager (its NpcId): the first mod that assigns one holds it, another mod's
/// change completes <c>OwnedBy:&lt;mod&gt;</c>. A change is queued at the call, committed at the start of the next step and
/// completed once in that step; 256 changes wait at most (then <c>QueueFull</c>). Revisions count the world's changes.
/// A mod that unloads loses its routines at the next step. The harness's end stops the world: queued changes complete
/// <c>WorldStopped</c> and every assignment gets its teardown event.</para>
/// <para><b>Played by the test.</b> Nothing chooses by itself: <see cref="Decide"/> is the villager reaching a decision
/// at the game time of <see cref="SetClock"/>, <see cref="Finish"/> the game ending its task, <see cref="SetSleepAllowed"/>
/// what the game's own sleep check answers.</para>
/// <para><b>Not modelled</b> (offline): the game's villager worker, its chooser, the walk, the seat and the bed; manual
/// suspension by <c>Villager.GoTo</c> and the others; restoring assignments after a restart.</para>
/// </remarks>
public sealed class HarnessRoutines
{
    private readonly KilnHarness _harness;

    internal HarnessRoutines(KilnHarness harness) => _harness = harness;

    private RoutinesState State => RoutinesFake.StateOf(_harness);

    /// <summary>Sets the game time the villagers' decisions read.</summary>
    public void SetClock(int hour, int minute = 0)
    {
        ArgumentOutOfRangeException.ThrowIfNegative(hour);
        ArgumentOutOfRangeException.ThrowIfNegative(minute);
        State.With(m =>
        {
            m.SetClock(hour, minute);
            return 0;
        });
    }

    /// <summary>The villager <paramref name="npcId"/> reaches a decision. Returns the task its routine took
    /// (<c>vanilla</c>, <c>idle</c>, <c>roam</c>, <c>sleep</c>, <c>sit</c>, <c>inspect</c>), or null when the game chose
    /// (no routine, a suspended one). The task events come with the next step.</summary>
    public string? Decide(uint npcId) =>
        State.With(m => m.Decide(npcId) is { } kind ? kind.ToString().ToLowerInvariant() : null);

    /// <summary>The game ends the villager's current task state: done (<paramref name="ok"/>) or failed (no seat, no
    /// path; <paramref name="reason"/>, default <c>TaskFailed</c>).</summary>
    public void Finish(uint npcId, bool ok, string? reason = null) =>
        State.With(m =>
        {
            m.Finish(npcId, ok, reason);
            return 0;
        });

    /// <summary>What the game's own sleep check answers for the villager: whether its sleep window lets it go to bed now.
    /// A routine's sleep task is refused (<c>NativeSleepNotCurrentlyAllowed</c>) while it is false.</summary>
    public void SetSleepAllowed(uint npcId, bool allowed) =>
        State.With(m =>
        {
            m.Villager(npcId).NativeSleepAllowed = allowed;
            return 0;
        });

    /// <summary>How many changes wait for the next step.</summary>
    public int Queued => State.With(m => m.Queued);
}
