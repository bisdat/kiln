// SPDX-License-Identifier: MIT
extern alias khooks;
using Xunit;
using HooksApi = khooks::Kiln.Hooks;

namespace Kiln.Hooks.Tests;

/// <summary>Hooks inside another loaded module (0.2.0-dev.29.19): XHL-Creative-Mode-Fix's mid hook in Creative Mode's
/// creative.dll was refused with "Hooks.Mid(0x7FFB83A7F5F7): invalid argument". A mod now opens the module
/// (Hooks.Module), and its addresses go to the native table, which accepts them once opened (the native test
/// hooks_v2_test.cpp test_foreign_module covers that side).</summary>
[Collection("kiln.hooks state")]
public sealed unsafe class ForeignModuleTests : IDisposable
{
    static int _seq;
    readonly string _id = $"modules-mod-{Interlocked.Increment(ref _seq)}";
    readonly Func<string, ulong> _createOwner = HookCookies.CreateOwner;
    static readonly nuint CreativeBase = unchecked((nuint)0x7FFB83920000UL);

    public ForeignModuleTests()
    {
        FakeRegistry.Install(modules: true);
        HookCookies.CreateOwner = _ => 0x7100;
        FakeModules.Loaded["creative.dll"] = new(CreativeBase, 6144000, CreativeBase + 0x1000, CreativeBase + 0x400000, HooksNative.ModulePinned);
        FakeModules.Loaded["enshrouded.exe"] = new(unchecked((nuint)0x140000000UL), 0x3000000, unchecked((nuint)0x140001000UL),
            unchecked((nuint)0x142000000UL), HooksNative.ModuleGame);
    }

    public void Dispose()
    {
        HookCookies.CreateOwner = _createOwner;
        FakeRegistry.Uninstall();
    }

    ModContext Mod() => TestMods.Make(_id, advanced: true, owner: 71);
    static void Nothing(ref Registers r) { }

    [Fact]
    public void TheTableLayoutMatchesTheHeader()
    {
        Assert.Equal(48, sizeof(HooksNative.ModuleRaw));
        Assert.Equal(328, sizeof(HooksNative.ModulesTable));
        HooksNative.ModulesTable t = default;
        Assert.Equal(312, (int)((byte*)&t.OpenModule - (byte*)&t));
        Assert.Equal(320, (int)((byte*)&t.ModuleScan - (byte*)&t));
        Assert.Equal(HooksNative.ModulesSize, (uint)sizeof(HooksNative.ModulesTable));
    }

    [Fact]
    public void ModuleOpensALoadedModuleAndAtGivesAnAddressInItsCode()
    {
        var mod = Mod();
        var m = TestMods.As(mod, () => HooksApi.Module("creative.dll"));
        Assert.Equal("creative.dll", m.Name);
        Assert.Equal(CreativeBase, m.Base);
        Assert.False(m.IsGame);
        Assert.Equal(CreativeBase + 0x15F5F7, m.At(0x15F5F7));  // netjunk's 0x7FFB83A7F5F7
        Assert.Equal("creative.dll+0x15F5F7", m.Describe(m.At(0x15F5F7)));
        Assert.Throws<ArgumentOutOfRangeException>(() => m.At(0x10));        // its headers
        Assert.Throws<ArgumentOutOfRangeException>(() => m.At(0x500000));    // past its code
        Assert.Throws<ArgumentOutOfRangeException>(() => m.At(unchecked((nuint)ulong.MaxValue)));
        Assert.Equal([(0x7100UL, "creative.dll")], FakeModules.OpenCalls);  // under the mod's hooks owner
    }

    [Fact]
    public void AMidHookInsideTheModuleReachesTheNativeTableWithItsAddress()
    {
        var mod = Mod();
        TestMods.As(mod, () =>
        {
            var at = HooksApi.Module("creative.dll").At(0x15F5F7);
            using var h = HooksApi.Mid(at, Nothing);
            var link = Assert.Single(FakeRegistry.Live);
            Assert.Equal(at, link.Address);
            Assert.Equal(HookCookies.Mid, link.Kind);
            // the name as the module was first opened in this process (TryModuleIsFalseForAModuleThatIsNotLoaded opens
            // "CREATIVE.DLL"; Windows module names have no case), so the test does not depend on the order tests run in
            Assert.Equal("creative.dll+0x15F5F7 (0x7FFB83A7F5F7)", HooksApi.Where(at), ignoreCase: true);
        });
    }

