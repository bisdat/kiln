// SPDX-License-Identifier: MIT
// Key and mouse presses for mods (since 0.2.0-dev.29.19): kiln.ui's key subscription (subscribe_key_ex, kiln_ui.h) as public
// API, for a mod that reads presses itself: its own "press a key" field, a key it handles by state rather than as an action, a
// shortcut layer of its own. KeyBindings stays the way to give an action a key the player can remap on the Keys page.
using System.Collections.Concurrent;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace Kiln;

/// <summary>One press or release of a key or a mouse input, as Kiln's overlay got it from the game's window (before the
/// game sees it). Since 0.2.0-dev.29.19.</summary>
/// <param name="VirtualKey">The Windows virtual-key code; for the mouse (<see cref="KeyListenFlags.Mouse"/>) 0x04 middle,
/// 0x05 Mouse4, 0x06 Mouse5, and 0x100 to 0x103 for a notch of the wheel up, down, left, right (the codes
/// <see cref="KeyChord.VirtualKey"/> uses).</param>
/// <param name="ScanCode">The key's scan code (0x100 set for an extended key); 0 for the mouse and for a key sent without
/// one.</param>
/// <param name="Mods">Ctrl, Shift and Alt held at the time.</param>
/// <param name="Down">A press (true) or a release. A wheel notch is a press with no release.</param>
/// <param name="Repeat">The auto-repeat of a held key.</param>
/// <param name="TextInput">A Kiln text field has the keyboard (the player is typing in Kiln's overlay).</param>
/// <param name="Captured">Kiln's overlay has the input now (the Kiln menu or a Kiln screen is open).</param>
public readonly record struct KeyPress(uint VirtualKey, uint ScanCode, KeyMods Mods, bool Down, bool Repeat, bool TextInput, bool Captured)
{
    /// <summary>The code a binding stores for this press (<see cref="KeyInput.PressCode"/>): a punctuation key is the key at
    /// that place of a US keyboard, found by its scan code on any layout; every other press is its virtual key.</summary>
    public uint Code => KeyNames.PressCode(VirtualKey, ScanCode);

    /// <summary>The press as the Kiln menu's Keys page saves it: <see cref="Code"/> with <see cref="Mods"/>. Its
    /// <see cref="KeyChord.ToString"/> is the name keybinds.json stores and <see cref="KeyBindings.Rebind(string, KeyChord)"/>
    /// takes.</summary>
    public KeyChord Chord => new(Code, Mods);

    /// <summary>A mouse input: the middle or a side button, or a notch of the wheel.</summary>
    public bool IsMouse => MouseKeys.IsButton(VirtualKey) || MouseKeys.IsWheel(VirtualKey);

    /// <summary>A game controller's button (<see cref="KeyListenFlags.Pad"/>). Since 0.2.0-dev.30.5.</summary>
    public bool IsPad => PadKeys.IsPad(VirtualKey);
}

/// <summary>What a key listener gets besides keyboard keys. Since 0.2.0-dev.29.19.</summary>
[Flags]
public enum KeyListenFlags
{
    /// <summary>Keyboard keys only.</summary>
    None = 0,

    /// <summary>The middle and side mouse buttons and the wheel's notches too (never the left and right buttons). Needs the
    /// kiln-ui.dll of the same release; next to an older one the listener gets keys only (<see cref="KeyListener.Mouse"/>
    /// false).</summary>
    Mouse = 1,

    /// <summary>A game controller's buttons too (codes 0x200 and up, <see cref="KeyPress.IsPad"/>; their
    /// <see cref="KeyPress.Mods"/> are the pad's shoulder buttons and triggers held, <see cref="KeyMods.PadLB"/> ..
    /// <see cref="KeyMods.PadRT"/>). They come on the thread the game reads the pad from, at the poll that sees the
    /// change; true for a press keeps that button from the game until it is let go. Needs the kiln-ui.dll of the same
    /// release (<see cref="KeyListener.Pad"/> false next to an older one). Since 0.2.0-dev.30.5 (mod API 0.2.14).</summary>
    Pad = 2,
}

