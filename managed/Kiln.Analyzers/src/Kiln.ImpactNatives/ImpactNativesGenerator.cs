// SPDX-License-Identifier: MIT
using System;
using System.Collections.Generic;
using System.Collections.Immutable;
using System.Globalization;
using System.Linq;
using System.Text;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp.Syntax;
using Microsoft.CodeAnalysis.Text;

namespace Kiln.ImpactNatives;

/// <summary>
/// A mod's <c>[ImpactNative("Name")]</c> methods as synchronous Impact natives (chapter 06c §4, §7.2, §9.5, §9.7;
/// docs/kiln/modding/impact.md "Typed natives"). Per method: a thunk that reads the call's arguments by the method's
/// parameter types (through <c>Kiln.ImpactNativeAccess</c>, type and size checked), a <c>NativeRef</c> field in the
/// namespace's <c>Natives</c> class, an assembly attribute with the method's impact/natives.json entry, and one module
/// initializer that hands the registration to Kiln.Abilities (which runs it for the mod at its next tick). A shape it
/// cannot generate is an error with the method's location (KILN170 to KILN173); no Kiln.Abilities reference: KILN174.
/// </summary>
[Generator(LanguageNames.CSharp)]
public sealed class ImpactNativesGenerator : IIncrementalGenerator
{
    internal const string AttributeName = "Kiln.ImpactNativeAttribute";

    internal static readonly DiagnosticDescriptor BadMethod = new(
        "KILN170", "[ImpactNative] method not generated",
        "[ImpactNative] {0}: {1}",
        "Kiln.ImpactNatives", DiagnosticSeverity.Error, isEnabledByDefault: true,
        helpLinkUri: "docs/kiln/modding/analyzers.md#kiln170");

    internal static readonly DiagnosticDescriptor BadParameter = new(
        "KILN171", "[ImpactNative] parameter of a type no native takes",
        "[ImpactNative] {0}: parameter '{1}' of type {2}: {3}",
        "Kiln.ImpactNatives", DiagnosticSeverity.Error, isEnabledByDefault: true,
        helpLinkUri: "docs/kiln/modding/analyzers.md#kiln171");

    internal static readonly DiagnosticDescriptor BadReturn = new(
        "KILN172", "[ImpactNative] return type",
        "[ImpactNative] {0} returns {1}: a native returns bool, byte or void (what the program's ECALL pushes; void pushes 1); give other results back through a ref or out parameter",
        "Kiln.ImpactNatives", DiagnosticSeverity.Error, isEnabledByDefault: true,
        helpLinkUri: "docs/kiln/modding/analyzers.md#kiln172");

    internal static readonly DiagnosticDescriptor DuplicateName = new(
        "KILN173", "[ImpactNative] name used twice",
        "[ImpactNative] name '{0}' is used by {1} and {2}: a mod's native names are unique",
        "Kiln.ImpactNatives", DiagnosticSeverity.Error, isEnabledByDefault: true,
        helpLinkUri: "docs/kiln/modding/analyzers.md#kiln173");

    internal static readonly DiagnosticDescriptor NeedsAbilities = new(
        "KILN174", "[ImpactNative] needs Kiln.Abilities",
        "This project has {0} [ImpactNative] method(s) but does not reference a Kiln.Abilities with ImpactNativeAccess (add Abilities to KilnServices, Kiln 0.2.0-dev.29 or newer): nothing was generated",
        "Kiln.ImpactNatives", DiagnosticSeverity.Warning, isEnabledByDefault: true,
        helpLinkUri: "docs/kiln/modding/analyzers.md#kiln174");

