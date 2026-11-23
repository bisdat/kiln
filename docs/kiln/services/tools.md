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
vanilla Overgrowth's family. A family has at most 64 distinct brushes; the game's picker shows the first **16**, in
the order the tool lists them (its brush list is a fixed 16-row array). Each brush item is based on vanilla
`Building_Overgrowth_Shape_Small`; its radius is that item's own impact config `0x5258131e` (BrushRadius, the value the
tool's programs read), written as the item rule `impactValues.0x5258131e`. `Identity = BlockIdentity.Legacy(guid)` or
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
| `tools.material_lookup` | The installed invocation-scoped lookup refuses terrain cells outside an Add policy's material pool. | Missing lookup hook, offline or hooks disabled; supported on client and server. |
| `tools.material_picker` | Terrain group2 is filtered only when the owned held-tool observation and the current call's resolved item agree; the game's aggregate supply counts are retained. | Missing selector/held-tool mapping; client-only presentation on a server. |
| `tools.material_snapshot` | A separate per-player backpack/magic-chest observation producer is qualified. | Currently unavailable: aggregate selector counts are never fabricated into two sources. |
| `tools.preview` | The explicit-item preview has an owned bounded row adapter. | Missing constructor mapping, wrong side or hooks disabled. |
| `tools.brush_families` | A checked call-site adapter changes only the current tool frame's owned brush-query GUIDs and writes the held family's rows into that call's fresh output. Its text ends with the picker trace ("Reading the brush picker trace"). | Missing exact frame/query/held-tool context, checked-hook refusal or wrong side. |
| `tools.identity` | The explicit-item root identity adapter is qualified. | Missing root helper, wrong side or hooks disabled. |

The native client-Post observer copies the actual main-hand/action selection into an owned scalar snapshot tagged by
actor, owner, generation, session and step. A missing, changed or older-than-one-step sample does not admit a tool.
Admission also checks the current selector call's resolved held item with guarded reads: a switch to a vanilla item
within that step, a missing pointer, a failed read or a root mismatch leaves every group untouched. It filters only
terrain group2, leaving all six other categories unchanged. The qualified preview uses each call's explicit item,
independently of that held observation. Presentation calls mask any outer impact policy for their full duration and
restore it on return, including nested vanilla calls. Failed owned-row reads leave an empty presentation, not an
unchecked pointer or an inherited selection.

The brush adapter is confined to the qualified frame drawer's call instruction. It requires the selected hotbar root
and resolved item to match the registered held family. The game's brush query scans only the item list
`ItemRegistryResource.terraformingTools` in the game's data, never the runtime item registry, so a family's brushes
(runtime items) are never in it, whatever their tags. On admission the adapter writes the family's own rows (item,
ratio i+1 over 1) into that call's fresh, empty output and replaces its include/ratio GUID values with the family
identity, which no listed item carries: the native query appends nothing and sorts the rows by ratio, which keeps the
declared order. Brushes carry no family tag. Other query sites, vanilla tools, callbacks and nested unrelated frames
inherit no admission. If this capability is unavailable, the ordinary picker has no established route to the tool's
own brushes; do not assume that Add/Remove is usable through the normal menu.

### Reading the brush picker trace

Every way out of the adapter is counted on the client (relaxed counters; the frame hook never allocates, locks or
logs). A client Post publisher turns a change of state into **one** `kiln.log` line and refreshes the
`tools.brush_families` capability text (`Capabilities.Why("tools.brush_families")`, `kiln/state/capabilities.json`), at
most once a second. When states change faster than that, the line names the state now and how many changes it covers.

```text
tools: brush picker: patched (the family's brushes were written into the picker); frames: no held family 212, patched 40 (3 rows)
```

