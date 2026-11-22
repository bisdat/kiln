// SPDX-License-Identifier: MIT
// Ui.Values.Slots / SubscribeSlots: the inventory slots the game drew in a completed UI frame (chapter 05a §5.3;
// native copy_slots / subscribe_slots, capability ui.slots.inspect). For slot marks and overlays: rectangles in the
// game's UI units, and pixels once check 05a-c5 has verified the mapping. A client rectangle is never inventory
// authority: a server must authorize anything a slot key does.
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace Kiln;

/// <summary>A rectangle (game UI units or pixels, as the property says).</summary>
public readonly record struct UiRect(float X, float Y, float Width, float Height);

/// <summary>One slot draw of a frame. <see cref="Occurrence"/> is the draw order, not an identity; a page/slot can be drawn
/// twice in one frame. Rectangles are null when the game gave none usable (zero, negative, not finite). NativeState is
/// the slot record's 16 state bytes: byte 0 is only a candidate hover flag until check 05a-c5 passed. Pixel rectangles
/// are null until the pixel mapping is verified (<see cref="Availability"/> bit 16; not in this version).
/// <see cref="Holder"/> is the client entity whose inventory holds the page, with Kiln's lifetime serial of it in this
/// process (<see cref="HolderSerial"/>: a destroyed and recreated holder gets a new one, never reused) and the page's
/// position among the holder's pages; <see cref="HolderFlags"/>: 1 known, 2 the local player, 4 ordinal set, 8 proven
/// to be the server's entity (since 0.2.0-dev.30.5: the local player, and the container of an open chest, chest-style or
/// factory screen). <see cref="ServerEntity"/> is the item's entity in the server's world for a slot of a proven holder
/// (since 0.2.0-dev.30.5), so it equals the server's <c>InventorySlot.ItemEntity</c> for the player's and an open chest's
/// slots; <see cref="EntityId.None"/> for a plain stack (no item entity) and any other holder. <see cref="Rarity"/> and
/// <see cref="Level"/> are the item's as the game drew the slot (since 0.2.0-dev.30.5).</summary>
public sealed record VisibleSlot(uint Occurrence, EntityId InventoryPage, byte SlotIndex, ItemId Item, uint Count,
    EntityId ClientEntity, EntityId ServerEntity, uint ViewFlags, UiRect? OuterUi, UiRect? InnerUi,
    IReadOnlyList<byte> NativeState, uint Availability, UiRect? OuterPixels, UiRect? InnerPixels)
{
    public EntityId Holder { get; init; }
    public ulong HolderSerial { get; init; }
    public uint PageOrdinal { get; init; }
    public uint HolderFlags { get; init; }

    /// <summary>The item's rarity as the game drew the slot (the value its tooltip shows), captured on the game's main/UI
    /// thread and copied to render-thread mark rules: no <c>Items.Read</c> needed in a mark rule. Null for an empty slot, a
    /// rarity outside Common..Legendary, or a Kiln without the value. Since 0.2.0-dev.30.5 (mod API 0.2.14).</summary>
    public ItemRarity? Rarity { get; init; }

    /// <summary>The item's level as the game drew the slot (0 for an empty slot, an item without levels, or a Kiln without
    /// the value). Since 0.2.0-dev.30.5 (mod API 0.2.14).</summary>
    public int Level { get; init; }

    internal static ItemRarity? RarityOf(uint available, uint rarity) =>
        (available & UiValuesNative.ItemValues) != 0 && rarity <= (uint)ItemRarity.Legendary ? (ItemRarity)rarity : null;

    internal static int LevelOf(uint available, uint level) =>
        (available & UiValuesNative.ItemValues) != 0 ? (int)Math.Min(level, int.MaxValue) : 0;
}

/// <summary>One completed frame's slots. <see cref="CaptureMs"/> is <c>Environment.TickCount64</c>'s clock
/// (GetTickCount64). <see cref="TransformFlags"/>: 8 the viewport numbers are sane, 16 the pixel mapping is verified.</summary>
public sealed record VisibleSlotsSnapshot(ulong Session, ulong Frame, ulong CaptureMs, EntityId LocalPlayer, GameScreen Screen,
    UiRect ViewportPixels, float PixelsPerUnit, uint TransformFlags, IReadOnlyList<VisibleSlot> Slots);

