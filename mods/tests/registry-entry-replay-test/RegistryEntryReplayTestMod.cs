// SPDX-License-Identifier: MIT
using System.Text.Json.Nodes;
using Kiln;

namespace RegistryEntryReplayTest;

// Deliberately no Kiln.Registry reference, OnRegistry override or registry data file: this warm start must remain lazy.
public sealed class RegistryEntryReplayTestMod : Mod
{
    private bool _ran;
    private int _pass, _fail, _loadedTicks;
    private void Check(bool ok, string message)
    {
        if (ok) ++_pass; else ++_fail;
        Log.Info($"registry-entry-replay-test: {(ok ? "PASS" : "FAIL")} {message}");
    }
    public override void OnLoad()
    {
        Context.Tick.Pre += (in TickInfo tick) =>
        {
            if (_ran || !Content.Ready || !Saves.WorldDataReady || Saves.Status?.State != "running") return;
            // The saved bundles arrive with WorldLoaded, then MissingContent polls every 100 ticks.
            if (++_loadedTicks < 120) return;
            _ran = true;
            try
            {
                var items = NewContent.Definitions().Where(d => d.Kind == ContentKind.Items && d.DefinitionJson is not null)
                    .Select(d => (Definition: d, Body: JsonNode.Parse(d.DefinitionJson!)!))
                    .Where(d => d.Body["registryId"]?.GetValue<string>()?.StartsWith("registry-entries-test:", StringComparison.Ordinal) == true).ToArray();
                Check(items.Length == 2 && items.All(d => d.Definition.IsPlaceholder && d.Definition.Worlds.Contains("server") && d.Definition.Problem is null),
                    "both missing-mod placeholders registered before any registry API read");
                Check(items.Length == 2 && items.All(d => d.Body["registryFields"]?["perks"] is JsonArray perks && perks.Count == 1),
                    "saved placeholder definitions retained nonempty stable perk refs");
                Check(items.Length == 2 && items.All(d => Content.Items.Get(d.Definition.Id)?.GetNumber("maxStackSize") ==
                    (d.Body["registryId"]!.GetValue<string>().EndsWith(":logs", StringComparison.Ordinal) ? 128 : 67)),
                    "native placeholder bytes retain effective values rather than reapplying Alter/Final");
                foreach (var item in items) Log.Info($"registry-entry-replay-test: ID {item.Body["registryId"]} guid={item.Definition.Guid:D}");
            }
            catch (Exception e) { Check(false, e.ToString()); }
            _ = Restored();
        };
    }

    /// <summary>Without the removing mod the removal is gone at this start: the drop table that names the removed item gives
    /// it again (the registry-entries-test start before this one saw that slot disabled).</summary>
    private async Task Restored()
    {
        try
        {
            for (var i = 0; i < 25 * 120 && !(Spawn.WorldRunning && Items.Status() is { Read: true }); i++) await Context.Scheduler.NextTick();
            var wood = new ItemId(Content.Items.Get("Material_T1_Wood_Logs")!.Id);
            var at = Items.FindInventories(InventoryKinds.Containers).Select(c => Spawn.Inspect(c)).FirstOrDefault(i => i != null)?.Position ?? new WorldPosition(3855.8, 834.5, 1399.8);
            var template = Spawn.Templates.FirstOrDefault(t => t.Name.Equals("lootchest_cheat_new13_loot_grassland", StringComparison.OrdinalIgnoreCase));
            if (template is null) { Check(false, "no template for the drop table check"); return; }
            var e = await Spawn.Entity(template.Name, at with { Y = at.Y + 2 }, options: new SpawnOptions { Tag = "registry-entry-replay-test", Flags = SpawnFlags.Transient });
            for (var i = 0; i < 200 && Items.Read(e) is not { UsedSlots: > 0 }; i++) await Context.Scheduler.NextTick();
            for (var i = 0; i < 5; i++) await Context.Scheduler.NextTick();
            var got = Items.Read(e);
            Check(got is not null && got.Count(wood) > 0, $"without the removing mod the drop table gives the item again: {got?.Count(wood) ?? -1} in {template.Name}");
            Spawn.Destroy(e);
        }
        catch (Exception x) { Check(false, x.ToString()); }
        finally { Log.Info($"registry-entry-replay-test: DONE pass={_pass} fail={_fail}"); }
    }
}
