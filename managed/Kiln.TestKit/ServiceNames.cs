// SPDX-License-Identifier: MIT
namespace Kiln.TestKit;

/// <summary>The native service tables Kiln's core publishes, by name: the <c>KILN_*_SERVICE</c> names of
/// every header under <c>native/</c>. Embedded, since a mod author's machine has no Kiln repository; a kit test compares
/// the list with the headers and fails when they differ.</summary>
internal static class ServiceNames
{
    // One name per line, sorted: a package that adds a service adds one line, and two such additions merge.
    public static readonly IReadOnlyList<string> All =
    [
        "kiln.abilities",
        "kiln.assets",
        "kiln.assets.vfx",
        "kiln.audio",
        "kiln.boot",
        "kiln.build",
        "kiln.caps",
        "kiln.characters",
        "kiln.chat",
        "kiln.client",
        "kiln.combat",
        "kiln.component_changes",
        "kiln.content",
        "kiln.craftmenu",
        "kiln.creature_control",
        "kiln.creatures",
        "kiln.decisions",
        "kiln.eml",
        "kiln.engine",
        "kiln.example",
        "kiln.flame",
        "kiln.gamedata",
        "kiln.gameplay",
        "kiln.gemcraft",
        "kiln.hooks",
        "kiln.items",
        "kiln.knowledge",
        "kiln.log",
        "kiln.mod-values",
        "kiln.mod-values.backend",
        "kiln.net",
        "kiln.npcai",
        "kiln.pacing",
        "kiln.playeranim",
        "kiln.players",
        "kiln.playerstate",
        "kiln.profile",
        "kiln.progress",
        "kiln.progression",
        "kiln.queries",
        "kiln.questmark",
        "kiln.render",
        "kiln.repl",
        "kiln.rules",
        "kiln.saves",
        "kiln.screen",
        "kiln.settings",
        "kiln.skills",
        "kiln.spawn",
        "kiln.stations",
        "kiln.steamguard",
        "kiln.surfaces",
        "kiln.tasks",
        "kiln.time",
        "kiln.toolwear",
        "kiln.types",
        "kiln.ui",
        "kiln.ui.inventory",
        "kiln.ui.snapshot",
        "kiln.ui.tick",
        "kiln.ui.values",
        "kiln.vfx",
        "kiln.window_clock",
        "kiln.world",
        "kiln.world.budget",
        "kiln.worldtables",
    ];

    private static readonly HashSet<string> Set = new(All, StringComparer.Ordinal);

    public static bool IsService(string name) => Set.Contains(name);
}
