// SPDX-License-Identifier: MIT
// Chest gems in the Gem Forge's GEMS grid, the client's half (0.2.0-dev.29.15, GitHub issue 61): the rows a server
// offered, handed to kiln.ui.values' gemforge_sources (kiln_ui_values.h). Internal: Kiln.Abilities' gem sources (the
// channel "kiln:gemsources") call this with what Kiln's server half sent. Picture only; the server decides every action.
namespace Kiln;

/// <summary>One offered chest gem as the client lists it.</summary>
internal readonly record struct GemForgeSourceRow(uint Page, byte Slot, uint Item, uint Pide, uint Level, byte Rarity, byte Group,
    ulong Token, long ExpiresMs);

/// <summary>What one payer group of the server holds of one material.</summary>
internal readonly record struct GemForgeSourcePayer(byte Group, bool PlayerFirst, uint Item, uint Count);

internal static unsafe class GemForgeSources
{
    /// <summary>The capability row (client).</summary>
    internal const string Row = "ui.gemforge.sources";

    /// <summary>This core's kiln.ui.values has gemforge_sources (a game client's).</summary>
    internal static bool TableHere => UiValuesNative.SourcesT != null;

    /// <summary>Replaces Kiln's rows (none: removes them). Never throws: (false, reason) on a server, an older core, a
    /// row that does not check out, or when ui.gemforge.sources is off (its hooks then stay out or inert).</summary>
    internal static (bool Ok, string? Reason) Set(IReadOnlyList<GemForgeSourceRow> rows, IReadOnlyList<GemForgeSourcePayer> payers)
    {
        var t = UiValuesNative.SourcesT;
        if (t == null)
            return (false, UiValuesNative.T == null ? "the Gem Forge lists are client only" : "this Kiln core's kiln.ui.values has no gemforge_sources");
        if (rows.Count > 256 || payers.Count > 64) return (false, "more than 256 rows or 64 payer rows");
        var r = new UiValuesNative.GemSourceRow[rows.Count];
        for (var i = 0; i < rows.Count; i++)
        {
            var x = rows[i];
            r[i] = new UiValuesNative.GemSourceRow
            {
                Size = (uint)sizeof(UiValuesNative.GemSourceRow), Page = x.Page, Slot = x.Slot, Rarity = x.Rarity, Group = x.Group,
                Item = x.Item, Pide = x.Pide, Level = x.Level, Token = x.Token, ExpiresMs = (ulong)Math.Max(0, x.ExpiresMs),
            };
        }
        var p = new UiValuesNative.GemSourcePayer[payers.Count];
        for (var i = 0; i < payers.Count; i++)
            p[i] = new UiValuesNative.GemSourcePayer
            {
                Size = (uint)sizeof(UiValuesNative.GemSourcePayer), Group = payers[i].Group, PlayerFirst = payers[i].PlayerFirst ? (byte)1 : (byte)0,
                Item = payers[i].Item, Count = payers[i].Count,
            };
        int rc;
        fixed (UiValuesNative.GemSourceRow* rp = r)
        fixed (UiValuesNative.GemSourcePayer* pp = p)
            rc = t->GemForgeSources(UiValueBridge.OwnerOf(null), rp, (uint)r.Length, pp, (uint)p.Length);
        return rc switch
        {
            UiValuesNative.Ok => (true, null),
            UiValuesNative.InvalidArgument => (false, "kiln.ui.values refused a row"),
            UiValuesNative.Unavailable => (false, $"{Row} is off on this client: {Capabilities.Why(Row)}"),
            _ => (false, $"kiln.ui.values: gemforge_sources failed ({rc})"),
        };
    }

    /// <summary>The materials the forge's price test asked about that no payer row covers yet (to ask the server for), and
    /// whether the hooks are in; null without the table.</summary>
    internal static (bool Installed, bool Off, uint[] Wanted, string Why)? State()
    {
        var t = UiValuesNative.SourcesT;
        if (t == null) return null;
        UiValuesNative.GemSourcesState s = default;
        s.Size = (uint)sizeof(UiValuesNative.GemSourcesState);
        if (t->GemForgeSourcesState(&s) != UiValuesNative.Ok) return null;
        var wanted = new uint[Math.Min(8, (int)s.WantedCount)];
        for (var i = 0; i < wanted.Length; i++) wanted[i] = s.Wanted[i];
        var n = 0;
        while (n < 128 && s.Why[n] != 0) n++;
        return (s.Installed != 0, s.Off != 0, wanted, System.Text.Encoding.UTF8.GetString(s.Why, n));
    }
}
