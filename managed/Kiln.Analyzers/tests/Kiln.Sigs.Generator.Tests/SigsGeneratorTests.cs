// SPDX-License-Identifier: MIT
using System.Reflection;
using Kiln.Sigs.Generator;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;
using Microsoft.CodeAnalysis.Diagnostics;
using Microsoft.CodeAnalysis.Text;
using Xunit;

namespace Kiln.Sigs.Generator.Tests;

public sealed class SigsGeneratorTests
{
    static readonly string Repo = typeof(SigsGeneratorTests).Assembly.GetCustomAttributes<AssemblyMetadataAttribute>()
        .Single(a => a.Key == "KilnRepo").Value!;
    static readonly string KilnSigsSource = Path.Combine(Repo, "managed", "services", "Kiln.Sigs", "Sigs.g.cs");
    const string KilnSigsOrigin = "sigdb/symbols/server.json and server.d/*.json";

    // ---- the core's descriptors and Kiln.Sigs ----

    static IEnumerable<(string Path, string Text)> CoreFiles()
    {
        var dir = Path.Combine(Repo, "sigdb", "symbols");
        yield return ("sigdb/symbols/server.json", File.ReadAllText(Path.Combine(dir, "server.json")));
        foreach (var f in Directory.GetFiles(Path.Combine(dir, "server.d"), "*.json").OrderBy(f => f, StringComparer.Ordinal))
            yield return ("sigdb/symbols/server.d/" + Path.GetFileName(f), File.ReadAllText(f));
    }

    [Fact]
    public void TheCoresTenDescriptorsParseWithoutAProblem()
    {
        var m = SigsModel.Parse(CoreFiles());
        Assert.Empty(m.BadFiles);
        Assert.Empty(m.Problems.Select(p => $"{p.Symbol} ({p.Source}): {p.Message}"));
        Assert.Equal(new[]
        {
            "GetComponent", "abilities/RegisterNative", "items/AttrChanged", "items/AttrFind", "items/GetItemVolume",
            "rules/QuatRotate", "time/GetGameTime", "time/SetTimeScale", "weather/SampleAt", "world/ReadOther",
        }, m.Sigs.Keys);
        Assert.Equal(new[] { "AttrRef", "OptS32", "WeatherSample" }, m.Structs.Keys.OrderBy(k => k, StringComparer.Ordinal));
        Assert.Empty(SigsEmitter.Clashes(m));
    }

    [Fact]
    public void KilnSigsIsTheEmittersOutputOfTheCoresDescriptors()
    {
        var text = SigsEmitter.Emit(SigsModel.Parse(CoreFiles()), SigsEmitter.DefaultRoot, isPublic: true, origin: KilnSigsOrigin);
        if (Environment.GetEnvironmentVariable("KILN_SIGS_REGEN") == "1") File.WriteAllText(KilnSigsSource, text);
        Assert.True(File.ReadAllText(KilnSigsSource) == text,
            $"{KilnSigsSource} is not the generator's output of the core's descriptors: run KILN_SIGS_REGEN=1 dotnet test " +
            "managed/Kiln.Analyzers/tests/Kiln.Sigs.Generator.Tests and commit the file (and its PublicAPI lines)");
    }

    [Theory]
    [InlineData("time/SetTimeScale", "Time", "SetTimeScale")]
    [InlineData("GetComponent", "Core", "GetComponent")]
    [InlineData("my-mod/sub/do_it", "MyMod", "SubDoIt")]
    [InlineData("m/3d", "M", "_3d")]
    public void NamesAreTheAreaAndTheRestInPascalCase(string symbol, string area, string name)
    {
        Assert.Equal((area, name), SigsEmitter.NameOf(symbol));
    }

    // ---- the generated shape, per kind ----

