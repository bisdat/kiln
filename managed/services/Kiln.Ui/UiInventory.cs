// SPDX-License-Identifier: MIT
// Ui.Inventory (0.2.0-dev.30): the game's own inventory slots as something a mod acts on. Marks drawn over the slots the
// game draws, keyed by the item (the server's entity id), and clicks on them with the item's identity. Native:
// kiln.ui.inventory (native/core/abi/services/kiln_ui_inventory.h) on kiln.ui.values' slot frames and kiln.ui's
// presses. Picture and wish only: a mod's server checks the item before it acts. docs/kiln/ui.md "The game's
// inventory slots".
using System.Collections.Concurrent;
using System.Numerics;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace Kiln;

/// <summary>A mouse button on a slot.</summary>
public enum SlotButton
{
    Left = 1,
    Right = 3,
}

/// <summary>Modifier keys held with a click (exact: Ctrl alone is not Ctrl+Shift).</summary>
[Flags]
public enum SlotModifiers
{
    None = 0,
    Ctrl = 1,
    Shift = 2,
    Alt = 4,
}

/// <summary>How a mark is drawn on a slot.</summary>
public enum SlotMarkStyle
{
    /// <summary>A frame along the slot's edge.</summary>
    Border = 0,
    /// <summary>A tint over the whole slot (use a colour with alpha, e.g. 80).</summary>
    Fill = 1,
    /// <summary>A small square in the slot's top-right corner.</summary>
    Corner = 2,
}

/// <summary>A mark on a slot: drawn over the game's slot, in this player's game only.</summary>
public readonly record struct SlotMark(Color Color, SlotMarkStyle Style = SlotMarkStyle.Border, float Thickness = 3);

/// <summary>Which clicks a handler gets (<see cref="Ui.Inventory.OnClick"/>).</summary>
public sealed record SlotClickOptions
{
    public SlotButton Button { get; init; } = SlotButton.Left;
    /// <summary>Held exactly (None: a plain click).</summary>
    public SlotModifiers Modifiers { get; init; }
    /// <summary>The game never sees the press (no pick-up, no move). False: the game acts on it too and you only watch.</summary>
    public bool Take { get; init; } = true;
}

/// <summary>A click on a slot the game drew. <see cref="ItemEntity"/> is the item's entity in this game's world: for an
/// item the server sent (every item in a player's or a chest's inventory), that is the server's id of it. Send it to your
/// server half, which checks it (holder, slot, type) before acting: a click is a wish, never authority.</summary>
public sealed record SlotClick(SlotButton Button, SlotModifiers Modifiers, VisibleSlot Slot, GameScreen Screen, bool Taken)
{
    public ItemId Item => Slot.Item;
    /// <summary>The item entity (weapons, tools, armour, cans). EntityId 0 for an empty slot and for a plain stack
    /// (materials, consumables), which the game keeps without an item entity: name it by Holder, slot and Item.</summary>
    public EntityId ItemEntity => Slot.ClientEntity;
    /// <summary>The entity whose inventory holds the slot (the player, a chest), when the game's models said.</summary>
    public EntityId Holder => Slot.Holder;

    /// <summary>
    /// Where the button went down, in the overlay's pixels: the space <see cref="UiDraw"/> draws in and
    /// <see cref="UiDraw.Pointer"/> reports, with (0, 0) at the top left of the game's picture. Kiln reads the press in the
    /// game window's client pixels, which are the overlay's pixels (the overlay is drawn at the window's size and Dear ImGui
    /// reads its pointer the same way), so it can be compared with a mark's or a panel's rectangle, or checked against
    /// <see cref="VisibleSlot"/>'s rectangles once mapped to pixels. Not UI units: <see cref="VisibleSlot.OuterUi"/> is in
    /// the game's units. Null for a click Kiln did not deliver (one a test made). Since 0.2.0-dev.30.3 (mod API 0.2.13).
    /// </summary>
    public Vector2? Pointer { get; init; }
}

