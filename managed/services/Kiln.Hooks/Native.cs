// SPDX-License-Identifier: MIT
// The native kiln.hooks table (native/core/abi/services/kiln_hooks.h). Field order and types must match the C header.
using System.Runtime.InteropServices;
using System.Text;

namespace Kiln;

internal static unsafe class HooksNative
{
    public const string Name = "kiln.hooks";
    public const uint Version = 1;
    public const int Ok = 0, InvalidArgument = 1, NotFound = 2, Unavailable = 3, WrongThread = 4, BufferTooSmall = 5;

    /// <summary>A detour call in progress; mirrors KilnHookCall. args are rcx,rdx,r8,r9.</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct HookCallRaw
    {
        public uint Size;
        public uint ArgCount;
        public ulong Hook;
        public nuint Arg0, Arg1, Arg2, Arg3;
        public void* Reserved;
    }

    /// <summary>Full register context of a mid hook; mirrors KilnRegisters / safetyhook Context64 exactly, so writing
    /// a field changes the register on resume.</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct RegsRaw
    {
        public fixed byte Xmm[256];  // xmm0..xmm15, 16 bytes each
        public nuint Rflags, R15, R14, R13, R12, R11, R10, R9, R8, Rdi, Rsi, Rdx, Rcx, Rbx, Rax, Rbp, Rsp,
            TrampolineRsp, Rip;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct SigSummary
    {
        public uint Size;
        public uint Symbols, SymbolsOk, Constants, ConstantsOk, Features, FeaturesOk;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct Stats
    {
        public uint Size;
        public uint DetourTargets, MidTargets, Handlers, Patches;
        public ulong DetourCalls, MidCalls;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct Table
    {
        public ServiceHeader Header;
        public delegate* unmanaged[Cdecl]<nuint> ImageBase;
        public delegate* unmanaged[Cdecl]<nuint> ImageSize;
        public delegate* unmanaged[Cdecl]<NativeStr, nuint*, int> Symbol;
        public delegate* unmanaged[Cdecl]<NativeStr, long*, int> Constant;
        public delegate* unmanaged[Cdecl]<NativeStr, nuint*, uint, uint*, int> Scan;
        public delegate* unmanaged[Cdecl]<NativeStr, nuint, NativeStr, long*, int> ScanHole;
        public delegate* unmanaged[Cdecl]<ulong, NativeStr, NativeStr, SigSummary*, int> LoadSymbols;
        public delegate* unmanaged[Cdecl]<NativeStr, int> UnloadSymbols;
        public delegate* unmanaged[Cdecl]<nuint, void*, nuint, int> Read;
        public delegate* unmanaged[Cdecl]<nuint, void*, nuint, int> Write;
        public delegate* unmanaged[Cdecl]<ulong, nuint, void*, nuint, ulong*, int> Patch;
        public delegate* unmanaged[Cdecl]<ulong, int> Unpatch;
        public delegate* unmanaged[Cdecl]<ulong, nuint, int, delegate* unmanaged[Cdecl]<HookCallRaw*, void*, nuint>, void*, ulong*, int> Detour;
        public delegate* unmanaged[Cdecl]<ulong, nuint, int, delegate* unmanaged[Cdecl]<RegsRaw*, void*, void>, void*, ulong*, int> Mid;
        public delegate* unmanaged[Cdecl]<HookCallRaw*, nuint> CallOriginal;
        public delegate* unmanaged[Cdecl]<ulong, int> Revoke;
        public delegate* unmanaged[Cdecl]<ulong, int> RevokeOwner;
        public delegate* unmanaged[Cdecl]<Stats*, int> GetStats;
    }

    // ---- typed links (kiln_hooks.h "typed links", since 0.2.0-dev.28) ----
    public const int SigMismatch = 100, SigUnknown = 101;  // KILN_HOOKS_ERR_SIG_MISMATCH / _UNKNOWN
    public const nuint TypedContinue = 0, TypedSkip = 1;    // KILN_TYPED_CONTINUE / KILN_TYPED_SKIP
    public const int MaxPositions = 20;                    // KILN_SIG_MAX_ARGS: 4 registers + 16 stack slots

    /// <summary>KilnCallFrame (0x90 bytes): one Win64 call as data.</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct CallFrame
    {
        public fixed ulong Gpr[4];      // +0x00 rcx rdx r8 r9
        public fixed byte Xmm[64];      // +0x20 xmm0..3, 16 bytes each
        public ulong* Stack;            // +0x60 the 5th and later arguments
        public ulong Rax;               // +0x68
        public fixed byte Xmm0R[16];    // +0x70
        public ulong Slot;              // +0x80
        public ulong Reserved;          // +0x88
    }

    /// <summary>KilnSigArg.</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct SigArgRaw
    {
        public NativeStr Name;
        public uint Kind;
        public uint Size;
    }

    /// <summary>KilnSig.</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct SigRaw
    {
        public uint Size;
        public uint Argc;
        public SigArgRaw* Args;
        public uint RetKind;
        public uint RetSize;
        public byte Sret;
        public fixed byte Pad[7];
    }

    /// <summary>KilnHookOrder (with the dev.21 flags).</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct OrderRaw
    {
        public uint Size;
        public int Priority;
        public NativeStr Label;
        public NativeStr Before;
        public NativeStr After;
        public uint Flags;
    }

    /// <summary>The kiln.hooks table up to typed_refusal: the v1 members (<see cref="Table"/>), the v2 block (not used
    /// by managed code, so untyped) and the typed block. A core is new enough when its header.size covers it.</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct TypedTable
    {
        public Table V1;
        public void* RawDetour, Caller, Detour2, CallOriginal2, Mid2, MapJson;
        public delegate* unmanaged[Cdecl]<ulong, nuint, OrderRaw*, SigRaw*, delegate* unmanaged[Cdecl]<CallFrame*, void*, nuint>, void*, ulong*, int> DetourTyped;
        public delegate* unmanaged[Cdecl]<CallFrame*, int> CallOriginalFrame;
        public delegate* unmanaged[Cdecl]<nuint, CallFrame*, uint, int> CallWithFrame;
        public delegate* unmanaged[Cdecl]<nuint, byte*, nuint, nuint*, int> Fingerprint;
        public delegate* unmanaged[Cdecl]<byte*, nuint, nuint*, int> TypedRefusal;
    }

    /// <summary>The typed members of the table, or null when kiln.hooks is missing or older than dev.28 (its
    /// header.size does not cover them). Never cached: it follows <see cref="T"/>.</summary>
    public static TypedTable* Typed
    {
        get
        {
            var t = T;
            return t != null && t->Header.Size >= (uint)sizeof(TypedTable) ? (TypedTable*)t : null;
        }
    }

    /// <summary>The typed table, or <see cref="KilnUnavailableException"/> naming <c>hooks.typed</c>.</summary>
    public static TypedTable* RequireTyped(string api)
    {
        var t = Typed;
        if (t != null) return t;
        HooksCaps.Rows.Ensure();
        throw ServiceCaps.Unavailable(HooksCaps.Typed, T == null
            ? $"{api}: kiln.hooks is unavailable on this build"
            : $"{api}: this core's kiln.hooks has no typed links (older than 0.2.0-dev.28)");
    }

    /// <summary>The text of the last refused detour_typed on this thread ("" when none).</summary>
    public static string TypedRefusal(TypedTable* t)
    {
        nuint len = 0;
        t->TypedRefusal(null, 0, &len);  // asks for the length (BufferTooSmall)
        if (len == 0 || len > 64 * 1024) return "";
        var buf = new byte[(int)len + 1];
        fixed (byte* p = buf)
            if (t->TypedRefusal(p, (nuint)buf.Length, &len) != Ok) return "";
        return Encoding.UTF8.GetString(buf, 0, (int)len);
    }

    private static Table* _table;
    private static bool _looked;

    public static Table* T
    {
        get
        {
            if (_table != null || _looked) return _table;
            _table = Services.Get<Table>(Name, Version);
            _looked = true;
            return _table;
        }
    }

    /// <summary>Puts <paramref name="table"/> in place of the core's (null: look the core's up again). For unit tests
    /// that drive the managed side against a fake registry; nothing in Kiln calls it.</summary>
    internal static void UseForTests(Table* table)
    {
        _table = table;
        _looked = table != null;
    }

    public static Table* Require(string api)
    {
        var t = T;
        if (t == null)
        {
            HooksCaps.Rows.Ensure();
            throw ServiceCaps.Unavailable(HooksCaps.Service, $"{api}: kiln.hooks is unavailable on this build");
        }
        return t;
    }

    public static string Describe(int rc) => rc switch
    {
        Ok => "ok",
        InvalidArgument => "invalid argument",
        NotFound => "not found",
        Unavailable => "unavailable",
        WrongThread => "wrong thread",
        BufferTooSmall => "buffer too small",
        SigMismatch => "the descriptor does not fit the function's code",
        SigUnknown => "the function's code could not be followed far enough to check the descriptor",
        _ => $"error {rc}",
    };

    /// <summary>Runs <paramref name="f"/> with <paramref name="s"/> as a UTF-8 (ptr,len) the native side can read.</summary>
    public static T WithUtf8<T>(string s, Func<NativeStr, T> f)
    {
        var bytes = Encoding.UTF8.GetBytes(s);
        fixed (byte* p = bytes) return f(new NativeStr { Ptr = p, Len = (nuint)bytes.Length });
    }
}
