// SPDX-License-Identifier: MIT
extern alias khooks;
using System.Runtime.CompilerServices;
using Xunit;
using HooksApi = khooks::Kiln.Hooks;

namespace Kiln.Hooks.Tests;

/// <summary>The managed half of typed hooks (chapter 06b §9.4, §9.8): <see cref="TypedHooks.Detour"/> and the
/// dispatcher of every managed typed link, driven through <see cref="FakeTyped"/> (the native call state machine) with
/// the real <c>[UnmanagedCallersOnly]</c> dispatcher: the once-only latch of CallOriginal, Skip, the fall-through, the
/// fault rule, retirement, refusals as results, and <see cref="TypedHooks.Invoke"/>'s preconditions.</summary>
[Collection("kiln.hooks state")]
public sealed unsafe class TypedHookTests : IDisposable
{
    static int _seq;
    readonly string _id = $"typed-mod-{Interlocked.Increment(ref _seq)}";
    static readonly nuint Address = unchecked((nuint)0x140003000UL);
    readonly Func<string, ulong> _createOwner = HookCookies.CreateOwner;
    readonly Func<string, bool> _rowOn = TypedHooks.SigRowOn;
    readonly Action<string> _tick = TypedHooks.RequireTick;

    // void SetTimeScale(Daytime*, float scale /*xmm1*/, s64 now /*r8*/)
    static readonly SigDescriptor TimeScale = new("test/SetTimeScale", SigKind.Void, 0, false, SigInvoke.Tick,
        new SigArg("daytime", SigKind.Ptr, 8), new SigArg("scale", SigKind.F32, 4), new SigArg("ecsNow", SigKind.S64, 8));
    // OptS32* GetItemVolume(OptS32* out /*sret*/, const ItemInfo*)
    static readonly SigDescriptor Volume = new("test/GetItemVolume", SigKind.Struct, 8, true, SigInvoke.Any,
        new SigArg("item", SigKind.Ptr, 8));
    // s64 GetGameTime(Daytime*, s64)
    static readonly SigDescriptor GameTime = new("test/GetGameTime", SigKind.S64, 8, false, SigInvoke.Tick,
        new SigArg("daytime", SigKind.Ptr, 8), new SigArg("ecsNow", SigKind.S64, 8));
    // float Six(s64 a, float b, double c, s64 d, float e /*stack 0*/, u8 f /*stack 1*/)
    static readonly SigDescriptor Six = new("test/Six", SigKind.F32, 4, false, SigInvoke.Any,
        new SigArg("a", SigKind.S64, 8), new SigArg("b", SigKind.F32, 4), new SigArg("c", SigKind.F64, 8),
        new SigArg("d", SigKind.S64, 8), new SigArg("e", SigKind.F32, 4), new SigArg("f", SigKind.U8, 1));

    public TypedHookTests()
    {
        FakeRegistry.Install(typed: true);
        HookCookies.CreateOwner = _ => 0x7000;
        foreach (var d in new[] { TimeScale, Volume, GameTime, Six }) FakeTyped.SymbolTable[d.Symbol] = Address;
    }

    public void Dispose()
    {
        HookCookies.CreateOwner = _createOwner;
        TypedHooks.SigRowOn = _rowOn;
        TypedHooks.RequireTick = _tick;
        FakeRegistry.Uninstall();
    }

    ModContext Mod(bool advanced = true) => TestMods.Make(_id, advanced, owner: 70);

    static FakeRegistry.Link TheLink() => Assert.Single(FakeRegistry.Live, l => l.Kind == HookCookies.Typed);

    // the original of Six: a + b + c + d + e + f, in xmm0
    static void SixOriginal(HooksNative.CallFrame* f)
    {
        var sum = (long)f->Gpr[0] + *(float*)&f->Xmm[16] + *(double*)&f->Xmm[32] + (long)f->Gpr[3] + *(float*)&f->Stack[0] + (byte)f->Stack[1];
        *(float*)f->Xmm0R = (float)sum;
    }