    public void Initialize(IncrementalGeneratorInitializationContext context)
    {
        var methods = context.SyntaxProvider.ForAttributeWithMetadataName(AttributeName,
            static (node, _) => node is MethodDeclarationSyntax,
            static (ctx, _) => NativeModel.From((IMethodSymbol)ctx.TargetSymbol, ctx.Attributes[0], ctx.TargetNode.GetLocation()))
            .Collect();
        var runtime = context.CompilationProvider.Select(static (c, _) => c.GetTypeByMetadataName("Kiln.ImpactNativeAccess") != null);
        context.RegisterSourceOutput(methods.Combine(runtime), static (spc, pair) =>
        {
            var (all, hasRuntime) = pair;
            if (all.IsDefaultOrEmpty) return;
            foreach (var m in all)
                foreach (var d in m.Problems)
                    spc.ReportDiagnostic(d);
            var good = all.Where(m => m.Problems.Length == 0).ToList();
            foreach (var group in good.GroupBy(m => m.Name, StringComparer.Ordinal).Where(g => g.Count() > 1))
            {
                var list = group.ToList();
                for (var i = 1; i < list.Count; i++)
                    spc.ReportDiagnostic(Diagnostic.Create(DuplicateName, list[i].Location, list[i].Name, list[0].Display, list[i].Display));
            }
            if (good.Count == 0) return;
            if (!hasRuntime)
            {
                spc.ReportDiagnostic(Diagnostic.Create(NeedsAbilities, good[0].Location, good.Count));
                return;
            }
            var unique = good.GroupBy(m => m.Name, StringComparer.Ordinal).Select(g => g.First()).ToList();
            spc.AddSource("Kiln.ImpactNatives.g.cs", SourceText.From(Emitter.Emit(unique), Encoding.UTF8));
        });
    }
}

/// <summary>How the thunk reads one argument.</summary>
internal enum ArgKind { Value, Time, Raw }

/// <summary>One argument of an [ImpactNative] method.</summary>
internal sealed class ParamModel
{
    public string Name = "";
    public string CSharpType = "";   // the global:: name of the value type (int, global::Kiln.EntityId, ...)
    public string ImpactType = "";   // the impact type name ("sint32", "any" for [Raw])
    public uint Hash;                // FNV-1a-32 of ImpactType; 0 = any
    public int Size;
    public ArgKind Kind;
    public RefKind RefKind;          // None/In: read; Ref: inout; Out: out
    public bool Optional;
    public bool Raw;

    public string Dir => RefKind == RefKind.Ref ? "inout" : RefKind == RefKind.Out ? "out" : "in";
}

internal enum ReturnKind { Bool, Byte, Void }

/// <summary>One [ImpactNative] method, read from its symbol (no symbol is kept).</summary>
internal sealed class NativeModel
{
    public string Name = "";           // the native's name without the mod id
    public string Display = "";        // Type.Method, for messages
    public string Namespace = "";      // "" = the global namespace
    public string TypeName = "";       // global::Ns.Outer.Type
    public string Method = "";
    public bool TakesContext;
    public bool ContextByIn;
    public ReturnKind Return;
    public List<ParamModel> Params = new();
    public Location? Location;
    public ImmutableArray<Diagnostic> Problems = ImmutableArray<Diagnostic>.Empty;

    // The value types an argument can have: (metadata name, impact type, size).
    private static readonly (string Type, string Impact, int Size)[] Known =
    {
        ("System.Int32", "sint32", 4), ("System.UInt32", "uint32", 4), ("System.Single", "float32", 4), ("System.Boolean", "bool", 1),
        ("System.TimeSpan", "Time", 8), ("Kiln.EntityId", "ecs.EntityId", 4), ("System.Numerics.Vector3", "float3", 12),
        ("Kiln.DamageSetup", "impact.DamageSetup", 52),
    };

    internal const string Supported = "int, uint, float, bool, TimeSpan, EntityId, Vector3 or DamageSetup (ref or out for an argument the native writes), Optional<T> of one of them, or [Raw] uint";

