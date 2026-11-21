# ShroudForge mods on Kiln: kiln mode

Kiln can run ShroudForge mods in two ways. By default it starts the ShroudForge runtime that comes with Kiln (or your
own installed copy) and lets it run your ShroudForge mods, as described in [shroudforge.md](shroudforge.md). **Kiln
mode** is the other way: Kiln runs ShroudForge-format mods itself, with its own interpreter of the ShroudForge mod API
(the API of ShroudForge 1.7.9), inside Kiln's runtime. No separate ShroudForge runtime is loaded in kiln mode.

Kiln mode is **opt-in**. Nothing changes until you choose it, and you can switch back at any time.

Adapted from ShroudForge by bonsaibauer, MIT.

## Choosing kiln mode

1. Close the game.
2. In the Kiln Launcher open **Settings** and find the **ShroudForge** card (see
   [launcher/shroudforge-backend.md](launcher/shroudforge-backend.md)). Pick **Kiln's interpreter**.
3. The launcher shows what the choice does and asks you to save it. It is stored per game in
   `<game>/kiln/kiln.json` as `"shroudforge": { "backend": "kiln" }` and applies from the next start.
4. Activate your profile and press **Play**.

Choosing **Kiln's interpreter** on the card is all it takes: no other setting is needed.

If the card shows **Kiln's interpreter** greyed out with "Not available in this Kiln build: ...", this Kiln build does
not offer kiln mode, for the reason it gives.

Kiln mode also stands aside (and no ShroudForge mod runs through Kiln) when:
- ShroudForge's own loader is in the game folder (its proxy DLL, `chain.ini` or `dinput8-chain.ini`, or
  `dbghelp=n,b` / `winmm=n,b` in the launch options): ShroudForge then runs your mods itself, and the card says so;
- `KILN_SHROUDFORGE=0` is set in the launch environment;
- `kiln/plugins/kiln-sfi.dll` is missing from the game: install Kiln again to add it.

## What happens at a kiln-mode start

- **Before Play**, the launcher checks every ShroudForge mod in your profile. A mod Kiln cannot run whole is **skipped
  for this start, with its reason**, and the launcher shows a warning before the game opens: what does not run, which
  part (startup data, Lua, DLLs), why, and which other mod it depends on. **Cancel** is the default; **Continue without
  these mods** starts the game without them. Continuing switches nothing off and deletes nothing; the next Play checks
  again. A start from Steam's own Play button skips this dialog: you get only the in-game notice.
- **Startup mods** (mods that change game data before the game starts) run in one pass before the game starts, as
  they do under ShroudForge.
- **In the game**, the Lua side of each mod runs on Kiln's interpreter: its lifecycle (`on_load`, updates,
  `on_unload`), settings, logging, clock, file access inside its own folder, `game.types`, the registry and component
  catalog reads, `runtime.network`, `runtime.overrides`, the world context, the UI actions and notifications, and
  `shroudforge.ui.toggle_modloader()`.
- **Mod DLLs** that a mod ships for ShroudForge's DLL loader are loaded by Kiln at the start, once. A DLL Kiln has no
  reviewed rule for is refused, and its mod is skipped with the reason.
- **ShroudForge's menu key** (F9 by default) and `shroudforge.ui.toggle_modloader()` open Kiln's page for ShroudForge mods
  (**Mods → ShroudForge** in the game). It lists what runs this start, each mod's settings, and its action buttons.
  Settings you change there are saved into the mod's own `extended.mod.json` and apply **at the next start**.
- **Live reload**: a reload request for a Lua-only mod reloads it in place. A mod whose files changed since the start, or
  a mod with DLLs, answers that it needs a restart, and the running mods are left alone.

### What answers "unavailable"

Some parts of the ShroudForge mod API are not in Kiln's interpreter yet. A call to one of them does not crash the mod
and never returns `nil` by surprise: it gives ShroudForge's own "unavailable" answer (`nil` or `false` plus a reason,
or an error where ShroudForge raises one), so a mod that checks it keeps running with that feature off. In this release
that covers:
- in-game world changes: ECS queries, reads and writes beyond the catalog, templates and attributes, voxels, props and
  entities (`runtime.ecs.query`, `read_fields`, `write`, `runtime.world.*` placement and voxels);
- in-game `game.assets` and `runtime.assets`, and the resource, attribute, service, template and component overrides;
- **mod shortcuts**: `shroudforge.input` keybind queries answer "not held" / "not pressed" in kiln mode for now;
- building input (it is unavailable under ShroudForge too on the current game builds);
- runtime operations from the startup pass answer "not verified for game build ...";
- anything that needs ShroudForge 1.8 is never offered.

