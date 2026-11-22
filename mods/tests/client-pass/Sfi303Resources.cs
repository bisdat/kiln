// SPDX-License-Identifier: MIT
using Kiln;
using Kiln.Testing;
using static Kiln.Testing.ClientCheckTags;

namespace ClientPass;

// Kiln's interpreter reading the game's loaded resources (game.assets.get_loaded_resource, get_loaded_resources_by_type,
// read_loaded_field and runtime.ecs.get_template on kiln.assets' loaded_snapshot and loaded_list, since 0.2.0-dev.30.3).
// The check asserts this start's record and kiln.log, then asks one verdict about a world played with mods that scan
// loaded resources; the kiln-mode probe (mods/tests/sf-kiln-probe) on too shows the reads in its resources.by_type,
// resources.get and ecs.get_template lines. docs/kiln/client-pass.md, "Kiln mode's checks since 0.2.0-dev.30.3".
// Order 3913.
public sealed partial class ClientPassMod
{
    private const string SfiLoadedSince = "0.2.0-dev.30.3";
    // ShroudForge mods whose first in-game step reads loaded resources or an entity's template
    private static readonly string[] SfiLoadedMods = ["crafting", "flight", "skeleton", "survival", "spiderhook", "world-editor"];

    [ClientCheck("sfi.loaded-resources", "Kiln mode reads and writes the game's loaded resources in a world; mods that change them play on",
        Area = SfiArea, Since = SfiLoadedSince, Tags = [Visual], Needs = ClientCheckNeeds.Hosted, Order = 3913)]
    public async Task SfiLoadedResources(CheckContext c)
    {
        if (await NeedKilnStart(c) is not { } s) return;
        var log = BootLog(c, "kiln.log");
        var on = log?.LastOrDefault(l => l.Contains("assets: loaded resources on (", StringComparison.Ordinal));
        c.Expect(on is not null, "kiln.log: \"assets: loaded resources on (<profile>)\" (an exact engine profile of this build)",
            on ?? log?.LastOrDefault(l => l.Contains("assets: loaded resources off", StringComparison.Ordinal)) ?? "(no line)");
        var writes = log?.LastOrDefault(l => l.Contains("assets: loaded resource writes on (", StringComparison.Ordinal));
        c.Expect(writes is not null, "kiln.log: \"assets: loaded resource writes on (<profile>)\" (the write gate of this build)",
            writes ?? log?.LastOrDefault(l => l.Contains("assets: loaded resource writes off", StringComparison.Ordinal)) ?? "(no line)");
        var missing = log?.FirstOrDefault(l => l.Contains("lacks the ResourceService", StringComparison.Ordinal));
        c.Expect(missing is null, "kiln.log: no \"... lacks the ResourceService ...\" answer in this start", missing);
        var mods = s.Packages.Where(p => p.Intent == "selected" && p.Skip is null &&
                                         SfiLoadedMods.Any(m => p.Id.Contains(m, StringComparison.OrdinalIgnoreCase)))
            .Select(p => p.Label).ToArray();
        if (mods.Length == 0)
        {
            c.Instruct("This check needs at least one of sf-crafting-essentials, sf-unlimited-flight, sf-survival or " +
                       $"skeleton-commander running in this kiln start; this start's record has none ({SfiList(s)}). Add one to " +
                       "the profile and start again.");
            await c.Skip("needs a mod that reads loaded resources running in a kiln start");
            return;
        }
        c.Instruct($"Use a copied world and host it in this kiln start ({string.Join(", ", mods)} running). Play for a minute: " +
                   "walk, open the inventory and a crafting station, look at a creature or a building piece. Then go back to " +
                   "the main menu and enter the world again. Pass only if all of these held: (1) the game did not freeze or " +
                   "crash when the world loaded, while you played, or when you left and entered it again, (2) no notice or " +
                   "kiln.log line of those mods says the ResourceService is missing (what they still cannot do, they say " +
                   "with another reason, such as an override that is not available yet), (3) what the mods change in " +
                   "items or recipes shows in the game (sf-crafting-essentials: its recipe changes at a crafting station), and " +
                   "kiln.log has an \"assets: loaded write\" line for each change, (4) after you switch those mods off and " +
                   "start again, the game's own values are back. With the kiln-mode probe (mods/tests/sf-kiln-probe) on too, its " +
                   "\"resources.by_type\" and \"resources.get\" lines answer ok=true in the world and its \"ecs.get_template\" line " +
                   "names a template; add any that does not to the note. Otherwise fail with a note naming which one and the mod.");
        await c.Verdict();
    }
}