/// <summary>A listener made by <see cref="KeyInput.Listen"/>. Dispose it to stop; a mod's listeners also stop when the mod
/// unloads. Since 0.2.0-dev.29.19.</summary>
public sealed class KeyListener : IDisposable, IKilnResult
{
    internal KeyListener(bool ok, string? reason, bool mouse, ModContext? owner, Func<KeyPress, bool>? onKey, bool pad = false)
    {
        Pad = pad;
        Ok = ok;
        Reason = reason;
        Mouse = mouse;
        Owner = owner;
        OnKey = onKey;
    }

    /// <summary>True when the listener gets presses.</summary>
    public bool Ok { get; }

    /// <summary>Why it gets none (no Kiln overlay here: a dedicated server, or kiln-ui.dll did not load); null when
    /// <see cref="Ok"/>.</summary>
    public string? Reason { get; }

    /// <summary>It gets the mouse inputs of <see cref="KeyListenFlags.Mouse"/> as well.</summary>
    public bool Mouse { get; }

    /// <summary>It gets a game controller's buttons (<see cref="KeyListenFlags.Pad"/>) as well. Since 0.2.0-dev.30.5.</summary>
    public bool Pad { get; }

    internal ModContext? Owner { get; }
    internal Func<KeyPress, bool>? OnKey { get; }
    internal long Id;
    internal ulong Handle;

    /// <summary>Stops the listener: once it returns, no new press reaches it. Any thread.</summary>
    public void Dispose() => KeyInput.Remove(this);
}

/// <summary>Key and mouse presses as they come, for a mod that reads them itself (its own "press a key" field, a key it
/// handles by state, a shortcut layer of its own). For an action with a key the player can remap, use
/// <see cref="KeyBindings.Register"/>. Game client only. Since 0.2.0-dev.29.19.</summary>
[ClientOnly]
public static unsafe class KeyInput
{
    private static readonly ConcurrentDictionary<long, KeyListener> Listeners = new();
    private static long _next;

    /// <summary>Calls <paramref name="onKey"/> for every key press and release Kiln's overlay gets, and with
    /// <see cref="KeyListenFlags.Mouse"/> (the default) for the middle and side mouse buttons and the wheel's notches. It runs
    /// on the game's window thread while the message is handled, before the game gets the press: keep it short. Return true
    /// to keep the press from the game; false lets it go on. Every listener and Kiln's own key bindings get every press
    /// whatever another returned (kiln.ui keeps a press from the game when any of them asked). Every press comes, also while a Kiln screen, the Kiln menu or a Kiln text field has the input
    /// (<see cref="KeyPress.Captured"/>, <see cref="KeyPress.TextInput"/> say so): a listener that only wants play skips
    /// those. A handler that throws is logged under its mod and keeps nothing. The listener belongs to the calling mod and
    /// stops when it unloads. Not <see cref="KeyListener.Ok"/> where there is no Kiln overlay (a dedicated server). Any
    /// thread.</summary>
    public static KeyListener Listen(Func<KeyPress, bool> onKey, KeyListenFlags flags = KeyListenFlags.Mouse)
    {
        ArgumentNullException.ThrowIfNull(onKey);
        if ((flags & ~(KeyListenFlags.Mouse | KeyListenFlags.Pad)) != 0) throw new ArgumentOutOfRangeException(nameof(flags), flags, "unknown key listener flags");
        var owner = UiModCalls.Caller;
        UiModCalls.RequireAlive(owner);
        var t = UiNative.T;
        if (t == null) return new KeyListener(false, "no Kiln overlay here (kiln.ui is not loaded: a dedicated server, or kiln-ui.dll did not start)", false, owner, null);
        var ex = UiNative.SubscribeKeyExSlot;
        var mouse = ex != null && (flags & KeyListenFlags.Mouse) != 0;
        var pad = ex != null && (flags & KeyListenFlags.Pad) != 0;
        var id = Interlocked.Increment(ref _next);
        ulong handle = 0;
        var user = (void*)(nint)id;
        var native = (mouse ? UiNative.KeysMouse : 0u) | (pad ? UiNative.KeysPad : 0u);
        KeyListener l = null!;
        int rc;
        // registered before the subscription goes in (a press may come at once); a kiln-ui.dll older than 0.2.0-dev.30.5
        // refuses KeysPad: then without the pad
        for (var attempt = 0; ; attempt++)
        {
            l = new KeyListener(true, null, mouse, owner, onKey, pad) { Id = id };
            Listeners[id] = l;
            rc = ex != null
                ? ex(UiModCalls.OwnerId(owner), &OnNativeKey, user, native, &handle)
                : t->SubscribeKey(UiModCalls.OwnerId(owner), &OnNativeKey, user, &handle);
            if (rc == UiNative.Ok || !pad || attempt > 0) break;
            pad = false;
            native &= ~UiNative.KeysPad;
        }
        if (rc != UiNative.Ok)
        {
            Listeners.TryRemove(l.Id, out _);
            return new KeyListener(false, $"kiln.ui refused the key subscription ({rc})", false, owner, null);
        }
        lock (l) l.Handle = handle;
        UiModCalls.Watch(owner);
        // the mod began to unload while this ran: its retirement may have missed this listener
        if (owner is { Alive: false }) Remove(l);
        return l;
    }