    const string Fragment = """
        {
          "structs": {
            "Pair": {"size": 8, "align": 4, "fields": [{"name": "a", "type": "f32", "offset": 0}, {"name": "b", "type": "f32", "offset": 4}]},
            "Big": {"size": 12, "align": 4, "fields": [{"name": "x", "type": "u32", "offset": 0}]},
            "Half": {"size": 4, "align": 4},
            "Out24": {"size": 24, "align": 8}
          },
          "symbols": {
            "m/Inline": {"find": [{"pattern": "90"}], "sig": {"ret": "u8", "args": [{"name": "flag", "type": "bool"}, {"name": "tiny", "type": "s8"}], "invoke": "any"}}
          },
          "sigs": {
            "m/Narrow": {"conv": "win64", "ret": "bool", "args": [{"name": "a", "type": "ptr"}, {"name": "b", "type": "u32"}, {"name": "c", "type": "ptr"},
                         {"name": "d", "type": "ptr"}, {"name": "argc", "type": "u8"}, {"name": "s16", "type": "s16"}],
                         "abi": {"reads": ["rcx", "rdx", "r8", "r9"], "stack": ["0x28", "0x30"]}, "invoke": "none"},
            "m/Floats": {"ret": "f64", "args": [{"name": "i", "type": "s64"}, {"name": "f", "type": "f32"}, {"name": "d", "type": "f64"},
                         {"name": "j", "type": "u64"}, {"name": "g", "type": "f32"}, {"name": "h", "type": "f64"}], "invoke": "any"},
            "m/ByValue": {"ret": "Half", "args": [{"name": "pair", "type": "Pair"}, {"name": "big", "type": "Big"}, {"name": "accessor", "type": "u16*"},
                          {"name": "daytime", "type": "keen::Daytime*"}], "invoke": "any"},
            "m/Sret": {"sret": "Out24", "args": [{"name": "result", "type": "s32"}], "abi": "trust", "invoke": "tick"}
          }
        }
        """;

    static string EmitFragment() => SigsEmitter.Emit(SigsModel.Parse([("sigdb/server.json", Fragment)]));

    [Fact]
    public void NarrowIntegersAreMaskedOnTheStackAndInRax()
    {
        var s = EmitFragment();
        // argc: a u8 at position 4 (the first stack slot): the slot's upper bytes are garbage (lab T1), the cast masks them
        Assert.Contains("/// <summary>Argument 5 <c>argc</c> (u8, stack+0x28).</summary>", s);
        Assert.Contains("public byte Argc { get => unchecked((byte)_c.Int(4)); set => _c.SetInt(4, (ulong)value); }", s);
        Assert.Contains("public short S16 { get => unchecked((short)_c.Int(5)); set => _c.SetInt(5, unchecked((ulong)(long)value)); }", s);
        Assert.Contains("public bool Result { get => (unchecked((byte)_c.RetInt) != 0); set => _c.SetRetInt((value ? 1UL : 0UL)); }", s);
        Assert.Contains("public uint B { get => unchecked((uint)_c.Int(1)); set => _c.SetInt(1, (ulong)value); }", s);
        Assert.Contains("public sbyte Tiny { get => unchecked((sbyte)_c.Int(1)); set => _c.SetInt(1, unchecked((ulong)(long)value)); }", s);
        Assert.Contains("public bool Flag { get => (unchecked((byte)_c.Int(0)) != 0); set => _c.SetInt(0, (value ? 1UL : 0UL)); }", s);
        Assert.Contains("return unchecked((byte)__frame.RetInt);", s);
    }

    [Fact]
    public void FloatsTravelInTheirXmmPositionOrOnTheStack()
    {
        var s = EmitFragment();
        Assert.Contains("/// <summary>Argument 2 <c>f</c> (f32, xmm1).</summary>", s);
        Assert.Contains("public float F { get => _c.F32(1); set => _c.SetF32(1, value); }", s);
        Assert.Contains("public double D { get => _c.F64(2); set => _c.SetF64(2, value); }", s);
        Assert.Contains("/// <summary>Argument 5 <c>g</c> (f32, stack+0x28).</summary>", s);
        Assert.Contains("public float G { get => _c.F32(4); set => _c.SetF32(4, value); }", s);
        Assert.Contains("public double H { get => _c.F64(5); set => _c.SetF64(5, value); }", s);
        Assert.Contains("public double Result { get => _c.RetF64; set => _c.SetRetF64(value); }", s);
        Assert.Contains("__frame.SetF32(4, g);", s);
        Assert.Contains("return __frame.RetF64;", s);
    }

