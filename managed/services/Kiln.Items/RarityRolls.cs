// SPDX-License-Identifier: MIT
// kiln.items v8 rarity rolls on the managed side (native/core/abi/services/kiln_items.h "rarity rolls", package P5 of the
// equipment wave), internal: Kiln.Blocks' RarityRollBlock (rarityRolls) resolves its rules to item types and sets them
// here. One table for all mods, under Kiln's own owner (0): Kiln.Blocks merges the mods' blocks itself.
namespace Kiln;

/// <summary>Rarity rolls at creation: the native rule table and its status. Internal to Kiln.Items and Kiln.Blocks.</summary>
internal static unsafe class ItemRarityRolls
{
    /// <summary>The capability row: the sigdb feature of the createPide hook.</summary>
    internal const string Capability = "items.gear.rolls";

    /// <summary>The owner Kiln.Blocks sets the merged table under (Kiln's own; no mod's unload drops it).</summary>
    internal const ulong BlocksOwner = 0;

    /// <summary>One item type's rule: sources (ItemsNative.Roll*) and the weights Common .. Legendary.</summary>
    internal readonly record struct Rule(uint Item, uint Sources, double Common, double Uncommon, double Rare, double Epic, double Legendary);

    /// <summary>The v8 table with rarity_rolls, or null when kiln.items is older (or not here).</summary>
    internal static ItemsNative.TableV8* V8
    {
        get
        {
            var t = Services.Get<ItemsNative.Table>(ItemsNative.Service, ItemsNative.Version);
            if (t == null || t->Header.Size < (uint)sizeof(ItemsNative.TableV8)) return null;
            return (ItemsNative.TableV8*)t;
        }
    }

    /// <summary>The native status, or null when kiln.items has no rarity rolls (older, or not here).</summary>
    internal static ItemsNative.RollStatus? Status()
    {
        var t = V8;
        if (t == null) return null;
        var s = new ItemsNative.RollStatus { Size = (uint)sizeof(ItemsNative.RollStatus) };
        return t->RollStatusOf(&s) == ItemsNative.Ok ? s : null;
    }

    /// <summary>Null when rolls can act here, else why not (for logs and the capability's text).</summary>
    internal static string? WhyOff
    {
        get
        {
            if (V8 == null) return Items.Available ? "kiln.items here has no rarity rolls (v8, package P5; update Kiln)" : "kiln.items is not loaded";
            if (Status() is not { } s) return "kiln.items did not answer roll_status";
            if (s.Available != 0) return null;
            var why = ItemsNative.Text(s.Detail, 160);
            return why.Length > 0 ? why : "feature items.gear.rolls is off on this game build (see capabilities)";
        }
    }

    /// <summary>The native entries of <paramref name="rules"/> (pure: tested without the game).</summary>
    internal static ItemsNative.RarityRoll[] Entries(IReadOnlyList<Rule> rules)
    {
        var a = new ItemsNative.RarityRoll[rules.Count];
        for (var i = 0; i < rules.Count; i++)
        {
            var r = rules[i];
            a[i].Item = r.Item;
            a[i].Sources = r.Sources;
            a[i].Weights[0] = (float)r.Common;
            a[i].Weights[1] = (float)r.Uncommon;
            a[i].Weights[2] = (float)r.Rare;
            a[i].Weights[3] = (float)r.Epic;
            a[i].Weights[4] = (float)r.Legendary;
        }
        return a;
    }

    /// <summary>Replaces <paramref name="owner"/>'s rules (none: ends them). The kiln.items result code; Unavailable when
    /// the table has no rarity rolls.</summary>
    internal static int Set(ulong owner, IReadOnlyList<Rule> rules)
    {
        var t = V8;
        if (t == null) return ItemsNative.Unavailable;
        if (rules.Count == 0) return t->RarityRolls(owner, null, 0, 0);
        var entries = Entries(rules);
        fixed (ItemsNative.RarityRoll* p = entries)
            return t->RarityRolls(owner, p, (uint)entries.Length, (uint)sizeof(ItemsNative.RarityRoll));
    }
}
