// SPDX-License-Identifier: MIT
// Rarity upgrades as a block (package P3 of the equipment wave, docs/kiln/modding/item-blocks.md "Rarity upgrades"): which
// items a mod's upgrade takes, up to which rarity, and what each step costs. Nothing reaches the game at compile time: the
// block is checked by field and becomes the mod's rule under "mod:name" in Kiln.Items (ItemUpgrades), which
// Items.UpgradeRarity(player, item, "name") runs, and a station's interaction later (package P4).
using System.Text.Json.Nodes;

namespace Kiln;

/// <summary>
/// A rarity upgrade: the rule a station, a command or a quest runs on an item through
/// <c>Items.UpgradeRarity(player, item, "name")</c>. Each step names the rarity it reaches and what it costs; an upgraded item
/// gets its next enhancement slot in place, which the player then unlocks at the workbench as usual. A block: in C#, or in
/// the <c>rarityUpgrades</c> section of <c>blocks/*.json</c>.
/// <code>
/// Blocks.Define("standard-upgrades", new RarityUpgradeBlock
/// {
///     Items = ["Weapon_*", "Armor_*"],
///     Steps =
///     {
///         [Rarity.Uncommon] = new UpgradeStep { Cost = { [GameItems.Material_T1_Metal_Scraps] = 10 } },
///         [Rarity.Rare] = new UpgradeStep { Cost = { ["ember-shard"] = 3 }, Chance = 0.75 },
///     },
/// });
/// </code>
/// Server; asks nothing of players' games (the item's new rarity is the game's own state).
/// </summary>
[BlockClients(ClientsLevel.None)]
public sealed record RarityUpgradeBlock : IBlock
{
    /// <summary>The item types it takes: <c>GameItems</c> constants, items of this mod by block name, <c>"other-mod:name"</c>,
    /// or a pattern with <c>*</c> over the game's item names (<c>"Weapon_*"</c>; <c>"my-mod:*"</c> is every item of that mod).
    /// None: every item with enhancement slots.</summary>
    public IReadOnlyList<ItemRef>? Items { get; init; }

    /// <summary>The highest rarity it upgrades to (default Legendary).</summary>
    public Rarity Max { get; init; } = Rarity.Legendary;

    /// <summary>The steps, by the rarity each reaches (Uncommon .. Legendary): its cost (item names as in
    /// <see cref="Items"/>), chance, what a failed roll costs, a level scale for the cost, and whether the new slot comes
    /// unlocked. At least one.</summary>
    public IDictionary<Rarity, UpgradeStep> Steps { get; init; } = new Dictionary<Rarity, UpgradeStep>();

    /// <summary>What happens to durability (default <see cref="DurabilityRule.KeepRatio"/>).</summary>
    public DurabilityRule Durability { get; init; } = DurabilityRule.KeepRatio;

    /// <summary>What happens about a gem socket (default <see cref="SocketRule.Keep"/>).</summary>
    public SocketRule Socket { get; init; } = SocketRule.Keep;

    /// <summary>Upgrade into an enhancement slot the item's type has no perk for (default false: refused).</summary>
    public bool AllowEmptySlot { get; init; }
}

internal static partial class Compiler
{
    private static BlockHandle DefineRarityUpgrade(BlockContext ctx, Lowered low, string name, RarityUpgradeBlock r)
    {
        var what = $"rarity upgrade {name}";
        RarityUpgradeRuntime.Check(what, r);
        var id = $"{ctx.Mod.Id}:{name}";
        var handle = new BlockHandle<RarityUpgradeBlock>(ctx.Mod.Id, name, BlockIds.Derive(ctx.Mod.Id, "rarity-upgrade", name), id, r);
        var rule = RarityUpgradeRuntime.RuleOf(ctx.Mod.Id, name, r);
        low.Json["rarityUpgrade"] = new JsonObject
        {
            ["id"] = id,
            ["items"] = r.Items is null ? null : new JsonArray(r.Items.Select(i => (JsonNode?)JsonValue.Create(i.Value)).ToArray()),
            ["max"] = r.Max.ToString(),
            ["steps"] = new JsonObject(r.Steps.OrderBy(s => s.Key).Select(s => KeyValuePair.Create(s.Key.ToString(), (JsonNode?)new JsonObject
            {
                ["cost"] = new JsonObject(s.Value.Cost.Select(c => KeyValuePair.Create(c.Key, (JsonNode?)JsonValue.Create(c.Value)))),
                ["chance"] = s.Value.Chance,
                ["onFail"] = s.Value.OnFail.ToString(),
                ["levelScale"] = s.Value.LevelScale,
                ["unlock"] = s.Value.Unlock,
            }))),
            ["durability"] = r.Durability.ToString(),
            ["socket"] = r.Socket.ToString(),
            ["allowEmptySlot"] = r.AllowEmptySlot,
        };
        low.AfterCommit(() => ItemUpgrades.Register(id, rule));
        return handle;
    }
}

