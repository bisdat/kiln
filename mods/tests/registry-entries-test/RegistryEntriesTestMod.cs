// SPDX-License-Identifier: MIT
#pragma warning disable KILNX031
using System.Text.Json.Nodes;
using Kiln;
using Kiln.Registry;
using DataRegistry = Kiln.Registry.Registry;

namespace RegistryEntriesTest;

public sealed class RegistryEntriesTestMod : Mod
{
    private const string Wood = "game:material_t1_wood_logs", Parent = "registry-entries-test:logs", Child = "registry-entries-test:logs_child", Recipe = "registry-entries-test:recipe";
    private bool _ran;
    private int _pass, _fail, _loadedTicks;
    private string Mode => File.Exists(Path.Combine(Context.DataDirectory, "incompatible")) ? "incompatible" : File.Exists(Path.Combine(Context.DataDirectory, "corrupt")) ? "corrupt" : File.Exists(Path.Combine(Context.DataDirectory, "item-only")) ? "item-only" : File.Exists(Path.Combine(Context.DataDirectory, "reload")) ? "reload" : "normal";
    // a drop table that names the removed item (its wood stack: chance 1, up to 50): the slot must stay inert while the removal is on
    internal const string WoodChest = "lootchest_cheat_new13_loot_grassland";
    // the spot of the lab world the other spawn tests use (blocks-loot-test, registry-proof-test)
    internal static readonly WorldPosition LabSpot = new(3855.8, 834.5, 1399.8);
    private void Check(bool ok, string message)
    {
        if (ok) ++_pass; else ++_fail;
        Log.Info($"registry-entries-test: {(ok ? "PASS" : "FAIL")} {message}");
    }
    public override void OnRegistry(RegistryContext context)
    {
        var registry = DataRegistry.For(context);
        registry.Add(view =>
        {
            var like = Mode == "incompatible" ? view.Items.Select("game:*").First(item => item.Id.Value != Wood).Id : new RegistryId(Wood);
            var perk = view.Items.Select("game:*").First(item => item.Perks.Count > 0).Perks[0];
            view.Items.AddLike(new(Parent), like, item => { item.MaxStack = 64; item.Perks.Replace([perk]); });
            view.Items.AddLike(new(Child), new(Parent), _ => { });
            // Select actual Kiln recipe ids, not a game function, GUID, reflection name or assumed archive position.
            var recipes = view.Recipes.Select("game:*");
            if (recipes.Count < 2) throw new InvalidOperationException("the server fixture requires two game recipes");
            view.Recipes.AddLike(new(Recipe), recipes[0].Id, recipe =>
            {
                recipe.Inputs.Clear(); var input = recipe.Inputs.AppendNew(); input.Item = new(Wood); input.Count = 1;
                recipe.Outputs.Clear(); var output = recipe.Outputs.AppendNew(); output.Item = new(Child); output.Count = 1;
            });
            view.Items.AddLike(new("registry-entries-test:refused"), new("game:registry_entries_missing_fixture"), item => item.MaxStack = 66);
            view.Items.AddLike(new("registry-entries-test:dependent"), new("registry-entries-test:refused"), item => item.MaxStack = 67);
        });
        registry.Alter(view =>
        {
            if (Mode != "item-only")
            {
                view.Recipes.Remove(view.Recipes.Select("game:*")[1].Id.Value);
                view.Loot.Remove(Wood);
            }
            // with the drop rows proven on this build (registry-proof, 31.x) the whole removal applies: hidden from random loot,
            // the recipes naming it and the drop slots naming it (disabled in place); its record stays
            view.Items.Remove(Wood);
        });
    }
    public override void OnLoad()
    {
        Context.Tick.Pre += (in TickInfo tick) =>
        {
            if (_ran || !Content.Ready || !Saves.WorldDataReady || Saves.Status?.State != "running") return;
            // WorldDataReady precedes the game's load by about ten seconds. Give MissingContent's 100-tick pass a real
            // loaded-world turn before a lab stop can save the fixture, rather than observing an empty pre-load bundle.
            if (++_loadedTicks < 120) return;
            _ran = true;
            var later = false;
            try { later = Run(); }
            catch (Exception e) { Check(false, e.ToString()); }
            finally { if (!later) Done(); }
            if (later) _ = Removal();
        };
    }
    private static int Stack(string id)
    {
        var stack = 0; DataRegistry.Read(view => stack = view.Items[id].MaxStack); return stack;
    }
    private void Done() => Log.Info($"registry-entries-test: DONE mode={Mode} pass={_pass} fail={_fail}");