    static void SixArgs(HooksNative.CallFrame* f)
    {
        f->Gpr[0] = 7;
        *(float*)&f->Xmm[16] = 1.5f;
        *(double*)&f->Xmm[32] = 2.25;
        f->Gpr[3] = 40;
        *(float*)&f->Stack[0] = 0.5f;
        f->Stack[1] = 0x11e8f3300UL + 100;  // a u8 on the stack: garbage above the low byte (lab T1)
    }

    [Fact]
    public void TheOriginalRunsOnceHoweverOftenTheHandlerCallsIt()
    {
        var mod = Mod();
        bool before = true, after = false;
        var r = TestMods.As(mod, () => TypedHooks.Detour(Six, c =>
        {
            before = c.OriginalCalled;
            c.CallOriginal();
            c.CallOriginal();
            after = c.OriginalCalled;
        }));
        Assert.True(r.Ok, r.Reason);
        var f = FakeTyped.CallTyped(TheLink(), SixOriginal, SixArgs);
        Assert.Equal(1, FakeTyped.OriginalCalls);
        Assert.False(before);
        Assert.True(after);
        Assert.Equal(7 + 1.5f + 2.25f + 40 + 0.5f + 100, *(float*)f.Xmm0R);
    }

    [Fact]
    public void ANarrowStackArgumentIsReadThroughItsMask()
    {
        var mod = Mod();
        ulong slot = 0;
        TestMods.As(mod, () => TypedHooks.Detour(Six, c => slot = c.Int(5)));
        FakeTyped.CallTyped(TheLink(), SixOriginal, SixArgs);
        Assert.Equal(0x11e8f3300UL + 100, slot);   // the raw slot keeps the garbage: generated code masks it (byte)
        Assert.Equal((byte)100, unchecked((byte)slot));
    }

    [Fact]
    public void SkipEndsTheCallWithoutTheOriginal()
    {
        var mod = Mod();
        TestMods.As(mod, () => TypedHooks.Detour(GameTime, c =>
        {
            c.SetRetInt(5);
            c.Skip();
        }));
        var f = FakeTyped.CallTyped(TheLink(), f => f->Rax = 99);
        Assert.Equal(0, FakeTyped.OriginalCalls);
        Assert.Equal(5UL, f.Rax);
    }

    [Fact]
    public void SkipWithAReturnBufferReturnsTheBufferTheHandlerFilled()
    {
        var mod = Mod();
        var buffer = new long[1];
        TestMods.As(mod, () => TypedHooks.Detour(Volume, c =>
        {
            *(int*)c.ReturnBuffer = 64;
            ((byte*)c.ReturnBuffer)[4] = 1;
            c.Skip();
        }));
        fixed (long* b = buffer)
        {
            var bp = (ulong)b;
            var f = FakeTyped.CallTyped(TheLink(), f => { *(int*)f->Gpr[0] = 32; f->Rax = f->Gpr[0]; }, f => { f->Gpr[0] = bp; f->Gpr[1] = 0x1234; });
            Assert.Equal(0, FakeTyped.OriginalCalls);
            Assert.Equal(bp, f.Rax);  // a function with sret returns its buffer
            Assert.Equal(64, *(int*)b);
            Assert.Equal(1, ((byte*)b)[4]);
        }
    }

    [Fact]
    public void SkipAfterTheOriginalChangesNothing()
    {
        var mod = Mod();
        TestMods.As(mod, () => TypedHooks.Detour(GameTime, c =>
        {
            c.CallOriginal();
            c.Skip();
        }));
        var f = FakeTyped.CallTyped(TheLink(), f => f->Rax = 99);
        Assert.Equal(1, FakeTyped.OriginalCalls);
        Assert.Equal(99UL, f.Rax);
    }

