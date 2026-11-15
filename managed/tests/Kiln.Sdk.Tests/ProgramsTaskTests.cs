// SPDX-License-Identifier: MIT
// The SDK's KilnPrograms task (Kiln.Sdk.Tasks.dll), run in-process on this test assembly, whose [Query] structs below are
// what a mod's would be: the kiln.programs.json entries it writes (chapter 06a 9.5), the keys Systems.Define binds, how
// it keeps a project's own entries and what it refuses.
using System.Collections;
using System.Text.Json.Nodes;
using Kiln.Sdk;
using Microsoft.Build.Framework;
using Xunit;

namespace Kiln.SdkTests;

[Query(Template = ["FuelSetup"], ReadOptional = ["CurrentTransform"], Write = ["Fuel"], SingletonsRead = ["impact::AttributeRegistry"],
    Without = ["DynamicShelter"], HostOnly = true)]
public ref struct SdkBarrels : IQuery<SdkBarrels>
{
    public EcsProgramContext Context;
    public static SdkBarrels Bind(EcsProgramContext context) => new() { Context = context };
}

public static class SdkOuter
{
    [Query(Key = "sdk_wells", Read = ["Fuel"], Pinned = true, Init = true)]
    public ref struct Wells : IQuery<Wells>
    {
        public static Wells Bind(EcsProgramContext context) => default;
    }
}

public class ProgramsTaskTests
{
    private sealed class Engine : IBuildEngine
    {
        public readonly List<BuildErrorEventArgs> Errors = [];
        public bool ContinueOnError => false;
        public int LineNumberOfTaskNode => 0;
        public int ColumnNumberOfTaskNode => 0;
        public string ProjectFileOfTaskNode => "ProgramsTaskTests";
        public bool BuildProjectFile(string projectFileName, string[] targetNames, IDictionary globalProperties, IDictionary targetOutputs) => false;
        public void LogCustomEvent(CustomBuildEventArgs e) { }
        public void LogErrorEvent(BuildErrorEventArgs e) => Errors.Add(e);
        public void LogMessageEvent(BuildMessageEventArgs e) { }
        public void LogWarningEvent(BuildWarningEventArgs e) { }
    }

    private static string Self => typeof(SdkBarrels).Assembly.Location;

    private static readonly JsonNode Expected = JsonNode.Parse("""
        {
          "format": 1,
          "programs": {
            "Kiln_SdkTests_SdkBarrels": {
              "tpl": ["FuelSetup"],
              "readOptional": ["CurrentTransform"],
              "write": ["Fuel"],
              "singletonsRead": ["impact::AttributeRegistry"],
              "without": ["DynamicShelter"],
              "flags": ["hostOnly"]
            },
            "sdk_wells": { "read": ["Fuel"], "flags": ["init", "pinned"] }
          }
        }
        """)!;

    private static (bool Ok, Engine Engine, string? Text) Run(string? source)
    {
        var dir = Directory.CreateDirectory(Path.Combine(AppContext.BaseDirectory, "programs-runs", Guid.NewGuid().ToString("N"))).FullName;
        var src = Path.Combine(dir, "project-kiln.programs.json");
        if (source is not null) File.WriteAllText(src, source);
        var output = Path.Combine(dir, "out", "kiln.programs.json");
        var engine = new Engine();
        var task = new KilnPrograms { BuildEngine = engine, AssemblyPath = Self, ProgramsPath = output, SourcePath = src };
        var ok = task.Execute();
        return (ok, engine, File.Exists(output) ? File.ReadAllText(output) : null);
    }

