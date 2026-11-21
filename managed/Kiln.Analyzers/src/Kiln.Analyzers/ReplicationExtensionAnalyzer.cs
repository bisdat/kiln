// SPDX-License-Identifier: MIT
using System.Collections.Immutable;
using System.Linq;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.Diagnostics;
using Microsoft.CodeAnalysis.Operations;

namespace Kiln.Analyzers;

/// <summary>KILN199: the value type of <c>Replication.Extension&lt;T&gt;</c> breaks the rule Kiln checks when the extension is
/// registered (ArgumentException at run time): an unmanaged struct with <c>[StructLayout(LayoutKind.Sequential, Pack = 1)]</c>
/// made only of byte, sbyte, short, ushort, int, uint, long, ulong, float, double, enums of those and nested structs under
/// the same rule, at most 1,024 bytes. The layout attribute is checked on the mod's own types (a referenced assembly's
/// layout is not visible to an analyzer; the run time checks it).</summary>
[DiagnosticAnalyzer(LanguageNames.CSharp)]
public sealed class ReplicationExtensionAnalyzer : DiagnosticAnalyzer
{
    private const int MaxSize = 1024, MaxDepth = 16;

    public override ImmutableArray<DiagnosticDescriptor> SupportedDiagnostics => ImmutableArray.Create(Descriptors.ExtensionValueType);

    public override void Initialize(AnalysisContext context)
    {
        context.ConfigureGeneratedCodeAnalysis(GeneratedCodeAnalysisFlags.None);
        context.EnableConcurrentExecution();
        context.RegisterCompilationStartAction(start =>
        {
            var replication = start.Compilation.GetTypeByMetadataName("Kiln.Replication");
            if (replication is null) return;
            start.RegisterOperationAction(ctx => Invocation(ctx, replication), OperationKind.Invocation);
        });
    }

    private static void Invocation(OperationAnalysisContext ctx, INamedTypeSymbol replication)
    {
        var call = (IInvocationOperation)ctx.Operation;
        var m = call.TargetMethod;
        if (m.Name != "Extension" || !m.IsGenericMethod || m.TypeArguments.Length != 1 ||
            !SymbolEqualityComparer.Default.Equals(m.ContainingType, replication))
            return;
        var t = m.TypeArguments[0];
        if (t is ITypeParameterSymbol) return;  // a generic helper: checked where it is closed (and at run time)
        var why = Check(t, "", 0, out _);
        if (why is null) return;
        ctx.ReportDiagnostic(Diagnostic.Create(Descriptors.ExtensionValueType, call.Syntax.GetLocation(), t.ToDisplayString(), why));
    }

    /// <summary>Why <paramref name="t"/> (at <paramref name="path"/>) is not allowed, or null; <paramref name="size"/> its bytes.</summary>
    private static string? Check(ITypeSymbol t, string path, int depth, out int size)
    {
        size = 0;
        var at = path.Length == 0 ? "" : $" (field {path})";
        if (depth > MaxDepth) return $"it nests structs deeper than {MaxDepth}";
        if (t.TypeKind != TypeKind.Struct || t.SpecialType != SpecialType.None || t is not INamedTypeSymbol named)
            return path.Length == 0 ? "it is not a struct" : $"field {path} is {t.ToDisplayString()}, not a struct";
        if (named.Locations.Any(l => l.IsInSource) && !SequentialPack1(named))
            return $"{named.Name}{at} needs [StructLayout(LayoutKind.Sequential, Pack = 1)]";
        var any = false;
        foreach (var f in named.GetMembers().OfType<IFieldSymbol>())
        {
            if (f.IsStatic || f.IsConst) continue;
            any = true;
            var p = path.Length == 0 ? f.Name : path + "." + f.Name;
            if (f.IsFixedSizeBuffer) return $"field {p} is a fixed buffer; use a nested struct of fields";
            var ft = f.Type;
            if (ft.TypeKind is TypeKind.Pointer or TypeKind.FunctionPointer) return $"field {p} is a pointer";
            var leaf = ft.TypeKind == TypeKind.Enum && ft is INamedTypeSymbol e && e.EnumUnderlyingType is { } u ? u : ft;
            var bytes = Leaf(leaf.SpecialType);
            if (bytes > 0)
            {
                size += bytes;
                continue;
            }
            if (ft.TypeKind == TypeKind.Enum) return $"field {p} is an enum of {leaf.ToDisplayString()}";
            if (ft.SpecialType != SpecialType.None || ft.TypeKind != TypeKind.Struct)
                return $"field {p} is {ft.ToDisplayString()}; allowed: byte, sbyte, short, ushort, int, uint, long, ulong, float, double, their enums and structs of them";
            var inner = Check(ft, p, depth + 1, out var innerSize);
            if (inner is not null) return inner;
            size += innerSize;
        }
        if (!any) return path.Length == 0 ? "it has no fields" : $"{named.Name}{at} has no fields";
        if (depth == 0 && size > MaxSize) return $"it is {size} bytes; an extension value is at most {MaxSize}";
        return null;
    }

    private static int Leaf(SpecialType s) => s switch
    {
        SpecialType.System_Byte or SpecialType.System_SByte => 1,
        SpecialType.System_Int16 or SpecialType.System_UInt16 => 2,
        SpecialType.System_Int32 or SpecialType.System_UInt32 or SpecialType.System_Single => 4,
        SpecialType.System_Int64 or SpecialType.System_UInt64 or SpecialType.System_Double => 8,
        _ => 0,
    };

    private static bool SequentialPack1(INamedTypeSymbol t)
    {
        foreach (var a in t.GetAttributes())
        {
            if (a.AttributeClass?.ToDisplayString() != "System.Runtime.InteropServices.StructLayoutAttribute") continue;
            if (a.ConstructorArguments.Length != 1 || a.ConstructorArguments[0].Value is not int kind || kind != 0) return false;  // 0 = Sequential
            return a.NamedArguments.Any(n => n.Key == "Pack" && n.Value.Value is int pack && pack == 1);
        }
        return false;
    }
}