    [Fact]
    public void FallingThroughRunsTheOriginalAfterTheHandlerWithItsArguments()
    {
        var mod = Mod();
        float seen = 0;
        TestMods.As(mod, () => TypedHooks.Detour(TimeScale, c => c.SetF32(1, c.F32(1) * 2)));
        FakeTyped.CallTyped(TheLink(), f => seen = *(float*)&f->Xmm[16], f => *(float*)&f->Xmm[16] = 1.0f);
        Assert.Equal(1, FakeTyped.OriginalCalls);
        Assert.Equal(2.0f, seen);
    }

    [Fact]
    public void AHandlerThatThrowsBeforeTheOriginalLetsItRunWithTheArgumentsItLeft()
    {
        var mod = Mod();
        float seen = 0;
        TestMods.As(mod, () => TypedHooks.Detour(TimeScale, c =>
        {
            c.SetF32(1, 3.0f);
            throw new InvalidOperationException("boom");
        }));
        FakeTyped.CallTyped(TheLink(), f => seen = *(float*)&f->Xmm[16], f => *(float*)&f->Xmm[16] = 1.0f);
        Assert.Equal(1, FakeTyped.OriginalCalls);
        Assert.Equal(3.0f, seen);
        Assert.Equal(1, HooksApi.Typed[(long)TheLink().User].Faults);
    }

    [Fact]
    public void AHandlerThatThrowsAfterTheOriginalHandsTheCallerTheOriginalsResult()
    {
        var mod = Mod();
        TestMods.As(mod, () => TypedHooks.Detour(Six, c =>
        {
            c.CallOriginal();
            c.SetRetF32(999);
            throw new InvalidOperationException("after");
        }));
        var f = FakeTyped.CallTyped(TheLink(), SixOriginal, SixArgs);
        Assert.Equal(1, FakeTyped.OriginalCalls);  // ran once, not again after the fault
        Assert.Equal(7 + 1.5f + 2.25f + 40 + 0.5f + 100, *(float*)f.Xmm0R);
    }

    [Fact]
    public void AHandlerThatThrowsAfterTheOriginalLeavesTheOriginalsReturnBuffer()
    {
        var mod = Mod();
        var buffer = new long[1];
        TestMods.As(mod, () => TypedHooks.Detour(Volume, c =>
        {
            c.CallOriginal();
            *(int*)c.ReturnBuffer = 999;
            c.SetRetInt(0);
            throw new InvalidOperationException("after");
        }));
        fixed (long* b = buffer)
        {
            var bp = (ulong)b;
            var f = FakeTyped.CallTyped(TheLink(), f => { *(int*)f->Gpr[0] = 32; f->Rax = f->Gpr[0]; }, f => f->Gpr[0] = bp);
            Assert.Equal(1, FakeTyped.OriginalCalls);
            Assert.Equal(32, *(int*)b);
            Assert.Equal(bp, f.Rax);
        }
    }

    [Fact]
    public void ARefusedCallOriginalIsNotLatchedAndTheOriginalStillRuns()
    {
        var mod = Mod();
        bool? called = null;
        Exception? refused = null;
        TestMods.As(mod, () => TypedHooks.Detour(GameTime, c =>
        {
            FakeTyped.RefuseCallOriginal = true;
            try { c.CallOriginal(); }
            catch (InvalidOperationException e) { refused = e; }
            FakeTyped.RefuseCallOriginal = false;
            called = c.OriginalCalled;
        }));
        var f = FakeTyped.CallTyped(TheLink(), f => f->Rax = 42);
        Assert.NotNull(refused);
        Assert.False(called);  // nothing ran: the handler must not believe it did
        Assert.Equal(1, FakeTyped.OriginalCalls);  // the fall-through ran it
        Assert.Equal(42UL, f.Rax);
    }