/// <summary>A slots read: Ok false with a reason when there is no frame yet, the frame is older than 1000 ms, or it had more
/// than 4096 slot draws (no partial list).</summary>
public sealed record VisibleSlotsResult(bool Ok, string? Reason, VisibleSlotsSnapshot? Snapshot) : IKilnResult;

public static partial class Ui
{
    public static partial class Values
    {
        /// <summary>The capability row of <see cref="Slots"/> and <see cref="SubscribeSlots"/>.</summary>
        public const string SlotsCapability = "ui.slots.inspect";

        /// <summary>The slots of the latest completed UI frame (a copy). Refused when it is older than 1000 ms (the game
        /// UI stopped drawing slots).</summary>
        [ClientOnly, AnyThread, RequiresCapability(SlotsCapability)]
        public static VisibleSlotsResult Slots() => UiSlotsBridge.Read();

        /// <summary>Calls <paramref name="onFrame"/> once per completed UI frame, on the game client's main thread right
        /// after the frame. Keep it short; the snapshot is yours to keep.</summary>
        [ClientOnly, AnyThread, RequiresCapability(SlotsCapability)]
        public static UiValueRegistration SubscribeSlots([ClientMainThread] Action<VisibleSlotsResult> onFrame) =>
            UiSlotsBridge.Subscribe(onFrame);
    }
}

internal static unsafe class UiSlotsBridge
{
    internal const ulong MaxAgeMs = 1000;
    private static readonly System.Collections.Concurrent.ConcurrentDictionary<long, (Action<VisibleSlotsResult> Fn, ModContext? Mod)> Subs = new();
    private static long _next;

    internal static Func<long> Now = () => Environment.TickCount64;

    internal static VisibleSlotsResult Read()
    {
        UiValueBridge.RequireCapability(Ui.Values.SlotsCapability);
        var t = UiValuesNative.T;
        if (t == null) throw new KilnUnavailableException(Ui.Values.SlotsCapability, Capabilities.Why(UiValuesNative.Service));
        var owner = UiValueBridge.OwnerOf(UiModCalls.Caller);
        UiValuesNative.SlotsFrame f = default;
        ulong session = 0, frame = 0;
        var capacity = 64u;
        for (var attempt = 0; attempt < 4; attempt++)
        {
            var buffer = new UiValuesNative.VisibleSlot[capacity];
            buffer[0].Size = (uint)sizeof(UiValuesNative.VisibleSlot);
            f = default;
            f.Size = (uint)sizeof(UiValuesNative.SlotsFrame);
            uint required = 0;
            int rc;
            fixed (UiValuesNative.VisibleSlot* p = buffer) rc = t->CopySlots(owner, session, frame, &f, p, capacity, &required);
            switch (rc)
            {
                case UiValuesNative.Ok when f.Ok == 0 && f.Reason == UiValuesNative.ReasonStaleFrame && session != 0:
                    session = frame = 0;  // aged out between the two calls: start again from the latest
                    continue;
                case UiValuesNative.Ok:
                    return Result(&f, buffer, f.Ok != 0 ? f.Count : 0);
                case UiValuesNative.BufferTooSmall:
                    (session, frame, capacity) = (f.Session, f.Frame, Math.Max(required, capacity * 2));
                    continue;
                case UiValuesNative.Unavailable:
                    throw new KilnUnavailableException(Ui.Values.SlotsCapability, Capabilities.Why(Ui.Values.SlotsCapability));
                case UiValuesNative.Revoked:
                    throw new InvalidOperationException("the mod is unloading");
                default:
                    throw new InvalidOperationException($"kiln.ui.values: copy_slots failed ({rc})");
            }
        }
        return new VisibleSlotsResult(false, "the slots changed faster than they could be copied", null);
    }

