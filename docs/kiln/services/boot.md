# kiln.boot: the boot generation (for native authors)

kiln.boot is the in-core service behind `Mod.OnBoot` (since 0.2.0-dev.28). Mod authors read
[../modding/onboot.md](../modding/onboot.md); this page is for native services that take part as a **collector**, and
for anyone who reads `kiln/state/boot.json`. The ABI is `native/core/abi/services/kiln_boot.h` (`KilnBootApiV1`,
`kiln.boot` v1); the collector side has a header-only helper, `native/core/abi/kiln_boot_collector.hpp`.

## The phase
One process has at most one boot generation. Every step runs on the boot thread, before the game's entry point.

1. **Decide**, once, before the service DLLs load: the process has a boot generation ("deferred mode") when hooks and
   .NET are on, an enabled Kiln mod of this side has `"boot"` in its `kiln.mod.json`, `mods.boot` is available, and on
   a game client `mods.boot.client` is available too. Otherwise nothing below happens, every
   staging call answers `KILN_ERR_UNAVAILABLE`, and every collector keeps its `on_ready` path. An enabled mod with
   `"boot": "required"` still stops the start with 0x4B01 when that route is unavailable; its content cannot be omitted.
2. **Bind:** each collector calls `collector()` once, from `kiln_plugin_main` or `on_ready`. `KILN_OK` means it will be
   prepared and must not load its definitions in `on_ready`. Registration closes when every `on_ready` ran; a collector
   that did not register makes `mods.boot` unavailable (`service-missing`), and Kiln.Host then refuses boot mods.
3. **Authors:** inside the .NET start Kiln.Host runs each boot mod's `OnBoot` on a throwaway copy:
   `begin_owner`, `stage` for each definition, `finish_owner`.
4. **Lowering:** Kiln.Host turns what was staged into each collector's own JSON and stages one IR envelope as its own
   (kind `ir`, key `envelope`).
5. **Commit:** `commit()` prepares every registered collector in the fixed order with the envelope, then publishes one
   generation root (status `PUBLISHED`).
6. **Check:** after the .NET start the core reads `status()`. Anything but `PUBLISHED` holds the entry point, writes
   `progress.json` ("failed"), `boot.json` and kiln.log, and ends the process with exit code `KILN_BOOT_EXIT_CODE`
   (0x4B01). No save or world is loaded.

## The table
| Member | Thread | Does |
|---|---|---|
| `status(out)` | boot | the phase: `COLLECTING`, `PREPARING`, `PUBLISHED`, `ABORTED`, `ACTIVATION_FAULT`, and the generation |
| `begin_owner(author, mod_id, roles, &gen)` | boot | opens a mod's author frame (Kiln.Host only) |
| `stage(owner, gen, kind, key, json, out)` | boot | stages one definition (Kiln.Host for the author; the envelope from the host) |
| `finish_owner(owner, gen, accept)` | boot | closes a frame: accepted, its `OnBoot` failed, or a dependency is out |
| `read_resource(owner, query, buf, cap, &required)` | boot | one record of the offline game-data view ([../modding/boot-gamedata.md](../modding/boot-gamedata.md)) |
| `commit(host, gen, out)` | boot | prepares and publishes (Kiln.Host only) |
| `owner_state(mod_id, out)` | any | one mod's state (the numbers of `Kiln.BootModState`) |
| `issues(gen, out, cap, stride, &required)` | any | the generation's issues, oldest first |
| `collector(owner, id, prepare, user)` | bind only | registers a collector |
| `active_plan(id, &gen)` | any | the published plan of a collector; NULL before the publication or without a generation |
| `activation_fault(owner, mod_id, kind, key, reason)` | any | appended in dev.28: published content of a mod could not be applied (below) |

Every out struct starts with its size, set by the caller; a smaller size is `KILN_ERR_INVALID_ARGUMENT`. A too-small
buffer writes nothing (`KILN_ERR_BUFFER_TOO_SMALL` with `*required`). Strings in are UTF-8 and copied.
`activation_fault` was appended to the v1 table: check `header.size` before calling it (the helper does).

Limits: a definition is at most `KILN_BOOT_DEFINITION_MAX` (1 MiB) of JSON, an author stages at most
`KILN_BOOT_OWNER_DEFINITIONS` (4096), the envelope is at most 64 MiB; one game-data read is at most 4 MiB and one author
reads at most 16 MiB.

