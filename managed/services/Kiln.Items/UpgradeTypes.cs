// SPDX-License-Identifier: MIT
// Rarity upgrades as values: the rule (RarityUpgradeRule and its UpgradeStep per rarity), the options of one upgrade, its
// result and its events. Items.UpgradeRarity runs one (Upgrades.cs); Kiln.Blocks' RarityUpgradeBlock (rarityUpgrades) is
// the same rule as a block. Guide: docs/kiln/services/items.md, "Upgrading rarity".
namespace Kiln;

/// <summary>What a failed roll of an upgrade costs (<see cref="UpgradeStep.OnFail"/>). An upgrade never destroys an item.</summary>
public enum UpgradeFail
{
    /// <summary>The item stays as it was and the cost is spent. The default.</summary>
    KeepItemLoseCost = 0,

    /// <summary>The item stays as it was and nothing is paid.</summary>
    KeepItemKeepCost = 1,

    /// <summary>The cost is spent and the item drops one rarity (its highest enhancement slot goes, an unlocked one with its
    /// level, as the game's own downgrade does). A Common item stays Common.</summary>
    Downgrade = 2,
}

/// <summary>What an upgrade does about a gem socket (<see cref="RarityUpgradeRule.Socket"/>).</summary>
public enum SocketRule
{
    /// <summary>The item keeps what it has: a socket stays, none is added. The default.</summary>
    Keep = 0,

    /// <summary>A weapon without a socket rolls one at the new rarity with the game's own chances for new weapons (the
    /// unchanged game's 0 %, 5 %, 25 %, 50 % and 100 % from Common to Legendary). A won socket re-creates the item in its slot
    /// (as <c>Gems.AddSocket</c> does: a new item entity, level, enhancements, durability and dye kept), so
    /// <see cref="UpgradeResult.After"/> names the new item. Not for an equipped weapon (it keeps no socket then) or other
    /// gear.</summary>
    GameChance = 1,
}

/// <summary>One step of a <see cref="RarityUpgradeRule"/>: what reaching one rarity costs and how likely it is.</summary>
public sealed record UpgradeStep
{
    /// <summary>The success chance of a step that names none: 1 (always).</summary>
    public const double DefaultChance = 1.0;

    /// <summary>What a failed roll costs when a step names nothing: <see cref="UpgradeFail.KeepItemLoseCost"/>.</summary>
    public const UpgradeFail DefaultOnFail = UpgradeFail.KeepItemLoseCost;

    /// <summary>The items it costs, by name: game items (<c>GameItems</c> constants), <c>"other-mod:name"</c>, or an item of
    /// the rule's own mod by block name; at most 8, each 1 to 100000. Plain items only (materials, runes): an item with an
    /// entity of its own (gear, a gem) cannot be a cost. Empty: free.</summary>
    public IDictionary<string, int> Cost { get; init; } = new Dictionary<string, int>();

    /// <summary>0 to 1: how likely the step is to succeed (default <see cref="DefaultChance"/>). The server rolls it; a
    /// player's game has no part in it.</summary>
    public double Chance { get; init; } = DefaultChance;

    /// <summary>What a failed roll costs (default <see cref="DefaultOnFail"/>).</summary>
    public UpgradeFail OnFail { get; init; } = DefaultOnFail;

    /// <summary>0 or more: the cost grows with the item's level, <c>count × (1 + LevelScale × (level - 1))</c> rounded up
    /// (at most 100000 each). Default 0: the same cost at every level.</summary>
    public double LevelScale { get; init; }

    /// <summary>The upgrade also unlocks one more enhancement slot at once, as the workbench does without its cost (one
    /// level, one more perk acting). Default false: the new slot waits, locked, for the player to unlock it at the workbench
    /// as usual.</summary>
    public bool Unlock { get; init; }
}

