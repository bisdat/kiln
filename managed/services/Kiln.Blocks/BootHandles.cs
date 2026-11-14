// SPDX-License-Identifier: MIT
// The boot generation's block handles (chapter 08 §4.2): a process-owned, immutable map of every committed boot definition's handle, by mod
// and name. Built once per generation from what the generation committed (BootGeneration.Committed), by the same pure lowering the boot
// phase ran (Compiler.Lower with nothing registered), so a handle has the GUID and game name the generation published. It belongs to no
// runtime owner: a runtime instance's unload or reload (ModBlocks.Reset) leaves it alone, as the native boot content stays until the process
// ends. Blocks.Find and a block's reference by name fall back to it; a runtime Define under a committed name is checked against it.
using System.Collections.Concurrent;
using System.Collections.Frozen;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace Kiln;

internal static class BootHandles
{
    /// <summary>One mod's committed boot definitions: the handles by name, and the committed definition (kind and canonical JSON) by name.</summary>
    private sealed record ModMap(IReadOnlyList<BootStaged> Source, FrozenDictionary<string, BlockHandle> Handles, FrozenDictionary<string, BootStaged> Definitions);

    private static readonly ConcurrentDictionary<string, ModMap> Maps = new(StringComparer.Ordinal);
    private static readonly object BuildGate = new();

    /// <summary>The handle of <paramref name="mod"/>'s committed boot definition <paramref name="name"/>, or null.</summary>
    internal static BlockHandle? Find(string mod, string name) => Map(mod) is { } m && m.Handles.TryGetValue(name, out var h) ? h : null;

    /// <summary>The committed boot definition <paramref name="name"/> of <paramref name="mod"/> (its kind and canonical JSON), or null.</summary>
    internal static BootStaged? Definition(string mod, string name) => Map(mod) is { } m && m.Definitions.TryGetValue(name, out var d) ? d : null;

    /// <summary>A runtime <c>Blocks.Define(name, block)</c> of <paramref name="mod"/> under the name of one of its committed boot definitions:
    /// the same definition (exactly that kind, the same canonical JSON) is a reference to the committed one (its handle; nothing is defined
    /// again, the generation registered it); another one is refused with <see cref="BlockError"/>: the content in the game cannot change
    /// before a restart. Null: the name is not a committed boot definition of the mod.</summary>
    internal static BlockHandle? SameAsCommitted(string mod, string name, IBlock block)
    {
        if (Map(mod) is not { } m || !m.Definitions.TryGetValue(name, out var committed)) return null;
        var label = $"{mod}:{name}";
        var kind = BootDefinitions.Kinds.GetValueOrDefault(block.GetType());
        if (kind != committed.Kind)
            throw new BlockError(label, null, $"RestartRequiredBoot: {name} is a {committed.Kind} definition of this process's boot generation; " +
                                              $"a {block.GetType().Name} under its name needs another name, or the change in OnBoot and a restart");
        string json;
        try { json = BootDefinitions.Canonical(JsonSerializer.SerializeToNode(block, block.GetType(), BlockJson.SharedOptions)); }
        catch (Exception e) when (e is JsonException or NotSupportedException or InvalidOperationException)
        {
            throw new BlockError(label, null, $"RestartRequiredBoot: {name} is a committed boot definition and this block cannot be compared with it ({e.Message})");
        }
        if (json != committed.Json)
            throw new BlockError(label, null, $"RestartRequiredBoot: {name} differs from the {committed.Kind} definition this process's boot generation " +
                                              "committed; the content in the game stays as committed until a restart. Define the same block (or none: " +
                                              "Blocks.Find finds the committed one), or change it in OnBoot and restart the game");
        return m.Handles[name];
    }

    private static ModMap? Map(string mod)
    {
        var committed = BootGeneration.Committed(mod);
        if (committed.Count == 0) return null;
        if (Maps.TryGetValue(mod, out var m) && ReferenceEquals(m.Source, committed)) return m;
        lock (BuildGate)
        {
            if (Maps.TryGetValue(mod, out m) && ReferenceEquals(m.Source, committed)) return m;
            m = Build(mod, committed);
            Maps[mod] = m;
            return m;
        }
    }

    // Lowers the committed definitions again, in their order, into a scratch ledger that no runtime owner holds: the handles only, nothing
    // registers (no commit; a sink that takes nothing in case). A definition that does not lower now (it did at the boot phase) has no
    // handle and is logged; the others keep theirs.
    private static ModMap Build(string mod, IReadOnlyList<BootStaged> committed)
    {
        var scratch = new ModBlocks(mod, 0) { ManifestClients = "mod" };  // the highest level: the boot phase checked the manifest's already
        var definitions = new Dictionary<string, BootStaged>(StringComparer.Ordinal);
        var previousSink = Lowered._sinkOverride;
        var previousBoot = Lowered._bootLowering;
        Lowered._sinkOverride = new NoSink();
        Lowered._bootLowering = true;
        try
        {
            foreach (var s in committed)
            {
                definitions[s.Key] = s;
                if (s.Kind == BootDefinitions.ResourcePatches) continue;  // a game resource's fields: no block, no handle
                var type = BootDefinitions.Kinds.FirstOrDefault(k => k.Value == s.Kind).Key;
                if (type == null) continue;
                try
                {
                    var block = (IBlock)JsonSerializer.Deserialize(s.Json, type, BlockJson.SharedOptions)!;
                    scratch.Handles[s.Key] = block switch
                    {
                        StatusBlock b => Compiler.Lower(scratch, s.Key, b).Handle,
                        EffectBlock b => Compiler.Lower(scratch, s.Key, b).Handle,
                        ItemBlock b => Compiler.Lower(scratch, s.Key, b).Handle,
                        SpellBlock b => Compiler.Lower(scratch, s.Key, b).Handle,
                        TalentBlock b => Compiler.Lower(scratch, s.Key, b).Handle,
                        _ => throw new ArgumentException($"{type.Name} is not a boot kind"),
                    };
                }
                catch (Exception e) when (e is BlockError or JsonException or ArgumentException or InvalidOperationException or NotSupportedException)
                {
                    BlocksBridge.Log(LogLevel.Error, $"blocks: {mod}: committed boot definition {s.Kind}/{s.Key} has no handle: {e.Message}");
                }
            }
        }
        finally
        {
            Lowered._sinkOverride = previousSink;
            Lowered._bootLowering = previousBoot;
        }
        return new ModMap(committed, scratch.Handles.ToFrozenDictionary(StringComparer.Ordinal), definitions.ToFrozenDictionary(StringComparer.Ordinal));
    }

    private sealed class NoSink : Lowered.ILoweredSink
    {
        public void Program(ulong owner, Guid guid, byte[] blob) { }
        public void Clone(ulong owner, string section, JsonObject def) { }
        public void Item(ulong owner, ItemDefinition d) { }
        public void Recipe(ulong owner, RecipeDefinition d) { }
    }
}