    [Fact]
    public void TheHandlerRunsAsItsModAndIsCountedInFlight()
    {
        var mod = Mod();
        ModContext? inside = null;
        long counted = -1;
        var box = TestMods.As(mod, () =>
        {
            TypedHooks.Detour(GameTime, c =>
            {
                inside = TestMods.Current;
                counted = HooksApi.Typed[(long)TheLink().User].InFlight.Value;
            });
            return HooksApi.Typed.Values.Single(r => r.Mod == mod).InFlight;
        });
        // a game worker thread: nothing of the mod is current there
        var t = new Thread(() => FakeTyped.CallTyped(TheLink(), f => f->Rax = 1));
        t.Start();
        t.Join();
        Assert.Same(mod, inside);
        Assert.Equal(1, counted);
        Assert.Equal(0, box.Value);
    }

    [Fact]
    public void ARemovedOrRetiredRegistrationPassesThrough()
    {
        var mod = Mod();
        var runs = 0;
        var r = TestMods.As(mod, () => TypedHooks.Detour(GameTime, c => runs++));
        var link = TheLink();
        r.Handle!.Remove();
        // a call already on its way through the revoked link finds no registration: the original runs
        FakeTyped.CallTyped(link, f => f->Rax = 1);
        Assert.Equal(0, runs);
        Assert.Equal(1, FakeTyped.OriginalCalls);

        TestMods.As(mod, () => TypedHooks.Detour(GameTime, c => runs++));
        link = TheLink();
        TestMods.Retire(mod);
        Assert.Empty(FakeRegistry.Live);
        Assert.DoesNotContain(HooksApi.Typed.Values, x => x.Mod == mod);
        FakeTyped.CallTyped(link, f => f->Rax = 1);
        Assert.Equal(0, runs);
        Assert.Equal(2, FakeTyped.OriginalCalls);
        Assert.Equal(HookCookies.MadeCount(_id, HookCookies.Typed, Address), HookCookies.FreeCount(_id, HookCookies.Typed, Address));
    }

    [Fact]
    public void ADescriptorTheCodeDoesNotFitIsARefusalNotAnException()
    {
        var mod = Mod();
        FakeTyped.Refuse = HooksNative.SigMismatch;
        FakeTyped.RefusalText = "descriptor arg 2 'scale' is xmm1, the function reads [rcx, rdx]";
        var r = TestMods.As(mod, () => TypedHooks.Detour(TimeScale, c => { }));
        Assert.False(r.Ok);
        Assert.Null(r.Handle);
        Assert.Equal(FakeTyped.RefusalText, r.Reason);
        Assert.Empty(FakeRegistry.Live);
        Assert.DoesNotContain(HooksApi.Typed.Values, x => x.Mod == mod);
        Assert.Equal(HookCookies.MadeCount(_id, HookCookies.Typed, Address), HookCookies.FreeCount(_id, HookCookies.Typed, Address));
    }

    [Fact]
    public void AnUnresolvedSymbolOrAFullTargetTableIsARefusal()
    {
        var mod = Mod();
        FakeTyped.SymbolTable.Remove(TimeScale.Symbol);
        var r = TestMods.As(mod, () => TypedHooks.Detour(TimeScale, c => { }));
        Assert.False(r.Ok);
        Assert.Contains("does not resolve on this game build", r.Reason);
        Assert.Null(FakeTyped.LastSig);  // nothing was asked of kiln.hooks

        FakeTyped.Refuse = HooksNative.Unavailable;
        r = TestMods.As(mod, () => TypedHooks.Detour(GameTime, c => { }));
        Assert.False(r.Ok);
        Assert.Contains("target cap", r.Reason);
    }

    [Fact]
    public void AMalformedDescriptorReportedByTheCoreThrows()
    {
        var mod = Mod();
        FakeTyped.Refuse = HooksNative.InvalidArgument;
        Assert.Throws<ArgumentException>(() => TestMods.As(mod, () => TypedHooks.Detour(GameTime, c => { })));
    }

