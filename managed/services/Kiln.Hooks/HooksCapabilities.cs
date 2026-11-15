// SPDX-License-Identifier: MIT
// Kiln.Hooks in the capability registry (Kiln.Capabilities; docs/kiln/services/caps.md): the core's row for the
// kiln.hooks table is the only one. Asking for the table makes Services.Get record why it is missing.
namespace Kiln;

internal static unsafe class HooksCaps
{
    /// <summary>The native table is here (the core's row).</summary>
    public const string Service = HooksNative.Name;

    /// <summary>Typed links (dev.28): a sigdb feature without symbols the core reports itself; it is off on a core
    /// whose kiln.hooks table lacks the typed members.</summary>
    public const string Typed = "hooks.typed";

    /// <summary>The row of a described symbol: on when it resolved and its descriptor fits the function's code.</summary>
    public static string Sig(string symbol) => "sig/" + symbol;

    internal static readonly ServiceCapRows Rows = new(Service, ReportCapabilities);

    // HooksNative looks the table up once per process: ask again, so the row says why it is missing under the registry
    // binding of now (the test kit starts each harness with an empty registry).
    private static void ReportCapabilities()
    {
        if (HooksNative.T == null) ServiceCaps.Probe(Service, HooksNative.Version);
        // hooks.typed is the core's sigdb feature; a kiln.hooks table without the typed members (no core, or one older
        // than 0.2.0-dev.28) lowers it with the reason, so it never reads available where a typed link cannot be made
        if (HooksNative.Typed == null)
        {
            var (reason, text) = HooksNative.T == null
                ? ServiceCaps.Cause(CapabilityReason.ServiceMissing, "kiln.hooks is unavailable on this build", Service)
                : (CapabilityReason.ServiceOlder, "this core's kiln.hooks has no typed links (older than 0.2.0-dev.28)");
            ServiceCaps.Lower(Typed, reason, text);
        }
    }
}
