// SPDX-License-Identifier: MIT
using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using Kiln.GameIds;

namespace Kiln.Sigs.Generator;

/// <summary>A struct block of a sigdb file (<c>"structs"</c>): its size and fields.</summary>
internal sealed class SigStruct
{
    public SigStruct(string name, uint size, uint align, List<SigField> fields, string source, bool external = false)
    {
        Name = name;
        Size = size;
        Align = align;
        Fields = fields;
        Source = source;
        External = external;
    }

    public string Name { get; }
    public uint Size { get; }
    public uint Align { get; }
    public List<SigField> Fields { get; }
    public string Source { get; }
    /// <summary>Declared by another assembly (a core struct of Kiln.Sigs that a mod's descriptor names): not emitted.</summary>
    public bool External { get; }
}

internal sealed class SigField
{
    public SigField(string name, string type, uint offset)
    {
        Name = name;
        Type = type;
        Offset = offset;
    }

    public string Name { get; }
    public string Type { get; }
    public uint Offset { get; }
}

/// <summary>A type of an argument or return, resolved the way kiln.hooks resolves it (hooks_fingerprint.cpp type_of).</summary>
internal sealed class SigType
{
    public SigType(string text, string kind, uint size, string? pointee = null, SigStruct? structDef = null)
    {
        Text = text;
        Kind = kind;
        Size = size;
        Pointee = pointee;
        Struct = structDef;
    }

    /// <summary>As written in the descriptor ("f32", "keen::Daytime*", "OptS32").</summary>
    public string Text { get; }
    /// <summary>The Kiln.SigKind member: S8..U64, F32, F64, Bool, Ptr, ByVal, Struct, Void.</summary>
    public string Kind { get; }
    public uint Size { get; }
    /// <summary>For a pointer: the pointed-to type as written ("u16" of "u16*").</summary>
    public string? Pointee { get; }
    /// <summary>For a by-value struct or a struct return.</summary>
    public SigStruct? Struct { get; }

    public bool IsFloat => Kind is "F32" or "F64";
}

internal sealed class SigArgDef
{
    public SigArgDef(string name, SigType type)
    {
        Name = name;
        Type = type;
    }

    public string Name { get; }
    public SigType Type { get; }
}

/// <summary>One descriptor (<c>"sig"</c> in a symbol's entry, or an entry of <c>"sigs"</c>).</summary>
internal sealed class SigDef
{
    public string Symbol = "";
    public string Source = "";
    public SigType Ret = null!;   // Void, a scalar, or Struct (sret or in rax)
    public bool Sret;
    public string Invoke = "none";  // none | tick | any
    public List<SigArgDef> Args = new();
    public List<string> AbiReads = new();
    public List<string> AbiPasses = new();
    public List<string> AbiStack = new();
    public bool AbiTrust;

    public int Positions => Args.Count + (Sret ? 1 : 0);

    /// <summary>The shape, for telling two declarations of one symbol apart.</summary>
    public string Shape =>
        $"{Ret.Text}|{Sret}|{Invoke}|" + string.Join(",", Args.Select(a => a.Name + ":" + a.Type.Text));
}

internal sealed class SigProblem
{
    public SigProblem(string symbol, string source, string message)
    {
        Symbol = symbol;
        Source = source;
        Message = message;
    }

    public string Symbol { get; }
    public string Source { get; }
    public string Message { get; }
}

/// <summary>Every descriptor and struct of a set of sigdb files, validated by the rules of kiln.hooks' own parser
/// (native/core/src/services/hooks_fingerprint.cpp parse_sig, parse_structs), so a descriptor the core would refuse as
/// malformed is reported here and never generated.</summary>
internal sealed class SigsModel
{
    public const int MaxPositions = 20;

    public readonly Dictionary<string, SigStruct> Structs = new(StringComparer.Ordinal);
    public readonly SortedDictionary<string, SigDef> Sigs = new(StringComparer.Ordinal);
    public readonly List<SigProblem> Problems = new();
    public readonly List<(string File, string Error)> BadFiles = new();
    public readonly List<string> Files = new();

    internal static readonly Dictionary<string, (string Kind, uint Size)> Scalars = new(StringComparer.Ordinal)
    {
        ["s8"] = ("S8", 1), ["u8"] = ("U8", 1), ["s16"] = ("S16", 2), ["u16"] = ("U16", 2),
        ["s32"] = ("S32", 4), ["u32"] = ("U32", 4), ["s64"] = ("S64", 8), ["u64"] = ("U64", 8),
        ["f32"] = ("F32", 4), ["f64"] = ("F64", 8), ["bool"] = ("Bool", 1), ["ptr"] = ("Ptr", 8),
    };