    [Fact]
    public void AReturnBufferShiftsTheArgumentsAndInvokeReturnsTheStructByValue()
    {
        var s = EmitFragment();
        Assert.Contains("Descriptor = new(Symbol, global::Kiln.SigKind.Struct, 24, true, global::Kiln.SigInvoke.Tick", s);
        Assert.Contains("/// <summary>Argument 1 <c>result</c> (s32, rdx).</summary>", s);  // position 1: rcx is the buffer
        Assert.Contains("public int ResultArg { get => unchecked((int)_c.Int(1)); set => _c.SetInt(1, unchecked((ulong)(long)value)); }", s);
        Assert.Contains("public ref global::Kiln.Sigs.Out24 Result => ref *(global::Kiln.Sigs.Out24*)_c.ReturnBuffer;", s);
        Assert.Contains("public static global::Kiln.Sigs.Out24 Invoke(int result)", s);
        Assert.Contains("var __result = default(global::Kiln.Sigs.Out24);", s);
        Assert.Contains("__frame.SetInt(0, unchecked((ulong)(nint)(&__result)));", s);
        Assert.Contains("__frame.SetInt(1, unchecked((ulong)(long)result));", s);
        Assert.Contains("return __result;", s);
        Assert.Contains("[global::Kiln.AdvancedApi, global::Kiln.TickThreadOnly, global::Kiln.RequiresCapability(\"hooks.typed\"), global::Kiln.RequiresCapability(\"sig/m/Sret\")]", s);
    }

    [Fact]
    public void ByValueStructsTravelInTheSlotOrByReferenceAndPointersKeepTheirType()
    {
        var s = EmitFragment();
        Assert.Contains("public ref global::Kiln.Sigs.Pair Pair => ref *(global::Kiln.Sigs.Pair*)_c.SlotAddress(0);", s);  // 8 bytes: in rcx
        Assert.Contains("public ref global::Kiln.Sigs.Big Big => ref *(global::Kiln.Sigs.Big*)unchecked((nint)(long)_c.Int(1));", s);  // 12: a pointer
        Assert.Contains("public ushort* Accessor { get => (ushort*)unchecked((nint)(long)_c.Int(2)); set => _c.SetInt(2, unchecked((ulong)(nint)value)); }", s);
        Assert.Contains("public nint Daytime { get => unchecked((nint)(long)_c.Int(3)); set => _c.SetInt(3, unchecked((ulong)(long)value)); }", s);
        Assert.Contains("public ref global::Kiln.Sigs.Half Result => ref *(global::Kiln.Sigs.Half*)_c.RetAddress;", s);  // 4 bytes: in rax
        Assert.Contains("*(global::Kiln.Sigs.Pair*)&__slot0 = pair;", s);
        Assert.Contains("__frame.SetInt(1, unchecked((ulong)(nint)__copy1));", s);
        Assert.Contains("return *(global::Kiln.Sigs.Half*)&__rax;", s);
        Assert.Contains("[global::System.Runtime.InteropServices.FieldOffset(4)] public float B;", s);
        Assert.Contains("public const int SizeOf = 12;", s);
    }

    [Theory]
    [InlineData(12)]
    [InlineData(16)]
    [InlineData(24)]
    public void IndirectAggregateTemporaryIsExplicitlyAligned(int size)
    {
        var json = "{\"structs\":{\"Aggregate\":{\"size\":" + size + ",\"align\":16}},\"sigs\":{\"m/F\":{\"args\":[{\"name\":\"a\",\"type\":\"Aggregate\"}],\"invoke\":\"any\"}}}";
        var text = SigsEmitter.Emit(SigsModel.Parse([("sigdb/server.json", json)]));
        Assert.Contains($"stackalloc byte[{size + 15}]", text);
        Assert.Contains("+ 15) & ~(nuint)15", text);
        Assert.Contains("*__copy0 = a;", text);
        Assert.DoesNotContain("(&__copy0)", text);
    }

    [Fact]
    public void AHookOnlyDescriptorHasNoInvokeAndHooksAreAnyThread()
    {
        var s = EmitFragment();
        var narrow = s.Substring(s.IndexOf("internal static unsafe class Narrow", StringComparison.Ordinal));
        narrow = narrow.Substring(0, narrow.IndexOf("\n    }\n", StringComparison.Ordinal));
        Assert.DoesNotContain("Invoke(", narrow);
        Assert.Contains("[global::Kiln.AdvancedApi, global::Kiln.AnyThread, global::Kiln.RequiresCapability(\"hooks.typed\")]", narrow);
        Assert.Contains("internal static unsafe class Narrow", narrow);
        Assert.Contains("its code reads [rcx, rdx, r8, r9, stack+0x28, stack+0x30]", s);  // the class summary, from "abi"
    }

