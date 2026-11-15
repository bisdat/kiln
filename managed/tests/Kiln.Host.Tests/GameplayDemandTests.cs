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
    private static int _requests, _announcements; private static uint _groups;
    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int RequestGroups(ulong owner,uint groups) { if(groups==0) { ++_announcements; return 0; } ++_requests; _groups=groups; _host!.Log(LogLevel.Info,$"gameplay-demand-request:{owner}"); return 0; }
    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Request(ulong owner) { ++_requests; _host!.Log(LogLevel.Info, $"gameplay-demand-request:{owner}"); return 0; }

    [Theory]
    [InlineData(true,136)]
    [InlineData(false,136)]
    [InlineData(true,144)]
    [InlineData(false,144)]
    public void Loaded_assembly_requests_in_its_owner_name_before_OnLoad_only_if_it_references_gameplay(bool gameplay,int size)
    {
        var host=new FakeHost(); _host=host; _requests=0; _groups=0;
        var table=(byte*)NativeMemory.AllocZeroed((nuint)size); *(uint*)table=(uint)size; *(uint*)(table+4)=1;
        *(nint*)(table+128)=(nint)(delegate* unmanaged[Cdecl]<ulong,int>)&Request;
        if(size==144) *(nint*)(table+136)=(nint)(delegate* unmanaged[Cdecl]<ulong,uint,int>)&RequestGroups;
        host.Services["kiln.gameplay"]=(nint)table;
        try {
            if(gameplay) {
                var dir=WriteManifest("demand", """{"id":"demand","version":"1.0.0","apiVersion":"0.2","entryAssembly":"Demand.dll","entryType":"Kiln.Host.Tests.GameplayDemandMod"}""");
                File.Copy(typeof(GameplayDemandMod).Assembly.Location, Path.Combine(dir,"Demand.dll"));
            } else InstallHello();
            using var mods=NewManager(host); mods.LoadAll();
            Assert.Equal(gameplay ? 1 : 0,_requests);
            Assert.Equal(gameplay && size==144 ? 31u : 0u,_groups);
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

    [Theory]
    [InlineData(typeof(Creatures),8u)] [InlineData(typeof(Rules),0u)] [InlineData(typeof(Quests),7u)] [InlineData(typeof(BootDefinitions),15u)]
    public void Shared_facade_references_count_their_indirect_gameplay_consumers(Type facade,uint expected)
    {
        var a=System.Reflection.Emit.AssemblyBuilder.DefineDynamicAssembly(new System.Reflection.AssemblyName("FacadeProbe"+Guid.NewGuid().ToString("N")),System.Reflection.Emit.AssemblyBuilderAccess.RunAndCollect);
        var type=a.DefineDynamicModule("main").DefineType("Probe");
        var method=type.DefineMethod("Reference",System.Reflection.MethodAttributes.Public|System.Reflection.MethodAttributes.Static,facade,Type.EmptyTypes);
        var il=method.GetILGenerator(); il.Emit(System.Reflection.Emit.OpCodes.Ldnull); il.Emit(System.Reflection.Emit.OpCodes.Ret); type.CreateType();
        var demand=GameplayBuildDemand.GroupsFor(a); Assert.Equal(expected,demand);
        var host=new FakeHost(); _host=host; _groups=0;
        var table=(byte*)NativeMemory.AllocZeroed(144); *(uint*)table=144; *(uint*)(table+4)=1;
        *(nint*)(table+136)=(nint)(delegate* unmanaged[Cdecl]<ulong,uint,int>)&RequestGroups;
        host.Services["kiln.gameplay"]=(nint)table;
        try { GameplayBuildDemand.Request(host,77,demand); Assert.Equal(expected,_groups); }
        finally { NativeMemory.Free(table); _host=null; }
    }

    [Theory]
    [InlineData("blocks",15u)]
    [InlineData("quests",7u)]
    public void Data_only_folders_prearm_their_declarative_gameplay_consumers(string folder,uint expected)
    {
        var host=new FakeHost(); _host=host; _groups=0;
        var dir=WriteManifest("data", """{"id":"data","version":"1.0.0","apiVersion":"0.2"}""");
        Directory.CreateDirectory(Path.Combine(dir,folder));
        var table=(byte*)NativeMemory.AllocZeroed(144); *(uint*)table=144; *(uint*)(table+4)=1;
        *(nint*)(table+136)=(nint)(delegate* unmanaged[Cdecl]<ulong,uint,int>)&RequestGroups;
        host.Services["kiln.gameplay"]=(nint)table;
        try { using var mods=NewManager(host); mods.PrepareGameplayHooks(); Assert.Equal(expected,_groups); }
        finally { NativeMemory.Free(table); _host=null; }
    }

    [Fact]
    public void Unreadable_helper_conservatively_prearms_all_groups_without_aborting_start()
    {
        var host=new FakeHost(); _host=host; _groups=0;
        InstallHello();
        File.WriteAllBytes(Path.Combine(ModsDir,"hello","BrokenHelper.dll"), [1,2,3]);
        var table=(byte*)NativeMemory.AllocZeroed(144); *(uint*)table=144; *(uint*)(table+4)=1;
        *(nint*)(table+136)=(nint)(delegate* unmanaged[Cdecl]<ulong,uint,int>)&RequestGroups;
        host.Services["kiln.gameplay"]=(nint)table;
        try { using var mods=NewManager(host); mods.PrepareGameplayHooks(); Assert.Equal(31u,_groups); }
        finally { NativeMemory.Free(table); _host=null; }
    }

    [Theory]
    [InlineData(136)] [InlineData(144)]
    public void Preflight_announces_demand_aware_host_even_without_any_mods(int size)
    {
        Assert.Equal(0u,GameplayBuildDemand.HostReady);
        var host=new FakeHost(); _host=host; _requests=_announcements=0;
        var table=(byte*)NativeMemory.AllocZeroed((nuint)size); *(uint*)table=(uint)size; *(uint*)(table+4)=1;
        *(nint*)(table+128)=(nint)(delegate* unmanaged[Cdecl]<ulong,int>)&Request;
        if(size==144) *(nint*)(table+136)=(nint)(delegate* unmanaged[Cdecl]<ulong,uint,int>)&RequestGroups;
        host.Services["kiln.gameplay"]=(nint)table;
        try { using var mods=NewManager(host); mods.PrepareGameplayHooks();
            Assert.Equal(size==144 ? 1 : 0,_announcements); Assert.Equal(0,_requests); }
        finally { NativeMemory.Free(table); _host=null; }
    }

    [Fact]
    public void Metadata_is_cached_across_mods_by_path_and_file_identity_only_within_one_pass()
    {
        InstallHello();
        var path=Path.Combine(ModsDir,"hello",Manifest.Read(Path.Combine(ModsDir,"hello")).EntryAssembly);
        var pass=new GameplayBuildDemand.MetadataPass();
        Assert.Equal(pass.References(path),pass.References(path)); Assert.Equal(1,pass.AssembliesRead);
        File.SetLastWriteTimeUtc(path,File.GetLastWriteTimeUtc(path).AddSeconds(2));
        pass.References(path); Assert.Equal(2,pass.AssembliesRead);
        var fresh=new GameplayBuildDemand.MetadataPass(); fresh.References(path); Assert.Equal(1,fresh.AssembliesRead);
    }
    [Fact]
    public void Preflight_logs_metadata_read_count_and_elapsed_time_even_for_an_empty_profile()
    {
        var host=new FakeHost(); using var mods=NewManager(host); mods.PrepareGameplayHooks();
        Assert.Contains(host.Lines,x=>x.Contains("gameplay pre-world demand: 0 assemblies read in ") && x.EndsWith(" ms"));
    }
    [Fact]
    public void Oversized_metadata_is_conservative_demand_without_reading_the_file()
    {
        InstallHello();
        using(var file=File.Create(Path.Combine(ModsDir,"hello","HugeHelper.dll"))) file.SetLength(64L*1024*1024+1);
        var host=new FakeHost(); _host=host; _groups=0;
        var table=(byte*)NativeMemory.AllocZeroed(144); *(uint*)table=144; *(uint*)(table+4)=1;
        *(nint*)(table+136)=(nint)(delegate* unmanaged[Cdecl]<ulong,uint,int>)&RequestGroups;
        host.Services["kiln.gameplay"]=(nint)table;
        try { using var mods=NewManager(host); mods.PrepareGameplayHooks(); Assert.Equal(31u,_groups); }
        finally { NativeMemory.Free(table); _host=null; }
    }

    [Fact]
    public void Shared_exports_are_read_once_across_all_admitted_mods_in_the_preflight()
    {
        InstallLibSet();
        var expected=Directory.EnumerateDirectories(ModsDir).SelectMany(d=>Directory.EnumerateFiles(d,"*.dll")).Count();
        var host=new FakeHost(); using var mods=NewManager(host); mods.PrepareGameplayHooks();
        Assert.Contains(host.Lines,x=>x.Contains($"gameplay pre-world demand: {expected} assemblies read in "));
    }

}
