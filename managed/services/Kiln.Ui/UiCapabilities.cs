// SPDX-License-Identifier: MIT
// Kiln.Ui in the capability registry (Kiln.Capabilities; docs/kiln/services/caps.md). Kiln.Ui's windows never throw for a
// missing capability (their calls draw nothing and return null or false), so they have no row of their own: kiln.ui,
// kiln.render and the client's sigdb features answer. Ui.Values (dev.28) throws by the error rule and answers with
// kiln.ui.values' seven sigdb features, lowered here when its table is missing. Reported at the service's start (the first service tick) and at its first use.
// The tables are published also when single hooks failed (kiln.ui and kiln.render install them at their start, before
// they publish): the features those hooks carry are lowered then. ui.world_outline (Ui.World.Outline, 0.2.0-dev.30.3) is
// lowered here without kiln.render, and as service_older with a v1 kiln.render table.
namespace Kiln;

internal static unsafe class UiCaps
{
    /// <summary>kiln.ui's table is here (the core's row).</summary>
    public const string Service = "kiln.ui";

    /// <summary>kiln.render's table is here (the core's row).</summary>
    public const string Render = "kiln.render";

    /// <summary>The client's sigdb features kiln.ui serves.</summary>
    public static readonly string[] Features =
    [
        "ui.screens", "ui.main_menu", "ui.menu_page", "ui.menu_page_sound", "ui.system_menu", "ui.crafting", "ui.error_popup",
        "ui.language", "ui.popups",
    ];

    /// <summary>kiln.ui.values' sigdb features (in-core, client only): Ui.Values' rows.</summary>
    public static readonly string[] ValueFeatures =
    [
        "ui.values.detail", "ui.values.slot", "ui.values.held", "ui.values.hud", "ui.values.tooltip_lines",
        "ui.slots.inspect", "ui.gemforge.inspect", "ui.gemforge.sources", "ui.tooltip.lines", "ui.tooltip.values", "ui.slots.click",
        "ui.inventory.panels", "ui.slots.tint", "ui.chest.buttons",
    ];

    internal static readonly ServiceCapRows Rows = new(Service, Report);

