// SPDX-License-Identifier: MIT
using Kiln;

namespace DebugCounter;

/// <summary>
/// A counter to attach a debugger to: every 2 s (about 50 ticks) it counts up in <see cref="Count"/> and logs
/// "debug-counter: count N". Put a breakpoint on the marked line, inspect <c>count</c> and <c>_total</c>, step, continue.
/// It touches nothing of the game, so stopping in it is as safe as stopping the game anywhere (which is not safe for a
/// joined game: a stopped client can lose its connection).
/// </summary>
public sealed class DebugCounterMod : Mod
{
    /// <summary>Ticks between two counts (the server ticks about 25 times a second).</summary>
    private const ulong Every = 50;

    private ulong _first;
    private long _total;

    public override void OnLoad()
    {
        Log.Info($"debug-counter: loaded (side {Mods.Side}); counting every {Every} ticks");
        Context.Tick.Pre += OnPre;
    }

    private void OnPre(in TickInfo tick)
    {
        if (_first == 0) _first = tick.Step;
        if ((tick.Step - _first) % Every != 0) return;
        Count(tick.Step);
    }

    private void Count(ulong step)
    {
        var count = _total + 1;  // DEBUG-COUNTER-BREAK: the line the guided checks and the lab put a breakpoint on
        _total = count;
        Log.Info($"debug-counter: count {count} at step {step}");
    }

    public override void OnUnload() => Log.Info($"debug-counter: unloaded after {_total} counts");
}
