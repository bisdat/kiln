#pragma warning disable KILNX028
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using Xunit;
namespace Kiln.Blocks.Tests;
[Collection("item icons")]
public sealed class MaterialPoolRegistrationTests
{
    [Fact]
    public void Abi_appends_preserve_the_shipped_prefix()
    {
        Assert.Equal(176,Marshal.SizeOf<WorldTablesNative.Table>());
        Assert.Equal(144,Marshal.OffsetOf<WorldTablesNative.Table>(nameof(WorldTablesNative.Table.MaterialBudget)).ToInt32());
        Assert.Equal(152,Marshal.OffsetOf<WorldTablesNative.Table>(nameof(WorldTablesNative.Table.PoolBudget)).ToInt32());
        Assert.Equal(160,Marshal.OffsetOf<WorldTablesNative.Table>(nameof(WorldTablesNative.Table.DefinePoolResult)).ToInt32());
        Assert.Equal(168,Marshal.OffsetOf<WorldTablesNative.Table>(nameof(WorldTablesNative.Table.PoolRegistration)).ToInt32());
        Assert.True(WorldTablesNative.HasAppend(144,136));
        Assert.False(WorldTablesNative.HasAppend(144,144));
        Assert.True(WorldTablesNative.HasAppend(152,144));
        Assert.False(WorldTablesNative.HasAppend(152,152));
    }
    [Fact]
    public void Pool_queries_accept_existing_block_name_rules_without_an_extra_length_budget()
    {
        Assert.True(WorldTables.PoolName("mod:"+new string('a',200)));
        Assert.True(WorldTables.PoolName("mod:."));
    }
    [Fact]
    public void Malformed_queries_throw_before_the_bridge()
    {
        Assert.Throws<ArgumentOutOfRangeException>(()=>WorldTables.GetMaterialBudget((WorldMaterialCategory)99));
        Assert.Throws<ArgumentException>(()=>WorldTables.GetPoolBudget("bad pool"));
        Assert.Throws<ArgumentException>(()=>WorldTables.GetPoolBudget("a:b:c"));
        Assert.Throws<ArgumentException>(()=>WorldTables.GetMaterialPoolRegistration(Guid.Empty));
    }
    [Fact]
    public void Missing_appends_name_the_capability()
    {
        Assert.Equal("worldtables.budgets",Assert.Throws<KilnUnavailableException>(()=>WorldTables.GetMaterialBudget(WorldMaterialCategory.Building)).Capability);
        Assert.Equal("worldtables.material_pools",Assert.Throws<KilnUnavailableException>(()=>WorldTables.GetMaterialPoolRegistration(Guid.NewGuid())).Capability);
    }
    [Fact]
    public void Explicit_options_validate_before_native_or_pending_storage()
    {
        var mod=new ModBlocks("pools",1){ManifestClients="mod"};
        var block=new BuildingMaterialPoolBlock{Name="Pool",Materials=[new(GameMaterial:"WoodRough")]};
        Assert.Throws<ArgumentNullException>(()=>Compiler.DefinePoolResult(mod,"p",block,null!));
        Assert.Throws<ArgumentOutOfRangeException>(()=>Compiler.DefinePoolResult(mod,"p",block,new((DefinitionTiming)99)));
        Assert.Throws<ArgumentException>(()=>Compiler.DefinePoolResult(mod,"p",block with {Materials=[]},new()));
    }
    // On the 26430253 server a pool whose two entries were one building item came back to its author as "The worldtables
    // request is malformed.": native knew which entries, the bridge dropped it. A bad argument carries native's reason.
    [Fact]
    public unsafe void A_refused_argument_names_what_native_named()
    {
        using var native=new RefusingTable();
        var mod=new ModBlocks("pools",1){ManifestClients="mod"};
        var block=new BuildingMaterialPoolBlock{Name="Pool",Materials=[new(GameMaterial:"StoneRough"),new(GameMaterial:"StoneRough")]};
        var e=Assert.Throws<ArgumentException>(()=>Compiler.DefinePoolResult(mod,"p",block,new()));
        Assert.Equal(RefusingTable.Reason,e.Message);
        Assert.Equal("The worldtables request is malformed.",Assert.Throws<ArgumentException>(()=>WorldTablesNative.ResultCode(1,"worldtables.material_pools")).Message);
    }

    private sealed unsafe class RefusingTable:IDisposable
    {
        internal const string Reason="block buildingMaterialPool p (mod pools): materials[0] (gameMaterial StoneRough) and materials[1] (gameMaterial StoneRough) are the same building item 2175686957";
        private static readonly FieldInfo Table=typeof(WorldTablesNative).GetField("_table",BindingFlags.Static|BindingFlags.NonPublic)!;
        private static readonly FieldInfo Looked=typeof(WorldTablesNative).GetField("_looked",BindingFlags.Static|BindingFlags.NonPublic)!;
        private readonly object? _table,_looked;
        private readonly WorldTablesNative.Table* _fake;
        internal RefusingTable()
        {
            _table=Table.GetValue(null);_looked=Looked.GetValue(null);
            _fake=(WorldTablesNative.Table*)NativeMemory.AllocZeroed((nuint)sizeof(WorldTablesNative.Table));
            _fake->Size=(uint)sizeof(WorldTablesNative.Table);_fake->Version=1;
            _fake->MaterialBudget=&Budget;_fake->PoolBudget=&PoolBudget;_fake->DefinePoolResult=&Define;_fake->PoolRegistration=&Registration;
            Table.SetValue(null,Pointer.Box(_fake,typeof(WorldTablesNative.Table*)));Looked.SetValue(null,true);
        }
        public void Dispose(){Table.SetValue(null,_table);Looked.SetValue(null,_looked);NativeMemory.Free(_fake);}
        [UnmanagedCallersOnly(CallConvs=[typeof(CallConvCdecl)])]
        private static int Budget(uint category,KilnCapacityInfo* info)=>3;
        [UnmanagedCallersOnly(CallConvs=[typeof(CallConvCdecl)])]
        private static int PoolBudget(WorldTablesNative.Str name,KilnCapacityInfo* info)=>3;
        [UnmanagedCallersOnly(CallConvs=[typeof(CallConvCdecl)])]
        private static int Registration(ulong owner,byte* id,KilnDefinitionReply* reply,byte* report,nuint capacity,nuint* length)=>3;
        // as native's t_define_pool: the length always, the text when it fits, 1 (invalid argument) either way
        [UnmanagedCallersOnly(CallConvs=[typeof(CallConvCdecl)])]
        private static int Define(ulong owner,WorldTablesNative.Str name,WorldTablesNative.Str document,uint timing,KilnDefinitionReply* reply,byte* report,nuint capacity,nuint* length)
        {
            var bytes=System.Text.Encoding.UTF8.GetBytes(Reason);*length=(nuint)bytes.Length;
            if(report==null || capacity<=(nuint)bytes.Length)return 5;
            bytes.CopyTo(new Span<byte>(report,(int)capacity));report[bytes.Length]=0;return 1;
        }
    }
}
