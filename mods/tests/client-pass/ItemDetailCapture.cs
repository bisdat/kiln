// SPDX-License-Identifier: MIT
using Kiln;
using Kiln.Testing;
using static Kiln.Testing.ClientCheckTags;

namespace ClientPass;

public sealed partial class ClientPassMod
{
    [ClientCheck("item-detail-capture", "Read game-built item detail text and numbers from a mod", Area = "Values in the game UI",
        Since = "0.2.0-dev.30.5", Tags = [Visual], Needs = ClientCheckNeeds.Hosted, Order = 3119)]
    public async Task ItemDetailCapture(CheckContext c)
    {
        if (!await NeedOpen(c, Ui.Values.ItemDetailCapability, Ui.Values.SlotsCapability)) return;
        VisibleSlot? saved = null;
        UiItemDetailSnapshot? captured = null;
        var follow = true;
        var window = Ui.Window("Captured item data", ui =>
        {
            if (ui.Button(follow ? "Freeze this slot" : "Follow selected/hovered slot")) follow = !follow;
            if (follow && (Ui.Inventory.Selected ?? Ui.Inventory.Hovered) is { } slot && !slot.Item.IsNone) saved = slot;
            if (saved is not { } source) { ui.TextWrapped("Open your backpack and select or hover a weapon."); return; }
            ui.Text($"Saved item: {source.Item}; client entity: {source.ClientEntity}; level: {source.Level}; rarity: {source.Rarity}");
            var result = Ui.Values.ItemDetail(source.ClientEntity, source.Item,
                source.Rarity.HasValue ? source.Level : null, source.Rarity);
            if (!result.Ok)
            {
                ui.TextWrapped("Game detail not captured — name/id only; no invented damage or effects.");
                ui.TextWrapped(result.Reason ?? "No matching capture");
                return;
            }
            Volatile.Write(ref captured, result.Snapshot);
            var variant = Ui.Values.ItemDetail(source.Item, source.Rarity.HasValue ? source.Level : null, source.Rarity);
            ui.TextWrapped(variant.Snapshot is { } typeCapture
                ? $"Latest matching type variant: entity {typeCapture.Entity}, level {typeCapture.Level}, rarity {typeCapture.Rarity}"
                : $"Type variant miss: {variant.Reason}");
            foreach (var line in result.Snapshot!.Lines)
            {
                if (line.Text.Length != 0) ui.TextWrapped(line.Text);
                else if (line.Value is { } number) ui.Text($"Numeric-only row (raw value): {number}");
            }
        }, new UiWindowOptions { StartOpen = true, Width = 720, Height = 650 });
        c.OnEnd(window.Dispose);
        c.Instruct("Select an item not inspected since first API use. Confirm a reasoned cache miss, then inspect it " +
            "in the game's own panel. This diagnostic window reads passive captures; it is not a new item-detail drawing API. " +
            "Freeze a slot to retain its identity while closing the inventory. No item or stat is changed.");
        if (!await c.Until(() => Volatile.Read(ref captured) is not null, TimeSpan.FromMinutes(3), "a real item panel was captured"))
        { c.Expect(false, "no real item-detail capture; this check cannot pass"); return; }
        var snapshot = Volatile.Read(ref captured)!;
        c.Expect(snapshot.Lines.Count > 0 && snapshot.Lines.Any(l => l.Value.HasValue), "game text and computed stat values captured");
        c.Instruct("Compare the captured text and numbers with the game's panel. Try two weapons of the same type at " +
            "different levels/rarities, an upgraded weapon, armor, gems/perks, food, a stackable item and a chest item. " +
            "Reinspect after changing language or item state. Unknown variants must miss, not borrow another instance's " +
            "numbers. Leave/rejoin with a frozen identity: it must miss before reinspection. Report any icon/colour-only " +
            "information separately; this API does not expose semantic socket/perk state. Pass only if constructed text " +
            "and captured numbers match, numeric metadata is not appended twice, and the game's panel is unchanged.");
        await c.Verdict();
    }
}
