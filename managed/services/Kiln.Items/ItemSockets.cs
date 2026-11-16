// SPDX-License-Identifier: MIT
// kiln.items v7 on the managed side (native/core/abi/services/kiln_items.h "gem sockets"), internal: the part of the gem
// wave's sockets on any gear (Kiln.Abilities' Gems.AllowSockets / AddSocket, docs/kiln/services/gems-sockets.md) that
// talks to kiln.items. Which gear kinds may get a socket is kept here (per mod), so Items.Give's Gear.GemSlot follows it.
using System.Runtime.InteropServices;

namespace Kiln;

/// <summary>Gem sockets on any gear: the kinds mods allow, the native socket rolls, the gear's state for a re-creation and
/// the re-creating give (KILN_ITEMS_REPLACE). Internal to Kiln.Items and Kiln.Abilities.</summary>
internal static unsafe class ItemSockets
{
    // Kind bits, the same values as Kiln.Abilities' public GemSocketKinds.
    internal const uint Rings = 1, Armour = 2, Shields = 4, Other = 8, AllKinds = 15;

    /// <summary>The kind bit of an item (0: none of the four: a weapon, a gem, a dye, customization, an unknown item), as
    /// kiln.items' createPide hook classifies it: Equipment by slot type (37 ring, 13-17 armour, 18 shield, else other),
    /// any other category but Weapons, WeaponGem, ColorPalette and Customization is other.</summary>
    internal static uint KindOf(ItemCategory category, byte equipSlot) => category switch
    {
        ItemCategory.Weapons or ItemCategory.WeaponGem or ItemCategory.ColorPalette or ItemCategory.Customization => 0,
        ItemCategory.Equipment => equipSlot switch
        {
            37 => Rings,
            >= 13 and <= 17 => Armour,
            18 => Shields,
            _ => Other,
        },
        _ => Other,
    };

    /// <summary>The kind bit of a catalog item; 0 when unknown or not one of the four.</summary>
    internal static uint KindOf(ItemId item) => Items.Get(item) is { } info ? KindOf(info.Category, info.EquipSlot) : 0;

    private static readonly object Gate = new();
    private static readonly Dictionary<ModContext, uint> Allowed = new();
    private static uint _kiln;  // Kiln's own (no mod on the thread: a test, the host)
    private static uint _union;

    /// <summary>The kinds any mod allows now.</summary>
    internal static uint AllowedKinds => Volatile.Read(ref _union);

    /// <summary>Sets the calling mod's allowed kinds (0 = none: that mod's allowance ends). Ends with the mod.</summary>
    internal static void SetAllowed(ModContext? mod, uint kinds)
    {
        lock (Gate)
        {
            if (mod == null) _kiln = kinds;
            else if (kinds == 0) Allowed.Remove(mod);
            else
            {
                if (!Allowed.ContainsKey(mod)) mod.Retiring += () => SetAllowed(mod, 0);
                Allowed[mod] = kinds;
            }
            var u = _kiln;
            foreach (var k in Allowed.Values) u |= k;
            Volatile.Write(ref _union, u & AllKinds);
        }
    }

    /// <summary>Items.Give's Gear.GemSlot on a non-weapon: true when the item's kind is allowed and the server part works
    /// here. Otherwise the give goes on without the socket (as before v7, and as Gear.GemSlot's documentation says),
    /// instead of failing as a whole on a build where gems.sockets is off.</summary>
    internal static bool AllowsGive(ItemId item)
    {
        var allowed = AllowedKinds;
        return allowed != 0 && GivesAnySlot(KindOf(item), allowed, static () => ServerSocketsOn);
    }

    /// <summary>The decision of <see cref="AllowsGive"/>: a kind of the four, allowed, and (asked last, it calls kiln.items)
    /// the server part on. Pure but for <paramref name="serverOn"/>.</summary>
    internal static bool GivesAnySlot(uint kind, uint allowed, Func<bool> serverOn) =>
        kind != 0 && (allowed & kind) != 0 && serverOn();

    /// <summary>kiln.items here has v7 and feature gems.sockets (<see cref="WhyOff"/> is null).</summary>
    internal static bool ServerSocketsOn => WhyOff == null;

    // ---- the native side ----

    private static ItemsNative.TableV7* _v7;

    /// <summary>The v7 table, or null when kiln.items is older (or not here).</summary>
    internal static ItemsNative.TableV7* V7
    {
        get
        {
            if (_v7 != null) return _v7;
            var t = Services.Get<ItemsNative.Table>(ItemsNative.Service, ItemsNative.Version);
            if (t == null || t->Header.Size < (uint)sizeof(ItemsNative.TableV7)) return null;
            return _v7 = (ItemsNative.TableV7*)t;
        }
    }

    /// <summary>kiln.items here has v7.</summary>
    internal static bool HasV7 => V7 != null;

    /// <summary>Null when the server-side sockets work here (kiln.items v7 with feature gems.sockets), else why not.</summary>
    internal static string? WhyOff
    {
        get
        {
            var t = V7;
            if (t == null)
                return Items.Available ? "kiln.items here has no gem sockets (v7; update Kiln)" : "kiln.items is not loaded";
            // An invalid roll (size 0) changes nothing: kiln.items answers Unavailable when the feature is off (checked
            // first), else InvalidArgument. A valid all-zero roll would end owner 0's rolls (Kiln's own, from a call
            // with no mod on the thread), so the probe never sends one.
            var probe = new ItemsNative.SocketRoll();
            var rc = t->SocketRollOf(0, &probe);
            return rc == ItemsNative.InvalidArgument ? null
                : rc == ItemsNative.Unavailable ? "feature gems.sockets is off on this game build (see capabilities)"
                : $"kiln.items answered the probe with {rc}";
        }
    }

