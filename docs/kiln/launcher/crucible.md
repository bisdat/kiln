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
| `overrides[]` | `{path, mode, side, sha256, size?}`: `size` the file's bytes (every build writes it, a read checks it when present; Crucible needs it to share); `mode` `merge-patch` (.json, RFC 7386), `ini` (.ini keys), `replace` (any other settings file: the whole file); `side` `both\|client\|server` |
| `server` | `{settings: {gameSettings: {...}}, notes}`: `settings` holds `gameSettings` only (a server's name, ports, user groups with their passwords and bans never travel: a pack would set them on every server it is put on), merged into the server's enshrouded_server.json (kiln.settings' rules) |

No kiln.json in packs and no client world settings (decided).

### Checks (every read: preview, install, update, verify)
- the zip's sha256 when the source gives one;
- the index's rules (ids, SemVer, paths, unique mods and overrides, `default` only on optional mods, credits with an
  https source, order and late naming the pack's own mods, override paths on the allowlist and modes fitting the file);
- every entry belongs to a listed mod, the layer or a listed override; no symlinks, absolute or `..` paths, no two
  entries one file on Windows or macOS (NFC + Unicode lower case: `A.json`/`a.json`, `Ä`/`ä`), no dot entries (kdh1
  skips them, so no hash would cover one; the build never packs them), no Windows device names (`aux`, `nul.json`,
  `COM1`); at most 50 000 entries, 512 MiB a file, 4 GiB unpacked, 64 KiB an override and
  1 MiB of overrides;
- each mod's and the layer's kdh1 recomputed from the entries; each override's sha256 (and size when given); a Kiln mod carrying cook
  outputs (`assets/cooked/`) is refused (they are made on each machine);
- JSON and INI overrides must parse.
The read also lists the mods with code files (.dll .exe .so .lua scripts ...) for the install warning.

### Settings allowlist (overrides)
Only files below the profile's configDir with a settings extension (`txt ini cfg json toml yaml yml`), never
`.schema/`, `native.json` (switches travel in the index), dot folders or logs; 64 KiB a file, 1 MiB in all. The
build keeps only the keys the creator chose (`alpha.json:range,loot.smash`), replaces the home folder's path with `~`
and refuses a file with something that looks private: a key whose name says token, secret, password, passwd, pass,
pwd, auth, api key, access key, private key, webhook, bearer, credential, cookie or session id holding text (numbers,
switches and empty values are fine), or text shaped like a secret under any key (Discord webhook URLs, also
`/api/v10/webhooks`, and bot tokens, Steam ids as 7656119... / `STEAM_0:1:n` / `[U:1:n]`, GitHub and Slack tokens, JWTs,
bearer tokens, private keys). JSON files are read as JSON (by key, whatever the layout), other files by line; words
in text ("the secret passage", an item called "Secret Stash") are no finding. The message names the keys (JSON) or
lines, the creator leaves those keys out. The server settings and the layer's settings-like files get the same scan.
Crucible runs the same scan again on upload (later brief).

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
| 3. The player | their settings (the live config folder), mods they add, and their data edits in the player layer `local.<pid>` | the config folder; the profile; `pack/local/` | the player |

Data packs apply in load order and the later mod wins (docs/kiln/services/content.md), so the layer wins over the
pack's mods and the player layer (loading after everything) wins over the layer.

### The player layer (`local.<pid>`)
`kiln_manage::modpack::layers`: a data-only Kiln mod per instance (`kiln.mod.json` without entryAssembly, `side: both`,
`clients: none`: joining players need nothing, `loadAfter` every other Kiln mod of the profile) whose working copy is
`profiles/<pid>/pack/local/` (`content/<name>.json`, one level). The editors write files (`layers::write`, Tauri
`pack_local_write`, `kilnctl pack local P --set content/X.json=FILE`; text must be one JSON object, 1 MiB at most) and
`layers::save` (`pack_local_save`) puts the folder into the Library as an immutable entry like every mod a profile uses
and makes it the profile's last Kiln mod; a working copy without data files takes it out of the profile again.
A data field is `[section, target, field]` (`["items", "Logs", "maxStackSize"]`; a target with conditions carries its
`@where`; a definition of an `add` list is `[section, "add", name]`; a value straight under a section is `[section, key]`),
whatever file holds it: a pack layer that moves a field to another file still overlaps. A profile naming its player
layer whose working copy is gone (no `local/kiln.mod.json`: a copied or restored profile) gets it back from the Library
entry at the next save or update. A pack built from an instance leaves the player layer out (a note says so): `local.`
ids are refused in packs, since profile ids repeat on every PC. Undo of an update also takes back player-layer edits
saved after it.

## Updating an instance
`pack::plan_update(dirs, pid, file, sha256?, optional) -> UpdatePlan` shows what an update does without writing
anything (`kilnctl pack diff <pid> <file>`); `pack::update(dirs, UpdateRequest, progress) -> UpdateResult` does it.
The new file must be the same pack (same `id`), else `refused`.

Inputs: base = `pack/base/` (the version installed, and `base/written.json`: each config key Kiln wrote at installs and
updates, with the value it left), theirs = the new file, mine = the profile, its config folder and its player layer now.
- **Mods**: the new version's set. Mods the player added are kept ("yours"), after the layer. The player's switch on a
  mod the installed version had is kept ("your switch"); a new optional mod takes the request's choice, else its
  default. Mods that leave the pack leave the profile and stay in the Library. The layer is replaced.
- **The player layer** stays and still loads last: a fresh Library entry whose manifest names the new mods to load
  after. `plan.local {id, files, fields, overlaps[]}`: each field the new pack layer sets too, `{key, section, target,
  field, pack, base?, mine, file, changed}` ("your change still wins"); `UpdateRequest.drop` lists keys to let go of
  ("Drop my change": the field leaves the player's file, an emptied target and section with it, comments kept), a key
  that is no overlap is refused.
- **Config, per key** (JSON: a key path; INI: `section.key`; replace: the whole file): only the pack changed it -> the
  pack's value is written ("updated by the pack"); only the player changed it, or the pack no longer sets it (a key or a
  whole override file the new version drops) -> the player's stays ("your change kept"); a key new in the pack ->
  written when the file does not hold it or holds what Kiln last wrote there; a value Kiln never wrote (the player's,
  or a default the mod wrote: Kiln cannot tell them apart) that differs is a conflict `{key, base: null, pack, mine}`;
  both changed it differently, a removal against an edit included -> a conflict `{key, base, pack, mine}`.
  Edits keep the file's comments and layout. Instances installed before `written.json` existed take new keys as before until their first update.
- **Renamed settings files**: an override the new version drops matched with one it adds is a rename
  (`files[].renamedFrom`) when the paths belong together (the same file name, the same folder, or the old name a folder of
  the new path: `alpha.json` -> `alpha/settings.json`) and the overrides match (same mode, the same values or the same
  keys). The player's values are read at the old name: a file of the same name not there yet starts as a copy of the
  old one; otherwise the pack's keys are carried key by key. The old file stays as it was.
- **Conflicts** need a choice before anything is written: `resolve` maps `"<path>#<key>"` (as the plan names them, e.g.
  `"kiln-auto-loot.json#range"`) to `pack` or `mine`, `all` answers the rest. Without one the update is refused with
  `conflict` and the plan in the error's `detail`.
- **Saves**: before anything changes, a save backup (reason `modpack`) of each game the instance is active on
  (`UpdateResult.backups`); a game without a save folder yet is skipped. Undo does the same. Placeholders keep saved
  stacks of content a removed mod defined (content.md "Missing content: placeholders").
- **All or nothing**: every file's merge is computed before anything is imported or written (a live file that does
  not parse stops the update there). The instance as it was is copied to `pack/undo.new/` (meta.json, the config
  folder, instance.json, base/, the player layer's local/); if a later step fails (the profile changed meanwhile, a file
  that cannot be written) the mods, the player layer, the config folder, base/ and instance.json come back from that
  copy and the error is returned. base/ is replaced by moving the old one aside first (`base.old`, read if a crash left
  no base).
- **Undo**: once the update stands, `pack/undo.new/` becomes `pack/undo/` (replacing the last one);
  `pack::undo(dirs, pid)` (`kilnctl pack undo`) puts it back (once; the next update replaces it), the player layer's
  working copy and its Library entry in the profile included.
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
`server` overrides go into `<server>/kiln/config` (a side's own override over the `both` one, whatever the index
order); `server.settings` into enshrouded_server.json (a running server is refused before any mod moves). With `modpackOut` the server modpack for players (format 1, [modpacks.md](modpacks.md)) is built right
after, so players joining get the same version through the existing join offer.

## Checks and the pack analysis
`kiln_manage::modpack::check::check(Input, &CheckOptions) -> CheckReport` (`kilnctl pack check`) is the analysis Crucible
runs on every upload (DESIGN.md section 6) and the launcher can show before an install; one code path for both. It reads
a whole `.kilnpack` (`Input::Pack`) or Crucible's stored form (`Input::Stored {index, files}`: the index as uploaded and
a folder holding every file it names as `<sha256>`: the mods' and the layer's archives as `pack::mod_archive` writes
them, the raw override files). The stored form is assembled into a `.kilnpack` by raw entry copies (nothing is
unpacked), so both go through `pack::read` and the same rules. It never fetches anything and never runs what a pack
holds; Crucible runs it in a sandbox without network (crucible repo, `src/worker.mjs`).

