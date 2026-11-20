// SPDX-License-Identifier: MIT
// Station menus in the capability registry (docs/kiln/services/caps.md "Rows added in dev.31"): stationmenus.store (both
// sides: kiln.content's kiln.stationmenus table), stationmenus.render (game client: the crafting-menu hook and the layout
// check, native), stationmenus.sync.v1 (the channel kiln:station-menu v1 is open; on a server also the hold gate).
namespace Kiln;

internal static class StationMenuCaps
{
    internal static readonly ServiceCapRows Rows = new(StationMenuNative.Service, Report, everySteps: 125);

    private static void Report()
    {
        var store = StationMenuStore.Available;
        ServiceCaps.Static(StationMenuStore.StoreRow, store, CapabilityReason.ServiceMissing,
            "kiln.content has no station-menu store (kiln.stationmenus; update Kiln)");
        if (!Kiln.Client.IsClient)
            ServiceCaps.Report(StationMenuStore.RenderRow, CapabilityState.Unavailable, CapabilityReason.WrongSide, "a dedicated server draws no menus");
        else if (StationMenuStore.Backend.Info() is { } info)
        {
            if (info.Render == 1) ServiceCaps.Report(StationMenuStore.RenderRow, CapabilityState.Available);
            else
                ServiceCaps.Report(StationMenuStore.RenderRow, CapabilityState.Unavailable,
                    Enum.IsDefined(typeof(CapabilityReason), (int)info.RenderReason) && info.RenderReason != 0 ? (CapabilityReason)info.RenderReason : CapabilityReason.SymbolMissing,
                    info.DetailText.Length > 0 ? info.DetailText : "the crafting-menu hook (content.recipe_menu) is off");
        }
        else ServiceCaps.Report(StationMenuStore.RenderRow, CapabilityState.Unavailable, CapabilityReason.ServiceMissing, "kiln.stationmenus is not loaded");
        var server = StationMenuSync.ServerRole && !Kiln.Client.IsClient;
        var sync = StationMenuSync.ChannelOpen && (!server || !NetNative.Available || NetAdmission.CanHoldKilnPeers);
        ServiceCaps.Static(StationMenuStore.SyncRow, sync, store ? CapabilityReason.SymbolMissing : CapabilityReason.ServiceMissing,
            !store ? "kiln.stationmenus is not loaded"
            : !StationMenuSync.ChannelOpen ? "the channel kiln:station-menu is closed (a game client opens it only while it can draw menus)"
            : "kiln.net cannot hold joining Kiln clients on this build (net.gate.hold_all)");
    }
}
