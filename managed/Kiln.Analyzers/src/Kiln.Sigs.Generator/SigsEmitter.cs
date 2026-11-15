// SPDX-License-Identifier: MIT
using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Text;

namespace Kiln.Sigs.Generator;

/// <summary>
/// The C# of a set of sigdb descriptors (chapter 06b §9.3): one <c>static unsafe class</c> per descriptor in
/// <c>&lt;root&gt;.&lt;Area&gt;</c> (the area is the symbol's part before its first <c>/</c> in Pascal case, <c>Core</c> for a
/// symbol without one), with <c>Symbol</c>, <c>Capability</c>, <c>Descriptor</c>, a <c>ref struct Call</c> over the call in
/// flight (typed properties: narrow integers masked, positions shifted by one behind an sret buffer), <c>Handler</c>,
/// <c>Detour</c> (a <c>HookResult</c>) and, when the descriptor's <c>invoke</c> is not <c>none</c>, <c>Invoke</c> (a struct
/// returned by value); and one struct per <c>"structs"</c> block in <c>&lt;root&gt;</c>. Used by the generator (a mod's own
/// fragment) and for Kiln.Sigs (the core's descriptors, committed; a test compares).
/// </summary>
internal static class SigsEmitter
{
    public const string DefaultRoot = "Kiln.Sigs";

    private static readonly HashSet<string> Keywords = new(StringComparer.Ordinal)
    {
        "abstract", "as", "base", "bool", "break", "byte", "case", "catch", "char", "checked", "class", "const", "continue",
        "decimal", "default", "delegate", "do", "double", "else", "enum", "event", "explicit", "extern", "false", "finally",
        "fixed", "float", "for", "foreach", "goto", "if", "implicit", "in", "int", "interface", "internal", "is", "lock",
        "long", "namespace", "new", "null", "object", "operator", "out", "override", "params", "private", "protected",
        "public", "readonly", "ref", "return", "sbyte", "sealed", "short", "sizeof", "stackalloc", "static", "string",
        "struct", "switch", "this", "throw", "true", "try", "typeof", "uint", "ulong", "unchecked", "unsafe", "ushort",
        "using", "virtual", "void", "volatile", "while",
    };

    // Members of Call a parameter may not be named after (and C#'s own).
    private static readonly HashSet<string> CallMembers = new(StringComparer.Ordinal)
    {
        "Call", "CallOriginal", "OriginalCalled", "Skip", "Result", "Equals", "GetHashCode", "ToString", "GetType",
    };

    private static readonly string[] IntRegs = { "rcx", "rdx", "r8", "r9" };

    /// <summary>Area and class name of a symbol: <c>time/SetTimeScale</c> → (Time, SetTimeScale).</summary>
    public static (string Area, string Name) NameOf(string symbol)
    {
        var slash = symbol.IndexOf('/');
        if (slash < 0) return ("Core", Pascal(symbol));
        var rest = symbol.Substring(slash + 1).Split('/');
        return (Pascal(symbol.Substring(0, slash)), string.Concat(rest.Select(Pascal)));
    }

    /// <summary>An identifier in Pascal case: pieces split at every character that is no letter or digit.</summary>
    public static string Pascal(string s)
    {
        var sb = new StringBuilder();
        var up = true;
        foreach (var c in s)
        {
            if (!char.IsLetterOrDigit(c))
            {
                up = true;
                continue;
            }
            sb.Append(up ? char.ToUpperInvariant(c) : c);
            up = false;
        }
        if (sb.Length == 0 || char.IsDigit(sb[0])) sb.Insert(0, '_');
        return sb.ToString();
    }

    private static string Camel(string s)
    {
        var p = Pascal(s);
        var c = p.Length > 1 && p[0] == '_' ? p : char.ToLowerInvariant(p[0]) + p.Substring(1);
        return Keywords.Contains(c) ? "@" + c : c;
    }

    private static string Xml(string s) => s.Replace("&", "&amp;").Replace("<", "&lt;").Replace(">", "&gt;").Replace("\"", "&quot;");

