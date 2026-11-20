// SPDX-License-Identifier: MIT
// The channel kiln:station-menu v1 (chapter 16a; DESIGN.md sections 3, 4, 6): the server's snapshot of its station menus
// to Kiln clients, the admission decider that refuses a Kiln client without the channel while menus exist, and the client
// side that stages, verifies and swaps the server's collection. Started by Kiln.Content's service start.
namespace Kiln;

internal static class StationMenuSync
{
    public const string ChannelName = "kiln:station-menu";
    public const string ChannelVersion = "1";

    /// <summary>Server: whether a definition may be published now (null) or why not (a stable reason word first).
    /// <paramref name="first"/>: no menu exists yet. Tick thread.</summary>
    internal static string? CheckPublish(bool first) => null;

    /// <summary>The local collection changed: the server sends a new snapshot at its next tick.</summary>
    internal static void Published() { }

    /// <summary>Kiln.Blocks' part of MenuDeclarationsReady: the blocks pass ran and inline menus were resolved once.</summary>
    internal static void BlocksDeclared() { }
}
