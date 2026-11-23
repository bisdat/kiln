// SPDX-License-Identifier: MIT
using System.Buffers;
using System.Runtime.InteropServices;
using System.Text;

namespace Kiln;

/// <summary>A constructed game text line, in display order. Value is the computed stat number, when this is the first
/// text row of a stat draw; it is metadata, not an extra number to append to Text. A number-only draw has empty Text.
/// Since 0.2.0-dev.30.5.</summary>
/// <param name="Text">The line's text, in the language at capture time.</param>
/// <param name="Value">The computed stat number of a stat's first text row; null otherwise.</param>
public sealed record UiItemDetailLine(string Text, float? Value);

/// <summary>An owned copy of the last completed panel for this item/variant. Text uses the language at capture time.
/// Reinspect after changing language or item state. These presentation values are not gameplay authority.
/// Since 0.2.0-dev.30.5.</summary>
/// <param name="Item">The item's id.</param>
/// <param name="Entity">The client entity of the item the panel was captured for.</param>
/// <param name="Level">The item's level.</param>
/// <param name="Rarity">The item's rarity, when known.</param>
/// <param name="Session">The client session of the capture.</param>
/// <param name="Frame">The game UI frame of the capture.</param>
/// <param name="CaptureMs">When it was captured, on the <c>Environment.TickCount64</c> clock.</param>
/// <param name="Lines">The panel's text lines in display order.</param>
public sealed record UiItemDetailSnapshot(ItemId Item, EntityId Entity, int Level, ItemRarity? Rarity,
    ulong Session, ulong Frame, ulong CaptureMs, IReadOnlyList<UiItemDetailLine> Lines);

/// <summary>Ok when a game panel was captured. Otherwise Snapshot is null and Reason explains the miss.
/// No panel or computed fallback is supplied; a mod can show a name/id and this reason. Since 0.2.0-dev.30.5.</summary>
/// <param name="Ok">True when a panel was captured.</param>
/// <param name="Reason">Why not; null when <see cref="Ok"/>.</param>
/// <param name="Snapshot">The captured panel; null when not <see cref="Ok"/>.</param>
public sealed record UiItemDetailResult(bool Ok, string? Reason, UiItemDetailSnapshot? Snapshot) : IKilnResult;

public static partial class Ui
{
    public static partial class Values
    {
        /// <summary>Passive capture of the game's item-detail text and computed numbers, not on-demand computation or drawing.
        /// Since 0.2.0-dev.30.5.</summary>
        public const string ItemDetailCapability = "ui.item_detail";

        /// <summary>Starts capture on first use and reads a completed panel for this CLIENT presentation entity (for example,
        /// VisibleSlot.ClientEntity), not an arbitrary server inventory entity. Optional item/level/rarity constrain the match.
        /// The client main/UI thread captures; any thread can read owned copies. No live item/world-memory reads here.
        /// Since 0.2.0-dev.30.5.</summary>
        [ClientOnly, AnyThread, RequiresCapability(ItemDetailCapability)]
        public static UiItemDetailResult ItemDetail(EntityId entity, ItemId item = default, int? level = null, ItemRarity? rarity = null) =>
            UiItemDetailBridge.Read(entity, item, level, rarity);

        /// <summary>Reads the most recently inspected matching variant in this client session, optionally constrained by
        /// level and rarity. It can include the inspected instance's gems/perks, not a pristine type's computed defaults.
        /// Retains 64 captures/refusals in capture order. A world change or absent local player clears the cache on next use.
        /// Language and other item-state changes require reinspection. No on-demand game computation. Since 0.2.0-dev.30.5.</summary>
        [ClientOnly, AnyThread, RequiresCapability(ItemDetailCapability)]
        public static UiItemDetailResult ItemDetail(ItemId item, int? level = null, ItemRarity? rarity = null) =>
            UiItemDetailBridge.Read(default, item, level, rarity);
    }
}