    [Fact]
    public void Each_Query_struct_becomes_one_kiln_programs_json_entry_under_the_key_Define_binds()
    {
        var found = KilnPrograms.Find(Self).OrderBy(q => q.Key, StringComparer.Ordinal).ToList();
        Assert.Equal(["Kiln_SdkTests_SdkBarrels", "sdk_wells"], found.Select(q => q.Key));
        Assert.All(found, q => Assert.Null(q.Problem));
        // the default key is the struct's full name with . and + as _ (Kiln.Systems' Systems.Define uses the same rule)
        Assert.Equal(typeof(SdkBarrels).FullName!.Replace('.', '_').Replace('+', '_'), found[0].Key);
        Assert.Equal("Kiln.SdkTests.SdkOuter+Wells", found[1].Type);
        var (ok, engine, text) = Run(null);
        Assert.True(ok, string.Join("; ", engine.Errors.Select(e => e.Message)));
        Assert.True(JsonNode.DeepEquals(Expected, JsonNode.Parse(text!)), text);
    }

    [Fact]
    public void A_project_s_own_entries_stay_and_a_second_build_writes_the_same_file()
    {
        const string own = """{ "format": 1, "programs": { "hand_made": { "read": ["Health"], "flags": [] } } }""";
        var (ok, engine, text) = Run(own);
        Assert.True(ok, string.Join("; ", engine.Errors.Select(e => e.Message)));
        var programs = JsonNode.Parse(text!)!["programs"]!.AsObject();
        Assert.Equal(["hand_made", "Kiln_SdkTests_SdkBarrels", "sdk_wells"], programs.Select(p => p.Key));
        // the same entry by hand is fine; a [Query]'s key with other lists is refused
        var same = KilnPrograms.Merge(text, KilnPrograms.Find(Self));
        Assert.Empty(same.Problems);
        Assert.Equal(text, same.Text);
        const string other = """{ "format": 1, "programs": { "sdk_wells": { "read": ["Health"], "flags": [] } } }""";
        var (ok2, engine2, _) = Run(other);
        Assert.False(ok2);
        var e = Assert.Single(engine2.Errors);
        Assert.Equal("KILN036", e.Code);
        Assert.Contains("'sdk_wells' by hand", e.Message);
    }

    [Fact]
    public void More_than_32_programs_and_what_kiln_world_cannot_reserve_are_errors()
    {
        var hand = new JsonObject();
        for (var i = 0; i < KilnPrograms.MostPrograms - 1; i++) hand[$"p{i}"] = new JsonObject { ["read"] = new JsonArray("Health"), ["flags"] = new JsonArray() };
        var (ok, engine, _) = Run(new JsonObject { ["format"] = 1, ["programs"] = hand }.ToJsonString());
        Assert.False(ok);   // 31 by hand and the two [Query] structs: 33
        Assert.Equal(32, KilnPrograms.MostPrograms);
        Assert.Contains(engine.Errors, x => x.Code == "KILN036" && x.Message.Contains("at most 32"));

        static Dictionary<string, string[]> L(params (string, string[])[] lists) => lists.ToDictionary(l => l.Item1, l => l.Item2);
        Assert.Null(KilnPrograms.Check("A", "a_b", L(("read", ["Fuel"]))));
        Assert.Contains("entity program", KilnPrograms.Check("A", "a_b", L(("without", ["Fuel"])))!);
        Assert.Contains("1 to 63", KilnPrograms.Check("A", new string('k', 64), L(("read", ["Fuel"])))!);
        Assert.Contains("1 to 63", KilnPrograms.Check("A", "a-b", L(("read", ["Fuel"])))!);
        Assert.Contains("not a component name", KilnPrograms.Check("A", "a", L(("read", ["keen::ecs::Fuel"])))!);
        Assert.Null(KilnPrograms.Check("A", "a", L(("read", ["Fuel"]), ("singletonsRead", ["impact::AttributeRegistry"]))));
        Assert.Contains("twice in one access group", KilnPrograms.Check("A", "a", L(("read", ["Fuel"]), ("write", ["Fuel"])))!);
        Assert.Contains("generic", KilnPrograms.Check("A`1", "a", L(("read", ["Fuel"])))!);
        Assert.Contains("more than the engine allows", KilnPrograms.Check("A", "a", L(("tpl", Enumerable.Range(0, 9).Select(i => $"T{i}").ToArray())))!);
    }
}
