// SPDX-License-Identifier: MIT
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using Kiln.Host;
using Xunit;
namespace Kiln.Host.Tests;

public sealed class GameplayDemandMod : Mod
{
    public static Type GameplayReference() => typeof(Gameplay);
    public override void OnBoot(BootContext boot) => Log.Info("gameplay-demand-onboot");
    public override void OnLoad() => Log.Info("gameplay-demand-onload");
}

public sealed unsafe class GameplayDemandTests : ModsFixture
{
    private static FakeHost? _host;
    private static int _requests;
    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Request(ulong owner) { ++_requests; _host!.Log(LogLevel.Info, $"gameplay-demand-request:{owner}"); return 0; }

    [Theory]
    [InlineData(true)]
    [InlineData(false)]
    public void Loaded_assembly_requests_in_its_owner_name_before_OnLoad_only_if_it_references_gameplay(bool gameplay)
    {
        var host=new FakeHost(); _host=host; _requests=0;
        var table=(byte*)NativeMemory.AllocZeroed(136); *(uint*)table=136; *(uint*)(table+4)=1;
        *(nint*)(table+128)=(nint)(delegate* unmanaged[Cdecl]<ulong,int>)&Request;
        host.Services["kiln.gameplay"]=(nint)table;
        try {
            if(gameplay) {
                var dir=WriteManifest("demand", """{"id":"demand","version":"1.0.0","apiVersion":"0.2","entryAssembly":"Demand.dll","entryType":"Kiln.Host.Tests.GameplayDemandMod"}""");
                File.Copy(typeof(GameplayDemandMod).Assembly.Location, Path.Combine(dir,"Demand.dll"));
            } else InstallHello();
            using var mods=NewManager(host); mods.LoadAll();
            Assert.Equal(gameplay ? 1 : 0,_requests);
            if(gameplay) {
                Assert.True(host.Has($"gameplay-demand-request:{mods.Find("demand")!.Owner}"),host.Dump());
                Assert.True(host.Lines.FindIndex(x=>x.Contains("gameplay-demand-request:")) < host.Lines.FindIndex(x=>x.Contains("gameplay-demand-onload")),host.Dump());
            }
        } finally { NativeMemory.Free(table); _host=null; }
    }

    [Theory]
    [InlineData(true, false)]
    [InlineData(false, false)]
    [InlineData(false, true)]
    public void Startup_preflight_requests_before_any_mod_executes(bool gameplay, bool sidecar)
    {
        var host=new FakeHost(); _host=host; _requests=0;
        var table=(byte*)NativeMemory.AllocZeroed(136); *(uint*)table=136; *(uint*)(table+4)=1;
        *(nint*)(table+128)=(nint)(delegate* unmanaged[Cdecl]<ulong,int>)&Request;
        host.Services["kiln.gameplay"]=(nint)table;
        try {
            if(gameplay) {
                var dir=WriteManifest("demand", """{"id":"demand","version":"1.0.0","apiVersion":"0.2","entryAssembly":"Demand.dll","entryType":"Kiln.Host.Tests.GameplayDemandMod"}""");
                File.Copy(typeof(GameplayDemandMod).Assembly.Location, Path.Combine(dir,"Demand.dll"));
            } else InstallHello();
            using var mods=NewManager(host);
            if(sidecar) File.Copy(typeof(GameplayDemandMod).Assembly.Location,Path.Combine(ModsDir,"hello","GameplayHelper.dll"));
            var prepare=typeof(ModManager).GetMethod("PrepareGameplayHooks",System.Reflection.BindingFlags.Instance|System.Reflection.BindingFlags.NonPublic);
            Assert.NotNull(prepare); prepare.Invoke(mods,null);
            Assert.Equal(gameplay || sidecar ? 1 : 0,_requests); Assert.False(host.Has("gameplay-demand-onload"));
            Assert.DoesNotContain(typeof(ModManager).Assembly.GetReferencedAssemblies(), a => a.Name == "Kiln.Gameplay");
        } finally { NativeMemory.Free(table); _host=null; }
    }
    [Fact]
    public void Boot_author_requests_before_OnBoot_and_normal_load_repeats_in_its_own_owner()
    {
        var host=new FakeHost(); _host=host; _requests=0;
        var table=(byte*)NativeMemory.AllocZeroed(136); *(uint*)table=136; *(uint*)(table+4)=1;
        *(nint*)(table+128)=(nint)(delegate* unmanaged[Cdecl]<ulong,int>)&Request;
        host.Services["kiln.gameplay"]=(nint)table;
        try {
            var dir=WriteManifest("demand", """{"id":"demand","version":"1.0.0","apiVersion":"0.2","entryAssembly":"Demand.dll","entryType":"Kiln.Host.Tests.GameplayDemandMod"}""");
            File.Copy(typeof(GameplayDemandMod).Assembly.Location, Path.Combine(dir,"Demand.dll"));
            using var mods=NewManager(host);
            var boot=new BootContext("demand",1,BootRoles.DedicatedServer,new UnavailableGameData());
            Assert.True(mods.RunBootAuthor(Manifest.Read(dir),dir,77,boot,out var why),why);
            Assert.Equal(1,_requests); Assert.True(host.Has("gameplay-demand-request:77"));
            Assert.True(host.Lines.FindIndex(x=>x.Contains("gameplay-demand-request:")) < host.Lines.FindIndex(x=>x.Contains("gameplay-demand-onboot")));
            mods.LoadAll(); Assert.Equal(2,_requests);
        } finally { NativeMemory.Free(table); _host=null; }
    }

}
