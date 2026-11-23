// SPDX-License-Identifier: MIT
using System;
using System.Collections.Immutable;
using System.Linq;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.Diagnostics;

namespace Kiln.Analyzers;

/// <summary>Cheap checks of the constant automatic-point declaration. The Host repeats these checks at load.</summary>
[DiagnosticAnalyzer(LanguageNames.CSharp)]
public sealed class RegistryAnalyzer : DiagnosticAnalyzer
{
    public override ImmutableArray<DiagnosticDescriptor> SupportedDiagnostics =>
        ImmutableArray.Create(Descriptors.RegistryOverrideMissing, Descriptors.RegistryPointsInvalid);
    public override void Initialize(AnalysisContext context)
    {
        context.ConfigureGeneratedCodeAnalysis(GeneratedCodeAnalysisFlags.None);
        context.EnableConcurrentExecution();
        context.RegisterCompilationStartAction(start =>
        {
            var mod = start.Compilation.GetTypeByMetadataName("Kiln.Mod");
            var attribute = start.Compilation.GetTypeByMetadataName("Kiln.RegistryEntryPointsAttribute");
            var onRegistry = mod?.GetMembers("OnRegistry").OfType<IMethodSymbol>().FirstOrDefault();
            if (mod is null || attribute is null || onRegistry is null) return;
            start.RegisterSymbolAction(ctx =>
            {
                var type = (INamedTypeSymbol)ctx.Symbol;
                var declaration = type.GetAttributes().FirstOrDefault(a => SymbolEqualityComparer.Default.Equals(a.AttributeClass, attribute));
                if (declaration is null) return;
                var bases = Ancestors(type).ToArray();
                if (!bases.Any(t => SymbolEqualityComparer.Default.Equals(t, mod))) return;
                var location = declaration.ApplicationSyntaxReference?.GetSyntax(ctx.CancellationToken).GetLocation()
                    ?? type.Locations.FirstOrDefault(l => l.IsInSource);
                if (location is null) return;
                var overridden = bases.SelectMany(t => t.GetMembers("OnRegistry").OfType<IMethodSymbol>()).Any(method =>
                {
                    for (var parent = method.OverriddenMethod; parent is not null; parent = parent.OverriddenMethod)
                        if (SymbolEqualityComparer.Default.Equals(parent, onRegistry)) return true;
                    return false;
                });
                if (!overridden) ctx.ReportDiagnostic(Diagnostic.Create(Descriptors.RegistryOverrideMissing, location, type.Name));
                if (declaration.ConstructorArguments.Length != 1 || declaration.ConstructorArguments[0].Value is not { } raw) return;
                var points = Convert.ToInt64(raw, System.Globalization.CultureInfo.InvariantCulture);
                if (points <= 0 || points > 15)
                    ctx.ReportDiagnostic(Diagnostic.Create(Descriptors.RegistryPointsInvalid, location, type.Name));
            }, SymbolKind.NamedType);
        });
    }
    private static System.Collections.Generic.IEnumerable<INamedTypeSymbol> Ancestors(INamedTypeSymbol type)
    {
        for (var current = type; current is not null; current = current.BaseType) yield return current;
    }
}
