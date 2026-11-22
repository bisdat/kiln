// SPDX-License-Identifier: MIT
using Kiln;
using Kiln.Testing;
using static Kiln.Testing.ClientCheckTags;

namespace ClientPass;

// Kiln's interpreter: a named service read and changed by a ShroudForge-format mod in a kiln start (since
// 0.2.0-dev.30.3). runtime.services.get and the service override target run on kiln.world's named services
// (named_service_open / _snapshot / _compare_exchange; docs/kiln/services/world.md, "Named services from tasks"); the
// mod is SF Survival, whose Infinite Weapon Durability setting sets the world settings' enableDurability to false and puts
// the world's own value back when it is switched off. The rows world.named_read and world.named_write must be available.
// With the kiln-mode probe (mods/tests/sf-kiln-probe) on too, its services.get and services.read_fields lines show the
// service and its enableDurability. Kiln mode applies the page's settings at the next start, so the check runs over three
// starts (the setting switched on, then off, each followed by a restart). docs/kiln/client-pass.md, "Kiln mode's checks since
// 0.2.0-dev.30.3". Order 3912.
public sealed partial class ClientPassMod
{
    private const string SfiNamedSince = "0.2.0-dev.30.3";
    private const string SfiSurvival = "sf-survival";
    private const string SfiNamedPart = "sfi.named-services";

    [ClientCheck("sfi.named-services", "A ShroudForge-format mod changes a world setting through a named service in a kiln start, and puts it back",
        Area = SfiArea, Since = SfiNamedSince, Tags = [Visual], Needs = ClientCheckNeeds.Hosted, Order = 3912)]
    public async Task SfiNamedServices(CheckContext c)
    {
        if (await NeedKilnStart(c) is not { } s) return;
        if (!await NeedPlayer(c)) return;
        if (s.Package(SfiSurvival) is not { Intent: "selected", Skip: null })
        {
            var now = s.Package(SfiSurvival) is { } p ? "it is " + p.State + " in this start" : "this start's record does not list it";
            c.Instruct("This check needs SF Survival running in this kiln start: switch it on in the launcher and start again " +
                       $"({now}). Use a copied world: the mod changes a setting of the whole world.");
            await c.Skip("needs SF Survival (sf-survival) running in a kiln start");
            return;
        }
        foreach (var row in new[] { "world.named_read", "world.named_write" })
            c.Expect(Capabilities.Is(row), $"{row} is available in this game", Capabilities.Why(row));
        await SfiSettingOnOff(c, s, SfiNamedPart, "Infinite Weapon Durability",
            "Take a weapon whose durability you can see, hit a tree or an enemy about ten times and look at its durability " +
            "in your inventory: it should not drop.",
            "Hit about ten more times with the same weapon: its durability drops as it normally does (the mod put the " +
            "world's own setting back).",
            "With the kiln-mode probe (mods/tests/sf-kiln-probe) on too, its \"services.get\" line answers ok=true and its " +
            "\"services.read_fields\" line shows enableDurability=false in part 2. ");
    }

    /// <summary>
    /// A check of one setting of a ShroudForge-format mod switched on and then off again, over three kiln starts: kiln mode
    /// applies a setting changed on Kiln's ShroudForge page at the next start, so each change is followed by a restart.
    /// Part 0 asks to switch the setting on and restart; part 1 (setting on) asks what the game did and to switch it off
    /// and restart; part 2 (setting off) asks what the game did and gives the verdict. A fail in part 1 starts over.
    /// </summary>
    private async Task SfiSettingOnOff(CheckContext c, SfiRecord s, string partName, string setting, string withOn,
        string withOff, string extra)
    {
        var before = SfiReadPart(c, partName);
        var key = SfiPageKey(c, out _);
        if (before.Part == 0)
        {
            SfiWritePart(c, partName, 1, s.LaunchId);
            c.Instruct($"Part 1 of 3. Use a copied world. Press {key}, open SF Survival's settings on Kiln's ShroudForge page and " +
                       $"switch \"{setting}\" on (settings apply at the next start). Then press skip, quit the game, start it " +
                       "again in kiln mode, host the same copied world and run this check again.");
            await c.Skip($"part 1 of 3: switch \"{setting}\" on, restart, and run the check again");
            return;
        }
        SfiNewStart(c, before, s);
        if (before.Part == 1)
        {
            SfiWritePart(c, partName, 2, s.LaunchId);
            c.Instruct($"Part 2 of 3 (\"{setting}\" on since this start). {withOn} Remember whether that held. Then press " +
                       $"{key}, switch \"{setting}\" off, press skip, quit, start again in kiln mode, host the same world and " +
                       "run this check again. If it did not hold, fail now with a note.");
            if (await c.Verdict() == ClientCheckVerdict.Fail) SfiWritePart(c, partName, 0, null);
            return;
        }
        SfiWritePart(c, partName, 0, null);
        c.Instruct($"Part 3 of 3 (\"{setting}\" off since this start). {withOff} Pass only if all of these held: (1) part 2 " +
                   $"did what it should with the setting on, (2) this start behaves as the game normally does, (3) SF " +
                   $"Survival's line on Kiln's ShroudForge page showed no failure (\"write-failed\") and no waiting reason in " +
                   $"either start. {extra}Otherwise fail with a note saying which one, with kiln/logs/kiln.log attached. Do " +
                   "not save this world.");
        await c.Verdict();
    }
}
