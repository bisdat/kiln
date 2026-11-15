// SPDX-License-Identifier: MIT
using Xunit;
using Volume = Kiln.Sigs.Items.GetItemVolume;
using RegisterNative = Kiln.Sigs.Abilities.RegisterNative;
using SetTimeScale = Kiln.Sigs.Time.SetTimeScale;
using GetGameTime = Kiln.Sigs.Time.GetGameTime;

namespace Kiln.Hooks.Tests;

/// <summary>The generated classes of Kiln.Sigs (the core's descriptors) on the fake typed table: the narrow stack
/// argument of RegisterNative is masked, SetTimeScale's float is xmm1, GetItemVolume returns its struct by value from
/// Invoke and a handler that changes it after the original reaches the caller (chapter 06b §9.10), a skipping handler
/// keeps a second one from running, and a tick descriptor's Invoke asks for the tick thread.</summary>
[Collection("kiln.hooks state")]
public sealed unsafe class GeneratedSigsTests : IDisposable
{
    static int _seq;
    readonly string _id = $"sigs-mod-{Interlocked.Increment(ref _seq)}";
    static readonly nuint Address = unchecked((nuint)0x140004000UL);
    readonly Func<string, ulong> _createOwner = HookCookies.CreateOwner;
    readonly Func<string, bool> _rowOn = TypedHooks.SigRowOn;
    readonly Action<string> _tick = TypedHooks.RequireTick;

    public GeneratedSigsTests()
    {
        FakeRegistry.Install(typed: true);
        HookCookies.CreateOwner = _ => 0x8000;
        foreach (var s in new[] { Volume.Symbol, RegisterNative.Symbol, SetTimeScale.Symbol, GetGameTime.Symbol }) FakeTyped.SymbolTable[s] = Address;
        TypedHooks.SigRowOn = _ => true;
    }

    public void Dispose()
    {
        HookCookies.CreateOwner = _createOwner;
        TypedHooks.SigRowOn = _rowOn;
        TypedHooks.RequireTick = _tick;
        FakeRegistry.Uninstall();
    }

    ModContext Mod() => TestMods.Make(_id, advanced: true, owner: 80);

    static List<FakeRegistry.Link> Links() => FakeRegistry.Live.Where(l => l.Kind == HookCookies.Typed).ToList();

    // GetItemVolume: the Volume of an item, 32 for the test's can
    static void Volume32(HooksNative.CallFrame* f)
    {
        var o = (Kiln.Sigs.OptS32*)f->Gpr[0];
        o->Value = f->Gpr[1] == 0 ? 18 : 32;
        o->Has = 1;
        f->Rax = f->Gpr[0];
    }

    [Fact]
    public void TheNarrowStackArgumentAndTheBoolReturnAreMasked()
    {
        var mod = Mod();
        byte argc = 0;
        bool before = true;
        TestMods.As(mod, () => RegisterNative.Detour((ref RegisterNative.Call c) =>
        {
            argc = c.Argc;
            c.CallOriginal();
            before = c.Result;
        }));
        var stack = new ulong[16];
        stack[0] = 0x11e8f3300UL + 25;  // argc 25 with the caller's garbage above it (lab T1, 'applyDamage')
        FakeTyped.CallTyped(Assert.Single(Links()), f => f->Rax = 0xABCD0001, f => { f->Gpr[1] = 0xb04d0e7a; }, stack);
        Assert.Equal(25, argc);
        Assert.True(before);  // AL = 1, whatever the upper bytes of rax hold
    }