| Check | Level | Code |
|---|---|---|
| index rules, zip safety (no symlinks, absolute or `..` paths, dot entries, device names, case twins, every entry in a listed part), kdh1 of every mod and the layer, override sha256/size, cooked files | error | `index`, `zip`, `refused`, `hash`, `format` |
| sizes: 512 MiB a file, 2 GiB a pack, 50 000 entries (read from the zip's end record before its table is built), 4 GiB unpacked, 64 KiB an override, 1 MiB of overrides | error | `size`, `zip`, `refused` |
| stored form: every named file present with the index's size and sha256, sha256 values that are hex (never a path) | error | `missing-file`, `size`, `hash`, `index` |
| blocklist: sha256 of the archives, overrides and the pack, kdh1 of every mod and the layer (`--blocklist`) | error | `blocked` |
| secret scan of the overrides, server settings and the layer's settings-like files (the build's rules) | error | `secret` |
| resolver on servers and on game clients (dependencies, incompatible, cycles inside the pack); a mod needing a newer Kiln than the checker, or an optional one, is a warning | error / warning | `resolve` |
| kiln lint on the layer with the pack's Kiln mods as its profile (catalog with `--catalog`; Crucible's is built from the dedicated server's files and never shipped) | error / warning | `KLxxx` |
| data conflicts between every pair of mods (KL701 data-conflict, [lint.md](../modding/lint.md#kl701-data-conflict)); listed in `conflicts[]` | warning | `KL701` |
| code scan: managed assemblies (CLR header), native DLLs/EXEs (PE imports: networking DLLs, process creation, registry), scripts; manifest capabilities (`advanced`, `boot`, `writes`, `features`). Roslyn analyzers need source, so compiled assemblies are read for the API names they reference instead | warning (touches) / note | `code-network`, `code-process`, `code-registry`, `code`, `capability` |

Identification (`credits[]`, DESIGN.md 6.2, first match wins): Kiln's own mods (`--own`, Crucible's
`data/kiln-own-mods.json`: by id "likely", with a listed kdh1 "certain") -> earlier confirmed credits for the same kdh1
(`--known`; two different ones: "conflict") -> the manifest's `updates` (GitHub repository or Nexus mod id: named, not
fetched, "unverified") -> the manifest's authors and url -> the creator's own credit ("creator") -> nothing. Each row:
`{kind, id, version, kdh1, author, source (https), match, confidence, nexusModId?, flag?}`; `flag` says why a row needs a
look (the creator credits someone else than earlier packs did).

