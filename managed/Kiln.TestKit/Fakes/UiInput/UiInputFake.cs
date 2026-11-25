// SPDX-License-Identifier: MIT
// The fake kiln.ui.input (native/core/abi/services/kiln_ui_input.h, v1; since 0.2.0-dev.31.13, mod API 0.2.22) under the
// real KeyInput (KeyListenFlags.Own, Pointer) and PadInput of Kiln.Ui. A test plays the player's side (KilnHarness.Input:
// Down, Repeat, Up, FocusLost, WorldChange, Leak, SetPad, Unplug) and reads what reached the game and Kiln's other
// handlers. The ownership rules are native/services/ui/input_own.h's, one for one: the owners are offered a new press in
// the order they subscribed and the first that takes it owns it; its repeats and its release go to the owner alone and
// stay kept from the game unless the game saw it go down (Leak); a new press of a claimed code ends that claim (lost);
// focus loss ends every claim (End Focus) and drops it; a world change ends the claims of the world before (End World) and
// keeps them as nobody's, as unsubscribe and disown do without a call. native/services/ui/tests/own_parity.json holds
// the cases both run (Kiln.TestKit.Tests UiInputFakeTests).
using System.Numerics;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace Kiln.TestKit;

internal sealed class UiInputFakeState
{
    public sealed class Claim
    {
        public ulong Sub;
        public uint Gen;
        public bool Orphan, Leaked;
    }

    public readonly object Lock = new();
    public readonly Dictionary<uint, Claim> Claims = [];
    public readonly List<string> Game = [], Others = [];
    public uint Generation = 1;
    public readonly UiNative.PadState[] Pads = new UiNative.PadState[4];

    public static uint Id(uint vk, int controller) => (vk & 0xFFFF) | ((uint)controller << 16);
}

internal sealed unsafe class UiInputFake : IKitFake
{
    public const string Service = "kiln.ui.input";
    public const string OwnedRow = "ui.input.owned", PadRow = "ui.input.pad_state";
    public const uint Keys = 1, Mouse = 2, Pointer = 4, Pad = 8;

    private static UiNative.InputTable* _table;
    // process-wide, as in the service: subscriptions are a mod's, a handle is never used twice
    private static readonly object Shared = new();
    private static readonly List<(ulong Handle, uint Sources, nint Fn, nint User)> Subs = [];
    private static ulong _nextHandle = 0x5100;
    // the press being offered on this thread, so that a disown inside its own call gives it up once taken (input_own.cpp)
    [ThreadStatic] private static (ulong Sub, uint Id, bool Disowned) _offering;

    string IKitFake.Service => Service;

    public void Register()
    {
        if (_table == null)
        {
            var t = (UiNative.InputTable*)NativeMemory.AllocZeroed((nuint)sizeof(UiNative.InputTable));  // process lifetime
            t->Header = new ServiceHeader { Size = (uint)sizeof(UiNative.InputTable), Version = 1 };
            t->SubscribeOwned = &SubscribeOwned;
            t->Unsubscribe = &Unsubscribe;
            t->Disown = &Disown;
            t->Owns = &Owns;
            t->PadStateFn = &PadStateOf;
            _table = t;
        }
        KitBackend.Instance.RegisterTable(Service, (nint)_table);
        FakeTables.Features([OwnedRow, PadRow]);
    }

    // a mod of an earlier harness has retired: its subscriptions went with it; a harness starts with no press held
    public void Start(KilnHarness harness)
    {
        lock (Shared) Subs.Clear();
    }

    public void BeforeStep(KilnHarness harness, ulong step) { }

    public void End(KilnHarness harness)
    {
        lock (Shared) Subs.Clear();
    }

    public void AddCoverage(List<CoverageRow> rows) =>
        rows.Add(new CoverageRow("Owned key, mouse and controller presses; the controllers' view", CoverageKind.RecordingFake,
            [Service, OwnedRow, PadRow],
            "KeyInput.Listen with KeyListenFlags.Own (and Pointer, Mouse, Pad) and PadInput on the kit's kiln.ui.input: a test " +
            "plays presses (KilnHarness.Input.Down, Repeat, Up), a focus loss, a world change and a press the game saw after all " +
            "(Leak), and reads what reached the game (Game) and Kiln's other key handlers (Others); the ownership rules are the " +
            "native ones (native/services/ui/tests/own_parity.json runs on both). SetPad and Unplug set what the game's last " +
            "poll of a controller read. Offline: the game window, raw input and XInput themselves, Kiln's menu key and Escape " +
            "ahead of the owners, the key subscribers' and bindings' own answers (Others take nothing), the cursor."));

    internal static UiInputFakeState? State =>
        FakeTables.Active(Service) is { } h ? KitFakes.State<UiInputFakeState>(h) : null;

    internal static uint SourceOf(uint vk) =>
        vk is 0x01 or 0x02 ? Pointer
        : vk is 0x04 or 0x05 or 0x06 ? Mouse
        : vk is 0 or 0x03 ? 0
        : vk < 0x100 ? Keys
        : vk is >= 0x200 and <= 0x211 ? Pad
        : 0;

