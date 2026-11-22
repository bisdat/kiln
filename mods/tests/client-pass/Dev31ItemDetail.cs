// SPDX-License-Identifier: MIT
using Kiln;
using Kiln.Testing;
using static Kiln.Testing.ClientCheckTags;

namespace ClientPass;

public sealed partial class ClientPassMod
{
    [ClientCheck("ui-item-detail.c1", "Game-built item detail text and numbers in a Kiln window", Area = "Values in the game UI",
        Since = Dev31, Tags = [Visual], Needs = ClientCheckNeeds.Hosted, Order = 3119)]
    public async Task ItemDetailInKilnWindow(CheckContext c)
    {
        if (!await NeedOpen(c, Ui.Values.ItemDetailCapability, Ui.Values.SlotsCapability)) return;
        VisibleSlot? saved = null;
        UiItemDetailSnapshot? captured = null;
        var follow = true;
        var width = 300f;
        var window = Ui.Window("Item detail #120", ui =>
        {
            if (ui.Button(follow ? "Freeze this slot" : "Follow selected/hovered slot")) follow = !follow;
            if (ui.Button("Change width")) width = width == 300 ? 140 : width == 140 ? 600 : 300;
            if (follow && (Ui.Inventory.Selected ?? Ui.Inventory.Hovered) is { } slot && !slot.Item.IsNone) saved = slot;
            if (saved is not { } source) { ui.TextWrapped("Open your backpack and select or hover a weapon."); return; }
            ui.Text($"Saved item: {source.Item}; entity: {source.ClientEntity}; level: {source.Level}; rarity: {source.Rarity}");
            var result = ui.ItemDetail(source, width);
            if (result.Ok) Volatile.Write(ref captured, result.Snapshot);
            else ui.TextWrapped(result.Reason ?? "fallback");
        }, new UiWindowOptions { StartOpen = true, Width = 720, Height = 650 });
        c.OnEnd(window.Dispose);
        c.Instruct("Open your backpack, select/hover a weapon, then inspect it in the game's own detail panel. " +
            "The #120 window must change from a labelled fallback to game text and numbers. Freeze the slot to keep it " +
            "while closing the inventory. No item or stat is changed by this check.");
        if (!await c.Until(() => Volatile.Read(ref captured) is not null, TimeSpan.FromMinutes(3), "a real item panel was captured"))
        { c.Expect(false, "no real item-detail capture; this check cannot pass"); return; }
        var snapshot = Volatile.Read(ref captured)!;
        c.Expect(snapshot.Lines.Count > 0 && snapshot.Lines.Any(l => l.Value.HasValue), "game text and computed stat values captured");
        c.Instruct("Compare every visible name/description/stat label and number with the game's panel. Change width " +
            "to 140/300/600 and scroll: long/non-English text must stay in the window. Try another weapon of the same " +
            "type with another level/rarity, an upgraded weapon, a stackable item and a chest item. Reinspect after " +
            "changing language. Unknown variants must show fallback, not another instance's numbers. Pass only if " +
            "all text and numbers match, no numeric row is duplicated, and the game's panel is unchanged.");
        await c.Verdict();
    }
}
