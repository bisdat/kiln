// SPDX-License-Identifier: MIT
// The in-process half of tools/registry-compat (registry P1 S12): at every content ready (the world's load, each pack hot
// reload) one dump, <data>/dump-<n>.json (n counts from 1):
//   - "resources": every resource of the registries' resource types as the game reads it now (kiln.assets: an override if
//     one is registered, else the game's own), by key, with its size and SHA-256: compare.py checks every one the registry
//     does not touch against the game's archive (a Kiln-off boot reads exactly those bytes);
//   - "store": kiln.content's changed fields (original, current) and conflicts (Content.Dump, content-dump/1);
//   - "registry": state/registry.json as it is then.
// Writes nothing else and changes nothing. "registry-compat-test: DUMP <n> <file> resources=<k>" in kiln.log.
using System.Security.Cryptography;
using System.Text.Json.Nodes;
using Kiln;
using Kiln.Assets;
using Kiln.Content;

namespace RegistryCompatTest;

public sealed class RegistryCompatTestMod : Mod
{
    private static readonly string[] Types =
    {
        "keen::ItemInfo", "keen::RecipeRegistryResource", "keen::LootableItemsResource", "keen::ecs::DefaultInventoryResource", "keen::BalancingTable",
    };
    private int _n;

    public override void OnLoad()
    {
        Content.OnReady(e =>
        {
            try { Write(e.Reload); }
            catch (Exception x) { Log.Error($"registry-compat-test: FAIL the dump threw: {x}"); }
        });
    }

    private void Write(bool reload)
    {
        var n = ++_n;
        var res = new JsonArray();
        foreach (var type in Types)
            foreach (var key in Assets.ListResources(Assets.TypeHash(type)))
            {
                var bytes = Assets.ReadResource(key);
                res.Add(new JsonObject
                {
                    ["type"] = type, ["typeHash"] = key.Type, ["guid"] = key.Guid.ToString(), ["part"] = key.Part,
                    ["size"] = bytes?.Length ?? -1, ["sha256"] = bytes is null ? null : Convert.ToHexStringLower(SHA256.HashData(bytes)),
                });
            }
        var store = Path.Combine(Context.DataDirectory, $"store-{n}.json");
        Content.Dump(store);
        var changed = new JsonArray();
        var full = JsonNode.Parse(File.ReadAllText(store))!;
        foreach (var reg in new[] { "items", "recipes", "loot", "drops" })
            foreach (var e in full[reg]?["entries"]?.AsArray() ?? new JsonArray())
                if (e?["changed"] is JsonObject c)
                    foreach (var (f, v) in c) changed.Add(new JsonObject { ["registry"] = reg, ["entry"] = (string?)e["name"], ["field"] = f, ["value"] = v?.DeepClone() });
        File.Delete(store);
        var statePath = Path.GetFullPath(Path.Combine(Context.Info.Directory, "..", "..", "state", "registry.json"));
        var doc = new JsonObject
        {
            ["format"] = 1, ["n"] = n, ["reload"] = reload, ["resources"] = res,
            ["store"] = new JsonObject { ["changed"] = changed, ["conflicts"] = full["conflicts"]?.DeepClone() },
            ["registry"] = File.Exists(statePath) ? JsonNode.Parse(File.ReadAllText(statePath)) : null,
        };
        var path = Path.Combine(Context.DataDirectory, $"dump-{n}.json");
        File.WriteAllText(path, doc.ToJsonString());
        Log.Info($"registry-compat-test: DUMP {n} {path} resources={res.Count} changed={changed.Count}");
    }
}
