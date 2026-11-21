// SPDX-License-Identifier: MIT
using Kiln;
using Kiln.Testing;
using static Kiln.Testing.ClientCheckTags;

namespace ClientPass;

// Kiln's interpreter in 0.2.0-dev.30.3: the attribute writes of kiln mode (runtime.ecs.update_attribute and the
// attribute override target, runtime.overrides.attribute, with the game's own attribute formulas run before each write).
// One guided check on a real ShroudForge-format mod that uses nothing else in game for one of its settings:
// sf-unlimited-flight's "Unlimited Glider Stamina" (its override target sets StaminaDepletionMod_Glider to 0 while the
// setting is on). The restore when a mod stops, conflicts between mods and a world change are covered by the
// interpreter's host tests. Not one of the client gate's interpreter checks (Sfi.cs). With the kiln-mode probe
// (mods/tests/sf-kiln-probe) on too, its attributes.list and attributes.read lines show the attribute reads.
// docs/kiln/client-pass.md, "Kiln mode's checks since 0.2.0-dev.30.3". Order 3911.
public sealed partial class ClientPassMod
{
    private const string SfiAttributesSince = "0.2.0-dev.30.3";
    private const string SfiFlight = "sf-unlimited-flight";

    [ClientCheck("sfi.attribute-writes", "In a kiln start, sf-unlimited-flight's Unlimited Glider Stamina stops glider stamina drain",
        Area = SfiArea, Since = SfiAttributesSince, Tags = [Visual], Needs = ClientCheckNeeds.Hosted, Order = 3911)]
    public async Task SfiAttributeWrites(CheckContext c)
    {
        if (await NeedKilnStart(c) is not { } s) return;
        var flight = s.Package(SfiFlight);
        c.Expect(flight is { Skip: null, Intent: not "disabled" }, $"sfi.json: {SfiFlight} runs this start",
            flight?.State ?? "(not in this start's record: add sf-unlimited-flight to the profile and enable it)");
        var key = SfiPageKey(c, out _);
        c.Instruct("Before this start: on Kiln's ShroudForge page (" + key + ") open sf-unlimited-flight's settings, switch the " +
                   "mod on with only \"Unlimited Glider Stamina\" on (leave Upward Gliding off and the speed sliders at 100 " +
                   "percent: they need parts kiln mode does not have yet), then quit and start again (the page applies " +
                   "settings at the next start). Now, on a copied world you host (single player is fine): glide from a " +
                   "height for at least ten seconds and watch the stamina bar, then land. Pass only if both held: (1) " +
                   "gliding did not drain stamina, (2) sf-unlimited-flight's line on Kiln's ShroudForge page showed no " +
                   "error or waiting reason. If it shows \"component write context or original command phase is " +
                   "unavailable\" or \"attribute calculations are not validated for this executable profile\", fail with " +
                   "that text as the note. With the kiln-mode probe (mods/tests/sf-kiln-probe) on too, add its " +
                   "\"attributes.list\" (count) and \"attributes.read\" (name, value) lines from kiln.log to the note. Do not " +
                   "save this world.");
        await c.Verdict();
    }
}
