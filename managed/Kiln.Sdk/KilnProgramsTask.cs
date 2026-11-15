// SPDX-License-Identifier: MIT
using System.Collections.Immutable;
using System.Reflection.Metadata;
using System.Reflection.PortableExecutable;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using Microsoft.Build.Framework;

namespace Kiln.Sdk;

/// <summary>
/// Writes the built mod's <c>kiln.programs.json</c> (<see cref="ProgramsPath"/>, the copy in the output folder) from its code:
/// every struct of the built assembly with Kiln.Systems' <c>[Query]</c> becomes one program entry, under the key
/// <c>Systems.Define</c> binds (the attribute's <c>Key</c>, or the struct's full name with <c>.</c> and <c>+</c> as <c>_</c>),
/// with the attribute's access lists and flags. kiln.world reserves these programs before the world starts (chapter 06a 9.5);
/// the author keeps no list by hand. Entries the project's own file declares stay; one that declares a [Query]'s key
/// with other lists is an error. The assembly is read with System.Reflection.Metadata and never loaded.
/// Error KILN036: a [Query] kiln.world cannot reserve (a bad key or name, no entity list, more than 8 programs, a
/// hand-written entry that says otherwise). Error KILN031: the assembly or the file cannot be read or written.
/// </summary>
public sealed class KilnPrograms : Microsoft.Build.Utilities.Task
{
    /// <summary>kiln.programs.json's lists, in the order the file and kiln.world (boot_programs.h kBootLists) use, with the
    /// [Query] property of each.</summary>
    public static readonly (string Json, string Property)[] Lists =
    [
        ("tpl", "Template"), ("tplOptional", "TemplateOptional"), ("read", "Read"), ("readOptional", "ReadOptional"),
        ("write", "Write"), ("writeOptional", "WriteOptional"), ("singletonsRead", "SingletonsRead"),
        ("singletonsWrite", "SingletonsWrite"), ("otherTpl", "OtherTemplate"), ("otherRead", "OtherRead"),
        ("otherWrite", "OtherWrite"), ("with", "With"), ("without", "Without"), ("anyOf", "AnyOf"),
        ("eventsIn", "EventsIn"), ("eventsOut", "EventsOut"),
    ];

    /// <summary>The [Query] flags, in kiln.programs.json's spelling.</summary>
    public static readonly (string Json, string Property)[] Flags =
        [("init", "Init"), ("hostOnly", "HostOnly"), ("exclusive", "Exclusive"), ("chunked", "Chunked"), ("pinned", "Pinned"), ("barrier", "Barrier")];

    /// <summary>The most programs one file may reserve (kiln-programs.schema.json, boot_programs.h).</summary>
    public const int MostPrograms = 8;

    /// <summary>The built mod assembly.</summary>
    [Required] public string AssemblyPath { get; set; } = "";

    /// <summary>The kiln.programs.json to write, in the output folder.</summary>
    [Required] public string ProgramsPath { get; set; } = "";

    /// <summary>The project's own kiln.programs.json (its hand-written entries), when it has one. The output's file is made
    /// from it and the [Query] structs each build, never from an earlier build's output.</summary>
    public string SourcePath { get; set; } = "";

    /// <summary>The keys written, ";"-separated (empty when the assembly has no [Query]).</summary>
    [Output] public string Written { get; private set; } = "";

    /// <summary>One [Query] struct as the build read it.</summary>
    public sealed record Query(string Type, string Key, IReadOnlyDictionary<string, string[]> Lists, IReadOnlyList<string> Flags, string? Problem);

