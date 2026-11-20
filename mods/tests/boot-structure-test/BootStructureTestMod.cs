// SPDX-License-Identifier: MIT
// Lab test of the structural edits of a boot resource patch (docs/kiln/modding/resource-patches.md, "Templates and
// lists"): in OnBoot it reads names:templates (and names:templates/<component>, the templates' own components only) and the
// game's records, then stages
//   - LootPickup_Gravity_Twigs: SpawnTime removed, PickupItemZone added (it has none), its lastAttempt and Gravity.isActive set;
//   - keen::BalancingTable.altarsPerFlameLevel: 17 and 18 appended above its last (the game's ends at 16), 3 not (below);
//   - Projectile_Enemy_Fogger_Elite_Flying_Critter: 2.5 appended to a list inside a component
//     (EnemyProjectileBehavior.horizontalHitAngles, a BlobArray<keen::Angle>);
// and checks the patch of the data-only mod boot-structure-half (deploy it next to this one): on LootPickup_Gravity_Aloe,
// SpawnTime removed and a field of PickupItemZone set, which Aloe has not got. The set fails when it is applied, so the
// whole patch is left out (SpawnTime stays) and boot.json names the patch once. (A mod whose boot content faulted is not
// loaded, so the failing patch is another mod's.)
// Since mod API 0.2.9 (EMBER's cases), also
//   - names:resources/keen::SceneResource and resource:<GUID>/<qualified type>/<part> (and the type hash form) in OnBoot:
//     the world scene's noBuildZones and playable areas read from the snapshot; the FbUiBundle asked for too (a dedicated
//     server has none: not found);
//   - Prop_Decoration_T1_Storage_24: InventoryCraftingStock copied from the Magic chest (Prop_Decoration_T1_Storage_24_Magic);
//   - Projectile_Enemy_Crocodile_WaterBlast: its EnemyProjectileBehavior replaced by the Fogger's (6 angles: the Fogger's own
//     append above does not carry over, the source is read as the game has it), then its force set on the copy;
//   - Projectile_Enemy_Scavenger_Crossbow_Elite_Poison_Arrow: horizontalHitAngles replaced by 32 values;
//   - and boot-structure-half's JSON patch "scene" clears the world scene's noBuildZones and playable areas.
// Once the world is up it reads the served resources back (ReflectedResources) and logs PASS/FAIL lines.
using System.Text.Json;
using Kiln;

namespace BootStructureTest;

