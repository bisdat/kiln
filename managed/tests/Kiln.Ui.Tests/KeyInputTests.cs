// SPDX-License-Identifier: MIT
// KeyInput (since 0.2.0-dev.29.19): kiln.ui's key subscription as public API. What kiln.ui is asked for (subscribe_key_ex
// with KILN_UI_KEYS_MOUSE on a v7 table, subscribe_key on an older one, nothing without kiln.ui), what a listener gets (the
// press with its binding code), what kiln.ui is told back (keep or let go), and the end of a listener (Dispose, its mod's
// unload). No game: a fake kiln.ui table.
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using Kiln.FakeUi;
using Xunit;

namespace Kiln.KeyInputTests;

[Collection(UiSerial.Name)]
public sealed unsafe class KeyInputTests : IDisposable
{
    private static readonly List<string> Calls = [];
    private static delegate* unmanaged[Cdecl]<UiNative.Key*, void*, byte> _fn;
    private static void* _user;
    private readonly IKilnBackend _previous = Backend.Current;
    private readonly UiNative.Table4* _fake;
    private readonly FakeUiTable.Backend _backend = new();
    private UiNative.Table7* _t7;

    public KeyInputTests()
    {
        FakeUiTable.Install();
        Backend.Current = _backend;
        _fake = FakeUiTable.Table;
        lock (Calls) Calls.Clear();
        _fn = null;
        _user = null;
        KeyInput.ResetForTests();
        UiNative.ResetForTests();
    }

