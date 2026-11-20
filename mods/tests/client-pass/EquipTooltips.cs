// SPDX-License-Identifier: MIT
using Kiln;
using Kiln.Testing;
using static Kiln.Testing.ClientCheckTags;

namespace ClientPass;

// Equipment wave P8b: Ui.Tooltip, lines and stat numbers inside the game's own item detail view (capabilities
// ui.tooltip.lines and ui.tooltip.values; on for every player since 0.2.0-dev.30, these checks confirm them in a real game).
// equip.c5 lines inside the scrolled content, equip.c6 wrapping and UI scale, equip.c7 colours, equip.c8 screens and the
// AfterStats place, equip.c9 the stat numbers scaled. Each check registers its own providers, removes them at its end and
// expects that the game called them. Orders 7000-7009.
public sealed partial class ClientPassMod
{
    private const string TooltipArea = "Tooltip lines and values";

    private static bool IsGear(in UiItemView v) => v.Item.Value != 0 && v.Count <= 1;

    [ClientCheck("equip.c5", "Kiln lines inside the item detail's scrolled content", Area = TooltipArea, Since = Dev29,
        Tags = [Visual], Needs = ClientCheckNeeds.Hosted, Order = 7000)]
    public async Task TooltipLinesInside(CheckContext c)
    {
        if (!await NeedOpen(c, Ui.Tooltip.LinesCapability)) return;
        var many = false;
        int calls = 0;
        var reg = Ui.Tooltip.AddLines((in UiValueContext x, in UiItemView v) =>
        {
            if (!IsGear(v)) return UiTooltipLines.None;
            Interlocked.Increment(ref calls);
            return many
                ? UiTooltipLines.With(Enumerable.Range(1, 16).Select(i => new TooltipLine($"TOOLTIP-LINE-{i:00}")).ToArray())
                : UiTooltipLines.With(new TooltipLine("TOOLTIP-SHORT"));
        });
        c.OnEnd(() => reg.Dispose());
        c.Expect(reg.Ok, $"the lines provider registered ({reg.Reason})");
        c.Instruct("Host a world, open the backpack and select a sword so its detail view shows. Press continue.");
        await c.Verdict();
        c.Expect(await c.Until(() => Volatile.Read(ref calls) > 0 && Ui.Tooltip.InGame, TimeSpan.FromSeconds(60),
            "the game drew the sword's detail view through the provider"), "the provider was called and lines are drawn in the game's view");
        c.Instruct("Under the last line of the sword's detail view there should be one line TOOLTIP-SHORT, inside the panel, " +
                   "covering nothing. Is that what you see?");
        await c.Verdict();
        many = true;
        c.Instruct("Select another item and the sword again: now 16 lines TOOLTIP-LINE-01 .. TOOLTIP-LINE-16 follow the " +
                   "vanilla lines. Scroll the panel with the wheel to the top and to the bottom: are the vanilla stats, the " +
                   "description and all 16 lines readable and reachable, and does the scrollbar follow?");
        await c.Verdict();
        c.Instruct("Select an item with a long description (a legendary weapon). Are its whole description and the 16 lines " +
                   "reachable by scrolling?");
        await c.Verdict();
    }

    [ClientCheck("equip.c6", "Long tooltip lines wrap at the panel's width, also after a UI scale change", Area = TooltipArea,
        Since = Dev29, Tags = [Visual], Needs = ClientCheckNeeds.Hosted, Order = 7001)]
    public async Task TooltipLinesWrap(CheckContext c)
    {
        if (!await NeedOpen(c, Ui.Tooltip.LinesCapability)) return;
        var english = "WRAP-EN-BEGIN " + string.Join(" ", Enumerable.Range(1, 60).Select(i => $"word{i}")) + " WRAP-EN-END";
        var german = "WRAP-DE-BEGIN Größenänderung der Ausrüstung, Übergrößenträgerin, Maßkrüge, Füllstandsanzeige ÄÖÜäöüß WRAP-DE-END";
        int calls = 0;
        var reg = Ui.Tooltip.AddLines((in UiValueContext x, in UiItemView v) =>
        {
            if (!IsGear(v)) return UiTooltipLines.None;
            Interlocked.Increment(ref calls);
            return UiTooltipLines.With(new TooltipLine(english), new TooltipLine(german));
        });
        c.OnEnd(() => reg.Dispose());
        c.Instruct("Host a world, open the backpack and select a sword so its detail view shows. Press continue.");
        await c.Verdict();
        c.Expect(await c.Until(() => Volatile.Read(ref calls) > 0, TimeSpan.FromSeconds(60), "the provider was called"),
            "the provider was called");
        c.Instruct("Two long lines (WRAP-EN-BEGIN .. WRAP-EN-END and WRAP-DE-BEGIN .. WRAP-DE-END) should wrap at the panel's " +
                   "width, no text past the border, the German letters drawn. Move between two items and back quickly: is " +
                   "there never a frame with a line running out of the panel?");
        await c.Verdict();
        c.Instruct("Settings > Interface: change the UI scale (or the resolution) once, come back to the sword. Do both lines " +
                   "still wrap inside the panel and scroll to their end?");
        await c.Verdict();
    }

