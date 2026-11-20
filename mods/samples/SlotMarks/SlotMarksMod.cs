// SPDX-License-Identifier: MIT
using Kiln;

namespace SlotMarks;

/// <summary>
/// Ui.Inventory (docs/kiln/ui.md "The game's inventory slots", 0.2.0-dev.30): the game's own slots as something a mod acts
/// on, in this player's game only.
///  - Ctrl+left click on a slot in the backpack, the inventory menu, a chest or a station marks the item with a red frame
///    (click again to clear it). The game never sees the press: it does not pick the item up.
///  - Alt+right click lists the slot in kiln.log and as a toast (watch only: the game gets the click as usual).
///  - Every stack of 50 or more gets a faint tint (a rule asked per drawn slot, on the render thread: keep it a lookup).
/// The mark follows the item, not the slot: move it to a chest and the frame moves with it. ItemEntity is the item's
/// entity id, which for an item the server sent is the server's id of it: a real mod (Kiln Auto Loot's salvage marks)
/// sends it to its own server half, which checks the item is the sender's before it acts, and keeps the mark there
/// (InstanceData, synced back). This sample keeps its marks in the client only; they go with the session.
/// </summary>
public sealed class SlotMarksMod : Mod
{
    private static readonly SlotMark Red = new(new Color(235, 60, 50), SlotMarkStyle.Border, 3);
    private static readonly SlotMark BigStack = new(new Color(240, 210, 90, 60), SlotMarkStyle.Fill);
    private readonly HashSet<EntityId> _marked = [];
    private readonly List<IDisposable> _subs = [];

    public override void OnLoad()
    {
        try
        {
            _subs.Add(Ui.Inventory.OnClick(new SlotClickOptions { Modifiers = SlotModifiers.Ctrl }, ToggleMark));
            _subs.Add(Ui.Inventory.OnClick(new SlotClickOptions { Button = SlotButton.Right, Modifiers = SlotModifiers.Alt, Take = false },
                Describe));
        }
        catch (KilnUnavailableException e)
        {
            Log.Info($"slotmarks: no slot clicks here: {e.Message}");
        }
        try
        {
            _subs.Add(Ui.Inventory.MarkWhen(s => s.Count >= 50 ? BigStack : null));
        }
        catch (KilnUnavailableException e)
        {
            Log.Info($"slotmarks: no slot marks here: {e.Message}");
        }
    }

    public override void OnUnload()
    {
        foreach (var s in _subs) s.Dispose();  // Kiln removes them with the mod anyway
        _subs.Clear();
    }

    private void ToggleMark(SlotClick click)
    {
        if (click.ItemEntity.Value == 0) return;  // an empty slot, or a stack the game drew without an entity
        if (_marked.Remove(click.ItemEntity)) Ui.Inventory.Unmark(click.ItemEntity);
        else if (_marked.Add(click.ItemEntity)) Ui.Inventory.Mark(click.ItemEntity, Red);
        Log.Info($"slotmarks: {(_marked.Contains(click.ItemEntity) ? "marked" : "cleared")} item {click.ItemEntity} " +
                 $"({click.Item}) in page {click.Slot.InventoryPage} slot {click.Slot.SlotIndex} of {click.Holder}, screen {click.Screen.Hud}");
    }

    private void Describe(SlotClick click)
    {
        var text = $"slot {click.Slot.SlotIndex} of page {click.Slot.InventoryPage}: item {click.Item} x{click.Slot.Count}, " +
                   $"entity {click.ItemEntity}, holder {click.Holder}";
        Log.Info("slotmarks: " + text);
        Hud.Toast(text);
    }
}
