// SPDX-License-Identifier: MIT
using Kiln;
using Kiln.Testing;
using static Kiln.Testing.ClientCheckTags;

namespace ClientPass;

// dev30 ui-hooks (/mnt/Dongus/kiln-wave/dev30/out/ui-hooks/DESIGN.md): Ui.Inventory, marks and clicks on the game's own
// slots. Rows: ui.slots.click (ui-hooks.c2, c3, c4) and the marks on ui.slots.inspect (ui-hooks.c1, which is also 05a-c5's
// pixel mapping). Each check registers its own marks and handlers and removes them at its end; a check whose handler never
// ran cannot pass. Orders 2410-2419.
public sealed partial class ClientPassMod
{
    private const string SlotsArea = "The game's inventory slots";
    private static readonly SlotMark CheckFrame = new(new Color(235, 60, 50), SlotMarkStyle.Border, 3);

    [ClientCheck("ui-hooks.c1", "Marks sit on the game's slots (backpack, chest, hotbar; two resolutions)", Area = SlotsArea,
        Since = Dev30, Tags = [Visual], Needs = ClientCheckNeeds.Hosted, Order = 2410)]
    public async Task SlotMarksOnSlots(CheckContext c)
    {
        if (!await NeedOpen(c, Ui.Inventory.MarkCapability)) return;
        int asked = 0;
        var rule = Ui.Inventory.MarkWhen(s =>
        {
            Interlocked.Increment(ref asked);
            return s.Item.Value != 0 ? CheckFrame : null;
        });
        c.OnEnd(rule.Dispose);
        c.Instruct("Host a world with a few items in your backpack and on the action bar. Open the inventory (I). Press continue.");
        await c.Verdict();
        c.Expect(await c.Until(() => Volatile.Read(ref asked) > 0, TimeSpan.FromSeconds(30), "the overlay asked the rule about drawn slots"),
            "marks are drawn from the game's slot frames");
        c.Instruct("Every slot that holds an item should have a thin red frame exactly on the slot's edge (not shifted, not " +
                   "larger or smaller), and empty slots none. Is that what you see?");
        await c.Verdict();
        c.Instruct("Open a storage chest that holds items. Do the chest's slots and your backpack's slots both have the frames " +
                   "on their edges?");
        await c.Verdict();
        c.Instruct("Close every menu. Do the items on the action bar (the hotbar at the bottom) have the frames too?");
        await c.Verdict();
        c.Instruct("Change the game's resolution or window size (Settings > Graphics), or the UI scale, and open the " +
                   "inventory again. Are the frames still exactly on the slots? (Write the resolutions you tried in the notes.)");
        await c.Verdict();
        rule.Dispose();
        c.Instruct("The rule is gone. Have all frames disappeared?");
        await c.Verdict();
    }

    [ClientCheck("ui-hooks.c2", "Ctrl+click on a backpack slot: the mod gets the item, the game does not pick it up", Area = SlotsArea,
        Since = Dev30, Tags = [Visual], Needs = ClientCheckNeeds.Hosted, Order = 2411)]
    public async Task SlotClickBackpack(CheckContext c)
    {
        if (!await NeedOpen(c, Ui.Inventory.ClickCapability)) return;
        SlotClick? got = null;
        var sub = Ui.Inventory.OnClick(new SlotClickOptions { Modifiers = SlotModifiers.Ctrl }, k =>
        {
            Volatile.Write(ref got, k);
            if (k.ItemEntity.Value != 0) Ui.Inventory.Mark(k.ItemEntity, CheckFrame);
        });
        c.OnEnd(() =>
        {
            sub.Dispose();
            Ui.Inventory.ClearMarks();
        });
        c.Instruct("Open the inventory (I). Hold Ctrl and left-click a weapon or tool in your backpack.");
        c.Expect(await c.Until(() => Volatile.Read(ref got) is not null, TimeSpan.FromMinutes(2), "a Ctrl+click on a slot"),
            "the click reached the mod");
        if (Volatile.Read(ref got) is { } k)
        {
            c.Log.Info($"ui-hooks.c2: {k}");
            c.Expect(k.ItemEntity.Value != 0 && k.Item.Value != 0 && k.Taken, "the click carries the item, its entity, and was taken",
                $"item {k.Item}, entity {k.ItemEntity}, page {k.Slot.InventoryPage} slot {k.Slot.SlotIndex}, holder {k.Holder}, screen {k.Screen.Hud}");
            c.Expect(k.Holder == Client.LocalPlayer, "the slot's holder is you", $"{k.Holder} vs {Client.LocalPlayer}");
            // the identity claim: the client's pide is the server's entity of that item (hosted: the server world is here)
            c.Expect((Items.Read(c.Player)?.Slots ?? []).Any(s => s.ItemEntity == k.ItemEntity && s.Item == k.Item),
                "the clicked entity is that item's entity in your inventory as the server world holds it");
        }
        c.Instruct("Did the item stay where it was (not picked up onto the cursor), and does it now have a red frame?");
        await c.Verdict();
        c.Instruct("Now left-click the same item WITHOUT Ctrl. Does the game pick it up as usual? Put it back.");
        await c.Verdict();
        c.Instruct("Move the marked item to another slot. Does the red frame move with it?");
        await c.Verdict();
    }