public sealed class BootStructureTestMod : Mod
{
    internal static readonly Guid Twigs = Guid.Parse("9bf5aac3-6ae0-43b1-9f27-18c2f98abfca");
    internal static readonly Guid Balancing = Guid.Parse("82706b40-61b1-4b8f-8b23-dcec6971bda1");
    internal static readonly Guid Fogger = Guid.Parse("056fbc9c-cb33-45b3-b8c5-67ed83fec33c");
    internal static readonly Guid Aloe = Guid.Parse("9dc19a07-a069-41d9-a433-bd015475b82e");
    internal static readonly Guid Scene = Guid.Parse("509feadb-4c60-425f-9c7c-deeefd9b6920");
    internal static readonly Guid UiBundle = Guid.Parse("46e14df6-fb8f-4f66-9463-e23b9e19e48e");
    internal static readonly Guid Chest = Guid.Parse("65baa890-022a-454b-9cb7-b92cb8c06cd4");
    internal static readonly Guid MagicChest = Guid.Parse("955ba32f-08ea-414d-bd98-ac75990aef04");
    internal static readonly Guid Crocodile = Guid.Parse("02a72997-47eb-4ad0-94d1-8fe5824ffa9b");
    internal static readonly Guid Crossbow = Guid.Parse("25c6622b-38dd-4034-b17b-3a9dbc110cf9");
    internal static readonly double[] Angles32 = Enumerable.Range(0, 32).Select(i => Math.Round(-1.5 + i * 0.1, 2)).ToArray();
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
            // names:templates/<component>: the templates that hold one (three hold a PickupItemZone on this game build)
            var zones = boot.GameData.Read("names:templates/PickupItemZone");
            var zoneNames = zones.Ok ? zones.Data!.Value.GetProperty("names").EnumerateArray().Select(x => x.GetString()).ToList() : [];
            var spawnTimes = boot.GameData.Read("names:templates/SpawnTime");
            var twigs = spawnTimes.Ok && spawnTimes.Data!.Value.GetProperty("names").EnumerateArray().Any(x => x.GetString() == "LootPickup_Gravity_Twigs");
            Log.Info($"boot-structure-test: INFO names:templates/PickupItemZone ok={zones.Ok} [{string.Join(", ", zoneNames)}]; SpawnTime has Twigs: {twigs}");
            File.WriteAllText(Path.Combine(Context.DataDirectory, "boot-components.txt"), $"{zoneNames.Count} {twigs}");
        }
        catch (Exception e)
        {
            Log.Info($"boot-structure-test: INFO names:templates unavailable: {e.Message}");
        }
        // discovery: every SceneResource, the world scene read by its qualified type name and by its type hash, and the
        // FbUiBundle (a client resource)
        var found = new List<string>();
        try
        {
            var scenes = boot.GameData.Read("names:resources/keen::SceneResource");
            var list = scenes.Ok ? scenes.Data!.Value.GetProperty("resources").EnumerateArray()
                .Select(r => $"{r.GetProperty("resourceId").GetString()}/{r.GetProperty("part").GetInt32()}").ToList() : [];
            Log.Info($"boot-structure-test: INFO names:resources/keen::SceneResource ok={scenes.Ok} [{string.Join(", ", list)}] ({scenes.Reason})");
            found.Add($"scenes={list.Count} world={list.Contains($"{Scene}/0")}");
            foreach (var key in new[] { $"resource:{Scene}/keen::SceneResource/0", $"resource:{Scene}/2bc68ebe/0" })
            {
                var r = boot.GameData.Read(key);
                if (!r.Ok) { Log.Info($"boot-structure-test: INFO {key}: {r.Reason}"); found.Add("scene=none"); continue; }
                var v = r.Data!.Value;
                var zones = Length(v, "noBuildZones");
                var areas = v.TryGetProperty("resetPlayersOutOfBounds", out var reset) ? Length(reset, "playableAreas") : -1;
                var bytes = v.GetRawText().Length;
                Log.Info($"boot-structure-test: INFO {key}: type {r.Data.TypeHash:x8}, noBuildZones {zones}, playableAreas {areas}, " +
                         $"iblIntensity {(v.TryGetProperty("iblIntensity", out var ibl) ? ibl.GetRawText() : "?")}, {bytes} bytes; first zone " +
                         (v.TryGetProperty("noBuildZones", out var z) && z.ValueKind == JsonValueKind.Array && z.GetArrayLength() > 0 ? z[0].GetRawText() : "-"));
                found.Add($"scene={zones},{areas}");
            }
            foreach (var key in new[] { "names:resources/keen::FbUiBundle", $"resource:{UiBundle}/keen::FbUiBundle/0" })
            {
                var r = boot.GameData.Read(key);
                Log.Info($"boot-structure-test: INFO {key}: ok={r.Ok} ({r.Reason})" + (r.Ok ? " " + r.Data!.Value.GetRawText()[..Math.Min(300, r.Data.Value.GetRawText().Length)] : ""));
                found.Add($"ui={r.Ok}");
            }
        }
        catch (Exception ex)
        {
            Log.Info($"boot-structure-test: INFO resource discovery unavailable: {ex.Message}");
            found.Add("discovery=" + ex.GetType().Name);
        }
        Directory.CreateDirectory(Context.DataDirectory);
        File.WriteAllText(Path.Combine(Context.DataDirectory, "boot-discovery.txt"), string.Join(" ", found));
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
        var c = d.Define("fogger", new ResourcePatchBlock
        {
            Resource = Fogger, Type = "keen::ecs::TemplateResource",
            Append = new() { ["EnemyProjectileBehavior.horizontalHitAngles"] = new ResourceListAppend { Values = [2.5] } },
        });
        if (!c.Ok) throw new InvalidOperationException(c.Reason);
        // copyComponents: the Magic chest's crafting stock onto a plain chest; a component with a list, structs and a
        // reference copied over another template's own, then a field of the copy set
        var stock = d.Define("stock", new ResourcePatchBlock
        {
            Resource = Chest, Type = "keen::ecs::TemplateResource",
            CopyComponents = new() { ["InventoryCraftingStock"] = MagicChest },
        });
        if (!stock.Ok) throw new InvalidOperationException(stock.Reason);
        var f = d.Define("projectile", new ResourcePatchBlock
        {
            Resource = Crocodile, Type = "keen::ecs::TemplateResource",
            CopyComponents = new() { ["EnemyProjectileBehavior"] = Fogger },
            Set = { ["EnemyProjectileBehavior.force"] = 9.0 },
        });
        if (!f.Ok) throw new InvalidOperationException(f.Reason);
        // replace: a list of floats gets exactly 32 values
        var g = d.Define("angles", new ResourcePatchBlock
        {
            Resource = Crossbow, Type = "keen::ecs::TemplateResource",
            Replace = new() { ["EnemyProjectileBehavior.horizontalHitAngles"] = new ResourceListReplace { Values = Angles32.Select(x => (ResourceValue)x).ToList() } },
        });
        if (!g.Ok) throw new InvalidOperationException(g.Reason);
    }

    private static int Length(JsonElement v, string member) =>
        v.TryGetProperty(member, out var a) && a.ValueKind == JsonValueKind.Array ? a.GetArrayLength() : -1;

    private static List<string> ComponentsOf(ReflectedResource t)
    {
        var n = t.Count("components");
        return Enumerable.Range(0, n).Select(i => t.VariantType($"components[{i}]")?.Name ?? "?")
            .Select(x => x[(x.LastIndexOf(':') + 1)..]).ToList();
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

            var componentsFile = Path.Combine(Context.DataDirectory, "boot-components.txt");
            var components = File.Exists(componentsFile) ? File.ReadAllText(componentsFile) : "";
            Check(components == "3 True", $"names:templates/PickupItemZone lists 3 templates, names:templates/SpawnTime has Twigs ({components})");

            // an append to a list inside a template's component
            var fog = ReflectedResources.Get(Fogger, "keen::ecs::TemplateResource");
            Check(fog is { Exists: true }, "Projectile_Enemy_Fogger_Elite_Flying_Critter is served");
            if (fog is { Exists: true })
            {
                var at = ComponentsOf(fog).IndexOf("EnemyProjectileBehavior");
                var n = at >= 0 ? fog.Count($"components[{at}].horizontalHitAngles") : -1;
                var last = n > 0 ? Convert.ToDouble(fog[$"components[{at}].horizontalHitAngles[{n - 1}]"]) : double.NaN;
                Check(n == 7 && Math.Abs(last - 2.5) < 1e-6, $"EnemyProjectileBehavior.horizontalHitAngles has 7 angles, the last 2.5 ({n}, {last})");
            }

            // a patch that fails when it is applied is left out whole: Aloe keeps its SpawnTime, the fault names the patch once
            var aloe = ReflectedResources.Get(Aloe, "keen::ecs::TemplateResource");
            if (aloe is { Exists: true })
            {
                var types = ComponentsOf(aloe);
                Check(types.Contains("SpawnTime") && !types.Contains("PickupItemZone"), $"the failed patch \"half\" changed nothing ({string.Join(", ", types)})");
            }
            else
            {
                Check(true, "the failed patch \"half\" changed nothing (Aloe is served as the game's own)");
            }
            // EMBER's cases (mod API 0.2.9)
            var discoveryFile = Path.Combine(Context.DataDirectory, "boot-discovery.txt");
            var discovery = File.Exists(discoveryFile) ? File.ReadAllText(discoveryFile) : "";
            Check(discovery.StartsWith("scenes=1 world=True", StringComparison.Ordinal), $"names:resources/keen::SceneResource lists the world scene in OnBoot ({discovery})");
            Check(discovery.Contains("scene=8,14 scene=8,14", StringComparison.Ordinal), $"resource:<scene>/keen::SceneResource/0 and /2bc68ebe/0 read the game's 8 no-build zones, 14 playable areas ({discovery})");
            Check(discovery.EndsWith("ui=False ui=False", StringComparison.Ordinal), $"a dedicated server's snapshot has no FbUiBundle ({discovery})");

            var scene = ReflectedResources.Get(Scene, "keen::SceneResource");
            Check(scene is { Exists: true }, "the world scene is served");
            if (scene is { Exists: true })
            {
                var zones = scene.Count("noBuildZones");
                var areas = scene.Count("resetPlayersOutOfBounds.playableAreas");
                Check(zones == 0 && areas == 0, $"boot-structure-half's \"scene\" cleared noBuildZones and playableAreas ({zones}, {areas})");
                Log.Info($"boot-structure-test: INFO scene after the clear: entityChunkCount.x {scene["entityChunkCount.x"]}, models {scene.Count("models")}");
            }

            var chest = ReflectedResources.Get(Chest, "keen::ecs::TemplateResource");
            if (chest is { Exists: true })
            {
                var types = ComponentsOf(chest);
                var at = types.IndexOf("InventoryCraftingStock");
                Check(at >= 0 && chest[$"components[{at}].canReceiveAreaDeposit"] is true,
                    $"Prop_Decoration_T1_Storage_24 has the Magic chest's InventoryCraftingStock (canReceiveAreaDeposit {(at >= 0 ? chest[$"components[{at}].canReceiveAreaDeposit"] : "-")})");
                Check(types.SequenceEqual(types.Order(StringComparer.Ordinal)), "its component list is sorted by type name");
            }
            else Check(false, "Prop_Decoration_T1_Storage_24 is served");

            var croc = ReflectedResources.Get(Crocodile, "keen::ecs::TemplateResource");
            if (croc is { Exists: true })
            {
                var types = ComponentsOf(croc);
                var at = types.IndexOf("EnemyProjectileBehavior");
                var n = at >= 0 ? croc.Count($"components[{at}].horizontalHitAngles") : -1;
                var first = n > 0 ? Convert.ToDouble(croc[$"components[{at}].horizontalHitAngles[0]"]) : double.NaN;
                var force = at >= 0 ? Convert.ToDouble(croc[$"components[{at}].force"]) : double.NaN;
                var item = at >= 0 ? croc[$"components[{at}].itemReference"]?.ToString() : null;
                var mode = at >= 0 ? croc[$"components[{at}].mode"]?.ToString() : null;
                Log.Info($"boot-structure-test: INFO crocodile projectile: {n} angles, first {first}, force {force}, item {item}, mode {mode}");
                Check(types.Count(t => t == "EnemyProjectileBehavior") == 1, "the crocodile's EnemyProjectileBehavior is replaced, not doubled");
                Check(n == 6 && Math.Abs(first - -0.5235987901687622) < 1e-6, $"the copy holds the Fogger's 6 angles as the game has them, not the Fogger's patched 7 ({n}, {first})");
                Check(Math.Abs(force - 9) < 1e-6, $"a set on the copied component applies after the copy (force {force})");
                Check(item != null && item.Contains("3be1ef69", StringComparison.OrdinalIgnoreCase), $"the copy keeps its reference (itemReference {item})");
            }
            else Check(false, "Projectile_Enemy_Crocodile_WaterBlast is served");

            var bow = ReflectedResources.Get(Crossbow, "keen::ecs::TemplateResource");
            if (bow is { Exists: true })
            {
                var at = ComponentsOf(bow).IndexOf("EnemyProjectileBehavior");
                var n = at >= 0 ? bow.Count($"components[{at}].horizontalHitAngles") : -1;
                var got = Enumerable.Range(0, Math.Max(n, 0)).Select(i => Convert.ToDouble(bow[$"components[{at}].horizontalHitAngles[{i}]"])).ToList();
                Check(n == 32 && got.Zip(Angles32).All(p => Math.Abs(p.First - p.Second) < 1e-5), $"horizontalHitAngles replaced by exactly the 32 values ({n}; {string.Join(", ", got.Take(3))} ... {got.LastOrDefault()})");
            }
            else Check(false, "Projectile_Enemy_Scavenger_Crossbow_Elite_Poison_Arrow is served");

            var half = Mods.BootState("boot-structure-half");
            Log.Info($"boot-structure-test: INFO boot-structure-half: {half.State}, {half.Issues.Count} issue(s)");
            var faults = half.Issues.Where(i => i.Key == "half").ToList();
            Check(faults.Count == 1 && faults[0].Reason.Contains("PickupItemZone") && faults[0].Reason.Contains("nothing of this patch was applied"),
                $"one fault for \"half\" ({string.Join(" | ", faults.Select(f => f.Reason))})");
        }
        catch (Exception e)
        {
            Check(false, $"exception: {e}");
        }
        Log.Info($"boot-structure-test: DONE pass={_pass} fail={_fail}");
    }
}
