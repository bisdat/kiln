# Registries

Kiln's registries are the game's data as **Kiln-owned names**: `items.maxStack`, `recipes.inputs`, `loot.stack.max`,
`drops.entries[3].chance`, `balance.levelCap`. A mod edits a field by its Kiln name and an entry by its Kiln id
(`game:material_t1_wood_logs`); it never names a game function, member, offset or reflection path. Kiln maps each field
onto the game for every game build it supports, so a game update breaks Kiln's mapping (which a Kiln update fixes), not
your mod.

Registries hold **data only**. Services and events (Kiln.Content, Kiln.Items, Kiln.World, ...) stay as they are: a
registry replaces raw data paths, not services.

**Status in this release: experimental (`KILNX031`), schema version 0.1.0.** The schemas, the generated C# types (`Kiln.Registry`), the field
catalog (`Kiln.Registry.RegistrySchema`) and these pages ship; the registry stage that reads a data pack's `registry`
section and runs `Mod.OnRegistry` comes in a later release. Until a registry is marked stable its shape may change in any
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
