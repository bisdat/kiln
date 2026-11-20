// SPDX-License-Identifier: MIT
using Kiln;

namespace SampleTasks;

public sealed class TasksMod : Mod
{
    public override void OnLoad()
    {
        try
        {
            StartTasks();
        }
        catch (KilnUnavailableException e)
        {
            Log.Warn($"sample-tasks: no tasks here: {e.Message}");
        }
    }

    private void StartTasks()
    {
        // Waits for its cancellation: it ends when the mod unloads, and its finally still runs.
        Context.Tasks.Run("waits", async ctx =>
        {
            try
            {
                await Task.Delay(Timeout.Infinite, ctx.Cancellation);
            }
            finally
            {
                Log.Info("sample-tasks: waits unwound");
            }
        });

        // Throws: only this task faults. The mod keeps running and gets no handler fault for it.
        Context.Tasks.Run("boom", _ => throw new InvalidOperationException("sample-tasks: deliberate task fault"));

        // Computes on a worker, applies on the tick.
        Context.Tasks.Run("scan", async ctx =>
        {
            long sum = 0;
            for (var i = 0; i < 100_000; i++) sum += i;
            var offTick = !Game.IsTickThread;
            await ctx.ToTick();
            Log.Info($"sample-tasks: scan applied (computed off the tick: {offTick}, applied on the tick: {Game.IsTickThread}, sum {sum})");
        });
    }

    public override void OnUnload() => Log.Info("sample-tasks: unloading");
}
