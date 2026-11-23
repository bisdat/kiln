// SPDX-License-Identifier: MIT
// Knowledge blocks (since 0.2.0-dev.30.5): the game's own tracking of an item (found: picked up) or a recipe (crafted once) for an item or
// recipe of a mod. The game tracks only ids its knowledge table lists, and Kiln's new items and recipes are not in it; a knowledge block puts
// the id there the way the game's own items and recipes are (kiln-quests' boot build, native/services/quests/questpack.cpp
// tracked_knowledge). The table is built at start, before any mod runs, so the block is written down for the build (Kiln.Quests
// Native/NativeExport.cs): from the next start, or from the first when Kiln.Sdk baked it into the mod. A Craft block also gives the recipe
// the game's "crafted once" completion query (RecipeDefinition.TrackCrafted). docs/kiln/modding/item-blocks.md "Tracking".
namespace Kiln;

/// <summary>What the game tracks: an item found, or a recipe crafted.</summary>
public enum KnowledgeTrigger
{
    /// <summary>The item was picked up: the world's knowledge (found somewhere in this world) and each player's, as the game keeps for its own items.</summary>
    Pickup,
    /// <summary>The recipe was crafted at least once: each player's knowledge, which the recipe's "crafted once" mark reads, as for the game's own recipes.</summary>
    Craft,
}

/// <summary>Whose knowledge it is.</summary>
public enum TrackingScope
{
    /// <summary>Each player's (character's) own.</summary>
    Player,
    /// <summary>The world's, shared by everyone in it. Items only.</summary>
    World,
    /// <summary>Both: the world's and each player's. Items only.</summary>
    Both,
}

/// <summary>
/// The game's tracking of one of the mod's items or recipes, as it tracks its own: <c>new KnowledgeBlock { Trigger = KnowledgeTrigger.Pickup, Of = "rubble-tool" }</c>.
/// Kiln's items count as known without it and its recipes are listed without it; this is for what the game does with the knowledge (an item's "found",
/// a recipe's "crafted once"). The game's knowledge table is built at start: from the next start, or the first one when the mod is built with Kiln.Sdk.
/// In <c>blocks/*.json</c> under <c>"knowledge"</c>. Players need the mod (<c>clients</c> "mod"): every player's game builds its own table. Since 0.2.0-dev.30.5 (mod API 0.2.14).
/// </summary>
[BlockClients(ClientsLevel.Mod)]
public sealed record KnowledgeBlock : IQuestBlock
{
    /// <summary>What is tracked: <see cref="KnowledgeTrigger.Pickup"/> for an item, <see cref="KnowledgeTrigger.Craft"/> for a recipe.</summary>
    public required KnowledgeTrigger Trigger { get; init; }

    /// <summary>The item or recipe, a block of this mod by name or handle: an item block (Pickup: the item; Craft: its own recipe), or a recipe block (Craft). Or
    /// set <see cref="Identity"/>.</summary>
    public string? Of { get; init; }

    /// <summary>The id to track when it is not a block of this mod by name: the identity of the item or recipe (<c>BlockIdentity.Legacy(guid)</c>). One of
    /// <see cref="Of"/> and this.</summary>
    public BlockIdentity? Identity { get; init; }

    /// <summary>Whose knowledge. Null: as the game has it, both for an item, the player's for a recipe. A recipe is only ever the player's.</summary>
    public TrackingScope? Scope { get; init; }
}

internal sealed class KnowledgeHandle(string mod, string name, Guid guid, string gameName, KnowledgeBlock block, uint id)
    : BlockHandle<KnowledgeBlock>(mod, name, guid, gameName, block)
{
    /// <summary>The tracked item's or recipe's id.</summary>
    public uint Id { get; } = id;
}

internal static class KnowledgeBlocks
{
    /// <summary>The name the build files the knowledge under (the quests file's "knowledge" section): its own, apart from a quest's.</summary>
    internal static string Entry(string block) => $"track.{block}";