The report (`--json`, kind `kiln.pack-check`, format 1): `{ok, complete, pack {id, name, version, sha256?, size?},
summary {errors, warnings, notes}, findings[{level: error|warning|note, code, message, mod?, file?}], mods[{kind, id,
version, hash, name?, authors[], url?, license?, updates?, code {managed[], native[], scripts[], touches[], evidence[]},
capabilities, blocked?}], conflicts[{section, field, earlier {mod, file, selector, value}, later {...}, conditional}],
credits[], catalog}`. `complete` is false when the pack could not be read; the mods and credits then come from the index
alone. Exit 0 without errors, 1 with.

## kilnctl pack
| Command | What |
|---|---|
| `pack check FILE \| --index I --files DIR [--own F] [--known F] [--blocklist F] [--catalog C] [--work DIR] [--json]` | the pack analysis (above); no data folder needed |
| `pack build P --version V [--id] [--name] [--layer DIR] [--override PATH[:K,K][@both\|client\|server]]... [--optional ID[=off]]... [--client-only ID]... [--server-only ID]... [--server-settings FILE] [--changes TEXT] [--out DIR] [--json]` | a pack from profile P |
| `pack verify FILE [--sha256 S] [--json]` | every check; prints the mods, layer and overrides (no data folder needed) |
| `pack install FILE --yes [--name N] [--optional ID=on\|off]... [--json]` | an instance; prints how to activate it (`kilnctl client activate`) |
| `pack update P FILE --yes [--resolve PATH#KEY=pack\|mine]... [--all pack\|mine] [--optional ID=on\|off]... [--json]` | the update; prints the plan, and the conflicts when it refuses |
| `pack diff P FILE`, `pack diff OLD.kilnpack NEW.kilnpack` | the update plan of an instance, or what changed between two packs |
| `pack undo P`, `pack uninstall P`, `pack list` | undo the last update; remove an instance; list instances |
| `pack server FILE --dir SERVER --yes [--optional ID=on\|off] [--no-settings] [--modpack-out DIR]` | the server part on a server folder |

