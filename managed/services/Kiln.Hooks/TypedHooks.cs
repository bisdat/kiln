// SPDX-License-Identifier: MIT
// Typed hooks and calls (since 0.2.0-dev.29, chapter 06b §9): the runtime the generated classes (Kiln.Sigs.<Area>.<Name>,
// and a mod's own from its sigdb fragment) are built on. The native half is kiln.hooks' typed link kind (kiln_hooks.h,
// "typed links"): a hooked call reaches a handler as one KilnCallFrame (rcx rdx r8 r9, xmm0-3, the caller's stack
// arguments, rax and xmm0 for the result). A mod never uses these types directly: the generator emits a class per
// descriptor whose Call, Detour and Invoke wrap them with the descriptor's names, types and masks.
using System.ComponentModel;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace Kiln;

/// <summary>The outcome of installing a typed hook. <see cref="Ok"/> is false when the game refused it: the
/// descriptor does not fit the function's code on this game build (the reason names the argument), the symbol does not
/// resolve on this build, or kiln.hooks has no room for another hooked address. A programming error (a null handler, a
/// malformed descriptor) throws <see cref="ArgumentException"/> instead, and a core without typed links throws
/// <see cref="KilnUnavailableException"/> naming <c>hooks.typed</c>.</summary>
/// <param name="Ok">True when installed or accepted pending; inspect Pending and Handle.State for completion.</param>
/// <param name="Reason">Why it was refused; null when <paramref name="Ok"/>.</param>
/// <param name="Handle">Removes the hook (it also goes when the mod unloads); null when refused.</param>
public sealed record HookResult(bool Ok, string? Reason, HookHandle? Handle) : IKilnResult
{
    /// <summary>The request was accepted and will install at the next safe tick. Observe Handle.State for completion.</summary>
    public bool Pending => Handle?.State == HookRegistrationState.Pending;
}

/// <summary>A typed hook was refused (<see cref="HookResult.Ok"/> false): a game update changed the function, or its
/// symbol no longer resolves. Posted on the tick thread (queued for the next step when the hook was asked for on
/// another thread), only while something subscribes. For status pages and diagnostics mods; nothing to cancel.</summary>
public sealed class HookRefusedEvent : KilnEvent
{
    internal HookRefusedEvent(string modId, string symbol, string reason)
    {
        ModId = modId;
        Symbol = symbol;
        Reason = reason;
    }

    /// <summary>The mod that asked for the hook.</summary>
    public string ModId { get; }

    /// <summary>The sigdb symbol it was asked for.</summary>
    public string Symbol { get; }

    /// <summary>Why it was refused (the same text as <see cref="HookResult.Reason"/>).</summary>
    public string Reason { get; }

    public override string ToString() => $"typed hook on {Symbol} for {ModId} refused: {Reason}";
}

/// <summary>The type of an argument or return of a typed descriptor (KILN_SIG_* in kiln_hooks.h).</summary>
[EditorBrowsable(EditorBrowsableState.Never)]
public enum SigKind : uint
{
    Void = 0, S8 = 1, U8 = 2, S16 = 3, U16 = 4, S32 = 5, U32 = 6, S64 = 7, U64 = 8, F32 = 9, F64 = 10, Bool = 11,
    Ptr = 12,
    /// <summary>A struct by value: 1, 2, 4 or 8 bytes travel in the integer position, any other size as a pointer to
    /// the caller's copy.</summary>
    ByVal = 13,
    /// <summary>Return only: a struct, through the hidden buffer (sret) or, at 1/2/4/8 bytes, in rax.</summary>
    Struct = 14,
}

/// <summary>Whether a descriptor offers a direct call (sigdb <c>"invoke"</c>).</summary>
[EditorBrowsable(EditorBrowsableState.Never)]
public enum SigInvoke
{
    /// <summary>Hook only.</summary>
    None = 0,
    /// <summary>A direct call on the tick thread only.</summary>
    Tick = 1,
    /// <summary>A direct call from any thread (pure functions).</summary>
    Any = 2,
}

