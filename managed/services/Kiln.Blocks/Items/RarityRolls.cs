// SPDX-License-Identifier: MIT
// Rarity at creation as a block (package P5 of the equipment wave, docs/kiln/modding/item-blocks.md "Rarity at creation"):
// which item types roll which rarity when the game crafts them, loot makes them or a give asks for none. Nothing reaches
// the game at compile time: the block is checked by field and becomes the mod's rule under "mod:name". RarityRollRuntime
// resolves every loaded mod's rules to item types once the server world's item registry has them (names, "other-mod:name",
// globs) and sets them in kiln.items (ItemRarityRolls: v8 rarity_rolls), whose createPide hook rolls them.
using System.Text.Json.Nodes;
using System.Text.RegularExpressions;

namespace Kiln;

/// <summary>Which creations of an item a <see cref="RarityRollBlock"/> rolls. In JSON <c>"craft, loot"</c>.</summary>
[Flags]
public enum ItemSource
{
    /// <summary>None: a rule needs at least one.</summary>
    None = 0,
    /// <summary>The game makes the item and asks for no rarity: crafting at a station or by hand (and
    /// <c>Items.GiveRecipeResult</c>), and any other creation of the game without a rarity.</summary>
    Craft = 1,
    /// <summary>The game makes the item with a rarity it rolled or fixed: random loot in containers and world pickups, random
    /// drop-table stacks. An item whose loot entry fixes its rarity is loot too: leave <c>Loot</c> out to keep that.</summary>
    Loot = 2,
    /// <summary>A Kiln give (<c>Items.Give</c>) of the item without a <c>Gear.Rarity</c>. A give with a rarity is never rolled.</summary>
    Give = 4,
}

/// <summary>
/// A rarity rolled when an item is made: for the item types it names, the server picks Common to Legendary by weight
/// whenever the game crafts one or loot makes one, and the game then makes the item at that rarity (its enhancement slots,
/// its weapon socket chance, its durability). A block: in C#, or in the <c>rarityRolls</c> section of <c>blocks/*.json</c>.
/// <code>
/// Blocks.Define("ember-rolls", new RarityRollBlock
/// {
///     Items = ["my-mod:*"], Sources = ItemSource.Craft | ItemSource.Loot,
///     Weights = new Dictionary&lt;Rarity, double&gt; { [Rarity.Common] = 50, [Rarity.Uncommon] = 30, [Rarity.Rare] = 15, [Rarity.Epic] = 4, [Rarity.Legendary] = 1 },
/// });
/// </code>
/// A rarity whose enhancement slot the item type lacks never comes up (a tool with three slots stops at Rare); with none
/// left the game's own rarity stays. A recipe's tooltip still shows the type's rarity. Server; asks nothing of players' games.
/// </summary>
[BlockClients(ClientsLevel.None)]
public sealed record RarityRollBlock : IBlock
{
    /// <summary>The item types: <c>GameItems</c> constants, items of this mod by block name, <c>"other-mod:name"</c>, or a
    /// pattern with <c>*</c> over the game's item names (<c>"Weapon_*"</c>; <c>"my-mod:*"</c> is every item of that mod). At least one.</summary>
    public required IReadOnlyList<ItemRef> Items { get; init; }

    /// <summary>Which creations roll (default crafts and loot).</summary>
    public ItemSource Sources { get; init; } = ItemSource.Craft | ItemSource.Loot;

    /// <summary>How likely each rarity is, among the five; a rarity left out is 0. At least one above 0.</summary>
    public required IReadOnlyDictionary<Rarity, double> Weights { get; init; }
}

internal static partial class Compiler
{
    private static BlockHandle DefineRarityRoll(BlockContext ctx, Lowered low, string name, RarityRollBlock r)
    {
        var what = $"rarity roll {name}";
        RarityRollRuntime.Check(what, r);
        var handle = new BlockHandle<RarityRollBlock>(ctx.Mod.Id, name, BlockIds.Derive(ctx.Mod.Id, "rarity-roll", name), $"{ctx.Mod.Id}:{name}", r);
        low.Json["rarityRoll"] = new JsonObject
        {
            ["id"] = $"{ctx.Mod.Id}:{name}",
            ["items"] = new JsonArray(r.Items.Select(i => (JsonNode?)JsonValue.Create(i.Value)).ToArray()),
            ["sources"] = r.Sources.ToString(),
            ["weights"] = new JsonObject(RarityRollRuntime.WeightsOf(r).Select((w, i) => KeyValuePair.Create(((Rarity)i).ToString(), (JsonNode?)JsonValue.Create(w)))),
        };
        var mod = ctx.Mod.Id;
        low.AfterCommit(() => RarityRollRuntime.Register(mod, name, handle, r));
        return handle;
    }
}

/// <summary>Every loaded mod's rarity rolls, resolved to item types and set in kiln.items.</summary>
internal static class RarityRollRuntime
{
    internal sealed record Def(string Id, string Mod, string Name, long Seq, BlockHandle Handle, RarityRollBlock Block);

