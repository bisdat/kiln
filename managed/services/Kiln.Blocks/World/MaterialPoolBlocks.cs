// SPDX-License-Identifier: MIT
#pragma warning disable KILNX028
using System.Text.Json.Nodes;
namespace Kiln;

/// <summary>One selector entry. Set exactly one named material reference or game building-material catalog name.</summary>
/// <param name="Material">A building material of this mod by its name, or <c>"other-mod:name"</c>. Set this or <paramref name="GameMaterial"/>, not both.</param>
/// <param name="GameMaterial">A building material of the game, by its catalog name. Set this or <paramref name="Material"/>, not both.</param>
public sealed record BuildingMaterialPoolEntry(PartRef<BuildingMaterialBlock>? Material = null, string? GameMaterial = null);

/// <summary>A boot-time pool of 1..64 building-material items for opt-in new blueprint shapes. It consumes no material IDs.
/// Registry ingestion and build-menu behavior require the 17-material-pools game check; host tests do not prove rendering.</summary>
[BlockClients(ClientsLevel.Mod)]
public sealed record BuildingMaterialPoolBlock : IBlock
{
    /// <summary>The declaration's readable name. The registry debug name remains mod:key.</summary>
    public required string Name { get; init; }
    /// <summary>One to 64 distinct live building-material block items, in selector order.</summary>
    public required IReadOnlyList<BuildingMaterialPoolEntry> Materials { get; init; }
    /// <summary>Uses the roof equipment slot; every member and bound shape must agree.</summary>
    public bool Roof { get; init; }
    /// <summary>The old key after a rename. It preserves the pool GUID and ID.</summary>
    public string? Was { get; init; }
}

internal static class MaterialPoolValidation
{
    internal static void Check(BuildingMaterialPoolBlock block)
    {
        ArgumentNullException.ThrowIfNull(block);
        if(string.IsNullOrWhiteSpace(block.Name)) throw new ArgumentException("A pool needs a name.",nameof(block));
        if(block.Materials is not {Count:>=1 and <=64}) throw new ArgumentException("A pool takes 1..64 materials.",nameof(block));
        foreach(var entry in block.Materials)
            if(entry is null || (entry.Material is null)==(entry.GameMaterial is null) || entry.GameMaterial is {Length:0}
                || entry.Material is {Name:null}) throw new ArgumentException("Each entry takes exactly one named material reference or gameMaterial.",nameof(block));
        if(block.Was is { } was && !Valid(was)) throw new ArgumentException("Invalid pool Was.",nameof(block));
    }
    internal static bool Valid(string name)=>name.Length>0 && name.All(c=>char.IsAsciiLetterOrDigit(c)||c is '-' or '_' or '.');
}

internal static partial class Compiler
{
    internal static JsonObject MaterialPoolNode(string mod, BuildingMaterialPoolBlock block)
    {
        MaterialPoolValidation.Check(block);
        var entries=new JsonArray();
        foreach(var e in block.Materials) {
            if(e.Material is {} material) {
                var key=material.Name!;
                var parts=key.Split(':');
                if(parts.Length>2 || parts.Any(p=>!MaterialPoolValidation.Valid(p))) throw new ArgumentException("Invalid material reference.",nameof(block));
                entries.Add(new JsonObject {["material"]=key.Contains(':')?key:mod+":"+key});
            } else entries.Add(new JsonObject {["gameMaterial"]=e.GameMaterial});
        }
        var node=new JsonObject {["name"]=block.Name,["roof"]=block.Roof,["materials"]=entries};
        if(block.Was is {} was) node["was"]=was;
        return node;
    }
    private static BlockHandle DefineMaterialPool(BlockContext ctx, Lowered low,string name,BuildingMaterialPoolBlock block)
    {
        ctx.Need(ClientsLevel.Mod,null,"a material pool must match in every player's game");
        var node=MaterialPoolNode(ctx.Mod.Id,block);
        WorldTablesNative.RequirePoolWriter();
        TableBlockStatus status;int id=-1;
        if(WorldTablesNative.Baking || WorldTablesNative.TestDefine is not null) (status,id)=AskTables(ctx,low,"buildingMaterialPools",name,node);
        else {
            var document=new JsonObject {["format"]=1,["mod"]=ctx.Mod.Id,["clients"]="mod",["buildingMaterialPools"]=new JsonObject {[name]=node.DeepClone()}};
            var result=WorldTablesNative.DefinePool(ctx.Mod.Owner,name,document.ToJsonString(),DefinitionTiming.AllowNextStart);
            if(!result.Ok)throw new BlockError(ctx.Block,"materialPool",result.Reason??result.Code.ToString());
            status=result.State==DefinitionRegistrationState.Registered?TableBlockStatus.Registered:TableBlockStatus.Pending;
            low.Json["status"]=status==TableBlockStatus.Registered?"registered":"pending";
        }
        low.Json["materialPool"]=node.DeepClone();
        if(!WorldTablesNative.Baking)WorldTablesEvents.Enable();
        return new TableBlockHandle<BuildingMaterialPoolBlock>(ctx.Mod.Id,name,BlockIds.Derive(ctx.Mod.Id,"materialpool",block.Was??name),ctx.Mod.Id+":"+name,block,status,id);
    }
}
