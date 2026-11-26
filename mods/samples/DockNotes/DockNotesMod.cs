// SPDX-License-Identifier: MIT
using Kiln;

namespace DockNotes;

/// <summary>
/// Window docking (docs/kiln/ui.md "Docking windows"), the window that docks: a notes window docked to the right of the
/// DockBook sample's spell book, another mod's window (<see cref="Ui.FindWindow"/>, <see cref="UiWindow.DockTo"/>). Kiln
/// keeps the two together: drag either one and both move, in the same frame. Shift+drag the notes to move them alone
/// (they leave the dock: <see cref="UiWindow.DockChanged"/>); drop them beside the book again and they dock again. Both
/// windows open with F6: on one key press the notes wait for the book, so they never show at their own place first.
/// Opened without the book, the notes go where the player left them (RememberPlacement keeps only the player's places).
/// </summary>
public sealed class DockNotesMod : Mod
{
    private UiWindow? _window;
    private string _note = "";
    private bool _alone;  // the player moved the notes away with Shift: open them alone from then on (this game start)

    public override void OnLoad()
    {
        if (!Ui.Available)
        {
            Log.Info("no overlay here (a dedicated server): nothing to show");
            return;
        }
        _window = Ui.Window("Notes", Draw, new UiWindowOptions
        {
            Width = 260, Height = 220, Position = (0.6f, 0.5f), StartOpen = false,
            PlacementId = "notes", RememberPlacement = true,
        });
        _window.DockChanged += change =>
        {
            if (!change.ByPlayer) return;
            _alone = change.DockedTo is null;
            Log.Info(_alone ? "the player took the notes off the book" : $"the player docked the notes again ({change.Side}, {change.Offset:F0} px)");
        };
        KeyBindings.Register("docknotes.toggle", "F6", Toggle, "Open the docking sample's notes");
    }

    private void Toggle()
    {
        var w = _window!;
        // the book's mod may load after this one, or not at all: look it up when the notes open
        if (!w.Open && !_alone && w.DockedTo is null && Ui.FindWindow("dockbook", "book") is { } book)
            w.DockTo(book, DockSide.Right);
        w.Toggle();
    }

    private void Draw(UiDraw ui)
    {
        ui.InputText("##note", ref _note, 256);
        ui.Separator();
        var book = _window!.DockedTo;
        if (book is null) ui.TextWrapped("On their own. Drop them next to the spell book to dock them again.");
        else if (!book.Available) ui.TextWrapped("Docked to the spell book, which is not loaded: they come back beside it when it is.");
        else if (!book.Open) ui.TextWrapped("Docked to the spell book, which is closed: it opens beside them.");
        else ui.TextWrapped("Docked to the spell book: drag either window, both move. Shift+drag these notes to move them alone.");
        // Shift held, Kiln took the notes off the book before this frame's draw: DockedTo is null already
        if (ui.WindowMovedByUser) ui.TextDisabled(book is { Open: true } ? "(moving with the book)" : "(moving alone)");
        else if ((ui.HeldMods & KeyMods.Shift) != 0 && book is { Open: true }) ui.TextDisabled("(Shift: a drag moves them alone)");
    }
}
