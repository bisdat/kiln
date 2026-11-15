// SPDX-License-Identifier: MIT
using System.Reflection;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;
using Xunit;

namespace Kiln.ImpactNatives.Tests;

public sealed class ImpactNativesGeneratorTests
{
    static readonly string Snapshots = typeof(ImpactNativesGeneratorTests).Assembly.GetCustomAttributes<AssemblyMetadataAttribute>()
        .Single(a => a.Key == "KilnImpactSnapshots").Value!;

    // Every parameter kind: values, ref (live), out, Optional<T>, [Raw] uint, TimeSpan (copied), the context, the three
    // results.
    const string Kinds = """
        using System;
        using System.Numerics;
        using Kiln;
        namespace FrostMark;
        public static class Hits
        {
            [ImpactNative("OnHit")]
            public static bool OnHit(in NativeContext ctx, EntityId target, EntityId attacker, ref float attackMod) => true;

            [ImpactNative("Kinds")]
            internal static byte Kinds(int s, uint u, float f, bool b, TimeSpan t, EntityId e, Vector3 v, DamageSetup d) => 3;

            [ImpactNative("Writes")]
            public static void Writes(ref int s, out uint u, ref TimeSpan t, out TimeSpan t2, out Vector3 v, ref DamageSetup d)
            {
                s++; u = 1; t += TimeSpan.FromSeconds(1); t2 = default; v = default;
            }

            [ImpactNative("Maybe")]
            public static bool Maybe(NativeContext ctx, Optional<float> scale, Optional<TimeSpan> wait, [Raw] uint tag) => scale.HasValue;
        }
        """;

    static MetadataReference[] Refs()
    {
        var refs = ((string)AppContext.GetData("TRUSTED_PLATFORM_ASSEMBLIES")!).Split(Path.PathSeparator)
            .Where(p => Path.GetFileName(p).StartsWith("System.", StringComparison.Ordinal) || Path.GetFileName(p) is "netstandard.dll" or "mscorlib.dll")
            .Select(p => (MetadataReference)MetadataReference.CreateFromFile(p)).ToList();
        refs.Add(MetadataReference.CreateFromFile(typeof(Kiln.Mod).Assembly.Location));
        refs.Add(MetadataReference.CreateFromFile(typeof(Kiln.ImpactNativeAccess).Assembly.Location));
        return [.. refs];
    }

    static (string? Source, IReadOnlyList<Diagnostic> Generator, IReadOnlyList<Diagnostic> Compile) Run(string source, bool withAbilities = true)
    {
        var refs = Refs().Where(r => withAbilities || !r.Display!.EndsWith("Kiln.Abilities.dll")).ToArray();
        var c = CSharpCompilation.Create("Mod", [CSharpSyntaxTree.ParseText(source, new CSharpParseOptions(LanguageVersion.Preview))], refs,
            new CSharpCompilationOptions(OutputKind.DynamicallyLinkedLibrary, allowUnsafe: true, nullableContextOptions: NullableContextOptions.Enable));
        GeneratorDriver driver = CSharpGeneratorDriver.Create([new ImpactNativesGenerator().AsSourceGenerator()],
            parseOptions: new CSharpParseOptions(LanguageVersion.Preview));
        driver = driver.RunGeneratorsAndUpdateCompilation(c, out var output, out var diags);
        var result = driver.GetRunResult().Results.Single();
        var text = result.GeneratedSources.FirstOrDefault().SourceText?.ToString();
        var compile = output.GetDiagnostics().Where(d => d.Severity == DiagnosticSeverity.Error).ToList();
        return (text, [.. diags, .. result.Diagnostics.Except(diags)], compile);
    }

    [Fact]
    public void EveryParameterKindIsTheCommittedSnapshotAndCompiles()
    {
        var (text, gen, compile) = Run(Kinds);
        Assert.Empty(gen);
        Assert.Empty(compile.Select(d => d.ToString()));
        Assert.NotNull(text);
        var path = Path.Combine(Snapshots, "Kinds.g.cs.txt");
        if (Environment.GetEnvironmentVariable("KILN_IMPACT_REGEN") == "1") File.WriteAllText(path, text);
        Assert.True(File.Exists(path) && File.ReadAllText(path) == text,
            $"{path} is not the generator's output: run KILN_IMPACT_REGEN=1 dotnet test managed/Kiln.Analyzers/tests/Kiln.ImpactNatives.Tests and review the diff");
    }

