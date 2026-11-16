// SPDX-License-Identifier: MIT
// Lab test of the structural edits of a boot resource patch (docs/kiln/modding/resource-patches.md, "Templates and
// lists"): in OnBoot it reads names:templates and the game's records, then stages
//   - LootPickup_Gravity_Twigs: SpawnTime removed, PickupItemZone added (it has none), its lastAttempt and Gravity.isActive set;
//   - keen::BalancingTable.altarsPerFlameLevel: 17 and 18 appended above its last (the game's ends at 16), 3 not (below).
// Once the world is up it reads the served resources back (ReflectedResources) and logs PASS/FAIL lines.
using System.Text.Json;
using Kiln;

namespace BootStructureTest;

public sealed class BootStructureTestMod : Mod
{
    internal static readonly Guid Twigs = Guid.Parse("9bf5aac3-6ae0-43b1-9f27-18c2f98abfca");
    internal static readonly Guid Balancing = Guid.Parse("82706b40-61b1-4b8f-8b23-dcec6971bda1");
    private int _pass, _fail;

    private void Check(bool ok, string what)
    {
        if (ok) { _pass++; Log.Info($"boot-structure-test: PASS {what}"); }
        else { _fail++; Log.Error($"boot-structure-test: FAIL {what}"); }
    }

    public override void OnBoot(BootContext boot)
    {
        // names:templates (the snapshot of this game build): the list, and one record read by its name
        try
        {
            var names = boot.GameData.Read("names:templates");
            var count = names.Ok ? names.Data!.Value.GetProperty("names").GetArrayLength() : -1;
            Log.Info($"boot-structure-test: INFO names:templates ok={names.Ok} count={count} ({names.Reason})");
            Directory.CreateDirectory(Context.DataDirectory);
            File.WriteAllText(Path.Combine(Context.DataDirectory, "boot-names.txt"), count.ToString());
        }
        catch (Exception e)
        {
            Log.Info($"boot-structure-test: INFO names:templates unavailable: {e.Message}");
        }
        var d = BootDefinitions.For(boot);
        var a = d.Define("twigs", new ResourcePatchBlock
        {
            Resource = Twigs, Type = "keen::ecs::TemplateResource",
            RemoveComponents = ["SpawnTime"],
            AddComponents = ["PickupItemZone"],
            Set = { ["PickupItemZone.lastAttempt.value"] = 7L, ["Gravity.isActive"] = false },
        });
        if (!a.Ok) throw new InvalidOperationException(a.Reason);
        var b = d.Define("altars", new ResourcePatchBlock
        {
            Resource = Balancing, Type = "keen::BalancingTable",
            Append = new() { ["altarsPerFlameLevel"] = new ResourceListAppend { Values = [17, 3, 18], AboveLast = true } },
        });
        if (!b.Ok) throw new InvalidOperationException(b.Reason);
    }

    public override void OnLoad()
    {
        if (Mods.Side != ModSide.Server) return;
        _ = Run();
    }

    private async Task Run()
    {
        try
        {
            for (var i = 0; i < 240; i++) await Context.Scheduler.NextTick();
            var state = Mods.BootState(Context.Info.Id);
            Log.Info($"boot-structure-test: INFO boot state {state.State}, definitions [{string.Join(", ", state.Definitions)}], {state.Issues.Count} issue(s)");
            foreach (var issue in state.Issues) Log.Info($"boot-structure-test: INFO issue {issue}");
            var namesFile = Path.Combine(Context.DataDirectory, "boot-names.txt");
            var names = File.Exists(namesFile) ? int.Parse(File.ReadAllText(namesFile)) : -1;
            Check(names > 1000, $"names:templates listed {names} templates in OnBoot");

            var t = ReflectedResources.Get(Twigs, "keen::ecs::TemplateResource");
            Check(t is { Exists: true }, "LootPickup_Gravity_Twigs is served");
            if (t is { Exists: true })
            {
                var n = t.Count("components");
                // VariantType names a component by its short name ("PickupItemZone")
                var types = Enumerable.Range(0, n).Select(i => t.VariantType($"components[{i}]")?.Name ?? "?")
                    .Select(x => x.StartsWith("keen::ecs::", StringComparison.Ordinal) ? x : "keen::ecs::" + x).ToList();
                Log.Info($"boot-structure-test: INFO components: {string.Join(", ", types)}");
                Check(!types.Contains("keen::ecs::SpawnTime"), $"SpawnTime removed ({n} components)");
                Check(types.Contains("keen::ecs::PickupItemZone"), "PickupItemZone added");
                var sorted = types.Select(x => x[(x.LastIndexOf(':') + 1)..]).ToList();
                Check(sorted.SequenceEqual(sorted.Order(StringComparer.Ordinal)), "the component list is sorted by type name");
                var zone = types.IndexOf("keen::ecs::PickupItemZone");
                if (zone >= 0) Check(Convert.ToInt64(t[$"components[{zone}].lastAttempt.value"]) == 7, "PickupItemZone.lastAttempt = 7");
                var gravity = types.IndexOf("keen::ecs::Gravity");
                if (gravity >= 0) Check(t[$"components[{gravity}].isActive"] is false, "Gravity.isActive = false");
            }

            var bal = ReflectedResources.Get(Balancing, "keen::BalancingTable");
            Check(bal is { Exists: true }, "the balancing table is served");
            if (bal is { Exists: true })
            {
                var n = bal.Count("altarsPerFlameLevel");
                var last2 = n >= 2 ? (Convert.ToInt64(bal[$"altarsPerFlameLevel[{n - 2}]"]), Convert.ToInt64(bal[$"altarsPerFlameLevel[{n - 1}]"])) : (0, 0);
                Check(last2 == (17, 18), $"altarsPerFlameLevel ends 17, 18 ({n} entries; 3 left out)");
            }
        }
        catch (Exception e)
        {
            Check(false, $"exception: {e}");
        }
        Log.Info($"boot-structure-test: DONE pass={_pass} fail={_fail}");
    }
}