    /// <summary>Reads <paramref name="files"/> (path, text). <paramref name="externalStruct"/> answers the size of a
    /// struct no file declares (a core struct of Kiln.Sigs a mod's descriptor names), or null.</summary>
    public static SigsModel Parse(IEnumerable<(string Path, string Text)> files, Func<string, uint?>? externalStruct = null)
    {
        var m = new SigsModel();
        var docs = new List<(string Path, Dictionary<string, object?> Doc)>();
        foreach (var (path, text) in files.OrderBy(f => f.Path, StringComparer.Ordinal))
        {
            try
            {
                if (new MiniJson(text).Value() is Dictionary<string, object?> doc) docs.Add((path, doc));
                else m.BadFiles.Add((path, "the file is not a JSON object"));
            }
            catch (Exception e)
            {
                m.BadFiles.Add((path, e.Message));
            }
        }
        // structs first: a descriptor may name a struct of another file (struct names are global across the files)
        foreach (var (path, doc) in docs)
        {
            if (!doc.TryGetValue("structs", out var so) || so is null) continue;
            if (so is not Dictionary<string, object?> structs)
            {
                m.Problems.Add(new SigProblem("structs", path, "\"structs\" must be an object"));
                continue;
            }
            foreach (var kv in structs)
            {
                var err = ParseStruct(kv.Key, kv.Value, path, out var def);
                if (err != null) m.Problems.Add(new SigProblem(kv.Key, path, err));
                else if (m.Structs.TryGetValue(kv.Key, out var was))
                    m.Problems.Add(new SigProblem(kv.Key, path, $"struct {kv.Key} is declared twice ({was.Source} and {path})"));
                else m.Structs[kv.Key] = def!;
            }
        }
        foreach (var (path, doc) in docs)
        {
            m.Files.Add(path);
            if (doc.TryGetValue("symbols", out var syms) && syms is Dictionary<string, object?> symbols)
                foreach (var kv in symbols)
                    if (kv.Value is Dictionary<string, object?> entry && entry.TryGetValue("sig", out var sig))
                        m.Add(kv.Key, sig, path, externalStruct);
            if (doc.TryGetValue("sigs", out var so) && so is not null)
            {
                if (so is Dictionary<string, object?> sigs)
                    foreach (var kv in sigs) m.Add(kv.Key, kv.Value, path, externalStruct);
                else m.Problems.Add(new SigProblem("sigs", path, "\"sigs\" must be an object"));
            }
        }
        return m;
    }

    private void Add(string symbol, object? json, string path, Func<string, uint?>? externalStruct)
    {
        var err = ParseSig(symbol, json, path, externalStruct, out var def);
        if (err != null)
        {
            Problems.Add(new SigProblem(symbol, path, err));
            return;
        }
        if (Sigs.TryGetValue(symbol, out var was))
        {
            // the server and client fragments of a mod may both describe a symbol: one class, if they agree
            if (was.Shape != def!.Shape)
                Problems.Add(new SigProblem(symbol, path, $"described differently in {was.Source} and {path}"));
            return;
        }
        Sigs[symbol] = def!;
    }

    private static string? ParseStruct(string name, object? json, string path, out SigStruct? def)
    {
        def = null;
        if (json is not Dictionary<string, object?> s || !TryUInt(s, "size", out var size))
            return $"struct {name}: \"size\" is missing";
        var align = TryUInt(s, "align", out var a) ? a : 8u;
        if (size == 0 || size > 4096 || align is not (1 or 2 or 4 or 8 or 16))
            return $"struct {name}: size must be 1..4096 and align 1, 2, 4, 8 or 16";
        var fields = new List<SigField>();
        if (s.TryGetValue("fields", out var fo) && fo is not null)
        {
            if (fo is not List<object?> list) return $"struct {name}: \"fields\" must be an array";
            foreach (var f in list)
            {
                if (f is not Dictionary<string, object?> fd) return $"struct {name}: a field must be an object";
                var fname = Str(fd, "name") ?? "";
                var ftype = Str(fd, "type") ?? "";
                var off = TryUInt(fd, "offset", out var o) ? o : 0u;
                uint fsize;
                if (Scalars.TryGetValue(ftype, out var sc)) fsize = sc.Size;
                else if (ftype.EndsWith("*", StringComparison.Ordinal)) fsize = 8;
                else return $"struct {name}: field '{fname}' needs a name and a scalar or pointer type";
                if (fname.Length == 0) return $"struct {name}: field '{fname}' needs a name and a scalar or pointer type";
                if ((ulong)off + fsize > size) return $"struct {name}: field '{fname}' ends behind the struct's size";
                fields.Add(new SigField(fname, ftype, off));
            }
        }
        def = new SigStruct(name, size, align, fields, path);
        return null;
    }

