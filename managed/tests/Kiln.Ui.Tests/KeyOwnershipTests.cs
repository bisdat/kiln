// SPDX-License-Identifier: MIT
// KeyListenFlags.Own / Pointer and PadInput (since 0.2.0-dev.31.13, mod API 0.2.22): kiln.ui.input's subscribe_owned and
// pad_state as public API. What kiln.ui.input is asked for (the sources), what an owning listener gets (the press, its
// repeats, its release, an end with its reason, a leaked release, the pointer's position, a pad's controller), what
// kiln.ui.input is told back (take a press; nothing else counts), Owns and Disown, the end of a listener (Dispose, its
// mod's unload) and a Kiln without kiln.ui.input. No game: a fake kiln.ui.input table. The ownership rules themselves are
// the native ones (native/services/ui/tests/input_own_test.cpp) and the kit's (Kiln.TestKit.Tests UiInputFakeTests).
using System.Numerics;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using Kiln.FakeUi;
using Xunit;

namespace Kiln.KeyInputTests;

[Collection(UiSerial.Name)]
public sealed unsafe class KeyOwnershipTests : IDisposable
{
    private static readonly List<string> Calls = [];
    private static delegate* unmanaged[Cdecl]<UiNative.OwnedInput*, void*, uint> _fn;
    private static void* _user;
    private static uint _owned;  // what owns answers
    private static UiNative.PadState _pad;
    private static int _padResult;
    private readonly IKilnBackend _previous = Backend.Current;
    private readonly UiNative.Table4* _fake;
    private readonly FakeUiTable.Backend _backend = new();
    private UiNative.InputTable* _input;

    public KeyOwnershipTests()
    {
        FakeUiTable.Install();
        Backend.Current = _backend;
        _fake = FakeUiTable.Table;
        lock (Calls) Calls.Clear();
        _fn = null;
        _user = null;
        _owned = 0;
        _pad = default;
        _padResult = 0;
        KeyInput.ResetForTests();
        UiNative.ResetForTests();
        _input = (UiNative.InputTable*)NativeMemory.AllocZeroed((nuint)sizeof(UiNative.InputTable));
        _input->Header = new ServiceHeader { Size = (uint)sizeof(UiNative.InputTable), Version = 1 };
        _input->SubscribeOwned = &Subscribe;
        _input->Unsubscribe = &Unsubscribe;
        _input->Disown = &Disown;
        _input->Owns = &Owns;
        _input->PadStateFn = &PadStateOf;
        UiNative.SetInputForTests(_input);
    }

