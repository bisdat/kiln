// SPDX-License-Identifier: MIT
#pragma warning disable KILNX031
using Kiln;
using DataRegistry = Kiln.Registry.Registry;

namespace RegistryEdits;

// Since API 0.2.17. Omit the attribute to author only at GameStart.
[RegistryEntryPoints(RegistryEntryPoint.GameStart | RegistryEntryPoint.WorldLoad | RegistryEntryPoint.HotReload)]
public sealed class RegistryEditsMod : Mod
{
    public sealed class Settings
    {
        [Range(1, 10000)] public int WoodStack { get; set; } = 500;
    }
    public override void OnRegistry(RegistryContext context)
    {
        // The host supplies a read-only input snapshot when no live config is registered yet.
        // Observed settings, code, point, pass and world identity contribute to context.Identity.
        var settings = Config.Register<Settings>().Value;
        var registry = DataRegistry.For(context);
        registry.Alter(view => view.Items["game:material_t1_wood_logs"].MaxStack = settings.WoodStack);
        // A Final helper is explicit. Ordinary assignments target Alter and are ignored in Add/Final.
        // Do not retain context, entries or lists: their operations expire when this callback returns.
    }
}