    private static void Report()
    {
        if (UiNative.T == null) ServiceCaps.LowerFeatures(Service, Features);
        else if (UiNative.HasGameUi && !ScreensOn())
            // the hook on the game's UI frame failed: native turns screens, both menus and crafting off together
            foreach (var name in (string[])["ui.screens", "ui.main_menu", "ui.system_menu", "ui.crafting"])
                ServiceCaps.LowerOff(name, CapabilityReason.Faulted,
                    "kiln.ui could not hook the game's UI frame (see kiln.log): no game-UI integration");
        // the game's popup panels for Ui.Inventory's marks (0.2.0-dev.31.9, GitHub issue 117)
        if (UiNative.T != null && UiNative.PopupsSlot == null)
            ServiceCaps.LowerOff("ui.popups", CapabilityReason.ServiceOlder,
                "kiln.ui has no popups (older than 0.2.0-dev.31.9): Ui.Inventory marks ignore the game's popups");
        if (UiValuesNative.T == null) ServiceCaps.LowerFeatures(UiValuesNative.Service, ValueFeatures);
        else if (UiValuesNative.GemForgeT == null)
            ServiceCaps.LowerOff("ui.gemforge.inspect", CapabilityReason.ServiceOlder,
                "kiln.ui.values has no Gem Forge snapshot table");
        if (UiValuesNative.T != null && UiValuesNative.SourcesT == null)
            ServiceCaps.LowerOff(GemForgeSources.Row, CapabilityReason.ServiceOlder, "kiln.ui.values has no gemforge_sources");
        if (UiValuesNative.T != null && UiValuesNative.TooltipT == null)
            foreach (var name in (string[])[Ui.Tooltip.LinesCapability, Ui.Tooltip.ValuesCapability])
                ServiceCaps.LowerOff(name, CapabilityReason.ServiceOlder, "kiln.ui.values has no native tooltip slots");
        if (UiValuesNative.T != null && UiInventoryNative.T == null)
            ServiceCaps.LowerOff(Ui.Inventory.ClickCapability, CapabilityReason.ServiceOlder, "kiln.ui.inventory is not there (an older kiln-core)");
        if (UiValuesNative.T != null && UiInventoryNative.T2 == null)
            ServiceCaps.LowerOff(Ui.Inventory.PanelCapability, CapabilityReason.ServiceOlder,
                "kiln.ui.inventory has no screen_view (v2: an older kiln-core)");
        // Ui.AmmoList (0.2.0-dev.30.6): kiln-tools serves kiln.ui.ammo; without its table neither row can work
        if (UiAmmoNative.T == null)
            ServiceCaps.LowerFeatures(UiAmmoNative.Service, Ui.AmmoList.ListCapability, Ui.AmmoList.PickCapability);
        if (UiItemDetailNative.T == null)
            ServiceCaps.LowerOff(Ui.Values.ItemDetailCapability, CapabilityReason.ServiceOlder, "kiln.ui.item_detail is not available");
        if (UiNativeChest.T == null)
            ServiceCaps.LowerOff(Ui.Inventory.ChestButtonCapability, CapabilityReason.ServiceOlder, "kiln.ui.chest is not available");
        if (UiNativeTints.T == null)
            ServiceCaps.LowerOff(Ui.Inventory.TintCapability, CapabilityReason.ServiceOlder, "kiln.ui.tints is not available");
        // Collected lines have one production consumer: the overlay's Item details window.
        ServiceCaps.LowerFeatures(Service, "ui.values.tooltip_lines");
        if (!RenderNative.Available) ServiceCaps.LowerFeatures(Render, "render.camera", Ui.World.OutlineCapability);
        else
        {
            if (RenderNative.ReadStatus() is { Camera: 0 })
                ServiceCaps.LowerOff("render.camera", CapabilityReason.Faulted, "kiln.render's camera hook is off (see kiln.log)");
            // outlines (0.2.0-dev.30.3) are kiln.render v2's; kiln.render itself lowers the feature when its hooks fail
            if (RenderNative.T2 == null)
                ServiceCaps.LowerOff(Ui.World.OutlineCapability, CapabilityReason.ServiceOlder,
                    "kiln.render has no outlines (v1: an older kiln-core)");
        }
        ReportThroughWalls();
    }

    /// <summary>ui.world_outline.through_walls (0.2.0-dev.30.5): Kiln.Ui's own row for OutlineOptions.ThroughWalls. Kiln.Ui draws
    /// the silhouette, so it needs the outline itself, kiln.render v3 (the entity's position), the camera and kiln.ui's
    /// overlay.</summary>
    internal static (bool Ok, CapabilityReason Reason, string Why) ThroughWallsState()
    {
        if (UiNative.T == null) return (false, CapabilityReason.ServiceMissing, "kiln.ui (Kiln's overlay) is not here");
        if (UiNative.T->Status != null && UiNative.DrawingOffReason is { } drawingOff)
            return (false, CapabilityReason.Faulted, drawingOff);
        if (!RenderNative.Available) return (false, CapabilityReason.ServiceMissing, "kiln.render is not here");
        if (!Capabilities.Is(Ui.World.OutlineCapability))
            return (false, CapabilityReason.ServiceOlder, $"outlines are off: {Capabilities.Why(Ui.World.OutlineCapability)}");
        if (!RenderNative.HasThroughWalls)
            return (false, CapabilityReason.ServiceOlder, "kiln.render has no through-walls positions (v2: an older kiln-ui.dll)");
        if (!Capabilities.Is("render.camera"))
            return (false, CapabilityReason.ServiceOlder, $"the camera is not known: {Capabilities.Why("render.camera")}");
        return (true, CapabilityReason.None, "");
    }

    private static void ReportThroughWalls()
    {
        var (ok, reason, why) = ThroughWallsState();
        Capabilities.Report(Ui.World.ThroughWallsCapability, ok ? CapabilityState.Available : CapabilityState.Unavailable, reason, why);
    }

    // kiln.ui's screen answers "unavailable" exactly when the game-UI integration is off (feature or frame hook)
    private static bool ScreensOn()
    {
        UiNative.Screen s = default;
        s.Size = (uint)sizeof(UiNative.Screen);
        return UiNative.T->GetScreen != null && UiNative.T->GetScreen(&s) == UiNative.Ok;
    }
}
