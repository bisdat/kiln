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
    private int _pass, _fail;
    private string Mode => File.Exists(Path.Combine(Context.DataDirectory, "incompatible")) ? "incompatible" : File.Exists(Path.Combine(Context.DataDirectory, "corrupt")) ? "corrupt" : File.Exists(Path.Combine(Context.DataDirectory, "item-only")) ? "item-only" : "normal";
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
            view.Items.AddLike(new(Parent), like, item => item.MaxStack = 64);
            view.Items.AddLike(new(Child), new(Parent), item => item.MaxStack = 65);
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
            view.Items.Remove(Wood); // current real build must refuse whole: its drop rows are unproven
        });
    }
    public override void OnLoad()
    {
        Context.Tick.Pre += (in TickInfo _) =>
        {
            if (_ran || !Content.Ready || !Saves.WorldDataReady) return;
            _ran = true;
            try { Run(); }
            catch (Exception e) { Check(false, e.ToString()); }
            finally { Log.Info($"registry-entries-test: DONE mode={Mode} pass={_pass} fail={_fail}"); }
        };
    }
    private static int Stack(string id)
    {
        var stack = 0; DataRegistry.Read(view => stack = view.Items[id].MaxStack); return stack;
    }
    private void Run()
    {
        var definitions = NewContent.Definitions().Where(d => d.DefinitionJson is not null)
            .Select(d => (Definition: d, Body: JsonNode.Parse(d.DefinitionJson!)!))
            .Where(d => d.Body["registryId"]?.GetValue<string>()?.StartsWith("registry-entries-test:", StringComparison.Ordinal) == true).ToArray();
        if (Mode is not ("normal" or "item-only"))
        {
            Check(definitions.All(d => d.Definition.IsPlaceholder), "incompatible/corrupt compatibility never publishes a replacement definition");
            return;
        }
        var items = definitions.Where(d => d.Definition.Kind == ContentKind.Items && !d.Definition.IsPlaceholder).ToArray();
        Check(items.Length == 2 && items.All(d => d.Definition.Worlds.Contains("server") && d.Definition.Problem is null), "both stable items registered in the native server world");
        Check(definitions.Count(d => d.Definition.Kind == ContentKind.Recipes && d.Definition.Worlds.Contains("server")) == 1, "new recipe registered with the added item reference");
        Check(Stack(Parent) == 64 && Stack(Child) == 65, "stable added ids read actual mapped clone fields");
        Check(items.Select(d => d.Body["base"]!.GetValue<string>()).Distinct().Count() == 1, "clone inheritance is flattened to the same canonical game base");
        foreach (var item in items) Log.Info($"registry-entries-test: ID {item.Body["registryId"]} guid={item.Definition.Guid:D} native={item.Definition.Id} base={item.Body["base"]}");
        var changed = DataRegistry.Change(view => view.Items[Child].MaxStack = 71);
        Check(changed.Result.Ok && Stack(Child) == 71, "later field edits bind the stable added id");
        Check(changed.Revert().Ok && Stack(Child) == 65, "revert restores the definition's GameStart baseline");
        var manual = DataRegistry.Change(view => { view.Recipes.Remove(Recipe); view.Items[Child].MaxStack = 99; });
        Check(manual.Result.Outcome == RegistryChangeOutcome.RestartRequired && Stack(Child) == 65, "Manual entry refusal commits none of its valid fields");
        Check(Stack(Wood) > 0, "unproven item removal leaves the original item addressable");
    }
}
