// SPDX-License-Identifier: MIT
using Kiln;
using Kiln.Testing;
using static Kiln.Testing.ClientCheckTags;

namespace ClientPass;

// 0.2.0-dev.30.3 (issue #111): Ui.World.Outline, an entity's model drawn with the game's own highlight (kiln.render v2,
// sigdb feature ui.world_outline; docs/kiln/render.md "Outlines", docs/re/client-render.md "Highlight groups"). The
// entity is a storage chest the player opens (Ui.Inventory.Screen's Container: the chest screen names its entity).
// Each check removes its outlines at its end. Orders 2420-2429.
public sealed partial class ClientPassMod
{
    private const string Dev303 = "0.2.0-dev.30.3";
    private const string OutlineArea = "Outlines (the game's highlight)";

    // The chest the player opens next (its entity), or EntityId 0 after the timeout.
    private static async Task<EntityId> OpenedChest(CheckContext c, string why)
    {
        c.Instruct("Host a world and open a storage chest (any chest you built). " + why + " The check goes on by itself once " +
                   "the chest screen is open.");
        EntityId chest = default;
        c.Expect(await c.Until(() =>
        {
            if (Ui.Inventory.Screen is { Kind: InventoryScreenKind.Chest } s && s.Container.Value != 0) chest = s.Container;
            return chest.Value != 0;
        }, TimeSpan.FromMinutes(3), "a chest screen naming its chest"), "the chest screen names the chest's entity");
        return chest;
    }

    private static bool NeedOutlines(CheckContext c)
    {
        var why = Capabilities.Why(Ui.World.OutlineCapability);
        return c.Expect(Ui.World.OutlinesAvailable, "ui.world_outline is available on this game build", why);
    }

    [ClientCheck("ui-outline.c1", "A chest lights up with the game's outline in gold, and goes back when the mod lets go", Area = OutlineArea,
        Since = Dev303, Tags = [Visual], Needs = ClientCheckNeeds.Hosted, Order = 2420)]
    public async Task OutlineChestGold(CheckContext c)
    {
        if (!NeedOutlines(c)) { await c.Verdict(); return; }
        var chest = await OpenedChest(c, "Kiln will outline it.");
        if (chest.Value == 0) { await c.Verdict(); return; }
        var o = Ui.World.Outline(chest, new OutlineOptions { Color = new Color(255, 196, 64) });
        c.OnEnd(o.Dispose);
        c.Instruct("Close the chest and step back so you see the whole chest. The check goes on by itself once Kiln shows the outline.");
        c.Expect(await c.Until(() => o.State == OutlineState.Shown, TimeSpan.FromMinutes(1), "state Shown"),
            "the outline is applied to the chest's model", $"state {o.State}, reason {o.Reason}");
        c.Instruct("Remember (1): is the chest drawn with a gold glow or outline, the same kind of highlight the game uses when " +
                   "you aim at a building piece with the build tool, and nothing else around it? Walk around it: (2) does it stay " +
                   "on the chest from every side, and (3) is it hidden where a wall or another object stands in front of the " +
                   "chest (no through-walls)? Kiln removes the outline by itself in 40 seconds.");
        await c.Delay(TimeSpan.FromSeconds(40));
        o.Dispose();
        c.Expect(o.State == OutlineState.Ended && o.Reason == OutlineReason.Removed, "Dispose ends it (Removed)", $"{o.State}/{o.Reason}");
        await c.Delay(TimeSpan.FromSeconds(2));
        c.Instruct("The outline was removed. Pass only if: (1) a gold highlight was on the chest, (2) from every side, (3) hidden " +
                   "behind walls and objects, and (4) now the chest looks as it did before (no highlight left). Otherwise Fail " +
                   "with a note naming which one.");
        await c.Verdict();
    }

    [ClientCheck("ui-outline.c2", "Two colours at once, one pulsing; a higher priority on the same chest wins, then hands back", Area = OutlineArea,
        Since = Dev303, Tags = [Visual], Needs = ClientCheckNeeds.Hosted, Order = 2421)]
    public async Task OutlineColours(CheckContext c)
    {
        if (!NeedOutlines(c)) { await c.Verdict(); return; }
        var first = await OpenedChest(c, "Kiln will outline it green, pulsing. Have a second chest nearby.");
        if (first.Value == 0) { await c.Verdict(); return; }
        var green = Ui.World.Outline(first, new OutlineOptions { Color = new Color(70, 255, 110), Pulse = true });
        c.OnEnd(green.Dispose);
        c.Instruct("Close it, then open a SECOND chest (a different one). The check goes on by itself.");
        EntityId second = default;
        c.Expect(await c.Until(() =>
        {
            if (Ui.Inventory.Screen is { Kind: InventoryScreenKind.Chest } s && s.Container.Value != 0 && s.Container != first)
                second = s.Container;
            return second.Value != 0;
        }, TimeSpan.FromMinutes(3), "a second chest"), "a second chest was opened");
        if (second.Value == 0) { await c.Verdict(); return; }
        var blue = Ui.World.Outline(second, new OutlineOptions { Color = new Color(60, 140, 255) });
        c.OnEnd(blue.Dispose);
        c.Instruct("Close it and stand where you see both chests. Remember (1): is the first chest green and pulsing (brighter " +
                   "and darker about once a second) and the second one steady blue? The check goes on in 30 seconds.");
        await c.Delay(TimeSpan.FromSeconds(30));
        var red = Ui.World.Outline(second, new OutlineOptions { Color = new Color(255, 40, 40), Priority = 5 });
        c.OnEnd(red.Dispose);
        await c.Ticks(10);
        c.Expect(blue.Reason == OutlineReason.Outshone, "a higher priority on the same chest outshines the first request", $"{blue.State}/{blue.Reason}");
        c.Instruct("Remember (2): a second request with a higher priority (red) is on the second chest now: is it steady red, " +
                   "and the first chest still green and pulsing? The check goes on in 20 seconds.");
        await c.Delay(TimeSpan.FromSeconds(20));
        red.Dispose();
        await c.Ticks(10);
        c.Expect(blue.State == OutlineState.Shown, "the outshone request shows again when the winner goes", $"{blue.State}/{blue.Reason}");
        c.Instruct("Remember (3): is the second chest blue again? The check removes everything in 15 seconds.");
        await c.Delay(TimeSpan.FromSeconds(15));
        green.Dispose();
        blue.Dispose();
        await c.Delay(TimeSpan.FromSeconds(2));
        c.Instruct("Pass only if: (1) green pulsing and steady blue at once, (2) the second chest red, the first still green, " +
                   "(3) blue again after the red went, and (4) both chests without a highlight now. Otherwise Fail with a note.");
        await c.Verdict();
    }

