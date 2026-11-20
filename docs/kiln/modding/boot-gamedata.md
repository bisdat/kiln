# The boot game-data snapshot

`Mod.OnBoot` runs before the game has loaded any of its data, so `BootContext.GameData.Read` cannot ask the running
game. It reads a **snapshot** of the game's data instead: files made before the start, by the Kiln launcher at Play (and at a single start with another profile), by
`tools/deploy-server.sh`, or by hand with `kiln boot-snapshot --game <dir>`. kiln.boot only reads it. Without a
snapshot of exactly this game build the capability `mods.boot.game_data` is unavailable and says what to run.

This page is the contract between the producer (the `kiln boot-snapshot` tool) and the reader (kiln.boot). Format 1.

The snapshot is the game's data **as shipped**. A record another mod loader patches (an EML mod, for example) reads
patched from the running game after world load and unpatched in `OnBoot`.

## Where
`<game folder>/kiln/data/boot-gamedata/<key>/`. A server and a client each have their own game folder, so each has its
own snapshot. kiln.boot opens only the directory whose name is the key it computed, never another one, and never writes
or deletes anything under `boot-gamedata/`. Directories of other keys are the producer's to remove.

## The key
`key` is the first 32 lower-case hex digits of SHA-256 over:

1. the game executable's SHA-256 as 64 lower-case hex ASCII characters,
2. one `\n`,
3. the version string of the game's `.kfc` file (`<exe stem>.kfc`): at header offset 0x10 there are two little-endian
   `u32`, `rel` and `len`; the string is the `len` bytes at `0x10 + rel`, with no terminator. The file starts with
   `KFC4`.

kiln.boot has both without decompressing anything: the hash of the executable it runs in, and one read of at most
64 KiB of the `.kfc` file. The key is not a hash of the `.kfc` file, on purpose: another loader may rewrite that file,
and its version string stays the same.

The producer reads the game's own archive: where `<stem>.kfc.bak` and `<stem>.kfc_resources.bak` exist they are the
originals and it reads those, otherwise the plain pair.

## Files
```
<key>/
  index.json            written last: its presence marks a finished snapshot
  items.index.json      items.data
  templates.index.json  templates.data
  skills.index.json     skills.data
  materials.index.json  materials.data
  resources.index.json  resources.data   every resource of the archive (kiln-bootsnap 2 on; optional for the reader)
```
The producer writes into `boot-gamedata/<key>.tmp-<pid>/` and renames the directory. All JSON is UTF-8 without a BOM.

`index.json`:
```json
{ "format": 1, "key": "<32 hex>", "side": "server",
  "exeSha256": "<64 hex>", "kfcVersion": "<the version string>", "gameBuild": "<its part before the first '|'>",
  "tool": "<producer name and version>", "written": "<UTC ISO 8601>",
  "counts": { "items": 0, "templates": 0, "skills": 0, "materials": 0 },
  "skipped": [ { "kind": "items", "name": "<name>", "reason": "<why>" } ],
  "runtimeOnly": [ "<Type>.<member>" ],
  "checksums": { "items.index.json": "<SHA-256, 64 hex>", "items.data": "<...>", ... } }
```
`side` is `server` or `client`. kiln.boot requires `format` 1, `key` equal to the directory's name and to the key it
computed, and all eight kind files present (the two resource files are optional: below). Anything else is "no snapshot", and the row's reason names the test that
failed. `checksums` (written by the producer since the 08b review) holds the SHA-256 of each of the eight files (and of the two resource files when they are there): kiln.boot
checks every file against it before anything is served (a start with a boot mod does it at once, about a second for a
300 MB snapshot). A file that does not match, or a list that leaves one out, makes the snapshot not ready: the row
`mods.boot.game_data` says `damaged` and names the file, and nothing of it is read. A snapshot written before checksums
has none and is read as before.

`counts.resources` is the number of entries of `resources.index.json`; `checksums` lists both resource files when the
producer wrote them.

`<kind>.index.json`:
```json
{ "format": 1, "kind": "items",
  "entries": { "<name>": [ <offset>, <length>, "<resource GUID>", "<8 hex type hash>", <part> ] } }
```
`<kind>.data` holds the entries' values back to back: the `length` bytes at `offset` are one complete JSON object, the
value. Offsets and lengths are bytes, below 2^53. A read loads one kind's index (once per process; an index file over
32 MiB is refused, and the producer writes none larger) and then reads `length` bytes.

The name of an entry, case-sensitive, is exactly the text after the query's prefix:

| Kind | Name | Query |
|---|---|---|
| `items` | the item's debug name | `item:Material_T1_Wood_Logs` |
| `templates` | the template's debug name | `template:NPC_Workshop_Fisher_Trader` |
| `skills` | `0x` and the 8 lower-case hex digits of the node's hash | `skill:0x8efcc472` (the query's digits are lower-cased first) |
| `materials` | the material row's name, `Cell<N>` for a row without one | `material:StoneRough` |

- Two records with one name: the first in the archive's resource order is stored; each later one is a `skipped` line
  with the reason `duplicate name`.
- The GUID (canonical lower-case text), the type hash and the part are the backing resource: the resource itself for an
  item or a template, the resource that holds the row for a skill or a material.
- A value larger than 4 MiB less 256 bytes is not stored: a `skipped` line with the reason `over 4 MiB`.