internal static unsafe class UiItemDetailNative
{
    internal const string Service = "kiln.ui.item_detail";
    [StructLayout(LayoutKind.Sequential)]
    internal struct Query { public uint Size, Entity, Item, Level, Rarity; }
    [StructLayout(LayoutKind.Sequential)]
    internal struct Line { public uint Size, Flags; public float Value; public uint TextBytes; public fixed byte Text[4096]; }
    [StructLayout(LayoutKind.Sequential)]
    internal struct Snapshot
    {
        public uint Size, Ok, Count, Item, Entity, Level, Rarity, Reserved;
        public ulong Session, Frame, CaptureMs;
    }
    [StructLayout(LayoutKind.Sequential)]
    internal struct Table
    {
        public ServiceHeader Header;
        public delegate* unmanaged[Cdecl]<ulong, Query*, Snapshot*, Line*, uint, int> Copy;
    }
    internal static Table* T
    {
        get
        {
            var t = Services.Get<Table>(Service, 1);
            return t != null && t->Header.Size >= sizeof(Table) && t->Copy != null ? t : null;
        }
    }
}

internal static unsafe class UiItemDetailBridge
{
    internal static void Validate(EntityId entity, ItemId item, int? level, ItemRarity? rarity)
    {
        if (entity.IsNone && item.IsNone) throw new ArgumentException("an item or entity is required");
        if (level < 0) throw new ArgumentOutOfRangeException(nameof(level));
        if (rarity.HasValue && (byte)rarity.Value > (byte)ItemRarity.Legendary) throw new ArgumentOutOfRangeException(nameof(rarity));
    }
    internal static UiItemDetailResult Read(EntityId entity, ItemId item, int? level, ItemRarity? rarity)
    {
        Validate(entity, item, level, rarity);
        UiValueBridge.RequireCapability(Ui.Values.ItemDetailCapability);
        var t = UiItemDetailNative.T;
        if (t == null) throw new KilnUnavailableException(Ui.Values.ItemDetailCapability, "kiln.ui.item_detail is not available");
        var q = new UiItemDetailNative.Query { Size = (uint)sizeof(UiItemDetailNative.Query), Entity = entity.Value,
            Item = item.Value, Level = level is { } l ? (uint)l : uint.MaxValue, Rarity = rarity is { } r ? (uint)r : uint.MaxValue };
        var s = new UiItemDetailNative.Snapshot { Size = (uint)sizeof(UiItemDetailNative.Snapshot) };
        var buffer = ArrayPool<UiItemDetailNative.Line>.Shared.Rent(64);
        try
        {
            int rc;
            fixed (UiItemDetailNative.Line* p = buffer) rc = t->Copy(UiValueBridge.OwnerOf(UiModCalls.Caller), &q, &s, p, 64);
            if (rc == UiValuesNative.Unavailable) throw new KilnUnavailableException(Ui.Values.ItemDetailCapability, "capture hooks unavailable");
            if (rc == UiValuesNative.Revoked) throw new InvalidOperationException("the mod is unloading");
            if (rc != 0) throw new InvalidOperationException($"kiln.ui.item_detail: copy failed ({rc})");
            if (s.Ok == 0) return new(false, s.Reserved switch
            {
                1 => "no active client session (offline or no local player)",
                2 => "game detail capture rejected (empty panel, invalid text/number, or capture budget)",
                _ => "game detail not captured for this instance or variant in this session; inspect it in the game's panel",
            }, null);
            if (s.Count > 64) throw new InvalidOperationException("invalid item detail line count");
            var lines = new UiItemDetailLine[s.Count];
            for (var i = 0; i < lines.Length; i++)
            {
                ref var row = ref buffer[i];
                if (row.TextBytes > 4096) throw new InvalidOperationException("invalid item detail text length");
                fixed (byte* text = row.Text) lines[i] = new(Encoding.UTF8.GetString(text, (int)row.TextBytes), (row.Flags & 1) != 0 ? row.Value : null);
            }
            return new(true, null, new(new(s.Item), new(s.Entity), (int)Math.Min(s.Level, int.MaxValue),
                s.Rarity <= (uint)ItemRarity.Legendary ? (ItemRarity)s.Rarity : null,
                s.Session, s.Frame, s.CaptureMs, Array.AsReadOnly(lines)));
        }
        finally { ArrayPool<UiItemDetailNative.Line>.Shared.Return(buffer, clearArray: true); }
    }
}
