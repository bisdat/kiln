// SPDX-License-Identifier: MIT
using System.Reflection.Metadata;
using System.Reflection.PortableExecutable;
using System.Text.Json;
using System.Text.Json.Nodes;
using Microsoft.Build.Framework;

namespace Kiln.Sdk;

/// <summary>
/// Writes the typed declarations of the built mod's <c>[ImpactNative]</c> methods (since 0.2.0-dev.29, chapter 06c S4)
/// into the output's <c>impact/natives.json</c>, where kiln.abilities reads them when it starts (before any program is
/// verified): one <c>"sync": true</c> entry per method, named <c>"&lt;modid&gt;.&lt;Name&gt;"</c> with the id of the
/// output's kiln.mod.json, its <c>"args"</c> as the generator Kiln.ImpactNatives wrote them into the assembly attribute
/// <c>Kiln.ImpactNativeEntryAttribute</c>. Entries of the project's own natives.json stay; one with the same name is
/// replaced (warning KILN037). The assembly is read with System.Reflection.Metadata and never loaded. No such attribute:
/// nothing is written. Error KILN036: the assembly, the manifest or natives.json cannot be read or written.
/// </summary>
public sealed class KilnImpactNatives : Microsoft.Build.Utilities.Task
{
    /// <summary>The built mod assembly.</summary>
    [Required] public string AssemblyPath { get; set; } = "";

    /// <summary>The output's kiln.mod.json (its "id" goes in front of each name).</summary>
    [Required] public string ManifestPath { get; set; } = "";

    /// <summary>The output's impact/natives.json (created when missing).</summary>
    [Required] public string NativesPath { get; set; } = "";

    /// <summary>The full names written, ";"-separated.</summary>
    [Output] public string Written { get; private set; } = "";

    private static readonly JsonDocumentOptions Lenient = new() { CommentHandling = JsonCommentHandling.Skip, AllowTrailingCommas = true };