/// <summary>One declared parameter of a descriptor.</summary>
[EditorBrowsable(EditorBrowsableState.Never)]
public readonly record struct SigArg(string Name, SigKind Kind, uint Size);

/// <summary>A typed descriptor as the generated class carries it: the declared parameters (never the hidden return
/// buffer), the return, <c>sret</c> and <c>invoke</c>. Validated when it is made, by the rules kiln.hooks applies
/// (detour_typed refuses the same shapes as malformed).</summary>
[EditorBrowsable(EditorBrowsableState.Never)]
public sealed class SigDescriptor
{
    /// <summary>A descriptor of <paramref name="symbol"/>.</summary>
    /// <exception cref="ArgumentException">An empty symbol; a return kind that is no return (<see cref="SigKind.ByVal"/>);
    /// an argument kind that is no argument (<see cref="SigKind.Void"/>, <see cref="SigKind.Struct"/>) or a by-value
    /// struct without its size; sret without a sized struct return, or a struct return other than 1/2/4/8 bytes without
    /// sret; more than 20 positions.</exception>
    public SigDescriptor(string symbol, SigKind ret, uint retSize, bool sret, SigInvoke invoke, params SigArg[] args)
    {
        if (string.IsNullOrEmpty(symbol)) throw new ArgumentException("a descriptor needs its symbol", nameof(symbol));
        ArgumentNullException.ThrowIfNull(args);
        if (ret is SigKind.ByVal || (uint)ret > (uint)SigKind.Struct)
            throw new ArgumentException($"{symbol}: {ret} is not a return kind", nameof(ret));
        if (sret && (ret != SigKind.Struct || retSize == 0))
            throw new ArgumentException($"{symbol}: sret needs a struct return with its size", nameof(sret));
        if (!sret && ret == SigKind.Struct && retSize is not (1 or 2 or 4 or 8))
            throw new ArgumentException($"{symbol}: a struct return of {retSize} bytes goes through a hidden buffer: it needs sret", nameof(sret));
        if (invoke is not (SigInvoke.None or SigInvoke.Tick or SigInvoke.Any))
            throw new ArgumentException($"{symbol}: {(int)invoke} is not an invoke kind", nameof(invoke));
        foreach (var a in args)
            if (a.Kind is SigKind.Void or SigKind.Struct || (uint)a.Kind > (uint)SigKind.Struct || (a.Kind == SigKind.ByVal && a.Size == 0))
                throw new ArgumentException($"{symbol}: argument '{a.Name}' has kind {a.Kind}", nameof(args));
        Symbol = symbol;
        Ret = ret;
        RetSize = retSize;
        Sret = sret;
        Invoke = invoke;
        Args = Array.AsReadOnly((SigArg[])args.Clone());
        if (Positions > HooksNative.MaxPositions)
            throw new ArgumentException($"{symbol}: {Positions} positions, a typed link forwards at most {HooksNative.MaxPositions}", nameof(args));
    }

    /// <summary>The sigdb symbol.</summary>
    public string Symbol { get; }
    /// <summary>The return kind.</summary>
    public SigKind Ret { get; }
    /// <summary>The return's size in bytes (a struct's; 0 for void).</summary>
    public uint RetSize { get; }
    /// <summary>The function returns a struct through a hidden first pointer.</summary>
    public bool Sret { get; }
    /// <summary>Whether a direct call is offered.</summary>
    public SigInvoke Invoke { get; }
    /// <summary>The declared parameters, in order.</summary>
    public IReadOnlyList<SigArg> Args { get; }

    /// <summary>Call positions: the declared parameters plus the hidden buffer.</summary>
    public int Positions => Args.Count + (Sret ? 1 : 0);

    /// <summary>Positions on the stack (beyond the fourth).</summary>
    public int StackCount => Math.Max(0, Positions - 4);
}

/// <summary>A handler as the generated <c>Detour</c> installs it.</summary>
[EditorBrowsable(EditorBrowsableState.Never)]
public delegate void SigHandler(SigCall call);

