// SPDX-License-Identifier: MIT
// Station menus with tabs of their own (chapter 16a, dev31 out/16a-station-tabs/DESIGN.md), with no game: what a station's
// blocks lower to without Tabs or MenuKey (pinned by goldens in Goldens/stations/, written from the compiler before 16a),
// the MenuKey binding table, the normalized record, its refusals, and the layers through the store facade on a fake backend.
using System.Runtime.CompilerServices;
using System.Text.Json;
using System.Text.Json.Nodes;
using Xunit;

namespace Kiln.BlockTests;

public sealed class StationMenuTests
{
    // ---- blocks without Tabs or MenuKey lower as before ------------------------------------------------------------

    private const string GoldenMod = "golden-stations";

    private static string GoldenDir([CallerFilePath] string self = "") => Path.Combine(Path.GetDirectoryName(self)!, "Goldens", "stations");

    /// <summary>The station cases, by golden name: (block type, the block as blocks/*.json writes it). An item case lowers after the
    /// "forge" station case, so that its recipe can name the station.</summary>
    internal static readonly (string Name, Type Type, string Json)[] GoldenCases =
    [
        ("forge", typeof(StationBlock), """
            { "name": "Rune Forge", "kind": "forge", "processing": { "slots": 6 }, "menu": { "like": "workbench" },
              "recipes": [ { "output": "Material_T1_Metal_Scraps", "needs": { "Material_T1_Wood_Logs": 1 }, "seconds": 6 },
                           { "output": "Material_T1_Metal_Scraps", "needs": { "Material_T1_Wood_Logs": 2 }, "seconds": 8 } ] }
            """),
        ("bench-shared", typeof(StationBlock), """
            { "name": "Bench", "kind": "workbench", "recipes": [ { "output": "Material_T1_Wood_Logs", "needs": { "Material_T1_Twigs": 2 } } ] }
            """),
        ("bench-like", typeof(StationBlock), """{ "name": "Like Bench", "kind": "workbench", "menu": { "like": "carpenter" } }"""),
        ("bolts", typeof(ItemBlock), """
            { "name": "Bolts", "recipe": { "station": "forge", "needs": { "Material_T1_Metal_Scraps": 2 }, "seconds": 4 } }
            """),
    ];

    public static TheoryData<string> GoldenNames => new(GoldenCases.Select(c => c.Name));

    private static readonly System.Reflection.MethodInfo ParseOpen = typeof(BlockJson).GetMethod("Parse", System.Reflection.BindingFlags.NonPublic | System.Reflection.BindingFlags.Static)!;
    private static readonly System.Reflection.MethodInfo LowerOpen = typeof(StationMenuTests).GetMethod(nameof(LowerTyped), System.Reflection.BindingFlags.NonPublic | System.Reflection.BindingFlags.Static)!;

    private static (BlockHandle Handle, JsonObject Json) LowerTyped<T>(ModBlocks mod, string name, T block) where T : class, IBlock
    {
        var (handle, low, _) = Compiler.Lower(mod, name, block);
        return (handle, low.Json);
    }

    private static JsonObject GoldenLowered(string name)
    {
        var mod = new ModBlocks(GoldenMod, 1) { ManifestClients = "mod" };
        JsonObject? made = null;
        foreach (var c in GoldenCases)
        {
            var block = ParseOpen.MakeGenericMethod(c.Type).Invoke(null, [c.Json])!;
            (BlockHandle Handle, JsonObject Json) r;
            try { r = ((BlockHandle, JsonObject))LowerOpen.MakeGenericMethod(c.Type).Invoke(null, [mod, c.Name, block])!; }
            catch (System.Reflection.TargetInvocationException e) when (e.InnerException != null) { throw e.InnerException; }
            mod.Handles[c.Name] = r.Handle;
            if (c.Name == name) made = r.Json;
        }
        return made!;
    }

    [Theory]
    [MemberData(nameof(GoldenNames))]
    public void A_station_without_tabs_or_menu_keys_lowers_to_its_golden(string name)
    {
        var made = GoldenLowered(name);
        var file = Path.Combine(GoldenDir(), name + ".json");
        // a missing golden says what it would hold: written once from the compiler before station menus, never refreshed to pass
        if (!File.Exists(file)) Assert.Fail($"no golden {file}; the compiler made:\n----8<----\n{ItemCompileGoldenTests.Text(made)}----8<----");
        ItemCompileGoldenTests.Compare(GoldenDir(), name, made);
    }
}