    // ---- through the compiler ----

    sealed class Text(string path, string? text) : AdditionalText
    {
        public override string Path { get; } = path;
        public override SourceText? GetText(CancellationToken cancellationToken = default) => text is null ? null : SourceText.From(text);
    }

    sealed class Opts(Dictionary<string, string> values) : AnalyzerConfigOptions
    {
        public override bool TryGetValue(string key, out string value) => values.TryGetValue(key, out value!);
    }

    sealed class Provider(Dictionary<string, string> global, HashSet<string> sigdb) : AnalyzerConfigOptionsProvider
    {
        public override AnalyzerConfigOptions GlobalOptions { get; } = new Opts(global);
        public override AnalyzerConfigOptions GetOptions(SyntaxTree tree) => new Opts(new());
        public override AnalyzerConfigOptions GetOptions(AdditionalText textFile) =>
            new Opts(sigdb.Contains(textFile.Path) ? new() { ["build_metadata.AdditionalFiles.KilnSigdb"] = "true" } : new());
    }

    static MetadataReference[] Refs(bool hooks = true, bool sigs = true)
    {
        var refs = ((string)AppContext.GetData("TRUSTED_PLATFORM_ASSEMBLIES")!).Split(Path.PathSeparator)
            .Where(p => Path.GetFileName(p).StartsWith("System.", StringComparison.Ordinal) || Path.GetFileName(p) is "netstandard.dll" or "mscorlib.dll")
            .Select(p => (MetadataReference)MetadataReference.CreateFromFile(p)).ToList();
        refs.Add(MetadataReference.CreateFromFile(typeof(Kiln.Mod).Assembly.Location));
        if (hooks) refs.Add(MetadataReference.CreateFromFile(typeof(Kiln.TypedHooks).Assembly.Location));
        if (sigs) refs.Add(MetadataReference.CreateFromFile(Assembly.Load("Kiln.Sigs").Location));
        return refs.ToArray();
    }

    static (Compilation Out, GeneratorDriverRunResult Run) Generate((string Path, string? Text, bool Tagged)[] files, string usage = "class Use { }",
                                                                    bool hooks = true, bool sigs = true, string? root = null)
    {
        var global = new Dictionary<string, string>();
        if (root != null) global["build_property.KilnSigsNamespace"] = root;
        var comp = CSharpCompilation.Create("mod", [CSharpSyntaxTree.ParseText(usage, new CSharpParseOptions(LanguageVersion.Preview))], Refs(hooks, sigs),
            new CSharpCompilationOptions(OutputKind.DynamicallyLinkedLibrary, allowUnsafe: true, nullableContextOptions: NullableContextOptions.Enable));
        GeneratorDriver driver = CSharpGeneratorDriver.Create([new SigsGenerator().AsSourceGenerator()],
            files.Select(f => (AdditionalText)new Text(f.Path, f.Text)), CSharpParseOptions.Default.WithLanguageVersion(LanguageVersion.Preview),
            new Provider(global, files.Where(f => f.Tagged).Select(f => f.Path).ToHashSet()));
        driver = driver.RunGeneratorsAndUpdateCompilation(comp, out var output, out _);
        return (output, driver.GetRunResult());
    }

    static void NoErrors(Compilation c)
    {
        var errors = c.GetDiagnostics().Where(d => d.Severity == DiagnosticSeverity.Error).Select(d => d.ToString()).ToList();
        Assert.True(errors.Count == 0, string.Join("\n", errors.Take(20)));
    }