    private static string Lit(string s) => "\"" + s.Replace("\\", "\\\\").Replace("\"", "\\\"") + "\"";

    /// <summary>The C# type of a scalar or pointer (a pointer to an integer or float scalar is typed, <c>u16*</c> is
    /// <c>ushort*</c>; any other pointer is <c>nint</c>), of a by-value struct, or of a struct return.</summary>
    private static string CsType(SigType t, string root)
    {
        switch (t.Kind)
        {
            case "S8": return "sbyte";
            case "U8": return "byte";
            case "S16": return "short";
            case "U16": return "ushort";
            case "S32": return "int";
            case "U32": return "uint";
            case "S64": return "long";
            case "U64": return "ulong";
            case "F32": return "float";
            case "F64": return "double";
            case "Bool": return "bool";
            case "ByVal":
            case "Struct":
                // a core struct a mod's descriptor names lives in Kiln.Sigs, whatever the mod's root
                return $"global::{(t.Struct!.External ? DefaultRoot : root)}.{Pascal(t.Struct.Name)}";
            case "Ptr":
                if (t.Pointee != null && SigsModel.Scalars.TryGetValue(t.Pointee, out _) && t.Pointee != "ptr" && t.Pointee != "bool")
                    return CsType(new SigType(t.Pointee, SigsModel.Scalars[t.Pointee].Kind, 0), root) + "*";
                return "nint";
            default:
                return "void";
        }
    }

    private static bool Typed(SigType t, string root) => t.Kind == "Ptr" && CsType(t, root) != "nint";

    // ulong slot -> the C# value (masks a narrow integer by its cast)
    private static string FromSlot(SigType t, string root, string slot) => t.Kind switch
    {
        "S8" => $"unchecked((sbyte){slot})",
        "U8" => $"unchecked((byte){slot})",
        "S16" => $"unchecked((short){slot})",
        "U16" => $"unchecked((ushort){slot})",
        "S32" => $"unchecked((int){slot})",
        "U32" => $"unchecked((uint){slot})",
        "S64" => $"unchecked((long){slot})",
        "U64" => slot,
        "Bool" => $"(unchecked((byte){slot}) != 0)",
        "Ptr" => Typed(t, root) ? $"({CsType(t, root)})unchecked((nint)(long){slot})" : $"unchecked((nint)(long){slot})",
        _ => throw new ArgumentException(t.Kind),
    };

    // the C# value -> ulong slot (sign-extends a signed one)
    private static string ToSlot(SigType t, string root, string v) => t.Kind switch
    {
        "S8" or "S16" or "S32" or "S64" => $"unchecked((ulong)(long){v})",
        "U8" or "U16" or "U32" => $"(ulong){v}",
        "U64" => v,
        "Bool" => $"({v} ? 1UL : 0UL)",
        "Ptr" => Typed(t, root) ? $"unchecked((ulong)(nint){v})" : $"unchecked((ulong)(long){v})",
        _ => throw new ArgumentException(t.Kind),
    };

    private static string Place(SigType t, int pos) =>
        pos < 4 ? (t.IsFloat ? $"xmm{pos}" : IntRegs[pos]) : $"stack+0x{0x28 + 8 * (pos - 4):x}";

    private static string Proto(SigDef d)
    {
        var args = new List<string>();
        if (d.Sret) args.Add($"{d.Ret.Text}* /*hidden return buffer*/");
        args.AddRange(d.Args.Select(a => $"{a.Type.Text} {a.Name}"));
        return $"{(d.Sret ? d.Ret.Text + "*" : d.Ret.Text)} ({string.Join(", ", args)})";
    }

