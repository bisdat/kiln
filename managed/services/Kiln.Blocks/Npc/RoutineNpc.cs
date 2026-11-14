// SPDX-License-Identifier: MIT
// NpcBlock.Routine: the plan an NPC block names is bound to that NPC when it is in the world (Routines, RoutineStore).
namespace Kiln;

internal static class RoutineNpc
{
    /// <summary>An NPC block's <c>Routine</c>: an inline plan becomes the block <c>&lt;npc-block-name&gt;.routine</c>, a name
    /// is a routine of this mod, none takes the NPC's earlier link away. All of it with the NPC's commit. Returns the plan's key.</summary>
    public static string? ForNpc(BlockContext ctx, Lowered low, string npcName, string npcKey, PartRef<RoutineBlock>? part)
    {
        var mod = ctx.Mod;
        if (part == null)
        {
            low.AfterCommit(() => Bind(mod, npcKey, null, null));
            return null;
        }
        if (part.Name is { } named)
        {
            var colon = named.IndexOf(':');
            if (colon > 0 && named[..colon] != mod.Id)
                throw new BlockError(ctx.Block, "routine", $"\"{named}\" is a routine of mod {named[..colon]}: a villager's routine belongs to the mod that assigns it, so define the plan in this mod");
            var local = named[(colon + 1)..];
            var found = ctx.Find(local, "routine");
            var handle = found as BlockHandle<RoutineBlock>
                         ?? throw new BlockError(ctx.Block, "routine", found != null
                             ? $"\"{local}\" is a {found.BlockType.Name.Replace("Block", "").ToLowerInvariant()}, not a routine"
                             : $"no routine \"{local}\" in this mod; define it first (Blocks.Define, or the routines section); there are: {string.Join(", ", mod.Handles.Where(x => x.Value is BlockHandle<RoutineBlock>).Select(x => x.Key).Order())}");
            // a routine handle's game name is its identity, <mod>:<Was-or-name>
            var key = handle.GameName;
            low.Json["npcRoutine"] = key;
            low.AfterCommit(() => Bind(mod, npcKey, null, key));
            return key;
        }
        var inlineName = $"{npcName}.routine";
        var made = RoutineCompiler.Define(ctx, low, inlineName, part.Inline!, $"{npcKey}.routine", "routine");
        low.AfterCommit(() => Bind(mod, npcKey, made, made.GameName));
        return made.GameName;
    }

    /// <summary>What the NPC's commit does for its routine: an inline plan's handle becomes a block of the mod, and the NPC's
    /// link is set (or, without a routine, taken away).</summary>
    internal static void Bind(ModBlocks mod, string npcKey, BlockHandle<RoutineBlock>? inline, string? key)
    {
        if (inline != null) mod.Handles[inline.Name] = inline;
        RoutineRegistry.BindNpc(mod.Id, npcKey, key);
    }
}