    const string Usage = """
        using Kiln.Sigs.M;
        static unsafe class Use
        {
            static void All()
            {
                Kiln.HookResult r = Narrow.Detour((ref Narrow.Call c) =>
                {
                    byte n = c.Argc; short s = c.S16; bool res = c.Result; c.Result = true; c.Argc = (byte)(n + 1);
                    if (!c.OriginalCalled) c.CallOriginal();
                    c.Skip();
                }, priority: 5);
                double d = Floats.Invoke(1, 2f, 3.0, 4, 5f, 6.0);
                Kiln.Sigs.Half h = ByValue.Invoke(new Kiln.Sigs.Pair { A = 1, B = 2 }, new Kiln.Sigs.Big { X = 3 }, (ushort*)0, 0);
                ByValue.Detour((ref ByValue.Call c) => { c.Pair.A = 2; ref Kiln.Sigs.Half rh = ref c.Result; ushort* acc = c.Accessor; });
                Kiln.Sigs.Out24 o = Sret.Invoke(5);
                Sret.Detour((ref Sret.Call c) => { ref Kiln.Sigs.Out24 buf = ref c.Result; int x = c.ResultArg; });
                byte u = Inline.Invoke(true, -1);
                string sym = Sret.Symbol, row = Sret.Capability;
                Kiln.SigDescriptor desc = Floats.Descriptor;
            }
        }
        """;

    [Fact]
    public void AFragmentsClassesCompileAgainstKilnHooksAndAreUsable()
    {
        var (output, run) = Generate([("/mod/sigdb/server.json", Fragment, true), ("/mod/notes.json", "{}", false)], Usage, sigs: false);
        Assert.Empty(run.Diagnostics);
        NoErrors(output);
        var gen = Assert.Single(Assert.Single(run.Results).GeneratedSources);
        Assert.Equal("KilnSigs.g.cs", gen.HintName);
        Assert.Contains("internal static unsafe class Floats", gen.SourceText.ToString());
    }

    [Fact]
    public void TheCoresOwnDescriptorsCompileAsAModsWould()
    {
        // the same emitter wrote Kiln.Sigs: its classes compile whole (all ten, the three structs)
        var files = CoreFiles().Select(f => (f.Path, (string?)f.Text, true)).ToArray();
        var (output, run) = Generate(files, "class Use { static void A() { Kiln.Sigs.Items.GetItemVolume.Invoke(0); } }", sigs: false, root: "Kiln.Sigs");
        Assert.Empty(run.Diagnostics);
        NoErrors(output);
    }

    [Fact]
    public void AModsDescriptorMayNameAStructOfKilnSigs()
    {
        const string frag = """{"sigs": {"m/Vol": {"sret": "OptS32", "args": [{"name": "item", "type": "ptr"}], "invoke": "any"}}}""";
        var (output, run) = Generate([("/m/sigdb/server.json", frag, true)],
            "class Use { static void A() { Kiln.Sigs.OptS32 v = Kiln.Sigs.M.Vol.Invoke(0); } }");
        Assert.Empty(run.Diagnostics);
        NoErrors(output);
        // without Kiln.Sigs the struct is unknown: the descriptor is reported and not generated
        (_, run) = Generate([("/m/sigdb/server.json", frag, true)], sigs: false);
        var d = Assert.Single(run.Diagnostics);
        Assert.Equal("KILN180", d.Id);
        Assert.Contains("struct 'OptS32' is not declared", d.GetMessage());
    }

    [Fact]
    public void AModsRootNamespaceIsAProperty()
    {
        var (output, run) = Generate([("/m/sigdb/server.json", Fragment, true)],
            "class Use { static void A() { double d = MyMod.Typed.M.Floats.Invoke(1, 2f, 3.0, 4, 5f, 6.0); } }", sigs: false, root: "MyMod.Typed");
        Assert.Empty(run.Diagnostics);
        NoErrors(output);
    }

    [Theory]
    [InlineData("""{"sigs": {"m/Bad": {"args": [{"name": "x", "type": "nosuch"}]}}}""", "type 'nosuch' is neither")]
    [InlineData("""{"structs": {"S16": {"size": 16}}, "sigs": {"m/Bad": {"ret": "S16"}}}""", "declare it as \"sret\"")]
    [InlineData("""{"sigs": {"m/Bad": {"conv": "cdecl"}}}""", "conv must be \"win64\"")]
    [InlineData("""{"sigs": {"m/Bad": {"varargs": true}}}""", "varargs")]
    [InlineData("""{"sigs": {"m/Bad": {"args": [{"name": "b", "type": "sret"}]}}}""", "never in args")]
    [InlineData("""{"sigs": {"m/Bad": {"invoke": "sometimes"}}}""", "invoke must be")]
    [InlineData("""{"sigs": {"m/Bad": {"args": [{"name": "a", "type": "u8"}, {"name": "a", "type": "u8"}]}}}""", "declared twice")]
    public void AMalformedDescriptorIsReportedAndNotGenerated(string json, string message)
    {
        var (output, run) = Generate([("/m/sigdb/server.json", json, true)]);
        var d = Assert.Single(run.Diagnostics);
        Assert.Equal("KILN180", d.Id);
        Assert.Contains("m/Bad", d.GetMessage());
        Assert.Contains(message, d.GetMessage());
        Assert.DoesNotContain(output.SyntaxTrees, t => t.ToString().Contains("class Bad"));
    }

