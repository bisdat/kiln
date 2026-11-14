// SPDX-License-Identifier: MIT
// RoutineBlock -> a checked plan with a stable identity, and the registry of the plans defined in this process.
// A routine makes no game resource: it lowers to its wire document (Lowered.Json["routine"]) and, at commit, a row of the
// registry. Identity: key <mod>:<Was-or-block-name>, GUID UUIDv5(<mod>:routine:<Was-or-block-name>). An inline routine
// of an NPC block is the block "<npc-block-name>.routine" with the key <mod>:<npc Was-or-name>.routine, so neither a
// rename of the NPC with Was nor a change of the plan's content moves it.
namespace Kiln;

internal static partial class Compiler
{
    internal static BlockHandle DefineRoutine(BlockContext ctx, Lowered low, string name, RoutineBlock block) =>
        RoutineCompiler.Define(ctx, low, name, block, null);
}

internal static class RoutineIdentity
{
    /// <summary>A local name: what a block name takes, and no ':' or path.</summary>
    public static bool IsLocal(string s) => s.Length > 0 && s.All(c => char.IsAsciiLetterOrDigit(c) || c is '-' or '_' or '.');

    /// <summary><c>&lt;mod&gt;:&lt;local name&gt;</c>, both non-empty; the mod id has no ':'.</summary>
    public static bool IsKey(string key)
    {
        var colon = key.IndexOf(':');
        return colon > 0 && IsLocal(key[(colon + 1)..]) && key.AsSpan(0, colon).IndexOfAny(" \t\r\n\"\\/") < 0;
    }

    public static string Key(string mod, string local) => $"{mod}:{local}";

    /// <summary>The mod of a key.</summary>
    public static string ModOf(string key) => key[..key.IndexOf(':')];

    public static Guid Guid(string mod, string local) => BlockIds.Derive(mod, "routine", local);
}

/// <summary>A plan as defined: whose, under which block name, and what it compiled to.</summary>
internal sealed record RoutineDefinition(string Mod, string BlockName, Guid Guid, RoutinePlan Plan, RoutineBlock Block)
{
    public string Key => Plan.Key;
}

/// <summary>The plans defined in this process, by identity. Filled when a routine block commits (during a mod's OnLoad or
/// its data pass, before the ModsLoaded barrier); a mod's rows go when its blocks are reset (reload, unload). Any thread.</summary>
internal static class RoutineRegistry
{
    private static readonly object Gate = new();
    private static readonly Dictionary<Guid, RoutineDefinition> ByGuid = new();
    private static readonly Dictionary<string, RoutineDefinition> ByKey = new(StringComparer.Ordinal);
    // (mod, npc key) -> the routine an NPC block names: bound when that NPC is in the world
    private static readonly Dictionary<(string Mod, string Npc), string> NpcRoutines = new();

    /// <summary>The block name that holds <paramref name="key"/> for another block of the same mod, or null: an alias collision.</summary>
    public static string? HeldBy(string key, string blockName)
    {
        lock (Gate) return ByKey.TryGetValue(key, out var d) && d.BlockName != blockName ? d.BlockName : null;
    }

    /// <summary>Adds or replaces (the same block defined again) a definition.</summary>
    public static void Add(RoutineDefinition d)
    {
        lock (Gate)
        {
            // the same block under another key (its Was changed): the old identity goes with it
            foreach (var old in ByKey.Values.Where(x => x.Mod == d.Mod && x.BlockName == d.BlockName && x.Key != d.Key).ToList())
            {
                ByKey.Remove(old.Key);
                ByGuid.Remove(old.Guid);
            }
            ByGuid[d.Guid] = d;
            ByKey[d.Key] = d;
        }
    }

    /// <summary>The definition a handle stands for, by the handle's id, not its name: null when it is not (or no longer)
    /// defined, or when the handle is another definition's of the same id.</summary>
    public static RoutineDefinition? Find(BlockHandle<RoutineBlock> handle)
    {
        lock (Gate) return ByGuid.TryGetValue(handle.Guid, out var d) && d.Mod == handle.Mod ? d : null;
    }

    public static RoutineDefinition? Find(string key)
    {
        lock (Gate) return ByKey.GetValueOrDefault(key);
    }

    /// <summary>An NPC block's routine: the plan bound to that NPC when it is in the world.</summary>
    public static void BindNpc(string mod, string npcKey, string? routineKey)
    {
        lock (Gate)
        {
            if (routineKey == null) NpcRoutines.Remove((mod, npcKey));
            else NpcRoutines[(mod, npcKey)] = routineKey;
        }
    }

