// SPDX-License-Identifier: MIT
// Kiln.ModValues in the capability registry (docs/kiln/services/caps.md). Managed rows only: the ledger needs no
// signature (chapter 05b section 3). Reported at the service's start (ModValuesStartup, the first service tick) and
// every 25 steps (a hosted world starts and ends).
namespace Kiln;

internal static class ModValuesCaps
{
    /// <summary>Describe: definitions are known on this side (both sides).</summary>
    public const string Definitions = "stats.definitions";
    /// <summary>Define, Read, Add, Spend, Set, Reset: the server's ledger (a dedicated server, a hosted world).</summary>
    public const string Own = "stats.own";
    /// <summary>Synced: the owning player's copy over kiln:mod-values (client).</summary>
    public const string Sync = "stats.sync";
    /// <summary>Format and the stat source package 09 binds: showing values in Kiln UI (client).</summary>
    public const string Ui = "stats.ui";

    private const string Saves = "kiln.saves", Net = "kiln.net", KilnUi = "kiln.ui";

    internal static readonly ServiceCapRows Rows = new("kiln.mod-values", Report, everySteps: 25);

    internal static bool ServerRole => Mods.Side == ModSide.Server || Client.Available && Client.IsHosting;

    private static void Report()
    {
        ServiceCaps.Report(Definitions, CapabilityState.Available);
        if (Mods.Side == ModSide.Server)
        {
            ServiceCaps.Static(Own, Capabilities.Is(Saves), CapabilityReason.ServiceMissing, "the ledger is saved by kiln.saves", Saves);
            ServiceCaps.Report(Sync, CapabilityState.Unavailable, CapabilityReason.WrongSide, "the synced copy is the client's");
            ServiceCaps.Report(Ui, CapabilityState.Unavailable, CapabilityReason.WrongSide, "values are shown by clients");
            return;
        }
        ServiceCaps.Session(Own, ServerRole && Capabilities.Is(Saves), "the ledger runs where the world runs: a hosted world", Saves);
        ServiceCaps.Static(Sync, Network.Available, CapabilityReason.ServiceMissing, "the copy comes over kiln.net", Net);
        ServiceCaps.Static(Ui, Capabilities.Is(KilnUi), CapabilityReason.ServiceMissing, "values are shown in Kiln UI", KilnUi);
    }

    /// <summary>Throws <see cref="KilnUnavailableException"/> naming <paramref name="capability"/> unless it is on now.</summary>
    public static void Require(string capability)
    {
        Rows.Ensure();
        if (Capabilities.Is(capability)) return;
        Report();
        if (Capabilities.Is(capability)) return;
        throw ServiceCaps.Unavailable(capability);
    }
}

/// <summary>Started by Kiln (ServiceTick.AutoStart) at its first service tick: the rows, the ledger, the channel
/// kiln:mod-values and the native backend are there before a mod or a native caller asks.</summary>
internal static class ModValuesStartup
{
    static ModValuesStartup() => ModValuesService.EnsureStarted();
}
