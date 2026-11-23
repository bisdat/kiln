// SPDX-License-Identifier: MIT
#pragma warning disable KILNX028
namespace Kiln.TestKit;
public sealed partial class KilnHarness
{
    /// <summary>Scripts boot pool snapshots and registration refusals. It does not model the game's selector or registry ingestion.</summary>
    public HarnessWorldTables WorldTables=>new(this);
    internal bool PoolOwnerActive(ulong owner){lock(_host.Revoked)return !_host.Revoked.Contains(owner);}
}
/// <summary>Scripted process boot data, not a game or renderer. New API definitions still meet a closed registry.</summary>
public sealed class HarnessWorldTables
{
    private readonly KilnHarness _h;
    internal HarnessWorldTables(KilnHarness h)=>_h=h;
    private PoolFakeState State=>KitFakes.State<PoolFakeState>(_h);
    public int PendingCount {get{lock(State.Gate)return State.Pending.Count;}}
    /// <summary>Scripts a placeable terrain item's item-to-cell mapping. It does not add membership to the immutable
    /// vanilla TerrainMaterials pool, so a non-member is still refused. Since 0.2.0-dev.30.5.</summary>
    public void TerrainItem(string name,uint item,byte cell)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(name);if(item==0 || cell is <1 or >127)throw new ArgumentException("A terrain item needs a nonzero item and cell 1..127.");
        lock(State.Gate){var entry=new TerrainPoolFakeItem(item,cell);State.TerrainItems[name]=entry;State.TerrainItems[item.ToString(System.Globalization.CultureInfo.InvariantCulture)]=entry;}
    }
    /// <summary>A copy of the accepted pending policy-pool and computed brush descriptors, in world-table JSON form.
    /// This is declaration recording, not live native tool registration. Since 0.2.0-dev.30.5.</summary>
    public string PendingToolDefinitionsJson(string mod){lock(State.Gate)return State.ToolDocument(mod).ToJsonString();}
    /// <summary>A copy of a pending sphere descriptor's raw fill bytes (x fastest, then y, then z), sampled at half-metre
    /// voxel centres as in the boot compiler. Null: no accepted descriptor. Not a live game resource. Since 0.2.0-dev.30.5.</summary>
    public byte[]? TerrainBrushCells(string mod,string key){lock(State.Gate)return State.ToolBrushes.TryGetValue(mod+":"+key,out var shape)?(byte[])shape.Cells.Clone():null;}
    public bool StorageFails {get{lock(State.Gate)return State.StorageFails;}set{lock(State.Gate)State.StorageFails=value;}}
    public CapacityReadiness Readiness {get{lock(State.Gate)return State.Readiness;}set{lock(State.Gate)State.Readiness=value;}}
    /// <summary>Configures one actual served boot row. Validation and entry counts match pool admission; this never claims UI ingestion.</summary>
    public void ServePool(string mod,string key,BuildingMaterialPoolBlock block)=>_h.Run(()=>{
        lock(State.Gate){var checkedPool=State.Validate(mod,block);if(checkedPool.Code!=DefinitionRefusalCode.None)throw new ArgumentException(checkedPool.Reason);var guid=BlockIds.Derive(mod,"materialpool",block.Was??key);if(State.OriginalIds.Contains(BlockIds.Fnv(guid)) || State.Served.Values.Any(p=>p.Guid!=guid && BlockIds.Fnv(p.Guid)==BlockIds.Fnv(guid)))throw new ArgumentException("pool id collision");State.Served[mod+":"+key]=new(mod,key,guid,checkedPool.Canonical,checkedPool.Items,block.Roof);var receipt=State.Prepare(mod,key,block,DefinitionTiming.RequireRegistered,true);State.Commit(receipt,mod,checkedPool.Canonical);}
        // Fixture boot rows are scripted after the harness has started its services. Re-run the real startup
        // reporter against that boot snapshot; bypassing it would hide missing production owner observation.
        WorldTablesEvents.Reset();BlocksRows.Rows.Refresh();
    });
    /// <summary>Configures a live or retained building/terrain material and its block item. A pool cannot use terrain, retained materials or item 0.</summary>
    public void Material(string mod,string key,uint item,bool roof=false,bool live=true,bool terrain=false){lock(State.Gate)State.Materials[mod+":"+key]=new(item,roof,live,terrain);}
    public void GameMaterial(string name,uint item,bool roof=false,bool terrain=false){lock(State.Gate)State.Game[name]=new(item,roof,true,terrain);}
    public void Dependency(string mod,string dependency){lock(State.Gate){if(!State.Dependencies.TryGetValue(mod,out var deps))State.Dependencies[mod]=deps=[];deps.Add(dependency);}}
    /// <summary>Models an occupied foreign pool ID so collision refusal can be tested without searching for an FNV collision.</summary>
    public void ReservePoolId(uint id){lock(State.Gate)State.OriginalIds.Add(id);}
}