## API for the launcher UI and the Crucible service (later briefs)
Tauri commands (launcher/src-tauri/src/commands/pack.rs; the network side is "The launcher's Crucible client" below). Long ones are tasks (`TaskId`, progress on
`kiln://task`), as `modpack_install` is. Rust types are in `kiln_manage::modpack::pack`; all serialize camelCase.

| Command | Arguments | Returns | Crate call |
|---|---|---|---|
| `pack_inspect` | `{source: {path, sha256?} \| {url, sha256}}` | `PackPreview {index, sha256, size, missing[], present[], code[], optional{id: bool}, instance?}` | `pack::inspect` (download first for a URL) |
| `pack_install` (task) | `gameId, InstallRequest {source, consent, allowHttp?, name?, optional{id: bool}, activate?}` | `InstallResult {profile, instance, imported[], overrides[], backup?, activation?}` | `pack::install` |
| `pack_instance` | `profileId` | `Instance \| null` | `pack::instance` |
| `pack_instances` | | `[profileId, Instance][]` | `pack::instances` |
| `pack_update_plan` | `profileId, source, optional?` | `UpdatePlan {profile, pack, from, to, changes?, mods{added, updated, removed, yours, keptSwitches}, layer{from, to, changed}, files[{path, side, mode, taken[], kept[], conflicts[{key, base, pack, mine}]}], conflicts}` | `pack::plan_update` |
| `pack_update` (task) | `UpdateRequest {profile, source, consent, allowHttp?, optional?, resolve{"path#key": "pack"\|"mine"}, all?, drop?[]}` | `UpdateResult {plan, profile, instance, imported[]}`; error `conflict` with the plan in `detail` | `pack::update` |
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

Per-mod archives (`mods[].file`, stored once at f/<sha256>.zip for every pack): `pack::mod_archive(dirs, LibraryRef,
out) -> {sha256, size}` zips a Library entry the same way on every PC: the entry folder's files at the zip's root (a
plugin: its one .dll), the files a pack carries of it (no dot entries, no `assets/cooked/` of a Kiln mod), sorted
bytewise, every entry dated 1980-01-01 00:00:00, mode 0644, deflate level 6. No file time or folder path reaches the
bytes; kdh1 over the entries (for a plugin, the sha256 of the one file) is the entry's hash. The bytes depend on the
deflate code too: a Kiln that changes its zip library may compress differently, which only costs one more upload
(Crucible checks the kdh1, not the archive bytes, for identity). A .kilnpack itself is the same bytes for the same
input on one PC (entry times are the newest file time, `created` likewise), not across PCs; Crucible never needs that.

## The launcher's Crucible client
Code: `crates/kiln-manage/src/crucible/` (`mod.rs` client and flows, `sign.rs` the signed envelope, `links.rs` kiln://,
`dev.rs` a local service), `launcher/src-tauri/src/commands/{crucible,pack}.rs` (contracts.md section 9: `pack_*`,
`crucible_*`, event `kiln://crucible-link`).

