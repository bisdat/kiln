# Enshrouded ModLoader packages on Kiln (kiln-dllmod)

Kiln runs mods made for AndyPimms' **Enshrouded ModLoader** ([aebischer/Enshrouded-ModLoader](https://github.com/aebischer/Enshrouded-ModLoader))
itself, without that loader's `dinput8.dll`. Its public mods are **CarryOn** (move chests with their contents,
furniture, doors and wells) and **Belt Lantern** (a lantern equipment slot). A mod for it is a *package*: a 64-bit DLL
and, beside it, a manifest named after the DLL plus `.mod` (`carryon.dll` + `carryon.dll.mod`) whose first line is
`ENSHROUDED_PACKAGE_V1` (ABI 1). In Kiln it is the Library kind `dllmod`, run by `kiln-dllmod.dll` in `kiln/plugins/`.

Game client only, build 26430625 (`enshrouded.exe` SHA-256 `5d9ef2f1...0c77`), single player and hosting your own
world, as the loader and its mods themselves. Kiln starts none of them on another game build or on a dedicated server,
and says so in `kiln/logs/kiln.log` and on the launcher's Engines view.

## Install a mod

Add the mod's release zip (or its folder, or the DLL together with its `.dll.mod`) in the Kiln launcher, as any mod, and
switch it on in your profile. When you start the game, Kiln copies the profile's switched-on packages to
`<game>/Mods/EnshroudedModLoader/<id>/`, the folder Enshrouded ModLoader 0.6.12 reads its packages from, and runs them
from there. A mod keeps the files it writes beside its DLL there (Belt Lantern's `characters/`) across updates; a mod you
switch off leaves that folder, and its files wait in `kiln/kept-mod-files/dllmod/<id>/` until you switch it on again.

Don't run the loader's `Install.cmd` for Kiln: you don't need its `dinput8.dll` any more.

The Library leaves out, with the reason: a mod DLL without its `.dll.mod` (nothing says its version or settings), a
manifest the package format refuses (an unknown key, `abi` other than 1, a bad setting line), a mod package without the
export `EnshroudedModInitialize`, and settings without `EnshroudedModConfigure`.

## When the mods start

Exactly when the loader starts them: when a world of your own starts (Play, or Load on a world in the world list),
before the game opens it. For each switched-on mod Kiln:

1. loads the packages it requires first (`require=<package>@<version>`, a `kind=library` package with that exact
   version; it must be in your profile as well), then the mod's DLL, from its folder and System32 only;
2. calls `EnshroudedModConfigure(world id, settings)` when the mod has settings;
3. calls `EnshroudedModInitialize` with the ABI 1 context: its size (24 bytes), the ABI version 1, the world's id
   (32 lower-case hex digits) and a log function. Each mod's lines go to `kiln.log` as `[dllmod:<id>] ...`.

A mod returns 1 when it started. Mods start once per game session, for one world, as with the loader:

- a mod that cannot run (its package refused, a required package missing or of another version, its DLL already loaded
  by something else) or that fails (its export returns anything but 1, or crashes) **keeps the world from starting**.
  A message box says which mod and why; switch it off in the launcher, or restart Enshrouded. A world opened without a
  mod can lose that mod's items when it saves (Belt Lantern says so in its own instructions), so Kiln does not open it
  without them;
- after a mod failed part way, every world start is refused until you restart the game;
- the same world again (back to the menu and Load) starts nothing twice; **another world needs a restart** of
  Enshrouded, because the mods were set up for the first one;
- **joining another player's world** while these mods are loaded is refused with a message, as the loader does: the
  mods are made for your own worlds.

## Switching mods per world

The loader keeps a list of switched-on mods per world (its MODS page in a world's Edit menu). Kiln uses your Kiln
profile instead: a mod switched on in the profile starts with whichever world you open. To play one world with a mod
and another without it, use two profiles.

Per-world switching from the game's own world Edit menu is a possible follow-up. It would take: hooking the world Edit
screen and its button drawing (the loader does this with five UI hooks on build 26430625), drawing a page with the
mods and their settings in the game's UI, storing the choice per world, and reading it at the world start. That is game
UI work tied to one build, with a guided client check per screen; the world start itself would not change.

## Settings

A mod with settings (CarryOn's Chests, Decor, Usable objects, Debug) gets, in this order:

1. Kiln's settings file for it, `<config>/dllmod/<id>.ini` (`<config>` is the profile's config folder, normally
   `<game>/kiln/config`), in the loader's format: a first line `ENSHROUDED_SETTINGS_V1`, then `key=value` lines;
2. the loader's own settings for the world being opened (`<game>/Mods/EnshroudedModLoader/worlds/<world id>.settings-v2/<SHA-256
   of the package id>.ini` since 0.6.15, else `<game>/ModLoader/worlds/...`), what its MODS page saved, so settings you made
   with the loader keep working;
3. the manifest's defaults.

A file that is there but invalid (an unknown key, a value the setting does not take) keeps the world from starting with
the file's name, as the loader does. The launcher has no settings page for these mods yet.

## When Enshrouded ModLoader is installed too

Since 0.6.15 its `Install.cmd` installs it next to Kiln as a Kiln native mod, `kiln/native/enshrouded-modloader/modloader.dll`
with a `native.json` (phase boot), and keeps its per-world profiles and settings in `Mods/EnshroudedModLoader/worlds/`.
kiln-core loads that native mod after every plugin started, so kiln-dllmod looks for it on disk as kiln-core does (a
switched-on native entry of the profile, or the `kiln/native` drop-in folder when the profile reads it) and steps aside
before it hooks anything. The launcher counts it as this kind's own loader the same way. Kiln never places a package
named `worlds`, and never removes or changes the loader's files, its `worlds/` folder or its installer's backups: Kiln
removes only copies its own receipts name.


Kiln never runs a package twice and never breaks the loader. The loader refuses to start when one of the game functions
it hooks was patched first, so when its `dinput8.dll` is in the game (chained by Kiln from `kiln/chain/`, listed in
`kiln/chain.ini` or `dinput8-chain.ini`, or added to the Library as a DLL mod) kiln-dllmod **steps aside and hooks
nothing**. The loader then runs the packages in `Mods/EnshroudedModLoader/`, Kiln's copies of your profile's mods
included: switch them on in the world's Edit > MODS page. `kiln.log` and the Engines view say so (`stepped aside`).
The loader's own `dinput8.dll` is still a DLL mod the Library takes and Kiln chain-loads.

None of Kiln's hooks is on the loader's eight functions on build 26430625, and kiln-dllmod's two hooks are on two of
them (the local world start `0xd8ed80` and the session start `0xd8efb0`) only when the loader is not there.

## Other things to know

- **Hooks the mods share with Kiln.** The mods hook the game themselves (MinHook) when they start. CarryOn hooks six of
  the functions Kiln's assets service hooks (`0x1080ae0`, `0x10805d0`, `0x107ed60`, `0x1081730`, `0xa56180`,
  `0x1083210`) and `0xe94470` (Kiln's item lookup), `0x29bfb0`, `0xe66060` and `0x12169b0`; Belt Lantern
  `0xe66060`, `0xe94470`, `0x4152d0`, `0x419740` and `0xc046c0` (Kiln's equip, unequip and item slot). MinHook puts its
  hook in front of the one already there, and Kiln reports a hook that was put on top of its own in `kiln/state/hooks.json`.
  This is the same with the loader and Kiln installed together; only a real game shows whether a pairing works.
- **Multiplayer and servers.** Not supported, as by the loader: the package format has no server side. The Library
  marks these mods client-side, and players who join you need nothing.
- **Status.** `kiln/state/dllmod.json`: `status` (`idle`, `waiting`: they start with your next world, `running`,
  `failed`, `stepped aside`, `unsupported`: another game build, `client only`: a dedicated server), `world`, and per
  mod its `status` (`waiting`, `started`, `failed`, `rejected`, `disabled`, `skipped`, `own-loader`, `not-loaded`)
  with the reason, the package id, the settings it was given and where they came from.
- **Drop-ins.** With every drop-in folder switched on in a profile (or without a profile), Kiln also starts the packages
  you put in `Mods/EnshroudedModLoader/` yourself. There is no switch for this folder on its own: a Kiln runtime before
  0.2.0-dev.29.19 would call such a profile invalid.

## Where the facts come from

The loader's repository has no licence, so no code, header or text of it is in Kiln. kiln-dllmod and the Library's
reader (`crates/kiln-manage/src/dllmod.rs`, `native/plugins/dllmod/src/package.h`) are written for Kiln from:

- the package format and the mod interface as the loader's SDK documents them (`sdk/STANDARD_V1.txt`,
  `sdk/include/enshrouded_mod.h`, read at commit 57586c65 and the 0.6.12 changes): the manifest's lines and limits, the
  two exports, their C calling convention and return values, the 24-byte context, Configure's `key=value` text sorted by
  key, the call order, the per-world settings file's name and first line, `Mods/EnshroudedModLoader` (0.6.12);
- the loader's observable behaviour as its source shows it: it loads a package with the search path of its own folder
  and System32, starts the mods inside the game's local world start before the game's own (`0xd8ed80` on build
  26430625; the world's id is the slot's 16 bytes at `+0x14` while the byte at `+0xe0` is set), refuses a join of
  another machine's world with mods loaded at the session start (`0xd8efb0`, a request whose first field is 0), refuses
  another world or a changed list until a restart, and has no shutdown or world-close call;
- the shipped manifests and entry points of CarryOn 0.1.50 and Belt Lantern 0.1.29 (public source on GitHub): CarryOn
  requires Configure before Initialize for the same world; Belt Lantern writes beside its DLL and checks the exe's hash
  itself;
- the game's own bytes at both functions in `enshrouded.exe` 5d9ef2f1, which kiln-dllmod checks before it hooks.

## Tests

- `native/plugins/dllmod/tests/run.sh`: the manifest, settings and settings files, package ids, world ids, the load
  order of required packages and SHA-256, on the Linux host.
- `native/plugins/dllmod/tests/run_host.sh`: the host with test packages built from Kiln's own source with the same
  ABI, under Wine: settings and context as a mod sees them, each mod's log lines, a library a mod imports, the same and
  another world, the join refusal, a mod that returns 0, one that crashes, one that cannot run, a missing or wrong
  library, a DLL already loaded, an unreadable world id, invalid settings, a package without its export.
- `native/plugins/dllmod/tests/run_plugin.py`: the plugin in compat's stand-in game: the profile's and the folder's
  packages, their statuses, another game build, a dedicated server.
- `crates/kiln-manage/tests/engine_dllmod.rs` and `engines.rs`: the release zip's shapes, what the Library leaves out,
  placement in `Mods/EnshroudedModLoader` across updates and a switch-off, Copy mode by runtime, the core's own parser.

## Manual test (real client)

On build 26430625, without Enshrouded ModLoader's `dinput8.dll` (remove it from `kiln/chain/` if Kiln chained it):

1. Add CarryOn 0.1.50 and Belt Lantern 0.1.29 (their release zips) in the launcher; switch both on. Start the game:
   `kiln.log` says `kiln-dllmod: 2 mod(s) start when a local world starts`, `Mods/EnshroudedModLoader/carryon` and
   `.../lantern` hold their files.
2. Load a single-player world: `kiln.log` has `[dllmod:carryon] started carryon/carryon.dll 0.1.50-dev for world ...`
   and the same for lantern, and the mods' own lines (`[dllmod:lantern] Lantern 0.1.29: ...`). Craft the transport
   pincers and move a chest with its contents; craft a belt lantern and equip it in its slot. Save, quit to the menu,
   load the same world: nothing starts twice, the lantern is still equipped.
3. Load another world: a message box says to restart Enshrouded. Restart, load it.
4. Try to join another player's world with the mods on: a message box refuses it.
5. With Enshrouded ModLoader's `dinput8.dll` chained again: `kiln.log` says `kiln-dllmod steps aside`, its MODS page
   lists carryon and lantern, and switched on there they run under the loader.