// The managed half of the call state machine (§9.8), on the dispatcher's stack for one handler's run: the once-only
// latch of CallOriginal, Skip, and what the original returned (kept so a handler that throws afterwards cannot change
// what the caller gets).
[StructLayout(LayoutKind.Sequential)]
internal unsafe struct SigState
{
    public const int SnapshotMax = 64;
    public int Called, Skip;
    public ulong Rax;
    public fixed byte Xmm0[16];
    public fixed byte Buffer[SnapshotMax];  // inline snapshot for the core descriptors
    public byte* LargeBuffer;              // larger fragment structs; released when dispatch leaves
}

/// <summary>A typed call in flight, as the generated <c>Call</c> wraps it. Positions count the hidden return buffer:
/// a function with sret has it at position 0 and its first declared parameter at 1. Valid during the handler only
/// (it is a ref struct: it cannot be kept).</summary>
[EditorBrowsable(EditorBrowsableState.Never)]
public readonly unsafe ref struct SigCall
{
    private readonly HooksNative.CallFrame* _f;
    private readonly SigState* _st;
    private readonly SigDescriptor _sig;

    internal SigCall(HooksNative.CallFrame* frame, SigState* state, SigDescriptor sig)
    {
        _f = frame;
        _st = state;
        _sig = sig;
    }

    // Position p of the integer class: gpr[p] (p < 4) or the caller's stack slot p - 4.
    private ulong* IntAt(int p) => (uint)p < 4 ? &_f->Gpr[0] + p : CheckedStack(p);
    // Position p of the float class: the low bytes of xmm[p] (p < 4) or the stack slot.
    private byte* FloatAt(int p) => (uint)p < 4 ? &_f->Xmm[0] + 16 * p : (byte*)CheckedStack(p);

    private ulong* CheckedStack(int p)
    {
        if (p < 4 || p >= _sig.Positions) throw new ArgumentOutOfRangeException(nameof(p), $"{_sig.Symbol} has {_sig.Positions} positions");
        return _f->Stack + (p - 4);
    }

    /// <summary>The 8-byte integer slot of position <paramref name="p"/> (upper bytes of a narrow type are garbage).</summary>
    public ulong Int(int p) => *IntAt(p);
    /// <summary>Writes the 8-byte integer slot of position <paramref name="p"/>.</summary>
    public void SetInt(int p, ulong value) => *IntAt(p) = value;
    /// <summary>A float position (the low 4 bytes of its xmm register or stack slot).</summary>
    public float F32(int p) => *(float*)FloatAt(p);
    /// <summary>Writes a float position.</summary>
    public void SetF32(int p, float value) => *(float*)FloatAt(p) = value;
    /// <summary>A double position.</summary>
    public double F64(int p) => *(double*)FloatAt(p);
    /// <summary>Writes a double position.</summary>
    public void SetF64(int p, double value) => *(double*)FloatAt(p) = value;

    /// <summary>The address of position <paramref name="p"/>'s integer slot (a by-value struct of 1/2/4/8 bytes lives in it).</summary>
    public void* SlotAddress(int p) => IntAt(p);

    /// <summary>The integer / pointer return (rax).</summary>
    public ulong RetInt => _f->Rax;
    /// <summary>Sets rax.</summary>
    public void SetRetInt(ulong value) => _f->Rax = value;
    /// <summary>The float return (the low 4 bytes of xmm0).</summary>
    public float RetF32 => *(float*)_f->Xmm0R;
    /// <summary>Sets the float return.</summary>
    public void SetRetF32(float value) => *(float*)_f->Xmm0R = value;
    /// <summary>The double return.</summary>
    public double RetF64 => *(double*)_f->Xmm0R;
    /// <summary>Sets the double return.</summary>
    public void SetRetF64(double value) => *(double*)_f->Xmm0R = value;

    /// <summary>The address of rax (a struct of 1/2/4/8 bytes returned by value lives in it).</summary>
    public void* RetAddress => &_f->Rax;

    /// <summary>The caller's return buffer (sret: position 0), valid during the call.</summary>
    /// <exception cref="InvalidOperationException">The descriptor has no sret.</exception>
    public void* ReturnBuffer => _sig.Sret ? (void*)_f->Gpr[0] : throw new InvalidOperationException($"{_sig.Symbol} returns no struct through a buffer");

    /// <summary>Runs the rest of the hook chain and the game's function once, with the arguments as they are now, and
    /// stores its result. A second call does nothing (the stored result stays).</summary>
    /// <exception cref="InvalidOperationException">kiln.hooks refused it (the call is not a handler's call in
    /// flight); the original has not run then.</exception>
    public void CallOriginal()
    {
        if (_st->Called != 0) return;
        if (_sig.Sret && _sig.RetSize > SigState.SnapshotMax && _st->LargeBuffer == null)
            _st->LargeBuffer = (byte*)NativeMemory.Alloc(_sig.RetSize);
        var t = HooksNative.Typed;
        if (t == null || t->CallOriginalFrame(_f) != HooksNative.Ok)
            throw new InvalidOperationException($"{_sig.Symbol}: kiln.hooks refused CallOriginal (the call is no longer in flight)");
        // Latched only once the native call ran: a refused call leaves OriginalCalled false, so the fault rule runs the
        // original instead of handing the caller a result nobody computed.
        _st->Called = 1;
        _st->Rax = _f->Rax;
        Unsafe.CopyBlockUnaligned(_st->Xmm0, _f->Xmm0R, 16);
        if (_sig.Sret && _f->Gpr[0] != 0)
        {
            Unsafe.CopyBlockUnaligned(_st->LargeBuffer != null ? _st->LargeBuffer : _st->Buffer,
                (void*)_f->Gpr[0], _sig.RetSize);
        }
    }

    /// <summary>Whether <see cref="CallOriginal"/> ran.</summary>
    public bool OriginalCalled => _st->Called != 0;

    /// <summary>Ends the call without the game's function: the result is what the handler set (zero otherwise; an
    /// sret buffer as the handler filled it). After <see cref="CallOriginal"/> it changes nothing.</summary>
    public void Skip() => _st->Skip = 1;
}