    public static BlockHandle Define(BlockContext ctx, Lowered low, string name, KnowledgeBlock k)
    {
        var mod = ctx.Mod.Id;
        ctx.Need(ClientsLevel.Mod, null, "the game's knowledge table is built by every player's game at start, from the mod");
        var id = TrackedId(ctx, k);
        var pickup = k.Trigger == KnowledgeTrigger.Pickup;
        var scope = k.Scope ?? (pickup ? TrackingScope.Both : TrackingScope.Player);
        if (!pickup && scope != TrackingScope.Player)
            throw new BlockError(ctx.Block, "scope", "the game keeps a recipe's \"crafted once\" in each player's knowledge: a recipe's scope is player");
        var entry = Entry(name);
        var track = pickup ? "pickup" : "craft";
        var scopeText = scope.ToString().ToLowerInvariant();
        low.Json["knowledge"] = new System.Text.Json.Nodes.JsonObject { ["entry"] = entry, ["id"] = $"0x{id:x8}", ["track"] = track, ["scope"] = scopeText };
        low.AfterCommit(() =>
        {
            NativeExport.Track(mod, entry, id, track, scopeText);
            if (NativeExport.Baking || NativeQuests.KnowledgeId(mod, entry) == id) return;
            var text = $"block {ctx.Block}: the game tracks 0x{id:x8} from the next start: its knowledge table is built at start (build the mod with Kiln.Sdk to have it at the first)";
            lock (ctx.Mod.Warnings)
                if (!ctx.Mod.Warnings.Contains(text)) ctx.Mod.Warnings.Add(text);
            ctx.Mod.ReportDirty = true;
            BlocksBridge.Log(LogLevel.Info, $"blocks: {mod}: {text}");
        });
        return new KnowledgeHandle(mod, name, BlockIds.Derive(mod, "knowledge", name), $"{mod}:{entry}", k, id);
    }

    /// <summary>The item's or recipe's id: of the block <see cref="KnowledgeBlock.Of"/> names, or <see cref="KnowledgeBlock.Identity"/>'s.</summary>
    private static uint TrackedId(BlockContext ctx, KnowledgeBlock k)
    {
        if ((k.Of == null) == (k.Identity == null))
            throw new BlockError(ctx.Block, "of", "say what is tracked: of (an item or recipe block of this mod) or identity, one of them");
        if (k.Identity is { } identity) return identity.Id;
        var of = k.Of!;
        if (of.Contains(':')) throw new BlockError(ctx.Block, "of", "a block of this mod: another mod's item or recipe is that mod's to track (or say its identity)");
        var handle = ctx.Find(of, "of") ?? throw new BlockError(ctx.Block, "of", $"no block \"{of}\" in this mod");
        switch (handle)
        {
            case BlockHandle<ItemBlock> or BlockHandle<WeaponBlock> or BlockHandle<WandBlock> or BlockHandle<ArmorBlock>
                or BlockHandle<BuildingToolBlock> or BlockHandle<TerrainBrushBlock> when k.Trigger == KnowledgeTrigger.Pickup:
                return BlockIds.Fnv(handle.Guid);
            case BlockHandle<ItemBlock> item:
                if (item.Block.Recipe is not { } recipe) throw new BlockError(ctx.Block, "of", $"item {of} has no recipe of its own to track: name a recipe block, or track its pickup");
                if (!recipe.TrackCrafted) throw new BlockError(ctx.Block, "of", $"item {of}'s recipe does not say trackCrafted: true, so the game would never mark it crafted");
                return BlockIds.Fnv(recipe.Identity?.Guid ?? BlockIds.Derive(ctx.Mod.Id, "recipe", item.Block.Was ?? of));
            case BlockHandle<RecipeBlock> r when k.Trigger == KnowledgeTrigger.Craft:
                if (!r.Block.TrackCrafted) throw new BlockError(ctx.Block, "of", $"recipe {of} does not say trackCrafted: true, so the game would never mark it crafted");
                return BlockIds.Fnv(r.Guid);
            case BlockHandle<RecipeBlock>:
                throw new BlockError(ctx.Block, "trigger", $"{of} is a recipe: a recipe is crafted (trigger craft), not picked up");
            default:
                throw new BlockError(ctx.Block, "of", $"\"{of}\" is a {handle.BlockType.Name}, not an item or a recipe");
        }
    }
}
