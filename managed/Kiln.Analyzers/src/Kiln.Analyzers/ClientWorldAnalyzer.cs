// SPDX-License-Identifier: MIT
using System.Collections.Immutable;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp.Syntax;
using Microsoft.CodeAnalysis.Diagnostics;
using Microsoft.CodeAnalysis.Operations;

namespace Kiln.Analyzers;

/// <summary>KILN109 (since 0.2.0-dev.29): <c>EcsPrograms.TryBind(key, callback, WorldKind.Client)</c> or
/// <c>(…, WorldKind.Prediction)</c>, which kiln.world takes on the game's main thread only, in code that runs off the game's
/// threads (Task.Run, threads, timers, after ConfigureAwait(false)). The world argument must be a constant to be judged;
/// the tick thread is not reported (in OnLoad it is the main thread of a client mod in the menu).</summary>
[DiagnosticAnalyzer(LanguageNames.CSharp)]
public sealed class ClientWorldAnalyzer : DiagnosticAnalyzer
{
    public override ImmutableArray<DiagnosticDescriptor> SupportedDiagnostics => ImmutableArray.Create(Descriptors.ClientWorldOffMainThread);

    public override void Initialize(AnalysisContext context)
    {
        context.ConfigureGeneratedCodeAnalysis(GeneratedCodeAnalysisFlags.None);
        context.EnableConcurrentExecution();
        context.RegisterCompilationStartAction(start =>
        {
            var programs = start.Compilation.GetTypeByMetadataName("Kiln.EcsPrograms");
            if (programs is null) return;
            var classifier = new ThreadClassifier(start.Compilation);
            start.RegisterOperationAction(ctx =>
            {
                var call = (IInvocationOperation)ctx.Operation;
                if (call.TargetMethod.Name != "TryBind" || !SymbolEqualityComparer.Default.Equals(call.TargetMethod.ContainingType, programs))
                    return;
                var world = call.Arguments.FirstOrDefault(a => a.Parameter?.Name == "world")?.Value;
                if (world?.ConstantValue is not { HasValue: true, Value: int kind } || kind is not (1 or 2)) return;  // Prediction, Client
                var where = classifier.Classify(call.Syntax, call.SemanticModel!, ctx.CancellationToken);
                if (where.Kind != ThreadKind.Off) return;
                var at = call.Syntax is InvocationExpressionSyntax { Expression: MemberAccessExpressionSyntax ma } ? ma.Name.GetLocation() : call.Syntax.GetLocation();
                ctx.ReportDiagnostic(Diagnostic.Create(Descriptors.ClientWorldOffMainThread, at, "EcsPrograms.TryBind", where.Where));
            }, OperationKind.Invocation);
        });
    }
}
