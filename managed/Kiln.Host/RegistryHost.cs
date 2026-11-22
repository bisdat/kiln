// SPDX-License-Identifier: MIT
// The registry stage's mod order (registry P1 slice S11; P1-PLAN.md section 5.1). kiln.registry (kiln-content.dll) runs every
// mod's registry edits in Kiln.Host's runtime load order, the order players see in the launcher profile (the resolver's:
// required and optional dependencies, loadAfter, loadBefore, ties by id). Kiln.Host hands it over before the boot phase
// through the Kiln-internal kiln.internal.registry table (native/services/content/include/kiln_registry_host.h), so the
// stage has it wherever it runs (resources-ready, or with OnBoot in mode onBoot). Without the table (an older or missing
// kiln-content.dll) the stage runs in kiln.content's mod-folder order and registry.json says "folders".
// OnBoot authors keep their own order (required boot dependencies, then id; BootPhase.cs); where the two differ for mods that
// are both, kiln.log names the difference (coordinator ruling, 2026-11-20).
using System.Runtime.InteropServices;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace Kiln.Host;

/// <summary>kiln.internal.registry v1 (kiln_registry_host.h), 24 bytes: field order is the table's.</summary>
[StructLayout(LayoutKind.Sequential)]
internal unsafe struct KilnRegistryHostApi
{
    public uint Size, Version;
    public delegate* unmanaged[Cdecl]<KilnStr, int> SetOrder;
    public delegate* unmanaged[Cdecl]<uint, int> SetMode;

    public const string Name = "kiln.internal.registry";
    public const uint Version1 = 1;
    public const uint ModeResourcesReady = 0, ModeOnBoot = 1;
}

internal static unsafe class RegistryHost
{
    /// <summary>The order as set_order takes it: [{"mod", "version"}], every mod once, in order.</summary>
    internal static string OrderJson(IEnumerable<(string Id, string Version)> order)
    {
        var a = new JsonArray();
        var seen = new HashSet<string>(StringComparer.Ordinal);
        foreach (var (id, version) in order)
            if (seen.Add(id)) a.Add(new JsonObject { ["mod"] = id, ["version"] = version });
        return a.ToJsonString();
    }

    /// <summary>kiln.log's line when the registry's order and OnBoot's differ for mods that are in both (null: they agree).</summary>
    internal static string? OnBootDifference(IReadOnlyList<string> load, IReadOnlyList<string> onBoot)
    {
        var both = new HashSet<string>(load.Intersect(onBoot, StringComparer.Ordinal), StringComparer.Ordinal);
        var l = load.Where(both.Contains).ToList();
        var b = onBoot.Where(both.Contains).ToList();
        if (l.SequenceEqual(b, StringComparer.Ordinal)) return null;
        // the first pair the two orders put the other way round
        for (var i = 0; i < l.Count; i++)
            for (var j = i + 1; j < l.Count; j++)
                if (b.IndexOf(l[i]) > b.IndexOf(l[j]))
                    return $"registry: the registry stage runs {l[i]} before {l[j]} (Kiln.Host's load order: {string.Join(", ", l)}); " +
                           $"their OnBoot ran {l[j]} first (boot dependencies, then id: {string.Join(", ", b)})";
        return null;
    }

    /// <summary>Hands the load order (and the stage mode, when kiln.json asks for one) to kiln.registry. Never throws.</summary>
    public static void Run(Func<string, uint, nint> service, ModManager mods, string? mode, Action<LogLevel, string> log)
    {
        try
        {
            var t = (KilnRegistryHostApi*)service(KilnRegistryHostApi.Name, KilnRegistryHostApi.Version1);
            if (t == null)
            {
                log(LogLevel.Info, "registry: no kiln.internal.registry table (kiln-content.dll does not run the registry stage): nothing to hand over");
                return;
            }
            if (t->Version != KilnRegistryHostApi.Version1 || t->Size != (uint)sizeof(KilnRegistryHostApi))
            {
                log(LogLevel.Warn, $"registry: kiln.internal.registry is v{t->Version}, {t->Size} bytes, not v1 of {sizeof(KilnRegistryHostApi)}: " +
                                   "the stage runs in the mod folders' order");
                return;
            }
            var order = mods.RegistryOrder();
            var json = OrderJson(order.Select(m => (m.Id, m.Version)));
            var rc = NativeCalls.WithUtf8(json, s => t->SetOrder(s));
            log(rc == KilnAbi.Ok ? LogLevel.Info : LogLevel.Warn,
                rc == KilnAbi.Ok ? $"registry: stage order = Kiln.Host load order, {order.Count} mod(s)" : $"registry: set_order refused the load order (rc {rc})");
            if (mode is null) return;
            var m = mode.Equals("onBoot", StringComparison.OrdinalIgnoreCase) ? KilnRegistryHostApi.ModeOnBoot
                : mode.Equals("resourcesReady", StringComparison.OrdinalIgnoreCase) ? KilnRegistryHostApi.ModeResourcesReady : uint.MaxValue;
            if (m == uint.MaxValue)
            {
                log(LogLevel.Warn, $"registry: kiln.json registry.stage \"{mode}\" is not onBoot or resourcesReady: the stage runs at resources-ready");
                return;
            }
            var mrc = t->SetMode(m);
            log(mrc == KilnAbi.Ok ? LogLevel.Info : LogLevel.Warn, $"registry: stage mode {mode} (rc {mrc})");
        }
        catch (Exception e)
        {
            log(LogLevel.Warn, $"registry: the load order was not handed over: {e.Message}");
        }
    }

    /// <summary>After the boot phase: names where the registry's order and OnBoot's differ.</summary>
    public static void CompareWithOnBoot(ModManager mods, IReadOnlyList<string> onBoot, Action<LogLevel, string> log)
    {
        try
        {
            if (onBoot.Count < 2) return;
            if (OnBootDifference(mods.RegistryOrder().Select(m => m.Id).ToList(), onBoot) is { } line) log(LogLevel.Info, line);
        }
        catch (Exception e)
        {
            log(LogLevel.Warn, $"registry: the OnBoot order was not compared: {e.Message}");
        }
    }

    /// <summary>kiln.json's "registry": {"stage": "onBoot" | "resourcesReady"} (null: not set).</summary>
    internal static string? ModeOf(string kilnJson)
    {
        try
        {
            if (!File.Exists(kilnJson)) return null;
            var root = JsonNode.Parse(File.ReadAllText(kilnJson), documentOptions: new JsonDocumentOptions { CommentHandling = JsonCommentHandling.Skip, AllowTrailingCommas = true });
            return root?["registry"]?["stage"]?.GetValue<string>();
        }
        catch (Exception)
        {
            return null;
        }
    }
}

internal sealed partial class ModManager
{
    /// <summary>The runtime load order of this start's Kiln mods, as the resolver orders them (the order LoadAll loads them in):
    /// every enabled mod folder that resolves, dependencies first.</summary>
    internal List<(string Id, string Version)> RegistryOrder()
    {
        var input = BootCandidates().Select(c => new ModCandidate { Manifest = c.Manifest, Directory = c.Directory }).ToList();
        var env = new ResolveEnvironment(_kilnVersion, _apiVersion, _options.Side, _options.HasFeature, ResolveEnvironment.NoneLoaded)
            { Hosting = _options.Side == ManifestSide.Client && _options.HostedServerMods, Capability = _options.Capability };
        return Resolver.Resolve(input, env).Order.Where(c => c.Manifest is not null).Select(c => (c.Manifest!.Id, c.Manifest!.Version)).ToList();
    }
}
