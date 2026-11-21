// SPDX-License-Identifier: MIT
using Kiln;
using Kiln.Testing;
using static Kiln.Testing.ClientCheckTags;

namespace ClientPass;

// 05a-value-providers (chapter 05a): Ui.Values, values in the game's own UI. Every row is on in every game since
// 0.2.0-dev.29 but remains unconfirmed: ui.values.detail and ui.values.slot by 05a-c1,
// ui.values.hud by 05a-c2, ui.values.held by 05a-c3, ui.values.tooltip_lines by 05a-c4-viewer, ui.slots.inspect by
// 05a-c5. Each check registers its own providers (the research's payloads as product providers), removes them at its
// end, and expects that the game actually called them: a check whose hook never ran cannot pass. Orders 2400-2499.
// 05a-c4-short and 05a-c4-native are settled research results with no product check (no native line is drawn).
public sealed partial class ClientPassMod
{
    private const string ValuesArea = "Values in the game UI";

    private static bool HasFuel(in UiItemView v) => v.FuelMax > 0;

    [ClientCheck("05a-c1", "Detail numbers and slot bars", Area = ValuesArea, Since = Dev28, Tags = [Visual],
        Needs = ClientCheckNeeds.Hosted, Order = 2400)]
    public async Task ValuesDetailAndSlot(CheckContext c)
    {
        if (!await NeedOpen(c, Ui.Values.DetailCapability, Ui.Values.SlotCapability)) return;
        int detail = 0, slot = 0;
        var d = Ui.Values.ItemBar(default, (in UiValueContext x, in UiItemView v, UiBarValue a) =>
        {
            if (!HasFuel(v)) return UiProviderDecision.Pass;
            Interlocked.Increment(ref detail);
            return UiProviderDecision.Replace(new UiBarEdit(Fraction: 0.25f, Current: 1_000_098, Max: 2_000_000));
        });
        var s = Ui.Values.SlotBar(default, (in UiValueContext x, in UiItemView v, UiBarValue a) =>
        {
            if (!HasFuel(v)) return UiProviderDecision.Pass;
            Interlocked.Increment(ref slot);
            return UiProviderDecision.Replace(new UiBarEdit(Fraction: 0.25f));
        });
        c.OnEnd(() =>
        {
            d.Dispose();
            s.Dispose();
        });
        c.Expect(d.Ok && s.Ok, $"both providers registered ({d.Reason ?? s.Reason})");
        c.Instruct("Host a world and put a watering can (any container that holds water, with some water in it) in your " +
                   "backpack. Open the inventory and select the can so its detail view shows. The check goes on by itself " +
                   "once the game draws the can through Kiln.");
        c.Expect(await c.Until(() => Volatile.Read(ref detail) > 0 && Volatile.Read(ref slot) > 0, TimeSpan.FromMinutes(3),
            "the game drew the can's detail bar and slot bar through the providers"), "detail and slot providers were called");
        c.Instruct("Keep the can selected. Remember whether the can's detail view reads 1000098 / 2000000 with the bar a quarter " +
                   "full, and the can's slot bar is a quarter full. Kiln removes its values by itself in 20 seconds.");
        await c.Delay(TimeSpan.FromSeconds(20));
        d.Dispose();
        s.Dispose();
        c.Instruct("Kiln's values are gone. Pass only if all of these held: (1) the detail view read 1000098 / 2000000 with the " +
                   "bar a quarter full and the slot bar was a quarter full, (2) now the can's number and both bars are back to " +
                   "the game's own values. Otherwise Fail with a note naming which one.");
        await c.Verdict();
    }

    [ClientCheck("05a-c2", "Retained post-update HUD writes", Area = ValuesArea, Since = Dev28, Tags = [Visual],
        Needs = ClientCheckNeeds.Hosted, Order = 2401)]
    public async Task ValuesHud(CheckContext c)
    {
        if (!await NeedOpen(c, Ui.Values.HudCapability)) return;
        int calls = 0;
        UiValueRegistration Pair(HudValue vital, uint value) => Ui.Values.Hud(vital, (in UiValueContext x, in UiItemView v, UiBarValue a) =>
        {
            Interlocked.Increment(ref calls);
            return UiProviderDecision.Replace(new UiBarEdit(Current: value, Max: 100));
        });
        var regs = new[] { Pair(HudValue.Health, 37), Pair(HudValue.Mana, 11) };
        c.OnEnd(() =>
        {
            foreach (var r in regs) r.Dispose();
        });
        c.Expect(regs.All(r => r.Ok), "the health and mana providers registered");
        c.Expect(await c.Until(() => Volatile.Read(ref calls) > 0, TimeSpan.FromSeconds(30), "the player-UI update ran the HUD providers"),
            "HUD providers were called");
        c.Instruct("Remember whether the HUD shows health at about 37% and mana at about 11%, steady (not flickering back to " +
                   "full). Kiln removes its values by itself in 20 seconds.");
        await c.Delay(TimeSpan.FromSeconds(20));
        foreach (var r in regs) r.Dispose();
        c.Instruct("Kiln's values are gone. Pass only if all of these held: (1) health showed about 37% and mana about 11%, " +
                   "steady, (2) now health and mana are back to your real values. Otherwise Fail with a note naming which one.");
        await c.Verdict();
    }

