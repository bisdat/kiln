// SPDX-License-Identifier: MIT
using System;
using System.Collections.Immutable;
using System.Linq;
using System.Text;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.Text;

namespace Kiln.Sigs.Generator;

/// <summary>
/// Typed hooks and calls of a mod's own game functions (chapter 06b §9.3; docs/kiln/services/hooks.md "Typed hooks"):
/// every <c>sig</c> descriptor of the mod's sigdb fragment (<c>sigdb/server.json</c>, <c>sigdb/client.json</c>: the
/// AdditionalFiles with metadata <c>KilnSigdb=true</c>, added by Kiln.Sdk's Kiln.Sigs.targets) becomes one class
/// <c>&lt;root&gt;.&lt;Area&gt;.&lt;Name&gt;</c> with <c>Detour</c>, <c>Call</c> and, where the descriptor offers it,
/// <c>Invoke</c>; its <c>"structs"</c> become structs in <c>&lt;root&gt;</c>. Build properties: <c>KilnSigsNamespace</c>
/// (the root, default <c>Kiln.Sigs</c>). The classes are internal: they are the mod's own, not its API. The core's ten
/// descriptors are not generated here: they are Kiln.Sigs (managed/services/Kiln.Sigs).
/// </summary>
[Generator(LanguageNames.CSharp)]
public sealed class SigsGenerator : IIncrementalGenerator
{
    internal static readonly DiagnosticDescriptor BadDescriptor = new(
        "KILN180", "sigdb descriptor not generated",
        "The descriptor of {0} in {1} was not generated: {2}",
        "Kiln.Sigs", DiagnosticSeverity.Warning, isEnabledByDefault: true,
        helpLinkUri: "docs/kiln/modding/analyzers.md#kiln180");

    internal static readonly DiagnosticDescriptor NeedsHooks = new(
        "KILN181", "Typed hooks need Kiln.Hooks",
        "This project's sigdb files describe {0} game function(s), but it does not reference Kiln.Hooks (add Hooks to KilnServices): no typed classes were generated",
        "Kiln.Sigs", DiagnosticSeverity.Warning, isEnabledByDefault: true,
        helpLinkUri: "docs/kiln/modding/analyzers.md#kiln181");

    internal static readonly DiagnosticDescriptor BadFile = new(
        "KILN182", "Unreadable sigdb file",
        "The sigdb file {0} could not be read ({1}): its descriptors were not generated",
        "Kiln.Sigs", DiagnosticSeverity.Warning, isEnabledByDefault: true,
        helpLinkUri: "docs/kiln/modding/analyzers.md#kiln182");

    public void Initialize(IncrementalGeneratorInitializationContext context)
    {
        var files = context.AdditionalTextsProvider
            .Combine(context.AnalyzerConfigOptionsProvider)
            .Where(p => p.Right.GetOptions(p.Left).TryGetValue("build_metadata.AdditionalFiles.KilnSigdb", out var v) &&
                        string.Equals(v, "true", StringComparison.OrdinalIgnoreCase))
            .Select((p, ct) => (Path: p.Left.Path, Text: p.Left.GetText(ct)?.ToString()))
            .Collect();
        var root = context.AnalyzerConfigOptionsProvider.Select((o, _) =>
            o.GlobalOptions.TryGetValue("build_property.KilnSigsNamespace", out var ns) && !string.IsNullOrWhiteSpace(ns) ? ns.Trim() : SigsEmitter.DefaultRoot);
        // what the compilation offers: Kiln.Hooks (the runtime), and Kiln.Sigs' structs (a mod's descriptor may name one)
        var refs = context.CompilationProvider.Select((c, _) =>
        {
            var hooks = c.GetTypeByMetadataName("Kiln.TypedHooks") != null;
            var structs = ImmutableDictionary.CreateBuilder<string, uint>(StringComparer.Ordinal);
            var sigs = c.GlobalNamespace.GetNamespaceMembers().FirstOrDefault(n => n.Name == "Kiln")?
                .GetNamespaceMembers().FirstOrDefault(n => n.Name == "Sigs");
            if (sigs != null)
                foreach (var t in sigs.GetTypeMembers())
                    if (t.TypeKind == TypeKind.Struct && t.GetMembers("SizeOf").OfType<IFieldSymbol>().FirstOrDefault() is { HasConstantValue: true } f &&
                        f.ConstantValue is int size && size > 0)
                        structs[t.Name] = (uint)size;
            return (Hooks: hooks, Structs: structs.ToImmutable());
        });
        context.RegisterSourceOutput(files.Combine(root).Combine(refs), (spc, input) =>
        {
            var ((texts, ns), (hooks, structs)) = input;
            if (texts.IsDefaultOrEmpty) return;
            var model = SigsModel.Parse(texts.Where(t => t.Text != null).Select(t => (t.Path, t.Text!)),
                name => structs.TryGetValue(name, out var s) ? s : (uint?)null);
            foreach (var t in texts.Where(t => t.Text == null))
                spc.ReportDiagnostic(Diagnostic.Create(BadFile, Location.None, t.Path, "it could not be opened"));
            foreach (var (file, error) in model.BadFiles)
                spc.ReportDiagnostic(Diagnostic.Create(BadFile, Location.None, file, error));
            foreach (var p in model.Problems)
                spc.ReportDiagnostic(Diagnostic.Create(BadDescriptor, Location.None, p.Symbol, p.Source, p.Message));
            foreach (var d in SigsEmitter.Clashes(model).ToList())
            {
                spc.ReportDiagnostic(Diagnostic.Create(BadDescriptor, Location.None, d.Symbol, d.Source,
                    $"its class {string.Join(".", SigsEmitter.NameOf(d.Symbol).Area, SigsEmitter.NameOf(d.Symbol).Name)} is taken by another descriptor"));
                model.Sigs.Remove(d.Symbol);
            }
            if (model.Sigs.Count == 0 && model.Structs.Count == 0) return;
            if (!hooks)
            {
                spc.ReportDiagnostic(Diagnostic.Create(NeedsHooks, Location.None, model.Sigs.Count));
                return;
            }
            spc.AddSource("KilnSigs.g.cs", SourceText.From(SigsEmitter.Emit(model, ns, isPublic: false), Encoding.UTF8));
        });
    }
}