    internal static VisibleSlotsResult Result(UiValuesNative.SlotsFrame* f, ReadOnlySpan<UiValuesNative.VisibleSlot> slots, uint count)
    {
        if (f->Ok == 0)
            return new VisibleSlotsResult(false, f->Reason switch
            {
                UiValuesNative.ReasonNoFrame => "no slot frame yet",
                UiValuesNative.ReasonFrameBudget => "more than 4096 slot draws in the frame",
                UiValuesNative.ReasonStaleFrame => "stale frame",
                _ => $"refused ({f->Reason})",
            }, null);
        var age = Now() - (long)f->CaptureMs;
        if (age > (long)MaxAgeMs) return new VisibleSlotsResult(false, $"the last slot frame is {age} ms old", null);
        var pixels = (f->TransformFlags & (UiValuesNative.Viewport | UiValuesNative.PixelMappingVerified)) ==
                     (UiValuesNative.Viewport | UiValuesNative.PixelMappingVerified);
        var vp = new UiRect(f->ViewportPx.X, f->ViewportPx.Y, f->ViewportPx.Width, f->ViewportPx.Height);
        var ppu = f->PixelsPerUnit;
        UiRect? Px(UiRect? r) => pixels && r is { } u ? new UiRect(vp.X + u.X * ppu, vp.Y + u.Y * ppu, u.Width * ppu, u.Height * ppu) : null;
        var list = new VisibleSlot[count];
        for (var i = 0; i < count; i++)
        {
            var s = slots[i];
            UiRect? outer = (s.Available & UiValuesNative.OuterRect) != 0 ? new UiRect(s.OuterUi.X, s.OuterUi.Y, s.OuterUi.Width, s.OuterUi.Height) : null;
            UiRect? inner = (s.Available & UiValuesNative.InnerRect) != 0 ? new UiRect(s.InnerUi.X, s.InnerUi.Y, s.InnerUi.Width, s.InnerUi.Height) : null;
            var state = new byte[16];
            for (var k = 0; k < 16; k++) state[k] = s.NativeState[k];
            list[i] = new VisibleSlot(s.Occurrence, new EntityId(s.InventoryPage), (byte)s.SlotIndex, new ItemId(s.Item), s.Count,
                new EntityId(s.ClientEntity), new EntityId(s.ServerEntity), s.ViewFlags, outer, inner, state, s.Available,
                Px(outer), Px(inner))
            {
                Holder = new EntityId(s.Holder), HolderSerial = s.HolderSerial, PageOrdinal = s.PageOrdinal, HolderFlags = s.HolderFlags,
                Rarity = VisibleSlot.RarityOf(s.Available, s.Rarity), Level = VisibleSlot.LevelOf(s.Available, s.Level),
            };
        }
        var screen = f->Screen.Size != 0 ? GameScreen.From(f->Screen) : GameScreen.None;
        return new VisibleSlotsResult(true, null, new VisibleSlotsSnapshot(f->Session, f->Frame, f->CaptureMs,
            new EntityId(f->LocalPlayer), screen, vp, ppu, f->TransformFlags, list));
    }

