// SPDX-License-Identifier: MIT
using Kiln;
using Kiln.Testing;
using static Kiln.Testing.ClientCheckTags;

namespace ClientPass;

// Kiln's interpreter: the world reads (since 0.2.0-dev.30.3). Kiln's interpreter reads the local player's building
// cursor (runtime.world.cursor.get) and the props of an area (runtime.world.entity.register_prop_recipes,
// query_props_in_bounds, get_transform) and the entities and components (runtime.ecs) through Kiln's world reads. The
// checks need a ShroudForge-format test mod that logs what those calls answer, the kiln-mode probe
// (mods/tests/sf-kiln-probe; its lines are listed in its README); they are guided: the interpreter's record and kiln.log
// are asserted (Sfi.cs's NeedKilnStart), the rest is your verdict. Not part of kiln-sfi.interpreter's row.
// docs/kiln/client-pass.md, "Kiln mode's checks since 0.2.0-dev.30.3". Orders 3908-3910.
public sealed partial class ClientPassMod
{
    private const string SfiWorldSince = "0.2.0-dev.30.3";
    private const string SfiProbe = "the kiln-mode probe (copy mods/tests/sf-kiln-probe into <game>/mods and switch it on; " +
                                    "it logs lines with \"sf-kiln-probe:\" every 30 seconds and when you press its shortcut, " +
                                    "F5, or its Run probes now button on Kiln's ShroudForge page)";

    [ClientCheck("sfi.world-cursor", "A ShroudForge mod in kiln mode reads the building cursor while you aim, hosting and joined",
        Area = SfiArea, Since = SfiWorldSince, Tags = [Visual], Needs = ClientCheckNeeds.Hosted, Order = 3908)]
    public async Task SfiWorldCursor(CheckContext c)
    {
        if (await NeedKilnStart(c) is not { } s) return;
        var logStart = BootLog(c, "kiln.log")?.Length ?? 0;
        c.OnEnd(() =>
        {
            var nil = (BootLog(c, "kiln.log") ?? []).Skip(Math.Max(0, logStart - 1))
                .Where(l => l.Contains("attempt to call a nil value", StringComparison.Ordinal)).ToArray();
            c.Expect(nil.Length == 0, "kiln.log: no \"attempt to call a nil value\" line while the check ran", nil.FirstOrDefault());
        });
        c.Instruct("This check needs a ShroudForge-format test mod running in this kiln start that logs what " +
                   "runtime.world.cursor.get() answers about once a second: its sequence, value.primaryTransform.position, " +
                   $"value.attachedToProp, or its reason: {SfiProbe}, whose \"world.cursor\" lines hold sequence, x, y, z " +
                   $"and attachedToProp (this start's mods: {SfiList(s)}). On a COPIED world, take a building " +
                   "tool and aim at the ground; move the cursor slowly, stop, then pick up a placed prop with the building tool " +
                   "and put it back. Pass only if all of these held in kiln.log: (1) the sequence grows with every answer, " +
                   "(2) the position follows the cursor and holds while it stands still, (3) attachedToProp is true only while " +
                   "the prop hangs on the cursor, (4) once you were in the world no answer was a reason (while it was loading, " +
                   "\"local cursor is waiting for the current world session\" and \"local cursor world session changed or " +
                   "is unavailable\" are fine). If you can, also join a friend's world with the same " +
                   "mod and say in the note whether (1) to (3) held there. Otherwise fail with a note naming which one and the " +
                   "reason the mod logged. If no such mod runs, press skip.");
        await c.Verdict();
    }

    [ClientCheck("sfi.world-props", "A ShroudForge mod in kiln mode finds the props of an area of a base, with their transforms",
        Area = SfiArea, Since = SfiWorldSince, Tags = [Visual], Needs = ClientCheckNeeds.Hosted, Order = 3909)]
    public async Task SfiWorldProps(CheckContext c)
    {
        if (await NeedKilnStart(c) is not { } s) return;
        c.Instruct("This check needs a ShroudForge-format test mod running in this kiln start that, on its action button, " +
                   "registers prop recipes for the items of your base (runtime.world.entity.register_prop_recipes) and logs " +
                   "what runtime.world.entity.query_props_in_bounds answers for a box around you (the count and, for the " +
                   "first result, runtime.world.entity.get_transform's position), or the reason: " + SfiProbe + ", whose " +
                   "\"world.props.before\", \"world.props.recipes\", \"world.props.query\" (count) and " +
                   "\"world.props.transform\" (inside=true) lines say it " +
                   $"(this start's mods: {SfiList(s)}). On a COPIED world, stand in a base with a few props (chests, " +
                   "lights, furniture). Press the mod's button once; place one more prop inside the box and press it again; " +
                   "place one outside the box and press it a third time. Pass only if all of these held in kiln.log: (1) the " +
                   "first count is the number of props of the base inside the box, (2) the second is one more, (3) the third " +
                   "is the same as the second, (4) the logged position lies inside the box, (5) before its recipes were " +
                   "registered the query answered \"native recipe-bounds prop query failed\" (if the mod logs that). " +
                   "Otherwise fail with a note naming which one and what the mod logged. If no such mod runs, press skip.");
        await c.Verdict();
    }

    [ClientCheck("sfi.world-ecs", "A ShroudForge mod in kiln mode queries entities and reads their components, hosting and joined",
        Area = SfiArea, Since = SfiWorldSince, Tags = [Visual], Needs = ClientCheckNeeds.Hosted, Order = 3910)]
    public async Task SfiWorldEcs(CheckContext c)
    {
        if (await NeedKilnStart(c) is not { } s) return;
        c.Instruct("This check needs " + SfiProbe + $" running in this kiln start (this start's mods: {SfiList(s)}). On a " +
                   "COPIED world you host, wait for one probe run (or press F5) and read its lines in kiln.log. Pass only if " +
                   "all of these held: (1) \"ecs.catalog\" ok=true with reads_ready=true, (2) \"ecs.query\" and " +
                   "\"ecs.players\" ok=true with a count above 0, (3) \"ecs.read_bytes\" ok=true with size=56 and a position " +
                   "near you, (4) \"ecs.read_fields\" ok=true, (5) \"ecs.resolve\" ok=true with same=true when a prop was " +
                   "found. \"ecs.get_template\" may answer a reason in this release; put its reason in the note. If you can, " +
                   "turn on the probe's \"Allow writes (copied world only)\" setting in an earlier start, press F5 and say in " +
                   "the note what \"ecs.write_fields\" and \"ecs.write_fields.restore\" (restored=true) logged; also join a " +
                   "friend's world with the probe and say whether (1) to (4) held there. Otherwise fail with a note naming " +
                   "which one and its reason. Do not save this world after writes.");
        await c.Verdict();
    }
}