    [Fact]
    public void TwentyOnePositionsAreTooMany()
    {
        var args = string.Join(", ", Enumerable.Range(0, 20).Select(i => $"{{\"name\": \"a{i}\", \"type\": \"u64\"}}"));
        var (_, run) = Generate([("/m/sigdb/server.json", "{\"structs\": {\"S\": {\"size\": 32}}, \"sigs\": {\"m/Bad\": {\"sret\": \"S\", \"args\": [" + args + "]}}}", true)]);
        Assert.Contains("more than 20 argument positions", Assert.Single(run.Diagnostics).GetMessage());
    }

    [Fact]
    public void ServerAndClientFragmentsMayDescribeOneSymbolAlike()
    {
        const string a = """{"sigs": {"m/X": {"ret": "u32", "args": [{"name": "p", "type": "ptr"}], "invoke": "any"}}}""";
        const string b = """{"sigs": {"m/X": {"ret": "u64", "args": [{"name": "p", "type": "ptr"}], "invoke": "any"}}}""";
        var (output, run) = Generate([("/m/sigdb/server.json", a, true), ("/m/sigdb/client.json", a, true)],
            "class Use { static void A() { uint v = Kiln.Sigs.M.X.Invoke(0); } }");
        Assert.Empty(run.Diagnostics);
        NoErrors(output);
        (_, run) = Generate([("/m/sigdb/server.json", a, true), ("/m/sigdb/client.json", b, true)]);
        Assert.Contains("described differently", Assert.Single(run.Diagnostics).GetMessage());
    }

    [Fact]
    public void TwoSymbolsWithOneClassNameAreReported()
    {
        const string json = """{"sigs": {"m/a-b": {"invoke": "none"}, "m/aB": {"invoke": "none"}}}""";
        var (output, run) = Generate([("/m/sigdb/server.json", json, true)]);
        var d = Assert.Single(run.Diagnostics);
        Assert.Equal("KILN180", d.Id);
        Assert.Contains("is taken by another descriptor", d.GetMessage());
        NoErrors(output);
    }

    [Fact]
    public void WithoutKilnHooksNothingIsGeneratedAndTheBuildSaysWhy()
    {
        var (output, run) = Generate([("/m/sigdb/server.json", Fragment, true)], hooks: false, sigs: false);
        var d = Assert.Single(run.Diagnostics);
        Assert.Equal("KILN181", d.Id);
        Assert.Contains("5 game function(s)", d.GetMessage());
        Assert.Empty(Assert.Single(run.Results).GeneratedSources);
        NoErrors(output);
    }

    [Fact]
    public void AnUnreadableFileIsReportedAndUntaggedFilesAreIgnored()
    {
        var (_, run) = Generate([("/m/sigdb/server.json", "{ not json", true), ("/m/other.json", Fragment, false)]);
        var d = Assert.Single(run.Diagnostics);
        Assert.Equal("KILN182", d.Id);
        Assert.Empty(Assert.Single(run.Results).GeneratedSources);
        (_, run) = Generate([("/m/other.json", Fragment, false)]);
        Assert.Empty(run.Diagnostics);
        Assert.Empty(Assert.Single(run.Results).GeneratedSources);
    }

    [Fact]
    public void AFragmentWithoutDescriptorsGeneratesNothing()
    {
        var (_, run) = Generate([("/m/sigdb/server.json", """{"symbols": {"m/F": {"find": [{"pattern": "90"}]}}}""", true)]);
        Assert.Empty(run.Diagnostics);
        Assert.Empty(Assert.Single(run.Results).GeneratedSources);
    }
}
