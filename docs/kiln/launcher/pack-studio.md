# Pack Studio (Crucible packs, the creator's side)

Pack Studio is the launcher's tool for making Crucible packs (design: /mnt/Dongus/kiln-wave/dev29/out/modpack-share/DESIGN.md
section 3, build steps 6 and 9). It is desktop only; the Deck installs Studio-made packs like any other pack. It shows
while Crucible is on (crucible_status.enabled, the same switch as every Crucible surface): header **Studio**, routes
/studio (the packs, New pack) and /studio/<id>/<tab>. The format, instances and updates are in [crucible.md](crucible.md).

Code: crates/kiln-manage/src/modpack/studio.rs (the pack source), crates/kiln-manage/src/crucible/publish.rs (publish),
launcher/src-tauri/src/commands/studio.rs (studio_*), launcher/src/studio/ (UI; data/ the data editors),
runner/kilnctl/src/pack.rs (kilnctl pack init/add/capture-settings/lint/credits/build/publish).

## The pack source

~~~text
<folder>/                    the launcher keeps one per pack in <data>/studio/<id>/; kilnctl pack works on any folder (keep it in git)
  kiln-pack.json             the source (kind "pack-source", format 1)
  credits.json               attribution by mod id: {author, source (https), match}
  layer/                     the pack layer, a data-only Kiln mod: kiln.mod.json (written when the pack is made) + content/*.json
  versions/                  every version published from this PC (<id>-<version>.kilnpack; a server pack is built from one)
  out/                       scratch (builds for the analysis and the test instance)
~~~

kiln-pack.json: id (fixed once made; the layer is pack.<id>), name, version (the next to publish), **profile** (the
launcher profile the pack is made from), summary, description, authors, tags, changes, mods (by id: dep required |
optional with default, env {client, server}), overrides ([{path, side, keys}], captured, never typed), serverSettings
({gameSettings} only), visibility, gameBuilds, published ({pack, slug, code, url, visibility, versions[]}),
testProfile/testStarted, created, updated. The profile holds the mods (each the exact Library version, kdh1), their
order and switches, and the config folder the settings are captured from: the creator plays that profile, and the
Studio is a creator's view of it. Starting a pack "Empty" makes a new profile named after the pack. A mod switched off
in the profile is optional, off, unless the source says otherwise.

## Tabs
| Tab | What | Crate call |
|---|---|---|
| Mods | each mod: required / optional on / optional off, runs on players and servers / players / servers, where it comes from (the analysis); Add from my Library, Add file, Remove (the profile) | studio::open, save; library_add_to_profile, profile_save |
| Settings | "Capture from my current settings": every settings file of the profile (the override allowlist), the keys that differ from the mod's defaults (its .schema/ snapshot; without one, every key and "Whole file"); hotkeys and key bindings are marked personal and left out unless ticked; a key that looks like a secret can never be chosen (the secret scan). With a game whose active profile is the pack's, the game's config is copied back first. Server game settings: taken from one of the launcher's servers (gameSettings only, ticked keys) | studio::capture, set_capture; server_settings_get |
| Data | the pack layer's editors: Items, Recipes, Loot, Drops, New items ("add" clones), Raw JSON; pickers from the game-data catalog and what the pack's mods (and the layer) add, with the game's art icons; the live lint after every write | studio::layer_*, pick, fields, layer_lint |
| Load order | the resolver's order of the Kiln mods (a client hosting its own world), the pack layer forced last, the mods that do not load; the data conflicts of the analysis (later wins) with "Set in the pack" (content/pack.json {section: {selector: {field: value}}}, comments kept) | studio::order, layer_set; analyze |
| Test | "Test this pack": the source built and installed as "<name> (test)" (pack::install: save backup, activation), the last test instance replaced (a game that has it loaded gets the pack's own profile back first: an active instance cannot be removed); Play starts it; what the game logged in its last boot (conflict lines, errors and warnings) comes back; End the test removes the instance and loads the pack's profile again | studio::test, test_report, test_end |
| Credits | a row per mod prefilled from the analysis' identification (crucible-checks: Kiln's own list, earlier credits, GitHub, Nexus, the manifest), confirmed or filled by the creator; public packs need an author and an https link for every mod | studio::analyze (modpack::check), set_credits |
| Publish | version (proposed: the next patch), what changed, game build tested, link only / public (summary, the attestation), the checks of the analysis; Publish; the published versions with "Put on a server"; Save as file | crucible::publish; studio::build |

The analysis (studio::analyze) is made again when Studio.revision changes (a hash of the source, the credits, the layer
files and the profile). It is the source built into out/analyze, then modpack::check over it (the same checks
Crucible runs on upload: format and zip safety, sizes, known-bad hashes, code scan, resolution, lint, data conflicts
KL701, prefilled credits). Offline; the launcher passes no Kiln's-own list and no earlier credits (Crucible identifies
again on upload and keeps confirmed credits across versions).

## Publish
crucible::publish::publish(dirs, crucible, folder, PublishRequest {version, changes, visibility, attest, gameBuilds}):
1. precheck (nothing is sent when it fails): a SemVer not published before, at least one mod; for public: a complete
   credit for every mod, a summary of 10+ characters, the attestation;
2. the version, changes and builds go into the source, which is built (pack::build) and split into Crucible's archives
   (the share's own code: one per mod, the layer, each override; cached by sha256);
3. POST /packs (the first time) or POST /packs/<id>/versions (a pack gone on Crucible is made anew); only the files
   Crucible lacks are uploaded;
4. PUT /versions/<v>/credits with every complete row;
5. PATCH /packs/<id>: title, summary, body, tags, and visibility when it changes. Crucible checks again before public
   (member of the Kiln Discord with an account 3+ days old, its analysis, credits, summary); a refusal leaves the pack
   link-only and its reasons come back as needs;
6. the .kilnpack is kept in versions/, published is recorded, the next version becomes the next patch.
An update of a public pack with unchanged credits is two clicks: the Publish tab, Publish.

Failures midway: the pack and the version are written into the source (published, the version pending) as soon as
Crucible answers step 3, so a publish that stops later (an upload, the credits, the visibility) resumes that pack and
version when the same version is published again (POST /uploads for what is still missing). A pack Crucible says is
gone (410: deleted or taken down) is never quietly made anew: the error says so (detail.gone) and the creator chooses
"Publish as a new pack" (PublishRequest.fresh); the local version history stays. A public pack Crucible did not take
yet (its checks still running, a Discord account too new) stays link-only with Crucible's reasons as needs; **Make
public** (studio_set_visibility, crucible::publish::set_visibility) asks again without a new version, and "Make it link
only" goes back.

A **server pack** from a published version ("Put on a server", studio_server_pack): pack::install_server of the kept
.kilnpack onto one of the launcher's servers (its server mods, the both/server overrides, gameSettings; refused while
the server runs) and the player modpack (format 1) built from that server into <data>/modpacks/built/<server>/, as
kilnctl pack server --modpack-out does. Servers test through the lab path (a lab server of the launcher).

## kilnctl pack (pack sources)
| Command | What |
|---|---|
| pack init DIR --name N [--profile P] [--id ID] | a pack source in DIR (from profile P, else a new empty profile) |
| pack add DIR KIND:ID@VERSION \| ID[@VERSION] \| PATH... | Library entries or files/folders (imported) into the source's profile |
| pack capture-settings DIR [--take PATH[:K,K][@side]]... [--take none] [--json] | the settings that differ from the defaults; --take sets what the pack carries (checked: no secrets, keys that exist) |
| pack lint DIR [--catalog C] [--json] | kiln lint over the layer (exit 1: errors) |
| pack credits DIR [--set ID=AUTHOR,URL]... [--accept] [--json] | saved credits and the analysis' suggestions; --accept takes every identified row |
| pack build DIR [--version V] [--out O] | the .kilnpack of a source given as a path (./my-pack); --version builds another version without changing the source; a bare name is a profile id as before |
| pack publish DIR [--version V] [--changes T] [--public --yes] [--session-file F] | publish (session: KILN_CRUCIBLE_SESSION or the file; a local service through KILN_CRUCIBLE_DEV in debug builds) |

kilnctl is built with the feature crucible (default; kiln-manage http) for publish.

## Tauri commands (contracts.md section 9)
studio_list, studio_create {name, from?, id?}, studio_get, studio_save (id, profile, published and test fields stay as
stored), studio_delete (packId, gameId?: the folder to backups/studio/, the test instance removed, the profile kept),
studio_credits_set, studio_capture (gameId?: copy back first), studio_capture_set, studio_order, studio_layer_files,
studio_layer_read, studio_layer_write / studio_layer_delete / studio_layer_set (each answers the layer's LintReport),
studio_layer_lint, studio_pick, studio_fields, studio_analyze (task -> CheckReport), studio_test (task ->
pack::InstallResult), studio_test_report, studio_test_end, studio_build (task -> BuiltPack), studio_publish_check
(the precheck, sends nothing), studio_publish (task -> PublishResult {pack, version, files, uploaded, skipped, public,
needs, file}), studio_set_visibility (packId, visibility, attest -> {pack, public, needs}), studio_server_pack (task ->
ServerInstallResult); studio_test_end takes gameId? too.

## Limits
- The editors write plain JSON: // comments in a layer file an editor saves are lost (Raw JSON keeps text as typed).
- A test instance shares the game's saves like any profile (crucible.md "Limits"): the Studio says to use a new world.
- The launcher's analysis has no Kiln's-own list and no network identification (GitHub, Nexus); Crucible does both on
  upload and prefills the version's credits.

## Checks done
cargo test -p kiln-manage --features zip,http,server --test studio (pack sources from a profile and empty, unique ids,
delete; capture against .schema defaults, personal and secret keys, set_capture refusals, the build carrying exactly
the chosen keys; choices, layer files and path safety, Set in the pack keeping comments, credits (https only, only
complete rows in the index), deterministic builds, keep_version and the publish record; the order with the layer
last and picks of what mods add; the test instance replaced by the next and removed at the end, the game log's last
boot read back; a test instance loaded in a game replaced and ended with the pack's profile loaded again; credits of
mods that left, build_as, the revision, no profile left behind by a refused create; the analysis' credits).
cargo test -p kiln-manage --lib crucible (publish against the in-process fake service: signed out, the precheck sending
nothing, link-only, the same version refused, public with credits and the PATCH, only new files sent; a publish whose
credits call fails resumes the same pack and version; Make public; a gone pack refused until fresh, history kept). The ignored launcher test commands::studio::tests::studio_publish_end_to_end_against_a_local_crucible
runs in launcher/scripts/crucible-e2e.mjs against the real service, MinIO and the fake relay: link-only, then public
with credits, then a friend installs it by its code (layer and captured key included). Playwright (mock mode):
tests/studio.spec.ts (every tab's flow, Crucible off, the Deck without Studio) and tests/studio-data.spec.ts (the
data editors). Not tried with a real game or server (the owner's tests).