    private static readonly object Gate = new();
    private static readonly Dictionary<string, Def> Defs = new(StringComparer.Ordinal);
    private static readonly HashSet<ModContext> Watched = new();
    private static readonly HashSet<string> Noted = new(StringComparer.Ordinal);
    private static long _seq;
    private static bool _dirty;
    private static IDisposable? _tick;
    private static int _published = -1;       // rules set in kiln.items now (-1: never set)
    private static int _catalogCount = -1;    // the item registry's size at the last resolve
    private static long _session = -1;        // Lifecycle.ServerSession at the last resolve

    /// <summary>What is wrong with a block, by field: <see cref="BlockError"/>.</summary>
    internal static void Check(string what, RarityRollBlock block)
    {
        if (block.Items is not { Count: >= 1 and <= 1024 }) throw new BlockError(what, "items", "names 1 to 1024 items or patterns");
        for (var i = 0; i < block.Items.Count; i++)
            if (string.IsNullOrWhiteSpace(block.Items[i].Value)) throw new BlockError(what, $"items[{i}]", "an item's name, not empty");
        if (block.Sources == ItemSource.None || (block.Sources & ~(ItemSource.Craft | ItemSource.Loot | ItemSource.Give)) != 0)
            throw new BlockError(what, "sources", "craft, loot or give, at least one");
        if (block.Weights is null || block.Weights.Count == 0) throw new BlockError(what, "weights", "gives a weight to at least one rarity");
        foreach (var (rarity, weight) in block.Weights)
        {
            if (rarity < Rarity.Common || rarity > Rarity.Legendary) throw new BlockError(what, "weights", $"{rarity} is no rarity (common to legendary)");
            if (!double.IsFinite(weight) || weight < 0 || weight > 1e6) throw new BlockError(what, $"weights.{JsonName(rarity)}", $"{weight}: a weight is 0 to 1000000");
        }
        if (block.Weights.Values.All(w => w <= 0)) throw new BlockError(what, "weights", "no rarity has a weight above 0: nothing could roll");
    }

    private static string JsonName(Rarity r) => r.ToString().ToLowerInvariant();

    /// <summary>The five weights, Common .. Legendary.</summary>
    internal static double[] WeightsOf(RarityRollBlock block)
    {
        var w = new double[5];
        foreach (var (rarity, weight) in block.Weights) w[(int)rarity] = weight;
        return w;
    }

    internal static uint SourcesOf(ItemSource s) =>
        (s.HasFlag(ItemSource.Craft) ? ItemsNative.RollCraft : 0) | (s.HasFlag(ItemSource.Loot) ? ItemsNative.RollLoot : 0) |
        (s.HasFlag(ItemSource.Give) ? ItemsNative.RollGive : 0);

    /// <summary>A rule compiled: the mod's from now on. The same name again replaces it (and is the latest rule).</summary>
    internal static void Register(string mod, string name, BlockHandle handle, RarityRollBlock block)
    {
        var id = $"{mod}:{name}";
        WatchOwner(mod);
        lock (Gate)
        {
            Defs[id] = new Def(id, mod, name, ++_seq, handle, block);
            _dirty = true;
        }
        EnsureStarted();
    }

    /// <summary>A mod's blocks were reset (it reloads, or unloaded): its rules go.</summary>
    internal static void Forget(string mod)
    {
        lock (Gate)
        {
            foreach (var k in Defs.Where(d => d.Value.Mod == mod).Select(d => d.Key).ToArray()) Defs.Remove(k);
            _dirty = true;
        }
    }

    internal static Def? Find(string id)
    {
        lock (Gate) return Defs.GetValueOrDefault(id);
    }

    private static void WatchOwner(string mod)
    {
        var live = ModContext.CurrentMod is { } c && c.Info.Id == mod ? c : Mods.Get(mod)?.Info is { } info ? ModContext.FindLive(info) : null;
        if (live == null) return;
        lock (Gate)
            if (!Watched.Add(live)) return;
        live.Retiring += () =>
        {
            lock (Gate) Watched.Remove(live);
            Forget(mod);
        };
    }

    private static void EnsureStarted()
    {
        lock (Gate)
        {
            if (_tick != null) return;
            var outer = ModContext.CurrentMod;
            ModContext.CurrentMod = null;  // Kiln's own tick, not the calling mod's
            try
            {
                _tick = ServiceTick.Add("kiln rarity rolls", Tick);
            }
            finally
            {
                ModContext.CurrentMod = outer;
            }
        }
    }

    // About every two seconds: rules that changed, a new server world, or an item registry that grew (mods' items arrive
    // after the world starts) resolve again. Rules act only while this process runs a server world (a dedicated server or
    // the hosting player's game); without one the table is emptied, so a game client joined to another server rolls nothing.
    private static void Tick(in TickInfo tick)
    {
        if (tick.Step % 48 != 0) return;
        Refresh();
    }

