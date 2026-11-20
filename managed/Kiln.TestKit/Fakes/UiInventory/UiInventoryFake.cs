// SPDX-License-Identifier: MIT
// The fake kiln.ui.inventory (native/core/abi/services/kiln_ui_inventory.h, v1) under the real Ui.Inventory of Kiln.Ui:
// click handlers subscribe as with the game, a test draws a screen's slots (KilnHarness.UiInventory.Draw), points at one
// (Hover) and clicks it (Click); the press rules of the native service run here (a drawn frame, a screen with a pointer,
// the button and the exact modifiers, take or watch). Marks stay Kiln.Ui's own: MarkOf asks it what a drawn slot would
// show. The game's drawing, its hooks and the pixel mapping are not modelled.
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace Kiln.TestKit;

internal sealed class UiInventoryFakeState
{
    public sealed record Reg(ulong Handle, ulong Owner, uint Button, uint Mods, bool Take, nint Fn, nint User);

    public readonly object Lock = new();
    public readonly List<Reg> Regs = [];
    public UiValuesNative.VisibleSlot[]? Slots;  // the latest drawn frame (null: nothing drawn yet)
    public HudScreen Screen;
    public ulong Frame;
    public int? Hovered;
    public ulong NextHandle = 1;
}

internal sealed unsafe class UiInventoryFake : IKitFake
{
    public const string Service = UiInventoryNative.Service;
    public const string ClickRow = Ui.Inventory.ClickCapability;
    public const string SlotsRow = Ui.Inventory.MarkCapability;
    public static readonly string[] Features = [ClickRow, SlotsRow];
    private const uint ModsAll = 1 | 2 | 4;  // KILN_UI_MOD_CTRL | SHIFT | ALT
    private const uint ReasonBudget = 8, NoSlot = 32, NoViewport = 34;
    private const uint ReasonNoFrame = UiValuesNative.ReasonNoFrame;

    private static UiInventoryNative.Table* _table;

    string IKitFake.Service => Service;

    public void Register()
    {
        if (_table == null)
        {
            var t = (UiInventoryNative.Table*)NativeMemory.AllocZeroed((nuint)sizeof(UiInventoryNative.Table));
            t->Header = new ServiceHeader { Size = (uint)sizeof(UiInventoryNative.Table), Version = 1 };
            t->SubscribeClicks = &SubscribeClicks;
            t->SlotAt = &SlotAt;
            t->GetMapping = &GetMapping;
            t->Unsubscribe = &Unsubscribe;
            _table = t;  // process lifetime
        }
        KitBackend.Instance.RegisterTable(Service, (nint)_table);
        FakeTables.Features(Features);
    }

    public void Start(KilnHarness harness) { }
    public void BeforeStep(KilnHarness harness, ulong step) { }
    public void End(KilnHarness harness) { }

    public void AddCoverage(List<CoverageRow> rows) =>
        rows.Add(new CoverageRow("The game's inventory slots", CoverageKind.RecordingFake, [Service, ClickRow, SlotsRow],
            "Ui.Inventory's own code on the kit's kiln.ui.inventory: OnClick handlers subscribe with their button, exact " +
            "modifiers and Take; a test draws a screen's slots (KilnHarness.UiInventory.Draw), hovers one (Hovered) and clicks " +
            "it (Click): only on a screen with a pointer, the matching handlers get the click with the slot's item and entity, " +
            "Taken when one asked for it. Marks and MarkWhen rules are Kiln.Ui's (MarkOf: what a drawn slot would show); " +
            "Ui.Values.Slots reads the drawn frame. Offline: the game's drawing and hooks, the pixel mapping, the 250 ms frame age."));

    internal static UiInventoryFakeState? State =>
        FakeTables.Active(Service) is { } h ? KitFakes.State<UiInventoryFakeState>(h) : null;

    /// <summary>The screens on which a press may hit a slot (kiln.ui.inventory's pointer screens).</summary>
    internal static bool PointerScreen(HudScreen s) => (byte)s is 4 or 8 or 9 or 22 or 25 or 27 or 28 or 41;

