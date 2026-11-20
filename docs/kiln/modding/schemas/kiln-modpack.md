# Kiln modpack manifest (kiln-modpack.json at the zip root): format 1 (server modpack) or 2 (Crucible pack)

<!-- Generated from schema/kiln-modpack.schema.json by kiln_manage::lint::docs (cargo test -p kiln-manage --test lint with KILN_LINT_WRITE=1). Do not edit. -->

Format 1: a server's modpack for its players (docs/kiln/launcher/modpacks.md). Format 2: a Crucible pack, a .kilnpack (docs/kiln/launcher/crucible.md). A launcher refuses a format it does not know and says to update.

- Files: `**/kiln-modpack.json`
- JSON: comments **not allowed**, trailing commas **not allowed**
- Schema: [`schema/kiln-modpack.schema.json`](../../../../schema/kiln-modpack.schema.json); rules: [lint.md](../lint.md)

## Top level

## format1

A modpack is a deterministic zip: kiln-modpack.json plus one folder per mod at <kind>/<id>/ (kiln/<id>/, native/<id>/, emf/<id>/) or plugin/<file>.dll. The launcher checks the zip's sha256 (from the server's network.json / last-server.json), then this file, then each mod folder's kdh1 before it imports anything. Contains the mods players need (manifest clients: mod).

| Key | Type | Required | Notes |
|---|---|---|---|
| `$schema` | string |  |  |
| `created` | `common.schema.json time` |  |  |
| `format` | `1` | yes |  |
| `kiln` | `common.schema.json range` |  | Kiln versions players need (default: any). |
| `mods` | array of object | yes |  |
| `name` | string | yes | min length 1 max length 128 |
| `server` | object | yes |  |
| `version` | string | yes | Free-form (the server admin's modpack version). min length 1 max length 64 |

## format2

A .kilnpack is a deterministic zip: kiln-modpack.json, mods/<kind>/<id>/... (plugins mods/plugin/<file>.dll), layer/ (the pack layer, a data-only Kiln mod pack.<id>), overrides/<path>, client-overrides/<path>, server-overrides/<path> (paths relative to the profile's configDir). The launcher checks every mod's and the layer's kdh1 and every override's sha256 before it imports anything. Docs: docs/kiln/launcher/crucible.md.

| Key | Type | Required | Notes |
|---|---|---|---|
| `$schema` | string |  |  |
| `api` | `common.schema.json range` |  | Kiln API versions the pack needs. |
| `authors` | array of string |  |  |
| `changes` | string |  |  |
| `created` | `common.schema.json time` |  |  |
| `crucible` | object |  | Filled in by Crucible's signed index. |
| `description` | string |  |  |
| `dropins` | array of `common.schema.json kind` |  | Drop-in folders the instance loads (default none). |
| `format` | `2` | yes |  |
| `game` | object |  | Steam build ids the pack was made for (a yellow note when the player's differs). |
| `id` |  | yes | The pack's id; its layer is pack.<id>. |
| `kiln` | `common.schema.json range` |  | Kiln versions the pack needs. |
| `kind` | `"pack"` | yes |  |
| `layer` | object |  |  |
| `mods` | array of object | yes |  |
| `name` | string | yes | min length 1 max length 128 |
| `order` | object |  | kiln: the creator's Kiln load order (the layer last; the runtime orders by manifests); late: native ids for chain.ini [late]. |
| `overrides` | array of object |  |  |
| `server` | object |  |  |
| `version` | `common.schema.json semver` | yes |  |

