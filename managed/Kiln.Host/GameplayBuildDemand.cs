// SPDX-License-Identifier: MIT
using System.Reflection;
using System.Reflection.Metadata;
using System.Reflection.PortableExecutable;

namespace Kiln.Host;

// Optional native service access: Kiln.Host must not acquire a reference to Kiln.Gameplay.
internal static class GameplayBuildDemand
{
    internal static bool ReferencesGameplay(Assembly assembly) =>
        assembly.GetReferencedAssemblies().Any(n => n.Name == "Kiln.Gameplay");

    internal static unsafe void Request(IHostServices host, ulong owner)
    {
        var table = (byte*)host.Service("kiln.gameplay", 1);
        // KilnGameplayApi: service header, 15 preceding function pointers, then request_build (append-only).
        var offset = 8 + 15 * sizeof(nint);
        if (table == null || *(uint*)table < offset + sizeof(nint) || *(uint*)(table + 4) < 1) return;
        var request = *(delegate* unmanaged[Cdecl]<ulong, int>*)(table + offset);
        if (request != null && request(owner) != 0)
            host.Log(LogLevel.Warn, $"mod owner {owner}: gameplay.build_hooks request refused: {Kiln.Capabilities.Why("gameplay.build_hooks")}");
    }

    // Startup demand must precede the first world even on dedicated servers, whose ordinary OnLoad runs at Pre.
    // Inspect metadata only: do not instantiate a mod or run its module initializer/OnLoad earlier.
    internal static bool ReferencesGameplay(string entry, IEnumerable<string> exportedAssemblies)
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
                if (name == "Kiln.Gameplay") return true;
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
        return false;
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
            if (!manifest.HasCode) continue;
            try
            {
                var dependencies = new HashSet<string>(StringComparer.Ordinal);
                void AddDependencies(Manifest m) {
                    foreach (var d in m.Dependencies)
                        if (dependencies.Add(d.Id) && admitted.FirstOrDefault(c => c.Manifest.Id == d.Id).Manifest is { } dependency)
                            AddDependencies(dependency);
                }
                AddDependencies(manifest);
                if (!GameplayBuildDemand.ReferencesGameplay(Path.Combine(directory, manifest.EntryAssembly),
                        admitted.Where(c => dependencies.Contains(c.Manifest.Id))
                            .SelectMany(c => c.Manifest.Exports.Select(f => Path.Combine(c.Directory, f))))) continue;
                var owner = _host.OwnerCreate("mod:" + manifest.Id);
                try { GameplayBuildDemand.Request(_host, owner); }
                finally { _host.OwnerRevoke(owner); }
            }
            catch (Exception e) when (e is IOException or BadImageFormatException or UnauthorizedAccessException)
            {
                _host.Log(LogLevel.Warn, $"mod {manifest.Id}: gameplay preflight could not read assembly references: {e.Message}; checked again at load");
            }
        }
    }
}
