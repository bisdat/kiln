// SPDX-License-Identifier: MIT
using Kiln;

namespace DockBook;

/// <summary>
/// Window docking (docs/kiln/ui.md "Docking windows"), the window others dock to: a spell book window that accepts docks
/// (<see cref="UiWindowOptions.AcceptDock"/>), so another mod's window can sit beside it and move with it: the DockNotes
/// sample docks its notes to its right. F6 opens and closes it; Kiln remembers where the player left it. Nothing else is
/// needed on this side: no messages, no frame counting.
/// </summary>
public sealed class DockBookMod : Mod
{
    private static readonly string[] Spells = ["Fireball", "Frost bolt", "Healing aura", "Lightning chain", "Shield"];
    private UiWindow? _window;
    private int _selected;

    public override void OnLoad()
    {
        if (!Ui.Available)
        {
            Log.Info("no overlay here (a dedicated server): nothing to show");
            return;
        }
        // other mods find it as Ui.FindWindow("dockbook", "book")
        _window = Ui.Window("Spell book", Draw, new UiWindowOptions
        {
            Width = 320, Height = 380, Position = (0.35f, 0.5f), StartOpen = false,
            PlacementId = "book", RememberPlacement = true, AcceptDock = true,
        });
        KeyBindings.Register("dockbook.toggle", "F6", _window.Toggle, "Open the docking sample's spell book");
    }

    private void Draw(UiDraw ui)
    {
        for (var i = 0; i < Spells.Length; i++)
            if (ui.Selectable(Spells[i], i == _selected)) _selected = i;
        ui.Separator();
        ui.TextWrapped("Drag this window: a window docked to it moves with it in the same frame.");
        if (ui.WindowMovedByUser) ui.TextDisabled("(being dragged)");
    }
}