    [Fact]
    public void TheNativesJsonEntriesCarryTypeSizeDirectionAndFlags()
    {
        var (text, _, _) = Run(Kinds);
        Assert.Contains("""[assembly: global::Kiln.ImpactNativeEntryAttribute("OnHit", "[{\"name\":\"target\",\"type\":\"ecs.EntityId\",\"dir\":\"in\",\"size\":4},{\"name\":\"attacker\",\"type\":\"ecs.EntityId\",\"dir\":\"in\",\"size\":4},{\"name\":\"attackMod\",\"type\":\"float32\",\"dir\":\"inout\",\"size\":4}]")]""", text);
        Assert.Contains("""{\"name\":\"tag\",\"type\":\"any\",\"dir\":\"in\",\"size\":4,\"raw\":true}""", text);
        Assert.Contains("""{\"name\":\"scale\",\"type\":\"float32\",\"dir\":\"in\",\"size\":4,\"optional\":true}""", text);
        Assert.Contains("""{\"name\":\"t\",\"type\":\"Time\",\"dir\":\"inout\",\"size\":8}""", text);
        Assert.Contains("""{\"name\":\"d\",\"type\":\"impact.DamageSetup\",\"dir\":\"inout\",\"size\":52}""", text);
        Assert.Contains("""{\"name\":\"v\",\"type\":\"float3\",\"dir\":\"out\",\"size\":12}""", text);
        // the context is no argument; the NativeRef names the parameters for Blocks' bindings
        Assert.Contains("""public static readonly global::Kiln.NativeRef OnHit = new("OnHit", new global::Kiln.NativeParameter[] { new("target",""", text);
    }

    [Theory]
    [InlineData("""[ImpactNative("Bad-Name")] public static bool M() => true;""", "KILN170", "is not [A-Za-z0-9_]+")]
    [InlineData("""[ImpactNative("M")] public bool M() => true;""", "KILN170", "must be static")]
    [InlineData("""[ImpactNative("M")] private static bool M() => true;""", "KILN170", "must be internal or public")]
    [InlineData("""[ImpactNative("M")] public static bool M<T>() => true;""", "KILN170", "must not be generic")]
    [InlineData("""[ImpactNative("M")] public static bool M(string s) => true;""", "KILN171", "a native takes int, uint")]
    [InlineData("""[ImpactNative("M")] public static bool M(ref Optional<int> o) => true;""", "KILN171", "read only")]
    [InlineData("""[ImpactNative("M")] public static bool M([Raw] int r) => true;""", "KILN171", "[Raw] goes on a uint")]
    [InlineData("""[ImpactNative("M")] public static bool M(int a, NativeContext c) => true;""", "KILN171", "must be the first parameter")]
    [InlineData("""[ImpactNative("M")] public static int M() => 1;""", "KILN172", "returns bool, byte or void")]
    [InlineData("""[ImpactNative("M")] public static bool M() => true; [ImpactNative("M")] public static bool N() => true;""", "KILN173", "is used by")]
    public void AShapeTheGeneratorCannotMakeIsAnErrorNamingTheMethod(string members, string id, string text)
    {
        var (_, gen, _) = Run($"using Kiln; public static class C {{ {members} }}");
        var d = Assert.Single(gen, g => g.Id == id);
        Assert.Equal(id == "KILN174" ? DiagnosticSeverity.Warning : DiagnosticSeverity.Error, d.Severity);
        Assert.Contains(text, d.GetMessage());
    }

    [Fact]
    public void WithoutKilnAbilitiesNothingIsGeneratedAndTheBuildSaysWhy()
    {
        // the attribute itself is Kiln.Abilities': a project that declares its own type of that name has no runtime
        var (text, gen, _) = Run("namespace Kiln { public sealed class ImpactNativeAttribute(string n) : System.Attribute; } " +
                                 "public static class C { [Kiln.ImpactNative(\"M\")] public static bool M() => true; }", withAbilities: false);
        Assert.Null(text);
        Assert.Equal("KILN174", Assert.Single(gen).Id);
    }

    [Fact]
    public void NoImpactNativeNoOutput()
    {
        var (text, gen, _) = Run("public static class C { public static bool M() => true; }");
        Assert.Null(text);
        Assert.Empty(gen);
    }
}