/// <summary>A call frame for a typed direct call (the generated <c>Invoke</c>): the four integer and four float
/// registers and up to 16 stack positions, then rax and xmm0 for the result.</summary>
[EditorBrowsable(EditorBrowsableState.Never)]
public unsafe struct SigFrame
{
    internal HooksNative.CallFrame Frame;
    internal fixed ulong StackArgs[16];

    /// <summary>Sets integer position <paramref name="p"/>.</summary>
    public void SetInt(int p, ulong value)
    {
        if ((uint)p < 4) Frame.Gpr[p] = value;
        else StackArgs[Stack(p)] = value;
    }

    /// <summary>Sets float position <paramref name="p"/>.</summary>
    public void SetF32(int p, float value)
    {
        if ((uint)p < 4) fixed (byte* x = Frame.Xmm) *(float*)(x + 16 * p) = value;
        else StackArgs[Stack(p)] = BitConverter.SingleToUInt32Bits(value);
    }

    /// <summary>Sets double position <paramref name="p"/>.</summary>
    public void SetF64(int p, double value)
    {
        if ((uint)p < 4) fixed (byte* x = Frame.Xmm) *(double*)(x + 16 * p) = value;
        else StackArgs[Stack(p)] = BitConverter.DoubleToUInt64Bits(value);
    }

    /// <summary>rax after the call.</summary>
    public readonly ulong RetInt => Frame.Rax;
    /// <summary>The low 4 bytes of xmm0 after the call.</summary>
    public readonly float RetF32 { get { fixed (byte* x = Frame.Xmm0R) return *(float*)x; } }
    /// <summary>The low 8 bytes of xmm0 after the call.</summary>
    public readonly double RetF64 { get { fixed (byte* x = Frame.Xmm0R) return *(double*)x; } }