    public override bool Execute()
    {
        if (!File.Exists(AssemblyPath)) return true;
        List<(string Name, string Args)> entries;
        try
        {
            entries = Read(AssemblyPath);
        }
        catch (Exception e) when (e is IOException or BadImageFormatException or InvalidOperationException or UnauthorizedAccessException)
        {
            return Fail(AssemblyPath, $"cannot read the mod assembly for its [ImpactNative] natives: {e.Message}");
        }
        if (entries.Count == 0) return true;
        string id;
        try
        {
            using var doc = JsonDocument.Parse(File.ReadAllText(ManifestPath), Lenient);
            id = doc.RootElement.TryGetProperty("id", out var v) && v.ValueKind == JsonValueKind.String ? v.GetString()! : "";
        }
        catch (Exception e) when (e is IOException or JsonException or UnauthorizedAccessException)
        {
            return Fail(ManifestPath, $"cannot read the mod's id for its [ImpactNative] natives: {e.Message}");
        }
        if (id.Length == 0) return Fail(ManifestPath, "kiln.mod.json has no \"id\": the [ImpactNative] natives cannot be named");
        try
        {
            var (text, written, replaced) = Merge(File.Exists(NativesPath) ? File.ReadAllText(NativesPath) : null, id, entries);
            foreach (var r in replaced)
                Log.LogWarning(null, "KILN037", null, NativesPath, 0, 0, 0, 0,
                    $"impact/natives.json declares {r}, which an [ImpactNative] method declares: the method's declaration replaces it");
            Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(NativesPath))!);
            if (!File.Exists(NativesPath) || File.ReadAllText(NativesPath) != text) File.WriteAllText(NativesPath, text);
            Written = string.Join(";", written);
            return true;
        }
        catch (Exception e) when (e is IOException or JsonException or InvalidOperationException or UnauthorizedAccessException)
        {
            return Fail(NativesPath, $"cannot write the [ImpactNative] natives into impact/natives.json: {e.Message}");
        }
    }

    private bool Fail(string file, string message)
    {
        Log.LogError(null, "KILN036", null, file, 0, 0, 0, 0, message);
        return false;
    }

    /// <summary>The (Name, Args) of every <c>Kiln.ImpactNativeEntryAttribute</c> on the assembly.</summary>
    public static List<(string Name, string Args)> Read(string assemblyPath)
    {
        using var stream = File.OpenRead(assemblyPath);
        using var pe = new PEReader(stream);
        var md = pe.GetMetadataReader();
        var list = new List<(string, string)>();
        foreach (var h in md.GetAssemblyDefinition().GetCustomAttributes())
        {
            var ca = md.GetCustomAttribute(h);
            EntityHandle parent = default;
            if (ca.Constructor.Kind == HandleKind.MemberReference) parent = md.GetMemberReference((MemberReferenceHandle)ca.Constructor).Parent;
            else if (ca.Constructor.Kind == HandleKind.MethodDefinition) parent = md.GetMethodDefinition((MethodDefinitionHandle)ca.Constructor).GetDeclaringType();
            var (ns, name) = parent.Kind switch
            {
                HandleKind.TypeReference => (md.GetString(md.GetTypeReference((TypeReferenceHandle)parent).Namespace), md.GetString(md.GetTypeReference((TypeReferenceHandle)parent).Name)),
                HandleKind.TypeDefinition => (md.GetString(md.GetTypeDefinition((TypeDefinitionHandle)parent).Namespace), md.GetString(md.GetTypeDefinition((TypeDefinitionHandle)parent).Name)),
                _ => ("", ""),
            };
            if (ns != "Kiln" || name != "ImpactNativeEntryAttribute") continue;
            var blob = md.GetBlobReader(ca.Value);
            if (blob.ReadUInt16() != 1) throw new InvalidOperationException("an ImpactNativeEntryAttribute blob without its prolog");
            var n = blob.ReadSerializedString() ?? throw new InvalidOperationException("an ImpactNativeEntryAttribute without a name");
            var a = blob.ReadSerializedString() ?? "[]";
            list.Add((n, a));
        }
        return list;
    }

    /// <summary>The natives.json text with the entries put in (a missing file is a new one). Returns the full names
    /// written and the names that replaced an entry of the project's own.</summary>
    public static (string Text, List<string> Written, List<string> Replaced) Merge(string? existing, string id, IReadOnlyList<(string Name, string Args)> entries)
    {
        JsonObject root;
        if (existing is null || existing.Trim().Length == 0)
            root = new JsonObject { ["format"] = 1, ["natives"] = new JsonArray() };
        else
        {
            root = JsonNode.Parse(existing, documentOptions: Lenient) as JsonObject ?? throw new InvalidOperationException("impact/natives.json is not a JSON object");
            if (root["format"] is { } f && f.GetValueKind() == JsonValueKind.Number && f.GetValue<int>() != 1)
                throw new InvalidOperationException("impact/natives.json: format is not 1");
        }
        if (root["natives"] is not JsonArray natives)
        {
            if (root["natives"] is not null) throw new InvalidOperationException("impact/natives.json: \"natives\" is not an array");
            root["natives"] = natives = new JsonArray();
        }
        var written = new List<string>();
        var replaced = new List<string>();
        foreach (var (name, args) in entries)
        {
            var full = $"{id}.{name}";
            var argList = JsonNode.Parse(args) as JsonArray ?? throw new InvalidOperationException($"{full}: its args are not a JSON array");
            var entry = new JsonObject
            {
                ["name"] = full, ["sync"] = true, ["argc"] = argList.Count, ["args"] = argList,
                ["doc"] = "generated from an [ImpactNative] method (Kiln.ImpactNatives)",
            };
            var at = -1;
            for (var i = 0; i < natives.Count; i++)
                if (natives[i] is JsonObject o && o["name"]?.GetValueKind() == JsonValueKind.String && o["name"]!.GetValue<string>() == full) at = i;
            if (at >= 0)
            {
                if (!JsonNode.DeepEquals(natives[at], entry)) replaced.Add(full);
                natives[at] = entry;
            }
            else natives.Add(entry);
            written.Add(full);
        }
        return (root.ToJsonString(new JsonSerializerOptions { WriteIndented = true }) + "\n", written, replaced);
    }
}