    /// <summary>The whole source: structs, then one class per descriptor. <paramref name="isPublic"/>: public types
    /// (Kiln.Sigs), else internal (a mod's own, which are no API of the mod).</summary>
    public static string Emit(SigsModel m, string root = DefaultRoot, bool isPublic = false, string? origin = null)
    {
        var vis = isPublic ? "public" : "internal";
        var sb = new StringBuilder();
        sb.AppendLine("// <auto-generated/>");
        sb.AppendLine("// Kiln.Sigs generator (managed/Kiln.Analyzers/src/Kiln.Sigs.Generator): typed hooks and calls from sigdb");
        sb.AppendLine($"// descriptors ({Xml(origin ?? string.Join(", ", m.Files.Select(System.IO.Path.GetFileName)))}). Do not edit: change the descriptors.");
        sb.AppendLine("#nullable enable");
        sb.AppendLine("#pragma warning disable CS1573, CS1591");
        var structs = m.Structs.Values.Where(s => !s.External).OrderBy(s => s.Name, StringComparer.Ordinal).ToList();
        if (structs.Count > 0)
        {
            sb.AppendLine();
            sb.AppendLine($"namespace {root}");
            sb.AppendLine("{");
            foreach (var s in structs) EmitStruct(sb, s, vis);
            sb.AppendLine("}");
        }
        var taken = new HashSet<string>(StringComparer.Ordinal);
        foreach (var group in m.Sigs.Values.GroupBy(d => NameOf(d.Symbol).Area).OrderBy(g => g.Key, StringComparer.Ordinal))
        {
            sb.AppendLine();
            sb.AppendLine($"namespace {root}.{group.Key}");
            sb.AppendLine("{");
            foreach (var d in group.OrderBy(x => x.Symbol, StringComparer.Ordinal))
            {
                var full = group.Key + "." + NameOf(d.Symbol).Name;
                if (!taken.Add(full)) continue;  // SigsGenerator reports the clash (Clashes)
                EmitClass(sb, d, root, vis);
            }
            sb.AppendLine("}");
        }
        return sb.ToString().Replace("\r\n", "\n");  // the same text on every OS (Kiln.Sigs commits it)
    }

    /// <summary>Descriptors whose class name another descriptor of the set already takes.</summary>
    public static IEnumerable<SigDef> Clashes(SigsModel m)
    {
        var taken = new HashSet<string>(StringComparer.Ordinal);
        foreach (var d in m.Sigs.Values)
        {
            var (area, name) = NameOf(d.Symbol);
            if (!taken.Add(area + "." + name)) yield return d;
        }
    }

    private static void EmitStruct(StringBuilder sb, SigStruct s, string vis)
    {
        var name = Pascal(s.Name);
        sb.AppendLine($"    /// <summary>The game's struct <c>{Xml(s.Name)}</c> as a sigdb descriptor declares it: {s.Size} bytes, aligned to {s.Align}" +
                      (s.Fields.Count > 0 ? "." : "; its fields are not decoded.") + "</summary>");
        sb.AppendLine($"    [global::System.Runtime.InteropServices.StructLayout(global::System.Runtime.InteropServices.LayoutKind.Explicit, Size = {s.Size})]");
        sb.AppendLine($"    {vis} struct {name}");
        sb.AppendLine("    {");
        sb.AppendLine("        /// <summary>The struct's size in bytes.</summary>");
        sb.AppendLine($"        public const int SizeOf = {s.Size};");
        var used = new HashSet<string>(StringComparer.Ordinal) { name, "SizeOf" };
        foreach (var f in s.Fields)
        {
            var fname = Pascal(f.Name);
            while (!used.Add(fname)) fname += "_";
            var type = f.Type switch
            {
                "s8" => "sbyte", "u8" => "byte", "s16" => "short", "u16" => "ushort", "s32" => "int", "u32" => "uint",
                "s64" => "long", "u64" => "ulong", "f32" => "float", "f64" => "double",
                "bool" => "byte",  // the game's bool byte as it is (a value other than 0 or 1 stays visible)
                _ => "nint",       // ptr, T*
            };
            sb.AppendLine($"        /// <summary><c>{Xml(f.Name)}</c> ({Xml(f.Type)}) at +0x{f.Offset:x}.</summary>");
            sb.AppendLine($"        [global::System.Runtime.InteropServices.FieldOffset({f.Offset})] public {type} {fname};");
        }
        sb.AppendLine("    }");
        sb.AppendLine();
    }