    private static int Stack(int p) =>
        p >= 4 && p < HooksNative.MaxPositions ? p - 4 : throw new ArgumentOutOfRangeException(nameof(p));
}

/// <summary>What the generated classes call: install a typed hook, call a described function. Mods use the generated
/// <c>Kiln.Sigs.&lt;Area&gt;.&lt;Name&gt;</c> classes (and their own, generated from their sigdb fragment) instead.</summary>
[EditorBrowsable(EditorBrowsableState.Never)]
public static unsafe class TypedHooks
{
    private sealed class Validation
    {
        public readonly object Gate = new();
        public nuint Table, Address;
        public ulong Revision;
        public int Rc;
        public string Reason = "";
        public bool HasValue;
    }
    private static readonly ConditionalWeakTable<SigDescriptor, Validation> Validations = new();

    // Whether a described symbol's row is on (seam: unit tests replace it). Core symbols have a kiln.caps row
    // "sig/<symbol>"; a mod's own fragment has none, so its status is queried from the current native resolver.
    internal static Func<string, bool> SigRowOn = DefaultSigRowOn;

    // The tick guard of a "tick" Invoke (seam: unit tests run on no tick thread of a game).
    internal static Action<string> RequireTick = Services.RequireTickThread;

    internal static bool DefaultSigRowOn(string symbol)
    {
        var row = HooksCaps.Sig(symbol);
        if (Capabilities.Is(row)) return true;
        return Capabilities.Get(row).Reason == CapabilityReason.Unknown && Symbols.FragmentSigMatches(symbol);
    }

    /// <summary>Installs a typed hook on <paramref name="sig"/>'s symbol. Advanced mods only; owner-scoped (removed
    /// when the mod unloads); the handler runs on the thread that calls the function (worker threads too).</summary>
    /// <exception cref="ArgumentNullException">A null descriptor or handler.</exception>
    /// <exception cref="ArgumentException">kiln.hooks says the descriptor is malformed.</exception>
    /// <exception cref="KilnUnavailableException"><c>hooks.typed</c> is off (no core, or a core without typed links).</exception>
    /// <exception cref="InvalidOperationException">The calling mod is not advanced, or has unloaded.</exception>
    [AdvancedApi, AnyThread, RequiresCapability(HooksCaps.Typed)]
    public static HookResult Detour(SigDescriptor sig, SigHandler handler, int priority = 0)
    {
        ArgumentNullException.ThrowIfNull(sig);
        ArgumentNullException.ThrowIfNull(handler);
        var api = sig.Symbol + ".Detour";
        var t = HooksNative.RequireTyped(api);
        var mod = HooksGate.RequireAdvanced(api);
        if (*(uint*)t < 288 || ((HooksNative.CheckedTable*)t)->InstallReady == null)
            return Refused(mod, sig.Symbol, "this core has no safe typed installation timing query");
        if (!CanInstallNow())
        {
            lock (HooksGate.Lifecycle)
            {
                if (!mod.Alive) throw HooksGate.Retired(api, mod);
                var pending = HookHandle.NewPending();
                HooksGate.Track(mod);
                mod.Retiring += pending.Remove;
                void InstallPending()
                {
                    if (!mod.Alive || pending.State == HookRegistrationState.Removed) return;
                    if (!CanInstallNow()) { mod.Scheduler.Post(InstallPending); return; }
                    try { pending.Complete(InstallNow(sig, handler, priority, mod)); }
                    catch (Exception e) { pending.Complete(Refused(mod, sig.Symbol, e.Message)); }
                }
                mod.Scheduler.Post(InstallPending);
                // Retirement can take its cleanup list before Track attaches the first callback.
                if (!mod.Alive)
                {
                    pending.Remove();
                    HooksGate.Untrack(mod);
                    throw HooksGate.Retired(api, mod);
                }
                return new HookResult(true, null, pending);
            }
        }
        return InstallNow(sig, handler, priority, mod);
    }

