// SPDX-License-Identifier: MIT
// Content that loaded in 0.2.0-dev.30.6 keeps loading. 0.2.0-dev.30.7 checks every content block as the schema and kiln lint do; a value they
// refuse that 0.2.0-dev.30.6 loaded without harm still loads, as it did then, with one warning per block (kiln.log, and the mod's warnings that
// `kiln blocks` shows). A value that made the game misbehave stays refused. The cases, and why: schema/fixtures/block-ranges.json and
// block-shapes.json ("runtime"), docs/kiln/modding/blocks.md "Values that loaded in 0.2.0-dev.30.6".
namespace Kiln;

internal static class BlockCompat
{
    internal const string Release = "0.2.0-dev.30.6";

    private static string Head(string label) => $"block {label}: loads as it did in {Release}, but ";

    /// <summary>The block's one warning: every tolerated field, what is wrong with it and what it does.</summary>
    internal static string Text(string label, IReadOnlyList<(string Field, string Problem)> notes) =>
        Head(label) + $"kiln lint reports {(notes.Count == 1 ? "an error" : $"{notes.Count} errors")} that a later release may refuse: " +
        string.Join("; ", notes.Select(n => $"{n.Field}: {n.Problem}"));

    /// <summary>Says it once per block per load: the mod's warnings (a reload clears them) keep one such line per block, the latest.</summary>
    internal static void Warn(ModBlocks mod, string label, IReadOnlyList<(string Field, string Problem)> notes)
    {
        var text = Text(label, notes);
        lock (mod.Warnings)
        {
            if (mod.Warnings.Contains(text)) return;
            mod.Warnings.RemoveAll(w => w.StartsWith(Head(label), StringComparison.Ordinal));
            mod.Warnings.Add(text);
        }
        mod.ReportDirty = true;
        BlocksBridge.Log(LogLevel.Warn, $"blocks: {mod.Id}: {text}");
    }
}
