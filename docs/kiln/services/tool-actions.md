<!-- SPDX-License-Identifier: MIT -->
# Building-tool actions

Since 0.2.0-dev.30.5. Managed assembly: `Kiln.Tools`; native service: `kiln.tools`, version 1.

A `BuildingToolBlock` binds its primary and secondary building-mode actions to a `TerrainToolAction`.
`TerrainToolMode.Remove` removes only the action's `RemoveMaterials` terrain cells; `TerrainToolMode.Add`
places the selected material from the tool's pool. `TerrainOnly` must be true. Cells zero and 128–255 are
not valid removal targets. An empty removal list permits no removal.

```csharp
Primary = new TerrainToolAction { Mode = TerrainToolMode.Add },
Secondary = new TerrainToolAction
{
    Mode = TerrainToolMode.Remove,
    RemoveMaterials = [8, 14, 48],
    TerrainOnly = true,
},
```

Each action has a unique Kiln-generated impact program and a clone of the game's building sequence. Only
that sequence's terrain-impact event changes. Its other events, timing and held-repeat cadence stay with
the game. Primary/secondary use the player's normal remapped building controls; Kiln installs no mouse
hooks, key bindings or repeat timers. The brush supplies the radius through the game's impact configuration.

## Authority and threads

The engine calls the terraforming native on simulation workers, not necessarily the tick thread. No managed
handler runs there. All policy, material and brush data is copied into immutable native snapshots at
publication. The native binding checks the program identity, engine-bound used item, declared brush/radius,
nonzero actor and authoritative world. It uses the engine's float-narrowed cursor geometry for the building
area check. A stroke must fit inside one current permitted building area in all three axes; it is never clipped.
The game's existing permission, range and protected-area checks still run.

The binding copies the impact's instance data for that invocation, substitutes the generic Remove/Add mode
only in that copy, and scopes the immutable policy around the normal engine operation. It never changes
shared program constants. Nested unrelated impacts temporarily clear the policy and restore it afterwards.
The gameplay and rules hooks remain in the continuation chain. No unrestricted dig or separate managed
inventory operation substitutes for a missing engine seam.

The selected material and cost are server decisions. The scoped material lookup refuses cells outside the
tool's immutable pool; the payment hook revalidates the native operation description against that same pool
and computes cost from actual added volume. A client-supplied count or price is not accepted. The operation
uses the game's backpack/connected-magic-chest payment, mining yields and normal building-history undo.
Ordinary chests are not an extra source.

Generated programs default to an invalid, inert terrain mode and zero damage. Without the matching native
binding, after its owner unregisters, or when any mandatory hook is missing, they do not perform a generic
edit. Arbitrary callback registration is not supported in this release: the reserved `register_action` ABI
slot returns `Unavailable`, retains no delegate and reports `tools.callbacks` as `Policy`, with the reason
“declarative tool actions only in this release”. There is no managed `ToolActions.Register` facade.

## Refusals and lifetime

Remove requires the terrain filter, building-removal suppression, whole-area guard and exact-history guard.
Add requires the paid-cost path, authoritative material lookup, whole-area guard and exact-history guard.
Both require the impact binding. Client-only picker/preview/family presentation hooks are reported separately;
a dedicated server does not require client presentation to edit authoritatively.

`tools.actions` reports missing mappings, wrong side (a joined client's non-authoritative process), and no
session. `tools.actions.remove` and `tools.actions.add` distinguish the required native paths. `tools.build_area`
reports whether the qualified whole-area check is wired into the impact binding. No-session state is not
permission to bypass a check. `KilnToolsStatus` separates qualified action support from a running session.

Removing a family closes new program admissions and drops its policy bindings. An invocation already inside
holds its owned native snapshot until it returns; there is no mod code or managed delegate to outlive an
assembly unload. Existing history receipts retain their exact-refund guard even after the tool's mod is removed.

The tools service DLL and hook continuations remain resident for the process lifetime; they are never freed
with `FreeLibrary`. Hook packages install at the all-services-ready callback, after other DLLs (including rules)
are loaded. Global shutdown first closes action admission and drains in-flight policy scopes, then disables
packages in reverse order. A bounded drain timeout leaves their storage resident and admission closed.
Normal tool-mod removal does not tear down shared hooks. The native build uses `/EHa`; native Windows tests
also inject a structured exception in a continuation and verify that the policy and original arguments restore.
A failed continuation is not replayed by the binding.

## Real-game checks

Native host tests do not prove the engine will select a new item's sequence or populate its `UsedItem`
configuration. Before treating a tool as game-verified, check primary and secondary once and while held,
all declared brush radii, remapped controls, vanilla tools before/after use, and unloading the tool mod.
Repeat on a host and a joined client; the host alone must commit edits. Check whole-area refusal before any
charge or terrain change, non-pool/stale material refusal, exact paid undo after interleaved vanilla edits,
and buildings/entities remaining untouched. Only the owner should run these game checks.