    [Fact]
    public void TryModuleIsFalseForAModuleThatIsNotLoaded()
    {
        var mod = Mod();
        TestMods.As(mod, () =>
        {
            Assert.False(HooksApi.TryModule("not-loaded.dll", out var none));
            Assert.Null(none);
            Assert.Throws<KeyNotFoundException>(() => HooksApi.Module("not-loaded.dll"));
            Assert.True(HooksApi.TryModule("CREATIVE.DLL", out var m));
            Assert.Equal(CreativeBase, m!.Base);
        });
    }

    [Fact]
    public void KilnsOwnModulesAreRefused()
    {
        var e = Assert.Throws<InvalidOperationException>(() => TestMods.As(Mod(), () => HooksApi.Module("kiln-core.dll")));
        Assert.Contains("Kiln's own modules", e.Message);
    }

    [Fact]
    public void FindSearchesTheModulesCode()
    {
        FakeModules.Matches[(CreativeBase, "41 BA ?? 89 C8")] = [CreativeBase + 0x15F5F7];
        FakeModules.Matches[(CreativeBase, "C3")] = [CreativeBase + 0x2000, CreativeBase + 0x3000, CreativeBase + 0x4000];
        TestMods.As(Mod(), () =>
        {
            Assert.Equal(CreativeBase + 0x15F5F7, HooksApi.FindInModule("creative.dll", "41 BA ?? 89 C8"));
            var m = HooksApi.Module("creative.dll");
            Assert.Throws<KeyNotFoundException>(() => m.Find("90 90"));
            Assert.Throws<InvalidOperationException>(() => m.Find("C3"));
            Assert.Equal(2, m.FindAll("C3", 2).Length);
            Assert.Throws<ArgumentException>(() => m.Find("zz"));
        });
    }

    [Fact]
    public void TheGamesExeAnswersAsTheGame()
    {
        var m = TestMods.As(Mod(), () => HooksApi.Module("enshrouded.exe"));
        Assert.True(m.IsGame);
        Assert.DoesNotContain(m.Base, FakeModules.Opened);
    }

    [Fact]
    public void ARefusedAddressSaysHowToHookInsideAnotherModule()
    {
        FakeRegistry.RefuseAddress = a => a == unchecked((nuint)0x7FFB83A7F5F7UL);
        var e = Assert.Throws<InvalidOperationException>(() =>
            TestMods.As(Mod(), () => HooksApi.Mid(unchecked((nuint)0x7FFB83A7F5F7UL), Nothing)));
        Assert.StartsWith("Hooks.Mid(", e.Message);
        Assert.Contains("0x7FFB83A7F5F7", e.Message);
        Assert.Contains("): invalid argument", e.Message);
        Assert.Contains("Hooks.Module(name)", e.Message);
    }

    [Fact]
    public void AnOlderCoreSaysHooksForeignModulesIsOff()
    {
        FakeRegistry.Install(typed: true);  // a dev.29 core before 29.19: its table stops before open_module
        var e = Assert.Throws<KilnUnavailableException>(() => TestMods.As(Mod(), () => HooksApi.Module("creative.dll")));
        Assert.Equal("hooks.foreign_modules", e.Capability);
        Assert.Contains("older than 0.2.0-dev.29.19", e.Message);
        Assert.Null(HooksNative.Modules);
    }

    [Fact]
    public void NoCoreSaysHooksForeignModulesIsOff()
    {
        FakeRegistry.Uninstall();
        var e = Assert.Throws<KilnUnavailableException>(() => TestMods.As(Mod(), () => HooksApi.Module("creative.dll")));
        Assert.Equal("hooks.foreign_modules", e.Capability);
    }

    [Fact]
    public void ModuleIsForAdvancedModsOnly()
    {
        var plain = TestMods.Make(_id + "-plain", advanced: false);
        TestMods.As(plain, () =>
        {
            TestMods.AssertGated("Hooks.Module", _id + "-plain", () => HooksApi.Module("creative.dll"));
            TestMods.AssertGated("Hooks.TryModule", _id + "-plain", () => HooksApi.TryModule("creative.dll", out _));
        });
        Assert.Empty(FakeModules.OpenCalls);
    }
}