| State | What it means | Look at |
|---|---|---|
| `disabled` | The adapter retired after a write fault (an adapter that never installed counts nothing: the `tools.brush_families` reason says why). | `tools.brush_families` reason. |
| `no held family` | The picker drew without a registered family in hand: a vanilla tool, or no current held-item sample. Normal while a vanilla tool is held. | Shown while the mod's tool is held: the family was never defined (a `blocks:` error at content ready) or the held-item observer has no sample. |
| `no identity` | The held family lost its identity record between two lookups (a removal racing the frame). | Mod reloads. |
| `other frame` | Not the Overgrowth branch's registers (the Decay branch, or another layout). | The root tool's base. |
| `not selected` / `not resolved` | The picker's selected hotbar item, or its resolved item, is not the held family's root. | Hotbar selection versus the held item. |
| `output not fresh` / `tag mismatch` | The call's brush list or include/ratio tags are not the vanilla Overgrowth frame's. | Another mod patching the picker; a game update. |
| `read fault` / `write fault` | Reading or writing the frame faulted; a write fault skips that query and retires the adapter. | A crash report next to it. |
| `patched` | The family's rows were written; `(n rows)` is how many. `truncated to 16 rows` counts families with more brushes. | If the picker still shows no brush after this, the rows reached the query and the miss is past it. |

No line at all while the tool's picker is open means the qualified frame drawer never reached this call.

**Still unavailable:** the per-actor producer that separates backpack and magic-chest observations. Static qualification,
filtering fixtures and native runner tests do not establish actual menu rendering, network behavior or the complete
real-game tool. **Not yet seen in the real game** (0.2.0-dev.30.7 is the first release that does each of these):
the picker drawing a family's rows, the rows' icons and names, selecting a brush from them, a stroke with a selected
brush (the family's Remove/Add action and its radius, `impactValues.0x5258131e`, written for the first time), and the
brushes' terrain model (`equipment.terrainVoxelObject`, resolved for the first time; vanilla Overgrowth shapes carry
none, only the game's terrain blueprints do). The item-presentation constructor's traced caller uses the selected
hotbar model, which need not be the running action's item; its last item must never be cached as evidence of what the
player holds.

## Checks without running the game

`native/services/tools/tests/test-ammo.sh` runs the presentation fixtures on the host, among them the brush-frame
adapter (`brush_frame.cpp`: rows, declared order, every refusal) and its trace (`brush_trace.cpp`). The managed
`ToolCompilerContentTests` (Kiln.Blocks.Tests) check what `ToolCompiler` emits for a removal-only tool with three
brushes; kiln.content's host tests check item tags and voxel models (`itemtag_test.cpp`) and served terrain models
(`refs_test.cpp`).

`test-tool-content.sh` runs that emitted content through kiln.content's own resolvers with the client's real
reflection and game data: definition fields and references, item rules, itemdata rules. Its negative controls are the
three refusals of the first real-game run (a radius rule on a bare config id, a root GUID as an item tag, an unserved
terrain model). It needs files this repository does not hold, so it is not part of the regular suites, and without
them it prints `SKIP` and exits 0:

```sh
# 1. the compiler's export (any machine with the .NET SDK)
mkdir -p out && KILN_TOOL_CONTENT_FIXTURE=$PWD/out/compiler.json dotnet test managed/tests/Kiln.Blocks.Tests --filter ToolCompilerContentTests
# 2. an installed game, only read as files (KILN_GAME defaults to ~/.local/share/Steam/steamapps/common/Enshrouded;
#    KILN_CLIENT_EXE names another copy of its client exe)
KILN_GAME=<game folder> native/services/tools/tests/test-tool-content.sh out/tool-content out/compiler.json
```

The script extracts the game inputs into `out/tool-content/inputs` (`extract_tool_content_inputs.py`, read-only).
Inputs extracted earlier can be passed instead: `test-tool-content.sh <out> <compiler.json> <types.json> <brush.bin>
<pool-item.bin> <registry.bin>`.

Only the player runs the real-game check: confirm root/brush names, independent vanilla Overgrowth choices, all pool
entries with backpack-only and magic-chest-only supplies, empty stock, switching between tools, inventory changes,
selection after leaving/rejoining a world, paid placement/removal and the game's normal/remapped undo.

## Changelog

- **0.2.0-dev.30.7:** the picker lists a family's own brushes (the game's query scans only its data's
  `terraformingTools` list, so they were never found); brush radii reach the game (`impactValues.0x5258131e`); no root
  GUID is written as an item tag; brush terrain models resolve; the brush-frame trace in `kiln.log` and
  `tools.brush_families`; a content check against the installed game's data (the game is not run).
  `equipment.terrainVoxelObject` takes only a terrain model and `voxelObject` only a shape's model in data packs and
  itemdata rules. Thanks bisdat (kiln#130).