    [ClientCheck("05a-c3", "Nonzero held water and visible stamina identity", Area = ValuesArea, Since = Dev28, Tags = [Visual],
        Needs = ClientCheckNeeds.Hosted, Order = 2402)]
    public async Task ValuesHeldAndStamina(CheckContext c)
    {
        if (!await NeedOpen(c, Ui.Values.HeldCapability, Ui.Values.HudCapability)) return;
        int held = 0, stamina = 0;
        var h = Ui.Values.HeldBar((in UiValueContext x, in UiItemView v, UiBarValue a) =>
        {
            Interlocked.Increment(ref held);
            return UiProviderDecision.Replace(new UiBarEdit(Current: 1, Max: 4));
        });
        var st = Ui.Values.Hud(HudValue.Stamina, (in UiValueContext x, in UiItemView v, UiBarValue a) =>
        {
            Interlocked.Increment(ref stamina);
            return UiProviderDecision.Replace(new UiBarEdit(Current: 23, Max: 100));
        });
        c.OnEnd(() =>
        {
            h.Dispose();
            st.Dispose();
        });
        c.Instruct("Put a watering can with some water (not empty) on your action bar and hold it in your hands. The check " +
                   "goes on by itself once the game draws the held can through Kiln.");
        c.Expect(await c.Until(() => Volatile.Read(ref held) > 0, TimeSpan.FromMinutes(3), "the held provider ran"),
            "the held provider was called (a held item with water)");
        c.Expect(await c.Until(() => Volatile.Read(ref stamina) > 0, TimeSpan.FromSeconds(30), "the stamina provider ran"),
            "the stamina provider was called");
        c.Instruct("Keep holding the can. Remember whether the held can's water indicator shows a quarter (1 of 4), and which " +
                   "HUD bar shows about 23% and stays there (it should be stamina). Kiln removes its values by itself in 25 seconds.");
        await c.Delay(TimeSpan.FromSeconds(25));
        h.Dispose();
        st.Dispose();
        c.Instruct("Kiln's values are gone. Pass only if all of these held: (1) the held can's water indicator showed a quarter " +
                   "(1 of 4), (2) the bar at about 23% was stamina and stayed there, (3) now the held indicator and that bar are " +
                   "back to normal. Otherwise Fail with a note naming which one (and which bar showed 23%, if not stamina).");
        await c.Verdict();
    }

    [ClientCheck("05a-c4-viewer", "Replacement owned details viewer", Area = ValuesArea, Since = Dev28, Tags = [Visual],
        Needs = ClientCheckNeeds.Hosted, Order = 2403)]
    public async Task ValuesDetailsViewer(CheckContext c)
    {
        if (!await NeedOpen(c, Ui.Values.TooltipCapability)) return;
        var multi = false;
        var english = "LAYOUT-LONG-BEGIN " + string.Join(" ", Enumerable.Range(1, 220).Select(i => $"word{i}")) + " LAYOUT-LONG-END";
        var german = "LAYOUT-DE-BEGIN Größenänderung der Wasserkanne, Füllstandsanzeige für den Brennstoffbehälter, " +
                     "Donaudampfschifffahrtsgesellschaftskapitänswitwenrente, Übergrößenträgerin, Maßkrüge ÄÖÜäöüß LAYOUT-DE-END";
        int calls = 0;
        var reg = Ui.Values.TooltipLines(default, (in UiValueContext x, in UiItemView v) =>
        {
            Interlocked.Increment(ref calls);
            return multi
                ? UiTooltipDecision.With(Enumerable.Range(1, 16).Select(i => (UiText)$"LAYOUT-MULTI-{i:00}").ToArray())
                : UiTooltipDecision.With(english, german);
        });
        c.OnEnd(() =>
        {
            Ui.Values.CloseTooltipDetails();
            reg.Dispose();
        });
        c.Instruct("Open the inventory and select any item so its detail view shows (selecting, not only hovering the action " +
                   "bar). The check goes on by itself once the detail view asks Kiln for its lines.");
        c.Expect(await c.Until(() => Volatile.Read(ref calls) > 0, TimeSpan.FromMinutes(3), "the detail view collected the lines"),
            "the tooltip-lines provider was called");
        var open = Ui.Values.OpenTooltipDetails();
        c.Expect(open.Ok, $"the Item details window opened ({open.Reason})");
        c.Instruct("A window \"Item details\" shows a long English text ending in LAYOUT-LONG-END and a German one ending in " +
                   "LAYOUT-DE-END. Scroll its own pane and remember whether both END markers are readable, every line inside the " +
                   "window (none running past its border), the German letters right, and the game's own item tooltip untouched. " +
                   "The check switches to the next test by itself in 60 seconds.");
        await c.Delay(TimeSpan.FromSeconds(60));
        multi = true;
        var before = Volatile.Read(ref calls);
        c.Instruct("Select the same item again (or another one and back). The check goes on by itself once the detail view " +
                   "asks Kiln for its lines again.");
        c.Expect(await c.Until(() => Volatile.Read(ref calls) > before, TimeSpan.FromMinutes(3), "the detail view collected the lines again"),
            "the tooltip-lines provider was called again");
        c.Instruct("Press Refresh in the Item details window. Remember whether it lists LAYOUT-MULTI-01 to LAYOUT-MULTI-16 in " +
                   "order (scroll to see 01 and 16, none missing or overlapping). Then change the game's UI scale (Settings, " +
                   "Interface) to another value (1.5 if you can), reopen the window from the pause menu (\"Item details\") and " +
                   "remember whether the same holds: everything inside, 01 and 16 reachable. The check closes the window by " +
                   "itself in 3 minutes.");
        await c.Delay(TimeSpan.FromMinutes(3));
        Ui.Values.CloseTooltipDetails();
        c.Instruct("The window is closed. Pass only if all of these held: (1) both END markers readable, every line inside the " +
                   "window, the German letters right, the game's tooltip untouched, (2) after Refresh, LAYOUT-MULTI-01 to 16 in " +
                   "order, none missing or overlapping, (3) the same at the other UI scale, (4) now the item's own stats and " +
                   "description are fully visible and as the game draws them. Otherwise Fail with a note naming which one.");
        await c.Verdict();
    }