## Collectors
Seven collectors, prepared in this order: `assets` (kiln.assets), `worldtables` (kiln.worldtables), `content`
(kiln.content), `skills` (kiln.skills), `knowledge` (kiln.quests), `vfx` (kiln.vfx), `patches` (kiln.content, resource
patches). A new collector id needs a core change: the list is closed.

**Bind.** `kiln::boot::collect(api, owner, "<id>", prepare, user)` registers and says `true` only in a boot process.
Then the service keeps its definitions back in `on_ready`. `false`: the old `on_ready` path, untouched.

**Prepare.** `KilnResult prepare(void* user, const KilnBootPrepareInputV1* in, KilnBootPreparedV1* out,
KilnBootIssueV1* issue)`, on the boot thread, with no lock of kiln.boot held.
- Read with `kiln::boot::parse(in)`: `excluded` (mods whose definitions **and files** stay out), each participant's
  process owner (`boot:<mod>`, alive until the process exits), and `docs("<id>", mod)`, this collector's documents of
  one mod in envelope order. Each document is the JSON the service already takes from Kiln.Blocks or its own files.
- Precedence for one identity: the mod's data file, then its baked or generated file, then its `OnBoot` documents,
  then the pending cache of earlier runs. Keep the first.
- It may read files and other services' tables, cook and allocate. It must not publish, register game content, call
  `when_resources_ready` or wait for any readiness: the game is held, such a wait never ends.
- Success: `kiln::boot::prepared(out, plan, destroy)`, `ok = 1` and a non-null plan (an empty plan is a pointer to
  something that holds nothing). A warning travels with `ok = 1` in `*issue`, bit 31 of `code` set.
- Refusal: `kiln::boot::refuse(out, issue, mod, kind, key, why)`, `ok = 0`. A document the collector cannot read
  throws `kiln::boot::Refusal(mod, kind, key, why)` in the helper's reading code and becomes such a refusal. Name the
  mod: a refusal that names none stops the start.
- Any other result code, `ok = 1` with a null plan or an error code, or a fault is a broken collector and stops the
  start.

**Required participants are prepared strictly.** Each participant in the input carries `"required"` (its
`kiln.mod.json` says `"boot": "required"`); `Input::strict(mod)` reads it. For such a participant every entry error the
collector would log and skip for another mod refuses the participant instead: a file of the mod that cannot be read or
is not JSON, a malformed entry or node, a file the cook trust refuses, a cooker failure. Call
`kiln::boot::entry_error(input, mod, kind, key, why)` where the error is found: for a required participant it throws
`Refusal` (the prepare takes back what it read and answers with `refuse()`), otherwise it returns `true` and the
collector logs and skips the entry as it always did. `key` names the file or the entry (`assets/assets.json`,
`skills/nodes.json`, `blocks/tweaks.json`), `why` says what is wrong in words that name the entry
(`entries[1] (resource good.bin): resource: bad guid`). Kiln's own data (what it kept from a mod's code at an earlier
start) is not the mod's boot content and stays lenient. What each collector checks:

