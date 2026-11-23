// SPDX-License-Identifier: MIT
using Kiln;
using Kiln.Testing;
using static Kiln.Testing.ClientCheckTags;

namespace ClientPass;

// dev31 09-ui-blocks (/mnt/Dongus/kiln-wave/dev31/out/09-ui-blocks/DESIGN.md): the richer UI blocks, played on the
// UiBlocks sample's Camp window (mods/samples/UiBlocks/blocks/camp.json, Ctrl+F9). Without the sample the check is
// skipped (fails closed), never passed. The joined-server and vanilla-server steps are in CLIENT-CHECKS.md (L and W).
// Orders 2430-2439.
public sealed partial class ClientPassMod
{
    private const string UiBlocksArea = "UI blocks";

    [ClientCheck("09-ui-blocks", "UI blocks: Rich rows and joined commands", Area = UiBlocksArea,
        Since = Dev31, Tags = [Visual], Needs = ClientCheckNeeds.Hosted, Order = 2430)]
    public async Task RichUiBlocks(CheckContext c)
    {
        if (!await Need(c, "ui-blocks") || !await NeedOpen(c, "ui.blocks.input", "ui.blocks.tables", "ui.blocks.state", "ui.blocks.bindings")) return;
        c.Expect(Capabilities.Is("ui.blocks.tabs") && Capabilities.Is("ui.blocks.trees"), "kiln.ui.blocks is here (tabs and trees follow their state)");
        c.Instruct("Host a world and wait until you are in it. Press Ctrl+F9 (or choose \"Camp (UI blocks sample)\" in the pause menu): the Camp window opens. Press Ctrl+F9 again: it closes; once more: it opens. Press continue.");
        await c.Verdict();
        c.Instruct("Click the Note field and type \"Grüße, ça va? 你好\", then Backspace twice, then Enter. " +
                   "Does the field keep \"Grüße, ça va? \" (the two Chinese characters gone), and did nothing you typed appear in the game's chat or move your character?");
        await c.Verdict();
        c.Instruct("Click the Note field, type more, then press Escape. Is the text back to what it was before you clicked? Then press Tab in the field: does the focus move on (no tab character)?");
        await c.Verdict();
        c.Instruct("Close Camp (Ctrl+F9). Start walking (hold W), and while you still hold W press Ctrl+F9: Camp opens and takes the keyboard. Now let go of W. " +
                   "Does your character stop (no key stuck)? Close Camp with Ctrl+F9 and walk a step to be sure.");
        await c.Verdict();
        c.Instruct("Open Camp again. Click the tabs Bag, Quests, Map and Picture in turn. Bag lists your inventory (item, count, page/slot); Quests lists three quests; " +
                   "Map shows \"Eastern hills\" open with \"Old mill\" and \"Crystal cave\" under it and \"danger N\" lines; Picture shows a small orange flame. Is all of that so?");
        await c.Verdict();
        c.Instruct("In Quests click \"Drive off the wolves\": the line \"Selected: wolves\" appears. In Map close \"Eastern hills\" and open \"Western marsh\". Type a word in Note and press Enter. " +
                   "Close Camp with Ctrl+F9 (it disappears), then open it again with Ctrl+F9. Are the note, the selected quest, the selected tab and the open branches as you left them?");
        await c.Verdict();
        c.Instruct("Check the top part: \"Health a/b\" and \"Stamina\" match the game's own bars, the position changes as you walk, \"slots used\" changes when you pick up or drop an item, " +
                   "and \"World hour\" shows a number (you host this world). Is that so? (The step number in the position line goes up as the world runs.)");
        await c.Verdict();
        c.Instruct("Click \"Rest at the fire\". A toast says \"You rest by the fire (N rests).\" and \"Rests at the fire\" goes up by 1 within a second or two. Is that so?");
        await c.Verdict();
        c.Expect(Capabilities.Is("ui.command.server"), "the hosted server's kiln:ui-command said hello (ui.command.server on)");
        c.Instruct("Leave the world to the main menu and host it again; open the Camp window. Are the note empty, no quest selected and only \"Eastern hills\" open again (state reset in a new world)?");
        await c.Verdict();
    }
}
