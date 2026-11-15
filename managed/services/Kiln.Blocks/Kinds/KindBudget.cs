// SPDX-License-Identifier: MIT
// The work a kind's plan may make while Kiln's compilers lower it (chapter 16d section 5; 16d review R3). A named effect is
// emitted where it is used, each time it is used, so a plan's frozen JSON says little about how much it expands: a chain of
// effects staged in dependency order nests without staging, and an effect that runs another twice doubles at every step.
// The budget counts while the effects are emitted (EffectEmitter.One), not after: every effect each time it is expanded,
// the nesting of named effects, and the bytes of the programs built so far and the one being built.
namespace Kiln;

internal sealed class KindBudget : IDisposable
{
    [ThreadStatic] private static KindBudget? _current;

    /// <summary>The budget of the plan being lowered on this thread, or null (a mod's own blocks: no budget).</summary>
    internal static KindBudget? Current => _current;

    private readonly KindBudget? _previous;
    private readonly string _label;
    private readonly int _maxNodes, _maxDepth;
    private readonly long _maxBytes;
    private int _nodes, _depth;
    private long _built;

    private KindBudget(string label, int maxNodes, int maxDepth, long maxBytes)
    {
        (_label, _maxNodes, _maxDepth, _maxBytes) = (label, maxNodes, maxDepth, maxBytes);
        _previous = _current;
    }

    /// <summary>Opens the budget of one plan on this thread, until it is disposed. The block being lowered is the first level of
    /// <paramref name="maxDepth"/>: a named effect it uses is the second.</summary>
    internal static KindBudget Open(string label, int maxNodes = KindCompiler.MaxNodes, int maxDepth = KindCompiler.MaxDepth, long maxBytes = KindApply.MaxPlanBytes) =>
        _current = new KindBudget(label, maxNodes, maxDepth, maxBytes);

    /// <summary>Effects expanded so far.</summary>
    internal int Nodes => _nodes;

    /// <summary>One effect is about to be emitted. <paramref name="named"/>: it is a block used by its name. <paramref name="building"/>:
    /// the bytes of the program it goes into so far. Over the budget: a <see cref="BlockError"/> on the block and path.</summary>
    internal void Enter(string block, string path, bool named, long building)
    {
        if (++_nodes > _maxNodes)
            throw new BlockError(_label, null, $"{block}, {path}: the plan expands to more than {_maxNodes} effects (an effect counts each time it is used)");
        if (named && ++_depth >= _maxDepth)
            throw new BlockError(_label, null, $"{block}, {path}: named effects nest more than {_maxDepth} deep");
        if (_built + building > _maxBytes)
            throw new BlockError(_label, null, $"{block}, {path}: the plan's programs grow to more than {Bytes(_maxBytes)}");
    }

    /// <summary>The effect <see cref="Enter"/> was told of is done.</summary>
    internal void Leave(bool named)
    {
        if (named) _depth--;
    }

    /// <summary>A program of the plan was laid out: its bytes count from now on.</summary>
    internal void Built(long bytes) => _built += bytes;

    private static string Bytes(long n) => n >= 1 << 20 && n % (1 << 20) == 0 ? $"{n >> 20} MiB" : $"{n} bytes";

    public void Dispose()
    {
        if (ReferenceEquals(_current, this)) _current = _previous;
    }
}