    private string? TypeOf(string type, Func<string, uint?>? externalStruct, out SigType? t)
    {
        t = null;
        if (type.Length == 0) return "a type is missing";
        if (Scalars.TryGetValue(type, out var sc))
        {
            t = new SigType(type, sc.Kind, sc.Size);
            return null;
        }
        if (type.EndsWith("*", StringComparison.Ordinal))
        {
            t = new SigType(type, "Ptr", 8, type.Substring(0, type.Length - 1).Trim());
            return null;
        }
        var st = StructNamed(type, externalStruct);
        if (st == null)
            return $"type '{type}' is neither a scalar (s8..u64, f32, f64, bool, ptr), a pointer (T*) nor a struct declared under \"structs\"";
        t = new SigType(type, "ByVal", st.Size, structDef: st);
        return null;
    }

    private SigStruct? StructNamed(string name, Func<string, uint?>? externalStruct)
    {
        if (Structs.TryGetValue(name, out var s)) return s;
        var size = externalStruct?.Invoke(name);
        return size is > 0 ? new SigStruct(name, size.Value, 8, new List<SigField>(), "", external: true) : null;
    }

    private string? ParseSig(string symbol, object? json, string path, Func<string, uint?>? externalStruct, out SigDef? def)
    {
        def = null;
        if (json is not Dictionary<string, object?> j) return "sig must be an object";
        if ((Str(j, "conv") ?? "win64") != "win64")
            return "conv must be \"win64\" (varargs and other conventions cannot be described)";
        if (j.TryGetValue("varargs", out var va) && va is true) return "a varargs function cannot be described";
        var d = new SigDef { Symbol = symbol, Source = path };
        var retText = Str(j, "ret") ?? "void";
        if (j.TryGetValue("sret", out var sretObj) && sretObj is not null)
        {
            var st = sretObj as string ?? "";
            var sd = StructNamed(st, externalStruct);
            if (sd == null) return $"sret: struct '{st}' is not declared under \"structs\"";
            if (j.ContainsKey("ret") && retText != "void" && retText != st)
                return "\"ret\" and \"sret\" disagree: a function returning through a hidden buffer has no other return";
            d.Sret = true;
            d.Ret = new SigType(st, "Struct", sd.Size, structDef: sd);
        }
        else if (retText == "void")
        {
            d.Ret = new SigType("void", "Void", 0);
        }
        else
        {
            var err = TypeOf(retText, externalStruct, out var rt);
            if (err != null) return "ret: " + err;
            if (rt!.Kind == "ByVal")
            {
                if (rt.Size is not (1 or 2 or 4 or 8))
                    return $"ret: struct '{retText}' is {rt.Size} bytes: it returns through a hidden buffer, declare it as \"sret\"";
                rt = new SigType(retText, "Struct", rt.Size, structDef: rt.Struct);
            }
            d.Ret = rt;
        }
        if (j.TryGetValue("args", out var ao) && ao is not null)
        {
            if (ao is not List<object?> args) return "args must be an array";
            foreach (var a in args)
            {
                if (a is not Dictionary<string, object?> ad) return "an argument must be an object";
                var name = Str(ad, "name") ?? "";
                var type = Str(ad, "type") ?? "";
                if (type == "sret") return $"arg '{name}': a hidden return buffer is declared as \"sret\": \"<Struct>\", never in args";
                var err = TypeOf(type, externalStruct, out var at);
                if (err != null) return $"arg '{name}': {err}";
                if (name.Length > 0 && d.Args.Any(x => x.Name == name)) return $"arg '{name}' is declared twice";
                d.Args.Add(new SigArgDef(name, at!));
            }
        }
        if (d.Positions > MaxPositions) return $"more than {MaxPositions} argument positions";
        d.Invoke = Str(j, "invoke") ?? "none";
        if (d.Invoke is not ("none" or "tick" or "any")) return "invoke must be \"none\", \"tick\" or \"any\"";
        if (j.TryGetValue("abi", out var abi) && abi is not null)
        {
            if (abi is string s)
            {
                if (s != "trust") return "abi must be an object or \"trust\"";
                d.AbiTrust = true;
            }
            else if (abi is Dictionary<string, object?> ab)
            {
                d.AbiReads = Strings(ab, "reads");
                d.AbiPasses = Strings(ab, "passes");
                d.AbiStack = Strings(ab, "stack");
            }
            else return "abi must be an object or \"trust\"";
        }
        def = d;
        return null;
    }

    private static string? Str(Dictionary<string, object?> d, string key) => d.TryGetValue(key, out var v) ? v as string : null;

    private static List<string> Strings(Dictionary<string, object?> d, string key) =>
        d.TryGetValue(key, out var v) && v is List<object?> l ? l.Select(x => x as string ?? Convert.ToString(x, CultureInfo.InvariantCulture) ?? "").ToList() : new List<string>();

    private static bool TryUInt(Dictionary<string, object?> d, string key, out uint value)
    {
        value = 0;
        if (!d.TryGetValue(key, out var v) || v is not double n || n < 0 || n > uint.MaxValue || Math.Floor(n) != n) return false;
        value = (uint)n;
        return true;
    }
}