    internal static UiValueRegistration Subscribe(Action<VisibleSlotsResult> onFrame)
    {
        ArgumentNullException.ThrowIfNull(onFrame);
        UiValueBridge.RequireCapability(Ui.Values.SlotsCapability);
        var t = UiValuesNative.T;
        if (t == null) throw new KilnUnavailableException(Ui.Values.SlotsCapability, Capabilities.Why(UiValuesNative.Service));
        var mod = UiModCalls.Caller ?? ModContext.OwnerOf(onFrame);
        UiModCalls.RequireAlive(mod);
        var id = Interlocked.Increment(ref _next);
        Subs[id] = (onFrame, mod);
        UiValuesNative.Registration r = default;
        r.Size = (uint)sizeof(UiValuesNative.Registration);
        var expanded = UiSlotsNative.T;
        var rc = expanded != null
            ? expanded->Subscribe(UiValueBridge.OwnerOf(mod), (uint)sizeof(UiValuesNative.VisibleSlot), &OnFrame, (void*)id, &r)
            : t->SubscribeSlots(UiValueBridge.OwnerOf(mod), &OnLegacyFrame, (void*)id, &r);
        if (rc != UiValuesNative.Ok || r.Ok == 0) Subs.TryRemove(id, out _);
        var handle = r.Handle;
        var accepted = r.Ok != 0;
        var reason = r.Reason;
        var result = rc switch
        {
            UiValuesNative.Ok when accepted => new UiValueRegistration(true, null, handle, Remove(id, handle)),
            UiValuesNative.Ok => new UiValueRegistration(false, UiValueBridge.ReasonText(reason), 0, null),
            UiValuesNative.Unavailable => throw new KilnUnavailableException(Ui.Values.SlotsCapability,
                Capabilities.Is(Ui.Values.SlotsCapability) ? "kiln.ui.values could not hook the slot sites (see kiln.log)" : Capabilities.Why(Ui.Values.SlotsCapability)),
            UiValuesNative.Revoked => throw new InvalidOperationException("the mod is unloading"),
            _ => throw new ArgumentException($"kiln.ui.values refused subscribe_slots ({rc})"),
        };
        if (result.Ok && mod is not null)
        {
            mod.Retiring += result.Dispose;
            if (!mod.Alive) result.Dispose();  // retirement may have happened before the handler was attached
        }
        return result;
    }

    private static Action<UiValueRegistration> Remove(long id, ulong handle) => _ =>
    {
        Subs.TryRemove(id, out var _);
        var t = UiValuesNative.T;
        if (t != null) t->Unsubscribe(handle);
    };

    internal static void ResetForTests() => Subs.Clear();

    // Frozen kiln.ui.values callback: old records are 112 bytes even when today's managed mirror is larger.
    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    internal static void OnLegacyFrame(UiValuesNative.SlotsFrame* f, UiValuesNative.VisibleSlot* slots, uint count, void* user)
    {
        try
        {
            if (!Subs.TryGetValue((long)user, out var sub)) return;
            var expanded = new UiValuesNative.VisibleSlot[count];
            for (var i = 0; i < expanded.Length; ++i)
                fixed (UiValuesNative.VisibleSlot* target = &expanded[i])
                    Buffer.MemoryCopy((byte*)slots + i * 112, target, sizeof(UiValuesNative.VisibleSlot), 112);
            var result = Result(f, expanded, f->Ok != 0 ? count : 0);
            UiModCalls.Invoke(sub.Mod, sub.Fn, result, "legacy slots subscriber");
        }
        catch (Exception e) { UiEngine.Log(LogLevel.Error, $"ui.values: legacy slots subscriber: {e}"); }
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    internal static void OnFrame(UiValuesNative.SlotsFrame* f, UiValuesNative.VisibleSlot* slots, uint count, void* user)
    {
        try
        {
            if (!Subs.TryGetValue((long)user, out var sub)) return;
            var result = Result(f, new ReadOnlySpan<UiValuesNative.VisibleSlot>(slots, (int)count), f->Ok != 0 ? count : 0);
            UiModCalls.Invoke(sub.Mod, sub.Fn, result, "slots subscriber");
        }
        catch (Exception e)
        {
            UiEngine.Log(LogLevel.Error, $"ui.values: slots subscriber: {e}");
        }
    }
}

internal static unsafe class UiSlotsNative
{
    internal const string Service = "kiln.ui.slots";
    [StructLayout(LayoutKind.Sequential)]
    internal struct Table
    {
        public ServiceHeader Header;
        public delegate* unmanaged[Cdecl]<ulong, uint,
            delegate* unmanaged[Cdecl]<UiValuesNative.SlotsFrame*, UiValuesNative.VisibleSlot*, uint, void*, void>,
            void*, UiValuesNative.Registration*, int> Subscribe;
    }
    internal static Table* T
    {
        get
        {
            var t = Services.Get<Table>(Service, 1);
            return t != null && t->Header.Size >= sizeof(Table) && t->Subscribe != null ? t : null;
        }
    }
}
