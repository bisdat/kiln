// SPDX-License-Identifier: MIT
using Kiln;
using Kiln.Testing;
using static Kiln.Testing.ClientCheckTags;

namespace ClientPass;

// dev31 05b-mod-stats (/mnt/Dongus/kiln-wave/dev31/out/05b-mod-stats/DESIGN.md): mods' own character values, shown from
// the player's synced copy (ModValues.Synced, ModValues.Format) by the sample "fishing" (mods/samples/ModValues). The
// check needs the sample: without it the check is skipped (fails closed), never passed. Rows stats.sync, stats.ui.
// Orders 2420-2429.
public sealed partial class ClientPassMod
{
    private const string StatsArea = "Mods' own character values";
    private static readonly ModValueKey FishXp = new("fishing", "xp"), FishTokens = new("fishing", "tokens");

    [ClientCheck("05b-stats-ui", "A mod's own XP and currency: live values, exact numbers above 2^53, cleared on leaving", Area = StatsArea,
        Since = Dev31, Tags = [Visual], Needs = ClientCheckNeeds.Hosted, Order = 2420)]
    public async Task ModValuesOnScreen(CheckContext c)
    {
        if (!await Need(c, "fishing") || !await NeedOpen(c, "stats.sync", "stats.ui")) return;
        c.Instruct("Host a world (or join one whose server runs Kiln with the fishing sample). Wait until you are in the world. Press continue.");
        await c.Verdict();
        c.Expect(await c.Until(() => ModValues.Synced(FishXp).Ok, TimeSpan.FromSeconds(60),
            "the server's stream brought the fishing values"), "the synced copy arrived");
        // the tokens have the default policies (OwnerOnly, OwnerMod): only the fishing mod itself reads them, this mod is refused
        c.Expect(ModValues.Synced(FishTokens).Reason == ModValueReasons.AccessDenied, "another mod cannot read the sample's tokens",
            ModValues.Synced(FishTokens).Reason);
        c.Instruct("Press F9: the Fishing window opens. The HUD (top right) shows a line \"Fishing Lv ... | ... XP | ... tokens\". " +
                   "Type /fishing-catch in chat twice. Press continue.");
        await c.Verdict();
        c.Expect(await c.Until(() => ModValues.Synced(FishXp) is { Ok: true, Value.Value: >= 50 }, TimeSpan.FromSeconds(30), "two catches"),
            "the XP went up by 25 per catch on the client's copy");
        if (ModValues.Synced(FishXp).Value is { } xp)
        {
            c.Instruct($"The HUD line and the window should start with exactly \"Fishing Lv {xp.Level} | {ModValues.Format(xp)} XP | \" and show " +
                       "your tokens after it (the HUD line is drawn as the fishing mod: its tokens are visible to it alone). Is that what you see?");
            await c.Verdict();
        }
        c.Instruct("Click \"Buy bait\" in the Fishing window (or type /fishing-buy-bait). You get a message; if the bait item " +
                   "could not be given, the tokens come back. Do the tokens in the HUD drop by 5 (or come back with the message saying so)?");
        await c.Verdict();
        c.Instruct("Type /fishing-big (you are the host, so you may). Press continue.");
        await c.Verdict();
        c.Expect(await c.Until(() => ModValues.Synced(FishXp) is { Ok: true, Value.Value: 9007199254740993 }, TimeSpan.FromSeconds(30), "XP 9007199254740993"),
            "a value above 2^53 arrived exactly", ModValues.Synced(FishXp).Value?.Value.ToString());
        if (ModValues.Synced(FishXp).Value is { } big)
        {
            c.Instruct($"The XP should read \"{ModValues.Format(big)}\" (ending in ...993, not ...992 or ...994). Is it exactly that, in the HUD and the window?");
            await c.Verdict();
        }
        c.Instruct("Leave the world to the main menu (keep this check open). Press continue when you are at the menu.");
        await c.Verdict();
        c.Expect(await c.Until(() => !ModValues.Synced(FishXp).Ok, TimeSpan.FromSeconds(30), "the copy cleared"),
            "leaving the world clears the synced copy", ModValues.Synced(FishXp).Reason);
        c.Instruct("Is the fishing HUD line gone, and does the window say there are no fishing values?");
        await c.Verdict();
    }
}
