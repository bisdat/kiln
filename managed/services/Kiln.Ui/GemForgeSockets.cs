// SPDX-License-Identifier: MIT
// Sockets on other gear, the client's half (gem wave G4): the Gem Forge's WEAPONS list and its GEMS list's carrier rows
// widened from Weapons to more gear categories, through kiln.ui.values' gemforge_sockets (kiln_ui_values.h). Internal:
// Kiln.Abilities' Gems.AllowSockets is the public face and calls this on the client. Picture only; the server decides.
namespace Kiln;

internal static unsafe class GemForgeSockets
{
    /// <summary>The capability row (on the client also kiln.items' socket options; on a server those alone).</summary>
    internal const string Row = "gems.sockets";

    /// <summary>The category bits the native service accepts: ItemCategory 0..13 (never WeaponGem 15).</summary>
    internal const uint Allowed = 0x3fff;

    /// <summary>This core's kiln.ui.values has gemforge_sockets (a game client's).</summary>
    internal static bool TableHere => UiValuesNative.SocketsT != null;

    /// <summary>Sets the calling mod's category mask (bit = 1 &lt;&lt; ItemCategory) for the forge's lists; the lists use
    /// the OR over every mod's mask, Weapons always included. 0 removes the mod's mask; unloading the mod does too. Only
    /// gear that has a socket shows up, and only from the backpack and action bars. Never throws: (false, reason) on a
    /// server, an older Kiln core, a bit outside 0..13, or when gems.sockets is off on this client.</summary>
    internal static (bool Ok, string? Reason) Widen(uint categoryMask)
    {
        if ((categoryMask & ~Allowed) != 0)
            return (false, $"category mask 0x{categoryMask:x} has bits outside the gear categories 0..13");
        var t = UiValuesNative.SocketsT;
        if (t == null)
            return (false, UiValuesNative.T == null
                ? "the Gem Forge lists are client only (kiln.ui.values is not here)"
                : "this Kiln core's kiln.ui.values has no gemforge_sockets");
        var mod = UiModCalls.Caller;
        if (mod is { Alive: false }) return (false, "the mod is unloading");
        var rc = t->GemForgeSockets(UiValueBridge.OwnerOf(mod), categoryMask);
        return rc switch
        {
            UiValuesNative.Ok => (true, null),
            UiValuesNative.InvalidArgument => (false, $"kiln.ui.values refused category mask 0x{categoryMask:x}"),
            UiValuesNative.Unavailable => (false, $"{Row} is off on this client: {Capabilities.Why(Row)}"),
            UiValuesNative.Revoked => (false, "the mod is unloading"),
            _ => (false, $"kiln.ui.values: gemforge_sockets failed ({rc})"),
        };
    }
}
