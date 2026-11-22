// SPDX-License-Identifier: MIT
// The capability rows of the richer UI blocks (dossier 09 §5.2), reported with Kiln.Blocks' rows. Input fields, tables,
// pictures, window state and mods' own data need a player's game with an overlay (kiln.ui); tabs and trees that follow a
// state also need kiln.ui.blocks (an older Kiln: off, the rest still works). The live game values (ui.blocks.world and the
// world.* rows) are reported with kiln.ui.snapshot; ui.command.server by the command sender once kiln:ui-command started
// (before that, here: why it is off).
namespace Kiln;

internal static class UiBlocksCaps
{
    public const string Input = "ui.blocks.input", Tables = "ui.blocks.tables", Tabs = "ui.blocks.tabs", Trees = "ui.blocks.trees",
        Images = "ui.blocks.images", State = BlockUi.StateCapability, Bindings = BlockUi.Bindings.Capability;

    public static void Report()
    {
        bool overlay;
        try { overlay = UiRuntime.Active; }
        catch (Exception e) when (e is TypeLoadException or FileNotFoundException or InvalidOperationException) { overlay = false; }
        var why = BlocksBridge.IsClient ? "no overlay on this game (kiln.ui is missing or off)" : "a dedicated server draws no windows: the blocks are checked and kept";
        var reason = BlocksBridge.IsClient ? CapabilityReason.ServiceMissing : CapabilityReason.WrongSide;
        foreach (var row in (string[])[Input, Tables, Images, State, Bindings])
            ServiceCaps.Report(row, overlay ? CapabilityState.Available : CapabilityState.Unavailable, overlay ? CapabilityReason.None : reason, overlay ? null : why);
        var widgets = overlay && BlocksBridge.HasService("kiln.ui.blocks", 1);
        foreach (var row in (string[])[Tabs, Trees])
            ServiceCaps.Report(row, widgets ? CapabilityState.Available : CapabilityState.Unavailable,
                widgets ? CapabilityReason.None : overlay ? CapabilityReason.ServiceOlder : reason,
                widgets ? null : overlay ? "kiln.ui.blocks is not here (an older Kiln): tabs and trees cannot follow a state" : why);
        UiCommandChannel.ReportBeforeStart();  // ui.command.server, until kiln:ui-command's start reports it
    }
}