- **Off until a key is known.** Indexes are trusted only through Crucible's Ed25519 signature, checked against public keys
  compiled in (`crucible::KEYS`). The list is empty until the owner approves the key the service makes at its first
  deploy (DESIGN.md 13.6); with none, `crucible_status.enabled` is false and the UI shows no Crucible surface. That is
  also the gate the design asks for until the privacy, terms and copyright pages are live.
- **Hosts.** The API `https://frostdev.io/kiln/crucible/api/v1` (the only host that gets the Bearer session), the CDN
  `https://cdn.frostdev.io` (indexes `m/<versionId>.json`, files `f/`, `l/`, `b/` by sha256), upload URLs only on
  `*.r2.cloudflarestorage.com`. Everything goes through `net::Client` (https, port 443, public addresses, no redirects);
  a URL from the service on another host is refused before anything is sent.
- **Receiving** (no sign-in): a code (`H7QK-3MZP`, any case, with or without the dash), a share link, a pack link or a
  kiln:// link -> `GET /codes/<code>` or `/packs/<slug>` -> the newest ready version's envelope from the CDN (the API's copy
  when the CDN has none) -> `sign::open` (the key id must be a trusted key, the signature over the payload bytes, then
  `check_index`; a payload naming another version is refused) -> the preview: what the Library lacks (`pack::lacking`:
  entries that still hash to their kdh1 count as present), sizes, game build fit, the Kiln range, code, the trust
  state, and Crucible's check of the version (`version.checks`, passed through as the service sends it: `{state:
  queued|running|waiting|done|failed|off, ok, errors[], warnings[], notes[], summary}`; the preview's facts show
  "Checks: passed", "N things to look at", "N problems found" (named, not blocking: Crucible keeps such a pack from going
  public), "still running on Crucible" with Refresh (crucible_open again: the full check ends minutes after an upload),
  or "couldn't run"; nothing from a service without checks; `launcher/src/crucible/code.ts` checksLine). Install: the lacking archives download to `<data>/crucible/cache/` (size and sha256 checked; cached for later),
  overrides by their sha256, then `pack::assemble` puts a .kilnpack together (Library folders + archive entries copied
  compressed) and `pack::read` checks every kdh1 as for any pack; `pack::install` makes the instance. A creator still
  uploading: `busy` with the progress. `count` (Settings "Count my installs", `crucibleStats`, on by default) sends the
  anonymous install count.
- **The installed pack's profile name** (dev.30.2): `InstallRequest.name`, from the preview's name field, defaulting to
  "<pack> (<creator>'s pack)" (the UI's translated form; the backend's English one when no name comes); either way
  `profile::unique_name` adds " (2)", " (3)"... when a profile has the name already (trimmed, any case, at most 128
  characters), in `pack::install` too: a friend's "My mods" never sits beside the player's own "My mods".
- **Install alone** (dev.30.2): `play: false` (`pack_install`: `activate: false`) makes the instance without the save
  backup and the activation: only the data folder changes (the Library, `profiles/<new id>`, the Crucible cache), never
  the game folder, so the preview's **Install** stays enabled while the game runs (and without Kiln in the game); **Install
  and play** still needs both. The toast has no Undo (the loaded profile did not change).
- **Share to Crucible on Play** has **What is Crucible?** beside it (https://frostdev.io/kiln/wiki/crucible-share in the
  browser). A profile's first share asks first (dev.30.2; players did not expect one click to upload ~20 MB): one screen
  says what goes up (the mod list, load order and switches, and the files of its N mods Crucible doesn't have yet, "up
  to" the Library entries' size: the archives are compressed and Crucible keeps files it has; `code.ts` shareEstimate),
  what stays (mod settings, the player layer; the Play share sends no overrides, Pack Studio does) and what is never
  sent (saves, worlds, characters, personal files), and that the pack is link-only (Public is shown off: public packs
  come from Pack Studio, which has the credits, the summary and the finished checks the service asks for). Share or
  Cancel; creating an unlisted share while signed out needs no Discord login on desktop or Deck. Anyone with the
  code can install; unlisted is not access-controlled. Update link for a pack shared before,
  Retry and a share stopped midway need no confirm. The Deck's Share dialog shows the same confirm.
- **Sharing**: new signed-out shares use `/anonymous/quickshare`, then `/anonymous/packs/<id>/versions`;
  a random 256-bit per-pack owner capability authorizes only those writes and scoped upload completion/resume.
  Native-only `<data>/crucible/owners/<hash-of-api-and-profile>.key` is atomically 0600 on Unix and DPAPI-encrypted
  for the Windows user. No owner key crosses IPC or enters a link, preview, log or `shared.json`. Keep the launcher
  data to update the same code; losing the key does not remove the pack or allow account recovery. Public publishing
  and existing account-owned shares keep their Discord session and existing gates. An account sign-out/forget does
  not delete unlinked anonymous packs. `SharedState.requiresSignIn` distinguishes account shares for the UI.
  Both paths reuse the same exact-import hashing, deterministic archives, override allowlist and secret scan.
  The authenticated flow remains: `pack::build` from the profile (version `1.0.0`, then
  the next patch; the change note "1 added, 1 updated" from the last share), `pack::mod_archive` per mod and
  `pack::folder_archive` for a layer (deterministic, cached by sha256), `POST /quickshare` (or `POST /packs/<id>/versions`
  for a profile shared before; a pack gone on Crucible is shared anew), PUT only what Crucible lacks (single PUT with its
  signed headers, or multipart parts with their ETags), `POST /uploads/<id>/complete`. `<data>/crucible/shared.json`
  remembers the pack per profile (the "Pack changed" pill: mods added, updated or switched, removed); a share stopped
  midway is finished by the next press (`POST /uploads` for the same files).
- **Updates**: `GET /packs/<id>/head` per Crucible instance (cached 5 minutes in the launcher), the new version fetched and
  planned with `pack::plan_update`, applied with `pack::update` (conflicts, undo as above).
- **Game clients take no server-only mods**: `fetch` downloads and assembles (`pack::assemble_for` with
  `pack::client_skips`, `<data>/crucible/packs/<vid>.client.kilnpack`) only what a client uses; the preview's sizes count
  the same. `pack::read_partial` reads such a file (`ReadPack.absent` names what it leaves out), install and update take
  it; the server part needs the whole pack (`pack server` with a full .kilnpack) and says so.
- **A dropped or opened file is checked here**: the preview of a .kilnpack runs the pack analysis
  (`pack_check` = `modpack::check::check` on the file, the code Crucible runs on uploads) and shows its errors in red
  (they block Install) and its warnings in yellow, "Checks: passed" otherwise.
- **Come back after installing Kiln** (DESIGN 1.3): the pack page's Get Kiln links `/kiln/download?crucible=CODE`.
  At start the UI asks `crucible_resume` once (`links::resume`): `firstStart` (no `<data>/crucible/started` yet) and a
  `code` the download carried: the Windows installer's note `crucible-resume.txt` beside the launcher (the NSIS
  post-install hook `launcher/src-tauri/nsis/hooks.nsh` writes the setup file's name; read once and removed) or, at the
  first start, the AppImage's own name, either holding `crucible-<CODE>` (`links::code_in_name`). A code opens that
  pack's preview ("Kiln is ready. Here is <pack>, the pack you came for."). The clipboard is never read because the
  launcher started or updated (`firstStart` is also true at the first start after an update from a launcher without
  Crucible): only `packWaiting` (the first start, the download's name says `crucible` but its code could not be read)
  reads it once and opens a Crucible link or code found there. Downloads whose name the launcher never sees (.deb,
  .rpm) carry no handoff: the player opens Got a code?, which fills in the clipboard's code. Every clipboard read is
  the backend's `crucible_clipboard` (arboard), never `navigator.clipboard`: WebView2 asks the player for clipboard
  access on every page read ("tauri.localhost wants to see text and images in the clipboard", dev.30). Only text that
  may be a Crucible code or link reaches the UI; Got a code? fills it in when it opens and from its Paste button
  (src/crucible/prefill.ts). Nothing installs without the preview's clicks. The
  download page's part (name the Windows download `...setup.crucible-<CODE>.exe` and copy the pack link when
  `?crucible=` is set) is the site's: out/pack-layers/TO-CRUCIBLE-WEB.md.
- **Trust**: "Always trust packs from <creator>" is kept in `<data>/crucible/trust.json` by Discord id; the warning shows
  every time.
- **kiln:// links**: `kiln://crucible/<CODE>`, `kiln://crucible/pack/<slug>`. `--link <url>` (or a bare kiln:// argument,
  nothing else) queues it in `<data>/crucible/inbox/`; the launcher holding `<data>/crucible/instance.lock` reads the
  inbox every 0.5 s and emits `kiln://crucible-link`. Registered at start while Crucible is on (Kiln is kiln://'s only
  handler): Linux `kiln-launcher-links.desktop` + xdg-mime, Windows `HKCU\Software\Classes\kiln`.