    /// <summary>Resolves and sets the rules when something changed. Tick thread.</summary>
    internal static void Refresh()
    {
        if (!Lifecycle.IsServerRunning || !Kiln.Items.Available)
        {
            if (_published > 0) Publish([]);
            _session = -1;
            return;
        }
        var count = Kiln.Items.All.Count;
        bool dirty;
        lock (Gate) dirty = _dirty;
        if (!dirty && count == _catalogCount && Lifecycle.ServerSession == _session) return;
        Def[] defs;
        lock (Gate)
        {
            _dirty = false;
            defs = Defs.Values.OrderBy(d => d.Seq).ToArray();
        }
        _catalogCount = count;
        _session = Lifecycle.ServerSession;
        var catalog = Kiln.Items.All.Select(i => (i.Id.Value, i.Name)).ToArray();
        var rules = Resolve(defs, catalog, LootRuntime.IdOf, Note);
        Publish(rules);
    }

    private static void Publish(IReadOnlyList<ItemRarityRolls.Rule> rules)
    {
        if (rules.Count == 0 && _published <= 0) return;
        var rc = ItemRarityRolls.Set(ItemRarityRolls.BlocksOwner, rules);
        if (rc == ItemsNative.Ok)
        {
            if (rules.Count != _published) BlocksBridge.Log(LogLevel.Info, $"rarity rolls: {rules.Count} item type(s) roll their rarity at creation");
            _published = rules.Count;
        }
        else
        {
            Note($"set|{rc}", $"rarity rolls: kiln.items refused the rules ({rc}): {ItemRarityRolls.WhyOff ?? "see kiln.log"}; items are made at the game's rarity");
            _published = 0;
        }
    }

    private static void Note(string key, string text)
    {
        lock (Gate)
            if (!Noted.Add(key)) return;
        BlocksBridge.Log(LogLevel.Warn, text);
    }

    /// <summary>
    /// Every rule as kiln.items takes it: each block's items resolved against the item registry (<paramref name="catalog"/>:
    /// id and game name), in definition order, so a later block wins an item type an earlier one also names. A name is a game
    /// item, an item of the block's mod (<c>"mod.name"</c> in the game), or <c>"other-mod:name"</c>; a pattern with <c>*</c>
    /// matches game names (<c>"mod:*"</c> is <c>"mod.*"</c>). Pure but for <paramref name="idOf"/>: tested without the game.
    /// </summary>
    internal static List<ItemRarityRolls.Rule> Resolve(IReadOnlyList<Def> defs, IReadOnlyList<(uint Id, string Name)> catalog,
        Func<string, uint?> idOf, Action<string, string> note)
    {
        var byItem = new Dictionary<uint, (Def Def, ItemRarityRolls.Rule Rule)>();
        foreach (var d in defs)
        {
            var w = WeightsOf(d.Block);
            var sources = SourcesOf(d.Block.Sources);
            foreach (var r in d.Block.Items)
            {
                var ids = Match(d.Mod, r.Value, catalog, idOf);
                if (ids.Count == 0) note($"none|{d.Id}|{r.Value}", $"rarity rolls: {d.Id}: \"{r.Value}\" names no item the game has (yet); it rolls on none");
                foreach (var id in ids)
                {
                    if (byItem.TryGetValue(id, out var held) && held.Def.Mod != d.Mod)
                        note($"both|{id}|{held.Def.Id}|{d.Id}", $"rarity rolls: item {id} is named by {held.Def.Id} and {d.Id}; {d.Id}, defined later, wins");
                    byItem[id] = (d, new ItemRarityRolls.Rule(id, sources, w[0], w[1], w[2], w[3], w[4]));
                }
            }
        }
        return byItem.Values.Select(v => v.Rule).OrderBy(r => r.Item).ToList();
    }

    /// <summary>The item types one entry of <see cref="RarityRollBlock.Items"/> names.</summary>
    internal static List<uint> Match(string mod, string entry, IReadOnlyList<(uint Id, string Name)> catalog, Func<string, uint?> idOf)
    {
        var s = entry.Trim();
        var colon = s.IndexOf(':');
        if (colon > 0) s = $"{s[..colon]}.{s[(colon + 1)..]}";  // "other-mod:name" is "other-mod.name" in the game
        if (s.Contains('*'))
        {
            var rx = new Regex("^" + Regex.Escape(s).Replace("\\*", ".*") + "$", RegexOptions.CultureInvariant);
            return catalog.Where(c => c.Name.Length > 0 && rx.IsMatch(c.Name)).Select(c => c.Id).Distinct().ToList();
        }
        var id = idOf(s) ?? (colon > 0 ? null : idOf($"{mod}.{s}"));
        return id is { } found ? [found] : [];
    }

    /// <summary>Tests: forget every rule and the published state.</summary>
    internal static void ResetForTests()
    {
        lock (Gate)
        {
            Defs.Clear();
            Noted.Clear();
            _dirty = false;
        }
        _published = -1;
        _catalogCount = -1;
        _session = -1;
    }
}