    [ClientCheck("equip.c7", "Tooltip line colours follow the game's own palette", Area = TooltipArea, Since = Dev29,
        Tags = [Visual], Needs = ClientCheckNeeds.Hosted, Order = 7002)]
    public async Task TooltipLineColours(CheckContext c)
    {
        if (!await NeedOpen(c, Ui.Tooltip.LinesCapability)) return;
        int calls = 0;
        var reg = Ui.Tooltip.AddLines((in UiValueContext x, in UiItemView v) =>
        {
            if (!IsGear(v)) return UiTooltipLines.None;
            Interlocked.Increment(ref calls);
            return UiTooltipLines.With(
                new TooltipLine("COLOUR Common", TooltipColor.Common), new TooltipLine("COLOUR Uncommon", TooltipColor.Uncommon),
                new TooltipLine("COLOUR Rare", TooltipColor.Rare), new TooltipLine("COLOUR Epic", TooltipColor.Epic),
                new TooltipLine("COLOUR Legendary", TooltipColor.Legendary), new TooltipLine("COLOUR Unique", TooltipColor.Unique),
                new TooltipLine("COLOUR Positive +12 %", TooltipColor.Positive), new TooltipLine("COLOUR Negative -5 %", TooltipColor.Negative),
                new TooltipLine("COLOUR Muted", TooltipColor.Muted), new TooltipLine("COLOUR small text", Small: true));
        });
        c.OnEnd(() => reg.Dispose());
        c.Instruct("Host a world, open the backpack and select a sword so its detail view shows. Press continue.");
        await c.Verdict();
        c.Expect(await c.Until(() => Volatile.Read(ref calls) > 0, TimeSpan.FromSeconds(60), "the provider was called"),
            "the provider was called");
        c.Instruct("Do the lines COLOUR Common .. COLOUR Unique have the game's own rarity colours (compare with items of each " +
                   "rarity)? Is COLOUR Positive green and COLOUR Negative in the text's own colour? Is COLOUR small text smaller?");
        await c.Verdict();
    }

    [ClientCheck("equip.c8", "Tooltip lines on each detail screen, and the place after the stats", Area = TooltipArea,
        Since = Dev29, Tags = [Visual], Needs = ClientCheckNeeds.Hosted, Order = 7003)]
    public async Task TooltipLinesScreens(CheckContext c)
    {
        if (!await NeedOpen(c, Ui.Tooltip.LinesCapability)) return;
        int calls = 0;
        var end = Ui.Tooltip.AddLines((in UiValueContext x, in UiItemView v) =>
        {
            Interlocked.Increment(ref calls);
            return UiTooltipLines.With(new TooltipLine("PLACE-END"));
        });
        var after = Ui.Tooltip.AddLines((in UiValueContext x, in UiItemView v) => UiTooltipLines.With(new TooltipLine("PLACE-AFTER-STATS")),
            TooltipPlace.AfterStats);
        c.OnEnd(() =>
        {
            end.Dispose();
            after.Dispose();
        });
        c.Instruct("Host a world, open the backpack and select a sword. PLACE-AFTER-STATS should sit between the stats and the " +
                   "description (the description moves down), PLACE-END after the last line, nothing overlapping. Is that so?");
        await c.Verdict();
        c.Expect(Volatile.Read(ref calls) > 0, "the providers were called");
        c.Instruct("Select a chest piece, a ring, a stack of wood and a potion; at a crafting station a recipe's output; in a " +
                   "chest an item; at a trader an item. Where the two lines are missing, they are in Kiln's Item details window " +
                   "(pause menu). Say on which screens the lines show in the game's own panel.");
        await c.Verdict();
    }

    [ClientCheck("equip.c9", "The item detail's stat numbers scaled on this client", Area = TooltipArea, Since = Dev29,
        Tags = [Visual], Needs = ClientCheckNeeds.Hosted, Order = 7004)]
    public async Task TooltipValuesScaled(CheckContext c)
    {
        if (!await NeedOpen(c, Ui.Tooltip.ValuesCapability)) return;
        int calls = 0;
        var reg = Ui.Tooltip.AddValues((in UiValueContext x, in UiItemView v) =>
        {
            if (!IsGear(v)) return UiTooltipScale.None;
            Interlocked.Increment(ref calls);
            return UiTooltipScale.By(2f);
        });
        c.OnEnd(() => reg.Dispose());
        c.Instruct("Host a world, open the backpack and select a weapon. Note its damage number, then press continue.");
        await c.Verdict();
        c.Expect(await c.Until(() => Volatile.Read(ref calls) > 0, TimeSpan.FromSeconds(60), "the values provider was called"),
            "the values provider was called");
        c.Instruct("Select another item and the weapon again: its damage number (and the other numbers of its stats block) " +
                   "should show doubled, and the better/worse styling should still make sense. Is that so?");
        await c.Verdict();
        reg.Dispose();
        c.Instruct("The provider is gone. Select another item and the weapon again: is the number back to the game's own?");
        await c.Verdict();
    }
}
