// SPDX-License-Identifier: MIT
using System.Buffers;
using System.Globalization;
using System.Runtime.InteropServices;
using System.Text;

namespace Kiln;

/// <summary>A constructed game text line, in display order. Value is the computed stat number, when this is the first
/// text row of a stat draw; it is metadata, not an extra number to append to Text. A number-only draw has empty Text.</summary>
public sealed record UiItemDetailLine(string Text, float? Value);

/// <summary>An owned copy of the last completed panel for this item/variant. Text uses the language at capture time.
/// Reinspect after changing language or item state. These presentation values are not gameplay authority.</summary>
public sealed record UiItemDetailSnapshot(ItemId Item, EntityId Entity, int Level, ItemRarity? Rarity,
    ulong Session, ulong Frame, ulong CaptureMs, IReadOnlyList<UiItemDetailLine> Lines);

/// <summary>Ok when a game panel was captured. Otherwise Snapshot is null and Reason explains the miss.
/// UiDraw.ItemDetail still draws its explicitly labelled Kiln fallback on a cache miss.</summary>
public sealed record UiItemDetailResult(bool Ok, string? Reason, UiItemDetailSnapshot? Snapshot) : IKilnResult;

public static partial class Ui
{
    public static partial class Values
    {
        /// <summary>Capturing the game's item-detail text and computed numbers (not its layout, icons or models).</summary>
        public const string ItemDetailCapability = "ui.item_detail";

        /// <summary>Starts capture on first use and reads a completed panel for this entity. Optional item/level/rarity
        /// constrain the match; an instance never borrows another instance's stats. No game/world reads on this thread.</summary>
        [ClientOnly, AnyThread, RequiresCapability(ItemDetailCapability)]
        public static UiItemDetailResult ItemDetail(EntityId entity, ItemId item = default, int? level = null, ItemRarity? rarity = null) =>
            UiItemDetailBridge.Read(entity, item, level, rarity);

        /// <summary>Reads the most recently inspected variant of this item type in this client session. Prefer an entity
        /// or a slot when instance accuracy matters. Retains the last 64 inspected instances/variants.</summary>
        [ClientOnly, AnyThread, RequiresCapability(ItemDetailCapability)]
        public static UiItemDetailResult ItemDetail(ItemId item) => UiItemDetailBridge.Read(default, item, null, null);
    }
}

public sealed unsafe partial class UiDraw
{
    /// <summary>Draws captured game text/numbers in Kiln's UI, wrapping to width pixels (0 = available width). Not a copy
    /// of the game's layout. If unseen or capture is unavailable, draws a labelled name-only Kiln fallback and returns
    /// Ok=false. Capture starts on first use: inspect the item in the game's panel afterwards.</summary>
    [ClientOnly, RenderThread]
    public UiItemDetailResult ItemDetail(ItemId item, float width = 0) =>
        DrawItemDetail(default, item, null, null, width, null, null);

    /// <summary>Like the item-id overload, but uses this exact item entity. No live entity access on the render thread.</summary>
    [ClientOnly, RenderThread]
    public UiItemDetailResult ItemDetail(EntityId entity, float width = 0) =>
        DrawItemDetail(entity, default, null, null, width, null, null);

    /// <summary>Draws this visible slot's instance/variant. The fallback uses its captured rarity, level and stack count.</summary>
    [ClientOnly, RenderThread]
    public UiItemDetailResult ItemDetail(VisibleSlot slot, float width = 0)
    {
        ArgumentNullException.ThrowIfNull(slot);
        return DrawItemDetail(slot.ClientEntity, slot.Item, slot.Rarity.HasValue ? slot.Level : null, slot.Rarity,
            width, null, slot.Count);
    }

    /// <summary>Draws a saved server inventory slot; no Items.Read on the render thread. The client must have inspected
    /// that instance in this session. The fallback uses the slot's known name, rarity, level and count.</summary>
    [ClientOnly, RenderThread]
    public UiItemDetailResult ItemDetail(InventorySlot slot, float width = 0)
    {
        ArgumentNullException.ThrowIfNull(slot);
        return DrawItemDetail(slot.ItemEntity, slot.Item, slot.Level, slot.Rarity, width, null, (uint)Math.Max(0, slot.Count));
    }

    /// <summary>Draws the latest captured variant of this catalog item, or a fallback with its known catalog metadata.
    /// Catalog ranges are not instance stats.</summary>
    [ClientOnly, RenderThread]
    public UiItemDetailResult ItemDetail(ItemInfo item, float width = 0)
    {
        ArgumentNullException.ThrowIfNull(item);
        return DrawItemDetail(default, item.Id, null, null, width, item, null);
    }

    private UiItemDetailResult DrawItemDetail(EntityId entity, ItemId item, int? level, ItemRarity? rarity,
        float width, ItemInfo? info, uint? count)
    {
        if (!float.IsFinite(width) || width < 0) throw new ArgumentOutOfRangeException(nameof(width));
        UiItemDetailBridge.Validate(entity, item, level, rarity);
        UiItemDetailResult result;
        try { result = UiItemDetailBridge.Read(entity, item, level, rarity); }
        catch (KilnUnavailableException) { result = new(false, "item detail capture unavailable", null); }
        var rows = new List<string>();
        if (result.Snapshot is { } snapshot)
            rows.AddRange(snapshot.Lines.Select(l => l.Text.Length != 0 ? l.Text : l.Value?.ToString("G", CultureInfo.InvariantCulture) ?? ""));
        else
        {
            rows.Add(item.IsNone ? $"Item entity {entity}" : Items.DisplayName(item) ?? info?.Label ?? $"Item {item}");
            rows.Add("Game detail not captured — Kiln fallback");
            if ((rarity ?? info?.Rarity) is { } r) rows.Add($"Rarity: {r}");
            if (level is { } l) rows.Add($"Level: {l}");
            if (count is { } n) rows.Add($"Count: {n}");
            if (info is not null)
            {
                rows.Add($"Category: {info.Category}");
                rows.Add($"Catalog levels: {info.LevelMin}–{info.LevelMax}");
                rows.Add($"Max stack: {info.MaxStack}");
            }
        }
        var available = Cursor.AvailWidth;
        if (width == 0) width = available > 0 ? available : 320;
        else if (available > 0) width = Math.Min(width, available);
        foreach (var row in UiTooltipWrap.Wrap(rows, Math.Max(1, width), s => MeasureText(s).Width)) Text(row);
        return result;
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
            if (s.Ok == 0) return new(false, "game detail not captured in this session", null);
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