/// <summary>
/// The rule of a rarity upgrade: which items it takes, up to which rarity, and what each step costs. A step names the
/// rarity it reaches: <c>Steps[ItemRarity.Rare]</c> is Uncommon to Rare. An item whose next rarity has no step, or whose
/// type has no enhancement slot for it, is refused. As a block: Kiln.Blocks' <c>RarityUpgradeBlock</c>
/// (<c>rarityUpgrades</c> in <c>blocks/*.json</c>).
/// <code>
/// var rule = new RarityUpgradeRule
/// {
///     Items = ["Weapon_*"],
///     Steps =
///     {
///         [ItemRarity.Uncommon] = new UpgradeStep { Cost = { ["Material_T1_Metal_Scraps"] = 10 } },
///         [ItemRarity.Rare] = new UpgradeStep { Cost = { ["Material_T1_Metal_Sheets"] = 5 }, Chance = 0.9 },
///     },
/// };
/// </code>
/// </summary>
public sealed record RarityUpgradeRule
{
    /// <summary>The item types it takes: game names, <c>"other-mod:name"</c>, an item of the rule's mod by block name, or a
    /// pattern with <c>*</c> over the game's item names (<c>"Weapon_*"</c>; <c>"my-mod:*"</c> is every item of that mod).
    /// Null or empty: every item with enhancement slots.</summary>
    public IReadOnlyList<string>? Items { get; init; }

    /// <summary>The highest rarity it upgrades to (default Legendary, the game's highest).</summary>
    public ItemRarity Max { get; init; } = ItemRarity.Legendary;

    /// <summary>The steps, by the rarity each reaches (Uncommon .. Legendary). A rarity without a step is not reached.</summary>
    public IDictionary<ItemRarity, UpgradeStep> Steps { get; init; } = new Dictionary<ItemRarity, UpgradeStep>();

    /// <summary>What happens to the item's durability (default <see cref="DurabilityRule.KeepRatio"/>: the game computes max
    /// durability again at the new rarity, the share kept).</summary>
    public DurabilityRule Durability { get; init; } = DurabilityRule.KeepRatio;

    /// <summary>What happens about a gem socket (default <see cref="SocketRule.Keep"/>).</summary>
    public SocketRule Socket { get; init; } = SocketRule.Keep;

    /// <summary>Give a rarity whose enhancement slot the item's type has no perk for (the slot exists and can be unlocked
    /// for a level, but no perk acts in it). Default false: such an upgrade ends <see cref="UpgradeStatus.NoSlot"/>.</summary>
    public bool AllowEmptySlot { get; init; }

    /// <summary>The mod whose block names the rule's own items (set by Kiln.Blocks; a rule in code uses the calling mod's).</summary>
    internal string? Mod { get; init; }

    /// <summary>The rule's id as a block (<c>"mod:name"</c>), or null for a rule made in code.</summary>
    internal string? Id { get; init; }
}

/// <summary>Options of one <see cref="Items.UpgradeRarity(EntityId, ItemInstanceRef, RarityUpgradeRule, UpgradeOptions?)"/>.</summary>
public sealed record UpgradeOptions
{
    /// <summary>The defaults: the player pays, the rule's costs and chances.</summary>
    public static UpgradeOptions Default { get; } = new();

    /// <summary>Who pays: a container your mod vouches for (a loaded chest's entity), or none (the default): the player.
    /// Another player or an NPC is refused.</summary>
    public EntityId PayFrom { get; init; }

    /// <summary>Nothing is paid (a quest reward, a creative tool); the chance still applies.</summary>
    public bool Free { get; init; }

    /// <summary>Always succeed, whatever the step's chance (a guaranteed upgrade).</summary>
    public bool Guaranteed { get; init; }

    /// <summary>Check, count and plan; pay and change nothing, roll nothing (<see cref="UpgradeResult.Rolled"/> null). The
    /// result says what a success would do and cost.</summary>
    public bool DryRun { get; init; }
}

/// <summary>What an upgrade did (<see cref="UpgradeResult.Status"/>).</summary>
public enum UpgradeStatus
{
    /// <summary>The item is one rarity higher (a dry run: would be), the cost paid.</summary>
    Done = 0,

    /// <summary>The roll failed: the step's <see cref="UpgradeStep.OnFail"/> happened (by default the cost is spent, the item
    /// stays).</summary>
    Failed = 1,

    /// <summary>The item's type has no enhancement slot for the next rarity (tools, a few rings and armour pieces): nothing
    /// paid, nothing changed.</summary>
    NoSlot = 2,

