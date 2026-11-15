// SPDX-License-Identifier: MIT
using Kiln;
namespace MaterialPoolsTest;
internal static class PoolGround
{
    // Bounds in metres, including the vertical search range above the lab base's ground.
    internal static readonly double[] DefaultBase=[3006,902.6,1530,3042,906.6,1570];
    internal static async Task<List<CellBox>> Find(int count,double[] bounds,double[]? position,
        Func<WorldPosition,CellBox> boxAround,Func<CellBox,Task<Voxel[]>> read)
    {
        if(count<1 || bounds.Length!=6 || bounds.Any(v=>!double.IsFinite(v)) ||
            bounds[3]-bounds[0]<2 || bounds[4]-bounds[1]<2 || bounds[5]-bounds[2]<2 ||
            position is not null && (position.Length!=3 || position.Any(v=>!double.IsFinite(v))))
            throw new InvalidDataException("base box needs six finite ordered coordinates with at least 2 m per side; position needs three");
        var found=new List<CellBox>();
        async Task<bool> Take(WorldPosition p) {
            var box=boxAround(p);var cells=await read(box);
            if(cells.LongLength!=box.Count || cells.Any(v=>!v.IsAir))return false;
            if(found.Any(b=>box.X<b.X+b.SizeX && b.X<box.X+box.SizeX && box.Y<b.Y+b.SizeY && b.Y<box.Y+box.SizeY && box.Z<b.Z+b.SizeZ && b.Z<box.Z+box.SizeZ))return false;
            found.Add(box);return true;
        }
        if(position is not null) {
            for(int i=0;i<count;++i)if(!await Take(new(position[0]+3*i,position[1],position[2])))
                throw new InvalidOperationException("base has no room at --position: occupied or unreadable cells; nothing written");
        } else {
            // Bound the work independently of a supplied box's size. Typical default: at most 1,536 reads.
            int inspected=0;
            for(double y=bounds[1]+1;y<=bounds[4]-1 && found.Count<count;y+=1)
                for(double z=bounds[2]+1;z<=bounds[5]-1 && found.Count<count;z+=2)
                    for(double x=bounds[0]+1;x<=bounds[3]-1 && found.Count<count;x+=2) {
                        if(++inspected>4096)throw new InvalidOperationException("base has no room within 4096 reads; nothing written");
                        await Take(new(x,y,z));
                    }
            if(found.Count!=count)throw new InvalidOperationException("base has no room for all test cells; nothing written");
        }
        return found;
    }
}