public static partial class Ui
{
    /// <summary>The game's own inventory slots (backpack, chests, stations): marks and clicks; panels beside the backpack and
    /// chest screens, and which of them is open with the chest's entity (since 0.2.0-dev.30). Client only.</summary>
    public static partial class Inventory
    {
        /// <summary>The capability row of <see cref="OnClick"/>.</summary>
        public const string ClickCapability = "ui.slots.click";
        /// <summary>The capability row of marks and <see cref="Hovered"/> (the slot inspection).</summary>
        public const string MarkCapability = Values.SlotsCapability;

        /// <summary>Marks the item's slot wherever the game draws it (backpack, chest, hotbar) until <see cref="Unmark"/>
        /// or your mod unloads. One mark per item and mod; a later call replaces it.</summary>
        [ClientOnly, AnyThread, RequiresCapability(MarkCapability)]
        public static void Mark(EntityId item, SlotMark mark) => UiInventoryBridge.Mark(item, mark);

        [ClientOnly, AnyThread]
        public static void Unmark(EntityId item) => UiInventoryBridge.Unmark(item);

        /// <summary>Removes every mark of your mod.</summary>
        [ClientOnly, AnyThread]
        public static void ClearMarks() => UiInventoryBridge.ClearMarks();

        /// <summary>A rule asked for each slot the game drew, every overlay frame (render thread: keep it a lookup).
        /// null = no mark. Dispose to remove it.</summary>
        [ClientOnly, AnyThread, RequiresCapability(MarkCapability)]
        public static IDisposable MarkWhen(Func<VisibleSlot, SlotMark?> rule) => UiInventoryBridge.AddRule(rule);

        /// <summary>Calls <paramref name="handler"/> (on the client's main thread, after the next UI frame) for each click
        /// matching <paramref name="options"/> on a slot of a screen with a pointer: the backpack and inventory menu, a
        /// chest, a crafting station or factory, an NPC's crafting. Never in the play view (the hotbar there attacks).
        /// Dispose to stop. Throws KilnUnavailableException when the row is off.</summary>
        [ClientOnly, AnyThread, RequiresCapability(ClickCapability)]
        public static IDisposable OnClick(SlotClickOptions options, Action<SlotClick> handler) =>
            UiInventoryBridge.Subscribe(options, handler);

        /// <summary>The slot under the cursor in the game's latest UI frame (null: none, or not known).</summary>
        [ClientOnly, AnyThread]
        public static VisibleSlot? Hovered => UiInventoryBridge.Hovered();
    }
}

internal static unsafe class UiInventoryNative
{
    public const string Service = "kiln.ui.inventory";
    public const uint ClickTake = 1;
    public const uint MapScaled = 1, MapVerified = 2;

