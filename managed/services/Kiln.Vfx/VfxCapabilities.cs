// SPDX-License-Identifier: MIT
// Kiln.Vfx in the capability registry (Kiln.Capabilities; docs/kiln/services/caps.md). In the game client the sigdb
// features and the kiln.vfx row answer for playback. On the server side nothing is played here: calls are relayed to
// Kiln clients, and vfx.relay and screen.relay are what Vfx.Available says there. Reported at the service's start (the first service
// tick) and refreshed on the service tick, which runs on the server world's thread.
// vfx.budget and blocks.vfx_registration (since 0.2.0-dev.29) are kiln.vfx's own rows, reported with its table; where that table is
// missing (a dedicated server) or older, nobody would report them, so they are reported here, off, with the reason.
namespace Kiln;

internal static class VfxCaps
{
    /// <summary>Kiln.Vfx's session row: a VFX played on the server side reaches someone (a Kiln client with the relay
    /// channel, or this host's own client). What <see cref="Vfx.Available"/> says on the server side.</summary>
    public const string RelayRow = "vfx.relay";

    /// <summary>Kiln.Vfx's row for screen effects on the server side: the relay channel is open
    /// (<see cref="ScreenEffects.Available"/> there).</summary>
    public const string ScreenRelayRow = "screen.relay";

    /// <summary>kiln.vfx's row of <see cref="Vfx.ResourceBudget"/> (native, since 0.2.0-dev.29).</summary>
    public const string BudgetRow = "vfx.budget";

    /// <summary>kiln.vfx's row of the result-bearing VFX definitions: Kiln.Blocks' <c>Blocks.Define(name, VfxBlock, options)</c> and
    /// <c>Blocks.GetVfxRegistration</c> (native, since 0.2.0-dev.29).</summary>
    public const string RegistrationRow = "blocks.vfx_registration";

    private const string Net = "kiln.net";

    internal static readonly ServiceCapRows Rows = new("kiln.vfx", Report, everySteps: 25);

    private static void Report()
    {
        Relay(RelayRow, VfxRelay.ChannelName, VfxRelay.Open, VfxRelay.HasListeners, "vfx.play");
        // screen effects: an open channel is all ScreenEffects.Available asks on the server side
        Relay(ScreenRelayRow, ScreenRelay.ChannelName, ScreenRelay.Open, ScreenRelay.Open, "kiln.screen");
        ReportBudgetRows();
    }

    /// <summary>The budget's two rows where kiln.vfx does not report them: its table is missing (a dedicated server has no VFX:
    /// <c>service-missing</c> with kiln.vfx's reason) or older than 0.2.0-dev.29 (<c>service-older</c>). Kiln.Blocks reports them
    /// with its own rows too, as its members name the second.</summary>
    internal static unsafe void ReportBudgetRows()
    {
        // once per registry binding: a row this binding has (kiln.vfx's own, or this report's) is left as it is, and kiln.vfx's
        // table is not asked for again at every refresh
        var budget = VfxNative.Has(VfxNative.Member.ResourceBudget) == null && Capabilities.Get(BudgetRow).Reason == CapabilityReason.Unknown;
        var registration = (VfxNative.Has(VfxNative.Member.DefineVfxResult) == null || VfxNative.Has(VfxNative.Member.VfxRegistration) == null) &&
                           Capabilities.Get(RegistrationRow).Reason == CapabilityReason.Unknown;
        if (!budget && !registration) return;
        ServiceCaps.Probe(VfxNative.Name);  // kiln.vfx's own row says why it is missing, under this registry binding
        var (reason, text) = VfxNative.T != null
            ? (CapabilityReason.ServiceOlder, "this kiln.vfx has no resource budget or result-bearing VFX definitions (older than 0.2.0-dev.29: update Kiln)")
            : (CapabilityReason.ServiceMissing, "kiln.vfx is not here (a dedicated server has no VFX; in a game client kiln-vfx.dll is missing or off)");
        if (budget) ServiceCaps.Static(BudgetRow, false, reason, text, VfxNative.Name);
        if (registration) ServiceCaps.Static(RegistrationRow, false, reason, text, VfxNative.Name);
    }

    /// <summary>The exception a budget member throws when its row is off; the row is reported first, so it says why.</summary>
    public static KilnUnavailableException Off(string capability, string detail)
    {
        Rows.Ensure();
        ReportBudgetRows();
        return ServiceCaps.Unavailable(capability, detail);
    }

    // One relay row: available while someone takes the relayed calls, pending while the channel is open and nobody
    // listens, unavailable without the channel (kiln.net) and in a game client that only joined a world.
    private static void Relay(string name, string channel, bool open, bool listeners, string clientRow)
    {
        if (!Client.IsDedicatedServer && !Client.IsHosting)
        {
            ServiceCaps.Report(name, CapabilityState.Unavailable, CapabilityReason.WrongSide,
                $"the relay is the server side's; a game client that joined a world plays for itself ({clientRow})");
            return;
        }
        if (listeners)
        {
            ServiceCaps.Report(name, CapabilityState.Available);
            return;
        }
        if (!open)
        {
            ServiceCaps.Probe(Net);  // kiln.net's wrapper asks for its table once: the row must say why under this binding
            ServiceCaps.Static(name, false, CapabilityReason.ServiceMissing, $"the relay channel {channel} is not open", Net);
            return;
        }
        ServiceCaps.Report(name, CapabilityState.Pending, CapabilityReason.Session, $"no Kiln client with the {channel} channel is connected");
    }
}