    /// <summary>The code a binding stores for a press of <paramref name="virtualKey"/> with <paramref name="scanCode"/>, as the
    /// Kiln menu's Keys page saves it: a punctuation key is the key at that place of a US keyboard, found by its scan code on
    /// any layout (the key left of 1 is <c>Backquote</c>, 0xC0, also on a UK, German or French layout); every other press,
    /// and a press without a scan code, is its virtual key. <c>new KeyChord(code, mods).ToString()</c> names it. Any
    /// thread.</summary>
    public static uint PressCode(uint virtualKey, uint scanCode) => KeyNames.PressCode(virtualKey, scanCode);

    internal static void Remove(KeyListener l)
    {
        if (!Listeners.TryRemove(l.Id, out _)) return;
        ulong handle;
        lock (l)
        {
            handle = l.Handle;
            l.Handle = 0;
        }
        var t = UiNative.T;
        // From inside a kiln.ui callback (a listener disposing itself) kiln.ui may answer KILN_ERR_WRONG_THREAD instead of
        // waiting: the subscription is already inactive there, and the listener is out of the table, so nothing more comes.
        if (handle != 0 && t != null) t->Unsubscribe(handle);
    }

    internal static void RemoveOwnedBy(ModContext mod)
    {
        foreach (var l in Listeners.Values)
            if (ReferenceEquals(l.Owner, mod)) Remove(l);
    }

    internal static int Count => Listeners.Count;

    /// <summary>The press as a listener sees it (and what kiln.ui is told: 1 keeps it).</summary>
    internal static bool Deliver(long id, in UiNative.Key k)
    {
        if (!Listeners.TryGetValue(id, out var l) || l.OnKey is not { } fn) return false;
        if (l.Owner is { Alive: false }) return false;
        var press = new KeyPress(k.Vk, k.Scan, (KeyMods)(k.Mods & (PadKeys.IsPad(k.Vk) ? (uint)PadKeys.Mods : 7u)), k.Down != 0, k.Repeat != 0,
            k.TextInput != 0, k.Captured != 0);
        var keep = false;
        UiModCalls.Invoke(l.Owner, p => keep = fn(p), press, "a key listener (KeyInput.Listen)");
        return keep;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static byte OnNativeKey(UiNative.Key* k, void* user)
    {
        try
        {
            return k != null && Deliver((long)(nint)user, *k) ? (byte)1 : (byte)0;
        }
        catch (Exception e)
        {
            UiEngine.Log(LogLevel.Error, $"key listener: {e}");
            return 0;
        }
    }

    internal static void ResetForTests() => Listeners.Clear();
}