    [StructLayout(LayoutKind.Sequential)]
    public struct ClickFilter
    {
        public uint Size, Button, Mods, Flags;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct Click
    {
        public uint Size, Button, Mods, Taken;
        public float X, Y;
        public ulong Session, Frame, PressMs;
        public UiNative.Screen Screen;
        public UiValuesNative.VisibleSlot Slot;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct Mapping
    {
        public uint Size, Ok, Reason, Flags;
        public float ScaleX, ScaleY, OffsetX, OffsetY;
        public ulong Session, Frame;
        public UiNative.Screen Screen;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct Hit
    {
        public uint Size, Ok, Reason, PointerScreen;
        public ulong Session, Frame;
        public UiValuesNative.VisibleSlot Slot;
    }

    /// <summary>KilnUiScreenView (v2).</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct ScreenView
    {
        public uint Size, Ok, Reason, Kind;
        public uint HudScreen, Container, LocalPlayer, Flags;
        public ulong Session, Frame;
        public uint SlotCount, PlayerSlots, ContainerSlots, Reserved;
        public UiValuesNative.Rect AllPx, PlayerPx, ContainerPx;
    }

    public const uint ViewBackpack = 1, ViewChest = 2;
    public const uint ViewHolderMatch = 1, ViewScaled = 2;

    /// <summary>v2: the table through screen_view (an older kiln-core has v1's 40 bytes: <see cref="T2"/> is null then).</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct TableV2
    {
        public Table Base;
        public delegate* unmanaged[Cdecl]<ulong, float, float, ScreenView*, int> GetScreenView;
    }

    /// <summary>The v2 table (screen_view), or null on a kiln-core with v1 only.</summary>
    public static TableV2* T2
    {
        get
        {
            var t = T;
            return t != null && t->Header.Size >= (uint)sizeof(TableV2) && ((TableV2*)t)->GetScreenView != null ? (TableV2*)t : null;
        }
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct Table
    {
        public ServiceHeader Header;
        public delegate* unmanaged[Cdecl]<ulong, ClickFilter*, delegate* unmanaged[Cdecl]<Click*, void*, void>, void*,
            UiValuesNative.Registration*, int> SubscribeClicks;
        public delegate* unmanaged[Cdecl]<ulong, float, float, float, float, Hit*, int> SlotAt;
        public delegate* unmanaged[Cdecl]<ulong, float, float, Mapping*, int> GetMapping;
        public delegate* unmanaged[Cdecl]<ulong, int> Unsubscribe;
    }

    private static Table* _table;
    private static readonly object Lock = new();

    public static Table* T
    {
        get
        {
            if (_table != null) return _table;
            lock (Lock)
            {
                if (_table != null) return _table;
                var t = Services.Get<Table>(Service, 1);
                return _table = t != null && t->Header.Size >= (uint)sizeof(Table) ? t : null;
            }
        }
    }

    internal static void ResetForTests()
    {
        lock (Lock) _table = null;
    }
}

internal static unsafe class UiInventoryBridge
{
    private sealed class Sub
    {
        public required Action<SlotClick> Handler;
        public ModContext? Mod;
        public ulong Handle;
    }

    private static readonly ConcurrentDictionary<long, Sub> Subs = new();
    private static long _next;
    // marks: (mod, item) -> mark; rules per mod
    private static readonly ConcurrentDictionary<(ModContext? Mod, uint Item), SlotMark> Marks = new();
    private static readonly OwnedList<Func<VisibleSlot, SlotMark?>> Rules = new();
    private static readonly HashSet<ModContext> Watched = new(ReferenceEqualityComparer.Instance);
    private static int _drawing;
    private static readonly Action<UiDraw> DrawFn = DrawMarks;

    // ---- clicks ----
    internal static IDisposable Subscribe(SlotClickOptions options, Action<SlotClick> handler)
    {
        ArgumentNullException.ThrowIfNull(options);
        ArgumentNullException.ThrowIfNull(handler);
        if (options.Button is not (SlotButton.Left or SlotButton.Right)) throw new ArgumentException("Button must be Left or Right");
        if ((options.Modifiers & ~(SlotModifiers.Ctrl | SlotModifiers.Shift | SlotModifiers.Alt)) != 0)
            throw new ArgumentException("Modifiers: Ctrl, Shift and Alt only");
        UiValueBridge.RequireCapability(Ui.Inventory.ClickCapability);
        var t = UiInventoryNative.T;
        if (t == null) throw new KilnUnavailableException(Ui.Inventory.ClickCapability, Capabilities.Why(UiInventoryNative.Service));
        var mod = UiModCalls.Caller ?? ModContext.OwnerOf(handler);
        UiModCalls.RequireAlive(mod);
        var id = Interlocked.Increment(ref _next);
        var sub = new Sub { Handler = handler, Mod = mod };
        Subs[id] = sub;
        UiInventoryNative.ClickFilter f = new()
        {
            Size = (uint)sizeof(UiInventoryNative.ClickFilter), Button = (uint)options.Button, Mods = (uint)options.Modifiers,
            Flags = options.Take ? UiInventoryNative.ClickTake : 0,
        };
        UiValuesNative.Registration r = default;
        r.Size = (uint)sizeof(UiValuesNative.Registration);
        var rc = t->SubscribeClicks(UiValueBridge.OwnerOf(mod), &f, &OnClick, (void*)id, &r);
        if (rc != UiValuesNative.Ok || r.Ok == 0)
        {
            Subs.TryRemove(id, out _);
            throw rc switch
            {
                UiValuesNative.Ok => new InvalidOperationException($"Ui.Inventory.OnClick refused: {UiValueBridge.ReasonText(r.Reason)}"),
                UiValuesNative.Unavailable => new KilnUnavailableException(Ui.Inventory.ClickCapability,
                    "kiln.ui.inventory could not hook the slot sites or kiln.ui's presses (see kiln.log)"),
                UiValuesNative.Revoked => new InvalidOperationException("the mod is unloading"),
                _ => new ArgumentException($"kiln.ui.inventory refused subscribe_clicks ({rc})"),
            };
        }
        sub.Handle = r.Handle;
        var reg = new Registration(id, mod);
        if (mod is not null)
        {
            mod.Retiring += reg.Dispose;
            if (!mod.Alive) reg.Dispose();
        }
        return reg;
    }

    private sealed class Registration(long id, ModContext? mod) : IDisposable
    {
        private int _done;
        public void Dispose()
        {
            if (Interlocked.Exchange(ref _done, 1) != 0) return;
            if (mod is not null) mod.Retiring -= Dispose;  // a mod that subscribes and disposes often keeps no dead handlers
            if (!Subs.TryRemove(id, out var sub)) return;
            var t = UiInventoryNative.T;
            if (t != null && sub.Handle != 0) t->Unsubscribe(sub.Handle);
        }
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    internal static void OnClick(UiInventoryNative.Click* c, void* user)
    {
        try
        {
            if (!Subs.TryGetValue((long)user, out var sub)) return;
            var click = ToClick(c);
            UiModCalls.Invoke(sub.Mod, sub.Handler, click, "slot click handler");
        }
        catch (Exception e)
        {
            UiEngine.Log(LogLevel.Error, $"ui.inventory: click: {e}");
        }
    }

    internal static SlotClick ToClick(UiInventoryNative.Click* c)
    {
        var screen = c->Screen.Size != 0 ? GameScreen.From(c->Screen) : GameScreen.None;
        return new SlotClick((SlotButton)c->Button, (SlotModifiers)c->Mods, ToVisible(c->Slot), screen, c->Taken != 0)
        {
            Pointer = new Vector2(c->X, c->Y),  // client pixels = the overlay's pixels (kiln_ui_inventory.h "the press in client pixels")
        };
    }

    internal static VisibleSlot ToVisible(in UiValuesNative.VisibleSlot s)
    {
        UiRect? outer = (s.Available & UiValuesNative.OuterRect) != 0 ? new UiRect(s.OuterUi.X, s.OuterUi.Y, s.OuterUi.Width, s.OuterUi.Height) : null;
        UiRect? inner = (s.Available & UiValuesNative.InnerRect) != 0 ? new UiRect(s.InnerUi.X, s.InnerUi.Y, s.InnerUi.Width, s.InnerUi.Height) : null;
        var state = new byte[16];
        for (var k = 0; k < 16; k++) state[k] = s.NativeState[k];
        return new VisibleSlot(s.Occurrence, new EntityId(s.InventoryPage), (byte)s.SlotIndex, new ItemId(s.Item), s.Count,
            new EntityId(s.ClientEntity), new EntityId(s.ServerEntity), s.ViewFlags, outer, inner, state, s.Available, null, null)
        {
            Holder = new EntityId(s.Holder), HolderSerial = s.HolderSerial, PageOrdinal = s.PageOrdinal, HolderFlags = s.HolderFlags,
        };
    }

    // ---- hovered ----
    internal static VisibleSlot? Hovered()
    {
        var t = UiInventoryNative.T;
        if (t == null) return null;
        UiInventoryNative.Hit h = default;
        h.Size = (uint)sizeof(UiInventoryNative.Hit);
        return t->SlotAt(UiValueBridge.OwnerOf(UiModCalls.Caller), -1, -1, 0, 0, &h) == UiValuesNative.Ok && h.Ok != 0
            ? ToVisible(h.Slot)
            : null;
    }

    // ---- marks ----
    internal static void Mark(EntityId item, SlotMark mark)
    {
        if (item.Value == 0) throw new ArgumentException("an item entity is needed (not 0)", nameof(item));
        if (!float.IsFinite(mark.Thickness) || mark.Thickness < 0) throw new ArgumentException("Thickness must be finite and >= 0");
        RequireMarks();
        var mod = UiModCalls.Caller;
        UiModCalls.RequireAlive(mod);
        Watch(mod);
        Marks[(mod, item.Value)] = mark;
        EnsureDrawing();
    }

    // marks are placed with kiln.ui.inventory's mapping: without that table (an older kiln-core) they could never show
    private static void RequireMarks()
    {
        UiValueBridge.RequireCapability(Ui.Inventory.MarkCapability);
        if (UiInventoryNative.T == null)
            throw new KilnUnavailableException(Ui.Inventory.MarkCapability, "kiln.ui.inventory is not there (an older kiln-core)");
    }

    internal static void Unmark(EntityId item)
    {
        Marks.TryRemove((UiModCalls.Caller, item.Value), out _);
        StopDrawingIfIdle();
    }

    internal static void ClearMarks() => ClearMarksOf(UiModCalls.Caller);

    private static void ClearMarksOf(ModContext? mod)
    {
        foreach (var k in Marks.Keys)
            if (ReferenceEquals(k.Mod, mod)) Marks.TryRemove(k, out _);
        StopDrawingIfIdle();
    }

    internal static IDisposable AddRule(Func<VisibleSlot, SlotMark?> rule)
    {
        ArgumentNullException.ThrowIfNull(rule);
        RequireMarks();
        var mod = UiModCalls.Caller;
        Watch(mod);
        Rules.Add(rule);
        EnsureDrawing();
        return new RuleHandle(rule);
    }

    private sealed class RuleHandle(Func<VisibleSlot, SlotMark?> rule) : IDisposable
    {
        public void Dispose()
        {
            Rules.Remove(rule);
            StopDrawingIfIdle();
        }
    }

    private static void Watch(ModContext? mod)
    {
        if (mod is null) return;
        lock (Watched)
            if (!Watched.Add(mod)) return;
        mod.Retiring += () =>
        {
            ClearMarksOf(mod);
            Rules.RemoveOwnedBy(mod);
            lock (Watched) Watched.Remove(mod);
            StopDrawingIfIdle();
        };
    }

    private static bool Idle => Marks.IsEmpty && Rules.Count == 0;

    private static void EnsureDrawing()
    {
        if (Interlocked.CompareExchange(ref _drawing, 1, 0) != 0) return;
        UiEngine.EnsureStarted();
        var prev = ModContext.CurrentMod;
        ModContext.CurrentMod = null;  // Kiln's own handler: a mod's unload must not take it
        try { UiEngine.FrameHandlers.Add(DrawFn); }
        finally { ModContext.CurrentMod = prev; }
        UiEngine.Recount();
    }

    private static void StopDrawingIfIdle()
    {
        if (!Idle || Interlocked.CompareExchange(ref _drawing, 0, 1) != 1) return;
        UiEngine.FrameHandlers.Remove(DrawFn);
        UiEngine.Recount();
        if (!Idle) EnsureDrawing();  // a mark came in between
    }

    // The marks of the slots the game drew in its latest frame, over them (render thread).
    internal static void DrawMarks(UiDraw ui)
    {
        if (Idle) return;
        var t = UiInventoryNative.T;
        if (t == null) return;
        UiInventoryNative.Mapping m = default;
        m.Size = (uint)sizeof(UiInventoryNative.Mapping);
        if (t->GetMapping(UiValueBridge.OwnerOf(null), ui.Width, ui.Height, &m) != UiValuesNative.Ok || m.Ok == 0) return;
        if (!MarkScreen(m.Screen)) return;
        VisibleSlotsResult slots;
        try { slots = UiSlotsBridge.Read(); }
        catch (Exception) { return; }  // the row went off: nothing to mark
        if (!slots.Ok || slots.Snapshot is not { } snap || snap.Session != m.Session) return;
        var rules = Rules.Snapshot();
        var byItem = ByItem();
        foreach (var s in snap.Slots)
        {
            if (s.OuterUi is not { } r) continue;
            var mark = Find(s, byItem, rules);
            if (mark is not { } k) continue;
            var x0 = m.OffsetX + r.X * m.ScaleX;
            var y0 = m.OffsetY + r.Y * m.ScaleY;
            var x1 = x0 + r.Width * m.ScaleX;
            var y1 = y0 + r.Height * m.ScaleY;
            DrawMark(ui, k, x0, y0, x1, y1);
        }
    }

    // the marks by item, for one frame (the mod that marked an item first... any one: marks of two mods on one item are
    // not layered in v1)
    internal static Dictionary<uint, SlotMark> ByItem()
    {
        var d = new Dictionary<uint, SlotMark>();
        foreach (var (key, mark) in Marks) d.TryAdd(key.Item, mark);
        return d;
    }

    /// <summary>The mark a slot shows now (an item mark first, then the rules): what DrawMarks would draw on it.</summary>
    internal static SlotMark? MarkOf(VisibleSlot s) => Find(s, ByItem(), Rules.Snapshot());

    internal static SlotMark? Find(VisibleSlot s, Dictionary<uint, SlotMark> byItem, (Func<VisibleSlot, SlotMark?> Fn, ModContext? Owner)[] rules)
    {
        if (s.ClientEntity.Value != 0 && byItem.TryGetValue(s.ClientEntity.Value, out var marked)) return marked;
        foreach (var (fn, owner) in rules)
        {
            SlotMark? got = null;
            if (UiModCalls.Invoke(owner, v => got = fn(v), s, "Ui.Inventory.MarkWhen rule") && got is { } g) return g;
        }
        return null;
    }

    internal static void DrawMark(UiDraw ui, SlotMark k, float x0, float y0, float x1, float y1)
    {
        var th = Math.Max(1f, k.Thickness * ui.Scale);
        switch (k.Style)
        {
            case SlotMarkStyle.Fill:
                ui.DrawRect(UiLayer.Foreground, x0, y0, x1, y1, k.Color);
                break;
            case SlotMarkStyle.Corner:
                var side = Math.Max(4f, (x1 - x0) * 0.22f);
                ui.DrawRect(UiLayer.Foreground, x1 - side - th, y0 + th, x1 - th, y0 + th + side, k.Color);
                break;
            default:
                ui.DrawRect(UiLayer.Foreground, x0 + th / 2, y0 + th / 2, x1 - th / 2, y1 - th / 2, k.Color, 0, th);
                break;
        }
    }

    // marks show where clicks do, and on the play view's hotbar
    private static bool MarkScreen(in UiNative.Screen s) =>
        s.Size != 0 && s.UiState == 7 && s.HudScreen is 1 or 4 or 8 or 9 or 22 or 25 or 27 or 28 or 41;

    internal static void ResetForTests()
    {
        Subs.Clear();
        Marks.Clear();
        foreach (var (fn, _) in Rules.Snapshot()) Rules.Remove(fn);
        if (Interlocked.Exchange(ref _drawing, 0) == 1) UiEngine.FrameHandlers.Remove(DrawFn);
        UiInventoryNative.ResetForTests();
    }
}