    private static (ulong Handle, uint Sources, nint Fn, nint User)[] Snapshot()
    {
        lock (Shared) return [.. Subs];
    }

    private static uint Call(nint fn, nint user, in UiNative.OwnedInput o)
    {
        var copy = o;
        return ((delegate* unmanaged[Cdecl]<UiNative.OwnedInput*, void*, uint>)fn)(&copy, (void*)user);
    }

    private static UiNative.OwnedInput Record(uint kind, uint vk, int controller, KeyMods mods, Vector2? at, uint end = 0, bool leaked = false) => new()
    {
        Size = (uint)sizeof(UiNative.OwnedInput), Kind = kind, Source = SourceOf(vk), Vk = vk, Mods = (uint)mods,
        Controller = (uint)controller, End = end, Flags = leaked ? UiNative.OwnedLeaked : 0u, X = at?.X ?? 0, Y = at?.Y ?? 0,
    };

    // the notes of one rule (input_own.h Note): made after the state's lock is let go
    private static void Deliver(List<(ulong Sub, UiNative.OwnedInput Rec)> notes)
    {
        if (notes.Count == 0) return;
        var subs = Snapshot();
        foreach (var (sub, rec) in notes)
            foreach (var s in subs)
                if (s.Handle == sub) Call(s.Fn, s.User, rec);
    }

    private static string Code(uint vk, int controller) => controller != 0 ? $"{vk:x}@{controller}" : $"{vk:x}";

    // input_own.h Table.transition: true = ownership decided it, keep = kept from the game
    private static bool Transition(UiInputFakeState st, uint vk, int controller, bool down, bool repeat, KeyMods mods, Vector2? at,
        List<(ulong, UiNative.OwnedInput)> notes, out bool keep)
    {
        keep = false;
        var id = UiInputFakeState.Id(vk, controller);
        if (!st.Claims.TryGetValue(id, out var c)) return false;
        if (!c.Orphan && c.Gen != st.Generation)
        {
            notes.Add((c.Sub, Record(UiNative.OwnedEnd, vk, controller, 0, null, 2)));
            c.Orphan = true;
        }
        if (down && !repeat)
        {
            if (!c.Orphan) notes.Add((c.Sub, Record(UiNative.OwnedEnd, vk, controller, 0, null, 3)));
            st.Claims.Remove(id);
            return false;
        }
        keep = !c.Leaked;
        if (!c.Orphan) notes.Add((c.Sub, Record(down ? UiNative.OwnedRepeat : UiNative.OwnedRelease, vk, controller, mods, at, 0, c.Leaked)));
        if (!down) st.Claims.Remove(id);
        return true;
    }

    /// <summary>A new press (HarnessInput.Down): true = an owner took it (kept from the game and everyone else).</summary>
    internal static bool Press(KilnHarness harness, uint vk, int controller, KeyMods mods, Vector2? at)
    {
        var st = KitFakes.State<UiInputFakeState>(harness);
        var notes = new List<(ulong, UiNative.OwnedInput)>();
        uint gen;
        lock (st.Lock)
        {
            Transition(st, vk, controller, true, false, mods, at, notes, out _);  // a claim whose release never came: lost
            gen = st.Generation;
        }
        Deliver(notes);
        var source = SourceOf(vk);
        if (source != 0 && !FakeTables.Off(OwnedRow))
        {
            var rec = Record(UiNative.OwnedPress, vk, controller, mods, at);
            var id = UiInputFakeState.Id(vk, controller);
            var outer = _offering;
            try
            {
                foreach (var s in Snapshot())
                {
                    if ((s.Sources & source) == 0) continue;
                    _offering = (s.Handle, id, false);
                    if (Call(s.Fn, s.User, rec) != UiNative.OwnTake) continue;
                    lock (st.Lock)
                        st.Claims[id] = new UiInputFakeState.Claim { Sub = s.Handle, Gen = gen, Orphan = _offering.Disowned };
                    return true;
                }
            }
            finally
            {
                _offering = outer;
            }
        }
        lock (st.Lock)
        {
            st.Others.Add($"down {Code(vk, controller)}");
            st.Game.Add($"down {Code(vk, controller)}");
        }
        return false;
    }

    /// <summary>A repeat or a release (HarnessInput.Repeat, Up): true = kept from the game.</summary>
    internal static bool Later(KilnHarness harness, uint vk, int controller, bool down, KeyMods mods, Vector2? at)
    {
        var st = KitFakes.State<UiInputFakeState>(harness);
        var notes = new List<(ulong, UiNative.OwnedInput)>();
        bool handled, keep;
        var what = down ? "repeat" : "up";
        lock (st.Lock)
        {
            handled = Transition(st, vk, controller, down, true, mods, at, notes, out keep);
            if (!handled) st.Others.Add($"{what} {Code(vk, controller)}");
            if (!keep) st.Game.Add($"{what} {Code(vk, controller)}");
        }
        Deliver(notes);
        return keep;
    }

