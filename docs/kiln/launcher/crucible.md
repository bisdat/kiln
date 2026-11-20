# Crucible packs (Kiln side)

Crucible is the modpack platform on frostdev.io (design: /mnt/Dongus/kiln-wave/dev29/out/modpack-share/DESIGN.md,
decided 2026-11-20: packs only, full uploads with every mod file, attribution for public packs, warnings + comments +
upvotes, no review gate, all in dev.30). This page is the Kiln side: the pack format (format 2 of
`kiln-modpack.json`, a `.kilnpack` file), building one from a profile, installing it as an isolated **instance**,
updating an instance with a three-way merge, the server part, `kilnctl pack`, and the shapes the launcher UI and the
Crucible service build on. Code: `crates/kiln-manage/src/modpack/pack.rs` (format, build, read, install, update, undo,
uninstall, diff, server part), `crates/kiln-manage/src/modpack/merge.rs` (the three-way merge of config overrides),
`runner/kilnctl/src/pack.rs` (CLI). Schemas: `schema/kiln-modpack.schema.json` (format 1 and 2),
`schema/kiln-pack-instance.schema.json`; examples `schema/examples/kiln-pack.json`, `kiln-pack-instance.json`.

Server modpacks (format 1, a server's mods for its players) stay as they are: [modpacks.md](modpacks.md). The two
readers name each other's files: the format-1 reader says "this is a Crucible pack (format 2)", the pack reader says
"this is a server modpack (format 1)", and both refuse a newer format with "update Kiln".

## The .kilnpack

A deterministic zip (entries sorted bytewise, one fixed time, mode 0644, deflate 6: the same profile gives the same
bytes and the same sha256):

~~~text
<id>-<version>.kilnpack            (MIME application/vnd.kiln.pack+zip)
  kiln-modpack.json                the index (format 2, kind "pack")
  mods/<kind>/<id>/...             every mod, as its Library entry holds it (a Kiln mod without assets/cooked/)
  mods/plugin/<file>.dll           a plugin DLL
  layer/                           the pack layer: a data-only Kiln mod, id pack.<pack id>
    kiln.mod.json                  no entryAssembly; loadAfter = every Kiln mod of the pack; version = the pack's
    content/*.json  assets/...
  overrides/<path>                 config overrides for both sides (path relative to the profile's configDir)
  client-overrides/<path>          ... for game clients only
  server-overrides/<path>          ... for servers only
~~~

### Index (`kiln-modpack.json`, format 2)

| Field | What |
|---|---|
| `format`, `kind` | `2`, `"pack"` |
| `id` | the pack's id: a mod id of at most 59 characters (its layer is `pack.<id>`) |
| `name`, `version` | 1-128 characters; SemVer |
| `created`, `authors`, `description`, `changes` | metadata; `changes` is the creator's words for this version |
| `game.builds` | Steam build ids it was made for (a yellow note when the player's differs) |
| `kiln`, `api` | version ranges of Kiln and of the Kiln API (a build writes `>=<this Kiln>`, `>=0.2.11`) |
| `crucible` | `{pack, version, slug, code}`, filled in by Crucible's signed index; absent in a local build |
| `mods[]` | `{kind, id, version, name?, path, hash, dep, default?, env?, file?, credit?}`: `path` `mods/<kind>/<id>` (plugin `mods/plugin/<file>.dll`), `hash` kdh1 (a Kiln mod's identity; sha256 for a plugin), `dep` `required` or `optional` with `default` on/off, `env` `{client, server}` each `required\|optional\|unsupported` (absent: required on both), `file` `{sha256, size, url}` where Crucible stores the archive, `credit` `{author, source (https), match}` |
| `layer` | `{id: "pack.<id>", version: <pack version>, path: "layer", hash: kdh1, file?}` |
| `order` | `kiln`: the creator's Kiln load order, layer last (the runtime orders by manifests; the layer's `loadAfter` puts it after every mod of the pack); `late`: native ids for chain.ini `[late]` |
| `dropins` | drop-in folders the instance loads (a build writes none: an instance has only its pack's mods) |
| `overrides[]` | `{path, mode, side, sha256}`: `mode` `merge-patch` (.json, RFC 7386), `ini` (.ini keys), `replace` (any other settings file: the whole file); `side` `both\|client\|server` |
| `server` | `{settings: {gameSettings: {...}}, notes}`: merged into the server's enshrouded_server.json (kiln.settings' rules) |

No kiln.json in packs and no client world settings (decided).

### Checks (every read: preview, install, update, verify)
- the zip's sha256 when the source gives one;
- the index's rules (ids, SemVer, paths, unique mods and overrides, `default` only on optional mods, credits with an
  https source, order and late naming the pack's own mods, override paths on the allowlist and modes fitting the file);
- every entry belongs to a listed mod, the layer or a listed override; no symlinks, absolute or `..` paths, no two
  entries differing only in case; at most 50 000 entries, 512 MiB a file, 4 GiB unpacked, 64 KiB an override and
  1 MiB of overrides;
- each mod's and the layer's kdh1 recomputed from the entries; each override's sha256; a Kiln mod carrying cook
  outputs (`assets/cooked/`) is refused (they are made on each machine);
- JSON and INI overrides must parse.
The read also lists the mods with code files (.dll .exe .so .lua scripts ...) for the install warning.

### Settings allowlist (overrides)
Only files below the profile's configDir with a settings extension (`txt ini cfg json toml yaml yml`), never
`.schema/`, `native.json` (switches travel in the index), dot folders or logs; 64 KiB a file, 1 MiB in all. The
build keeps only the keys the creator chose (`alpha.json:range,loot.smash`), replaces the home folder's path with `~`
and refuses a file with something that looks private (a key named token, secret, password, api key, webhook, bearer or
private_key with a text value, a Steam id, a Discord webhook URL): the message names the lines, the creator leaves
those keys out. Crucible runs the same scan again on upload (later brief).

## Building a pack from a profile
`pack::build(dirs, BuildRequest, out_dir, progress) -> BuiltPack` (`kilnctl pack build`):
- every mod of the profile, as its Library entry holds it; each folder's kdh1 must still be the profile's hash (a mod
  edited in its Library folder is refused: take it over first);
- a mod switched off in the profile becomes optional, off; `optional` makes others optional with a default; `env` sets
  the sides (a Library entry with `side: client` is unsupported on servers by default);
- the layer: a folder with `content/` (and `assets/`); its `kiln.mod.json` is written when absent (id `pack.<id>`,
  `apiVersion 0.2`, side both, clients mod, `loadAfter` every Kiln mod), or checked when present (data only, the right
  id; version and `loadAfter` are set);
- overrides captured from the profile's config folder (above); server game settings; credits per mod id;
- the result is read back with every check before `build` returns.

## Installing: the instance
`pack::install(dirs, Option<&GameInstall>, InstallRequest, progress) -> InstallResult` (`kilnctl pack install --yes`):
1. `consent` must be true (else `consent_required`): the preview's warning, "this pack contains mods, including code,
   from other people";
2. the file: a local path, or a download (`ModpackSource::Url{url, sha256}`, cached in
   `<data>/modpacks/downloads/<sha256>.kilnpack`; https only unless allowed);
3. every check above while unpacking into `<data>/modpacks/staging/<random>/`; the client mods
   (`env.client` not unsupported) and the layer go into the Library (immutable entries, shared with other profiles);
4. a new profile (named after the pack unless `name` is given) with exactly those mods (optional ones as the player
   chose, else their default), the layer last, `late` from the pack, no drop-in folders;
5. the overrides of `both` and `client` merged into the profile's config folder (keys set, the rest of the file kept);
6. `profiles/<pid>/pack/instance.json` (schema `kiln-pack-instance.schema.json`) and `profiles/<pid>/pack/base/` (the
   index and the override files as installed: the base of the next update);
7. with a game (unless `activate: false`): a save backup (reason `modpack`) and activation, as a server modpack does.
A failure after the profile was made deletes it again (to `backups/profiles/`).

### Three layers
| Layer | What | Where | Changed by |
|---|---|---|---|
| 1. Pack mods | the mods | Library entries | nobody; an update swaps versions |
| 2. Pack layer | the data mod, the overrides, the switches and order | Library entry `pack.<id>/<version>`; `pack/base/` | the creator (new versions) |
| 3. The player | their settings and any mod they add (a `local.<pid>` data mod made by the launcher's editors, any Library mod) | the live config folder; the profile | the player |

Data packs apply in load order and the later mod wins (docs/kiln/services/content.md), so the layer wins over the
pack's mods and a player's own data mod (loading after the layer) wins over the layer.

## Updating an instance
`pack::plan_update(dirs, pid, file, sha256?, optional) -> UpdatePlan` shows what an update does without writing
anything (`kilnctl pack diff <pid> <file>`); `pack::update(dirs, UpdateRequest, progress) -> UpdateResult` does it.
The new file must be the same pack (same `id`), else `refused`.

Inputs: base = `pack/base/` (the version installed), theirs = the new file, mine = the profile and its config folder
now.
- **Mods**: the new version's set. Mods the player added are kept ("yours"), after the layer. The player's switch on a
  mod the installed version had is kept ("your switch"); a new optional mod takes the request's choice, else its
  default. Mods that leave the pack leave the profile and stay in the Library. The layer is replaced.
- **Config, per key** (JSON: a key path; INI: `section.key`; replace: the whole file): only the pack changed it -> the
  pack's value is written ("updated by the pack"); only the player changed it, or the pack no longer sets it -> the
  player's stays ("your change kept"); a key new in the pack -> written; both changed it differently -> a conflict
  `{key, base, pack, mine}`. Edits keep the file's comments and layout.
- **Conflicts** need a choice before anything is written: `resolve` maps `"<path>#<key>"` (as the plan names them, e.g.
  `"kiln-auto-loot.json#range"`) to `pack` or `mine`, `all` answers the rest. Without one the update is refused with
  `conflict` and the plan in the error's `detail`.
- **Undo**: before writing, `pack/undo/` keeps the profile's meta.json, the config folder, instance.json and base/;
  `pack::undo(dirs, pid)` (`kilnctl pack undo`) puts them back (once; the next update replaces it).
- The game folder follows at the profile's next activation (Play activates), as for any profile change.

`pack::diff(old, new) -> PackDiff` compares two versions of a pack (mods added/updated/removed, layer changed,
overrides added/changed/removed, server settings changed): Crucible's automatic change list.

## Uninstalling
`pack::uninstall(dirs, pid)` (`kilnctl pack uninstall`): refused while a game has the instance active; the profile goes
to `backups/profiles/` like any deleted profile; the Library keeps the mods and the result lists the pack's entries no
profile uses any more (the Library's clean-up removes them).

## The server part (same version)
`pack::install_server(dirs, &ServerInstance, ServerInstallRequest, progress)` (`kilnctl pack server FILE --dir D --yes`):
the pack's server mods (`env.server` not unsupported, optional ones only when on) and the layer go into the Library and
are synced into the server's folders with kiln.server's sync (`server::control::sync`: the launcher's mods there become
exactly these; hand-placed folders stay; kinds without a server folder are listed in `leftOut`); the `both` and
`server` overrides go into `<server>/kiln/config`; `server.settings` into enshrouded_server.json (refused while the
server runs). With `modpackOut` the server modpack for players (format 1, [modpacks.md](modpacks.md)) is built right
after, so players joining get the same version through the existing join offer.

## kilnctl pack
| Command | What |
|---|---|
| `pack build P --version V [--id] [--name] [--layer DIR] [--override PATH[:K,K][@both\|client\|server]]... [--optional ID[=off]]... [--client-only ID]... [--server-only ID]... [--server-settings FILE] [--changes TEXT] [--out DIR] [--json]` | a pack from profile P |
| `pack verify FILE [--sha256 S] [--json]` | every check; prints the mods, layer and overrides (no data folder needed) |
| `pack install FILE --yes [--name N] [--optional ID=on\|off]... [--json]` | an instance; prints how to activate it (`kilnctl client activate`) |
| `pack update P FILE --yes [--resolve PATH#KEY=pack\|mine]... [--all pack\|mine] [--optional ID=on\|off]... [--json]` | the update; prints the plan, and the conflicts when it refuses |
| `pack diff P FILE`, `pack diff OLD.kilnpack NEW.kilnpack` | the update plan of an instance, or what changed between two packs |
| `pack undo P`, `pack uninstall P`, `pack list` | undo the last update; remove an instance; list instances |
| `pack server FILE --dir SERVER --yes [--optional ID=on\|off] [--no-settings] [--modpack-out DIR]` | the server part on a server folder |

## API for the launcher UI and the Crucible service (later briefs)
Proposed Tauri commands (none exists yet; names follow `modpack_*`). Long ones are tasks (`TaskId`, progress on
`kiln://task`), as `modpack_install` is. Rust types are in `kiln_manage::modpack::pack`; all serialize camelCase.

| Command | Arguments | Returns | Crate call |
|---|---|---|---|
| `pack_inspect` | `{source: {path, sha256?} \| {url, sha256}}` | `PackPreview {index, sha256, size, missing[], present[], code[], optional{id: bool}, instance?}` | `pack::inspect` (download first for a URL) |
| `pack_install` (task) | `gameId, InstallRequest {source, consent, allowHttp?, name?, optional{id: bool}, activate?}` | `InstallResult {profile, instance, imported[], overrides[], backup?, activation?}` | `pack::install` |
| `pack_instance` | `profileId` | `Instance \| null` | `pack::instance` |
| `pack_instances` | | `[profileId, Instance][]` | `pack::instances` |
| `pack_update_plan` | `profileId, source, optional?` | `UpdatePlan {profile, pack, from, to, changes?, mods{added, updated, removed, yours, keptSwitches}, layer{from, to, changed}, files[{path, side, mode, taken[], kept[], conflicts[{key, base, pack, mine}]}], conflicts}` | `pack::plan_update` |
| `pack_update` (task) | `UpdateRequest {profile, source, consent, allowHttp?, optional?, resolve{"path#key": "pack"\|"mine"}, all?}` | `UpdateResult {plan, profile, instance, imported[]}`; error `conflict` with the plan in `detail` | `pack::update` |
| `pack_undo` | `profileId` | `Profile` | `pack::undo` |
| `pack_uninstall` | `profileId` | `UninstallResult {profile, unused[]}` | `pack::uninstall` |
| `pack_build` (task) | `BuildRequest {profile, version, id?, name?, authors?, description?, changes?, optional{id: bool}, env{id: {client, server}}, credits{id: {author, source, match?}}, layer?, overrides[{path, side, keys[]}], serverSettings?, gameBuilds[]}`, `outDir` | `BuiltPack {file, sha256, size, index, notes[]}` | `pack::build` ("Save as file", Pack Studio) |
| `pack_verify` | `path, sha256?` | `PackIndex` | `pack::verify` |
| `pack_diff` | `oldPath, newPath` | `PackDiff` | `pack::diff` |
| `pack_server_install` (task) | `serverId, ServerInstallRequest {source, consent, allowHttp?, optional?, settings?, modpackOut?}` | `ServerInstallResult {mods[], imported[], overrides[], settings, leftOut[], modpack?}` | `pack::install_server` |
| `pack_secret_scan` | `file, text` | `string[]` | `pack::secret_findings` (Pack Studio's live warning) |

Routing a dropped zip: `pack::peek_format(path)` is 1 (the server modpack consent) or 2 (the pack preview).

For the Crucible service: the index is this format; `mods[].file` and `layer.file` name the content-addressed archives
on cdn.frostdev.io; `crucible` carries the pack/version/slug/code; the credits are `mods[].credit`. The service's
checks can run the same Rust (a `kiln crucible-check` CLI over `pack::read` and `pack::secret_findings`).

## Not done here (later briefs)
- the signed envelope around a downloaded index, and limiting downloads to cdn.frostdev.io (the Crucible client in
  `crates/kiln-manage/src/crucible/`); installing from Crucible's split storage (index + archives the Library lacks)
  rather than one zip;
- the launcher UI (preview, Install and play, update banner, Pack Studio with its data editors and the `local.<pid>`
  player data mod), the kiln:// links, the Deck screens;
- engine mods whose settings live in their own folder (not the configDir) cannot carry overrides yet;
- a pack's optional mods are imported into the Library even when off (so switching one on later needs no download).

## Checks done
`cargo test -p kiln-manage --features zip,http,server --test packs`: a deterministic build from a profile (same
bytes twice), the index (optional-off from a switched-off mod, the layer manifest written with `loadAfter`, chosen keys
only), the format-1 reader refusing a pack; verify refusing a wrong sha256, an unlisted entry, a changed mod file, a
changed layer file, a changed override; the secret scan and the allowlist; install (consent, preview counts, the
instance's mods and switches, no drop-ins, overrides written, instance.json), a player's change + own mod + switch,
the creator's next version, the plan (added, yours, kept switch, taken key, one conflict), the update refused until the
conflict is resolved, the merged file keeping its comment, undo, a different pack refused, uninstall with the unused
list; the server part (mods synced, overrides, game settings, the player modpack built and verified). Unit tests:
`modpack::merge` (merge patch, INI, replace, three-way). `cargo test -p kilnctl`: argument parsing and the data lock of
`pack` commands. Schema: `node schema/tests/kiln_pack.mjs` (examples and bad documents); `tests/contracts.rs` reads the
examples. Not tried with a real game or server (the owner's tests).
