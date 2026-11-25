// SPDX-License-Identifier: MIT
// A read-only view of the game controllers (since 0.2.0-dev.31.13, mod API 0.2.22; XHL's Free Climb reads the triggers):
// kiln.ui.input's pad_state (native/core/abi/services/kiln_ui_input.h, native/services/ui/pad_view.h). For presses use
// KeyInput.Listen with KeyListenFlags.Pad, or KeyBindings with "Pad:" chords.
using System.Numerics;

namespace Kiln;

/// <summary>A game controller's buttons as bits (<see cref="PadState.Buttons"/>): bit <c>code - 0x200</c> of the
/// <see cref="KeyChord"/> codes of "Pad:" chords (kiln_ui.h KILN_UI_VK_PAD_*), the triggers as <see cref="LT"/> and
/// <see cref="RT"/> from a pull of 30 of 255 (when a "Pad:LT" binding fires). Since 0.2.0-dev.31.13, mod API 0.2.22.</summary>
[Flags]
public enum PadButtons : uint
{
    /// <summary>Nothing held.</summary>
    None = 0,
    /// <summary>The d-pad's up ("Pad:Up").</summary>
    Up = 1u << 0,
    /// <summary>The d-pad's down.</summary>
    Down = 1u << 1,
    /// <summary>The d-pad's left.</summary>
    Left = 1u << 2,
    /// <summary>The d-pad's right.</summary>
    Right = 1u << 3,
    /// <summary>Start (Menu).</summary>
    Start = 1u << 4,
    /// <summary>Back (View).</summary>
    Back = 1u << 5,
    /// <summary>The left stick pressed (L3, "Pad:LS").</summary>
    LeftStick = 1u << 6,
    /// <summary>The right stick pressed (R3, "Pad:RS").</summary>
    RightStick = 1u << 7,
    /// <summary>The left shoulder button.</summary>
    LB = 1u << 8,
    /// <summary>The right shoulder button.</summary>
    RB = 1u << 9,
    /// <summary>A.</summary>
    A = 1u << 12,
    /// <summary>B.</summary>
    B = 1u << 13,
    /// <summary>X.</summary>
    X = 1u << 14,
    /// <summary>Y.</summary>
    Y = 1u << 15,
    /// <summary>The left trigger pulled (from 30 of 255).</summary>
    LT = 1u << 16,
    /// <summary>The right trigger pulled.</summary>
    RT = 1u << 17,
}

/// <summary>
/// One game controller as the game's last poll of it read it (<see cref="PadInput.Read"/>), before Kiln hid a kept or
/// owned button from the game. Since 0.2.0-dev.31.13, mod API 0.2.22.
/// </summary>
/// <param name="Controller">The controller, 0 to 3 (XInput's user index).</param>
/// <param name="Connected">The game's last poll of it succeeded.</param>
/// <param name="Buttons">The buttons held.</param>
/// <param name="LeftStick">The left stick, -1 to 1 each way (right and up positive), XInput's dead zone (7849 of 32767,
/// radially) taken out: (0, 0) inside it, then from its edge to full tilt along the stick's direction.</param>
/// <param name="RightStick">The right stick, likewise (dead zone 8689).</param>
/// <param name="LeftTrigger">The left trigger, 0 to 1: 0 under a pull of 30 of 255, above 0 from there (when
/// <see cref="PadButtons.LT"/> is held).</param>
/// <param name="RightTrigger">The right trigger, likewise.</param>
public readonly record struct PadState(int Controller, bool Connected, PadButtons Buttons, Vector2 LeftStick, Vector2 RightStick,
    float LeftTrigger, float RightTrigger)
{
    /// <summary>The buttons an owning key listener (<see cref="KeyListenFlags.Own"/>) owns now.</summary>
    public PadButtons Owned { get; init; }

    /// <summary>The buttons the game reads as up now although they are held: owned, kept by a key listener or binding, or
    /// everything while a Kiln screen has the input.</summary>
    public PadButtons Hidden { get; init; }

    /// <summary>XInput's packet number of the poll (it changes when the controller's state does).</summary>
    public uint Packet { get; init; }

    /// <summary>The game's polls of this controller since Kiln started (0: never polled; the state is then empty).</summary>
    public ulong Polls { get; init; }

    /// <summary>When the game last polled it, in milliseconds since the PC started (GetTickCount64); 0 before any poll.</summary>
    public ulong PolledAtMs { get; init; }

    /// <summary>XInput's own stick values (-32768 to 32767), before the dead zone.</summary>
    public (short LeftX, short LeftY, short RightX, short RightY) RawSticks { get; init; }

    /// <summary>XInput's own trigger values (0 to 255).</summary>
    public (byte Left, byte Right) RawTriggers { get; init; }

    /// <summary>Every one of <paramref name="buttons"/> is held (false for <see cref="PadButtons.None"/>).</summary>
    public bool IsDown(PadButtons buttons) => buttons != PadButtons.None && (Buttons & buttons) == buttons;
}