- **A local service** (debug builds, or the `crucible-dev` feature): `KILN_CRUCIBLE_DEV="api=http://127.0.0.1:<port>/kiln/crucible/api/v1
  cdn=http://127.0.0.1:<port>/<bucket> key=<base64>"` maps the local origins to made-up https hosts (`api.crucible.test`,
  `cdn.crucible.test`) that `crucible::dev::LocalTransport` sends to 127.0.0.1 with the Host header the service signed
  for. `CRUCIBLE_DIR=<service repo> node launcher/scripts/crucible-e2e.mjs` starts MinIO, the fake relay and the service and
  runs the launcher's ignored end-to-end test against it.

## Not done here (later briefs)
- engine mods whose settings live in their own folder (not the configDir) cannot carry overrides yet;
- a pack's optional mods are imported into the Library even when off (on purpose: switching one on later needs no
  download and works offline);
- the preview's local check of a dropped file reads it whole: a client-only pack (Crucible's
  `<vid>.client.kilnpack` cache, never offered to players) fails it;
- the Crucible browse page, comments and upvotes in the launcher. Pack Studio (making packs, credits, publishing link-only
  or public, Make public, server packs, kilnctl pack init/add/capture-settings/lint/credits/publish): [pack-studio.md](pack-studio.md).

## Checks done
dev.30.2 Crucible UX: unit tests (`unit/crucible.test.ts`: uniqueName, shareEstimate, checksLine), kiln-manage
`crucible::tests::an_installed_pack_is_named_after_the_pack_and_its_creator_and_never_twice` (the default name with the
creator, " (2)" for a taken name and for a typed one, the service's `checks` reaching the preview, `play: false` with
no game), `profile::review_tests::unique_names_add_a_number_when_a_profile_has_the_name`; Playwright crucible-share (What
is Crucible?, the confirm and Cancel, signed out: confirm then sign-in, Update link without a confirm), crucible-receive
(the check line passed / one warning / still running then Refresh, the name field and a taken name, Install alone while
the game runs), crucible-deck (the confirm on the Deck, the check line), crucible-resume (both install buttons stay off).

