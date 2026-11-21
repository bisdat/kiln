// SPDX-License-Identifier: MIT
// What package 09 (UI blocks) binds "stats.<mod>/<id>" (and /level, /revision, /available) to: definition metadata
// known before any handshake, the client's immutable synced snapshot with its nonce, and the exact formatter. Internal
// (Kiln.Blocks sees it); capability stats.ui. A binding ".available" opens the stats channel (it always is open on a
// Kiln client: kiln:mod-values starts with Kiln.ModValues).
namespace Kiln;

internal static class ModValuesStatSource
{
    /// <summary>The definition of a key: described or active here, else as the stream sent it.</summary>
    internal static ModValueDefinition? Metadata(ModValueKey key) => ModValues.Definition(key);

    /// <summary>The client's whole copy (immutable; compare <see cref="SyncedState.Nonce"/> and Seq to see a change).</summary>
    internal static SyncedState Snapshot => ModValuesClient.State;

    /// <summary>A value for a binding of <paramref name="reader"/> (the block's mod): the read policy applies.</summary>
    internal static ModValueReadResult Read(string? reader, ModValueKey key) => ModValuesClient.Read(Snapshot, reader, key);

    /// <summary>The value as text in the definition's format and the game's language; null when not readable.</summary>
    internal static string? Text(string? reader, ModValueKey key) =>
        Read(reader, key) is { Ok: true, Value: { } v } && Metadata(key) is { } d ? ModValueFormat.Format(v.Value, v.Scale, d.NumberFormat.ToString(), GameLanguage.Current) : null;
}