    [ClientCheck("ui-outline.c3", "The game's building highlight goes first, Kiln's outline comes back after it", Area = OutlineArea,
        Since = Dev303, Tags = [Visual], Needs = ClientCheckNeeds.Hosted, Order = 2422)]
    public async Task OutlineGameFirst(CheckContext c)
    {
        if (!NeedOutlines(c)) { await c.Verdict(); return; }
        var chest = await OpenedChest(c, "Kiln will outline it gold, then you aim at it with the build tool.");
        if (chest.Value == 0) { await c.Verdict(); return; }
        var o = Ui.World.Outline(chest, new OutlineOptions());
        c.OnEnd(o.Dispose);
        c.Instruct("Close the chest. Take the building tool (the Construction Hammer) and aim at the chest so the game highlights " +
                   "it (blue, or red where it cannot be removed). The check goes on by itself once the game's highlight is on it.");
        c.Expect(await c.Until(() => o.State == OutlineState.GameHighlight, TimeSpan.FromMinutes(3), "state GameHighlight"),
            "Kiln sees the game's own highlight and steps aside", $"{o.State}/{o.Reason}");
        c.Instruct("Remember (1): does the chest show the game's own blue (or red) highlight, not Kiln's gold? Now aim away from " +
                   "the chest and put the tool away. The check goes on by itself once Kiln's outline is back.");
        c.Expect(await c.Until(() => o.State == OutlineState.Shown, TimeSpan.FromMinutes(2), "state Shown again"),
            "Kiln's outline comes back after the game's highlight", $"{o.State}/{o.Reason}");
        c.Instruct("Remember (2): is the chest gold again (and not stuck in the game's blue)? Kiln removes the outline in 20 seconds.");
        await c.Delay(TimeSpan.FromSeconds(20));
        o.Dispose();
        await c.Delay(TimeSpan.FromSeconds(2));
        c.Instruct("Pass only if (1) the game's own highlight showed while you aimed at the chest, (2) gold came back after it, " +
                   "and (3) the chest has no highlight now. If the game's blue stayed on the chest after you looked away, Fail " +
                   "and say so (docs/re/client-render.md, 'Open').");
        await c.Verdict();
    }

    [ClientCheck("ui-outline.c4", "Leaving the world ends every outline; nothing stays on the chest in the next game", Area = OutlineArea,
        Since = Dev303, Tags = [Visual], Needs = ClientCheckNeeds.Hosted, Order = 2423)]
    public async Task OutlineWorldEnds(CheckContext c)
    {
        if (!NeedOutlines(c)) { await c.Verdict(); return; }
        var chest = await OpenedChest(c, "Kiln will outline it, then you go back to the main menu and load the world again.");
        if (chest.Value == 0) { await c.Verdict(); return; }
        var o = Ui.World.Outline(chest, new OutlineOptions { Color = new Color(255, 196, 64), Duration = TimeSpan.FromMinutes(30) });
        c.OnEnd(o.Dispose);
        c.Expect(await c.Until(() => o.State == OutlineState.Shown, TimeSpan.FromMinutes(1), "state Shown"), "shown", $"{o.State}/{o.Reason}");
        c.Instruct("Close the chest, check it is gold, then quit to the main menu (Esc > Quit to main menu) and load the same world " +
                   "again. The check goes on by itself once the outline has ended.");
        c.Expect(await c.Until(() => o.State == OutlineState.Ended, TimeSpan.FromMinutes(5), "ended"),
            "the outline ends with the client world", $"{o.State}/{o.Reason}");
        c.Expect(o.Reason == OutlineReason.WorldEnded, "its reason is WorldEnded", o.Reason.ToString());
        c.Instruct("Back in the world, go to the chest. Pass only if it has no highlight (Kiln's outline did not come back on its " +
                   "own and nothing is stuck on it). Otherwise Fail with a note.");
        await c.Verdict();
    }
}
