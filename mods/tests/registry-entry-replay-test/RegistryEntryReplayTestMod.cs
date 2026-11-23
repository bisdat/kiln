// SPDX-License-Identifier: MIT
using System.Text.Json.Nodes;
using Kiln;

namespace RegistryEntryReplayTest;

// Deliberately no Kiln.Registry reference, OnRegistry override or registry data file: this warm start must remain lazy.
public sealed class RegistryEntryReplayTestMod : Mod
{
    private bool _ran;
    private int _pass, _fail;
    private void Check(bool ok, string message)
    {
        if (ok) ++_pass; else ++_fail;
        Log.Info($"registry-entry-replay-test: {(ok ? "PASS" : "FAIL")} {message}");
    }
    public override void OnLoad()
    {
        Context.Tick.Pre += (in TickInfo _) =>
        {
            if (_ran || !Content.Ready || !Saves.WorldDataReady) return;
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
            finally { Log.Info($"registry-entry-replay-test: DONE pass={_pass} fail={_fail}"); }
        };
    }
}
