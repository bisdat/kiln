// SPDX-License-Identifier: MIT
// The fake kiln.ui.values (native/core/abi/services/kiln_ui_values.h, v1) under the real Ui.Values of Kiln.Ui: providers
// register as with the game, and a test draws a value or an item detail view (KilnHarness.UiValues) to run them in order
// over a fake item, as the game's UI would. The game's drawing, its hooks, the slot frames and change publication are
// not modelled.
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;

namespace Kiln.TestKit;

/// <summary>Mirror of <c>kiln_ui_values.h</c> (the records the fake uses).</summary>
internal static unsafe class UiValuesAbi
{
    public const string Service = "kiln.ui.values";
    public const uint Version = 1;
    public const uint Lines = 6;  // the list of tooltip-lines providers
    public const uint Slots = 7;  // the list of slot-frame subscribers
    public static readonly string[] Rows =
        ["ui.values.detail", "ui.values.slot", "ui.values.held", "ui.values.hud", "ui.values.tooltip_lines"];
    public const string SlotsRow = "ui.slots.inspect";

    [StructLayout(LayoutKind.Sequential)]
    public struct Screen
    {
        public uint Size;
        public byte UiState, HudScreen, Crafting, Available;
        public uint Interaction, Station, Workshop, Recipe, Language, UiThread;
        public ulong Frame;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct ItemView
    {
        public uint Size, Flags, LocalPlayer, ClientEntity, ServerEntity, Item;
        public uint InventoryPage, SlotIndex, Count, Durability, DurabilityMax;
        public uint Fuel, FuelMax, UnitsPerCharge, Level, Rarity;
        public Screen Screen;
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
    public struct LineWriter
    {
        public uint Size, Capacity, Count, Status;
        public fixed byte Lines[16 * 16];
        public void* Reserved;
        public delegate* unmanaged[Cdecl]<LineWriter*, ulong, int> Append;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct Table
    {
        public ServiceHeader Header;
        public delegate* unmanaged[Cdecl]<ulong, uint, uint, void*, void*, Registration*, int> Subscribe;
        public delegate* unmanaged[Cdecl]<ulong, uint, void*, void*, Registration*, int> TooltipLines;
        public delegate* unmanaged[Cdecl]<ulong, int> Unsubscribe;
        public delegate* unmanaged[Cdecl]<ulong, ulong, ulong, void*, void*, uint, uint*, int> CopySlots;
        public delegate* unmanaged[Cdecl]<ulong, void*, void*, Registration*, int> SubscribeSlots;
        public delegate* unmanaged[Cdecl]<ulong, void*, void*, Registration*, int> SubscribeChanges;
        public delegate* unmanaged[Cdecl]<ulong, void*, int> OpenTooltipDetails;
        public delegate* unmanaged[Cdecl]<ulong, int> CloseTooltipDetails;
        public delegate* unmanaged[Cdecl]<ulong, uint, void*, int> TooltipDetails;
        public delegate* unmanaged[Cdecl]<ulong, void*, int> RefreshTooltipDetails;
        // ---- appended: ui.gemforge.inspect (not modelled: unavailable) and gems.sockets' gemforge_sockets ----
        public delegate* unmanaged[Cdecl]<ulong, ulong, ulong, void*, void*, uint, uint*, int> CopyGemForge;
        public delegate* unmanaged[Cdecl]<ulong, void*, void*, Registration*, int> SubscribeGemForge;
        public delegate* unmanaged[Cdecl]<ulong, uint, int> GemForgeSockets;
    }

    public const string GemForgeRow = "ui.gemforge.inspect";
    public const string SocketsRow = "gems.sockets";
    public const uint SocketCategories = 0x3fff;  // gemforge_sockets accepts ItemCategory bits 0..13
}

internal sealed class UiValuesFakeState
{
    public sealed record Reg(ulong Handle, ulong Owner, uint List, uint Item, nint Fn, nint User);

    public readonly object Lock = new();
    public readonly List<Reg> Regs = [];
    public readonly Dictionary<ulong, string> Texts = [];
    public readonly List<FakeUiDecision> Decisions = [];
    /// <summary>gemforge_sockets' masks by owner (0 removes): what the client's Gem Forge lists would be widened to.</summary>
    public readonly Dictionary<ulong, uint> SocketMasks = [];
    public ulong NextHandle = 1;
}

internal sealed unsafe class UiValuesFake : IKitFake
{
    public string Service => UiValuesAbi.Service;

    private static UiValuesAbi.Table* _table;
    internal static UiValuesAbi.TextOps* Ops;

    public void Register()
    {
        if (_table == null)
        {
            var t = (UiValuesAbi.Table*)NativeMemory.AllocZeroed((nuint)sizeof(UiValuesAbi.Table));  // process lifetime
            t->Header = new ServiceHeader { Size = (uint)sizeof(UiValuesAbi.Table), Version = UiValuesAbi.Version };
            t->Subscribe = &Subscribe;
            t->TooltipLines = &TooltipLines;
            t->Unsubscribe = &Unsubscribe;
            t->CopySlots = &CopySlots;
            t->SubscribeSlots = &SubscribeSlots;
            t->SubscribeChanges = &NoRegistration;
            t->OpenTooltipDetails = &NoDetails;
            t->CloseTooltipDetails = &Close;
            t->TooltipDetails = &NoDetailsWhich;
            t->RefreshTooltipDetails = &NoDetails;
            t->CopyGemForge = &NoCopy;
            t->SubscribeGemForge = &NoRegistration;
            t->GemForgeSockets = &GemForgeSockets;
            var ops = (UiValuesAbi.TextOps*)NativeMemory.AllocZeroed((nuint)sizeof(UiValuesAbi.TextOps));
            ops->Size = (uint)sizeof(UiValuesAbi.TextOps);
            ops->Version = 1;
            ops->Intern = &Intern;
            ops->Resolve = &Resolve;
            Ops = ops;
            _table = t;
        }
        KitBackend.Instance.RegisterTable(UiValuesAbi.Service, (nint)_table);
        FakeTables.Features(UiValuesAbi.Rows);
        // ui.slots.inspect is the kit's kiln.ui.inventory fake's row (Fakes/UiInventory): the slot frames a test draws
        FakeTables.Offline(UiValuesAbi.GemForgeRow, "no game UI draws the Gem Forge offline");
    }

    public void Start(KilnHarness harness) { }
    public void BeforeStep(KilnHarness harness, ulong step) { }
    public void End(KilnHarness harness) { }

    public void AddCoverage(List<CoverageRow> rows) =>
        rows.Add(new CoverageRow("Values in the game UI", CoverageKind.RecordingFake, [UiValuesAbi.Service, .. UiValuesAbi.Rows],
            "Ui.Values' own code on the kit's kiln.ui.values: providers register (ItemBar, SlotBar, HeldBar, Hud, TooltipLines) and a test " +
            "draws a value or a detail view (KilnHarness.UiValues.Draw, DetailLines) over a fake item: the providers run in order over the " +
            "accumulated value, edits are checked by the kind's masks, refusals and faults are recorded (Decisions). Offline: the game's " +
            "drawing and hooks, change events and the Item details window. Slot frames (ui.slots.inspect): the frames a test draws " +
            "with KilnHarness.UiInventory.Draw."));

    internal static UiValuesFakeState? State => KitBackend.Instance.Harness is { } h ? KitFakes.State<UiValuesFakeState>(h) : null;

    internal static int Add(ulong owner, uint list, uint item, void* fn, void* user, UiValuesAbi.Registration* r)
    {
        if (r == null || r->Size < (uint)sizeof(UiValuesAbi.Registration) || fn == null || owner == 0) return FakeTables.InvalidArgument;
        var row = list switch { 0 => "ui.values.detail", 1 => "ui.values.slot", 2 => "ui.values.held", UiValuesAbi.Lines => "ui.values.tooltip_lines", UiValuesAbi.Slots => UiValuesAbi.SlotsRow, _ => "ui.values.hud" };
        if (FakeTables.Active(UiValuesAbi.Service) is null || FakeTables.Off(row) || State is not { } st) return FakeTables.Unavailable;
        lock (st.Lock)
        {
            if (st.Regs.Count(x => x.Owner == owner) >= 128)
            {
                r->Ok = 0;
                r->Reason = 8;  // KUI_REGISTRATION_BUDGET
                return FakeTables.Ok;
            }
            var h = (0x5576ul << 48) | st.NextHandle++;
            st.Regs.Add(new UiValuesFakeState.Reg(h, owner, list, item, (nint)fn, (nint)user));
            r->Ok = 1;
            r->Handle = h;
        }
        return FakeTables.Ok;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Subscribe(ulong owner, uint kind, uint item, void* fn, void* user, UiValuesAbi.Registration* r)
    {
        try
        {
            if (kind > 5 || (kind >= 3 && item != 0)) return FakeTables.InvalidArgument;
            return Add(owner, kind, item, fn, user, r);
        }
        catch
        {
            return FakeTables.Internal;
        }
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int TooltipLines(ulong owner, uint item, void* fn, void* user, UiValuesAbi.Registration* r)
    {
        try
        {
            return Add(owner, UiValuesAbi.Lines, item, fn, user, r);
        }
        catch
        {
            return FakeTables.Internal;
        }
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Unsubscribe(ulong h)
    {
        if (State is not { } st) return FakeTables.NotFound;
        lock (st.Lock) return st.Regs.RemoveAll(x => x.Handle == h) > 0 ? FakeTables.Ok : FakeTables.NotFound;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int NoCopy(ulong o, ulong s, ulong f, void* frame, void* items, uint capacity, uint* required) => FakeTables.Unavailable;

    // copy_slots: the frame a test drew (KilnHarness.UiInventory.Draw), as just captured (frames do not age offline)
    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int CopySlots(ulong o, ulong s, ulong f, void* frame, void* slots, uint capacity, uint* required)
    {
        try
        {
            var fr = (Kiln.UiValuesNative.SlotsFrame*)frame;
            if (o == 0 || fr == null || fr->Size < (uint)sizeof(Kiln.UiValuesNative.SlotsFrame)) return FakeTables.InvalidArgument;
            if (FakeTables.Off(UiValuesAbi.SlotsRow) || UiInventoryFake.State is not { } st) return FakeTables.Unavailable;
            lock (st.Lock) return FillFrame(st, fr, (Kiln.UiValuesNative.VisibleSlot*)slots, capacity, required);
        }
        catch
        {
            return FakeTables.Internal;
        }
    }

    internal static int FillFrame(UiInventoryFakeState st, Kiln.UiValuesNative.SlotsFrame* fr, Kiln.UiValuesNative.VisibleSlot* slots,
        uint capacity, uint* required)
    {
        var size = fr->Size;
        *fr = default;
        fr->Size = size;
        if (st.Slots is not { } drawn)
        {
            fr->Reason = Kiln.UiValuesNative.ReasonNoFrame;
            return FakeTables.Ok;
        }
        fr->Session = 1;
        fr->Frame = st.Frame;
        fr->Screen = UiInventoryFake.ScreenOf(st.Screen, st.Frame);
        fr->CaptureMs = (ulong)Environment.TickCount64;
        fr->CanvasWidth = 1920;
        fr->CanvasHeight = 1080;
        fr->PixelsPerUnit = 1;
        fr->ViewportPx = new Kiln.UiValuesNative.Rect { Width = 1920, Height = 1080 };
        fr->TransformFlags = Kiln.UiValuesNative.Viewport;  // the mapping is never verified offline: no pixel rectangles
        if (required != null) *required = (uint)drawn.Length;
        if (capacity < drawn.Length || (drawn.Length > 0 && slots == null)) return FakeTables.BufferTooSmall;
        for (var i = 0; i < drawn.Length; i++) slots[i] = drawn[i];
        fr->Ok = 1;
        fr->Count = (uint)drawn.Length;
        return FakeTables.Ok;
    }

    // subscribe_slots: called with each frame a test draws
    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int SubscribeSlots(ulong owner, void* fn, void* user, UiValuesAbi.Registration* r)
    {
        try
        {
            if (FakeTables.Off(UiValuesAbi.SlotsRow)) return FakeTables.Unavailable;
            return Add(owner, UiValuesAbi.Slots, 0, fn, user, r);
        }
        catch
        {
            return FakeTables.Internal;
        }
    }

    // gems.sockets: records the owner's mask (the forge lists are not drawn offline). A switched-off row answers as the
    // native service does; a removal (0) always succeeds.
    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int GemForgeSockets(ulong owner, uint mask)
    {
        if (owner == 0 || (mask & ~UiValuesAbi.SocketCategories) != 0) return FakeTables.InvalidArgument;
        if (FakeTables.Active(UiValuesAbi.Service) is null || State is not { } st) return FakeTables.Unavailable;
        if (mask != 0 && FakeTables.Off(UiValuesAbi.SocketsRow)) return FakeTables.Unavailable;
        lock (st.Lock)
        {
            if (mask == 0) st.SocketMasks.Remove(owner);
            else st.SocketMasks[owner] = mask;
        }
        return FakeTables.Ok;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int NoRegistration(ulong o, void* fn, void* user, UiValuesAbi.Registration* r) => FakeTables.Unavailable;

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int NoDetails(ulong o, void* out_) => FakeTables.Unavailable;

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int NoDetailsWhich(ulong o, uint which, void* out_) => FakeTables.Unavailable;

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Close(ulong o) => FakeTables.Unavailable;

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Intern(UiValuesAbi.ProviderContext* c, NativeStr s, ulong* id)
    {
        if (id == null || State is not { } st) return FakeTables.InvalidArgument;
        var text = s.ToString();
        if (text.Length == 0)
        {
            *id = 0;
            return FakeTables.Ok;
        }
        lock (st.Lock)
        {
            foreach (var (k, v) in st.Texts)
                if (v == text)
                {
                    *id = k;
                    return FakeTables.Ok;
                }
            var n = (0x5554ul << 48) | (ulong)(st.Texts.Count + 1);
            st.Texts[n] = text;
            *id = n;
        }
        return FakeTables.Ok;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Resolve(UiValuesAbi.ProviderContext* c, ulong id, NativeStr* out_) => FakeTables.NotFound;

    internal static string? TextOf(UiValuesFakeState st, ulong id)
    {
        if (id == 0) return "";
        lock (st.Lock) return st.Texts.TryGetValue(id, out var t) ? t : null;
    }

    internal static ulong TextId(UiValuesFakeState st, string text)
    {
        if (text.Length == 0) return 0;
        lock (st.Lock)
        {
            foreach (var (k, v) in st.Texts)
                if (v == text) return k;
            var n = (0x5554ul << 48) | (ulong)(st.Texts.Count + 1);
            st.Texts[n] = text;
            return n;
        }
    }
}
