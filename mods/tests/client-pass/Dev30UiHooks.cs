// SPDX-License-Identifier: MIT
using Kiln;
using Kiln.Testing;
using static Kiln.Testing.ClientCheckTags;

namespace ClientPass;

// dev30 ui-hooks (/mnt/Dongus/kiln-wave/dev30/out/ui-hooks/DESIGN.md): Ui.Inventory, marks and clicks on the game's own
// slots. Rows: ui.slots.click (ui-hooks.c2, c3, c4) and the marks on ui.slots.inspect (ui-hooks.c1, which is also 05a-c5's
// pixel mapping). Slice 2 (ui.inventory.panels): a Kiln panel docked beside the backpack and chest screens and the chest's
// entity (ui-hooks.c5, c6, c7). Each check registers its own marks, handlers and panels and removes them at its end; a
// check whose handler never ran cannot pass. Orders 2410-2419.
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

    private const string PanelsArea = "Panels beside the game's inventory screens";

    [ClientCheck("ui-hooks.c5", "A panel sits beside the backpack and the chest (two resolutions or UI scales)", Area = PanelsArea,
        Since = Dev30, Tags = [Visual], Needs = ClientCheckNeeds.Hosted, Order = 2414)]
    public async Task InventoryPanelBeside(CheckContext c)
    {
        if (!await NeedOpen(c, Ui.Inventory.PanelCapability)) return;
        int drawn = 0;
        var gate = new object();
        InventoryScreen? last = null;
        InventoryScreen? Last() { lock (gate) return last; }
        var panel = Ui.Inventory.AddPanel("Kiln check panel", (ui, s) =>
        {
            Interlocked.Increment(ref drawn);
            lock (gate) last = s;
            ui.Text($"{s.Kind} ({s.Hud})");
            ui.TextDisabled("ui-hooks.c5: this panel should sit beside the slots");
        });
        c.OnEnd(panel.Dispose);
        c.Instruct("Host a world. Open the inventory (I). Press continue.");
        await c.Verdict();
        c.Expect(await c.Until(() => Volatile.Read(ref drawn) > 0, TimeSpan.FromSeconds(30), "the panel was drawn"),
            "the panel is drawn on the backpack screen", $"last screen {Last()}");
        c.Instruct("A small window \"Kiln check panel\" saying Backpack should sit right next to your backpack's slots (to " +
                   "their right, or to their left when there is no room), not on top of any slot and not off the screen. Is it there?");
        await c.Verdict();
        c.Instruct("Open a storage chest. The panel should now say Chest and sit beside the slots (the chest's and the backpack's " +
                   "together), level with the chest's slots. Is it there, without covering a slot?");
        await c.Verdict();
        c.Expect(Last() is { Kind: InventoryScreenKind.Chest }, "the panel saw the chest screen", $"{Last()}");
        c.Instruct("Change the game's resolution or window size (Settings > Graphics), or the UI scale, open the chest again. " +
                   "Does the panel still sit beside the slots? (Write the resolutions or scales you tried in the notes.)");
        await c.Verdict();
        c.Instruct("Hover the panel and click on it (not on a slot). Did the game ignore that click (nothing picked up or moved)?");
        await c.Verdict();
    }

    [ClientCheck("ui-hooks.c6", "The chest screen names the chest's entity", Area = PanelsArea,
        Since = Dev30, Needs = ClientCheckNeeds.Hosted, Order = 2415)]
    public async Task InventoryChestEntity(CheckContext c)
    {
        if (!await NeedOpen(c, Ui.Inventory.PanelCapability) || !await NeedPlayer(c)) return;
        var gate = new object();
        InventoryScreen? chest = null;
        InventoryScreen? Chest() { lock (gate) return chest; }
        Action<InventoryScreen?, InventoryScreen?> h = (_, now) =>
        {
            if (now is { Kind: InventoryScreenKind.Chest, Container.Value: not 0 } s) lock (gate) chest = s;
        };
        Ui.Inventory.ScreenChanged += h;
        c.OnEnd(() => Ui.Inventory.ScreenChanged -= h);
        c.Instruct("Put a few items in a storage chest, close it, then open it again.");
        c.Expect(await c.Until(() => Chest() is not null, TimeSpan.FromMinutes(2), "a chest screen with its entity"),
            "ScreenChanged named the chest's entity");
        if (Chest() is not { } s) return;
        c.Log.Info($"ui-hooks.c6: {s}, GameScreen.Target {Ui.Screens.Current.Target}");
        c.Expect(Ui.Screens.Current.Target == s.Container, "GameScreen.Target is the same entity", $"{Ui.Screens.Current.Target}");
        // the entity is the chest: the chest's own slots name it as their holder, and the server world has its inventory
        var slots = Ui.Values.Slots();
        c.Expect(slots.Ok && slots.Snapshot!.Slots.Any(v => v.Holder == s.Container),
            "the chest's slots on screen are held by that entity", $"holders {string.Join(", ", slots.Snapshot?.Slots.Select(v => v.Holder).Distinct() ?? [])}");
        var inv = Items.Read(s.Container);
        c.Expect(inv is not null && inv.Slots.Any(v => !v.IsEmpty), "the server world reads that entity's inventory (the chest's items)",
            inv is null ? "no inventory" : $"{inv.Slots.Count(v => !v.IsEmpty)} stacks");
        var at = World.Position(c.Player);
        var where = World.Position(s.Container);
        c.Expect(at is not null && where is not null && where.Value.DistanceTo(at.Value) < 15, "the entity stands next to you",
            at is null || where is null ? "no position" : $"{where.Value.DistanceTo(at.Value):F1} m");
        c.Instruct("Does the log line ui-hooks.c6 name a chest entity (a number), and did the items you put in match the stacks it counted?");
        await c.Verdict();
    }

    [ClientCheck("ui-hooks.c7", "Closing the chest or backpack hides the panel; opening, switching and closing are events",
        Area = PanelsArea, Since = Dev30, Tags = [Visual], Needs = ClientCheckNeeds.Hosted, Order = 2416)]
    public async Task InventoryPanelCloses(CheckContext c)
    {
        if (!await NeedOpen(c, Ui.Inventory.PanelCapability)) return;
        var events = new List<string>();
        Action<InventoryScreen?, InventoryScreen?> h = (before, now) =>
        {
            lock (events) events.Add($"{before?.Kind.ToString() ?? "none"} -> {now?.Kind.ToString() ?? "none"} {now?.Container}");
        };
        Ui.Inventory.ScreenChanged += h;
        var panel = Ui.Inventory.AddPanel("Kiln check panel", (ui, s) => ui.Text($"{s.Kind} {s.Container}"),
            new InventoryPanelOptions { Screens = InventoryScreens.Chest });
        c.OnEnd(() =>
        {
            Ui.Inventory.ScreenChanged -= h;
            panel.Dispose();
        });
        c.Instruct("Open a storage chest (the panel shows beside it), close it, open the inventory (I), close it, then open a " +
                   "second, different chest and close it. Press continue.");
        await c.Verdict();
        string[] seen;
        lock (events) seen = [.. events];
        c.Log.Info("ui-hooks.c7: " + string.Join(" | ", seen));
        c.Expect(seen.Count(e => e.StartsWith("none -> Chest")) >= 2 && seen.Count(e => e.StartsWith("Chest -> none")) >= 2,
            "two chest openings and closings were reported", string.Join(" | ", seen));
        c.Expect(seen.Any(e => e.StartsWith("none -> Backpack")), "the backpack's opening was reported");
        c.Instruct("Did the panel disappear each time the chest closed, and did it NOT show on the inventory screen (this " +
                   "panel asked for chests only)?");
        await c.Verdict();
    }

    [ClientCheck("ui-hooks.c8", "A client mod's own symbol fragment: its typed hook installs and runs in the game client",
        Area = "Client hooks for mods", Since = Dev30, Needs = ClientCheckNeeds.Hosted, Order = 2417)]
    public async Task ClientOwnFragment(CheckContext c)
    {
        // mods/samples/ClientHookSample (side client, advanced): sigdb/client.json names GetItemVolume under its own name
        if (!await Need(c, "client-hook-sample")) return;
        const string row = "sig/client-hook-sample/ItemVolume";
        c.Expect(Capabilities.Is("hooks.typed"), "hooks.typed is available in this game", Capabilities.Why("hooks.typed"));
        c.Expect(await c.Until(() => Capabilities.Is(row), TimeSpan.FromSeconds(30), "the sample's descriptor row"),
            "the sample's own client descriptor fits this game client", Capabilities.Why(row));
        c.Instruct("Host a world. Fill a watering can at a well (or pour water from it). Then open kiln/logs/kiln.log and " +
                   "look for 'client-hook-sample'. Is there a line 'hooked' (or 'the hook goes in at the next safe tick'), " +
                   "and a line 'GetItemVolume called N time(s)'? Did the game play on normally, with the usual amount of water?");
        await c.Verdict();
    }
}
