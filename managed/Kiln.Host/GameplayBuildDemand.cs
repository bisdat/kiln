// SPDX-License-Identifier: MIT
using System.Reflection;
using System.Reflection.Metadata;
using System.Reflection.PortableExecutable;

namespace Kiln.Host;

// Optional native service access: Kiln.Host must not acquire a reference to Kiln.Gameplay.
internal static class GameplayBuildDemand
{
    // Optional service facades that themselves consume gameplay events/rules. Keep this explicit: following every
    // shared Kiln.* dependency would make an unrelated facade count as using every service.
    private static uint GroupsForReference(string? name) => name switch {
        "Kiln.Gameplay" => 31u,
        "Kiln.Creatures" => 8u, // CreatureVariants natural-spawn events/replacements
        "Kiln.Rules" => 5u, // RulesRuntime's BuildEvent
        "Kiln.Quests" => 7u, // craft/build objectives
        "Kiln.Blocks" => 15u, // NPC variants and declarative craft/build objectives
        _ => 0u,
    };
    internal static uint GroupsFor(Assembly assembly) =>
        assembly.GetReferencedAssemblies().Aggregate(0u,(mask,n)=>mask|GroupsForReference(n.Name));

    internal static unsafe void Request(IHostServices host, ulong owner, uint demand=31)
    {
        var table = (byte*)host.Service("kiln.gameplay", 1);
        // KilnGameplayApi: request_build stays at 128, request_hooks follows at 136.
        var offset = 8 + 15 * sizeof(nint);
        if (table == null || *(uint*)table < offset + sizeof(nint) || *(uint*)(table + 4) < 1) return;
        if (*(uint*)table >= 144) {
            var groups = *(delegate* unmanaged[Cdecl]<ulong, uint, int>*)(table + 136);
            if (groups != null) {
                if (groups(owner,demand) != 0) host.Log(LogLevel.Warn,$"mod owner {owner}: gameplay hook demand refused (see group capability rows)");
                return;
            }
        }
        if((demand & 1)==0) return; // Old service already installs the four other groups at startup.
        var request = *(delegate* unmanaged[Cdecl]<ulong, int>*)(table + offset);
        if (request != null && request(owner) != 0)
            host.Log(LogLevel.Warn, $"mod owner {owner}: gameplay.build_hooks request refused: {Kiln.Capabilities.Why("gameplay.build_hooks")}");
    }

    // Startup demand must precede the first world even on dedicated servers, whose ordinary OnLoad runs at Pre.
    // Inspect metadata only: do not instantiate a mod or run its module initializer/OnLoad earlier.
    internal static uint GroupsFor(string entry, IEnumerable<string> exportedAssemblies)
    {
        var pending = new Queue<string>(); pending.Enqueue(entry);
        // A shipped helper can be loaded by name in OnLoad, without an AssemblyRef from the entry assembly.
        // Conservatively inspect those helpers too, before the first world; actual loads are checked again.
        foreach (var helper in Directory.EnumerateFiles(Path.GetDirectoryName(entry)!, "*.dll"))
        {
            var name = Path.GetFileName(helper);
            if (name.StartsWith("Kiln.", StringComparison.Ordinal) &&
                File.Exists(Path.Combine(Path.GetDirectoryName(typeof(ModManager).Assembly.Location)!, name))) continue;
            pending.Enqueue(helper);
        }
        uint demand=0;
        var seen = new HashSet<string>(StringComparer.Ordinal);
        var exports = exportedAssemblies.ToArray();
        while (pending.TryDequeue(out var path))
        {
            if (!seen.Add(path)) continue;
            using var stream = File.OpenRead(path);
            using var pe = new PEReader(stream);
            MetadataReader metadata;
            try { if (!pe.HasMetadata) continue; metadata = pe.GetMetadataReader(); }
            catch (BadImageFormatException) when (path != entry) { continue; } // native helper, not an assembly

            foreach (var handle in metadata.AssemblyReferences)
            {
                var name = metadata.GetString(metadata.GetAssemblyReference(handle).Name);
                demand |= GroupsForReference(name);
                if(demand==31) return demand;
                if (name != Path.GetFileName(name) || name.Contains('\\')) continue;
                // Match the loader: shared Kiln.* assemblies are provided by the host, not the mod's own folder.
                if (name.StartsWith("Kiln.", StringComparison.Ordinal) &&
                    File.Exists(Path.Combine(Path.GetDirectoryName(typeof(ModManager).Assembly.Location)!, name + ".dll"))) continue;
                var own = Path.Combine(Path.GetDirectoryName(path)!, name + ".dll");
                if (File.Exists(own)) pending.Enqueue(own);
                else if (exports.FirstOrDefault(p => Path.GetFileNameWithoutExtension(p).Equals(name, StringComparison.OrdinalIgnoreCase) && File.Exists(p)) is { } exported)
                    pending.Enqueue(exported);
            }
        }
        return demand;
    }
}

internal sealed partial class ModManager
{
    internal void PrepareGameplayHooks()
    {
        var candidates = BootCandidates(); // Same profile/disabled/duplicate rules as the boot admission pass.
        var roles = ResolveBootRoles(candidates, out _);
        var admitted = candidates.Where(c => roles.ContainsKey(c.Directory)).ToArray();
        foreach (var (manifest, directory) in admitted)
        {
            try
            {
                var dependencies = new HashSet<string>(StringComparer.Ordinal);
                void AddDependencies(Manifest m) {
                    foreach (var d in m.Dependencies)
                        if (dependencies.Add(d.Id) && admitted.FirstOrDefault(c => c.Manifest.Id == d.Id).Manifest is { } dependency)
                            AddDependencies(dependency);
                }
                AddDependencies(manifest);
                uint demand=Directory.Exists(Path.Combine(directory,"blocks")) ? 15u : 0u;
                if(manifest.HasCode) demand |= GameplayBuildDemand.GroupsFor(Path.Combine(directory, manifest.EntryAssembly),
                        admitted.Where(c => dependencies.Contains(c.Manifest.Id))
                            .SelectMany(c => c.Manifest.Exports.Select(f => Path.Combine(c.Directory, f))));
                if(demand==0) continue;
                var owner = _host.OwnerCreate("mod:" + manifest.Id);
                try { GameplayBuildDemand.Request(_host, owner,demand); }
                finally { _host.OwnerRevoke(owner); }
            }
            catch (Exception e) when (e is IOException or BadImageFormatException or UnauthorizedAccessException)
            {
                _host.Log(LogLevel.Warn, $"mod {manifest.Id}: gameplay preflight could not read assembly references: {e.Message}; checked again at load");
            }
        }
    }
}
