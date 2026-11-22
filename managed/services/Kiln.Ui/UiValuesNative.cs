// SPDX-License-Identifier: MIT
// The native table kiln.ui.values (native/core/abi/services/kiln_ui_values.h): records and slots, as laid out in C.
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace Kiln;

internal static unsafe class UiValuesNative
{
    public const string Service = "kiln.ui.values";
    public const int Ok = 0, InvalidArgument = 1, NotFound = 2, Unavailable = 3, WrongThread = 4, BufferTooSmall = 5,
        Revoked = 6, Internal = 7;

    // KilnUiEditFlags
    public const uint EditFraction = 1, EditCurrent = 2, EditMaximum = 4, EditLabel = 8, EditGameLabel = 16;

    // KilnUiValueReason
    public const uint ReasonNone = 0, ReasonRejected = 1, ReasonTextBudget = 2, ReasonInvalidEdit = 3,
        ReasonCallbackFault = 4, ReasonNoFrame = 5, ReasonStaleFrame = 6, ReasonBufferSmall = 7,
        ReasonRegistrationBudget = 8, ReasonFrameBudget = 9, ReasonNoTooltip = 10;

    [StructLayout(LayoutKind.Sequential)]
    public struct ItemView
    {
        public uint Size, Flags, LocalPlayer, ClientEntity, ServerEntity, Item;
        public uint InventoryPage, SlotIndex, Count, Durability, DurabilityMax;
        public uint Fuel, FuelMax, UnitsPerCharge, Level, Rarity;
        public UiNative.Screen Screen;
        public ulong Session, Frame;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct BarValue
    {
        public uint Size, Kind;
        public float Fraction;
        public uint Current, Maximum, LabelIsOverride;
        public ulong LabelId;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct BarEdit
    {
        public uint Size, Flags;
        public float Fraction;
        public uint Current, Maximum, Reason;
        public ulong LabelId;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct TextOps
    {
        public uint Size, Version;
        public delegate* unmanaged[Cdecl]<ProviderContext*, NativeStr, ulong*, int> Intern;
        public delegate* unmanaged[Cdecl]<ProviderContext*, ulong, NativeStr*, int> Resolve;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct ProviderContext
    {
        public uint Size, Kind;
        public ulong Session, Frame, ClientStep, Owner;
        public TextOps* Text;
        public void* Reserved;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct Registration
    {
        public uint Size, Ok, Reason, Reserved;
        public ulong Handle;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct Identity
    {
        public uint Size, Kind, LocalPlayer, UiState, HudScreen, Crafting, Station;
        public uint InventoryPage, SlotIndex, ClientEntity, Item, SyntheticGroup;
        public ulong Session;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct ValueChange
    {
        public uint Size, ChangeKind, HasBefore, HasAfter, Reason, Reserved;
        public ulong Frame, OldSession, NewSession;
        public Identity Identity;
        public BarValue Before, After;
        public NativeStr BeforeLabel, AfterLabel;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct Rect
    {
        public float X, Y, Width, Height;
    }

    // KilnUiSlotAvailability
    public const uint OuterRect = 1, InnerRect = 2, NativeState = 4, Viewport = 8, PixelMappingVerified = 16, ItemValues = 32;
    // KilnUiHolderFlags / KilnUiViewFlags (dev.30.5: a proven holder's item entity is the server's)
    public const uint HolderServerProven = 8, ViewServerEntity = 2;

    [StructLayout(LayoutKind.Sequential)]
    public struct VisibleSlot
    {
        public uint Size, Available, Occurrence, InventoryPage, SlotIndex;
        public uint Item, Count, ClientEntity, ServerEntity, ViewFlags;
        public Rect OuterUi, InnerUi;
        public fixed byte NativeState[16];
        public uint Holder, HolderFlags;
        public ulong HolderSerial;
        public uint PageOrdinal, Reserved2;
        public uint Level, Rarity;  // dev.30.5: valid with ItemValues
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct SlotsFrame
    {
        public uint Size, Ok, Reason, Count, LocalPlayer, Reserved;
        public UiNative.Screen Screen;
        public ulong Session, Frame, CaptureMs;
        public float CanvasWidth, CanvasHeight;
        public Rect ViewportPx;
        public float PixelsPerUnit;
        public uint TransformFlags;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct GemForgeCandidate
    {
        public uint Size, Index, HasWeaponCarrier, Reserved;
        public ItemView SourceRow, DisplayedGem, WeaponCarrier;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct GemForgeFrame
    {
        public uint Size, Ok, Reason, Count, LocalPlayer, Mode;
        public uint Role, HasSelectedRow, HasSelectedWeapon, SelectedPage, SelectedIndex, Reserved;
        public ItemView SelectedWeapon;
        public ulong Session, Frame, CaptureMs, Generation;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct Line
    {
        public uint Size, Flags;
        public ulong Text;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct LineWriter
    {
        public uint Size, Capacity, Count, Status;
        public fixed byte Lines[16 * 16];
        public void* Reserved;
        public delegate* unmanaged[Cdecl]<LineWriter*, ulong, int> Append;
        // ---- appended in 0.2.0-dev.29.19: read Size (an older native writer is 8 bytes shorter) ----
        public delegate* unmanaged[Cdecl]<LineWriter*, ulong, uint, int> AppendLine;
    }

    // KilnUiLineFlags (bits 0..3: KilnUiLineColor = TooltipColor) and KilnUiNativeLinesFlags
    public const uint LineSmall = 0x10, LineAfterStats = 0x100, LinesInGameTooltip = 1;

    [StructLayout(LayoutKind.Sequential)]
    public struct TooltipOpen
    {
        public uint Size, Ok, Reason, Reserved;
        public ulong Session, SourceFrame;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct TooltipDetails
    {
        public uint Size, Ok, Reason, Count;
        public ulong Session, SourceFrame, CaptureMs;
        public uint Item, Open, InventoryPage, SlotIndex, ClientEntity, ViewFlags;
        public TooltipLineViews Lines;
    }

    [InlineArray(16)]
    public struct TooltipLineViews
    {
        private NativeStr _first;
    }

    public const uint ChangeAdded = 0, ChangeUpdated = 1, ChangeRemoved = 2, ChangeEvicted = 3, ChangeReset = 4;

    [StructLayout(LayoutKind.Sequential)]
    public struct Table
    {
        public ServiceHeader Header;
        public delegate* unmanaged[Cdecl]<ulong, uint, uint,
            delegate* unmanaged[Cdecl]<ProviderContext*, ItemView*, BarValue*, BarEdit*, void*, int>, void*, Registration*, int> Subscribe;
        public delegate* unmanaged[Cdecl]<ulong, uint,
            delegate* unmanaged[Cdecl]<ProviderContext*, ItemView*, LineWriter*, void*, int>, void*, Registration*, int> TooltipLines;
        public delegate* unmanaged[Cdecl]<ulong, int> Unsubscribe;
        public delegate* unmanaged[Cdecl]<ulong, ulong, ulong, SlotsFrame*, VisibleSlot*, uint, uint*, int> CopySlots;
        public delegate* unmanaged[Cdecl]<ulong, delegate* unmanaged[Cdecl]<SlotsFrame*, VisibleSlot*, uint, void*, void>, void*, Registration*, int> SubscribeSlots;
        public delegate* unmanaged[Cdecl]<ulong, delegate* unmanaged[Cdecl]<ValueChange*, void*, void>, void*, Registration*, int> SubscribeChanges;
        public delegate* unmanaged[Cdecl]<ulong, TooltipOpen*, int> OpenTooltipDetails;
        public delegate* unmanaged[Cdecl]<ulong, int> CloseTooltipDetails;
        // ---- appended in the first release (the table's header.size covers them) ----
        public delegate* unmanaged[Cdecl]<ulong, uint, TooltipDetails*, int> GetTooltipDetails;
        public delegate* unmanaged[Cdecl]<ulong, TooltipOpen*, int> RefreshTooltipDetails;
    }

    // Keep Table's 88-byte prefix: existing value APIs also work with an older native service.
    [StructLayout(LayoutKind.Sequential)]
    public struct GemForgeTable
    {
        public Table Base;
        public delegate* unmanaged[Cdecl]<ulong, ulong, ulong, GemForgeFrame*, GemForgeCandidate*, uint, uint*, int> CopyGemForge;
        public delegate* unmanaged[Cdecl]<ulong, delegate* unmanaged[Cdecl]<GemForgeFrame*, GemForgeCandidate*, uint, void*, void>, void*, Registration*, int> SubscribeGemForge;
    }

    // ---- appended (gem wave): gems.sockets' gemforge_sockets(owner, category_mask) ----
    [StructLayout(LayoutKind.Sequential)]
    public struct SocketsTable
    {
        public GemForgeTable Base;
        public delegate* unmanaged[Cdecl]<ulong, uint, int> GemForgeSockets;
    }

    /// <summary>The table with gemforge_sockets, or null (a server, an older native service).</summary>
    public static SocketsTable* SocketsT
    {
        get
        {
            var t = T;
            if (t == null || t->Header.Size < sizeof(SocketsTable)) return null;
            var s = (SocketsTable*)t;
            return s->GemForgeSockets != null ? s : null;
        }
    }

    // ---- appended in 0.2.0-dev.29.15: ui.gemforge.sources' gemforge_sources(owner, rows, count, payers, payer_count) ----
    [StructLayout(LayoutKind.Sequential)]
    public struct SourcesTable
    {
        public SocketsTable Base;
        public delegate* unmanaged[Cdecl]<ulong, GemSourceRow*, uint, GemSourcePayer*, uint, int> GemForgeSources;
        public delegate* unmanaged[Cdecl]<GemSourcesState*, int> GemForgeSourcesState;
    }

    /// <summary>KilnUiGemForgeSource (48 bytes).</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct GemSourceRow
    {
        public uint Size, Page;
        public byte Slot, Rarity, Group, Reserved0;
        public uint Item, Pide, Level, Reserved1, Reserved2;
        public ulong Token, ExpiresMs;
    }

    /// <summary>KilnUiGemForgePayer (16 bytes).</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct GemSourcePayer
    {
        public uint Size;
        public byte Group, PlayerFirst, Reserved0, Reserved1;
        public uint Item, Count;
    }

    /// <summary>KilnUiGemForgeSourcesState (200 bytes).</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct GemSourcesState
    {
        public uint Size;
        public byte Installed, Off, Reserved0, Reserved1;
        public uint Rows, WantedCount;
        public fixed uint Wanted[8];
        public ulong Injected, Resolved, Priced;
        public fixed byte Why[128];
    }

    /// <summary>The table with gemforge_sources, or null (a server, an older native service).</summary>
    public static SourcesTable* SourcesT
    {
        get
        {
            var t = T;
            if (t == null || t->Header.Size < sizeof(SourcesTable)) return null;
            var s = (SourcesTable*)t;
            return s->GemForgeSources != null && s->GemForgeSourcesState != null ? s : null;
        }
    }

    // ---- appended in 0.2.0-dev.29.19: ui.tooltip.lines / ui.tooltip.values ----
    [StructLayout(LayoutKind.Sequential)]
    public struct TooltipTable
    {
        public SourcesTable Base;
        public delegate* unmanaged[Cdecl]<ulong, uint, uint,
            delegate* unmanaged[Cdecl]<ProviderContext*, ItemView*, LineWriter*, void*, int>, void*, Registration*, int> TooltipLinesNative;
        public delegate* unmanaged[Cdecl]<ulong, uint,
            delegate* unmanaged[Cdecl]<ProviderContext*, ItemView*, float*, void*, int>, void*, Registration*, int> TooltipValues;
        public delegate* unmanaged[Cdecl]<TooltipNativeState*, int> TooltipNativeState;
    }

    /// <summary>KilnUiTooltipNativeState (152 bytes).</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct TooltipNativeState
    {
        public uint Size;
        public byte LinesOn, ValuesOn, AfterStats, Probe;
        public ulong DrawnLines, ScaledValues;
        public fixed byte Why[128];
    }

    /// <summary>The table with the native tooltip slots, or null (a server, an older native service).</summary>
    public static TooltipTable* TooltipT
    {
        get
        {
            var t = T;
            if (t == null || t->Header.Size < sizeof(TooltipTable)) return null;
            var s = (TooltipTable*)t;
            return s->TooltipLinesNative != null && s->TooltipValues != null && s->TooltipNativeState != null ? s : null;
        }
    }

    public static GemForgeTable* GemForgeT
    {
        get
        {
            var t = T;
            if (t == null || t->Header.Size < sizeof(GemForgeTable)) return null;
            var g = (GemForgeTable*)t;
            return g->CopyGemForge != null && g->SubscribeGemForge != null ? g : null;
        }
    }

    private static Table* _table;
    private static readonly object Lock = new();

    /// <summary>The table, or null (a server, a runtime without the service). A miss is asked again at the next use, so a
    /// table published later (or a new registry binding) is found.</summary>
    public static Table* T
    {
        get
        {
            if (_table != null) return _table;
            lock (Lock)
                return _table != null ? _table : (_table = Services.Get<Table>(Service, 1));
        }
    }

    internal static void ResetForTests()
    {
        lock (Lock) _table = null;
    }

    /// <summary>Text through a provider's context: the native store copies it and returns an id (0 = empty).</summary>
    public static int Intern(ProviderContext* c, string text, out ulong id)
    {
        id = 0;
        if (c == null || c->Text == null) return Internal;
        // Reject before scanning or allocating an unbounded mod string on the game UI thread.
        // UTF-8 needs at least one byte per UTF-16 code unit, including replacement fallback.
        if (text.Length > 4096 || System.Text.Encoding.UTF8.GetByteCount(text) > 4096) return BufferTooSmall;
        var bytes = System.Text.Encoding.UTF8.GetBytes(text);
        ulong got = 0;
        fixed (byte* p = bytes)
        {
            var rc = c->Text->Intern(c, new NativeStr { Ptr = p, Len = (nuint)bytes.Length }, &got);
            id = got;
            return rc;
        }
    }

    /// <summary>The bytes of a text id through a provider's context; null when it does not resolve.</summary>
    public static string? Resolve(ProviderContext* c, ulong id)
    {
        if (id == 0) return "";
        if (c == null || c->Text == null) return null;
        NativeStr s = default;
        return c->Text->Resolve(c, id, &s) == Ok ? s.ToString() : null;
    }
}
