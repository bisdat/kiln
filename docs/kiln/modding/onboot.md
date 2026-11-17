# OnBoot: content that is there on the first start

Kiln's services collect some of the game's content before any mod's `OnLoad` runs: the programs behind statuses,
effects and spells, the skill tree, VFX, the world tables. What a mod defines in C# after that comes too late for the
first start; Kiln keeps it, and the game has it from the next start (data files and baked files aside:
[baked.md](baked.md)). A server's first start with a new mod is then not quite the real thing.

`Mod.OnBoot` runs earlier: before those collectors and before the game starts. What a mod stages there is checked
together with every mod's files and handed to the game in one piece, before the game reads any of it. The first start
has it, with the same ids as a definition made in `OnLoad`.

Since 0.2.0-dev.28 on dedicated servers, and since 0.2.0-dev.29 in a player's game too (a hosted world, and a game
that joins a server). The native side is kiln.boot ([../services/boot.md](../services/boot.md)).

## Turning it on
Add `boot` to `kiln.mod.json` ([manifest.md](manifest.md#boot-content-boot-since-020-dev28)):

```jsonc
{
  "id": "frost-tokens",
  "boot": true            // or "required"
}
```

- `true`: the mod's `OnBoot` runs. If its boot content fails, only this mod (and the mods that require it) is left out;
  the server starts without them.
- `"required"`: the same, but a failure stops the start (below). Use it when a server without your content would be
  worse than no server: a world that already holds your items, for example.

A Kiln.Sdk project generates its manifest: add `<KilnBoot>true</KilnBoot>` (or `required`) to the project file and the
generated `kiln.mod.json` says `"boot"` ([mdk.md](mdk.md)). A project that keeps its own `kiln.mod.json` beside the
project file says `"boot"` there; the build checks it and ships it, and the file wins over `KilnBoot`.

## What OnBoot is
```csharp
public override void OnBoot(BootContext boot) { ... }
```

- It runs **once per process**, before the game's entry point, on Kiln's boot thread.
- It runs on a **throwaway copy** of your mod: Kiln loads the mod's assembly on its own, creates an instance, calls
  `OnBoot`, and unloads it all again. The instance that later gets `OnLoad` is a new one. Nothing you keep in a field or
  a static survives: hand everything over by staging it. What `OnBoot` read (`boot.GameData`) cannot be handed to
  `OnLoad` in memory either: write what you need later into a file of your mod's data folder (`Context.DataDirectory`
  is the same folder in `OnBoot` and in `OnLoad`; create it first), or read it again through the run-time services.
- `boot.ModId`, `boot.Generation` (the process's boot generation), `boot.Roles` (where the mod runs: `DedicatedServer`,
  or in a player's game `HostedServer` and `Client` together) and `boot.GameData` (the game's data, read only, below).

**What you may do:** stage definitions with `BootDefinitions.For(boot)` (Kiln.Blocks), read `boot.GameData`, log
(`Log`), read your mod's own files (the folder is `Context.Info.Directory`).

**What you may not do:** anything that needs the running game. There is no tick, no world, no player and no live
registry yet. `Blocks.Define` throws `InvalidOperationException` ("PhaseClosed: ... stage the block with
BootDefinitions.For(boot).Define"). Run-time services (`World`, `Players`, `Content`, `Items`, ...) are not there to
call.

## Staging definitions
```csharp
var defs = BootDefinitions.For(boot);
var r = defs.Define("chill", new StatusBlock { Name = "Chill", Seconds = 6 });
defs.DefineJson("items", "frost-token", """{ "name": "Frost Token", "stack": 50 }""");
```

- `Define(name, block)` copies the block at once. It must be plain data: a delegate or a stream anywhere in it is
  refused (`ArgumentException`). Its type must be exactly one of the kinds below, not a subclass.
- `DefineJson(kind, name, json)` takes the block as it would stand in `blocks/*.json` under `<kind>.<name>`.
- A refusal is a result, not an exception: `Ok` false with `Reason` starting with a code.
  - `DuplicateDefinition`: the same kind and name was staged already with other content. The same content again is fine.
  - `PhaseClosed`: `OnBoot` has returned. Stage inside `OnBoot` only.
  - `TooMany`: more than 4096 definitions.
  - `NotInBootKinds`: a kind this build does not stage (below). Define it in `OnLoad` or a data file instead.
- `Ok` means staged, not committed. `Mods.BootState` tells you later whether it made it in.

**The kinds this build stages:** statuses, effects, items, spells, talents, and resource patches
([resource-patches.md](resource-patches.md)). Every other kind (creatures, NPCs, materials, props, stations, quests,
VFX, ...) answers `NotInBootKinds`.

**Names and limits:** a name is `^[a-z][a-z0-9_.-]{0,95}$`, without `..`. One mod stages at most 4096 definitions; one
definition is at most 1 MiB as JSON.

**References:** a boot definition can name a definition the same mod staged before it (`OnHit = ["chill-touch"]` in the
example). It cannot name another mod's block, or a block of your own data files: each mod's boot definitions are
compiled on their own, before any block is registered. Such a definition does not compile, and the mod is left out, with
the block error `BootReferenceOutside` in kiln.log and `state/boot.json`, for example: `BootReferenceOutside: "other-mod:frost"
is a block of mod other-mod; a boot definition names only blocks its own OnBoot staged before it`, or `BootReferenceOutside:
"frost" is a block of this mod's data files (blocks/*.json)`. Stage the block in the same `OnBoot`, before the definition
that names it.

**A data file wins.** If your `blocks/*.json` defines the same kind and name, the file's block is used and the staged
one is dropped, with a warning (`ShadowedByFile`) in kiln.log and `state/boot.json`.

**Icons and other files.** A PNG icon of a boot definition is a file of your mod's folder with a relative path, as in
`OnLoad`: an item's `Visual = new VisualBlock { Icon = "icons/token.png" }`, a status's or a game-tree talent's `Icon`
(ship the file with the mod, and set `"clients": "mod"`). The boot generation names an item's or a game-tree talent's
file by its full, link-resolved path in the mod's folder, as a definition made in `OnLoad` does, so the first start shows it. Since 0.2.0-dev.29: before, a boot-staged
item or talent kept its starter's icon (kiln.log: `cannot open ...\kiln\icons/token.png`). A status's icon is served when
the game's content is ready, as for `OnLoad`. A missing file, or a path that leaves the folder (`..`, a link out of it),
costs only the icon: the starter's stays. The refusal is retained in `state/boot.json`, even if the mod never loads;
kiln.log says why once when the mod loads, with the line `OnLoad` would
give (`blocks: <id>: block item token, visual.icon: ...; the base icon stays`); the SDK's bake warns with it (KILN030).
At the join an icon counts by its place in the folder (`$mod/icons/token.png`), so a player whose game is in another
folder matches the server. The other fields of the boot kinds name no file: a model is a GUID or `item:<name>`, a
sound or a VFX a name.

## Reading the game's data
`boot.GameData.Read(key)` reads one record of the game's data as shipped, from a snapshot made before the start
([boot-snapshot.md](boot-snapshot.md)): `item:<debugName>`, `template:<debugName>`, `skill:0x<8 hex digits>`,
`material:<name>`, `resource:<GUID>/<type hash>/<part>` (any resource of the game, of any type; the type also by its
qualified name: `resource:<GUID>/keen::SceneResource/0`). It answers `BootDataResult`, with the record as camelCase JSON.

To find resources, `names:<kind>` lists one kind's names (`names:items`), and `names:resources/<qualified type name>`
lists every resource of a type, with its GUID, part and name:

```csharp
var scenes = boot.GameData.Read("names:resources/keen::SceneResource");
foreach (var r in scenes.Data!.Value.GetProperty("resources").EnumerateArray())
{
    var scene = boot.GameData.Read($"resource:{r.GetProperty("resourceId").GetString()}/keen::SceneResource/{r.GetProperty("part").GetInt32()}");
}
```

Bulk asset types (animations, meshes, world chunks, effects) are listed but their values are not in the snapshot: a
`resource:` read of one is unavailable. The list is in [boot-gamedata.md](boot-gamedata.md).

- Without a snapshot of this game build, `mods.boot.game_data` is unavailable and `Read` throws
  `KilnUnavailableException`. The row's reason says what to run.
- One record is at most 4 MiB; one mod reads at most 16 MiB in all.
- It shows the game's own values: not another loader's patches, and not resource patches (they are written later).
- The format and the producer are in [boot-gamedata.md](boot-gamedata.md).

## Reading your mod's settings
`Config.Register<T>().Value` works in `OnBoot`: it reads the mod's config file and its defaults as `OnLoad` does,
and writes nothing. Register the same config again in `OnLoad`; that registration takes over the read, so `OnBoot` and
the running mod see the same values. Writes, `Reload`, a `Changed` handler and `RegisterWorld` are refused in `OnBoot`
("… is not available in OnBoot (it runs before the game's first tick); call it in OnLoad"). Details and an example:
[config.md](../config.md#in-modonboot).

**Settings and the join** (since 0.2.0-dev.29). Each process makes its boot content from its own settings, once, and
nothing syncs it later: a server and a player with other settings of your mod have other game data. So the join
compares it. For each boot mod with side `both`, both sides fingerprint what their generation committed (the staged
definitions as lowered, and the mod's resource patches, also those of its `blocks/*.json`), and the server treats a
difference like a version difference of the mod: refused under `modSync: refuse`, a warning under `warn` or the default, no comparison under `off`, with a message
that tells the player to use the server's settings and restart ([../network.md](../network.md#boot-content-at-the-join)).
Read in `OnBoot` only settings a server and its players share (balance values the server sets for everyone), and say on
your mod's page that players take the server's. A setting that may differ per player belongs in `OnLoad`. Stage the same
content for every role as well: content that depends on `boot.Roles` differs between a player's game and a dedicated
server, and the join counts that as a difference.

## When a mod fails
- **`OnBoot` throws**, or a staged definition does not compile: the mod is out. None of its definitions and none of its
  files are in the generation, and every mod that requires it is out too. The server starts without them.
- **A collector refuses a definition** (a resource patch on a field another mod sets, for example): the same.
- **A bad entry in the mod's own files** (an `assets/assets.json` entry with a bad GUID or a missing file, a
  `blocks/*.json`, `skills/*.json` or `quests/*.json` that is not JSON or cannot be read, a block or game-tree node with
  an error): Kiln logs it and leaves that entry out; the rest of the mod loads, as without `OnBoot`.
- An excluded mod does not load later in this process either: kiln.log says `RestartRequiredBoot: ... restart after
  fixing it`. Fix it and restart the server.
- **A mod with `"boot": "required"`** gets all of its boot content or the start stops: a failed `OnBoot`, a refused
  definition, and also any bad entry in its own files (the list above) that would be left out of another mod. The game
  is never entered: no world or save is loaded. The process ends with exit code `0x4B01`; on Linux, under Wine, the
  exit status is its low byte, 1. kiln.log ends with the stop line, which names the mod, the file or entry and the
  reason, for example `boot: the start stops before the game's entry point (exit code 0x4B01): my-mod: assets
  assets/assets.json: collector assets: kiln.assets: assets/assets.json: entries[1] (resource good.bin): resource: bad
  guid; state/boot.json lists every boot mod's state and issue`. `kiln/state/boot.json` has the same. What can only fail
  once the game's data comes up stops the start the same way, before a world loads: a resource patch that cannot be
  written, a world-table block the merge cannot apply, quests or NPCs of the mod that the quest build leaves out, and on
  a game client a texture override that cannot be applied. What only a loading world shows stops it while that world
  loads, before a player is in it: a game-tree node the tree build leaves out (an unknown `like` or `from`), an item,
  recipe or clone the world does not register. On a game client a required mod with VFX or materials stops the start
  when this start builds none (kiln.vfx turned off in `kiln/config/vfx.json`, a game update Kiln's VFX are not made
  for yet); a dedicated server builds no VFX at all (each player's game does), so that never stops a server.
- A mod with `boot` that was enabled after the start (`MissedPhase`), or whose files changed since (below), does not
  load until the next start.

`kiln/state/boot.json`, section `boot`: whether the phase ran (`deferred`), the boot mods, the collectors, each mod's
state and definition count, every issue (mod, kind, key, phase, code, reason) and the exit code when the start stopped.

## Afterwards
- `Mods.BootState(modId)` (any thread) gives the mod's state, its committed definitions as `kind/key`, and its issues.
  `Committed` means its definitions are in the game. `NotParticipating` means no boot phase took it.
- `BootDefinitionsCommittedEvent` goes out once each time the mod's run-time instance loads, on Kiln.Blocks' next step
  after `OnLoad`, when its committed definitions are active (their run-time parts: rules, icons). Subscribe in
  `OnLoad`. A mod whose `OnBoot` staged nothing gets the event too, with no definitions.
- The staged blocks keep their names: `mod:name` from another mod, as for a block defined in `OnLoad`.
- `Blocks.Find(name)` finds a committed boot block from the commit on: in `OnLoad` already (before the event), and after
  the mod's run-time unload, because the content stays in the game until the process ends. A block defined in `OnLoad`
  can name it (`Applies = new StatusApply { Status = "chill" }`). The handle has the ids the generation published.
- `Blocks.Define` in `OnLoad` under the name of a committed boot block: the same block (the same kind, the same values)
  gives the committed handle and registers nothing again; another one throws `BlockError`
  (`RestartRequiredBoot: chill differs from the statuses definition this process's boot generation committed ...`).
  What is in the game cannot change before a restart: change it in `OnBoot` and restart.

## After a change: restart
The boot generation keeps what the start read. When the generation commits, Kiln freezes each boot mod's inputs:

- **every file of the mod's folder**, at any depth: the manifest, the assemblies, `blocks/`, `assets/`, `quests/`,
  `skills/`, and any other file your `OnBoot` may read itself;
- **the settings your `OnBoot` read**: for each config it registered with `Config.Register`, the values it got (the
  file's values, checked and clamped, or the defaults).

A later load of the mod in the same process (after `Mods.UnloadAsync`, or the launcher's switch) compares them. Any
difference is refused before the mod's code loads:
`RestartRequiredBoot: mod frost-tokens changed since this process's boot generation committed its boot content
(blocks/frost.json changed); restart the game (or the server) to load the new version`, or for a setting
`RestartRequiredBoot: mod frost-tokens's config frost-tokens changed since its OnBoot read it (altars: 4 -> 6); its boot
content was made with the earlier values: restart the game (or the server) to load it with the new ones`. Put the file
back as it was, or restart. `Mods.ReloadAsync` of a loaded boot mod is refused the same way, whether anything changed or
not.

What is not a change:
- a JSON file written again with the same values (other spacing, comments, line endings, a byte-order mark): it counts
  by its parsed value; the order of its keys does count;
- a config file written again with the same values: it counts by the values the mod reads from it, so Kiln's own
  rewrite of it when the mod registers it in `OnLoad` (comments, missing entries, its canonical form) is no change. A
  changed value of a config `OnBoot` registered is one, even for an entry `OnBoot` did not use;
- files and folders whose name starts with `.` (the launcher's `.disabled` and `.kiln-source.json`), and `*.kiln-tmp`;
- at the top of the folder: `*.deps.json`, `*.runtimeconfig.json` and Kiln's own `Kiln.*` files (`Kiln.Api.dll`, the
  shared `Kiln.*.dll`, their `.pdb` and `.xml`), which no loader takes from a mod (a deploy leaves them out);
- the mod's data folder (`Context.DataDirectory`) and `kiln/config` (besides the configs above), should they lie inside
  the mod's folder: what the mod and Kiln write while they run. Do not write into your own mod folder while the game
  runs: such a file counts, and the mod then loads again only after a restart.

Reading the files costs a little time at the start: Kiln hashes every file of each boot mod once, before its `OnBoot`.
The check at the commit and later loads hash only the files whose size or write time changed since.

If a mod's inputs change while the boot phase runs (between its `OnBoot` and the commit), its content matches no
version of the files: kiln.log warns (`its files changed while the boot phase ran`), and the mod loads again only after a
restart.

## Where it runs
- **Dedicated servers.** The phase runs when hooks and .NET are on, an enabled mod of the server says `boot`, and
  `mods.boot` is available.
- **Game clients** (a world a player hosts, and a player's game that joins a server) run it the same way since
  0.2.0-dev.29: the rows `mods.boot.client` and `mods.boot.resource_patches` are open. In 0.2.0-dev.28 they were held
  and a boot mod did not load on a client without `"preview"` in `kiln/kiln.json`. A client builds every mod's VFX
  and material files at start, before the game's entry point and outside the boot generation, as it always did.
  Staging VFX or materials into a boot generation is not in this build (`mods.boot.vfx` reads unavailable, "not in
  this build", on a client). One of a required mod's blocks files left out, a VFX or material error of the cook,
  or a game build Kiln's VFX or materials cannot be built for still stops the start; the stop line names the file and
  why. An optional boot mod and a mod without `boot` lose only that file, with a line in kiln.log.
- A server where the phase does not run (safe idle, .NET off) does not load boot mods either. The row `mods.boot.phase`
  says why.

An enabled mod with `"boot": "required"` stops the start with 0x4B01 if the boot route is unavailable, including a
capability hold if one is added in a future release (this release holds none). No world or save is loaded without its required content.

## Testing
Unit tests of `OnBoot` without a game: the test kit's boot runner, `KilnHarness.BootPhase` ([unit-tests.md](unit-tests.md#boot-phase-since-020-dev28)).
`kiln.BootPhase.SetRecord("item:Material_T1_Wood_Logs", """{"maxStackSize": 250}""")` scripts what `boot.GameData` reads;
`var run = kiln.BootPhase.Run(modFolder)` runs your `OnBoot` as a start does and gives what it staged (`run.Staged`: kind,
key and the JSON Kiln lowers), what lowered (`run.Definitions`), and why it failed (`run.Reason`, `run.Issues`). Then
`kiln.LoadFolder(modFolder)` loads the mod with its boot content counted as committed. The SDK's bake runs `OnBoot` the
same way and fails the build when it fails ([baked.md](baked.md)).

## A complete example
`FrostTokens.csproj` with `<KilnServices>Blocks</KilnServices>` and `<KilnBoot>true</KilnBoot>` (or, instead of
`KilnBoot`, a `kiln.mod.json` of your own beside the project with `"boot": true`), and:

```csharp
using Kiln;

namespace FrostTokens;

public sealed class FrostTokensMod : Mod
{
    private const string Id = "frost-tokens";

    // Before the game starts, on a copy of the mod that is thrown away afterwards.
    public override void OnBoot(BootContext boot)
    {
        var defs = BootDefinitions.For(boot);
        Stage(defs.Define("chill", new StatusBlock { Name = "Chill", Seconds = 6 }));
        Stage(defs.Define("chill-touch", new EffectBlock { Applies = new StatusApply { Status = "chill" } }));
        Stage(defs.Define("frost-token", new ItemBlock { Name = "Frost Token", Stack = 50 }));
        Stage(defs.Define("frost-dart", new SpellBlock
        {
            Name = "Frost Dart",
            Cast = new CastBlock { ManaCost = 4 },
            Delivery = new ProjectileBlock { OnHit = ["chill-touch"] },
        }));
    }

    // A refusal is a result: throw, and the whole mod stays out rather than half of it.
    private static void Stage<T>(BootDefinitionResult<T> result) where T : class, IBlock
    {
        if (!result.Ok) throw new InvalidOperationException(result.Reason);
    }

    // The mod's run-time instance: a new one.
    public override void OnLoad()
    {
        var state = Mods.BootState(Id);
        Log.Info($"boot: {state.State}, {state.Definitions.Count} definition(s) in generation {state.Generation}");
        Events.On<BootDefinitionsCommittedEvent>(e =>
        {
            if (e.ModId == Id) Log.Info($"boot definitions active: {string.Join(", ", e.Definitions)}");
        });
    }
}
```

The lab mod `mods/tests/boot-definitions-test` does the same and checks each kind on the first start.