    public static string? OfNpc(string mod, string npcKey)
    {
        lock (Gate) return NpcRoutines.GetValueOrDefault((mod, npcKey));
    }

    /// <summary>Forgets what a mod defined: it is reloading or gone.</summary>
    public static void Forget(string mod)
    {
        lock (Gate)
        {
            foreach (var d in ByKey.Values.Where(x => x.Mod == mod).ToList())
            {
                ByKey.Remove(d.Key);
                ByGuid.Remove(d.Guid);
            }
            foreach (var k in NpcRoutines.Keys.Where(k => k.Mod == mod).ToList()) NpcRoutines.Remove(k);
        }
    }

    public static IReadOnlyList<RoutineDefinition> All
    {
        get { lock (Gate) return [.. ByKey.Values.OrderBy(d => d.Key, StringComparer.Ordinal)]; }
    }
}

internal static class RoutineCompiler
{
    /// <summary>Checks a routine block and makes its plan; nothing is registered. <paramref name="local"/>: the local part
    /// of the key (the block's Was, else its name, else what an NPC block gives its inline routine).</summary>
    public static RoutinePlan Plan(string label, string mod, string local, RoutineBlock block, string? path = null)
    {
        string At(string field) => string.IsNullOrEmpty(path) ? field : $"{path}.{field}";
        if (block.Was is { } was && !RoutineIdentity.IsLocal(was))
            throw new BlockError(label, At("was"), $"\"{was}\" is not a local name: letters, digits, '-', '_' and '.', and never another mod's");
        if (block.Windows is null) throw new BlockError(label, At("windows"), "null: a plan without windows says windows: []");
        if (block.Fallback is null) throw new BlockError(label, At("fallback"), "null: leave it out for the game's own choice");
        var windows = new List<PlanWindow>(block.Windows.Count);
        for (var i = 0; i < block.Windows.Count && i <= RoutinePlan.MaxWindows; i++)
        {
            var w = block.Windows[i] ?? throw new BlockError(label, At($"windows[{i}]"), "null");
            if (w.Task is null) throw new BlockError(label, At($"windows[{i}].task"), "a window needs a task");
            windows.Add(new PlanWindow(w.StartMinute, w.EndMinute, w.AllDay, w.Priority, RoutineKinds.Of(w.Task)));
        }
        if (block.Windows.Count > RoutinePlan.MaxWindows)
            throw new BlockError(label, At("windows"), $"{block.Windows.Count} windows: a plan takes at most {RoutinePlan.MaxWindows}");
        var fallback = RoutineKinds.Of(block.Fallback);
        if (RoutinePlan.Check(block.Version, windows, fallback) is { } bad) throw new BlockError(label, At(bad.Field), bad.Problem);
        return RoutinePlan.Make(RoutineIdentity.Key(mod, local), block.Version, windows, fallback);
    }

    /// <summary>A routine block's compile. <paramref name="inlineLocal"/>: the key an NPC block gives its inline routine when
    /// the routine says no Was of its own.</summary>
    public static BlockHandle<RoutineBlock> Define(BlockContext ctx, Lowered low, string name, RoutineBlock block, string? inlineLocal, string? path = null)
    {
        var mod = ctx.Mod.Id;
        var local = block.Was ?? inlineLocal ?? name;
        var plan = Plan(ctx.Block, mod, local, block, path);
        if (RoutineRegistry.HeldBy(plan.Key, name) is { } other)
            throw new BlockError(ctx.Block, string.IsNullOrEmpty(path) ? (block.Was != null ? "was" : null) : path,
                $"the routine \"{other}\" of this mod already has the identity {plan.Key}: two plans cannot share one (a Was names the plan's own earlier name)");
        var guid = RoutineIdentity.Guid(mod, local);
        var def = new RoutineDefinition(mod, name, guid, plan, block);
        // one routine per Lowered under "routine"; an NPC block's inline one is under "npcRoutine" (its own "npc" is the NPC file)
        low.Json[path == null ? "routine" : "npcRoutine"] = System.Text.Json.Nodes.JsonNode.Parse(plan.Wire);
        low.AfterCommit(() => RoutineRegistry.Add(def));
        return new BlockHandle<RoutineBlock>(mod, name, guid, plan.Key, block);
    }
}