Player layer and update merge (dev.30 pack-layers): `tests/pack_layers.rs` (the player layer last across an update,
its data overlap with a renamed layer file, a drop refused for no overlap, Drop my change keeping comments, undo of
both; a renamed settings file new and already there with a conflict; a key new to the pack changed by the player since
vs. left alone; an override the pack stops shipping; a client pack without the server-only mod read, installed and
planned, refused whole and for a server; the save backup of the active game on update and undo), merge matrix unit
tests in `modpack::merge`, `modpack::layers` units, `crucible::links` resume, `crucible::tests` a client downloading
no server-only mod; Playwright crucible-update (overlaps, drop), crucible-resume, crucible-receive (file checks);
`crucible-e2e.mjs` with a player-layer change surviving the update and undo. The NSIS hook compiles (makensis); a real
Windows install from a renamed setup file is not tried.

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
examples. Review fixes (tests/packs.rs and pack.rs unit tests): `overrides[].size` written and checked; server settings
other than gameSettings refused; dot entries, device names and Unicode case twins refused; the secret scan's false
positives and negatives; a side override over the both one in either index order; an update whose merge fails and one
whose write fails both leave the instance as it was (mods, config, base, instance.json, no undo copy); per-mod
archives the same bytes after file times change, kdh1 from the entries. Not tried with a real game or server (the
owner's tests).
