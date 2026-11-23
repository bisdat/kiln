// SPDX-License-Identifier: MIT
using System.Diagnostics.CodeAnalysis;
using System.Reflection;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;
using Xunit;

namespace Kiln.Analyzers.Tests;

/// <summary>Kiln.World's task-thread world reads and writes (WorldConcurrent and its types) were experimental under
/// KILNX003 from 0.2.0-dev.29.18; 0.2.0-dev.30.7 makes them normal API: a mod uses them without any opt-in, a project that
/// still has KILNX003 in its NoWarn builds as before, and unrelated experimental API keeps its opt-in.</summary>
public sealed class ExperimentalIdTests
{
    private const string Reads = "KILNX003";
    private const string Lab = "KILNX001";

    /// <summary>The reads, the writes and WorldConcurrent itself: the types KILNX003 marked until 0.2.0-dev.30.7.</summary>
    private static readonly string[] Family =
    [
        "Kiln.WorldConcurrent", "Kiln.ReadMode", "Kiln.ReadStatus", "Kiln.ConcurrentEntity", "Kiln.ConcurrentRead`1",
        "Kiln.EntityQuery", "Kiln.ComponentSet", "Kiln.ConcurrentSnapshot", "Kiln.ConcurrentCopy",
        "Kiln.ConcurrentQueryResult", "Kiln.ExchangeStatus", "Kiln.ExchangeFlags", "Kiln.WriteReason",
        "Kiln.ExchangeResult`1", "Kiln.ExchangeBytesResult", "Kiln.ComponentWritePolicy", "Kiln.ComponentExchangedEvent",
    ];

    /// <summary>A mod's task that reads and writes the world, as FetchNearby and the guides do.</summary>
    private const string Mod = """
        public sealed class M : Mod
        {
            private ConcurrentEntity _e;

            public override void OnLoad()
            {
                Context.Tasks.Run("world", async ctx =>
                {
                    var open = WorldConcurrent.Open(new EntityId(12));
                    if (open.Status != ReadStatus.Ok) return;
                    _e = open.Value;
                    ConcurrentRead<int> hp = WorldConcurrent.Read<int>(_e, "Health");
                    WorldConcurrent.Query(new EntityQuery(WorldKind.Server, "Health"), new ConcurrentEntity[4]);
                    ExchangeResult<long> w = WorldConcurrent.TryCompareExchange(_e, "Charge", 1L, 2L);
                    if (w.Status == ExchangeStatus.Busy) await ctx.NextGateWindow();
                });
            }
        }
        """;

    private static IReadOnlyList<Diagnostic> Build(string source = Mod, params string[] noWarn)
    {
        var options = new CSharpCompilationOptions(OutputKind.DynamicallyLinkedLibrary, allowUnsafe: true,
                nullableContextOptions: NullableContextOptions.Enable)
            .WithSpecificDiagnosticOptions(noWarn.Select(id => KeyValuePair.Create(id, ReportDiagnostic.Suppress)));  // <NoWarn>
        var parse = new CSharpParseOptions(LanguageVersion.Preview);
        var comp = CSharpCompilation.Create("TestMod",
            [CSharpSyntaxTree.ParseText("global using System; global using System.Threading.Tasks; global using Kiln;", parse),
             CSharpSyntaxTree.ParseText(source, parse, "Mod.cs")],
            Harness.References, options);
        return comp.GetDiagnostics().Where(d => d.Severity is DiagnosticSeverity.Error or DiagnosticSeverity.Warning).ToList();
    }

    private static string Show(IEnumerable<Diagnostic> d) => string.Join("\n", d.Select(x => $"{x.Id}: {x.GetMessage()}"));

    [Fact]
    public void A_mod_uses_the_world_reads_and_writes_without_an_opt_in()
    {
        var d = Build();
        Assert.True(d.Count == 0, "expected no warnings or errors:\n" + Show(d));
    }

    [Fact]
    public void A_project_that_still_opts_in_with_KILNX003_builds_as_before()
    {
        var d = Build(noWarn: [Reads]);
        Assert.True(d.Count == 0, "expected no warnings or errors:\n" + Show(d));
    }

    [Theory]
    [InlineData(Lab, "public static class M { public static void Run() => ComponentChanges.LabCounters(); }")]
    [InlineData("KILNX002", "public static class M { public static string Run() => SteamGuard.Capability; }")]
    public void Retired_task_opt_in_does_not_suppress_other_experimental_API(string id, string source)
    {
        var d = Build(source, Reads);
        Assert.NotEmpty(d);
        Assert.All(d, x => Assert.Equal((id, DiagnosticSeverity.Error), (x.Id, x.Severity)));
        Assert.Empty(Build(source, id));
    }

    private static string? Id(MemberInfo m) => m.GetCustomAttribute<ExperimentalAttribute>(inherit: false)?.DiagnosticId;

    [Fact]
    public void Nothing_of_the_world_reads_and_writes_is_experimental()
    {
        var world = typeof(ComponentChanges).Assembly;
        const BindingFlags All = BindingFlags.Public | BindingFlags.Static | BindingFlags.Instance | BindingFlags.DeclaredOnly;
        foreach (var name in Family)
        {
            var type = world.GetType(name, throwOnError: true)!;
            Assert.True(Id(type) == null, $"{name} is still [Experimental(\"{Id(type)}\")]");
            foreach (var m in type.GetMembers(All))
                Assert.True(Id(m) == null, $"{name}.{m.Name} is still [Experimental(\"{Id(m)}\")]");
        }
        // what stays experimental in Kiln.World: the KILNX001 lab members only (hidden from completion, named Lab*); nothing
        // anywhere in it still names KILNX003
        var marked = world.GetExportedTypes().SelectMany(t => t.GetMembers(All).Append<MemberInfo>(t)).Where(m => Id(m) != null)
            .Select(m => $"{Id(m)} {(m is Type t ? t.FullName : $"{m.DeclaringType!.Name}.{m.Name}")}").Order().ToList();
        Assert.Equal([$"{Lab} ComponentChanges.LabCounters", $"{Lab} ComponentChanges.LabHoldDelivery"], marked);
    }
}