    public static NativeModel From(IMethodSymbol method, AttributeData attribute, Location location)
    {
        var m = new NativeModel
        {
            Location = location, Method = method.Name,
            Display = $"{method.ContainingType?.Name}.{method.Name}",
            Namespace = method.ContainingNamespace is { IsGlobalNamespace: false } ns ? ns.ToDisplayString() : "",
            TypeName = method.ContainingType?.ToDisplayString(SymbolDisplayFormat.FullyQualifiedFormat) ?? "",
        };
        var problems = ImmutableArray.CreateBuilder<Diagnostic>();
        void Bad(string why) => problems.Add(Diagnostic.Create(ImpactNativesGenerator.BadMethod, location, m.Display, why));
        m.Name = attribute.ConstructorArguments.Length == 1 && attribute.ConstructorArguments[0].Value is string n ? n : "";
        if (!ValidName(m.Name)) Bad($"the name '{m.Name}' is not [A-Za-z0-9_]+ of 1 to 60 characters (the mod's id goes in front of it)");
        if (!method.IsStatic) Bad("the method must be static");
        if (method.IsGenericMethod) Bad("the method must not be generic");
        if (method.IsAsync) Bad("the method must not be async (it answers inside the program's call)");
        for (var t = method.ContainingType; t != null; t = t.ContainingType)
        {
            if (t.IsGenericType) Bad($"its type {t.Name} must not be generic");
            if (t.DeclaredAccessibility is Accessibility.Private or Accessibility.Protected or Accessibility.ProtectedAndInternal)
                Bad($"its type {t.Name} must be internal or public (the generated registration calls it)");
        }
        if (method.DeclaredAccessibility is Accessibility.Private or Accessibility.Protected or Accessibility.ProtectedAndInternal)
            Bad("the method must be internal or public (the generated registration calls it)");

        var ret = method.ReturnType;
        if (ret.SpecialType == SpecialType.System_Boolean) m.Return = ReturnKind.Bool;
        else if (ret.SpecialType == SpecialType.System_Byte) m.Return = ReturnKind.Byte;
        else if (ret.SpecialType == SpecialType.System_Void) m.Return = ReturnKind.Void;
        else problems.Add(Diagnostic.Create(ImpactNativesGenerator.BadReturn, location, m.Display, ret.ToDisplayString()));

        var ps = method.Parameters;
        var first = 0;
        if (ps.Length > 0 && Is(ps[0].Type, "Kiln.NativeContext"))
        {
            if (ps[0].RefKind is RefKind.Ref or RefKind.Out)
                problems.Add(Diagnostic.Create(ImpactNativesGenerator.BadParameter, location, m.Display, ps[0].Name, "NativeContext",
                    "the call's context is passed by value or with 'in'"));
            m.TakesContext = true;
            m.ContextByIn = ps[0].RefKind == RefKind.In;
            first = 1;
        }
        if (ps.Length - first > 31) Bad($"{ps.Length - first} arguments: a native takes at most 31");
        for (var i = first; i < ps.Length; i++)
        {
            var p = ps[i];
            var pm = Param(p, out var why);
            if (pm is null)
                problems.Add(Diagnostic.Create(ImpactNativesGenerator.BadParameter, location, m.Display, p.Name, p.Type.ToDisplayString(), why));
            else m.Params.Add(pm);
        }
        m.Problems = problems.ToImmutable();
        return m;
    }

    private static ParamModel? Param(IParameterSymbol p, out string why)
    {
        why = "";
        var type = p.Type;
        var raw = p.GetAttributes().Any(a => a.AttributeClass?.ToDisplayString() == "Kiln.RawAttribute");
        var optional = type is INamedTypeSymbol { IsGenericType: true } g && g.ConstructedFrom.ToDisplayString() == "Kiln.Optional<T>";
        if (Is(type, "Kiln.NativeContext")) { why = "the call's context must be the first parameter"; return null; }
        if (p.RefKind == RefKind.RefReadOnlyParameter) { why = "use 'in' or no modifier for an argument the native reads"; return null; }
        if (optional)
        {
            if (p.RefKind is RefKind.Ref or RefKind.Out) { why = "an Optional<T> argument is read only (no ref or out)"; return null; }
            if (raw) { why = "[Raw] goes on a uint, not on an Optional<T>"; return null; }
            type = ((INamedTypeSymbol)type).TypeArguments[0];
        }
        var name = type.ToDisplayString(new SymbolDisplayFormat(typeQualificationStyle: SymbolDisplayTypeQualificationStyle.NameAndContainingTypesAndNamespaces));
        var known = Known.FirstOrDefault(k => k.Type == name);
        if (known.Type is null) { why = $"a native takes {Supported}"; return null; }
        var pm = new ParamModel
        {
            Name = p.Name, CSharpType = type.ToDisplayString(SymbolDisplayFormat.FullyQualifiedFormat), ImpactType = known.Impact,
            Size = known.Size, RefKind = p.RefKind == RefKind.In ? RefKind.None : p.RefKind, Optional = optional, Raw = raw,
            Kind = known.Type == "System.TimeSpan" ? ArgKind.Time : ArgKind.Value,
        };
        if (raw)
        {
            if (known.Type != "System.UInt32" || p.RefKind != RefKind.None) { why = "[Raw] goes on a uint argument the native reads (no ref or out)"; return null; }
            pm.Kind = ArgKind.Raw;
            pm.ImpactType = "any";  // a raw integer, or any 4-byte variable
        }
        pm.Hash = pm.ImpactType == "any" ? 0 : Fnv(pm.ImpactType);
        return pm;
    }

    private static bool Is(ITypeSymbol t, string metadataName) => t.ToDisplayString() == metadataName;

    internal static bool ValidName(string n) =>
        n.Length is > 0 and <= 60 && n.All(c => c is >= 'A' and <= 'Z' or >= 'a' and <= 'z' or >= '0' and <= '9' or '_');