    [Fact]
    public void TheCoreGetsTheDescriptorsShapeAndTheSymbolAsLabel()
    {
        var mod = Mod();
        TestMods.As(mod, () => TypedHooks.Detour(Volume, c => { }, priority: 7));
        var s = FakeTyped.LastSig!;
        Assert.Equal((uint)sizeof(HooksNative.SigRaw), s.Size);
        Assert.Equal(1u, s.Argc);
        Assert.Equal((uint)SigKind.Struct, s.RetKind);
        Assert.Equal(8u, s.RetSize);
        Assert.Equal(1, s.Sret);
        Assert.Equal(new[] { (uint)SigKind.Ptr }, s.Kinds);
        Assert.Equal(new[] { "item" }, s.Names);
        Assert.Equal((uint)sizeof(HooksNative.OrderRaw), s.OrderSize);
        Assert.Equal(7, s.Priority);
        Assert.Equal(Volume.Symbol, s.Label);
    }

    [Fact]
    public void WithoutTypedLinksDetourAndInvokeNameHooksTyped()
    {
        FakeRegistry.Install(typed: false);  // a core older than dev.28: its table stops before the typed members
        var mod = Mod();
        var e = Assert.Throws<KilnUnavailableException>(() => TestMods.As(mod, () => TypedHooks.Detour(GameTime, c => { })));
        Assert.Equal("hooks.typed", e.Capability);
        var frame = default(SigFrame);
        e = Assert.Throws<KilnUnavailableException>(() => TestMods.As(mod, () => TypedHooks.Invoke(Six, ref frame)));
        Assert.Equal("hooks.typed", e.Capability);
    }

    [Fact]
    public void AnOrdinaryModIsRefused()
    {
        var mod = Mod(advanced: false);
        TestMods.AssertGated("test/GetGameTime.Detour", _id, () => TestMods.As(mod, () => TypedHooks.Detour(GameTime, c => { })));
        var frame = default(SigFrame);
        TestMods.AssertGated("test/Six.Invoke", _id, () => TestMods.As(mod, () => TypedHooks.Invoke(Six, ref frame)));
    }

    [Fact]
    public void AnUnloadedModCannotInstall()
    {
        var mod = Mod();
        TestMods.As(mod, () => TypedHooks.Detour(GameTime, c => { }));
        TestMods.Retire(mod);
        Assert.Throws<InvalidOperationException>(() => TestMods.As(mod, () => TypedHooks.Detour(GameTime, c => { })));
        Assert.Empty(FakeRegistry.Live);
    }

    [Fact]
    public void InvokeFillsTheFrameAndForwardsTheStackArguments()
    {
        var mod = Mod();
        TypedHooks.SigRowOn = _ => true;
        FakeTyped.Functions[Address] = SixOriginal;
        var frame = default(SigFrame);
        frame.SetInt(0, 7);
        frame.SetF32(1, 1.5f);
        frame.SetF64(2, 2.25);
        frame.SetInt(3, 40);
        frame.SetF32(4, 0.5f);
        frame.SetInt(5, 100);
        TestMods.As(mod, () => TypedHooks.Invoke(Six, ref frame));
        Assert.Equal(2u, FakeTyped.LastNStack);
        Assert.Equal(7 + 1.5f + 2.25f + 40 + 0.5f + 100, frame.RetF32);
    }

    [Fact]
    public void InvokeNeverCallsAFunctionWhoseRowIsOff()
    {
        var mod = Mod();
        TypedHooks.SigRowOn = _ => false;
        var ran = false;
        FakeTyped.Functions[Address] = _ => ran = true;
        var frame = default(SigFrame);
        var e = Assert.Throws<KilnUnavailableException>(() => TestMods.As(mod, () => TypedHooks.Invoke(Six, ref frame)));
        Assert.Equal("sig/test/Six", e.Capability);
        Assert.False(ran);
    }