    /// <summary>The item is at the rule's <see cref="RarityUpgradeRule.Max"/> or at Legendary.</summary>
    AtMax = 3,

    /// <summary>The rule does not take this item, or has no step to its next rarity.</summary>
    NotAllowed = 4,

    /// <summary>The payers hold too little of a cost item (<see cref="UpgradeResult.Message"/> says which): nothing paid.</summary>
    CantPay = 5,

    /// <summary>A <see cref="GearUpgradingEvent"/> handler canceled it: nothing paid, nothing changed.</summary>
    Canceled = 6,

    /// <summary>Refused: a payer that is no container, a cost item the game does not have or that is no plain item, the
    /// item's rarity changed since the upgrade was priced (another upgrade of it in the same step), a component laid out as
    /// Kiln does not expect, or the game's own transaction refused (see the message). Nothing changed.</summary>
    Refused = 7,

    /// <summary>Upgrades went off after the call (a fault): nothing changed.</summary>
    Unavailable = 8,

    /// <summary>The item is gone, or its ref names another item now.</summary>
    Stale = 9,

    /// <summary>The item has no enhancement slots (its type has none): it has no rarity of its own to raise.</summary>
    NotGear = 10,

    /// <summary>Not run: the server world closed, or did not step for 10 seconds.</summary>
    Dropped = 11,
}

/// <summary>What <see cref="Items.UpgradeRarity(EntityId, ItemInstanceRef, RarityUpgradeRule, UpgradeOptions?)"/> did.
/// <see cref="Ok"/> only for <see cref="UpgradeStatus.Done"/>; a failed roll (<see cref="UpgradeStatus.Failed"/>) happened
/// too and may have cost something (<see cref="Paid"/>).</summary>
/// <param name="Status">The outcome.</param>
/// <param name="Player">The player it ran for.</param>
/// <param name="Item">The item asked about.</param>
/// <param name="From">Its rarity before.</param>
/// <param name="To">The rarity the step reaches (also when it failed or was refused; <see cref="From"/> at the top).</param>
/// <param name="Before">The item before; null when it was not found.</param>
/// <param name="After">The item after (a dry run: what a success would make; a refusal: as before); null as
/// <see cref="Before"/>. With <see cref="SocketRule.GameChance"/> and a won socket it is the re-created item.</param>
/// <param name="Paid">What the payers paid, by item (a dry run: what a success would cost; empty when nothing was paid).</param>
/// <param name="Chance">The chance the roll had (after the <see cref="GearUpgradingEvent"/> handlers).</param>
/// <param name="Rolled">The server's roll, 0 (inclusive) to 1 (exclusive): below <see cref="Chance"/> succeeds. Null when
/// nothing was rolled (a chance of 0 or 1, <see cref="UpgradeOptions.Guaranteed"/>, a dry run, a refusal before the roll).</param>
/// <param name="Message">Why, for anything but <see cref="UpgradeStatus.Done"/>; what the socket roll did, when it ran.</param>
public sealed record UpgradeResult(
    UpgradeStatus Status,
    EntityId Player,
    ItemInstanceRef Item,
    ItemRarity From,
    ItemRarity To,
    GearState? Before,
    GearState? After,
    IReadOnlyDictionary<ItemId, int> Paid,
    double Chance,
    double? Rolled,
    string Message) : IKilnResult
{
    /// <summary>True when the item is one rarity higher (a dry run: would be).</summary>
    public bool Ok => Status == UpgradeStatus.Done;

    /// <summary>Why it did not happen; null when <see cref="Ok"/>.</summary>
    public string? Reason => Ok ? null : Message.Length > 0 ? Message : Status.ToString();

    /// <summary>The rule's id when it was a block (<c>"mod:name"</c>); null for a rule made in code.</summary>
    public string? Rule { get; init; }

    /// <summary>A dry run: nothing changed.</summary>
    public bool DryRun { get; init; }

    /// <summary>With <see cref="SocketRule.GameChance"/>: the item won a socket and was re-created with it.</summary>
    public bool SocketAdded { get; init; }

    /// <summary>The outcome in one line, for a log.</summary>
    public override string ToString() =>
        $"upgrade {Status} {Item}: {From} -> {To}" + (Paid.Count > 0 ? $", paid {string.Join(", ", Paid.Select(p => $"{p.Value} x {p.Key}"))}" : "") +
        (Rolled is { } r ? $", rolled {r:0.###} for {Chance:0.###}" : "") + (DryRun ? " (dry run)" : "") + (Message.Length > 0 ? $": {Message}" : "");
}

