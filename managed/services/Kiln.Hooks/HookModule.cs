// SPDX-License-Identifier: MIT
// Hooks inside another loaded module (0.2.0-dev.29.19): another mod's DLL or a loader's DLL, opened with Hooks.Module.
// The native side is kiln_hooks.h open_module / module_scan; docs/kiln/hooks.md "Hooks inside another mod's DLL".
using System.Diagnostics.CodeAnalysis;
using System.Text;

namespace Kiln;

/// <summary>A loaded module opened for hooks with <see cref="Hooks.Module"/>: another mod's DLL or a loader's DLL
/// (Creative Mode's <c>creative.dll</c>, say). Addresses inside its executable code are hook targets like the game's
/// own code, for <see cref="Hooks.Detour(nuint, DetourHandler, int)"/> and <see cref="Hooks.Mid(nuint, MidHandler, int)"/>.
/// Kiln pins the module: it stays loaded until the game exits, so a hook never outlives the code it was written into.
/// Typed hooks stay the game's (their descriptor is checked against the game's code).</summary>
public sealed unsafe class HookModule
{
    internal HookModule(string name, in HooksNative.ModuleRaw raw)
    {
        Name = name;
        Base = raw.Base;
        Size = (nuint)raw.ImageSize;
        CodeStart = raw.CodeLo;
        CodeEnd = raw.CodeHi;
        IsGame = (raw.Flags & HooksNative.ModuleGame) != 0;
    }

    /// <summary>The name it was opened by (its file name, as given to <see cref="Hooks.Module"/>).</summary>
    public string Name { get; }

    /// <summary>Where the module is loaded.</summary>
    public nuint Base { get; }

    /// <summary>Its image size in bytes.</summary>
    public nuint Size { get; }

    /// <summary>The start of its first executable section (absolute).</summary>
    public nuint CodeStart { get; }

    /// <summary>The end of its last executable section (absolute, exclusive). A hook target must lie in one of the
    /// executable sections, which Kiln checks when the hook is installed.</summary>
    public nuint CodeEnd { get; }

    /// <summary>True for the game's exe (Kiln hooked its code already; nothing was pinned).</summary>
    public bool IsGame { get; }

    /// <summary>The absolute address of the code at <paramref name="offset"/> from the module's base (an RVA, as a
    /// disassembler shows it for the DLL). Throws when the offset is outside the module's code.</summary>
    /// <exception cref="ArgumentOutOfRangeException">The address would not be in the module's code.</exception>
    public nuint At(nuint offset)
    {
        var a = Base + offset;
        if (offset >= Size || a < CodeStart || a >= CodeEnd)
            throw new ArgumentOutOfRangeException(nameof(offset),
                $"0x{offset:X} is outside {Name}'s code (0x{CodeStart - Base:X}..0x{CodeEnd - Base:X})");
        return a;
    }

    /// <summary>Whether <paramref name="address"/> lies in the module's code span.</summary>
    public bool Contains(nuint address) => address >= CodeStart && address < CodeEnd;

    /// <summary>The single address in the module's code matching <paramref name="pattern"/> (sigdb syntax: hex bytes,
    /// <c>??</c> wildcards, holes act as wildcards). Throws when there is not exactly one match. A pattern survives an
    /// update of the module that moves its code, where a fixed offset does not.</summary>
    /// <exception cref="KeyNotFoundException">Nothing matched.</exception>
    /// <exception cref="InvalidOperationException">More than one match; narrow the pattern.</exception>
    public nuint Find(string pattern)
    {
        var all = FindAll(pattern, 2);
        return all.Length switch
        {
            1 => all[0],
            0 => throw new KeyNotFoundException($"pattern '{pattern}' matched nothing in {Name}'s code"),
            _ => throw new InvalidOperationException($"pattern '{pattern}' matched {all.Length}+ times in {Name}'s code; narrow it"),
        };
    }

    /// <summary>Up to <paramref name="max"/> addresses in the module's code matching <paramref name="pattern"/>, in
    /// address order. The module's live bytes are searched: another engine's hook there can hide a match.</summary>
    public nuint[] FindAll(string pattern, int max = 64)
    {
        ArgumentNullException.ThrowIfNull(pattern);
        if (IsGame) return Symbols.FindAll(pattern, max);
        HooksGate.RequireAdvanced("HookModule.Find");
        var t = HooksNative.RequireModules("HookModule.Find");
        if (max < 1) max = 1;
        var buf = new nuint[max];
        uint count = 0;
        int rc;
        var bytes = Encoding.UTF8.GetBytes(pattern);
        fixed (nuint* p = buf)
        fixed (byte* pat = bytes)
            rc = t->ModuleScan(Base, new NativeStr { Ptr = pat, Len = (nuint)bytes.Length }, p, (uint)max, &count);
        if (rc == HooksNative.NotFound) return [];
        if (rc == HooksNative.InvalidArgument) throw new ArgumentException($"bad pattern '{pattern}'", nameof(pattern));
        if (rc != HooksNative.Ok && rc != HooksNative.BufferTooSmall)
            throw new InvalidOperationException($"HookModule.Find({Name}): {HooksNative.Describe(rc)}");
        return buf.AsSpan(0, (int)count).ToArray();
    }