    /// <summary>Sets <paramref name="owner"/>'s socket rolls (chances 0..1; all 0 ends them). The kiln.items result code.</summary>
    internal static int SetRoll(ulong owner, float rings, float armour, float shields, float other)
    {
        var t = V7;
        if (t == null) return ItemsNative.Unavailable;
        var r = new ItemsNative.SocketRoll
        {
            Size = (uint)sizeof(ItemsNative.SocketRoll), Rings = rings, Armour = armour, Shields = shields, Other = other,
        };
        return t->SocketRollOf(owner, &r);
    }

    /// <summary>One item entity's state as a re-creation needs it (tick thread, server).</summary>
    internal readonly record struct GearState(
        bool Found, bool Placed, bool Equipped, ItemId Item, EntityId Holder, SlotId Slot, bool HasSocket, ItemId Gem, int GemLevel,
        int Level, bool HasDurability, int Durability, int DurabilityMax, ItemId Dye, int PerksAvailable, int PerksUnlocked);

    /// <summary>Reads one item entity (kiln.items v4 read_instances). Throws as ItemInstances does when it is off.</summary>
    internal static GearState Read(ItemInstanceRef item)
    {
        ItemInstances.Require();
        Span<ItemsNative.Instance> one = stackalloc ItemsNative.Instance[1];
        one[0].Entity = item.Entity.Id.Value;
        ItemInstances.ReadRaw(one, 0, "Gems.AddSocket");
        ref var x = ref one[0];
        var found = (x.Flags & ItemsNative.InstFound) != 0 && x.Item == item.Item.Value &&
                    ItemInstances.RefOf(in x) is var r && !r.IsNone && r.Entity.Serial == item.Entity.Serial;
        return new GearState(found, (x.Flags & ItemsNative.InstPlaced) != 0, (x.Flags & ItemsNative.InstEquipped) != 0,
            new ItemId(x.Item), new EntityId(x.Holder), new SlotId(x.Slot), (x.Flags & ItemsNative.InstGem) != 0,
            new ItemId(x.Gem), x.GemLevel, (x.Flags & ItemsNative.InstLevel) != 0 ? (int)x.Level : 0,
            (x.Flags & ItemsNative.InstDurability) != 0, x.Durability, x.DurabilityMax,
            (x.Flags & ItemsNative.InstDye) != 0 ? new ItemId(x.Dye) : ItemId.None, x.PerksAvailable, x.PerksUnlocked);
    }

    /// <summary>What a re-creation asks kiln.items for: the item at its base level (the level without the unlocked
    /// upgrades, which the give unlocks again and adds back), its rarity, its upgrades, dye and durability, and a socket.
    /// Pure, so it is tested without the game.</summary>
    internal static ItemsNative.RequestEx RecreateRequest(EntityId holder, GearState g, ItemRarity? rarity, bool socket)
    {
        var rx = new ItemsNative.RequestEx
        {
            Base = new ItemsNative.Request
            {
                Size = (uint)sizeof(ItemsNative.RequestEx), Flags = ItemsNative.Replace | ItemsNative.AllOrNothing, Entity = holder.Value,
                Item = g.Item.Value, Count = 1, Slot = g.Slot.Value,  // Pide: set by Recreate
            },
        };
        var baseLevel = Math.Clamp(g.Level - g.PerksUnlocked, 1, 255);
        if (g.Level > 0)
        {
            rx.Base.Flags |= ItemsNative.Level;
            rx.Base.Level = (uint)baseLevel;
        }
        if (rarity is { } ra)
        {
            rx.Base.Flags |= ItemsNative.Rarity;
            rx.Base.Rarity = (byte)ra;
        }
        if (g.PerksUnlocked > 0)
        {
            rx.Base.Flags |= ItemsNative.Unlock;
            rx.Base.Unlock = g.PerksUnlocked >= 5 ? (byte)255 : (byte)g.PerksUnlocked;
        }
        if (socket) rx.Base.Flags |= ItemsNative.GemSlot | ItemsNative.GemSlotAny;
        if (!g.Dye.IsNone)
        {
            rx.Base.Flags |= ItemsNative.Dye;
            rx.DyeItem = g.Dye.Value;
        }
        if (g.HasDurability && g.DurabilityMax > 0)
        {
            rx.Base.Flags |= ItemsNative.DurabilityFlag;
            rx.Durability = Math.Clamp((float)g.Durability / g.DurabilityMax, 0f, 1f);
        }
        return rx;
    }

    /// <summary>Re-creates the item entity <paramref name="pide"/> in its slot (one transaction: take, then give).</summary>
    internal static Task<ItemsResult> Recreate(EntityId holder, EntityId pide, ItemsNative.RequestEx rx)
    {
        rx.Base.Pide = pide.Value;
        return Items.SubmitEx(false, rx, "Gems.AddSocket");
    }

    /// <summary>The rarity of the item entity in <paramref name="slot"/> of <paramref name="holder"/> (ItemState), or null.</summary>
    internal static ItemRarity? RarityAt(EntityId holder, SlotId slot, EntityId pide)
    {
        var inv = Items.Read(holder);
        if (inv == null) return null;
        foreach (var s in inv.Slots)
            if (s.Slot == slot && s.ItemEntity == pide) return s.Rarity;
        return null;
    }
}
