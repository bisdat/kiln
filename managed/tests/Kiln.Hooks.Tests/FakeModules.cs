// SPDX-License-Identifier: MIT
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;

namespace Kiln.Hooks.Tests;

/// <summary>open_module and module_scan of the fake kiln.hooks table (0.2.0-dev.29.19), with the core's answers
/// (native/core/src/services/hooks.cpp): a loaded module is found by file name in any case, Kiln's own modules are
/// refused (KILN_ERR_UNAVAILABLE), one that is not loaded is not found; module_scan wants an opened module's base.</summary>
internal static unsafe class FakeModules
{
    public sealed record Module(nuint Base, ulong ImageSize, nuint CodeLo, nuint CodeHi, uint Flags);

    /// <summary>The loaded modules by file name (lower case).</summary>
    public static readonly Dictionary<string, Module> Loaded = new(StringComparer.Ordinal);

    /// <summary>module_scan's matches: (base, pattern) -> addresses.</summary>
    public static readonly Dictionary<(nuint, string), nuint[]> Matches = new();

    public static readonly HashSet<nuint> Opened = [];
    public static readonly List<(ulong Owner, string Name)> OpenCalls = [];

    public static void Reset()
    {
        Loaded.Clear();
        Matches.Clear();
        Opened.Clear();
        OpenCalls.Clear();
    }

    public static void Fill(HooksNative.ModulesTable* t)
    {
        *(nint*)&t->OpenModule = (nint)(delegate* unmanaged[Cdecl]<ulong, FakeRegistry.NativeText, HooksNative.ModuleRaw*, int>)&OpenModule;
        *(nint*)&t->ModuleScan = (nint)(delegate* unmanaged[Cdecl]<nuint, FakeRegistry.NativeText, nuint*, uint, uint*, int>)&ModuleScan;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    static int OpenModule(ulong owner, FakeRegistry.NativeText name, HooksNative.ModuleRaw* out_)
    {
        if (out_ == null || out_->Size < (uint)sizeof(HooksNative.ModuleRaw) || name.Len == 0) return HooksNative.InvalidArgument;
        var n = Encoding.UTF8.GetString(name.Ptr, (int)name.Len).ToLowerInvariant();
        OpenCalls.Add((owner, n));
        if (n.StartsWith("kiln-", StringComparison.Ordinal)) return HooksNative.Unavailable;
        if (!Loaded.TryGetValue(n, out var m)) return HooksNative.NotFound;
        out_->Flags = m.Flags;
        out_->Base = m.Base;
        out_->ImageSize = m.ImageSize;
        out_->CodeLo = m.CodeLo;
        out_->CodeHi = m.CodeHi;
        out_->CodeSections = 1;
        if ((m.Flags & HooksNative.ModuleGame) == 0) Opened.Add(m.Base);
        return HooksNative.Ok;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    static int ModuleScan(nuint module, FakeRegistry.NativeText pattern, nuint* out_, uint cap, uint* count)
    {
        *count = 0;
        var p = Encoding.UTF8.GetString(pattern.Ptr, (int)pattern.Len);
        if (!Opened.Contains(module) || p.Contains('z')) return HooksNative.InvalidArgument;
        if (!Matches.TryGetValue((module, p), out var hits) || hits.Length == 0) return HooksNative.NotFound;
        var n = (uint)Math.Min(cap, hits.Length);
        for (var i = 0; i < n; i++) out_[i] = hits[i];
        *count = n;
        return hits.Length > cap ? HooksNative.BufferTooSmall : HooksNative.Ok;
    }
}