    internal static UiNative.Screen ScreenOf(HudScreen s, ulong frame) => new()
    {
        Size = (uint)sizeof(UiNative.Screen), UiState = 7, HudScreen = (byte)s, Available = 1, Frame = frame,
    };

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int SubscribeClicks(ulong owner, UiInventoryNative.ClickFilter* f, delegate* unmanaged[Cdecl]<UiInventoryNative.Click*, void*, void> fn,
        void* user, UiValuesNative.Registration* r)
    {
        try
        {
            if (r == null || r->Size < (uint)sizeof(UiValuesNative.Registration)) return FakeTables.InvalidArgument;
            r->Ok = 0;
            r->Reason = 0;
            r->Handle = 0;
            if (owner == 0 || fn == null || f == null || f->Size < (uint)sizeof(UiInventoryNative.ClickFilter) ||
                f->Button is not (1 or 3) || (f->Mods & ~ModsAll) != 0 || (f->Flags & ~UiInventoryNative.ClickTake) != 0)
                return FakeTables.InvalidArgument;
            if (FakeTables.Off(ClickRow) || FakeTables.Off(SlotsRow) || State is not { } st) return FakeTables.Unavailable;
            lock (st.Lock)
            {
                if (st.Regs.Count(x => x.Owner == owner) >= 128)
                {
                    r->Reason = ReasonBudget;  // KUI_REGISTRATION_BUDGET
                    return FakeTables.Ok;
                }
                var h = (0x5569ul << 48) | st.NextHandle++;
                st.Regs.Add(new UiInventoryFakeState.Reg(h, owner, f->Button, f->Mods, (f->Flags & UiInventoryNative.ClickTake) != 0,
                    (nint)fn, (nint)user));
                r->Ok = 1;
                r->Handle = h;
            }
            return FakeTables.Ok;
        }
        catch
        {
            return FakeTables.Internal;
        }
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int SlotAt(ulong owner, float x, float y, float w, float h, UiInventoryNative.Hit* o)
    {
        if (owner == 0 || o == null || o->Size < (uint)sizeof(UiInventoryNative.Hit)) return FakeTables.InvalidArgument;
        var size = o->Size;
        *o = default;
        o->Size = size;
        if (FakeTables.Off(SlotsRow) || State is not { } st) return FakeTables.Unavailable;
        lock (st.Lock)
        {
            if (st.Slots is not { } slots)
            {
                o->Reason = ReasonNoFrame;
                return FakeTables.Ok;
            }
            o->Session = 1;
            o->Frame = st.Frame;
            o->PointerScreen = PointerScreen(st.Screen) ? 1u : 0u;
            // only the cursor is modelled (x < 0): the slot the test hovers
            if (x >= 0 || st.Hovered is not { } i || i >= slots.Length)
            {
                o->Reason = NoSlot;
                return FakeTables.Ok;
            }
            o->Ok = 1;
            o->Slot = slots[i];
        }
        return FakeTables.Ok;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int GetMapping(ulong owner, float w, float h, UiInventoryNative.Mapping* o)
    {
        if (owner == 0 || o == null || o->Size < (uint)sizeof(UiInventoryNative.Mapping)) return FakeTables.InvalidArgument;
        var size = o->Size;
        *o = default;
        o->Size = size;
        if (FakeTables.Off(SlotsRow) || State is null) return FakeTables.Unavailable;
        o->Reason = NoViewport;  // the game's canvas is not modelled: Kiln.Ui draws no marks offline (MarkOf says what they are)
        return FakeTables.Ok;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Unsubscribe(ulong handle)
    {
        if (State is not { } st) return FakeTables.NotFound;
        lock (st.Lock) return st.Regs.RemoveAll(x => x.Handle == handle) > 0 ? FakeTables.Ok : FakeTables.NotFound;
    }

    /// <summary>A press on drawn slot <paramref name="index"/>: the native service's rules, then the matching handlers.</summary>
    internal static (int Delivered, bool Taken) Press(UiInventoryFakeState st, int index, uint button, uint mods)
    {
        UiInventoryNative.Click c;
        UiInventoryFakeState.Reg[] matched;
        lock (st.Lock)
        {
            if (st.Slots is not { } slots || !PointerScreen(st.Screen) || FakeTables.Off(ClickRow) || FakeTables.Off(SlotsRow))
                return (0, false);
            ArgumentOutOfRangeException.ThrowIfNegative(index);
            ArgumentOutOfRangeException.ThrowIfGreaterThanOrEqual(index, slots.Length);
            matched = st.Regs.Where(x => x.Button == button && x.Mods == (mods & ModsAll)).ToArray();
            if (matched.Length == 0) return (0, false);
            c = new UiInventoryNative.Click
            {
                Size = (uint)sizeof(UiInventoryNative.Click), Button = button, Mods = mods & ModsAll,
                Taken = matched.Any(x => x.Take) ? 1u : 0u, Session = 1, Frame = st.Frame, PressMs = (ulong)Environment.TickCount64,
                Screen = ScreenOf(st.Screen, st.Frame), Slot = slots[index],
            };
        }
        var delivered = 0;
        foreach (var reg in matched)
        {
            lock (st.Lock)
                if (!st.Regs.Contains(reg)) continue;  // an earlier handler disposed this one
            ((delegate* unmanaged[Cdecl]<UiInventoryNative.Click*, void*, void>)reg.Fn)(&c, (void*)reg.User);
            delivered++;
        }
        return (delivered, c.Taken != 0);
    }
}
