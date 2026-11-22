// SPDX-License-Identifier: MIT
// Kiln.Network in the capability registry (Kiln.Capabilities; docs/kiln/services/caps.md). Reported at the service's
// start (NetworkStartup, the first service tick). kiln.net has only the core's row; its sigdb features are lowered when
// the table is missing. players.kick and players.bans are reported by PlayerGate (they need the kick and the
// connection records, and a process that serves clients). net.steam_messages is kiln-net.dll's (and the core's) row:
// reported here only where neither can (an older kiln.net without the Steam members, or no Kiln core).
namespace Kiln;

internal static class NetworkCaps
{
    /// <summary>kiln.net's table is here (the core's row): the transport.</summary>
    public const string Service = "kiln.net";

    /// <summary>The sigdb features kiln.net serves.</summary>
    public static readonly string[] Features =
        ["net.client_guard", "net.disconnect", "net.gate", "net.kick", "net.players", "net.transport", "net.withhold"];

    internal static readonly ServiceCapRows Rows = new(Service, Report);

    private static void Report()
    {
        if (!NetNative.Available) ServiceCaps.LowerFeatures(Service, Features);
        SteamMessages.ReportRow();
    }
}
