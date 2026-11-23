# Registries

Kiln's registries are the game's data as **Kiln-owned names**: `items.maxStack`, `recipes.inputs`, `loot.stack.max`,
`drops.entries[3].chance`, `balance.levelCap`. A mod edits a field by its Kiln name and an entry by its Kiln id
(`game:material_t1_wood_logs`); it never names a game function, member, offset or reflection path. Kiln maps each field
onto the game for every game build it supports, so a game update breaks Kiln's mapping (which a Kiln update fixes), not
your mod.

Registries hold **data only**. Services and events (Kiln.Content, Kiln.Items, Kiln.World, ...) stay as they are: a
registry replaces raw data paths, not services.

**Status in this release: experimental (`KILNX031`), schema version 0.1.0.** The schemas, the generated C# types (`Kiln.Registry`), the field
catalog (`Kiln.Registry.RegistrySchema`) and these pages ship, and the registry stage reads a data pack's `registry` section at
start (fields that apply now or at world load also follow a pack's hot reload). Since mod API **0.2.17**, C# mods can author
through `Mod.OnRegistry(RegistryContext)` and use explicit owner-scoped live changes through `Kiln.Registry.Registry.Change`.
Entry add and remove remain refused, each with its reason in kiln.log and registry.json, until their back-end ships.
Until a registry is marked stable its shape may change in any
release; from API 1.0.0 the stable registries' contract is frozen (additive only).

**Versions.** Experimental schemas are 0.x; a schema reaches 1.0.0 when it is frozen with API 1.0.0. Within one breaking
line (0.<minor> while experimental, <major> from 1.0.0) a field may only be added, widen its range, get new enum values
at the end, or apply earlier; anything else (a narrower range, other values, another type, a later applies, a removed
field) needs the next breaking version (0.2.0 after 0.1.x; 2.0.0 after 1.x) and ships an id migration
(`schema/registry/migrations/<registry>-<from>-<to>.json`, ids and fields old -> new), applied to edits before they are
matched. `schema/registry/ids.lock.json` keeps every field's contract and every registry's version history;
`tools/registrygen/registrygen.py` refuses the rest.

| Registry | Entries | Page |
|---|---|---|
| `items` | every item: stacks, rarity, levels, power, speed, damage, armour, costs, perks, socket | [items.md](items.md) |
| `recipes` | every crafting recipe: inputs, outputs, duration | [recipes.md](recipes.md) |
| `loot` | random loot per item: stack sizes, rarities | [loot.md](loot.md) |
| `drops` | the game's drop tables (default inventories) | [drops.md](drops.md) |
| `balance` | the balance table: experience, levels, flame, durability, sockets, perk costs | [balance.md](balance.md) |

The per-registry pages are generated from `schema/registry/<registry>.json` by `tools/registrygen/registrygen.py`; edit
the schema, never the page.

## C# authoring (API 0.2.17)

Reference `Registry` in `KilnServices` and opt into experimental diagnostic `KILNX031`. Published entry types remain in
`Kiln.Registry`; the context and `Mod.OnRegistry` live in `Kiln.Api`. See the complete
[RegistryEdits sample](../../../../mods/samples/RegistryEdits/RegistryEditsMod.cs).

```csharp
[RegistryEntryPoints(RegistryEntryPoint.GameStart | RegistryEntryPoint.WorldLoad)]
public sealed class Stacks : Mod
{
    public override void OnRegistry(RegistryContext context)
    {
        var registry = Kiln.Registry.Registry.For(context);
        registry.Alter(view => view.Items["game:material_t1_wood_logs"].MaxStack = 500);
    }
}
```

The class attribute is constant metadata; Kiln reads it without running the mod. No attribute on a real override means
**GameStart**. Declaring points without an override (`KILN122`), zero/unknown flags, or automatic Manual (`KILN123`) is invalid.

| Point | Invocation and inputs |
|---|---|
| `GameStart` | A retained author instance, once per Add/Alter/Final pass at resources-ready. Global settings only. It is not the transient `OnBoot` instance. |
| `ModLoad` | Runtime instance after `OnLoad`, once the base registry exists. Global settings only, even when a world already exists. |
| `WorldLoad` | Runtime instance for each native world token, after that world's save settings are bound. |
| `HotReload` | Replacement runtime instance, after the old owner's live changes are removed. When declared, it replaces `ModLoad` on reload; the two points never both run for the same replacement. |
| `Manual` | Explicit `Registry.Change`, not an automatic class flag. Requires an active world. |

GameStart authors get the registry context, **not cross-mod runtime exports**. Declared dependencies still order the edits:
a dependency's edits run first. A mod that needs another mod's runtime export chooses ModLoad or later. A declared export
lookup at GameStart is refused with `RegistryContextError.RuntimeExportUnavailable` (assembly loading can wrap that error).
Private helper assemblies in the mod's own folder remain loadable; they are not another mod's runtime export graph.

Automatic points run all selected mods' Add, then Alter, then Final in load order. Ordinary property assignments are absolute and target
Alter; `registry.Final(...)` and `registry.Items.Final(pattern, ...)` explicitly target Final. Final handlers read the
Add+Alter view, never another mod's Final commit. A callback's own pending writes are projected into its typed read view;
they are submitted only after successful return. A throwing callback submits none of that turn's buffered edits.
`Select(pattern)` returns entries by Kiln id; list properties use the generated clear/append/remove/index APIs.
All three passes are invoked even though entry creation is not yet exposed. Runtime points wait for the host tick;
native resources-ready callbacks never execute runtime authors inline. Layers from distinct declared points compose:
for example, a WorldLoad append stacks on a retained GameStart append. Prefer absolute assignments or list replacement
when the same intended value is authored at several points.

Contexts carry `EntryPoint`, `Phase`, `World` (Kiln token, kind and bound save id), and `Identity`. Code, point/pass,
world identity and settings actually observed in that callback contribute to Identity; no executable plan cache is used.
Global `Config.Register<T>()` and `WorldConfig<T>()` can be used as inputs; fallback authoring snapshots do not install
watchers or write defaults. Register live settings in `OnLoad` to get ordinary change events. Per-world settings are
refused at GameStart/ModLoad, and while a world's settings are unbound. Later points never write restart-only fields:
they return/report `RestartRequired` rather than silently applying them.

Keep copied values, not contexts/entries/lists. Their operations fail with `RegistryContextException.Code` after return
(`Closed`), unload (`Unloaded`), on another thread (`WrongThread`), or a world switch (`WorldChanged`). Metadata and copied
results remain snapshots. A resolved `Registry.Read(view => ...)` frame is expiring and read-only.

### Explicit changes and ready

Inside a loaded mod callback on the active world's thread:

```csharp
var change = Kiln.Registry.Registry.Change(view =>
    view.Items["game:material_t1_wood_logs"].MaxStack = 500);
if (!change.Result.Ok) Log.Warn(change.Result.Reason ?? "registry change not applied");
// Keep the owner-bound handle if needed. Revert recomposes the remaining layers, not an old memory snapshot.
var reverted = change.Revert();
if (!reverted.Ok) Log.Warn(reverted.Reason ?? "registry revert not applied");
```

`RegistryChangeResult` copies outcome, accepted/refused/skipped counts, world token, generation and reason. `Ok` means
Applied or Reverted with no refused edits or skipped foreign bytes. PendingWorld/PendingResource do not claim bytes were
changed. RestartRequired, WorldRequired, Refused, Skipped and Unverified stay distinct. Disposal reverts once; owner
retirement also cleans up live changes. Process ModLoad layers persist across worlds; world-bound layers expire with
their world. Already committed GameStart layers stay until process restart, including through a runtime hot reload.

Subscribe to `Registry.Ready` in `OnLoad` for copied stage/live ready notifications; the Host removes the handler at unload.
Notifications are posted through the owner's scheduler, never run inline on a native loader thread or inside a commit.
A late subscriber likewise receives the last snapshot on its next tick. This event reports readiness/generation, not proof that
every field in a transaction applied: inspect results and the registry report. `Registry.Available` checks the transport;
per-field capability rows still decide whether a field is mapped.

### Offline authoring tests

`KilnHarness.Registry.Seed(registry, id, field, valueJson)` supplies copied JSON. `Author(mod, point, phase, world)` invokes
one loaded instance's OnRegistry pass and returns copied IR (or null for no edits). Contexts expire before it returns.
This is a recording-only fake: seed the next pass explicitly. It does not simulate live application, pass composition,
mapping proof or automatic scheduling, and leaves native registry availability false. Host transport/lifecycle tests and
native backend tests cover those separate paths; offline authoring is not real-game byte proof.

## Capabilities

At resources-ready, the registry stage loads the regmap and reports `registry.<name>` for each registry and
`registry.<name>.<field>` for each schema field. A field is available when its mapping is verified for the running
executable and the field applies to this side. A registry is available when at least one of its fields is available.
Check the field you need with `Capabilities.Is("registry.items.power")` or use `Capabilities.Get` to read its reason
and detail. The detail includes the field's `applies` value even when the field is unavailable. The host resolves
manifest features before the stage, so use these rows in runtime checks after resources-ready.
[Capability states and reasons](../../services/caps.md#rows-added-in-dev31)

Capability names preserve the field's case and dots and omit the `[]` list marker. Use the schema path in edits and
the capability name when querying `Capabilities`.

| Capability name | Schema field path |
|---|---|
| `registry.recipes.inputs.item` | `recipes.inputs[].item` |
| `registry.recipes.inputs.category` | `recipes.inputs[].category` |
| `registry.recipes.inputs.count` | `recipes.inputs[].count` |
| `registry.recipes.outputs.item` | `recipes.outputs[].item` |
| `registry.recipes.outputs.count` | `recipes.outputs[].count` |
| `registry.drops.entries.item` | `drops.entries[].item` |
| `registry.drops.entries.count.min` | `drops.entries[].count.min` |
| `registry.drops.entries.count.max` | `drops.entries[].count.max` |
| `registry.drops.entries.chance` | `drops.entries[].chance` |
| `registry.drops.entries.weight` | `drops.entries[].weight` |
| `registry.balance.buildzoneSizesPerAltarLevel.x` | `balance.buildzoneSizesPerAltarLevel[].x` |
| `registry.balance.buildzoneSizesPerAltarLevel.y` | `balance.buildzoneSizesPerAltarLevel[].y` |
| `registry.balance.buildzoneSizesPerAltarLevel.z` | `balance.buildzoneSizesPerAltarLevel[].z` |

## When an edit takes effect: applies

Every field of every registry says when an edit of it takes effect, in the schema, on the C# property
(`[RegistryField("items.maxStack", RegistryApplies.WorldLoad)]`), in `RegistrySchema.Find(...).Applies` and on its page:

| Applies | Means | Typical fields |
|---|---|---|
| `now` | at the next use (the next craft, roll, hit, payment) | gear numbers, recipe inputs, loot stack sizes, drop chances |
| `worldLoad` | when a world loads: in place before the world's save is read | `items.maxStack` (a lower value cuts stacks in that save) |
| `restart` | after a game restart: the game reads it once at start | the balance table |

A field whose read time Kiln has not traced yet ships with the more cautious value (worldLoad or restart, never now) and an
unproven mapping (refused per entry, with a reason, until a Kiln update proves it): items.block, items.fuel.burnTime and
items.comfort in this release. Each page's Mapping column says which fields this Kiln maps, which are unproven and why.

**Lists.** A list of plain values (`items.perks`) takes clear, append, remove (by value) and replace; a list of entries
with fields (`recipes.inputs`) takes clear, append (a new entry, then its fields) and remove (the entries a condition
picks, named by their values), so two mods' appends and removals stack. A list of fixed length (`balance.xpPerLevel`,
exactly 5; `balance.altarsPerFlameLevel`, one value per flame level) is edited one element at a time
(`xpPerLevel[2]`). A reference that may be absent (a recipe input's item or category) can be cleared.

## Stages and conflicts

At each start Kiln runs three stages, each across **all** mods (in the load order players see) before the next: every mod's
`add`, then every mod's `alter`, then every mod's `final`.

- Later in load order wins, and the report says so ("overridden").
- Relative changes stack ("composed": x4 then x2 = x8).
- `exclusive` on an edit keeps the strict rule: another mod's edit of the same field is a conflict and the later one is
  refused whole.
- `yield` applies an edit only where no earlier writer set the field.
- `final` follows alter's rules; a mod's final sees the result of add and alter, not other mods' finals.
- Registry edits layer on top of Kiln's older paths (boot resource patches, data packs) and on top of values another
  loader wrote into the game's archive before Kiln, and are reported with them. Old mods are never refused because of a
  registry edit. Conflicts inside the registry and with Kiln's older paths are found for certain; other loaders' DLL
  patches and hooks are best-effort and labelled so.

### Native callback and owner lifetime

`kiln.registry` keeps submitted batches and callbacks under their caller's owner. Revoking that owner removes its queued
batches (including parse refusals) and its stage/ready subscriptions, even before any world exists. Another live owner of
the same mod keeps its own batches. Admission and dispatch also check the core's current owner state, so a retiring
notification still being delivered cannot admit or start another callback into the dead owner.

The stage takes the surviving queued batches at startup. Edits it already took remain until the next process start;
revocation is not a rollback of committed registry layers. A stage callback already in flight may finish, but if its
owner was revoked, that turn's uncommitted edits are dropped and the report names the reason. A fault in either kind of
native callback removes that subscription; a faulted stage turn is dropped as well.

Unsubscribe and revoke never wait for callback quiescence. A callback whose dispatch already started on another thread
may still finish, so its native function and user state must stay valid until that dispatch has completed. Ready dispatch
rechecks each subscription, including when an earlier callback in the same START or WORLDLOAD event unsubscribes or
revokes a later one. These lifetime rules add no members to `kiln.registry`, which stays 88 bytes. Live transactions
use the separate 56-byte `kiln.registry.live` v1 table. The managed Host owns these subscriptions and their retirement.

### Live values in the report

After world load or a pack reload, `kiln/state/registry.json` shows the live `result` and writer chain for recipe inputs
and outputs, drop-table entries, item perks, and power and armor. The chain shows older packs below the registry edit.
`composedOn` names the pack whose value a relative edit used, and `overridden` names the pack an absolute edit replaced.
Drop-table rows refresh within the current generation when the world acquires their tables.

List values are arrays of entries in game order. The report keeps the first 64 entries per array and includes the omitted
count in `resultEntriesLeftOut`, `base.entriesLeftOut`, or the writer's `valueEntriesLeftOut`, `listEntriesLeftOut`, or
`opEntriesLeftOut` as applicable. Formula rows keep the numeric `result` and include `formulaInputs` in evaluation order.
Power lists the damage factor, attack speed factor, and minimum level. Armor lists the armor share and minimum level.

## Ids

Every entry has a Kiln id: `game:<stable name>` for the game's entries, `<mod id>:<name>` for a mod's. The grammar is
`<namespace>:<name>` with the namespace `[a-z0-9][a-z0-9_.-]*`, the name `[a-z0-9_.-]+`, and for a game entry an optional
clash suffix `~<8 to 32 hex digits>`; the mod ids `game` and `kiln` are reserved. A debug name with other characters
(spaces, apostrophes, letters outside ASCII) has each of them as `_` and is pinned. Ids are lower case,
never reused, and keep working across game updates: Kiln keeps an alias table per game build (keyed by the entry's GUID)
that follows renames, so a game entry keeps the id it had when the game renames it. A game entry a game update removes
refuses every edit that names it ("removed by game build ..."). The old debug-name selectors of data packs keep working.

How a game entry's stable name is made (each registry's page repeats its own rule):

| Registry | Stable name | On game build 1287369 (server) |
|---|---|---|
| items | the item's debug name in lower case; `item_<item id, 8 hex>` when it has none | 4,268 items: no empty name, no clash (also none ignoring case); levels run to 105 (gems), so the formulas take the field's whole 1..105 |
| recipes | the recipe's debug name in lower case; `recipe_<recipe id, 8 hex>` when it has none (the name kiln.content gives such a recipe today) | 2,542 recipes: no empty name, no clash; 703 share a name with an item (different registries: no clash) |
| loot | the id of its item | 4,213 entries |
| drops | see "Drop tables" below | 420 tables |
| balance | `game:balance` (one entry) | 1 |

**Two entries with one name.** The entry that held a stable name first keeps it for good; a later entry whose name is the
same (also when the two differ only in case) gets `game:<name>~<first 8 hex digits of its GUID>` (more digits if those
are taken as well), and the plain name as a selector means only its holder. "First" on the first game build Kiln maps is
the order of the game's archive (the order the boot snapshot and boot resource patches see, which already keep the
first and report the later one as a duplicate); from then on Kiln's alias table records every suffix it gave, so a later
game build never moves a name to another entry. Templates are the case today: 12,269 templates on 12,229 names on build
1287369, 40 later entries with a name taken before them plus 1 that differs only in case, on 39 names. They get the suffix
when a templates registry exists; no R1 registry has a clash on this build.

**Drop tables** have no names in the game. A table is named after the templates that use it (their `DefaultInventory`
component): `game:` + the first of their stable names in ordinal order, in lower case
(`game:lootcontainer_enemy_scavenger_melee01` for the scavenger's table that 12 templates use). A table no template uses is
`game:drops_<first 8 hex digits of its GUID>`. Two tables can only get one name if a template used two tables; then the
clash rule above applies. On game build 1287369 (client data): 837 templates use 357 distinct tables (242 tables by one
template, 115 by several, up to 68), the rule gives no clash, and the server's 420 tables leave about 63 unused by any
template, which take the `drops_` form. The table's GUID also selects it, as data packs' `drops` section does today.

## Tiers

| Tier | What | Promise |
|---|---|---|
| Stable | registry schemas, Kiln ids and fields, `Kiln.Registry` C# and data sections, typed blocks | survives game updates: Kiln updates the mapping; frozen by API 1.0.0 |
| Advanced | raw reflection paths (`raw` in an edit), resource patches, GameFields/GameTypes, boot.GameData keys | works while the game's names hold; checked and refused per entry, never silent corruption |
| Native | hooks by address, raw memory, native plugins | the mod's own risk |

## Files

| File | What |
|---|---|
| `schema/registry/<registry>.json` | a registry's schema: fields, types, ranges, ops, applies, ids rule (Kiln names only) |
| `schema/registry-schema.schema.json` | the schema of those files |
| `schema/registry/ids.lock.json` | every published registry and field name with its numeric id; never reused |
| `schema/registry/migrations/` | id and field renames a major schema version ships |
| `schema/registry-section.schema.json` | the data section's schema (generated) |
| `regmap/` | how each field maps onto the game, per game build (Kiln's part: mods never read it) |
| `tools/registrygen/registrygen.py` | the generator; `--check` fails on stale output, `lock` and `retire` keep the ids |