    public void Dispose()
    {
        KeyInput.ResetForTests();
        FakeUiTable.Table = _fake;
        UiNative.SetInputForTests(null);
        NativeMemory.Free(_input);
        Backend.Current = _previous;
        UiNative.ResetForTests();
        FakeUiTable.Free();
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Subscribe(ulong o, uint sources, delegate* unmanaged[Cdecl]<UiNative.OwnedInput*, void*, uint> f, void* u, ulong* h)
    {
        lock (Calls) Calls.Add($"subscribe_owned owner {o} sources {sources}");
        _fn = f;
        _user = u;
        *h = 77;
        return 0;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Refuse(ulong o, uint sources, delegate* unmanaged[Cdecl]<UiNative.OwnedInput*, void*, uint> f, void* u, ulong* h) => 7;

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Unsubscribe(ulong h)
    {
        lock (Calls) Calls.Add($"unsubscribe {h}");
        return 0;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static uint Disown(ulong h, uint vk, uint controller)
    {
        lock (Calls) Calls.Add($"disown {h} {vk:x} {controller}");
        return 1;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static byte Owns(ulong h, uint vk, uint controller) => h == 77 && vk == _owned ? (byte)1 : (byte)0;

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int PadStateOf(uint controller, UiNative.PadState* out_)
    {
        if (out_->Size < (uint)sizeof(UiNative.PadState)) return 2;
        *out_ = _pad;
        out_->Size = (uint)sizeof(UiNative.PadState);
        out_->Controller = controller;
        return _padResult;
    }

    /// <summary>kiln.ui.input hands the owning listener one record, as input_own.cpp does: 1 = take the press.</summary>
    private static uint Send(uint kind, uint vk, uint mods = 0, uint end = 0, uint flags = 0, uint controller = 0, float x = 0, float y = 0,
        uint scan = 0)
    {
        Assert.True(_fn != null, "no owned-input callback was subscribed");
        var o = new UiNative.OwnedInput
        {
            Size = (uint)sizeof(UiNative.OwnedInput), Kind = kind, Vk = vk, Mods = mods, End = end, Flags = flags, Controller = controller,
            X = x, Y = y, Scan = scan,
        };
        return _fn(&o, _user);
    }

    [Fact]
    public void The_mirrors_are_kiln_ui_input_h_s_layout()
    {
        Assert.Equal(64, sizeof(UiNative.OwnedInput));
        Assert.Equal(40, (int)Marshal.OffsetOf<UiNative.OwnedInput>(nameof(UiNative.OwnedInput.X)));
        Assert.Equal(56, (int)Marshal.OffsetOf<UiNative.OwnedInput>(nameof(UiNative.OwnedInput.TextInput)));
        Assert.Equal(88, sizeof(UiNative.PadState));
        Assert.Equal(24, (int)Marshal.OffsetOf<UiNative.PadState>(nameof(UiNative.PadState.LeftX)));
        Assert.Equal(60, (int)Marshal.OffsetOf<UiNative.PadState>(nameof(UiNative.PadState.Packet)));
        Assert.Equal(72, (int)Marshal.OffsetOf<UiNative.PadState>(nameof(UiNative.PadState.Polls)));
        Assert.Equal(48, sizeof(UiNative.InputTable));
    }

    [Fact]
    public void Blueprint_owns_a_right_button_gesture_press_and_release()
    {
        var got = new List<KeyPress>();
        using var l = KeyInput.Listen(p => { got.Add(p); return p.IsPointer && p.VirtualKey == 0x02; },
            KeyListenFlags.Own | KeyListenFlags.Pointer);
        Assert.True(l.Ok);
        Assert.True(l.Own);
        Assert.True(l.Pointer);
        Assert.False(l.Mouse);
        lock (Calls) Assert.Equal(["subscribe_owned owner 0 sources 5"], Calls);  // keys | pointer
        Assert.Equal(1u, Send(UiNative.OwnedPress, 0x02, mods: 2, x: 640, y: 360));  // taken: Blueprint owns it
        Assert.Equal(0u, Send(UiNative.OwnedPress, 0x01, x: 10, y: 20));           // the left button: passed
        Assert.Equal(0u, Send(UiNative.OwnedRelease, 0x02, x: 700, y: 380));       // a release's answer counts for nothing
        Assert.Equal(3, got.Count);
        Assert.Equal(new KeyPress(0x02, 0, KeyMods.Shift, true, false, false, false) { Position = new Vector2(640, 360) }, got[0]);
        Assert.False(got[0].Owned);
        Assert.True(got[2].Owned && !got[2].Down && got[2].Ended == KeyOwnEnd.None && got[2].Position == new Vector2(700, 380));
        _owned = 0x02;
        Assert.True(l.Owns(0x02));
        Assert.False(l.Owns(0x01));
        Assert.False(l.Owns(0x02, controller: 9));
    }

    [Fact]
    public void Repeats_ends_and_a_leaked_release_reach_the_owner_as_owned()
    {
        var got = new List<KeyPress>();
        using var l = KeyInput.Listen(p => { got.Add(p); return true; }, KeyListenFlags.Own | KeyListenFlags.Mouse | KeyListenFlags.Pad);
        lock (Calls) Assert.Equal(["subscribe_owned owner 0 sources 11"], Calls);  // keys | mouse | pad
        Assert.Equal(1u, Send(UiNative.OwnedPress, 0x42, scan: 0x30));
        Send(UiNative.OwnedRepeat, 0x42, scan: 0x30);
        Send(UiNative.OwnedEnd, 0x42, end: 1);                         // focus
        Send(UiNative.OwnedPress, 0x20D, mods: 8, controller: 2);      // a pad's B with LB held, on controller 2
        Send(UiNative.OwnedEnd, 0x20D, end: 2, controller: 2);         // a world change
        Send(UiNative.OwnedPress, 0x73);
        Send(UiNative.OwnedRelease, 0x73, flags: UiNative.OwnedLeaked);  // Alt+F4: the game saw it
        Send(UiNative.OwnedEnd, 0x04, end: 3);                         // lost
        Assert.True(got[1].Down && got[1].Repeat && got[1].Owned && got[1].ScanCode == 0x30);
        Assert.Equal((false, true, KeyOwnEnd.Focus), (got[2].Down, got[2].Owned, got[2].Ended));
        Assert.Equal((2, KeyMods.PadLB, true), (got[3].Controller, got[3].Mods, got[3].IsPad));
        Assert.Equal((KeyOwnEnd.World, 2), (got[4].Ended, got[4].Controller));
        Assert.True(got[6].Leaked && got[6].Owned && !got[6].Down);
        Assert.Equal(KeyOwnEnd.Lost, got[7].Ended);
        Assert.All(got, p => Assert.Null(p.Position));  // no pointer here
    }

    [Fact]
    public void Disown_and_dispose_go_to_kiln_ui_input_and_nothing_comes_after()
    {
        var n = 0;
        var l = KeyInput.Listen(_ => { n++; return true; }, KeyListenFlags.Own | KeyListenFlags.Pad);
        Assert.Equal(1u, Send(UiNative.OwnedPress, 0x20D, controller: 1));
        Assert.Equal(1, l.Disown(0x20D, 1));
        Assert.Equal(1, l.Disown());
        Assert.Equal(0, l.Disown(0x20D, 7));  // no such controller: nothing asked
        l.Dispose();
        l.Dispose();  // twice is fine
        lock (Calls) Assert.Equal(["subscribe_owned owner 0 sources 9", "disown 77 20d 1", "disown 77 0 0", "unsubscribe 77"], Calls);
        Assert.Equal(0u, Send(UiNative.OwnedPress, 0x20D));  // one already in flight: not delivered, not taken
        Assert.Equal(1, n);
        Assert.Equal(0, KeyInput.Count);
        Assert.False(l.Owns(0x20D));
    }

    [Fact]
    public void A_mods_owning_listener_runs_as_the_mod_and_stops_when_it_unloads()
    {
        var mod = HostAccess.CreateContext(new ModInfo("blueprint", "Blueprint", "1.0.0", "."), 93, ".");
        ModContext? seen = null;
        KeyListener l;
        var previous = HostAccess.Current;
        HostAccess.Current = mod;
        try
        {
            l = KeyInput.Listen(_ => { seen = HostAccess.Current; return true; }, KeyListenFlags.Own | KeyListenFlags.Pointer);
        }
        finally
        {
            HostAccess.Current = previous;
        }
        Assert.True(l.Ok);
        Assert.Equal(1u, Send(UiNative.OwnedPress, 0x02));
        Assert.Same(mod, seen);
        mod.Retire();
        lock (Calls) Assert.Equal(["subscribe_owned owner 93 sources 5", "unsubscribe 77"], Calls);  // its presses: nobody's
        Assert.Equal(0, KeyInput.Count);
        Assert.Equal(0u, Send(UiNative.OwnedPress, 0x02));
    }

    [Fact]
    public void A_handler_that_throws_takes_nothing_and_is_logged()
    {
        using var l = KeyInput.Listen(_ => throw new InvalidOperationException("boom"), KeyListenFlags.Own);
        Assert.Equal(0u, Send(UiNative.OwnedPress, 0x42));
        lock (_backend.Logs) Assert.Contains(_backend.Logs, s => s.Contains("KeyListenFlags.Own", StringComparison.Ordinal) && s.Contains("boom", StringComparison.Ordinal));
    }

    [Fact]
    public void Pointer_needs_Own_and_unknown_flags_throw()
    {
        Assert.Throws<ArgumentException>(() => KeyInput.Listen(_ => true, KeyListenFlags.Pointer));
        Assert.Throws<ArgumentOutOfRangeException>(() => KeyInput.Listen(_ => true, (KeyListenFlags)16));
        lock (Calls) Assert.Empty(Calls);
    }

    [Fact]
    public void Without_kiln_ui_input_an_owning_listener_says_why()
    {
        UiNative.SetInputForTests(null);
        var l = KeyInput.Listen(_ => true, KeyListenFlags.Own);
        Assert.False(l.Ok);
        Assert.False(l.Own);
        Assert.Contains("0.2.0-dev.31.13", l.Reason);
        Assert.Equal(0, l.Disown());
        Assert.False(l.Owns(0x42));
        l.Dispose();
        Assert.Equal(0, KeyInput.Count);
        lock (Calls) Assert.Empty(Calls);
    }

    [Fact]
    public void A_refused_owning_subscription_is_not_ok()
    {
        _input->SubscribeOwned = &Refuse;
        var l = KeyInput.Listen(_ => true, KeyListenFlags.Own);
        Assert.False(l.Ok);
        Assert.Contains("refused", l.Reason);
        Assert.Equal(0, KeyInput.Count);
    }

    [Fact]
    public void PadInput_reads_the_controllers_view()
    {
        _pad = new UiNative.PadState
        {
            Connected = 1, Buttons = 0x2000 | 0x10000, Owned = 0x2000, Hidden = 0x2000, LeftX = 0.5f, LeftY = -0.25f, RightX = 1,
            LeftTrigger = 0.75f, RawLeftX = 16000, RawLeftTrigger = 200, Packet = 9, Polls = 3, PollMs = 1234,
        };
        var p = PadInput.Read(2);
        Assert.Equal(new PadState(2, true, PadButtons.B | PadButtons.LT, new Vector2(0.5f, -0.25f), new Vector2(1, 0), 0.75f, 0)
        {
            Owned = PadButtons.B, Hidden = PadButtons.B, Packet = 9, Polls = 3, PolledAtMs = 1234, RawSticks = (16000, 0, 0, 0),
            RawTriggers = (200, 0),
        }, p);
        Assert.True(p.IsDown(PadButtons.B) && p.IsDown(PadButtons.B | PadButtons.LT) && !p.IsDown(PadButtons.B | PadButtons.A));
        Assert.False(p.IsDown(PadButtons.None));
        Assert.True(PadInput.Available);
        _padResult = 6;  // UNAVAILABLE: XInput never loaded; the record still says not connected
        _pad = default;
        Assert.False(PadInput.Read().Connected);
        Assert.False(PadInput.Available);
        Assert.Throws<ArgumentOutOfRangeException>(() => PadInput.Read(4));
        Assert.Throws<ArgumentOutOfRangeException>(() => PadInput.Read(-1));
        UiNative.SetInputForTests(null);
        Assert.Equal(new PadState(1, false, 0, default, default, 0, 0), PadInput.Read(1));
    }

    [Fact]
    public void PadButtons_are_the_pad_codes_bits()
    {
        Assert.Equal(1u << (int)(PadKeys.A - PadKeys.First), (uint)PadButtons.A);
        Assert.Equal(1u << (int)(PadKeys.B - PadKeys.First), (uint)PadButtons.B);
        Assert.Equal(1u << (int)(PadKeys.LB - PadKeys.First), (uint)PadButtons.LB);
        Assert.Equal(1u << (int)(PadKeys.LT - PadKeys.First), (uint)PadButtons.LT);
        Assert.Equal(1u << (int)(PadKeys.RT - PadKeys.First), (uint)PadButtons.RT);
        foreach (var (vk, name) in PadKeys.Table)
            Assert.True(Enum.IsDefined((PadButtons)(1u << (int)(vk - PadKeys.First))), $"Pad:{name} has a PadButtons bit");
    }
}