    internal static uint Fnv(string s)
    {
        var h = 0x811c9dc5u;
        foreach (var b in Encoding.UTF8.GetBytes(s)) h = unchecked((h ^ b) * 0x01000193u);
        return h;
    }
}

/// <summary>The generated source of a compilation's [ImpactNative] methods.</summary>
internal static class Emitter
{
    internal static string Emit(IReadOnlyList<NativeModel> natives)
    {
        var sb = new StringBuilder();
        sb.AppendLine("// <auto-generated/>  Kiln.ImpactNatives: the [ImpactNative] methods of this project (docs/kiln/modding/impact.md).");
        sb.AppendLine("#nullable enable");
        sb.AppendLine("#pragma warning disable CS1591, CA2255");
        sb.AppendLine();
        foreach (var n in natives)
            sb.Append("[assembly: global::Kiln.ImpactNativeEntryAttribute(").Append(Str(n.Name)).Append(", ").Append(Str(ArgsJson(n))).AppendLine(")]");
        sb.AppendLine();
        foreach (var ns in natives.GroupBy(n => n.Namespace, StringComparer.Ordinal).OrderBy(g => g.Key, StringComparer.Ordinal))
        {
            var indent = ns.Key.Length > 0 ? "    " : "";
            if (ns.Key.Length > 0) sb.Append("namespace ").Append(ns.Key).AppendLine().AppendLine("{");
            sb.Append(indent).AppendLine("/// <summary>This project's [ImpactNative] natives (generated by Kiln.ImpactNatives).</summary>");
            sb.Append(indent).AppendLine("public static partial class Natives");
            sb.Append(indent).AppendLine("{");
            foreach (var n in ns.OrderBy(x => x.Name, StringComparer.Ordinal))
            {
                sb.Append(indent).Append("    /// <summary>The native \"&lt;modid&gt;.").Append(n.Name).Append("\" (").Append(Xml(n.Display)).AppendLine(").</summary>");
                sb.Append(indent).Append("    public static readonly global::Kiln.NativeRef ").Append(Field(n.Name)).Append(" = new(").Append(Str(n.Name)).Append(", new global::Kiln.NativeParameter[] {");
                sb.Append(string.Join(",", n.Params.Select(p =>
                    $" new({Str(p.Name)}, new global::Kiln.NativeArg({Str(p.ImpactType)}, global::Kiln.NativeArgDirection.{(p.RefKind == RefKind.Ref ? "InOut" : p.RefKind == RefKind.Out ? "Out" : "In")}, {B(p.Optional)}, {B(p.Raw)}, {p.Size}))")));
                sb.AppendLine(" });");
            }
            sb.Append(indent).AppendLine("}");
            if (ns.Key.Length > 0) sb.AppendLine("}");
            sb.AppendLine();
        }
        sb.AppendLine("namespace Kiln.ImpactNatives.Generated");
        sb.AppendLine("{");
        sb.AppendLine("    /// <summary>Registers this project's [ImpactNative] natives when its mod loads (generated by Kiln.ImpactNatives).</summary>");
        sb.AppendLine("    [global::System.ComponentModel.EditorBrowsable(global::System.ComponentModel.EditorBrowsableState.Never)]");
        sb.AppendLine("    internal static class Registration");
        sb.AppendLine("    {");
        sb.AppendLine("        [global::System.Runtime.CompilerServices.ModuleInitializer]");
        sb.AppendLine("        internal static void Init() => global::Kiln.ImpactNativeAccess.Provide(Register);");
        sb.AppendLine();
        sb.AppendLine("        private static void Register()");
        sb.AppendLine("        {");
        foreach (var n in natives)
            sb.Append("            ").Append(Qualified(n)).Append(".Register(").Append(Thunk(n)).AppendLine(");");
        sb.AppendLine("        }");
        foreach (var n in natives) EmitThunk(sb, n);
        sb.AppendLine("    }");
        sb.AppendLine("}");
        return sb.ToString();
    }