    [Fact]
    public void InvokeOfATickDescriptorAsksForTheTickThreadAndAnAnyOneDoesNot()
    {
        var mod = Mod();
        TypedHooks.SigRowOn = _ => true;
        var asked = new List<string>();
        TypedHooks.RequireTick = api =>
        {
            asked.Add(api);
            throw new InvalidOperationException($"{api} must be called on the tick thread");
        };
        FakeTyped.Functions[Address] = f => f->Rax = 1;
        var frame = default(SigFrame);
        Assert.Throws<InvalidOperationException>(() => TestMods.As(mod, () => TypedHooks.Invoke(GameTime, ref frame)));
        Assert.Equal(new[] { "test/GetGameTime.Invoke" }, asked);
        Assert.Equal(0UL, frame.RetInt);  // refused before the call
        TestMods.As(mod, () => TypedHooks.Invoke(Six, ref frame));  // "any": no tick guard
        Assert.Single(asked);
    }

    [Fact]
    public void InvokeOfAHookOnlyDescriptorThrows()
    {
        var mod = Mod();
        var hookOnly = new SigDescriptor("test/AttrChanged", SigKind.Void, 0, false, SigInvoke.None,
            new SigArg("component", SigKind.Ptr, 8), new SigArg("registry", SigKind.Ptr, 8));
        var frame = default(SigFrame);
        Assert.Throws<InvalidOperationException>(() => TestMods.As(mod, () => TypedHooks.Invoke(hookOnly, ref frame)));
    }

    [Theory]
    [InlineData(SigKind.Struct, 16u, false)]   // a 16-byte struct return needs sret
    [InlineData(SigKind.U32, 4u, true)]        // sret needs a struct return
    [InlineData(SigKind.Struct, 0u, true)]     // sret needs the struct's size
    [InlineData(SigKind.ByVal, 8u, false)]     // by-value is no return kind
    public void AMalformedDescriptorIsRefusedWhenItIsMade(SigKind ret, uint size, bool sret)
    {
        Assert.Throws<ArgumentException>(() => new SigDescriptor("test/Bad", ret, size, sret, SigInvoke.None));
    }

    [Fact]
    public void ADescriptorKeepsAtMostTwentyPositionsAndNoArgumentKindsThatAreNone()
    {
        var twenty = Enumerable.Range(0, 20).Select(i => new SigArg($"a{i}", SigKind.U64, 8)).ToArray();
        Assert.Equal(16, new SigDescriptor("test/Twenty", SigKind.Void, 0, false, SigInvoke.None, twenty).StackCount);
        Assert.Throws<ArgumentException>(() => new SigDescriptor("test/TwentyOne", SigKind.Struct, 32, true, SigInvoke.None, twenty));
        Assert.Throws<ArgumentException>(() => new SigDescriptor("test/V", SigKind.Void, 0, false, SigInvoke.None, new SigArg("v", SigKind.Void, 0)));
        Assert.Throws<ArgumentException>(() => new SigDescriptor("test/B", SigKind.Void, 0, false, SigInvoke.None, new SigArg("b", SigKind.ByVal, 0)));
        var args = new[] { new SigArg("x", SigKind.U8, 1) };
        var d = new SigDescriptor("test/Copy", SigKind.Void, 0, false, SigInvoke.None, args);
        args[0] = new SigArg("y", SigKind.F64, 8);
        Assert.Equal("x", d.Args[0].Name);  // the descriptor keeps its own copy
    }

    [Fact]
    public void AFragmentsDescriptorStatusComesFromItsReport()
    {
        var report = """
            {"symbols": {"m/Fn": {"ok": true}}, "sigs": {
              "m/Fn": {"status": "match", "text": "2 arguments (rcx, rdx), the function reads [rcx, rdx]"},
              "m/Changed": {"status": "mismatch"}, "m/Dark": {"status": "unknown"}, "m/Odd": 3}}
            """;
        var sigs = Symbols.ParseFragmentSigs(report);
        Assert.True(sigs["m/Fn"]);
        Assert.False(sigs["m/Changed"]);
        Assert.False(sigs["m/Dark"]);
        Assert.False(sigs["m/Odd"]);
        Assert.Empty(Symbols.ParseFragmentSigs("""{"symbols": {}}"""));
    }

