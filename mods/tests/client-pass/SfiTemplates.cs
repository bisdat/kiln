// SPDX-License-Identifier: MIT
using Kiln;
using Kiln.Testing;
using static Kiln.Testing.ClientCheckTags;

namespace ClientPass;

// Kiln's interpreter: template components of ShroudForge-format mods (since 0.2.0-dev.30.3). runtime.ecs.get_template_component
// on kiln.world's ecs_registration and the loaded template resource, its read_fields and write_field, and the template
// override target (runtime.overrides.get_template_claim, runtime.overrides.template). The mod is SF Survival, whose
// "No Fall Damage" setting changes the fall damage of the player's template and puts the template's own values back when
// it is switched off. Kiln mode applies the page's settings at the next start, so the check runs over three starts
// (SfiSettingOnOff in SfiNamedServices.cs). docs/kiln/client-pass.md, "Kiln mode's checks since 0.2.0-dev.30.3". Order 3915.
public sealed partial class ClientPassMod
{
    private const string SfiTemplateSince = "0.2.0-dev.30.3";
    private const string SfiTemplatePart = "sfi.template-components";

    [ClientCheck("sfi.template-components", "A ShroudForge-format mod changes a template component in a kiln start (SF Survival's No Fall Damage), and puts it back",
        Area = SfiArea, Since = SfiTemplateSince, Tags = [Visual], Needs = ClientCheckNeeds.Hosted, Order = 3915)]
    public async Task SfiTemplateComponents(CheckContext c)
    {
        if (await NeedKilnStart(c) is not { } s) return;
        if (!await NeedPlayer(c)) return;
        if (s.Package(SfiSurvival) is not { Intent: "selected", Skip: null })
        {
            var now = s.Package(SfiSurvival) is { } p ? "it is " + p.State + " in this start" : "this start's record does not list it";
            c.Instruct("This check needs SF Survival running in this kiln start: switch it on in the launcher and start again " +
                       $"({now}). Use a copied world.");
            await c.Skip("needs SF Survival (sf-survival) running in a kiln start");
            return;
        }
        c.Expect(Capabilities.Is("world.ecs_read"), "world.ecs_read is available in this game", Capabilities.Why("world.ecs_read"));
        await SfiSettingOnOff(c, s, SfiTemplatePart, "No Fall Damage",
            "Without the glider, drop from a height that normally costs health (a tall cliff or a tower): you should take no " +
            "fall damage.",
            "Drop from the same height again: you take fall damage as the game normally does (the mod put the template's " +
            "own values back).",
            "");
    }
}
