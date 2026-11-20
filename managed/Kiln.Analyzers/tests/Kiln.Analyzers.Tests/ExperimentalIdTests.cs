// SPDX-License-Identifier: MIT
using System.Diagnostics.CodeAnalysis;
using System.Reflection;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;
using Xunit;

namespace Kiln.Analyzers.Tests;

/// <summary>Kiln.World's task-thread world reads and writes (WorldConcurrent and its types) have an experimental id of
/// their own, KILNX003, apart from KILNX001's lab and test members: a mod opts in to them with KILNX003 alone, and
/// KILNX001 does not opt it in.</summary>
public sealed class ExperimentalIdTests
{
    private const string Reads = "KILNX003";
    private const string Lab = "KILNX001";

    /// <summary>The reads, the writes and WorldConcurrent itself: Kiln.World's public types marked experimental.</summary>
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

    private static IReadOnlyList<Diagnostic> Build(params string[] noWarn)
    {
        var options = new CSharpCompilationOptions(OutputKind.DynamicallyLinkedLibrary, allowUnsafe: true,
                nullableContextOptions: NullableContextOptions.Enable)
            .WithSpecificDiagnosticOptions(noWarn.Select(id => KeyValuePair.Create(id, ReportDiagnostic.Suppress)));  // <NoWarn>
        var parse = new CSharpParseOptions(LanguageVersion.Preview);
        var comp = CSharpCompilation.Create("TestMod",
            [CSharpSyntaxTree.ParseText("global using System; global using System.Threading.Tasks; global using Kiln;", parse),
             CSharpSyntaxTree.ParseText(Mod, parse, "Mod.cs")],
            Harness.References, options);
        return comp.GetDiagnostics().Where(d => d.Severity is DiagnosticSeverity.Error or DiagnosticSeverity.Warning).ToList();
    }

    private static string Show(IEnumerable<Diagnostic> d) => string.Join("\n", d.Select(x => $"{x.Id}: {x.GetMessage()}"));

    [Fact]
    public void A_mod_that_opts_in_with_KILNX003_alone_builds_without_a_warning()
    {
        var d = Build(Reads);
        Assert.True(d.Count == 0, "expected no warnings or errors:\n" + Show(d));
    }

    [Fact]
    public void KILNX001_alone_does_not_opt_a_mod_in_to_the_world_reads_and_writes()
    {
        var d = Build(Lab);
        Assert.NotEmpty(d);
        Assert.All(d, x => Assert.Equal((Reads, DiagnosticSeverity.Error), (x.Id, x.Severity)));
        // the reads' types and members and the writes' are each reported under the new id
        Assert.Contains(d, x => x.GetMessage().Contains("WorldConcurrent"));
        Assert.Contains(d, x => x.GetMessage().Contains("ExchangeResult"));
    }

    [Fact]
    public void Without_an_opt_in_every_use_is_a_KILNX003_error()
    {
        var d = Build();
        Assert.NotEmpty(d);
        Assert.All(d, x => Assert.Equal(Reads, x.Id));
    }

    private static string? Id(MemberInfo m) => m.GetCustomAttribute<ExperimentalAttribute>(inherit: false)?.DiagnosticId;

    [Fact]
    public void Every_public_member_of_the_world_reads_and_writes_carries_KILNX003()
    {
        var world = typeof(ComponentChanges).Assembly;  // Kiln.World, by a type that is not experimental
        // the family is exactly Kiln.World's public types marked experimental, each with KILNX003
        var marked = world.GetExportedTypes().Where(t => Id(t) != null).ToDictionary(t => t.FullName!, t => Id(t));
        Assert.Equal(Family.Order(), marked.Keys.Order());
        Assert.All(marked, p => Assert.True(p.Value == Reads, $"{p.Key} is [Experimental(\"{p.Value}\")]"));
        // and no member of theirs carries another id (a member's own mark would replace the type's for its callers)
        const BindingFlags All = BindingFlags.Public | BindingFlags.Static | BindingFlags.Instance | BindingFlags.DeclaredOnly;
        foreach (var name in Family)
        {
            var type = world.GetType(name, throwOnError: true)!;
            var members = type.GetMembers(All);
            Assert.NotEmpty(members);
            foreach (var m in members)
                Assert.True((Id(m) ?? Id(type)) == Reads, $"{name}.{m.Name} is [Experimental(\"{Id(m)}\")]");
        }
        // what stays KILNX001 in Kiln.World: the lab members only (hidden from completion, named Lab*)
        var lab = world.GetExportedTypes().SelectMany(t => t.GetMembers(All)).Where(m => Id(m) == Lab)
            .Select(m => $"{m.DeclaringType!.Name}.{m.Name}").Order().ToList();
        Assert.Equal(["ComponentChanges.LabCounters", "ComponentChanges.LabHoldDelivery"], lab);
    }
}