/// <summary>
/// An upgrade is about to run (<see cref="Items.UpgradeRarity(EntityId, ItemInstanceRef, RarityUpgradeRule, UpgradeOptions?)"/>,
/// from code or a station's interaction), after its checks and before anything is paid or rolled. A handler may change
/// <see cref="Cost"/> and <see cref="Chance"/>, or cancel it (<see cref="UpgradeStatus.Canceled"/>: nothing paid, nothing
/// changed). Posted on the tick thread inside the call, not for a dry run. Capability <c>items.gear.upgrade</c>.
/// </summary>
[ServerOnly]
public sealed class GearUpgradingEvent : CancelableEvent
{
    internal GearUpgradingEvent(EntityId player, GearState gear, ItemRarity to, IReadOnlyDictionary<ItemId, int> cost, double chance, string? rule, string origin)
    {
        Player = player;
        Gear = gear;
        To = to;
        Cost = cost;
        Chance = chance;
        Rule = rule;
        Origin = origin;
    }

    /// <summary>The player it runs for.</summary>
    public EntityId Player { get; }

    /// <summary>The item now.</summary>
    public GearState Gear { get; }

    /// <summary>The item.</summary>
    public ItemInstanceRef Item => Gear.Item;

    /// <summary>Its rarity now.</summary>
    public ItemRarity From => Gear.Rarity;

    /// <summary>The rarity the upgrade reaches.</summary>
    public ItemRarity To { get; }

    /// <summary>What it costs, by item (already scaled by the item's level; empty: free). Set it to change the price: at most
    /// 8 items, each 1 to 100000, plain items only (an entry of 0 or less is dropped).</summary>
    public IReadOnlyDictionary<ItemId, int> Cost { get; set; }

    /// <summary>0 to 1: the chance of success (a value outside is clamped; not a number counts as 0).</summary>
    public double Chance { get; set; }

    /// <summary>The rule's id when it is a block (<c>"mod:name"</c>); null for a rule made in code.</summary>
    public string? Rule { get; }

    /// <summary>Id of the mod that asked ("kiln" outside mod code).</summary>
    public string Origin { get; }

    /// <summary>The upgrade in one line, for a log.</summary>
    public override string ToString() => $"gear upgrading {Item}: {From} -> {To} for {Player} (chance {Chance:0.###}, by {Origin})";
}

/// <summary>An upgrade happened: it succeeded (<see cref="UpgradeStatus.Done"/>) or its roll failed
/// (<see cref="UpgradeStatus.Failed"/>). Posted on the tick thread when the result arrives (the Pre of the step after it ran),
/// before the caller's task continues; not for refusals or dry runs. The rarity change itself is also a
/// <see cref="GearRarityChangedEvent"/> with <see cref="GearChangeSource.Upgrade"/>. Capability <c>items.gear.upgrade</c>.</summary>
[ServerOnly]
public sealed class GearUpgradedEvent : KilnEvent
{
    internal GearUpgradedEvent(UpgradeResult result, string origin)
    {
        Result = result;
        Origin = origin;
    }

    /// <summary>What happened.</summary>
    public UpgradeResult Result { get; }

    /// <summary>The player it ran for.</summary>
    public EntityId Player => Result.Player;

    /// <summary>The item (after a won socket, the re-created one).</summary>
    public ItemInstanceRef Item => Result.After?.Item ?? Result.Item;

    /// <summary>It succeeded.</summary>
    public bool Succeeded => Result.Status == UpgradeStatus.Done;

    /// <summary>Id of the mod that asked ("kiln" outside mod code).</summary>
    public string Origin { get; }

    /// <summary>The upgrade in one line, for a log.</summary>
    public override string ToString() => $"gear upgraded by {Origin}: {Result}";
}