    public void Dispose()
    {
        KeyInput.ResetForTests();
        FakeUiTable.Table = _fake;
        if (_t7 != null) NativeMemory.Free(_t7);
        Backend.Current = _previous;
        UiNative.ResetForTests();
        FakeUiTable.Free();
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int OldKeys(ulong o, delegate* unmanaged[Cdecl]<UiNative.Key*, void*, byte> f, void* u, ulong* h)
    {
        lock (Calls) Calls.Add($"subscribe_key owner {o}");
        _fn = f;
        _user = u;
        *h = 21;
        return 0;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int NewKeys(ulong o, delegate* unmanaged[Cdecl]<UiNative.Key*, void*, byte> f, void* u, uint flags, ulong* h)
    {
        lock (Calls) Calls.Add($"subscribe_key_ex owner {o} flags {flags}");
        _fn = f;
        _user = u;
        *h = 42;
        return 0;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Refuse(ulong o, delegate* unmanaged[Cdecl]<UiNative.Key*, void*, byte> f, void* u, uint flags, ulong* h) => 7;

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static nint Zero() => 0;

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Unsubscribe(ulong h)
    {
        lock (Calls) Calls.Add($"unsubscribe {h}");
        return 0;
    }

    /// <summary>The fake table grown to v7; <paramref name="size"/> says how much of it this kiln.ui has.</summary>
    private void InstallV7(int size)
    {
        _t7 = (UiNative.Table7*)NativeMemory.AllocZeroed((nuint)sizeof(UiNative.Table7));
        _t7->Base.Base.Base = *_fake;
        ref var t = ref _t7->Base.Base.Base.Base.Base.Base;
        t.Header.Size = (uint)size;
        t.Header.Version = size >= sizeof(UiNative.Table7) ? 7u : 6u;
        t.SubscribeKey = &OldKeys;
        t.Unsubscribe = &Unsubscribe;
        _t7->SubscribeKeyEx = &NewKeys;
        // the slots past v4 (menus, themes...) answer "nothing" too: a mod's unload asks some of them
        var slots = (nint*)((byte*)_t7 + sizeof(ServiceHeader));
        for (var i = 0; i < (sizeof(UiNative.Table7) - sizeof(ServiceHeader)) / sizeof(nint); i++)
            if (slots[i] == 0) slots[i] = (nint)(delegate* unmanaged[Cdecl]<nint>)&Zero;
        FakeUiTable.Table = (UiNative.Table4*)_t7;
        UiNative.ResetForTests();
    }

    /// <summary>kiln.ui hands the listener a press, as keys.cpp does: 1 = keep it from the game.</summary>
    private static byte Send(uint vk, uint scan = 0, uint mods = 0, bool down = true, bool repeat = false, bool text = false, bool captured = false)
    {
        Assert.True(_fn != null, "no key callback was subscribed");
        var k = new UiNative.Key
        {
            Size = (uint)sizeof(UiNative.Key), Vk = vk, Scan = scan, Mods = mods, Down = down ? (byte)1 : (byte)0,
            Repeat = repeat ? (byte)1 : (byte)0, TextInput = text ? (byte)1 : (byte)0, Captured = captured ? (byte)1 : (byte)0,
        };
        return _fn(&k, _user);
    }

    [Fact]
    public void A_v7_kiln_ui_gives_a_listener_keys_and_the_mouse()
    {
        InstallV7(sizeof(UiNative.Table7));
        var got = new List<KeyPress>();
        using var l = KeyInput.Listen(p => { got.Add(p); return p.VirtualKey == 0x05; });
        Assert.True(l.Ok);
        Assert.Null(l.Reason);
        Assert.True(l.Mouse);
        lock (Calls) Assert.Equal(["subscribe_key_ex owner 0 flags 1"], Calls);

        Assert.Equal(1, Send(0x05));                                              // Mouse4: kept
        Assert.Equal(0, Send('K', 0x25, mods: 1 | 4, down: false, repeat: true, text: true, captured: true));
        Assert.Equal(2, got.Count);
        Assert.Equal(new KeyPress(0x05, 0, KeyMods.None, true, false, false, false), got[0]);
        Assert.True(got[0].IsMouse);
        Assert.Equal(new KeyPress('K', 0x25, KeyMods.Ctrl | KeyMods.Alt, false, true, true, true), got[1]);
        Assert.False(got[1].IsMouse);
        Assert.Equal("Ctrl+Alt+K", got[1].Chord.ToString());
    }

    [Fact]
    public void KeyListenFlags_Pad_gives_a_listener_the_controllers_buttons()
    {
        // 0.2.0-dev.30.5 (GitHub issue 119): KILN_UI_KEYS_PAD; a pad press carries the pad's modifiers only
        InstallV7(sizeof(UiNative.Table7));
        var got = new List<KeyPress>();
        using var l = KeyInput.Listen(p => { got.Add(p); return p.IsPad; }, KeyListenFlags.Mouse | KeyListenFlags.Pad);
        Assert.True(l.Ok && l.Mouse && l.Pad);
        lock (Calls) Assert.Equal(["subscribe_key_ex owner 0 flags 3"], Calls);
        Assert.Equal(1, Send(0x20C, mods: 8));  // A with LB held: kept
        Assert.Equal(new KeyPress(0x20C, 0, KeyMods.PadLB, true, false, false, false), got[0]);
        Assert.Equal("Pad:LB+A", got[0].Chord.ToString());
        Assert.Equal(0, Send('K', 0x25, mods: 1 | 8));  // a keyboard key never carries pad bits
        Assert.Equal(KeyMods.Ctrl, got[1].Mods);
    }

    [Fact]
    public void Keyboard_only_asks_kiln_ui_for_no_mouse()
    {
        InstallV7(sizeof(UiNative.Table7));
        using var l = KeyInput.Listen(_ => false, KeyListenFlags.None);
        Assert.True(l.Ok);
        Assert.False(l.Mouse);
        lock (Calls) Assert.Equal(["subscribe_key_ex owner 0 flags 0"], Calls);
        Assert.Throws<ArgumentOutOfRangeException>(() => KeyInput.Listen(_ => false, (KeyListenFlags)16));  // 4 and 8 are Own and Pointer (31.13)
        Assert.Throws<ArgumentNullException>(() => KeyInput.Listen(null!));
    }

    [Fact]
    public void An_older_kiln_ui_gives_keys_only()
    {
        InstallV7(sizeof(UiNative.Table6Scroll));  // a kiln-ui.dll from before subscribe_key_ex
        var n = 0;
        using var l = KeyInput.Listen(_ => { n++; return true; });
        Assert.True(l.Ok);
        Assert.False(l.Mouse);
        lock (Calls) Assert.Equal(["subscribe_key owner 0"], Calls);
        Assert.Equal(1, Send(0x77));
        Assert.Equal(1, n);
    }

    [Fact]
    public void Without_kiln_ui_a_listener_says_why()
    {
        FakeUiTable.Table = null;  // a dedicated server: no kiln.ui
        UiNative.ResetForTests();
        try
        {
            var l = KeyInput.Listen(_ => true);
            Assert.False(l.Ok);
            Assert.False(l.Mouse);
            Assert.Contains("kiln.ui", l.Reason);
            l.Dispose();  // harmless
            Assert.Equal(0, KeyInput.Count);
        }
        finally
        {
            FakeUiTable.Table = _fake;
        }
    }

    [Fact]
    public void A_refused_subscription_is_not_ok()
    {
        InstallV7(sizeof(UiNative.Table7));
        _t7->SubscribeKeyEx = &Refuse;
        var l = KeyInput.Listen(_ => true);
        Assert.False(l.Ok);
        Assert.Contains("refused", l.Reason);
        Assert.Equal(0, KeyInput.Count);
    }

    [Fact]
    public void Dispose_unsubscribes_and_no_press_comes_after()
    {
        InstallV7(sizeof(UiNative.Table7));
        var n = 0;
        var l = KeyInput.Listen(_ => { n++; return true; });
        Assert.Equal(1, Send(0x77));
        l.Dispose();
        l.Dispose();  // twice is fine
        lock (Calls) Assert.Equal(["subscribe_key_ex owner 0 flags 1", "unsubscribe 42"], Calls);
        Assert.Equal(0, Send(0x77));  // a press already in flight in kiln.ui: not delivered, not kept
        Assert.Equal(1, n);
        Assert.Equal(0, KeyInput.Count);
    }

    [Fact]
    public void A_handler_that_throws_keeps_nothing_and_is_logged()
    {
        InstallV7(sizeof(UiNative.Table7));
        using var l = KeyInput.Listen(_ => throw new InvalidOperationException("boom"));
        Assert.Equal(0, Send(0x77));
        lock (_backend.Logs) Assert.Contains(_backend.Logs, s => s.Contains("KeyInput.Listen", StringComparison.Ordinal) && s.Contains("boom", StringComparison.Ordinal));
    }

    [Fact]
    public void A_mods_listener_runs_as_the_mod_and_stops_when_it_unloads()
    {
        InstallV7(sizeof(UiNative.Table7));
        var mod = HostAccess.CreateContext(new ModInfo("listener", "Listener", "1.0.0", "."), 91, ".");
        ModContext? seen = null;
        KeyListener l;
        var previous = HostAccess.Current;
        HostAccess.Current = mod;
        try
        {
            l = KeyInput.Listen(_ => { seen = HostAccess.Current; return true; });
        }
        finally
        {
            HostAccess.Current = previous;
        }
        Assert.True(l.Ok);
        lock (Calls) Assert.Equal(["subscribe_key_ex owner 91 flags 1"], Calls);  // kiln.ui knows whose it is
        Assert.Equal(1, Send(0x77));
        Assert.Same(mod, seen);
        mod.Retire();
        lock (Calls) Assert.Equal(["subscribe_key_ex owner 91 flags 1", "unsubscribe 42"], Calls);
        Assert.Equal(0, KeyInput.Count);
        Assert.Equal(0, Send(0x77));
    }

    [Theory]
    [InlineData(0xDFu, 0x29u, 0xC0u, "Backquote")]  // UK layout: VK_OEM_8 on the key left of 1
    [InlineData(0xDCu, 0x29u, 0xC0u, "Backquote")]  // German layout: VK_OEM_5 there
    [InlineData(0xC0u, 0x28u, 0xDEu, "Quote")]      // UK: the ' key sends the US backquote's code
    [InlineData(0xC0u, 0u, 0xC0u, "Backquote")]     // an injected key without a scan code: its virtual key
    [InlineData((uint)'K', 0x25u, (uint)'K', "K")]  // letters stay virtual keys
    [InlineData(0x100u, 0u, 0x100u, "WheelUp")]
    public void The_press_code_is_the_keys_pages(uint vk, uint scan, uint code, string name)
    {
        Assert.Equal(code, KeyInput.PressCode(vk, scan));
        Assert.Equal(KeyNames.PressCode(vk, scan), KeyInput.PressCode(vk, scan));
        var p = new KeyPress(vk, scan, KeyMods.None, true, false, false, false);
        Assert.Equal(code, p.Code);
        Assert.Equal(name, p.Chord.ToString());
        Assert.True(KeyChord.TryParse(name, out var back));
        Assert.Equal(p.Chord, back);
    }
}
