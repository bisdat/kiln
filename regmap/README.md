# regmap: Kiln's registries mapped onto the game

Mods name Kiln's registry fields and ids (`items.maxStack`, `game:material_t1_wood_logs`; schema/registry/,
docs/kiln/modding/registries/). This folder is how Kiln translates them for each game build. It is Kiln's part: no mod
reads it, and a game update that moves a member changes a row here, not a mod.

| File | What |
|---|---|
| `common/<registry>.json` | where the registry's entries come from and one row per Kiln field |
| `server/<exe sha256>.json`, `client/<exe sha256>.json` | the record of one game build: verified or not, each row's state, the root types' layout hashes, the alias deltas |

Read by kiln.content (`native/services/content/registry/regmap.{h,cpp}`); checked offline by
`tools/registrygen/regmap_check.py` (against the committed type dumps under `sigdb/types/`, or against a game
executable's own reflection with `--exe`, the file read as data and never run).

## common/<registry>.json

```jsonc
{ "regmap": 1, "registry": "items",
  "entries": { "kind": "resource",            // resource: one resource per entry; array: the elements of one resource's
                                              // array member ("array"); single: one resource ("resourceGuid")
               "resourceType": "keen::ItemInfo", "name": "debugName", "guid": "objectId", "id": "itemId",
               "unnamed": "item_%08x",        // the stable name of an entry without a debug name (printf over its id;
                                              // "%.8s" takes the GUID text instead)
               "nameFrom": "",                // drop tables: "<component>.<member>": the templates that use the entry name it
               "key": "" },                   // loot: "items": the entry has the id of the item with its 32-bit id
  "rows": {
    "maxStack": { "paths": ["maxStackSize"], "conv": "int", "read": "worldLoad", "backend": "live", "clientNeeds": true,
                  "evidence": "docs/re/content.md (StackClamp)" },
    "comfort":  { "paths": ["comfortSetup.comfortAmount"], "conv": "int", "read": "unknown", "backend": "live",
                  "state": "unproven", "reason": "when the game reads it is not traced" } } }
```

A row: `paths` (the game's member paths from the entry's root; for a field of a list's elements, from the element),
`conv` (below), `read` (when the game reads the member: now, worldLoad, restart, unknown), `backend` (live: kiln.content's
store at world load; boot: the boot resource patch before the game reads it; none: read-only), `clientNeeds` (a client
shows or predicts from its own copy), `state` (mapped, unproven, unmapped; the last two with a `reason`), `evidence`.

| conv | paths | Kiln value |
|---|---|---|
| int, float, bool | the member | the number, as the schema's range allows |
| time | a `keen::Time` | seconds (the game keeps nanoseconds) |
| enum | an enum member; `values` maps Kiln names to the game's | one Kiln name |
| flags | a bitmask member; `values` as enum | a set of Kiln names |
| optionalEnum | [the switch (bool), the enum]; `values` with `none` for the switch off | one Kiln name |
| ref | [the 32-bit id, the reference (GUID)], kept in step | an entry's Kiln id |
| refList | [the array of references, the array of ids], kept in step | a list of Kiln ids |
| formula | the members the formula reads and writes; `formula` names it (formulas.h) | the Kiln number (tooltip power, armour) |
| impactConfig | the impact configurations; `config` names the configuration | the number |
| list | the array (blob or static) | a list; its elements' fields have rows of their own (`inputs[].count`) |
| stackCount | the count members of the element's alternatives (item stack, input category) | the count |
| inventoryWalk | the root group: stacks first, then each child group, depth first | the table's entries in walk order |
| unlock | the knowledge requirement | game or always |

**The routing rule.** The schema's `applies` is the contract. A row's `read` may not be later than its field's
`applies` (now < worldLoad < restart); a `boot` back-end serves only restart fields and `live` only now and worldLoad
ones; a read-only field has back-end `none`; a read time that is not traced makes the row unproven, and its field ships the
cautious `applies` (worldLoad or restart, never now). regmap_check.py fails on a row that breaks it; the native reader
(`load_dir`) loads such a row as unproven (refused per entry, its reason the rule it breaks) and names it in its errors.

**Reasons are shown to players and mod authors.** A row's `reason` is in Kiln words (the field, what is not known); the
game's member and type names go in `evidence` only. regmap_check.py refuses a reason that names one.

## <side>/<exe sha256>.json: one game build

```jsonc
{ "regmap": 1, "side": "server", "exeSha256": "f0d8...", "gameBuild": "1287369", "steamBuild": "26430253",
  "verified": true, "proof": "regmap_check.py --exe: every row resolved and type-checked against the exe's reflection",
  "layouts": { "keen::ItemInfo": "0x07009d37" },          // the reflection's layout hash of each root type
  "rows": { "items.maxStack": "ok" },                      // ok | changed | unmapped
  "aliases": { "renames": [], "removals": [], "suffixes": [] } }
```

A field is applied only on a build whose record is `verified` and lists its row `ok`; everything else is refused per
entry with the reason, which names the field and the game build and ends "; this edit was not applied" (BRIEF 6.2:
"Unmapped: items.power is not mapped for game build 1287369 yet (a Kiln update is needed); this edit was not applied").
**No record for the running exe** (a game update Kiln has not mapped yet): every field is Unproven ("... is not proven
for this game build (server exe f0d824be) (no verified regmap record for it yet; a Kiln update is needed)"), so Kiln
never writes a field it has not proven on that exact build; reads of the game's own values are not affected.

A record is written by `regmap_check.py --exe <a copy of the exe> --record --side <side> --game-build <b>` (later
`kilnctl update`) after the proof; never by hand. With `--entries` (this build's entries, made by
`regmap_entries.py --exe <copy> --out <file>`), `--ids-in` (the previous build's id table) and `--ids-out` it also computes
the alias deltas below; the entries file and the id tables hold the game's names and stay outside the repository (a
build folder; at run time kiln.content keeps the id table of the running build in its own data folder). Without
`--entries` the previous record's aliases are carried as they are.

**Alias deltas (D3).** The full name-to-GUID table is made at every start from the game's own data; a record keeps only
what must not move, carried forward from the previous build's record:
- `renames` ({registry, guid, id}): this entry keeps this id: its debug name changed, the order of the archive changed,
  its drop table's templates changed, it holds a name another entry also has, or its name had characters outside the id
  grammar;
- `suffixes` ({registry, guid, id}): a clash suffix given once and kept;
- `removals` ({registry, id, removedIn}): an entry the build no longer has; its id is retired (never given again) and
  every edit naming it is refused "removed by game build ...".

Identifiers only (GUIDs and Kiln ids), never names or values of the game's data (coordinator ruling, 2026-11-20). Drop
tables are pinned when their name would move (or clash), not all at once: pinning every template-derived table name would
put a list of the game's template names in the repository; the previous build's id table plus these deltas keep every
id. How ids are made: docs/kiln/modding/registries/README.md "Ids"; the same rules in regmap_ids.py and regmap.cpp, tested
on native/services/content/tests/registry_ids_cases.json.
