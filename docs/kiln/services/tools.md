# Building tools

Since **0.2.0-dev.30.5**. `Kiln.Tools` supplies declarative terrain actions and owned material observations;
`Kiln.Blocks` supplies the tool, brush and material-pool declarations. Every participating game needs the same mod
(`clients: mod`). No game address, borrowed view or engine function belongs in a mod.

## Declarations

```csharp
var materials = Blocks.Define("loose-stone", new ToolMaterialPoolBlock {
    Name = "Loose stone",
    Entries = [new("Build_TerrainMaterial_T1_Rubble", 8, 16)],
});
var small = Blocks.Define("small", new TerrainBrushBlock {
    Name = "Small stone brush",
    Description = "A small sphere of loose stone.",
    Shape = TerrainBrushShape.Sphere,
    Radius = 0.5,
});
var tool = Blocks.Define("stone-tool", new BuildingToolBlock {
    Name = "Stone tool",
    Description = "Places and clears loose stone.",
    MaterialPool = materials,
    Brushes = [small],
    Primary = new TerrainToolAction { Mode = TerrainToolMode.Add },
    Secondary = new TerrainToolAction {
        Mode = TerrainToolMode.Remove,
        RemoveMaterials = [8],
        TerrainOnly = true,
    },
});
```

A pool contains **1–64 distinct items and distinct terrain cells** (1–127). `VoxelsPerItem` is 1–1,000,000;
placement pays the ceiling of actual added volume divided by that rate. It is not inferred from the material's mining
drop. Native compilation checks that each item actually places its declared terrain cell **and belongs to the
qualified vanilla `TerrainMaterials` pool**. A non-member is a compiler error, not a fallback to a broader pool.
A tool without an Add action may omit its material pool.

A brush is a sphere of radius **0.25–8 metres**. Its own item identity, name and description are separate from the
root tool. The tool owns its brush membership; declaring a brush part alone does not publish an orphan item into
vanilla Overgrowth's family. A family has at most 64 distinct brushes. `Identity = BlockIdentity.Legacy(guid)` or
`Legacy(itemId)` preserves an existing saved item; `Was` preserves the derived identity after a Kiln key rename.
`Icon` on the root accepts a PNG in the mod's folder.

The pool is an immutable **Kiln policy subset**, not a new game-visible pool row. Root and brush items reference the
existing vanilla terrain pool; the held tool's picker filters its rows to the policy subset. Registering a policy leaves
vanilla pool bytes, rows and magic-stock enumeration unchanged. Adding a second row of the same items would count
magic-chest stock twice, even with a vanilla tool held, so that lowering is forbidden.

The compiler also emits raw-fill brush entries in the game's blueprint registry, item definitions, and per-action
Impact programs and action sequences. It does not add terrain entries to `BuildingMaterialPoolBlock`. New table
metadata and brush definitions can be pending until the next game start. A pending definition is not proof that the
game has ingested or drawn it.

## Execution and safety

The game's building input and held-repeat cadence remain in charge. Policies are immutable native snapshots; no
managed delegate runs on a simulation worker. The authoritative game pipeline performs permissions, payment, mining
yields and history. Required hook mappings must all be qualified before a family action is admitted. Missing mappings
refuse; they never fall through to unrestricted digging or an independent inventory debit.

Root identity is derived from the root item's GUID and the same name/description text-tag purposes as ordinary Kiln
items. It does not borrow vanilla Overgrowth's identity. HUD material rows are compacted only in owned, bounded
storage, and their view is rebound to that storage after the game constructor returns. A stale selection, an empty
stock entry, a different tool, a different player or a different session is not a valid selection.

## Material observations

`ToolMaterials.TrySnapshot(actor, tool, out snapshot)` is read-only. A successful `ToolMaterialSnapshot` owns at most
64 immutable `ToolMaterialStock` entries, with backpack and connected magic-chest counts. It retains no game pointer.
A snapshot is **not authority** for payment or editing: a native invocation rechecks its current policy and game
supplies. An unavailable producer returns `false`; Kiln does not invent counts or return another player's cached view.

## Capability and qualification status

| Capability | Meaning | Refusal |
|---|---|---|
| `tools.actions` | The complete authoritative action pipeline is qualified. | Missing required hook/context, offline or no session. |
| `tools.material_picker` | Terrain group2 is filtered using an owned native held-tool observation; the game's aggregate supply counts are retained. | Missing selector/held-tool mapping; client-only presentation on a server. |
| `tools.material_snapshot` | A separate per-player backpack/magic-chest observation producer is qualified. | Currently unavailable: aggregate selector counts are never fabricated into two sources. |
| `tools.preview` | The explicit-item preview has an owned bounded row adapter. | Missing constructor mapping, wrong side or hooks disabled. |
| `tools.brush_families` | The brush query can distinguish an owning tool from vanilla families. | Missing query/held-tool context or wrong side. |
| `tools.identity` | The explicit-item root identity adapter is qualified. | Missing root helper, wrong side or hooks disabled. |

The native client-Post observer copies the actual main-hand/action selection into an owned scalar snapshot tagged by
actor, owner, generation, session and step. A missing, changed or older-than-one-step sample does not admit a tool.
It filters only terrain group2, leaving all six other categories unchanged. The qualified preview uses each call's
explicit item, independently of that held observation.

**Not yet established:** brush-query family context and the per-actor producer that separates backpack and magic-chest
observations. Their capabilities remain unavailable until their mappings are qualified. Static qualification,
filtering fixtures and native runner tests do not establish actual menu rendering, network behavior or the complete
real-game tool. The item-presentation constructor's traced caller uses the selected hotbar model, which need not be the
running action's item; its last item must never be cached as evidence of what the player holds.

Only the player runs the real-game check: confirm root/brush names, independent vanilla Overgrowth choices, all pool
entries with backpack-only and magic-chest-only supplies, empty stock, switching between tools, inventory changes,
selection after leaving/rejoining a world, paid placement/removal and the game's normal/remapped undo.