    [ClientCheck("ui-hooks.c3", "Clicks in a chest: the chest's items, a watched click still reaches the game", Area = SlotsArea,
        Since = Dev30, Tags = [Visual], Needs = ClientCheckNeeds.Hosted, Order = 2412)]
    public async Task SlotClickChest(CheckContext c)
    {
        if (!await NeedOpen(c, Ui.Inventory.ClickCapability)) return;
        SlotClick? taken = null, watched = null;
        var a = Ui.Inventory.OnClick(new SlotClickOptions { Modifiers = SlotModifiers.Ctrl }, k => Volatile.Write(ref taken, k));
        var b = Ui.Inventory.OnClick(new SlotClickOptions { Button = SlotButton.Right, Modifiers = SlotModifiers.Alt, Take = false },
            k => Volatile.Write(ref watched, k));
        c.OnEnd(() =>
        {
            a.Dispose();
            b.Dispose();
        });
        c.Instruct("Open a storage chest that holds items. Hold Ctrl and left-click an item IN THE CHEST.");
        c.Expect(await c.Until(() => Volatile.Read(ref taken) is not null, TimeSpan.FromMinutes(2), "a Ctrl+click in the chest"),
            "the chest click reached the mod");
        if (Volatile.Read(ref taken) is { } k)
        {
            c.Log.Info($"ui-hooks.c3 chest: {k}");
            c.Expect(k.ItemEntity.Value != 0 && k.Holder != Client.LocalPlayer && k.Screen.Hud == HudScreen.StorageChest,
                "a chest item: its entity, a holder that is not you, the chest screen", $"holder {k.Holder}, screen {k.Screen.Hud}");
        }
        c.Instruct("Did the chest item stay where it was? Now hold Alt and right-click any item in the chest or backpack.");
        c.Expect(await c.Until(() => Volatile.Read(ref watched) is not null, TimeSpan.FromMinutes(2), "an Alt+right click"),
            "the watched click reached the mod");
        c.Instruct("Did the game also do what it does on a right click there (the click was only watched)?");
        await c.Verdict();
    }

    [ClientCheck("ui-hooks.c4", "No slot clicks in the play view; the cursor's slot (Hovered) and the game's hover byte",
        Area = SlotsArea, Since = Dev30, Tags = [Visual], Needs = ClientCheckNeeds.Hosted, Order = 2413)]
    public async Task SlotClickPlayView(CheckContext c)
    {
        if (!await NeedOpen(c, Ui.Inventory.ClickCapability)) return;
        int clicks = 0;
        var sub = Ui.Inventory.OnClick(new SlotClickOptions { Modifiers = SlotModifiers.Ctrl }, _ => Interlocked.Increment(ref clicks));
        c.OnEnd(sub.Dispose);
        c.Instruct("Close every menu. In the play view, hold Ctrl and left-click a few times (also with the cursor over the " +
                   "hotbar). Your character should act as usual. Press continue.");
        await c.Verdict();
        c.Expect(Volatile.Read(ref clicks) == 0, "no slot click in the play view", $"{clicks} click(s)");
        c.Instruct("Open the inventory and rest the mouse cursor on an item in the backpack (do not click). Press continue.");
        await c.Verdict();
        var h = Ui.Inventory.Hovered;
        c.Expect(h is { Item.Value: not 0 }, "Hovered names the slot under the cursor", h?.ToString());
        if (h is not null)
            c.Log.Info($"ui-hooks.c4: hovered {h.Item} page {h.InventoryPage} slot {h.SlotIndex}, native state byte 0 = " +
                       $"{h.NativeState[0]} (the candidate hover byte of 05a-c5)");
        c.Instruct("Write in the notes which item is under the cursor; the log line ui-hooks.c4 names what Kiln saw.");
        await c.Verdict();
    }
}