## Every resource: resources.index.json and resources.data
Written by `kiln-bootsnap 2` and later. They list every resource of the game's archive, of every type:
```json
{ "format": 1, "kind": "resources",
  "types": { "<qualified type name>": { "hash": "<8 hex>", "entries": [ [ "<GUID>", <part>, "<name>", <offset>, <length> ] ] } } }
```
- The type's name is the reflection's name for its hash (`keen::SceneResource`); a hash the reflection lacks is listed
  as `0x<8 hex>`. `hash` is the 32-bit FNV-1a of the name's UTF-8 bytes (offset basis 0x811C9DC5, prime 0x01000193).
- Each type's entries are sorted by (name, GUID, part). The name is the root struct's BlobString member `debugName`,
  else `name` (looked up as the live reader looks up a member); `""` for a type without one or a resource whose string
  is empty.
- `offset` and `length` place the resource's value in `resources.data` (values back to back, as a kind's data file).
  `-1` and `0` mean `resources.data` holds no value for it: an item or template stored in `items.data` or
  `templates.data` (read through those indexes), or a value that is not stored. A value that is not stored has a
  `skipped` line with kind `resources`: name `<type>/<GUID>/<part>` and the reason (`over 4 MiB`, `decode failed: ...`),
  or, for a type whose values are not stored at all, the type's name and the reason
  `values of this type are not stored (bulk asset data)` (the producer's list `kinds.VALUES_NOT_STORED`:
  `keen::JointAnimation`, `keen::RootMotionAnimation`, `keen::SceneEntityChunkResource`, `keen::RenderModel`,
  `keen::RenderModelChunkModelResource`, `keen::VfxResource`).
- A resource's value is the live reader's JSON of the whole resource, like an item's.

kiln.boot opens a snapshot without these files (made by an older producer) and serves every other key. When
`index.json`'s `checksums` lists one of them, it is checked with the others, and listed but missing is damaged. The list
is loaded at the first query that needs it (one file of at most 32 MiB). The producer requires both files, so a
snapshot made without them is made again at the next start through the launcher.

## The value
One JSON object: the same camelCase object Kiln's live game-data reader gives for that record after world load.
Member names as the game's reflection names them, GUIDs as canonical strings, arrays in order, resource references as
GUIDs, numbers with the game's values and units. An item is its ItemInfo; a skill or a material is its one row.

Equality is JSON equality after parsing: member order and whitespace do not count, and a float compares as the float32
it came from. No member is added, renamed or left out, with one exception: a member only the running game fills is
left out, and its path is listed once in `index.json` under `runtimeOnly`.

## What kiln.boot answers
| Query | Answer |
|---|---|
| `item:`, `template:`, `skill:`, `material:` | the entry of that name |
| `names:items`, `names:templates`, `names:skills`, `names:materials` | the names that kind holds, sorted, as `{"names": [...]}` (the result's `resourceId` is the empty GUID); read a record by its name. Counts against the read budget like a value |
| `names:templates/<component type>` | the templates whose record has a component of that type (`names:templates/MiningNode`), sorted, the same way. Only the template's own components count: a variant of that type inside a component's data does not. The first such query of a type reads `templates.data` once (about a second for 160 MB), later ones are answered from memory |
| `names:resources/<qualified type name>` | every resource of that type (`names:resources/keen::SceneResource`): the value is `{"names": [...], "resources": [{"resourceId", "part", "name"}, ...]}`. `names` are the distinct names, sorted, an unnamed resource by its GUID; `resources` lists every resource sorted by (name, GUID, part). The result's `typeHash` is the type's. A type the list does not hold, or holds without resources: not found. A type the producer listed as `0x<hash>` is found by its name's hash. Counts against the read budget like a value |
| `resource:<GUID>/<8 hex type hash>/<part>` | the resource's value, of any type: an item or template from its kind, any other from `resources.data`. Not in the list: not found. Listed without a value (above): unavailable |
| `resource:<GUID>/<qualified type name>/<part>` | the same, the name standing for its hash (`resource:<GUID>/keen::SceneResource/0`) |

A qualified type name in a query has letters, digits, `_` and `:`, at least one `::`, at most 160 characters. Without
the resource files, `names:resources/...` and a `resource:` of a type other than `keen::ItemInfo` and
`keen::ecs::TemplateResource` answer unavailable with "this snapshot predates the resource list: start through the Kiln
launcher or run `kiln boot-snapshot --game <dir> --force`". An item or template the snapshot has not got is not found
either way: those two kinds are complete without the list.

The result is `{"resourceId","typeHash","part","value"}`: the GUID, the type hash as 8 lower-case hex digits, the part
as a number and the value as stored. For `skill:` the offline view's `resourceId` is the backing resource's; kiln.content's
live reader reports none (an empty GUID) for the same entry, so compare skills by value. One result is at most 4 MiB; one mod reads at most 16 MiB of values during its
`OnBoot`. An entry whose offset and length do not lie inside its data file, or whose bytes are not one JSON object, is
refused as unavailable and never read out of range.

Capability `mods.boot.game_data`: available with a matching snapshot. Otherwise unavailable, with the reason "no
game-data snapshot for this game build: start through the Kiln launcher, or run `kiln boot-snapshot --game <dir>`" and
the test that failed.
