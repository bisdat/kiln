// SPDX-License-Identifier: MIT
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;

namespace Kiln.Hooks.Tests;

/// <summary>The typed members of the fake kiln.hooks table, with the native call state machine of
/// native/core/src/services/hooks_chain.cpp (kiln_hk_typed_dispatch): the frame's result is zeroed before the handler,
/// call_original_frame runs the original once and only for the frame of the handler in flight, and after a handler
/// that neither called it nor skipped, the original runs. "Game functions" are C# delegates on the frame.</summary>
internal static unsafe class FakeTyped
{
    /// <summary>A game function: reads its arguments from the frame and writes rax / xmm0r.</summary>
    public delegate void Function(HooksNative.CallFrame* f);

    public static readonly Dictionary<nuint, Function> Functions = new();
    public static readonly Dictionary<string, nuint> SymbolTable = new(StringComparer.Ordinal);
    public static SigDescriptor? ShippedSig;
    public static uint InstallReady = 1;
    public static int ValidationCalls;
    public static int ResolverResult;
    public static ulong ResolverRevision = 1;
    public static int Refuse;              // the result of the next detour_typed (0: it installs)
    public static int UncheckedFor;        // detour_typed_checked answers KILN_HOOKS_ERR_UNCHECKED this many times first
    public static int CheckedCalls;        // calls of detour_typed_checked (dev.29)
    public static int PlainCalls;          // calls of detour_typed (the unchecked member: managed code must not use it)
    public static string RefusalText = "";  // what typed_refusal says after a refusal
    public static bool RefuseCallOriginal;  // call_original_frame answers KILN_ERR_INVALID_ARGUMENT
    public static int OriginalCalls;
    public static uint LastNStack;
    public static SigSeen? LastSig;

    public sealed record SigSeen(uint Size, uint Argc, uint RetKind, uint RetSize, byte Sret, uint[] Kinds, uint[] Sizes,
        string[] Names, uint OrderSize, int Priority, string Label);

    [ThreadStatic] static string? _refusal;
    [ThreadStatic] static Call? _current;

    sealed class Call
    {
        public HooksNative.CallFrame* Frame;
        public bool Called;
        public Function? Original;
    }

    public static void Reset()
    {
        InstallReady = 1;
        ShippedSig = null;
        ValidationCalls = 0;
        ResolverResult = 0;
        ResolverRevision++;
        Functions.Clear();
        SymbolTable.Clear();
        Refuse = 0;
        UncheckedFor = 0;
        CheckedCalls = 0;
        PlainCalls = 0;
        RefusalText = "";
        RefuseCallOriginal = false;
        OriginalCalls = 0;
        LastNStack = 0;
        LastSig = null;
    }