    private static void EmitThunk(StringBuilder sb, NativeModel n)
    {
        const string A = "global::Kiln.ImpactNativeAccess";
        sb.AppendLine();
        sb.Append("        private static byte ").Append(Thunk(n)).AppendLine("(ref global::Kiln.NativeCall call)");
        sb.AppendLine("        {");
        if (n.TakesContext) sb.Append("            var ctx = ").Append(A).AppendLine(".Context(ref call);");
        var args = new List<string>();
        if (n.TakesContext) args.Add(n.ContextByIn ? "in ctx" : "ctx");
        var after = new List<string>();
        for (var i = 0; i < n.Params.Count; i++)
        {
            var p = n.Params[i];
            var v = "a" + i.ToString(CultureInfo.InvariantCulture);
            var hash = "0x" + p.Hash.ToString("x8", CultureInfo.InvariantCulture) + "u";
            string read = p.Kind switch
            {
                ArgKind.Raw => $"{A}.RawValue(ref call, {i})",
                ArgKind.Time => $"{A}.Time(ref call, {i})",
                _ => $"{A}.Ref<{p.CSharpType}>(ref call, {i}, {hash})",
            };
            if (p.Optional)
                sb.Append("            var ").Append(v).Append(" = ").Append(A).Append(".Connected(ref call, ").Append(i)
                  .Append(") ? new global::Kiln.Optional<").Append(p.CSharpType).Append(">(").Append(read).Append(") : default;").AppendLine();
            else if (p.Kind == ArgKind.Time && p.RefKind is RefKind.Ref or RefKind.Out)
            {
                // a Time variable is int64 ns, a TimeSpan 100-ns ticks: copied in (for ref) and written back after the call
                sb.Append("            ").Append(p.RefKind == RefKind.Ref ? $"var {v} = {read};" : $"_ = {read}; global::System.TimeSpan {v};").AppendLine();
                after.Add($"            {A}.SetTime(ref call, {i}, {v});");
            }
            else if (p.RefKind is RefKind.Ref or RefKind.Out)
                sb.Append("            ref ").Append(p.CSharpType).Append(' ').Append(v).Append(" = ref ").Append(read).AppendLine(";");
            else
                sb.Append("            var ").Append(v).Append(" = ").Append(read).AppendLine(";");
            args.Add((p.RefKind == RefKind.Ref ? "ref " : p.RefKind == RefKind.Out ? "out " : "") + v);
        }
        var callText = $"{n.TypeName}.{n.Method}({string.Join(", ", args)})";
        switch (n.Return)
        {
            case ReturnKind.Bool: sb.Append("            var r = ").Append(callText).AppendLine(";"); break;
            case ReturnKind.Byte: sb.Append("            var r = ").Append(callText).AppendLine(";"); break;
            default: sb.Append("            ").Append(callText).AppendLine(";"); break;
        }
        foreach (var line in after) sb.AppendLine(line);
        sb.Append("            return ").Append(n.Return switch { ReturnKind.Bool => "r ? (byte)1 : (byte)0", ReturnKind.Byte => "r", _ => "1" }).AppendLine(";");
        sb.AppendLine("        }");
    }

    // The natives.json entry's "args" (the SDK task adds the mod id, "sync", "argc").
    internal static string ArgsJson(NativeModel n)
    {
        var sb = new StringBuilder("[");
        for (var i = 0; i < n.Params.Count; i++)
        {
            var p = n.Params[i];
            if (i > 0) sb.Append(',');
            sb.Append("{\"name\":").Append(Json(p.Name)).Append(",\"type\":").Append(Json(p.ImpactType)).Append(",\"dir\":").Append(Json(p.Dir))
              .Append(",\"size\":").Append(p.Size.ToString(CultureInfo.InvariantCulture));
            if (p.Optional) sb.Append(",\"optional\":true");
            if (p.Raw) sb.Append(",\"raw\":true");
            sb.Append('}');
        }
        return sb.Append(']').ToString();
    }

    private static string Qualified(NativeModel n) => (n.Namespace.Length > 0 ? "global::" + n.Namespace + "." : "global::") + "Natives." + Field(n.Name);
    private static string Thunk(NativeModel n) => "Thunk_" + n.Name;
    private static string Field(string name) => char.IsDigit(name[0]) ? "_" + name : name;
    private static string B(bool b) => b ? "true" : "false";
    private static string Xml(string s) => s.Replace("&", "&amp;").Replace("<", "&lt;").Replace(">", "&gt;");

    private static string Str(string s)
    {
        var sb = new StringBuilder("\"");
        foreach (var c in s)
            sb.Append(c switch { '"' => "\\\"", '\\' => "\\\\", '\n' => "\\n", '\r' => "\\r", _ => c.ToString() });
        return sb.Append('"').ToString();
    }

    private static string Json(string s) => Str(s);  // the names are [A-Za-z0-9_.]: no other escape needed
}
