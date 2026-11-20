// SPDX-License-Identifier: MIT
using Kiln;
using MaterialPoolsTest;
using Xunit;
namespace Kiln.Contract.Tests;
public sealed class MaterialPoolGroundTests
{
    private static CellBox Box(WorldPosition p)=>new((int)p.X,(int)p.Y,(int)p.Z,1,1,1);
    [Fact] public async Task Reads_all_candidates_and_skips_occupied_cells()
    {
        int reads=0;
        var boxes=await PoolGround.Find(3,[0,0,0,12,4,12],null,Box,b=>{++reads;return Task.FromResult(new[]{reads==1?Voxel.Of(128,255):default});});
        Assert.Equal(3,boxes.Count);Assert.Equal(4,reads);Assert.Equal(3,boxes.Distinct().Count());Assert.DoesNotContain(new CellBox(1,1,1,1,1,1),boxes);
    }
    [Fact] public async Task No_room_refuses_before_any_writes_can_be_requested()
    {
        var e=await Assert.ThrowsAsync<InvalidOperationException>(()=>PoolGround.Find(3,[0,0,0,4,3,4],null,Box,_=>Task.FromResult(new[]{Voxel.Of(128,255)})));
        Assert.Contains("base has no room",e.Message);
    }
    [Fact] public async Task Override_is_read_and_refused_when_occupied()
    {
        int reads=0;
        await Assert.ThrowsAsync<InvalidOperationException>(()=>PoolGround.Find(2,[0,0,0,12,4,12],[2,2,2],Box,_=>Task.FromResult(new[]{++reads==2?Voxel.Of(128,255):default})));
        Assert.Equal(2,reads);
    }
    [Fact] public async Task Empty_or_partial_reads_cannot_prove_clear_ground()
    {
        await Assert.ThrowsAsync<InvalidOperationException>(()=>PoolGround.Find(1,[0,0,0,4,3,4],null,Box,_=>Task.FromResult(Array.Empty<Voxel>())));
    }
}