    private static void EmitClass(StringBuilder sb, SigDef d, string root, string vis)
    {
        var (_, cls) = NameOf(d.Symbol);
        var row = "sig/" + d.Symbol;
        var sym = Xml(d.Symbol);
        var shift = d.Sret ? 1 : 0;
        var names = new List<string>();
        var props = new HashSet<string>(StringComparer.Ordinal) { cls };
        foreach (var a in d.Args)
        {
            var p = a.Name.Length == 0 ? $"Arg{names.Count + 1}" : Pascal(a.Name);
            if (CallMembers.Contains(p) || p == cls) p += "Arg";
            while (!props.Add(p)) p += "_";
            names.Add(p);
        }
        // Invoke's parameters: the descriptor's names in camel case (locals of Invoke start with "__")
        var pnames = new List<string>();
        var pused = new HashSet<string>(StringComparer.Ordinal);
        foreach (var a in d.Args)
        {
            var p = a.Name.Length == 0 ? $"arg{pnames.Count + 1}" : Camel(a.Name);
            while (!pused.Add(p)) p += "_";
            pnames.Add(p);
        }
        var invokeText = d.Invoke switch
        {
            "tick" => "<see cref=\"Invoke\"/> calls it directly, on the tick thread only.",
            "any" => "<see cref=\"Invoke\"/> calls it directly, from any thread.",
            _ => "It has no direct call (its descriptor says <c>invoke: none</c>): hook it only.",
        };
        var abi = d.AbiTrust ? "trusted by its fragment (<c>\"abi\": \"trust\"</c>)"
            : d.AbiReads.Count + d.AbiStack.Count > 0
                ? $"its code reads [{Xml(string.Join(", ", d.AbiReads.Concat(d.AbiStack.Select(s => "stack+" + s))))}]" +
                  (d.AbiPasses.Count > 0 ? $" and passes [{Xml(string.Join(", ", d.AbiPasses))}] on" : "")
                : "no fingerprint recorded";
        sb.AppendLine($"    /// <summary>The game function <c>{sym}</c> as a typed hook and call: <c>{Xml(Proto(d))}</c> (sigdb descriptor;");
        sb.AppendLine($"    /// {abi}). {invokeText} Advanced mods only. Its capability row <c>{Xml(row)}</c> is on when the symbol resolved");
        sb.AppendLine("    /// and the function's code fits the descriptor on this game build; when it does not, <see cref=\"Detour\"/> is refused");
        sb.AppendLine("    /// with the reason and a direct call throws.");
        sb.AppendLine("    /// <para>A matching fingerprint does not prove complete type identity: f32 versus f64, two types in one stack slot,");
        sb.AppendLine("    /// integer versus pointer, aggregate layout, and the extent of a return buffer are not told apart. Review these");
        sb.AppendLine("    /// meanings for this executable; a matching fingerprint is not a substitute for that review.</para></summary>");
        sb.AppendLine($"    {vis} static unsafe class {cls}");
        sb.AppendLine("    {");
        sb.AppendLine("        /// <summary>The sigdb symbol.</summary>");
        sb.AppendLine($"        public const string Symbol = {Lit(d.Symbol)};");
        sb.AppendLine();
        sb.AppendLine("        /// <summary>The descriptor's capability row: on when the symbol resolved and its code fits the descriptor.</summary>");
        sb.AppendLine($"        public const string Capability = {Lit(row)};");
        sb.AppendLine();
        sb.AppendLine("        /// <summary>The descriptor as kiln.hooks takes it.</summary>");
        sb.AppendLine("        [global::System.ComponentModel.EditorBrowsable(global::System.ComponentModel.EditorBrowsableState.Never)]");
        sb.Append($"        public static readonly global::Kiln.SigDescriptor Descriptor = new(Symbol, global::Kiln.SigKind.{d.Ret.Kind}, {d.Ret.Size}, {(d.Sret ? "true" : "false")}, global::Kiln.SigInvoke.{Pascal(d.Invoke)}");
        foreach (var a in d.Args)
            sb.Append($",\n            new global::Kiln.SigArg({Lit(a.Name)}, global::Kiln.SigKind.{a.Type.Kind}, {a.Type.Size})");
        sb.AppendLine(");");
        sb.AppendLine();

        // ---- Call ----
        sb.AppendLine("        /// <summary>One call of the function in flight, inside a <see cref=\"Handler\"/>: its arguments (read and write; a");
        sb.AppendLine("        /// narrow integer is masked to its type), " + (d.Ret.Kind == "Void" ? "" : "its <see cref=\"Result\"/>, ") +
                      "<see cref=\"CallOriginal\"/> and <see cref=\"Skip\"/>. Valid during");
        sb.AppendLine("        /// the handler only.</summary>");
        sb.AppendLine("        public readonly ref struct Call");
        sb.AppendLine("        {");
        sb.AppendLine("            private readonly global::Kiln.SigCall _c;");
        sb.AppendLine();
        sb.AppendLine("            internal Call(global::Kiln.SigCall c) => _c = c;");
        for (var i = 0; i < d.Args.Count; i++)
        {
            var a = d.Args[i];
            var pos = i + shift;
            var t = CsType(a.Type, root);
            sb.AppendLine();
            sb.AppendLine($"            /// <summary>Argument {i + 1} <c>{Xml(a.Name)}</c> ({Xml(a.Type.Text)}, {Place(a.Type, pos)}).</summary>");
            switch (a.Type.Kind)
            {
                case "F32":
                    sb.AppendLine($"            public float {names[i]} {{ get => _c.F32({pos}); set => _c.SetF32({pos}, value); }}");
                    break;
                case "F64":
                    sb.AppendLine($"            public double {names[i]} {{ get => _c.F64({pos}); set => _c.SetF64({pos}, value); }}");
                    break;
                case "ByVal":
                    // 1/2/4/8 bytes live in the slot; any other size travels as a pointer to the caller's copy
                    sb.AppendLine(a.Type.Size is 1 or 2 or 4 or 8
                        ? $"            public ref {t} {names[i]} => ref *({t}*)_c.SlotAddress({pos});"
                        : $"            public ref {t} {names[i]} => ref *({t}*)unchecked((nint)(long)_c.Int({pos}));");
                    break;
                default:
                    sb.AppendLine($"            public {t} {names[i]} {{ get => {FromSlot(a.Type, root, $"_c.Int({pos})")}; set => _c.SetInt({pos}, {ToSlot(a.Type, root, "value")}); }}");
                    break;
            }
        }
        if (d.Ret.Kind != "Void")
        {
            var rt = CsType(d.Ret, root);
            sb.AppendLine();
            if (d.Sret)
            {
                sb.AppendLine($"            /// <summary>The caller's return buffer ({Xml(d.Ret.Text)}): a reference into the caller's memory, valid during the");
                sb.AppendLine("            /// call; what the caller gets. Fill it before <see cref=\"Skip\"/>: a skipped call returns it as it is.</summary>");
                sb.AppendLine($"            public ref {rt} Result => ref *({rt}*)_c.ReturnBuffer;");
            }
            else if (d.Ret.Kind == "Struct")
            {
                sb.AppendLine($"            /// <summary>The returned struct ({Xml(d.Ret.Text)}, in rax): zero until the original ran or the handler set it.</summary>");
                sb.AppendLine($"            public ref {rt} Result => ref *({rt}*)_c.RetAddress;");
            }
            else
            {
                var reg = d.Ret.IsFloat ? "xmm0" : "rax";
                sb.AppendLine($"            /// <summary>The return value ({Xml(d.Ret.Text)}, {reg}): zero until the original ran or the handler set it; what the");
                sb.AppendLine("            /// caller gets.</summary>");
                if (d.Ret.Kind == "F32") sb.AppendLine("            public float Result { get => _c.RetF32; set => _c.SetRetF32(value); }");
                else if (d.Ret.Kind == "F64") sb.AppendLine("            public double Result { get => _c.RetF64; set => _c.SetRetF64(value); }");
                else sb.AppendLine($"            public {rt} Result {{ get => {FromSlot(d.Ret, root, "_c.RetInt")}; set => _c.SetRetInt({ToSlot(d.Ret, root, "value")}); }}");
            }
        }
        sb.AppendLine();
        sb.AppendLine("            /// <summary>Runs the rest of the hook chain and the game's function once, with the arguments as they are now;");
        sb.AppendLine("            /// a second call does nothing. Without it (and without <see cref=\"Skip\"/>) the function runs after the handler.</summary>");
        sb.AppendLine("            public void CallOriginal() => _c.CallOriginal();");
        sb.AppendLine();
        sb.AppendLine("            /// <summary>Whether <see cref=\"CallOriginal\"/> ran.</summary>");
        sb.AppendLine("            public bool OriginalCalled => _c.OriginalCalled;");
        sb.AppendLine();
        sb.AppendLine("            /// <summary>Ends the call without the game's function: the caller gets " +
                      (d.Ret.Kind == "Void" ? "nothing" : "<see cref=\"Result\"/> as the handler set it") + ". After");
        sb.AppendLine("            /// <see cref=\"CallOriginal\"/> it changes nothing.</summary>");
        sb.AppendLine("            public void Skip() => _c.Skip();");
        sb.AppendLine("        }");
        sb.AppendLine();
        sb.AppendLine("        /// <summary>A handler of the function: it gets the call in flight by reference.</summary>");
        sb.AppendLine("        public delegate void Handler(ref Call call);");
        sb.AppendLine();

        // ---- Detour ----
        sb.AppendLine("        /// <summary>Hooks the function: <paramref name=\"handler\"/> runs on every call, on the calling thread (game worker");
        sb.AppendLine("        /// threads too), in priority order (higher first). An accepted request can be pending until the next safe Pre:");
        sb.AppendLine("        /// inspect the result's Pending and Handle.State. Removed with the handle or when the mod unloads. Refused");
        sb.AppendLine("        /// (<c>Ok</c> false with the reason; a <see cref=\"global::Kiln.HookRefusedEvent\"/> is posted) when the function's code");
        sb.AppendLine("        /// does not fit the descriptor on this game build or the symbol does not resolve.</summary>");
        sb.AppendLine("        /// <exception cref=\"global::System.ArgumentNullException\">A null handler.</exception>");
        sb.AppendLine("        /// <exception cref=\"global::Kiln.KilnUnavailableException\"><c>hooks.typed</c> is off (a core without typed links).</exception>");
        sb.AppendLine("        /// <exception cref=\"global::System.InvalidOperationException\">The calling mod is not advanced, or has unloaded.</exception>");
        sb.AppendLine("        [global::Kiln.AdvancedApi, global::Kiln.AnyThread, global::Kiln.RequiresCapability(\"hooks.typed\")]");
        sb.AppendLine("        public static global::Kiln.HookResult Detour(Handler handler, int priority = 0)");
        sb.AppendLine("        {");
        sb.AppendLine("            if (handler is null) throw new global::System.ArgumentNullException(nameof(handler));");
        sb.AppendLine("            return global::Kiln.TypedHooks.Detour(Descriptor, c => { var call = new Call(c); handler(ref call); }, priority);");
        sb.AppendLine("        }");

        // ---- Invoke ----
        if (d.Invoke != "none")
        {
            var rt = d.Ret.Kind == "Void" ? "void" : CsType(d.Ret, root);
            var ps = d.Args.Select((a, i) => $"{CsType(a.Type, root)} {pnames[i]}").ToList();
            sb.AppendLine();
            sb.AppendLine("        /// <summary>Calls the game function directly, through its hook chain as the game's own callers do" +
                          (d.Invoke == "tick" ? ", on the tick thread (never from a hook handler on a worker)" : ", from any thread") + ".");
            sb.AppendLine("        /// The caller owns every precondition of the function (its objects alive, the game state it needs)." +
                          (d.Sret ? " The struct it returns is copied out of a buffer on this call's stack." : "") + "</summary>");
            sb.AppendLine("        /// <exception cref=\"global::Kiln.KilnUnavailableException\"><c>hooks.typed</c> is off, or <see cref=\"Capability\"/> is (the");
            sb.AppendLine("        /// function's code does not fit the descriptor on this game build, or its symbol does not resolve).</exception>");
            sb.AppendLine("        /// <exception cref=\"global::System.InvalidOperationException\">The calling mod is not advanced" +
                          (d.Invoke == "tick" ? ", or this is not the tick thread" : "") + ".</exception>");
            sb.AppendLine($"        [global::Kiln.AdvancedApi, global::Kiln.{(d.Invoke == "tick" ? "TickThreadOnly" : "AnyThread")}, global::Kiln.RequiresCapability(\"hooks.typed\"), global::Kiln.RequiresCapability({Lit(row)})]");
            sb.AppendLine($"        public static {rt} Invoke({string.Join(", ", ps)})");
            sb.AppendLine("        {");
            sb.AppendLine("            var __frame = default(global::Kiln.SigFrame);");
            if (d.Sret)
            {
                sb.AppendLine($"            var __result = default({rt});");
                sb.AppendLine("            __frame.SetInt(0, unchecked((ulong)(nint)(&__result)));");
            }
            for (var i = 0; i < d.Args.Count; i++)
            {
                var a = d.Args[i];
                var pos = i + shift;
                var v = pnames[i];
                var t = CsType(a.Type, root);
                switch (a.Type.Kind)
                {
                    case "F32": sb.AppendLine($"            __frame.SetF32({pos}, {v});"); break;
                    case "F64": sb.AppendLine($"            __frame.SetF64({pos}, {v});"); break;
                    case "ByVal":
                        if (a.Type.Size is 1 or 2 or 4 or 8)
                        {
                            sb.AppendLine($"            ulong __slot{i} = 0;");
                            sb.AppendLine($"            *({t}*)&__slot{i} = {v};");
                            sb.AppendLine($"            __frame.SetInt({pos}, __slot{i});");
                        }
                        else
                        {
                            sb.AppendLine($"            byte* __raw{i} = stackalloc byte[{a.Type.Size + 15}];");
                            sb.AppendLine($"            var __copy{i} = ({t}*)(((nuint)__raw{i} + 15) & ~(nuint)15);");
                            sb.AppendLine($"            *__copy{i} = {v};");
                            sb.AppendLine($"            __frame.SetInt({pos}, unchecked((ulong)(nint)__copy{i}));");
                        }
                        break;
                    default: sb.AppendLine($"            __frame.SetInt({pos}, {ToSlot(a.Type, root, v)});"); break;
                }
            }
            sb.AppendLine("            global::Kiln.TypedHooks.Invoke(Descriptor, ref __frame);");
            if (d.Sret) sb.AppendLine("            return __result;");
            else if (d.Ret.Kind == "Struct")
            {
                sb.AppendLine("            var __rax = __frame.RetInt;");
                sb.AppendLine($"            return *({rt}*)&__rax;");
            }
            else if (d.Ret.Kind == "F32") sb.AppendLine("            return __frame.RetF32;");
            else if (d.Ret.Kind == "F64") sb.AppendLine("            return __frame.RetF64;");
            else if (d.Ret.Kind != "Void") sb.AppendLine($"            return {FromSlot(d.Ret, root, "__frame.RetInt")};");
            sb.AppendLine("        }");
        }
        sb.AppendLine("    }");
        sb.AppendLine();
    }
}