    private static bool CanInstallNow()
    {
        var t = HooksNative.T;
        return t != null && *(uint*)t >= 288 && ((HooksNative.CheckedTable*)t)->InstallReady != null &&
            ((HooksNative.CheckedTable*)t)->InstallReady() != 0;
    }

    private static HookResult InstallNow(SigDescriptor sig, SigHandler handler, int priority, ModContext mod)
    {
        var api = sig.Symbol + ".Detour";
        var t = HooksNative.RequireTyped(api);
        nuint address = 0;
        if (HooksNative.WithUtf8(sig.Symbol, s => { nuint a; var r = t->V1.Symbol(s, &a); address = a; return r; }) != HooksNative.Ok || address == 0)
            return Refused(mod, sig.Symbol, $"symbol {sig.Symbol} does not resolve on this game build");
        var refusal = "";
        var (handle, rc) = Hooks.TryInstall(api, mod, HookCookies.Typed, address,
            (cookie, inFlight, install) => new Hooks.TypedReg(handler, sig, mod, inFlight, cookie, install),
            (owner, cookie, native) =>
            {
                var r = InstallNative(t, owner, address, sig, priority, cookie, native);
                if (r is HooksNative.SigMismatch or HooksNative.SigUnknown) refusal = HooksNative.TypedRefusal(t);  // same thread, right after
                return r;
            });
        if (handle != null)
        {
            mod.Log.Info($"hooks: typed hook on {sig.Symbol} (priority {priority})");
            return new HookResult(true, null, handle);
        }
        return rc switch
        {
            HooksNative.InvalidArgument => throw new ArgumentException($"{api}: kiln.hooks refused the descriptor as malformed (see kiln.log)", nameof(sig)),
            HooksNative.SigMismatch or HooksNative.SigUnknown => Refused(mod, sig.Symbol,
                refusal.Length > 0 ? refusal : HooksNative.Describe(rc)),
            HooksNative.Unavailable => Refused(mod, sig.Symbol, "kiln.hooks cannot hook another address (its target cap is reached)"),
            _ => Refused(mod, sig.Symbol, $"kiln.hooks refused the hook: {HooksNative.Describe(rc)}"),
        };
    }

    private static int ValidateNative(HooksNative.TypedTable* t, SigDescriptor sig, nuint address, ulong revision) =>
        InstallNative(t, 0, address, sig, 0, 0, null, revision);

    private static int InstallNative(HooksNative.TypedTable* t, ulong owner, nuint address, SigDescriptor sig, int priority,
        long cookie, ulong* native, ulong revision = 0)
    {
        var n = sig.Args.Count;
        var names = new byte[n][];
        var handles = new GCHandle[n];
        var args = stackalloc HooksNative.SigArgRaw[Math.Max(n, 1)];
        var label = System.Text.Encoding.UTF8.GetBytes(sig.Symbol);
        try
        {
            for (var i = 0; i < n; i++)
            {
                names[i] = System.Text.Encoding.UTF8.GetBytes(sig.Args[i].Name ?? "");
                handles[i] = GCHandle.Alloc(names[i], GCHandleType.Pinned);
                args[i].Name = new NativeStr { Ptr = (byte*)handles[i].AddrOfPinnedObject(), Len = (nuint)names[i].Length };
                args[i].Kind = (uint)sig.Args[i].Kind;
                args[i].Size = sig.Args[i].Size;
            }
            HooksNative.SigRaw raw = default;
            raw.Size = (uint)sizeof(HooksNative.SigRaw);
            raw.Argc = (uint)n;
            raw.Args = args;
            raw.RetKind = (uint)sig.Ret;
            raw.RetSize = sig.RetSize;
            raw.Sret = sig.Sret ? (byte)1 : (byte)0;
            fixed (byte* l = label)
            {
                HooksNative.OrderRaw order = default;
                order.Size = (uint)sizeof(HooksNative.OrderRaw);
                order.Priority = priority;
                order.Label = new NativeStr { Ptr = l, Len = (nuint)label.Length };
                if (native == null)
                    return HooksNative.Checked->ValidateTyped(order.Label, address, revision, &raw);
                // kiln.hooks copies the label and reads the descriptor during the call only: the pins end here
                return InstallAtSafePoint(t, owner, address, &order, &raw, cookie, native);
            }
        }
        finally
        {
            foreach (var h in handles)
                if (h.IsAllocated) h.Free();
        }
    }

