// SPDX-License-Identifier: MIT
using System.Security.Cryptography;
using System.Text;
using System.Text.Json.Nodes;
namespace Kiln.TestKit;

internal sealed record TerrainPoolFakeItem(uint Item,byte Cell);
internal sealed record TerrainBrushFakeShape(Guid Item,int Side,byte[] Cells,string Canonical);

internal sealed partial class PoolFakeState
{
    // The fixture records its shipped client build, registry resource and decode fields. It is data, not a
    // substitute pool: the 56 vanilla IDs stay immutable even when a test scripts another placeable item.
    internal static readonly JsonObject TerrainFixture=ReadTerrainFixture();
    internal static readonly HashSet<uint> VanillaTerrainItems=TerrainFixture["items"]!.AsArray().Select(i=>(uint)i!["item"]!).ToHashSet();
    internal readonly Dictionary<string,TerrainPoolFakeItem> TerrainItems=TerrainCatalog();
    internal readonly Dictionary<string,JsonObject> ToolDefinitions=new(StringComparer.Ordinal);
    internal readonly Dictionary<string,TerrainBrushFakeShape> ToolBrushes=new(StringComparer.Ordinal);
    internal bool RecipeSinkInstalled;
    private static JsonObject ReadTerrainFixture()
    {
        using var stream=typeof(PoolFakeState).Assembly.GetManifestResourceStream("Kiln.TestKit.Fakes.WorldTables.TerrainPool.json")!;
        return JsonNode.Parse(stream)!.AsObject();
    }
    private static Dictionary<string,TerrainPoolFakeItem> TerrainCatalog()
    {
        var result=new Dictionary<string,TerrainPoolFakeItem>(StringComparer.Ordinal);
        foreach(var row in TerrainFixture["items"]!.AsArray()) {
            var item=(uint)row!["item"]!;var entry=new TerrainPoolFakeItem(item,(byte)row["cell"]!);
            result[(string)row["name"]!]=entry;result[item.ToString(System.Globalization.CultureInfo.InvariantCulture)]=entry;
        }
        return result;
    }
    internal JsonObject ToolDocument(string mod)
    {
        if(!ToolDefinitions.TryGetValue(mod,out var doc))ToolDefinitions[mod]=doc=new() { ["format"]=1,["mod"]=mod };
        return doc;
    }
    private void RememberTool(string mod,string section,string key,JsonObject value)
    {
        var doc=ToolDocument(mod);if(doc[section] is not JsonObject entries)doc[section]=entries=new();
        entries[key]=value.DeepClone();
    }
    internal void DefineToolPools(string mod,JsonObject pools,JsonArray blocks,JsonArray errors)
    {
        foreach(var (key,value) in pools) {
            var label=$"block toolMaterialPool {key} (mod {mod}): ";
            if(value is not JsonObject node || !MaterialPoolValidation.Valid(key) || node.Any(f=>f.Key is not ("name" or "was" or "entries"))) { errors.Add(label+"expected a named pool object");continue; }
            ToolMaterialPoolBlock block;
            try { block=BlockJson.Parse<ToolMaterialPoolBlock>(node.ToJsonString());ToolBlockValidation.PoolNode(block); }
            catch(ArgumentException e) {errors.Add(label+e.Message);continue;}
            var items=new HashSet<uint>();string? problem=null;
            for(var i=0;i<block.Entries.Count;i++) {
                var entry=block.Entries[i];var name=entry.Item.Value;
                if(Guid.TryParse(name,out var itemGuid))name=BlockIds.Fnv(itemGuid).ToString(System.Globalization.CultureInfo.InvariantCulture);
                if(!TerrainItems.TryGetValue(name,out var item) || item.Item==0)problem=$"entries[{i}]: no unambiguous placeable terrain item {name}";
                else if(item.Cell!=entry.TerrainMaterial)problem=$"entries[{i}]: item does not place the declared terrain material";
                else if(!VanillaTerrainItems.Contains(item.Item))problem=$"entries[{i}].item: {name} is not a member of the qualified vanilla TerrainMaterials pool; tool pools are policy subsets, not new game rows";
                else if(!items.Add(item.Item))problem=$"materials[0] (gameMaterial ) and materials[{i}] (gameMaterial ) are the same building item {item.Item}";
                if(problem is not null)break;
            }
            var guid=BlockIds.Derive(mod,"toolmaterialpool",block.Was??key);var canonical=Canonical(node);
            if(Readiness!=CapacityReadiness.Ready)problem="the boot tables are not ready";
            if(OriginalIds.Contains(BlockIds.Fnv(guid)) || Served.Values.Any(p=>p.Guid!=guid && BlockIds.Fnv(p.Guid)==BlockIds.Fnv(guid)))problem="pool id collision";
            if(problem is not null) {errors.Add(label+problem);continue;}
            var id=DefinitionRegistration.DeriveId(mod,DefinitionRegistrationKind.BuildingMaterialPool,guid,SHA256.HashData(Encoding.UTF8.GetBytes(canonical)));
            var receipt=new DefinitionRegistrationResult(true,null,DefinitionRefusalCode.None,id,guid,DefinitionRegistrationKind.BuildingMaterialPool,DefinitionRegistrationState.AcceptedNextStart,0,Revision,[]);
            receipt=Commit(receipt,mod,canonical);
            if(!receipt.Ok) {errors.Add(label+receipt.Reason);continue;}
            RememberTool(mod,"toolMaterialPools",key,node);
            blocks.Add(new JsonObject { ["name"]=key,["kind"]="toolMaterialPool",["guid"]=guid.ToString(),["status"]="pending" });
        }
    }
    internal void DefineToolBrushes(string mod,JsonObject shapes,JsonArray blocks,JsonArray errors)
    {
        foreach(var (key,value) in shapes) {
            var label=$"block terrainBrush {key}: ";
            if(!MaterialPoolValidation.Valid(key) || value is not JsonObject node || node.Count!=3 ||
                node["item"] is not JsonValue item || !item.TryGetValue<string>(out var text) || !Guid.TryParse(text,out var guid) || guid==Guid.Empty ||
                node["shape"] is not JsonValue shape || !shape.TryGetValue<string>(out var kind) || kind!="sphere" ||
                node["radius"] is not JsonValue radiusNode || !radiusNode.TryGetValue<double>(out var radius)) {
                errors.Add(label+"expected item GUID, sphere shape and radius");continue;
            }
            if(!double.IsFinite(radius) || radius is <0.25 or >8) {errors.Add(label+"radius is 0.25..8 metres; requires clients: mod");continue;}
            if(StorageFails) {errors.Add(label+"could not persist the brush definition");continue;}
            var side=(int)Math.Ceiling(radius*4);var cells=new byte[side*side*side];
            for(var z=0;z<side;z++)for(var y=0;y<side;y++)for(var x=0;x<side;x++) {
                var dx=(x+0.5-side/2.0)*0.5;var dy=(y+0.5-side/2.0)*0.5;var dz=(z+0.5-side/2.0)*0.5;
                if(dx*dx+dy*dy+dz*dz<=radius*radius)cells[x+side*(y+side*z)]=255;
            }
            var canonical=Canonical(node);ToolBrushes[mod+":"+key]=new(guid,side,cells,canonical);Pending[guid]=canonical;
            RememberTool(mod,"toolBrushShapes",key,node);
            blocks.Add(new JsonObject { ["name"]=key,["kind"]="terrainBrush",["item"]=guid.ToString(),["status"]="pending" });
        }
    }
}