    /// <summary>"creative.dll+0x15F5F7" for an address in the module.</summary>
    public string Describe(nuint address) => $"{Name}+0x{address - Base:X}";

    /// <summary>The module's name and base address, like <c>creative.dll at 0x7FFB83A70000</c>.</summary>
    public override string ToString() => $"{Name} at 0x{Base:X}";
}

public static unsafe partial class Hooks
{
    // The modules opened in this process (by any mod), for naming an address in a mod's log. Modules are pinned, so a
    // base never goes to another module.
    private static readonly List<HookModule> Opened = [];

    /// <summary>Opens the loaded module <paramref name="name"/> (its file name: <c>"creative.dll"</c>) for hooks: another
    /// mod's DLL or a loader's DLL. From then on addresses inside its executable code are hook targets for
    /// <see cref="Detour(nuint, DetourHandler, int)"/> and <see cref="Mid(nuint, MidHandler, int)"/>, like the game's
    /// own code; anything else outside the game stays refused. Kiln pins the module (it stays loaded until the game
    /// exits) and never loads it: open it once it is there. kiln.log names the module, and each hook in it as
    /// <c>creative.dll+0x...</c>. Opening it again is cheap.</summary>
    /// <exception cref="KeyNotFoundException">No module of that name is loaded (yet).</exception>
    /// <exception cref="InvalidOperationException">One of Kiln's own modules (<c>kiln-*.dll</c>), or a module Kiln
    /// could not read or pin (kiln.log says why).</exception>
    /// <exception cref="KilnUnavailableException"><c>hooks.foreign_modules</c> is off (a Kiln runtime older than
    /// 0.2.0-dev.29.19).</exception>
    [RequiresCapability(HooksCaps.ForeignModules)]
    public static HookModule Module(string name)
        => Open("Hooks.Module", name) ?? throw new KeyNotFoundException($"Hooks.Module: no module '{name}' is loaded");

    /// <summary><see cref="Module"/> that returns false when no module of that name is loaded (yet).</summary>
    [RequiresCapability(HooksCaps.ForeignModules)]
    public static bool TryModule(string name, [NotNullWhen(true)] out HookModule? module)
    {
        module = Open("Hooks.TryModule", name);
        return module is not null;
    }

    /// <summary>The single address matching <paramref name="pattern"/> in the code of the loaded module
    /// <paramref name="moduleName"/>, which this opens for hooks: <c>Hooks.Module(moduleName).Find(pattern)</c>.</summary>
    [RequiresCapability(HooksCaps.ForeignModules)]
    public static nuint FindInModule(string moduleName, string pattern) => Module(moduleName).Find(pattern);

    private static HookModule? Open(string api, string name)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(name);
        var mod = HooksGate.RequireAdvanced(api);
        var t = HooksNative.RequireModules(api);
        HooksNative.ModuleRaw raw = default;
        raw.Size = (uint)sizeof(HooksNative.ModuleRaw);
        var bytes = Encoding.UTF8.GetBytes(name);
        int rc;
        fixed (byte* p = bytes)
            rc = t->OpenModule(HookCookies.Owner(mod), new NativeStr { Ptr = p, Len = (nuint)bytes.Length }, &raw);
        switch (rc)
        {
            case HooksNative.Ok:
                break;
            case HooksNative.NotFound:
                return null;
            case HooksNative.Unavailable:
                throw new InvalidOperationException(
                    $"{api}(\"{name}\"): Kiln does not open this module for hooks (one of Kiln's own modules, or it could not be pinned; kiln.log says why)");
            default:
                throw new InvalidOperationException(
                    $"{api}(\"{name}\"): {HooksNative.Describe(rc)} (no executable code, or its headers could not be read; kiln.log says why)");
        }
        var m = new HookModule(name, raw);
        lock (Opened)
        {
            if (!Opened.Exists(o => o.Base == m.Base))
            {
                Opened.Add(m);
                if (!m.IsGame)
                    mod.Log.Info($"hooks: {name} opened for hooks (base 0x{m.Base:X}, code 0x{m.CodeStart - m.Base:X}..0x{m.CodeEnd - m.Base:X}; pinned)");
            }
        }
        return m;
    }

    // "creative.dll+0x15F5F7 (0x7FFB83A7F5F7)" for an address in an opened module, else "0x7FFB83A7F5F7".
    internal static string Where(nuint address)
    {
        lock (Opened)
            foreach (var m in Opened)
                if (!m.IsGame && m.Contains(address))
                    return $"{m.Describe(address)} (0x{address:X})";
        return "0x" + address.ToString("X");
    }
}