    public override bool Execute()
    {
        if (!File.Exists(AssemblyPath)) return true;
        List<Query> queries;
        try
        {
            queries = Find(AssemblyPath);
        }
        catch (Exception e) when (e is IOException or BadImageFormatException or InvalidOperationException or UnauthorizedAccessException)
        {
            Log.LogError(null, "KILN031", null, AssemblyPath, 0, 0, 0, 0, $"cannot read the mod assembly for its [Query] programs: {e.Message}");
            return false;
        }
        if (queries.Count == 0) return true;
        var ok = true;
        foreach (var q in queries.Where(q => q.Problem is not null))
        {
            Log.LogError(null, "KILN036", null, AssemblyPath, 0, 0, 0, 0, $"[Query] {q.Type}: {q.Problem}");
            ok = false;
        }
        if (!ok) return false;
        try
        {
            var existing = SourcePath.Length > 0 && File.Exists(SourcePath) ? File.ReadAllText(SourcePath) : null;
            var (text, problems) = Merge(existing, queries);
            foreach (var p in problems) Log.LogError(null, "KILN036", null, ProgramsPath, 0, 0, 0, 0, p);
            if (problems.Count > 0) return false;
            Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(ProgramsPath))!);
            File.WriteAllText(ProgramsPath, text, new UTF8Encoding(false));
            Written = string.Join(";", queries.Select(q => q.Key));
            Log.LogMessage(MessageImportance.Normal, $"kiln.programs.json: {string.Join(", ", queries.Select(q => q.Key))} (the [Query] structs of {Path.GetFileName(AssemblyPath)})");
            return true;
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or JsonException or InvalidDataException)
        {
            Log.LogError(null, "KILN031", null, ProgramsPath, 0, 0, 0, 0, $"cannot write the mod's [Query] programs into kiln.programs.json: {e.Message}");
            return false;
        }
    }

    /// <summary>Every struct of the assembly with <c>Kiln.QueryAttribute</c> (Kiln.Systems), with what kiln.world would
    /// refuse of it in <see cref="Query.Problem"/>.</summary>
    public static List<Query> Find(string path)
    {
        using var pe = new PEReader(File.OpenRead(path));
        var md = pe.GetMetadataReader();
        var found = new List<Query>();
        foreach (var th in md.TypeDefinitions)
        {
            var type = md.GetTypeDefinition(th);
            foreach (var ah in type.GetCustomAttributes())
            {
                var ca = md.GetCustomAttribute(ah);
                if (!IsQueryAttribute(md, ca)) continue;
                var name = FullName(md, th);
                var value = ca.DecodeValue(new Types());
                string? key = null;
                var lists = new Dictionary<string, string[]>(StringComparer.Ordinal);
                var flags = new List<string>();
                foreach (var arg in value.NamedArguments)
                {
                    if (arg.Name == "Key") key = arg.Value as string;
                    else if (Array.FindIndex(Lists, l => l.Property == arg.Name) is var li and >= 0)
                        lists[Lists[li].Json] = arg.Value is ImmutableArray<CustomAttributeTypedArgument<string>> items
                            ? [.. items.Select(i => i.Value as string ?? "")] : [];
                    else if (Array.FindIndex(Flags, f => f.Property == arg.Name) is var fi and >= 0 && arg.Value is true)
                        flags.Add(Flags[fi].Json);
                }
                var ordered = Flags.Select(f => f.Json).Where(flags.Contains).ToList();
                key ??= name.Replace('.', '_').Replace('+', '_');
                found.Add(new Query(name, key, lists, ordered, Check(name, key, lists)));
            }
        }
        return found;
    }

    /// <summary>What kiln.world's boot parser (boot_programs.h parse_boot_programs) would refuse of a [Query] on type
    /// <paramref name="type"/> with <paramref name="key"/> and <paramref name="lists"/> (kiln.programs.json's names), said
    /// for the author; null when nothing.</summary>
    public static string? Check(string type, string key, IReadOnlyDictionary<string, string[]> lists)
    {
        if (type.Contains('`')) return "a generic struct cannot be a program (its key would not name one program)";
        if (key.Length is 0 or > 63 || !key.All(Word))
            return $"its program key '{key}' is not 1 to 63 letters, digits or _: set [Query(Key = \"...\")]";
        foreach (var (list, names) in lists)
        {
            var scoped = list is "singletonsRead" or "singletonsWrite";
            if (names.Length > 160) return $"{list} lists more than 160 names";
            foreach (var n in names)
                if (n.Length is 0 or > 120 || !n.All(c => Word(c) || (scoped && c == ':')))
                    return $"'{n}' in {list} is not a component name (letters, digits, _{(scoped ? ", ::" : "")})";
            if (names.Distinct(StringComparer.Ordinal).Count() != names.Length) return $"{list} lists a name twice";
        }
        int Count(params string[] of) => of.Sum(l => lists.TryGetValue(l, out var v) ? v.Length : 0);
        if (Count("tpl", "tplOptional", "read", "readOptional", "write", "writeOptional", "with", "anyOf") == 0)
            return "a [Query] is an entity program: it needs at least one template, read, write, with or anyOf name";
        if (Count("tpl", "tplOptional") > 8 || Count("read", "readOptional", "write", "writeOptional") > 42 ||
            Count("singletonsRead") > 64 || Count("singletonsWrite") > 64 || Count("otherTpl") > 9 ||
            Count("otherRead", "otherWrite") > 92 || Count("eventsIn", "eventsOut") > 24)
            return "it lists more than the engine allows (8 templates, 42 components, 64 singletons each way, 9 other templates, 92 other-entity names, 24 events)";
        foreach (var group in new[] { new[] { "tpl", "tplOptional" }, ["read", "readOptional", "write", "writeOptional"], ["with", "without", "anyOf"] })
        {
            var all = group.SelectMany(l => lists.TryGetValue(l, out var v) ? v : []).ToList();
            if (all.Distinct(StringComparer.Ordinal).Count() != all.Count)
                return $"a component appears twice in one access group ({string.Join(", ", group)})";
        }
        return null;
    }

    private static bool Word(char c) => c is (>= 'a' and <= 'z') or (>= 'A' and <= 'Z') or (>= '0' and <= '9') or '_';

    /// <summary>The entry kiln.programs.json gets for <paramref name="q"/>: its non-empty lists in file order, then its flags
    /// (always written: an absent "flags" means SYSTEM | HOST_ONLY to kiln.world, and a [Query] is an entity program).</summary>
    public static JsonObject Entry(Query q)
    {
        var o = new JsonObject();
        foreach (var (json, _) in Lists)
            if (q.Lists.TryGetValue(json, out var names) && names.Length > 0)
                o[json] = new JsonArray([.. names.Select(n => (JsonNode)JsonValue.Create(n)!)]);
        o["flags"] = new JsonArray([.. q.Flags.Select(f => (JsonNode)JsonValue.Create(f)!)]);
        return o;
    }

    /// <summary>The file with every [Query]'s entry: the project's own entries stay (in their order), the queries' follow
    /// by key. Problems: a hand-written entry under a query's key that says otherwise, more than <see cref="MostPrograms"/>.</summary>
    public static (string Text, List<string> Problems) Merge(string? existing, IReadOnlyList<Query> queries)
    {
        var problems = new List<string>();
        JsonObject programs;
        if (existing is null) programs = new JsonObject();
        else
        {
            var root = JsonNode.Parse(existing) as JsonObject ?? throw new InvalidDataException("kiln.programs.json: the file must contain one JSON object");
            if (root["format"]?.GetValue<int>() != 1) throw new InvalidDataException("kiln.programs.json: \"format\" must be 1");
            programs = root["programs"] as JsonObject ?? throw new InvalidDataException("kiln.programs.json: \"programs\" must be an object");
            programs = (JsonObject)programs.DeepClone();
        }
        foreach (var q in queries.OrderBy(q => q.Key, StringComparer.Ordinal))
        {
            var entry = Entry(q);
            if (programs[q.Key] is { } hand)
            {
                if (!JsonNode.DeepEquals(hand, entry))
                    problems.Add($"kiln.programs.json declares '{q.Key}' by hand with other lists or flags than [Query] {q.Type}: remove the hand-written entry (the build writes it)");
                continue;
            }
            programs[q.Key] = entry;
        }
        if (programs.Count > MostPrograms)
            problems.Add($"kiln.programs.json would reserve {programs.Count} programs; kiln.world reserves at most {MostPrograms} per mod");
        var file = new JsonObject { ["format"] = 1, ["programs"] = programs };
        return (file.ToJsonString(new JsonSerializerOptions { WriteIndented = true }) + "\n", problems);
    }

    private static bool IsQueryAttribute(MetadataReader md, CustomAttribute ca)
    {
        if (ca.Constructor.Kind != HandleKind.MemberReference) return false;
        var ctor = md.GetMemberReference((MemberReferenceHandle)ca.Constructor);
        if (ctor.Parent.Kind != HandleKind.TypeReference) return false;
        var t = md.GetTypeReference((TypeReferenceHandle)ctor.Parent);
        return md.StringComparer.Equals(t.Name, "QueryAttribute") && md.StringComparer.Equals(t.Namespace, "Kiln");
    }

    private static string FullName(MetadataReader md, TypeDefinitionHandle h)
    {
        var t = md.GetTypeDefinition(h);
        var name = md.GetString(t.Name);
        var outer = t.GetDeclaringType();
        if (!outer.IsNil) return FullName(md, outer) + "+" + name;
        var ns = md.GetString(t.Namespace);
        return ns.Length > 0 ? ns + "." + name : name;
    }

    // Decodes [Query]'s arguments: strings, string arrays and bools are all it has.
    private sealed class Types : ICustomAttributeTypeProvider<string>
    {
        public string GetPrimitiveType(PrimitiveTypeCode typeCode) => typeCode.ToString();
        public string GetSystemType() => "System.Type";
        public string GetSZArrayType(string elementType) => elementType + "[]";
        public string GetTypeFromDefinition(MetadataReader reader, TypeDefinitionHandle handle, byte rawTypeKind) => FullName(reader, handle);
        public string GetTypeFromReference(MetadataReader reader, TypeReferenceHandle handle, byte rawTypeKind)
        {
            var t = reader.GetTypeReference(handle);
            return reader.GetString(t.Namespace) + "." + reader.GetString(t.Name);
        }
        public string GetTypeFromSerializedName(string name) => name;
        public PrimitiveTypeCode GetUnderlyingEnumType(string type) => throw new BadImageFormatException($"[Query] has no enum argument ({type})");
        public bool IsSystemType(string type) => type == "System.Type";
    }
}
