// SPDX-License-Identifier: MIT
using System.Diagnostics;

namespace Kiln.Host;

/// <summary>
/// The frame a task's tick hop runs in on the server tick: <c>ToTick</c>, <c>OnTick</c> and <c>PostToTick</c> work, and
/// tick continuations of a task. The core queues the hops and dispatches them before the Pre subscribers, in order per
/// mod, mods in turn, within its budgets (64 hops and 2 ms per mod, 4 ms in all per step; the rest waits a step). This
/// dispatcher takes each dispatch for its mod: the hop runs as that mod's tick work and its time is booked to the mod.
/// It is separate from <c>Scheduler.Post</c> / <c>NextTick</c>: that queue, its drain in the mod's Pre and its limit
/// are untouched, and a hop never goes through it.
/// </summary>
/// <remarks>Tick thread only (the server world's). A hop the core flushes cancelled from another thread (a world's end)
/// runs no mod code on the tick and never comes here. The task's own scope (current mod, synchronization context)
/// is installed and restored by the task around the hop; a fault in the hop faults that task and is never a handler
/// fault of the mod.</remarks>
internal sealed class TaskTickDispatcher(ModManager mods)
{
    /// <summary>Hops dispatched through here since boot (diagnostics, tests).</summary>
    public long Hops { get; private set; }

    public void Run(ulong owner, Action hop)
    {
        Hops++;
        var start = Stopwatch.GetTimestamp();
        try
        {
            hop();  // never throws: the task catches its work's exceptions
        }
        finally
        {
            mods.AccountTaskHop(owner, Stopwatch.GetTimestamp() - start);
        }
    }
}
