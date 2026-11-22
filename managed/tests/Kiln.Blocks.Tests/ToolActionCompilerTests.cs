// SPDX-License-Identifier: MIT
using System.Text.Json.Nodes;
using Xunit;
namespace Kiln.BlockTests;
public sealed class ToolActionCompilerTests
{
    private static readonly TerrainToolAction Add = new() { Mode = TerrainToolMode.Add };
    private static readonly TerrainToolAction Remove = new() { Mode = TerrainToolMode.Remove, RemoveMaterials = [8, 91] };
    private static (ModBlocks Mod, BlockContext Context, Lowered Low) Fixture()
    {
        var mod = new ModBlocks("tool-action-test", 1) { ManifestClients = "mod" };
        return (mod, new BlockContext(mod, "tool"), new Lowered(mod));
    }
    [Fact] public void EmitsStableDistinctProgramsAndOnlyReplacesTheTerrainImpact()
    {
        var f=Fixture();
        var plan=Compiler.LowerToolActions(f.Context,f.Low,"shaper","Shaper",Add,Remove);
        Assert.Equal(BlockIds.Derive(f.Mod.Id,"program","shaper.primary"),plan.PrimaryProgram);
        Assert.Equal(BlockIds.Derive(f.Mod.Id,"program","shaper.secondary"),plan.SecondaryProgram);
        Assert.NotEqual(plan.PrimaryProgram,plan.SecondaryProgram);
        Assert.Equal(2,f.Mod.Programs.Count);
        var sequences=f.Low.Json["sequences"]!.AsArray(); Assert.Equal(2,sequences.Count);
        Assert.Equal("Building_Overgrowth_Add",(string?)sequences[0]!["base"]);
        Assert.Equal("Building_Overgrowth_Remove",(string?)sequences[1]!["base"]);
        foreach(var s in sequences)
        {
            var fields=s!["fields"]!.AsObject();Assert.Single(fields);
            Assert.True(fields.ContainsKey("events[SpawnImpact#1].impact"));
        }
    }
    [Fact] public void DefaultProgramIsInertAndBindsUsedItemAndBrushRadius()
    {
        var f=Fixture(); var p=Compiler.LowerToolActions(f.Context,f.Low,"shaper","Shaper",Add,Remove);
        var blob=f.Mod.Programs[p.PrimaryProgram].Blob;
        var layout=0x28+BitConverter.ToInt32(blob,0x28);var count=BitConverter.ToInt32(blob,0x2c);
        var data=0x30+BitConverter.ToInt32(blob,0x30);bool used=false,range=false,poison=false,damage=false;
        for(int i=0;i<count;i++)
        {
            int at=layout+i*24;uint config=BitConverter.ToUInt32(blob,at+4),type=BitConverter.ToUInt32(blob,at+8);int off=BitConverter.ToUInt16(blob,at+14);
            if(config==0x220e6b10){used=true;Assert.Equal(0x5bfc0dbbu,type);}
            if(config==0x5258131e){range=true;Assert.Equal(0xe89f7410u,type);}
            if(type==0x4835c9e9){poison=true;Assert.Equal(255,blob[data+off]);Assert.Equal(0u,config);}
            if(type==0xc4b5300b){damage=true;Assert.All(blob.Skip(data+off).Take(20),x=>Assert.Equal(0,x));}
        }
        Assert.True(used&&range&&poison&&damage);
    }
    [Fact] public void ItemBindingsPreserveBuildingInputRepeatAndPrimarySecondarySlots()
    {
        var f=Fixture();var plan=Compiler.LowerToolActions(f.Context,f.Low,"shaper","Shaper",Add,Remove);
        var item=new ItemDefinition{Guid=Guid.NewGuid(),Name="brush",BaseOn="Building_Overgrowth_Shape_Small"};
        var bound=Compiler.ApplyToolActions(item,plan);
        Assert.True((bool)bound.Fields!["equipment.repeatActionWhenPressed"]);
        Assert.Equal(plan.SecondarySequence.ToString(),bound.Refs!["sequences[0].sequence"]);
        Assert.Equal(plan.PrimarySequence.ToString(),bound.Refs!["sequences[1].sequence"]);
        Assert.Null(item.Refs);
    }
    [Theory] [InlineData(0)] [InlineData(128)] [InlineData(255)]
    public void RefusesNonTerrainRemovalBeforeAnyDefinitionsAreEmitted(byte cell)
    {
        var f=Fixture();
        Assert.Throws<BlockError>(()=>Compiler.LowerToolActions(f.Context,f.Low,"shaper","Shaper",Add,Remove with {RemoveMaterials=[cell]}));
        Assert.Empty(f.Mod.Programs);Assert.Empty(f.Low.Json);
    }
    [Fact] public void RefusesBuildingTargetsAndUnknownModes()
    {
        var f=Fixture();
        Assert.Throws<BlockError>(()=>Compiler.LowerToolActions(f.Context,f.Low,"shaper","Shaper",Add,Remove with {TerrainOnly=false}));
        Assert.Throws<BlockError>(()=>Compiler.LowerToolActions(f.Context,f.Low,"shaper","Shaper",Add with {Mode=(TerrainToolMode)99},Remove));
    }
}