    public static void Fill(HooksNative.TypedTable* t)
    {
        *(nint*)&t->V1.Symbol = (nint)(delegate* unmanaged[Cdecl]<FakeRegistry.NativeText, nuint*, int>)&Symbol;
        *(nint*)&t->DetourTyped = (nint)(delegate* unmanaged[Cdecl]<ulong, nuint, OrderText*, SigText*, nint, void*, ulong*, int>)&DetourTyped;
        t->CallOriginalFrame = &CallOriginalFrame;
        t->CallWithFrame = &CallWithFrame;
        t->TypedRefusal = &TypedRefusal;
        var c = (HooksNative.CheckedTable*)t;
        *(nint*)&c->ValidateTyped = (nint)(delegate* unmanaged[Cdecl]<FakeRegistry.NativeText, nuint, ulong, SigText*, int>)&Validate;
        *(nint*)&c->SigStatus = (nint)(delegate* unmanaged[Cdecl]<FakeRegistry.NativeText, nuint*, ulong*, int>)&Status;
        c->InstallReady = &ReadInstallReady;
        *(nint*)&c->DetourTypedChecked = (nint)(delegate* unmanaged[Cdecl]<ulong, nuint, OrderText*, SigText*, nint, void*, ulong*, int>)&DetourTypedChecked;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    static uint ReadInstallReady() => InstallReady;

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    static int Status(FakeRegistry.NativeText symbol, nuint* address, ulong* revision)
    {
        *revision = ResolverRevision;
        var name = Text(symbol);
        *address = SymbolTable.GetValueOrDefault(name);
        var slash = name.IndexOf('/');
        if (*address == 0 && slash > 0 && name.EndsWith("/Fn") && FakeRegistry.HasSymbols(name[..slash])) *address = 0x1234;
        return ResolverResult;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    static int Validate(FakeRegistry.NativeText symbol, nuint address, ulong revision, SigText* s)
    {
        ValidationCalls++;
        if (ShippedSig is {} d && (s->Argc != d.Args.Count || s->RetKind != (uint)d.Ret || s->RetSize != d.RetSize ||
            s->Sret != (d.Sret ? 1 : 0))) { _refusal = "compiled descriptor differs from resolver descriptor"; return HooksNative.SigMismatch; }
        if (ShippedSig is {} sig)
            for (var i = 0; i < s->Argc; i++)
                if (s->Args[i].Kind != (uint)sig.Args[i].Kind || s->Args[i].Size != sig.Args[i].Size)
                { _refusal = "compiled descriptor argument differs from resolver descriptor"; return HooksNative.SigMismatch; }
        return HooksNative.Ok;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct OrderText
    {
        public uint Size;
        public int Priority;
        public FakeRegistry.NativeText Label, Before, After;
        public uint Flags;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct ArgText
    {
        public FakeRegistry.NativeText Name;
        public uint Kind, Size;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct SigText
    {
        public uint Size, Argc;
        public ArgText* Args;
        public uint RetKind, RetSize;
        public byte Sret;
    }

    static string Text(FakeRegistry.NativeText t) => t.Ptr == null ? "" : Encoding.UTF8.GetString(t.Ptr, (int)t.Len);

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    static int Symbol(FakeRegistry.NativeText name, nuint* address)
    {
        if (!SymbolTable.TryGetValue(Text(name), out var a)) return HooksNative.NotFound;
        *address = a;
        return HooksNative.Ok;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    static int DetourTyped(ulong owner, nuint address, OrderText* order, SigText* sig, nint fn, void* user, ulong* handle)
    {
        PlainCalls++;
        return Install(owner, address, order, sig, fn, user, handle);
    }

    // detour_typed_checked: the descriptor first (Refuse), then the thread check of the first patch (UncheckedFor)
    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    static int DetourTypedChecked(ulong owner, nuint address, OrderText* order, SigText* sig, nint fn, void* user, ulong* handle)
    {
        CheckedCalls++;
        if (Refuse == 0 && UncheckedFor > 0)
        {
            UncheckedFor--;
            _refusal = "";
            return HooksNative.Unchecked;
        }
        return Install(owner, address, order, sig, fn, user, handle);
    }

    static int Install(ulong owner, nuint address, OrderText* order, SigText* sig, nint fn, void* user, ulong* handle)
    {
        var kinds = new uint[sig->Argc];
        var sizes = new uint[sig->Argc];
        var names = new string[sig->Argc];
        for (var i = 0; i < sig->Argc; i++)
        {
            kinds[i] = sig->Args[i].Kind;
            sizes[i] = sig->Args[i].Size;
            names[i] = Text(sig->Args[i].Name);
        }
        LastSig = new SigSeen(sig->Size, sig->Argc, sig->RetKind, sig->RetSize, sig->Sret, kinds, sizes, names, order->Size,
            order->Priority, Text(order->Label));
        if (Refuse != 0)
        {
            _refusal = RefusalText;
            return Refuse;
        }
        _refusal = "";
        return FakeRegistry.Add(HookCookies.Typed, owner, address, fn, (nint)user, handle);
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    static int TypedRefusal(byte* buf, nuint cap, nuint* len)
    {
        var bytes = Encoding.UTF8.GetBytes(_refusal ?? "");
        *len = (nuint)bytes.Length;
        if (buf == null || cap < (nuint)bytes.Length + 1) return HooksNative.BufferTooSmall;
        bytes.CopyTo(new Span<byte>(buf, bytes.Length));
        buf[bytes.Length] = 0;
        return HooksNative.Ok;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    static int CallOriginalFrame(HooksNative.CallFrame* frame)
    {
        var c = _current;
        if (RefuseCallOriginal || c is null || c.Frame != frame) return HooksNative.InvalidArgument;
        if (!c.Called) Run(c, frame);
        return HooksNative.Ok;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    static int CallWithFrame(nuint fn, HooksNative.CallFrame* frame, uint nstack)
    {
        LastNStack = nstack;
        if (!Functions.TryGetValue(fn, out var f)) return HooksNative.InvalidArgument;
        f(frame);
        return HooksNative.Ok;
    }

    static void Run(Call c, HooksNative.CallFrame* frame)
    {
        c.Called = true;
        Interlocked.Increment(ref OriginalCalls);
        c.Original?.Invoke(frame);
    }

    /// <summary>A game call through a typed link, as kiln_hk_typed and its dispatch make it: <paramref name="setup"/>
    /// fills the arguments (the stack arguments in <paramref name="stack"/>), the result fields start at zero.</summary>
    public static HooksNative.CallFrame CallTyped(FakeRegistry.Link link, Function? original, Function? setup = null, ulong[]? stack = null)
    {
        var frame = default(HooksNative.CallFrame);
        var args = stack ?? new ulong[16];
        fixed (ulong* s = args)
        {
            frame.Stack = s;
            setup?.Invoke(&frame);
            frame.Rax = 0;
            for (var i = 0; i < 16; i++) frame.Xmm0R[i] = 0;
            var c = new Call { Frame = &frame, Original = original };
            var outer = _current;
            _current = c;
            nuint verdict;
            try { verdict = ((delegate* unmanaged[Cdecl]<HooksNative.CallFrame*, void*, nuint>)link.Fn)(&frame, (void*)link.User); }
            finally { _current = outer; }
            if (!c.Called && verdict != HooksNative.TypedSkip) Run(c, &frame);
            frame.Stack = null;
            return frame;
        }
    }
}