/// <summary>Every loaded mod's rarity upgrades, registered in Kiln.Items by id.</summary>
internal static class RarityUpgradeRuntime
{
    private static string JsonName(Rarity r) => r.ToString().ToLowerInvariant();

    /// <summary>What is wrong with a block, by field: <see cref="BlockError"/>.</summary>
    internal static void Check(string what, RarityUpgradeBlock block)
    {
        if (block.Items is { Count: > 1024 }) throw new BlockError(what, "items", "names at most 1024 items or patterns");
        if (block.Items != null)
            for (var i = 0; i < block.Items.Count; i++)
                if (string.IsNullOrWhiteSpace(block.Items[i].Value)) throw new BlockError(what, $"items[{i}]", "an item's name, not empty");
        if (!Enum.IsDefined(block.Max)) throw new BlockError(what, "max", $"{block.Max} is no rarity (common to legendary)");
        if (block.Max == Rarity.Common) throw new BlockError(what, "max", "common: no item would ever be upgraded (uncommon to legendary)");
        if (!Enum.IsDefined(block.Durability)) throw new BlockError(what, "durability", $"{block.Durability} is no durability rule (keepRatio, keep, repair)");
        if (!Enum.IsDefined(block.Socket)) throw new BlockError(what, "socket", $"{block.Socket} is no socket rule (keep, gameChance)");
        if (block.Steps is not { Count: > 0 }) throw new BlockError(what, "steps", "names at least one step (the rarity it reaches: uncommon to legendary)");
        foreach (var (rarity, step) in block.Steps)
        {
            var at = $"steps.{JsonName(rarity)}";
            if (!Enum.IsDefined(rarity) || rarity == Rarity.Common) throw new BlockError(what, "steps", $"{rarity}: a step reaches uncommon to legendary");
            if (rarity > block.Max) throw new BlockError(what, at, $"above the block's max ({JsonName(block.Max)}): it would never be reached");
            if (step is null) throw new BlockError(what, at, "no step");
            if (!double.IsFinite(step.Chance) || step.Chance is < 0 or > 1) throw new BlockError(what, $"{at}.chance", $"{step.Chance}: a chance is 0 to 1");
            if (!Enum.IsDefined(step.OnFail)) throw new BlockError(what, $"{at}.onFail", $"{step.OnFail} is no failure rule (keepItemLoseCost, keepItemKeepCost, downgrade)");
            if (!double.IsFinite(step.LevelScale) || step.LevelScale is < 0 or > 1000) throw new BlockError(what, $"{at}.levelScale", $"{step.LevelScale}: 0 to 1000");
            if (step.Cost is null) throw new BlockError(what, $"{at}.cost", "none (leave it out for free)");
            if (step.Cost.Count > 8) throw new BlockError(what, $"{at}.cost", "at most 8 items");
            foreach (var (item, count) in step.Cost)
            {
                if (string.IsNullOrWhiteSpace(item)) throw new BlockError(what, $"{at}.cost", "an item's name, not empty");
                if (count is < 1 or > 100000) throw new BlockError(what, $"{at}.cost.{item}", $"{count}: a count is 1 to 100000");
            }
        }
    }

    /// <summary>The block as Kiln.Items' rule, its names read in <paramref name="mod"/>.</summary>
    internal static RarityUpgradeRule RuleOf(string mod, string name, RarityUpgradeBlock b)
    {
        var steps = new Dictionary<ItemRarity, UpgradeStep>();
        foreach (var (rarity, step) in b.Steps) steps[(ItemRarity)(int)rarity] = step with { Cost = new Dictionary<string, int>(step.Cost) };
        return new RarityUpgradeRule
        {
            Items = b.Items?.Select(i => i.Value).ToArray(),
            Max = (ItemRarity)(int)b.Max,
            Steps = steps,
            Durability = b.Durability,
            Socket = b.Socket,
            AllowEmptySlot = b.AllowEmptySlot,
            Mod = mod,
            Id = $"{mod}:{name}",
        };
    }
}