    // Integration seam for package 07: its mid2_checked only accepts a KilnMidFn and CANNOT install a typed link.
    // Coordinator: expose the same checked target policy as detour_typed_checked, then switch ONLY this call to it,
    // with a header.size check. Do not substitute mid2_checked: the handler ABI is different.
    internal static int InstallAtSafePoint(HooksNative.TypedTable* t, ulong owner, nuint address,
        HooksNative.OrderRaw* order, HooksNative.SigRaw* sig, long cookie, ulong* native) =>
        t->DetourTyped(owner, address, order, sig, &Dispatch, (void*)(nint)cookie, native);

    private static HookResult Refused(ModContext mod, string symbol, string reason)
    {
        mod.Log.Warn($"hooks: typed hook on {symbol} refused: {reason}");
        try
        {
            if (Events.HasSubscribers<HookRefusedEvent>())
            {
                var ev = new HookRefusedEvent(mod.Info.Id, symbol, reason);
                if (Game.IsTickThread) Events.Post(ev);
                else Events.Enqueue(ev);
            }
        }
        catch (Exception)
        {
            // a full queue, no tick bus here, a subscriber's own fault: the result and the log line still say it
        }
        return new HookResult(false, reason, null);
    }

    /// <summary>Calls <paramref name="sig"/>'s function with <paramref name="frame"/> (through its hook chain, as the
    /// game's own callers do) and leaves the result in it. The generated <c>Invoke</c> fills the frame and reads the
    /// result.</summary>
    /// <exception cref="KilnUnavailableException"><c>hooks.typed</c> is off, or the descriptor's row
    /// <c>sig/&lt;symbol&gt;</c> is (the function changed on this build, or its symbol does not resolve).</exception>
    /// <exception cref="InvalidOperationException">A <c>tick</c> descriptor off the tick thread (a worker, a hook
    /// handler on one), a mod that is not advanced, or a descriptor without <c>invoke</c>.</exception>
    [AdvancedApi, AnyThread, RequiresCapability(HooksCaps.Typed)]
    public static void Invoke(SigDescriptor sig, ref SigFrame frame)
    {
        ArgumentNullException.ThrowIfNull(sig);
        var api = sig.Symbol + ".Invoke";
        if (sig.Invoke == SigInvoke.None) throw new InvalidOperationException($"{api}: the descriptor offers no direct call (invoke: none)");
        var t = HooksNative.RequireTyped(api);
        HooksGate.RequireAdvanced(api, "Calling game functions directly is powerful and build-specific");
        if (sig.Invoke == SigInvoke.Tick) RequireTick(api);
        var row = HooksCaps.Sig(sig.Symbol);
        // never a call into a function whose code does not fit the descriptor (§9.4): its row is off then
        if (!SigRowOn(sig.Symbol)) throw ServiceCaps.Unavailable(row, $"{api}: the function does not fit its descriptor on this game build, or its symbol does not resolve");
        var checkedTable = HooksNative.Checked;
        if (checkedTable == null) throw ServiceCaps.Unavailable(row, $"{api}: this core has no typed descriptor validation; refusing an unchecked call");
        nuint address = 0;
        ulong revision = 0;
        var status = HooksNative.WithUtf8(sig.Symbol, s =>
        {
            nuint a; ulong r;
            var rc = checkedTable->SigStatus(s, &a, &r);
            address = a; revision = r;
            return rc;
        });
        if (status != HooksNative.Ok || address == 0)
            throw ServiceCaps.Unavailable(row, $"{api}: {HooksNative.TypedRefusal(t)} (current resolver descriptor is unavailable)");
        var cached = Validations.GetValue(sig, _ => new Validation());
        lock (cached.Gate)
        {
            if (!cached.HasValue || cached.Table != (nuint)t || cached.Address != address || cached.Revision != revision)
            {
                cached.Rc = ValidateNative(t, sig, address, revision);
                cached.Reason = cached.Rc == HooksNative.Ok ? "" : HooksNative.TypedRefusal(t);
                cached.Table = (nuint)t; cached.Address = address; cached.Revision = revision; cached.HasValue = true;
            }
            if (cached.Rc == HooksNative.InvalidArgument) throw new ArgumentException($"{api}: malformed descriptor", nameof(sig));
            if (cached.Rc != HooksNative.Ok) throw ServiceCaps.Unavailable(row, $"{api}: {cached.Reason}");
        }
        fixed (SigFrame* f = &frame)
        {
            f->Frame.Stack = f->StackArgs;
            f->Frame.Rax = 0;
            var rc = t->CallWithFrame(address, &f->Frame, (uint)sig.StackCount);
            f->Frame.Stack = null;  // pointed into the pinned frame: never kept
            if (rc != HooksNative.Ok) throw new InvalidOperationException($"{api}: kiln.hooks refused the call: {HooksNative.Describe(rc)}");
        }
    }