| Collector | Refuses a required participant for |
|---|---|
| assets | `assets/assets.json` (or a boot document) that cannot be read or is not JSON, any entry error (`entries[<i>]`: a bad GUID or type, a file that cannot be read or lies outside the mod, a patch blob, a refused put), a cooked file this game's cook did not make; on a game client, after the entry point (activation fault): a texture override or a game item's icon queued until the game's resources come up that then fails |
| worldtables | a `blocks/*.json` or `blocks.tables.json` that cannot be read or is not JSON, any world-table block error; after the entry point (activation fault): a block the merge cannot apply (an unknown material or item, no room), or kiln.worldtables going off |
| content | every content document error (as for every participant: a refusal drops an optional mod whole); after the entry point (activation fault): an item, recipe or clone of the mod that a world does not register when it loads its registries (the stop comes at world load) |
| skills | a `skills/*.json` that cannot be read or is not JSON, `gameTree` that is not `{"nodes": [...]}`, a node that is refused (its GUID, its fields, a GUID another mod defines), a boot node without a GUID; after the entry point (activation fault): a node the first tree build leaves out (an unknown `like`, `from` or `to`, a GUID whose id is a game node's or another mod's) or a tree that does not build at all (the stop comes when a world loads its skill tree, before the game reads it) |
| knowledge | a `blocks/*.json` or `quests/*.json` (and `quests/generated/`) that cannot be read or is not JSON, an `assets/cook.json` that is not JSON; after the entry point (activation fault): the mod's quests, knowledge or NPCs that the build leaves out, or a build that serves nothing |
| vfx | a game client: a blocks file left out (a link, over a limit, cannot be opened, nested too deep), any VFX or material error of the cook, a mod refused by the budget or a manifest rule, and VFX or materials of the mod (its files or staged) while this start builds none of that kind (kiln.vfx off by `kiln/config/vfx.json`, a game build whose particle or material layouts changed, no kiln.assets, a failed build; staged ones while the client gate holds `mods.boot.vfx`). A dedicated server builds no VFX by design (each player's game builds them, with the same boot mod): its vfx collector prepares an empty plan and refuses nothing for this; kiln.log says so for a required mod with staged VFX |
| patches | every patch error (as for every participant), and a blocks file that names `resourcePatches` and cannot be read or is not JSON. Blocks files are read as Kiln.Blocks reads them (comments and trailing commas: `kiln_blocks_json.hpp`) |

The stop line names the entry: `boot: the start stops before the game's entry point (exit code 0x4B01): <mod>:
<kind> <key>: collector <id>: <reason>; state/boot.json lists every boot mod's state and issue`.

**Destroy.** An unpublished plan goes to `destroy`; a published one lives until the process exits. Most collectors
prepare into their service's own state and undo it in `destroy`: kiln.boot calls it on every failed round, in reverse
order, before it prepares again.

**Failure policy.** A refusal, a failed `OnBoot` or a dependency that is out drops that mod's whole contribution:
every plan prepared so far is destroyed in reverse order, the mod and every participant that requires it go into
`excluded`, and every collector prepares again. Nothing of a dropped mod is published. A mod with
`"boot": "required"` (`KILN_BOOT_ROLE_REQUIRED`) that fails, a refusal that names no mod, a collector fault or a broken
contract stop the start instead.

**Consumer.** The hooks the game reaches after the entry point read what was prepared: through
`kiln::boot::published(api, "<id>")` (the plan) or the state the prepare filled.

## activation_fault
A collector whose published content cannot be applied after the entry point (kiln.content's `patches`, when a
resource patch cannot be written at `when_resources_ready`; kiln.worldtables, a required mod's block its merge cannot
apply; kiln.knowledge, a required mod's quests its build leaves out; kiln.skills, a required mod's node the first tree
build leaves out; kiln.content, a required mod's definition a world does not register; kiln.assets on a client, a
required mod's texture that cannot be applied) reports it. The helper `kiln::boot::LateFaults` keeps the prepare's
required set and says once per mod when to report:
`kiln::boot::activation_fault(api, owner, mod, kind, key, reason)`. kiln.boot records an issue (code
`ActivationFault`, phase `activation`), and the mod's state and the phase status become `ACTIVATION_FAULT`; nothing is
undone. For a mod whose boot content is required the call does not return: the state files are written and the
process ends with exit code 0x4B01, as before the entry point. A required transitive dependent also stops the start;
dependent boot owners become `ACTIVATION_FAULT`, and managed `Mods.BootState` reads the native state so they cannot
load against the failed content. Only a registered collector's owner may call it.

## The exit code
`KILN_BOOT_EXIT_CODE` is 0x4B01 (19201). A start that kiln.boot stops ends with it, before the game's entry point or
at an activation fault of a required mod; `boot.json` says `"exitCode": 19201`. Under Wine the Linux parent gets only
the low byte as the exit status: 1. Seen on a test server (the lab's wrapper writes it to `kiln/state/exit-code.txt`):
1 for a stop before the entry point (a required mod's patch of an unknown type) and for one after it (a required mod's
patch of a missing resource), 0 for a normal stop with Ctrl-C. Not run on Windows, where a parent sees the whole
code. kiln.log ends with `boot: the start stops before the game's entry point (exit code 0x4B01)` (or
`boot: the start stops (exit code 0x4B01)` after the entry point).

## boot.json
`kiln/state/boot.json`, section `boot`:

| Key | Says |
|---|---|
| `deferred` | whether this process had a boot generation; `reason` says why (or which boot mods) |
| `bootMods` | the boot mods found, `(required)` marked |
| `collectors` | the collectors that registered |
| `generation`, `status`, `published` | the generation and where it ended: `published`, `aborted`, `activation-fault` |
| `owners` | each participant: `id`, `author` (it ran `OnBoot`), `required`, `state`, `definitions`, `issues` |
| `issues` | every issue: `mod`, `kind`, `key`, `phase` (stage, callback, prepare, commit, activation), `code`, `reason`, `warning` |
| `exitCode` | 19201 when the start stopped, else null |

## After the commit (managed side)
Kiln.Host freezes each boot mod's inputs at the commit (every file of its folder but Kiln's own and the runtime's, JSON by
its parsed value; and the effective values of each config its `OnBoot` registered) and refuses a later runtime load of a changed mod with
`RestartRequiredBoot` ([../modding/onboot.md](../modding/onboot.md#after-a-change-restart)); the IR envelope's
`owners[].fingerprint` is the hash of those inputs. Kiln.Blocks keeps a process-wide map of the committed definitions'
handles, which no runtime unload clears: `Blocks.Find` and a block's reference by name fall back to it.

## Rows
`mods.boot`, `mods.boot.phase`, `mods.boot.game_data`, `mods.boot.client`, `mods.boot.vfx` and
`mods.boot.resource_patches`: [caps.md](caps.md#rows-added-in-dev28).

## Tests
- Host: `native/core/tests/test-boot-service.sh` (kiln.boot with fake collectors: staging, every drop and abort,
  reverse destruction, the seven collectors, files-only participants, `activation_fault`, the game-data reader;
  `BOOT-STRICT-*`: the `required` flag in every prepare's input, and per collector slot a collector built on
  `entry_error` with a malformed file, a malformed entry and an unreadable file, required (the start stops naming the
  entry, nothing published) and optional (the entry skipped, the rest published)).
- Offline: `managed/tests/Kiln.Blocks.Tests` (staging, lowering, `BootHandleTests`: the handle map), `Kiln.Host.Tests`
  (`BootPhaseTests`, including the pin of `KilnBootApiV1`'s member order; `BootInputsTests`: the frozen inputs).
- Offline, the test kit: `Kiln.TestKit.Tests` (`BootFakeTests`: `KilnHarness.BootPhase`), `Kiln.Sdk.Tests`
  (`BootBakeTests`: the bake runs `OnBoot`).
- Lab: `mods/tests/boot-definitions-test` (the five kinds on a first start, the same ids on the next start, and with the
  fault mods `boot-callback-fail` and `boot-dependent` their exclusion; `Blocks.Find` and `Blocks.Define` of a boot block in
  `OnLoad`), `mods/tests/boot-reload-driver` beside it (unload, load unchanged, a block file added: refused with
  `RestartRequiredBoot`, removed: loads), `mods/tests/boot-patches-test` and
  `boot-patches-other` (resource patches), `mods/tests/boot-gamedata-test` (the snapshot against the live reader),
  `mods/tests/boot-required-test` (eighteen fixture mods, one bad entry each for a collector: required, each start stops
  with exit status 1 and a stop line naming the entry, before the game's entry point or, for what only the game's data or
  a world shows, when it loads; a resource patch in a file with comments and trailing commas goes on; optional, one start
  with all of them goes on and skips them). Client only: the guided check `08-boot.required-client` with the fixtures
  `run.sh client` makes (a texture override, VFX while kiln.vfx is off).

## Lab switches
- **`boot.testRefuse`** (kiln.json, honoured only with `"policy": "any"`; a release never sets it):
  `"boot": { "testRefuse": { "<mod id>": "<collector>" } }`. Each round, kiln.boot hands that collector an unreadable
  document of that mod (key `lab.test-refuse`), so the collector refuses the mod as it refuses any such document and the
  mod is dropped (or the start stops, for a `"required"` mod); every collector prepared before it destroys its plan and
  prepares again. kiln.log says `boot: lab switch boot.testRefuse: collector <c> gets an unreadable document of <mod>`.
  It forces a collector's refusal in a lab start (the per-collector drop proof).
- **`boot.testFault`** (kiln.json, the same rules: policy `"any"` only, never in a release):
  `"boot": { "testFault": { "<mod id>": "content" } }`. kiln.boot names the mod in every prepare's IR (`"testFault"`;
  `Input::test_fault`), and kiln.content makes every world refuse that mod's first boot item or recipe at registration.
  It is the proof of a late fault that a typed definition cannot cause on a test server: a required mod stops the start
  when the world loads its registries, an optional one runs without the definition. kiln.log says
  `boot: lab switch boot.testFault: collector content fails a definition of <mod> after the publication`. Only
  `content` has a late fault to force; another collector name is ignored with a warning.
- **The fault start's exit status**: `tools/lab/lab.sh start` writes the server process's exit status to
  `<lab>/kiln/state/exit-code.txt`. A start stopped by a boot failure reads `1` there under Wine (the low byte of
  0x4B01); `boot.json` says `"exitCode": 19201`.