    internal static void FocusLost(KilnHarness harness)
    {
        var st = KitFakes.State<UiInputFakeState>(harness);
        var notes = new List<(ulong, UiNative.OwnedInput)>();
        lock (st.Lock)
        {
            foreach (var (id, c) in st.Claims.OrderBy(p => p.Key))
                if (!c.Orphan) notes.Add((c.Sub, Record(UiNative.OwnedEnd, id & 0xFFFF, (int)(id >> 16), 0, null, 1)));
            st.Claims.Clear();
        }
        Deliver(notes);
    }

    internal static void WorldChange(KilnHarness harness)
    {
        var st = KitFakes.State<UiInputFakeState>(harness);
        var notes = new List<(ulong, UiNative.OwnedInput)>();
        lock (st.Lock)
        {
            st.Generation++;
            foreach (var (id, c) in st.Claims.OrderBy(p => p.Key))
                if (!c.Orphan && c.Gen != st.Generation)
                {
                    notes.Add((c.Sub, Record(UiNative.OwnedEnd, id & 0xFFFF, (int)(id >> 16), 0, null, 2)));
                    c.Orphan = true;
                }
        }
        Deliver(notes);
    }

    internal static void Leak(KilnHarness harness, uint vk, int controller)
    {
        var st = KitFakes.State<UiInputFakeState>(harness);
        lock (st.Lock)
        {
            if (st.Claims.TryGetValue(UiInputFakeState.Id(vk, controller), out var c)) c.Leaked = true;
            st.Game.Add($"down {Code(vk, controller)}");
        }
    }

    internal static bool Hidden(KilnHarness harness, uint vk, int controller)
    {
        var st = KitFakes.State<UiInputFakeState>(harness);
        lock (st.Lock) return st.Claims.TryGetValue(UiInputFakeState.Id(vk, controller), out var c) && !c.Leaked;
    }

    private static void DisownLocked(UiInputFakeState st, ulong sub, uint id)
    {
        foreach (var (k, c) in st.Claims)
            if (!c.Orphan && c.Sub == sub && (id == 0 || k == id)) c.Orphan = true;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int SubscribeOwned(ulong owner, uint sources, delegate* unmanaged[Cdecl]<UiNative.OwnedInput*, void*, uint> fn, void* user, ulong* out_)
    {
        if (fn == null || sources == 0 || (sources & ~15u) != 0) return FakeTables.InvalidArgument;
        if (FakeTables.Off(OwnedRow) || State is null) return FakeTables.Unavailable;
        lock (Shared)
        {
            var h = ++_nextHandle;
            Subs.Add((h, sources, (nint)fn, (nint)user));
            if (out_ != null) *out_ = h;
        }
        return FakeTables.Ok;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Unsubscribe(ulong h)
    {
        bool found;
        lock (Shared) found = Subs.RemoveAll(s => s.Handle == h) > 0;
        if (!found) return FakeTables.NotFound;
        if (State is { } st)
            lock (st.Lock) DisownLocked(st, h, 0);
        return FakeTables.Ok;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static uint Disown(ulong h, uint vk, uint controller)
    {
        if (h == 0 || State is not { } st) return 0;
        lock (st.Lock)
        {
            var id = vk == 0 ? 0u : UiInputFakeState.Id(vk, (int)controller);
            var n = st.Claims.Count(p => !p.Value.Orphan && p.Value.Sub == h && (id == 0 || p.Key == id));
            DisownLocked(st, h, id);
            // inside the call that offers a press to h: that press too, once it is taken
            if (_offering.Sub == h && !_offering.Disowned && (id == 0 || id == _offering.Id))
            {
                _offering.Disowned = true;
                n++;
            }
            return (uint)n;
        }
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static byte Owns(ulong h, uint vk, uint controller)
    {
        if (h == 0 || State is not { } st) return 0;
        lock (st.Lock)
            return st.Claims.TryGetValue(UiInputFakeState.Id(vk, (int)controller), out var c) && !c.Orphan && c.Sub == h ? (byte)1 : (byte)0;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int PadStateOf(uint controller, UiNative.PadState* out_)
    {
        if (out_ == null || out_->Size < (uint)sizeof(UiNative.PadState) || controller > 3) return FakeTables.InvalidArgument;
        *out_ = default;
        out_->Size = (uint)sizeof(UiNative.PadState);
        out_->Controller = controller;
        if (FakeTables.Off(PadRow) || State is not { } st) return FakeTables.Unavailable;
        lock (st.Lock)
        {
            *out_ = st.Pads[controller];
            out_->Size = (uint)sizeof(UiNative.PadState);
            out_->Controller = controller;
            uint owned = 0;
            foreach (var (id, c) in st.Claims)
                if (!c.Orphan && (id >> 16) == controller && (id & 0xFFFF) is >= 0x200 and <= 0x211) owned |= 1u << (int)((id & 0xFFFF) - 0x200);
            out_->Owned = owned;
            out_->Hidden |= owned;
        }
        return FakeTables.Ok;
    }
}
