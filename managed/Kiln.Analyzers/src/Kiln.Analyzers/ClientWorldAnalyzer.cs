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

    /// <summary>A call of <c>Kiln.EcsPrograms.TryBind</c> whose world is the constant <c>WorldKind.Client</c> or
    /// <c>WorldKind.Prediction</c>: the main thread's (KILN109), not the tick thread's (KILN101 leaves it to this rule).</summary>
    internal static bool IsClientWorldBind(IOperation op)
    {
        if (op is not IInvocationOperation call || call.TargetMethod.Name != "TryBind" ||
            call.TargetMethod.ContainingType?.ToDisplayString() != "Kiln.EcsPrograms")
            return false;
        var world = call.Arguments.FirstOrDefault(a => a.Parameter?.Name == "world")?.Value;
        return world?.ConstantValue is { HasValue: true, Value: int kind } && kind is 1 or 2;  // Prediction, Client
    }

    public override void Initialize(AnalysisContext context)
    {
        context.ConfigureGeneratedCodeAnalysis(GeneratedCodeAnalysisFlags.None);
        context.EnableConcurrentExecution();
        context.RegisterCompilationStartAction(start =>
        {
            if (start.Compilation.GetTypeByMetadataName("Kiln.EcsPrograms") is null) return;
            var classifier = new ThreadClassifier(start.Compilation);
            start.RegisterOperationAction(ctx =>
            {
                var call = (IInvocationOperation)ctx.Operation;
                if (!IsClientWorldBind(call)) return;
                var where = classifier.Classify(call.Syntax, call.SemanticModel!, ctx.CancellationToken);
                if (where.Kind != ThreadKind.Off) return;
                var at = call.Syntax is InvocationExpressionSyntax { Expression: MemberAccessExpressionSyntax ma } ? ma.Name.GetLocation() : call.Syntax.GetLocation();
                ctx.ReportDiagnostic(Diagnostic.Create(Descriptors.ClientWorldOffMainThread, at, "EcsPrograms.TryBind", where.Where));
            }, OperationKind.Invocation);
        });
    }
}
