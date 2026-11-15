// SPDX-License-Identifier: MIT
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using Xunit;
using WorldProgramsQuota;
namespace Kiln.Contract.Tests;
public sealed unsafe class QuotaFixtureTests : FakeTableTest
{
    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static uint Caps() => 4;
    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Status(WorldNative.Status* status) {var size=status->Size;*status=default;status->Size=size;return 0;}
    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Bind(ulong owner,NativeStr mod,NativeStr key,delegate* unmanaged[Cdecl]<void*,ulong*,uint,void*,void> call,void* user,ulong* handle) => 1;

    private void Exercise(string path,bool expectFailure)
    {
        // Start capability discovery against the empty backend, before exposing a deliberately partial program table.
        _ = World.Status;
        var table=Alloc<EcsWorldTable>();
        table->Prefix.Header=new ServiceHeader {Size=(uint)sizeof(EcsWorldTable),Version=2};
        table->Capabilities=&Caps;table->Bind=&Bind;table->Status=(nint)(delegate* unmanaged[Cdecl]<WorldNative.Status*,int>)&Status;
        Swap(typeof(WorldNative),"_table",Box((WorldNative.Table*)table));
        Backend.Tables["kiln.world"]=(nint)table;
        var ctx=HostAccess.CreateContext(new ModInfo("world-programs-quota-a","quota","1","."),88,path);
        var mod=new QuotaMod();HostAccess.Bind(mod,ctx);HostAccess.Current=ctx;
        try
        {
            Assert.Null(Record.Exception(mod.OnLoad));
            if(expectFailure) Assert.Contains(Backend.Lines,x=>x.Contains("FAIL quota proof",StringComparison.Ordinal));
            else Assert.True(Directory.Exists(path));
            Assert.Null(Record.Exception(mod.OnUnload));
        }
        finally {ctx.Retire();HostAccess.Current=Mod;}
    }
    [Fact]
    public void A_fresh_data_folder_does_not_throw_during_OnLoad()
    {
        var path=Path.Combine(Path.GetTempPath(),"kiln-quota-"+Guid.NewGuid());
        try {Exercise(path,false);} finally {if(Directory.Exists(path))Directory.Delete(path,true);}
    }
    [Fact]
    public void Proof_IO_failure_is_logged_as_a_failed_row_without_refusing_the_mod()
    {
        var path=Path.Combine(Path.GetTempPath(),"kiln-quota-"+Guid.NewGuid());File.WriteAllText(path,"a file blocks the data folder");
        try {Exercise(path,true);} finally {File.Delete(path);}
    }
}