    [Fact]
    public void SetTimeScaleSeesTheScaleInXmm1AndNowInR8()
    {
        var mod = Mod();
        float scale = 0;
        long now = 0;
        float passed = 0;
        TestMods.As(mod, () => SetTimeScale.Detour((ref SetTimeScale.Call c) =>
        {
            scale = c.Scale;
            now = c.EcsNow;
            c.Scale = 2.0f;
        }));
        FakeTyped.CallTyped(Assert.Single(Links()), f => passed = *(float*)&f->Xmm[16], f =>
        {
            f->Gpr[0] = 0x1242a6a20;
            *(float*)&f->Xmm[16] = 1.0f;
            f->Gpr[2] = 7050000141;
        });
        Assert.Equal(1.0f, scale);
        Assert.Equal(7050000141, now);
        Assert.Equal(2.0f, passed);  // the original ran after the handler with the changed scale
    }

    [Fact]
    public void InvokeReturnsTheStructByValueAndAHandlerThatChangesItAfterTheOriginalReachesTheCaller()
    {
        var mod = Mod();
        FakeTyped.Functions[Address] = Volume32;
        var direct = TestMods.As(mod, () => Volume.Invoke(0x1234));
        Assert.Equal(32, direct.Value);
        Assert.Equal(1, direct.Has);
        Assert.Equal(2u, (uint)Volume.Descriptor.Positions);

        TestMods.As(mod, () => Volume.Detour((ref Volume.Call c) =>
        {
            c.CallOriginal();
            if (c.Result.Has != 0) c.Result.Value = 64;
        }));
        var buffer = new Kiln.Sigs.OptS32[1];
        fixed (Kiln.Sigs.OptS32* b = buffer)
        {
            var bp = (ulong)b;
            var f = FakeTyped.CallTyped(Assert.Single(Links()), Volume32, f => { f->Gpr[0] = bp; f->Gpr[1] = 0x1234; });
            Assert.Equal(64, b->Value);  // the next TransferFuelEvent cap of the lab row
            Assert.Equal(bp, f.Rax);
        }
    }

    [Fact]
    public void TwoHandlersTheFirstSkipsTheSecondNeverRuns()
    {
        var mod = Mod();
        var second = 0;
        TestMods.As(mod, () =>
        {
            GetGameTime.Detour((ref GetGameTime.Call c) => { c.Result = 5; c.Skip(); }, priority: 10);
            GetGameTime.Detour((ref GetGameTime.Call c) => second++, priority: 0);
        });
        var links = Links();
        Assert.Equal(2, links.Count);
        // the chain as kiln.hooks runs it: the first link's original is the second link, then the game's function
        var first = links[0];
        var next = links[1];
        var f = FakeTyped.CallTyped(first, f => FakeTyped.CallTyped(next, g => g->Rax = 99));
        Assert.Equal(0, second);
        Assert.Equal(0, FakeTyped.OriginalCalls);
        Assert.Equal(5UL, f.Rax);
    }

    [Fact]
    public void ATickDescriptorsInvokeAsksForTheTickThreadAndAnAnyOneDoesNot()
    {
        var mod = Mod();
        var asked = 0;
        TypedHooks.RequireTick = api =>
        {
            asked++;
            throw new InvalidOperationException($"{api} must be called on the tick thread (use Scheduler.Post)");
        };
        FakeTyped.Functions[Address] = Volume32;
        var e = Assert.Throws<InvalidOperationException>(() => TestMods.As(mod, () => GetGameTime.Invoke(0, 0)));
        Assert.Contains("time/GetGameTime.Invoke", e.Message);
        Assert.Equal(1, asked);
        TestMods.As(mod, () => Volume.Invoke(0));  // invoke "any"
        Assert.Equal(1, asked);
    }

    [Fact]
    public void AnOffRowStopsInvokeBeforeTheFunction()
    {
        var mod = Mod();
        TypedHooks.SigRowOn = s => s != Volume.Symbol;
        var ran = false;
        FakeTyped.Functions[Address] = _ => ran = true;
        var e = Assert.Throws<KilnUnavailableException>(() => TestMods.As(mod, () => Volume.Invoke(0)));
        Assert.Equal(Volume.Capability, e.Capability);
        Assert.False(ran);
    }
}