    [ClientCheck("05a-c5", "Cursor, gamepad, geometry and chest transfer", Area = ValuesArea, Since = Dev28, Tags = [Visual],
        Needs = ClientCheckNeeds.Hosted, Order = 2404)]
    public async Task ValuesSlotGeometry(CheckContext c)
    {
        if (!await NeedOpen(c, Ui.Values.SlotsCapability)) return;
        c.Instruct("Open the inventory (backpack). The check goes on by itself once Kiln reads the slots the game drew.");
        VisibleSlotsResult last = new(false, "not read", null);
        c.Expect(await c.Until(() => (last = Ui.Values.Slots()) is { Ok: true, Snapshot.Slots.Count: > 0 }, TimeSpan.FromMinutes(3),
            "the slots of a drawn frame"), $"Slots() returned the drawn slots ({last.Reason})");
        if (last.Snapshot is not { } snap) return;
        c.Expect(snap.Slots.Any(s => s.OuterUi is not null), "at least one slot has an outer rectangle");
        c.Expect(snap.Slots.Any(s => (s.HolderFlags & 2) != 0), "the backpack's slots name the local player as holder");
        // outlines over every slot the game drew, in pixels from the UI units (viewport + units x pixels per unit)
        var latest = snap;
        var sub = Ui.Values.SubscribeSlots(r =>
        {
            if (r.Snapshot is { } s) latest = s;
        });
        Action<UiDraw> draw = ui =>
        {
            var s = latest;
            foreach (var slot in s.Slots)
            {
                if (slot.OuterUi is not { } u) continue;
                var x = s.ViewportPixels.X + u.X * s.PixelsPerUnit;
                var y = s.ViewportPixels.Y + u.Y * s.PixelsPerUnit;
                var color = slot.NativeState.Count > 0 && slot.NativeState[0] != 0 ? new Color(255, 200, 0) : new Color(0, 200, 255);
                ui.DrawRect(x, y, x + u.Width * s.PixelsPerUnit, y + u.Height * s.PixelsPerUnit, color, 0, filled: false);
            }
        };
        Ui.Frame += draw;
        c.OnEnd(() =>
        {
            Ui.Frame -= draw;
            sub.Dispose();
        });
        c.Instruct("Thin blue outlines are drawn where Kiln thinks each slot is. Remember: (1) do they sit exactly on the slots " +
                   "(no offset, same size), and does a slot's outline turn yellow when you hover it with the mouse? (2) Use a " +
                   "gamepad (or the keyboard's navigation) to move the selection between slots: does the yellow outline follow " +
                   "that selection too? (3) Change the UI scale to another value and open the inventory again: do the outlines " +
                   "still sit on the slots? Then open a chest next to you; the check goes on by itself once the chest's slots show.");
        VisibleSlotsResult chest = new(false, "not read", null);
        c.Expect(await c.Until(() => (chest = Ui.Values.Slots()).Snapshot?.Slots.Any(s => (s.HolderFlags & 1) != 0 && (s.HolderFlags & 2) == 0) ?? false,
            TimeSpan.FromMinutes(5), "a chest's slots on screen"),
            "with a chest open, some slots name a holder that is not the local player", chest.Reason);
        c.Instruct("Move an item from the backpack into the chest. Pass only if all of these held: (1) the outlines sat exactly " +
                   "on the slots and turned yellow on hover, (2) the yellow outline followed the gamepad or keyboard selection, " +
                   "(3) the outlines still sat on the slots at the other UI scale, (4) outlines sit on the chest's slots too and " +
                   "the moved item's outline moved with it. Otherwise Fail with a note naming which one (and what you saw for (2)).");
        await c.Verdict();
    }
}