    /// <summary>The synchronous checks; true when <see cref="Removal"/> follows (it prints DONE).</summary>
    private bool Run()
    {
        var definitions = NewContent.Definitions().Where(d => d.DefinitionJson is not null)
            .Select(d => (Definition: d, Body: JsonNode.Parse(d.DefinitionJson!)!))
            .Where(d => d.Body["registryId"]?.GetValue<string>()?.StartsWith("registry-entries-test:", StringComparison.Ordinal) == true).ToArray();
        if (Mode is not ("normal" or "item-only" or "reload"))
        {
            Check(definitions.All(d => d.Definition.IsPlaceholder), "incompatible/corrupt compatibility never publishes a replacement definition");
            return false;
        }
        var items = definitions.Where(d => d.Definition.Kind == ContentKind.Items && !d.Definition.IsPlaceholder).ToArray();
        Check(items.Length == 2 && items.All(d => d.Definition.Worlds.Contains("server") && d.Definition.Problem is null), "both stable items registered in the native server world");
        Check(definitions.Count(d => d.Definition.Kind == ContentKind.Recipes && d.Definition.Worlds.Contains("server")) == 1, "new recipe registered with the added item reference");
        Check(Stack(Parent) == 128 && Stack(Child) == 67, "relative Alter/Final apply once over explicit and inherited Add constructor baselines");
        Check(items.All(d => d.Body["registryFields"]?["perks"] is JsonArray perks && perks.Count == 1) &&
            items.All(d => d.Body["registryFields"]?["maxStack"]?.GetValue<int>() == (d.Body["registryId"]!.GetValue<string>() == Parent ? 128 : 67)),
            "saved/synced definitions export effective maxStack and nonempty stable perk refs");
        Check(items.Select(d => d.Body["base"]!.GetValue<string>()).Distinct().Count() == 1, "clone inheritance is flattened to the same canonical game base");
        foreach (var item in items) Log.Info($"registry-entries-test: ID {item.Body["registryId"]} guid={item.Definition.Guid:D} native={item.Definition.Id} base={item.Body["base"]}");
        var changed = DataRegistry.Change(view => view.Items[Child].MaxStack = 71);
        Check(changed.Result.Ok && Stack(Child) == 71, "later field edits bind the stable added id");
        Check(changed.Revert().Ok && Stack(Child) == 67, "revert restores the definition's GameStart baseline");
        var manual = DataRegistry.Change(view => { view.Recipes.Remove(Recipe); view.Items[Child].MaxStack = 99; });
        Check(manual.Result.Outcome == RegistryChangeOutcome.RestartRequired && Stack(Child) == 67, "Manual entry refusal commits none of its valid fields");
        Check(Content.Items.Get("Material_T1_Wood_Logs") is { } w && w.GetNumber("maxStackSize") > 0, "the removed item's record stays addressable: hidden, never deleted");
        var loot = JsonNode.Parse(NewContent.InfoJson())?["loot"];
        var candidates = loot?["getter"]?["count"]?.GetValue<int>() ?? -1;
        var records = Content.Status()?.Loot ?? -1;
        Check(loot?["on"]?.GetValue<bool>() == true && records > 0 && candidates == records - 1,
            "the actual random-loot getter excludes only the removed item's candidate while retaining its record");
        return true;
    }

    private async Task Steps(int n)
    {
        for (var i = 0; i < n; i++) await Context.Scheduler.NextTick();
    }

    /// <summary>The removal in the running world: owned copies of the removed item keep working and survive the save and a
    /// reload; a drop table naming it, loaded now, has that slot disabled (the chest gets everything else).</summary>
    private async Task Removal()
    {
        try
        {
            for (var i = 0; i < 25 * 120 && !(Spawn.WorldRunning && Items.Status() is { Transactions: true, Read: true }); i++) await Context.Scheduler.NextTick();
            var wood = new ItemId(Content.Items.Get("Material_T1_Wood_Logs")!.Id);
            int Owned() => Items.FindInventories(InventoryKinds.Containers).Sum(c => Items.Read(c)?.Count(wood) ?? 0);
            // owned copies: a container of the lab's world, when it has one the server runs (a player's chest is built, not spawned)
            var chest = Items.FindInventories(InventoryKinds.Containers).FirstOrDefault(c => Items.Read(c) is { } inv && inv.FreeSlots() >= 1);
            var file = Path.Combine(Context.DataDirectory, "owned-wood.txt");
            if (chest.IsNone)
                Log.Info("registry-entries-test: SKIP owned copies: the lab's world has no container the server runs");
            else if (Mode == "reload")
            {
                var saved = File.Exists(file) ? int.Parse(File.ReadAllText(file).Trim(), System.Globalization.CultureInfo.InvariantCulture) : -1;
                Check(saved > 0 && Owned() == saved, $"owned copies of the removed item survived the save and the reload: {Owned()} in the world's containers (saved {saved})");
            }
            else
            {
                var before = Owned();
                var give = await Items.Give(chest, wood, 7, options: GiveOptions.AllOrNothing);
                await Steps(5);
                Check(give is { Status: ItemsStatus.Done } && Owned() == before + 7, $"the removed item can still be owned: 7 given into {chest}, {before} -> {Owned()} in the world's containers");
                File.WriteAllText(file, Owned().ToString(System.Globalization.CultureInfo.InvariantCulture));
            }
            var at = Items.FindInventories(InventoryKinds.Containers).Select(c => Spawn.Inspect(c)).FirstOrDefault(i => i != null)?.Position ?? LabSpot;
            var template = Spawn.Templates.FirstOrDefault(t => t.Name.Equals(WoodChest, StringComparison.OrdinalIgnoreCase));
            if (template is null) { Check(false, $"a drop table naming the removed item: no template {WoodChest}"); return; }
            var e = await Spawn.Entity(template.Name, at with { Y = at.Y + 2 }, options: new SpawnOptions { Tag = "registry-entries-test", Flags = SpawnFlags.Transient });
            for (var i = 0; i < 200 && Items.Read(e) is not { UsedSlots: > 0 }; i++) await Context.Scheduler.NextTick();
            await Steps(5);
            var got = Items.Read(e);
            Check(got is { UsedSlots: > 0 } && got.Count(wood) == 0,
                $"the drop table's slot of the removed item is disabled: {template.Name} holds {got?.UsedSlots ?? 0} stacks, {got?.Count(wood) ?? -1} of the removed item");
            Spawn.Destroy(e);
        }
        catch (Exception x) { Check(false, x.ToString()); }
        finally { Done(); }
    }
}
