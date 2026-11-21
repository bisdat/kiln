// SPDX-License-Identifier: MIT
// What a sidecar value type may be, and its layout hash (the dossier's chapter 15 §4 M2 "Values"; DESIGN "Wire"): an
// unmanaged struct with [StructLayout(LayoutKind.Sequential, Pack = 1)] made only of byte, sbyte, short, ushort, int, uint,
// long, ulong, float, double, enums of those and nested structs under the same rule, at most 1,024 bytes. On the wire it
// is its own bytes (MemoryMarshal.AsBytes, little-endian). The layout hash is FNV-1a-32 over "<path>:<type>@<offset>;" of
// every leaf field in declaration order (an enum is its underlying type: the same bytes travel). The analyzer rule
// KILN199 says the same at compile time.
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;

namespace Kiln;

internal static class LayoutHash
{
    /// <summary>The largest value an extension may have (bytes).</summary>
    internal const int MaxSize = 1024;

    private const int MaxDepth = 16;

    /// <summary>The wire names of the allowed leaf types.</summary>
    private static readonly Dictionary<Type, (string Name, int Size)> Leaves = new()
    {
        [typeof(byte)] = ("u8", 1), [typeof(sbyte)] = ("i8", 1), [typeof(ushort)] = ("u16", 2), [typeof(short)] = ("i16", 2),
        [typeof(uint)] = ("u32", 4), [typeof(int)] = ("i32", 4), [typeof(ulong)] = ("u64", 8), [typeof(long)] = ("i64", 8),
        [typeof(float)] = ("f32", 4), [typeof(double)] = ("f64", 8),
    };

    /// <summary>The leaf lines of <paramref name="type"/> ("charges:u32@0;"), checked against every rule; the reason of
    /// the first broken rule as an ArgumentException (parameter <c>T</c>).</summary>
    /// <param name="type">The value type.</param>
    /// <param name="size">Its size as the runtime lays it out (Unsafe.SizeOf).</param>
    internal static IReadOnlyList<string> LeafLines(Type type, int size)
    {
        var lines = new List<string>();
        var end = Walk(type, "", 0, lines, 0, type);
        if (lines.Count == 0) throw Bad(type, "has no fields");
        if (end != size) throw Bad(type, $"lays out as {size} bytes, its fields add up to {end} (only Pack = 1 sequential structs keep their fields where they are written)");
        if (size > MaxSize) throw Bad(type, $"is {size} bytes; an extension value is at most {MaxSize}");
        return lines;
    }

    /// <summary>FNV-1a-32 over the leaf lines.</summary>
    internal static uint Of(IReadOnlyList<string> lines)
    {
        var h = 2166136261u;
        foreach (var line in lines)
            foreach (var b in Encoding.UTF8.GetBytes(line)) h = (h ^ b) * 16777619u;
        return h;
    }

    /// <summary>The checks and the hash of <typeparamref name="T"/>.</summary>
    internal static (int Size, uint Hash) Of<T>() where T : unmanaged
    {
        var size = Unsafe.SizeOf<T>();
        return (size, Of(LeafLines(typeof(T), size)));
    }

    private static int Walk(Type t, string path, int offset, List<string> lines, int depth, Type root)
    {
        if (depth > MaxDepth) throw Bad(root, $"nests structs deeper than {MaxDepth}");
        if (!t.IsValueType || t.IsPrimitive || t.IsEnum || t.IsPointer)
            throw Bad(root, path.Length == 0 ? "is not a struct" : $"field {path} is {t.Name}, not a struct");
        if (t.IsGenericTypeDefinition) throw Bad(root, "is an open generic type");
        var layout = t.StructLayoutAttribute;
        if (layout is null || layout.Value != LayoutKind.Sequential || layout.Pack != 1)
            throw Bad(root, $"{(path.Length == 0 ? t.Name : $"{t.Name} (field {path})")} needs [StructLayout(LayoutKind.Sequential, Pack = 1)]");
        var fields = t.GetFields(BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic);
        Array.Sort(fields, static (a, b) => a.MetadataToken.CompareTo(b.MetadataToken));
        foreach (var f in fields)
        {
            var p = path.Length == 0 ? f.Name : path + "." + f.Name;
            if (f.GetCustomAttribute<FixedBufferAttribute>() is not null) throw Bad(root, $"field {p} is a fixed buffer; use a nested struct of fields");
            var ft = f.FieldType;
            if (ft.IsPointer || ft.IsFunctionPointer || ft.IsUnmanagedFunctionPointer) throw Bad(root, $"field {p} is a pointer");
            var leaf = ft.IsEnum ? Enum.GetUnderlyingType(ft) : ft;
            if (Leaves.TryGetValue(leaf, out var l))
            {
                lines.Add($"{p}:{l.Name}@{offset};");
                offset += l.Size;
                continue;
            }
            if (ft.IsEnum) throw Bad(root, $"field {p} is an enum of {leaf.Name}");
            if (ft == typeof(bool) || ft == typeof(char) || ft == typeof(nint) || ft == typeof(nuint) || !ft.IsValueType || ft.IsPrimitive)
                throw Bad(root, $"field {p} is {Pretty(ft)}; allowed: byte, sbyte, short, ushort, int, uint, long, ulong, float, double, their enums and structs of them");
            offset = Walk(ft, p, offset, lines, depth + 1, root);
        }
        return offset;
    }

    private static string Pretty(Type t) =>
        t == typeof(bool) ? "bool" : t == typeof(char) ? "char" : t == typeof(nint) ? "nint" : t == typeof(nuint) ? "nuint" :
        !t.IsValueType ? $"a reference type ({t.Name})" : t.Name;

    private static ArgumentException Bad(Type root, string what) =>
        new($"extension value type {root.FullName ?? root.Name} {what}", "T");
}