/// <summary>
/// The game controllers, read only: what the game's last poll of each read (the game polls XInput every frame), for a mod
/// that follows the sticks or the triggers rather than presses. It never changes what the game reads. Controllers are
/// XInput's (an Xbox pad, or any pad Steam Input presents as one); a pad the game reads through DirectInput is not seen.
/// Game client only. Since 0.2.0-dev.31.13, mod API 0.2.22.
/// </summary>
/// <example><code>
/// var pad = PadInput.Read();
/// if (pad.Connected &amp;&amp; pad.RightTrigger &gt; 0.5f) Climb(pad.LeftStick);
/// </code></example>
[ClientOnly]
public static unsafe class PadInput
{
    /// <summary>How many controllers XInput has: 0 to 3.</summary>
    public const int Controllers = 4;

    /// <summary>True where Kiln reads the controllers: a game client whose Kiln has kiln.ui.input and saw the game load
    /// XInput. Any thread.</summary>
    public static bool Available
    {
        get
        {
            var t = UiNative.Input;
            if (t == null) return false;
            UiNative.PadState s = default;
            s.Size = (uint)sizeof(UiNative.PadState);
            return t->PadStateFn(0, &s) == UiNative.Ok;
        }
    }

    /// <summary>Controller <paramref name="controller"/> (0 to 3) as the game's last poll read it; not
    /// <see cref="PadState.Connected"/> before a poll, when it is unplugged, and where Kiln does not read controllers (see
    /// <see cref="Available"/>). Any thread.</summary>
    /// <exception cref="ArgumentOutOfRangeException"><paramref name="controller"/> is not 0 to 3.</exception>
    public static PadState Read(int controller = 0)
    {
        ArgumentOutOfRangeException.ThrowIfNegative(controller);
        ArgumentOutOfRangeException.ThrowIfGreaterThan(controller, Controllers - 1);
        var t = UiNative.Input;
        UiNative.PadState s = default;
        s.Size = (uint)sizeof(UiNative.PadState);
        var rc = t == null ? -1 : t->PadStateFn((uint)controller, &s);
        if (t == null || (rc != UiNative.Ok && s.Size != (uint)sizeof(UiNative.PadState))) return new PadState(controller, false, 0, default, default, 0, 0);
        return Of(s);
    }

    internal static PadState Of(in UiNative.PadState s) =>
        new((int)s.Controller, s.Connected != 0, (PadButtons)s.Buttons, new Vector2(s.LeftX, s.LeftY), new Vector2(s.RightX, s.RightY),
            s.LeftTrigger, s.RightTrigger)
        {
            Owned = (PadButtons)s.Owned,
            Hidden = (PadButtons)s.Hidden,
            Packet = s.Packet,
            Polls = s.Polls,
            PolledAtMs = s.PollMs,
            RawSticks = (s.RawLeftX, s.RawLeftY, s.RawRightX, s.RawRightY),
            RawTriggers = (s.RawLeftTrigger, s.RawRightTrigger),
        };
}