    // ---- the dispatcher of every managed typed link (default load context: a stable function pointer) ----
    // Guarded whole: a managed exception never unwinds into the game's frames. Fail open: a removed registration, a
    // retired mod or a handler that throws lets the game's function run once and the caller get its result (the native
    // state machine runs the original when the handler neither called it nor skipped).
    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static nuint Dispatch(HooksNative.CallFrame* frame, void* user)
    {
        Hooks.TypedReg? reg = null;
        SigState st = default;
        try
        {
            if (!Hooks.Typed.TryGetValue((long)user, out reg)) return HooksNative.TypedContinue;  // removed: the original runs
            var box = reg.InFlight;
            Interlocked.Increment(ref box.Value);
            try
            {
                // Retirement drops the registration and then waits for the mod's in-flight count. Look again after
                // counting this call: a registration dropped in between is not run, so no handler starts after its
                // mod's retire stopped waiting.
                if (!Hooks.Typed.TryGetValue((long)user, out var again) || !ReferenceEquals(again, reg) || !reg.Mod.Alive)
                    return HooksNative.TypedContinue;
                var outer = ModContext.CurrentMod;
                ModContext.CurrentMod = reg.Mod;  // as DetourDispatch: gated calls inside the handler run as its mod
                try { reg.Handler(new SigCall(frame, &st, reg.Sig)); }
                finally { ModContext.CurrentMod = outer; }
            }
            finally
            {
                Interlocked.Decrement(ref box.Value);
            }
            if (st.Skip != 0 && st.Called == 0)
            {
                if (reg.Sig.Sret) frame->Rax = frame->Gpr[0];  // a function with sret returns its buffer
                return HooksNative.TypedSkip;
            }
            return HooksNative.TypedContinue;
        }
        catch (Exception e)
        {
            // As the native fault rule: when the original ran, the caller gets what it returned (whatever the handler
            // wrote since); else it runs with the arguments as the handler left them (Continue).
            Hooks.ReportFault(reg, "typed hook", e);
            if (st.Called != 0)
            {
                frame->Rax = st.Rax;
                Unsafe.CopyBlockUnaligned(frame->Xmm0R, st.Xmm0, 16);
                if (reg is { Sig.Sret: true } && frame->Gpr[0] != 0 &&
                    (reg.Sig.RetSize <= SigState.SnapshotMax || st.LargeBuffer != null))
                    Unsafe.CopyBlockUnaligned((void*)frame->Gpr[0],
                        st.LargeBuffer != null ? st.LargeBuffer : st.Buffer, reg.Sig.RetSize);
            }
            return HooksNative.TypedContinue;
        }
        finally
        {
            NativeMemory.Free(st.LargeBuffer);
        }
    }
}
