// SPDX-License-Identifier: MIT
using System.Collections;
using System.Text.Json;
using Kiln.Sdk;
using Microsoft.Build.Framework;
using Xunit;

namespace Kiln.Blocks.Tests;

/// <summary>The SDK's KilnImpactNatives task (Kiln.Sdk.Tasks.dll) on this test assembly, whose [ImpactNative] methods
/// (NativesGeneratedTests) the generator gave their entry attributes: the impact/natives.json kiln.abilities reads.</summary>
public sealed class ImpactNativesTaskTests
{
    sealed class Engine : IBuildEngine
    {
        public readonly List<BuildErrorEventArgs> Errors = [];
        public readonly List<BuildWarningEventArgs> Warnings = [];
        public bool ContinueOnError => false;
        public int LineNumberOfTaskNode => 0;
        public int ColumnNumberOfTaskNode => 0;
        public string ProjectFileOfTaskNode => "ImpactNativesTaskTests";
        public bool BuildProjectFile(string projectFileName, string[] targetNames, IDictionary globalProperties, IDictionary targetOutputs) => false;
        public void LogCustomEvent(CustomBuildEventArgs e) { }
        public void LogErrorEvent(BuildErrorEventArgs e) => Errors.Add(e);
        public void LogMessageEvent(BuildMessageEventArgs e) { }
        public void LogWarningEvent(BuildWarningEventArgs e) => Warnings.Add(e);
    }

    static string Assembly => typeof(ImpactNativesTaskTests).Assembly.Location;

    static (bool Ok, Engine Engine, string? Natives) Run(string manifest, string? natives)
    {
        var dir = Directory.CreateDirectory(Path.Combine(Path.GetTempPath(), "kiln-impact-task", Guid.NewGuid().ToString("N"))).FullName;
        try
        {
            File.WriteAllText(Path.Combine(dir, "kiln.mod.json"), manifest);
            var path = Path.Combine(dir, "impact", "natives.json");
            if (natives is not null)
            {
                Directory.CreateDirectory(Path.GetDirectoryName(path)!);
                File.WriteAllText(path, natives);
            }
            var engine = new Engine();
            var task = new KilnImpactNatives { BuildEngine = engine, AssemblyPath = Assembly, ManifestPath = Path.Combine(dir, "kiln.mod.json"), NativesPath = path };
            var ok = task.Execute();
            return (ok, engine, File.Exists(path) ? File.ReadAllText(path) : null);
        }
        finally { Directory.Delete(dir, recursive: true); }
    }

    static JsonElement Entry(string natives, string name) =>
        JsonDocument.Parse(natives).RootElement.GetProperty("natives").EnumerateArray().Single(e => e.GetProperty("name").GetString() == name);

    [Fact]
    public void TheGeneratedEntriesAreTypedSyncDeclarationsUnderTheModsId()
    {
        var (ok, engine, text) = Run("""{ "id": "frost-mark", /* comment */ "version": "1.0.0", }""", null);
        Assert.True(ok, string.Join("; ", engine.Errors.Select(e => e.Message)));
        var e = Entry(text!, "frost-mark.GenOnHit");
        Assert.True(e.GetProperty("sync").GetBoolean());
        Assert.Equal(2, e.GetProperty("argc").GetInt32());
        var args = e.GetProperty("args").EnumerateArray().ToList();
        Assert.Equal(("target", "ecs.EntityId", "in", 4), (args[0].GetProperty("name").GetString(), args[0].GetProperty("type").GetString(),
            args[0].GetProperty("dir").GetString(), args[0].GetProperty("size").GetInt32()));
        Assert.Equal("inout", args[1].GetProperty("dir").GetString());
        Assert.Equal(1, JsonDocument.Parse(text!).RootElement.GetProperty("format").GetInt32());
    }

    [Fact]
    public void TheProjectsOwnEntriesStayAndOneOfTheSameNameIsReplacedWithAWarning()
    {
        const string own = """
            {"format": 1, "comment": "hand-written", "natives": [
              {"name": "frost-mark.Ping", "argc": 1, "suspends": true},
              {"name": "frost-mark.GenMaybe", "argc": 2}
            ]}
            """;
        var (ok, engine, text) = Run("""{"id": "frost-mark"}""", own);
        Assert.True(ok);
        Assert.Equal("hand-written", JsonDocument.Parse(text!).RootElement.GetProperty("comment").GetString());
        Assert.True(Entry(text!, "frost-mark.Ping").GetProperty("suspends").GetBoolean());
        Assert.True(Entry(text!, "frost-mark.GenMaybe").GetProperty("sync").GetBoolean());
        Assert.Equal("KILN037", Assert.Single(engine.Warnings).Code);
        // the same text again: no warning (the entry is the method's already)
        var (_, again, text2) = Run("""{"id": "frost-mark"}""", text);
        Assert.Empty(again.Warnings);
        Assert.Equal(text, text2);
    }

    [Fact]
    public void AManifestWithoutAnIdOrAnotherFormatIsAnError()
    {
        var (ok, engine, _) = Run("""{"version": "1.0.0"}""", null);
        Assert.False(ok);
        Assert.Equal("KILN036", Assert.Single(engine.Errors).Code);
        (ok, engine, _) = Run("""{"id": "m"}""", """{"format": 2, "natives": []}""");
        Assert.False(ok);
        Assert.Contains("format is not 1", Assert.Single(engine.Errors).Message);
    }

    [Fact]
    public void TheReaderFindsTheAttributesWithoutLoadingTheAssembly()
    {
        var entries = KilnImpactNatives.Read(Assembly);
        Assert.Equal(["GenMaybe", "GenOnHit", "GenWrites"], entries.Select(e => e.Name).Order());
        Assert.StartsWith("[{\"name\":\"scale\"", entries.Single(e => e.Name == "GenMaybe").Args);
    }
}