A mod that uses one of these keeps running with that feature off (for example, skeleton-commander's leash). A mod
that needs them for everything it does loads but may do nothing useful until a later Kiln release adds them; switch it
off or use the default backend for it.

## The do-not-save notice

Some mods keep data in your saves (Quiver & Spellbook's ammo, for example). If such a mod does not run this start, or
was running at an earlier start and is now off, **saving a world can lose that data**. Kiln warns you in three places:
- the warning before Play: "Do not save without them";
- in the game, a banner at the main menu ("Do not save: N mods are off this start"), a line above the main menu footer
  ("N mods off: do not save"), a **DO NOT SAVE** badge on the mod's row in **Mods**, and a marker on Kiln's Mods entry
  in the pause menu. The banner comes back when you enter a world, when the list changes and at each save;
- `kiln/state/sfi.json` and `kiln/logs/kiln.log`.

The notice says "Do not save while this mod is off; restore it and restart first." Do that: switch the mod back on,
restart, and only then save. Kiln never blocks or changes a save; the notice is the only protection. Mods that are off
but cannot lose save data are marked "off this start" instead.

The in-game banner needs Kiln's in-game menu. If you start without it, rely on the launcher's warning.

## Switching back

Choose **Kiln's bundled ShroudForge** (or **Back to the default**) on the ShroudForge card, or set
`"backend": "hosted"` in `kiln.json`. Activate and press Play.
Your mods, their settings and your own mod folders stay as they are; the first start after kiln mode makes ShroudForge
prepare its startup data again, which can take a little longer once. Nothing switches back to the default on its own.

## Where to look

| What | Where |
|---|---|
| Kiln's log (this start, last start) | `<game>/kiln/logs/kiln.log`, `kiln.prev.log`; interpreter lines start with `kiln-sfi:` |
| What ran this start | `<game>/kiln/state/sfi.json`: phase (`starting`, `running`, `ended`, `aside`, `refused`), reason, one row per mod |
| The backend this start used | `<game>/kiln/state/launch.json`, and the card's "Last start" line |
| The launcher | **Mods → Engines** shows the interpreter's row; the support report includes the files above |

A skipped mod is logged as `kiln-sfi: WARNING: ShroudForge package '<id>' will not run: <reason>`. When kiln mode does
not run at all, the line is `kiln-sfi: WARNING: Kiln's interpreter does not run this start: <reason>`.

## Known differences

Kiln aims to behave as ShroudForge does. Where it deliberately does not, the difference is listed here.

**Errors and limits**
- Where ShroudForge's own code would crash the game, Kiln raises a Lua error in that mod with a stated cause.
- A single callback that runs more than 10 seconds of Lua time is stopped, and that mod ends for this start (the other
  mods go on).
- Compute-heavy Lua runs slower: Kiln watches every mod so it can stop a runaway one. A tight arithmetic loop is about
  2.5 times slower; ordinary table and string work about 8% slower.
- File access refuses links that lead out of the mod's folder before anything is written.
- An impossible array count in game data is an error, not a huge allocation.
- Some error texts differ slightly (typed-value errors, "failed to convert modified value"); messages for the same
  failures stay close to ShroudForge's.

**Game data and the world**
- A plain ECS write answers "busy" after three conflicts in a row; engine calls refuse code that another mod hooked or
  patched, and stop for the session after a fault.
- A Kiln mod holding the engine gate can, rarely, make a ShroudForge mod see "busy".
- Overrides are never restored into a different world session than the one they were applied in.
- `game.assets` writes in the startup pass refuse to create a resource the game already has, and refuse writes after
  the pass has closed (ShroudForge would drop them silently).
- `runtime.world` context changes follow Kiln's world lifecycle and can be a few frames off ShroudForge's.

**DLLs and starting**
- With `native.policy` set to `never` (or `verified-only` on an unverified build) in `kiln.json`, no mod DLL loads in
  kiln mode; the mods that need them are skipped with Kiln's reason. The default `always` behaves as ShroudForge.
- The 15-second limit for a mod DLL's start includes Kiln's own checks; on a timeout Kiln skips those mods and the game
  goes on.
- If a mod's DLL file is replaced on disk at the exact moment it loads, Kiln notices after it ran, then turns that mod
  off with a do-not-save notice.
- Whether a process is the client or the server comes from the game executable Kiln recognises, not from a file name.

**Settings, UI and reload**
- Settings changed in kiln mode apply at the next start.
- Kiln's package files are edited in place, keeping their formatting.
- UI limits: a 2 MiB message, 1,024 pending actions per mod, 4,096 notification keys. Past a limit the mod gets an
  error instead of losing data silently. An action file that another program holds locked waits for the next turn.
- Live reload uses Kiln's own request files (`<game>/kiln/sfi/runtime/mod-reload-request.json` and
  `mod-reload-result.json`), checked every 100 ms. Only Lua-only mods that were admitted at the start reload; an edited
  mod needs a restart.
- Log lines from the network and UI parts use Kiln's log layout; the texts are the same.

**Multiplayer**
- On a dedicated server running in kiln mode, the connected-player list comes from the server's log and starts empty: on a server with a long
  log it can take a few seconds to fill. At most 256 players are accepted automatically; the rest need a manual accept.

To help test kiln mode, see [Testing kiln mode](shroudforge-kiln-mode-testing.md).