    [Fact]
    public void LoadShippedReadsTheFragmentsDescriptorsAndTheUnloadDropsThem()
    {
        var dir = Path.Combine(AppContext.BaseDirectory, "no-such-mod-folder", _id);
        Directory.CreateDirectory(Path.Combine(dir, "sigdb"));
        File.WriteAllText(Path.Combine(dir, "sigdb", "server.json"), "{}");
        var report = Path.Combine(dir, "report.json");
        File.WriteAllText(report, "{\"sigs\": {\"" + _id + "/Fn\": {\"status\": \"match\"}, \"" + _id + "/Old\": {\"status\": \"mismatch\"}}}");
        var was = Symbols.FragmentReport;
        Symbols.FragmentReport = _ => report;
        try
        {
            var mod = Mod();
            TestMods.As(mod, () => Symbols.LoadShipped());
            Assert.True(Symbols.FragmentSigMatches($"{_id}/Fn"));
            Assert.False(Symbols.FragmentSigMatches($"{_id}/Old"));
            Assert.False(Symbols.FragmentSigMatches($"{_id}/Missing"));
            Assert.False(Symbols.FragmentSigMatches("other-mod/Fn"));
            Assert.True(TypedHooks.DefaultSigRowOn($"{_id}/Fn"));  // no kiln.caps row: the fragment's report decides
            Assert.False(TypedHooks.DefaultSigRowOn($"{_id}/Old"));
            TestMods.Retire(mod);
            Assert.False(Symbols.FragmentSigMatches($"{_id}/Fn"));  // nothing of the fragment survives the unload
            Assert.False(FakeRegistry.HasSymbols(_id));
        }
        finally
        {
            Symbols.FragmentReport = was;
        }
    }

    [Fact]
    public void AnUnloadThatTimesOutKeepsTheFragmentUntilTheLastHandlerReturns()
    {
        var dir = Path.Combine(AppContext.BaseDirectory, "no-such-mod-folder", _id);
        Directory.CreateDirectory(Path.Combine(dir, "sigdb"));
        File.WriteAllText(Path.Combine(dir, "sigdb", "server.json"), "{}");
        var wasReport = Symbols.FragmentReport;
        var wasWait = HooksGate.DrainWaitMs;
        Symbols.FragmentReport = _ => Path.Combine(dir, "none.json");
        HooksGate.DrainWaitMs = 50;
        using var entered = new ManualResetEventSlim();
        using var release = new ManualResetEventSlim();
        try
        {
            var mod = Mod();
            TestMods.As(mod, () =>
            {
                Symbols.LoadShipped();
                TypedHooks.Detour(GameTime, c =>
                {
                    entered.Set();
                    release.Wait(TimeSpan.FromSeconds(10));
                });
            });
            var link = TheLink();
            var game = new Thread(() => FakeTyped.CallTyped(link, f => f->Rax = 1));
            game.Start();
            Assert.True(entered.Wait(TimeSpan.FromSeconds(5)));
            TestMods.Retire(mod);  // waits 50 ms, then lets the unload go on
            Assert.True(FakeRegistry.HasSymbols(_id));  // the running handler's fragment is still there
            release.Set();
            game.Join();
            var sw = System.Diagnostics.Stopwatch.StartNew();
            while (FakeRegistry.HasSymbols(_id) && sw.ElapsedMilliseconds < 5000) Thread.Sleep(10);
            Assert.False(FakeRegistry.HasSymbols(_id));  // dropped once the call returned
        }
        finally
        {
            release.Set();
            Symbols.FragmentReport = wasReport;
            HooksGate.DrainWaitMs = wasWait;
        }
    }
}
