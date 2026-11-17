# The game-data snapshot for OnBoot

`Mod.OnBoot` runs before the game has loaded anything, so the game's items, templates, skill nodes, materials and other
resources are not in memory yet. What `boot.GameData.Read(...)` answers comes from a **snapshot**: a copy of those records, made from
the game's own files before the start, once per game build. This page is about the snapshot: who makes it, where it
lies, what it costs, and what a mod sees when it is not there.

## Who makes it

| Who | When | What you see |
|---|---|---|
| The Kiln launcher | at Play (and Launch in a server's modpack offer), right after the cook, and before a single start with another profile ("play once", which cooks nothing), when an enabled Kiln mod of the start has `"boot": true` or `"required"` in its kiln.mod.json | a progress line ("Game data for OnBoot mods (about 200 MB, once per game build): …"); while it builds one, the start's own bar shows the step "Making the game data for OnBoot mods (once per game update)" with the tool's line (none when the snapshot is there); a notice in one line only when something went wrong. Without such a mod: "no mod needs the boot snapshot", and nothing runs |
| `kiln boot-snapshot --game <dir> [--force] [--json]` | by hand, whatever the mods say: a server you start yourself, or to make it again | progress on stderr, then one line (or the JSON result) |
| `tools/deploy-server.sh` | at each deploy into a server folder, when an enabled Kiln mod in its profile or allowed drop-in folders has `"boot": true` or `"required"` (`KILN_NO_BOOT_SNAPSHOT=1` skips the step) | the tool's lines; "no mod needs the boot snapshot" without such a mod; a warning when it was not made |

All three run the same tool (`tools/bootsnap/bootsnap.py`, carried by the launcher and by `kiln`) with Kiln's Python:
the launcher's own on Windows (python.org's embeddable 3.14, in the launcher's folder), the system's `python3` on Linux
(3.10 or newer; one older than 3.14 needs the system's zstd library or the `zstandard` package), and for `kiln.exe` on
Windows `KILN_PYTHON`, a `python\python.exe` beside it, or `python` on `PATH`.

**The producer does not refuse a start.** If Python or the tool is missing, if the tool refuses or fails, or if it takes
longer than 10 minutes (the launcher stops it then), the launcher continues without a snapshot and says so in one line.
An OnBoot mod must still handle unavailable game data: an exception in its callback is subject to the boot phase's
startup failure policy.

The main Play action runs this step for the active profile, after refreshing that profile and cooking it; Launch in a
server's modpack offer does the same for the modpack's profile its install made active. A single start with another
profile ("play once", launch mode `once`) cooks nothing, but runs the same step for the mods of that start before the
start writes its profile file (`kiln_manage::boot_snapshot::boot_mods_once`: the profile's enabled Kiln mods, and the
Kiln drop-ins when that profile loads them). Launching through Steam uses only a snapshot already made; it does not run
the producer (run `kiln boot-snapshot --game <dir>` once for that game build, or start the game through the launcher once).

## What it costs

**Only a game or server with an OnBoot mod pays it**: the launcher and the deploy step make the snapshot only when an
enabled Kiln mod has `"boot": true` (or `"required"`); a dedicated server does not count client-only mods. Mods of other
loaders (EML, SET, ShroudForge, Flame, ...) never need it. When no mod needs it, a snapshot already there is left as it
is (a later start with an OnBoot mod reuses it).

- **The first start after a game update**: half a minute to a minute (32 to 44 s measured on a busy PC, the client's
  and a dedicated server's data alike). A later start hashes the exe, reads the 64 KiB archive header, and verifies
  checksums of all ten snapshot files (about 300 MB of reads). Reuse time depends on storage speed; the earlier
  sub-second measurements predate checksum verification. A damaged snapshot is rebuilt. Older snapshots without
  producer checksums are rebuilt once, while the reader's format remains 1.
- **Disk**: about **300 MB** in the game folder (game build 1287369: 292 MB for the dedicated server, templates 158 MB,
  resources 80 MB, items 48 MB; about 335 MB for the client, whose resources are 117 MB). One
  snapshot is kept: the one of the current game build. An older one is deleted only after the new one is written and
  reads back.
- It is written into the game folder you play (or serve) from; a client and a dedicated server each have their own.

## Where it lies

`<game folder>/kiln/data/boot-gamedata/<key>/`, where `<key>` names the game build: the first 32 hex digits of the
SHA-256 of the game exe's SHA-256 and the version string of the game's `.kfc`. kiln.boot computes the same key at boot
and opens only that folder. The key hashes the exe's 64 lower-case SHA-256 hex characters, one newline, then the archive
version bytes, and keeps the first 32 lower-case hex characters. The files (`index.json` written last, alongside one
index and one data file per kind) follow
the snapshot contract, format 1, which Kiln's boot service reads; a folder without a readable `index.json` is not a
snapshot.

The producer refuses symlinks and Windows junctions in the output tree below the selected game folder. Its `--root`
option must name that same output directory. A concurrent producer refuses immediately; a later start can retry.

## What it holds

Each value is the same camelCase JSON object kiln.content's live game-data reader gives for that record after the world
has loaded (member names as the game's reflection has them, GUIDs as text, enums and flags by name):

| Query | Entries |
|---|---|
| `item:<debugName>` | every `keen::ItemInfo` (`item:Material_T1_Wood_Logs`) |
| `template:<name>` | every template, by its name (or its GUID when it has none) |
| `skill:0x<8 hex>` | the nodes of the game's skill tree, by id (`skill:0x8efcc472`) |
| `material:<name>` | the rows of the game's building and terrain material table, named as the `GameMaterials` catalog names them (`material:StoneRough`, `Cell<id>` for a row no item places) |
| `names:resources/<qualified type name>` | every resource of the archive, of every type: its GUID, part and name (`names:resources/keen::SceneResource`) |
| `resource:<GUID>/<type>/<part>` | the value of a resource of any type, the type as its 8 hex hash or its qualified name, except the bulk asset types below |

### Every resource

Since kiln-bootsnap 2 the snapshot also lists **every resource** of the game's archive (`resources.index.json`), by
qualified type name: every type in the archive can be listed, 85 types and 43,958 resources on the dedicated server of
game build 1287369, 146 types and 91,909 resources on its client. A resource has a name when its type's root has a
`debugName` or `name` string: on the server `TemplateResource`, `ItemInfo`, `SequenceResource`, `Perk`, `MidiResource`
and `VoxelBlueprintConfig`; the client adds `SoundResource`, `RenderModel`, `UiTextureResource`,
`RenderMaterialResource`, `RenderTextureResource`, `GpcResource` and `SoundReverbPresetResource`. Every other resource is
listed by its GUID.

`resources.data` holds the value of every resource that is not an item or template already stored in its kind, so
`resource:` reads a `SceneResource`, the `BalancingTable` or the `RecipeRegistryResource` in `OnBoot`. Not stored:

- **bulk asset types**, listed with their names but without values (one `skipped` line per type):
  `keen::JointAnimation`, `keen::RootMotionAnimation`, `keen::SceneEntityChunkResource`, `keen::RenderModel`,
  `keen::RenderModelChunkModelResource`, `keen::VfxResource`. Storing them would cost about 960 MB and 7 minutes on the client
  per game build (JointAnimation alone 600 MB and almost 6 minutes);
- **values over 4 MiB**: on build 1287369 `GameKnowledgeResource`, `GameKnowledgeQueryResourceDb`,
  `SceneRandomLootResource` and, on the client, `MaterialFeedbackCollectionResource`.

A `resource:` read of either answers unavailable. A resource value may hold up to 1,000,000 JSON nodes (the four
kinds: 100,000).

What the list costs, measured on game build 1287369 (a busy PC, load average about 30):

| | dedicated server | client |
|---|---|---|
| `resources.index.json` | 3.2 MB, 43,958 entries | 7.2 MB, 91,909 entries |
| `resources.data` | 80 MB | 117 MB |
| added time to make the snapshot | 24 s | 31 s |
| listing alone (names, without values) | 0.2 s | 0.7 s |

The names of items and templates are those their kinds read; only the other named types' strings are read again (two
small reads each). A snapshot made before kiln-bootsnap 2 has no list: it is made again once at the next start through
the launcher. Until then every other key reads as before and `names:resources/...` answers unavailable with what to
run.

It is **the game's data as it shipped**, read from the game's own archive: when EML (or another loader keeping EML's
backups) has patched the archive, the snapshot is made from the backup pair it kept (`.kfc.bak`,
`.kfc_resources.bak`). A record an EML mod patches therefore reads as the game's own in `OnBoot`, and as the patched one
after world load.

### Limits

- **40 template names occur twice** in the game's archive (game build 1287369). The snapshot keeps the first in the
  archive's order and lists the others under `skipped` in `index.json`. A mod that reads one of those names in
  `OnBoot` may get another template than the live registry gives after world load.
- **Materials** have no live reader to compare with: their rows are checked against the `GameMaterials` catalog rule.
- **Impact configuration names** (the keys under an `impactValues`-like member) come from the game's own impact
  programs. A config only a mod's program names reads as its `0x…` id in the snapshot.
- **An optional or variant member holding a single number** reads as `null`, as the live reader shows it.
- **A value larger than 4 MiB** is not stored (a `skipped` line says so).
- **Decoder budgets** also refuse oversized or malformed inputs before large allocations: 256 MiB executable/image,
  16 MiB archive directory or resource blob, 8 MiB decompressed chunk, 100,000 entries per kind and 100,000 JSON nodes
  per value (1,000,000 for a value in `resources.data`, 250,000 resources in the list). Each kind's data is limited to
  1 GiB and each index to 32 MiB. A value exceeding its walk/byte budget is
  skipped with a reason; an archive exceeding its input budgets is refused. These bounds can require an update to
  the producer after a game format change.
- **A kind the game's archive cannot give** (a type the reflection lacks after a game update) is written empty: every
  read of that kind finds nothing. The launcher, `kiln boot-snapshot` and the deploy step each name such a kind in a
  warning.

## When there is no snapshot

On a start without a snapshot of the running game build (the tool could not run, it failed, or the game was started
from Steam before a start through the launcher made one),
`OnBoot` still runs, and:

- the capability row `mods.boot.game_data` is **unavailable**, with the reason "no game-data snapshot for this game
  build: start through the Kiln launcher, or run `kiln boot-snapshot --game <dir>`" and the test that failed (no
  folder of this key, an unreadable `index.json`, a kind file missing);
- `boot.GameData.Read(...)` throws `KilnUnavailableException("mods.boot.game_data")`.

A mod that can do without the game's data checks the row first (`Capabilities.Is("mods.boot.game_data")`;
`Capabilities.Get(...)` gives the reason) and keeps its own defaults. The next Play through the launcher makes the
snapshot (or run the command above), and from then on the reads work.

## The command

```
kiln boot-snapshot --game "C:\Program Files (x86)\Steam\steamapps\common\EnshroudedServer"
kiln boot-snapshot --game ~/srv/enshrouded --force --json
```

Exit 0 when a snapshot of this game build is in place (made now, or current), also when a kind is empty (a warning
names it); 1 when there is none (the reason is printed). `--json` prints the result: `{status, key, path, counts,
emptyKinds, skipped, archive, bytes, seconds, reason}`, `status` one of `made`, `reused`, `refused`, `failed`,
`unavailable` (the launcher and the deploy step also give `skipped`: no mod needs it).

From a Kiln checkout the tool also runs by itself: `python3 -I tools/bootsnap/bootsnap.py make --game <dir>` (and
`show --game <dir> item:<name> …` prints entries of the snapshot there).
